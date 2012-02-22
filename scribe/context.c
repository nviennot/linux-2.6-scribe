/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include <linux/sched.h>
#include <linux/seq_file.h>
#include <linux/jiffies.h>
#include <linux/nsproxy.h>
#include <linux/scribe.h>

struct scribe_context *scribe_alloc_context(void)
{
	struct scribe_context *ctx;

	ctx = kmalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return NULL;

	atomic_set(&ctx->ref_cnt, 1);
	ctx->id = current->pid;
	ctx->flags = 0;

	spin_lock_init(&ctx->tasks_lock);
	INIT_LIST_HEAD(&ctx->tasks);
	init_waitqueue_head(&ctx->tasks_wait);
	ctx->max_num_tasks = 0;
	ctx->num_tasks = 0;
	ctx->init_nsproxy = NULL;
	ctx->monitor_nsproxy = NULL;

	spin_lock_init(&ctx->queues_lock);
	INIT_LIST_HEAD(&ctx->queues);
	init_waitqueue_head(&ctx->queues_wait);
	ctx->last_event_jiffies = jiffies;

	scribe_init_stream(&ctx->notifications);

	ctx->idle_event = NULL;
	ctx->diverge_event = NULL;
	ctx->last_error = 0;

	spin_lock_init(&ctx->backtrace_lock);
	ctx->backtrace = NULL;

	ctx->res_ctx = scribe_alloc_res_context();
	if (!ctx->res_ctx)
		goto err_ctx;

	ctx->bmark = scribe_bookmark_alloc(ctx);
	if (!ctx->bmark)
		goto err_resources;

	ctx->mm_ctx = scribe_alloc_mm_context();
	if (!ctx->mm_ctx)
		goto err_bmark;

	return ctx;

err_bmark:
	scribe_bookmark_free(ctx->bmark);
err_resources:
	scribe_free_res_context(ctx->res_ctx);
err_ctx:
	kfree(ctx);
	return NULL;
}

static void wait_for_ctx_empty(struct scribe_context *ctx)
{
	struct scribe_queue *queue, *tmp;

	/* No locks are needed: from now on, tasks cannot be added */
	wait_event(ctx->tasks_wait, list_empty(&ctx->tasks));

	spin_lock(&ctx->queues_lock);
	list_for_each_entry_safe(queue, tmp, &ctx->queues, node)
		scribe_unset_persistent(queue);
	spin_unlock(&ctx->queues_lock);

	wait_event(ctx->queues_wait, list_empty(&ctx->queues));
}

static void clear_nsproxies(struct scribe_context *ctx);
void scribe_free_context(struct scribe_context *ctx)
{
	scribe_kill(ctx, 0);
	wait_for_ctx_empty(ctx);

	scribe_free_all_events(&ctx->notifications);
	scribe_free_mm_context(ctx->mm_ctx);
	scribe_free_res_context(ctx->res_ctx);
	scribe_bookmark_free(ctx->bmark);
	clear_nsproxies(ctx);

	scribe_put_context(ctx);
}

static void setup_init_nsproxy(struct scribe_context *ctx,
			       struct scribe_ps *scribe)
{
	assert_spin_locked(&ctx->tasks_lock);

	if (!ctx->init_nsproxy) {
		ctx->init_nsproxy = scribe->p->nsproxy;
		get_nsproxy(ctx->init_nsproxy);
	}
}

static void setup_monitor_nsproxy(struct scribe_context *ctx)
{
	assert_spin_locked(&ctx->tasks_lock);

	if (!ctx->monitor_nsproxy) {
		ctx->monitor_nsproxy = current->nsproxy;
		get_nsproxy(ctx->monitor_nsproxy);
	}
}

static void clear_nsproxies(struct scribe_context *ctx)
{
	if (ctx->init_nsproxy) {
		put_nsproxy(ctx->init_nsproxy);
		ctx->init_nsproxy = NULL;
	}

	if (ctx->monitor_nsproxy) {
		put_nsproxy(ctx->monitor_nsproxy);
		ctx->monitor_nsproxy = NULL;
	}
}

static int context_start(struct scribe_context *ctx, unsigned long flags,
			 struct scribe_event_context_idle *idle_event,
			 struct scribe_event_diverge *diverge_event,
			 struct scribe_backtrace *backtrace)
{
	assert_spin_locked(&ctx->tasks_lock);

	if (!is_scribe_context_dead(ctx))
		return -EPERM;

	/*
	 * The task list might not be empty (just got an scribe_kill()).
	 */
	wait_for_ctx_empty(ctx);

	ctx->max_num_tasks = ctx->num_tasks;

	ctx->queues_sealed = 0;

	ctx->idle_event = idle_event;
	ctx->diverge_event = diverge_event;
	ctx->backtrace = backtrace;

	ctx->flags = flags;
	ctx->last_error = 0;

	/*
	 * TODO reset only when context_start() isn't called for the first
	 * time.
	 */
	scribe_reset_resources(ctx->res_ctx);

	atomic_set(&ctx->signal_cookie, 0);

	clear_nsproxies(ctx); /* cleanup from a previous run */
	setup_monitor_nsproxy(ctx);

	return 0;
}

/*
 * This is the place to put the context to an idle state.
 * It is assume that the context is not idle.
 * @reason can be:
 * - NULL: everything went well.
 * - an error (IS_ERR(reason) == 1): something bad happened, such as -ENODATA.
 * - a diverge event: a specific diverge error happened.
 *
 */
static void context_idle(struct scribe_context *ctx,
			 struct scribe_event *reason)
{
	struct scribe_backtrace *backtrace;
	struct scribe_queue *queue;
	unsigned long flags;

	assert_spin_locked(&ctx->tasks_lock);

	if (ctx->flags & SCRIBE_REPLAY) {
		spin_lock(&ctx->queues_lock);
		list_for_each_entry(queue, &ctx->queues, node)
			scribe_kill_queue(queue);
		ctx->queues_sealed = 1;
		spin_unlock(&ctx->queues_lock);
	}

	ctx->flags &= ~SCRIBE_STATE_MASK;

	spin_lock_irqsave(&ctx->backtrace_lock, flags);
	backtrace = ctx->backtrace;
	ctx->backtrace = NULL;
	spin_unlock_irqrestore(&ctx->backtrace_lock, flags);

	if (backtrace) {
		/* We don't dump the backtrace if everything went well */
		if (reason)
			scribe_backtrace_dump(backtrace, &ctx->notifications);
		scribe_free_backtrace(backtrace);
	}

	if (IS_ERR_OR_NULL(reason))
		ctx->last_error = ctx->idle_event->error = PTR_ERR(reason);
	else {
		ctx->last_error = ctx->idle_event->error = -EDIVERGE;
		scribe_queue_event_stream(&ctx->notifications, reason);
	}

	scribe_queue_event_stream(&ctx->notifications, ctx->idle_event);
	ctx->idle_event = NULL;

	if (ctx->diverge_event) {
		scribe_free_event(ctx->diverge_event);
		ctx->diverge_event = NULL;
	}

	wake_up(&ctx->tasks_wait);
}

static int event_diverge_max_size_type(void)
{
	struct scribe_event *event;
	size_t max_size = 0;
	int max_size_type = 0;
	int i;

	for (i = 0; i < (__typeof__(event->type))-1; i++) {
		if (!is_diverge_type(i))
			continue;
		if (sizeof_event_from_type(i) > max_size) {
			max_size = sizeof_event_from_type(i);
			max_size_type = i;
		}
	}

	return max_size_type;
}

int scribe_start(struct scribe_context *ctx, unsigned long flags,
		 int backtrace_len)
{
	int ret = -ENOMEM;
	struct scribe_event_context_idle *idle_event;
	struct scribe_event_diverge *diverge_event;
	struct scribe_backtrace *backtrace = NULL;

	idle_event = scribe_alloc_event(SCRIBE_EVENT_CONTEXT_IDLE);
	if (!idle_event)
		goto err;

	/*
	 * We cannot allocate the diverge event when the diverge happens,
	 * because the context may not allow to.
	 * Allocating the maximum size will suffice.
	 */
	diverge_event = scribe_alloc_event(event_diverge_max_size_type());
	if (!diverge_event)
		goto err_idle_event;

	if (backtrace_len) {
		backtrace = scribe_alloc_backtrace(backtrace_len);
		if (!backtrace)
			goto err_diverge_event;
	}

	spin_lock(&ctx->tasks_lock);
	ret = context_start(ctx, flags, idle_event, diverge_event, backtrace);
	spin_unlock(&ctx->tasks_lock);
	if (ret)
		goto err_backtrace;
	return ret;

err_backtrace:
	if (backtrace)
		scribe_free_backtrace(backtrace);
err_diverge_event:
	scribe_free_event(diverge_event);
err_idle_event:
	scribe_free_event(idle_event);
err:
	return ret;
}

void __scribe_kill(struct scribe_context *ctx, struct scribe_event *reason)
{
	struct scribe_ps *scribe;

	assert_spin_locked(&ctx->tasks_lock);

	if (is_scribe_context_dead(ctx)) {
		if (!IS_ERR(reason))
			scribe_free_event(reason);
		return;
	}

	/*
	 * The SCRIBE_STATE_MASK flags has to be clear here (as opposed to
	 * after the killing) to guard against races with scribe_attach()
	 * called from copy_process() or execve(). See in scribe_attach() for
	 * more details.
	 */
	context_idle(ctx, reason);

	/*
	 * The tasks list is most likely to be empty by now.
	 * If it's not empty, it means that the userspace monitor process has
	 * gone missing. We'll kill all the scribed tasks because we cannot
	 * guarantee that they can continue properly.
	 */
	rcu_read_lock();
	list_for_each_entry(scribe, &ctx->tasks, node) {
		/* The init process must die too */
		scribe->p->signal->flags &= ~SIGNAL_UNKILLABLE;
		do_send_sig_info(SIGKILL, SEND_SIG_PRIV, scribe->p, 1);
		/*
		 * do_send_sig_info() may fail because the signal handler is
		 * gone, and the task is waiting on a resource in do_exit()
		 * during the replay. wake_up_process() is necessary in that
		 * case.
		 * It may also fail because the init process is unkillable.
		 * It also wakes up tasks waiting on some event and checking
		 * on is_scribe_context_dead().
		 */
		wake_up_process(scribe->p);
	}
	rcu_read_unlock();
}

/*
 * XXX This is not an scribe_kill(). This is just a notification that will
 * initiate a graceful ending.
 * Processes are checking for the SCRIBE_STOP flag when entering a syscall.
 */
int scribe_stop(struct scribe_context *ctx)
{
	int ret = 0;

	spin_lock(&ctx->tasks_lock);
	if (is_scribe_context_dead(ctx))
		ret = -EPERM;
	else if (list_empty(&ctx->tasks))
		context_idle(ctx, NULL);
	else {
		ctx->flags |= SCRIBE_STOP;
		if (ctx->flags & SCRIBE_RECORD)
			scribe_wake_all_fake_sig(ctx);
		scribe_bookmark_resume(ctx->bmark);
	}
	spin_unlock(&ctx->tasks_lock);

	/*
	 * FIXME send a signal wakeup to tasks, set TIF_SIGPENDING or
	 * something.
	 */

	return ret;
}

static bool __scribe_is_deadlocked(struct scribe_context *ctx)
{
	struct scribe_ps *scribe;
	bool has_runners = false;
	bool has_tasks = false;

	spin_lock(&ctx->tasks_lock);
	list_for_each_entry(scribe, &ctx->tasks, node) {
		if (scribe->p->state == TASK_RUNNING || scribe->p->se.on_rq) {
			has_runners = true;
			break;
		}
		has_tasks = true;
	}
	spin_unlock(&ctx->tasks_lock);

	return !has_runners && has_tasks;
}

int scribe_check_deadlock(struct scribe_context *ctx)
{
	unsigned long deadline;
	int i;

	if (!(ctx->flags & SCRIBE_REPLAY))
		return -EPERM;

	deadline = jiffies - HZ/10;

	for (i = 0; i < 50; i++) {
		if (time_after(ctx->last_event_jiffies, deadline))
			return 0;
		if (!__scribe_is_deadlocked(ctx))
			return 0;
		__set_current_state(TASK_UNINTERRUPTIBLE);
		schedule_timeout(HZ/50);
		__set_current_state(TASK_RUNNING);
	}

	scribe_kill(ctx, -EDEADLK);
	return 0;
}

/* scribe_wake_all_fake_sig() interrupts syscalls */
void scribe_wake_all_fake_sig(struct scribe_context *ctx)
{
	unsigned long flags;
	struct scribe_ps *scribe;

	assert_spin_locked(&ctx->tasks_lock);

	list_for_each_entry(scribe, &ctx->tasks, node) {
		if (lock_task_sighand(scribe->p, &flags)) {
			signal_wake_up(scribe->p, 0);
			unlock_task_sighand(scribe->p, &flags);
		}
	}
}

int scribe_set_attach_on_exec(struct scribe_context *ctx, int enable)
{
	struct task_struct *p = current;
	int ret;

	if (is_ps_scribed(p))
		return -EPERM;

	if (is_scribe_context_dead(ctx))
		return -EPERM;

	exit_scribe(p);

	if (!enable)
		return 0;

	ret = init_scribe(p, ctx, SCRIBE_PS_ENABLE_ALL);
	if (ret)
		return ret;

	p->scribe->flags |= SCRIBE_PS_ATTACH_ON_EXEC;

	return 0;
}

static inline void tasks_accounting(struct scribe_context *ctx, int delta)
{
	ctx->num_tasks += delta;
	if (delta > 0 && ctx->max_num_tasks < ctx->num_tasks)
		ctx->max_num_tasks = ctx->num_tasks;
}

static void notify_attach(struct scribe_context *ctx, struct scribe_ps *scribe)
{
	pid_t real_pid, scribe_pid;

	real_pid = task_pid_nr_ns(scribe->p, ctx->monitor_nsproxy->pid_ns);
	scribe_pid = task_pid_nr_ns(scribe->p, ctx->init_nsproxy->pid_ns);

	/* XXX We fail silently */
	(void)scribe_queue_new_event_stream(&ctx->notifications,
					    SCRIBE_EVENT_ON_ATTACH,
					    .real_pid = real_pid,
					    .scribe_pid = scribe_pid);
}

/*
 * scribe_attach() and scribe_detach() must only be called when
 * current == scribe->p, or when scribe->p is sleeping (and thus not accessing
 * scribe->flags).
 */
void scribe_attach(struct scribe_ps *scribe)
{
	struct scribe_context *ctx = scribe->ctx;

	/*
	 * First get the queue, and only then, add to the task list:
	 * It guarantees that if a task is in the task list, its
	 * queue would also be in the queue list.
	 */
	BUG_ON(scribe->queue);
	BUG_ON(!scribe->pre_alloc_queue);
	scribe->queue = scribe_get_queue_by_pid(ctx, &scribe->pre_alloc_queue,
						task_pid_vnr(scribe->p));
	kfree(scribe->pre_alloc_queue);
	scribe->pre_alloc_queue = NULL;

	spin_lock(&ctx->tasks_lock);
	BUG_ON(is_scribed(scribe));

	if (unlikely(is_scribe_context_dead(ctx))) {
		spin_unlock(&ctx->tasks_lock);

		/*
		 * Two reasons we landed here:
		 * 1) We got caught in the attach_on_exec race:
		 *    - the process calls scribe_set_attach_on_exec(ctx)
		 *    - the context goes idle (event pump, or device closed)
		 *    - the process calls execve(), and lands here
		 *
		 * 2) copy_process() was about to attach a child, when
		 * suddenly scribe_kill() got called and distributed
		 * some SIGKILLs, but only to the parent.
		 *
		 * We can SIGKILL the current process because if we attached
		 * right before the context went dead, we would have received
		 * the SIGKILL from scribe_kill() anyways. It's a race.
		 *
		 * Note: During replay, in case a new event comes in for our
		 * pid, a new queue will be instantiated by the device, and
		 * will never be picked up by any process. But that's fine
		 * because it means something went wrong, and the scribe
		 * context is about to die, the queue will get freed anyways.
		 */

		scribe_put_queue(scribe->queue);
		scribe->queue = NULL;

		force_sig(SIGKILL, scribe->p);
		exit_scribe(scribe->p);
		return;
	}

	list_add_tail(&scribe->node, &ctx->tasks);
	tasks_accounting(ctx, +1);
	setup_init_nsproxy(ctx, scribe);

	/* ctx->flags must be read within the critical region */
	scribe->flags |= (ctx->flags & SCRIBE_RECORD) ? SCRIBE_PS_RECORD : 0;
	scribe->flags |= (ctx->flags & SCRIBE_REPLAY) ? SCRIBE_PS_REPLAY : 0;

	spin_unlock(&ctx->tasks_lock);

	wake_up(&ctx->tasks_wait);

	if (is_recording(scribe)) {
		/*
		 * The monitor will be waiting on ctx->queue_wait, and all
		 * processes sends their event queue notifications to it.
		 */
		scribe->queue->stream.wait = &ctx->queues_wait;
	} else { /* is_replaying(scribe) == 1 */

		/*
		 * We can now release the persistent reference that was holding
		 * the queue waiting the process to attach since we got our
		 * reference in scribe_get_queue_by_pid()
		 */
		spin_lock(&ctx->queues_lock);
		scribe_unset_persistent(scribe->queue);
		spin_unlock(&ctx->queues_lock);

		BUG_ON(scribe->queue->stream.wait !=
		       &scribe->queue->stream.default_wait);

		/*
		 * During the replay, we update the global @last_event_jiffies
		 * for deadlock detection.
		 */
		scribe->queue->stream.last_event_jiffies =
			&ctx->last_event_jiffies;
	}

	scribe->in_syscall = 0;
	scribe->need_syscall_ret = false;
	scribe->prepared_data_event.generic = NULL;
	scribe->data_flags = 0;
	scribe->can_uaccess = 0;
	scribe->in_read_write = false;

	scribe_attach_arch(scribe);

	BUG_ON(scribe_mem_init_st(scribe));

	notify_attach(ctx, scribe);

	scribe_handle_custom_actions(scribe);
}

void __scribe_detach(struct scribe_ps *scribe)
{
	struct scribe_context *ctx = scribe->ctx;
	unsigned int scribe_flags;
	bool sighand_locked;
	unsigned long flags;

	BUG_ON(!is_scribed(scribe));

	WARN_ON(scribe->can_uaccess && !is_scribe_context_dead(ctx));
	WARN_ON(is_recording(scribe) && scribe->signal.should_defer);

	scribe_detach_arch(scribe);

	if (scribe->prepared_data_event.generic) {
		WARN(1, "prepared_data_event present\n");
		scribe_free_event(scribe->prepared_data_event.generic);
	}

	spin_lock(&ctx->tasks_lock);

	list_del(&scribe->node);
	tasks_accounting(ctx, -1);

	/*
	 * The last task in the context is detaching, it's time to set it to
	 * idle. Userspace will get notified.
	 */
	if (list_empty(&ctx->tasks) && !is_scribe_context_dead(ctx))
		context_idle(ctx, NULL);
	spin_unlock(&ctx->tasks_lock);

	/*
	 * The sighand lock guards against some races within the signal code.
	 */
	scribe_flags = scribe->flags;

	sighand_locked = !!lock_task_sighand(scribe->p, &flags);
	scribe->flags &= ~(SCRIBE_PS_RECORD | SCRIBE_PS_REPLAY);
	if (sighand_locked)
		unlock_task_sighand(scribe->p, &flags);

	/*
	 * We want to set the sealed flag and put the queue in an atomic
	 * way so that the event pump sends a QUEUE_EOF event once and only
	 * once.
	 */
	spin_lock(&ctx->queues_lock);
	if (scribe_flags & SCRIBE_PS_RECORD)
		scribe_seal_queue(scribe->queue);
	scribe_put_queue_locked(scribe->queue);
	spin_unlock(&ctx->queues_lock);

	scribe->queue = NULL;
}

void scribe_detach(struct scribe_ps *scribe)
{
	scribe->flags |= SCRIBE_PS_DETACHING;
	if (is_replaying(scribe))
		scribe_kill_queue(scribe->queue);

	scribe_mem_exit_st(scribe);
	__scribe_detach(scribe);
}

static bool should_detach(struct scribe_ps *scribe)
{
	if (scribe->ctx->flags & SCRIBE_STOP)
		return true;

	if (is_replaying(scribe) &&
	    scribe_is_queue_dead(scribe->queue, SCRIBE_WAIT))
		return true;

	return false;
}

bool scribe_maybe_detach(struct scribe_ps *scribe)
{
	struct scribe_context *ctx = scribe->ctx;
	struct task_struct *p;

	if (!should_detach(scribe))
		return false;

	scribe_assert_no_locked_region(&scribe->resources);

	p = scribe->p;

	scribe_get_context(ctx);
	scribe_detach(scribe);
	exit_scribe(scribe->p);

	//wait_event(ctx->tasks_wait, list_empty(&ctx->tasks) || ctx->last_error);
	/*
	 * FIXME possible race: when list_empty(ctx->task), the context might
	 * have started again, and other tasks might have joined the context.
	 */
	if (ctx->last_error)
		force_sig(SIGKILL, p);
	scribe_put_context(ctx);

	/*
	 * Delayed already queues signals should be handled, but we don't want
	 * to clear the SIGPENDING flag in case we have a -ERESTARTNOHAND
	 * syscall return value or something.
	 */
	recalc_sigpending_and_wake(current);

	return true;
}
