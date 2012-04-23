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
#include <linux/hardirq.h>
#include <linux/pagemap.h>
#include <linux/module.h>

void __scribe_allow_uaccess(struct scribe_ps *scribe)
{
	/* If we are already at 3 level deep... Something must be wrong */
	WARN(scribe->can_uaccess > 3,
	     "scribe->can_uaccess == %d\n", scribe->can_uaccess);

	if (!scribe->can_uaccess)
		scribe_mem_sync_point(scribe, MEM_SYNC_OUT);

	scribe->can_uaccess++;
}

void __scribe_forbid_uaccess(struct scribe_ps *scribe)
{
	/* If we are already at a negative level... Something must be wrong */
	WARN(scribe->can_uaccess < 0,
	     "scribe->can_uaccess == %d\n", scribe->can_uaccess);

	scribe->can_uaccess--;

	if (!scribe->can_uaccess)
		scribe_mem_sync_point(scribe, MEM_SYNC_IN);
}

int is_kernel_copy(void)
{
	return !memcmp(&get_fs(), &get_ds(), sizeof(mm_segment_t));
}

struct data_desc {
	void *data;
	void __user *user_ptr;
	size_t size;
	unsigned int flags;
	union scribe_event_data_union event;

	/* At most one of the following three flags are set */
	bool do_non_det;
	bool do_det;
	bool do_info;

	bool do_extra;
	bool do_zero;
};

static void post_init_data_desc(struct scribe_ps *scribe,
				struct data_desc *desc)
{
	/*
	 * One not so intuitive thing: clear_user() can generate
	 * deterministic, or non-deterministic copies. It's just easier to
	 * implement the handlers.
	 * It will look as if copy_to_user(ptr, zero_page, size) was
	 * performed.
	 */

	if (desc->flags & SCRIBE_DATA_INPUT)
		desc->flags &= ~SCRIBE_DATA_NON_DETERMINISTIC;

	desc->do_non_det = desc->flags & SCRIBE_DATA_NON_DETERMINISTIC;
	desc->do_det = should_scribe_data_always(scribe) ||
			((desc->flags & SCRIBE_DATA_STRING) &&
			 should_scribe_data_string_always(scribe));
	desc->do_info = desc->flags & SCRIBE_DATA_NEED_INFO;
	desc->do_extra = should_scribe_data_extra(scribe) || is_mutating(scribe);
	desc->do_zero = desc->flags & SCRIBE_DATA_ZERO;

	if (desc->do_non_det)
		desc->do_det = false;

	if (desc->do_det || desc->do_non_det) {
		if (desc->do_info)
			desc->do_extra = true;
		desc->do_info = false;
	}
}

static bool need_action(struct scribe_ps *scribe, struct data_desc *desc)
{
	if (!desc->size)
		return false;

	if (!should_scribe_data(scribe))
		return false;

	if (is_kernel_copy())
		return false;

	if (desc->flags & SCRIBE_DATA_IGNORE)
		return false;

	return desc->do_det || desc->do_non_det || desc->do_info;
}

static int get_data_event(struct scribe_ps *scribe, struct data_desc *desc)
{
	union scribe_event_data_union event;

	if (is_recording(scribe)) {
		event = scribe->prepared_data_event;
		if (event.generic) {
			scribe->prepared_data_event.generic = NULL;

			if (desc->do_info) {
				/* we're good */
			} else if (desc->do_extra) {
				BUG_ON(event.extra->h.size < desc->size);
				event.extra->h.size = desc->size;
			} else {
				BUG_ON(event.regular->h.size < desc->size);
				event.regular->h.size = desc->size;
			}
			goto out;
		}

		if (desc->do_info)
			event.info = scribe_alloc_event(
					SCRIBE_EVENT_DATA_INFO);
		else if (desc->do_extra)
			event.extra = scribe_alloc_event_sized(
					SCRIBE_EVENT_DATA_EXTRA, desc->size);
		else
			event.regular = scribe_alloc_event_sized(
					SCRIBE_EVENT_DATA, desc->size);
		if (!event.generic) {
			scribe_kill(scribe->ctx, -ENOMEM);
			return -ENOMEM;
		}
		goto out;
	} else /* replaying */ {
		event = scribe->prepared_data_event;
		if (event.generic) {
			scribe->prepared_data_event.generic = NULL;
			goto out;
		}

		/*
		 * Not using scribe_dequeue_event_sized() because we don't
		 * really know the size (maybe we are in
		 * scribe_prepare_data_event() and @desc->size would only be the
		 * maximum size).
		 */

		/*
		 * XXX XXX We cannot dequeue in the middle of the stream
		 * because our data event may act as a fence.
		 * So we'll peek, which means that we'll have to dequeue it
		 * eventually.
		 */

		if (desc->do_info)
			event.generic = scribe_find_event_specific(
					scribe, SCRIBE_EVENT_DATA_INFO);
		else if (desc->do_extra)
			event.generic = scribe_find_event_specific(
					scribe, SCRIBE_EVENT_DATA_EXTRA);
		else
			event.generic = scribe_find_event_specific(
					scribe, SCRIBE_EVENT_DATA);
		if (IS_ERR(event.generic))
			return PTR_ERR(event.generic);
	}
out:
	desc->event = event;
	return 0;
}

void scribe_prepare_data_event(size_t pre_alloc_size)
{
	struct scribe_ps *scribe = current->scribe;
	struct data_desc desc;

	if (!is_scribed(scribe))
		return;

	desc.data = NULL;
	desc.user_ptr = NULL;
	desc.size = pre_alloc_size;
	desc.flags = scribe->data_flags;
	desc.event.generic = NULL;

	post_init_data_desc(scribe, &desc);

	if (!need_action(scribe, &desc))
		return;

	if (get_data_event(scribe, &desc))
		return;

	scribe->prepared_data_event = desc.event;
}
EXPORT_SYMBOL(scribe_prepare_data_event);

void scribe_pre_uaccess(const void *data, const void __user *user_ptr,
			size_t size, int flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!is_scribed(scribe))
		return;

	if (!is_kernel_copy() && size)
		__scribe_allow_uaccess(scribe);
	scribe->to_be_copied_size = size;
}
EXPORT_SYMBOL(scribe_pre_uaccess);

/*
 * This version of memcmp() returns the offset of the mismatch,
 * or -1 when the buffers are matching
 */
static int __memcmp(const void *cs, const void *ct, size_t count)
{
	const unsigned char *su1, *su2;
	size_t orig_count = count;

	for (su1 = cs, su2 = ct; count > 0; ++su1, ++su2, count--)
		if (*su1 != *su2)
			return orig_count - count;;
	return -1;
}

struct data_copy_args {
	const void *data;
	size_t count;
};

static inline void data_copy_hook(struct data_copy_args *args,
			   struct scribe_event_diverge_data_content *de)
{
	const void *data = args->data;
	size_t count = args->count;

	de->offset = 0;
	de->size = min(count, sizeof(de->data));
	memcpy(de->data, data, de->size);
	memset(de->data + de->size, 0, sizeof(de->data) - de->size);
}

static inline void diverge_on_data(struct scribe_ps *scribe,
				   const void *data, size_t count)
{
	scribe_diverge_hooked(scribe, &data_copy_hook,
			       (&(struct data_copy_args){data, count}),
			       SCRIBE_EVENT_DIVERGE_DATA_CONTENT);
}

void scribe_ensure_data_correctness(struct scribe_ps *scribe,
				    const void *recorded_data, const void *data,
				    size_t count)
{
	int offset;

	offset = __memcmp(recorded_data, data, count);

	if (offset != -1)
		diverge_on_data(scribe, data, count);
}

static void scribe_post_uaccess_record(struct scribe_ps *scribe,
				       struct data_desc *desc)
{
	void *event_data;

	if (desc->do_info) {
		desc->event.info->user_ptr = (__u32)desc->user_ptr;
		desc->event.info->size = desc->size;
		desc->event.info->data_type = desc->flags;
		event_data = NULL;
	} else if (desc->do_extra) {
		desc->event.extra->data_type = desc->flags;
		desc->event.extra->user_ptr = (__u32)desc->user_ptr;
		event_data = desc->event.extra->data;
	} else
		event_data = desc->event.regular->data;

	if (event_data) {
		if (desc->flags & SCRIBE_DATA_ZERO)
			memset(event_data, 0, desc->size);
		else
			memcpy(event_data, desc->data, desc->size);
	}

	scribe_queue_event(scribe->queue, desc->event.generic);
	desc->event.generic = NULL;
}

static inline int check_info(struct scribe_ps *scribe,
			     struct data_desc *desc,
			     void __user *recorded_user_ptr,
			     size_t recorded_size, unsigned int recorded_flags)
{
	if (recorded_user_ptr != desc->user_ptr) {
		scribe_diverge(scribe, SCRIBE_EVENT_DIVERGE_DATA_PTR,
			       .user_ptr = (u32)desc->user_ptr);
		return -EDIVERGE;
	}

	/* We don't want to check for the data_zero flag since it may change */
	if ((recorded_flags & ~SCRIBE_DATA_ZERO) !=
	    (desc->flags & ~SCRIBE_DATA_ZERO)) {
		scribe_diverge(scribe, SCRIBE_EVENT_DIVERGE_DATA_TYPE,
			       .type = desc->flags);
		return -EDIVERGE;
	}

	if (recorded_size != desc->size) {
		diverge_on_data(scribe, desc->data, desc->size);
		return -EDIVERGE;
	}

	return 0;
}

static void scribe_post_uaccess_replay(struct scribe_ps *scribe,
				       struct data_desc *desc)
{
	const void *event_data;
	int old_data_flags;
	int ret;

	if (desc->do_info) {
		check_info(scribe, desc,
			   (void __user *)desc->event.info->user_ptr,
			   desc->event.info->size,
			   desc->event.info->data_type);
		return;
	}

	if (desc->do_extra) {
		ret = check_info(scribe, desc,
				 (void __user *)desc->event.extra->user_ptr,
				 desc->event.extra->h.size,
				 desc->event.extra->data_type);
	} else {
		ret = check_info(scribe, desc,
				 desc->user_ptr,
				 desc->event.extra->h.size,
				 desc->flags);
	}

	if (ret)
		return;


	if (desc->do_zero) {
		/*
		 * Avoiding the use of scribe_data_ignore so that we
		 * don't pollute the data flags 'stack'.
		 */
		old_data_flags = scribe->data_flags;
		scribe->data_flags = SCRIBE_DATA_IGNORE;
		if (__clear_user(desc->user_ptr, desc->size))
			scribe_kill(scribe->ctx, -EDIVERGE);
		scribe->data_flags = old_data_flags;
		return;
	}

	event_data = desc->do_extra ? desc->event.extra->data :
				      desc->event.regular->data;

	if (!(desc->flags & SCRIBE_DATA_NON_DETERMINISTIC)) {
		if (likely(desc->data))
			scribe_ensure_data_correctness(scribe, event_data,
						       desc->data, desc->size);
		else
			scribe_diverge(scribe, SCRIBE_EVENT_DIVERGE_DATA_TYPE,
				       .type = SCRIBE_DATA_NON_DETERMINISTIC);

		return;
	}

	/*
	 * FIXME Do the copying in pre_uaccess and skip the extra copy_to_user
	 * that happened before.
	 */
	old_data_flags = scribe->data_flags;
	scribe->data_flags = SCRIBE_DATA_IGNORE;
	/*
	 * We're using the inatomic version so that we don't get the
	 * might_sleep(), but if we're not in an atomic context, it's
	 * equivalent to __copy_to_user().
	 */
	if (__copy_to_user_inatomic(desc->user_ptr, event_data, desc->size)) {
		/*
		 * FIXME If we are in an atomic region, the copy may or may
		 * not have happended. We need to make sure that the copy
		 * happens anyway.
		 */
		WARN(in_atomic(), "Need to implement proper "
				  "atomic copies in replay\n");
		scribe_kill(scribe->ctx, -EDIVERGE);
	}

	scribe->data_flags = old_data_flags;
}

static void __scribe_post_uaccess(struct scribe_ps *scribe,
				  struct data_desc *desc)
{
	struct scribe_event *event;

	if (!need_action(scribe, desc))
		goto out;

	WARN_ON((long)desc->user_ptr > TASK_SIZE);

	if (get_data_event(scribe, desc))
		goto out;

	if (is_recording(scribe))
		scribe_post_uaccess_record(scribe, desc);
	else /* replay */ {
		scribe_post_uaccess_replay(scribe, desc);

		event = scribe_peek_event(scribe->queue, SCRIBE_WAIT);
		WARN_ON(IS_ERR(event));
		if (event == desc->event.generic)
			scribe_dequeue_event(scribe->queue, SCRIBE_NO_WAIT);
		desc->event.generic = NULL;
	}

out:
	if (!is_kernel_copy() && scribe->to_be_copied_size)
		__scribe_forbid_uaccess(scribe);
	WARN(scribe->prepared_data_event.generic,
	     "pre-allocated data event not used\n");
}

void scribe_post_uaccess(const void *data, const void __user *user_ptr,
			 size_t size, int flags)
{
	struct scribe_ps *scribe = current->scribe;
	struct data_desc desc;

	if (!is_scribed(scribe))
		return;

	desc.data = (void *)data;
	desc.user_ptr = (void __user *)user_ptr;
	desc.size = size;
	desc.flags = scribe->data_flags | flags;
	desc.event.generic = NULL;

	post_init_data_desc(scribe, &desc);
	__scribe_post_uaccess(scribe, &desc);
	scribe_free_event(desc.event.generic);
}
EXPORT_SYMBOL(scribe_post_uaccess);

static void scribe_copy_to_user_recorded(void __user *to, long n,
					 union scribe_event_data_union *event)
{
	struct data_desc desc;
	struct scribe_ps *scribe = current->scribe;

	BUG_ON(!is_replaying(scribe));

	desc.data = NULL;
	desc.user_ptr = to;
	desc.size = n;
	desc.flags = scribe->data_flags;
	desc.event.generic = NULL;
	post_init_data_desc(scribe, &desc);

	scribe_pre_uaccess(NULL, to, n, scribe->data_flags);
	__scribe_post_uaccess(scribe, &desc);

	if (event)
		*event = desc.event;
	else
		scribe_free_event(desc.event.generic);
}

/*
 * emul copy_to_user() calls.
 * if @buf is NULL, the user pointer will be read from the log file
 */
size_t scribe_emul_copy_to_user(struct scribe_ps *scribe,
				void __user *buf, ssize_t len)
{
	union scribe_event_data_union data_event;
	struct scribe_event *event = NULL;
	bool has_user_buf;
	size_t data_size, ret;
	unsigned int recorded_flags;

	BUG_ON(!(scribe->data_flags & SCRIBE_DATA_NON_DETERMINISTIC));

	has_user_buf = buf ? true : false;

	for (ret = 0; ret < len; ret += data_size, buf += data_size) {
		/*
		 * We are peeking events without a regular fence, but that's
		 * okay since we'll stop once ret >= len.
		 */

		event = scribe_find_event_specific(scribe,
				SCRIBE_EVENT_DATA_EXTRA, SCRIBE_EVENT_DATA);
		if (IS_ERR(event))
			return ret;

		data_event.generic = (struct scribe_event *)event;
		data_size = data_event.generic_sized->size;

		if (!has_user_buf) {
			if (event->type != SCRIBE_EVENT_DATA_EXTRA)
				return ret;
			buf = (void __user *)data_event.extra->user_ptr;
		}

		if (event->type == SCRIBE_EVENT_DATA_EXTRA) {
			recorded_flags = data_event.extra->data_type;
			recorded_flags &= ~(SCRIBE_DATA_ZERO |
					    SCRIBE_DATA_INPUT |
					    SCRIBE_DATA_STRING);
			if (recorded_flags != scribe->data_flags)
				return ret;
		}

		scribe_copy_to_user_recorded(buf, data_size, NULL);
	}
	return ret;
}

/*
 * emul copy_from_user() calls.
 */
size_t scribe_emul_copy_from_user(struct scribe_ps *scribe,
				  void __user *buf, ssize_t len)
{
	union scribe_event_data_union data_event;
	struct scribe_event *event;
	size_t data_size, ret;
	unsigned int recorded_flags;

	BUG_ON(scribe->data_flags & SCRIBE_DATA_NON_DETERMINISTIC);

	for (ret = 0; ret < len; ret += data_size, buf += data_size) {
		/*
		 * We are peeking events without a regular fence, but that's
		 * okey since we'll stop once ret >= len.
		 */
		event = scribe_peek_event(scribe->queue, SCRIBE_WAIT);
		if (IS_ERR(event))
			return ret;

		if (event->type != SCRIBE_EVENT_DATA_EXTRA &&
		    event->type != SCRIBE_EVENT_DATA &&
		    event->type != SCRIBE_EVENT_DATA_INFO)
			return ret;

		data_event.generic = (struct scribe_event *)event;

		if (event->type == SCRIBE_EVENT_DATA_INFO)
			data_size = data_event.info->size;
		else
			data_size = data_event.generic_sized->size;

		recorded_flags = -1;
		if (event->type == SCRIBE_EVENT_DATA_EXTRA)
			recorded_flags = data_event.extra->data_type;
		else if (event->type == SCRIBE_EVENT_DATA_INFO)
			recorded_flags = data_event.info->data_type;
		if (recorded_flags != -1) {
			recorded_flags &= ~(SCRIBE_DATA_ZERO |
					    SCRIBE_DATA_INPUT |
					    SCRIBE_DATA_STRING);
			if (recorded_flags != scribe->data_flags)
				return ret;
		}

		event = scribe_dequeue_event(scribe->queue, SCRIBE_NO_WAIT);

		if (!is_kernel_copy())
			__scribe_allow_uaccess(scribe);
		/* FIXME we do nothing for now (checking ?) ... */
		if (!is_kernel_copy())
			__scribe_forbid_uaccess(scribe);

		scribe_free_event(event);
	}
	return ret;
}

size_t scribe_emul_copy_to_user_iov(struct scribe_ps *scribe,
				    struct iovec *iov,
				    unsigned long nr_segs,
				    size_t len)
{
	int i;
	size_t to_xfer, xfered, ret = 0;

	for (i = 0; i < nr_segs; i++) {
		to_xfer = min(iov[i].iov_len, len);
		xfered = scribe_emul_copy_to_user(scribe,
						  iov[i].iov_base, to_xfer);
		ret += xfered;
		if (xfered < to_xfer)
			return ret;
	}
	return ret;
}

size_t scribe_emul_copy_from_user_iov(struct scribe_ps *scribe,
				      struct iovec *iov,
				      unsigned long nr_segs,
				      size_t len)
{
	int i;
	size_t to_xfer, xfered, ret = 0;

	for (i = 0; i < nr_segs; i++) {
		to_xfer = min(iov[i].iov_len, len);
		xfered = scribe_emul_copy_from_user(scribe,
						    iov[i].iov_base, to_xfer);
		ret += xfered;
		if (xfered < to_xfer)
			return ret;
	}
	return ret;
}

int __scribe_buffer_record(struct scribe_ps *scribe, scribe_insert_point_t *ip,
			   const void *data, size_t size)
{
	int data_extra = should_scribe_data_extra(scribe);
	int need_info = scribe->data_flags & SCRIBE_DATA_NEED_INFO;
	union scribe_event_data_union event;

	data_extra |= need_info;

	if (data_extra)
		event.extra = scribe_alloc_event_sized(
						SCRIBE_EVENT_DATA_EXTRA, size);
	else
		event.regular = scribe_alloc_event_sized(
						SCRIBE_EVENT_DATA, size);

	if (!event.generic)
		return -ENOMEM;

	if (data_extra) {
		event.extra->data_type = SCRIBE_DATA_INTERNAL | need_info;
		event.extra->user_ptr = 0;
		memcpy(event.extra->data, data, size);
	} else {
		memcpy(event.regular->data, data, size);
	}
	scribe_queue_event_at(ip, event.regular);
	return 0;
}

int __scribe_buffer_replay(struct scribe_ps *scribe, void *data, size_t size)
{
	int data_extra = should_scribe_data_extra(scribe);
	union scribe_event_data_union event;

	 data_extra |= scribe->data_flags & SCRIBE_DATA_NEED_INFO;

	if (data_extra)
		event.extra = scribe_dequeue_event_sized(scribe,
						 SCRIBE_EVENT_DATA_EXTRA, size);
	else
		event.regular = scribe_dequeue_event_sized(scribe,
						 SCRIBE_EVENT_DATA, size);

	if (IS_ERR(event.generic))
		return PTR_ERR(event.generic);

	if (data_extra) {
		if (!(event.extra->data_type & SCRIBE_DATA_INTERNAL)) {
			scribe_free_event(event.generic);
			scribe_diverge(scribe, SCRIBE_EVENT_DIVERGE_DATA_TYPE,
				       .type = SCRIBE_DATA_INTERNAL);
			return -EDIVERGE;
		}

		memcpy(data, event.extra->data, size);
	} else {
		memcpy(data, event.regular->data, size);
	}

	scribe_free_event(event.generic);
	return 0;
}

int scribe_buffer_at(void *buffer, size_t size, scribe_insert_point_t *ip)
{
	struct scribe_ps *scribe = current->scribe;

	if (!is_scribed(scribe) || !should_scribe_data(scribe))
		return 0;

	if (is_recording(scribe)) {
		if (!ip)
			ip = &scribe->queue->stream.master;
		return __scribe_buffer_record(scribe, ip, buffer, size);
	} else
		return __scribe_buffer_replay(scribe, buffer, size);
}

void scribe_allow_uaccess(void)
{
	struct scribe_ps *scribe = current->scribe;
	if (!is_scribed(scribe))
		return;

	__scribe_allow_uaccess(scribe);
}
EXPORT_SYMBOL(scribe_allow_uaccess);

void scribe_forbid_uaccess(void)
{
	struct scribe_ps *scribe = current->scribe;
	if (!is_scribed(scribe))
		return;

	__scribe_forbid_uaccess(scribe);
}
EXPORT_SYMBOL(scribe_forbid_uaccess);

void scribe_pre_schedule(void)
{
	struct scribe_ps *scribe = current->scribe;
	if (!is_scribed(scribe))
		return;

	scribe_mem_schedule_in(scribe);
}

void scribe_post_schedule(void)
{
	struct scribe_ps *scribe = current->scribe;
	if (!is_scribed(scribe))
		return;

	scribe_mem_schedule_out(scribe);
}

int fault_in_pages_writeable(char __user *uaddr, int size)
{
	struct scribe_ps *scribe = current->scribe;
	int old_data_flags = 0;
	int ret;

	if (may_be_scribed(scribe)) {
		old_data_flags = scribe->data_flags;
		scribe->data_flags = SCRIBE_DATA_IGNORE;
	}
	ret = __fault_in_pages_writeable(uaddr, size);
	if (may_be_scribed(scribe))
		scribe->data_flags = old_data_flags;

	return ret;
}

int fault_in_pages_readable(char __user *uaddr, int size)
{
	struct scribe_ps *scribe = current->scribe;
	int old_data_flags = 0;
	int ret;

	if (may_be_scribed(scribe)) {
		old_data_flags = scribe->data_flags;
		scribe->data_flags = SCRIBE_DATA_IGNORE;
	}
	ret = __fault_in_pages_readable(uaddr, size);
	if (may_be_scribed(scribe))
		scribe->data_flags = old_data_flags;

	return ret;
}

/* XXX There is only one level of data_flags stack levels */
void scribe_data_push_flags(int flags)
{
	struct scribe_ps *scribe = current->scribe;
	if (!may_be_scribed(scribe))
		return;

	scribe->old_data_flags = scribe->data_flags;
	scribe->data_flags = flags;
}
EXPORT_SYMBOL(scribe_data_push_flags);

void scribe_data_det(void)
{
	scribe_data_push_flags(0);
}
EXPORT_SYMBOL(scribe_data_det);

void scribe_data_non_det(void)
{
	scribe_data_push_flags(SCRIBE_DATA_NON_DETERMINISTIC);
}
EXPORT_SYMBOL(scribe_data_non_det);

void scribe_data_need_info(void)
{
	scribe_data_push_flags(SCRIBE_DATA_NEED_INFO);
}

void scribe_data_non_det_need_info(void)
{
	scribe_data_push_flags(SCRIBE_DATA_NON_DETERMINISTIC |
			       SCRIBE_DATA_NEED_INFO);
}
EXPORT_SYMBOL(scribe_data_non_det_need_info);

void scribe_data_ignore(void)
{
	scribe_data_push_flags(SCRIBE_DATA_IGNORE);
}
EXPORT_SYMBOL(scribe_data_ignore);

void scribe_data_pop_flags(void)
{
	struct scribe_ps *scribe = current->scribe;
	if (!is_scribed(scribe))
		return;

	scribe->data_flags = scribe->old_data_flags;
}
EXPORT_SYMBOL(scribe_data_pop_flags);
