/*
 *  linux/fs/read_write.c
 *
 *  Copyright (C) 1991, 1992  Linus Torvalds
 */

#include <linux/slab.h> 
#include <linux/stat.h>
#include <linux/fcntl.h>
#include <linux/file.h>
#include <linux/uio.h>
#include <linux/smp_lock.h>
#include <linux/fsnotify.h>
#include <linux/security.h>
#include <linux/module.h>
#include <linux/syscalls.h>
#include <linux/pagemap.h>
#include <linux/splice.h>
#include <linux/scribe.h>
#include <linux/magic.h>
#include "read_write.h"

#include <asm/uaccess.h>
#include <asm/unistd.h>

const struct file_operations generic_ro_fops = {
	.llseek		= generic_file_llseek,
	.read		= do_sync_read,
	.aio_read	= generic_file_aio_read,
	.mmap		= generic_file_readonly_mmap,
	.splice_read	= generic_file_splice_read,
};

EXPORT_SYMBOL(generic_ro_fops);

/**
 * generic_file_llseek_unlocked - lockless generic llseek implementation
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @origin:	type of seek
 *
 * Updates the file offset to the value specified by @offset and @origin.
 * Locking must be provided by the caller.
 */
loff_t
generic_file_llseek_unlocked(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_mapping->host;

	switch (origin) {
	case SEEK_END:
		offset += inode->i_size;
		break;
	case SEEK_CUR:
		/*
		 * Here we special-case the lseek(fd, 0, SEEK_CUR)
		 * position-querying operation.  Avoid rewriting the "same"
		 * f_pos value back to the file because a concurrent read(),
		 * write() or lseek() might have altered it
		 */
		if (offset == 0)
			return file->f_pos;
		offset += file->f_pos;
		break;
	}

	if (offset < 0 || offset > inode->i_sb->s_maxbytes)
		return -EINVAL;

	/* Special lock needed here? */
	if (offset != file->f_pos) {
		file->f_pos = offset;
		file->f_version = 0;
	}

	return offset;
}
EXPORT_SYMBOL(generic_file_llseek_unlocked);

/**
 * generic_file_llseek - generic llseek implementation for regular files
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @origin:	type of seek
 *
 * This is a generic implemenation of ->llseek useable for all normal local
 * filesystems.  It just updates the file offset to the value specified by
 * @offset and @origin under i_mutex.
 */
loff_t generic_file_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t rval;

	mutex_lock(&file->f_dentry->d_inode->i_mutex);
	rval = generic_file_llseek_unlocked(file, offset, origin);
	mutex_unlock(&file->f_dentry->d_inode->i_mutex);

	return rval;
}
EXPORT_SYMBOL(generic_file_llseek);

/**
 * noop_llseek - No Operation Performed llseek implementation
 * @file:	file structure to seek on
 * @offset:	file offset to seek to
 * @origin:	type of seek
 *
 * This is an implementation of ->llseek useable for the rare special case when
 * userspace expects the seek to succeed but the (device) file is actually not
 * able to perform the seek. In this case you use noop_llseek() instead of
 * falling back to the default implementation of ->llseek.
 */
loff_t noop_llseek(struct file *file, loff_t offset, int origin)
{
	return file->f_pos;
}
EXPORT_SYMBOL(noop_llseek);

loff_t no_llseek(struct file *file, loff_t offset, int origin)
{
	return -ESPIPE;
}
EXPORT_SYMBOL(no_llseek);

loff_t default_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t retval;

	lock_kernel();
	switch (origin) {
		case SEEK_END:
			offset += i_size_read(file->f_path.dentry->d_inode);
			break;
		case SEEK_CUR:
			if (offset == 0) {
				retval = file->f_pos;
				goto out;
			}
			offset += file->f_pos;
	}
	retval = -EINVAL;
	if (offset >= 0) {
		if (offset != file->f_pos) {
			file->f_pos = offset;
			file->f_version = 0;
		}
		retval = offset;
	}
out:
	unlock_kernel();
	return retval;
}
EXPORT_SYMBOL(default_llseek);

loff_t vfs_llseek(struct file *file, loff_t offset, int origin)
{
	loff_t (*fn)(struct file *, loff_t, int);

	fn = no_llseek;
	if (file->f_mode & FMODE_LSEEK) {
		fn = default_llseek;
		if (file->f_op && file->f_op->llseek)
			fn = file->f_op->llseek;
	}
	return fn(file, offset, origin);
}
EXPORT_SYMBOL(vfs_llseek);

SYSCALL_DEFINE3(lseek, unsigned int, fd, off_t, offset, unsigned int, origin)
{
	off_t retval;
	struct file * file;
	int fput_needed;

	if (scribe_track_next_file_no_inode())
		return -ENOMEM;

	retval = -EBADF;
	file = fget_light(fd, &fput_needed);
	if (!file)
		goto bad;

	retval = -EINVAL;
	if (origin <= SEEK_MAX) {
		loff_t res = vfs_llseek(file, offset, origin);
		retval = res;
		if (res != (loff_t)retval)
			retval = -EOVERFLOW;	/* LFS: should only happen on 32 bit platforms */
	}
	fput_light(file, fput_needed);
bad:
	return retval;
}

#ifdef __ARCH_WANT_SYS_LLSEEK
SYSCALL_DEFINE5(llseek, unsigned int, fd, unsigned long, offset_high,
		unsigned long, offset_low, loff_t __user *, result,
		unsigned int, origin)
{
	int retval;
	struct file * file;
	loff_t offset;
	int fput_needed;

	if (scribe_track_next_file_no_inode())
		return -ENOMEM;

	retval = -EBADF;
	file = fget_light(fd, &fput_needed);
	if (!file)
		goto bad;

	retval = -EINVAL;
	if (origin > SEEK_MAX)
		goto out_putf;

	offset = vfs_llseek(file, ((loff_t) offset_high << 32) | offset_low,
			origin);

	retval = (int)offset;
	if (offset >= 0) {
		retval = -EFAULT;
		if (!copy_to_user(result, &offset, sizeof(offset)))
			retval = 0;
	}
out_putf:
	fput_light(file, fput_needed);
bad:
	return retval;
}
#endif

/*
 * rw_verify_area doesn't like huge counts. We limit
 * them to something that fits in "int" so that others
 * won't have to do range checks all the time.
 */
#define MAX_RW_COUNT (INT_MAX & PAGE_CACHE_MASK)

int rw_verify_area(int read_write, struct file *file, loff_t *ppos, size_t count)
{
	struct inode *inode;
	loff_t pos;
	int retval = -EINVAL;

	inode = file->f_path.dentry->d_inode;
	if (unlikely((ssize_t) count < 0))
		return retval;
	pos = *ppos;
	if (unlikely((pos < 0) || (loff_t) (pos + count) < 0))
		return retval;

	if (unlikely(inode->i_flock && mandatory_lock(inode))) {
		retval = locks_mandatory_area(
			read_write == READ ? FLOCK_VERIFY_READ : FLOCK_VERIFY_WRITE,
			inode, file, pos, count);
		if (retval < 0)
			return retval;
	}
	retval = security_file_permission(file,
				read_write == READ ? MAY_READ : MAY_WRITE);
	if (retval)
		return retval;
	return count > MAX_RW_COUNT ? MAX_RW_COUNT : count;
}

static void wait_on_retry_sync_kiocb(struct kiocb *iocb)
{
	set_current_state(TASK_UNINTERRUPTIBLE);
	if (!kiocbIsKicked(iocb))
		schedule();
	else
		kiocbClearKicked(iocb);
	__set_current_state(TASK_RUNNING);
}

ssize_t do_sync_read(struct file *filp, char __user *buf, size_t len, loff_t *ppos)
{
	struct iovec iov = { .iov_base = buf, .iov_len = len };
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;
	kiocb.ki_left = len;
	kiocb.ki_nbytes = len;

	for (;;) {
		ret = filp->f_op->aio_read(&kiocb, &iov, 1, kiocb.ki_pos);
		if (ret != -EIOCBRETRY)
			break;
		wait_on_retry_sync_kiocb(&kiocb);
	}

	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

EXPORT_SYMBOL(do_sync_read);

static ssize_t __do_read(struct file *file, char __user *buf,
			 size_t len, loff_t *ppos)
{
	if (file->f_op->read)
		return file->f_op->read(file, buf, len, ppos);
	else
		return do_sync_read(file, buf, len, ppos);
}

static ssize_t do_read(struct file *file, char __user *buf,
		       size_t len, loff_t *ppos, int force_block)
{
	unsigned int saved_flags;
	ssize_t ret, count;

	if (!force_block)
		return __do_read(file, buf, len, ppos);

	saved_flags = file->f_flags;
	file->f_flags &= ~O_NONBLOCK;

	ret = 0;
	while (len > 0) {
		count = __do_read(file, buf, len, ppos);
		if (count == 0)
			break;
		if (count < 0) {
			ret = ret ?: count;
			break;
		}
		len -= count;
		buf += count;
		ret += count;
	}

	file->f_flags = saved_flags;

	return ret;
}

#ifdef CONFIG_SCRIBE
static int is_deterministic(struct file *file)
{
	struct inode *inode;
	umode_t inode_mode;
	int s_magic;

	inode = file->f_dentry->d_inode;
	inode_mode = inode->i_mode;

	if (S_ISCHR(inode_mode))
		return 0;

	/*
	 * We make the socket deterministic at this level since the
	 * non-determinism is all handled in the scribe socket ops
	 */

	s_magic = file->f_dentry->d_sb->s_magic;
	if (s_magic == PROC_SUPER_MAGIC)
		return 0;

	return 1;
}

static ssize_t scribe_do_read(struct file *file, char __user *buf,
			      ssize_t len, loff_t *ppos)
{
	struct scribe_ps *scribe = current->scribe;
	int force_block = 0;
	ssize_t ret;
	bool allowed_uaccess = false;


	if (!is_scribed(scribe))
		return do_read(file, buf, len, ppos, force_block);

	if (is_kernel_copy())
		goto do_real;

	scribe_allow_uaccess();
	allowed_uaccess = true;

	if (!should_scribe_data(scribe))
		goto do_real;


	scribe_need_syscall_ret(scribe);

	if (is_replaying(scribe)) {
		force_block = 1;
		if (scribe->orig_ret <= 0 && is_deterministic(file))
			goto do_real;

		if (scribe->orig_ret < 0)
			len = scribe->orig_ret;
		else if (should_have_fixed_io(scribe))
			len = min(len, (ssize_t)scribe->orig_ret);
		if (len <= 0) {
			ret = len;
			goto out;
		}
	}

	if (!is_deterministic(file)) {
		scribe_data_non_det();

		if (is_recording(scribe))
			goto do_real;

		ret = scribe_emul_copy_to_user(scribe, buf, len);
		goto out;
	}

do_real:
	scribe->in_read_write = true;
	ret = do_read(file, buf, len, ppos, force_block);
	scribe->in_read_write = false;
out:
	if (allowed_uaccess)
		scribe_forbid_uaccess();
	return ret;
}
#else
static inline ssize_t scribe_do_read(struct file *file, char __user *buf,
				     size_t len, loff_t *ppos)
{
	return do_read(file, buf, len, ppos, 0);
}
#endif /* CONFIG_SCRIBE */

ssize_t vfs_read(struct file *file, char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!file->f_op || (!file->f_op->read && !file->f_op->aio_read))
		return -EINVAL;
	if (unlikely(!access_ok(VERIFY_WRITE, buf, count)))
		return -EFAULT;

	ret = rw_verify_area(READ, file, pos, count);
	if (ret >= 0) {
		count = ret;
		ret = scribe_do_read(file, buf, count, pos);
		if (ret > 0) {
			fsnotify_access(file->f_path.dentry);
			add_rchar(current, ret);
		}
		inc_syscr(current);
	}

	return ret;
}

EXPORT_SYMBOL(vfs_read);

ssize_t do_sync_write(struct file *filp, const char __user *buf, size_t len, loff_t *ppos)
{
	struct iovec iov = { .iov_base = (void __user *)buf, .iov_len = len };
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;
	kiocb.ki_left = len;
	kiocb.ki_nbytes = len;

	for (;;) {
		ret = filp->f_op->aio_write(&kiocb, &iov, 1, kiocb.ki_pos);
		if (ret != -EIOCBRETRY)
			break;
		wait_on_retry_sync_kiocb(&kiocb);
	}

	if (-EIOCBQUEUED == ret)
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

EXPORT_SYMBOL(do_sync_write);

static ssize_t __do_write(struct file *file, const char __user *buf,
			  size_t len, loff_t *ppos)
{
	if (file->f_op->write)
		return file->f_op->write(file, buf, len, ppos);
	else
		return do_sync_write(file, buf, len, ppos);
}

static ssize_t do_write(struct file *file, const char __user *buf,
			size_t len, loff_t *ppos, int force_block)
{
	unsigned int saved_flags;
	ssize_t ret, count;

	if (!force_block)
		return __do_write(file, buf, len, ppos);

	/* Pretty much a copy of do_read() */
	saved_flags = file->f_flags;
	file->f_flags &= ~O_NONBLOCK;

	ret = 0;
	while (len > 0) {
		count = __do_write(file, buf, len, ppos);
		if (count == 0)
			break;
		if (count < 0) {
			ret = ret ?: count;
			break;
		}
		len -= count;
		buf += count;
		ret += count;
	}

	file->f_flags = saved_flags;

	return ret;
}

#ifdef CONFIG_SCRIBE
static ssize_t scribe_do_write(struct file *file, const char __user *buf,
			       ssize_t count, loff_t *ppos)
{
	struct scribe_ps *scribe = current->scribe;
	int force_block = 0;
	int ret;

	if (!is_scribed(scribe))
		return do_write(file, buf, count, ppos, force_block);

	if (is_kernel_copy())
		goto do_real;

	if (!should_scribe_data(scribe))
		goto do_real;

	scribe_need_syscall_ret(scribe);

	if (is_replaying(scribe)) {
		force_block = 1;
		if (scribe->orig_ret <= 0 && is_deterministic(file))
			goto do_real;

		if (scribe->orig_ret < 0)
			count = scribe->orig_ret;
		else if (should_have_fixed_io(scribe))
			count = min(count, (ssize_t)scribe->orig_ret);

		if (count <= 0)
			return count;
	}

do_real:
	scribe->in_read_write = true;
	ret = do_write(file, buf, count, ppos, force_block);
	scribe->in_read_write = false;

	return ret;
}
#else
static inline ssize_t scribe_do_write(struct file *file, const char __user *buf,
				      ssize_t count, loff_t *ppos)
{
	return do_write(file, buf, count, ppos, 0);
}
#endif /* CONFIG_SCRIBE */

ssize_t vfs_write(struct file *file, const char __user *buf, size_t count, loff_t *pos)
{
	ssize_t ret;

	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!file->f_op || (!file->f_op->write && !file->f_op->aio_write))
		return -EINVAL;
	if (unlikely(!access_ok(VERIFY_READ, buf, count)))
		return -EFAULT;

	ret = rw_verify_area(WRITE, file, pos, count);
	if (ret >= 0) {
		count = ret;
		ret = scribe_do_write(file, buf, count, pos);
		if (ret > 0) {
			fsnotify_modify(file->f_path.dentry);
			add_wchar(current, ret);
		}
		inc_syscw(current);
	}

	return ret;
}

EXPORT_SYMBOL(vfs_write);

static inline loff_t file_pos_read(struct file *file)
{
	return file->f_pos;
}

static inline void file_pos_write(struct file *file, loff_t pos)
{
	file->f_pos = pos;
}

SYSCALL_DEFINE3(read, unsigned int, fd, char __user *, buf, size_t, count)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (scribe_track_next_file_read_interruptible())
		return -ENOMEM;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_read(file, buf, count, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	} else if (scribe_was_file_locking_interrupted())
		ret = -ERESTARTSYS;

	return ret;
}

SYSCALL_DEFINE3(write, unsigned int, fd, const char __user *, buf,
		size_t, count)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (scribe_track_next_file_write_interruptible())
		return -ENOMEM;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_write(file, buf, count, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	} else if (scribe_was_file_locking_interrupted())
		ret = -ERESTARTSYS;

	return ret;
}

SYSCALL_DEFINE(pread64)(unsigned int fd, char __user *buf,
			size_t count, loff_t pos)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	if (scribe_track_next_file_read_interruptible())
		return -ENOMEM;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PREAD)
			ret = vfs_read(file, buf, count, &pos);
		fput_light(file, fput_needed);
	} else if (scribe_was_file_locking_interrupted())
		ret = -ERESTARTSYS;

	return ret;
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_pread64(long fd, long buf, long count, loff_t pos)
{
	return SYSC_pread64((unsigned int) fd, (char __user *) buf,
			    (size_t) count, pos);
}
SYSCALL_ALIAS(sys_pread64, SyS_pread64);
#endif

SYSCALL_DEFINE(pwrite64)(unsigned int fd, const char __user *buf,
			 size_t count, loff_t pos)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	if (scribe_track_next_file_write_interruptible())
		return -ENOMEM;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PWRITE)  
			ret = vfs_write(file, buf, count, &pos);
		fput_light(file, fput_needed);
	} else if (scribe_was_file_locking_interrupted())
		ret = -ERESTARTSYS;

	return ret;
}
#ifdef CONFIG_HAVE_SYSCALL_WRAPPERS
asmlinkage long SyS_pwrite64(long fd, long buf, long count, loff_t pos)
{
	return SYSC_pwrite64((unsigned int) fd, (const char __user *) buf,
			     (size_t) count, pos);
}
SYSCALL_ALIAS(sys_pwrite64, SyS_pwrite64);
#endif

/*
 * Reduce an iovec's length in-place.  Return the resulting number of segments
 */
unsigned long iov_shorten(struct iovec *iov, unsigned long nr_segs, size_t to)
{
	unsigned long seg = 0;
	size_t len = 0;

	while (seg < nr_segs) {
		seg++;
		if (len + iov->iov_len >= to) {
			iov->iov_len = to - len;
			break;
		}
		len += iov->iov_len;
		iov++;
	}
	return seg;
}
EXPORT_SYMBOL(iov_shorten);

ssize_t do_sync_readv_writev(struct file *filp, const struct iovec *iov,
		unsigned long nr_segs, size_t len, loff_t *ppos, iov_fn_t fn)
{
	struct kiocb kiocb;
	ssize_t ret;

	init_sync_kiocb(&kiocb, filp);
	kiocb.ki_pos = *ppos;
	kiocb.ki_left = len;
	kiocb.ki_nbytes = len;

	for (;;) {
		ret = fn(&kiocb, iov, nr_segs, kiocb.ki_pos);
		if (ret != -EIOCBRETRY)
			break;
		wait_on_retry_sync_kiocb(&kiocb);
	}

	if (ret == -EIOCBQUEUED)
		ret = wait_on_sync_kiocb(&kiocb);
	*ppos = kiocb.ki_pos;
	return ret;
}

/* Do it by hand, with file-ops */
static ssize_t __do_loop_readv_writev(struct file *filp, struct iovec *iov,
				      unsigned long nr_segs, size_t tot_len,
				      loff_t *ppos, io_fn_t fn)
{
	struct iovec *vector = iov;
	ssize_t ret = 0;

	while (nr_segs > 0) {
		void __user *base;
		size_t len;
		ssize_t nr;

		base = vector->iov_base;
		len = vector->iov_len;
		vector++;
		nr_segs--;

		if (tot_len && tot_len < len)
			len = tot_len;

		nr = fn(filp, base, len, ppos);

		if (nr < 0) {
			if (!ret)
				ret = nr;
			break;
		}
		ret += nr;
		if (nr != len)
			break;

		if (tot_len) {
			tot_len -= nr;
			if (tot_len <= 0)
				break;
		}
	}

	return ret;
}

ssize_t do_loop_readv_writev(struct file *filp, struct iovec *iov,
			     unsigned long nr_segs, loff_t *ppos, io_fn_t fn)
{
	return __do_loop_readv_writev(filp, iov, nr_segs, 0, ppos, fn);
}

/* A write operation does a read from user space and vice versa */
#define vrfy_dir(type) ((type) == READ ? VERIFY_WRITE : VERIFY_READ)

ssize_t rw_copy_check_uvector(int type, const struct iovec __user * uvector,
			      unsigned long nr_segs, unsigned long fast_segs,
			      struct iovec *fast_pointer,
			      struct iovec **ret_pointer)
  {
	unsigned long seg;
  	ssize_t ret;
	struct iovec *iov = fast_pointer;

  	/*
  	 * SuS says "The readv() function *may* fail if the iovcnt argument
  	 * was less than or equal to 0, or greater than {IOV_MAX}.  Linux has
  	 * traditionally returned zero for zero segments, so...
  	 */
	if (nr_segs == 0) {
		ret = 0;
  		goto out;
	}

  	/*
  	 * First get the "struct iovec" from user memory and
  	 * verify all the pointers
  	 */
	if (nr_segs > UIO_MAXIOV) {
		ret = -EINVAL;
  		goto out;
	}
	if (nr_segs > fast_segs) {
  		iov = kmalloc(nr_segs*sizeof(struct iovec), GFP_KERNEL);
		if (iov == NULL) {
			ret = -ENOMEM;
  			goto out;
		}
  	}
	if (copy_from_user(iov, uvector, nr_segs*sizeof(*uvector))) {
		ret = -EFAULT;
  		goto out;
	}

  	/*
	 * According to the Single Unix Specification we should return EINVAL
	 * if an element length is < 0 when cast to ssize_t or if the
	 * total length would overflow the ssize_t return value of the
	 * system call.
  	 */
	ret = 0;
  	for (seg = 0; seg < nr_segs; seg++) {
  		void __user *buf = iov[seg].iov_base;
  		ssize_t len = (ssize_t)iov[seg].iov_len;

		/* see if we we're about to use an invalid len or if
		 * it's about to overflow ssize_t */
		if (len < 0 || (ret + len < ret)) {
			ret = -EINVAL;
  			goto out;
		}
		if (unlikely(!access_ok(vrfy_dir(type), buf, len))) {
			ret = -EFAULT;
  			goto out;
		}

		ret += len;
  	}
out:
	*ret_pointer = iov;
	return ret;
}

static ssize_t __do_readv_writev(int type, struct file *file,
				 struct iovec *iov,
				 unsigned long nr_segs, size_t tot_len,
				 loff_t *pos)
{
	ssize_t ret;
	io_fn_t fn;
	iov_fn_t fnv;

	if (!file->f_op) {
		ret = -EINVAL;
		goto out;
	}

	ret = rw_verify_area(type, file, pos, tot_len);
	if (ret < 0)
		goto out;

	fnv = NULL;
	if (type == READ) {
		fn = file->f_op->read;
		fnv = file->f_op->aio_read;
	} else {
		fn = (io_fn_t)file->f_op->write;
		fnv = file->f_op->aio_write;
	}

	if (fnv)
		ret = do_sync_readv_writev(file, iov, nr_segs, tot_len,
						pos, fnv);
	else
		ret = __do_loop_readv_writev(file, iov, nr_segs, tot_len,
					     pos, fn);

out:
	if ((ret + (type == READ)) > 0) {
		if (type == READ)
			fsnotify_access(file->f_path.dentry);
		else
			fsnotify_modify(file->f_path.dentry);
	}
	return ret;
}

static ssize_t do_readv_writev(struct scribe_ps *scribe, int type, struct file *file,
			       struct iovec *iov, unsigned long nr_segs, size_t tot_len,
			       loff_t *pos, int force_block)
{
	unsigned int saved_flags;
	ssize_t ret;

	if (scribe)
		scribe->in_read_write = true;

	if (force_block) {
		saved_flags = file->f_flags;
		file->f_flags &= ~O_NONBLOCK;
	}

	ret = __do_readv_writev(type, file, iov, nr_segs, tot_len, pos);

	if (force_block)
		file->f_flags = saved_flags;

	if (scribe)
		scribe->in_read_write = false;

	return ret;
}

static ssize_t io_scribe_emul_copy_to_user(struct file *filp, char __user *buf,
					   size_t len, loff_t *ppos)
{
	return scribe_emul_copy_to_user(current->scribe, buf, len);
}

static ssize_t scribe_do_readv_writev(int type, struct file *file,
				      const struct iovec __user * uvector,
				      unsigned long nr_segs, loff_t *pos)
{
	struct scribe_ps *scribe = current->scribe;
	struct iovec iovstack[UIO_FASTIOV];
	struct iovec *iov = iovstack;
	int force_block = 0;
	ssize_t ret, tot_len = 0;

	ret = rw_copy_check_uvector(type, uvector, nr_segs,
				    ARRAY_SIZE(iovstack), iovstack,
				    &iov);
	if (ret <= 0)
		goto out;
	tot_len = ret;

	if (!is_scribed(scribe)) {
		scribe = NULL;
		goto do_real;
	}

	if (is_kernel_copy())
		goto do_real;

	if (!should_scribe_data(scribe))
		goto do_real;

	scribe_need_syscall_ret(scribe);

	if (is_replaying(scribe)) {
		force_block = 1;
		if (scribe->orig_ret <= 0 && is_deterministic(file))
			goto do_real;

		if (scribe->orig_ret < 0)
			tot_len = scribe->orig_ret;
		else if (should_have_fixed_io(scribe))
			tot_len = min(tot_len, (ssize_t)scribe->orig_ret);

		if (tot_len <= 0) {
			ret = tot_len;
			goto out;
		}
	}

	if (type == READ && !is_deterministic(file)) {
		scribe_data_non_det();

		if (is_recording(scribe))
			goto do_real;

		ret =  __do_loop_readv_writev(file, iov, nr_segs, tot_len, pos,
					      io_scribe_emul_copy_to_user);
		goto out;
	}

do_real:
	ret = do_readv_writev(scribe, type, file, iov, nr_segs, tot_len, pos,
			      force_block);

out:
	if (iov != iovstack)
		kfree(iov);
	return ret;
}

ssize_t vfs_readv(struct file *file, const struct iovec __user *vec,
		  unsigned long vlen, loff_t *pos)
{
	if (!(file->f_mode & FMODE_READ))
		return -EBADF;
	if (!file->f_op || (!file->f_op->aio_read && !file->f_op->read))
		return -EINVAL;

	return scribe_do_readv_writev(READ, file, vec, vlen, pos);
}

EXPORT_SYMBOL(vfs_readv);

ssize_t vfs_writev(struct file *file, const struct iovec __user *vec,
		   unsigned long vlen, loff_t *pos)
{
	if (!(file->f_mode & FMODE_WRITE))
		return -EBADF;
	if (!file->f_op || (!file->f_op->aio_write && !file->f_op->write))
		return -EINVAL;

	return scribe_do_readv_writev(WRITE, file, vec, vlen, pos);
}

EXPORT_SYMBOL(vfs_writev);

SYSCALL_DEFINE3(readv, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (scribe_track_next_file_read_interruptible())
		return -ENOMEM;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_readv(file, vec, vlen, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	} else if (scribe_was_file_locking_interrupted())
		ret = -ERESTARTSYS;

	if (ret > 0)
		add_rchar(current, ret);
	inc_syscr(current);
	return ret;
}

SYSCALL_DEFINE3(writev, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen)
{
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (scribe_track_next_file_write_interruptible())
		return -ENOMEM;

	file = fget_light(fd, &fput_needed);
	if (file) {
		loff_t pos = file_pos_read(file);
		ret = vfs_writev(file, vec, vlen, &pos);
		file_pos_write(file, pos);
		fput_light(file, fput_needed);
	} else if (scribe_was_file_locking_interrupted())
		ret = -ERESTARTSYS;

	if (ret > 0)
		add_wchar(current, ret);
	inc_syscw(current);
	return ret;
}

static inline loff_t pos_from_hilo(unsigned long high, unsigned long low)
{
#define HALF_LONG_BITS (BITS_PER_LONG / 2)
	return (((loff_t)high << HALF_LONG_BITS) << HALF_LONG_BITS) | low;
}

SYSCALL_DEFINE5(preadv, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	if (scribe_track_next_file_read_interruptible())
		return -ENOMEM;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PREAD)
			ret = vfs_readv(file, vec, vlen, &pos);
		fput_light(file, fput_needed);
	} else if (scribe_was_file_locking_interrupted())
		ret = -ERESTARTSYS;

	if (ret > 0)
		add_rchar(current, ret);
	inc_syscr(current);
	return ret;
}

SYSCALL_DEFINE5(pwritev, unsigned long, fd, const struct iovec __user *, vec,
		unsigned long, vlen, unsigned long, pos_l, unsigned long, pos_h)
{
	loff_t pos = pos_from_hilo(pos_h, pos_l);
	struct file *file;
	ssize_t ret = -EBADF;
	int fput_needed;

	if (pos < 0)
		return -EINVAL;

	if (scribe_track_next_file_write_interruptible())
		return -ENOMEM;

	file = fget_light(fd, &fput_needed);
	if (file) {
		ret = -ESPIPE;
		if (file->f_mode & FMODE_PWRITE)
			ret = vfs_writev(file, vec, vlen, &pos);
		fput_light(file, fput_needed);
	} else if (scribe_was_file_locking_interrupted())
		ret = -ERESTARTSYS;

	if (ret > 0)
		add_wchar(current, ret);
	inc_syscw(current);
	return ret;
}

static ssize_t do_sendfile(int out_fd, int in_fd, loff_t *ppos,
			   size_t count, loff_t max)
{
	struct file * in_file, * out_file;
	struct inode * in_inode, * out_inode;
	loff_t pos;
	ssize_t retval;
	int fput_needed_in, fput_needed_out, fl;

	/*
	 * FIXME Scribe: Lock the two file descriptor in the right order
	 * (but what would be the right order ?)
	 */

	/*
	 * Get input file, and verify that it is ok..
	 */
	retval = -EBADF;
	in_file = fget_light(in_fd, &fput_needed_in);
	if (!in_file)
		goto out;
	if (!(in_file->f_mode & FMODE_READ))
		goto fput_in;
	retval = -ESPIPE;
	if (!ppos)
		ppos = &in_file->f_pos;
	else
		if (!(in_file->f_mode & FMODE_PREAD))
			goto fput_in;
	retval = rw_verify_area(READ, in_file, ppos, count);
	if (retval < 0)
		goto fput_in;
	count = retval;

	/*
	 * Get output file, and verify that it is ok..
	 */
	retval = -EBADF;
	out_file = fget_light(out_fd, &fput_needed_out);
	if (!out_file)
		goto fput_in;
	if (!(out_file->f_mode & FMODE_WRITE))
		goto fput_out;
	retval = -EINVAL;
	in_inode = in_file->f_path.dentry->d_inode;
	out_inode = out_file->f_path.dentry->d_inode;
	retval = rw_verify_area(WRITE, out_file, &out_file->f_pos, count);
	if (retval < 0)
		goto fput_out;
	count = retval;

	if (!max)
		max = min(in_inode->i_sb->s_maxbytes, out_inode->i_sb->s_maxbytes);

	pos = *ppos;
	if (unlikely(pos + count > max)) {
		retval = -EOVERFLOW;
		if (pos >= max)
			goto fput_out;
		count = max - pos;
	}

	fl = 0;
#if 0
	/*
	 * We need to debate whether we can enable this or not. The
	 * man page documents EAGAIN return for the output at least,
	 * and the application is arguably buggy if it doesn't expect
	 * EAGAIN on a non-blocking file descriptor.
	 */
	if (in_file->f_flags & O_NONBLOCK)
		fl = SPLICE_F_NONBLOCK;
#endif
	retval = do_splice_direct(in_file, ppos, out_file, count, fl);

	if (retval > 0) {
		add_rchar(current, retval);
		add_wchar(current, retval);
	}

	inc_syscr(current);
	inc_syscw(current);
	if (*ppos > max)
		retval = -EOVERFLOW;

fput_out:
	fput_light(out_file, fput_needed_out);
fput_in:
	fput_light(in_file, fput_needed_in);
out:
	return retval;
}

SYSCALL_DEFINE4(sendfile, int, out_fd, int, in_fd, off_t __user *, offset, size_t, count)
{
	loff_t pos;
	off_t off;
	ssize_t ret;

	if (offset) {
		if (unlikely(get_user(off, offset)))
			return -EFAULT;
		pos = off;
		ret = do_sendfile(out_fd, in_fd, &pos, count, MAX_NON_LFS);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}

SYSCALL_DEFINE4(sendfile64, int, out_fd, int, in_fd, loff_t __user *, offset, size_t, count)
{
	loff_t pos;
	ssize_t ret;

	if (offset) {
		if (unlikely(copy_from_user(&pos, offset, sizeof(loff_t))))
			return -EFAULT;
		ret = do_sendfile(out_fd, in_fd, &pos, count, 0);
		if (unlikely(put_user(pos, offset)))
			return -EFAULT;
		return ret;
	}

	return do_sendfile(out_fd, in_fd, NULL, count, 0);
}
