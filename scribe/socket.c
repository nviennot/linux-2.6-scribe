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
#include <linux/module.h>
#include <linux/socket.h>
#include <linux/net.h>
#include <net/sock.h>
#include <linux/syscalls.h>
#include <net/af_unix.h>

static bool scribe_is_deterministic(struct socket *sock);

/*
 * We cannot use scribe_need_syscall_ret() because one syscall may call
 * many of these functions.
 */

static int scribe_release(struct socket *sock)
{
	BUG_ON(!sock);
	BUG_ON(!sock->real_ops);
	return sock->real_ops->release(sock);
}

static int scribe_bind(struct socket *sock, struct sockaddr *myaddr,
		       int sockaddr_len)
{
	return sock->real_ops->bind(sock, myaddr, sockaddr_len);
}

static int scribe_connect(struct socket *sock, struct sockaddr *vaddr,
			  int sockaddr_len, int flags)
{
	int ret, err;

	if (sock->real_ops->family == PF_UNIX) {
		/*
		 * Unix socket: doing the real connect to go through the file
		 * system traversal.
		 */
		return sock->real_ops->connect(sock, vaddr,
					       sockaddr_len, flags);
	}

	err = scribe_result(
		ret, sock->real_ops->connect(sock, vaddr, sockaddr_len, flags));
	if (err == -EDIVERGE)
		return sock->real_ops->connect(sock, vaddr,
					       sockaddr_len, flags);
	return err ?: ret;
}

static int scribe_socketpair(struct socket *sock1, struct socket *sock2)
{
	return sock1->real_ops->socketpair(sock1, sock2);
}

static int scribe_accept(struct socket *sock, struct socket *newsock, int flags)
{
	/* Handled at the syscall level */
	return sock->real_ops->accept(sock, newsock, flags);
}

static int scribe_getname(struct socket *sock, struct sockaddr *addr,
			  int *sockaddr_len, int peer)
{
	struct scribe_ps *scribe = current->scribe;
	int ret, err;

	if (!is_scribed(scribe))
		return sock->real_ops->getname(sock, addr, sockaddr_len, peer);

	err = scribe_result(
		ret, sock->real_ops->getname(sock, addr, sockaddr_len, peer));
	if (err)
		goto out;
	if (ret < 0)
		goto out;

	err = scribe_value(sockaddr_len);
	if (err)
		goto out;

	err = scribe_buffer(addr, *sockaddr_len);

out:
	if (err) {
		if (err == -EDIVERGE)
			return sock->real_ops->getname(sock, addr,
						       sockaddr_len, peer);
		scribe_emergency_stop(scribe->ctx, ERR_PTR(err));
		return err;
	}
	return ret;
}

static unsigned int scribe_poll(struct file *file, struct socket *sock,
				struct poll_table_struct *wait)
{
	return sock->real_ops->poll(file, sock, wait);
}

static int scribe_ioctl(struct socket *sock, unsigned int cmd,
			unsigned long arg)
{
	int ret;
	scribe_data_non_det();
	ret = sock->real_ops->ioctl(sock, cmd, arg);
	scribe_data_pop_flags();
	return ret;
}

#ifdef CONFIG_COMPAT
static int scribe_compat_ioctl(struct socket *sock, unsigned int cmd,
			       unsigned long arg)
{
	if (!sock->real_ops->compat_ioctl)
		return -ENOIOCTLCMD;
	return sock->real_ops->compat_ioctl(sock, cmd, arg);
}
#endif

static int scribe_listen(struct socket *sock, int len)
{
	return sock->real_ops->listen(sock, len);
}

static int scribe_shutdown(struct socket *sock, int flags)
{
	return sock->real_ops->shutdown(sock, flags);
}

static int scribe_setsockopt(struct socket *sock, int level,
			     int optname, char __user *optval,
			     unsigned int optlen)
{
	struct scribe_ps *scribe = current->scribe;
	int ret, err;

	scribe_data_need_info();

	err = scribe_result(
		ret, sock->real_ops->setsockopt(sock, level, optname,
						optval, optlen));
	if (err) {
		if (err == -EDIVERGE)
			return sock->real_ops->setsockopt(sock, level, optname,
							  optval, optlen);
		return err;
	}
	if (ret < 0)
		return ret;

	if (is_replaying(scribe))
		scribe_emul_copy_from_user(scribe, NULL, optlen);
	return ret;
}

static int scribe_getsockopt(struct socket *sock, int level,
			     int optname, char __user *optval,
			     int __user *optlen)
{
	struct scribe_ps *scribe = current->scribe;
	int ret, err;

	scribe_data_non_det_need_info();

	err = scribe_result(
		ret, sock->real_ops->getsockopt(sock, level, optname,
						optval, optlen));
	if (err) {
		if (err == -EDIVERGE)
			return sock->real_ops->getsockopt(sock, level, optname,
							  optval, optlen);
		return err;
	}
	if (ret < 0)
		return ret;

	if (is_replaying(scribe)) {
		scribe_emul_copy_to_user(scribe, NULL, INT_MAX);
		/*
		 * FIXME We have no idea if the bytes copied is actually
		 * correct (In case we diverge, we should fall back to the
		 * real call.
		 */
	}
	return ret;
}

#ifdef CONFIG_COMPAT
static int scribe_compat_setsockopt(struct socket *sock, int level,
				    int optname, char __user *optval,
				    unsigned int optlen)
{
	return sock->real_ops->setsockopt(sock, level, optname, optval, optlen);
}

static int scribe_compat_getsockopt(struct socket *sock, int level,
				    int optname, char __user *optval,
				    int __user *optlen)
{
	return sock->real_ops->getsockopt(sock, level, optname, optval, optlen);
}
#endif

static int scribe_sendmsg(struct kiocb *iocb, struct socket *sock,
			  struct msghdr *m, size_t total_len)
{
	struct scribe_ps *scribe = current->scribe;
	int ret, err;

	if (scribe_is_deterministic(sock) || !is_scribed(scribe)) {
		if (is_replaying(scribe))
			m->msg_flags &= ~MSG_DONTWAIT;
		ret = sock->real_ops->sendmsg(iocb, sock, m, total_len);
		return ret;
	}

	scribe_data_need_info();

	err = scribe_result_cond(
		ret, sock->real_ops->sendmsg(iocb, sock, m, total_len),
		!scribe_is_in_read_write(scribe) || ret > 0);
	if (err)
		goto out;
	if (ret <= 0)
		goto out;

	if (is_replaying(scribe))
		scribe_emul_copy_from_user(scribe, NULL, INT_MAX);

out:
	scribe_data_pop_flags();
	if (err == -EDIVERGE) {
		/* Diverge on a fake socket */
		err = -ENOTCONN;
	}
	return err ?: ret;
}

static int scribe_recvmsg(struct kiocb *iocb, struct socket *sock,
			  struct msghdr *m, size_t total_len,
			  int flags)
{
	struct scribe_ps *scribe = current->scribe;
	int ret, err;

	if (scribe_is_deterministic(sock) || !is_scribed(scribe)) {
		if (is_replaying(scribe)) {
			flags &= ~MSG_DONTWAIT;
			flags |= MSG_WAITALL;
		}
		scribe_data_det();
		ret = sock->real_ops->recvmsg(iocb, sock, m, total_len, flags),
		scribe_data_pop_flags();
		return ret;
	}

	scribe_data_non_det_need_info();

	err = scribe_result_cond(
		ret, sock->real_ops->recvmsg(iocb, sock, m, total_len, flags),
		!scribe_is_in_read_write(scribe) || ret > 0);
	if (err)
		goto out;
	if (ret <= 0)
		goto out;

	if (is_replaying(scribe)) {
		scribe_emul_copy_to_user(scribe, NULL, INT_MAX);
		/*
		 * FIXME We have no idea if the bytes copied is actually
		 * correct (In case we diverge, we should fall back to the
		 * real call.
		 */
	}

	err = scribe_value(&m->msg_namelen);
	if (err)
		goto out;
	err = scribe_buffer(m->msg_name, m->msg_namelen);

out:
	scribe_data_pop_flags();
	if (err == -EDIVERGE) {
		/* Diverge on a fake socket */
		err = -ENOTCONN;
	}
	return err ?: ret;
}

static int scribe_mmap(struct file *file, struct socket *sock,
		       struct vm_area_struct *vma)
{
	struct scribe_ps *scribe = current->scribe;

	if (!is_scribed(scribe))
		return sock->real_ops->mmap(file, sock , vma);

	return sock_no_mmap(file, sock, vma);
}

static ssize_t scribe_sendpage(struct socket *sock, struct page *page,
			       int offset, size_t size, int flags)
{
	/*
	 * Disabling sendpage: a accept() socket will have a different real_ops,
	 * the unix socket to be specific. So we want to force the same
	 * behavior
	 */
	return sock_no_sendpage(sock, page, offset, size, flags);
}

static ssize_t scribe_splice_read(struct socket *sock,  loff_t *ppos,
				  struct pipe_inode_info *pipe, size_t len,
				  unsigned int flags)
{
	if (unlikely(!sock->real_ops->splice_read))
		return -EINVAL;
	return sock->real_ops->splice_read(sock, ppos, pipe, len, flags);
}

#define unix_peer(sk) (unix_sk(sk)->peer)
static bool is_unix_sock_deterministic(struct scribe_ps *scribe,
				       struct sock *sk)
{
	if (sk->sk_scribe_ctx != scribe->ctx)
		return false;

	if (!unix_peer(sk))
		return false;

	if (unix_peer(sk)->sk_scribe_ctx != scribe->ctx)
		return false;

	return true;
}

static bool scribe_is_deterministic(struct socket *sock)
{
	struct scribe_ps *scribe = current->scribe;
	struct sock *sk = sock->sk;

	if (!is_scribed(scribe))
		return false;

	if (sock->real_ops->family == PF_UNIX)
		return is_unix_sock_deterministic(scribe, sk);
	return false;
}

const struct proto_ops scribe_ops = {
	.family            = PF_UNSPEC,
	.owner             = THIS_MODULE,
	.release           = scribe_release,
	.bind              = scribe_bind,
	.connect           = scribe_connect,
	.socketpair        = scribe_socketpair,
	.accept            = scribe_accept,
	.getname           = scribe_getname,
	.poll              = scribe_poll,
	.ioctl             = scribe_ioctl,
#ifdef CONFIG_COMPAT
	.compat_ioctl      = scribe_compat_ioctl,
#endif
	.listen            = scribe_listen,
	.shutdown          = scribe_shutdown,
	.getsockopt        = scribe_getsockopt,
	.setsockopt        = scribe_setsockopt,
#ifdef CONFIG_COMPAT
	.compat_setsockopt = scribe_compat_setsockopt,
	.compat_getsockopt = scribe_compat_getsockopt,
#endif
	.sendmsg           = scribe_sendmsg,
	.recvmsg           = scribe_recvmsg,
	.mmap              = scribe_mmap,
	.sendpage          = scribe_sendpage,
	.splice_read       = scribe_splice_read,
	.is_deterministic  = scribe_is_deterministic,
};

/*
 * XXX sys_accept() doesn't call this function
 */
int scribe_interpose_socket(struct socket *sock)
{
	struct scribe_ps *scribe = current->scribe;

	if (!is_scribed(scribe))
		return 0;

	/* TODO We should revert the ops to real_ops when the context dies */
	sock->real_ops = sock->ops;
	sock->ops = &scribe_ops;

	/* The value of sock->sk->sk_scribe_ctx is already set in sk_alloc */

	return 0;
}
