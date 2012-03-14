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

static struct kmem_cache *mres_cache;

void __init scribe_res_init_caches(void)
{
	mres_cache = KMEM_CACHE(scribe_mapped_res,
				SLAB_HWCACHE_ALIGN | SLAB_PANIC);
}

struct scribe_mapped_res *scribe_alloc_mres(
					void *key, struct scribe_res_user *user)
{
	struct scribe_mapped_res *mres;

	mres = scribe_get_pre_alloc_mres(user);
	mres->mr_key = key;

	return mres;
}

void scribe_free_mres(struct scribe_mapped_res *mres)
{
	kmem_cache_free(mres_cache, mres);
}

struct scribe_lock_region *scribe_alloc_lock_region(int doing_recording,
						    int res_extra)
{
	struct scribe_lock_region *lock_region;

	lock_region = kmalloc(sizeof(*lock_region), GFP_KERNEL);
	if (!lock_region)
		return NULL;

	lock_region->lock_event.generic = NULL;
	lock_region->unlock_event = NULL;

	if (!doing_recording) {
		/*
		 * During replaying, we don't really need those events, since
		 * they will be coming from the event pump.
		 */
		return lock_region;
	}

	if (res_extra) {
		/*
		 * We don't know how long will be the description, we'll
		 * assume the maximum.
		 */
		lock_region->lock_event.extra = scribe_alloc_event_sized(
				SCRIBE_EVENT_RESOURCE_LOCK_EXTRA, RES_DESC_MAX);
	} else
		lock_region->lock_event.regular = scribe_alloc_event(
				SCRIBE_EVENT_RESOURCE_LOCK);

	if (!lock_region->lock_event.generic)
		goto err;

	if (res_extra) {
		lock_region->unlock_event = scribe_alloc_event(
						SCRIBE_EVENT_RESOURCE_UNLOCK);
		if (!lock_region->unlock_event)
			goto err;
	}

	return lock_region;

err:
	scribe_free_lock_region(lock_region);
	return NULL;
}

/* Use this when you didn't had the chance to lock()/unlock() the resource */
void scribe_free_lock_region(struct scribe_lock_region *lock_region)
{
	scribe_free_event(lock_region->lock_event.generic);
	scribe_free_event(lock_region->unlock_event);
	kfree(lock_region);
}

void scribe_resource_init_user(struct scribe_res_user *user)
{
	INIT_HLIST_HEAD(&user->pre_alloc_mres);
	user->num_pre_alloc_mres = 0;
	INIT_LIST_HEAD(&user->pre_alloc_regions);
	user->num_pre_alloc_regions = 0;
	user->pre_alloc_sunaddr = NULL;
	INIT_LIST_HEAD(&user->locked_regions);
}

static int resource_pre_alloc(struct scribe_res_user *user,
			      int doing_recording, int res_extra)
{
	struct scribe_lock_region *lock_region;
	struct scribe_mapped_res *mres;
	struct sunaddr *sunaddr;

	while (user->num_pre_alloc_mres < MAX_PRE_ALLOC) {
		mres = kmem_cache_alloc(mres_cache, GFP_KERNEL);
		if (!mres)
			return -ENOMEM;

		hlist_add_head(&mres->mr_node, &user->pre_alloc_mres);
		user->num_pre_alloc_mres++;
	}

	while (user->num_pre_alloc_regions < MAX_PRE_ALLOC) {
		lock_region = scribe_alloc_lock_region(doing_recording,
						       res_extra);
		if (!lock_region)
			return -ENOMEM;

		list_add(&lock_region->user_node, &user->pre_alloc_regions);
		user->num_pre_alloc_regions++;
	}

	if (!user->pre_alloc_sunaddr) {
		sunaddr = kmalloc(sizeof(*sunaddr), GFP_KERNEL);
		if (!sunaddr)
			return -ENOMEM;
		user->pre_alloc_sunaddr = sunaddr;
	}

	return 0;
}

static int __resource_prepare(struct scribe_ps *scribe)
{
	might_sleep();

	if (resource_pre_alloc(&scribe->resources, is_recording(scribe),
			       should_scribe_res_extra(scribe)))
		return -ENOMEM;

	return 0;
}

int scribe_resource_prepare(void)
{
	struct scribe_ps *scribe = current->scribe;

	if (!should_handle_resources(scribe))
		return 0;

	return __resource_prepare(scribe);
}

void scribe_assert_no_locked_region(struct scribe_res_user *user)
{
	WARN(!list_empty(&user->locked_regions),
	     "Some regions are left unlocked\n");
}

void scribe_resource_exit_user(struct scribe_res_user *user)
{
	struct scribe_mapped_res *mres;
	struct hlist_node *tmp1, *tmp2;
	struct scribe_lock_region *lockr, *ltmp;

	scribe_assert_no_locked_region(user);

	hlist_for_each_entry_safe(mres, tmp1, tmp2,
				  &user->pre_alloc_mres, mr_node) {
		hlist_del(&mres->mr_node);
		kmem_cache_free(mres_cache, mres);
	}

	list_for_each_entry_safe(lockr, ltmp,
				 &user->pre_alloc_regions, user_node) {
		list_del(&lockr->user_node);
		scribe_free_lock_region(lockr);
	}

	kfree(user->pre_alloc_sunaddr);
}

struct scribe_mapped_res *scribe_get_pre_alloc_mres(
						struct scribe_res_user *user)
{
	struct scribe_mapped_res *mres;
	BUG_ON(hlist_empty(&user->pre_alloc_mres));
	mres = hlist_entry(user->pre_alloc_mres.first,
			   struct scribe_mapped_res, mr_node);
	hlist_del(&mres->mr_node);
	user->num_pre_alloc_mres--;
	return mres;
}

struct sunaddr *scribe_get_pre_alloc_sunaddr(struct scribe_res_user *user)
{
	struct sunaddr *new_sun;
	new_sun = user->pre_alloc_sunaddr;
	BUG_ON(!new_sun);
	user->pre_alloc_sunaddr = NULL;
	return new_sun;
}

struct scribe_lock_region *scribe_get_pre_alloc_lock_region(
						struct scribe_res_user *user)
{
	struct scribe_lock_region *lock_region;
	BUG_ON(list_empty(&user->pre_alloc_regions));
	lock_region = list_first_entry(&user->pre_alloc_regions,
				       struct scribe_lock_region, user_node);
	list_del(&lock_region->user_node);
	user->num_pre_alloc_regions--;
	return lock_region;
}
