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

#ifndef _LINUX_SCRIBE_UACCESS_H_
#define _LINUX_SCRIBE_UACCESS_H_

#define SCRIBE_DATA_INPUT		0x01
#define SCRIBE_DATA_STRING		0x02
#define SCRIBE_DATA_NON_DETERMINISTIC	0x04
#define SCRIBE_DATA_INTERNAL		0x08
#define SCRIBE_DATA_ZERO		0x10
#define SCRIBE_DATA_NEED_INFO		0x20
#define SCRIBE_DATA_IGNORE		0x40

/*
 * For x86, all user accesses can be probed by hooking on:
 *	get_user
 *	put_user
 *	__get_user_nocheck
 *	__put_user_nocheck
 *	__get_user_size_ex
 *	__put_user_size_ex
 *	__copy_to_user
 *	__copy_from_user
 *
 *	__copy_to_user_inatomic
 *	__copy_from_user_inatomic
 *	__copy_from_user_nocache
 *	__copy_from_user_inatomic_nocache
 *
 *	__do_strncpy_from_user
 *	__do_clear_user
 *	strnlen_user
 */


#ifdef CONFIG_SCRIBE

/* FIXME Those two functions should be inlined */
extern void scribe_pre_uaccess(const void *data, const void __user *user_ptr,
			       size_t size, int flags);
extern long scribe_post_uaccess(const void *data, const void __user *user_ptr,
				size_t size, int flags);

extern pgd_t *scribe_get_pgd(struct mm_struct *next, struct task_struct *tsk);

struct scribe_ps;

#ifndef may_be_scribed
#define may_be_scribed may_be_scribed
static inline int may_be_scribed(struct scribe_ps *scribe)
{
	return scribe != NULL;
}
#endif /* may_be_scribed */

#else /* CONFIG_SCRIBE */

static inline void scribe_pre_uaccess(const void *data,
				      const void __user *user_ptr, size_t size,
				      int flags) {}
static inline void scribe_post_uaccess(const void *data,
				       const void __user *user_ptr,
				       size_t size, int flags) {}

static inline pgd_t *scribe_get_pgd(struct mm_struct *next,
				    struct task_struct *tsk)
{
	return NULL;
}

#ifndef may_be_scribed
#define may_be_scribed
static inline int may_be_scribed(struct scribe_ps *scribe)
{
	return 0;
}
#endif /* may_be_scribed */

#endif /* CONFIG_SCRIBE */

#endif /* _LINUX_SCRIBE_UACCESS_H_ */
