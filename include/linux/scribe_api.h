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
 * This is the include file that gets shared with userspace.
 * It describes the events that are used in two places:
 * - The logfile, which is the output of a scribe recording
 * - The protocol of the scribe device (through read() and write()).
 */

#ifndef _LINUX_SCRIBE_API_H
#define _LINUX_SCRIBE_API_H

#include <linux/types.h>
#include <linux/ptrace.h>
#include <linux/net.h>
#include <linux/futex.h>
#include <linux/scribe_defines.h>
#include <linux/scribe_resource.h>

#ifdef __KERNEL__
#include <linux/list.h>
#else
#include <sys/types.h>
#ifndef __always_inline
#define __always_inline inline
#endif
#endif /* __KERNEL__ */

/*
 * FIXME This file contain architecture dependent code, such as EDIVERGE,
 * the 32/64bits values, the pt_regs struct...
 */
#define EDIVERGE			200	/* Replay diverged */

#define SCRIBE_DEVICE_NAME		"scribe"


/*
 * These flags are used in the context flags, they are passed along with the
 * record/replay command
 */
#ifdef __KERNEL__
#define SCRIBE_RECORD			0x00000001
#define SCRIBE_REPLAY			0x00000002
#define SCRIBE_STOP			0x00000004
#define SCRIBE_STATE_MASK		(SCRIBE_RECORD |  SCRIBE_REPLAY | \
					 SCRIBE_STOP)
#endif

#define SCRIBE_SYSCALL_RET		0x00000100
#define SCRIBE_SYSCALL_EXTRA		0x00000200
#define SCRIBE_SIG_EXTRA		0x00000400
#define SCRIBE_SIG_COOKIE		0x00000800
#define SCRIBE_RES_EXTRA		0x00001000
#define SCRIBE_MEM_EXTRA		0x00002000
#define SCRIBE_DATA_EXTRA		0x00004000
#define SCRIBE_DATA_STRING_ALWAYS	0x00008000
#define SCRIBE_DATA_ALWAYS		0x00010000
#define SCRIBE_RES_ALWAYS		0x00020000
#define SCRIBE_FENCE_ALWAYS		0x00040000
#define SCRIBE_REGS			0x00080000
#define SCRIBE_ALL			0x00ffff00
#define SCRIBE_DEFAULT			(SCRIBE_SYSCALL_EXTRA)

#define SCRIBE_DISABLE_MM		0x01000000
#define SCRIBE_DISABLE_FUTEX_HASH	0x02000000
#define SCRIBE_FLAGS_MASK		0xffffff00

/*
 * These flags are used for the scribe syscalls such as sys_set_scribe_flags().
 */
#define SCRIBE_PS_RECORD		0x00000001
#define SCRIBE_PS_REPLAY		0x00000002
#define SCRIBE_PS_ATTACH_ON_EXEC	0x00000004
#define SCRIBE_PS_DETACHING		0x00000008
#define SCRIBE_PS_ENABLE_SYSCALL	0x00000100
#define SCRIBE_PS_ENABLE_DATA		0x00000200
#define SCRIBE_PS_ENABLE_RESOURCE	0x00000400
#define SCRIBE_PS_ENABLE_SIGNAL		0x00000800
#define SCRIBE_PS_ENABLE_TSC		0x00001000
#define SCRIBE_PS_ENABLE_MM		0x00002000
#define SCRIBE_PS_RET_CHECK		0x00004000
#define SCRIBE_PS_STRICT_REPLAY		0x00008000
#define SCRIBE_PS_FIXED_IO		0x00010000
#define SCRIBE_PS_ENABLE_ALL		0x00ffff00

/*
 * These flags are used as a data type
 * They are also defined in scribe_uaccess.h
 */
#define SCRIBE_DATA_INPUT		0x01
#define SCRIBE_DATA_STRING		0x02
#define SCRIBE_DATA_NON_DETERMINISTIC	0x04
#define SCRIBE_DATA_INTERNAL		0x08
#define SCRIBE_DATA_ZERO		0x10
#define SCRIBE_DATA_NEED_INFO		0x20
#ifdef __KERNEL__
#define SCRIBE_DATA_IGNORE		0x40
#endif

/*
 * Bookmark types
 */
#define SCRIBE_BOOKMARK_PRE_SYSCALL	0x00
#define SCRIBE_BOOKMARK_POST_SYSCALL	0x01


/*
 * Duration arguments
 */
#define SCRIBE_PERMANANT		0x00
#define SCRIBE_UNTIL_NEXT_SYSCALL	0x01

/*
 * Syscalls offsets for multiplexed calls
 */
#define SCRIBE_SOCKETCALL_BASE		0xf000
#define SCRIBE_FUTEX_BASE		0xf100
#define SCRIBE_SYSCALL_BASE_MASK	0xff00

#define __NR_socket		(SCRIBE_SOCKETCALL_BASE + SYS_SOCKET)
#define __NR_bind		(SCRIBE_SOCKETCALL_BASE + SYS_BIND)
#define __NR_connect		(SCRIBE_SOCKETCALL_BASE + SYS_CONNECT)
#define __NR_listen		(SCRIBE_SOCKETCALL_BASE + SYS_LISTEN)
#define __NR_accept		(SCRIBE_SOCKETCALL_BASE + SYS_ACCEPT)
#define __NR_getsockname	(SCRIBE_SOCKETCALL_BASE + SYS_GETSOCKNAME)
#define __NR_getpeername	(SCRIBE_SOCKETCALL_BASE + SYS_GETPEERNAME)
#define __NR_socketpair		(SCRIBE_SOCKETCALL_BASE + SYS_SOCKETPAIR)
#define __NR_send		(SCRIBE_SOCKETCALL_BASE + SYS_SEND)
#define __NR_recv		(SCRIBE_SOCKETCALL_BASE + SYS_RECV)
#define __NR_sendto		(SCRIBE_SOCKETCALL_BASE + SYS_SENDTO)
#define __NR_recvfrom		(SCRIBE_SOCKETCALL_BASE + SYS_RECVFROM)
#define __NR_shutdown		(SCRIBE_SOCKETCALL_BASE + SYS_SHUTDOWN)
#define __NR_setsockopt		(SCRIBE_SOCKETCALL_BASE + SYS_SETSOCKOPT)
#define __NR_getsockopt		(SCRIBE_SOCKETCALL_BASE + SYS_GETSOCKOPT)
#define __NR_sendmsg		(SCRIBE_SOCKETCALL_BASE + SYS_SENDMSG)
#define __NR_recvmsg		(SCRIBE_SOCKETCALL_BASE + SYS_RECVMSG)
#define __NR_accept4		(SCRIBE_SOCKETCALL_BASE + SYS_ACCEPT4)
#define __NR_recvmmsg2		(SCRIBE_SOCKETCALL_BASE + SYS_RECVMMSG)

#define __NR_futex_wait		(SCRIBE_FUTEX_BASE + FUTEX_WAIT)
#define __NR_futex_wake		(SCRIBE_FUTEX_BASE + FUTEX_WAKE)
#define __NR_futex_fd		(SCRIBE_FUTEX_BASE + FUTEX_FD)
#define __NR_futex_requeue	(SCRIBE_FUTEX_BASE + FUTEX_REQUEUE)
#define __NR_futex_cmp_requeue	(SCRIBE_FUTEX_BASE + FUTEX_CMP_REQUEUE)
#define __NR_futex_wake_op	(SCRIBE_FUTEX_BASE + FUTEX_WAKE_OP)
#define __NR_futex_lock_pi	(SCRIBE_FUTEX_BASE + FUTEX_LOCK_PI)
#define __NR_futex_unlock_pi	(SCRIBE_FUTEX_BASE + FUTEX_UNLOCK_PI)
#define __NR_futex_trylock_pi	(SCRIBE_FUTEX_BASE + FUTEX_TRYLOCK_PI)
#define __NR_futex_wait_bitset	(SCRIBE_FUTEX_BASE + FUTEX_WAIT_BITSET)
#define __NR_futex_wake_bitset	(SCRIBE_FUTEX_BASE + FUTEX_WAKE_BITSET)
#define __NR_futex_wait_requeue_pi (SCRIBE_FUTEX_BASE + FUTEX_WAIT_REQUEUE_PI)
#define __NR_futex_cmp_requeue_pi (SCRIBE_FUTEX_BASE + FUTEX_CMP_REQUEUE_PI)


enum scribe_event_type {
	SCRIBE_EVENT_DUMMY1 = 0, /* skip the type 0 for safety */

#define __SCRIBE_EVENT(name, ...) upper##name,
#define SCRIBE_START_COMMAND_DECL \
	SCRIBE_EVENT_DUMMY2 = 127, /*
				    * Start all device events at 128, it helps
				    * for backward compatibility.
				    */
	#include <linux/scribe_events.h>
};

struct scribe_event {
#ifdef __KERNEL__
	struct list_head node;
	/*
	 * The log_offset is only used during replay for diverge backtraces and
	 * the diverge event.
	 * This way, userspace can figure out where and what happened.
	 */
	loff_t log_offset;
	__u8 __align__[3];
	char payload_offset[0];
	/*
	 * type must directly follow payload_offset,
	 * serialization routines depend on it.
	 */
#endif
	__u8 type;
} __attribute__((packed));

struct scribe_event_sized {
	struct scribe_event h;
	__u16 size;
} __attribute__((packed));

struct scribe_event_diverge {
	struct scribe_event h;
	__u32 pid;
	__u32 fatal;
	__u32 num_ev_consumed;
	__u64 last_event_offset;
} __attribute__((packed));

#define __SCRIBE_EVENT(name, ...)	\
	struct name {			\
		__VA_ARGS__		\
	} __attribute__((packed));
#define __header_regular	struct scribe_event h;
#define __header_sized		struct scribe_event_sized h;
#define __header_diverge	struct scribe_event_diverge h;
#define __field(type, name)	type name;
#include <linux/scribe_events.h>

static inline int is_diverge_type(int type)
{
#define __SCRIBE_EVENT(...)
#define __SCRIBE_EVENT_SIZED(...)
#define __SCRIBE_EVENT_DIVERGE(name, ...) type == upper##name ||
	return
		#include <linux/scribe_events.h>
		0;
}

static __always_inline int is_sized_type(int type)
{
#define __SCRIBE_EVENT(...)
#define __SCRIBE_EVENT_SIZED(name, ...) type == upper##name ||
#define __SCRIBE_EVENT_DIVERGE(...)
	return
		#include <linux/scribe_events.h>
		0;
}

void you_are_using_an_unknown_scribe_type(void);
/*
 * XXX The additional payload of sized event is NOT accounted here.
 */
static __always_inline size_t sizeof_event_from_type(__u8 type)
{
#define __SCRIBE_EVENT(name, ...)	\
	if (type == upper##name) return sizeof(struct name);
	#include <linux/scribe_events.h>

	if (__builtin_constant_p(type))
		you_are_using_an_unknown_scribe_type();

	return (size_t)-1;
}

static inline size_t sizeof_event(struct scribe_event *event)
{
	size_t sz = sizeof_event_from_type(event->type);
	if (is_sized_type(event->type))
		sz += ((struct scribe_event_sized *)event)->size;
	return sz;
}

#ifdef __KERNEL__
static inline size_t sizeof_event_payload(struct scribe_event *event)
{
	return sizeof_event(event) - offsetof(__typeof__(*event),
					      payload_offset);
}

static inline char *get_event_payload(struct scribe_event *event)
{
	return (char *)(event->payload_offset);
}
#endif /* __KERNEL __ */

#endif /* _LINUX_SCRIBE_API_H_ */
