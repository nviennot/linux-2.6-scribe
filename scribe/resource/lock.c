/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include "internal.h"

/*
 * A few notes:
 * - scribe_resource_prepare() must be called prior to any resource lock/open
 *   operations with the exception that get_unused_fd() will successfully
 *   return only with the user refilled (so that fd_install() can proceed).
 *
 * - The files_struct lock must be taken as well to access any close_on_exec
 *   flags.
 *
 * - The filp lock must be taken to access any of the file related data.
 *
 * - The filp+inode lock must be taken to access any of the file related data
 *   and/or inode related data, with the exception of pipes
 *   (inode_need_explicit_locking() returns false in that case).
 *
 * - For pipes, it's quite convenient to have two different resources for each
 *   endpoint:
 *   - Reduced overhead (decoupling the readers/writers)
 *   - It Solve the deadlocking issue when the writer wants to send data to the
 *     pipe while the pipe buffer is full (the reader will not be able to
 *     consume the data since the writer has the resource lock).
 *   Instead of having two resources per inode, we are just using the filp
 *   resource to synchronize each end point (and don't need to take the inode
 *   resource since no other file pointer can exist on the pipe inode).
 *   (In rare cases we may take the inode resource lock -- see in fs/fcntl.c)
 *
 * - Taking a lock with a read access is not different from taking it with
 *   write access during replay.
 */

static bool is_locking_necessary(struct scribe_ps *scribe,
				 struct scribe_resource *res)
{
	if (should_scribe_res_always(scribe))
		return true;

	/*
	 * It is assumed that the init process won't do anything racy with the
	 * first child, this way when the number of processes is equal to 2,
	 * we can disable resource tracking.
	 * When the number of processes is equal to 3, we need resource
	 * tracking, but we cannot go back to the disabled resource tracking
	 * easily. We would need a MEM_ALONE event or something to
	 * deterministically switch back to this state.
	 * In our case, we are lazy and stay in that mode.
	 * TODO send a RES_ALONE event when necessary.
	 */
	if (scribe->ctx->max_num_tasks > 2)
		return true;

	/*
	 * The only resource we need for the init process is the task one.
	 * Forcing the synchronization on it is an easy way to avoid races
	 */
	if (res->type == SCRIBE_RES_TYPE_PID)
		return true;

	return false;
}

static int __do_lock_record(struct scribe_ps *scribe,
			    struct scribe_resource *res,
			    int do_write, int do_intr, int nested)
{
	int ret;
	nested = !!nested;

	if (use_spinlock(res)) {
		spin_lock_nested(&res->lock.spinlock, nested);
		return 0;
	}

	if (!do_intr) {
		if (do_write)
			down_write_nested(&res->lock.semaphore, nested);
		else
			down_read_nested(&res->lock.semaphore, nested);
		return 0;
	}

	if (do_write)
		ret = wait_event_interruptible_exclusive(res->wait,
		  down_write_trylock_nested(&res->lock.semaphore, nested));
	else
		ret = wait_event_interruptible(res->wait,
		  down_read_trylock_nested(&res->lock.semaphore, nested));

	return ret;
}

static void __do_lock_read_serial(struct scribe_resource *res)
{
	/*
	 * This works because when @first_read_serial is equal to -1,
	 * @res->serial cannot change because it gets incremented only
	 * during the unlock(). So once @res->serial changes,
	 * @first_read_serial would already be assigned.
	 */
	cmpxchg(&res->first_read_serial, -1, atomic_read(&res->serial));
}

static void priority_lock(struct scribe_resource *res, int priority)
{
	if (priority) {
		BUG_ON(use_spinlock(res));
		atomic_inc(&res->priority_users);
	}
	else {
		wait_event(res->wait,
			   likely(!atomic_read(&res->priority_users)));
	}
}

static void priority_unlock(struct scribe_resource *res, int priority)
{
	if (priority) {
		atomic_dec(&res->priority_users);
		wake_up(&res->wait);
	}
}

static void untrack_user(struct scribe_lock_region *lock_region)
{
	struct scribe_resource *res = lock_region->res;

	if (!scribe_resource_ops[res->type].track_users)
		return;

	spin_lock(&res->lock_regions_lock);
	list_del(&lock_region->res_node);
	spin_unlock(&res->lock_regions_lock);
}

static void do_unlock_discard(struct scribe_ps *scribe,
			      struct scribe_lock_region *lock_region);
static int track_user(struct scribe_ps *scribe,
		      struct scribe_lock_region *lock_region)
{
	struct scribe_resource *res = lock_region->res;
	int priority = lock_region->flags & SCRIBE_HIGH_PRIORITY;

	if (!scribe_resource_ops[res->type].track_users)
		return 0;

	lock_region->owner = scribe;

	spin_lock(&res->lock_regions_lock);
	list_add(&lock_region->res_node, &res->lock_regions);
	spin_unlock(&res->lock_regions_lock);

	/* We need to avoid races with INTERRUPT_OTHERS and the priority */
	if (!priority && unlikely(atomic_read(&res->priority_users))) {
		untrack_user(lock_region);
		do_unlock_discard(scribe, lock_region);
		return -EAGAIN;
	}
	return 0;
}

static void do_interrupt_users(struct scribe_resource *res)
{
	struct scribe_lock_region *lock_region;
	struct task_struct *p;
	unsigned long flags;

	spin_lock(&res->lock_regions_lock);
	list_for_each_entry(lock_region, &res->lock_regions, res_node) {
		p = lock_region->owner->p;

		if (lock_task_sighand(p, &flags)) {
			signal_wake_up(p, 0);
			unlock_task_sighand(p, &flags);
		}
	}
	spin_unlock(&res->lock_regions_lock);
}

static int do_lock_record(struct scribe_ps *scribe,
			  struct scribe_lock_region *lock_region,
			  struct scribe_resource *res)
{
	struct scribe_event_resource_lock_extra *lock_event;
	struct scribe_event_resource_lock_intr *event;
	int do_intr = lock_region->flags & SCRIBE_INTERRUPTIBLE;
	int do_write = lock_region->flags & SCRIBE_WRITE;
	int nested = lock_region->flags & SCRIBE_NESTED;
	int priority = lock_region->flags & SCRIBE_HIGH_PRIORITY;
	int interrupt_users = lock_region->flags & SCRIBE_INTERRUPT_USERS;
	size_t size;
	int ret;

	if (scribe->debug_res) {
		dump_stack();
	}

	if (should_scribe_res_extra(scribe)) {
		/*
		 * We want to fill out the description because we can unlock
		 * object that are dead (and we won't be able to get the
		 * description).
		 */

		lock_event = lock_region->lock_event.extra;
		size = get_description(res, lock_event->desc, RES_DESC_MAX);
		lock_event->h.size = size;
	}

	priority_lock(res, priority);

	if (unlikely(interrupt_users))
		do_interrupt_users(res);

	scribe_create_insert_point(&lock_region->ip, &scribe->queue->stream);

	ret = __do_lock_record(scribe, res, do_write, do_intr, nested);
	if (unlikely(ret)) {
		/* Interrupted ... */
		priority_unlock(res, priority);

		event = lock_region->lock_event.intr;
		lock_region->lock_event.intr = NULL;

		/*
		 * The lock_intr event is smaller than the allocated one, so
		 * casting the event works.
		 */
		event->h.type = SCRIBE_EVENT_RESOURCE_LOCK_INTR;
		scribe_queue_event_at(&lock_region->ip, event);
		scribe_commit_insert_point(&lock_region->ip);
		return -EINTR;
	}

	if (!do_write)
		__do_lock_read_serial(res);

	return 0;
}

static int serial_match(struct scribe_ps *scribe,
			struct scribe_resource *res, int serial)
{
	if (serial <= atomic_read(&res->serial))
		return 1;

	if (unlikely(is_scribe_context_dead(scribe->ctx))) {
		/* scribe_kill() has been triggered, we need to leave */
		return 1;
	}

	return 0;
}

static int __do_lock_replay(struct scribe_ps *scribe,
			    struct scribe_lock_region *lock_region,
			    struct scribe_resource *res)
{
	struct scribe_event *intr_event;
	int type;
	int serial;

	intr_event = scribe_peek_event(scribe->queue, SCRIBE_WAIT);
	if (IS_ERR(intr_event))
		return PTR_ERR(intr_event);

	if (intr_event->type == SCRIBE_EVENT_RESOURCE_LOCK_INTR) {
		intr_event = scribe_dequeue_event(scribe->queue, SCRIBE_WAIT);
		scribe_free_event(intr_event);
		return -EINTR;
	}

	if (should_scribe_res_extra(scribe)) {
		struct scribe_event_resource_lock_extra *event;

		event = scribe_dequeue_event_specific(scribe,
					      SCRIBE_EVENT_RESOURCE_LOCK_EXTRA);
		if (IS_ERR(event))
			return PTR_ERR(event);

		type = event->type;
		serial = event->serial;
		scribe_free_event(event);

		if (type != res->type) {
			scribe_diverge(scribe,
				       SCRIBE_EVENT_DIVERGE_RESOURCE_TYPE,
				       .type = res->type);
			return -EDIVERGE;
		}
	} else {
		struct scribe_event_resource_lock *event;

		event = scribe_dequeue_event_specific(scribe,
						SCRIBE_EVENT_RESOURCE_LOCK);
		if (IS_ERR(event))
			return PTR_ERR(event);

		serial = event->serial;
		scribe_free_event(event);
	}

	/* That's for avoiding a thundering herd */
	scribe->waiting_for_serial = serial;
	wmb();
	wait_event(res->wait, serial_match(scribe, res, serial));

	return 0;
}

static int do_lock_replay(struct scribe_ps *scribe,
			  struct scribe_lock_region *lock_region,
			  struct scribe_resource *res)
{
	if (__do_lock_replay(scribe, lock_region, res)) {
		if (lock_region->flags & SCRIBE_INTERRUPTIBLE)
			return -EINTR;
	}

	return 0;
}

static int do_lock(struct scribe_ps *scribe,
		   struct scribe_lock_region *lock_region)
{
	int ret = 0;
	struct scribe_resource *res = lock_region->res;

	if (!is_locking_necessary(scribe, res))
		goto no_lock;

	if (unlikely(is_detaching(scribe))) {
		if (lock_region->flags & SCRIBE_INTERRUPTIBLE)
			ret = -EINTR;
		goto no_lock;
	}

	BUG_ON(!(lock_region->flags & (SCRIBE_READ|SCRIBE_WRITE)));

	if (is_recording(scribe))
		return do_lock_record(scribe, lock_region, res);
	else
		return do_lock_replay(scribe, lock_region, res);

no_lock:
	lock_region->flags |= SCRIBE_NO_LOCK;
	return ret;
}

static void do_lock_downgrade_record(struct scribe_ps *scribe,
				     struct scribe_lock_region *lock_region,
				     struct scribe_resource *res)
{
	if (!use_spinlock(res))
		downgrade_write(&res->lock.semaphore);

	lock_region->flags &= ~SCRIBE_WRITE;
	lock_region->flags |= SCRIBE_READ;
	__do_lock_read_serial(res);
}

static void do_lock_downgrade(struct scribe_ps *scribe,
			      struct scribe_lock_region *lock_region)
{
	struct scribe_resource *res = lock_region->res;

	if (unlikely(is_detaching(scribe)))
		return;

	BUG_ON(!(lock_region->flags & SCRIBE_WRITE));

	if (is_recording(scribe))
		do_lock_downgrade_record(scribe, lock_region, res);
	/* no-op for replay */
}

static void __do_unlock_record(struct scribe_resource *res, int do_write)
{
	if (use_spinlock(res))
		spin_unlock(&res->lock.spinlock);
	else {
		if (do_write)
			up_write(&res->lock.semaphore);
		else
			up_read(&res->lock.semaphore);

		/* We need to wake the ones in wait_event_interruptible.  */
		wake_up(&res->wait);
	}
}

static void do_unlock_record(struct scribe_ps *scribe,
			     struct scribe_lock_region *lock_region,
			     struct scribe_resource *res)
{
	int do_write = lock_region->flags & SCRIBE_WRITE;
	int priority = lock_region->flags & SCRIBE_HIGH_PRIORITY;
	unsigned long serial;

	if (do_write) {
		/*
		 * We have a lock write on the resource, so there won't be no
		 * race.
		 * @serial has already been incremented.
		 */
		serial = atomic_read(&res->serial) - 1;
		res->first_read_serial = -1;
	} else {
		/*
		 * The value is stable until we release the read lock.
		 */
		serial = res->first_read_serial;
	}

	__do_unlock_record(res, do_write);
	priority_unlock(res, priority);

	if (should_scribe_res_extra(scribe)) {
		struct scribe_event_resource_lock_extra *lock_event;
		struct scribe_event_resource_unlock *unlock_event;
		lock_event = lock_region->lock_event.extra;
		unlock_event = lock_region->unlock_event;
		lock_region->lock_event.extra = NULL;
		lock_region->unlock_event = NULL;

		lock_event->type = res->type;
		lock_event->write_access = !!do_write;
		lock_event->id = res->id;
		lock_event->serial = serial;

		scribe_queue_event_at(&lock_region->ip, lock_event);
		scribe_commit_insert_point(&lock_region->ip);

		unlock_event->id = res->id;
		scribe_queue_event(scribe->queue, unlock_event);
	} else {
		struct scribe_event_resource_lock *lock_event;
		lock_event = lock_region->lock_event.regular;
		lock_region->lock_event.extra = NULL;

		lock_event->serial = serial;
		scribe_queue_event_at(&lock_region->ip, lock_event);
		scribe_commit_insert_point(&lock_region->ip);
	}
}

static void __do_unlock_replay(struct scribe_resource *res)
{
	unsigned long serial = atomic_read(&res->serial);
	wait_queue_head_t *q = &res->wait;
	wait_queue_t *wq, *tmp;

	spin_lock(&q->lock);
	list_for_each_entry_safe(wq, tmp, &q->task_list, task_list) {
		struct task_struct *p = wq->private;
		if (p->scribe->waiting_for_serial <= serial)
			wq->func(wq, TASK_NORMAL, 0, NULL);
	}
	spin_unlock(&q->lock);
}

static void do_unlock_replay(struct scribe_ps *scribe,
			     struct scribe_lock_region *lock_region,
			     struct scribe_resource *res)
{
	struct scribe_event_resource_unlock *event;

	__do_unlock_replay(res);

	if (unlikely(is_scribe_context_dead(scribe->ctx)))
		return;

	if (!should_scribe_res_extra(scribe))
		return;

	event = scribe_dequeue_event_specific(scribe,
					      SCRIBE_EVENT_RESOURCE_UNLOCK);
	if (!IS_ERR(event))
		scribe_free_event(event);
}

static void do_unlock(struct scribe_ps *scribe,
		      struct scribe_lock_region *lock_region)
{
	struct scribe_resource *res = lock_region->res;

	if (unlikely(is_detaching(scribe)))
		return;

	atomic_inc(&res->serial);

	if (is_recording(scribe))
		do_unlock_record(scribe, lock_region, res);
	else
		do_unlock_replay(scribe, lock_region, res);
}

static void do_unlock_discard(struct scribe_ps *scribe,
			      struct scribe_lock_region *lock_region)
{
	struct scribe_resource *res = lock_region->res;

	if (unlikely(is_detaching(scribe)))
		return;

	if (is_recording(scribe)) {
		int do_write = lock_region->flags & SCRIBE_WRITE;
		__do_unlock_record(res, do_write);
		scribe_commit_insert_point(&lock_region->ip);
	} else {
		WARN(!is_scribe_context_dead(scribe->ctx),
		     "Discarding resource lock on replay\n");
	}
}

void scribe_assert_no_locked_region(struct scribe_res_user *user)
{
	struct scribe_lock_region *lock_region;
	struct scribe_resource *res;
	char desc[256];

	if (list_empty(&user->locked_regions))
		return;

	printk("Some regions are left locked:\n");

	list_for_each_entry(lock_region, &user->locked_regions, user_node) {
		res = lock_region->res;
		get_description(res, desc, sizeof(desc));
		printk("  Resource id=%d, serial=%d, desc=%s\n",
		       res->id, atomic_read(&res->serial), desc);
	}

	WARN_ON(1);
}

struct scribe_lock_region *scribe_find_lock_region(struct scribe_res_user *user,
						   void *object)
{
	struct scribe_lock_region *lock_region;
	struct scribe_resource *res;

	list_for_each_entry(lock_region, &user->locked_regions, user_node) {
		res = lock_region->res;
		if (res && res->object == object)
			return lock_region;
	}

	return NULL;
}

/* Will always succeed if (@flags & SCRIBE_INTERRUPTIBLE) is not set */
int __lock_object(struct scribe_ps *scribe, struct scribe_lock_arg *arg,
		  void *nested_object)
{
	struct scribe_res_user *user;
	struct scribe_lock_region *lock_region;
	int no_lock = arg->flags & SCRIBE_NO_LOCK;
	int ret = 0;

	/* First we need to check if the resource is tracked */
	scribe_track_resource(scribe->ctx, arg->res);

	user = &scribe->resources;

	lock_region = scribe_get_pre_alloc_lock_region(user);
	lock_region->res = arg->res;
	lock_region->nested_object = nested_object;
	lock_region->flags = arg->flags;

retry:
	if (!no_lock) {
		ret = do_lock(scribe, lock_region);
		/* do_lock() may change the SCRIBE_NO_LOCK flag */
		no_lock = lock_region->flags & SCRIBE_NO_LOCK;
	}

	if (!ret && !no_lock)
		ret = track_user(scribe, lock_region);
	if (ret == -EAGAIN)
		goto retry;

	if (ret)
		scribe_free_lock_region(lock_region);
	else
		list_add(&lock_region->user_node, &user->locked_regions);
	return ret;
}

int __scribe_lock_object(struct scribe_ps *scribe, struct scribe_lock_arg *arg)
{
	return __lock_object(scribe, arg, NULL);
}

int __scribe_lock_objects(struct scribe_ps *scribe,
			  struct scribe_lock_arg *args, int count)
{
	struct scribe_lock_region *lock_region;
	struct scribe_res_user *user;
	void *nested_object;
	int ret = 0;
	int i;

	for (i = 0; i < count; i++) {
		nested_object = NULL;
		if (i != count-1)
			nested_object = args[i+1].res->object;
		/* The recursion level is around 2/3, so we are good */
		ret = __lock_object(scribe, &args[i], nested_object);
		if (ret)
			goto undo;
	}

	return ret;

undo:
	for (i--; i >= 0; i--) {
		user = &scribe->resources;
		lock_region = scribe_find_lock_region(user, args[i].res->object);
		list_del(&lock_region->user_node);
		untrack_user(lock_region);
		do_unlock_discard(scribe, lock_region);

		/* Put back in the pre alloc regions, lock was discarded */
		list_add(&lock_region->user_node, &user->pre_alloc_regions);
		user->num_pre_alloc_regions++;
	}

	return ret;
}

void __scribe_unlock_region(struct scribe_ps *scribe,
			    struct scribe_lock_region *lock_region,
			    bool discard)
{
	struct scribe_res_user *user = &scribe->resources;
	int put_region_back = 0;
	int no_lock;

	if (lock_region->nested_object) {
		__scribe_unlock_object(scribe, lock_region->nested_object,
				       discard);
	}

	no_lock = lock_region->flags & SCRIBE_NO_LOCK;

	list_del(&lock_region->user_node);
	if (!no_lock)
		untrack_user(lock_region);

	if (unlikely(discard)) {
		if (!no_lock)
			do_unlock_discard(scribe, lock_region);
		put_region_back = 1;
	}

	put_region_back |= no_lock;
	if (put_region_back) {
		list_add(&lock_region->user_node, &user->pre_alloc_regions);
		user->num_pre_alloc_regions++;
	} else {
		do_unlock(scribe, lock_region);
		scribe_free_lock_region(lock_region);
	}
}

void __scribe_downgrade_region(struct scribe_ps *scribe,
			       struct scribe_lock_region *lock_region)
{
	int no_lock = lock_region->flags & SCRIBE_NO_LOCK;

	if (!no_lock)
		do_lock_downgrade(scribe, lock_region);
}

void __scribe_assert_locked_object(struct scribe_ps *scribe, void *object)
{
	WARN_ON(!scribe_find_lock_region(&scribe->resources, object));
}
