/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#ifndef _LINUX_SCRIBE_RESOURCE_H_
#define _LINUX_SCRIBE_RESOURCE_H_

#ifdef CONFIG_SCRIBE

#ifdef __KERNEL__

#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/wait.h>
#include <linux/rcupdate.h>
#include <asm/atomic.h>

/*
 * This is not in scribe.h because of the compilation overhead: linux/fs.h
 * depends on this file.
 */

struct scribe_resource {
	/* Attached context, conveniant to know if a resource is tracked */
	struct scribe_context *ctx;

	/* This node correspond to the scribe_resources->tracked list */
	struct list_head node;

	int id;
	int type;
	void *object;

	/* The write part is on a different cache line */

	/*
	 * @first_read_serial is used during the recording to save the first
	 * serial number of read accesses.
	 */
	unsigned long first_read_serial  ____cacheline_aligned_in_smp;

	/*
	 * An atomic type is needed here because the replay doesn't take any
	 * locks.
	 */
	atomic_t serial;

	union {
		struct rw_semaphore semaphore;
		spinlock_t spinlock;
	} lock;

	wait_queue_head_t wait;
	atomic_t priority_users;

	spinlock_t lock_regions_lock;
	struct list_head lock_regions;
};

void scribe_init_resource(struct scribe_resource *res, void *object, int type);
void scribe_reset_resource(struct scribe_resource *res);
struct scribe_res_map;
void scribe_reset_res_map(struct scribe_res_map *map);

#ifdef CONFIG_LOCKDEP
bool is_scribe_resource_key(struct lock_class_key *key);
#endif

/*
 * scribe_res_map is a generic container for resources.
 * It provides a mapping key -> resource, which is used for:
 * - per scribe context resource
 * - pid -> resource
 * - unix abstract address -> resource
 * The elements are defined in scribe/resource/internal.h
 */
struct scribe_res_map {
	spinlock_t lock;
	struct scribe_res_map_ops *ops;

	/*
	 * In some case, we don't want a hash table, but a simple list,
	 * embedded into another struct (cannot do dynamic allocation of the
	 * hash table).
	 * In other cases, we want to specify the size of the hash table.
	 */
	struct hlist_head head[1];
};

struct scribe_res_user;
struct scribe_res_map_ops {
	int type;

	/* alloc_mres is responsible for setting the mr_key value */
	struct scribe_mapped_res * (*alloc_mres) (void *key,
						  struct scribe_res_user *user);
	void (*free_mres) (struct scribe_mapped_res *mres);

	void * (*get_object) (struct scribe_res_map *map, void *key);

	unsigned long hash_bits;
	unsigned long (*hash_fn) (struct scribe_res_map *map, void *key);
	bool (*cmp_keys) (void *key1, void *key2);
};

/*
 * Initializer for a resource mapper without a hash table.
 * In this case, the key is generally be a scribe context.
 */
extern void scribe_init_res_map(struct scribe_res_map *map,
				struct scribe_res_map_ops *ops);
extern void scribe_exit_res_map(struct scribe_res_map *map);

extern struct scribe_mapped_res *scribe_alloc_mres(
				void *key, struct scribe_res_user *user);
extern void scribe_free_mres(struct scribe_mapped_res *mres);


struct scribe_res_user {
	struct hlist_head pre_alloc_mres;
	int num_pre_alloc_mres;

	struct list_head pre_alloc_regions;
	int num_pre_alloc_regions;

	struct sunaddr *pre_alloc_sunaddr;

	struct list_head locked_regions;
};

struct scribe_res_context;
extern struct scribe_res_context *scribe_alloc_res_context(void);
extern void scribe_reset_resources(struct scribe_res_context *res_ctx);
extern void scribe_free_res_context(struct scribe_res_context *res_ctx);

extern void scribe_resource_init_user(struct scribe_res_user *user);
extern void scribe_resource_exit_user(struct scribe_res_user *user);
extern void scribe_assert_no_locked_region(struct scribe_res_user *user);
extern int scribe_resource_prepare(void);

#define SCRIBE_INTERRUPTIBLE	0x0001
#define SCRIBE_READ		0x0002
#define SCRIBE_WRITE		0x0004
#define SCRIBE_INODE_READ	0x0008
#define SCRIBE_INODE_WRITE	0x0010
#define SCRIBE_INODE_EXPLICIT	0x0020
#define SCRIBE_NESTED		0x0040
#define SCRIBE_NO_LOCK		0x0080
#define SCRIBE_HIGH_PRIORITY	0x0100
#define SCRIBE_INTERRUPT_USERS	0x0200
#define SCRIBE_IMPLICIT_UNLOCK	0x0400
#define SCRIBE_CAN_DOWNGRADE	0x0800
extern void scribe_lock_object(struct scribe_resource *res, int flags);
extern void scribe_lock_object_key(struct scribe_res_map *map,
				   void *key, int flags);

extern void scribe_lock_file_no_inode(struct file *file);
extern void scribe_lock_file_read(struct file *file);
extern void scribe_lock_file_write(struct file *file);
extern int scribe_lock_file_read_interruptible(struct file *file);
extern int scribe_lock_file_write_interruptible(struct file *file);

struct inode;
extern void scribe_lock_inode_read(struct inode *inode);
extern void scribe_lock_inode_write(struct inode *inode);
extern void scribe_lock_inode_write_nested(struct inode *inode);

extern int scribe_track_next_file(int flags);
extern int scribe_track_next_file_no_inode(void);
extern int scribe_track_next_file_read(void);
extern int scribe_track_next_file_write(void);
extern int scribe_track_next_file_explicit_inode_read(void);
extern int scribe_track_next_file_explicit_inode_write(void);
extern int scribe_track_next_file_read_interruptible(void);
extern int scribe_track_next_file_write_interruptible(void);
extern bool scribe_was_file_locking_interrupted(void);

struct scribe_fput_context {
	struct scribe_lock_region *lock_region;
	bool file_has_been_destroyed;
};

struct files_struct;
extern void scribe_pre_fget(struct files_struct *files, int *lock_flags);
extern int scribe_post_fget(struct files_struct *files, struct file *file,
			    int lock_flags);
extern void scribe_pre_fput(struct file *file,
			    struct scribe_fput_context *fput_ctx);
extern void scribe_post_fput(struct file *file,
			     struct scribe_fput_context *fput_ctx);

extern void scribe_lock_files_read(struct files_struct *files);
extern void scribe_lock_files_write(struct files_struct *files);

extern void scribe_lock_pid_read(pid_t pid);
extern void scribe_lock_pid_write(pid_t pid);
extern void scribe_unlock_pid(pid_t pid);
extern void scribe_unlock_pid_discard(pid_t pid);

struct ipc_namespace;
extern void scribe_lock_ipc(struct ipc_namespace *ns);

extern void scribe_lock_mmap_read(struct mm_struct *mm);
extern void scribe_lock_mmap_write(struct mm_struct *mm);

extern void scribe_lock_ppid_ptr_read(struct task_struct *p);
extern void scribe_lock_ppid_ptr_write(struct task_struct *p);

struct sockaddr_un;
typedef void *lock_cookie_t;
extern void scribe_lock_sunaddr_read(struct sockaddr_un *sunaddr, int addr_len,
				     lock_cookie_t *cookie);
extern void scribe_lock_sunaddr_write(struct sockaddr_un *sunaddr, int addr_len,
				      lock_cookie_t *cookie);

extern void scribe_unlock(void *object);
extern void scribe_unlock_discard(void *object);
extern void scribe_unlock_err(void *object, int err);
extern void scribe_downgrade(void *object);
extern void scribe_assert_locked(void *object);

#endif /* __KERNEL__ */

/*
 * XXX When adding a new resource type, don't forget to call reset_resource()
 * when the resource object is about to vanish...
 */

enum scribe_resource_type {
	SCRIBE_RES_TYPE_INODE,
	SCRIBE_RES_TYPE_FILE,
	SCRIBE_RES_TYPE_FILES_STRUCT,
	SCRIBE_RES_TYPE_PID,
	SCRIBE_RES_TYPE_FUTEX,
	SCRIBE_RES_TYPE_IPC,
	SCRIBE_RES_TYPE_MMAP,
	SCRIBE_RES_TYPE_PPID,
	SCRIBE_RES_TYPE_SUNADDR,
	SCRIBE_RES_NUM_TYPES
};
#endif /* CONFIG_SCRIBE */

#endif /* _LINUX_SCRIBE_RESOURCE_H_ */
