/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/major.h>
#include <linux/device.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include <linux/sched.h>
#include <linux/scribe.h>

/*
 * The scribe device instantiate the scribe context associated with it, and
 * the event pump which takes care of the serialization/deserialization of the
 * events.
 */

struct scribe_dev {
	struct scribe_context *ctx;
	struct mutex lock_read;
	struct mutex lock_write;
	struct scribe_pump *pump;
	struct scribe_event *pending_event;
};

static int do_start(struct scribe_dev *dev, int state,
		    unsigned long flags, int log_fd,
		    unsigned int backtrace_len)
{
	struct file *logfile;
	int ret;

	if (backtrace_len < 0)
		return -EINVAL;

	ret = scribe_pump_prepare_start(dev->pump);
	if (ret)
		return ret;

	ret = -EBADF;
	logfile = fget(log_fd);
	if (!logfile)
		goto err_pump;

	ret = scribe_start(dev->ctx, state | flags, backtrace_len);
	if (ret)
		goto err_file;

	scribe_pump_start(dev->pump, state, logfile);
	fput(logfile);
	return 0;

err_file:
	fput(logfile);
err_pump:
	scribe_pump_abort_start(dev->pump);
	return ret;
}

static int handle_command(struct scribe_dev *dev, struct scribe_event *event)
{
	struct scribe_event_record *event_record;
	struct scribe_event_replay *event_replay;

	switch (event->type) {
	case SCRIBE_EVENT_ATTACH_ON_EXECVE:
		return scribe_set_attach_on_exec(dev->ctx,
		      ((struct scribe_event_attach_on_execve *)event)->enable);
	case SCRIBE_EVENT_RECORD:
		event_record = ((struct scribe_event_record *)event);
		return do_start(dev, SCRIBE_RECORD,
				event_record->flags & SCRIBE_FLAGS_MASK,
				event_record->log_fd, 0);
	case SCRIBE_EVENT_REPLAY:
		event_replay = ((struct scribe_event_replay *)event);
		return do_start(dev, SCRIBE_REPLAY,
				event_replay->flags & SCRIBE_FLAGS_MASK,
				event_replay->log_fd,
				event_replay->backtrace_len);
	case SCRIBE_EVENT_STOP:
		return scribe_stop(dev->ctx);
	case SCRIBE_EVENT_BOOKMARK_REQUEST:
		return scribe_bookmark_request(dev->ctx->bmark);
	case SCRIBE_EVENT_RESUME:
		return scribe_bookmark_resume(dev->ctx->bmark);
	case SCRIBE_EVENT_CHECK_DEADLOCK:
		return scribe_check_deadlock(dev->ctx);
	default:
		return -EINVAL;
	}
}

static ssize_t dev_write(struct file *file,
			 const char __user *buf, size_t count, loff_t *ppos)
{
	struct scribe_dev *dev = file->private_data;
	struct scribe_event *event;
	__typeof__(event->type) type;
	size_t to_copy;
	ssize_t ret;

	if (count < sizeof(type))
		return -EINVAL;
	if (get_user(type, buf))
		return -EFAULT;

	if (is_sized_type(type))
		return -EINVAL;

	event = scribe_alloc_event(type);
	if (!event)
		return -ENOMEM;

	to_copy = sizeof_event_payload(event);
	if (count != to_copy) {
		scribe_free_event(event);
		return -EINVAL;
	}

	if (copy_from_user(get_event_payload(event), buf, to_copy)) {
		scribe_free_event(event);
		return -EFAULT;
	}
	event->type = type; /* guard against TOCTTOU */

	mutex_lock(&dev->lock_write);
	ret = handle_command(dev, event);
	mutex_unlock(&dev->lock_write);

	scribe_free_event(event);
	if (ret)
		return ret;
	return to_copy;
}

static ssize_t dev_read(struct file *file,
			char __user *buf, size_t count, loff_t * ppos)
{
	struct scribe_dev *dev = file->private_data;
	struct scribe_context *ctx = dev->ctx;
	struct scribe_event *event;
	ssize_t err;
	size_t to_copy = 0;

	mutex_lock(&dev->lock_read);
	event = dev->pending_event;
	if (!event) {
		event = scribe_dequeue_event_stream(&ctx->notifications,
						    SCRIBE_WAIT_INTERRUPTIBLE);
		if (IS_ERR(event)) {
			err = PTR_ERR(event);
			event = NULL;
			goto out;
		}
	}

	if (event->type == SCRIBE_EVENT_CONTEXT_IDLE) {
		err = -ERESTARTSYS;
		/*
		 * We want to make sure that the pump (and hence the log file)
		 * is complete before returning to userspace. This is the only
		 * way userspace can know if the log file is written entirely.
		 */
		if (scribe_pump_wait_completion_interruptible(dev->pump))
			goto out;
	}

	to_copy = sizeof_event_payload(event);
	err = -EINVAL;
	if (count < to_copy)
		goto out;
	err = -EFAULT;
	if (copy_to_user(buf, get_event_payload(event), to_copy))
		goto out;

	scribe_free_event(event);
	event = NULL;
	err = 0;

out:
	dev->pending_event = event;
	mutex_unlock(&dev->lock_read);
	if (err)
		return err;
	return to_copy;
}

static int dev_open(struct inode *inode, struct file *file)
{
	struct scribe_dev *dev;
	int ret;

	ret = -ENOMEM;
	dev = kmalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev)
		goto out;

	dev->ctx = scribe_alloc_context();
	if (!dev->ctx)
		goto out_dev;

	dev->pump = scribe_pump_alloc(dev->ctx);
	if (!dev->pump)
		goto out_ctx;

	mutex_init(&dev->lock_read);
	mutex_init(&dev->lock_write);

	dev->pending_event = NULL;

	file->private_data = dev;
	return 0;

out_ctx:
	scribe_free_context(dev->ctx);
out_dev:
	kfree(dev);
out:
	return ret;
}

static int dev_release(struct inode *inode, struct file *file)
{
	struct scribe_dev *dev = file->private_data;

	scribe_pump_free(dev->pump);

	if (dev->pending_event)
		scribe_free_event(dev->pending_event);

	scribe_free_context(dev->ctx);
	mutex_destroy(&dev->lock_read);
	mutex_destroy(&dev->lock_write);
	kfree(dev);
	return 0;
}

static const struct file_operations scribe_fops = {
	.read    = dev_read,
	.write   = dev_write,
	.open    = dev_open,
	.release = dev_release,
};

int __init scribe_init_device(void)
{
	struct class *cls;
	struct device *dev;

	if (register_chrdev(SCRIBE_MAJOR, SCRIBE_DEVICE_NAME, &scribe_fops))
		return -EBUSY;

	cls = class_create(THIS_MODULE, SCRIBE_DEVICE_NAME);
	if (IS_ERR(cls)) {
		unregister_chrdev(SCRIBE_MAJOR, SCRIBE_DEVICE_NAME);
		return PTR_ERR(cls);
	}

	dev = device_create(cls, NULL, MKDEV(SCRIBE_MAJOR, 0),
			    NULL, SCRIBE_DEVICE_NAME);
	if (IS_ERR(dev)) {
		unregister_chrdev(SCRIBE_MAJOR, SCRIBE_DEVICE_NAME);
		class_destroy(cls);
		return PTR_ERR(dev);
	}

	return 0;
}
