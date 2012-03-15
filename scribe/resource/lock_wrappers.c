/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include "internal.h"
#include <linux/hash.h>
#include <linux/magic.h>
#include <asm/cmpxchg.h>
#include <linux/fs.h>
#include <linux/fdtable.h>
#include <linux/pid_namespace.h>
#include <linux/ipc_namespace.h>


void scribe_lock_object(struct scribe_resource *res, int flags)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_lock_arg arg;

	if (!should_handle_resources(scribe))
		return;

	__scribe_lock_object(scribe, __lock_arg(&arg, res, flags));
}

void scribe_lock_object_key(struct scribe_res_map *map,
			    void *key, int flags)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_lock_arg arg;

	if (!should_handle_resources(scribe))
		return;

	__scribe_lock_object(scribe, __lock_arg_keyed(scribe, &arg,
					map, key, flags));
}

static inline struct inode *file_inode(struct file *file)
{
	return file->f_path.dentry->d_inode;
}

void scribe_unlock_err(void *object, int err)
{
	struct scribe_ps *scribe = current->scribe;
	if (!should_handle_resources(scribe))
		return;

	__scribe_unlock_object(scribe, object, IS_ERR_VALUE(err));
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

	if (!should_handle_resources(scribe))
		return;

	__scribe_downgrade_object(scribe, object);
}

void scribe_assert_locked(void *object)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return;

	__scribe_assert_locked_object(scribe, object);
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
	if (S_ISFIFO(mode) || S_ISSOCK(mode))
		return true;

	return false;
}

static inline struct scribe_lock_arg *__lock_arg_inode(
						struct scribe_ps *scribe,
						struct scribe_lock_arg *arg,
						struct inode *inode, int flags)
{
	/*
	 * For /proc, we don't need to synchronize the inode because they are
	 * all fake anyways. We save the data read from any files in /proc
	 * (see is_deterministic() in fs/read_write.c).
	 */
	if (inode->i_sb->s_magic == PROC_SUPER_MAGIC)
		flags |= SCRIBE_NO_LOCK;

	return __lock_arg_keyed(scribe, arg, &inode->i_scribe_resource,
				scribe->ctx, flags);
}

static int lock_file(struct file *file, int flags)
{
	struct scribe_ps *scribe = current->scribe;
	struct inode *inode;

	struct scribe_lock_arg args[2];
	struct scribe_lock_arg *file_arg = &args[0];
	struct scribe_lock_arg *inode_arg = &args[1];
	int count = 1;

	if (!should_handle_resources(scribe))
		return 0;

	inode = file_inode(file);
	if (inode_need_explicit_locking(file, inode) &&
	    !(flags & SCRIBE_INODE_EXPLICIT))
		flags &= ~(SCRIBE_INODE_READ | SCRIBE_INODE_WRITE);

	__lock_arg_keyed(scribe, file_arg, &file->scribe_resource,
			 scribe->ctx, flags);

	if (flags & (SCRIBE_INODE_READ | SCRIBE_INODE_WRITE)) {
		flags = flags & SCRIBE_INODE_READ ? SCRIBE_READ : SCRIBE_WRITE;
		__lock_arg_inode(scribe, inode_arg, inode, flags);
		count++;
	}

	return __scribe_lock_objects(scribe, args, count);
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
	struct scribe_lock_arg arg;

	if (!should_handle_resources(scribe))
		return;

	__scribe_lock_object(scribe, __lock_arg_inode(
					scribe, &arg, inode, flags));
}

void scribe_lock_inode_read(struct inode *inode)
{
	lock_inode(inode, SCRIBE_READ);
}

void scribe_lock_inode_write(struct inode *inode)
{
	lock_inode(inode, SCRIBE_WRITE);
}

void scribe_lock_inode_write_nested(struct inode *inode)
{
	lock_inode(inode, SCRIBE_WRITE | SCRIBE_NESTED);
}

int scribe_track_next_file(int flags)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return 0;

	if (scribe_resource_prepare())
		return -ENOMEM;

	scribe->lock_next_file = flags;
	scribe->was_file_locking_interrupted = false;
	return 0;
}

int scribe_track_next_file_no_inode(void)
{
	return scribe_track_next_file(SCRIBE_WRITE);
}

int scribe_track_next_file_read(void)
{
	return scribe_track_next_file(SCRIBE_WRITE | SCRIBE_INODE_READ);
}

int scribe_track_next_file_write(void)
{
	return scribe_track_next_file(SCRIBE_WRITE | SCRIBE_INODE_WRITE);
}

int scribe_track_next_file_explicit_inode_read(void)
{
	return scribe_track_next_file(SCRIBE_WRITE | SCRIBE_INODE_EXPLICIT |
				      SCRIBE_INODE_READ);
}

int scribe_track_next_file_explicit_inode_write(void)
{
	return scribe_track_next_file(SCRIBE_WRITE | SCRIBE_INODE_EXPLICIT |
				      SCRIBE_INODE_WRITE);
}

int scribe_track_next_file_read_interruptible(void)
{
	return scribe_track_next_file(SCRIBE_INTERRUPTIBLE |
				      SCRIBE_WRITE | SCRIBE_INODE_READ);
}

int scribe_track_next_file_write_interruptible(void)
{
	return scribe_track_next_file(SCRIBE_INTERRUPTIBLE |
				      SCRIBE_WRITE | SCRIBE_INODE_WRITE);
}

void scribe_pre_fget(struct files_struct *files, int *lock_flags)
{
	struct scribe_ps *scribe = current->scribe;

	*lock_flags = 0;

	if (!is_scribed(scribe))
		return;

	if (scribe->lock_next_file) {
		*lock_flags = scribe->lock_next_file | SCRIBE_IMPLICIT_UNLOCK;
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

	return 0;
}

void scribe_pre_fput(struct file *file, struct scribe_fput_context *fput_ctx)
{
	bool sync_fput = false;
	struct scribe_ps *scribe = current->scribe;
	struct scribe_lock_region *lock_region;
	struct scribe_res_user *user;

	fput_ctx->lock_region = NULL;

	if (!is_scribed(scribe))
		return;

	user = &scribe->resources;
	lock_region = scribe_find_lock_region(user, file);

	if (file->f_op->scribe_sync_fput)
		sync_fput = file->f_op->scribe_sync_fput(file);

	if (sync_fput) {
		/* unlock will be done in post_fput() */
		if (lock_region) {
			fput_ctx->lock_region = lock_region;
			return;
		}

		if (scribe_resource_prepare()) {
			scribe_kill(scribe->ctx, -ENOMEM);
			return;
		}

		lock_file(file, SCRIBE_WRITE | SCRIBE_CAN_DOWNGRADE |
			        SCRIBE_HIGH_PRIORITY | SCRIBE_INTERRUPT_USERS);

		/* TODO Optimize so that we don't need to search for the lock region */
		lock_region = scribe_find_lock_region(user, file);
		fput_ctx->lock_region = lock_region;
	} else {
		/*
		 * We don't need to sync fput, so we can unlock before fput().
		 */
		if (!lock_region)
			return;

		if (lock_region->flags & SCRIBE_IMPLICIT_UNLOCK)
			__scribe_unlock_region(scribe, lock_region, false);
	}
}

void scribe_post_fput(struct file *file, struct scribe_fput_context *fput_ctx)
{
	struct scribe_lock_region *lock_region = fput_ctx->lock_region;
	struct scribe_ps *scribe;

	if (!lock_region)
		return;

	scribe = current->scribe;

	/*
	 * The fput locking is done in a write mode only when the file pointer
	 * has been destroyed.
	 */
	if (!fput_ctx->file_has_been_destroyed &&
	    (lock_region->flags & SCRIBE_CAN_DOWNGRADE))
		__scribe_downgrade_region(scribe, lock_region);

	__scribe_unlock_region(scribe, lock_region, false);
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
	struct scribe_lock_arg arg;

	if (!should_handle_resources(scribe))
		return;

	__scribe_lock_object(scribe, __lock_arg(
			&arg, &files->scribe_resource, flags));
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
	struct scribe_lock_arg arg;

	if (!should_handle_resources(scribe))
		return;

	__scribe_lock_object(scribe, __lock_arg_keyed(scribe, &arg,
			scribe->ctx->res_ctx->pid_map, (void *)pid, flags));
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
	struct scribe_lock_arg arg;

	if (!should_handle_resources(scribe))
		return;

	/* For now all IPC things are synchronized on the same resource */
	__scribe_lock_object(scribe, __lock_arg(
			&arg, &ns->scribe_resource, SCRIBE_WRITE));
}

static void lock_mmap(struct mm_struct *mm, unsigned long flags)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_lock_arg arg;

	if (!should_handle_resources(scribe))
		return;

	__scribe_lock_object(scribe, __lock_arg(
			&arg, &mm->scribe_mmap_res, flags));
}

void scribe_lock_mmap_read(struct mm_struct *mm)
{
	lock_mmap(mm, SCRIBE_READ);
}

void scribe_lock_mmap_write(struct mm_struct *mm)
{
	lock_mmap(mm, SCRIBE_WRITE);
}

static void lock_ppid_ptr(struct task_struct *p, unsigned long flags)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_lock_arg arg;

	if (!should_handle_resources(scribe))
		return;

	__scribe_lock_object(scribe, __lock_arg(
			&arg, &p->scribe_ppid_ptr_res, flags));
}

void scribe_lock_ppid_ptr_read(struct task_struct *p)
{
	lock_ppid_ptr(p, SCRIBE_READ);
}

void scribe_lock_ppid_ptr_write(struct task_struct *p)
{
	lock_ppid_ptr(p, SCRIBE_WRITE);
}

static void lock_sunaddr(struct sockaddr_un *sunaddr, int addr_len,
			 lock_cookie_t *lock_cookie, unsigned long flags)
{
	struct scribe_ps *scribe = current->scribe;
	struct scribe_lock_arg arg;
	struct sunaddr internal_sunaddr;

	if (!should_handle_resources(scribe))
		return;

	internal_sunaddr.len = addr_len;
	memcpy(&internal_sunaddr.addr, sunaddr, addr_len);

	__scribe_lock_object(scribe, __lock_arg_keyed(scribe, &arg,
			scribe->ctx->res_ctx->sunaddr_map,
			&internal_sunaddr, flags));
	*lock_cookie = arg.res->object;
}

void scribe_lock_sunaddr_read(struct sockaddr_un *sunaddr, int addr_len,
			      lock_cookie_t *lock_cookie)
{
	lock_sunaddr(sunaddr, addr_len, lock_cookie, SCRIBE_READ);
}

void scribe_lock_sunaddr_write(struct sockaddr_un *sunaddr, int addr_len,
			       lock_cookie_t *lock_cookie)
{
	lock_sunaddr(sunaddr, addr_len, lock_cookie, SCRIBE_WRITE);
}
