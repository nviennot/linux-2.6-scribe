/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include <linux/kernel.h>
#include <linux/file.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/splice.h>
#include <linux/completion.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/scribe.h>

/*
 * The pump takes care of serializing (record) and deserializing (replay) the
 * evens to/from a file.
 * TODO Have one kthread for n contexts, right now we have n kthreads for n
 * contexts.
 */

#define PUMP_BUFFER_ORDER 2
#define PUMP_BUFFER_SIZE (PAGE_SIZE << PUMP_BUFFER_ORDER)

struct scribe_pump {
	struct scribe_context *ctx;
	struct task_struct *kthread;
	struct completion done;
	char *buffer;
	struct file *logfile;
	int flags;

	struct list_head node;
	bool force_run;
	wait_queue_head_t idle_wait;
};

/* Shrinker */

static LIST_HEAD(pumps);
static DEFINE_MUTEX(pumps_lock);

static void going_idle(struct scribe_pump *pump)
{
	pump->force_run = false;
	wake_up(&pump->idle_wait);
}

static int pump_shrink(struct shrinker *s, int nr_to_scan, gfp_t gfp_mask)
{
	struct scribe_pump *pump;
	DEFINE_WAIT(wait);

	if ((gfp_mask & GFP_KERNEL) != GFP_KERNEL)
		return (nr_to_scan == 0) ? 0 : -1;

	mutex_lock(&pumps_lock);

	/* We tell all the queue to flush their buffers */
	list_for_each_entry(pump, &pumps, node) {
		pump->force_run = true;
		/* Not the ideal wait queue, but I guess that's fine  for now. */
		wake_up(&pump->ctx->tasks_wait);
	}

	/* and wait until they go idle again */
	list_for_each_entry(pump, &pumps, node) {
		wait_event(pump->idle_wait, !pump->force_run);
	}

	mutex_unlock(&pumps_lock);

	/*
	 * XXX We are not complying with the API, but doing so would be hard.
	 * Is it important ?
	 */

	return 0;
}

static struct shrinker pump_shrinker = {
	.shrink = pump_shrink,
	.seeks = DEFAULT_SEEKS
};

void scribe_init_pump(void)
{
	register_shrinker(&pump_shrinker);
}

/* Record */

static inline int is_queue_active(struct scribe_queue *queue)
{
	return !scribe_is_stream_empty(&queue->stream) ||
		queue->stream.sealed;
}

/*
 * __get_active_queue() tries to find the the next non empty queue, or ready
 * to be ripped off. Returns -ENODATA if the queue list is empty and will stay
 * empty, -EAGAIN if at least one queue is present and all queues are empty
 * (or that we are waiting for the recording to start).
 */
static int __get_active_queue(struct scribe_context *ctx,
			      struct scribe_queue **current_queue)
{
	struct scribe_queue *queue;
	int ret = 0;

	queue = *current_queue;
	if (queue && is_queue_active(queue))
		return 0;

	spin_lock(&ctx->queues_lock);
	queue = list_prepare_entry(queue, &ctx->queues, node);
	list_for_each_entry_continue(queue, &ctx->queues, node) {
		if (is_queue_active(queue)) {
			scribe_get_queue(queue);
			goto out;
		}
	}

	queue = NULL;
	ret = -EAGAIN;
	if (list_empty(&ctx->queues)) {
		/*
		 * There are no more queues in the context, which means that
		 * there are no tasks attached as well. Thus the context
		 * state is either:
		 * - dead: the recording is over, and so we want
		 *   serialize_events() to return 0
		 * - SCRIBE_RECORD: the recording has not started yet, we want
		 *   to wait.
		 */
		if (is_scribe_context_dead(ctx))
			ret = -ENODATA;
	}
out:
	spin_unlock(&ctx->queues_lock);

	if (*current_queue)
		scribe_put_queue(*current_queue);
	*current_queue = queue;

	return ret;
}

static int get_active_queue(struct scribe_context *ctx,
			    struct scribe_queue **current_queue,
			    int wait)
{
	int ret;

	if (wait == SCRIBE_NO_WAIT)
		return __get_active_queue(ctx, current_queue);

	wait_event(ctx->queues_wait,
		   (ret = __get_active_queue(ctx, current_queue)) != -EAGAIN);

	return ret;
}

/*
 * get_next_event() returns in most cases the next event on the
 * @current_queue. When switching queues, it returns a PID event, and when the
 * queue is dead, it returned a QUEUE_EOF event.
 *
 * Note: This is where queues get to be freed when dead
 */
static int get_next_event(pid_t *last_pid, struct scribe_event **event,
			  struct scribe_queue **current_queue)
{
	struct scribe_queue *queue;
	struct scribe_event_pid *event_pid;
	struct scribe_event_queue_eof *event_eof;

	queue = *current_queue;

	if (unlikely(*last_pid != queue->pid)) {
		/* We've changed pid, inserting a pid event */
		event_pid = scribe_alloc_event(SCRIBE_EVENT_PID);
		if (!event_pid)
			return -ENOMEM;

		event_pid->pid = queue->pid;
		*last_pid = queue->pid;

		*event = (struct scribe_event *)event_pid;
	} else if (likely(!scribe_is_stream_empty(&queue->stream))) {
		*event = scribe_dequeue_event(queue, SCRIBE_NO_WAIT);
	} else {
		BUG_ON(!&queue->stream.sealed);

		event_eof = scribe_alloc_event(SCRIBE_EVENT_QUEUE_EOF);
		if (!event_eof)
			return -ENOMEM;

		*event = (struct scribe_event *)event_eof;

		/*
		 * When the queue is empty and sealed, we can consider it as
		 * dead. Releasing our persistent token on it will make the
		 * queue go away if the associated process detaches.
		 */
		spin_lock(&queue->ctx->queues_lock);
		scribe_unset_persistent(queue);
		spin_unlock(&queue->ctx->queues_lock);

		scribe_put_queue(queue);
		*current_queue = NULL;
	}

	return 0;
}

static ssize_t serialize_events(struct scribe_context *ctx,
				char *buf, size_t count,
				pid_t *last_pid,
				struct scribe_queue **current_queue,
				struct scribe_event **pending_event,
				size_t *pending_offset)
{
	struct scribe_event *event;
	size_t to_write;
	ssize_t ret = 0;
	int err = 0;

	/*
	 * Two cases:
	 * - We are dealing with a partially sent event, we need to pick up
	 *   where we left off.
	 * - In the other case, we'll just grab the next event on the next non
	 *   empty queue.
	 */
	event = *pending_event;
	*pending_event = NULL;
	if (!event) {
		err = get_active_queue(ctx, current_queue, SCRIBE_WAIT);
		if (err) {
			/*
			 * if err == -ENODATA, it means that the context is in
			 * idle state and that the queue list is empty.
			 * It's time for EOF
			 */
			if (err == -ENODATA)
				err = 0;
			goto out;
		}
	}

	for (;;) {
		if (!event) {
			err = get_next_event(last_pid, &event, current_queue);
			if (err)
				goto out;
		}

		to_write = sizeof_event_payload(event) - *pending_offset;
		if (to_write > count) {
			to_write = count;
			*pending_event = event;
		}
		memcpy(buf,
		       get_event_payload(event) + *pending_offset, to_write);
		ret += to_write;

		if (*pending_event) {
			*pending_offset += to_write;
			goto out;
		}

		scribe_free_event(event);
		event = NULL;
		*pending_offset = 0;

		buf += to_write;
		count -= to_write;

		err = get_active_queue(ctx, current_queue, SCRIBE_NO_WAIT);
		if (err)
			goto out;
	}

out:
	if (ret)
		return ret;
	return err;
}

static void event_pump_record(struct scribe_pump *pump)
{
	struct scribe_context *ctx = pump->ctx;
	char *buf = pump->buffer;
	struct file *file = pump->logfile;

	struct scribe_queue *current_queue = NULL;
	struct scribe_event *pending_event = NULL;
	size_t pending_offset = 0;
	pid_t last_pid = 0;

	int buffer_full = 0;
	ssize_t ret;
	size_t to_write;
	char *write_buf;

	while (!kthread_should_stop()) {
		/*
		 * Looping here on every event is inefficient.
		 * We are waiting one second on each full iteration.
		 * And instant return when the context goes idle
		 */
		if (!buffer_full) {
			going_idle(pump);
			wait_event_timeout(ctx->tasks_wait,
				is_scribe_context_dead(ctx) || pump->force_run,
				HZ);
		}

		ret = serialize_events(ctx, buf, PUMP_BUFFER_SIZE,
				       &last_pid, &current_queue,
				       &pending_event, &pending_offset);
		if (ret < 0)
			goto err;
		if (!ret)
			break;

		buffer_full = (ret == PUMP_BUFFER_SIZE);

		to_write = ret;
		write_buf = buf;
		while (to_write) {
			ret = kernel_write(file,
					   write_buf, to_write, file->f_pos);
			if (unlikely(is_interruption(ret)))
				continue;

			if (ret < 0)
				goto err;

			to_write -= ret;
			write_buf += ret;
			file->f_pos += ret;
		}
	}
free:
	if (current_queue)
		scribe_put_queue(current_queue);
	if (pending_event)
		scribe_free_event(pending_event);
	return;
err:
	scribe_kill(ctx, ret);
	goto free;
}

/* Replay */

static int handle_event_pid(struct scribe_context *ctx,
			    struct scribe_event *event,
			    struct scribe_queue **current_queue,
			    struct scribe_queue **pre_alloc_queue)
{
	pid_t pid = ((struct scribe_event_pid *)event)->pid;

	if (!*pre_alloc_queue) {
		*pre_alloc_queue = kmalloc(sizeof(**pre_alloc_queue),
					   GFP_KERNEL);
		if (!*pre_alloc_queue)
			return -ENOMEM;
	}

	if (*current_queue)
		scribe_put_queue(*current_queue);
	*current_queue = scribe_get_queue_by_pid(ctx, pre_alloc_queue, pid);
	return 0;
}

static ssize_t get_sized_event_size(const char *buf, size_t count)
{
	struct scribe_event_sized *event;
	__typeof__(event->size) size = 0;
	int size_offset;

	size_offset = offsetof(struct scribe_event_sized, size)
		    - offsetof(struct scribe_event_sized, h.payload_offset);

	if (count < size_offset + sizeof(size))
		return -EINVAL;
	return *(__typeof__(size) *)(buf + size_offset);
}

static int alloc_next_event(const char *buf, size_t count,
			    struct scribe_event **event)
{
	__typeof__((*event)->type) type;
	ssize_t size;

	if (sizeof(type) > count)
		return -EAGAIN;
	type = *(__typeof__(type) *)buf;

	if (is_sized_type(type)) {
		size = get_sized_event_size(buf, count);
		if (size < 0)
			return size;
		*event = scribe_alloc_event_sized(type, size);
	} else {
		*event = scribe_alloc_event(type);
	}

	if (!*event)
		return -ENOMEM;

	return 0;
}

static ssize_t deserialize_events(struct scribe_context *ctx, const char *buf,
				  size_t count, loff_t file_pos,
				  struct scribe_queue **current_queue,
				  struct scribe_queue **pre_alloc_queue,
				  struct scribe_event **pending_event,
				  size_t *pending_offset)
{
	struct scribe_event *event = NULL;
	size_t to_copy;
	ssize_t ret = 0;
	int err = 0;

	event = *pending_event;
	*pending_event = NULL;

	for (;;) {
		if (!event) {
			err = alloc_next_event(buf, count, &event);
			if (err)
				goto out;
			event->log_offset = file_pos;
		}

		to_copy = sizeof_event_payload(event) - *pending_offset;

		if (to_copy > count) {
			to_copy = count;
			*pending_event = event;
		}
		memcpy(get_event_payload(event) + *pending_offset,
		       buf, to_copy);

		if (*pending_event) {
			ret += to_copy;
			*pending_offset += to_copy;
			goto out;
		}
		*pending_offset = 0;

		if (event->type == SCRIBE_EVENT_PID) {
			err = handle_event_pid(ctx, event,
					       current_queue, pre_alloc_queue);
			scribe_free_event(event);
			if (err)
				goto out;
		} else if (event->type == SCRIBE_EVENT_NOP) {
			scribe_free_event(event);
		} else {
			if (unlikely(!*current_queue)) {
				err = -EINVAL;
				goto out;
			}
			if (event->type == SCRIBE_EVENT_QUEUE_EOF) {
				scribe_seal_queue(*current_queue);
				scribe_free_event(event);
			} else { /* generic event handling */
				if (unlikely((*current_queue)->stream.sealed))
					scribe_free_event(event);
				else
					scribe_queue_event(*current_queue, event);
			}
		}

		event = NULL;
		ret += to_copy;
		buf += to_copy;
		count -= to_copy;
		file_pos += to_copy;
	}

out:
	if (ret)
		return ret;
	return err;
}

/*
 * event_pump_replay() reads from @file to @buf, and call deserialize_events()
 * to instantiate each event.
 */
static void event_pump_replay(struct scribe_pump *pump)

{
	struct scribe_context *ctx = pump->ctx;
	char *buf = pump->buffer;
	struct file *file = pump->logfile;

	struct scribe_queue *queue;
	struct scribe_queue *current_queue = NULL;
	struct scribe_queue *pre_alloc_queue = NULL;
	struct scribe_event *pending_event = NULL;
	size_t pending_offset = 0;
	size_t count = 0;
	ssize_t ret = 0;

	while (!kthread_should_stop() && !ctx->queues_sealed) {
		ret = kernel_read(file, file->f_pos,
				  buf + count,
				  PUMP_BUFFER_SIZE - count);

		if (unlikely(is_interruption(ret)))
			continue;
		if (!ret) {
			/*
			 * We might have a pending event in our buffer (count
			 * is not 0), which means something is went wrong.
			 * Otherwise, we've reached EOF.
			 */
			if (count)
				ret = -EPIPE;
			break;
		}
		if (ret < 0)
			break;

		file->f_pos += ret;
		count += ret;

		ret = deserialize_events(ctx, buf, count, file->f_pos - count,
					 &current_queue, &pre_alloc_queue,
					 &pending_event, &pending_offset);
		if (ret < 0)
			break;

		BUG_ON(!ret);
		count -= ret;
		if (count) {
			/*
			 * Only a portion of the buffer has been processed, it
			 * will get processed on the next round.
			 */
			memmove(buf, buf + ret, count);
		}
	}

	/*
	 * scribe_kill() is called before kill_queue() to avoid races with
	 * scribe_maybe_detach().
	 */
	if (ret < 0)
		scribe_kill(ctx, ret);

retry:
	spin_lock(&ctx->queues_lock);
	list_for_each_entry(queue, &ctx->queues, node) {
		/*
		 * If some queue were left open, that means that we didn't
		 * have the entire event stream, we need to kill the context.
		 */
		scribe_seal_queue(queue);
		if (ret < 0) {
			if (!is_scribe_context_dead(ctx)) {
				spin_unlock(&ctx->queues_lock);
				scribe_kill(ctx, ret);
				/*
				 * after the scribe_kill() we are now idle,
				 * so this goto will only occur once.
				 */
				goto retry;
			}

			scribe_kill_queue(queue);
		}
	}
	ctx->queues_sealed = 1;
	spin_unlock(&ctx->queues_lock);

	if (current_queue)
		scribe_put_queue(current_queue);
	kfree(pre_alloc_queue);
	scribe_free_event(pending_event);
}

/*
 * We need a kthread for performance reasons: consider a single process
 * getting recorded. When that process queues some events in its queue, we
 * want it to immediately return to work, and the actual serialization will
 * happen on another CPU.
 */
static int pump_kthread(void *_pump)
{
	struct scribe_pump *pump = _pump;

	mutex_lock(&pumps_lock);
	list_add_tail(&pump->node, &pumps);
	mutex_unlock(&pumps_lock);

	if (pump->flags & SCRIBE_RECORD)
		event_pump_record(pump);
	else if (pump->flags & SCRIBE_REPLAY)
		event_pump_replay(pump);
	else
		BUG();

	going_idle(pump);
	fput(pump->logfile);

	mutex_lock(&pumps_lock);
	list_del(&pump->node);
	mutex_unlock(&pumps_lock);

	complete(&pump->done);

	do_exit(0);
}

struct scribe_pump *scribe_pump_alloc(struct scribe_context *ctx)
{
	struct scribe_pump *pump;
	pump = kmalloc(sizeof(*pump), GFP_KERNEL);
	if (!pump)
		return NULL;

	pump->ctx = ctx;
	pump->kthread = NULL;
	init_completion(&pump->done);
	pump->logfile = NULL;
	pump->flags = 0;

	init_waitqueue_head(&pump->idle_wait);
	pump->force_run = false;

	pump->buffer = (char *)__get_free_pages(GFP_KERNEL, PUMP_BUFFER_ORDER);
	if (!pump->buffer) {
		kfree(pump);
		return NULL;
	}

	return pump;
}

void scribe_pump_free(struct scribe_pump *pump)
{
	scribe_pump_stop(pump);
	free_pages((unsigned long)pump->buffer, PUMP_BUFFER_ORDER);
	kfree(pump);
}

/* FIXME collapse prepare_start and alloc */
int scribe_pump_prepare_start(struct scribe_pump *pump)
{
	int ret;

	scribe_pump_stop(pump);

	pump->kthread = kthread_create(pump_kthread, pump,
				       "scribe%d", pump->ctx->id);
	if (IS_ERR(pump->kthread)) {
		ret = PTR_ERR(pump->kthread);
		pump->kthread = NULL;
		return ret;
	}

	return 0;
}

void scribe_pump_abort_start(struct scribe_pump *pump)
{
	kthread_stop(pump->kthread);
	put_task_struct(pump->kthread);
	pump->kthread = NULL;
}

void scribe_pump_start(struct scribe_pump *pump, int state,
		       struct file *logfile)
{
	get_file(logfile);
	pump->logfile = logfile;
	pump->flags = state;

	get_task_struct(pump->kthread);
	INIT_COMPLETION(pump->done);
	wake_up_process(pump->kthread);
}

void scribe_pump_stop(struct scribe_pump *pump)
{
	if (!pump->kthread)
		return;

	scribe_kill(pump->ctx, -EINTR);
	scribe_pump_abort_start(pump);
}

int scribe_pump_wait_completion_interruptible(struct scribe_pump *pump)
{
	return wait_for_completion_interruptible(&pump->done);
}
