/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include <linux/scribe.h>
#include <linux/scribe_resource.h>
#include <linux/scribe_container.h>
#include <linux/hash.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/sched.h>
#include <linux/pid_namespace.h>
#include <linux/ipc_namespace.h>
#include <linux/writeback.h>
#include <asm/cmpxchg.h>

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
 * - TODO use a rw_semaphore instead of a mutex to synchronize accesses (the
 *   resource API already use read/write version, but it does the same thing).
 */

#define RES_DESC_MAX PATH_MAX

static inline int should_handle_resources(struct scribe_ps *scribe)
{
	if (!is_scribed(scribe))
		return 0;

	return should_scribe_resources(scribe);
}

#define SCRIBE_ID_RES_HASH_BITS	10
#define SCRIBE_ID_RES_HASH_SIZE	(1 << SCRIBE_ID_RES_HASH_BITS)

/*
 * On demand mapping @id -> @resource. The resources are persistent.
 * The @id is different from the resource id:
 * e.g. we want to map a pid to a resource, but that resource may have a
 * totally different id.
 */
struct scribe_res_map {
	spinlock_t lock;
	int res_type;
	struct hlist_head hash[SCRIBE_ID_RES_HASH_SIZE];
};

struct scribe_idres {
	struct scribe_res_map *map;
	int id;
	struct hlist_node node;
	struct scribe_resource res;
	struct rcu_head rcu;
};

static void scribe_init_res_map(struct scribe_res_map *map, int res_type)
{
	int i;
	spin_lock_init(&map->lock);
	map->res_type = res_type;
	for (i = 0; i < SCRIBE_ID_RES_HASH_SIZE; i++)
		INIT_HLIST_HEAD(&map->hash[i]);
}

static inline unsigned long idres_hashfn(int id)
{
	return hash_long(id, SCRIBE_ID_RES_HASH_BITS);
}

static struct scribe_idres *__find_idres(struct hlist_head *head, int id)
{
	struct scribe_idres *idres;
	struct hlist_node *node;

	hlist_for_each_entry_rcu(idres, node, head, node) {
		if (idres->id == id)
			return idres;
	}
	return NULL;
}

static struct scribe_idres *get_pre_alloc_idres(struct scribe_res_user *user);

/*
 * XXX get_mapped_res() must be followed by __lock_object() to add the
 * resource into the tracked resources (and thus allowing it to be removed)
 */
static struct scribe_idres *get_mapped_res(struct scribe_res_map *map, int id,
					   struct scribe_res_user *user)
{
	struct scribe_idres *idres;
	struct hlist_head *head;

	head = &map->hash[idres_hashfn(id)];

	rcu_read_lock();
	idres = __find_idres(head, id);
	rcu_read_unlock();

	if (idres)
		return idres;

	spin_lock(&map->lock);
	idres = __find_idres(head, id);
	if (unlikely(idres)) {
		spin_unlock(&map->lock);
		return idres;
	}

	idres = get_pre_alloc_idres(user);
	idres->map = map;
	idres->id = id;
	scribe_init_resource(&idres->res, map->res_type);

	hlist_add_head_rcu(&idres->node, head);
	spin_unlock(&map->lock);

	return idres;
}

static void free_rcu_idres(struct rcu_head *rcu)
{
	struct scribe_idres *idres;
	idres = container_of(rcu, struct scribe_idres, rcu);
	kfree(idres);
}

static void remove_idres(struct scribe_idres *idres)
{
	struct scribe_res_map *map = idres->map;

	spin_lock(&map->lock);
	hlist_del_rcu(&idres->node);
	spin_unlock(&map->lock);
	call_rcu(&idres->rcu, free_rcu_idres);
}

struct scribe_resources {
	/*
	 * Only resources with a potentially > serial will be in that list.
	 * In other words, only used resources are kept in that list.
	 */
	spinlock_t lock;
	int next_id;
	struct list_head tracked;

	struct scribe_res_map pid_map;
};

struct scribe_resources *scribe_alloc_resources(void)
{
	struct scribe_resources *resources;

	resources = kmalloc(sizeof(*resources), GFP_KERNEL);
	if (!resources)
		return NULL;

	spin_lock_init(&resources->lock);
	resources->next_id = 0;
	INIT_LIST_HEAD(&resources->tracked);

	scribe_init_res_map(&resources->pid_map, SCRIBE_RES_TYPE_PID);

	return resources;
}


struct scribe_resource_handle {
	struct scribe_handle handle;
	struct scribe_resource res;
};

static inline int use_spinlock(struct scribe_resource *res)
{
	return res->type & SCRIBE_RES_SPINLOCK;
}

void scribe_init_resource(struct scribe_resource *res, int type)
{
	res->ctx = NULL;
	res->on_reset = NULL;
	res->id = -1; /* The id will be set once the resource is tracked */
	res->type = type;

	res->first_read_serial = -1;
	atomic_set(&res->serial, 0);

	if (use_spinlock(res))
		spin_lock_init(&res->lock.spinlock);
	else
		init_rwsem(&res->lock.semaphore);

	init_waitqueue_head(&res->wait);
}

static int on_reset_idres(struct scribe_context *ctx,
			  struct scribe_resource *res)
{
	struct scribe_idres *idres;
	idres = container_of(res, struct scribe_idres, res);
	remove_idres(idres);
	return 0;
}

static int on_reset_hres(struct scribe_context *ctx,
			 struct scribe_resource *res)
{
	struct scribe_resource_handle *hres;
	hres = container_of(res, struct scribe_resource_handle, res);
	remove_scribe_handle(&hres->handle);
	return 0;
}

static struct inode *__get_inode_from_res(struct scribe_resource *res)
{
	struct scribe_resource_handle *hres;
	struct scribe_container *container;
	struct inode *inode;

	hres = container_of(res, struct scribe_resource_handle, res);
	container = hres->handle.container;
	inode = container_of(container, struct inode, i_scribe_resource);

	return inode;
}

static void on_create_res_inode(struct scribe_resource *res)
{
	struct inode *inode = __get_inode_from_res(res);
	spin_lock(&inode_lock);
	__iget(inode);
	spin_unlock(&inode_lock);
}

static int on_reset_res_inode(struct scribe_context *ctx,
			      struct scribe_resource *res)
{
	struct inode *inode = __get_inode_from_res(res);
	on_reset_hres(ctx, res);
	spin_unlock(&ctx->resources->lock);
	/* iput sleeps */
	iput(inode);
	return -EAGAIN;
}

static void track_resource(struct scribe_context *ctx,
			   struct scribe_resource *res)
{
	struct scribe_resources *resources;
	void (*on_create) (struct scribe_resource *);
	int (*on_reset) (struct scribe_context *, struct scribe_resource *);

	if (res->ctx) {
		BUG_ON(res->ctx != ctx);
		return;
	}

	on_create = NULL;
	on_reset = NULL;
	switch (res->type & SCRIBE_RES_TYPE_MASK) {
	case SCRIBE_RES_TYPE_INODE:
		on_create = on_create_res_inode;
		on_reset = on_reset_res_inode;
		break;
	case SCRIBE_RES_TYPE_FUTEX:
		on_reset = on_reset_hres;
		break;
	case SCRIBE_RES_TYPE_PID:
		on_reset = on_reset_idres;
		break;
	}

	resources = ctx->resources;
	spin_lock(&resources->lock);
	if (likely(!res->ctx)) {
		BUG_ON(atomic_read(&res->serial));
		res->ctx = ctx;
		res->id = resources->next_id++;
		list_add(&res->node, &resources->tracked);
		res->on_reset = on_reset;
		if (on_create)
			on_create(res);
	}
	spin_unlock(&resources->lock);
}

static int __scribe_reset_resource(struct scribe_resource *res)
{
	struct scribe_context *ctx;
	int ret = 0;

	ctx = res->ctx;
	res->ctx = NULL;
	list_del(&res->node);

	res->first_read_serial = -1;
	atomic_set(&res->serial, 0);

	if (res->on_reset)
		ret = res->on_reset(ctx, res);

	return ret;
}

void scribe_reset_resource(struct scribe_resource *res)
{
	int ret;
	struct scribe_resources *resources;

	if (!res->ctx)
		return;
	resources = res->ctx->resources;

	spin_lock(&resources->lock);
	ret = __scribe_reset_resource(res);
	if (ret == -EAGAIN)
		return;
	spin_unlock(&resources->lock);
}

void scribe_reset_resource_container(struct scribe_container *container)
{
	struct scribe_resource_handle *hres;
	struct scribe_resources *resources;
	struct scribe_context *ctx;
	int ret;

retry:
	rcu_read_lock();
	if (list_empty(&container->handles)) {
		rcu_read_unlock();
		return;
	}

	hres = list_first_entry_rcu(&container->handles,
				    struct scribe_resource_handle, handle.node);
	ctx = hres->res.ctx;

	/*
	 * ctx should always be valid: we are in a place where processes
	 * cannot add any handles to the list (umount).
	 */
	BUG_ON(!ctx);
	resources = ctx->resources;

	spin_lock(&resources->lock);
	rcu_read_unlock();

	ret = __scribe_reset_resource(&hres->res);
	if (ret == -EAGAIN)
		goto retry;
	spin_unlock(&resources->lock);
	goto retry;
}

void scribe_reset_resources(struct scribe_resources *resources)
{
	struct scribe_resource *res, *tmp;
	int ret;

retry:
	spin_lock(&resources->lock);
	list_for_each_entry_safe(res, tmp, &resources->tracked, node) {
		ret = __scribe_reset_resource(res);
		if (ret == -EAGAIN) {
			/* The lock was dropped */
			goto retry;
		}
	}
	spin_unlock(&resources->lock);
}

void scribe_free_resources(struct scribe_resources *resources)
{
	scribe_reset_resources(resources);
	/*
	 * XXX There is no possible race with scribe_reset_resource() since
	 * all potential processes that could call scribe_reset_resource() and
	 * scribe_reset_resource_container() are gone.
	 */
	kfree(resources);
}

struct scribe_lock_region {
	struct list_head node;
	scribe_insert_point_t ip;
	union {
		struct scribe_event *generic;
		struct scribe_event_resource_lock *regular;
		struct scribe_event_resource_lock_intr *intr;
		struct scribe_event_resource_lock_extra *extra;
	} lock_event;
	struct scribe_event_resource_unlock *unlock_event;
	struct scribe_resource *res;
	void *object;
	int flags;
};

static void free_lock_region(struct scribe_lock_region *lock_region);

static struct scribe_lock_region *alloc_lock_region(int doing_recording,
						    int res_extra)
{
	struct scribe_lock_region *lock_region;

	lock_region = kmalloc(sizeof(*lock_region), GFP_KERNEL);
	if (!lock_region)
		return NULL;

	lock_region->lock_event.generic = NULL;
	lock_region->unlock_event = NULL;

	if (!doing_recording) {
		/*
		 * During replaying, we don't really need those events, since
		 * they will be coming from the event pump.
		 */
		return lock_region;
	}

	if (res_extra) {
		/*
		 * We don't know how long will be the description, we'll
		 * assume the maximum.
		 */
		lock_region->lock_event.extra = scribe_alloc_event_sized(
				SCRIBE_EVENT_RESOURCE_LOCK_EXTRA, RES_DESC_MAX);
	} else
		lock_region->lock_event.regular = scribe_alloc_event(
				SCRIBE_EVENT_RESOURCE_LOCK);

	if (!lock_region->lock_event.generic)
		goto err;

	if (res_extra) {
		lock_region->unlock_event = scribe_alloc_event(
						SCRIBE_EVENT_RESOURCE_UNLOCK);
		if (!lock_region->unlock_event)
			goto err;
	}

	return lock_region;

err:
	free_lock_region(lock_region);
	return NULL;
}

/* Use this when you didn't had the chance to lock()/unlock() the resource */
static void free_lock_region(struct scribe_lock_region *lock_region)
{
	scribe_free_event(lock_region->lock_event.generic);
	scribe_free_event(lock_region->unlock_event);
	kfree(lock_region);
}

void scribe_resource_init_user(struct scribe_res_user *user)
{
	INIT_HLIST_HEAD(&user->pre_alloc_idres);
	user->num_pre_alloc_idres = 0;
	INIT_LIST_HEAD(&user->pre_alloc_hres);
	user->num_pre_alloc_hres = 0;
	INIT_LIST_HEAD(&user->pre_alloc_regions);
	user->num_pre_alloc_regions = 0;
	INIT_LIST_HEAD(&user->locked_regions);
}

/*
 * We need at most 4 lock_regions pre allocated upfront, e.g in fd_install():
 * Two for the open/close region on the inode registration, and one for the
 * files_struct.
 */
#define MAX_PRE_ALLOC 4

int scribe_resource_pre_alloc(struct scribe_res_user *user,
			      int doing_recording, int res_extra)
{
	struct scribe_idres *idres;
	struct scribe_lock_region *lock_region;
	struct scribe_resource_handle *hres;

	while (user->num_pre_alloc_idres < MAX_PRE_ALLOC) {
		idres = kmalloc(sizeof(*idres), GFP_KERNEL);
		if (!idres)
			return -ENOMEM;

		hlist_add_head(&idres->node, &user->pre_alloc_idres);
		user->num_pre_alloc_idres++;
	}

	while (user->num_pre_alloc_hres < MAX_PRE_ALLOC) {
		hres = kmalloc(sizeof(*hres), GFP_KERNEL);
		if (!hres)
			return -ENOMEM;

		list_add(&hres->handle.node, &user->pre_alloc_hres);
		user->num_pre_alloc_hres++;
	}

	while (user->num_pre_alloc_regions < MAX_PRE_ALLOC) {
		lock_region = alloc_lock_region(doing_recording, res_extra);
		if (!lock_region)
			return -ENOMEM;

		list_add(&lock_region->node, &user->pre_alloc_regions);
		user->num_pre_alloc_regions++;
	}

	return 0;
}

static int __resource_prepare(struct scribe_ps *scribe)
{
	might_sleep();

	if (scribe_resource_pre_alloc(&scribe->resources, is_recording(scribe),
				      should_scribe_res_extra(scribe)))
		return -ENOMEM;

	return 0;
}

int scribe_resource_prepare(void)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return 0;

	return __resource_prepare(scribe);
}

void scribe_assert_no_locked_region(struct scribe_res_user *user)
{
	WARN(!list_empty(&user->locked_regions),
	     "Some regions are left unlocked\n");
}

void scribe_resource_exit_user(struct scribe_res_user *user)
{
	struct scribe_idres *idres;
	struct hlist_node *tmp1, *tmp2;
	struct scribe_lock_region *lockr, *ltmp;
	struct scribe_resource_handle *hres, *htmp;

	scribe_assert_no_locked_region(user);

	hlist_for_each_entry_safe(idres, tmp1, tmp2,
				 &user->pre_alloc_idres, node) {
		hlist_del(&idres->node);
		kfree(idres);
	}

	list_for_each_entry_safe(hres, htmp,
				 &user->pre_alloc_hres, handle.node) {
		list_del(&hres->handle.node);
		kfree(hres);
	}

	list_for_each_entry_safe(lockr, ltmp,
				 &user->pre_alloc_regions, node) {
		list_del(&lockr->node);
		free_lock_region(lockr);
	}
}

static struct scribe_idres *get_pre_alloc_idres(struct scribe_res_user *user)
{
	struct scribe_idres *idres;
	BUG_ON(hlist_empty(&user->pre_alloc_idres));
	idres = hlist_entry(user->pre_alloc_idres.first,
			    struct scribe_idres, node);
	hlist_del(&idres->node);
	user->num_pre_alloc_idres--;
	return idres;
}

static struct scribe_resource_handle *get_pre_alloc_hres(
						struct scribe_res_user *user)
{
	struct scribe_resource_handle *hres;
	BUG_ON(list_empty(&user->pre_alloc_hres));
	hres = list_first_entry(&user->pre_alloc_hres,
				struct scribe_resource_handle, handle.node);
	list_del(&hres->handle.node);
	user->num_pre_alloc_hres--;
	return hres;
}

static struct scribe_lock_region *get_pre_alloc_lock_region(
						struct scribe_res_user *user)
{
	struct scribe_lock_region *lock_region;
	BUG_ON(list_empty(&user->pre_alloc_regions));
	lock_region = list_first_entry(&user->pre_alloc_regions,
				       struct scribe_lock_region, node);
	list_del(&lock_region->node);
	user->num_pre_alloc_regions--;
	return lock_region;
}

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

#ifdef CONFIG_DEBUG_LOCK_ALLOC
static int get_lockdep_subclass(int type)
{
	/* MAX_LOCKDEP_SUBCLASSES is small, trying not to overflow it */
	return type & SCRIBE_RES_TYPE_MASK;
}
#else
static inline int get_lockdep_subclass(int type)
{
	return 0;
}
#endif

static size_t get_lock_region_desc(struct scribe_ps *scribe,
				   char *buffer, size_t size,
				   struct scribe_lock_region *lock_region)
{
	int type = lock_region->res->type & SCRIBE_RES_TYPE_MASK;
	struct file *file;
	char *tmp, *pathname;
	ssize_t ret;

	switch (type) {
	case SCRIBE_RES_TYPE_FILE:
		file = lock_region->object;

		tmp = (char *)__get_free_page(GFP_TEMPORARY);
		if (!tmp) {
			return snprintf(buffer, size,
					"memory allocation failed");
		}

		scribe->do_dpath_scribing = false;
		pathname = d_path(&file->f_path, tmp, PAGE_SIZE);
		scribe->do_dpath_scribing = true;
		if (IS_ERR(pathname)) {
			ret = snprintf(buffer, size, "d_path failed with %ld",
				       PTR_ERR(pathname));
		} else
			ret = snprintf(buffer, size, "%s", pathname);

		free_page((unsigned long)tmp);
		break;
	default:
		ret = snprintf(buffer, size, "none");
	}

	return ret;
}

static int __do_lock_record(struct scribe_ps *scribe,
			    struct scribe_resource *res,
			    int do_write, int do_intr)
{
	int class;
	int ret;

	class = get_lockdep_subclass(res->type);

	if (use_spinlock(res)) {
		spin_lock_nested(&res->lock.spinlock, class);
		return 0;
	}

	if (!do_intr) {
		if (do_write)
			down_write_nested(&res->lock.semaphore, class);
		else
			down_read_nested(&res->lock.semaphore, class);
		return 0;
	}

	if (do_write)
		ret = wait_event_interruptible_exclusive(res->wait,
		  down_write_trylock_nested(&res->lock.semaphore, class));
	else
		ret = wait_event_interruptible(res->wait,
		  down_read_trylock_nested(&res->lock.semaphore, class));

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

static int do_lock_record(struct scribe_ps *scribe,
			  struct scribe_lock_region *lock_region,
			  struct scribe_resource *res)
{
	struct scribe_event_resource_lock_intr *event;
	int do_intr = lock_region->flags & SCRIBE_INTERRUPTIBLE;
	int do_write = lock_region->flags & SCRIBE_WRITE;

	scribe_create_insert_point(&lock_region->ip, &scribe->queue->stream);

	if (__do_lock_record(scribe, res, do_write, do_intr)) {
		/* Interrupted ... */
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
		/* emergency_stop() has been triggered, we need to leave */
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
	struct scribe_resource *res = lock_region->res;
	might_sleep();

	if (!is_locking_necessary(scribe, res))
		return 0;

	if (unlikely(is_detaching(scribe))) {
		if (lock_region->flags & SCRIBE_INTERRUPTIBLE)
			return -EINTR;
		return 0;
	}

	BUG_ON(!(lock_region->flags & (SCRIBE_READ|SCRIBE_WRITE)));

	if (is_recording(scribe))
		return do_lock_record(scribe, lock_region, res);
	else
		return do_lock_replay(scribe, lock_region, res);
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

	if (!is_locking_necessary(scribe, res))
		return;

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

		/* We need to wake the ones in wait_event_interruptible */
		wake_up(&res->wait);
	}
}

static void do_unlock_record(struct scribe_ps *scribe,
			     struct scribe_lock_region *lock_region,
			     struct scribe_resource *res)
{
	int do_write = lock_region->flags & SCRIBE_WRITE;
	unsigned long serial;
	size_t size;

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

		size = get_lock_region_desc(scribe,
					    lock_event->desc, RES_DESC_MAX,
					    lock_region);
		lock_event->h.size = size;

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

	if (!is_locking_necessary(scribe, res))
		return;

	if (unlikely(is_detaching(scribe)))
		return;

	atomic_inc(&res->serial);

	if (is_recording(scribe))
		do_unlock_record(scribe, lock_region, res);
	else
		do_unlock_replay(scribe, lock_region, res);

	might_sleep();
}

static void do_unlock_discard(struct scribe_ps *scribe,
			      struct scribe_lock_region *lock_region)
{
	struct scribe_resource *res = lock_region->res;

	if (!is_locking_necessary(scribe, res))
		return;

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

/* Will always succeed if (@flags & SCRIBE_INTERRUPTIBLE) is not set */
static int __lock_object(struct scribe_ps *scribe,
			 void *object, struct scribe_resource *res, int flags)
{
	struct scribe_res_user *user;
	struct scribe_lock_region *lock_region;
	int ret;

	/* First we need to check if the resource is tracked */
	track_resource(scribe->ctx, res);

	user = &scribe->resources;

	lock_region = get_pre_alloc_lock_region(user);
	lock_region->res = res;
	lock_region->object = object;
	lock_region->flags = flags;

	ret = do_lock(scribe, lock_region);
	if (ret)
		free_lock_region(lock_region);
	else
		list_add(&lock_region->node, &user->locked_regions);
	return ret;
}

void scribe_lock_object(void *object, struct scribe_resource *res, int flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	__lock_object(scribe, object, res, flags);
}

static int __lock_id(struct scribe_ps *scribe,
		     struct scribe_res_map *map, int id, int flags)
{
	struct scribe_idres *idres;
	idres = get_mapped_res(map, id, &scribe->resources);

	/*
	 * FIXME We need an object to be able to find the lock region in
	 * scribe_unlock(), and unlock the resource.
	 * Unfortunately, we only have an id, no unique pointer.
	 * We'll cast the id to a pointer, but this is limiting:
	 * we cannot lock the same id from two different maps at the same
	 * time. For now this is not a problem.
	 */

	return __lock_object(scribe, (void *)id, &idres->res, flags);
}

static void free_resource_handle(struct scribe_handle *handle)
{
	struct scribe_resource_handle *hres;
	hres = container_of(handle, struct scribe_resource_handle, handle);
	kfree(hres);
}

struct get_new_arg {
	int type;
	struct scribe_res_user *user;
	int created;
};

static struct scribe_resource_handle *get_pre_alloc_hres(
						struct scribe_res_user *user);
static struct scribe_handle *get_new_resource_handle(void *_arg)
{
	struct get_new_arg *arg = _arg;
	struct scribe_resource_handle *hres;

	hres = get_pre_alloc_hres(arg->user);
	scribe_init_resource(&hres->res, arg->type);
	arg->created = 1;
	return &hres->handle;
}

/*
 * XXX get_resource_handle() must be followed by __lock_object() to add the
 * resource into the tracked resources (and thus allowing it to be removed)
 */
static struct scribe_resource_handle *get_resource_handle(
		struct scribe_context *ctx,
		struct scribe_container *container,
		int type, struct scribe_res_user *user)
{
	struct scribe_handle *handle;
	struct scribe_handle_ctor ctor;
	struct get_new_arg arg;

	arg.type = type;
	arg.user = user;
	arg.created = 0;

	ctor.get_new = get_new_resource_handle;
	ctor.arg = &arg;
	ctor.free = free_resource_handle;

	handle = get_scribe_handle(container, ctx, &ctor);

	return container_of(handle, struct scribe_resource_handle, handle);
}

static int __lock_object_handle(struct scribe_ps *scribe, void *object,
				struct scribe_container *container,
				int type, int flags)
{
	struct scribe_resource_handle *hres;

	hres = get_resource_handle(scribe->ctx, container,
				   type, &scribe->resources);
	return __lock_object(scribe, object, &hres->res, flags);
}

void scribe_lock_object_handle(void *object,
		struct scribe_container *container, int type, int flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	__lock_object_handle(scribe, object, container, type, flags);
}

static inline struct inode *file_inode(struct file *file)
{
	return file->f_path.dentry->d_inode;
}

static struct scribe_lock_region *find_locked_region(
						struct scribe_res_user *user,
						void *object)
{
	struct scribe_lock_region *lock_region;

	list_for_each_entry(lock_region, &user->locked_regions, node) {
		if (lock_region->object == object)
			return lock_region;
	}
	return NULL;
}

static void scribe_unlock_region(struct scribe_ps *scribe,
				 struct scribe_res_user *user,
				 struct scribe_lock_region *lock_region,
				 int err)
{
	struct file *file;

	list_del(&lock_region->node);

	if (lock_region->flags & (SCRIBE_INODE_READ | SCRIBE_INODE_WRITE)) {
		file = lock_region->object;
		scribe_unlock_err(file_inode(file), err);
	}

	if (likely(!IS_ERR_VALUE(err))) {
		do_unlock(scribe, lock_region);
		free_lock_region(lock_region);
	} else {
		do_unlock_discard(scribe, lock_region);
		list_add(&lock_region->node, &user->pre_alloc_regions);
	}
}

void scribe_unlock_all_resources(struct scribe_ps *scribe)
{
	struct scribe_res_user *user = &scribe->resources;
	struct scribe_lock_region *lock_region, *tmp;

	list_for_each_entry_safe(lock_region, tmp,
				 &user->locked_regions, node) {
		scribe_unlock_region(scribe, user, lock_region, 0);
	}
}

void scribe_unlock_err(void *object, int err)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_lock_region *lock_region;
	struct scribe_res_user *user;

	if (!should_handle_resources(scribe))
		return;

	user = &scribe->resources;
	lock_region = find_locked_region(user, object);
	BUG_ON(!lock_region);

	scribe_unlock_region(scribe, user, lock_region, err);
}

void scribe_unlock(void *object)
{
	scribe_unlock_err(object, 0);
}

void scribe_unlock_discard(void *object)
{
	scribe_unlock_err(object, -EAGAIN);
}

void scribe_downgrade(void *object)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_res_user *user;
	struct scribe_lock_region *lock_region;

	if (!should_handle_resources(scribe))
		return;

	user = &scribe->resources;
	lock_region = find_locked_region(user, object);
	BUG_ON(!lock_region);

	do_lock_downgrade(scribe, lock_region);
}

void scribe_assert_locked(void *object)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	WARN_ON(!find_locked_region(&scribe->resources, object));
}

static inline int inode_need_reg_sync(struct inode *inode)
{
	umode_t mode = inode->i_mode;
	/*
	 * For fifos and sockets, we don't need to synchronize open/close
	 * because once closed permanantly (ref_cnt reaches 0), they cannot be
	 * reopened: the open/close race cannot happen.
	 */
	return !(S_ISFIFO(mode) || S_ISSOCK(mode));
}

static inline int inode_need_explicit_locking(struct file *file,
					      struct inode *inode)
{
	umode_t mode;

	if (file->f_op->scribe_need_explicit_inode_lock)
		return file->f_op->scribe_need_explicit_inode_lock(file);

	/*
	 * For fifos and sockets, each endpoint has to be locked independently
	 * (otherwise deadlocks could happen when the buffer is full...).
	 * It's also better in terms of performance.
	 */
	mode = inode->i_mode;
	return S_ISFIFO(mode) || S_ISSOCK(mode);
}

static int __lock_inode(struct scribe_ps *scribe,
			struct inode *inode, int flags)
{
	return __lock_object_handle(scribe, inode,
				    &inode->i_scribe_resource,
				    SCRIBE_RES_TYPE_INODE,
				    flags);
}

static int lock_file(struct file *file, int flags)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_res_user *user;
	struct scribe_lock_region *lock_region;
	struct inode *inode;
	int intr;

	if (!should_handle_resources(scribe))
		return 0;

	inode = file_inode(file);
	if (inode_need_explicit_locking(file, inode) &&
	    !(flags & SCRIBE_INODE_EXPLICIT))
		flags &= ~(SCRIBE_INODE_READ | SCRIBE_INODE_WRITE);

	if (__lock_object(scribe, file, &file->scribe_resource, flags))
		return -EINTR;

	if (flags & SCRIBE_INODE_READ)
		flags = SCRIBE_READ;
	else if (flags & SCRIBE_INODE_WRITE)
		flags = SCRIBE_WRITE;
	else
		return 0;

	/*
	 * We may associate the inode_lock_obj with the file, because this is
	 * what gets passed to the scribe_unlock() function.
	 * This is how the inode will get unlocked.
	 */
	intr = __lock_inode(scribe, inode, flags);
	if (intr) {
		user = &scribe->resources;
		lock_region = find_locked_region(user, file);
		/* Was in locked_regions */
		list_del(&lock_region->node);
		do_unlock_discard(scribe, lock_region);
		/* Put back int the pre alloc regions, lock was discarded */
		list_add(&lock_region->node, &user->pre_alloc_regions);
	}

	return intr;
}

void scribe_lock_file_no_inode(struct file *file)
{
	lock_file(file, SCRIBE_WRITE);
}

void scribe_lock_file_read(struct file *file)
{
	lock_file(file, SCRIBE_WRITE | SCRIBE_INODE_READ);
}

void scribe_lock_file_write(struct file *file)
{
	lock_file(file, SCRIBE_WRITE | SCRIBE_INODE_WRITE);
}

int scribe_lock_file_read_interruptible(struct file *file)
{
	return lock_file(file, SCRIBE_INTERRUPTIBLE |
				 SCRIBE_WRITE | SCRIBE_INODE_READ);
}

int scribe_lock_file_write_interruptible(struct file *file)
{
	return lock_file(file, SCRIBE_INTERRUPTIBLE |
				 SCRIBE_WRITE | SCRIBE_INODE_WRITE);
}

static void lock_inode(struct inode *inode, int flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	__lock_inode(scribe, inode, flags);
}

void scribe_lock_inode_read(struct inode *inode)
{
	lock_inode(inode, SCRIBE_READ);
}

void scribe_lock_inode_write(struct inode *inode)
{
	lock_inode(inode, SCRIBE_WRITE);
}

static int __track_next_file(int flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return 0;

	if (__resource_prepare(scribe))
		return -ENOMEM;

	scribe->lock_next_file = flags;
	scribe->was_file_locking_interrupted = false;
	return 0;
}

int scribe_track_next_file_no_inode(void)
{
	return __track_next_file(SCRIBE_WRITE);
}

int scribe_track_next_file_read(void)
{
	return __track_next_file(SCRIBE_WRITE | SCRIBE_INODE_READ);
}

int scribe_track_next_file_write(void)
{
	return __track_next_file(SCRIBE_WRITE | SCRIBE_INODE_WRITE);
}

int scribe_track_next_file_explicit_inode_read(void)
{
	return __track_next_file(SCRIBE_WRITE | SCRIBE_INODE_EXPLICIT |
				 SCRIBE_INODE_READ);
}

int scribe_track_next_file_explicit_inode_write(void)
{
	return __track_next_file(SCRIBE_WRITE | SCRIBE_INODE_EXPLICIT |
				 SCRIBE_INODE_WRITE);
}

int scribe_track_next_file_read_interruptible(void)
{
	return __track_next_file(SCRIBE_INTERRUPTIBLE |
				 SCRIBE_WRITE | SCRIBE_INODE_READ);
}

int scribe_track_next_file_write_interruptible(void)
{
	return __track_next_file(SCRIBE_INTERRUPTIBLE |
				 SCRIBE_WRITE | SCRIBE_INODE_WRITE);
}

void scribe_pre_fget(struct files_struct *files, int *lock_flags)
{
	struct scribe_ps *scribe = current->scribe;

	*lock_flags = 0;

	if (!is_scribed(scribe))
		return;

	if (scribe->lock_next_file) {
		*lock_flags = scribe->lock_next_file;
		scribe->lock_next_file = 0;

		/*
		 * We need to lock the files_struct while doing fcheck_files()
		 * to guards against races with fd_install()
		 */
		scribe_lock_files_read(files);
	}
}

int scribe_post_fget(struct files_struct *files, struct file *file,
		      int lock_flags)
{
	if (!lock_flags)
		return 0;

	scribe_unlock(files);

	if (!file)
		return 0;

	if (lock_file(file, lock_flags)) {
		current->scribe->was_file_locking_interrupted = true;
		return -EINTR;
	}

	current->scribe->locked_file = file;
	return 0;
}

void scribe_pre_fput(struct file *file)
{
	struct scribe_ps *scribe = current->scribe;

	if (!is_scribed(scribe))
		return;

	if (scribe->locked_file) {
		scribe_unlock(scribe->locked_file);
		scribe->locked_file = NULL;
	}
}

bool scribe_was_file_locking_interrupted(void)
{
	struct scribe_ps *scribe = current->scribe;

	if (!may_be_scribed(scribe))
		return false;

	return scribe->was_file_locking_interrupted;
}

static void lock_files(struct files_struct *files, int flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	__lock_object(scribe, files, &files->scribe_resource, flags);
}

void scribe_lock_files_read(struct files_struct *files)
{
	lock_files(files, SCRIBE_READ);
}

void scribe_lock_files_write(struct files_struct *files)
{
	lock_files(files, SCRIBE_WRITE);
}

static void lock_pid(pid_t pid, int flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	__lock_id(scribe, &scribe->ctx->resources->pid_map, pid, flags);
}

void scribe_lock_pid_read(pid_t pid)
{
	lock_pid(pid, SCRIBE_READ);
}

void scribe_lock_pid_write(pid_t pid)
{
	lock_pid(pid, SCRIBE_WRITE);
}

void scribe_unlock_pid(pid_t pid)
{
	scribe_unlock((void *)pid);
}

void scribe_unlock_pid_discard(pid_t pid)
{
	scribe_unlock_discard((void *)pid);
}

void scribe_lock_ipc(struct ipc_namespace *ns)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	/* For now all IPC things are synchronized on the same resource */
	__lock_object(scribe, ns, &ns->scribe_resource, SCRIBE_WRITE);
}

static void lock_mmap(struct mm_struct *mm, unsigned long flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	__lock_object(scribe, mm, &mm->scribe_mmap_res, flags);
}

void scribe_lock_mmap_read(struct mm_struct *mm)
{
	lock_mmap(mm, SCRIBE_READ);
}

void scribe_lock_mmap_write(struct mm_struct *mm)
{
	lock_mmap(mm, SCRIBE_WRITE);
}
