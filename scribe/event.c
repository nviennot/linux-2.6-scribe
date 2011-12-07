/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include <linux/scribe.h>
#include <linux/sched.h>

static void init_substream(struct scribe_stream *stream,
			   struct scribe_substream *substream)
{
	substream->stream = stream;
	INIT_LIST_HEAD(&substream->events);
	INIT_LIST_HEAD(&substream->node);
	substream->clear_region_on_commit_set = 0;
	substream->region_set = 0;
}

void scribe_init_stream(struct scribe_stream *stream)
{
	spin_lock_init(&stream->lock);
	init_substream(stream, &stream->master);
	stream->last_event_jiffies = NULL;
	stream->sealed = 0;
	init_waitqueue_head(&stream->default_wait);
	stream->wait = &stream->default_wait;
}

static void init_queue(struct scribe_queue *queue,
		       struct scribe_context *ctx, pid_t pid)
{
	/*
	 * XXX the context reference is not taken: the caller should maintain
	 * that reference until that queue dies.
	 */
	scribe_init_stream(&queue->stream);
	atomic_set(&queue->ref_cnt, 1);
	queue->persistent = 0;
	queue->ctx = ctx;
	INIT_LIST_HEAD(&queue->node);
	queue->pid = pid;
	queue->regions_set = 0;
	memset(&queue->fence_events, 0, sizeof(queue->fence_events));
	queue->fence_serial = 0;
	queue->last_event_offset = -1;
	queue->num_ev_consumed = 0;
}

static void exit_queue(struct scribe_queue *queue)
{
	int i;
	scribe_free_all_events(&queue->stream);

	for (i = 0; i < ARRAY_SIZE(queue->fence_events); i++)
		scribe_free_event(queue->fence_events[i]);
}

static struct scribe_queue *find_queue(struct scribe_context *ctx, pid_t pid)
{
	struct scribe_queue *queue;

	list_for_each_entry(queue, &ctx->queues, node)
		if (queue->pid == pid)
			return queue;

	return NULL;
}

/*
 * scribe_get_queue_by_pid() never fails. The pre allocated queue is useful
 * for attach_process() to perform without failing.
 * When the pre allocated queue is used, its address is set NULL.
 */
struct scribe_queue *scribe_get_queue_by_pid(
				struct scribe_context *ctx,
				struct scribe_queue **pre_alloc_queue,
				pid_t pid)
{
	struct scribe_queue *queue;

	spin_lock(&ctx->queues_lock);
	queue = find_queue(ctx, pid);
	if (queue) {
		scribe_get_queue(queue);
		goto out;
	}

	queue = *pre_alloc_queue;
	*pre_alloc_queue = NULL;

	init_queue(queue, ctx, pid);
	if (ctx->queues_sealed)
		queue->stream.sealed = 1;

	/*
	 * Making the new queue persistent: We are keeping an internal
	 * reference to prevent the queue from being freed when the producer
	 * is done with the queue and the consumer is not available yet.
	 */
	scribe_set_persistent(queue);
	list_add_tail(&queue->node, &ctx->queues);
out:
	spin_unlock(&ctx->queues_lock);
	wake_up(&ctx->queues_wait);
	return queue;
}

void scribe_get_queue(struct scribe_queue *queue)
{
	atomic_inc(&queue->ref_cnt);
}

void scribe_put_queue(struct scribe_queue *queue)
{
	struct scribe_context *ctx = queue->ctx;

	if (atomic_dec_and_lock(&queue->ref_cnt, &ctx->queues_lock)) {
		list_del(&queue->node);
		spin_unlock(&ctx->queues_lock);
		wake_up(&queue->ctx->queues_wait);
		exit_queue(queue);
		kfree(queue);
	}
}

void scribe_put_queue_locked(struct scribe_queue *queue)
{
	if (atomic_dec_and_test(&queue->ref_cnt)) {
		list_del(&queue->node);
		wake_up(&queue->ctx->queues_wait);
		exit_queue(queue);
		kfree(queue);
	}
}

void scribe_set_persistent(struct scribe_queue *queue)
{
	assert_spin_locked(&queue->ctx->queues_lock);

	if (queue->persistent)
		return;

	queue->persistent = 1;
	scribe_get_queue(queue);
}
void scribe_unset_persistent(struct scribe_queue *queue)
{
	assert_spin_locked(&queue->ctx->queues_lock);

	if (!queue->persistent)
		return;

	queue->persistent = 0;
	scribe_put_queue_locked(queue);
}

void scribe_free_all_events(struct scribe_stream *stream)
{
	struct scribe_event *event, *tmp;
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);

	/* Some insert points are in progress... */
	BUG_ON(!list_empty(&stream->master.node));

	list_for_each_entry_safe(event, tmp, &stream->master.events, node) {
		list_del(&event->node);
		scribe_free_event(event);
	}

	spin_unlock_irqrestore(&stream->lock, flags);
}

/*
 * This is where lies the difference between an insert point and a substream.
 */
static struct scribe_substream *get_substream(scribe_insert_point_t *ip)
{
	return list_entry(ip->node.next, __typeof__(*ip), node);
}

static void set_region(struct scribe_stream *stream,
		       scribe_insert_point_t *ip, int region)
{
	struct scribe_substream *substream;
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	substream = get_substream(ip);
	if (substream != ip)
		substream->clear_region_on_commit_set &= ~(1 << region);
	BUG_ON(substream->region_set & (1 << region));
	substream->region_set |= (1 << region);
	spin_unlock_irqrestore(&stream->lock, flags);
}

static void clear_region(struct scribe_stream *stream,
			 scribe_insert_point_t *ip, int region)
{
	struct scribe_substream *substream;
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	substream = get_substream(ip);
	if (ip == substream)
		substream->region_set &= ~(1 << region);
	else
		substream->clear_region_on_commit_set |= (1 << region);
	spin_unlock_irqrestore(&stream->lock, flags);
}

static void __clear_regions_on_commit(scribe_insert_point_t *ip,
				      struct scribe_substream *substream)
{
	substream->region_set &= ~ip->clear_region_on_commit_set;
}

static void __insert_fences(struct scribe_stream *stream,
			    struct scribe_substream *substream)
{
	struct scribe_queue *queue;
	int num_regions;
	int region;

	if (!substream->region_set)
		return;

	num_regions = 0;
	queue = container_of(stream, struct scribe_queue, stream);

	while (substream->region_set) {
		region = ffs(substream->region_set)-1;
		substream->region_set &= ~(1 << region);

		list_add_tail(&queue->fence_events[region]->h.node,
			      &substream->events);
		queue->fence_events[region] = NULL;
		num_regions++;
	}

	if (num_regions > 1) {
		/* We don't want to get a performance hit on sorting */
		WARN(1, "Need to sort the events by serial number\n");
		scribe_kill(current->scribe->ctx, -ENOSYS);
	}
}

static void init_insert_point(scribe_insert_point_t *ip,
			      struct scribe_stream *stream,
			      struct scribe_substream *where)
{
	unsigned long flags;

	init_substream(stream, ip);

	spin_lock_irqsave(&stream->lock, flags);
	list_add(&ip->node, &where->node);
	spin_unlock_irqrestore(&stream->lock, flags);
}

void scribe_create_insert_point(scribe_insert_point_t *ip,
				struct scribe_stream *stream)
{
	init_insert_point(ip, stream, &stream->master);
}

static inline void __scribe_queue_at(struct scribe_stream *stream,
				     scribe_insert_point_t *ip,
				     struct scribe_event *event,
				     struct list_head *events)
{
	struct scribe_substream *substream = get_substream(ip);

	/*
	 * When queuing events, we want to put them in the next
	 * insert point event list because the current insert point is
	 * blocked by the insert point.
	 * when the next insert point is committed, those events will be
	 * merge into the current insert point with
	 * scribe_commit_insert_point().
	 */
	if (likely(event)) {
		__insert_fences(stream, substream);
		list_add_tail(&event->node, &substream->events);
	}
	if (likely(events) && !list_empty(events)) {
		__insert_fences(stream, substream);
		list_splice_tail_init(events, &substream->events);
	}

	if (substream == &stream->master)
		wake_up(stream->wait);
}

void scribe_commit_insert_point(scribe_insert_point_t *ip)
{
	struct scribe_stream *stream = ip->stream;
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	__clear_regions_on_commit(ip, get_substream(ip));
	__scribe_queue_at(stream, ip, NULL, &ip->events);
	list_del(&ip->node);
	spin_unlock_irqrestore(&stream->lock, flags);
}

static void commit_pending_insert_points(struct scribe_stream *stream)
{
	scribe_insert_point_t *ip, *tmp;

	list_for_each_entry_safe(ip, tmp, &stream->master.node, node)
		scribe_commit_insert_point(ip);
}

static inline void scribe_queue_at(struct scribe_stream *stream,
				   scribe_insert_point_t *ip,
				   struct scribe_event *event,
				   struct list_head *events)
{
	unsigned long flags;

	spin_lock_irqsave(&stream->lock, flags);
	__scribe_queue_at(stream, ip, event, events);
	spin_unlock_irqrestore(&stream->lock, flags);
}

void scribe_queue_event_at(scribe_insert_point_t *ip, void *event)
{
	scribe_queue_at(ip->stream, ip, event, NULL);
}

void scribe_queue_event_stream(struct scribe_stream *stream, void *event)
{
	scribe_queue_at(stream, &stream->master, event, NULL);
}

void scribe_queue_event(struct scribe_queue *queue, void *event)
{
	scribe_queue_event_stream(&queue->stream, event);
}

void scribe_queue_events_stream(struct scribe_stream *stream,
				struct list_head *events)
{
	scribe_queue_at(stream, &stream->master, NULL, events);
}

static struct scribe_event *__scribe_peek_event(struct scribe_stream *stream,
						int wait, int remove)
{
	scribe_insert_point_t *ip = &stream->master;
	struct scribe_substream *substream;
	struct scribe_event *event;
	unsigned long flags;

retry:
	if (wait == SCRIBE_WAIT_INTERRUPTIBLE &&
	    wait_event_interruptible(*stream->wait,
				     !scribe_is_stream_empty(stream) ||
				     stream->sealed))
		return ERR_PTR(-ERESTARTSYS);

	if (wait == SCRIBE_WAIT)
		wait_event(*stream->wait,
		       !scribe_is_stream_empty(stream) ||
		       stream->sealed);

	spin_lock_irqsave(&stream->lock, flags);
	/*
	 * We are not using get_substream() because we are reaching events on
	 * the head of the stream.
	 */
	substream = ip;
	if (list_empty(&substream->events)) {
		spin_unlock_irqrestore(&stream->lock, flags);
		/*
		 * If the queue is sealed, the queue is officially dead.
		 * There is no point waiting.
		 */
		if (stream->sealed)
			return ERR_PTR(-ENODATA);
		if (wait)
			goto retry;
		return ERR_PTR(-EAGAIN);
	}
	event = list_first_entry(&substream->events, __typeof__(*event), node);
	if (likely(remove)) {
		list_del(&event->node);
		if (stream->last_event_jiffies)
			*stream->last_event_jiffies = jiffies;
	}
	spin_unlock_irqrestore(&stream->lock, flags);

	return event;
}

struct scribe_event *scribe_dequeue_event(struct scribe_queue *queue, int wait)
{
	struct scribe_event *event;
	struct scribe_context *ctx = queue->ctx;
	unsigned long flags;

	event = __scribe_peek_event(&queue->stream, wait, 1);
	if (IS_ERR(event))
		return event;

	if (ctx->backtrace) {
		spin_lock_irqsave(&ctx->backtrace_lock, flags);
		if (ctx->backtrace)
			scribe_backtrace_add(ctx->backtrace, event);
		spin_unlock_irqrestore(&ctx->backtrace_lock, flags);
	}

	queue->last_event_offset = event->log_offset;
	queue->num_ev_consumed++;
	return event;
}

struct scribe_event *scribe_dequeue_event_stream(struct scribe_stream *stream,
						 int wait)
{
	return __scribe_peek_event(stream, wait, 1);
}

/*
 * scribe_peek_event() returns the first event (like dequeue), but doesn't
 * remove it from the queue. If you want to consume the event, dequeue it.
 * XXX BE CAREFUL: do not free the event, do not access the event list node,
 * another process must not dequeue the event while your are accessing it.
 */
struct scribe_event *scribe_peek_event(struct scribe_queue *queue, int wait)
{
	struct scribe_event *event;
	event = __scribe_peek_event(&queue->stream, wait, 0);
	if (!IS_ERR(event))
		queue->last_event_offset = event->log_offset;
	return event;
}

bool scribe_is_stream_empty(struct scribe_stream *stream)
{
	int ret;
	unsigned long flags;

	/* FIXME is the spinlock really necessary ? */
	spin_lock_irqsave(&stream->lock, flags);
	ret = list_empty(&stream->master.events);
	spin_unlock_irqrestore(&stream->lock, flags);
	return ret;
}

void scribe_seal_stream(struct scribe_stream *stream)
{
	commit_pending_insert_points(stream);
	stream->sealed = 1;
	wake_up(stream->wait);
}

void scribe_kill_stream(struct scribe_stream *stream)
{
	scribe_seal_stream(stream);
	/*
	 * The write barrier ensure that the pump doesn't add new events once
	 * the queue is sealed.
	 */
	smp_wmb();
	scribe_free_all_events(stream);
}

bool scribe_is_stream_dead(struct scribe_stream *stream, int wait)
{
	if (!scribe_is_stream_empty(stream))
		return false;

	if (wait == SCRIBE_WAIT)
		__scribe_peek_event(stream, SCRIBE_WAIT, 0);

	return stream->sealed && scribe_is_stream_empty(stream);
}

void *__scribe_alloc_event(int type, gfp_t flags)
{
	return __scribe_alloc_event_const(type, flags);
}

static int realloc_fence_event(struct scribe_queue *queue, int region,
			       unsigned int serial)
{
	struct scribe_event_fence **pfence_event;

	pfence_event = &queue->fence_events[region];
	if (!*pfence_event) {
		*pfence_event = scribe_alloc_event(SCRIBE_EVENT_FENCE);
		if (!*pfence_event)
			return -ENOMEM;
	}
	(*pfence_event)->serial = serial;
	return 0;
}

static int scribe_enter_fenced_region_always(struct scribe_ps *scribe,
					     int region, unsigned int serial)
{
	struct scribe_event_fence *event;
	int ret;

	if (is_recording(scribe)) {
		return scribe_queue_new_event(scribe->queue, SCRIBE_EVENT_FENCE,
					      .serial = serial);
	}
	/* is_replaying == true */
	event = scribe_dequeue_event_specific(scribe, SCRIBE_EVENT_FENCE);
	if (IS_ERR(event))
		return PTR_ERR(event);

	ret = 0;
	if (serial != event->serial) {
		scribe_diverge(scribe, SCRIBE_EVENT_DIVERGE_FENCE_SERIAL,
			       .serial = serial);
		ret = -EDIVERGE;
	}

	scribe_free_event(event);
	return ret;
}

int scribe_enter_fenced_region(int region)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_event *event;
	struct scribe_event_fence *fence_event;
	struct scribe_queue *queue;
	unsigned int serial;

	if (!is_scribed(scribe))
		return 0;

	WARN_ON(scribe->queue->regions_set & (1 << region));
	scribe->queue->regions_set |= (1 << region);

	serial = scribe->queue->fence_serial++;

	if (should_scribe_fence_always(scribe)) {
		return scribe_enter_fenced_region_always(scribe,
							 region, serial);
	}

	queue = scribe->queue;

	if (is_recording(scribe)) {
		if (realloc_fence_event(queue, region, serial))
			return -ENOMEM;

		set_region(&queue->stream, &queue->stream.master, region);
		return 0;
	}

	/* is_replaying() == true */
	event = scribe_peek_event(scribe->queue, SCRIBE_WAIT);
	if (IS_ERR(event) || event->type != SCRIBE_EVENT_FENCE)
		return 0;

	fence_event = (struct scribe_event_fence *)event;
	if (fence_event->serial == serial) {
		/* That's the right fence event, dequeuing it */
		event = scribe_dequeue_event(queue, SCRIBE_NO_WAIT);
		scribe_free_event(event);
	}

	return 0;
}

void scribe_leave_fenced_region(int region)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_queue *queue;

	if (!is_scribed(scribe))
		return;

	WARN_ON(!(scribe->queue->regions_set & (1 << region)));
	scribe->queue->regions_set &= ~(1 << region);

	if (should_scribe_fence_always(scribe))
		return;

	queue = scribe->queue;
	if (is_recording(scribe)) {
		clear_region(&queue->stream, &queue->stream.master,
				    region);
	}
}
