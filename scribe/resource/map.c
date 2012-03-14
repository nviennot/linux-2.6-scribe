/*
 * Copyright (C) 2010 Oren Laadan <orenl@cs.columbia.edu>
 * Copyright (C) 2010 Nicolas Viennot <nicolas@viennot.biz>
 *
 *  This file is subject to the terms and conditions of the GNU General Public
 *  License.  See the file COPYING in the main directory of the Linux
 *  distribution for more details.
 */

#include "internal.h"
#include <linux/rculist.h>
#include <linux/hash.h>
#include <asm/checksum.h>

void scribe_init_res_map(struct scribe_res_map *map,
			 struct scribe_res_map_ops *ops)
{
	BUG_ON(ops->hash_fn);

	spin_lock_init(&map->lock);
	map->ops = ops;
	INIT_HLIST_HEAD(&map->head[0]);
}

void scribe_exit_res_map(struct scribe_res_map *map)
{
	BUG_ON(map->ops->hash_fn);
	WARN_ON(!hlist_empty(&map->head[0]));
}

struct scribe_res_map *scribe_alloc_res_map(struct scribe_res_map_ops *ops)
{
	struct scribe_res_map *map;
	int hash_size = 1 << ops->hash_bits;
	int i;

	/* we use hash_size - 1 because the struct already has one */
	map = kmalloc(sizeof(*map) + (hash_size-1)*sizeof(*map->head),
		      GFP_KERNEL);
	if (!map)
		return NULL;

	spin_lock_init(&map->lock);
	map->ops = ops;
	for (i = 0; i < hash_size; i++)
		INIT_HLIST_HEAD(&map->head[i]);

	return map;
}

void scribe_free_res_map(struct scribe_res_map *map)
{
	BUG_ON(!map->ops->hash_fn);
	kfree(map);
}

static struct scribe_mapped_res *__find_mapped_res(
	struct scribe_res_map_ops *ops, struct hlist_head *head, void *key)
{
	struct scribe_mapped_res *mres;
	struct hlist_node *node;

	if (ops->cmp_keys) {
		hlist_for_each_entry_rcu_bh(mres, node, head, mr_node) {
			if (ops->cmp_keys(mres->mr_key, key))
				return mres;
		}
	} else {
		hlist_for_each_entry_rcu_bh(mres, node, head, mr_node) {
			if (mres->mr_key == key)
				return mres;
		}
	}

	return NULL;
}

/*
 * XXX get_mapped_res() must be followed by __lock_object() to add the
 * resource into the tracked resources (and thus allowing it to be removed)
 */
struct scribe_mapped_res *scribe_get_mapped_res(struct scribe_res_map *map,
					void *key, struct scribe_res_user *user)
{
	struct scribe_res_map_ops *ops = map->ops;
	struct scribe_mapped_res *mres;
	struct hlist_head *head;

	if (ops->hash_fn)
		head = &map->head[ops->hash_fn(map, key)];
	else
		head = &map->head[0];

	rcu_read_lock_bh();
	mres = __find_mapped_res(ops, head, key);
	rcu_read_unlock_bh();

	if (mres)
		return mres;

	spin_lock_bh(&map->lock);
	mres = __find_mapped_res(ops, head, key);
	if (unlikely(mres)) {
		spin_unlock_bh(&map->lock);
		return mres;
	}

	mres = ops->alloc_mres(key, user);
	mres->mr_map = map;

	scribe_init_resource(&mres->mr_res,
			     ops->get_object(map, key), ops->type);

	hlist_add_head_rcu(&mres->mr_node, head);
	spin_unlock_bh(&map->lock);

	return mres;
}

static void free_rcu_mapped_res(struct rcu_head *rcu)
{
	struct scribe_mapped_res *mres;
	mres = container_of(rcu, struct scribe_mapped_res, mr_rcu);
	mres->mr_free(mres);
}

void scribe_remove_mapped_res(struct scribe_mapped_res *mres)
{
	struct scribe_res_map *map = mres->mr_map;

	spin_lock_bh(&map->lock);
	hlist_del_rcu(&mres->mr_node);
	spin_unlock_bh(&map->lock);

	mres->mr_free = map->ops->free_mres;
	call_rcu(&mres->mr_rcu, free_rcu_mapped_res);
}

/*
 * The following is the map operation definitions we'll use.
 */

static void *key_object(struct scribe_res_map *map, void *key)
{
	return key;
}

/*
 * On demand mapping @pid -> @resource. The resources are persistent.
 * The @pid is different from the resource id:
 * e.g. we want to map a pid to a resource, but that resource may have a
 * totally different id.
 */

static unsigned long hash_fn_pid(struct scribe_res_map *map, void *key)
{
	return hash_long((int)key, PID_RES_HASH_BITS);
}

struct scribe_res_map_ops scribe_pid_map_ops = {
	.type =  SCRIBE_RES_TYPE_PID,
	.alloc_mres = scribe_alloc_mres,
	.free_mres = scribe_free_mres,
	.get_object = key_object,
	.hash_bits = PID_RES_HASH_BITS,
	.hash_fn = hash_fn_pid,
};


/* Mapping of unix abstract path to res */
static struct scribe_mapped_res *alloc_mres_sunaddr(
					void *key, struct scribe_res_user *user)
{
	struct scribe_mapped_res *mres = scribe_alloc_mres(key, user);
	struct sunaddr *sun = key, *new_sun;

	new_sun = scribe_get_pre_alloc_sunaddr(user);
	new_sun->len = sun->len;
	memcpy(&new_sun->addr, &sun->addr, sun->len);

	mres->mr_key = new_sun;

	return mres;
}

static void free_mres_sunaddr(struct scribe_mapped_res *mres)
{
	struct sunaddr *sun = mres->mr_key;
	kfree(sun);
	scribe_free_mres(mres);
}

static unsigned long hash_fn_sunaddr(struct scribe_res_map *map, void *key)
{
	struct sunaddr *sun = key;
	return hash_long(csum_partial(&sun->addr, sun->len, 0),
			 SUNADDR_RES_HASH_BITS);
}

static bool cmp_keys_sunaddr(void *key1, void *key2)
{
	struct sunaddr *sun1 = key1, *sun2 = key2;

	return sun1->len == sun2->len &&
		!memcmp(&sun1->addr, &sun2->addr, sun1->len);
}

struct scribe_res_map_ops scribe_sunaddr_map_ops = {
	.type = SCRIBE_RES_TYPE_SUNADDR,
	.alloc_mres = alloc_mres_sunaddr,
	.free_mres = free_mres_sunaddr,
	.get_object = key_object,
	.hash_bits = SUNADDR_RES_HASH_BITS,
	.hash_fn = hash_fn_sunaddr,
	.cmp_keys = cmp_keys_sunaddr,
};
