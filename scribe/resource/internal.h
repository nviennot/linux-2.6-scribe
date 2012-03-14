/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#ifndef _SCRIBE_RESOURCE_INTERNAL_H_
#define _SCRIBE_RESOURCE_INTERNAL_H_

#include <linux/scribe.h>
#include <linux/un.h>

/* Initializer for a resource mapper with a hash table */
extern struct scribe_res_map *scribe_alloc_res_map(
						struct scribe_res_map_ops *ops);
extern void scribe_free_res_map(struct scribe_res_map *map);

/* And its accessors */
extern struct scribe_mapped_res *scribe_get_mapped_res(
		struct scribe_res_map *map, void *key,
		struct scribe_res_user *user);

extern void scribe_remove_mapped_res(struct scribe_mapped_res *mres);

struct scribe_mapped_res {
	union {
		struct {
			struct hlist_node node;
			struct scribe_res_map *map;
		} alive;
		struct {
			void (*free) (struct scribe_mapped_res *);
			struct rcu_head rcu;
		} dying;
	} u;
	void *_key;
	struct scribe_resource _res;
};

#define mr_node		u.alive.node
#define mr_map		u.alive.map
#define mr_free		u.dying.free
#define mr_rcu		u.dying.rcu
#define mr_key		_key
#define mr_res		_res

struct sunaddr {
	int len;
	struct sockaddr_un addr;
};

#define PID_RES_HASH_BITS	10
#define SUNADDR_RES_HASH_BITS	4

/*
 * We need at most 4 lock_regions pre allocated upfront, e.g in fd_install():
 * Two for the open/close region on the inode registration, and one for the
 * files_struct.
 */
#define MAX_PRE_ALLOC 4
extern struct scribe_mapped_res *scribe_get_pre_alloc_mres(
						struct scribe_res_user *user);
extern struct sunaddr *scribe_get_pre_alloc_sunaddr(
						struct scribe_res_user *user);
extern struct scribe_lock_region *scribe_get_pre_alloc_lock_region(
						struct scribe_res_user *user);

/*
 * scribe_res_context is the container per scribe context where all resources
 * are held.
 */
struct scribe_res_context {
	/*
	 * Only resources with a potentially >0 serial will be in that list.
	 * In other words, only used resources are kept in that list.
	 */
	spinlock_t lock;
	int next_id;
	struct list_head tracked;

	struct scribe_res_map *pid_map;
	struct scribe_res_map *sunaddr_map;
};

extern void scribe_track_resource(struct scribe_context *ctx,
				  struct scribe_resource *res);

struct scribe_lock_region;
struct resource_ops_struct {
	bool use_spinlock;
	bool track_users;
	void (*acquire) (struct scribe_context *, struct scribe_resource *,
			 bool *);
	void (*release) (struct scribe_resource *, bool *);

	size_t (*get_description)(struct scribe_resource *, char *, size_t);

#ifdef CONFIG_LOCKDEP
	struct lock_class_key key;
	const char *name;
#endif
};

#define RES_DESC_MAX PATH_MAX

extern struct resource_ops_struct scribe_resource_ops[SCRIBE_RES_NUM_TYPES];

static inline int use_spinlock(struct scribe_resource *res)
{
	return scribe_resource_ops[res->type].use_spinlock;
}

static inline size_t get_description(struct scribe_resource *res,
				     char *buffer, size_t size)
{
	struct resource_ops_struct *ops = &scribe_resource_ops[res->type];

	if (ops->get_description)
		return ops->get_description(res, buffer, size);
	return snprintf(buffer, size, "none");
}

extern struct scribe_res_map_ops scribe_pid_map_ops;
extern struct scribe_res_map_ops scribe_sunaddr_map_ops;

static inline int should_handle_resources(struct scribe_ps *scribe)
{
	if (!is_scribed(scribe))
		return 0;

	return should_scribe_resources(scribe);
}

struct scribe_lock_region {
	struct scribe_ps *owner;
	struct list_head user_node;
	struct list_head res_node;
	scribe_insert_point_t ip;
	union {
		struct scribe_event *generic;
		struct scribe_event_resource_lock *regular;
		struct scribe_event_resource_lock_intr *intr;
		struct scribe_event_resource_lock_extra *extra;
	} lock_event;
	struct scribe_event_resource_unlock *unlock_event;
	void *nested_object; /* Used with __lock_objects() */
	struct scribe_resource *res;
	int flags;
};

extern struct scribe_lock_region *scribe_find_lock_region(
				struct scribe_res_user *user, void *object);

struct scribe_lock_arg {
	struct scribe_resource *res;
	int flags;
};

static inline struct scribe_lock_arg *__lock_arg(
		struct scribe_lock_arg *arg,
		struct scribe_resource *res, int flags)
{
	arg->res = res;
	arg->flags = flags;
	return arg;
}

static inline struct scribe_lock_arg *__lock_arg_keyed(
		struct scribe_ps *scribe,
		struct scribe_lock_arg *arg,
		struct scribe_res_map *map, void *key,
		int flags)
{
	struct scribe_mapped_res *mres;

	mres = scribe_get_mapped_res(map, key, &scribe->resources);

	arg->res = &mres->mr_res;
	arg->flags = flags;

	return arg;
}

extern int __lock_object(struct scribe_ps *scribe, struct scribe_lock_arg *arg,
			 void *nested_object);
extern int __scribe_lock_object(struct scribe_ps *scribe,
				struct scribe_lock_arg *arg);
extern int __scribe_lock_objects(struct scribe_ps *scribe,
				 struct scribe_lock_arg *args, int count);
extern void __scribe_unlock_region(struct scribe_ps *scribe,
				   struct scribe_lock_region *lock_region,
				   bool discard);

static inline void __scribe_unlock_object(struct scribe_ps *scribe,
					  void *object, bool discard)
{
	struct scribe_lock_region *lock_region;
	struct scribe_res_user *user;

	user = &scribe->resources;
	lock_region = scribe_find_lock_region(user, object);
	BUG_ON(!lock_region);

	__scribe_unlock_region(scribe, lock_region, discard);
}

extern void __scribe_downgrade_region(struct scribe_ps *scribe,
				      struct scribe_lock_region *lock_region);

static inline void __scribe_downgrade_object(struct scribe_ps *scribe,
					     void *object)
{
	struct scribe_lock_region *lock_region;
	struct scribe_res_user *user;

	user = &scribe->resources;
	lock_region = scribe_find_lock_region(user, object);
	BUG_ON(!lock_region);

	__scribe_downgrade_region(scribe, lock_region);
}

extern void __scribe_assert_locked_object(struct scribe_ps *scribe,
					  void *object);
extern struct scribe_lock_region *scribe_alloc_lock_region(int doing_recording,
							   int res_extra);
extern void scribe_free_lock_region(struct scribe_lock_region *lock_region);

#endif /* _SCRIBE_RESOURCE_INTERNAL_H_ */
