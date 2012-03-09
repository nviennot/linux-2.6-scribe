/*
 *  Scribe, the record/replay mechanism
 *
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

/*
 * This file contains event definitions, it should not be included directly
 */

#ifndef __SCRIBE_EVENT
#error "Do not include this file directly, use <linux/scribe_api.h>"
#endif

/* Default settings */
#ifndef __header_regular
#define __header_regular
#endif

#ifndef __header_sized
#define __header_sized
#endif

#ifndef __header_diverge
#define __header_diverge
#endif

#ifndef SCRIBE_EVENT
#define SCRIBE_EVENT(name, ...)				\
	__SCRIBE_EVENT(scribe_event_##name,		\
		       __header_regular __VA_ARGS__)
#endif

#ifndef __SCRIBE_EVENT_SIZED
#define __SCRIBE_EVENT_SIZED __SCRIBE_EVENT
#endif

#ifndef SCRIBE_EVENT_SIZED
#define SCRIBE_EVENT_SIZED(name, ...)			\
	__SCRIBE_EVENT_SIZED(scribe_event_##name,	\
			     __header_sized __VA_ARGS__)
#endif

#ifndef __SCRIBE_EVENT_DIVERGE
#define __SCRIBE_EVENT_DIVERGE __SCRIBE_EVENT
#endif

#ifndef SCRIBE_EVENT_DIVERGE
#define SCRIBE_EVENT_DIVERGE(name, ...)				\
	__SCRIBE_EVENT_DIVERGE(scribe_event_diverge_##name,	\
			       __header_diverge __VA_ARGS__)
#endif

#ifndef SCRIBE_START_LOG_FILE_DECL
#define SCRIBE_START_LOG_FILE_DECL
#endif

#ifndef SCRIBE_START_COMMAND_DECL
#define SCRIBE_START_COMMAND_DECL
#endif

#ifndef SCRIBE_START_NOTIFICATION_DECL
#define SCRIBE_START_NOTIFICATION_DECL
#endif

#ifndef SCRIBE_START_DIVERGE_NOTIFICATION_DECL
#define SCRIBE_START_DIVERGE_NOTIFICATION_DECL
#endif

/* The declarations begin here */

/* Log file events */
SCRIBE_START_LOG_FILE_DECL

SCRIBE_EVENT_SIZED(init,
	__field(__u32, flags)
	__field(__u16, argc)
	__field(__u16, envc)
	__field(__u8, data[0])
)

SCRIBE_EVENT(pid,
	__field(__u32, pid)
)

SCRIBE_EVENT(data_info,
	__field(__u32, user_ptr)
	__field(__u16, size)
	__field(__u8, data_type)
)

SCRIBE_EVENT_SIZED(data,
	__field(__u8, data[0])
	__field(__u32, ldata[0])
)

SCRIBE_EVENT_SIZED(data_extra,
	__field(__u32, user_ptr) /* FIXME 64 bit support ? */
	__field(__u8, data_type)
	__field(__u8, data[0])
	__field(__u32, ldata[0])
)

SCRIBE_EVENT(syscall,
	__field(__s32, ret)
)

SCRIBE_EVENT_SIZED(syscall_extra,
	__field(__s32, ret)
	__field(__u16, nr)
	__field(__u32, args[0])
)

SCRIBE_EVENT(syscall_end)

SCRIBE_EVENT(queue_eof)

SCRIBE_EVENT(resource_lock,
	__field(__u32, serial)
)

SCRIBE_EVENT(resource_lock_intr)

SCRIBE_EVENT_SIZED(resource_lock_extra,
	__field(__u8, type)
	__field(__u8, write_access)
	__field(__u32, id)
	__field(__u32, serial)
	__field(__u8, desc[0])
)

SCRIBE_EVENT(resource_unlock,
	__field(__u32, id)
)

SCRIBE_EVENT(rdtsc,
	__field(__u64, tsc)
)

SCRIBE_EVENT_SIZED(signal,
	__field(__u8, nr)
	__field(__u8, deferred)
	__field(__u8, info[0])
)

SCRIBE_EVENT(fence,
	__field(__u32, serial)
)

SCRIBE_EVENT(mem_owned_read,
	__field(__u32, serial)
)

SCRIBE_EVENT(mem_owned_write,
	__field(__u32, serial)
)

SCRIBE_EVENT(mem_owned_read_extra,
	__field(__u32, id)
	__field(__u32, address)
	__field(__u32, serial)
)

SCRIBE_EVENT(mem_owned_write_extra,
	__field(__u32, id)
	__field(__u32, address)
	__field(__u32, serial)
)

SCRIBE_EVENT(mem_public_read,
	__field(__u32, address)
)

SCRIBE_EVENT(mem_public_write,
	__field(__u32, address)
)

SCRIBE_EVENT(mem_alone)

SCRIBE_EVENT(regs,
	__field(struct pt_regs, regs)
)

SCRIBE_EVENT(bookmark,
	__field(__u8, type)
	__field(__u32, id)
	__field(__u32, npr)
)

SCRIBE_EVENT(sig_send_cookie,
	__field(__u32, cookie)
)

SCRIBE_EVENT(sig_recv_cookie,
	__field(__u32, cookie)
)

SCRIBE_EVENT(sig_handled_cookie,
	__field(__u32, cookie)
)

SCRIBE_EVENT(sig_handled,
	__field(__u8, nr)
)

SCRIBE_EVENT_SIZED(set_flags,
	__field(__u32, flags)
	__field(__u8, duration)
	__field(__u8, extra[0])
)

/* Command events */
SCRIBE_START_COMMAND_DECL

SCRIBE_EVENT(attach_on_execve,
	__field(__u8, enable)
)

SCRIBE_EVENT(record,
	__field(__u32, flags)
	__field(__u32, log_fd)
)

SCRIBE_EVENT(replay,
	__field(__u32, flags)
	__field(__u32, log_fd)
	__field(__s32, backtrace_len)
)

SCRIBE_EVENT(stop)

SCRIBE_EVENT(bookmark_request)

SCRIBE_EVENT(check_deadlock)

SCRIBE_EVENT(resume)

/* Notification events */
SCRIBE_START_NOTIFICATION_DECL

SCRIBE_EVENT(backtrace,
	__field(__u64, event_offset)
)

SCRIBE_EVENT(context_idle,
	__field(__s32, error)
)

SCRIBE_EVENT(bookmark_reached,
	__field(__u32, id)
	__field(__u32, npr)
)

SCRIBE_EVENT(on_attach,
	__field(__u32, real_pid)
	__field(__u32, scribe_pid)
)

/* Diverge Notification events */
SCRIBE_START_DIVERGE_NOTIFICATION_DECL

SCRIBE_EVENT_DIVERGE(event_type,
	__field(__u8, type)
)

SCRIBE_EVENT_DIVERGE(event_size,
	__field(__u16, size)
)

SCRIBE_EVENT_DIVERGE(data_type,
	__field(__u8, type)
)

SCRIBE_EVENT_DIVERGE(data_ptr,
	__field(__u32, user_ptr)
)

SCRIBE_EVENT_DIVERGE(data_content,
	__field(__u16, offset)
	__field(__u8, size)
	__field(__u8, data[128])
)

SCRIBE_EVENT_DIVERGE(resource_type,
	__field(__u8, type)
)

SCRIBE_EVENT_DIVERGE(syscall,
	__field(__u16, nr)
	__field(__u8, num_args)
	__field(__u32, args[6])
)

SCRIBE_EVENT_DIVERGE(syscall_ret,
	__field(__s32, ret)
)

SCRIBE_EVENT_DIVERGE(fence_serial,
	__field(__u32, serial)
)

SCRIBE_EVENT_DIVERGE(mem_owned,
	__field(__u32, address)
	__field(__u8, write_access)
)

SCRIBE_EVENT_DIVERGE(mem_not_owned)

SCRIBE_EVENT_DIVERGE(regs,
	__field(struct pt_regs, regs)
)

SCRIBE_EVENT_DIVERGE(queue_not_empty)

#undef __header_regular
#undef __header_sized
#undef __header_diverge
#undef __SCRIBE_EVENT
#undef SCRIBE_EVENT
#undef __SCRIBE_EVENT_SIZED
#undef SCRIBE_EVENT_SIZED
#undef __SCRIBE_EVENT_DIVERGE
#undef SCRIBE_EVENT_DIVERGE
#undef SCRIBE_START_LOG_FILE_DECL
#undef SCRIBE_START_COMMAND_DECL
#undef SCRIBE_START_NOTIFICATION_DECL
#undef SCRIBE_START_DIVERGE_NOTIFICATION_DECL
