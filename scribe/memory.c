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

#include <linux/rcupdate.h>
#include <linux/rculist.h>
#include <linux/hash.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/pagemap.h>
#include <linux/acct.h>
#include <linux/hardirq.h>
#include <linux/bootmem.h>
#include "../mm/internal.h"
#include <asm/tlb.h>

/* TODO heavy reformatting and documentation needed */

/* We don't want to have the ref counter in the scribe_page struct
 * since it may not even exist (the scribe_page structs are allocated on demand).
 * Thoses ref_cnt structs are kept in a per monitor list (ctx->mem_list).
 * Reference counters are needed on mm_structs and inodes (FIXME It's dirty,
 * separate structures should be used):
 * - the mm_struct ref counter is used to determine if a task is operating in
 *   single or multithreaded (Can't use the mm->mm_users counter properly).
 * - the inode counter represents the number of scribed task using that
 *   file mapping (it's per monitor). The counter is incremented per mm_struct
 *   and not per task.
 * When the counter hits 0, scribe_page_remove(object, OFFSET_ALL) is called
 * to clean the scribe_pages.
 */
struct scribe_obj_ref {
	struct list_head	node;
	void			*object;
	atomic_t		counter;

	/* unsed only when object is a mm_struct. It is locked by the
	 * underlying mm->mmap_sem. It allows iterating trough scribetasks
	 */
	struct list_head	mm_list;
};

#define SCRIBE_PAGE_HASH_BITS	14
#define SCRIBE_PAGE_HASH_SIZE	(1 << SCRIBE_PAGE_HASH_BITS)


/* SHOULD NOT BE BIGGER THAN 31 */
#define SCRIBE_OWNERS_ARRAY_SIZE	4

struct scribe_ownership {
	struct list_head	node;
	struct scribe_page	*page;
	struct scribe_ps	*owner;

	/* the virtual address has to be saved: consider two processes mapping
	 * a file at different addresses. task A owns the page, task B steals it.
	 * B have to queue a memory event in B's queue which contains the
	 * virtual address of the page in B's mapping (So in the replay, B can
	 * process the event properly).
	 */
	unsigned long		virt_address;

	/* the serial number is the serial value when we got the page, it helps
	 * to get the history of a page (for the read->write thing).
	 */
	unsigned int		start_serial;

#define EXPIRATION_USEC (1000*100) /* cannot be more than a sec */
	struct timeval		timestamp;

	/* mm->shared_req list contains scribe_pages, through req_node */
	struct list_head	req_node;
};

/* the scribe_page are contained in a hash table (ctx->mem_hash). They are
 * identified with a scribe_page_key object which is a pair of
 * (mm_struct addr, page_address) or (inode addr, file_offset).
 */
struct scribe_page_key {
	void		*object;	/* either a mm struct or an inode */
	unsigned long	offset;		/* either an address or an offset */
};


#define READ_THEN_WRITE_MIN_THRESHOLD 1
#define READ_THEN_WRITE_MAX_THRESHOLD 100

struct scribe_page {
	struct hlist_node		node;	/* hash node */
	struct scribe_page_key		key;	/* hash key */

	/* the owner of the page, if any */
	spinlock_t			owners_lock;
	unsigned long			owners_static_usage; /* bitset */
	struct scribe_ownership		owners_static[SCRIBE_OWNERS_ARRAY_SIZE];
	struct list_head                owners;

	unsigned int			read_waiters, write_waiters;

	/* read_then_write: counter up to 128,
	 * -1 means write_access is forced.
	 * protected with owners_lock.
	 */
	char				read_then_write;

	char				write_access;
	int				ownership_token;
	wait_queue_head_t		ownership_wait;

	/* The serial number is incremented when a task takes ownership */
	atomic_t			serial;
	wait_queue_head_t		serial_wait;

	struct rcu_head rcu;
};

struct page_hash_bucket {
	spinlock_t lock;
	struct hlist_head pages;
};

struct scribe_mm_context {
	spinlock_t		obj_refs_lock;
	struct list_head	obj_refs;

	struct page_hash_bucket buckets[SCRIBE_PAGE_HASH_SIZE];
};

struct scribe_mm {
	/* node for mm_struct->scribe_list */
	struct list_head node;

	/* the scribe_mm struct is per task */
	struct scribe_ps	*scribe;

	/* own_pgd is per task private page table which reflects the
	 * mm_struct->pgd table (the real one) but with non present pte's on
	 * shared pages to be able to fault on them.
	 */
	pgd_t			*own_pgd;

	int is_alone;

	/* we'll add requests for page sharing here */
	spinlock_t		req_lock;
	struct list_head	shared_req;
	int			weak_owner;

	scribe_insert_point_t	weak_owner_events_ip;

	int disable_sync_sleep;
};

static struct kmem_cache *page_cache;

void __init scribe_mem_init_caches(void)
{
	page_cache = KMEM_CACHE(scribe_page, SLAB_HWCACHE_ALIGN | SLAB_PANIC);
}

/* lock order:
 *	page->owners_lock
 *		mm->req_lock
 */

static inline int should_handle_mm(struct scribe_ps *scribe)
{
	if (!may_be_scribed(scribe) || !scribe->mm)
		return 0;

	return should_scribe_mm(scribe);
}

pgd_t *scribe_get_pgd(struct mm_struct *next, struct task_struct *tsk)
{
	struct scribe_ps *scribe = tsk->scribe;

	if (unlikely(tsk->mm != next))
		return next->pgd;

	/* we want to use our own pgd if available */
	if (should_handle_mm(scribe) && likely(scribe->mm && scribe->mm->own_pgd))
		return scribe->mm->own_pgd;
	return next->pgd;
}


/********************************************************
    Owner list management
********************************************************/

static struct scribe_ownership *__alloc_ownership_struct(
						struct scribe_page *page)
{
	struct scribe_ownership *os;
	unsigned int index;

	/*
	 * A few ownerships struct are pre-allocated to get better
	 * performances.
	 */

	index = ffz(page->owners_static_usage);
	if (index < SCRIBE_OWNERS_ARRAY_SIZE) {
		set_bit(index, &page->owners_static_usage);
		return &page->owners_static[index];
	}

	spin_unlock(&page->owners_lock);
	os = kmalloc(sizeof(*os), GFP_KERNEL);
	spin_lock(&page->owners_lock);

	return os;
}

static void __free_ownership_struct(struct scribe_ownership *os)
{
	struct scribe_page *page = os->page;
	unsigned int index;

	index = os - page->owners_static;
	if (index < SCRIBE_OWNERS_ARRAY_SIZE)
		clear_bit(index, &page->owners_static_usage);
	else
		kfree(os);
}

static struct scribe_ownership *find_ownership(struct scribe_page *page,
					       struct scribe_ps *owner)
{
	struct scribe_ownership *os;

	list_for_each_entry(os, &page->owners, node) {
		if (os->owner == owner)
			return os;
	}

	return NULL;
}

static inline bool is_owned_by(struct scribe_page *page,
			       struct scribe_ps *owner)
{
	return (bool)find_ownership(page, owner);
}

static struct scribe_ownership *add_page_ownership(struct scribe_page *page,
						   struct scribe_ps *owner,
						   unsigned long virt_address)
{
	/* XXX add_page_ownership release the lock owners_lock */

	struct scribe_ownership *os;

#ifdef CONFIG_DEBUG_KERNEL
	assert_spin_locked(&page->owners_lock);
	BUG_ON(is_owned_by(page, owner));
#endif

	os = __alloc_ownership_struct(page);
	if (!os)
		return ERR_PTR(-ENOMEM);

	list_add(&os->node, &page->owners);
	os->page = page;
	os->owner = owner;
	os->virt_address = virt_address;
	os->start_serial = atomic_read(&page->serial);
	do_gettimeofday(&os->timestamp);
	INIT_LIST_HEAD(&os->req_node);

	return os;
}

static void rm_page_ownership(struct scribe_ownership *os)
{
#ifdef CONFIG_DEBUG_KERNEL
	struct scribe_page *page = os->page;
	assert_spin_locked(&page->owners_lock);
	BUG_ON(!list_empty(&os->req_node));
#endif

	list_del(&os->node);
	__free_ownership_struct(os);
}

/********************************************************
    Object referencing (mm_struct and inodes)
*********************************************************/

static struct scribe_obj_ref * __find_obj_ref(struct scribe_mm_context *mm_ctx,
					      void *object)
{
	struct scribe_obj_ref *ref;

	list_for_each_entry(ref, &mm_ctx->obj_refs, node) {
		if (ref->object == object)
			return ref;
	}

	return NULL;
}

static struct scribe_obj_ref *find_obj_ref(struct scribe_mm_context *mm_ctx,
					   void *object)
{
	struct scribe_obj_ref *ref;

	spin_lock(&mm_ctx->obj_refs_lock);
	ref = __find_obj_ref(mm_ctx, object);
	spin_unlock(&mm_ctx->obj_refs_lock);

	return ref;
}

static struct scribe_obj_ref *get_obj_ref(struct scribe_mm_context *mm_ctx,
					  void *object)
{
	struct scribe_obj_ref *ref, *ref_alloc;

	spin_lock(&mm_ctx->obj_refs_lock);
	ref = __find_obj_ref(mm_ctx, object);
	if (!ref) {
		spin_unlock(&mm_ctx->obj_refs_lock);
		ref_alloc = (struct scribe_obj_ref *)
				kmalloc(sizeof(*ref), GFP_KERNEL);
		if (!ref_alloc)
			return ERR_PTR(-ENOMEM);
		spin_lock(&mm_ctx->obj_refs_lock);

		/* raced ? */
		ref = __find_obj_ref(mm_ctx, object);
		if (ref)
			kfree(ref_alloc);
		else {
			ref = ref_alloc;
			ref->object = object;
			atomic_set(&ref->counter, 0);
			INIT_LIST_HEAD(&ref->mm_list);
			list_add(&ref->node, &mm_ctx->obj_refs);
		}
	}

	atomic_inc(&ref->counter);
	spin_unlock(&mm_ctx->obj_refs_lock);
	return ref;
}

static void remove_pages_of(struct scribe_mm_context *mm_ctx, void *key_object);

static void put_obj_ref(struct scribe_mm_context *mm_ctx, void *object)
{
	struct scribe_obj_ref *ref;

	ref = find_obj_ref(mm_ctx, object);

	if (atomic_dec_and_lock(&ref->counter, &mm_ctx->obj_refs_lock)) {
		list_del(&ref->node);
		spin_unlock(&mm_ctx->obj_refs_lock);

		remove_pages_of(mm_ctx, ref->object);
		kfree(ref);
	}
}

static int get_all_objects(struct scribe_mm_context *mm_ctx,
			   struct mm_struct *mm)
{
	struct scribe_obj_ref *ref;
	struct vm_area_struct *vma;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (!(vma->vm_flags & VM_SHARED))
			continue;

		ref = get_obj_ref(mm_ctx, vma->vm_file->f_dentry->d_inode);
		if (IS_ERR(ref)) {
			WARN_ON(1); /* FIXME do the proper cleaning */
			return PTR_ERR(ref);
		}
	}

	return 0;
}

static void put_all_objects(struct scribe_mm_context *mm_ctx,
			    struct mm_struct *mm)
{
	struct vm_area_struct *vma;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma->vm_flags & VM_SHARED)
			put_obj_ref(mm_ctx, vma->vm_file->f_dentry->d_inode);
	}
}

static int get_scribe_cnt(struct mm_struct *mm)
{
	/*
	 * FIXME what's up with this lock ... ? It doesn't seem to be
	 * synchronizing anything
	 */

	int ret;
	spin_lock(&mm->scribe_lock);
	ret = mm->scribe_cnt;
	spin_unlock(&mm->scribe_lock);
	return ret;
}

static inline bool is_sharing_mm(struct mm_struct *mm)
{
	return get_scribe_cnt(mm) > 1;
}

static int __rm_shadow_mm(struct scribe_mm *scribe_mm, struct mm_struct *mm)
{
	int cnt;

	spin_lock(&mm->scribe_lock);
	list_del(&scribe_mm->node);
	cnt = --mm->scribe_cnt;
	spin_unlock(&mm->scribe_lock);
	wake_up(&mm->scribe_wait);

	return cnt;
}

static void rm_shadow_mm(struct scribe_mm *scribe_mm, struct mm_struct *mm)
{
	struct scribe_mm_context *mm_ctx;
	int cnt;

	cnt = __rm_shadow_mm(scribe_mm, mm);
	if (!cnt) {
		mm_ctx = scribe_mm->scribe->ctx->mm_ctx;
		down_read(&mm->mmap_sem);
		put_all_objects(mm_ctx, mm);
		up_read(&mm->mmap_sem);

		remove_pages_of(mm_ctx, mm);
	}
}

static int add_shadow_mm(struct scribe_mm *scribe_mm, struct mm_struct *mm)
{
	struct scribe_mm_context *mm_ctx;
	int cnt, ret = 0;

	down_write(&mm->mmap_sem);

	spin_lock(&mm->scribe_lock);
	list_add(&scribe_mm->node, &mm->scribe_list);
	cnt = mm->scribe_cnt++;
	spin_unlock(&mm->scribe_lock);
	wake_up(&mm->scribe_wait);

	if (!cnt) {
		mm_ctx = scribe_mm->scribe->ctx->mm_ctx;
		ret = get_all_objects(mm_ctx, mm);
		if (ret < 0)
			__rm_shadow_mm(scribe_mm, mm);
	}

	up_write(&mm->mmap_sem);
	return ret;
}

/********************************************************
    scribe_page manipulation
*********************************************************/

static void init_page_hash_bucket(struct page_hash_bucket *hb)
{
	spin_lock_init(&hb->lock);
	INIT_HLIST_HEAD(&hb->pages);
}

struct scribe_mm_context *scribe_alloc_mm_context(void)
{
	struct scribe_mm_context *mm_ctx;
	int i;

	mm_ctx = kmalloc(sizeof(*mm_ctx), GFP_KERNEL);
	if (!mm_ctx)
		return NULL;

	spin_lock_init(&mm_ctx->obj_refs_lock);
	INIT_LIST_HEAD(&mm_ctx->obj_refs);

	for (i = 0; i < SCRIBE_PAGE_HASH_SIZE; i++)
		init_page_hash_bucket(&mm_ctx->buckets[i]);

	return mm_ctx;
}

void scribe_free_mm_context(struct scribe_mm_context *mm_ctx)
{
#ifdef CONFIG_DEBUG_KERNEL
	struct page_hash_bucket *hb;
	int i;

	for (i = 0; i < SCRIBE_PAGE_HASH_SIZE; i++) {
		hb = &mm_ctx->buckets[i];
		WARN_ON(!hlist_empty(&hb->pages));
	}
#endif

	kfree(mm_ctx);
}

static struct page_hash_bucket *get_page_hash_bucket(
					struct scribe_mm_context *mm_ctx,
					struct scribe_page_key *key)
{
	unsigned long hash;

	hash = (unsigned long)(key->object) << 3;
	hash ^= key->offset;
	hash ^= key->offset << 16;
	hash = hash_long(hash, SCRIBE_PAGE_HASH_BITS);

	return &mm_ctx->buckets[hash];
}

static inline int equal_page_keys(struct scribe_page_key *key1,
				  struct scribe_page_key *key2)
{
	return key1->offset == key2->offset && key1->object == key2->object;
}

static struct scribe_page *__find_scribe_page(struct page_hash_bucket *hb,
					      struct scribe_page_key *key)
{
	struct scribe_page *page;
	struct hlist_node *node;

	hlist_for_each_entry_rcu(page, node, &hb->pages, node) {
		if (equal_page_keys(&page->key, key))
			return page;
	}
	return NULL;
}

static struct scribe_page *get_scribe_page(struct scribe_mm_context *mm_ctx,
					   struct scribe_page_key *key)
{
	struct page_hash_bucket *hb = get_page_hash_bucket(mm_ctx, key);
	struct scribe_page *page;
	struct scribe_page *page_alloc;

	rcu_read_lock();
	page = __find_scribe_page(hb, key);
	rcu_read_unlock();
	if (page)
		return page;

	/* not found ... allocating */
	page_alloc = kmem_cache_zalloc(page_cache, GFP_KERNEL);
	if (!page_alloc)
		return ERR_PTR(-ENOMEM);

	page_alloc->key = *key;
	spin_lock_init(&page_alloc->owners_lock);
	INIT_LIST_HEAD(&page_alloc->owners);
	init_waitqueue_head(&page_alloc->ownership_wait);
	init_waitqueue_head(&page_alloc->serial_wait);

	/* raced ? */
	spin_lock(&hb->lock);
	page = __find_scribe_page(hb, key);
	if (unlikely(page)) {
		spin_unlock(&hb->lock);
		kmem_cache_free(page_cache, page_alloc);
		return page;
	}

	page = page_alloc;
	hlist_add_head_rcu(&page->node, &hb->pages);
	spin_unlock(&hb->lock);

	return page;
}

static void scribe_make_page_public(struct scribe_ownership *os,
				    int write_access);
static void __scribe_page_release_ownership(struct scribe_ps *scribe,
					    void *key_object)
{
	struct scribe_mm_context *mm_ctx = scribe->ctx->mm_ctx;
	struct page_hash_bucket *hb;
	struct hlist_node *node;
	struct scribe_page *page;
	struct scribe_ownership *os;
	int i;

	rcu_read_lock();
	for (i = 0; i < SCRIBE_PAGE_HASH_SIZE; i++) {
		hb = &mm_ctx->buckets[i];
		hlist_for_each_entry_rcu(page, node, &hb->pages, node) {
			if (page->key.object != key_object)
				continue;

			spin_lock(&page->owners_lock);
			os = find_ownership(page, scribe);
			if (os) {
				spin_lock(&scribe->mm->req_lock);
				if (!list_empty(&os->req_node))
					list_del_init(&os->req_node);
				spin_unlock(&scribe->mm->req_lock);

				scribe_make_page_public(os, 1);
			}
			spin_unlock(&page->owners_lock);
		}
	}
	rcu_read_unlock();
}

#define ALL_PAGES 0
#define ONLY_ANONYMOUS_PAGES 1
static void scribe_page_release_ownership(struct scribe_ps *scribe,
					  int only_anonymous_pages)
{
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	void *object;

	mm = scribe->p->mm;

	__scribe_page_release_ownership(scribe, mm);

	if (only_anonymous_pages == ONLY_ANONYMOUS_PAGES)
		return;

	for (vma = mm->mmap; vma; vma = vma->vm_next) {
		if (vma->vm_flags & VM_SHARED) {
			object = vma->vm_file->f_dentry->d_inode;
			__scribe_page_release_ownership(scribe, object);
		}
	}
}

static void free_rcu_page(struct rcu_head *rcu)
{
	struct scribe_page *page;
	page = container_of(rcu, struct scribe_page, rcu);
	kmem_cache_free(page_cache, page);
}

/* remove_pages_of() is called when a shmem is unmapped or when a
 * mm_struct goes away.
 * It basically remove tracking of a page (serial number would be reset).
 */
static void remove_pages_of(struct scribe_mm_context *mm_ctx, void *key_object)
{
	struct page_hash_bucket *hb;
	struct scribe_page *page;
	struct hlist_node *node, *tmp;
	int i;

	for (i = 0; i < SCRIBE_PAGE_HASH_SIZE; i++) {
		hb = &mm_ctx->buckets[i];
		spin_lock(&hb->lock);
		hlist_for_each_entry_safe(page, node, tmp, &hb->pages, node) {
			if (page->key.object != key_object)
				continue;

			hlist_del_rcu(&page->node);
			call_rcu(&page->rcu, free_rcu_page);
		}
		spin_unlock(&hb->lock);
	}
}

static void get_page_key(struct vm_area_struct *vma, unsigned long address,
			 struct scribe_page_key *key)
{

	if (vma->vm_flags & VM_SHARED) {
		key->object = vma->vm_file->f_dentry->d_inode;
		key->offset = (address - vma->vm_start) >> PAGE_SHIFT;
		key->offset += vma->vm_pgoff;
	} else {
		key->object = vma->vm_mm;
		key->offset = address & PAGE_MASK;
	}
}

static bool inline page_access_ok(struct scribe_ps *scribe,
				  struct scribe_page *page, int write_access)
{
	if (!is_owned_by(page, scribe))
		return false;

	if (!write_access)
		return true;

	if (page->write_access)
		return true;

	return false;
}

static int page_down_trylock(struct scribe_page *page, int write_access)
{
	int ret = 0;

	spin_lock(&page->owners_lock);

	if (page->ownership_token && (write_access || page->write_access))
		goto out;

	page->ownership_token++;
	page->write_access = write_access;
	ret = 1;

out:
	spin_unlock(&page->owners_lock);
	return ret;
}


static void page_down(struct scribe_page *page, int write_access)
{
	DEFINE_WAIT(__wait);

	for (;;) {
		if (write_access)
			prepare_to_wait_exclusive(&page->ownership_wait,
					&__wait, TASK_UNINTERRUPTIBLE);
		else
			prepare_to_wait(&page->ownership_wait,
					&__wait, TASK_UNINTERRUPTIBLE);
		if (page_down_trylock(page, write_access))
			break;
		schedule();
	}
	finish_wait(&page->ownership_wait, &__wait);
}

static void page_up(struct scribe_page *page, int write_access)
{
	assert_spin_locked(&page->owners_lock);
	page->ownership_token--;
	wake_up(&page->ownership_wait);
}

static void page_downgrade_write(struct scribe_page *page)
{
	assert_spin_locked(&page->owners_lock);
	BUG_ON(!page->write_access);
	page->write_access = 0;
	wake_up(&page->ownership_wait);
}

static inline void inc_waiters(struct scribe_page *page, int write_access)
{
	if (write_access)
		page->write_waiters++;
	else
		page->read_waiters++;
}

static inline void dec_waiters(struct scribe_page *page, int write_access)
{
	if (write_access)
		page->write_waiters--;
	else
		page->read_waiters--;
}

/********************************************************
    private page table management
*********************************************************/

static void update_private_pte_locked(struct scribe_mm *scribe_mm,
		struct mm_struct *mm, struct vm_area_struct *vma,
		pte_t *real_pte, unsigned long address, int write_access)
{
	pgd_t *own_pgd;
	pud_t *own_pud;
	pmd_t *own_pmd;
	pte_t *own_pte;

	own_pgd = scribe_mm->own_pgd;
	BUG_ON(!own_pgd);

	own_pgd += pgd_index(address);
	if (pgd_none(*own_pgd))
		return;
	own_pud = pud_offset(own_pgd, address);
	if (pud_none(*own_pud))
		return;
	own_pmd = pmd_offset(own_pud, address);
	if (pmd_none(*own_pmd))
		return;

	own_pte = pte_offset_map_nested2(own_pmd, address);
	if (pte_present(*own_pte)) {
		flush_cache_page(vma, address, pte_pfn(*own_pte));

		if (write_access)
			pte_clear(mm, address, own_pte);
		else
			ptep_set_wrprotect(mm, address, own_pte);

		flush_tlb_page(vma, address);
		update_mmu_cache(vma, address, own_pte);

		if (pte_dirty(*own_pte)) {
			/* Propagating the dirty flag to the real pte */
			ptep_set_access_flags(vma, address, real_pte,
					      pte_mkdirty(*real_pte), 1);
		}
	}
	pte_unmap_nested2(own_pte);
}

static struct scribe_ps *get_scribe_from_mm(struct mm_struct *mm)
{
	struct task_struct *p = mm->owner;

	if (!p)
		return NULL;
	return p->scribe;
}

void scribe_clear_shadow_pte_locked(struct mm_struct *mm,
				    struct vm_area_struct *vma,
				    pte_t *real_pte, unsigned long addr)
{
	struct scribe_mm *scribe_mm;
	spin_lock(&mm->scribe_lock);
	list_for_each_entry(scribe_mm, &mm->scribe_list, node) {
		update_private_pte_locked(scribe_mm, mm, vma,
					  real_pte, addr, 1);
	}
	spin_unlock(&mm->scribe_lock);
}

static void update_private_pte(struct scribe_ps *scribe,
		struct mm_struct *mm, struct vm_area_struct *vma,
		unsigned long address, int write_access)
{
	pgd_t *pgd;
	pud_t *pud;
	pmd_t *pmd;
	pte_t *pte;
	spinlock_t *ptl;

	pgd = pgd_offset(mm, address);
	if (pgd_none(*pgd))
		return;
	pud = pud_offset(pgd, address);
	if (pud_none(*pud))
		return;
	pmd = pmd_offset(pud, address);
	if (pmd_none(*pmd))
		return;

	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	update_private_pte_locked(scribe->mm, mm, vma, pte,
				  address, write_access);
	pte_unmap_unlock(pte, ptl);
}

/********************************************************
    memory context initialization/destruction
*********************************************************/

static struct scribe_mm *get_new_scribe_mm(struct scribe_ps *scribe)
{
	struct scribe_mm *scribe_mm;

	scribe_mm = kmalloc(sizeof(*scribe_mm), GFP_KERNEL);
	if (!scribe_mm)
		return NULL;

	scribe_mm->own_pgd = pgd_alloc(scribe->p->mm);
	if (!scribe_mm->own_pgd) {
		kfree(scribe_mm);
		return NULL;
	}

	scribe_mm->scribe = scribe;

	spin_lock_init(&scribe_mm->req_lock);
	INIT_LIST_HEAD(&scribe_mm->shared_req);
	scribe_mm->weak_owner = 0;
	scribe_mm->disable_sync_sleep = 0;

	return scribe_mm;
}

static void free_shadow_pgd_range(struct mm_struct *mm, pgd_t *pgd,
				  unsigned long addr, unsigned long end)
{
	struct mmu_gather *tlb;

	flush_cache_mm(mm);
	tlb = tlb_gather_mmu(mm, 1);

	/*
	 * The spinlock is necessary to prevent races with rmap calling
	 * scribe_clear_shadow_pte_locked()
	 */
	spin_lock(&mm->scribe_lock);
	__free_pgd_range(tlb, pgd, addr, end, 0, 0);
	spin_unlock(&mm->scribe_lock);

	tlb_finish_mmu(tlb, addr, end);
}

void scribe_free_all_shadow_pgd_range(struct mmu_gather *tlb,
				unsigned long addr, unsigned long end,
				unsigned long floor, unsigned long ceiling)
{
	struct mm_struct *mm = tlb->mm;
	struct scribe_mm *scribe_mm;

	spin_lock(&mm->scribe_lock);
	list_for_each_entry(scribe_mm, &mm->scribe_list, node)
		__free_pgd_range(tlb, scribe_mm->own_pgd,
				 addr, end, floor, ceiling);
	spin_unlock(&mm->scribe_lock);
}


static void free_scribe_mm(struct scribe_mm *scribe_mm, struct mm_struct *mm)
{
	free_shadow_pgd_range(mm, scribe_mm->own_pgd,
			      FIRST_USER_ADDRESS, TASK_SIZE);
	pgd_free(mm, scribe_mm->own_pgd);
	kfree(scribe_mm);
}

static inline int is_vma_scribed(struct scribe_ps *scribe,
				 struct vm_area_struct *vma);
static void maybe_go_multithreaded(struct mm_struct *mm)
{
	struct scribe_ps *scribe = current->scribe;

	/*
	 * XXX @scribe points to the current process, no the new scribed
	 * processed to be attached.
	 */

	if (current->mm != mm) {
		/* The target belongs to another memory context */
		return;
	}

	if (!scribe->mm->is_alone) {
		/* We already made the switch to multithreading */
		return;
	}

	/*
	 * We are going from singlethreaded to multithreaded and the
	 * address space is now shared.
	 */
	scribe->mm->is_alone = 0;
	BUG_ON(!is_sharing_mm(scribe->p->mm));

	/* We don't need the mmap_sem to be taken because we are still alone */
	free_shadow_pgd_range(mm, scribe->mm->own_pgd,
			      FIRST_USER_ADDRESS, TASK_SIZE);
}

int scribe_mem_init_st(struct scribe_ps *scribe)
{
	struct scribe_mm *scribe_mm;
	struct mm_struct *mm = scribe->p->mm;
	int ret;

	if (scribe_mm_disabled(scribe))
		return 0;

	scribe_mm = get_new_scribe_mm(scribe);
	if (!scribe_mm)
		return -ENOMEM;

	ret = add_shadow_mm(scribe_mm, mm);
	if (ret < 0) {
		free_scribe_mm(scribe_mm, mm);
		return ret;
	}
	scribe_mm->is_alone = !is_sharing_mm(mm);

	/*
	 * The wmb protects the context switcher to pick a bad pgd:
	 * scribe_mm->own_pgd must be written before scribe->mm
	 */
	smp_wmb();
	scribe->mm = scribe_mm;

	if (current->scribe == scribe)
		load_cr3(scribe_mm->own_pgd);
	else
		maybe_go_multithreaded(mm);

	scribe_mem_sync_point(scribe, MEM_SYNC_IN);

	return 0;
}

void scribe_mem_exit_st(struct scribe_ps *scribe)
{
	struct scribe_mm *scribe_mm = scribe->mm;
	struct mm_struct *mm = scribe->p->mm;

	if (!scribe_mm)
		return;

	if (unlikely(scribe_mm->weak_owner != MEM_SYNC_IN)) {
		/* BUG() or something got called */
		scribe_mem_sync_point(scribe, MEM_SYNC_IN);
	}

	/*
	 * we don't want any schedule() to call mem_sync_point() while we are
	 * in MEM_SYNC_SLEEP
	 */
	scribe_mm->disable_sync_sleep = 1;

	/* take care of the pending shared memory requests */
	scribe_mem_sync_point(scribe, MEM_SYNC_IN | MEM_SYNC_SLEEP);

	down_read(&mm->mmap_sem);
	scribe_page_release_ownership(scribe, ALL_PAGES);
	up_read(&mm->mmap_sem);

	scribe_mem_sync_point(scribe, MEM_SYNC_OUT | MEM_SYNC_SLEEP);
	scribe_mem_sync_point(scribe, MEM_SYNC_OUT);

	BUG_ON(!list_empty(&scribe_mm->shared_req));

	scribe->mm = NULL;
	smp_wmb();

	if (current->scribe == scribe)
		load_cr3(mm->pgd);

	rm_shadow_mm(scribe_mm, mm);
	free_scribe_mm(scribe_mm, mm);
}

void scribe_mem_reload(struct scribe_ps *scribe)
{
	if (current->scribe != scribe)
		return;

	/*
	 * FIXME we should actually flush the cache and stuff, but on i386 we
	 * don't need it.
	 */
	if (should_handle_mm(scribe))
		load_cr3(scribe->mm->own_pgd);
	else
		load_cr3(current->mm->pgd);
}

/********************************************************
    the logic :)
*********************************************************/

static inline int increment_serial(struct scribe_page *page)
{
	int ret;

	/* we want the value before the incrementation */
	ret = atomic_inc_return(&page->serial) - 1;
	if (waitqueue_active(&page->serial_wait))
		wake_up(&page->serial_wait);

	return ret;
}

static void scribe_make_page_public(struct scribe_ownership *os,
				    int write_access)
{
	struct scribe_page *page;
	struct scribe_ps *scribe;
	struct mm_struct *mm;
	struct vm_area_struct *vma;
	unsigned long virt_address;

	page = os->page;
	scribe = os->owner;

	virt_address = os->virt_address;

	increment_serial(page);

	if (write_access) {
		rm_page_ownership(os);

		if (is_recording(current->scribe))
			page_up(page, page->write_access);
	}
	else {
		if (is_recording(current->scribe))
			page_downgrade_write(page);
		else
			page->write_access = 0;
	}

	/* Now we have to clear the owner's pte (the page is now public) */
	mm = scribe->p->mm;

	/*
	 * FIXME find a way to lock mm->mmap_sem (for find_vma()), maybe the
	 * owner is not sharing our memory mapping.
	 */
	vma = find_vma(mm, virt_address);
	BUG_ON(!vma);
	update_private_pte(scribe, mm, vma, virt_address, write_access);
}

static void scribe_make_page_public_log(struct scribe_ownership *os,
					struct scribe_event *public_event,
					int write_access)
{
	struct scribe_event_mem_public_read *public_read_event;
	struct scribe_event_mem_public_write *public_write_event;
	BUILD_BUG_ON(sizeof(public_read_event) != sizeof(public_write_event));

	if (write_access) {
		public_write_event = (void *)public_event;
		public_write_event->h.type = SCRIBE_EVENT_MEM_PUBLIC_WRITE;
		public_write_event->address = os->virt_address & PAGE_MASK;
	} else {
		public_read_event = (void *)public_event;
		public_read_event->h.type = SCRIBE_EVENT_MEM_PUBLIC_READ;
		public_read_event->address = os->virt_address & PAGE_MASK;
	}
	if (os->owner->mm->weak_owner)
		scribe_queue_event_at(&os->owner->mm->weak_owner_events_ip,
				      public_event);
	else
		scribe_queue_event(os->owner->queue, public_event);
	scribe_make_page_public(os, write_access);
}

static int scribe_make_page_owned_log(struct scribe_ps *scribe,
		struct scribe_page *page, unsigned long address,
		int write_access)
{
	int ret;
	int serial;

	serial = increment_serial(page);
	if (should_scribe_mem_extra(scribe)) {
		if (write_access)
			ret = scribe_queue_new_event(scribe->queue,
					SCRIBE_EVENT_MEM_OWNED_WRITE_EXTRA,
					.address = address & PAGE_MASK,
					.serial = serial);
		else
			ret = scribe_queue_new_event(scribe->queue,
					SCRIBE_EVENT_MEM_OWNED_READ_EXTRA,
					.address = address & PAGE_MASK,
					.serial = serial);
	} else {
		if (write_access)
			ret = scribe_queue_new_event(scribe->queue,
					SCRIBE_EVENT_MEM_OWNED_WRITE,
					.serial = serial);
		else
			ret = scribe_queue_new_event(scribe->queue,
					SCRIBE_EVENT_MEM_OWNED_READ,
					.serial = serial);
	}
	if (ret)
		atomic_dec_return(&page->serial);
	return ret;
}

static int scribe_make_page_owned_replay(struct scribe_ps *scribe,
		struct scribe_page *page, unsigned long address,
		int write_access)
{
	struct scribe_ownership *os;

	os = add_page_ownership(page, scribe, address);
	if (IS_ERR(os))
		return PTR_ERR(os);

	increment_serial(page);
	page->write_access = write_access;

	return 0;
}

#ifdef CONFIG_SCRIBE_MEM_PG_EXPIRE
static int has_ownership_expired(struct scribe_ownership *os, struct timeval *now)
{
	struct timeval *start = &os->timestamp;

	if (!now->tv_sec)
		do_gettimeofday(now);

	if (start->tv_sec < now->tv_sec)
		return 1;

	if (start->tv_usec + EXPIRATION_USEC < now->tv_usec)
		return 1;

	return 0;
}
#else
static inline int has_ownership_expired(struct scribe_ownership *os, struct timeval *now)
{
	return 1;
}
#endif /* CONFIG_SCRIBE_MEM_PG_EXPIRE */

static int serve_shared_req(struct scribe_ps *scribe, int mode)
{
	struct scribe_event *public_event = NULL;
	struct scribe_ownership *os;
	struct scribe_page *page;
	struct timeval now = {0, 0};

retry:
	list_for_each_entry(os, &scribe->mm->shared_req, req_node) {
		if (!(mode & MEM_SYNC_SLEEP) &&
		    !has_ownership_expired(os, &now))
			continue;

		page = os->page;
		spin_unlock(&scribe->mm->req_lock);

		if (!public_event) {
			public_event = scribe_alloc_event(SCRIBE_EVENT_MEM_PUBLIC_READ);
			if (!public_event) {
				spin_lock(&scribe->mm->req_lock);
				return -ENOMEM;
			}
		}

		spin_lock(&page->owners_lock);
		spin_lock(&scribe->mm->req_lock);

		if (!is_owned_by(page, scribe) || list_empty(&os->req_node)) {
			spin_unlock(&page->owners_lock);
			goto retry;
		}

		list_del_init(&os->req_node);

		if (page->read_waiters || page->write_waiters)
			scribe_make_page_public_log(os, public_event,
						    page->write_waiters ? 1 : 0);
		spin_unlock(&page->owners_lock);
		public_event = NULL;
		goto retry;
	}

	if (public_event)
		scribe_free_event(public_event);

	return 0;
}

/*
 * If we enter a weak_owner zone, we have to handle all shared requests
 * and turn the flag weak_owner on atomically
 */
static int scribe_mem_sync_point_record(struct scribe_ps *scribe, int mode)
{
	int ret = 0;

	BUG_ON(!scribe->mm);

	spin_lock(&scribe->mm->req_lock);
	if (mode & MEM_SYNC_IN) {
		ret = serve_shared_req(scribe, mode);

		if (mode & MEM_SYNC_SLEEP) {
			/* The insert point is already created */
		} else {
			scribe_create_insert_point(
					&scribe->mm->weak_owner_events_ip,
					&scribe->queue->stream);
		}

		scribe->mm->weak_owner = mode;
	}
	else /* if (mode & MEM_SYNC_OUT) */ {
		if (mode & MEM_SYNC_SLEEP)
			scribe->mm->weak_owner = MEM_SYNC_IN;
		else {
			scribe_commit_insert_point(
					&scribe->mm->weak_owner_events_ip);
			scribe->mm->weak_owner = 0;
		}
	}
	spin_unlock(&scribe->mm->req_lock);

	return ret;
}

static int scribe_handle_public_event(struct scribe_ps *scribe,
				      struct scribe_event *event);

/* we have to handle all public event pending on the queue */
static int scribe_mem_sync_point_replay(struct scribe_ps *scribe, int mode)
{
	int ret = 0;
	struct scribe_event *event;

	/* only when entering a zone */
	if (mode & MEM_SYNC_IN) {
		for (;;) {
			event = scribe_peek_event(scribe->queue, SCRIBE_WAIT);
			if (IS_ERR(event))
				break;

			if (event->type != SCRIBE_EVENT_MEM_PUBLIC_READ &&
			    event->type != SCRIBE_EVENT_MEM_PUBLIC_WRITE)
				break;

			event = scribe_dequeue_event(scribe->queue,
						     SCRIBE_NO_WAIT);
			ret = scribe_handle_public_event(scribe, event);
		}

		scribe->mm->weak_owner = mode;
	}
	else /* if (mode & MEM_SYNC_OUT) */ {
		if (mode & MEM_SYNC_SLEEP)
			scribe->mm->weak_owner = MEM_SYNC_IN;
		else
			scribe->mm->weak_owner = 0;
	}
	return ret;
}

#ifdef CONFIG_SCRIBE_MEM_DBG
static const char* get_sync_mode_str(int mode)
{
	switch(mode) {
	case 0: return "none";
	case MEM_SYNC_IN: return "MEM_SYNC_IN";
	case MEM_SYNC_IN | MEM_SYNC_SLEEP: return "MEM_SYNC_IN | MEM_SYNC_SLEEP";
	case MEM_SYNC_OUT | MEM_SYNC_SLEEP: return "MEM_SYNC_OUT | MEM_SYNC_SLEEP";
	case MEM_SYNC_OUT: return "MEM_SYNC_OUT";
	}
	return "unknown :(";
}

static void assert_sync_mode(struct scribe_ps *scribe, int expected_mode)
{
	int mode = scribe->mm->weak_owner;
	WARN(mode != expected_mode,
	     "Current mode is %s, but the expected one is %s\n",
	     get_sync_mode_str(mode), get_sync_mode_str(expected_mode));
}
#else
static void assert_sync_mode(struct scribe_ps *scribe, int expected_mode) {}
#endif

void scribe_mem_sync_point(struct scribe_ps *scribe, int mode)
{
	int need_fence;
	int ret, fence_ret = 0;
	if (!should_handle_mm(scribe))
		return;

	if (mode & MEM_SYNC_IN) {
		might_sleep();
		if (mode & MEM_SYNC_SLEEP)
			assert_sync_mode(scribe, MEM_SYNC_IN);
		else
			assert_sync_mode(scribe, 0);
	} else {
		if (mode & MEM_SYNC_SLEEP)
			assert_sync_mode(scribe, MEM_SYNC_IN | MEM_SYNC_SLEEP);
		else
			assert_sync_mode(scribe, MEM_SYNC_IN);
	}

	need_fence = current->scribe == scribe && mode == MEM_SYNC_IN;

	if (need_fence)
		fence_ret = scribe_enter_fenced_region(SCRIBE_REGION_MEM);
	if (is_recording(scribe))
		ret = scribe_mem_sync_point_record(scribe, mode);
	else
		ret = scribe_mem_sync_point_replay(scribe, mode);
	if (need_fence) {
		scribe_leave_fenced_region(SCRIBE_REGION_MEM);
		if (fence_ret)
			ret = fence_ret;
	}

	if (ret < 0)
		scribe_kill(scribe->ctx, ret);
}

void scribe_disable_sync_sleep(void)
{
	struct scribe_ps *scribe = current->scribe;
	if (may_be_scribed(scribe) && scribe->mm)
		scribe->mm->disable_sync_sleep = 1;
}

void scribe_enable_sync_sleep(void)
{
	struct scribe_ps *scribe = current->scribe;
	if (may_be_scribed(scribe) && scribe->mm)
		scribe->mm->disable_sync_sleep = 0;
}

void scribe_mem_schedule_in(struct scribe_ps *scribe)
{
	if (!is_recording(scribe))
		return;

	if (!scribe->mm)
		return;

	if (scribe->p->state != TASK_INTERRUPTIBLE &&
	    scribe->p->state != TASK_UNINTERRUPTIBLE)
		return;

	if (scribe->mm->disable_sync_sleep)
		return;

	if (!scribe->mm->weak_owner)
		return;

	scribe_mem_sync_point(scribe, MEM_SYNC_IN | MEM_SYNC_SLEEP);
}

void scribe_mem_schedule_out(struct scribe_ps *scribe)
{
	if (!is_recording(scribe))
		return;

	if (!scribe->mm)
		return;

	if (scribe->mm->disable_sync_sleep)
		return;

	if (scribe->mm->weak_owner & MEM_SYNC_SLEEP)
		scribe_mem_sync_point(scribe, MEM_SYNC_OUT | MEM_SYNC_SLEEP);
}

static int scribe_handle_public_event(struct scribe_ps *scribe,
				      struct scribe_event *event)
{
	struct mm_struct *mm = scribe->p->mm;
	struct vm_area_struct *vma;
	struct scribe_page_key page_key;
	struct scribe_page *page;
	struct scribe_ownership *os;

	struct scribe_event_mem_public_read *public_read_event;
	struct scribe_event_mem_public_write *public_write_event;
	unsigned long page_addr;
	int rw_flag;
	int ret;

	if (event->type == SCRIBE_EVENT_MEM_PUBLIC_WRITE) {
		public_write_event = (void *)event;
		page_addr = public_write_event->address;
		rw_flag = 1;
	} else if (event->type == SCRIBE_EVENT_MEM_PUBLIC_READ) {
		public_read_event = (void *)event;
		page_addr = public_read_event->address;
		rw_flag = 0;
	} else
		BUG();

	scribe_free_event(event);

	down_read(&mm->mmap_sem);

	vma = find_vma(mm, page_addr);
	if (!vma) {
		up_read(&mm->mmap_sem);
		return -EINVAL;
	}
	get_page_key(vma, page_addr, &page_key);
	page = get_scribe_page(scribe->ctx->mm_ctx, &page_key);
	if (IS_ERR(page)) {
		up_read(&mm->mmap_sem);
		return -ENOMEM;
	}

	spin_lock(&page->owners_lock);
	os = find_ownership(page, scribe);
	if (likely(os)) {
		scribe_make_page_public(os, rw_flag);
		ret = 0;
	} else {
		scribe_diverge(scribe, SCRIBE_EVENT_DIVERGE_MEM_NOT_OWNED);
		ret = -EDIVERGE;
	}
	spin_unlock(&page->owners_lock);

	up_read(&mm->mmap_sem);
	return ret;
}

static inline int is_vma_scribed(struct scribe_ps *scribe, struct vm_area_struct *vma)
{
	unsigned long vm_flags = vma->vm_flags;

	/* every shared memory is scribed
	 * FIXME check for the obj ref count > 2 */
	if (vm_flags & VM_SHARED)
		return 1;

	/* readonly ? */
	if (!(vm_flags & VM_WRITE))
		return 0;

	/* threaded ? -- note that using is_sharing_mm() is incorrect, because
	 * the value during replay must match the one during logging
	 */
	if (scribe->mm->is_alone)
		return 0;

	return 1;
}

/* Returns 1 if we went alone, 0 if not, -ENOMEM if the allocation failed */
static int check_for_aloneness(struct scribe_ps *scribe)
{
	if (is_sharing_mm(scribe->p->mm))
		return 0;

	if (scribe->mm->is_alone)
		return 0;

	/* It's time to make things official */
	scribe->mm->is_alone = 1;

	scribe_page_release_ownership(scribe, ONLY_ANONYMOUS_PAGES);
	remove_pages_of(scribe->ctx->mm_ctx, scribe->p->mm);

	if (scribe_queue_new_event(scribe->queue,
				   SCRIBE_EVENT_MEM_ALONE))
		return -ENOMEM;

	return 1;
}

static inline int is_ownership_stealable(struct scribe_ps *scribe,
					 struct scribe_ownership *os,
					 struct timeval *now)
{
	int weak_owner;

	if (os->owner == scribe)
		return 1;

	weak_owner = os->owner->mm->weak_owner;
	if (!weak_owner)
		return 0;

	if (weak_owner & MEM_SYNC_SLEEP)
		return 1;

	if (has_ownership_expired(os, now))
		return 1;

	return 0;
}

static void try_make_public(struct scribe_ps *scribe, struct scribe_page *page,
			    int write_access)
{
#define MAX_EVENT_ALLOC_NUM 32
	struct scribe_ownership *os, *tmp;
	struct scribe_event *event[MAX_EVENT_ALLOC_NUM];
	struct scribe_ps *owner;
	struct timeval now = { 0, 0 };
	int i;

	/*
	 * Negative values of num_event means we want to allocate
	 * abs(num_event) events.
	 */
	int num_event = 0;

retry:
	if (!write_access && !page->write_access)
		goto out;

	list_for_each_entry_safe(os, tmp, &page->owners, node) {
		owner = os->owner;

		spin_lock(&owner->mm->req_lock);

		if (is_ownership_stealable(scribe, os, &now) &&
		    --num_event >= 0) {
			if (!list_empty(&os->req_node))
				list_del_init(&os->req_node);

			scribe_make_page_public_log(os,
						event[num_event], write_access);
			/*
			 * If num_event is going negative, it will mean that
			 * some allocation is needed
			 */
		} else if (list_empty(&os->req_node))
			list_add(&os->req_node, &owner->mm->shared_req);
		spin_unlock(&owner->mm->req_lock);
	}

out:
	if (num_event < 0) {
		spin_unlock(&page->owners_lock);
		num_event = min(-num_event, MAX_EVENT_ALLOC_NUM);
		for (i = 0; i < num_event; i++) {
			event[i] = scribe_alloc_event(
					SCRIBE_EVENT_MEM_PUBLIC_READ);
			BUG_ON(!event[i]);
		}
		spin_lock(&page->owners_lock);
		goto retry;
	}
	if (num_event) {
		/* allocation ? */

		/* too many events were allocated, leaving */
		for (i = 0; i < num_event; i++) {
			scribe_free_event(event[i]);
		}
	}
}

static void read_write_accounting(struct scribe_page *page, struct scribe_ps *scribe,
				  int *write_access)
{
	struct scribe_ownership *os;

	if (page->read_then_write >= 0) {
		os = find_ownership(page, scribe);
		if (!os)
			return;

		if (os->start_serial + 1 == atomic_read(&page->serial)) {
			/* We are faulting again on the same page */
			if (++page->read_then_write >= READ_THEN_WRITE_MIN_THRESHOLD) {
				page->read_then_write = -READ_THEN_WRITE_MAX_THRESHOLD;
			}
		}
		else
			page->read_then_write  = 0;
	}
	else {
		page->read_then_write++;

		/* force write_access ! */
		*write_access = 1;
	}
}

static int scribe_page_access_record(struct scribe_ps *scribe,
		struct mm_struct *mm, struct vm_area_struct *vma,
		struct scribe_page *page, unsigned long address,
		int write_access)
{
	struct scribe_ownership *os;
	int ret;

	spin_lock(&page->owners_lock);

	if (page_access_ok(scribe, page, write_access)) {
		spin_unlock(&page->owners_lock);
		return 0;
	}

	/*
	 * The sync point must be entered before writing any public events in
	 * our queue (we might send ourselves some public events in
	 * try_make_public() to go from a readonly access to a write access.
	 */
	spin_unlock(&page->owners_lock);
	scribe_mem_sync_point(scribe, MEM_SYNC_IN);
	spin_lock(&page->owners_lock);

	read_write_accounting(page, scribe, &write_access);
	/*
	 * We have to increment the waiter counter before trying to make
	 * public: during make_public, allocation may be needed, and the owner
	 * lock will be dropped. As a result, another task may process some
	 * already pending request. It will need to know if someone is waiting
	 * on the page.
	 */
	inc_waiters(page, write_access);
	try_make_public(scribe, page, write_access);

	spin_unlock(&page->owners_lock);

	up_read(&mm->mmap_sem);

	page_down(page, write_access);

	scribe_mem_sync_point(scribe, MEM_SYNC_OUT);
	down_read(&mm->mmap_sem);

	ret = check_for_aloneness(scribe);
	if (ret) {
		if (!is_vma_scribed(scribe, vma))
			ret = -EAGAIN;
		if (ret < 0) {
			spin_lock(&page->owners_lock);
			dec_waiters(page, write_access);
			page_up(page, write_access);
			spin_unlock(&page->owners_lock);
			return ret;
		}
	}

	spin_lock(&page->owners_lock);
	dec_waiters(page, write_access);

	os = add_page_ownership(page, scribe, address);
	if (IS_ERR(os)) {
		page_up(page, write_access);
		spin_unlock(&page->owners_lock);
		return PTR_ERR(os);
	}

	/* if a pending ownership is found, we'll add a shared request */
	if (page->write_waiters || (page->read_waiters && write_access)) {
		spin_lock(&scribe->mm->req_lock);
		if (list_empty(&os->req_node))
			list_add(&os->req_node, &scribe->mm->shared_req);
		spin_unlock(&scribe->mm->req_lock);
	}

	spin_unlock(&page->owners_lock);

	return scribe_make_page_owned_log(scribe, page, address, write_access);
}

static int serial_match(struct scribe_ps *scribe,
			struct scribe_page *page, int serial)
{
	if (atomic_read(&page->serial) >= serial)
		return 1;

	if (is_scribe_context_dead(scribe->ctx))
		return 1;

	return 0;
}

static int get_owned_event_info(struct scribe_ps *scribe,
				struct scribe_event *event,
				int *rw_flag, unsigned long *page_addr,
				unsigned int *serial)
{
	struct scribe_event_mem_owned_read *owned_read_event;
	struct scribe_event_mem_owned_write *owned_write_event;
	struct scribe_event_mem_owned_read_extra *owned_read_event_extra;
	struct scribe_event_mem_owned_write_extra *owned_write_event_extra;

	if (should_scribe_mem_extra(scribe)) {
		switch (event->type) {
		case SCRIBE_EVENT_MEM_OWNED_WRITE_EXTRA:
			owned_write_event_extra = (void *)event;
			*page_addr = owned_write_event_extra->address;
			*serial = owned_write_event_extra->serial;
			*rw_flag = 1;
			return 0;
		case SCRIBE_EVENT_MEM_OWNED_READ_EXTRA:
			owned_read_event_extra = (void *)event;
			*page_addr = owned_read_event_extra->address;
			*serial = owned_read_event_extra->serial;
			*rw_flag = 0;
			return 0;
		}
	} else {
		switch (event->type) {
		case SCRIBE_EVENT_MEM_OWNED_WRITE:
			owned_write_event = (void *)event;
			*serial = owned_write_event->serial;
			*rw_flag = 1;
			return 0;
		case SCRIBE_EVENT_MEM_OWNED_READ:
			owned_read_event = (void *)event;
			*serial = owned_read_event->serial;
			*rw_flag = 0;
			return 0;
		}
	}

	return -ENODATA;
}

static int scribe_page_access_replay(struct scribe_ps *scribe,
		struct mm_struct *mm, struct vm_area_struct *vma,
		struct scribe_page *page, unsigned long address,
		int write_access)
{
	struct scribe_event *event;
	unsigned long page_addr;
	int rw_flag;
	int serial;

	spin_lock(&page->owners_lock);
	if (page_access_ok(scribe, page, write_access)) {
		spin_unlock(&page->owners_lock);
		return 0;
	}
	spin_unlock(&page->owners_lock);

	up_read(&mm->mmap_sem);

	scribe_mem_sync_point(scribe, MEM_SYNC_IN);
	scribe_mem_sync_point(scribe, MEM_SYNC_OUT);

	event = scribe_dequeue_event(scribe->queue, SCRIBE_WAIT);
	if (IS_ERR(event)) {
		scribe_kill(scribe->ctx, PTR_ERR(event));
		down_read(&mm->mmap_sem);
		return PTR_ERR(event);
	}

	page_addr = address & PAGE_MASK;
	if (!get_owned_event_info(scribe, event,
				  &rw_flag, &page_addr, &serial)) {
		scribe_free_event(event);

		if (unlikely(page_addr != (address & PAGE_MASK) ||
			     (!rw_flag && write_access))) {
			scribe_diverge(scribe, SCRIBE_EVENT_DIVERGE_MEM_OWNED,
				       .address = address & PAGE_MASK,
				       .write_access = write_access);
		}

		wait_event(page->serial_wait,
			   serial_match(scribe, page, serial));
		down_read(&mm->mmap_sem);

		spin_lock(&page->owners_lock);
		scribe_make_page_owned_replay(scribe, page, address, rw_flag);
		spin_unlock(&page->owners_lock);
		return 0;
	}

	if (event->type == SCRIBE_EVENT_MEM_ALONE) {
		scribe_free_event(event);

		wait_event(mm->scribe_wait, !is_sharing_mm(scribe->p->mm));
		scribe->mm->is_alone = 1;

		down_read(&mm->mmap_sem);
		scribe_page_release_ownership(scribe, ONLY_ANONYMOUS_PAGES);
		remove_pages_of(scribe->ctx->mm_ctx, mm);
		return -EAGAIN;
	}

	scribe_free_event(event);
	scribe_diverge(scribe, SCRIBE_EVENT_DIVERGE_MEM_OWNED,
		       .address = address & PAGE_MASK,
		       .write_access = write_access);
	down_read(&mm->mmap_sem);
	return -EDIVERGE;
}

static inline int scribe_page_access(struct scribe_ps *scribe,
		struct mm_struct *mm, struct vm_area_struct *vma,
		struct scribe_page *page, unsigned long address,
		int write_access)
{
	int ret;

	if (is_recording(scribe))
		ret = scribe_page_access_record(scribe, mm, vma, page,
						address, write_access);
	else
		ret = scribe_page_access_replay(scribe, mm, vma, page,
						address, write_access);

#ifdef CONFIG_SCRIBE_MEM_DBG
	if (!ret) {
		spin_lock(&page->owners_lock);
		BUG_ON(!is_owned_by(page, scribe));
		spin_unlock(&page->owners_lock);
	}
#endif
	return ret;
}

static int own_pte_alloc_map(struct scribe_ps *scribe, struct mm_struct *mm,
			     pte_t **pown_pte, pmd_t **pown_pmd,
			     unsigned long address)
{
	pgd_t *own_pgd;
	pud_t *own_pud;
	pmd_t *own_pmd;
	pte_t *own_pte;

	own_pgd = scribe->mm->own_pgd + pgd_index(address);
	own_pud = pud_alloc(mm, own_pgd, address);
	if (!own_pud)
		return -ENOMEM;
	own_pmd = pmd_alloc(mm, own_pud, address);
	if (!own_pmd)
		return -ENOMEM;
	own_pte = pte_alloc_map(mm, own_pmd, address);
	if (!own_pte)
		return -ENOMEM;
	*pown_pte = own_pte;
	*pown_pmd = own_pmd;
	return 0;
}

int do_scribe_page(struct scribe_ps *scribe, struct mm_struct *mm,
		   struct vm_area_struct *vma, unsigned long address,
		   pte_t *pte, pmd_t *pmd, unsigned int flags)
{
	struct scribe_page_key page_key;
	struct scribe_page *page;
	pmd_t *own_pmd;
	pte_t *own_pte;
	spinlock_t *ptl;
	pte_t entry;
	int ret;

	pte_unmap(pte);

	if (unlikely(!should_handle_mm(scribe) || scribe->p->mm != mm))
		return VM_FAULT_SCRIBE;

	WARN(scribe->mm->weak_owner, "Access in a weak owner zone\n");

	if (own_pte_alloc_map(scribe, mm, &own_pte, &own_pmd, address))
		return VM_FAULT_OOM;
	pte_unmap(own_pte);

retry:
	if (!is_vma_scribed(scribe, vma)) {
		page = NULL;
		goto set_pte;
	}

	get_page_key(vma, address, &page_key);
	page = get_scribe_page(scribe->ctx->mm_ctx, &page_key);
	if (IS_ERR(page))
		return VM_FAULT_OOM;

	ret = scribe_page_access(scribe, mm, vma, page, address,
				 flags & FAULT_FLAG_WRITE);
	if (ret) {
		if (ret == -EAGAIN)
			goto retry;
		if (ret == -ENOMEM)
			return VM_FAULT_OOM;
		page = NULL;
	}

set_pte:
	/* pte and own_pte are both protected with the same spinlock. */
	pte = pte_offset_map_lock(mm, pmd, address, &ptl);
	own_pte = pte_offset_map_nested2(own_pmd, address);

	entry = *pte;
	if (flags & FAULT_FLAG_WRITE) {
		entry = pte_mkdirty(entry);
		set_pte_at(mm, address, pte, entry);
	}
	/*
	 * In case of a READ then WRITE access, the shadow pte will be marked
	 * as dirty, but not the real one. It will get propagated in
	 * update_private_pte_locked().
	 */
	if (page && !page->write_access)
		entry = pte_wrprotect(entry);
	set_pte_at(mm, address, own_pte, entry);
	update_mmu_cache(vma, address, own_pte);

	pte_unmap_nested2(own_pte);
	pte_unmap_unlock(pte, ptl);
	return VM_FAULT_SCRIBE;
}

/******************************************************************************/

void scribe_add_vma(struct vm_area_struct *vma)
{
	struct scribe_ps *scribe = get_scribe_from_mm(vma->vm_mm);
	struct scribe_obj_ref *ref;
	void *object;

	if (!should_handle_mm(scribe))
		return;

	if (!(vma->vm_flags & VM_SHARED))
		return;

	/*
	 * When a new shared memory region appears, we want to get a reference
	 * on the file.
	 */

	object = vma->vm_file->f_dentry->d_inode;
	ref = get_obj_ref(scribe->ctx->mm_ctx, object);

	WARN(IS_ERR(ref), "Cannot get_obj_ref()\n");
}

void scribe_remove_vma(struct vm_area_struct *vma)
{
	struct mm_struct *mm = vma->vm_mm;
	struct scribe_ps *scribe = get_scribe_from_mm(mm);

	if (!should_handle_mm(scribe))
		return;

	if (vma->vm_flags & VM_SHARED) {
		put_obj_ref(scribe->ctx->mm_ctx,
			    vma->vm_file->f_dentry->d_inode);
	}

	/*
	 * We don't want to remove any pages. It may suck some memory, but
	 * since we don't take any synchronization lock on a page fault,
	 * we cannot reset the serial number of the pages to free some memory.
	 * We'll be able to free some memory when we'll be alone
	 */
}
