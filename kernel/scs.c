// SPDX-License-Identifier: GPL-2.0
/*
 * Shadow Call Stack support.
 *
 * Copyright (C) 2019 Google LLC
 */

#include <linux/cpuhotplug.h>
#include <linux/mm.h>
#include <linux/mmzone.h>
#include <linux/scs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/vmstat.h>
#include <asm/scs.h>

static inline void *__scs_base(struct task_struct *tsk)
{
	return (void *)((uintptr_t)task_scs(tsk) & ~(SCS_SIZE - 1));
}

#ifdef CONFIG_SHADOW_CALL_STACK_VMAP

/* Keep a cache of shadow stacks */
#define SCS_CACHE_SIZE 2
static DEFINE_PER_CPU(void *, scs_cache[SCS_CACHE_SIZE]);

static void *scs_alloc(int node)
{
	int i;

	for (i = 0; i < SCS_CACHE_SIZE; i++) {
		void *s;

		s = this_cpu_xchg(scs_cache[i], NULL);
		if (s) {
			memset(s, 0, SCS_SIZE);
			return s;
		}
	}

	/*
	 * We allocate a full page for the shadow stack, which should be
	 * more than we need. Check the assumption nevertheless.
	 */
	BUILD_BUG_ON(SCS_SIZE > PAGE_SIZE);

	return __vmalloc_node_range(PAGE_SIZE, SCS_SIZE,
				    VMALLOC_START, VMALLOC_END,
				    GFP_SCS, PAGE_KERNEL, 0,
				    node, __builtin_return_address(0));
}

static void scs_free(void *s)
{
	int i;

	for (i = 0; i < SCS_CACHE_SIZE; i++)
		if (this_cpu_cmpxchg(scs_cache[i], 0, s) == 0)
			return;

	vfree_atomic(s);
}

static struct page *__scs_page(struct task_struct *tsk)
{
	return vmalloc_to_page(__scs_base(tsk));
}

static int scs_cleanup(unsigned int cpu)
{
	int i;
	void **cache = per_cpu_ptr(scs_cache, cpu);

	for (i = 0; i < SCS_CACHE_SIZE; i++) {
		vfree(cache[i]);
		cache[i] = NULL;
	}

	return 0;
}

void __init scs_init(void)
{
	cpuhp_setup_state(CPUHP_BP_PREPARE_DYN, "scs:scs_cache", NULL,
		scs_cleanup);
}

#else /* !CONFIG_SHADOW_CALL_STACK_VMAP */

static struct kmem_cache *scs_cache;

static inline void *scs_alloc(int node)
{
	return kmem_cache_alloc_node(scs_cache, GFP_SCS, node);
}

static inline void scs_free(void *s)
{
	kmem_cache_free(scs_cache, s);
}

static struct page *__scs_page(struct task_struct *tsk)
{
	return virt_to_page(__scs_base(tsk));
}

void __init scs_init(void)
{
	scs_cache = kmem_cache_create("scs_cache", SCS_SIZE, SCS_SIZE,
				0, NULL);
	WARN_ON(!scs_cache);
}

#endif /* CONFIG_SHADOW_CALL_STACK_VMAP */

static inline unsigned long *scs_magic(struct task_struct *tsk)
{
	return (unsigned long *)(__scs_base(tsk) + SCS_SIZE) - 1;
}

static inline void scs_set_magic(struct task_struct *tsk)
{
	*scs_magic(tsk) = SCS_END_MAGIC;
}

void scs_task_reset(struct task_struct *tsk)
{
	task_set_scs(tsk, __scs_base(tsk));
}

static void scs_account(struct task_struct *tsk, int account)
{
	mod_zone_page_state(page_zone(__scs_page(tsk)), NR_KERNEL_SCS_BYTES,
		account * SCS_SIZE);
}

int scs_prepare(struct task_struct *tsk, int node)
{
	void *s;

	s = scs_alloc(node);
	if (!s)
		return -ENOMEM;

	task_set_scs(tsk, s);
	scs_set_magic(tsk);
	scs_account(tsk, 1);

	return 0;
}

bool scs_corrupted(struct task_struct *tsk)
{
	return *scs_magic(tsk) != SCS_END_MAGIC;
}

void scs_release(struct task_struct *tsk)
{
	void *s;

	s = __scs_base(tsk);
	if (!s)
		return;

	WARN_ON(scs_corrupted(tsk));

	scs_account(tsk, -1);
	task_set_scs(tsk, NULL);
	scs_free(s);
}
