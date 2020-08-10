/*
 * Copyright (c) 2011-2019 Apple Inc. All rights reserved.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_START@
 *
 * This file contains Original Code and/or Modifications of Original Code
 * as defined in and that are subject to the Apple Public Source License
 * Version 2.0 (the 'License'). You may not use this file except in
 * compliance with the License. The rights granted to you under the License
 * may not be used to create, or enable the creation or redistribution of,
 * unlawful or unlicensed copies of an Apple operating system, or to
 * circumvent, violate, or enable the circumvention or violation of, any
 * terms of an Apple operating system software license agreement.
 *
 * Please obtain a copy of the License at
 * http://www.opensource.apple.com/apsl/ and read it before using this file.
 *
 * The Original Code and all software distributed under the License are
 * distributed on an 'AS IS' basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE, QUIET ENJOYMENT OR NON-INFRINGEMENT.
 * Please see the License for the specific language governing rights and
 * limitations under the License.
 *
 * @APPLE_OSREFERENCE_LICENSE_HEADER_END@
 */
#include <string.h>
#include <mach_assert.h>
#include <mach_ldebug.h>

#include <mach/shared_region.h>
#include <mach/vm_param.h>
#include <mach/vm_prot.h>
#include <mach/vm_map.h>
#include <mach/machine/vm_param.h>
#include <mach/machine/vm_types.h>

#include <mach/boolean.h>
#include <kern/bits.h>
#include <kern/thread.h>
#include <kern/sched.h>
#include <kern/zalloc.h>
#include <kern/kalloc.h>
#include <kern/ledger.h>
#include <kern/spl.h>
#include <kern/trustcache.h>

#include <os/overflow.h>

#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#include <vm/vm_protos.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/cpm.h>

#include <libkern/img4/interface.h>
#include <libkern/section_keywords.h>

#include <machine/atomic.h>
#include <machine/thread.h>
#include <machine/lowglobals.h>

#include <arm/caches_internal.h>
#include <arm/cpu_data.h>
#include <arm/cpu_data_internal.h>
#include <arm/cpu_capabilities.h>
#include <arm/cpu_number.h>
#include <arm/machine_cpu.h>
#include <arm/misc_protos.h>
#include <arm/trap.h>

#if     (__ARM_VMSA__ > 7)
#include <arm64/proc_reg.h>
#include <pexpert/arm64/boot.h>
#if CONFIG_PGTRACE
#include <stdint.h>
#include <arm64/pgtrace.h>
#if CONFIG_PGTRACE_NONKEXT
#include <arm64/pgtrace_decoder.h>
#endif // CONFIG_PGTRACE_NONKEXT
#endif
#endif

#include <pexpert/device_tree.h>

#include <san/kasan.h>
#include <sys/cdefs.h>

#if defined(HAS_APPLE_PAC)
#include <ptrauth.h>
#endif

#define PMAP_TT_L0_LEVEL        0x0
#define PMAP_TT_L1_LEVEL        0x1
#define PMAP_TT_L2_LEVEL        0x2
#define PMAP_TT_L3_LEVEL        0x3
#if (__ARM_VMSA__ == 7)
#define PMAP_TT_MAX_LEVEL       PMAP_TT_L2_LEVEL
#else
#define PMAP_TT_MAX_LEVEL       PMAP_TT_L3_LEVEL
#endif
#define PMAP_TT_LEAF_LEVEL      PMAP_TT_MAX_LEVEL
#define PMAP_TT_TWIG_LEVEL      (PMAP_TT_MAX_LEVEL - 1)

static bool alloc_asid(pmap_t pmap);
static void free_asid(pmap_t pmap);
static void flush_mmu_tlb_region_asid_async(vm_offset_t va, unsigned length, pmap_t pmap);
static void flush_mmu_tlb_tte_asid_async(vm_offset_t va, pmap_t pmap);
static void flush_mmu_tlb_full_asid_async(pmap_t pmap);
static pt_entry_t wimg_to_pte(unsigned int wimg);

struct page_table_ops {
	bool (*alloc_id)(pmap_t pmap);
	void (*free_id)(pmap_t pmap);
	void (*flush_tlb_region_async)(vm_offset_t va, unsigned length, pmap_t pmap);
	void (*flush_tlb_tte_async)(vm_offset_t va, pmap_t pmap);
	void (*flush_tlb_async)(pmap_t pmap);
	pt_entry_t (*wimg_to_pte)(unsigned int wimg);
};

static const struct page_table_ops native_pt_ops =
{
	.alloc_id = alloc_asid,
	.free_id = free_asid,
	.flush_tlb_region_async = flush_mmu_tlb_region_asid_async,
	.flush_tlb_tte_async = flush_mmu_tlb_tte_asid_async,
	.flush_tlb_async = flush_mmu_tlb_full_asid_async,
	.wimg_to_pte = wimg_to_pte,
};

#if (__ARM_VMSA__ > 7)
const struct page_table_level_info pmap_table_level_info_16k[] =
{
	[0] = {
		.size       = ARM_16K_TT_L0_SIZE,
		.offmask    = ARM_16K_TT_L0_OFFMASK,
		.shift      = ARM_16K_TT_L0_SHIFT,
		.index_mask = ARM_16K_TT_L0_INDEX_MASK,
		.valid_mask = ARM_TTE_VALID,
		.type_mask  = ARM_TTE_TYPE_MASK,
		.type_block = ARM_TTE_TYPE_BLOCK
	},
	[1] = {
		.size       = ARM_16K_TT_L1_SIZE,
		.offmask    = ARM_16K_TT_L1_OFFMASK,
		.shift      = ARM_16K_TT_L1_SHIFT,
		.index_mask = ARM_16K_TT_L1_INDEX_MASK,
		.valid_mask = ARM_TTE_VALID,
		.type_mask  = ARM_TTE_TYPE_MASK,
		.type_block = ARM_TTE_TYPE_BLOCK
	},
	[2] = {
		.size       = ARM_16K_TT_L2_SIZE,
		.offmask    = ARM_16K_TT_L2_OFFMASK,
		.shift      = ARM_16K_TT_L2_SHIFT,
		.index_mask = ARM_16K_TT_L2_INDEX_MASK,
		.valid_mask = ARM_TTE_VALID,
		.type_mask  = ARM_TTE_TYPE_MASK,
		.type_block = ARM_TTE_TYPE_BLOCK
	},
	[3] = {
		.size       = ARM_16K_TT_L3_SIZE,
		.offmask    = ARM_16K_TT_L3_OFFMASK,
		.shift      = ARM_16K_TT_L3_SHIFT,
		.index_mask = ARM_16K_TT_L3_INDEX_MASK,
		.valid_mask = ARM_PTE_TYPE_VALID,
		.type_mask  = ARM_PTE_TYPE_MASK,
		.type_block = ARM_TTE_TYPE_L3BLOCK
	}
};

const struct page_table_level_info pmap_table_level_info_4k[] =
{
	[0] = {
		.size       = ARM_4K_TT_L0_SIZE,
		.offmask    = ARM_4K_TT_L0_OFFMASK,
		.shift      = ARM_4K_TT_L0_SHIFT,
		.index_mask = ARM_4K_TT_L0_INDEX_MASK,
		.valid_mask = ARM_TTE_VALID,
		.type_mask  = ARM_TTE_TYPE_MASK,
		.type_block = ARM_TTE_TYPE_BLOCK
	},
	[1] = {
		.size       = ARM_4K_TT_L1_SIZE,
		.offmask    = ARM_4K_TT_L1_OFFMASK,
		.shift      = ARM_4K_TT_L1_SHIFT,
		.index_mask = ARM_4K_TT_L1_INDEX_MASK,
		.valid_mask = ARM_TTE_VALID,
		.type_mask  = ARM_TTE_TYPE_MASK,
		.type_block = ARM_TTE_TYPE_BLOCK
	},
	[2] = {
		.size       = ARM_4K_TT_L2_SIZE,
		.offmask    = ARM_4K_TT_L2_OFFMASK,
		.shift      = ARM_4K_TT_L2_SHIFT,
		.index_mask = ARM_4K_TT_L2_INDEX_MASK,
		.valid_mask = ARM_TTE_VALID,
		.type_mask  = ARM_TTE_TYPE_MASK,
		.type_block = ARM_TTE_TYPE_BLOCK
	},
	[3] = {
		.size       = ARM_4K_TT_L3_SIZE,
		.offmask    = ARM_4K_TT_L3_OFFMASK,
		.shift      = ARM_4K_TT_L3_SHIFT,
		.index_mask = ARM_4K_TT_L3_INDEX_MASK,
		.valid_mask = ARM_PTE_TYPE_VALID,
		.type_mask  = ARM_PTE_TYPE_MASK,
		.type_block = ARM_TTE_TYPE_L3BLOCK
	}
};

struct page_table_attr {
	const struct page_table_level_info * const pta_level_info;
	const struct page_table_ops * const pta_ops;
	const uintptr_t ap_ro;
	const uintptr_t ap_rw;
	const uintptr_t ap_rona;
	const uintptr_t ap_rwna;
	const uintptr_t ap_xn;
	const uintptr_t ap_x;
	const unsigned int pta_root_level;
	const unsigned int pta_max_level;
};

const struct page_table_attr pmap_pt_attr_4k = {
	.pta_level_info = pmap_table_level_info_4k,
	.pta_root_level = PMAP_TT_L1_LEVEL,
	.pta_max_level  = PMAP_TT_L3_LEVEL,
	.pta_ops = &native_pt_ops,
	.ap_ro = ARM_PTE_AP(AP_RORO),
	.ap_rw = ARM_PTE_AP(AP_RWRW),
	.ap_rona = ARM_PTE_AP(AP_RONA),
	.ap_rwna = ARM_PTE_AP(AP_RWNA),
	.ap_xn = ARM_PTE_PNX | ARM_PTE_NX,
	.ap_x = ARM_PTE_PNX,
};

const struct page_table_attr pmap_pt_attr_16k = {
	.pta_level_info = pmap_table_level_info_16k,
	.pta_root_level = PMAP_TT_L1_LEVEL,
	.pta_max_level  = PMAP_TT_L3_LEVEL,
	.pta_ops = &native_pt_ops,
	.ap_ro = ARM_PTE_AP(AP_RORO),
	.ap_rw = ARM_PTE_AP(AP_RWRW),
	.ap_rona = ARM_PTE_AP(AP_RONA),
	.ap_rwna = ARM_PTE_AP(AP_RWNA),
	.ap_xn = ARM_PTE_PNX | ARM_PTE_NX,
	.ap_x = ARM_PTE_PNX,
};

#if __ARM_16K_PG__
const struct page_table_attr * const native_pt_attr = &pmap_pt_attr_16k;
#else /* !__ARM_16K_PG__ */
const struct page_table_attr * const native_pt_attr = &pmap_pt_attr_4k;
#endif /* !__ARM_16K_PG__ */


#else /* (__ARM_VMSA__ > 7) */
/*
 * We don't support pmap parameterization for VMSA7, so use an opaque
 * page_table_attr structure.
 */
const struct page_table_attr * const native_pt_attr = NULL;
#endif /* (__ARM_VMSA__ > 7) */

typedef struct page_table_attr pt_attr_t;

/* Macro for getting pmap attributes; not a function for const propagation. */
#if ARM_PARAMETERIZED_PMAP
/* The page table attributes are linked to the pmap */
#define pmap_get_pt_attr(pmap) ((pmap)->pmap_pt_attr)
#define pmap_get_pt_ops(pmap) ((pmap)->pmap_pt_attr->pta_ops)
#else /* !ARM_PARAMETERIZED_PMAP */
/* The page table attributes are fixed (to allow for const propagation) */
#define pmap_get_pt_attr(pmap) (native_pt_attr)
#define pmap_get_pt_ops(pmap) (&native_pt_ops)
#endif /* !ARM_PARAMETERIZED_PMAP */

#if (__ARM_VMSA__ > 7)
static inline uint64_t
pt_attr_ln_size(const pt_attr_t * const pt_attr, unsigned int level)
{
	return pt_attr->pta_level_info[level].size;
}

__unused static inline uint64_t
pt_attr_ln_shift(const pt_attr_t * const pt_attr, unsigned int level)
{
	return pt_attr->pta_level_info[level].shift;
}

__unused static inline uint64_t
pt_attr_ln_offmask(const pt_attr_t * const pt_attr, unsigned int level)
{
	return pt_attr->pta_level_info[level].offmask;
}

static inline unsigned int
pt_attr_twig_level(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_max_level - 1;
}

static inline unsigned int
pt_attr_root_level(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_root_level;
}

static __unused inline uint64_t
pt_attr_leaf_size(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_level_info[pt_attr->pta_max_level].size;
}

static __unused inline uint64_t
pt_attr_leaf_offmask(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_level_info[pt_attr->pta_max_level].offmask;
}

static inline uint64_t
pt_attr_leaf_shift(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_level_info[pt_attr->pta_max_level].shift;
}

static __unused inline uint64_t
pt_attr_leaf_index_mask(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_level_info[pt_attr->pta_max_level].index_mask;
}

static inline uint64_t
pt_attr_twig_size(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_level_info[pt_attr->pta_max_level - 1].size;
}

static inline uint64_t
pt_attr_twig_offmask(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_level_info[pt_attr->pta_max_level - 1].offmask;
}

static inline uint64_t
pt_attr_twig_shift(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_level_info[pt_attr->pta_max_level - 1].shift;
}

static __unused inline uint64_t
pt_attr_twig_index_mask(const pt_attr_t * const pt_attr)
{
	return pt_attr->pta_level_info[pt_attr->pta_max_level - 1].index_mask;
}

static inline uint64_t
pt_attr_leaf_table_size(const pt_attr_t * const pt_attr)
{
	return pt_attr_twig_size(pt_attr);
}

static inline uint64_t
pt_attr_leaf_table_offmask(const pt_attr_t * const pt_attr)
{
	return pt_attr_twig_offmask(pt_attr);
}

static inline uintptr_t
pt_attr_leaf_rw(const pt_attr_t * const pt_attr)
{
	return pt_attr->ap_rw;
}

static inline uintptr_t
pt_attr_leaf_ro(const pt_attr_t * const pt_attr)
{
	return pt_attr->ap_ro;
}

static inline uintptr_t
pt_attr_leaf_rona(const pt_attr_t * const pt_attr)
{
	return pt_attr->ap_rona;
}

static inline uintptr_t
pt_attr_leaf_rwna(const pt_attr_t * const pt_attr)
{
	return pt_attr->ap_rwna;
}

static inline uintptr_t
pt_attr_leaf_xn(const pt_attr_t * const pt_attr)
{
	return pt_attr->ap_xn;
}

static inline uintptr_t
pt_attr_leaf_x(const pt_attr_t * const pt_attr)
{
	return pt_attr->ap_x;
}

#else /* (__ARM_VMSA__ > 7) */

static inline unsigned int
pt_attr_twig_level(__unused const pt_attr_t * const pt_attr)
{
	return PMAP_TT_L1_LEVEL;
}

static inline uint64_t
pt_attr_twig_size(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_TWIG_SIZE;
}

static inline uint64_t
pt_attr_twig_offmask(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_TWIG_OFFMASK;
}

static inline uint64_t
pt_attr_twig_shift(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_TWIG_SHIFT;
}

static __unused inline uint64_t
pt_attr_twig_index_mask(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_TWIG_INDEX_MASK;
}

__unused static inline uint64_t
pt_attr_leaf_size(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_LEAF_SIZE;
}

__unused static inline uint64_t
pt_attr_leaf_offmask(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_LEAF_OFFMASK;
}

static inline uint64_t
pt_attr_leaf_shift(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_LEAF_SHIFT;
}

static __unused inline uint64_t
pt_attr_leaf_index_mask(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_LEAF_INDEX_MASK;
}

static inline uint64_t
pt_attr_leaf_table_size(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_L1_PT_SIZE;
}

static inline uint64_t
pt_attr_leaf_table_offmask(__unused const pt_attr_t * const pt_attr)
{
	return ARM_TT_L1_PT_OFFMASK;
}

static inline uintptr_t
pt_attr_leaf_rw(__unused const pt_attr_t * const pt_attr)
{
	return ARM_PTE_AP(AP_RWRW);
}

static inline uintptr_t
pt_attr_leaf_ro(__unused const pt_attr_t * const pt_attr)
{
	return ARM_PTE_AP(AP_RORO);
}

static inline uintptr_t
pt_attr_leaf_rona(__unused const pt_attr_t * const pt_attr)
{
	return ARM_PTE_AP(AP_RONA);
}

static inline uintptr_t
pt_attr_leaf_rwna(__unused const pt_attr_t * const pt_attr)
{
	return ARM_PTE_AP(AP_RWNA);
}

static inline uintptr_t
pt_attr_leaf_xn(__unused const pt_attr_t * const pt_attr)
{
	return ARM_PTE_NX;
}

#endif /* (__ARM_VMSA__ > 7) */

static inline void
pmap_sync_tlb(bool strong __unused)
{
	sync_tlb_flush();
}

#if MACH_ASSERT
int vm_footprint_suspend_allowed = 1;

extern int pmap_ledgers_panic;
extern int pmap_ledgers_panic_leeway;

int pmap_stats_assert = 1;
#define PMAP_STATS_ASSERTF(cond, pmap, fmt, ...)                    \
	MACRO_BEGIN                                         \
	if (pmap_stats_assert && (pmap)->pmap_stats_assert) \
	        assertf(cond, fmt, ##__VA_ARGS__);                  \
	MACRO_END
#else /* MACH_ASSERT */
#define PMAP_STATS_ASSERTF(cond, pmap, fmt, ...)
#endif /* MACH_ASSERT */

#if DEVELOPMENT || DEBUG
#define PMAP_FOOTPRINT_SUSPENDED(pmap) \
	(current_thread()->pmap_footprint_suspended)
#else /* DEVELOPMENT || DEBUG */
#define PMAP_FOOTPRINT_SUSPENDED(pmap) (FALSE)
#endif /* DEVELOPMENT || DEBUG */


#if XNU_MONITOR
/*
 * PPL External References.
 */
extern vm_offset_t   segPPLDATAB;
extern unsigned long segSizePPLDATA;
extern vm_offset_t   segPPLTEXTB;
extern unsigned long segSizePPLTEXT;
#if __APRR_SUPPORTED__
extern vm_offset_t   segPPLTRAMPB;
extern unsigned long segSizePPLTRAMP;
extern void ppl_trampoline_start;
extern void ppl_trampoline_end;
#endif
extern vm_offset_t   segPPLDATACONSTB;
extern unsigned long segSizePPLDATACONST;


/*
 * PPL Global Variables
 */

#if (DEVELOPMENT || DEBUG)
/* Indicates if the PPL will enforce mapping policies; set by -unsafe_kernel_text */
SECURITY_READ_ONLY_LATE(boolean_t) pmap_ppl_disable = FALSE;
#else
const boolean_t pmap_ppl_disable = FALSE;
#endif

/* Indicates if the PPL has started applying APRR. */
boolean_t pmap_ppl_locked_down MARK_AS_PMAP_DATA = FALSE;

/*
 * The PPL cannot invoke the kernel in order to allocate memory, so we must
 * maintain a list of free pages that the PPL owns.  The kernel can give the PPL
 * additional pages.
 */
decl_simple_lock_data(, pmap_ppl_free_page_lock MARK_AS_PMAP_DATA);
void ** pmap_ppl_free_page_list MARK_AS_PMAP_DATA = NULL;
uint64_t pmap_ppl_free_page_count MARK_AS_PMAP_DATA = 0;
uint64_t pmap_ppl_pages_returned_to_kernel_count_total = 0;

struct pmap_cpu_data_array_entry pmap_cpu_data_array[MAX_CPUS] MARK_AS_PMAP_DATA;

#ifdef CPU_CLUSTER_OFFSETS
const uint64_t pmap_cluster_offsets[] = CPU_CLUSTER_OFFSETS;
_Static_assert((sizeof(pmap_cluster_offsets) / sizeof(pmap_cluster_offsets[0])) == __ARM_CLUSTER_COUNT__,
    "pmap_cluster_offsets[] count does not match __ARM_CLUSTER_COUNT__");
#endif

extern void *pmap_stacks_start;
extern void *pmap_stacks_end;
SECURITY_READ_ONLY_LATE(pmap_paddr_t) pmap_stacks_start_pa = 0;
SECURITY_READ_ONLY_LATE(pmap_paddr_t) pmap_stacks_end_pa = 0;
SECURITY_READ_ONLY_LATE(pmap_paddr_t) ppl_cpu_save_area_start = 0;
SECURITY_READ_ONLY_LATE(pmap_paddr_t) ppl_cpu_save_area_end = 0;

/* Allocation data/locks for pmap structures. */
decl_simple_lock_data(, pmap_free_list_lock MARK_AS_PMAP_DATA);
SECURITY_READ_ONLY_LATE(unsigned long) pmap_array_count = 0;
SECURITY_READ_ONLY_LATE(void *) pmap_array_begin = NULL;
SECURITY_READ_ONLY_LATE(void *) pmap_array_end = NULL;
SECURITY_READ_ONLY_LATE(pmap_t) pmap_array = NULL;
pmap_t pmap_free_list MARK_AS_PMAP_DATA = NULL;

/* Allocation data/locks/structs for task ledger structures. */
#define PMAP_LEDGER_DATA_BYTES \
	(((sizeof(task_ledgers) / sizeof(int)) * sizeof(struct ledger_entry)) + sizeof(struct ledger))

/*
 * Maximum number of ledgers allowed are maximum number of tasks
 * allowed on system plus some more i.e. ~10% of total tasks = 200.
 */
#define MAX_PMAP_LEDGERS (MAX_ASID + 200)

typedef struct pmap_ledger_data {
	char pld_data[PMAP_LEDGER_DATA_BYTES];
} pmap_ledger_data_t;

typedef struct pmap_ledger {
	union {
		struct pmap_ledger_data ple_data;
		struct pmap_ledger * next;
	};

	struct pmap_ledger ** back_ptr;
} pmap_ledger_t;

SECURITY_READ_ONLY_LATE(bool) pmap_ledger_alloc_initialized = false;
decl_simple_lock_data(, pmap_ledger_lock MARK_AS_PMAP_DATA);
SECURITY_READ_ONLY_LATE(void *) pmap_ledger_refcnt_begin = NULL;
SECURITY_READ_ONLY_LATE(void *) pmap_ledger_refcnt_end = NULL;
SECURITY_READ_ONLY_LATE(os_refcnt_t *) pmap_ledger_refcnt = NULL;
SECURITY_READ_ONLY_LATE(void *) pmap_ledger_ptr_array_begin = NULL;
SECURITY_READ_ONLY_LATE(void *) pmap_ledger_ptr_array_end = NULL;
SECURITY_READ_ONLY_LATE(pmap_ledger_t * *) pmap_ledger_ptr_array = NULL;
uint64_t pmap_ledger_ptr_array_free_index MARK_AS_PMAP_DATA = 0;
pmap_ledger_t * pmap_ledger_free_list MARK_AS_PMAP_DATA = NULL;

#define pmap_ledger_debit(p, e, a) ledger_debit_nocheck((p)->ledger, e, a)
#define pmap_ledger_credit(p, e, a) ledger_credit_nocheck((p)->ledger, e, a)

static inline void
pmap_check_ledger_fields(ledger_t ledger)
{
	if (ledger == NULL) {
		return;
	}

	thread_t cur_thread = current_thread();
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.alternate_accounting);
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.alternate_accounting_compressed);
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.internal);
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.internal_compressed);
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.page_table);
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.phys_footprint);
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.phys_mem);
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.tkm_private);
	ledger_check_new_balance(cur_thread, ledger, task_ledgers.wired_mem);
}

#define pmap_ledger_check_balance(p) pmap_check_ledger_fields((p)->ledger)

#else /* XNU_MONITOR */

#define pmap_ledger_debit(p, e, a) ledger_debit((p)->ledger, e, a)
#define pmap_ledger_credit(p, e, a) ledger_credit((p)->ledger, e, a)

#endif /* !XNU_MONITOR */

#if DEVELOPMENT || DEBUG
int panic_on_unsigned_execute = 0;
#endif /* DEVELOPMENT || DEBUG */


/* Virtual memory region for early allocation */
#if     (__ARM_VMSA__ == 7)
#define VREGION1_HIGH_WINDOW    (0)
#else
#define VREGION1_HIGH_WINDOW    (PE_EARLY_BOOT_VA)
#endif
#define VREGION1_START          ((VM_MAX_KERNEL_ADDRESS & CPUWINDOWS_BASE_MASK) - VREGION1_HIGH_WINDOW)
#define VREGION1_SIZE           (trunc_page(VM_MAX_KERNEL_ADDRESS - (VREGION1_START)))

extern unsigned int not_in_kdp;

extern vm_offset_t first_avail;

extern pmap_paddr_t avail_start;
extern pmap_paddr_t avail_end;

extern vm_offset_t     virtual_space_start;     /* Next available kernel VA */
extern vm_offset_t     virtual_space_end;       /* End of kernel address space */
extern vm_offset_t     static_memory_end;

extern int maxproc, hard_maxproc;

#if (__ARM_VMSA__ > 7)
/* The number of address bits one TTBR can cover. */
#define PGTABLE_ADDR_BITS (64ULL - T0SZ_BOOT)

/*
 * The bounds on our TTBRs.  These are for sanity checking that
 * an address is accessible by a TTBR before we attempt to map it.
 */
#define ARM64_TTBR0_MIN_ADDR (0ULL)
#define ARM64_TTBR0_MAX_ADDR (0ULL + (1ULL << PGTABLE_ADDR_BITS) - 1)
#define ARM64_TTBR1_MIN_ADDR (0ULL - (1ULL << PGTABLE_ADDR_BITS))
#define ARM64_TTBR1_MAX_ADDR (~0ULL)

/* The level of the root of a page table. */
const uint64_t arm64_root_pgtable_level = (3 - ((PGTABLE_ADDR_BITS - 1 - ARM_PGSHIFT) / (ARM_PGSHIFT - TTE_SHIFT)));

/* The number of entries in the root TT of a page table. */
const uint64_t arm64_root_pgtable_num_ttes = (2 << ((PGTABLE_ADDR_BITS - 1 - ARM_PGSHIFT) % (ARM_PGSHIFT - TTE_SHIFT)));
#else
const uint64_t arm64_root_pgtable_level = 0;
const uint64_t arm64_root_pgtable_num_ttes = 0;
#endif

struct pmap                     kernel_pmap_store MARK_AS_PMAP_DATA;
SECURITY_READ_ONLY_LATE(pmap_t) kernel_pmap = &kernel_pmap_store;

struct vm_object pmap_object_store __attribute__((aligned(VM_PACKED_POINTER_ALIGNMENT)));       /* store pt pages */
vm_object_t     pmap_object = &pmap_object_store;

static struct zone *pmap_zone;  /* zone of pmap structures */

decl_simple_lock_data(, pmaps_lock MARK_AS_PMAP_DATA);
decl_simple_lock_data(, tt1_lock MARK_AS_PMAP_DATA);
unsigned int    pmap_stamp MARK_AS_PMAP_DATA;
queue_head_t    map_pmap_list MARK_AS_PMAP_DATA;

decl_simple_lock_data(, pt_pages_lock MARK_AS_PMAP_DATA);
queue_head_t    pt_page_list MARK_AS_PMAP_DATA; /* pt page ptd entries list */

decl_simple_lock_data(, pmap_pages_lock MARK_AS_PMAP_DATA);

typedef struct page_free_entry {
	struct page_free_entry  *next;
} page_free_entry_t;

#define PAGE_FREE_ENTRY_NULL    ((page_free_entry_t *) 0)

page_free_entry_t       *pmap_pages_reclaim_list MARK_AS_PMAP_DATA;     /* Reclaimed pt page list */
unsigned int            pmap_pages_request_count MARK_AS_PMAP_DATA;     /* Pending requests to reclaim pt page */
unsigned long long      pmap_pages_request_acum MARK_AS_PMAP_DATA;


typedef struct tt_free_entry {
	struct tt_free_entry    *next;
} tt_free_entry_t;

#define TT_FREE_ENTRY_NULL      ((tt_free_entry_t *) 0)

tt_free_entry_t *free_page_size_tt_list MARK_AS_PMAP_DATA;
unsigned int    free_page_size_tt_count MARK_AS_PMAP_DATA;
unsigned int    free_page_size_tt_max MARK_AS_PMAP_DATA;
#define FREE_PAGE_SIZE_TT_MAX   4
tt_free_entry_t *free_two_page_size_tt_list MARK_AS_PMAP_DATA;
unsigned int    free_two_page_size_tt_count MARK_AS_PMAP_DATA;
unsigned int    free_two_page_size_tt_max MARK_AS_PMAP_DATA;
#define FREE_TWO_PAGE_SIZE_TT_MAX       4
tt_free_entry_t *free_tt_list MARK_AS_PMAP_DATA;
unsigned int    free_tt_count MARK_AS_PMAP_DATA;
unsigned int    free_tt_max MARK_AS_PMAP_DATA;

#define TT_FREE_ENTRY_NULL      ((tt_free_entry_t *) 0)

boolean_t pmap_gc_allowed MARK_AS_PMAP_DATA = TRUE;
boolean_t pmap_gc_forced MARK_AS_PMAP_DATA = FALSE;
boolean_t pmap_gc_allowed_by_time_throttle = TRUE;

unsigned int    inuse_user_ttepages_count MARK_AS_PMAP_DATA = 0;        /* non-root, non-leaf user pagetable pages, in units of PAGE_SIZE */
unsigned int    inuse_user_ptepages_count MARK_AS_PMAP_DATA = 0;        /* leaf user pagetable pages, in units of PAGE_SIZE */
unsigned int    inuse_user_tteroot_count MARK_AS_PMAP_DATA = 0;  /* root user pagetables, in units of PMAP_ROOT_ALLOC_SIZE */
unsigned int    inuse_kernel_ttepages_count MARK_AS_PMAP_DATA = 0; /* non-root, non-leaf kernel pagetable pages, in units of PAGE_SIZE */
unsigned int    inuse_kernel_ptepages_count MARK_AS_PMAP_DATA = 0; /* leaf kernel pagetable pages, in units of PAGE_SIZE */
unsigned int    inuse_kernel_tteroot_count MARK_AS_PMAP_DATA = 0; /* root kernel pagetables, in units of PMAP_ROOT_ALLOC_SIZE */
unsigned int    inuse_pmap_pages_count = 0;     /* debugging */

SECURITY_READ_ONLY_LATE(tt_entry_t *) invalid_tte  = 0;
SECURITY_READ_ONLY_LATE(pmap_paddr_t) invalid_ttep = 0;

SECURITY_READ_ONLY_LATE(tt_entry_t *) cpu_tte  = 0;                     /* set by arm_vm_init() - keep out of bss */
SECURITY_READ_ONLY_LATE(pmap_paddr_t) cpu_ttep = 0;                     /* set by arm_vm_init() - phys tte addr */

#if DEVELOPMENT || DEBUG
int nx_enabled = 1;                                     /* enable no-execute protection */
int allow_data_exec  = 0;                               /* No apps may execute data */
int allow_stack_exec = 0;                               /* No apps may execute from the stack */
unsigned long pmap_asid_flushes MARK_AS_PMAP_DATA = 0;
#else /* DEVELOPMENT || DEBUG */
const int nx_enabled = 1;                                       /* enable no-execute protection */
const int allow_data_exec  = 0;                         /* No apps may execute data */
const int allow_stack_exec = 0;                         /* No apps may execute from the stack */
#endif /* DEVELOPMENT || DEBUG */

/*
 *      pv_entry_t - structure to track the active mappings for a given page
 */
typedef struct pv_entry {
	struct pv_entry *pve_next;              /* next alias */
	pt_entry_t      *pve_ptep;              /* page table entry */
}
#if __arm__ && (__BIGGEST_ALIGNMENT__ > 4)
/* For the newer ARMv7k ABI where 64-bit types are 64-bit aligned, but pointers
 * are 32-bit:
 * Since pt_desc is 64-bit aligned and we cast often from pv_entry to
 * pt_desc.
 */
__attribute__ ((aligned(8))) pv_entry_t;
#else
pv_entry_t;
#endif

#define PV_ENTRY_NULL   ((pv_entry_t *) 0)

/*
 * PMAP LEDGERS:
 * We use the least significant bit of the "pve_next" pointer in a "pv_entry"
 * as a marker for pages mapped through an "alternate accounting" mapping.
 * These macros set, clear and test for this marker and extract the actual
 * value of the "pve_next" pointer.
 */
#define PVE_NEXT_ALTACCT        ((uintptr_t) 0x1)
#define PVE_NEXT_SET_ALTACCT(pve_next_p) \
	*(pve_next_p) = (struct pv_entry *) (((uintptr_t) *(pve_next_p)) | \
	                                     PVE_NEXT_ALTACCT)
#define PVE_NEXT_CLR_ALTACCT(pve_next_p) \
	*(pve_next_p) = (struct pv_entry *) (((uintptr_t) *(pve_next_p)) & \
	                                     ~PVE_NEXT_ALTACCT)
#define PVE_NEXT_IS_ALTACCT(pve_next)   \
	((((uintptr_t) (pve_next)) & PVE_NEXT_ALTACCT) ? TRUE : FALSE)
#define PVE_NEXT_PTR(pve_next) \
	((struct pv_entry *)(((uintptr_t) (pve_next)) & \
	                     ~PVE_NEXT_ALTACCT))
#if MACH_ASSERT
static void pmap_check_ledgers(pmap_t pmap);
#else
static inline void
pmap_check_ledgers(__unused pmap_t pmap)
{
}
#endif /* MACH_ASSERT */

SECURITY_READ_ONLY_LATE(pv_entry_t * *) pv_head_table;           /* array of pv entry pointers */

pv_entry_t              *pv_free_list MARK_AS_PMAP_DATA;
pv_entry_t              *pv_kern_free_list MARK_AS_PMAP_DATA;
decl_simple_lock_data(, pv_free_list_lock MARK_AS_PMAP_DATA);
decl_simple_lock_data(, pv_kern_free_list_lock MARK_AS_PMAP_DATA);

decl_simple_lock_data(, phys_backup_lock);

/*
 *		pt_desc - structure to keep info on page assigned to page tables
 */
#if (__ARM_VMSA__ == 7)
#define PT_INDEX_MAX    1
#else
#if (ARM_PGSHIFT == 14)
#define PT_INDEX_MAX    1
#else
#define PT_INDEX_MAX    4
#endif
#endif

#define PT_DESC_REFCOUNT                0x4000U
#define PT_DESC_IOMMU_REFCOUNT          0x8000U

typedef struct pt_desc {
	queue_chain_t                   pt_page;
	union {
		struct pmap             *pmap;
	};
	/*
	 * Locate this struct towards the end of the pt_desc; our long term
	 * goal is to make this a VLA to avoid wasting memory if we don't need
	 * multiple entries.
	 */
	struct {
		/*
		 * For non-leaf pagetables, should always be PT_DESC_REFCOUNT
		 * For leaf pagetables, should reflect the number of non-empty PTEs
		 * For IOMMU pages, should always be PT_DESC_IOMMU_REFCOUNT
		 */
		unsigned short          refcnt;
		/*
		 * For non-leaf pagetables, should be 0
		 * For leaf pagetables, should reflect the number of wired entries
		 * For IOMMU pages, may optionally reflect a driver-defined refcount (IOMMU operations are implicitly wired)
		 */
		unsigned short          wiredcnt;
		vm_offset_t             va;
	} ptd_info[PT_INDEX_MAX];
} pt_desc_t;


#define PTD_ENTRY_NULL  ((pt_desc_t *) 0)

SECURITY_READ_ONLY_LATE(pt_desc_t *) ptd_root_table;

pt_desc_t               *ptd_free_list MARK_AS_PMAP_DATA = PTD_ENTRY_NULL;
SECURITY_READ_ONLY_LATE(boolean_t) ptd_preboot = TRUE;
unsigned int    ptd_free_count MARK_AS_PMAP_DATA = 0;
decl_simple_lock_data(, ptd_free_list_lock MARK_AS_PMAP_DATA);

/*
 *	physical page attribute
 */
typedef u_int16_t pp_attr_t;

#define PP_ATTR_WIMG_MASK               0x003F
#define PP_ATTR_WIMG(x)                 ((x) & PP_ATTR_WIMG_MASK)

#define PP_ATTR_REFERENCED              0x0040
#define PP_ATTR_MODIFIED                0x0080

#define PP_ATTR_INTERNAL                0x0100
#define PP_ATTR_REUSABLE                0x0200
#define PP_ATTR_ALTACCT                 0x0400
#define PP_ATTR_NOENCRYPT               0x0800

#define PP_ATTR_REFFAULT                0x1000
#define PP_ATTR_MODFAULT                0x2000

#if XNU_MONITOR
/*
 * Denotes that a page is owned by the PPL.  This is modified/checked with the
 * PVH lock held, to avoid ownership related races.  This does not need to be a
 * PP_ATTR bit (as we have the lock), but for now this is a convenient place to
 * put the bit.
 */
#define PP_ATTR_MONITOR                 0x4000

/*
 * Denotes that a page *cannot* be owned by the PPL.  This is required in order
 * to temporarily 'pin' kernel pages that are used to store PPL output parameters.
 * Otherwise a malicious or buggy caller could pass PPL-owned memory for these
 * parameters and in so doing stage a write gadget against the PPL.
 */
#define PP_ATTR_NO_MONITOR              0x8000

/*
 * All of the bits owned by the PPL; kernel requests to set or clear these bits
 * are illegal.
 */
#define PP_ATTR_PPL_OWNED_BITS          (PP_ATTR_MONITOR | PP_ATTR_NO_MONITOR)
#endif

SECURITY_READ_ONLY_LATE(pp_attr_t*)     pp_attr_table;

typedef struct pmap_io_range {
	uint64_t addr;
	uint64_t len;
	#define PMAP_IO_RANGE_STRONG_SYNC (1UL << 31) // Strong DSB required for pages in this range
	#define PMAP_IO_RANGE_CARVEOUT (1UL << 30) // Corresponds to memory carved out by bootloader
	uint32_t wimg; // lower 16 bits treated as pp_attr_t, upper 16 bits contain additional mapping flags
	uint32_t signature; // 4CC
} __attribute__((packed)) pmap_io_range_t;

SECURITY_READ_ONLY_LATE(pmap_io_range_t*)       io_attr_table;

SECURITY_READ_ONLY_LATE(pmap_paddr_t)   vm_first_phys = (pmap_paddr_t) 0;
SECURITY_READ_ONLY_LATE(pmap_paddr_t)   vm_last_phys = (pmap_paddr_t) 0;

SECURITY_READ_ONLY_LATE(unsigned int)   num_io_rgns = 0;

SECURITY_READ_ONLY_LATE(boolean_t)      pmap_initialized = FALSE;       /* Has pmap_init completed? */

SECURITY_READ_ONLY_LATE(uint64_t) pmap_nesting_size_min;
SECURITY_READ_ONLY_LATE(uint64_t) pmap_nesting_size_max;

SECURITY_READ_ONLY_LATE(vm_map_offset_t) arm_pmap_max_offset_default  = 0x0;
#if defined(__arm64__)
SECURITY_READ_ONLY_LATE(vm_map_offset_t) arm64_pmap_max_offset_default = 0x0;
#endif

#define PMAP_MAX_SW_ASID ((MAX_ASID + MAX_HW_ASID - 1) / MAX_HW_ASID)
_Static_assert(PMAP_MAX_SW_ASID <= (UINT8_MAX + 1),
    "VASID bits can't be represented by an 8-bit integer");

decl_simple_lock_data(, asid_lock MARK_AS_PMAP_DATA);
static bitmap_t asid_bitmap[BITMAP_LEN(MAX_ASID)] MARK_AS_PMAP_DATA;


#if     (__ARM_VMSA__ > 7)
SECURITY_READ_ONLY_LATE(pmap_t) sharedpage_pmap;
#endif

#if XNU_MONITOR
/*
 * We define our target as 8 pages; enough for 2 page table pages, a PTD page,
 * and a PV page; in essence, twice as many pages as may be necessary to satisfy
 * a single pmap_enter request.
 */
#define PMAP_MIN_FREE_PPL_PAGES 8
#endif

#define pa_index(pa)                                                                    \
	(atop((pa) - vm_first_phys))

#define pai_to_pvh(pai)                                                                 \
	(&pv_head_table[pai])

#define pa_valid(x)                                                                     \
	((x) >= vm_first_phys && (x) < vm_last_phys)

/* PTE Define Macros */

#define pte_is_wired(pte)                                                               \
	(((pte) & ARM_PTE_WIRED_MASK) == ARM_PTE_WIRED)

#define pte_set_wired(ptep, wired)                                                                              \
	do {                                                                                                    \
	        SInt16	*ptd_wiredcnt_ptr;                                                                      \
	        ptd_wiredcnt_ptr = (SInt16 *)&(ptep_get_ptd(ptep)->ptd_info[ARM_PT_DESC_INDEX(ptep)].wiredcnt);   \
	        if (wired) {                                                                                    \
	                        *ptep |= ARM_PTE_WIRED;                                                         \
	                        OSAddAtomic16(1, ptd_wiredcnt_ptr);                                             \
	        } else {                                                                                        \
	                        *ptep &= ~ARM_PTE_WIRED;                                                        \
	                        OSAddAtomic16(-1, ptd_wiredcnt_ptr);                                            \
	        }                                                                                               \
	} while(0)

#define pte_was_writeable(pte) \
	(((pte) & ARM_PTE_WRITEABLE) == ARM_PTE_WRITEABLE)

#define pte_set_was_writeable(pte, was_writeable) \
	do {                                         \
	        if ((was_writeable)) {               \
	                (pte) |= ARM_PTE_WRITEABLE;  \
	        } else {                             \
	                (pte) &= ~ARM_PTE_WRITEABLE; \
	        }                                    \
	} while(0)

/* PVE Define Macros */

#define pve_next(pve)                                                                   \
	((pve)->pve_next)

#define pve_link_field(pve)                                                             \
	(&pve_next(pve))

#define pve_link(pp, e)                                                                 \
	((pve_next(e) = pve_next(pp)),	(pve_next(pp) = (e)))

#define pve_unlink(pp, e)                                                               \
	(pve_next(pp) = pve_next(e))

/* bits held in the ptep pointer field */

#define pve_get_ptep(pve)                                                               \
	((pve)->pve_ptep)

#define pve_set_ptep(pve, ptep_new)                                                     \
	do {                                                                            \
	        (pve)->pve_ptep = (ptep_new);                                           \
	} while (0)

/* PTEP Define Macros */

/* mask for page descriptor index */
#define ARM_TT_PT_INDEX_MASK            ARM_PGMASK

#if     (__ARM_VMSA__ == 7)
#define ARM_PT_DESC_INDEX_MASK          0x00000
#define ARM_PT_DESC_INDEX_SHIFT         0

/*
 * Shift value used for reconstructing the virtual address for a PTE.
 */
#define ARM_TT_PT_ADDR_SHIFT            (10U)

#define ptep_get_va(ptep)                                                                               \
	((((pt_desc_t *) (pvh_list(pai_to_pvh(pa_index(ml_static_vtop((((vm_offset_t)(ptep) & ~ARM_PGMASK))))))))->ptd_info[ARM_PT_DESC_INDEX(ptep)].va)+ ((((unsigned)(ptep)) & ARM_TT_PT_INDEX_MASK)<<ARM_TT_PT_ADDR_SHIFT))

#define ptep_get_pmap(ptep)                                                                             \
	((((pt_desc_t *) (pvh_list(pai_to_pvh(pa_index(ml_static_vtop((((vm_offset_t)(ptep) & ~ARM_PGMASK))))))))->pmap))

#else

#if (ARM_PGSHIFT == 12)
#define ARM_PT_DESC_INDEX_MASK          ((PAGE_SHIFT_CONST == ARM_PGSHIFT )? 0x00000ULL : 0x03000ULL)
#define ARM_PT_DESC_INDEX_SHIFT         ((PAGE_SHIFT_CONST == ARM_PGSHIFT )? 0 : 12)
/*
 * Shift value used for reconstructing the virtual address for a PTE.
 */
#define ARM_TT_PT_ADDR_SHIFT            (9ULL)
#else

#define ARM_PT_DESC_INDEX_MASK          (0x00000)
#define ARM_PT_DESC_INDEX_SHIFT         (0)
/*
 * Shift value used for reconstructing the virtual address for a PTE.
 */
#define ARM_TT_PT_ADDR_SHIFT            (11ULL)
#endif


#define ARM_PT_DESC_INDEX(ptep)                                                                         \
	(((unsigned)(ptep) & ARM_PT_DESC_INDEX_MASK) >> ARM_PT_DESC_INDEX_SHIFT)

#define ptep_get_va(ptep)                                                                               \
	((((pt_desc_t *) (pvh_list(pai_to_pvh(pa_index(ml_static_vtop((((vm_offset_t)(ptep) & ~ARM_PGMASK))))))))->ptd_info[ARM_PT_DESC_INDEX(ptep)].va)+ ((((unsigned)(ptep)) & ARM_TT_PT_INDEX_MASK)<<ARM_TT_PT_ADDR_SHIFT))

#define ptep_get_pmap(ptep)                                                                             \
	((((pt_desc_t *) (pvh_list(pai_to_pvh(pa_index(ml_static_vtop((((vm_offset_t)(ptep) & ~ARM_PGMASK))))))))->pmap))

#endif

#define ARM_PT_DESC_INDEX(ptep)                                                                         \
	(((unsigned)(ptep) & ARM_PT_DESC_INDEX_MASK) >> ARM_PT_DESC_INDEX_SHIFT)

#define ptep_get_ptd(ptep)                                                                              \
	((struct pt_desc *)(pvh_list(pai_to_pvh(pa_index(ml_static_vtop((vm_offset_t)(ptep)))))))


/* PVH Define Macros */

/* pvhead type */
#define PVH_TYPE_NULL        0x0UL
#define PVH_TYPE_PVEP        0x1UL
#define PVH_TYPE_PTEP        0x2UL
#define PVH_TYPE_PTDP        0x3UL

#define PVH_TYPE_MASK        (0x3UL)

#ifdef  __arm64__

/* All flags listed below are stored in the PV head pointer unless otherwise noted */
#define PVH_FLAG_IOMMU       0x4UL /* Stored in each PTE, or in PV head for single-PTE PV heads */
#define PVH_FLAG_IOMMU_TABLE (1ULL << 63) /* Stored in each PTE, or in PV head for single-PTE PV heads */
#define PVH_FLAG_CPU         (1ULL << 62)
#define PVH_LOCK_BIT         61
#define PVH_FLAG_LOCK        (1ULL << PVH_LOCK_BIT)
#define PVH_FLAG_EXEC        (1ULL << 60)
#define PVH_FLAG_LOCKDOWN    (1ULL << 59)
#define PVH_HIGH_FLAGS       (PVH_FLAG_CPU | PVH_FLAG_LOCK | PVH_FLAG_EXEC | PVH_FLAG_LOCKDOWN)

#else  /* !__arm64__ */

#define PVH_LOCK_BIT         31
#define PVH_FLAG_LOCK        (1UL << PVH_LOCK_BIT)
#define PVH_HIGH_FLAGS       PVH_FLAG_LOCK

#endif

#define PVH_LIST_MASK   (~PVH_TYPE_MASK)

#define pvh_test_type(h, b)                                                                                     \
	((*(vm_offset_t *)(h) & (PVH_TYPE_MASK)) == (b))

#define pvh_ptep(h)                                                                                             \
	((pt_entry_t *)((*(vm_offset_t *)(h) & PVH_LIST_MASK) | PVH_HIGH_FLAGS))

#define pvh_list(h)                                                                                             \
	((pv_entry_t *)((*(vm_offset_t *)(h) & PVH_LIST_MASK) | PVH_HIGH_FLAGS))

#define pvh_get_flags(h)                                                                                        \
	(*(vm_offset_t *)(h) & PVH_HIGH_FLAGS)

#define pvh_set_flags(h, f)                                                                                     \
	do {                                                                                                    \
	        os_atomic_store((vm_offset_t *)(h), (*(vm_offset_t *)(h) & ~PVH_HIGH_FLAGS) | (f),              \
	             relaxed);                                                                                  \
	} while (0)

#define pvh_update_head(h, e, t)                                                                                \
	do {                                                                                                    \
	        assert(*(vm_offset_t *)(h) & PVH_FLAG_LOCK);                                                    \
	        os_atomic_store((vm_offset_t *)(h), (vm_offset_t)(e) | (t) | PVH_FLAG_LOCK,                     \
	             relaxed);                                                                                  \
	} while (0)

#define pvh_update_head_unlocked(h, e, t)                                                                       \
	do {                                                                                                    \
	        assert(!(*(vm_offset_t *)(h) & PVH_FLAG_LOCK));                                                 \
	        *(vm_offset_t *)(h) = ((vm_offset_t)(e) | (t)) & ~PVH_FLAG_LOCK;                                \
	} while (0)

#define pvh_add(h, e)                                                                   \
	do {                                                                            \
	        assert(!pvh_test_type((h), PVH_TYPE_PTEP));                             \
	        pve_next(e) = pvh_list(h);                                              \
	        pvh_update_head((h), (e), PVH_TYPE_PVEP);                               \
	} while (0)

#define pvh_remove(h, p, e)                                                                             \
	do {                                                                                            \
	        assert(!PVE_NEXT_IS_ALTACCT(pve_next((e))));                                            \
	        if ((p) == (h)) {                                                                       \
	                if (PVE_NEXT_PTR(pve_next((e))) == PV_ENTRY_NULL) {                             \
	                        pvh_update_head((h), PV_ENTRY_NULL, PVH_TYPE_NULL);                     \
	                } else {                                                                        \
	                        pvh_update_head((h), PVE_NEXT_PTR(pve_next((e))), PVH_TYPE_PVEP);       \
	                }                                                                               \
	        } else {                                                                                \
	/* \
	 * PMAP LEDGERS: \
	 * preserve the "alternate accounting" bit \
	 * when updating "p" (the previous entry's \
	 * "pve_next"). \
	 */                                                                                             \
	                boolean_t	__is_altacct;                                                   \
	                __is_altacct = PVE_NEXT_IS_ALTACCT(*(p));                                       \
	                *(p) = PVE_NEXT_PTR(pve_next((e)));                                             \
	                if (__is_altacct) {                                                             \
	                        PVE_NEXT_SET_ALTACCT((p));                                              \
	                } else {                                                                        \
	                        PVE_NEXT_CLR_ALTACCT((p));                                              \
	                }                                                                               \
	        }                                                                                       \
	} while (0)


/* PPATTR Define Macros */

#define ppattr_set_bits(h, b)                                                                                   \
	do {                                                                                                    \
	        while (!OSCompareAndSwap16(*(pp_attr_t *)(h), *(pp_attr_t *)(h) | (b), (pp_attr_t *)(h)));      \
	} while (0)

#define ppattr_clear_bits(h, b)                                                                                 \
	do {                                                                                                    \
	        while (!OSCompareAndSwap16(*(pp_attr_t *)(h), *(pp_attr_t *)(h) & ~(b), (pp_attr_t *)(h)));     \
	} while (0)

#define ppattr_test_bits(h, b)                                                          \
	((*(pp_attr_t *)(h) & (b)) == (b))

#define pa_set_bits(x, b)                                                               \
	do {                                                                            \
	        if (pa_valid(x))                                                        \
	                ppattr_set_bits(&pp_attr_table[pa_index(x)],                    \
	                             (b));                                              \
	} while (0)

#define pa_test_bits(x, b)                                                              \
	(pa_valid(x) ? ppattr_test_bits(&pp_attr_table[pa_index(x)],\
	                             (b)) : FALSE)

#define pa_clear_bits(x, b)                                                             \
	do {                                                                            \
	        if (pa_valid(x))                                                        \
	                ppattr_clear_bits(&pp_attr_table[pa_index(x)],                  \
	                               (b));                                            \
	} while (0)

#define pa_set_modify(x)                                                                \
	pa_set_bits(x, PP_ATTR_MODIFIED)

#define pa_clear_modify(x)                                                              \
	pa_clear_bits(x, PP_ATTR_MODIFIED)

#define pa_set_reference(x)                                                             \
	pa_set_bits(x, PP_ATTR_REFERENCED)

#define pa_clear_reference(x)                                                           \
	pa_clear_bits(x, PP_ATTR_REFERENCED)

#if XNU_MONITOR
#define pa_set_monitor(x) \
	pa_set_bits((x), PP_ATTR_MONITOR)

#define pa_clear_monitor(x) \
	pa_clear_bits((x), PP_ATTR_MONITOR)

#define pa_test_monitor(x) \
	pa_test_bits((x), PP_ATTR_MONITOR)

#define pa_set_no_monitor(x) \
	pa_set_bits((x), PP_ATTR_NO_MONITOR)

#define pa_clear_no_monitor(x) \
	pa_clear_bits((x), PP_ATTR_NO_MONITOR)

#define pa_test_no_monitor(x) \
	pa_test_bits((x), PP_ATTR_NO_MONITOR)
#endif

#define IS_INTERNAL_PAGE(pai) \
	ppattr_test_bits(&pp_attr_table[pai], PP_ATTR_INTERNAL)
#define SET_INTERNAL_PAGE(pai) \
	ppattr_set_bits(&pp_attr_table[pai], PP_ATTR_INTERNAL)
#define CLR_INTERNAL_PAGE(pai) \
	ppattr_clear_bits(&pp_attr_table[pai], PP_ATTR_INTERNAL)

#define IS_REUSABLE_PAGE(pai) \
	ppattr_test_bits(&pp_attr_table[pai], PP_ATTR_REUSABLE)
#define SET_REUSABLE_PAGE(pai) \
	ppattr_set_bits(&pp_attr_table[pai], PP_ATTR_REUSABLE)
#define CLR_REUSABLE_PAGE(pai) \
	ppattr_clear_bits(&pp_attr_table[pai], PP_ATTR_REUSABLE)

#define IS_ALTACCT_PAGE(pai, pve_p)                                                     \
	(((pve_p) == NULL)                                                              \
	 ? ppattr_test_bits(&pp_attr_table[pai], PP_ATTR_ALTACCT)                       \
	 : PVE_NEXT_IS_ALTACCT(pve_next((pve_p))))
#define SET_ALTACCT_PAGE(pai, pve_p)                                                    \
	if ((pve_p) == NULL) {                                                          \
	        ppattr_set_bits(&pp_attr_table[pai], PP_ATTR_ALTACCT);                  \
	} else {                                                                        \
	        PVE_NEXT_SET_ALTACCT(&pve_next((pve_p)));                               \
	}
#define CLR_ALTACCT_PAGE(pai, pve_p)                                                    \
	if ((pve_p) == NULL) {                                                          \
	        ppattr_clear_bits(&pp_attr_table[pai], PP_ATTR_ALTACCT);                \
	} else {                                                                        \
	        PVE_NEXT_CLR_ALTACCT(&pve_next((pve_p)));                               \
	}

#define IS_REFFAULT_PAGE(pai) \
	ppattr_test_bits(&pp_attr_table[pai], PP_ATTR_REFFAULT)
#define SET_REFFAULT_PAGE(pai) \
	ppattr_set_bits(&pp_attr_table[pai], PP_ATTR_REFFAULT)
#define CLR_REFFAULT_PAGE(pai) \
	ppattr_clear_bits(&pp_attr_table[pai], PP_ATTR_REFFAULT)

#define IS_MODFAULT_PAGE(pai) \
	ppattr_test_bits(&pp_attr_table[pai], PP_ATTR_MODFAULT)
#define SET_MODFAULT_PAGE(pai) \
	ppattr_set_bits(&pp_attr_table[pai], PP_ATTR_MODFAULT)
#define CLR_MODFAULT_PAGE(pai) \
	ppattr_clear_bits(&pp_attr_table[pai], PP_ATTR_MODFAULT)

#define tte_get_ptd(tte)                                                                \
	((struct pt_desc *)(pvh_list(pai_to_pvh(pa_index((vm_offset_t)((tte) & ~PAGE_MASK))))))


#if     (__ARM_VMSA__ == 7)

#define tte_index(pmap, pt_attr, addr) \
	ttenum((addr))

#define pte_index(pmap, pt_attr, addr) \
	ptenum((addr))

#else

#define ttn_index(pmap, pt_attr, addr, pt_level) \
	(((addr) & (pt_attr)->pta_level_info[(pt_level)].index_mask) >> (pt_attr)->pta_level_info[(pt_level)].shift)

#define tt0_index(pmap, pt_attr, addr) \
	ttn_index((pmap), (pt_attr), (addr), PMAP_TT_L0_LEVEL)

#define tt1_index(pmap, pt_attr, addr) \
	ttn_index((pmap), (pt_attr), (addr), PMAP_TT_L1_LEVEL)

#define tt2_index(pmap, pt_attr, addr) \
	ttn_index((pmap), (pt_attr), (addr), PMAP_TT_L2_LEVEL)

#define tt3_index(pmap, pt_attr, addr) \
	ttn_index((pmap), (pt_attr), (addr), PMAP_TT_L3_LEVEL)

#define tte_index(pmap, pt_attr, addr) \
	tt2_index((pmap), (pt_attr), (addr))

#define pte_index(pmap, pt_attr, addr) \
	tt3_index((pmap), (pt_attr), (addr))

#endif

/*
 *	Lock on pmap system
 */

lck_grp_t pmap_lck_grp;

#define PMAP_LOCK_INIT(pmap) {                                                          \
	simple_lock_init(&(pmap)->lock, 0);                                             \
	                }

#define PMAP_LOCK(pmap) {                                                               \
	pmap_simple_lock(&(pmap)->lock);                                                \
}

#define PMAP_UNLOCK(pmap) {                                                             \
	pmap_simple_unlock(&(pmap)->lock);                                              \
}

#if MACH_ASSERT
#define PMAP_ASSERT_LOCKED(pmap) {                                                      \
	simple_lock_assert(&(pmap)->lock, LCK_ASSERT_OWNED);                            \
}
#else
#define PMAP_ASSERT_LOCKED(pmap)
#endif

#if defined(__arm64__)
#define PVH_LOCK_WORD 1 /* Assumes little-endian */
#else
#define PVH_LOCK_WORD 0
#endif

#define ASSERT_PVH_LOCKED(index)                                                                                                        \
	do {                                                                                                                            \
	        assert((vm_offset_t)(pv_head_table[index]) & PVH_FLAG_LOCK);                                                            \
	} while (0)

#define LOCK_PVH(index)                                                                                                                 \
	do {                                                                                                                            \
	        pmap_lock_bit((uint32_t*)(&pv_head_table[index]) + PVH_LOCK_WORD, PVH_LOCK_BIT - (PVH_LOCK_WORD * 32));         \
	} while (0)

#define UNLOCK_PVH(index)                                                                                                       \
	do {                                                                                                                    \
	        ASSERT_PVH_LOCKED(index);                                                                                       \
	        pmap_unlock_bit((uint32_t*)(&pv_head_table[index]) + PVH_LOCK_WORD, PVH_LOCK_BIT - (PVH_LOCK_WORD * 32));       \
	} while (0)

#define PMAP_UPDATE_TLBS(pmap, s, e, strong) {                                          \
	pmap_get_pt_ops(pmap)->flush_tlb_region_async(s, (unsigned)(e - s), pmap);      \
	pmap_sync_tlb(strong);                                                          \
}

#define FLUSH_PTE_RANGE(spte, epte)                                                     \
	__builtin_arm_dmb(DMB_ISH);

#define FLUSH_PTE(pte_p)                                                                \
	__builtin_arm_dmb(DMB_ISH);

#define FLUSH_PTE_STRONG(pte_p)                                                         \
	__builtin_arm_dsb(DSB_ISH);

#define FLUSH_PTE_RANGE_STRONG(spte, epte)                                              \
	__builtin_arm_dsb(DSB_ISH);

#define WRITE_PTE_FAST(pte_p, pte_entry)                                                \
	__unreachable_ok_push                                                           \
	if (TEST_PAGE_RATIO_4) {                                                        \
	        if (((unsigned)(pte_p)) & 0x1f) {                                       \
	                panic("%s: WRITE_PTE_FAST is unaligned, "                       \
	                      "pte_p=%p, pte_entry=%p",                                 \
	                       __FUNCTION__,                                            \
	                       pte_p, (void*)pte_entry);                                \
	        }                                                                       \
	        if (((pte_entry) & ~ARM_PTE_COMPRESSED_MASK) == ARM_PTE_EMPTY) {        \
	                *(pte_p) = (pte_entry);                                         \
	                *((pte_p)+1) = (pte_entry);                                     \
	                *((pte_p)+2) = (pte_entry);                                     \
	                *((pte_p)+3) = (pte_entry);                                     \
	        } else {                                                                \
	                *(pte_p) = (pte_entry);                                         \
	                *((pte_p)+1) = (pte_entry) | 0x1000;                            \
	                *((pte_p)+2) = (pte_entry) | 0x2000;                            \
	                *((pte_p)+3) = (pte_entry) | 0x3000;                            \
	        }                                                                       \
	} else {                                                                        \
	        *(pte_p) = (pte_entry);                                                 \
	}                                                                               \
	__unreachable_ok_pop

#define WRITE_PTE(pte_p, pte_entry)                                                     \
	WRITE_PTE_FAST(pte_p, pte_entry);                                               \
	FLUSH_PTE(pte_p);

#define WRITE_PTE_STRONG(pte_p, pte_entry)                                              \
	WRITE_PTE_FAST(pte_p, pte_entry);                                               \
	FLUSH_PTE_STRONG(pte_p);

/*
 * Other useful macros.
 */
#define current_pmap()                                                                  \
	(vm_map_pmap(current_thread()->map))

#if XNU_MONITOR
/*
 * PPL-related macros.
 */
#define ARRAY_ELEM_PTR_IS_VALID(_ptr_, _elem_size_, _array_begin_, _array_end_) \
	(((_ptr_) >= (typeof(_ptr_))_array_begin_) && \
	 ((_ptr_) < (typeof(_ptr_))_array_end_) && \
	 !((((void *)(_ptr_)) - ((void *)_array_begin_)) % (_elem_size_)))

#define PMAP_PTR_IS_VALID(x) ARRAY_ELEM_PTR_IS_VALID(x, sizeof(struct pmap), pmap_array_begin, pmap_array_end)

#define USER_PMAP_IS_VALID(x) (PMAP_PTR_IS_VALID(x) && (os_atomic_load(&(x)->ref_count, relaxed) > 0))

#define VALIDATE_USER_PMAP(x)                                                           \
	if (__improbable(!USER_PMAP_IS_VALID(x)))                                       \
	        panic("%s: invalid pmap %p", __func__, (x));

#define VALIDATE_PMAP(x)                                                                \
	if (__improbable(((x) != kernel_pmap) && !USER_PMAP_IS_VALID(x)))               \
	        panic("%s: invalid pmap %p", __func__, (x));

#define VALIDATE_LEDGER_PTR(x) \
	if (__improbable(!ARRAY_ELEM_PTR_IS_VALID(x, sizeof(void *), pmap_ledger_ptr_array_begin, pmap_ledger_ptr_array_end))) \
	        panic("%s: invalid ledger ptr %p", __func__, (x));

#define ARRAY_ELEM_INDEX(x, _elem_size_, _array_begin_) ((uint64_t)((((void *)(x)) - (_array_begin_)) / (_elem_size_)))

static uint64_t
pmap_ledger_validate(void * ledger)
{
	uint64_t array_index;
	pmap_ledger_t ** ledger_ptr_array_ptr = ((pmap_ledger_t*)ledger)->back_ptr;
	VALIDATE_LEDGER_PTR(ledger_ptr_array_ptr);
	array_index = ARRAY_ELEM_INDEX(ledger_ptr_array_ptr, sizeof(pmap_ledger_t *), pmap_ledger_ptr_array_begin);

	if (array_index >= MAX_PMAP_LEDGERS) {
		panic("%s: ledger %p array index invalid, index was %#llx", __func__, ledger, array_index);
	}

	pmap_ledger_t *ledger_ptr = *ledger_ptr_array_ptr;

	if (__improbable(ledger_ptr != ledger)) {
		panic("%s: ledger pointer mismatch, %p != %p", __func__, ledger, ledger_ptr);
	}

	return array_index;
}

#else /* XNU_MONITOR */

#define VALIDATE_USER_PMAP(x)
#define VALIDATE_PMAP(x)
#define VALIDATE_LEDGER(x)

#endif

#if DEVELOPMENT || DEBUG

/*
 * Trace levels are controlled by a bitmask in which each
 * level can be enabled/disabled by the (1<<level) position
 * in the boot arg
 * Level 1: pmap lifecycle (create/destroy/switch)
 * Level 2: mapping lifecycle (enter/remove/protect/nest/unnest)
 * Level 3: internal state management (tte/attributes/fast-fault)
 */

SECURITY_READ_ONLY_LATE(unsigned int) pmap_trace_mask = 0;

#define PMAP_TRACE(level, ...) \
	if (__improbable((1 << (level)) & pmap_trace_mask)) { \
	        KDBG_RELEASE(__VA_ARGS__); \
	}
#else

#define PMAP_TRACE(level, ...)

#endif


/*
 * Internal function prototypes (forward declarations).
 */

static void pv_init(
	void);

static boolean_t pv_alloc(
	pmap_t pmap,
	unsigned int pai,
	pv_entry_t **pvepp);

static void pv_free(
	pv_entry_t *pvep);

static void pv_list_free(
	pv_entry_t *pvehp,
	pv_entry_t *pvetp,
	unsigned int cnt);

static void ptd_bootstrap(
	pt_desc_t *ptdp, unsigned int ptd_cnt);

static inline pt_desc_t *ptd_alloc_unlinked(bool reclaim);

static pt_desc_t *ptd_alloc(pmap_t pmap, bool reclaim);

static void ptd_deallocate(pt_desc_t *ptdp);

static void ptd_init(
	pt_desc_t *ptdp, pmap_t pmap, vm_map_address_t va, unsigned int ttlevel, pt_entry_t * pte_p);

static void             pmap_zone_init(
	void);

static void             pmap_set_reference(
	ppnum_t pn);

ppnum_t                 pmap_vtophys(
	pmap_t pmap, addr64_t va);

void pmap_switch_user_ttb(
	pmap_t pmap);

static kern_return_t pmap_expand(
	pmap_t, vm_map_address_t, unsigned int options, unsigned int level);

static int pmap_remove_range(
	pmap_t, vm_map_address_t, pt_entry_t *, pt_entry_t *, uint32_t *);

static int pmap_remove_range_options(
	pmap_t, vm_map_address_t, pt_entry_t *, pt_entry_t *, uint32_t *, bool *, int);

static tt_entry_t *pmap_tt1_allocate(
	pmap_t, vm_size_t, unsigned int);

#define PMAP_TT_ALLOCATE_NOWAIT         0x1

static void pmap_tt1_deallocate(
	pmap_t, tt_entry_t *, vm_size_t, unsigned int);

#define PMAP_TT_DEALLOCATE_NOBLOCK      0x1

static kern_return_t pmap_tt_allocate(
	pmap_t, tt_entry_t **, unsigned int, unsigned int);

#define PMAP_TT_ALLOCATE_NOWAIT         0x1

static void pmap_tte_deallocate(
	pmap_t, tt_entry_t *, unsigned int);

#ifdef __ARM64_PMAP_SUBPAGE_L1__
#if (__ARM_VMSA__ <= 7)
#error This is not supported for old-style page tables
#endif /* (__ARM_VMSA__ <= 7) */
#define PMAP_ROOT_ALLOC_SIZE (((ARM_TT_L1_INDEX_MASK >> ARM_TT_L1_SHIFT) + 1) * sizeof(tt_entry_t))
#else /* !defined(__ARM64_PMAP_SUBPAGE_L1__) */
#if (__ARM_VMSA__ <= 7)
#define PMAP_ROOT_ALLOC_SIZE (ARM_PGBYTES * 2)
#else /* (__ARM_VMSA__ > 7) */
#define PMAP_ROOT_ALLOC_SIZE (ARM_PGBYTES)
#endif /* (__ARM_VMSA__ > 7) */
#endif /* !defined(__ARM64_PMAP_SUBPAGE_L1__) */

const unsigned int arm_hardware_page_size = ARM_PGBYTES;
const unsigned int arm_pt_desc_size = sizeof(pt_desc_t);
const unsigned int arm_pt_root_size = PMAP_ROOT_ALLOC_SIZE;

#define PMAP_TT_DEALLOCATE_NOBLOCK      0x1

#if     (__ARM_VMSA__ > 7)

static inline tt_entry_t *pmap_tt1e(
	pmap_t, vm_map_address_t);

static inline tt_entry_t *pmap_tt2e(
	pmap_t, vm_map_address_t);

static inline pt_entry_t *pmap_tt3e(
	pmap_t, vm_map_address_t);

static inline pt_entry_t *pmap_ttne(
	pmap_t, unsigned int, vm_map_address_t);

static void pmap_unmap_sharedpage(
	pmap_t pmap);

static boolean_t
pmap_is_64bit(pmap_t);


#endif
static inline tt_entry_t *pmap_tte(
	pmap_t, vm_map_address_t);

static inline pt_entry_t *pmap_pte(
	pmap_t, vm_map_address_t);

static void pmap_update_cache_attributes_locked(
	ppnum_t, unsigned);

boolean_t arm_clear_fast_fault(
	ppnum_t ppnum,
	vm_prot_t fault_type);

static pmap_paddr_t     pmap_pages_reclaim(
	void);

static kern_return_t pmap_pages_alloc(
	pmap_paddr_t    *pa,
	unsigned    size,
	unsigned    option);

#define PMAP_PAGES_ALLOCATE_NOWAIT              0x1
#define PMAP_PAGES_RECLAIM_NOWAIT               0x2

static void pmap_pages_free(
	pmap_paddr_t    pa,
	unsigned        size);

static void pmap_pin_kernel_pages(vm_offset_t kva, size_t nbytes);

static void pmap_unpin_kernel_pages(vm_offset_t kva, size_t nbytes);

static void pmap_trim_self(pmap_t pmap);
static void pmap_trim_subord(pmap_t subord);

#if __APRR_SUPPORTED__
static uint64_t pte_to_xprr_perm(pt_entry_t pte);
static pt_entry_t xprr_perm_to_pte(uint64_t perm);
#endif /* __APRR_SUPPORTED__*/

#if XNU_MONITOR
static pmap_paddr_t pmap_alloc_page_for_kern(void);
static void pmap_alloc_page_for_ppl(void);


/*
 * This macro generates prototypes for the *_internal functions, which
 * represent the PPL interface.  When the PPL is enabled, this will also
 * generate prototypes for the PPL entrypoints (*_ppl), as well as generating
 * the entrypoints.
 */
#define GEN_ASM_NAME(__function_name) _##__function_name##_ppl

#define PMAP_SUPPORT_PROTOTYPES_WITH_ASM_INTERNAL(__return_type, __function_name, __function_args, __function_index, __assembly_function_name) \
	static __return_type __function_name##_internal __function_args; \
	extern __return_type __function_name##_ppl __function_args; \
	__asm__ (".text \n" \
	         ".align 2 \n" \
	         ".globl " #__assembly_function_name "\n" \
	         #__assembly_function_name ":\n" \
	         "mov x15, " #__function_index "\n" \
	         "b _aprr_ppl_enter\n")

#define PMAP_SUPPORT_PROTOTYPES_WITH_ASM(__return_type, __function_name, __function_args, __function_index, __assembly_function_name) \
	PMAP_SUPPORT_PROTOTYPES_WITH_ASM_INTERNAL(__return_type, __function_name, __function_args, __function_index, __assembly_function_name)

#define PMAP_SUPPORT_PROTOTYPES(__return_type, __function_name, __function_args, __function_index) \
	PMAP_SUPPORT_PROTOTYPES_WITH_ASM(__return_type, __function_name, __function_args, __function_index, GEN_ASM_NAME(__function_name))
#else /* XNU_MONITOR */
#define PMAP_SUPPORT_PROTOTYPES(__return_type, __function_name, __function_args, __function_index) \
	static __return_type __function_name##_internal __function_args
#endif /* XNU_MONITOR */

PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	arm_fast_fault, (pmap_t pmap,
	vm_map_address_t va,
	vm_prot_t fault_type,
	bool was_af_fault,
	bool from_user), ARM_FAST_FAULT_INDEX);


PMAP_SUPPORT_PROTOTYPES(
	boolean_t,
	arm_force_fast_fault, (ppnum_t ppnum,
	vm_prot_t allow_mode,
	int options), ARM_FORCE_FAST_FAULT_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	mapping_free_prime, (void), MAPPING_FREE_PRIME_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	mapping_replenish, (uint32_t kern_target_count, uint32_t user_target_count), MAPPING_REPLENISH_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	boolean_t,
	pmap_batch_set_cache_attributes, (ppnum_t pn,
	unsigned int cacheattr,
	unsigned int page_cnt,
	unsigned int page_index,
	boolean_t doit,
	unsigned int *res), PMAP_BATCH_SET_CACHE_ATTRIBUTES_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_change_wiring, (pmap_t pmap,
	vm_map_address_t v,
	boolean_t wired), PMAP_CHANGE_WIRING_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	pmap_t,
	pmap_create_options, (ledger_t ledger,
	vm_map_size_t size,
	unsigned int flags), PMAP_CREATE_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_destroy, (pmap_t pmap), PMAP_DESTROY_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	pmap_enter_options, (pmap_t pmap,
	vm_map_address_t v,
	ppnum_t pn,
	vm_prot_t prot,
	vm_prot_t fault_type,
	unsigned int flags,
	boolean_t wired,
	unsigned int options), PMAP_ENTER_OPTIONS_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	vm_offset_t,
	pmap_extract, (pmap_t pmap,
	vm_map_address_t va), PMAP_EXTRACT_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	ppnum_t,
	pmap_find_phys, (pmap_t pmap,
	addr64_t va), PMAP_FIND_PHYS_INDEX);

#if (__ARM_VMSA__ > 7)
PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	pmap_insert_sharedpage, (pmap_t pmap), PMAP_INSERT_SHAREDPAGE_INDEX);
#endif


PMAP_SUPPORT_PROTOTYPES(
	boolean_t,
	pmap_is_empty, (pmap_t pmap,
	vm_map_offset_t va_start,
	vm_map_offset_t va_end), PMAP_IS_EMPTY_INDEX);


PMAP_SUPPORT_PROTOTYPES(
	unsigned int,
	pmap_map_cpu_windows_copy, (ppnum_t pn,
	vm_prot_t prot,
	unsigned int wimg_bits), PMAP_MAP_CPU_WINDOWS_COPY_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	pmap_nest, (pmap_t grand,
	pmap_t subord,
	addr64_t vstart,
	addr64_t nstart,
	uint64_t size), PMAP_NEST_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_page_protect_options, (ppnum_t ppnum,
	vm_prot_t prot,
	unsigned int options), PMAP_PAGE_PROTECT_OPTIONS_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_protect_options, (pmap_t pmap,
	vm_map_address_t start,
	vm_map_address_t end,
	vm_prot_t prot,
	unsigned int options,
	void *args), PMAP_PROTECT_OPTIONS_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	pmap_query_page_info, (pmap_t pmap,
	vm_map_offset_t va,
	int *disp_p), PMAP_QUERY_PAGE_INFO_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	mach_vm_size_t,
	pmap_query_resident, (pmap_t pmap,
	vm_map_address_t start,
	vm_map_address_t end,
	mach_vm_size_t * compressed_bytes_p), PMAP_QUERY_RESIDENT_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_reference, (pmap_t pmap), PMAP_REFERENCE_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	int,
	pmap_remove_options, (pmap_t pmap,
	vm_map_address_t start,
	vm_map_address_t end,
	int options), PMAP_REMOVE_OPTIONS_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	pmap_return, (boolean_t do_panic,
	boolean_t do_recurse), PMAP_RETURN_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_set_cache_attributes, (ppnum_t pn,
	unsigned int cacheattr), PMAP_SET_CACHE_ATTRIBUTES_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_update_compressor_page, (ppnum_t pn,
	unsigned int prev_cacheattr, unsigned int new_cacheattr), PMAP_UPDATE_COMPRESSOR_PAGE_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_set_nested, (pmap_t pmap), PMAP_SET_NESTED_INDEX);

#if MACH_ASSERT || XNU_MONITOR
PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_set_process, (pmap_t pmap,
	int pid,
	char *procname), PMAP_SET_PROCESS_INDEX);
#endif

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_unmap_cpu_windows_copy, (unsigned int index), PMAP_UNMAP_CPU_WINDOWS_COPY_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	kern_return_t,
	pmap_unnest_options, (pmap_t grand,
	addr64_t vaddr,
	uint64_t size,
	unsigned int option), PMAP_UNNEST_OPTIONS_INDEX);

#if XNU_MONITOR
PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_cpu_data_init, (unsigned int cpu_number), PMAP_CPU_DATA_INIT_INDEX);
#endif

PMAP_SUPPORT_PROTOTYPES(
	void,
	phys_attribute_set, (ppnum_t pn,
	unsigned int bits), PHYS_ATTRIBUTE_SET_INDEX);

#if XNU_MONITOR
PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_mark_page_as_ppl_page, (pmap_paddr_t pa), PMAP_MARK_PAGE_AS_PMAP_PAGE_INDEX);
#endif

PMAP_SUPPORT_PROTOTYPES(
	void,
	phys_attribute_clear, (ppnum_t pn,
	unsigned int bits,
	int options,
	void *arg), PHYS_ATTRIBUTE_CLEAR_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_switch, (pmap_t pmap), PMAP_SWITCH_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_switch_user_ttb, (pmap_t pmap), PMAP_SWITCH_USER_TTB_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_clear_user_ttb, (void), PMAP_CLEAR_USER_TTB_INDEX);

#if XNU_MONITOR
PMAP_SUPPORT_PROTOTYPES(
	uint64_t,
	pmap_release_ppl_pages_to_kernel, (void), PMAP_RELEASE_PAGES_TO_KERNEL_INDEX);
#endif

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_set_jit_entitled, (pmap_t pmap), PMAP_SET_JIT_ENTITLED_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_trim, (pmap_t grand,
	pmap_t subord,
	addr64_t vstart,
	addr64_t nstart,
	uint64_t size), PMAP_TRIM_INDEX);

#if HAS_APPLE_PAC && XNU_MONITOR
PMAP_SUPPORT_PROTOTYPES(
	void *,
	pmap_sign_user_ptr, (void *value, ptrauth_key key, uint64_t discriminator), PMAP_SIGN_USER_PTR);
PMAP_SUPPORT_PROTOTYPES(
	void *,
	pmap_auth_user_ptr, (void *value, ptrauth_key key, uint64_t discriminator), PMAP_AUTH_USER_PTR);
#endif /* HAS_APPLE_PAC && XNU_MONITOR */




#if XNU_MONITOR
static void pmap_mark_page_as_ppl_page(pmap_paddr_t pa);
#endif

void pmap_footprint_suspend(vm_map_t    map,
    boolean_t   suspend);
PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_footprint_suspend, (vm_map_t map,
	boolean_t suspend),
	PMAP_FOOTPRINT_SUSPEND_INDEX);

#if XNU_MONITOR
PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_ledger_alloc_init, (size_t),
	PMAP_LEDGER_ALLOC_INIT_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	ledger_t,
	pmap_ledger_alloc, (void),
	PMAP_LEDGER_ALLOC_INDEX);

PMAP_SUPPORT_PROTOTYPES(
	void,
	pmap_ledger_free, (ledger_t),
	PMAP_LEDGER_FREE_INDEX);
#endif

#if CONFIG_PGTRACE
boolean_t pgtrace_enabled = 0;

typedef struct {
	queue_chain_t   chain;

	/*
	 *   pmap        - pmap for below addresses
	 *   ova         - original va page address
	 *   cva         - clone va addresses for pre, target and post pages
	 *   cva_spte    - clone saved ptes
	 *   range       - trace range in this map
	 *   cloned      - has been cloned or not
	 */
	pmap_t          pmap;
	vm_map_offset_t ova;
	vm_map_offset_t cva[3];
	pt_entry_t      cva_spte[3];
	struct {
		pmap_paddr_t    start;
		pmap_paddr_t    end;
	} range;
	bool            cloned;
} pmap_pgtrace_map_t;

static void pmap_pgtrace_init(void);
static bool pmap_pgtrace_enter_clone(pmap_t pmap, vm_map_offset_t va_page, vm_map_offset_t start, vm_map_offset_t end);
static void pmap_pgtrace_remove_clone(pmap_t pmap, pmap_paddr_t pa_page, vm_map_offset_t va_page);
static void pmap_pgtrace_remove_all_clone(pmap_paddr_t pa);
#endif

#if     (__ARM_VMSA__ > 7)
/*
 * The low global vector page is mapped at a fixed alias.
 * Since the page size is 16k for H8 and newer we map the globals to a 16k
 * aligned address. Readers of the globals (e.g. lldb, panic server) need
 * to check both addresses anyway for backward compatibility. So for now
 * we leave H6 and H7 where they were.
 */
#if (ARM_PGSHIFT == 14)
#define LOWGLOBAL_ALIAS         (LOW_GLOBAL_BASE_ADDRESS + 0x4000)
#else
#define LOWGLOBAL_ALIAS         (LOW_GLOBAL_BASE_ADDRESS + 0x2000)
#endif

#else
#define LOWGLOBAL_ALIAS         (0xFFFF1000)
#endif

long long alloc_tteroot_count __attribute__((aligned(8))) MARK_AS_PMAP_DATA = 0LL;
long long alloc_ttepages_count __attribute__((aligned(8))) MARK_AS_PMAP_DATA = 0LL;
long long alloc_ptepages_count __attribute__((aligned(8))) MARK_AS_PMAP_DATA = 0LL;
long long alloc_pmap_pages_count __attribute__((aligned(8))) = 0LL;

int pt_fake_zone_index = -1;            /* index of pmap fake zone */

#if XNU_MONITOR
/*
 * Table of function pointers used for PPL dispatch.
 */
const void * const ppl_handler_table[PMAP_COUNT] = {
	[ARM_FAST_FAULT_INDEX] = arm_fast_fault_internal,
	[ARM_FORCE_FAST_FAULT_INDEX] = arm_force_fast_fault_internal,
	[MAPPING_FREE_PRIME_INDEX] = mapping_free_prime_internal,
	[MAPPING_REPLENISH_INDEX] = mapping_replenish_internal,
	[PHYS_ATTRIBUTE_CLEAR_INDEX] = phys_attribute_clear_internal,
	[PHYS_ATTRIBUTE_SET_INDEX] = phys_attribute_set_internal,
	[PMAP_BATCH_SET_CACHE_ATTRIBUTES_INDEX] = pmap_batch_set_cache_attributes_internal,
	[PMAP_CHANGE_WIRING_INDEX] = pmap_change_wiring_internal,
	[PMAP_CREATE_INDEX] = pmap_create_options_internal,
	[PMAP_DESTROY_INDEX] = pmap_destroy_internal,
	[PMAP_ENTER_OPTIONS_INDEX] = pmap_enter_options_internal,
	[PMAP_EXTRACT_INDEX] = pmap_extract_internal,
	[PMAP_FIND_PHYS_INDEX] = pmap_find_phys_internal,
	[PMAP_INSERT_SHAREDPAGE_INDEX] = pmap_insert_sharedpage_internal,
	[PMAP_IS_EMPTY_INDEX] = pmap_is_empty_internal,
	[PMAP_MAP_CPU_WINDOWS_COPY_INDEX] = pmap_map_cpu_windows_copy_internal,
	[PMAP_MARK_PAGE_AS_PMAP_PAGE_INDEX] = pmap_mark_page_as_ppl_page_internal,
	[PMAP_NEST_INDEX] = pmap_nest_internal,
	[PMAP_PAGE_PROTECT_OPTIONS_INDEX] = pmap_page_protect_options_internal,
	[PMAP_PROTECT_OPTIONS_INDEX] = pmap_protect_options_internal,
	[PMAP_QUERY_PAGE_INFO_INDEX] = pmap_query_page_info_internal,
	[PMAP_QUERY_RESIDENT_INDEX] = pmap_query_resident_internal,
	[PMAP_REFERENCE_INDEX] = pmap_reference_internal,
	[PMAP_REMOVE_OPTIONS_INDEX] = pmap_remove_options_internal,
	[PMAP_RETURN_INDEX] = pmap_return_internal,
	[PMAP_SET_CACHE_ATTRIBUTES_INDEX] = pmap_set_cache_attributes_internal,
	[PMAP_UPDATE_COMPRESSOR_PAGE_INDEX] = pmap_update_compressor_page_internal,
	[PMAP_SET_NESTED_INDEX] = pmap_set_nested_internal,
	[PMAP_SET_PROCESS_INDEX] = pmap_set_process_internal,
	[PMAP_SWITCH_INDEX] = pmap_switch_internal,
	[PMAP_SWITCH_USER_TTB_INDEX] = pmap_switch_user_ttb_internal,
	[PMAP_CLEAR_USER_TTB_INDEX] = pmap_clear_user_ttb_internal,
	[PMAP_UNMAP_CPU_WINDOWS_COPY_INDEX] = pmap_unmap_cpu_windows_copy_internal,
	[PMAP_UNNEST_OPTIONS_INDEX] = pmap_unnest_options_internal,
	[PMAP_FOOTPRINT_SUSPEND_INDEX] = pmap_footprint_suspend_internal,
	[PMAP_CPU_DATA_INIT_INDEX] = pmap_cpu_data_init_internal,
	[PMAP_RELEASE_PAGES_TO_KERNEL_INDEX] = pmap_release_ppl_pages_to_kernel_internal,
	[PMAP_SET_JIT_ENTITLED_INDEX] = pmap_set_jit_entitled_internal,
	[PMAP_TRIM_INDEX] = pmap_trim_internal,
	[PMAP_LEDGER_ALLOC_INIT_INDEX] = pmap_ledger_alloc_init_internal,
	[PMAP_LEDGER_ALLOC_INDEX] = pmap_ledger_alloc_internal,
	[PMAP_LEDGER_FREE_INDEX] = pmap_ledger_free_internal,
#if HAS_APPLE_PAC && XNU_MONITOR
	[PMAP_SIGN_USER_PTR] = pmap_sign_user_ptr_internal,
	[PMAP_AUTH_USER_PTR] = pmap_auth_user_ptr_internal,
#endif /* HAS_APPLE_PAC && XNU_MONITOR */
};

static uint64_t
pmap_get_ppl_cpu_id(void)
{
	uint64_t mpidr_el1_value = 0;

	/* We identify the CPU based on the constant bits of MPIDR_EL1. */
	MRS(mpidr_el1_value, "MPIDR_EL1");

#ifdef CPU_CLUSTER_OFFSETS
	uint64_t cluster_id = (mpidr_el1_value & MPIDR_AFF1_MASK) >> MPIDR_AFF1_SHIFT;
	assert(cluster_id < (sizeof(pmap_cluster_offsets) / sizeof(pmap_cluster_offsets[0])));

	/* For multi-cluster configurations, AFF0 reflects the core number within the cluster. */
	mpidr_el1_value = (mpidr_el1_value & MPIDR_AFF0_MASK) + pmap_cluster_offsets[cluster_id];
#else
	/*
	 * AFF2 is not constant (it can change for e-core versus p-core on H9),
	 * so mask it out.
	 */
	mpidr_el1_value &= MPIDR_AFF0_MASK;
#endif

	if (mpidr_el1_value > MAX_CPUS) {
		panic("%s: mpidr_el1_value=%#llx > MAX_CPUS=%#x",
		    __FUNCTION__, mpidr_el1_value, MAX_CPUS);
	}

	return mpidr_el1_value;
}


#endif


/*
 * Allocates and initializes a per-CPU data structure for the pmap.
 */
MARK_AS_PMAP_TEXT static void
pmap_cpu_data_init_internal(unsigned int cpu_number)
{
	pmap_cpu_data_t * pmap_cpu_data = pmap_get_cpu_data();

#if XNU_MONITOR
	/* Verify cacheline-aligned */
	assert(((vm_offset_t)pmap_cpu_data & ((1 << L2_CLINE) - 1)) == 0);
	if (pmap_cpu_data->cpu_number != PMAP_INVALID_CPU_NUM) {
		panic("%s: pmap_cpu_data->cpu_number=%u, "
		    "cpu_number=%u",
		    __FUNCTION__, pmap_cpu_data->cpu_number,
		    cpu_number);
	}
#endif
	pmap_cpu_data->cpu_number = cpu_number;
}

void
pmap_cpu_data_init(void)
{
#if XNU_MONITOR
	pmap_cpu_data_init_ppl(cpu_number());
#else
	pmap_cpu_data_init_internal(cpu_number());
#endif
}

static void
pmap_cpu_data_array_init(void)
{
#if XNU_MONITOR
	unsigned int i = 0;
	pmap_paddr_t ppl_cpu_save_area_cur = 0;
	pt_entry_t template, *pte_p;
	vm_offset_t stack_va = (vm_offset_t)pmap_stacks_start + ARM_PGBYTES;
	assert((pmap_stacks_start != NULL) && (pmap_stacks_end != NULL));
	pmap_stacks_start_pa = avail_start;

	for (i = 0; i < MAX_CPUS; i++) {
		for (vm_offset_t cur_va = stack_va; cur_va < (stack_va + PPL_STACK_SIZE); cur_va += ARM_PGBYTES) {
			assert(cur_va < (vm_offset_t)pmap_stacks_end);
			pte_p = pmap_pte(kernel_pmap, cur_va);
			assert(*pte_p == ARM_PTE_EMPTY);
			template = pa_to_pte(avail_start) | ARM_PTE_AF | ARM_PTE_SH(SH_OUTER_MEMORY) | ARM_PTE_TYPE |
			    ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DEFAULT) | xprr_perm_to_pte(XPRR_PPL_RW_PERM);
#if __ARM_KERNEL_PROTECT__
			template |= ARM_PTE_NG;
#endif /* __ARM_KERNEL_PROTECT__ */
			WRITE_PTE(pte_p, template);
			__builtin_arm_isb(ISB_SY);
			avail_start += ARM_PGBYTES;
		}
#if KASAN
		kasan_map_shadow(stack_va, PPL_STACK_SIZE, false);
#endif
		pmap_cpu_data_array[i].cpu_data.cpu_id = i;
		pmap_cpu_data_array[i].cpu_data.cpu_number = PMAP_INVALID_CPU_NUM;
		pmap_cpu_data_array[i].cpu_data.ppl_state = PPL_STATE_KERNEL;
		pmap_cpu_data_array[i].cpu_data.ppl_stack = (void*)(stack_va + PPL_STACK_SIZE);
		stack_va += (PPL_STACK_SIZE + ARM_PGBYTES);
	}
	sync_tlb_flush();
	pmap_stacks_end_pa = avail_start;

	ppl_cpu_save_area_start = avail_start;
	ppl_cpu_save_area_end = ppl_cpu_save_area_start;
	ppl_cpu_save_area_cur = ppl_cpu_save_area_start;

	for (i = 0; i < MAX_CPUS; i++) {
		while ((ppl_cpu_save_area_end - ppl_cpu_save_area_cur) < sizeof(arm_context_t)) {
			avail_start += PAGE_SIZE;
			ppl_cpu_save_area_end = avail_start;
		}

		pmap_cpu_data_array[i].cpu_data.save_area = (arm_context_t *)phystokv(ppl_cpu_save_area_cur);
		ppl_cpu_save_area_cur += sizeof(arm_context_t);
	}
#endif

	pmap_cpu_data_init();
}

pmap_cpu_data_t *
pmap_get_cpu_data(void)
{
	pmap_cpu_data_t * pmap_cpu_data = NULL;

#if XNU_MONITOR
	uint64_t cpu_id = 0;

	cpu_id = pmap_get_ppl_cpu_id();
	pmap_cpu_data = &pmap_cpu_data_array[cpu_id].cpu_data;

	if (pmap_cpu_data->cpu_id != cpu_id) {
		panic("%s: CPU ID mismatch, cpu_id=0x%#llx, pmap_cpu_data->cpu_id=%#llx",
		    __FUNCTION__, cpu_id, pmap_cpu_data->cpu_id);
	}
#else
	pmap_cpu_data = &getCpuDatap()->cpu_pmap_cpu_data;
#endif

	return pmap_cpu_data;
}

#if XNU_MONITOR
/*
 * pmap_set_range_xprr_perm takes a range (specified using start and end) that
 * falls within the physical aperture.  All mappings within this range have
 * their protections changed from those specified by the expected_perm to those
 * specified by the new_perm.
 */
static void
pmap_set_range_xprr_perm(vm_address_t start,
    vm_address_t end,
    unsigned int expected_perm,
    unsigned int new_perm)
{
#if (__ARM_VMSA__ == 7)
#error This function is not supported on older ARM hardware
#else
	pmap_t pmap = NULL;

	vm_address_t va = 0;
	vm_address_t tte_start = 0;
	vm_address_t tte_end = 0;

	tt_entry_t *tte_p = NULL;
	pt_entry_t *pte_p = NULL;
	pt_entry_t *cpte_p = NULL;
	pt_entry_t *bpte_p = NULL;
	pt_entry_t *epte_p = NULL;

	tt_entry_t tte = 0;
	pt_entry_t cpte = 0;
	pt_entry_t template = 0;

	pmap = kernel_pmap;

	va = start;

	/*
	 * Validate our arguments; any invalid argument will be grounds for a
	 * panic.
	 */
	if ((start | end) % ARM_PGBYTES) {
		panic("%s: start or end not page aligned, "
		    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
		    __FUNCTION__,
		    (void *)start, (void *)end, new_perm, expected_perm);
	}

	if (start > end) {
		panic("%s: start > end, "
		    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
		    __FUNCTION__,
		    (void *)start, (void *)end, new_perm, expected_perm);
	}

	if (start < gVirtBase) {
		panic("%s: start is before physical aperture, "
		    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
		    __FUNCTION__,
		    (void *)start, (void *)end, new_perm, expected_perm);
	}

	if (end > static_memory_end) {
		panic("%s: end is after physical aperture, "
		    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
		    __FUNCTION__,
		    (void *)start, (void *)end, new_perm, expected_perm);
	}

	if ((new_perm > XPRR_MAX_PERM) || (expected_perm > XPRR_MAX_PERM)) {
		panic("%s: invalid XPRR index, "
		    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
		    __FUNCTION__,
		    (void *)start, (void *)end, new_perm, expected_perm);
	}

	/*
	 * Walk over the PTEs for the given range, and set the protections on
	 * those PTEs.
	 */
	while (va < end) {
		tte_start = va;
		tte_end = ((va + pt_attr_twig_size(native_pt_attr)) & ~pt_attr_twig_offmask(native_pt_attr));

		if (tte_end > end) {
			tte_end = end;
		}

		tte_p = pmap_tte(pmap, va);

		/*
		 * The physical aperture should not have holes.
		 * The physical aperture should be contiguous.
		 * Do not make eye contact with the physical aperture.
		 */
		if (tte_p == NULL) {
			panic("%s: physical aperture tte is NULL, "
			    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
			    __FUNCTION__,
			    (void *)start, (void *)end, new_perm, expected_perm);
		}

		tte = *tte_p;

		if ((tte & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
			/*
			 * Walk over the given L3 page table page and update the
			 * PTEs.
			 */
			pte_p = (pt_entry_t *)ttetokv(tte);
			bpte_p = &pte_p[ptenum(va)];
			epte_p = bpte_p + ((tte_end - va) >> pt_attr_leaf_shift(native_pt_attr));

			for (cpte_p = bpte_p; cpte_p < epte_p;
			    cpte_p += PAGE_SIZE / ARM_PGBYTES, va += PAGE_SIZE) {
				int pai = (int)pa_index(pte_to_pa(*cpte_p));
				LOCK_PVH(pai);
				cpte = *cpte_p;

				/*
				 * Every PTE involved should be valid, should
				 * not have the hint bit set, and should have
				 * Every valid PTE involved should
				 * not have the hint bit set and should have
				 * the expected APRR index.
				 */
				if ((cpte & ARM_PTE_TYPE_MASK) ==
				    ARM_PTE_TYPE_FAULT) {
					panic("%s: physical aperture PTE is invalid, va=%p, "
					    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
					    __FUNCTION__,
					    (void *)va,
					    (void *)start, (void *)end, new_perm, expected_perm);
					UNLOCK_PVH(pai);
					continue;
				}

				if (cpte & ARM_PTE_HINT_MASK) {
					panic("%s: physical aperture PTE has hint bit set, va=%p, cpte=0x%llx, "
					    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
					    __FUNCTION__,
					    (void *)va, cpte,
					    (void *)start, (void *)end, new_perm, expected_perm);
				}

				if (pte_to_xprr_perm(cpte) != expected_perm) {
					panic("%s: perm=%llu does not match expected_perm, cpte=0x%llx, "
					    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
					    __FUNCTION__,
					    pte_to_xprr_perm(cpte), cpte,
					    (void *)start, (void *)end, new_perm, expected_perm);
				}

				template = cpte;
				template &= ~ARM_PTE_XPRR_MASK;
				template |= xprr_perm_to_pte(new_perm);

				WRITE_PTE_STRONG(cpte_p, template);
				UNLOCK_PVH(pai);
			}
		} else {
			panic("%s: tte=0x%llx is not a table type entry, "
			    "start=%p, end=%p, new_perm=%u, expected_perm=%u",
			    __FUNCTION__,
			    tte,
			    (void *)start, (void *)end, new_perm, expected_perm);
		}

		va = tte_end;
	}

	PMAP_UPDATE_TLBS(pmap, start, end, false);
#endif /* (__ARM_VMSA__ == 7) */
}

/*
 * A convenience function for setting protections on a single page.
 */
static inline void
pmap_set_xprr_perm(vm_address_t page_kva,
    unsigned int expected_perm,
    unsigned int new_perm)
{
	pmap_set_range_xprr_perm(page_kva, page_kva + PAGE_SIZE, expected_perm, new_perm);
}
#endif /* XNU_MONITOR */


/* TODO */
pmap_paddr_t
pmap_pages_reclaim(
	void)
{
	boolean_t               found_page;
	unsigned                i;
	pt_desc_t               *ptdp;

	/*
	 * pmap_pages_reclaim() is returning a page by freeing an active pt page.
	 * To be eligible, a pt page is assigned to a user pmap. It doesn't have any wired pte
	 * entry and it  contains at least one valid pte entry.
	 *
	 * In a loop, check for a page in the reclaimed pt page list.
	 * if one is present, unlink that page and return the physical page address.
	 * Otherwise, scan the pt page list for an eligible pt page to reclaim.
	 * If found, invoke pmap_remove_range() on its pmap and address range then
	 * deallocates that pt page. This will end up adding the pt page to the
	 * reclaimed pt page list.
	 * If no eligible page were found in the pt page list, panic.
	 */

	pmap_simple_lock(&pmap_pages_lock);
	pmap_pages_request_count++;
	pmap_pages_request_acum++;

	while (1) {
		if (pmap_pages_reclaim_list != (page_free_entry_t *)NULL) {
			page_free_entry_t       *page_entry;

			page_entry = pmap_pages_reclaim_list;
			pmap_pages_reclaim_list = pmap_pages_reclaim_list->next;
			pmap_simple_unlock(&pmap_pages_lock);

			return (pmap_paddr_t)ml_static_vtop((vm_offset_t)page_entry);
		}

		pmap_simple_unlock(&pmap_pages_lock);

		pmap_simple_lock(&pt_pages_lock);
		ptdp = (pt_desc_t *)queue_first(&pt_page_list);
		found_page = FALSE;

		while (!queue_end(&pt_page_list, (queue_entry_t)ptdp)) {
			if ((ptdp->pmap->nested == FALSE)
			    && (pmap_simple_lock_try(&ptdp->pmap->lock))) {
				assert(ptdp->pmap != kernel_pmap);
				unsigned refcnt_acc = 0;
				unsigned wiredcnt_acc = 0;

				for (i = 0; i < PT_INDEX_MAX; i++) {
					if (ptdp->ptd_info[i].refcnt == PT_DESC_REFCOUNT) {
						/* Do not attempt to free a page that contains an L2 table */
						refcnt_acc = 0;
						break;
					}
					refcnt_acc += ptdp->ptd_info[i].refcnt;
					wiredcnt_acc += ptdp->ptd_info[i].wiredcnt;
				}
				if ((wiredcnt_acc == 0) && (refcnt_acc != 0)) {
					found_page = TRUE;
					/* Leave ptdp->pmap locked here.  We're about to reclaim
					 * a tt page from it, so we don't want anyone else messing
					 * with it while we do that. */
					break;
				}
				pmap_simple_unlock(&ptdp->pmap->lock);
			}
			ptdp = (pt_desc_t *)queue_next((queue_t)ptdp);
		}
		if (!found_page) {
			panic("%s: No eligible page in pt_page_list", __FUNCTION__);
		} else {
			int                     remove_count = 0;
			bool                    need_strong_sync = false;
			vm_map_address_t        va;
			pmap_t                  pmap;
			pt_entry_t              *bpte, *epte;
			pt_entry_t              *pte_p;
			tt_entry_t              *tte_p;
			uint32_t                rmv_spte = 0;

			pmap_simple_unlock(&pt_pages_lock);
			pmap = ptdp->pmap;
			PMAP_ASSERT_LOCKED(pmap); // pmap lock should be held from loop above

			__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

			for (i = 0; i < PT_INDEX_MAX; i++) {
				va = ptdp->ptd_info[i].va;

				/* If the VA is bogus, this may represent an unallocated region
				 * or one which is in transition (already being freed or expanded).
				 * Don't try to remove mappings here. */
				if (va == (vm_offset_t)-1) {
					continue;
				}

				tte_p = pmap_tte(pmap, va);
				if ((tte_p != (tt_entry_t *) NULL)
				    && ((*tte_p & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE)) {
					pte_p = (pt_entry_t *) ttetokv(*tte_p);
					bpte = &pte_p[pte_index(pmap, pt_attr, va)];
					epte = bpte + PAGE_SIZE / sizeof(pt_entry_t);
					/*
					 * Use PMAP_OPTIONS_REMOVE to clear any
					 * "compressed" markers and update the
					 * "compressed" counter in pmap->stats.
					 * This means that we lose accounting for
					 * any compressed pages in this range
					 * but the alternative is to not be able
					 * to account for their future decompression,
					 * which could cause the counter to drift
					 * more and more.
					 */
					remove_count += pmap_remove_range_options(
						pmap, va, bpte, epte,
						&rmv_spte, &need_strong_sync, PMAP_OPTIONS_REMOVE);
					if (ptdp->ptd_info[ARM_PT_DESC_INDEX(pte_p)].refcnt != 0) {
						panic("%s: ptdp %p, count %d", __FUNCTION__, ptdp, ptdp->ptd_info[ARM_PT_DESC_INDEX(pte_p)].refcnt);
					}

					pmap_tte_deallocate(pmap, tte_p, PMAP_TT_TWIG_LEVEL);

					if (remove_count > 0) {
						pmap_get_pt_ops(pmap)->flush_tlb_region_async(va, (unsigned int)pt_attr_leaf_table_size(pt_attr), pmap);
					} else {
						pmap_get_pt_ops(pmap)->flush_tlb_tte_async(va, pmap);
					}
				}
			}
			// Undo the lock we grabbed when we found ptdp above
			PMAP_UNLOCK(pmap);
			pmap_sync_tlb(need_strong_sync);
		}
		pmap_simple_lock(&pmap_pages_lock);
	}
}

#if XNU_MONITOR
/*
 * Return a PPL page to the free list.
 */
static void
pmap_give_free_ppl_page(pmap_paddr_t paddr)
{
	assert((paddr & ARM_PGMASK) == 0);
	void ** new_head = (void **)phystokv(paddr);
	pmap_simple_lock(&pmap_ppl_free_page_lock);

	void * cur_head = pmap_ppl_free_page_list;
	*new_head = cur_head;
	pmap_ppl_free_page_list = new_head;
	pmap_ppl_free_page_count++;

	pmap_simple_unlock(&pmap_ppl_free_page_lock);
}

/*
 * Get a PPL page from the free list.
 */
static pmap_paddr_t
pmap_get_free_ppl_page(void)
{
	pmap_paddr_t result = 0;

	pmap_simple_lock(&pmap_ppl_free_page_lock);

	if (pmap_ppl_free_page_list != NULL) {
		void ** new_head = NULL;
		new_head = *((void**)pmap_ppl_free_page_list);
		result = kvtophys((vm_offset_t)pmap_ppl_free_page_list);
		pmap_ppl_free_page_list = new_head;
		pmap_ppl_free_page_count--;
	} else {
		result = 0L;
	}

	pmap_simple_unlock(&pmap_ppl_free_page_lock);
	assert((result & ARM_PGMASK) == 0);

	return result;
}

/*
 * pmap_mark_page_as_ppl_page claims a page on behalf of the PPL by marking it
 * as PPL-owned and only allowing the PPL to write to it.
 */
MARK_AS_PMAP_TEXT static void
pmap_mark_page_as_ppl_page_internal(pmap_paddr_t pa)
{
	vm_offset_t kva = 0;
	unsigned int pai = 0;
	pp_attr_t attr;

	/*
	 * Mark each page that we allocate as belonging to the monitor, as we
	 * intend to use it for monitor-y stuff (page tables, table pages, that
	 * sort of thing).
	 */
	assert(!TEST_PAGE_RATIO_4);

	if (!pa_valid(pa)) {
		panic("%s: bad address, "
		    "pa=%p",
		    __func__,
		    (void *)pa);
	}

	pai = (unsigned int)pa_index(pa);
	LOCK_PVH(pai);

	/* A page that the PPL already owns can't be given to the PPL. */
	if (pa_test_monitor(pa)) {
		panic("%s: page already belongs to PPL, "
		    "pa=0x%llx",
		    __FUNCTION__,
		    pa);
	}
	/* The page cannot be mapped outside of the physical aperture. */
	if (!pmap_verify_free((ppnum_t)atop(pa))) {
		panic("%s: page is not free, "
		    "pa=0x%llx",
		    __FUNCTION__,
		    pa);
	}

	do {
		attr = pp_attr_table[pai];
		if (attr & PP_ATTR_NO_MONITOR) {
			panic("%s: page excluded from PPL, "
			    "pa=0x%llx",
			    __FUNCTION__,
			    pa);
		}
	} while (!OSCompareAndSwap16(attr, attr | PP_ATTR_MONITOR, &pp_attr_table[pai]));

	UNLOCK_PVH(pai);

	kva = phystokv(pa);
	pmap_set_xprr_perm(kva, XPRR_KERN_RW_PERM, XPRR_PPL_RW_PERM);
	bzero((void *)(kva & ~PAGE_MASK), PAGE_SIZE);

	pmap_give_free_ppl_page(pa);
}

static void
pmap_mark_page_as_ppl_page(pmap_paddr_t pa)
{
	pmap_mark_page_as_ppl_page_ppl(pa);
}

static void
pmap_mark_page_as_kernel_page(pmap_paddr_t pa)
{
	vm_offset_t kva = 0;
	unsigned int pai = 0;

	pai = (unsigned int)pa_index(pa);
	LOCK_PVH(pai);

	if (!pa_test_monitor(pa)) {
		panic("%s: page is not a PPL page, "
		    "pa=%p",
		    __FUNCTION__,
		    (void *)pa);
	}

	pa_clear_monitor(pa);
	UNLOCK_PVH(pai);

	kva = phystokv(pa);
	pmap_set_xprr_perm(kva, XPRR_PPL_RW_PERM, XPRR_KERN_RW_PERM);
}

MARK_AS_PMAP_TEXT static pmap_paddr_t
pmap_release_ppl_pages_to_kernel_internal(void)
{
	pmap_paddr_t pa = 0;

	if (pmap_ppl_free_page_count <= PMAP_MIN_FREE_PPL_PAGES) {
		goto done;
	}

	pa = pmap_get_free_ppl_page();

	if (!pa) {
		goto done;
	}

	pmap_mark_page_as_kernel_page(pa);

done:
	return pa;
}

static uint64_t
pmap_release_ppl_pages_to_kernel(void)
{
	pmap_paddr_t pa          = 0;
	vm_page_t    m           = VM_PAGE_NULL;
	vm_page_t    local_freeq = VM_PAGE_NULL;
	uint64_t     pmap_ppl_pages_returned_to_kernel_count = 0;

	while (pmap_ppl_free_page_count > PMAP_MIN_FREE_PPL_PAGES) {
		pa = pmap_release_ppl_pages_to_kernel_ppl();

		if (!pa) {
			break;
		}

		/* If we retrieved a page, add it to the free queue. */
		vm_object_lock(pmap_object);
		m = vm_page_lookup(pmap_object, (pa - gPhysBase));
		assert(m != VM_PAGE_NULL);
		assert(VM_PAGE_WIRED(m));

		m->vmp_busy = TRUE;
		m->vmp_snext = local_freeq;
		local_freeq = m;
		pmap_ppl_pages_returned_to_kernel_count++;
		pmap_ppl_pages_returned_to_kernel_count_total++;

		vm_object_unlock(pmap_object);
	}

	if (local_freeq) {
		/* We need to hold the object lock for freeing pages. */
		vm_object_lock(pmap_object);
		vm_page_free_list(local_freeq, TRUE);
		vm_object_unlock(pmap_object);
	}

	return pmap_ppl_pages_returned_to_kernel_count;
}
#endif

static kern_return_t
pmap_pages_alloc(
	pmap_paddr_t    *pa,
	unsigned                size,
	unsigned                option)
{
#if XNU_MONITOR
	if (size != PAGE_SIZE) {
		panic("%s: size != PAGE_SIZE, "
		    "pa=%p, size=%u, option=%u",
		    __FUNCTION__,
		    pa, size, option);
	}

	if (option & PMAP_PAGES_RECLAIM_NOWAIT) {
		*pa = pmap_pages_reclaim();
		assert(*pa);
		return KERN_SUCCESS;
	}

	assert(option & PMAP_PAGES_ALLOCATE_NOWAIT);

	*pa = pmap_get_free_ppl_page();

	if (*pa == 0) {
		return KERN_RESOURCE_SHORTAGE;
	} else {
		return KERN_SUCCESS;
	}
#else
	vm_page_t       m = VM_PAGE_NULL, m_prev;

	if (option & PMAP_PAGES_RECLAIM_NOWAIT) {
		assert(size == PAGE_SIZE);
		*pa = pmap_pages_reclaim();
		return KERN_SUCCESS;
	}
	if (size == PAGE_SIZE) {
		while ((m = vm_page_grab()) == VM_PAGE_NULL) {
			if (option & PMAP_PAGES_ALLOCATE_NOWAIT) {
				return KERN_RESOURCE_SHORTAGE;
			}

			VM_PAGE_WAIT();
		}
		vm_page_lock_queues();
		vm_page_wire(m, VM_KERN_MEMORY_PTE, TRUE);
		vm_page_unlock_queues();
	}
	if (size == 2 * PAGE_SIZE) {
		while (cpm_allocate(size, &m, 0, 1, TRUE, 0) != KERN_SUCCESS) {
			if (option & PMAP_PAGES_ALLOCATE_NOWAIT) {
				return KERN_RESOURCE_SHORTAGE;
			}

			VM_PAGE_WAIT();
		}
	}

	*pa = (pmap_paddr_t)ptoa(VM_PAGE_GET_PHYS_PAGE(m));

	vm_object_lock(pmap_object);
	while (m != VM_PAGE_NULL) {
		vm_page_insert_wired(m, pmap_object, (vm_object_offset_t) ((ptoa(VM_PAGE_GET_PHYS_PAGE(m))) - gPhysBase), VM_KERN_MEMORY_PTE);
		m_prev = m;
		m = NEXT_PAGE(m_prev);
		*(NEXT_PAGE_PTR(m_prev)) = VM_PAGE_NULL;
	}
	vm_object_unlock(pmap_object);

	OSAddAtomic(size >> PAGE_SHIFT, &inuse_pmap_pages_count);
	OSAddAtomic64(size >> PAGE_SHIFT, &alloc_pmap_pages_count);

	return KERN_SUCCESS;
#endif
}

#if XNU_MONITOR
static pmap_paddr_t
pmap_alloc_page_for_kern(void)
{
	pmap_paddr_t paddr = 0;
	vm_page_t    m, m_prev;

	while ((m = vm_page_grab()) == VM_PAGE_NULL) {
		VM_PAGE_WAIT();
	}

	vm_page_lock_queues();
	vm_page_wire(m, VM_KERN_MEMORY_PTE, TRUE);
	vm_page_unlock_queues();

	paddr = (pmap_paddr_t)ptoa(VM_PAGE_GET_PHYS_PAGE(m));

	if (paddr == 0) {
		panic("%s: paddr is 0",
		    __FUNCTION__);
	}

	vm_object_lock(pmap_object);

	while (m != VM_PAGE_NULL) {
		vm_page_insert_wired(m, pmap_object, (vm_object_offset_t) ((ptoa(VM_PAGE_GET_PHYS_PAGE(m))) - gPhysBase), VM_KERN_MEMORY_PTE);
		m_prev = m;
		m = NEXT_PAGE(m_prev);
		*(NEXT_PAGE_PTR(m_prev)) = VM_PAGE_NULL;
	}

	vm_object_unlock(pmap_object);

	OSAddAtomic(1, &inuse_pmap_pages_count);
	OSAddAtomic64(1, &alloc_pmap_pages_count);

	return paddr;
}

static void
pmap_alloc_page_for_ppl(void)
{
	pmap_mark_page_as_ppl_page(pmap_alloc_page_for_kern());
}

static pmap_t
pmap_alloc_pmap(void)
{
	pmap_t pmap = PMAP_NULL;

	pmap_simple_lock(&pmap_free_list_lock);

	if (pmap_free_list != PMAP_NULL) {
		pmap = pmap_free_list;
		pmap_free_list = *((pmap_t *)pmap);

		if (!PMAP_PTR_IS_VALID(pmap)) {
			panic("%s: allocated pmap is not valid, pmap=%p",
			    __FUNCTION__, pmap);
		}
	}

	pmap_simple_unlock(&pmap_free_list_lock);

	return pmap;
}

static void
pmap_free_pmap(pmap_t pmap)
{
	if (!PMAP_PTR_IS_VALID(pmap)) {
		panic("%s: pmap is not valid, "
		    "pmap=%p",
		    __FUNCTION__,
		    pmap);
	}

	pmap_simple_lock(&pmap_free_list_lock);
	*((pmap_t *)pmap) = pmap_free_list;
	pmap_free_list = pmap;
	pmap_simple_unlock(&pmap_free_list_lock);
}

static void
pmap_bootstrap_pmap_free_list(void)
{
	pmap_t cur_head = PMAP_NULL;
	unsigned long i = 0;

	simple_lock_init(&pmap_free_list_lock, 0);

	for (i = 0; i < pmap_array_count; i++) {
		*((pmap_t *)(&pmap_array[i])) = cur_head;
		cur_head = &pmap_array[i];
	}

	pmap_free_list = cur_head;
}
#endif

static void
pmap_pages_free(
	pmap_paddr_t    pa,
	unsigned        size)
{
	pmap_simple_lock(&pmap_pages_lock);

	if (pmap_pages_request_count != 0) {
		page_free_entry_t       *page_entry;

		pmap_pages_request_count--;
		page_entry = (page_free_entry_t *)phystokv(pa);
		page_entry->next = pmap_pages_reclaim_list;
		pmap_pages_reclaim_list = page_entry;
		pmap_simple_unlock(&pmap_pages_lock);

		return;
	}

	pmap_simple_unlock(&pmap_pages_lock);

#if XNU_MONITOR
	(void)size;

	pmap_give_free_ppl_page(pa);
#else
	vm_page_t       m;
	pmap_paddr_t    pa_max;

	OSAddAtomic(-(size >> PAGE_SHIFT), &inuse_pmap_pages_count);

	for (pa_max = pa + size; pa < pa_max; pa = pa + PAGE_SIZE) {
		vm_object_lock(pmap_object);
		m = vm_page_lookup(pmap_object, (pa - gPhysBase));
		assert(m != VM_PAGE_NULL);
		assert(VM_PAGE_WIRED(m));
		vm_page_lock_queues();
		vm_page_free(m);
		vm_page_unlock_queues();
		vm_object_unlock(pmap_object);
	}
#endif
}

static inline void
PMAP_ZINFO_PALLOC(
	pmap_t pmap, int bytes)
{
	pmap_ledger_credit(pmap, task_ledgers.tkm_private, bytes);
}

static inline void
PMAP_ZINFO_PFREE(
	pmap_t pmap,
	int bytes)
{
	pmap_ledger_debit(pmap, task_ledgers.tkm_private, bytes);
}

static inline void
pmap_tt_ledger_credit(
	pmap_t          pmap,
	vm_size_t       size)
{
	if (pmap != kernel_pmap) {
		pmap_ledger_credit(pmap, task_ledgers.phys_footprint, size);
		pmap_ledger_credit(pmap, task_ledgers.page_table, size);
	}
}

static inline void
pmap_tt_ledger_debit(
	pmap_t          pmap,
	vm_size_t       size)
{
	if (pmap != kernel_pmap) {
		pmap_ledger_debit(pmap, task_ledgers.phys_footprint, size);
		pmap_ledger_debit(pmap, task_ledgers.page_table, size);
	}
}

static bool
alloc_asid(pmap_t pmap)
{
	int vasid;
	uint16_t hw_asid;

	pmap_simple_lock(&asid_lock);
	vasid = bitmap_first(&asid_bitmap[0], MAX_ASID);
	if (vasid < 0) {
		pmap_simple_unlock(&asid_lock);
		return false;
	}
	assert(vasid < MAX_ASID);
	bitmap_clear(&asid_bitmap[0], (unsigned int)vasid);
	pmap_simple_unlock(&asid_lock);
	// bitmap_first() returns highest-order bits first, but a 0-based scheme works
	// slightly better with the collision detection scheme used by pmap_switch_internal().
	vasid = MAX_ASID - 1 - vasid;
	hw_asid = vasid % MAX_HW_ASID;
	pmap->sw_asid = vasid / MAX_HW_ASID;
	hw_asid += 1;  // Account for ASID 0, which is reserved for the kernel
#if __ARM_KERNEL_PROTECT__
	hw_asid <<= 1;  // We're really handing out 2 hardware ASIDs, one for EL0 and one for EL1 access
#endif
	pmap->hw_asid = hw_asid;
	return true;
}

static void
free_asid(pmap_t pmap)
{
	unsigned int vasid;
	uint16_t hw_asid = pmap->hw_asid;
	assert(hw_asid != 0); // Should not try to free kernel ASID

#if __ARM_KERNEL_PROTECT__
	hw_asid >>= 1;
#endif
	hw_asid -= 1;

	vasid = ((unsigned int)pmap->sw_asid * MAX_HW_ASID) + hw_asid;
	vasid = MAX_ASID - 1 - vasid;

	pmap_simple_lock(&asid_lock);
	assert(!bitmap_test(&asid_bitmap[0], vasid));
	bitmap_set(&asid_bitmap[0], vasid);
	pmap_simple_unlock(&asid_lock);
}


#ifndef PMAP_PV_LOAD_FACTOR
#define PMAP_PV_LOAD_FACTOR            1
#endif

#define PV_LOW_WATER_MARK_DEFAULT      (0x200 * PMAP_PV_LOAD_FACTOR)
#define PV_KERN_LOW_WATER_MARK_DEFAULT (0x200 * PMAP_PV_LOAD_FACTOR)
#define PV_ALLOC_CHUNK_INITIAL         (0x200 * PMAP_PV_LOAD_FACTOR)
#define PV_KERN_ALLOC_CHUNK_INITIAL    (0x200 * PMAP_PV_LOAD_FACTOR)
#define PV_ALLOC_INITIAL_TARGET        (PV_ALLOC_CHUNK_INITIAL * 5)
#define PV_KERN_ALLOC_INITIAL_TARGET   (PV_KERN_ALLOC_CHUNK_INITIAL)

uint32_t pv_free_count MARK_AS_PMAP_DATA = 0;
uint32_t pv_page_count MARK_AS_PMAP_DATA = 0;
uint32_t pv_kern_free_count MARK_AS_PMAP_DATA = 0;

uint32_t pv_low_water_mark MARK_AS_PMAP_DATA;
uint32_t pv_kern_low_water_mark MARK_AS_PMAP_DATA;
uint32_t pv_alloc_chunk MARK_AS_PMAP_DATA;
uint32_t pv_kern_alloc_chunk MARK_AS_PMAP_DATA;

thread_t mapping_replenish_thread;
event_t mapping_replenish_event;
volatile uint32_t mappingrecurse = 0;

unsigned pmap_mapping_thread_wakeups;
unsigned pmap_reserve_replenish_stat MARK_AS_PMAP_DATA;
unsigned pmap_kern_reserve_alloc_stat MARK_AS_PMAP_DATA;


static void
pv_init(
	void)
{
	simple_lock_init(&pv_free_list_lock, 0);
	simple_lock_init(&pv_kern_free_list_lock, 0);
	pv_free_list = PV_ENTRY_NULL;
	pv_free_count = 0x0U;
	pv_kern_free_list = PV_ENTRY_NULL;
	pv_kern_free_count = 0x0U;
}

static inline void      PV_ALLOC(pv_entry_t **pv_ep);
static inline void      PV_KERN_ALLOC(pv_entry_t **pv_e);
static inline void      PV_FREE_LIST(pv_entry_t *pv_eh, pv_entry_t *pv_et, int pv_cnt, uint32_t kern_target);

static boolean_t
pv_alloc(
	pmap_t pmap,
	unsigned int pai,
	pv_entry_t **pvepp)
{
	if (pmap != NULL) {
		PMAP_ASSERT_LOCKED(pmap);
	}
	ASSERT_PVH_LOCKED(pai);
	PV_ALLOC(pvepp);
	if (PV_ENTRY_NULL == *pvepp) {
		if ((pmap == NULL) || (kernel_pmap == pmap)) {
			PV_KERN_ALLOC(pvepp);

			if (PV_ENTRY_NULL == *pvepp) {
				pv_entry_t              *pv_e;
				pv_entry_t              *pv_eh;
				pv_entry_t              *pv_et;
				int                             pv_cnt;
				unsigned                j;
				pmap_paddr_t    pa;
				kern_return_t   ret;

				UNLOCK_PVH(pai);
				if (pmap != NULL) {
					PMAP_UNLOCK(pmap);
				}

				ret = pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_ALLOCATE_NOWAIT);

				if (ret == KERN_RESOURCE_SHORTAGE) {
					ret = pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_RECLAIM_NOWAIT);
				}

				if (ret != KERN_SUCCESS) {
					panic("%s: failed to alloc page for kernel, ret=%d, "
					    "pmap=%p, pai=%u, pvepp=%p",
					    __FUNCTION__, ret,
					    pmap, pai, pvepp);
				}

				pv_page_count++;

				pv_e = (pv_entry_t *)phystokv(pa);
				pv_cnt = 0;
				pv_eh = pv_et = PV_ENTRY_NULL;
				*pvepp = pv_e;
				pv_e++;

				for (j = 1; j < (PAGE_SIZE / sizeof(pv_entry_t)); j++) {
					pv_e->pve_next = pv_eh;
					pv_eh = pv_e;

					if (pv_et == PV_ENTRY_NULL) {
						pv_et = pv_e;
					}
					pv_cnt++;
					pv_e++;
				}
				PV_FREE_LIST(pv_eh, pv_et, pv_cnt, pv_kern_low_water_mark);
				if (pmap != NULL) {
					PMAP_LOCK(pmap);
				}
				LOCK_PVH(pai);
				return FALSE;
			}
		} else {
			UNLOCK_PVH(pai);
			PMAP_UNLOCK(pmap);

			pv_entry_t              *pv_e;
			pv_entry_t              *pv_eh;
			pv_entry_t              *pv_et;
			int                             pv_cnt;
			unsigned                j;
			pmap_paddr_t    pa;
			kern_return_t   ret;

#if XNU_MONITOR
			/*
			 * The PPL has no guarantee that its allocation
			 * will succeed, so steal pages if necessary to
			 * ensure that we can free up a PV allocation.
			 */
			ret = pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_ALLOCATE_NOWAIT);

			if (ret == KERN_RESOURCE_SHORTAGE) {
				ret = pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_RECLAIM_NOWAIT);
			}
#else
			ret = pmap_pages_alloc(&pa, PAGE_SIZE, 0);
#endif

			if (ret != KERN_SUCCESS) {
				panic("%s: failed to alloc page, ret=%d, "
				    "pmap=%p, pai=%u, pvepp=%p",
				    __FUNCTION__, ret,
				    pmap, pai, pvepp);
			}

			pv_page_count++;

			pv_e = (pv_entry_t *)phystokv(pa);
			pv_cnt = 0;
			pv_eh = pv_et = PV_ENTRY_NULL;
			*pvepp = pv_e;
			pv_e++;

			for (j = 1; j < (PAGE_SIZE / sizeof(pv_entry_t)); j++) {
				pv_e->pve_next = pv_eh;
				pv_eh = pv_e;

				if (pv_et == PV_ENTRY_NULL) {
					pv_et = pv_e;
				}
				pv_cnt++;
				pv_e++;
			}

			PV_FREE_LIST(pv_eh, pv_et, pv_cnt, pv_kern_low_water_mark);

			PMAP_LOCK(pmap);
			LOCK_PVH(pai);
			return FALSE;
		}
	}
	assert(PV_ENTRY_NULL != *pvepp);
	return TRUE;
}

static void
pv_free(
	pv_entry_t *pvep)
{
	PV_FREE_LIST(pvep, pvep, 1, pv_kern_low_water_mark);
}

static void
pv_list_free(
	pv_entry_t *pvehp,
	pv_entry_t *pvetp,
	unsigned int cnt)
{
	PV_FREE_LIST(pvehp, pvetp, cnt, pv_kern_low_water_mark);
}

static inline void
pv_water_mark_check(void)
{
	if (__improbable((pv_free_count < pv_low_water_mark) || (pv_kern_free_count < pv_kern_low_water_mark))) {
		if (!mappingrecurse && os_atomic_cmpxchg(&mappingrecurse, 0, 1, acq_rel)) {
			thread_wakeup(&mapping_replenish_event);
		}
	}
}

static inline void
PV_ALLOC(pv_entry_t **pv_ep)
{
	assert(*pv_ep == PV_ENTRY_NULL);
#if !XNU_MONITOR
	if (pv_kern_free_count < pv_kern_low_water_mark) {
		/*
		 * If the kernel reserved pool is low, let non-kernel mappings wait for a page
		 * from the VM.
		 */
		return;
	}
#endif
	pmap_simple_lock(&pv_free_list_lock);

	if ((*pv_ep = pv_free_list) != 0) {
		pv_free_list = (pv_entry_t *)(*pv_ep)->pve_next;
		(*pv_ep)->pve_next = PV_ENTRY_NULL;
		pv_free_count--;
	}

	pmap_simple_unlock(&pv_free_list_lock);
}

static inline void
PV_FREE_LIST(pv_entry_t *pv_eh, pv_entry_t *pv_et, int pv_cnt, uint32_t kern_target)
{
	bool use_kernel_list = false;
	pmap_simple_lock(&pv_kern_free_list_lock);
	if (pv_kern_free_count < kern_target) {
		pv_et->pve_next = pv_kern_free_list;
		pv_kern_free_list = pv_eh;
		pv_kern_free_count += pv_cnt;
		use_kernel_list = true;
	}
	pmap_simple_unlock(&pv_kern_free_list_lock);

	if (!use_kernel_list) {
		pmap_simple_lock(&pv_free_list_lock);
		pv_et->pve_next = (pv_entry_t *)pv_free_list;
		pv_free_list = pv_eh;
		pv_free_count += pv_cnt;
		pmap_simple_unlock(&pv_free_list_lock);
	}
}

static inline void
PV_KERN_ALLOC(pv_entry_t **pv_e)
{
	assert(*pv_e == PV_ENTRY_NULL);
	pmap_simple_lock(&pv_kern_free_list_lock);

	if ((*pv_e = pv_kern_free_list) != 0) {
		pv_kern_free_list = (pv_entry_t *)(*pv_e)->pve_next;
		(*pv_e)->pve_next = PV_ENTRY_NULL;
		pv_kern_free_count--;
		pmap_kern_reserve_alloc_stat++;
	}

	pmap_simple_unlock(&pv_kern_free_list_lock);
}

/*
 * Creates a target number of free pv_entry_t objects for the kernel free list
 * and the general free list.
 */
MARK_AS_PMAP_TEXT static kern_return_t
mapping_free_prime_internal(void)
{
	SECURITY_READ_ONLY_LATE(static boolean_t) mapping_free_prime_internal_called = FALSE;
	SECURITY_READ_ONLY_LATE(static boolean_t) mapping_free_prime_internal_done = FALSE;

	if (mapping_free_prime_internal_done) {
		return KERN_FAILURE;
	}

	if (!mapping_free_prime_internal_called) {
		mapping_free_prime_internal_called = TRUE;

		pv_low_water_mark = PV_LOW_WATER_MARK_DEFAULT;

		/* Alterable via sysctl */
		pv_kern_low_water_mark = PV_KERN_LOW_WATER_MARK_DEFAULT;

		pv_kern_alloc_chunk = PV_KERN_ALLOC_CHUNK_INITIAL;
		pv_alloc_chunk = PV_ALLOC_CHUNK_INITIAL;
	}

	return mapping_replenish_internal(PV_KERN_ALLOC_INITIAL_TARGET, PV_ALLOC_INITIAL_TARGET);
}

void
mapping_free_prime(void)
{
	kern_return_t kr = KERN_FAILURE;

#if XNU_MONITOR
	unsigned int i = 0;

	/*
	 * Allocate the needed PPL pages up front, to minimize the change that
	 * we will need to call into the PPL multiple times.
	 */
	for (i = 0; i < PV_ALLOC_INITIAL_TARGET; i += (PAGE_SIZE / sizeof(pv_entry_t))) {
		pmap_alloc_page_for_ppl();
	}

	for (i = 0; i < PV_KERN_ALLOC_INITIAL_TARGET; i += (PAGE_SIZE / sizeof(pv_entry_t))) {
		pmap_alloc_page_for_ppl();
	}

	while ((kr = mapping_free_prime_ppl()) == KERN_RESOURCE_SHORTAGE) {
		pmap_alloc_page_for_ppl();
	}
#else
	kr = mapping_free_prime_internal();
#endif

	if (kr != KERN_SUCCESS) {
		panic("%s: failed, kr=%d",
		    __FUNCTION__, kr);
	}
}

void mapping_replenish(void);

void
mapping_adjust(void)
{
	kern_return_t mres;

	mres = kernel_thread_start_priority((thread_continue_t)mapping_replenish, NULL, MAXPRI_KERNEL, &mapping_replenish_thread);
	if (mres != KERN_SUCCESS) {
		panic("%s: mapping_replenish thread creation failed",
		    __FUNCTION__);
	}
	thread_deallocate(mapping_replenish_thread);
}

/*
 * Fills the kernel and general PV free lists back up to their low watermarks.
 */
MARK_AS_PMAP_TEXT static kern_return_t
mapping_replenish_internal(uint32_t kern_target_count, uint32_t user_target_count)
{
	pv_entry_t    *pv_e;
	pv_entry_t    *pv_eh;
	pv_entry_t    *pv_et;
	int            pv_cnt;
	unsigned       j;
	pmap_paddr_t   pa;
	kern_return_t  ret = KERN_SUCCESS;

	while ((pv_free_count < user_target_count) || (pv_kern_free_count < kern_target_count)) {
		pv_cnt = 0;
		pv_eh = pv_et = PV_ENTRY_NULL;

#if XNU_MONITOR
		if ((ret = pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_ALLOCATE_NOWAIT)) != KERN_SUCCESS) {
			return ret;
		}
#else
		ret = pmap_pages_alloc(&pa, PAGE_SIZE, 0);
		assert(ret == KERN_SUCCESS);
#endif

		pv_page_count++;

		pv_e = (pv_entry_t *)phystokv(pa);

		for (j = 0; j < (PAGE_SIZE / sizeof(pv_entry_t)); j++) {
			pv_e->pve_next = pv_eh;
			pv_eh = pv_e;

			if (pv_et == PV_ENTRY_NULL) {
				pv_et = pv_e;
			}
			pv_cnt++;
			pv_e++;
		}
		pmap_reserve_replenish_stat += pv_cnt;
		PV_FREE_LIST(pv_eh, pv_et, pv_cnt, kern_target_count);
	}

	return ret;
}

/*
 * Continuation function that keeps the PV free lists from running out of free
 * elements.
 */
__attribute__((noreturn))
void
mapping_replenish(void)
{
	kern_return_t kr;

	/* We qualify for VM privileges...*/
	current_thread()->options |= TH_OPT_VMPRIV;

	for (;;) {
#if XNU_MONITOR

		while ((kr = mapping_replenish_ppl(pv_kern_low_water_mark, pv_low_water_mark)) == KERN_RESOURCE_SHORTAGE) {
			pmap_alloc_page_for_ppl();
		}
#else
		kr = mapping_replenish_internal(pv_kern_low_water_mark, pv_low_water_mark);
#endif

		if (kr != KERN_SUCCESS) {
			panic("%s: failed, kr=%d", __FUNCTION__, kr);
		}

		/* Check if the kernel pool has been depleted since the
		 * first pass, to reduce refill latency.
		 */
		if (pv_kern_free_count < pv_kern_low_water_mark) {
			continue;
		}
		/* Block sans continuation to avoid yielding kernel stack */
		assert_wait(&mapping_replenish_event, THREAD_UNINT);
		mappingrecurse = 0;
		thread_block(THREAD_CONTINUE_NULL);
		pmap_mapping_thread_wakeups++;
	}
}


static void
ptd_bootstrap(
	pt_desc_t *ptdp,
	unsigned int ptd_cnt)
{
	simple_lock_init(&ptd_free_list_lock, 0);
	while (ptd_cnt != 0) {
		(*(void **)ptdp) = (void *)ptd_free_list;
		ptd_free_list = ptdp;
		ptdp++;
		ptd_cnt--;
		ptd_free_count++;
	}
	ptd_preboot = FALSE;
}

static pt_desc_t*
ptd_alloc_unlinked(bool reclaim)
{
	pt_desc_t       *ptdp;
	unsigned        i;

	if (!ptd_preboot) {
		pmap_simple_lock(&ptd_free_list_lock);
	}

	if (ptd_free_count == 0) {
		unsigned int    ptd_cnt;
		pt_desc_t               *ptdp_next;

		if (ptd_preboot) {
			ptdp = (pt_desc_t *)avail_start;
			avail_start += ARM_PGBYTES;
			ptdp_next = ptdp;
			ptd_cnt = ARM_PGBYTES / sizeof(pt_desc_t);
		} else {
			pmap_paddr_t    pa;
			kern_return_t   ret;

			pmap_simple_unlock(&ptd_free_list_lock);

			if (pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_ALLOCATE_NOWAIT) != KERN_SUCCESS) {
				if (reclaim) {
					ret =  pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_RECLAIM_NOWAIT);
					assert(ret == KERN_SUCCESS);
				} else {
					return NULL;
				}
			}
			ptdp = (pt_desc_t *)phystokv(pa);

			pmap_simple_lock(&ptd_free_list_lock);
			ptdp_next = ptdp;
			ptd_cnt = PAGE_SIZE / sizeof(pt_desc_t);
		}

		while (ptd_cnt != 0) {
			(*(void **)ptdp_next) = (void *)ptd_free_list;
			ptd_free_list = ptdp_next;
			ptdp_next++;
			ptd_cnt--;
			ptd_free_count++;
		}
	}

	if ((ptdp = ptd_free_list) != PTD_ENTRY_NULL) {
		ptd_free_list = (pt_desc_t *)(*(void **)ptdp);
		ptd_free_count--;
	} else {
		panic("%s: out of ptd entry",
		    __FUNCTION__);
	}

	if (!ptd_preboot) {
		pmap_simple_unlock(&ptd_free_list_lock);
	}

	ptdp->pt_page.next = NULL;
	ptdp->pt_page.prev = NULL;
	ptdp->pmap = NULL;

	for (i = 0; i < PT_INDEX_MAX; i++) {
		ptdp->ptd_info[i].va = (vm_offset_t)-1;
		ptdp->ptd_info[i].refcnt = 0;
		ptdp->ptd_info[i].wiredcnt = 0;
	}

	return ptdp;
}

static inline pt_desc_t*
ptd_alloc(pmap_t pmap, bool reclaim)
{
	pt_desc_t *ptdp = ptd_alloc_unlinked(reclaim);

	if (ptdp == NULL) {
		return NULL;
	}

	ptdp->pmap = pmap;
	if (pmap != kernel_pmap) {
		/* We should never try to reclaim kernel pagetable pages in
		 * pmap_pages_reclaim(), so don't enter them into the list. */
		pmap_simple_lock(&pt_pages_lock);
		queue_enter(&pt_page_list, ptdp, pt_desc_t *, pt_page);
		pmap_simple_unlock(&pt_pages_lock);
	}

	pmap_tt_ledger_credit(pmap, sizeof(*ptdp));
	return ptdp;
}

static void
ptd_deallocate(pt_desc_t *ptdp)
{
	pmap_t          pmap = ptdp->pmap;

	if (ptd_preboot) {
		panic("%s: early boot, "
		    "ptdp=%p",
		    __FUNCTION__,
		    ptdp);
	}

	if (ptdp->pt_page.next != NULL) {
		pmap_simple_lock(&pt_pages_lock);
		queue_remove(&pt_page_list, ptdp, pt_desc_t *, pt_page);
		pmap_simple_unlock(&pt_pages_lock);
	}
	pmap_simple_lock(&ptd_free_list_lock);
	(*(void **)ptdp) = (void *)ptd_free_list;
	ptd_free_list = (pt_desc_t *)ptdp;
	ptd_free_count++;
	pmap_simple_unlock(&ptd_free_list_lock);
	if (pmap != NULL) {
		pmap_tt_ledger_debit(pmap, sizeof(*ptdp));
	}
}

static void
ptd_init(
	pt_desc_t *ptdp,
	pmap_t pmap,
	vm_map_address_t va,
	unsigned int level,
	pt_entry_t *pte_p)
{
	if (ptdp->pmap != pmap) {
		panic("%s: pmap mismatch, "
		    "ptdp=%p, pmap=%p, va=%p, level=%u, pte_p=%p",
		    __FUNCTION__,
		    ptdp, pmap, (void*)va, level, pte_p);
	}

#if     (__ARM_VMSA__ == 7)
	assert(level == 2);
	ptdp->ptd_info[ARM_PT_DESC_INDEX(pte_p)].va = (vm_offset_t) va & ~(ARM_TT_L1_PT_OFFMASK);
#else
	assert(level > pt_attr_root_level(pmap_get_pt_attr(pmap)));
	ptdp->ptd_info[ARM_PT_DESC_INDEX(pte_p)].va = (vm_offset_t) va & ~(pt_attr_ln_offmask(pmap_get_pt_attr(pmap), level - 1));
#endif
	if (level < PMAP_TT_MAX_LEVEL) {
		ptdp->ptd_info[ARM_PT_DESC_INDEX(pte_p)].refcnt = PT_DESC_REFCOUNT;
	}
}


boolean_t
pmap_valid_address(
	pmap_paddr_t addr)
{
	return pa_valid(addr);
}

#if     (__ARM_VMSA__ == 7)

/*
 *      Given an offset and a map, compute the address of the
 *      corresponding translation table entry.
 */
static inline tt_entry_t *
pmap_tte(pmap_t pmap,
    vm_map_address_t addr)
{
	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	if (!(tte_index(pmap, pt_attr, addr) < pmap->tte_index_max)) {
		return (tt_entry_t *)NULL;
	}
	return &pmap->tte[tte_index(pmap, pt_attr, addr)];
}


/*
 *	Given an offset and a map, compute the address of the
 *	pte.  If the address is invalid with respect to the map
 *	then PT_ENTRY_NULL is returned (and the map may need to grow).
 *
 *	This is only used internally.
 */
static inline pt_entry_t *
pmap_pte(
	pmap_t pmap,
	vm_map_address_t addr)
{
	pt_entry_t     *ptp;
	tt_entry_t     *ttp;
	tt_entry_t      tte;

	ttp = pmap_tte(pmap, addr);
	if (ttp == (tt_entry_t *)NULL) {
		return PT_ENTRY_NULL;
	}
	tte = *ttp;
#if MACH_ASSERT
	if ((tte & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_BLOCK) {
		panic("%s: Attempt to demote L1 block, tte=0x%lx, "
		    "pmap=%p, addr=%p",
		    __FUNCTION__, (unsigned long)tte,
		    pmap, (void*)addr);
	}
#endif
	if ((tte & ARM_TTE_TYPE_MASK) != ARM_TTE_TYPE_TABLE) {
		return PT_ENTRY_NULL;
	}
	ptp = (pt_entry_t *) ttetokv(tte) + ptenum(addr);
	return ptp;
}

__unused static inline tt_entry_t *
pmap_ttne(pmap_t pmap,
    unsigned int target_level,
    vm_map_address_t addr)
{
	tt_entry_t * ret_ttep = NULL;

	switch (target_level) {
	case 1:
		ret_ttep = pmap_tte(pmap, addr);
		break;
	case 2:
		ret_ttep = (tt_entry_t *)pmap_pte(pmap, addr);
		break;
	default:
		panic("%s: bad level, "
		    "pmap=%p, target_level=%u, addr=%p",
		    __FUNCTION__,
		    pmap, target_level, (void *)addr);
	}

	return ret_ttep;
}

#else

static inline tt_entry_t *
pmap_ttne(pmap_t pmap,
    unsigned int target_level,
    vm_map_address_t addr)
{
	tt_entry_t * ttp = NULL;
	tt_entry_t * ttep = NULL;
	tt_entry_t   tte = ARM_TTE_EMPTY;
	unsigned int cur_level;

	const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	ttp = pmap->tte;

	assert(target_level <= pt_attr->pta_max_level);

	for (cur_level = pt_attr->pta_root_level; cur_level <= target_level; cur_level++) {
		ttep = &ttp[ttn_index(pmap, pt_attr, addr, cur_level)];

		if (cur_level == target_level) {
			break;
		}

		tte = *ttep;

#if MACH_ASSERT
		if ((tte & (ARM_TTE_TYPE_MASK | ARM_TTE_VALID)) == (ARM_TTE_TYPE_BLOCK | ARM_TTE_VALID)) {
			panic("%s: Attempt to demote L%u block, tte=0x%llx, "
			    "pmap=%p, target_level=%u, addr=%p",
			    __FUNCTION__, cur_level, tte,
			    pmap, target_level, (void*)addr);
		}
#endif
		if ((tte & (ARM_TTE_TYPE_MASK | ARM_TTE_VALID)) != (ARM_TTE_TYPE_TABLE | ARM_TTE_VALID)) {
			return TT_ENTRY_NULL;
		}

		ttp = (tt_entry_t*)phystokv(tte & ARM_TTE_TABLE_MASK);
	}

	return ttep;
}

/*
 *	Given an offset and a map, compute the address of level 1 translation table entry.
 *	If the tranlation is invalid then PT_ENTRY_NULL is returned.
 */
static inline tt_entry_t *
pmap_tt1e(pmap_t pmap,
    vm_map_address_t addr)
{
	return pmap_ttne(pmap, PMAP_TT_L1_LEVEL, addr);
}

/*
 *	Given an offset and a map, compute the address of level 2 translation table entry.
 *	If the tranlation is invalid then PT_ENTRY_NULL is returned.
 */
static inline tt_entry_t *
pmap_tt2e(pmap_t pmap,
    vm_map_address_t addr)
{
	return pmap_ttne(pmap, PMAP_TT_L2_LEVEL, addr);
}


/*
 *	Given an offset and a map, compute the address of level 3 translation table entry.
 *	If the tranlation is invalid then PT_ENTRY_NULL is returned.
 */
static inline pt_entry_t *
pmap_tt3e(
	pmap_t pmap,
	vm_map_address_t addr)
{
	return (pt_entry_t*)pmap_ttne(pmap, PMAP_TT_L3_LEVEL, addr);
}

static inline tt_entry_t *
pmap_tte(
	pmap_t pmap,
	vm_map_address_t addr)
{
	return pmap_tt2e(pmap, addr);
}

static inline pt_entry_t *
pmap_pte(
	pmap_t pmap,
	vm_map_address_t addr)
{
	return pmap_tt3e(pmap, addr);
}

#endif

#if __APRR_SUPPORTED__
/*
 * Indicates whether the given PTE has special restrictions due to the current
 * APRR settings.
 */
static boolean_t
is_pte_aprr_protected(pt_entry_t pte)
{
	uint64_t aprr_el0_value;
	uint64_t aprr_el1_value;
	uint64_t aprr_index;

	MRS(aprr_el0_value, APRR_EL0);
	MRS(aprr_el1_value, APRR_EL1);
	aprr_index = PTE_TO_APRR_INDEX(pte);

	/* Check to see if this mapping had APRR restrictions. */
	if ((APRR_EXTRACT_IDX_ATTR(aprr_el0_value, aprr_index) != APRR_EXTRACT_IDX_ATTR(APRR_EL0_RESET, aprr_index)) ||
	    (APRR_EXTRACT_IDX_ATTR(aprr_el1_value, aprr_index) != APRR_EXTRACT_IDX_ATTR(APRR_EL1_RESET, aprr_index))
	    ) {
		return TRUE;
	}

	return FALSE;
}
#endif /* __APRR_SUPPORTED__ */


#if __APRR_SUPPORTED__
static boolean_t
is_pte_xprr_protected(pt_entry_t pte)
{
#if __APRR_SUPPORTED__
	return is_pte_aprr_protected(pte);
#else /* __APRR_SUPPORTED__ */
#error "XPRR configuration error"
#endif /* __APRR_SUPPORTED__ */
}
#endif /* __APRR_SUPPORTED__*/

#if __APRR_SUPPORTED__
static uint64_t
__unused pte_to_xprr_perm(pt_entry_t pte)
{
#if   __APRR_SUPPORTED__
	switch (PTE_TO_APRR_INDEX(pte)) {
	case APRR_FIRM_RX_INDEX:  return XPRR_FIRM_RX_PERM;
	case APRR_FIRM_RO_INDEX:  return XPRR_FIRM_RO_PERM;
	case APRR_PPL_RW_INDEX:   return XPRR_PPL_RW_PERM;
	case APRR_KERN_RW_INDEX:  return XPRR_KERN_RW_PERM;
	case APRR_FIRM_RW_INDEX:  return XPRR_FIRM_RW_PERM;
	case APRR_KERN0_RW_INDEX: return XPRR_KERN0_RW_PERM;
	case APRR_USER_JIT_INDEX: return XPRR_USER_JIT_PERM;
	case APRR_USER_RW_INDEX:  return XPRR_USER_RW_PERM;
	case APRR_PPL_RX_INDEX:   return XPRR_PPL_RX_PERM;
	case APRR_KERN_RX_INDEX:  return XPRR_KERN_RX_PERM;
	case APRR_PPL_RO_INDEX:   return XPRR_PPL_RO_PERM;
	case APRR_KERN_RO_INDEX:  return XPRR_KERN_RO_PERM;
	case APRR_KERN0_RX_INDEX: return XPRR_KERN0_RO_PERM;
	case APRR_KERN0_RO_INDEX: return XPRR_KERN0_RO_PERM;
	case APRR_USER_RX_INDEX:  return XPRR_USER_RX_PERM;
	case APRR_USER_RO_INDEX:  return XPRR_USER_RO_PERM;
	default:                  return XPRR_MAX_PERM;
	}
#else
#error "XPRR configuration error"
#endif /**/
}

#if __APRR_SUPPORTED__
static uint64_t
xprr_perm_to_aprr_index(uint64_t perm)
{
	switch (perm) {
	case XPRR_FIRM_RX_PERM:  return APRR_FIRM_RX_INDEX;
	case XPRR_FIRM_RO_PERM:  return APRR_FIRM_RO_INDEX;
	case XPRR_PPL_RW_PERM:   return APRR_PPL_RW_INDEX;
	case XPRR_KERN_RW_PERM:  return APRR_KERN_RW_INDEX;
	case XPRR_FIRM_RW_PERM:  return APRR_FIRM_RW_INDEX;
	case XPRR_KERN0_RW_PERM: return APRR_KERN0_RW_INDEX;
	case XPRR_USER_JIT_PERM: return APRR_USER_JIT_INDEX;
	case XPRR_USER_RW_PERM:  return APRR_USER_RW_INDEX;
	case XPRR_PPL_RX_PERM:   return APRR_PPL_RX_INDEX;
	case XPRR_KERN_RX_PERM:  return APRR_KERN_RX_INDEX;
	case XPRR_PPL_RO_PERM:   return APRR_PPL_RO_INDEX;
	case XPRR_KERN_RO_PERM:  return APRR_KERN_RO_INDEX;
	case XPRR_KERN0_RX_PERM: return APRR_KERN0_RO_INDEX;
	case XPRR_KERN0_RO_PERM: return APRR_KERN0_RO_INDEX;
	case XPRR_USER_RX_PERM:  return APRR_USER_RX_INDEX;
	case XPRR_USER_RO_PERM:  return APRR_USER_RO_INDEX;
	default:                 return APRR_MAX_INDEX;
	}
}
#endif /* __APRR_SUPPORTED__ */

static pt_entry_t
__unused xprr_perm_to_pte(uint64_t perm)
{
#if   __APRR_SUPPORTED__
	return APRR_INDEX_TO_PTE(xprr_perm_to_aprr_index(perm));
#else
#error "XPRR configuration error"
#endif /**/
}
#endif /* __APRR_SUPPORTED__*/


/*
 *      Map memory at initialization.  The physical addresses being
 *      mapped are not managed and are never unmapped.
 *
 *      For now, VM is already on, we only need to map the
 *      specified memory.
 */
vm_map_address_t
pmap_map(
	vm_map_address_t virt,
	vm_offset_t start,
	vm_offset_t end,
	vm_prot_t prot,
	unsigned int flags)
{
	kern_return_t   kr;
	vm_size_t       ps;

	ps = PAGE_SIZE;
	while (start < end) {
		kr = pmap_enter(kernel_pmap, virt, (ppnum_t)atop(start),
		    prot, VM_PROT_NONE, flags, FALSE);

		if (kr != KERN_SUCCESS) {
			panic("%s: failed pmap_enter, "
			    "virt=%p, start_addr=%p, end_addr=%p, prot=%#x, flags=%#x",
			    __FUNCTION__,
			    (void *) virt, (void *) start, (void *) end, prot, flags);
		}

		virt += ps;
		start += ps;
	}
	return virt;
}

vm_map_address_t
pmap_map_bd_with_options(
	vm_map_address_t virt,
	vm_offset_t start,
	vm_offset_t end,
	vm_prot_t prot,
	int32_t options)
{
	pt_entry_t      tmplate;
	pt_entry_t     *ptep;
	vm_map_address_t vaddr;
	vm_offset_t     paddr;
	pt_entry_t      mem_attr;

	switch (options & PMAP_MAP_BD_MASK) {
	case PMAP_MAP_BD_WCOMB:
		mem_attr = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITECOMB);
#if     (__ARM_VMSA__ > 7)
		mem_attr |= ARM_PTE_SH(SH_OUTER_MEMORY);
#else
		mem_attr |= ARM_PTE_SH;
#endif
		break;
	case PMAP_MAP_BD_POSTED:
		mem_attr = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_POSTED);
		break;
	case PMAP_MAP_BD_POSTED_REORDERED:
		mem_attr = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_POSTED_REORDERED);
		break;
	case PMAP_MAP_BD_POSTED_COMBINED_REORDERED:
		mem_attr = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_POSTED_COMBINED_REORDERED);
		break;
	default:
		mem_attr = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DISABLE);
		break;
	}

	tmplate = pa_to_pte(start) | ARM_PTE_AP((prot & VM_PROT_WRITE) ? AP_RWNA : AP_RONA) |
	    mem_attr | ARM_PTE_TYPE | ARM_PTE_NX | ARM_PTE_PNX | ARM_PTE_AF;
#if __ARM_KERNEL_PROTECT__
	tmplate |= ARM_PTE_NG;
#endif /* __ARM_KERNEL_PROTECT__ */

	vaddr = virt;
	paddr = start;
	while (paddr < end) {
		ptep = pmap_pte(kernel_pmap, vaddr);
		if (ptep == PT_ENTRY_NULL) {
			panic("%s: no PTE for vaddr=%p, "
			    "virt=%p, start=%p, end=%p, prot=0x%x, options=0x%x",
			    __FUNCTION__, (void*)vaddr,
			    (void*)virt, (void*)start, (void*)end, prot, options);
		}

		assert(!ARM_PTE_IS_COMPRESSED(*ptep, ptep));
		WRITE_PTE_STRONG(ptep, tmplate);

		pte_increment_pa(tmplate);
		vaddr += PAGE_SIZE;
		paddr += PAGE_SIZE;
	}

	if (end >= start) {
		flush_mmu_tlb_region(virt, (unsigned)(end - start));
	}

	return vaddr;
}

/*
 *      Back-door routine for mapping kernel VM at initialization.
 *      Useful for mapping memory outside the range
 *      [vm_first_phys, vm_last_phys] (i.e., devices).
 *      Otherwise like pmap_map.
 */
vm_map_address_t
pmap_map_bd(
	vm_map_address_t virt,
	vm_offset_t start,
	vm_offset_t end,
	vm_prot_t prot)
{
	pt_entry_t      tmplate;
	pt_entry_t              *ptep;
	vm_map_address_t vaddr;
	vm_offset_t             paddr;

	/* not cacheable and not buffered */
	tmplate = pa_to_pte(start)
	    | ARM_PTE_TYPE | ARM_PTE_AF | ARM_PTE_NX | ARM_PTE_PNX
	    | ARM_PTE_AP((prot & VM_PROT_WRITE) ? AP_RWNA : AP_RONA)
	    | ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DISABLE);
#if __ARM_KERNEL_PROTECT__
	tmplate |= ARM_PTE_NG;
#endif /* __ARM_KERNEL_PROTECT__ */

	vaddr = virt;
	paddr = start;
	while (paddr < end) {
		ptep = pmap_pte(kernel_pmap, vaddr);
		if (ptep == PT_ENTRY_NULL) {
			panic("pmap_map_bd");
		}
		assert(!ARM_PTE_IS_COMPRESSED(*ptep, ptep));
		WRITE_PTE_STRONG(ptep, tmplate);

		pte_increment_pa(tmplate);
		vaddr += PAGE_SIZE;
		paddr += PAGE_SIZE;
	}

	if (end >= start) {
		flush_mmu_tlb_region(virt, (unsigned)(end - start));
	}

	return vaddr;
}

/*
 *      Back-door routine for mapping kernel VM at initialization.
 *      Useful for mapping memory specific physical addresses in early
 *      boot (i.e., before kernel_map is initialized).
 *
 *      Maps are in the VM_HIGH_KERNEL_WINDOW area.
 */

vm_map_address_t
pmap_map_high_window_bd(
	vm_offset_t pa_start,
	vm_size_t len,
	vm_prot_t prot)
{
	pt_entry_t              *ptep, pte;
#if (__ARM_VMSA__ == 7)
	vm_map_address_t        va_start = VM_HIGH_KERNEL_WINDOW;
	vm_map_address_t        va_max = VM_MAX_KERNEL_ADDRESS;
#else
	vm_map_address_t        va_start = VREGION1_START;
	vm_map_address_t        va_max = VREGION1_START + VREGION1_SIZE;
#endif
	vm_map_address_t        va_end;
	vm_map_address_t        va;
	vm_size_t               offset;

	offset = pa_start & PAGE_MASK;
	pa_start -= offset;
	len += offset;

	if (len > (va_max - va_start)) {
		panic("%s: area too large, "
		    "pa_start=%p, len=%p, prot=0x%x",
		    __FUNCTION__,
		    (void*)pa_start, (void*)len, prot);
	}

scan:
	for (; va_start < va_max; va_start += PAGE_SIZE) {
		ptep = pmap_pte(kernel_pmap, va_start);
		assert(!ARM_PTE_IS_COMPRESSED(*ptep, ptep));
		if (*ptep == ARM_PTE_TYPE_FAULT) {
			break;
		}
	}
	if (va_start > va_max) {
		panic("%s: insufficient pages, "
		    "pa_start=%p, len=%p, prot=0x%x",
		    __FUNCTION__,
		    (void*)pa_start, (void*)len, prot);
	}

	for (va_end = va_start + PAGE_SIZE; va_end < va_start + len; va_end += PAGE_SIZE) {
		ptep = pmap_pte(kernel_pmap, va_end);
		assert(!ARM_PTE_IS_COMPRESSED(*ptep, ptep));
		if (*ptep != ARM_PTE_TYPE_FAULT) {
			va_start = va_end + PAGE_SIZE;
			goto scan;
		}
	}

	for (va = va_start; va < va_end; va += PAGE_SIZE, pa_start += PAGE_SIZE) {
		ptep = pmap_pte(kernel_pmap, va);
		pte = pa_to_pte(pa_start)
		    | ARM_PTE_TYPE | ARM_PTE_AF | ARM_PTE_NX | ARM_PTE_PNX
		    | ARM_PTE_AP((prot & VM_PROT_WRITE) ? AP_RWNA : AP_RONA)
		    | ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DEFAULT);
#if     (__ARM_VMSA__ > 7)
		pte |= ARM_PTE_SH(SH_OUTER_MEMORY);
#else
		pte |= ARM_PTE_SH;
#endif
#if __ARM_KERNEL_PROTECT__
		pte |= ARM_PTE_NG;
#endif /* __ARM_KERNEL_PROTECT__ */
		WRITE_PTE_STRONG(ptep, pte);
	}
	PMAP_UPDATE_TLBS(kernel_pmap, va_start, va_start + len, false);
#if KASAN
	kasan_notify_address(va_start, len);
#endif
	return va_start;
}

#define PMAP_ALIGN(addr, align) ((addr) + ((align) - 1) & ~((align) - 1))

static vm_size_t
pmap_compute_io_rgns(void)
{
	DTEntry entry;
	pmap_io_range_t *ranges;
	uint64_t rgn_end;
	void *prop = NULL;
	int err;
	unsigned int prop_size;

	err = DTLookupEntry(NULL, "/defaults", &entry);
	assert(err == kSuccess);

	if (kSuccess != DTGetProperty(entry, "pmap-io-ranges", &prop, &prop_size)) {
		return 0;
	}

	ranges = prop;
	for (unsigned int i = 0; i < (prop_size / sizeof(*ranges)); ++i) {
		if (ranges[i].addr & PAGE_MASK) {
			panic("pmap I/O region %u addr 0x%llx is not page-aligned", i, ranges[i].addr);
		}
		if (ranges[i].len & PAGE_MASK) {
			panic("pmap I/O region %u length 0x%llx is not page-aligned", i, ranges[i].len);
		}
		if (os_add_overflow(ranges[i].addr, ranges[i].len, &rgn_end)) {
			panic("pmap I/O region %u addr 0x%llx length 0x%llx wraps around", i, ranges[i].addr, ranges[i].len);
		}
		if (((ranges[i].addr <= gPhysBase) && (rgn_end > gPhysBase)) ||
		    ((ranges[i].addr < avail_end) && (rgn_end >= avail_end)) ||
		    ((ranges[i].addr > gPhysBase) && (rgn_end < avail_end))) {
			panic("pmap I/O region %u addr 0x%llx length 0x%llx overlaps physical memory", i, ranges[i].addr, ranges[i].len);
		}

		++num_io_rgns;
	}

	return num_io_rgns * sizeof(*ranges);
}

/*
 * return < 0 for a < b
 *          0 for a == b
 *        > 0 for a > b
 */
typedef int (*cmpfunc_t)(const void *a, const void *b);

extern void
qsort(void *a, size_t n, size_t es, cmpfunc_t cmp);

static int
cmp_io_rgns(const void *a, const void *b)
{
	const pmap_io_range_t *range_a = a;
	const pmap_io_range_t *range_b = b;
	if ((range_b->addr + range_b->len) <= range_a->addr) {
		return 1;
	} else if ((range_a->addr + range_a->len) <= range_b->addr) {
		return -1;
	} else {
		return 0;
	}
}

static void
pmap_load_io_rgns(void)
{
	DTEntry entry;
	pmap_io_range_t *ranges;
	void *prop = NULL;
	int err;
	unsigned int prop_size;

	if (num_io_rgns == 0) {
		return;
	}

	err = DTLookupEntry(NULL, "/defaults", &entry);
	assert(err == kSuccess);

	err = DTGetProperty(entry, "pmap-io-ranges", &prop, &prop_size);
	assert(err == kSuccess);

	ranges = prop;
	for (unsigned int i = 0; i < (prop_size / sizeof(*ranges)); ++i) {
		io_attr_table[i] = ranges[i];
	}

	qsort(io_attr_table, num_io_rgns, sizeof(*ranges), cmp_io_rgns);
}

#if __arm64__
/*
 * pmap_get_arm64_prot
 *
 * return effective armv8 VMSA block protections including
 * table AP/PXN/XN overrides of a pmap entry
 *
 */

uint64_t
pmap_get_arm64_prot(
	pmap_t pmap,
	vm_offset_t addr)
{
	tt_entry_t tte = 0;
	unsigned int level = 0;
	uint64_t tte_type = 0;
	uint64_t effective_prot_bits = 0;
	uint64_t aggregate_tte = 0;
	uint64_t table_ap_bits = 0, table_xn = 0, table_pxn = 0;
	const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	for (level = pt_attr->pta_root_level; level <= pt_attr->pta_max_level; level++) {
		tte = *pmap_ttne(pmap, level, addr);

		if (!(tte & ARM_TTE_VALID)) {
			return 0;
		}

		tte_type = tte & ARM_TTE_TYPE_MASK;

		if ((tte_type == ARM_TTE_TYPE_BLOCK) ||
		    (level == pt_attr->pta_max_level)) {
			/* Block or page mapping; both have the same protection bit layout. */
			break;
		} else if (tte_type == ARM_TTE_TYPE_TABLE) {
			/* All of the table bits we care about are overrides, so just OR them together. */
			aggregate_tte |= tte;
		}
	}

	table_ap_bits = ((aggregate_tte >> ARM_TTE_TABLE_APSHIFT) & AP_MASK);
	table_xn = (aggregate_tte & ARM_TTE_TABLE_XN);
	table_pxn = (aggregate_tte & ARM_TTE_TABLE_PXN);

	/* Start with the PTE bits. */
	effective_prot_bits = tte & (ARM_PTE_APMASK | ARM_PTE_NX | ARM_PTE_PNX);

	/* Table AP bits mask out block/page AP bits */
	effective_prot_bits &= ~(ARM_PTE_AP(table_ap_bits));

	/* XN/PXN bits can be OR'd in. */
	effective_prot_bits |= (table_xn ? ARM_PTE_NX : 0);
	effective_prot_bits |= (table_pxn ? ARM_PTE_PNX : 0);

	return effective_prot_bits;
}
#endif /* __arm64__ */


/*
 *	Bootstrap the system enough to run with virtual memory.
 *
 *	The early VM initialization code has already allocated
 *	the first CPU's translation table and made entries for
 *	all the one-to-one mappings to be found there.
 *
 *	We must set up the kernel pmap structures, the
 *	physical-to-virtual translation lookup tables for the
 *	physical memory to be managed (between avail_start and
 *	avail_end).
 *
 *	Map the kernel's code and data, and allocate the system page table.
 *	Page_size must already be set.
 *
 *	Parameters:
 *	first_avail	first available physical page -
 *			   after kernel page tables
 *	avail_start	PA of first managed physical page
 *	avail_end	PA of last managed physical page
 */

void
pmap_bootstrap(
	vm_offset_t vstart)
{
	pmap_paddr_t    pmap_struct_start;
	vm_size_t       pv_head_size;
	vm_size_t       ptd_root_table_size;
	vm_size_t       pp_attr_table_size;
	vm_size_t       io_attr_table_size;
	unsigned int    npages;
	vm_map_offset_t maxoffset;

	lck_grp_init(&pmap_lck_grp, "pmap", LCK_GRP_ATTR_NULL);

#if XNU_MONITOR

#if DEVELOPMENT || DEBUG
	PE_parse_boot_argn("-unsafe_kernel_text", &pmap_ppl_disable, sizeof(pmap_ppl_disable));
#endif

	simple_lock_init(&pmap_ppl_free_page_lock, 0);

#if __APRR_SUPPORTED__
	if (((uintptr_t)(&ppl_trampoline_start)) % PAGE_SIZE) {
		panic("%s: ppl_trampoline_start is not page aligned, "
		    "vstart=%#lx",
		    __FUNCTION__,
		    vstart);
	}

	if (((uintptr_t)(&ppl_trampoline_end)) % PAGE_SIZE) {
		panic("%s: ppl_trampoline_end is not page aligned, "
		    "vstart=%#lx",
		    __FUNCTION__,
		    vstart);
	}
#endif /* __APRR_SUPPORTED__ */
#endif /* XNU_MONITOR */

#if DEVELOPMENT || DEBUG
	if (PE_parse_boot_argn("pmap_trace", &pmap_trace_mask, sizeof(pmap_trace_mask))) {
		kprintf("Kernel traces for pmap operations enabled\n");
	}
#endif

	/*
	 *	Initialize the kernel pmap.
	 */
	pmap_stamp = 1;
#if ARM_PARAMETERIZED_PMAP
	kernel_pmap->pmap_pt_attr = native_pt_attr;
#endif /* ARM_PARAMETERIZED_PMAP */
#if HAS_APPLE_PAC
	kernel_pmap->disable_jop = 0;
#endif /* HAS_APPLE_PAC */
	kernel_pmap->tte = cpu_tte;
	kernel_pmap->ttep = cpu_ttep;
#if (__ARM_VMSA__ > 7)
	kernel_pmap->min = ARM64_TTBR1_MIN_ADDR;
#else
	kernel_pmap->min = VM_MIN_KERNEL_AND_KEXT_ADDRESS;
#endif
	kernel_pmap->max = VM_MAX_KERNEL_ADDRESS;
	os_atomic_init(&kernel_pmap->ref_count, 1);
	kernel_pmap->gc_status = 0;
	kernel_pmap->nx_enabled = TRUE;
#ifdef  __arm64__
	kernel_pmap->is_64bit = TRUE;
#else
	kernel_pmap->is_64bit = FALSE;
#endif
	kernel_pmap->stamp = os_atomic_inc(&pmap_stamp, relaxed);

	kernel_pmap->nested_region_grand_addr = 0x0ULL;
	kernel_pmap->nested_region_subord_addr = 0x0ULL;
	kernel_pmap->nested_region_size = 0x0ULL;
	kernel_pmap->nested_region_asid_bitmap = NULL;
	kernel_pmap->nested_region_asid_bitmap_size = 0x0UL;

#if (__ARM_VMSA__ == 7)
	kernel_pmap->tte_index_max = 4 * NTTES;
#endif
	kernel_pmap->hw_asid = 0;
	kernel_pmap->sw_asid = 0;

	PMAP_LOCK_INIT(kernel_pmap);
	memset((void *) &kernel_pmap->stats, 0, sizeof(kernel_pmap->stats));

	/* allocate space for and initialize the bookkeeping structures */
	io_attr_table_size = pmap_compute_io_rgns();
	npages = (unsigned int)atop(mem_size);
	pp_attr_table_size = npages * sizeof(pp_attr_t);
	pv_head_size = round_page(sizeof(pv_entry_t *) * npages);
	// allocate enough initial PTDs to map twice the available physical memory
	ptd_root_table_size = sizeof(pt_desc_t) * (mem_size / ((PAGE_SIZE / sizeof(pt_entry_t)) * ARM_PGBYTES)) * 2;

	pmap_struct_start = avail_start;

	pp_attr_table = (pp_attr_t *) phystokv(avail_start);
	avail_start = PMAP_ALIGN(avail_start + pp_attr_table_size, __alignof(pp_attr_t));
	io_attr_table = (pmap_io_range_t *) phystokv(avail_start);
	avail_start = PMAP_ALIGN(avail_start + io_attr_table_size, __alignof(pv_entry_t*));
	pv_head_table = (pv_entry_t **) phystokv(avail_start);
	avail_start = PMAP_ALIGN(avail_start + pv_head_size, __alignof(pt_desc_t));
	ptd_root_table = (pt_desc_t *)phystokv(avail_start);
	avail_start = round_page(avail_start + ptd_root_table_size);

	memset((char *)phystokv(pmap_struct_start), 0, avail_start - pmap_struct_start);

	pmap_load_io_rgns();
	ptd_bootstrap(ptd_root_table, (unsigned int)(ptd_root_table_size / sizeof(pt_desc_t)));

#if XNU_MONITOR
	pmap_array_begin = (void *)phystokv(avail_start);
	pmap_array = pmap_array_begin;
	avail_start += round_page(MAX_ASID * sizeof(struct pmap));
	pmap_array_end = (void *)phystokv(avail_start);

	pmap_array_count = ((pmap_array_end - pmap_array_begin) / sizeof(struct pmap));

	pmap_bootstrap_pmap_free_list();

	pmap_ledger_ptr_array_begin = (void *)phystokv(avail_start);
	pmap_ledger_ptr_array = pmap_ledger_ptr_array_begin;
	avail_start += round_page(MAX_PMAP_LEDGERS * sizeof(void*));
	pmap_ledger_ptr_array_end = (void *)phystokv(avail_start);

	pmap_ledger_refcnt_begin = (void *)phystokv(avail_start);
	pmap_ledger_refcnt = pmap_ledger_refcnt_begin;
	avail_start += round_page(MAX_PMAP_LEDGERS * sizeof(os_refcnt_t));
	pmap_ledger_refcnt_end = (void *)phystokv(avail_start);

	simple_lock_init(&pmap_ledger_lock, 0);
#endif
	pmap_cpu_data_array_init();

	vm_first_phys = gPhysBase;
	vm_last_phys = trunc_page(avail_end);

	simple_lock_init(&pmaps_lock, 0);
	simple_lock_init(&asid_lock, 0);
	simple_lock_init(&tt1_lock, 0);
	queue_init(&map_pmap_list);
	queue_enter(&map_pmap_list, kernel_pmap, pmap_t, pmaps);
	free_page_size_tt_list = TT_FREE_ENTRY_NULL;
	free_page_size_tt_count = 0;
	free_page_size_tt_max = 0;
	free_two_page_size_tt_list = TT_FREE_ENTRY_NULL;
	free_two_page_size_tt_count = 0;
	free_two_page_size_tt_max = 0;
	free_tt_list = TT_FREE_ENTRY_NULL;
	free_tt_count = 0;
	free_tt_max = 0;

	simple_lock_init(&pt_pages_lock, 0);
	queue_init(&pt_page_list);

	simple_lock_init(&pmap_pages_lock, 0);
	pmap_pages_request_count = 0;
	pmap_pages_request_acum = 0;
	pmap_pages_reclaim_list = PAGE_FREE_ENTRY_NULL;

	virtual_space_start = vstart;
	virtual_space_end = VM_MAX_KERNEL_ADDRESS;

	bitmap_full(&asid_bitmap[0], MAX_ASID);



	if (PE_parse_boot_argn("arm_maxoffset", &maxoffset, sizeof(maxoffset))) {
		maxoffset = trunc_page(maxoffset);
		if ((maxoffset >= pmap_max_offset(FALSE, ARM_PMAP_MAX_OFFSET_MIN))
		    && (maxoffset <= pmap_max_offset(FALSE, ARM_PMAP_MAX_OFFSET_MAX))) {
			arm_pmap_max_offset_default = maxoffset;
		}
	}
#if defined(__arm64__)
	if (PE_parse_boot_argn("arm64_maxoffset", &maxoffset, sizeof(maxoffset))) {
		maxoffset = trunc_page(maxoffset);
		if ((maxoffset >= pmap_max_offset(TRUE, ARM_PMAP_MAX_OFFSET_MIN))
		    && (maxoffset <= pmap_max_offset(TRUE, ARM_PMAP_MAX_OFFSET_MAX))) {
			arm64_pmap_max_offset_default = maxoffset;
		}
	}
#endif

#if DEVELOPMENT || DEBUG
	PE_parse_boot_argn("panic_on_unsigned_execute", &panic_on_unsigned_execute, sizeof(panic_on_unsigned_execute));
#endif /* DEVELOPMENT || DEBUG */

	pmap_nesting_size_min = ARM_NESTING_SIZE_MIN;
	pmap_nesting_size_max = ARM_NESTING_SIZE_MAX;

	simple_lock_init(&phys_backup_lock, 0);


#if MACH_ASSERT
	PE_parse_boot_argn("pmap_stats_assert",
	    &pmap_stats_assert,
	    sizeof(pmap_stats_assert));
	PE_parse_boot_argn("vm_footprint_suspend_allowed",
	    &vm_footprint_suspend_allowed,
	    sizeof(vm_footprint_suspend_allowed));
#endif /* MACH_ASSERT */

#if KASAN
	/* Shadow the CPU copy windows, as they fall outside of the physical aperture */
	kasan_map_shadow(CPUWINDOWS_BASE, CPUWINDOWS_TOP - CPUWINDOWS_BASE, true);
#endif /* KASAN */
}

#if XNU_MONITOR

static inline void
pa_set_range_monitor(pmap_paddr_t start_pa, pmap_paddr_t end_pa)
{
	pmap_paddr_t cur_pa;
	for (cur_pa = start_pa; cur_pa < end_pa; cur_pa += ARM_PGBYTES) {
		assert(pa_valid(cur_pa));
		pa_set_monitor(cur_pa);
	}
}

static void
pa_set_range_xprr_perm(pmap_paddr_t start_pa,
    pmap_paddr_t end_pa,
    unsigned int expected_perm,
    unsigned int new_perm)
{
	vm_offset_t start_va = phystokv(start_pa);
	vm_offset_t end_va = start_va + (end_pa - start_pa);

	pa_set_range_monitor(start_pa, end_pa);
	pmap_set_range_xprr_perm(start_va, end_va, expected_perm, new_perm);
}

void
pmap_static_allocations_done(void)
{
	pmap_paddr_t monitor_start_pa;
	pmap_paddr_t monitor_end_pa;

	/*
	 * We allocate memory for bootstrap starting at topOfKernelData (which
	 * is at the end of the device tree and ramdisk data, if applicable).
	 * We use avail_start as a pointer to the first address that has not
	 * been reserved for bootstrap, so we know which pages to give to the
	 * virtual memory layer.
	 *
	 * These bootstrap allocations will be used primarily for page tables.
	 * If we wish to secure the page tables, we need to start by marking
	 * these bootstrap allocations as pages that we want to protect.
	 */
	monitor_start_pa = BootArgs->topOfKernelData;
	monitor_end_pa = BootArgs->topOfKernelData + BOOTSTRAP_TABLE_SIZE;

	/* The bootstrap page tables are mapped RO at boostrap. */
	pa_set_range_xprr_perm(monitor_start_pa, monitor_end_pa, XPRR_KERN_RO_PERM, XPRR_PPL_RO_PERM);

	monitor_start_pa = BootArgs->topOfKernelData + BOOTSTRAP_TABLE_SIZE;
	monitor_end_pa = avail_start;

	/* The other bootstrap allocations are mapped RW at bootstrap. */
	pa_set_range_xprr_perm(monitor_start_pa, monitor_end_pa, XPRR_KERN_RW_PERM, XPRR_PPL_RW_PERM);

	/* The RO page tables are mapped RW at bootstrap. */
	monitor_start_pa = kvtophys((vm_offset_t)&ropagetable_begin);
	monitor_end_pa = monitor_start_pa + ((vm_offset_t)&ropagetable_end - (vm_offset_t)&ropagetable_begin);
	pa_set_range_xprr_perm(monitor_start_pa, monitor_end_pa, XPRR_KERN_RW_PERM, XPRR_PPL_RW_PERM);

	monitor_start_pa = kvtophys(segPPLDATAB);
	monitor_end_pa = monitor_start_pa + segSizePPLDATA;

	/* PPL data is RW for the PPL, RO for the kernel. */
	pa_set_range_xprr_perm(monitor_start_pa, monitor_end_pa, XPRR_KERN_RW_PERM, XPRR_PPL_RW_PERM);

	monitor_start_pa = kvtophys(segPPLTEXTB);
	monitor_end_pa = monitor_start_pa + segSizePPLTEXT;

	/* PPL text is RX for the PPL, RO for the kernel. */
	pa_set_range_xprr_perm(monitor_start_pa, monitor_end_pa, XPRR_KERN_RX_PERM, XPRR_PPL_RX_PERM);

#if __APRR_SUPPORTED__
	monitor_start_pa = kvtophys(segPPLTRAMPB);
	monitor_end_pa = monitor_start_pa + segSizePPLTRAMP;

	/*
	 * The PPLTRAMP pages will be a mix of PPL RX/kernel RO and
	 * PPL RX/kernel RX.  However, all of these pages belong to the PPL.
	 */
	pa_set_range_monitor(monitor_start_pa, monitor_end_pa);
#endif

	/*
	 * In order to support DTrace, the save areas for the PPL must be
	 * writable.  This is due to the fact that DTrace will try to update
	 * register state.
	 */
	if (pmap_ppl_disable) {
		vm_offset_t monitor_start_va = phystokv(ppl_cpu_save_area_start);
		vm_offset_t monitor_end_va = monitor_start_va + (ppl_cpu_save_area_end - ppl_cpu_save_area_start);

		pmap_set_range_xprr_perm(monitor_start_va, monitor_end_va, XPRR_PPL_RW_PERM, XPRR_KERN_RW_PERM);
	}

#if __APRR_SUPPORTED__
	/* The trampoline must also be specially protected. */
	pmap_set_range_xprr_perm((vm_offset_t)&ppl_trampoline_start, (vm_offset_t)&ppl_trampoline_end, XPRR_KERN_RX_PERM, XPRR_PPL_RX_PERM);
#endif

	if (segSizePPLDATACONST > 0) {
		monitor_start_pa = kvtophys(segPPLDATACONSTB);
		monitor_end_pa = monitor_start_pa + segSizePPLDATACONST;

		pa_set_range_xprr_perm(monitor_start_pa, monitor_end_pa, XPRR_KERN_RO_PERM, XPRR_PPL_RO_PERM);
	}

	/*
	 * Mark the original physical aperture mapping for the PPL stack pages RO as an additional security
	 * precaution.  The real RW mappings are at a different location with guard pages.
	 */
	pa_set_range_xprr_perm(pmap_stacks_start_pa, pmap_stacks_end_pa, XPRR_PPL_RW_PERM, XPRR_PPL_RO_PERM);
}


void
pmap_lockdown_ppl(void)
{
	/* Mark the PPL as being locked down. */

#if __APRR_SUPPORTED__
	pmap_ppl_locked_down = TRUE;
	/* Force a trap into to the PPL to update APRR_EL1. */
	pmap_return(FALSE, FALSE);
#else
#error "XPRR configuration error"
#endif /* __APRR_SUPPORTED__ */

}
#endif /* XNU_MONITOR */

void
pmap_virtual_space(
	vm_offset_t *startp,
	vm_offset_t *endp
	)
{
	*startp = virtual_space_start;
	*endp = virtual_space_end;
}


boolean_t
pmap_virtual_region(
	unsigned int region_select,
	vm_map_offset_t *startp,
	vm_map_size_t *size
	)
{
	boolean_t       ret = FALSE;
#if     __ARM64_PMAP_SUBPAGE_L1__ && __ARM_16K_PG__
	if (region_select == 0) {
		/*
		 * In this config, the bootstrap mappings should occupy their own L2
		 * TTs, as they should be immutable after boot.  Having the associated
		 * TTEs and PTEs in their own pages allows us to lock down those pages,
		 * while allowing the rest of the kernel address range to be remapped.
		 */
#if     (__ARM_VMSA__ > 7)
		*startp = LOW_GLOBAL_BASE_ADDRESS & ~ARM_TT_L2_OFFMASK;
#else
#error Unsupported configuration
#endif
		*size = ((VM_MAX_KERNEL_ADDRESS - *startp) & ~PAGE_MASK);
		ret = TRUE;
	}
#else
#if     (__ARM_VMSA__ > 7)
	unsigned long low_global_vr_mask = 0;
	vm_map_size_t low_global_vr_size = 0;
#endif

	if (region_select == 0) {
#if     (__ARM_VMSA__ == 7)
		*startp = gVirtBase & 0xFFC00000;
		*size = ((virtual_space_start - (gVirtBase & 0xFFC00000)) + ~0xFFC00000) & 0xFFC00000;
#else
		/* Round to avoid overlapping with the V=P area; round to at least the L2 block size. */
		if (!TEST_PAGE_SIZE_4K) {
			*startp = gVirtBase & 0xFFFFFFFFFE000000;
			*size = ((virtual_space_start - (gVirtBase & 0xFFFFFFFFFE000000)) + ~0xFFFFFFFFFE000000) & 0xFFFFFFFFFE000000;
		} else {
			*startp = gVirtBase & 0xFFFFFFFFFF800000;
			*size = ((virtual_space_start - (gVirtBase & 0xFFFFFFFFFF800000)) + ~0xFFFFFFFFFF800000) & 0xFFFFFFFFFF800000;
		}
#endif
		ret = TRUE;
	}
	if (region_select == 1) {
		*startp = VREGION1_START;
		*size = VREGION1_SIZE;
		ret = TRUE;
	}
#if     (__ARM_VMSA__ > 7)
	/* We need to reserve a range that is at least the size of an L2 block mapping for the low globals */
	if (!TEST_PAGE_SIZE_4K) {
		low_global_vr_mask = 0xFFFFFFFFFE000000;
		low_global_vr_size = 0x2000000;
	} else {
		low_global_vr_mask = 0xFFFFFFFFFF800000;
		low_global_vr_size = 0x800000;
	}

	if (((gVirtBase & low_global_vr_mask) != LOW_GLOBAL_BASE_ADDRESS) && (region_select == 2)) {
		*startp = LOW_GLOBAL_BASE_ADDRESS;
		*size = low_global_vr_size;
		ret = TRUE;
	}

	if (region_select == 3) {
		/* In this config, we allow the bootstrap mappings to occupy the same
		 * page table pages as the heap.
		 */
		*startp = VM_MIN_KERNEL_ADDRESS;
		*size = LOW_GLOBAL_BASE_ADDRESS - *startp;
		ret = TRUE;
	}
#endif
#endif
	return ret;
}

unsigned int
pmap_free_pages(
	void)
{
	return (unsigned int)atop(avail_end - first_avail);
}


boolean_t
pmap_next_page_hi(
	ppnum_t            * pnum,
	__unused boolean_t might_free)
{
	return pmap_next_page(pnum);
}


boolean_t
pmap_next_page(
	ppnum_t *pnum)
{
	if (first_avail != avail_end) {
		*pnum = (ppnum_t)atop(first_avail);
		first_avail += PAGE_SIZE;
		return TRUE;
	}
	return FALSE;
}


/*
 *	Initialize the pmap module.
 *	Called by vm_init, to initialize any structures that the pmap
 *	system needs to map virtual memory.
 */
void
pmap_init(
	void)
{
	/*
	 *	Protect page zero in the kernel map.
	 *	(can be overruled by permanent transltion
	 *	table entries at page zero - see arm_vm_init).
	 */
	vm_protect(kernel_map, 0, PAGE_SIZE, TRUE, VM_PROT_NONE);

	pmap_initialized = TRUE;

	pmap_zone_init();


	/*
	 *	Initialize the pmap object (for tracking the vm_page_t
	 *	structures for pages we allocate to be page tables in
	 *	pmap_expand().
	 */
	_vm_object_allocate(mem_size, pmap_object);
	pmap_object->copy_strategy = MEMORY_OBJECT_COPY_NONE;

	pv_init();

	/*
	 * The values of [hard_]maxproc may have been scaled, make sure
	 * they are still less than the value of MAX_ASID.
	 */
	if (maxproc > MAX_ASID) {
		maxproc = MAX_ASID;
	}
	if (hard_maxproc > MAX_ASID) {
		hard_maxproc = MAX_ASID;
	}

#if CONFIG_PGTRACE
	pmap_pgtrace_init();
#endif
}

boolean_t
pmap_verify_free(
	ppnum_t ppnum)
{
	pv_entry_t              **pv_h;
	int             pai;
	pmap_paddr_t    phys = ptoa(ppnum);

	assert(phys != vm_page_fictitious_addr);

	if (!pa_valid(phys)) {
		return FALSE;
	}

	pai = (int)pa_index(phys);
	pv_h = pai_to_pvh(pai);

	return pvh_test_type(pv_h, PVH_TYPE_NULL);
}

#if MACH_ASSERT
void
pmap_assert_free(ppnum_t ppnum)
{
	assertf(pmap_verify_free(ppnum), "page = 0x%x", ppnum);
	(void)ppnum;
}
#endif


/*
 *    Initialize zones used by pmap.
 */
static void
pmap_zone_init(
	void)
{
	/*
	 *	Create the zone of physical maps
	 *	and the physical-to-virtual entries.
	 */
	pmap_zone = zinit((vm_size_t) sizeof(struct pmap), (vm_size_t) sizeof(struct pmap) * 256,
	    PAGE_SIZE, "pmap");
}

#if XNU_MONITOR
MARK_AS_PMAP_TEXT static void
pmap_ledger_alloc_init_internal(size_t size)
{
	pmap_simple_lock(&pmap_ledger_lock);

	if (pmap_ledger_alloc_initialized) {
		panic("%s: already initialized, "
		    "size=%lu",
		    __func__,
		    size);
	}

	if (size != sizeof(pmap_ledger_data_t)) {
		panic("%s: size mismatch, expected %lu, "
		    "size=%lu",
		    __func__, PMAP_LEDGER_DATA_BYTES,
		    size);
	}

	pmap_ledger_alloc_initialized = true;

	pmap_simple_unlock(&pmap_ledger_lock);
}

MARK_AS_PMAP_TEXT static ledger_t
pmap_ledger_alloc_internal(void)
{
	pmap_paddr_t paddr;
	uint64_t vaddr, vstart, vend;
	uint64_t index;

	ledger_t new_ledger;
	uint64_t array_index;

	pmap_simple_lock(&pmap_ledger_lock);
	if (pmap_ledger_free_list == NULL) {
		paddr = pmap_get_free_ppl_page();

		if (paddr == 0) {
			pmap_simple_unlock(&pmap_ledger_lock);
			return NULL;
		}

		vstart = phystokv(paddr);
		vend = vstart + PAGE_SIZE;

		for (vaddr = vstart; (vaddr < vend) && ((vaddr + sizeof(pmap_ledger_t)) <= vend); vaddr += sizeof(pmap_ledger_t)) {
			pmap_ledger_t *free_ledger;

			index = pmap_ledger_ptr_array_free_index++;

			if (index >= MAX_PMAP_LEDGERS) {
				panic("%s: pmap_ledger_ptr_array is full, index=%llu",
				    __func__, index);
			}

			free_ledger = (pmap_ledger_t*)vaddr;

			pmap_ledger_ptr_array[index] = free_ledger;
			free_ledger->back_ptr = &pmap_ledger_ptr_array[index];

			free_ledger->next = pmap_ledger_free_list;
			pmap_ledger_free_list = free_ledger;
		}

		pa_set_range_xprr_perm(paddr, paddr + PAGE_SIZE, XPRR_PPL_RW_PERM, XPRR_KERN_RW_PERM);
	}

	new_ledger = (ledger_t)pmap_ledger_free_list;
	pmap_ledger_free_list = pmap_ledger_free_list->next;

	array_index = pmap_ledger_validate(new_ledger);
	os_ref_init(&pmap_ledger_refcnt[array_index], NULL);

	pmap_simple_unlock(&pmap_ledger_lock);

	return new_ledger;
}

MARK_AS_PMAP_TEXT static void
pmap_ledger_free_internal(ledger_t ledger)
{
	pmap_ledger_t* free_ledger;

	free_ledger = (pmap_ledger_t*)ledger;

	pmap_simple_lock(&pmap_ledger_lock);
	uint64_t array_index = pmap_ledger_validate(ledger);

	if (os_ref_release(&pmap_ledger_refcnt[array_index]) != 0) {
		panic("%s: ledger still referenced, "
		    "ledger=%p",
		    __func__,
		    ledger);
	}

	free_ledger->next = pmap_ledger_free_list;
	pmap_ledger_free_list = free_ledger;
	pmap_simple_unlock(&pmap_ledger_lock);
}


static void
pmap_ledger_retain(ledger_t ledger)
{
	pmap_simple_lock(&pmap_ledger_lock);
	uint64_t array_index = pmap_ledger_validate(ledger);
	os_ref_retain(&pmap_ledger_refcnt[array_index]);
	pmap_simple_unlock(&pmap_ledger_lock);
}

static void
pmap_ledger_release(ledger_t ledger)
{
	pmap_simple_lock(&pmap_ledger_lock);
	uint64_t array_index = pmap_ledger_validate(ledger);
	os_ref_release_live(&pmap_ledger_refcnt[array_index]);
	pmap_simple_unlock(&pmap_ledger_lock);
}

void
pmap_ledger_alloc_init(size_t size)
{
	pmap_ledger_alloc_init_ppl(size);
}

ledger_t
pmap_ledger_alloc(void)
{
	ledger_t retval = NULL;

	while ((retval = pmap_ledger_alloc_ppl()) == NULL) {
		pmap_alloc_page_for_ppl();
	}

	return retval;
}

void
pmap_ledger_free(ledger_t ledger)
{
	pmap_ledger_free_ppl(ledger);
}
#else /* XNU_MONITOR */
__dead2
void
pmap_ledger_alloc_init(size_t size)
{
	panic("%s: unsupported, "
	    "size=%lu",
	    __func__, size);
}

__dead2
ledger_t
pmap_ledger_alloc(void)
{
	panic("%s: unsupported",
	    __func__);
}

__dead2
void
pmap_ledger_free(ledger_t ledger)
{
	panic("%s: unsupported, "
	    "ledger=%p",
	    __func__, ledger);
}
#endif /* XNU_MONITOR */

/*
 *	Create and return a physical map.
 *
 *	If the size specified for the map
 *	is zero, the map is an actual physical
 *	map, and may be referenced by the
 *	hardware.
 *
 *	If the size specified is non-zero,
 *	the map will be used in software only, and
 *	is bounded by that size.
 */
MARK_AS_PMAP_TEXT static pmap_t
pmap_create_options_internal(
	ledger_t ledger,
	vm_map_size_t size,
	unsigned int flags)
{
	unsigned        i;
	unsigned        tte_index_max;
	pmap_t          p;
	bool is_64bit = flags & PMAP_CREATE_64BIT;
#if defined(HAS_APPLE_PAC)
	bool disable_jop = flags & PMAP_CREATE_DISABLE_JOP;
#endif /* defined(HAS_APPLE_PAC) */

	/*
	 *	A software use-only map doesn't even need a pmap.
	 */
	if (size != 0) {
		return PMAP_NULL;
	}

#if XNU_MONITOR
	if ((p = pmap_alloc_pmap()) == PMAP_NULL) {
		return PMAP_NULL;
	}
#else
	/*
	 *	Allocate a pmap struct from the pmap_zone.  Then allocate
	 *	the translation table of the right size for the pmap.
	 */
	if ((p = (pmap_t) zalloc(pmap_zone)) == PMAP_NULL) {
		return PMAP_NULL;
	}
#endif

	if (flags & PMAP_CREATE_64BIT) {
		p->min = MACH_VM_MIN_ADDRESS;
		p->max = MACH_VM_MAX_ADDRESS;
	} else {
		p->min = VM_MIN_ADDRESS;
		p->max = VM_MAX_ADDRESS;
	}

#if defined(HAS_APPLE_PAC)
	p->disable_jop = disable_jop;
#endif /* defined(HAS_APPLE_PAC) */

	p->nested_region_true_start = 0;
	p->nested_region_true_end = ~0;

	os_atomic_init(&p->ref_count, 1);
	p->gc_status = 0;
	p->stamp = os_atomic_inc(&pmap_stamp, relaxed);
	p->nx_enabled = TRUE;
	p->is_64bit = is_64bit;
	p->nested = FALSE;
	p->nested_pmap = PMAP_NULL;

#if ARM_PARAMETERIZED_PMAP
	p->pmap_pt_attr = native_pt_attr;
#endif /* ARM_PARAMETERIZED_PMAP */

	if (!pmap_get_pt_ops(p)->alloc_id(p)) {
		goto id_alloc_fail;
	}


#if XNU_MONITOR
	if (ledger) {
		pmap_ledger_validate(ledger);
		pmap_ledger_retain(ledger);
	}
#endif /* XNU_MONITOR */

	p->ledger = ledger;

	PMAP_LOCK_INIT(p);
	memset((void *) &p->stats, 0, sizeof(p->stats));

	p->tt_entry_free = (tt_entry_t *)0;
	tte_index_max = PMAP_ROOT_ALLOC_SIZE / sizeof(tt_entry_t);

#if     (__ARM_VMSA__ == 7)
	p->tte_index_max = tte_index_max;
#endif

#if XNU_MONITOR
	p->tte = pmap_tt1_allocate(p, PMAP_ROOT_ALLOC_SIZE, PMAP_TT_ALLOCATE_NOWAIT);
#else
	p->tte = pmap_tt1_allocate(p, PMAP_ROOT_ALLOC_SIZE, 0);
#endif
	if (!(p->tte)) {
		goto tt1_alloc_fail;
	}

	p->ttep = ml_static_vtop((vm_offset_t)p->tte);
	PMAP_TRACE(3, PMAP_CODE(PMAP__TTE), VM_KERNEL_ADDRHIDE(p), VM_KERNEL_ADDRHIDE(p->min), VM_KERNEL_ADDRHIDE(p->max), p->ttep);

	/* nullify the translation table */
	for (i = 0; i < tte_index_max; i++) {
		p->tte[i] = ARM_TTE_TYPE_FAULT;
	}

	FLUSH_PTE_RANGE(p->tte, p->tte + tte_index_max);

	/*
	 *  initialize the rest of the structure
	 */
	p->nested_region_grand_addr = 0x0ULL;
	p->nested_region_subord_addr = 0x0ULL;
	p->nested_region_size = 0x0ULL;
	p->nested_region_asid_bitmap = NULL;
	p->nested_region_asid_bitmap_size = 0x0UL;

	p->nested_has_no_bounds_ref = false;
	p->nested_no_bounds_refcnt = 0;
	p->nested_bounds_set = false;


#if MACH_ASSERT
	p->pmap_stats_assert = TRUE;
	p->pmap_pid = 0;
	strlcpy(p->pmap_procname, "<nil>", sizeof(p->pmap_procname));
#endif /* MACH_ASSERT */
#if DEVELOPMENT || DEBUG
	p->footprint_was_suspended = FALSE;
#endif /* DEVELOPMENT || DEBUG */

	pmap_simple_lock(&pmaps_lock);
	queue_enter(&map_pmap_list, p, pmap_t, pmaps);
	pmap_simple_unlock(&pmaps_lock);

	return p;

tt1_alloc_fail:
	pmap_get_pt_ops(p)->free_id(p);
id_alloc_fail:
#if XNU_MONITOR
	pmap_free_pmap(p);

	if (ledger) {
		pmap_ledger_release(ledger);
	}
#else
	zfree(pmap_zone, p);
#endif
	return PMAP_NULL;
}

pmap_t
pmap_create_options(
	ledger_t ledger,
	vm_map_size_t size,
	unsigned int flags)
{
	pmap_t pmap;

	PMAP_TRACE(1, PMAP_CODE(PMAP__CREATE) | DBG_FUNC_START, size, flags);

	ledger_reference(ledger);

#if XNU_MONITOR
	/*
	 * TODO: It should be valid for pmap_create_options_internal to fail; we could
	 * be out of ASIDs.
	 */
	while ((pmap = pmap_create_options_ppl(ledger, size, flags)) == PMAP_NULL) {
		pmap_alloc_page_for_ppl();
	}
#else
	pmap = pmap_create_options_internal(ledger, size, flags);
#endif

	if (pmap == PMAP_NULL) {
		ledger_dereference(ledger);
	}

	PMAP_TRACE(1, PMAP_CODE(PMAP__CREATE) | DBG_FUNC_END, VM_KERNEL_ADDRHIDE(pmap), PMAP_VASID(pmap), pmap->hw_asid);

	return pmap;
}

#if XNU_MONITOR
/*
 * This symbol remains in place when the PPL is enabled so that the dispatch
 * table does not change from development to release configurations.
 */
#endif
#if MACH_ASSERT || XNU_MONITOR
MARK_AS_PMAP_TEXT static void
pmap_set_process_internal(
	__unused pmap_t pmap,
	__unused int pid,
	__unused char *procname)
{
#if MACH_ASSERT
	if (pmap == NULL) {
		return;
	}

	VALIDATE_PMAP(pmap);

	pmap->pmap_pid = pid;
	strlcpy(pmap->pmap_procname, procname, sizeof(pmap->pmap_procname));
	if (pmap_ledgers_panic_leeway) {
		/*
		 * XXX FBDP
		 * Some processes somehow trigger some issues that make
		 * the pmap stats and ledgers go off track, causing
		 * some assertion failures and ledger panics.
		 * Turn off the sanity checks if we allow some ledger leeway
		 * because of that.  We'll still do a final check in
		 * pmap_check_ledgers() for discrepancies larger than the
		 * allowed leeway after the address space has been fully
		 * cleaned up.
		 */
		pmap->pmap_stats_assert = FALSE;
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.phys_footprint);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.internal);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.internal_compressed);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.iokit_mapped);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.alternate_accounting);
		ledger_disable_panic_on_negative(pmap->ledger,
		    task_ledgers.alternate_accounting_compressed);
	}
#endif /* MACH_ASSERT */
}
#endif /* MACH_ASSERT || XNU_MONITOR */

#if MACH_ASSERT
void
pmap_set_process(
	pmap_t pmap,
	int pid,
	char *procname)
{
#if XNU_MONITOR
	pmap_set_process_ppl(pmap, pid, procname);
#else
	pmap_set_process_internal(pmap, pid, procname);
#endif
}
#endif /* MACH_ASSERT */

/*
 * We maintain stats and ledgers so that a task's physical footprint is:
 * phys_footprint = ((internal - alternate_accounting)
 *                   + (internal_compressed - alternate_accounting_compressed)
 *                   + iokit_mapped
 *                   + purgeable_nonvolatile
 *                   + purgeable_nonvolatile_compressed
 *                   + page_table)
 * where "alternate_accounting" includes "iokit" and "purgeable" memory.
 */


/*
 *	Retire the given physical map from service.
 *	Should only be called if the map contains
 *	no valid mappings.
 */
MARK_AS_PMAP_TEXT static void
pmap_destroy_internal(
	pmap_t pmap)
{
	if (pmap == PMAP_NULL) {
		return;
	}

	VALIDATE_PMAP(pmap);

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	int32_t ref_count = os_atomic_dec(&pmap->ref_count, relaxed);
	if (ref_count > 0) {
		return;
	} else if (ref_count < 0) {
		panic("pmap %p: refcount underflow", pmap);
	} else if (pmap == kernel_pmap) {
		panic("pmap %p: attempt to destroy kernel pmap", pmap);
	}

	pt_entry_t     *ttep;

#if (__ARM_VMSA__ > 7)
	pmap_unmap_sharedpage(pmap);
#endif /* (__ARM_VMSA__ > 7) */

	pmap_simple_lock(&pmaps_lock);
	while (pmap->gc_status & PMAP_GC_INFLIGHT) {
		pmap->gc_status |= PMAP_GC_WAIT;
		assert_wait((event_t) &pmap->gc_status, THREAD_UNINT);
		pmap_simple_unlock(&pmaps_lock);
		(void) thread_block(THREAD_CONTINUE_NULL);
		pmap_simple_lock(&pmaps_lock);
	}
	queue_remove(&map_pmap_list, pmap, pmap_t, pmaps);
	pmap_simple_unlock(&pmaps_lock);

	pmap_trim_self(pmap);

	/*
	 *	Free the memory maps, then the
	 *	pmap structure.
	 */
#if (__ARM_VMSA__ == 7)
	unsigned int i = 0;

	PMAP_LOCK(pmap);
	for (i = 0; i < pmap->tte_index_max; i++) {
		ttep = &pmap->tte[i];
		if ((*ttep & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
			pmap_tte_deallocate(pmap, ttep, PMAP_TT_L1_LEVEL);
		}
	}
	PMAP_UNLOCK(pmap);
#else /* (__ARM_VMSA__ == 7) */
	vm_map_address_t c;
	unsigned int level;

	for (level = pt_attr->pta_max_level - 1; level >= pt_attr->pta_root_level; level--) {
		for (c = pmap->min; c < pmap->max; c += pt_attr_ln_size(pt_attr, level)) {
			ttep = pmap_ttne(pmap, level, c);

			if ((ttep != PT_ENTRY_NULL) && (*ttep & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
				PMAP_LOCK(pmap);
				pmap_tte_deallocate(pmap, ttep, level);
				PMAP_UNLOCK(pmap);
			}
		}
	}
#endif /* (__ARM_VMSA__ == 7) */



	if (pmap->tte) {
#if (__ARM_VMSA__ == 7)
		pmap_tt1_deallocate(pmap, pmap->tte, pmap->tte_index_max * sizeof(tt_entry_t), 0);
		pmap->tte_index_max = 0;
#else /* (__ARM_VMSA__ == 7) */
		pmap_tt1_deallocate(pmap, pmap->tte, PMAP_ROOT_ALLOC_SIZE, 0);
#endif /* (__ARM_VMSA__ == 7) */
		pmap->tte = (tt_entry_t *) NULL;
		pmap->ttep = 0;
	}

	assert((tt_free_entry_t*)pmap->tt_entry_free == NULL);

	pmap_get_pt_ops(pmap)->flush_tlb_async(pmap);
	sync_tlb_flush();

	/* return its asid to the pool */
	pmap_get_pt_ops(pmap)->free_id(pmap);
	pmap_check_ledgers(pmap);

	if (pmap->nested_region_asid_bitmap) {
#if XNU_MONITOR
		pmap_pages_free(kvtophys((vm_offset_t)(pmap->nested_region_asid_bitmap)), PAGE_SIZE);
#else
		kfree(pmap->nested_region_asid_bitmap, pmap->nested_region_asid_bitmap_size * sizeof(unsigned int));
#endif
	}

#if XNU_MONITOR
	if (pmap->ledger) {
		pmap_ledger_release(pmap->ledger);
	}

	pmap_free_pmap(pmap);
#else
	zfree(pmap_zone, pmap);
#endif
}

void
pmap_destroy(
	pmap_t pmap)
{
	ledger_t ledger;

	PMAP_TRACE(1, PMAP_CODE(PMAP__DESTROY) | DBG_FUNC_START, VM_KERNEL_ADDRHIDE(pmap), PMAP_VASID(pmap), pmap->hw_asid);

	ledger = pmap->ledger;

#if XNU_MONITOR
	pmap_destroy_ppl(pmap);

	pmap_check_ledger_fields(ledger);
#else
	pmap_destroy_internal(pmap);
#endif

	ledger_dereference(ledger);

	PMAP_TRACE(1, PMAP_CODE(PMAP__DESTROY) | DBG_FUNC_END);
}


/*
 *	Add a reference to the specified pmap.
 */
MARK_AS_PMAP_TEXT static void
pmap_reference_internal(
	pmap_t pmap)
{
	if (pmap != PMAP_NULL) {
		VALIDATE_PMAP(pmap);
		os_atomic_inc(&pmap->ref_count, relaxed);
	}
}

void
pmap_reference(
	pmap_t pmap)
{
#if XNU_MONITOR
	pmap_reference_ppl(pmap);
#else
	pmap_reference_internal(pmap);
#endif
}

static tt_entry_t *
pmap_tt1_allocate(
	pmap_t          pmap,
	vm_size_t       size,
	unsigned        option)
{
	tt_entry_t      *tt1 = NULL;
	tt_free_entry_t *tt1_free;
	pmap_paddr_t    pa;
	vm_address_t    va;
	vm_address_t    va_end;
	kern_return_t   ret;

	pmap_simple_lock(&tt1_lock);
	if ((size == PAGE_SIZE) && (free_page_size_tt_count != 0)) {
		free_page_size_tt_count--;
		tt1 = (tt_entry_t *)free_page_size_tt_list;
		free_page_size_tt_list = ((tt_free_entry_t *)tt1)->next;
	} else if ((size == 2 * PAGE_SIZE) && (free_two_page_size_tt_count != 0)) {
		free_two_page_size_tt_count--;
		tt1 = (tt_entry_t *)free_two_page_size_tt_list;
		free_two_page_size_tt_list = ((tt_free_entry_t *)tt1)->next;
	} else if ((size < PAGE_SIZE) && (free_tt_count != 0)) {
		free_tt_count--;
		tt1 = (tt_entry_t *)free_tt_list;
		free_tt_list = (tt_free_entry_t *)((tt_free_entry_t *)tt1)->next;
	}

	pmap_simple_unlock(&tt1_lock);

	if (tt1 != NULL) {
		pmap_tt_ledger_credit(pmap, size);
		return (tt_entry_t *)tt1;
	}

	ret = pmap_pages_alloc(&pa, (unsigned)((size < PAGE_SIZE)? PAGE_SIZE : size), ((option & PMAP_TT_ALLOCATE_NOWAIT)? PMAP_PAGES_ALLOCATE_NOWAIT : 0));

	if (ret == KERN_RESOURCE_SHORTAGE) {
		return (tt_entry_t *)0;
	}

#if XNU_MONITOR
	assert(pa);
#endif

	if (size < PAGE_SIZE) {
		va = phystokv(pa) + size;
		tt_free_entry_t *local_free_list = (tt_free_entry_t*)va;
		tt_free_entry_t *next_free = NULL;
		for (va_end = phystokv(pa) + PAGE_SIZE; va < va_end; va = va + size) {
			tt1_free = (tt_free_entry_t *)va;
			tt1_free->next = next_free;
			next_free = tt1_free;
		}
		pmap_simple_lock(&tt1_lock);
		local_free_list->next = free_tt_list;
		free_tt_list = next_free;
		free_tt_count += ((PAGE_SIZE / size) - 1);
		if (free_tt_count > free_tt_max) {
			free_tt_max = free_tt_count;
		}
		pmap_simple_unlock(&tt1_lock);
	}

	/* Always report root allocations in units of PMAP_ROOT_ALLOC_SIZE, which can be obtained by sysctl arm_pt_root_size.
	 * Depending on the device, this can vary between 512b and 16K. */
	OSAddAtomic((uint32_t)(size / PMAP_ROOT_ALLOC_SIZE), (pmap == kernel_pmap ? &inuse_kernel_tteroot_count : &inuse_user_tteroot_count));
	OSAddAtomic64(size / PMAP_ROOT_ALLOC_SIZE, &alloc_tteroot_count);
	pmap_tt_ledger_credit(pmap, size);

	return (tt_entry_t *) phystokv(pa);
}

static void
pmap_tt1_deallocate(
	pmap_t pmap,
	tt_entry_t *tt,
	vm_size_t size,
	unsigned option)
{
	tt_free_entry_t *tt_entry;

	tt_entry = (tt_free_entry_t *)tt;
	assert(not_in_kdp);
	pmap_simple_lock(&tt1_lock);

	if (size < PAGE_SIZE) {
		free_tt_count++;
		if (free_tt_count > free_tt_max) {
			free_tt_max = free_tt_count;
		}
		tt_entry->next = free_tt_list;
		free_tt_list = tt_entry;
	}

	if (size == PAGE_SIZE) {
		free_page_size_tt_count++;
		if (free_page_size_tt_count > free_page_size_tt_max) {
			free_page_size_tt_max = free_page_size_tt_count;
		}
		tt_entry->next = free_page_size_tt_list;
		free_page_size_tt_list = tt_entry;
	}

	if (size == 2 * PAGE_SIZE) {
		free_two_page_size_tt_count++;
		if (free_two_page_size_tt_count > free_two_page_size_tt_max) {
			free_two_page_size_tt_max = free_two_page_size_tt_count;
		}
		tt_entry->next = free_two_page_size_tt_list;
		free_two_page_size_tt_list = tt_entry;
	}

	if (option & PMAP_TT_DEALLOCATE_NOBLOCK) {
		pmap_simple_unlock(&tt1_lock);
		pmap_tt_ledger_debit(pmap, size);
		return;
	}

	while (free_page_size_tt_count > FREE_PAGE_SIZE_TT_MAX) {
		free_page_size_tt_count--;
		tt = (tt_entry_t *)free_page_size_tt_list;
		free_page_size_tt_list = ((tt_free_entry_t *)tt)->next;

		pmap_simple_unlock(&tt1_lock);

		pmap_pages_free(ml_static_vtop((vm_offset_t)tt), PAGE_SIZE);

		OSAddAtomic(-(int32_t)(PAGE_SIZE / PMAP_ROOT_ALLOC_SIZE), (pmap == kernel_pmap ? &inuse_kernel_tteroot_count : &inuse_user_tteroot_count));

		pmap_simple_lock(&tt1_lock);
	}

	while (free_two_page_size_tt_count > FREE_TWO_PAGE_SIZE_TT_MAX) {
		free_two_page_size_tt_count--;
		tt = (tt_entry_t *)free_two_page_size_tt_list;
		free_two_page_size_tt_list = ((tt_free_entry_t *)tt)->next;

		pmap_simple_unlock(&tt1_lock);

		pmap_pages_free(ml_static_vtop((vm_offset_t)tt), 2 * PAGE_SIZE);

		OSAddAtomic(-2 * (int32_t)(PAGE_SIZE / PMAP_ROOT_ALLOC_SIZE), (pmap == kernel_pmap ? &inuse_kernel_tteroot_count : &inuse_user_tteroot_count));

		pmap_simple_lock(&tt1_lock);
	}
	pmap_simple_unlock(&tt1_lock);
	pmap_tt_ledger_debit(pmap, size);
}

static kern_return_t
pmap_tt_allocate(
	pmap_t pmap,
	tt_entry_t **ttp,
	unsigned int level,
	unsigned int options)
{
	pmap_paddr_t pa;
	*ttp = NULL;

	PMAP_LOCK(pmap);
	if ((tt_free_entry_t *)pmap->tt_entry_free != NULL) {
		tt_free_entry_t *tt_free_next;

		tt_free_next = ((tt_free_entry_t *)pmap->tt_entry_free)->next;
		*ttp = (tt_entry_t *)pmap->tt_entry_free;
		pmap->tt_entry_free = (tt_entry_t *)tt_free_next;
	}
	PMAP_UNLOCK(pmap);

	if (*ttp == NULL) {
		pt_desc_t       *ptdp;

		/*
		 *  Allocate a VM page for the level x page table entries.
		 */
		while (pmap_pages_alloc(&pa, PAGE_SIZE, ((options & PMAP_TT_ALLOCATE_NOWAIT)? PMAP_PAGES_ALLOCATE_NOWAIT : 0)) != KERN_SUCCESS) {
			if (options & PMAP_OPTIONS_NOWAIT) {
				return KERN_RESOURCE_SHORTAGE;
			}
			VM_PAGE_WAIT();
		}

		while ((ptdp = ptd_alloc(pmap, false)) == NULL) {
			if (options & PMAP_OPTIONS_NOWAIT) {
				pmap_pages_free(pa, PAGE_SIZE);
				return KERN_RESOURCE_SHORTAGE;
			}
			VM_PAGE_WAIT();
		}

		if (level < PMAP_TT_MAX_LEVEL) {
			OSAddAtomic64(1, &alloc_ttepages_count);
			OSAddAtomic(1, (pmap == kernel_pmap ? &inuse_kernel_ttepages_count : &inuse_user_ttepages_count));
		} else {
			OSAddAtomic64(1, &alloc_ptepages_count);
			OSAddAtomic(1, (pmap == kernel_pmap ? &inuse_kernel_ptepages_count : &inuse_user_ptepages_count));
		}

		pmap_tt_ledger_credit(pmap, PAGE_SIZE);

		PMAP_ZINFO_PALLOC(pmap, PAGE_SIZE);

		pvh_update_head_unlocked(pai_to_pvh(pa_index(pa)), ptdp, PVH_TYPE_PTDP);

		__unreachable_ok_push
		if (TEST_PAGE_RATIO_4) {
			vm_address_t    va;
			vm_address_t    va_end;

			PMAP_LOCK(pmap);

			for (va_end = phystokv(pa) + PAGE_SIZE, va = phystokv(pa) + ARM_PGBYTES; va < va_end; va = va + ARM_PGBYTES) {
				((tt_free_entry_t *)va)->next = (tt_free_entry_t *)pmap->tt_entry_free;
				pmap->tt_entry_free = (tt_entry_t *)va;
			}
			PMAP_UNLOCK(pmap);
		}
		__unreachable_ok_pop

		*ttp = (tt_entry_t *)phystokv(pa);
	}

#if XNU_MONITOR
	assert(*ttp);
#endif

	return KERN_SUCCESS;
}


static void
pmap_tt_deallocate(
	pmap_t pmap,
	tt_entry_t *ttp,
	unsigned int level)
{
	pt_desc_t *ptdp;
	unsigned pt_acc_cnt;
	unsigned i, max_pt_index = PAGE_RATIO;
	vm_offset_t     free_page = 0;

	PMAP_LOCK(pmap);

	ptdp = ptep_get_ptd((vm_offset_t)ttp);

	ptdp->ptd_info[ARM_PT_DESC_INDEX(ttp)].va = (vm_offset_t)-1;

	if ((level < PMAP_TT_MAX_LEVEL) && (ptdp->ptd_info[ARM_PT_DESC_INDEX(ttp)].refcnt == PT_DESC_REFCOUNT)) {
		ptdp->ptd_info[ARM_PT_DESC_INDEX(ttp)].refcnt = 0;
	}

	if (ptdp->ptd_info[ARM_PT_DESC_INDEX(ttp)].refcnt != 0) {
		panic("pmap_tt_deallocate(): ptdp %p, count %d\n", ptdp, ptdp->ptd_info[ARM_PT_DESC_INDEX(ttp)].refcnt);
	}

	ptdp->ptd_info[ARM_PT_DESC_INDEX(ttp)].refcnt = 0;

	for (i = 0, pt_acc_cnt = 0; i < max_pt_index; i++) {
		pt_acc_cnt += ptdp->ptd_info[i].refcnt;
	}

	if (pt_acc_cnt == 0) {
		tt_free_entry_t *tt_free_list = (tt_free_entry_t *)&pmap->tt_entry_free;
		unsigned pt_free_entry_cnt = 1;

		while (pt_free_entry_cnt < max_pt_index && tt_free_list) {
			tt_free_entry_t *tt_free_list_next;

			tt_free_list_next = tt_free_list->next;
			if ((((vm_offset_t)tt_free_list_next) - ((vm_offset_t)ttp & ~PAGE_MASK)) < PAGE_SIZE) {
				pt_free_entry_cnt++;
			}
			tt_free_list = tt_free_list_next;
		}
		if (pt_free_entry_cnt == max_pt_index) {
			tt_free_entry_t *tt_free_list_cur;

			free_page = (vm_offset_t)ttp & ~PAGE_MASK;
			tt_free_list = (tt_free_entry_t *)&pmap->tt_entry_free;
			tt_free_list_cur = (tt_free_entry_t *)&pmap->tt_entry_free;

			while (tt_free_list_cur) {
				tt_free_entry_t *tt_free_list_next;

				tt_free_list_next = tt_free_list_cur->next;
				if ((((vm_offset_t)tt_free_list_next) - free_page) < PAGE_SIZE) {
					tt_free_list->next = tt_free_list_next->next;
				} else {
					tt_free_list = tt_free_list_next;
				}
				tt_free_list_cur = tt_free_list_next;
			}
		} else {
			((tt_free_entry_t *)ttp)->next = (tt_free_entry_t *)pmap->tt_entry_free;
			pmap->tt_entry_free = ttp;
		}
	} else {
		((tt_free_entry_t *)ttp)->next = (tt_free_entry_t *)pmap->tt_entry_free;
		pmap->tt_entry_free = ttp;
	}

	PMAP_UNLOCK(pmap);

	if (free_page != 0) {
		ptd_deallocate(ptep_get_ptd((vm_offset_t)free_page));
		*(pt_desc_t **)pai_to_pvh(pa_index(ml_static_vtop(free_page))) = NULL;
		pmap_pages_free(ml_static_vtop(free_page), PAGE_SIZE);
		if (level < PMAP_TT_MAX_LEVEL) {
			OSAddAtomic(-1, (pmap == kernel_pmap ? &inuse_kernel_ttepages_count : &inuse_user_ttepages_count));
		} else {
			OSAddAtomic(-1, (pmap == kernel_pmap ? &inuse_kernel_ptepages_count : &inuse_user_ptepages_count));
		}
		PMAP_ZINFO_PFREE(pmap, PAGE_SIZE);
		pmap_tt_ledger_debit(pmap, PAGE_SIZE);
	}
}

static void
pmap_tte_remove(
	pmap_t pmap,
	tt_entry_t *ttep,
	unsigned int level)
{
	tt_entry_t tte = *ttep;

	if (tte == 0) {
		panic("pmap_tte_deallocate(): null tt_entry ttep==%p\n", ttep);
	}

	if (((level + 1) == PMAP_TT_MAX_LEVEL) && (tte_get_ptd(tte)->ptd_info[ARM_PT_DESC_INDEX(ttetokv(*ttep))].refcnt != 0)) {
		panic("pmap_tte_deallocate(): pmap=%p ttep=%p ptd=%p refcnt=0x%x \n", pmap, ttep,
		    tte_get_ptd(tte), (tte_get_ptd(tte)->ptd_info[ARM_PT_DESC_INDEX(ttetokv(*ttep))].refcnt));
	}

#if     (__ARM_VMSA__ == 7)
	{
		tt_entry_t *ttep_4M = (tt_entry_t *) ((vm_offset_t)ttep & 0xFFFFFFF0);
		unsigned i;

		for (i = 0; i < 4; i++, ttep_4M++) {
			*ttep_4M = (tt_entry_t) 0;
		}
		FLUSH_PTE_RANGE_STRONG(ttep_4M - 4, ttep_4M);
	}
#else
	*ttep = (tt_entry_t) 0;
	FLUSH_PTE_STRONG(ttep);
#endif
}

static void
pmap_tte_deallocate(
	pmap_t pmap,
	tt_entry_t *ttep,
	unsigned int level)
{
	pmap_paddr_t pa;
	tt_entry_t tte;

	PMAP_ASSERT_LOCKED(pmap);

	tte = *ttep;

#if     MACH_ASSERT
	if (tte_get_ptd(tte)->pmap != pmap) {
		panic("pmap_tte_deallocate(): ptd=%p ptd->pmap=%p pmap=%p \n",
		    tte_get_ptd(tte), tte_get_ptd(tte)->pmap, pmap);
	}
#endif

	pmap_tte_remove(pmap, ttep, level);

	if ((tte & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
#if     MACH_ASSERT
		{
			pt_entry_t      *pte_p = ((pt_entry_t *) (ttetokv(tte) & ~ARM_PGMASK));
			unsigned        i;

			for (i = 0; i < (ARM_PGBYTES / sizeof(*pte_p)); i++, pte_p++) {
				if (ARM_PTE_IS_COMPRESSED(*pte_p, pte_p)) {
					panic("pmap_tte_deallocate: tte=0x%llx pmap=%p, pte_p=%p, pte=0x%llx compressed\n",
					    (uint64_t)tte, pmap, pte_p, (uint64_t)(*pte_p));
				} else if (((*pte_p) & ARM_PTE_TYPE_MASK) != ARM_PTE_TYPE_FAULT) {
					panic("pmap_tte_deallocate: tte=0x%llx pmap=%p, pte_p=%p, pte=0x%llx\n",
					    (uint64_t)tte, pmap, pte_p, (uint64_t)(*pte_p));
				}
			}
		}
#endif
		PMAP_UNLOCK(pmap);

		/* Clear any page offset: we mean to free the whole page, but armv7 TTEs may only be
		 * aligned on 1K boundaries.  We clear the surrounding "chunk" of 4 TTEs above. */
		pa = tte_to_pa(tte) & ~ARM_PGMASK;
		pmap_tt_deallocate(pmap, (tt_entry_t *) phystokv(pa), level + 1);
		PMAP_LOCK(pmap);
	}
}

/*
 *	Remove a range of hardware page-table entries.
 *	The entries given are the first (inclusive)
 *	and last (exclusive) entries for the VM pages.
 *	The virtual address is the va for the first pte.
 *
 *	The pmap must be locked.
 *	If the pmap is not the kernel pmap, the range must lie
 *	entirely within one pte-page.  This is NOT checked.
 *	Assumes that the pte-page exists.
 *
 *	Returns the number of PTE changed, and sets *rmv_cnt
 *	to the number of SPTE changed.
 */
static int
pmap_remove_range(
	pmap_t pmap,
	vm_map_address_t va,
	pt_entry_t *bpte,
	pt_entry_t *epte,
	uint32_t *rmv_cnt)
{
	bool need_strong_sync = false;
	int num_changed = pmap_remove_range_options(pmap, va, bpte, epte, rmv_cnt,
	    &need_strong_sync, PMAP_OPTIONS_REMOVE);
	if (num_changed > 0) {
		PMAP_UPDATE_TLBS(pmap, va, va + (PAGE_SIZE * (epte - bpte)), need_strong_sync);
	}
	return num_changed;
}


#ifdef PVH_FLAG_EXEC

/*
 *	Update the access protection bits of the physical aperture mapping for a page.
 *	This is useful, for example, in guranteeing that a verified executable page
 *	has no writable mappings anywhere in the system, including the physical
 *	aperture.  flush_tlb_async can be set to true to avoid unnecessary TLB
 *	synchronization overhead in cases where the call to this function is
 *	guaranteed to be followed by other TLB operations.
 */
static void
pmap_set_ptov_ap(unsigned int pai __unused, unsigned int ap __unused, boolean_t flush_tlb_async __unused)
{
#if __ARM_PTE_PHYSMAP__
	ASSERT_PVH_LOCKED(pai);
	vm_offset_t kva = phystokv(vm_first_phys + (pmap_paddr_t)ptoa(pai));
	pt_entry_t *pte_p = pmap_pte(kernel_pmap, kva);

	pt_entry_t tmplate = *pte_p;
	if ((tmplate & ARM_PTE_APMASK) == ARM_PTE_AP(ap)) {
		return;
	}
	tmplate = (tmplate & ~ARM_PTE_APMASK) | ARM_PTE_AP(ap);
#if (__ARM_VMSA__ > 7)
	if (tmplate & ARM_PTE_HINT_MASK) {
		panic("%s: physical aperture PTE %p has hint bit set, va=%p, pte=0x%llx",
		    __func__, pte_p, (void *)kva, tmplate);
	}
#endif
	WRITE_PTE_STRONG(pte_p, tmplate);
	flush_mmu_tlb_region_asid_async(kva, PAGE_SIZE, kernel_pmap);
	if (!flush_tlb_async) {
		sync_tlb_flush();
	}
#endif
}

#endif /* defined(PVH_FLAG_EXEC) */

static void
pmap_remove_pv(
	pmap_t pmap,
	pt_entry_t *cpte,
	int pai,
	int *num_internal,
	int *num_alt_internal,
	int *num_reusable,
	int *num_external)
{
	pv_entry_t    **pv_h, **pve_pp;
	pv_entry_t     *pve_p;

	ASSERT_PVH_LOCKED(pai);
	pv_h = pai_to_pvh(pai);
	vm_offset_t pvh_flags = pvh_get_flags(pv_h);

#if XNU_MONITOR
	if (pvh_flags & PVH_FLAG_LOCKDOWN) {
		panic("%d is locked down (%#lx), cannot remove", pai, pvh_flags);
	}
#endif

	if (pvh_test_type(pv_h, PVH_TYPE_PTEP)) {
		if (__builtin_expect((cpte != pvh_ptep(pv_h)), 0)) {
			panic("%s: cpte=%p does not match pv_h=%p (%p), pai=0x%x\n", __func__, cpte, pv_h, pvh_ptep(pv_h), pai);
		}
		if (IS_ALTACCT_PAGE(pai, PV_ENTRY_NULL)) {
			assert(IS_INTERNAL_PAGE(pai));
			(*num_internal)++;
			(*num_alt_internal)++;
			CLR_ALTACCT_PAGE(pai, PV_ENTRY_NULL);
		} else if (IS_INTERNAL_PAGE(pai)) {
			if (IS_REUSABLE_PAGE(pai)) {
				(*num_reusable)++;
			} else {
				(*num_internal)++;
			}
		} else {
			(*num_external)++;
		}
		pvh_update_head(pv_h, PV_ENTRY_NULL, PVH_TYPE_NULL);
	} else if (pvh_test_type(pv_h, PVH_TYPE_PVEP)) {
		pve_pp = pv_h;
		pve_p = pvh_list(pv_h);

		while (pve_p != PV_ENTRY_NULL &&
		    (pve_get_ptep(pve_p) != cpte)) {
			pve_pp = pve_link_field(pve_p);
			pve_p = PVE_NEXT_PTR(pve_next(pve_p));
		}

		if (__builtin_expect((pve_p == PV_ENTRY_NULL), 0)) {
			panic("%s: cpte=%p (pai=0x%x) not in pv_h=%p\n", __func__, cpte, pai, pv_h);
		}

#if MACH_ASSERT
		if ((pmap != NULL) && (kern_feature_override(KF_PMAPV_OVRD) == FALSE)) {
			pv_entry_t *check_pve_p = PVE_NEXT_PTR(pve_next(pve_p));
			while (check_pve_p != PV_ENTRY_NULL) {
				if (pve_get_ptep(check_pve_p) == cpte) {
					panic("%s: duplicate pve entry cpte=%p pmap=%p, pv_h=%p, pve_p=%p, pai=0x%x",
					    __func__, cpte, pmap, pv_h, pve_p, pai);
				}
				check_pve_p = PVE_NEXT_PTR(pve_next(check_pve_p));
			}
		}
#endif

		if (IS_ALTACCT_PAGE(pai, pve_p)) {
			assert(IS_INTERNAL_PAGE(pai));
			(*num_internal)++;
			(*num_alt_internal)++;
			CLR_ALTACCT_PAGE(pai, pve_p);
		} else if (IS_INTERNAL_PAGE(pai)) {
			if (IS_REUSABLE_PAGE(pai)) {
				(*num_reusable)++;
			} else {
				(*num_internal)++;
			}
		} else {
			(*num_external)++;
		}

		pvh_remove(pv_h, pve_pp, pve_p);
		pv_free(pve_p);
		if (!pvh_test_type(pv_h, PVH_TYPE_NULL)) {
			pvh_set_flags(pv_h, pvh_flags);
		}
	} else {
		panic("%s: unexpected PV head %p, cpte=%p pmap=%p pv_h=%p pai=0x%x",
		    __func__, *pv_h, cpte, pmap, pv_h, pai);
	}

#ifdef PVH_FLAG_EXEC
	if ((pvh_flags & PVH_FLAG_EXEC) && pvh_test_type(pv_h, PVH_TYPE_NULL)) {
		pmap_set_ptov_ap(pai, AP_RWNA, FALSE);
	}
#endif
}

static int
pmap_remove_range_options(
	pmap_t pmap,
	vm_map_address_t va,
	pt_entry_t *bpte,
	pt_entry_t *epte,
	uint32_t *rmv_cnt,
	bool *need_strong_sync __unused,
	int options)
{
	pt_entry_t     *cpte;
	int             num_removed, num_unwired;
	int             num_pte_changed;
	int             pai = 0;
	pmap_paddr_t    pa;
	int             num_external, num_internal, num_reusable;
	int             num_alt_internal;
	uint64_t        num_compressed, num_alt_compressed;

	PMAP_ASSERT_LOCKED(pmap);

	num_removed = 0;
	num_unwired = 0;
	num_pte_changed = 0;
	num_external = 0;
	num_internal = 0;
	num_reusable = 0;
	num_compressed = 0;
	num_alt_internal = 0;
	num_alt_compressed = 0;

	for (cpte = bpte; cpte < epte;
	    cpte += PAGE_SIZE / ARM_PGBYTES, va += PAGE_SIZE) {
		pt_entry_t      spte;
		boolean_t       managed = FALSE;

		spte = *cpte;

#if CONFIG_PGTRACE
		if (pgtrace_enabled) {
			pmap_pgtrace_remove_clone(pmap, pte_to_pa(spte), va);
		}
#endif

		while (!managed) {
			if (pmap != kernel_pmap &&
			    (options & PMAP_OPTIONS_REMOVE) &&
			    (ARM_PTE_IS_COMPRESSED(spte, cpte))) {
				/*
				 * "pmap" must be locked at this point,
				 * so this should not race with another
				 * pmap_remove_range() or pmap_enter().
				 */

				/* one less "compressed"... */
				num_compressed++;
				if (spte & ARM_PTE_COMPRESSED_ALT) {
					/* ... but it used to be "ALTACCT" */
					num_alt_compressed++;
				}

				/* clear marker */
				WRITE_PTE_FAST(cpte, ARM_PTE_TYPE_FAULT);
				/*
				 * "refcnt" also accounts for
				 * our "compressed" markers,
				 * so let's update it here.
				 */
				if (OSAddAtomic16(-1, (SInt16 *) &(ptep_get_ptd(cpte)->ptd_info[ARM_PT_DESC_INDEX(cpte)].refcnt)) <= 0) {
					panic("pmap_remove_range_options: over-release of ptdp %p for pte %p\n", ptep_get_ptd(cpte), cpte);
				}
				spte = *cpte;
			}
			/*
			 * It may be possible for the pte to transition from managed
			 * to unmanaged in this timeframe; for now, elide the assert.
			 * We should break out as a consequence of checking pa_valid.
			 */
			//assert(!ARM_PTE_IS_COMPRESSED(spte));
			pa = pte_to_pa(spte);
			if (!pa_valid(pa)) {
#if XNU_MONITOR || HAS_MILD_DSB
				unsigned int cacheattr = pmap_cache_attributes((ppnum_t)atop(pa));
#endif
#if XNU_MONITOR
				if (!pmap_ppl_disable && (cacheattr & PP_ATTR_MONITOR)) {
					panic("%s: attempt to remove mapping of PPL-protected I/O address 0x%llx", __func__, (uint64_t)pa);
				}
#endif
				break;
			}
			pai = (int)pa_index(pa);
			LOCK_PVH(pai);
			spte = *cpte;
			pa = pte_to_pa(spte);
			if (pai == (int)pa_index(pa)) {
				managed = TRUE;
				break; // Leave pai locked as we will unlock it after we free the PV entry
			}
			UNLOCK_PVH(pai);
		}

		if (ARM_PTE_IS_COMPRESSED(*cpte, cpte)) {
			/*
			 * There used to be a valid mapping here but it
			 * has already been removed when the page was
			 * sent to the VM compressor, so nothing left to
			 * remove now...
			 */
			continue;
		}

		/* remove the translation, do not flush the TLB */
		if (*cpte != ARM_PTE_TYPE_FAULT) {
			assertf(!ARM_PTE_IS_COMPRESSED(*cpte, cpte), "unexpected compressed pte %p (=0x%llx)", cpte, (uint64_t)*cpte);
			assertf((*cpte & ARM_PTE_TYPE_VALID) == ARM_PTE_TYPE, "invalid pte %p (=0x%llx)", cpte, (uint64_t)*cpte);
#if MACH_ASSERT
			if (managed && (pmap != kernel_pmap) && (ptep_get_va(cpte) != va)) {
				panic("pmap_remove_range_options(): cpte=%p ptd=%p pte=0x%llx va=0x%llx\n",
				    cpte, ptep_get_ptd(cpte), (uint64_t)*cpte, (uint64_t)va);
			}
#endif
			WRITE_PTE_FAST(cpte, ARM_PTE_TYPE_FAULT);
			num_pte_changed++;
		}

		if ((spte != ARM_PTE_TYPE_FAULT) &&
		    (pmap != kernel_pmap)) {
			assertf(!ARM_PTE_IS_COMPRESSED(spte, cpte), "unexpected compressed pte %p (=0x%llx)", cpte, (uint64_t)spte);
			assertf((spte & ARM_PTE_TYPE_VALID) == ARM_PTE_TYPE, "invalid pte %p (=0x%llx)", cpte, (uint64_t)spte);
			if (OSAddAtomic16(-1, (SInt16 *) &(ptep_get_ptd(cpte)->ptd_info[ARM_PT_DESC_INDEX(cpte)].refcnt)) <= 0) {
				panic("pmap_remove_range_options: over-release of ptdp %p for pte %p\n", ptep_get_ptd(cpte), cpte);
			}
			if (rmv_cnt) {
				(*rmv_cnt)++;
			}
		}

		if (pte_is_wired(spte)) {
			pte_set_wired(cpte, 0);
			num_unwired++;
		}
		/*
		 * if not managed, we're done
		 */
		if (!managed) {
			continue;
		}
		/*
		 * find and remove the mapping from the chain for this
		 * physical address.
		 */

		pmap_remove_pv(pmap, cpte, pai, &num_internal, &num_alt_internal, &num_reusable, &num_external);

		UNLOCK_PVH(pai);
		num_removed++;
	}

	/*
	 *	Update the counts
	 */
	OSAddAtomic(-num_removed, (SInt32 *) &pmap->stats.resident_count);
	pmap_ledger_debit(pmap, task_ledgers.phys_mem, machine_ptob(num_removed));

	if (pmap != kernel_pmap) {
		/* sanity checks... */
#if MACH_ASSERT
		if (pmap->stats.internal < num_internal) {
			if ((!pmap_stats_assert ||
			    !pmap->pmap_stats_assert)) {
				printf("%d[%s] pmap_remove_range_options(%p,0x%llx,%p,%p,0x%x): num_internal=%d num_removed=%d num_unwired=%d num_external=%d num_reusable=%d num_compressed=%lld num_alt_internal=%d num_alt_compressed=%lld num_pte_changed=%d stats.internal=%d stats.reusable=%d\n",
				    pmap->pmap_pid,
				    pmap->pmap_procname,
				    pmap,
				    (uint64_t) va,
				    bpte,
				    epte,
				    options,
				    num_internal,
				    num_removed,
				    num_unwired,
				    num_external,
				    num_reusable,
				    num_compressed,
				    num_alt_internal,
				    num_alt_compressed,
				    num_pte_changed,
				    pmap->stats.internal,
				    pmap->stats.reusable);
			} else {
				panic("%d[%s] pmap_remove_range_options(%p,0x%llx,%p,%p,0x%x): num_internal=%d num_removed=%d num_unwired=%d num_external=%d num_reusable=%d num_compressed=%lld num_alt_internal=%d num_alt_compressed=%lld num_pte_changed=%d stats.internal=%d stats.reusable=%d",
				    pmap->pmap_pid,
				    pmap->pmap_procname,
				    pmap,
				    (uint64_t) va,
				    bpte,
				    epte,
				    options,
				    num_internal,
				    num_removed,
				    num_unwired,
				    num_external,
				    num_reusable,
				    num_compressed,
				    num_alt_internal,
				    num_alt_compressed,
				    num_pte_changed,
				    pmap->stats.internal,
				    pmap->stats.reusable);
			}
		}
#endif /* MACH_ASSERT */
		PMAP_STATS_ASSERTF(pmap->stats.external >= num_external,
		    pmap,
		    "pmap=%p num_external=%d stats.external=%d",
		    pmap, num_external, pmap->stats.external);
		PMAP_STATS_ASSERTF(pmap->stats.internal >= num_internal,
		    pmap,
		    "pmap=%p num_internal=%d stats.internal=%d num_reusable=%d stats.reusable=%d",
		    pmap,
		    num_internal, pmap->stats.internal,
		    num_reusable, pmap->stats.reusable);
		PMAP_STATS_ASSERTF(pmap->stats.reusable >= num_reusable,
		    pmap,
		    "pmap=%p num_internal=%d stats.internal=%d num_reusable=%d stats.reusable=%d",
		    pmap,
		    num_internal, pmap->stats.internal,
		    num_reusable, pmap->stats.reusable);
		PMAP_STATS_ASSERTF(pmap->stats.compressed >= num_compressed,
		    pmap,
		    "pmap=%p num_compressed=%lld num_alt_compressed=%lld stats.compressed=%lld",
		    pmap, num_compressed, num_alt_compressed,
		    pmap->stats.compressed);

		/* update pmap stats... */
		OSAddAtomic(-num_unwired, (SInt32 *) &pmap->stats.wired_count);
		if (num_external) {
			OSAddAtomic(-num_external, &pmap->stats.external);
		}
		if (num_internal) {
			OSAddAtomic(-num_internal, &pmap->stats.internal);
		}
		if (num_reusable) {
			OSAddAtomic(-num_reusable, &pmap->stats.reusable);
		}
		if (num_compressed) {
			OSAddAtomic64(-num_compressed, &pmap->stats.compressed);
		}
		/* ... and ledgers */
		pmap_ledger_debit(pmap, task_ledgers.wired_mem, machine_ptob(num_unwired));
		pmap_ledger_debit(pmap, task_ledgers.internal, machine_ptob(num_internal));
		pmap_ledger_debit(pmap, task_ledgers.alternate_accounting, machine_ptob(num_alt_internal));
		pmap_ledger_debit(pmap, task_ledgers.alternate_accounting_compressed, machine_ptob(num_alt_compressed));
		pmap_ledger_debit(pmap, task_ledgers.internal_compressed, machine_ptob(num_compressed));
		/* make needed adjustments to phys_footprint */
		pmap_ledger_debit(pmap, task_ledgers.phys_footprint,
		    machine_ptob((num_internal -
		    num_alt_internal) +
		    (num_compressed -
		    num_alt_compressed)));
	}

	/* flush the ptable entries we have written */
	if (num_pte_changed > 0) {
		FLUSH_PTE_RANGE_STRONG(bpte, epte);
	}

	return num_pte_changed;
}


/*
 *	Remove the given range of addresses
 *	from the specified map.
 *
 *	It is assumed that the start and end are properly
 *	rounded to the hardware page size.
 */
void
pmap_remove(
	pmap_t pmap,
	vm_map_address_t start,
	vm_map_address_t end)
{
	pmap_remove_options(pmap, start, end, PMAP_OPTIONS_REMOVE);
}

MARK_AS_PMAP_TEXT static int
pmap_remove_options_internal(
	pmap_t pmap,
	vm_map_address_t start,
	vm_map_address_t end,
	int options)
{
	int             remove_count = 0;
	pt_entry_t     *bpte, *epte;
	pt_entry_t     *pte_p;
	tt_entry_t     *tte_p;
	uint32_t        rmv_spte = 0;
	bool            need_strong_sync = false;
	bool            flush_tte = false;

	if (__improbable(end < start)) {
		panic("%s: invalid address range %p, %p", __func__, (void*)start, (void*)end);
	}

	VALIDATE_PMAP(pmap);

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	PMAP_LOCK(pmap);

	tte_p = pmap_tte(pmap, start);

	if (tte_p == (tt_entry_t *) NULL) {
		goto done;
	}

	if ((*tte_p & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
		pte_p = (pt_entry_t *) ttetokv(*tte_p);
		bpte = &pte_p[ptenum(start)];
		epte = bpte + ((end - start) >> pt_attr_leaf_shift(pt_attr));

		remove_count += pmap_remove_range_options(pmap, start, bpte, epte,
		    &rmv_spte, &need_strong_sync, options);

		if (rmv_spte && (ptep_get_ptd(pte_p)->ptd_info[ARM_PT_DESC_INDEX(pte_p)].refcnt == 0) &&
		    (pmap != kernel_pmap) && (pmap->nested == FALSE)) {
			pmap_tte_deallocate(pmap, tte_p, pt_attr_twig_level(pt_attr));
			flush_tte = true;
		}
	}

done:
	PMAP_UNLOCK(pmap);

	if (remove_count > 0) {
		PMAP_UPDATE_TLBS(pmap, start, end, need_strong_sync);
	} else if (flush_tte > 0) {
		pmap_get_pt_ops(pmap)->flush_tlb_tte_async(start, pmap);
		sync_tlb_flush();
	}
	return remove_count;
}

void
pmap_remove_options(
	pmap_t pmap,
	vm_map_address_t start,
	vm_map_address_t end,
	int options)
{
	int             remove_count = 0;
	vm_map_address_t va;

	if (pmap == PMAP_NULL) {
		return;
	}

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	PMAP_TRACE(2, PMAP_CODE(PMAP__REMOVE) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE(start),
	    VM_KERNEL_ADDRHIDE(end));

#if MACH_ASSERT
	if ((start | end) & PAGE_MASK) {
		panic("pmap_remove_options() pmap %p start 0x%llx end 0x%llx\n",
		    pmap, (uint64_t)start, (uint64_t)end);
	}
	if ((end < start) || (start < pmap->min) || (end > pmap->max)) {
		panic("pmap_remove_options(): invalid address range, pmap=%p, start=0x%llx, end=0x%llx\n",
		    pmap, (uint64_t)start, (uint64_t)end);
	}
#endif

	/*
	 *      Invalidate the translation buffer first
	 */
	va = start;
	while (va < end) {
		vm_map_address_t l;

		l = ((va + pt_attr_twig_size(pt_attr)) & ~pt_attr_twig_offmask(pt_attr));
		if (l > end) {
			l = end;
		}

#if XNU_MONITOR
		remove_count += pmap_remove_options_ppl(pmap, va, l, options);

		pmap_ledger_check_balance(pmap);
#else
		remove_count += pmap_remove_options_internal(pmap, va, l, options);
#endif

		va = l;
	}

	PMAP_TRACE(2, PMAP_CODE(PMAP__REMOVE) | DBG_FUNC_END);
}


/*
 *	Remove phys addr if mapped in specified map
 */
void
pmap_remove_some_phys(
	__unused pmap_t map,
	__unused ppnum_t pn)
{
	/* Implement to support working set code */
}

void
pmap_set_pmap(
	pmap_t pmap,
#if     !__ARM_USER_PROTECT__
	__unused
#endif
	thread_t        thread)
{
	pmap_switch(pmap);
#if __ARM_USER_PROTECT__
	if (pmap->tte_index_max == NTTES) {
		thread->machine.uptw_ttc = 2;
	} else {
		thread->machine.uptw_ttc = 1;
	}
	thread->machine.uptw_ttb = ((unsigned int) pmap->ttep) | TTBR_SETUP;
	thread->machine.asid = pmap->hw_asid;
#endif
}

static void
pmap_flush_core_tlb_asid(pmap_t pmap)
{
#if (__ARM_VMSA__ == 7)
	flush_core_tlb_asid(pmap->hw_asid);
#else
	flush_core_tlb_asid(((uint64_t) pmap->hw_asid) << TLBI_ASID_SHIFT);
#endif
}

MARK_AS_PMAP_TEXT static void
pmap_switch_internal(
	pmap_t pmap)
{
	VALIDATE_PMAP(pmap);
	pmap_cpu_data_t *cpu_data_ptr = pmap_get_cpu_data();
	uint16_t        asid_index = pmap->hw_asid;
	boolean_t       do_asid_flush = FALSE;

#if __ARM_KERNEL_PROTECT__
	asid_index >>= 1;
#endif

#if     (__ARM_VMSA__ > 7)
	pmap_t           last_nested_pmap = cpu_data_ptr->cpu_nested_pmap;
#endif

#if MAX_ASID > MAX_HW_ASID
	if (asid_index > 0) {
		asid_index -= 1;
		/* Paranoia. */
		assert(asid_index < (sizeof(cpu_data_ptr->cpu_asid_high_bits) / sizeof(*cpu_data_ptr->cpu_asid_high_bits)));

		/* Extract the "virtual" bits of the ASIDs (which could cause us to alias). */
		uint8_t asid_high_bits = pmap->sw_asid;
		uint8_t last_asid_high_bits = cpu_data_ptr->cpu_asid_high_bits[asid_index];

		if (asid_high_bits != last_asid_high_bits) {
			/*
			 * If the virtual ASID of the new pmap does not match the virtual ASID
			 * last seen on this CPU for the physical ASID (that was a mouthful),
			 * then this switch runs the risk of aliasing.  We need to flush the
			 * TLB for this phyiscal ASID in this case.
			 */
			cpu_data_ptr->cpu_asid_high_bits[asid_index] = asid_high_bits;
			do_asid_flush = TRUE;
		}
	}
#endif /* MAX_ASID > MAX_HW_ASID */

	pmap_switch_user_ttb_internal(pmap);

#if     (__ARM_VMSA__ > 7)
	/* If we're switching to a different nested pmap (i.e. shared region), we'll need
	 * to flush the userspace mappings for that region.  Those mappings are global
	 * and will not be protected by the ASID.  It should also be cheaper to flush the
	 * entire local TLB rather than to do a broadcast MMU flush by VA region. */
	if ((pmap != kernel_pmap) && (last_nested_pmap != NULL) && (pmap->nested_pmap != last_nested_pmap)) {
		flush_core_tlb();
	} else
#endif
	if (do_asid_flush) {
		pmap_flush_core_tlb_asid(pmap);
#if DEVELOPMENT || DEBUG
		os_atomic_inc(&pmap_asid_flushes, relaxed);
#endif
	}
}

void
pmap_switch(
	pmap_t pmap)
{
	PMAP_TRACE(1, PMAP_CODE(PMAP__SWITCH) | DBG_FUNC_START, VM_KERNEL_ADDRHIDE(pmap), PMAP_VASID(pmap), pmap->hw_asid);
#if XNU_MONITOR
	pmap_switch_ppl(pmap);
#else
	pmap_switch_internal(pmap);
#endif
	PMAP_TRACE(1, PMAP_CODE(PMAP__SWITCH) | DBG_FUNC_END);
}

void
pmap_page_protect(
	ppnum_t ppnum,
	vm_prot_t prot)
{
	pmap_page_protect_options(ppnum, prot, 0, NULL);
}

/*
 *	Routine:	pmap_page_protect_options
 *
 *	Function:
 *		Lower the permission for all mappings to a given
 *		page.
 */
MARK_AS_PMAP_TEXT static void
pmap_page_protect_options_internal(
	ppnum_t ppnum,
	vm_prot_t prot,
	unsigned int options)
{
	pmap_paddr_t    phys = ptoa(ppnum);
	pv_entry_t    **pv_h;
	pv_entry_t    **pve_pp;
	pv_entry_t     *pve_p;
	pv_entry_t     *pveh_p;
	pv_entry_t     *pvet_p;
	pt_entry_t     *pte_p;
	pv_entry_t     *new_pve_p;
	pt_entry_t     *new_pte_p;
	vm_offset_t     pvh_flags;
	int             pai;
	boolean_t       remove;
	boolean_t       set_NX;
	boolean_t       tlb_flush_needed = FALSE;
	unsigned int    pvh_cnt = 0;

	assert(ppnum != vm_page_fictitious_addr);

	/* Only work with managed pages. */
	if (!pa_valid(phys)) {
		return;
	}

	/*
	 * Determine the new protection.
	 */
	switch (prot) {
	case VM_PROT_ALL:
		return;         /* nothing to do */
	case VM_PROT_READ:
	case VM_PROT_READ | VM_PROT_EXECUTE:
		remove = FALSE;
		break;
	default:
		remove = TRUE;
		break;
	}

	pai = (int)pa_index(phys);
	LOCK_PVH(pai);
	pv_h = pai_to_pvh(pai);
	pvh_flags = pvh_get_flags(pv_h);

#if XNU_MONITOR
	if (remove && (pvh_flags & PVH_FLAG_LOCKDOWN)) {
		panic("%d is locked down (%#llx), cannot remove", pai, pvh_get_flags(pv_h));
	}
#endif

	pte_p = PT_ENTRY_NULL;
	pve_p = PV_ENTRY_NULL;
	pve_pp = pv_h;
	pveh_p = PV_ENTRY_NULL;
	pvet_p = PV_ENTRY_NULL;
	new_pve_p = PV_ENTRY_NULL;
	new_pte_p = PT_ENTRY_NULL;
	if (pvh_test_type(pv_h, PVH_TYPE_PTEP)) {
		pte_p = pvh_ptep(pv_h);
	} else if (pvh_test_type(pv_h, PVH_TYPE_PVEP)) {
		pve_p = pvh_list(pv_h);
		pveh_p = pve_p;
	}

	while ((pve_p != PV_ENTRY_NULL) || (pte_p != PT_ENTRY_NULL)) {
		vm_map_address_t va;
		pmap_t          pmap;
		pt_entry_t      tmplate;
		boolean_t       update = FALSE;

		if (pve_p != PV_ENTRY_NULL) {
			pte_p = pve_get_ptep(pve_p);
		}

#ifdef PVH_FLAG_IOMMU
		if ((vm_offset_t)pte_p & PVH_FLAG_IOMMU) {
#if XNU_MONITOR
			if (pvh_flags & PVH_FLAG_LOCKDOWN) {
				panic("pmap_page_protect: ppnum 0x%x locked down, cannot be owned by iommu 0x%llx, pve_p=%p",
				    ppnum, (uint64_t)pte_p & ~PVH_FLAG_IOMMU, pve_p);
			}
#endif
			if (remove) {
				if (options & PMAP_OPTIONS_COMPRESSOR) {
					panic("pmap_page_protect: attempt to compress ppnum 0x%x owned by iommu 0x%llx, pve_p=%p",
					    ppnum, (uint64_t)pte_p & ~PVH_FLAG_IOMMU, pve_p);
				}
				if (pve_p != PV_ENTRY_NULL) {
					pv_entry_t *temp_pve_p = PVE_NEXT_PTR(pve_next(pve_p));
					pvh_remove(pv_h, pve_pp, pve_p);
					pveh_p = pvh_list(pv_h);
					pve_next(pve_p) = new_pve_p;
					new_pve_p = pve_p;
					pve_p = temp_pve_p;
					continue;
				} else {
					new_pte_p = pte_p;
					break;
				}
			}
			goto protect_skip_pve;
		}
#endif
		pmap = ptep_get_pmap(pte_p);
		va = ptep_get_va(pte_p);

		if (pte_p == PT_ENTRY_NULL) {
			panic("pmap_page_protect: pmap=%p prot=%d options=%u, pv_h=%p, pveh_p=%p, pve_p=%p, va=0x%llx ppnum: 0x%x\n",
			    pmap, prot, options, pv_h, pveh_p, pve_p, (uint64_t)va, ppnum);
		} else if ((pmap == NULL) || (atop(pte_to_pa(*pte_p)) != ppnum)) {
#if MACH_ASSERT
			if (kern_feature_override(KF_PMAPV_OVRD) == FALSE) {
				pv_entry_t *check_pve_p = pveh_p;
				while (check_pve_p != PV_ENTRY_NULL) {
					if ((check_pve_p != pve_p) && (pve_get_ptep(check_pve_p) == pte_p)) {
						panic("pmap_page_protect: duplicate pve entry pte_p=%p pmap=%p prot=%d options=%u, pv_h=%p, pveh_p=%p, pve_p=%p, pte=0x%llx, va=0x%llx ppnum: 0x%x\n",
						    pte_p, pmap, prot, options, pv_h, pveh_p, pve_p, (uint64_t)*pte_p, (uint64_t)va, ppnum);
					}
					check_pve_p = PVE_NEXT_PTR(pve_next(check_pve_p));
				}
			}
#endif
			panic("pmap_page_protect: bad pve entry pte_p=%p pmap=%p prot=%d options=%u, pv_h=%p, pveh_p=%p, pve_p=%p, pte=0x%llx, va=0x%llx ppnum: 0x%x\n",
			    pte_p, pmap, prot, options, pv_h, pveh_p, pve_p, (uint64_t)*pte_p, (uint64_t)va, ppnum);
		}

#if DEVELOPMENT || DEBUG
		if ((prot & VM_PROT_EXECUTE) || !nx_enabled || !pmap->nx_enabled)
#else
		if ((prot & VM_PROT_EXECUTE))
#endif
		{ set_NX = FALSE;} else {
			set_NX = TRUE;
		}

		/* Remove the mapping if new protection is NONE */
		if (remove) {
			boolean_t is_altacct = FALSE;

			if (IS_ALTACCT_PAGE(pai, pve_p)) {
				is_altacct = TRUE;
			} else {
				is_altacct = FALSE;
			}

			if (pte_is_wired(*pte_p)) {
				pte_set_wired(pte_p, 0);
				if (pmap != kernel_pmap) {
					pmap_ledger_debit(pmap, task_ledgers.wired_mem, PAGE_SIZE);
					OSAddAtomic(-1, (SInt32 *) &pmap->stats.wired_count);
				}
			}

			if (*pte_p != ARM_PTE_TYPE_FAULT &&
			    pmap != kernel_pmap &&
			    (options & PMAP_OPTIONS_COMPRESSOR) &&
			    IS_INTERNAL_PAGE(pai)) {
				assert(!ARM_PTE_IS_COMPRESSED(*pte_p, pte_p));
				/* mark this PTE as having been "compressed" */
				tmplate = ARM_PTE_COMPRESSED;
				if (is_altacct) {
					tmplate |= ARM_PTE_COMPRESSED_ALT;
					is_altacct = TRUE;
				}
			} else {
				tmplate = ARM_PTE_TYPE_FAULT;
			}

			if ((*pte_p != ARM_PTE_TYPE_FAULT) &&
			    tmplate == ARM_PTE_TYPE_FAULT &&
			    (pmap != kernel_pmap)) {
				if (OSAddAtomic16(-1, (SInt16 *) &(ptep_get_ptd(pte_p)->ptd_info[ARM_PT_DESC_INDEX(pte_p)].refcnt)) <= 0) {
					panic("pmap_page_protect_options(): over-release of ptdp %p for pte %p\n", ptep_get_ptd(pte_p), pte_p);
				}
			}

			if (*pte_p != tmplate) {
				WRITE_PTE_STRONG(pte_p, tmplate);
				update = TRUE;
			}
			pvh_cnt++;
			pmap_ledger_debit(pmap, task_ledgers.phys_mem, PAGE_SIZE);
			OSAddAtomic(-1, (SInt32 *) &pmap->stats.resident_count);

#if MACH_ASSERT
			/*
			 * We only ever compress internal pages.
			 */
			if (options & PMAP_OPTIONS_COMPRESSOR) {
				assert(IS_INTERNAL_PAGE(pai));
			}
#endif

			if (pmap != kernel_pmap) {
				if (IS_REUSABLE_PAGE(pai) &&
				    IS_INTERNAL_PAGE(pai) &&
				    !is_altacct) {
					PMAP_STATS_ASSERTF(pmap->stats.reusable > 0, pmap, "stats.reusable %d", pmap->stats.reusable);
					OSAddAtomic(-1, &pmap->stats.reusable);
				} else if (IS_INTERNAL_PAGE(pai)) {
					PMAP_STATS_ASSERTF(pmap->stats.internal > 0, pmap, "stats.internal %d", pmap->stats.internal);
					OSAddAtomic(-1, &pmap->stats.internal);
				} else {
					PMAP_STATS_ASSERTF(pmap->stats.external > 0, pmap, "stats.external %d", pmap->stats.external);
					OSAddAtomic(-1, &pmap->stats.external);
				}
				if ((options & PMAP_OPTIONS_COMPRESSOR) &&
				    IS_INTERNAL_PAGE(pai)) {
					/* adjust "compressed" stats */
					OSAddAtomic64(+1, &pmap->stats.compressed);
					PMAP_STATS_PEAK(pmap->stats.compressed);
					pmap->stats.compressed_lifetime++;
				}

				if (IS_ALTACCT_PAGE(pai, pve_p)) {
					assert(IS_INTERNAL_PAGE(pai));
					pmap_ledger_debit(pmap, task_ledgers.internal, PAGE_SIZE);
					pmap_ledger_debit(pmap, task_ledgers.alternate_accounting, PAGE_SIZE);
					if (options & PMAP_OPTIONS_COMPRESSOR) {
						pmap_ledger_credit(pmap, task_ledgers.internal_compressed, PAGE_SIZE);
						pmap_ledger_credit(pmap, task_ledgers.alternate_accounting_compressed, PAGE_SIZE);
					}

					/*
					 * Cleanup our marker before
					 * we free this pv_entry.
					 */
					CLR_ALTACCT_PAGE(pai, pve_p);
				} else if (IS_REUSABLE_PAGE(pai)) {
					assert(IS_INTERNAL_PAGE(pai));
					if (options & PMAP_OPTIONS_COMPRESSOR) {
						pmap_ledger_credit(pmap, task_ledgers.internal_compressed, PAGE_SIZE);
						/* was not in footprint, but is now */
						pmap_ledger_credit(pmap, task_ledgers.phys_footprint, PAGE_SIZE);
					}
				} else if (IS_INTERNAL_PAGE(pai)) {
					pmap_ledger_debit(pmap, task_ledgers.internal, PAGE_SIZE);

					/*
					 * Update all stats related to physical footprint, which only
					 * deals with internal pages.
					 */
					if (options & PMAP_OPTIONS_COMPRESSOR) {
						/*
						 * This removal is only being done so we can send this page to
						 * the compressor; therefore it mustn't affect total task footprint.
						 */
						pmap_ledger_credit(pmap, task_ledgers.internal_compressed, PAGE_SIZE);
					} else {
						/*
						 * This internal page isn't going to the compressor, so adjust stats to keep
						 * phys_footprint up to date.
						 */
						pmap_ledger_debit(pmap, task_ledgers.phys_footprint, PAGE_SIZE);
					}
				} else {
					/* external page: no impact on ledgers */
				}
			}

			if (pve_p != PV_ENTRY_NULL) {
				assert(pve_next(pve_p) == PVE_NEXT_PTR(pve_next(pve_p)));
			}
		} else {
			pt_entry_t      spte;
			const pt_attr_t *const pt_attr = pmap_get_pt_attr(pmap);

			spte = *pte_p;

			if (pmap == kernel_pmap) {
				tmplate = ((spte & ~ARM_PTE_APMASK) | ARM_PTE_AP(AP_RONA));
			} else {
				tmplate = ((spte & ~ARM_PTE_APMASK) | pt_attr_leaf_ro(pt_attr));
			}

			pte_set_was_writeable(tmplate, false);
			/*
			 * While the naive implementation of this would serve to add execute
			 * permission, this is not how the VM uses this interface, or how
			 * x86_64 implements it.  So ignore requests to add execute permissions.
			 */
			if (set_NX) {
				tmplate |= pt_attr_leaf_xn(pt_attr);
			}

#if __APRR_SUPPORTED__
			if (__improbable(is_pte_xprr_protected(spte))) {
				panic("pmap_page_protect: modifying an xPRR mapping pte_p=%p pmap=%p prot=%d options=%u, pv_h=%p, pveh_p=%p, pve_p=%p, pte=0x%llx, tmplate=0x%llx, va=0x%llx ppnum: 0x%x",
				    pte_p, pmap, prot, options, pv_h, pveh_p, pve_p, (uint64_t)spte, (uint64_t)tmplate, (uint64_t)va, ppnum);
			}

			if (__improbable(is_pte_xprr_protected(tmplate))) {
				panic("pmap_page_protect: creating an xPRR mapping pte_p=%p pmap=%p prot=%d options=%u, pv_h=%p, pveh_p=%p, pve_p=%p, pte=0x%llx, tmplate=0x%llx, va=0x%llx ppnum: 0x%x",
				    pte_p, pmap, prot, options, pv_h, pveh_p, pve_p, (uint64_t)spte, (uint64_t)tmplate, (uint64_t)va, ppnum);
			}
#endif /* __APRR_SUPPORTED__*/

			if (*pte_p != ARM_PTE_TYPE_FAULT &&
			    !ARM_PTE_IS_COMPRESSED(*pte_p, pte_p) &&
			    *pte_p != tmplate) {
				WRITE_PTE_STRONG(pte_p, tmplate);
				update = TRUE;
			}
		}

		/* Invalidate TLBs for all CPUs using it */
		if (update) {
			tlb_flush_needed = TRUE;
			pmap_get_pt_ops(pmap)->flush_tlb_region_async(va, PAGE_SIZE, pmap);
		}

#ifdef PVH_FLAG_IOMMU
protect_skip_pve:
#endif
		pte_p = PT_ENTRY_NULL;
		pvet_p = pve_p;
		if (pve_p != PV_ENTRY_NULL) {
			if (remove) {
				assert(pve_next(pve_p) == PVE_NEXT_PTR(pve_next(pve_p)));
			}
			pve_pp = pve_link_field(pve_p);
			pve_p = PVE_NEXT_PTR(pve_next(pve_p));
		}
	}

#ifdef PVH_FLAG_EXEC
	if (remove && (pvh_get_flags(pv_h) & PVH_FLAG_EXEC)) {
		pmap_set_ptov_ap(pai, AP_RWNA, tlb_flush_needed);
	}
#endif
	if (tlb_flush_needed) {
		sync_tlb_flush();
	}

	/* if we removed a bunch of entries, take care of them now */
	if (remove) {
		if (new_pve_p != PV_ENTRY_NULL) {
			pvh_update_head(pv_h, new_pve_p, PVH_TYPE_PVEP);
			pvh_set_flags(pv_h, pvh_flags);
		} else if (new_pte_p != PT_ENTRY_NULL) {
			pvh_update_head(pv_h, new_pte_p, PVH_TYPE_PTEP);
			pvh_set_flags(pv_h, pvh_flags);
		} else {
			pvh_update_head(pv_h, PV_ENTRY_NULL, PVH_TYPE_NULL);
		}
	}

	UNLOCK_PVH(pai);

	if (remove && (pvet_p != PV_ENTRY_NULL)) {
		pv_list_free(pveh_p, pvet_p, pvh_cnt);
	}
}

void
pmap_page_protect_options(
	ppnum_t ppnum,
	vm_prot_t prot,
	unsigned int options,
	__unused void *arg)
{
	pmap_paddr_t    phys = ptoa(ppnum);

	assert(ppnum != vm_page_fictitious_addr);

	/* Only work with managed pages. */
	if (!pa_valid(phys)) {
		return;
	}

	/*
	 * Determine the new protection.
	 */
	if (prot == VM_PROT_ALL) {
		return;         /* nothing to do */
	}

	PMAP_TRACE(2, PMAP_CODE(PMAP__PAGE_PROTECT) | DBG_FUNC_START, ppnum, prot);

#if XNU_MONITOR
	pmap_page_protect_options_ppl(ppnum, prot, options);
#else
	pmap_page_protect_options_internal(ppnum, prot, options);
#endif

	PMAP_TRACE(2, PMAP_CODE(PMAP__PAGE_PROTECT) | DBG_FUNC_END);
}

/*
 * Indicates if the pmap layer enforces some additional restrictions on the
 * given set of protections.
 */
bool
pmap_has_prot_policy(__unused vm_prot_t prot)
{
	return FALSE;
}

/*
 *	Set the physical protection on the
 *	specified range of this map as requested.
 *	VERY IMPORTANT: Will not increase permissions.
 *	VERY IMPORTANT: Only pmap_enter() is allowed to grant permissions.
 */
void
pmap_protect(
	pmap_t pmap,
	vm_map_address_t b,
	vm_map_address_t e,
	vm_prot_t prot)
{
	pmap_protect_options(pmap, b, e, prot, 0, NULL);
}

MARK_AS_PMAP_TEXT static void
pmap_protect_options_internal(
	pmap_t pmap,
	vm_map_address_t start,
	vm_map_address_t end,
	vm_prot_t prot,
	unsigned int options,
	__unused void *args)
{
	const pt_attr_t *const pt_attr = pmap_get_pt_attr(pmap);
	tt_entry_t      *tte_p;
	pt_entry_t      *bpte_p, *epte_p;
	pt_entry_t      *pte_p;
	boolean_t        set_NX = TRUE;
#if (__ARM_VMSA__ > 7)
	boolean_t        set_XO = FALSE;
#endif
	boolean_t        should_have_removed = FALSE;
	bool             need_strong_sync = false;

	if (__improbable(end < start)) {
		panic("%s called with bogus range: %p, %p", __func__, (void*)start, (void*)end);
	}

#if DEVELOPMENT || DEBUG
	if (options & PMAP_OPTIONS_PROTECT_IMMEDIATE) {
		if ((prot & VM_PROT_ALL) == VM_PROT_NONE) {
			should_have_removed = TRUE;
		}
	} else
#endif
	{
		/* Determine the new protection. */
		switch (prot) {
#if (__ARM_VMSA__ > 7)
		case VM_PROT_EXECUTE:
			set_XO = TRUE;
			/* fall through */
#endif
		case VM_PROT_READ:
		case VM_PROT_READ | VM_PROT_EXECUTE:
			break;
		case VM_PROT_READ | VM_PROT_WRITE:
		case VM_PROT_ALL:
			return;         /* nothing to do */
		default:
			should_have_removed = TRUE;
		}
	}

	if (should_have_removed) {
		panic("%s: should have been a remove operation, "
		    "pmap=%p, start=%p, end=%p, prot=%#x, options=%#x, args=%p",
		    __FUNCTION__,
		    pmap, (void *)start, (void *)end, prot, options, args);
	}

#if DEVELOPMENT || DEBUG
	if ((prot & VM_PROT_EXECUTE) || !nx_enabled || !pmap->nx_enabled)
#else
	if ((prot & VM_PROT_EXECUTE))
#endif
	{
		set_NX = FALSE;
	} else {
		set_NX = TRUE;
	}

	VALIDATE_PMAP(pmap);
	PMAP_LOCK(pmap);
	tte_p = pmap_tte(pmap, start);

	if ((tte_p != (tt_entry_t *) NULL) && (*tte_p & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
		bpte_p = (pt_entry_t *) ttetokv(*tte_p);
		bpte_p = &bpte_p[ptenum(start)];
		epte_p = bpte_p + arm_atop(end - start);
		pte_p = bpte_p;

		for (pte_p = bpte_p;
		    pte_p < epte_p;
		    pte_p += PAGE_SIZE / ARM_PGBYTES) {
			pt_entry_t spte;
#if DEVELOPMENT || DEBUG
			boolean_t  force_write = FALSE;
#endif

			spte = *pte_p;

			if ((spte == ARM_PTE_TYPE_FAULT) ||
			    ARM_PTE_IS_COMPRESSED(spte, pte_p)) {
				continue;
			}

			pmap_paddr_t    pa;
			int             pai = 0;
			boolean_t       managed = FALSE;

			while (!managed) {
				/*
				 * It may be possible for the pte to transition from managed
				 * to unmanaged in this timeframe; for now, elide the assert.
				 * We should break out as a consequence of checking pa_valid.
				 */
				// assert(!ARM_PTE_IS_COMPRESSED(spte));
				pa = pte_to_pa(spte);
				if (!pa_valid(pa)) {
					break;
				}
				pai = (int)pa_index(pa);
				LOCK_PVH(pai);
				spte = *pte_p;
				pa = pte_to_pa(spte);
				if (pai == (int)pa_index(pa)) {
					managed = TRUE;
					break; // Leave the PVH locked as we will unlock it after we free the PTE
				}
				UNLOCK_PVH(pai);
			}

			if ((spte == ARM_PTE_TYPE_FAULT) ||
			    ARM_PTE_IS_COMPRESSED(spte, pte_p)) {
				continue;
			}

			pt_entry_t      tmplate;

			if (pmap == kernel_pmap) {
#if DEVELOPMENT || DEBUG
				if ((options & PMAP_OPTIONS_PROTECT_IMMEDIATE) && (prot & VM_PROT_WRITE)) {
					force_write = TRUE;
					tmplate = ((spte & ~ARM_PTE_APMASK) | ARM_PTE_AP(AP_RWNA));
				} else
#endif
				{
					tmplate = ((spte & ~ARM_PTE_APMASK) | ARM_PTE_AP(AP_RONA));
				}
			} else {
#if DEVELOPMENT || DEBUG
				if ((options & PMAP_OPTIONS_PROTECT_IMMEDIATE) && (prot & VM_PROT_WRITE)) {
					force_write = TRUE;
					tmplate = ((spte & ~ARM_PTE_APMASK) | pt_attr_leaf_rw(pt_attr));
				} else
#endif
				{
					tmplate = ((spte & ~ARM_PTE_APMASK) | pt_attr_leaf_ro(pt_attr));
				}
			}

			/*
			 * XXX Removing "NX" would
			 * grant "execute" access
			 * immediately, bypassing any
			 * checks VM might want to do
			 * in its soft fault path.
			 * pmap_protect() and co. are
			 * not allowed to increase
			 * access permissions.
			 */
			if (set_NX) {
				tmplate |= pt_attr_leaf_xn(pt_attr);
			} else {
#if     (__ARM_VMSA__ > 7)
				if (pmap == kernel_pmap) {
					/* do NOT clear "PNX"! */
					tmplate |= ARM_PTE_NX;
				} else {
					/* do NOT clear "NX"! */
					tmplate |= pt_attr_leaf_x(pt_attr);
					if (set_XO) {
						tmplate &= ~ARM_PTE_APMASK;
						tmplate |= pt_attr_leaf_rona(pt_attr);
					}
				}
#endif
			}

#if DEVELOPMENT || DEBUG
			if (force_write) {
				/*
				 * TODO: Run CS/Monitor checks here.
				 */
				if (managed) {
					/*
					 * We are marking the page as writable,
					 * so we consider it to be modified and
					 * referenced.
					 */
					pa_set_bits(pa, PP_ATTR_REFERENCED | PP_ATTR_MODIFIED);
					tmplate |= ARM_PTE_AF;

					if (IS_REFFAULT_PAGE(pai)) {
						CLR_REFFAULT_PAGE(pai);
					}

					if (IS_MODFAULT_PAGE(pai)) {
						CLR_MODFAULT_PAGE(pai);
					}
				}
			} else if (options & PMAP_OPTIONS_PROTECT_IMMEDIATE) {
				/*
				 * An immediate request for anything other than
				 * write should still mark the page as
				 * referenced if managed.
				 */
				if (managed) {
					pa_set_bits(pa, PP_ATTR_REFERENCED);
					tmplate |= ARM_PTE_AF;

					if (IS_REFFAULT_PAGE(pai)) {
						CLR_REFFAULT_PAGE(pai);
					}
				}
			}
#endif

			/* We do not expect to write fast fault the entry. */
			pte_set_was_writeable(tmplate, false);

#if __APRR_SUPPORTED__
			if (__improbable(is_pte_xprr_protected(spte) && (pte_to_xprr_perm(spte) != XPRR_USER_JIT_PERM))) {
				/* Only test for PPL protection here,  User-JIT mappings may be mutated by this function. */
				panic("%s: modifying a PPL mapping pte_p=%p pmap=%p prot=%d options=%u, pte=0x%llx, tmplate=0x%llx",
				    __func__, pte_p, pmap, prot, options, (uint64_t)spte, (uint64_t)tmplate);
			}

			if (__improbable(is_pte_xprr_protected(tmplate))) {
				panic("%s: creating an xPRR mapping pte_p=%p pmap=%p prot=%d options=%u, pte=0x%llx, tmplate=0x%llx",
				    __func__, pte_p, pmap, prot, options, (uint64_t)spte, (uint64_t)tmplate);
			}
#endif /* __APRR_SUPPORTED__*/
			WRITE_PTE_FAST(pte_p, tmplate);

			if (managed) {
				ASSERT_PVH_LOCKED(pai);
				UNLOCK_PVH(pai);
			}
		}
		FLUSH_PTE_RANGE_STRONG(bpte_p, epte_p);
		PMAP_UPDATE_TLBS(pmap, start, end, need_strong_sync);
	}

	PMAP_UNLOCK(pmap);
}

void
pmap_protect_options(
	pmap_t pmap,
	vm_map_address_t b,
	vm_map_address_t e,
	vm_prot_t prot,
	unsigned int options,
	__unused void *args)
{
	vm_map_address_t l, beg;

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	if ((b | e) & PAGE_MASK) {
		panic("pmap_protect_options() pmap %p start 0x%llx end 0x%llx\n",
		    pmap, (uint64_t)b, (uint64_t)e);
	}

#if DEVELOPMENT || DEBUG
	if (options & PMAP_OPTIONS_PROTECT_IMMEDIATE) {
		if ((prot & VM_PROT_ALL) == VM_PROT_NONE) {
			pmap_remove_options(pmap, b, e, options);
			return;
		}
	} else
#endif
	{
		/* Determine the new protection. */
		switch (prot) {
		case VM_PROT_EXECUTE:
		case VM_PROT_READ:
		case VM_PROT_READ | VM_PROT_EXECUTE:
			break;
		case VM_PROT_READ | VM_PROT_WRITE:
		case VM_PROT_ALL:
			return;         /* nothing to do */
		default:
			pmap_remove_options(pmap, b, e, options);
			return;
		}
	}

	PMAP_TRACE(2, PMAP_CODE(PMAP__PROTECT) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE(b),
	    VM_KERNEL_ADDRHIDE(e));

	beg = b;

	while (beg < e) {
		l = ((beg + pt_attr_twig_size(pt_attr)) & ~pt_attr_twig_offmask(pt_attr));

		if (l > e) {
			l = e;
		}

#if XNU_MONITOR
		pmap_protect_options_ppl(pmap, beg, l, prot, options, args);
#else
		pmap_protect_options_internal(pmap, beg, l, prot, options, args);
#endif

		beg = l;
	}

	PMAP_TRACE(2, PMAP_CODE(PMAP__PROTECT) | DBG_FUNC_END);
}

/* Map a (possibly) autogenned block */
kern_return_t
pmap_map_block(
	pmap_t pmap,
	addr64_t va,
	ppnum_t pa,
	uint32_t size,
	vm_prot_t prot,
	int attr,
	__unused unsigned int flags)
{
	kern_return_t   kr;
	addr64_t        original_va = va;
	uint32_t        page;

	for (page = 0; page < size; page++) {
		kr = pmap_enter(pmap, va, pa, prot, VM_PROT_NONE, attr, TRUE);

		if (kr != KERN_SUCCESS) {
			/*
			 * This will panic for now, as it is unclear that
			 * removing the mappings is correct.
			 */
			panic("%s: failed pmap_enter, "
			    "pmap=%p, va=%#llx, pa=%u, size=%u, prot=%#x, flags=%#x",
			    __FUNCTION__,
			    pmap, va, pa, size, prot, flags);

			pmap_remove(pmap, original_va, va - original_va);
			return kr;
		}

		va += PAGE_SIZE;
		pa++;
	}

	return KERN_SUCCESS;
}

/*
 *	Insert the given physical page (p) at
 *	the specified virtual address (v) in the
 *	target physical map with the protection requested.
 *
 *	If specified, the page will be wired down, meaning
 *	that the related pte can not be reclaimed.
 *
 *	NB:  This is the only routine which MAY NOT lazy-evaluate
 *	or lose information.  That is, this routine must actually
 *	insert this page into the given map eventually (must make
 *	forward progress eventually.
 */
kern_return_t
pmap_enter(
	pmap_t pmap,
	vm_map_address_t v,
	ppnum_t pn,
	vm_prot_t prot,
	vm_prot_t fault_type,
	unsigned int flags,
	boolean_t wired)
{
	return pmap_enter_options(pmap, v, pn, prot, fault_type, flags, wired, 0, NULL);
}


static inline void
pmap_enter_pte(pmap_t pmap, pt_entry_t *pte_p, pt_entry_t pte, vm_map_address_t v)
{
	if (pmap != kernel_pmap && ((pte & ARM_PTE_WIRED) != (*pte_p & ARM_PTE_WIRED))) {
		SInt16  *ptd_wiredcnt_ptr = (SInt16 *)&(ptep_get_ptd(pte_p)->ptd_info[ARM_PT_DESC_INDEX(pte_p)].wiredcnt);
		if (pte & ARM_PTE_WIRED) {
			OSAddAtomic16(1, ptd_wiredcnt_ptr);
			pmap_ledger_credit(pmap, task_ledgers.wired_mem, PAGE_SIZE);
			OSAddAtomic(1, (SInt32 *) &pmap->stats.wired_count);
		} else {
			OSAddAtomic16(-1, ptd_wiredcnt_ptr);
			pmap_ledger_debit(pmap, task_ledgers.wired_mem, PAGE_SIZE);
			OSAddAtomic(-1, (SInt32 *) &pmap->stats.wired_count);
		}
	}
	if (*pte_p != ARM_PTE_TYPE_FAULT &&
	    !ARM_PTE_IS_COMPRESSED(*pte_p, pte_p)) {
		WRITE_PTE_STRONG(pte_p, pte);
		PMAP_UPDATE_TLBS(pmap, v, v + PAGE_SIZE, false);
	} else {
		WRITE_PTE(pte_p, pte);
		__builtin_arm_isb(ISB_SY);
	}

	PMAP_TRACE(3, PMAP_CODE(PMAP__TTE), VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE(v), VM_KERNEL_ADDRHIDE(v + PAGE_SIZE), pte);
}

MARK_AS_PMAP_TEXT static pt_entry_t
wimg_to_pte(unsigned int wimg)
{
	pt_entry_t pte;

	switch (wimg & (VM_WIMG_MASK)) {
	case VM_WIMG_IO:
	case VM_WIMG_RT:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DISABLE);
		pte |= ARM_PTE_NX | ARM_PTE_PNX;
		break;
	case VM_WIMG_POSTED:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_POSTED);
		pte |= ARM_PTE_NX | ARM_PTE_PNX;
		break;
	case VM_WIMG_POSTED_REORDERED:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_POSTED_REORDERED);
		pte |= ARM_PTE_NX | ARM_PTE_PNX;
		break;
	case VM_WIMG_POSTED_COMBINED_REORDERED:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_POSTED_COMBINED_REORDERED);
		pte |= ARM_PTE_NX | ARM_PTE_PNX;
		break;
	case VM_WIMG_WCOMB:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITECOMB);
		pte |= ARM_PTE_NX | ARM_PTE_PNX;
		break;
	case VM_WIMG_WTHRU:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITETHRU);
#if     (__ARM_VMSA__ > 7)
		pte |= ARM_PTE_SH(SH_OUTER_MEMORY);
#else
		pte |= ARM_PTE_SH;
#endif
		break;
	case VM_WIMG_COPYBACK:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);
#if     (__ARM_VMSA__ > 7)
		pte |= ARM_PTE_SH(SH_OUTER_MEMORY);
#else
		pte |= ARM_PTE_SH;
#endif
		break;
	case VM_WIMG_INNERWBACK:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_INNERWRITEBACK);
#if     (__ARM_VMSA__ > 7)
		pte |= ARM_PTE_SH(SH_INNER_MEMORY);
#else
		pte |= ARM_PTE_SH;
#endif
		break;
	default:
		pte = ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DEFAULT);
#if     (__ARM_VMSA__ > 7)
		pte |= ARM_PTE_SH(SH_OUTER_MEMORY);
#else
		pte |= ARM_PTE_SH;
#endif
	}

	return pte;
}

static boolean_t
pmap_enter_pv(
	pmap_t pmap,
	pt_entry_t *pte_p,
	int pai,
	unsigned int options,
	pv_entry_t **pve_p,
	boolean_t *is_altacct)
{
	pv_entry_t    **pv_h;
	pv_h = pai_to_pvh(pai);
	boolean_t first_cpu_mapping;

	ASSERT_PVH_LOCKED(pai);

	vm_offset_t pvh_flags = pvh_get_flags(pv_h);

#if XNU_MONITOR
	if (pvh_flags & PVH_FLAG_LOCKDOWN) {
		panic("%d is locked down (%#lx), cannot enter", pai, pvh_flags);
	}
#endif

#ifdef PVH_FLAG_CPU
	/* An IOMMU mapping may already be present for a page that hasn't yet
	 * had a CPU mapping established, so we use PVH_FLAG_CPU to determine
	 * if this is the first CPU mapping.  We base internal/reusable
	 * accounting on the options specified for the first CPU mapping.
	 * PVH_FLAG_CPU, and thus this accounting, will then persist as long
	 * as there are *any* mappings of the page.  The accounting for a
	 * page should not need to change until the page is recycled by the
	 * VM layer, and we assert that there are no mappings when a page
	 * is recycled.   An IOMMU mapping of a freed/recycled page is
	 * considered a security violation & potential DMA corruption path.*/
	first_cpu_mapping = ((pmap != NULL) && !(pvh_flags & PVH_FLAG_CPU));
	if (first_cpu_mapping) {
		pvh_flags |= PVH_FLAG_CPU;
	}
#else
	first_cpu_mapping = pvh_test_type(pv_h, PVH_TYPE_NULL);
#endif

	if (first_cpu_mapping) {
		if (options & PMAP_OPTIONS_INTERNAL) {
			SET_INTERNAL_PAGE(pai);
		} else {
			CLR_INTERNAL_PAGE(pai);
		}
		if ((options & PMAP_OPTIONS_INTERNAL) &&
		    (options & PMAP_OPTIONS_REUSABLE)) {
			SET_REUSABLE_PAGE(pai);
		} else {
			CLR_REUSABLE_PAGE(pai);
		}
	}
	if (pvh_test_type(pv_h, PVH_TYPE_NULL)) {
		pvh_update_head(pv_h, pte_p, PVH_TYPE_PTEP);
		if (pmap != NULL && pmap != kernel_pmap &&
		    ((options & PMAP_OPTIONS_ALT_ACCT) ||
		    PMAP_FOOTPRINT_SUSPENDED(pmap)) &&
		    IS_INTERNAL_PAGE(pai)) {
			/*
			 * Make a note to ourselves that this mapping is using alternative
			 * accounting. We'll need this in order to know which ledger to
			 * debit when the mapping is removed.
			 *
			 * The altacct bit must be set while the pv head is locked. Defer
			 * the ledger accounting until after we've dropped the lock.
			 */
			SET_ALTACCT_PAGE(pai, PV_ENTRY_NULL);
			*is_altacct = TRUE;
		} else {
			CLR_ALTACCT_PAGE(pai, PV_ENTRY_NULL);
		}
	} else {
		if (pvh_test_type(pv_h, PVH_TYPE_PTEP)) {
			pt_entry_t      *pte1_p;

			/*
			 * convert pvh list from PVH_TYPE_PTEP to PVH_TYPE_PVEP
			 */
			pte1_p = pvh_ptep(pv_h);
			pvh_set_flags(pv_h, pvh_flags);
			if ((*pve_p == PV_ENTRY_NULL) && (!pv_alloc(pmap, pai, pve_p))) {
				return FALSE;
			}

			pve_set_ptep(*pve_p, pte1_p);
			(*pve_p)->pve_next = PV_ENTRY_NULL;

			if (IS_ALTACCT_PAGE(pai, PV_ENTRY_NULL)) {
				/*
				 * transfer "altacct" from
				 * pp_attr to this pve
				 */
				CLR_ALTACCT_PAGE(pai, PV_ENTRY_NULL);
				SET_ALTACCT_PAGE(pai, *pve_p);
			}
			pvh_update_head(pv_h, *pve_p, PVH_TYPE_PVEP);
			*pve_p = PV_ENTRY_NULL;
		} else if (!pvh_test_type(pv_h, PVH_TYPE_PVEP)) {
			panic("%s: unexpected PV head %p, pte_p=%p pmap=%p pv_h=%p",
			    __func__, *pv_h, pte_p, pmap, pv_h);
		}
		/*
		 * Set up pv_entry for this new mapping and then
		 * add it to the list for this physical page.
		 */
		pvh_set_flags(pv_h, pvh_flags);
		if ((*pve_p == PV_ENTRY_NULL) && (!pv_alloc(pmap, pai, pve_p))) {
			return FALSE;
		}

		pve_set_ptep(*pve_p, pte_p);
		(*pve_p)->pve_next = PV_ENTRY_NULL;

		pvh_add(pv_h, *pve_p);

		if (pmap != NULL && pmap != kernel_pmap &&
		    ((options & PMAP_OPTIONS_ALT_ACCT) ||
		    PMAP_FOOTPRINT_SUSPENDED(pmap)) &&
		    IS_INTERNAL_PAGE(pai)) {
			/*
			 * Make a note to ourselves that this
			 * mapping is using alternative
			 * accounting. We'll need this in order
			 * to know which ledger to debit when
			 * the mapping is removed.
			 *
			 * The altacct bit must be set while
			 * the pv head is locked. Defer the
			 * ledger accounting until after we've
			 * dropped the lock.
			 */
			SET_ALTACCT_PAGE(pai, *pve_p);
			*is_altacct = TRUE;
		}

		*pve_p = PV_ENTRY_NULL;
	}

	pvh_set_flags(pv_h, pvh_flags);

	return TRUE;
}

MARK_AS_PMAP_TEXT static kern_return_t
pmap_enter_options_internal(
	pmap_t pmap,
	vm_map_address_t v,
	ppnum_t pn,
	vm_prot_t prot,
	vm_prot_t fault_type,
	unsigned int flags,
	boolean_t wired,
	unsigned int options)
{
	pmap_paddr_t    pa = ptoa(pn);
	pt_entry_t      pte;
	pt_entry_t      spte;
	pt_entry_t      *pte_p;
	pv_entry_t      *pve_p;
	boolean_t       set_NX;
	boolean_t       set_XO = FALSE;
	boolean_t       refcnt_updated;
	boolean_t       wiredcnt_updated;
	unsigned int    wimg_bits;
	boolean_t       was_compressed, was_alt_compressed;
	kern_return_t   kr = KERN_SUCCESS;

	VALIDATE_PMAP(pmap);

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	if ((v) & PAGE_MASK) {
		panic("pmap_enter_options() pmap %p v 0x%llx\n",
		    pmap, (uint64_t)v);
	}

	if ((prot & VM_PROT_EXECUTE) && (prot & VM_PROT_WRITE) && (pmap == kernel_pmap)) {
		panic("pmap_enter_options(): WX request on kernel_pmap");
	}

#if DEVELOPMENT || DEBUG
	if ((prot & VM_PROT_EXECUTE) || !nx_enabled || !pmap->nx_enabled)
#else
	if ((prot & VM_PROT_EXECUTE))
#endif
	{ set_NX = FALSE;} else {
		set_NX = TRUE;
	}

#if (__ARM_VMSA__ > 7)
	if (prot == VM_PROT_EXECUTE) {
		set_XO = TRUE;
	}
#endif

	assert(pn != vm_page_fictitious_addr);

	refcnt_updated = FALSE;
	wiredcnt_updated = FALSE;
	pve_p = PV_ENTRY_NULL;
	was_compressed = FALSE;
	was_alt_compressed = FALSE;

	PMAP_LOCK(pmap);

	/*
	 *	Expand pmap to include this pte.  Assume that
	 *	pmap is always expanded to include enough hardware
	 *	pages to map one VM page.
	 */
	while ((pte_p = pmap_pte(pmap, v)) == PT_ENTRY_NULL) {
		/* Must unlock to expand the pmap. */
		PMAP_UNLOCK(pmap);

		kr = pmap_expand(pmap, v, options, PMAP_TT_MAX_LEVEL);

		if (kr != KERN_SUCCESS) {
			return kr;
		}

		PMAP_LOCK(pmap);
	}

	if (options & PMAP_OPTIONS_NOENTER) {
		PMAP_UNLOCK(pmap);
		return KERN_SUCCESS;
	}

Pmap_enter_retry:

	spte = *pte_p;

	if (ARM_PTE_IS_COMPRESSED(spte, pte_p)) {
		/*
		 * "pmap" should be locked at this point, so this should
		 * not race with another pmap_enter() or pmap_remove_range().
		 */
		assert(pmap != kernel_pmap);

		/* one less "compressed" */
		OSAddAtomic64(-1, &pmap->stats.compressed);
		pmap_ledger_debit(pmap, task_ledgers.internal_compressed,
		    PAGE_SIZE);

		was_compressed = TRUE;
		if (spte & ARM_PTE_COMPRESSED_ALT) {
			was_alt_compressed = TRUE;
			pmap_ledger_debit(pmap, task_ledgers.alternate_accounting_compressed, PAGE_SIZE);
		} else {
			/* was part of the footprint */
			pmap_ledger_debit(pmap, task_ledgers.phys_footprint, PAGE_SIZE);
		}

		/* clear "compressed" marker */
		/* XXX is it necessary since we're about to overwrite it ? */
		WRITE_PTE_FAST(pte_p, ARM_PTE_TYPE_FAULT);
		spte = ARM_PTE_TYPE_FAULT;

		/*
		 * We're replacing a "compressed" marker with a valid PTE,
		 * so no change for "refcnt".
		 */
		refcnt_updated = TRUE;
	}

	if ((spte != ARM_PTE_TYPE_FAULT) && (pte_to_pa(spte) != pa)) {
		pmap_remove_range(pmap, v, pte_p, pte_p + 1, 0);
	}

	pte = pa_to_pte(pa) | ARM_PTE_TYPE;

	/* Don't bother tracking wiring for kernel PTEs.  We use ARM_PTE_WIRED to track
	 * wired memory statistics for user pmaps, but kernel PTEs are assumed
	 * to be wired in nearly all cases.  For VM layer functionality, the wired
	 * count in vm_page_t is sufficient. */
	if (wired && pmap != kernel_pmap) {
		pte |= ARM_PTE_WIRED;
	}

	if (set_NX) {
		pte |= pt_attr_leaf_xn(pt_attr);
	} else {
#if     (__ARM_VMSA__ > 7)
		if (pmap == kernel_pmap) {
			pte |= ARM_PTE_NX;
		} else {
			pte |= pt_attr_leaf_x(pt_attr);
		}
#endif
	}

	if (pmap == kernel_pmap) {
#if __ARM_KERNEL_PROTECT__
		pte |= ARM_PTE_NG;
#endif /* __ARM_KERNEL_PROTECT__ */
		if (prot & VM_PROT_WRITE) {
			pte |= ARM_PTE_AP(AP_RWNA);
			pa_set_bits(pa, PP_ATTR_MODIFIED | PP_ATTR_REFERENCED);
		} else {
			pte |= ARM_PTE_AP(AP_RONA);
			pa_set_bits(pa, PP_ATTR_REFERENCED);
		}
#if     (__ARM_VMSA__ == 7)
		if ((_COMM_PAGE_BASE_ADDRESS <= v) && (v < _COMM_PAGE_BASE_ADDRESS + _COMM_PAGE_AREA_LENGTH)) {
			pte = (pte & ~(ARM_PTE_APMASK)) | ARM_PTE_AP(AP_RORO);
		}
#endif
	} else {
		if (!pmap->nested) {
			pte |= ARM_PTE_NG;
		} else if ((pmap->nested_region_asid_bitmap)
		    && (v >= pmap->nested_region_subord_addr)
		    && (v < (pmap->nested_region_subord_addr + pmap->nested_region_size))) {
			unsigned int index = (unsigned int)((v - pmap->nested_region_subord_addr)  >> pt_attr_twig_shift(pt_attr));

			if ((pmap->nested_region_asid_bitmap)
			    && testbit(index, (int *)pmap->nested_region_asid_bitmap)) {
				pte |= ARM_PTE_NG;
			}
		}
#if MACH_ASSERT
		if (pmap->nested_pmap != NULL) {
			vm_map_address_t nest_vaddr;
			pt_entry_t              *nest_pte_p;

			nest_vaddr = v - pmap->nested_region_grand_addr + pmap->nested_region_subord_addr;

			if ((nest_vaddr >= pmap->nested_region_subord_addr)
			    && (nest_vaddr < (pmap->nested_region_subord_addr + pmap->nested_region_size))
			    && ((nest_pte_p = pmap_pte(pmap->nested_pmap, nest_vaddr)) != PT_ENTRY_NULL)
			    && (*nest_pte_p != ARM_PTE_TYPE_FAULT)
			    && (!ARM_PTE_IS_COMPRESSED(*nest_pte_p, nest_pte_p))
			    && (((*nest_pte_p) & ARM_PTE_NG) != ARM_PTE_NG)) {
				unsigned int index = (unsigned int)((v - pmap->nested_region_subord_addr)  >> pt_attr_twig_shift(pt_attr));

				if ((pmap->nested_pmap->nested_region_asid_bitmap)
				    && !testbit(index, (int *)pmap->nested_pmap->nested_region_asid_bitmap)) {
					panic("pmap_enter(): Global attribute conflict nest_pte_p=%p pmap=%p v=0x%llx spte=0x%llx \n",
					    nest_pte_p, pmap, (uint64_t)v, (uint64_t)*nest_pte_p);
				}
			}
		}
#endif
		if (prot & VM_PROT_WRITE) {
			if (pa_valid(pa) && (!pa_test_bits(pa, PP_ATTR_MODIFIED))) {
				if (fault_type & VM_PROT_WRITE) {
					if (set_XO) {
						pte |= pt_attr_leaf_rwna(pt_attr);
					} else {
						pte |= pt_attr_leaf_rw(pt_attr);
					}
					pa_set_bits(pa, PP_ATTR_REFERENCED | PP_ATTR_MODIFIED);
				} else {
					if (set_XO) {
						pte |= pt_attr_leaf_rona(pt_attr);
					} else {
						pte |= pt_attr_leaf_ro(pt_attr);
					}
					pa_set_bits(pa, PP_ATTR_REFERENCED);
					pte_set_was_writeable(pte, true);
				}
			} else {
				if (set_XO) {
					pte |= pt_attr_leaf_rwna(pt_attr);
				} else {
					pte |= pt_attr_leaf_rw(pt_attr);
				}
				pa_set_bits(pa, PP_ATTR_REFERENCED);
			}
		} else {
			if (set_XO) {
				pte |= pt_attr_leaf_rona(pt_attr);
			} else {
				pte |= pt_attr_leaf_ro(pt_attr);;
			}
			pa_set_bits(pa, PP_ATTR_REFERENCED);
		}
	}

	pte |= ARM_PTE_AF;

	volatile uint16_t *refcnt = NULL;
	volatile uint16_t *wiredcnt = NULL;
	if (pmap != kernel_pmap) {
		refcnt = &(ptep_get_ptd(pte_p)->ptd_info[ARM_PT_DESC_INDEX(pte_p)].refcnt);
		wiredcnt = &(ptep_get_ptd(pte_p)->ptd_info[ARM_PT_DESC_INDEX(pte_p)].wiredcnt);
		/* Bump the wired count to keep the PTE page from being reclaimed.  We need this because
		 * we may drop the PVH and pmap locks later in pmap_enter() if we need to allocate
		 * a new PV entry. */
		if (!wiredcnt_updated) {
			OSAddAtomic16(1, (volatile int16_t*)wiredcnt);
			wiredcnt_updated = TRUE;
		}
		if (!refcnt_updated) {
			OSAddAtomic16(1, (volatile int16_t*)refcnt);
			refcnt_updated = TRUE;
		}
	}

	if (pa_valid(pa)) {
		int         pai;
		boolean_t   is_altacct, is_internal;

		is_internal = FALSE;
		is_altacct = FALSE;

		pai = (int)pa_index(pa);

		LOCK_PVH(pai);

Pmap_enter_loop:
		if ((flags & (VM_WIMG_MASK | VM_WIMG_USE_DEFAULT))) {
			wimg_bits = (flags & (VM_WIMG_MASK | VM_WIMG_USE_DEFAULT));
		} else {
			wimg_bits = pmap_cache_attributes(pn);
		}

		/* We may be retrying this operation after dropping the PVH lock.
		 * Cache attributes for the physical page may have changed while the lock
		 * was dropped, so clear any cache attributes we may have previously set
		 * in the PTE template. */
		pte &= ~(ARM_PTE_ATTRINDXMASK | ARM_PTE_SHMASK);
		pte |= pmap_get_pt_ops(pmap)->wimg_to_pte(wimg_bits);

#if XNU_MONITOR
		/* The regular old kernel is not allowed to remap PPL pages. */
		if (pa_test_monitor(pa)) {
			panic("%s: page belongs to PPL, "
			    "pmap=%p, v=0x%llx, pn=%u, prot=0x%x, fault_type=0x%x, flags=0x%x, wired=%u, options=0x%x",
			    __FUNCTION__,
			    pmap, v, pn, prot, fault_type, flags, wired, options);
		}

		if (pvh_get_flags(pai_to_pvh(pai)) & PVH_FLAG_LOCKDOWN) {
			panic("%s: page locked down, "
			    "pmap=%p, v=0x%llx, pn=%u, prot=0x%x, fault_type=0x%x, flags=0x%x, wired=%u, options=0x%x",
			    __FUNCTION__,
			    pmap, v, pn, prot, fault_type, flags, wired, options);
		}
#endif


		if (pte == *pte_p) {
			/*
			 * This pmap_enter operation has been completed by another thread
			 * undo refcnt on pt and return
			 */
			UNLOCK_PVH(pai);
			goto Pmap_enter_cleanup;
		} else if (pte_to_pa(*pte_p) == pa) {
			pmap_enter_pte(pmap, pte_p, pte, v);
			UNLOCK_PVH(pai);
			goto Pmap_enter_cleanup;
		} else if (*pte_p != ARM_PTE_TYPE_FAULT) {
			/*
			 * pte has been modified by another thread
			 * hold refcnt on pt and retry pmap_enter operation
			 */
			UNLOCK_PVH(pai);
			goto Pmap_enter_retry;
		}
		if (!pmap_enter_pv(pmap, pte_p, pai, options, &pve_p, &is_altacct)) {
			goto Pmap_enter_loop;
		}

		pmap_enter_pte(pmap, pte_p, pte, v);

		if (pmap != kernel_pmap) {
			if (IS_REUSABLE_PAGE(pai) &&
			    !is_altacct) {
				assert(IS_INTERNAL_PAGE(pai));
				OSAddAtomic(+1, &pmap->stats.reusable);
				PMAP_STATS_PEAK(pmap->stats.reusable);
			} else if (IS_INTERNAL_PAGE(pai)) {
				OSAddAtomic(+1, &pmap->stats.internal);
				PMAP_STATS_PEAK(pmap->stats.internal);
				is_internal = TRUE;
			} else {
				OSAddAtomic(+1, &pmap->stats.external);
				PMAP_STATS_PEAK(pmap->stats.external);
			}
		}

		UNLOCK_PVH(pai);

		if (pmap != kernel_pmap) {
			pmap_ledger_credit(pmap, task_ledgers.phys_mem, PAGE_SIZE);

			if (is_internal) {
				/*
				 * Make corresponding adjustments to
				 * phys_footprint statistics.
				 */
				pmap_ledger_credit(pmap, task_ledgers.internal, PAGE_SIZE);
				if (is_altacct) {
					/*
					 * If this page is internal and
					 * in an IOKit region, credit
					 * the task's total count of
					 * dirty, internal IOKit pages.
					 * It should *not* count towards
					 * the task's total physical
					 * memory footprint, because
					 * this entire region was
					 * already billed to the task
					 * at the time the mapping was
					 * created.
					 *
					 * Put another way, this is
					 * internal++ and
					 * alternate_accounting++, so
					 * net effect on phys_footprint
					 * is 0. That means: don't
					 * touch phys_footprint here.
					 */
					pmap_ledger_credit(pmap, task_ledgers.alternate_accounting, PAGE_SIZE);
				} else {
					pmap_ledger_credit(pmap, task_ledgers.phys_footprint, PAGE_SIZE);
				}
			}
		}

		OSAddAtomic(1, (SInt32 *) &pmap->stats.resident_count);
		if (pmap->stats.resident_count > pmap->stats.resident_max) {
			pmap->stats.resident_max = pmap->stats.resident_count;
		}
	} else {
		if (prot & VM_PROT_EXECUTE) {
			kr = KERN_FAILURE;
			goto Pmap_enter_cleanup;
		}

		wimg_bits = pmap_cache_attributes(pn);
		if ((flags & (VM_WIMG_MASK | VM_WIMG_USE_DEFAULT))) {
			wimg_bits = (wimg_bits & (~VM_WIMG_MASK)) | (flags & (VM_WIMG_MASK | VM_WIMG_USE_DEFAULT));
		}

		pte |= pmap_get_pt_ops(pmap)->wimg_to_pte(wimg_bits);

#if XNU_MONITOR
		if (!pmap_ppl_disable && (wimg_bits & PP_ATTR_MONITOR)) {
			uint64_t xprr_perm = pte_to_xprr_perm(pte);
			pte &= ~ARM_PTE_XPRR_MASK;
			switch (xprr_perm) {
			case XPRR_KERN_RO_PERM:
				pte |= xprr_perm_to_pte(XPRR_PPL_RO_PERM);
				break;
			case XPRR_KERN_RW_PERM:
				pte |= xprr_perm_to_pte(XPRR_PPL_RW_PERM);
				break;
			default:
				panic("Unsupported xPRR perm %llu for pte 0x%llx", xprr_perm, (uint64_t)pte);
			}
		}
#endif
		pmap_enter_pte(pmap, pte_p, pte, v);
	}

	goto Pmap_enter_return;

Pmap_enter_cleanup:

	if (refcnt != NULL) {
		assert(refcnt_updated);
		if (OSAddAtomic16(-1, (volatile int16_t*)refcnt) <= 0) {
			panic("pmap_enter(): over-release of ptdp %p for pte %p\n", ptep_get_ptd(pte_p), pte_p);
		}
	}

Pmap_enter_return:

#if CONFIG_PGTRACE
	if (pgtrace_enabled) {
		// Clone and invalidate original mapping if eligible
		for (int i = 0; i < PAGE_RATIO; i++) {
			pmap_pgtrace_enter_clone(pmap, v + ARM_PGBYTES * i, 0, 0);
		}
	}
#endif

	if (pve_p != PV_ENTRY_NULL) {
		pv_free(pve_p);
	}

	if (wiredcnt_updated && (OSAddAtomic16(-1, (volatile int16_t*)wiredcnt) <= 0)) {
		panic("pmap_enter(): over-unwire of ptdp %p for pte %p\n", ptep_get_ptd(pte_p), pte_p);
	}

	PMAP_UNLOCK(pmap);

	return kr;
}

kern_return_t
pmap_enter_options(
	pmap_t pmap,
	vm_map_address_t v,
	ppnum_t pn,
	vm_prot_t prot,
	vm_prot_t fault_type,
	unsigned int flags,
	boolean_t wired,
	unsigned int options,
	__unused void   *arg)
{
	kern_return_t kr = KERN_FAILURE;

	PMAP_TRACE(2, PMAP_CODE(PMAP__ENTER) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE(v), pn, prot);

#if XNU_MONITOR
	if (options & PMAP_OPTIONS_NOWAIT) {
		/* If NOWAIT was requested, just return the result. */
		kr = pmap_enter_options_ppl(pmap, v, pn, prot, fault_type, flags, wired, options);
	} else {
		/*
		 * If NOWAIT was not requested, loop until the enter does not
		 * fail due to lack of resources.
		 */
		while ((kr = pmap_enter_options_ppl(pmap, v, pn, prot, fault_type, flags, wired, options | PMAP_OPTIONS_NOWAIT)) == KERN_RESOURCE_SHORTAGE) {
			pv_water_mark_check();
			pmap_alloc_page_for_ppl();
		}
	}

	pmap_ledger_check_balance(pmap);
#else
	kr = pmap_enter_options_internal(pmap, v, pn, prot, fault_type, flags, wired, options);
#endif
	pv_water_mark_check();

	PMAP_TRACE(2, PMAP_CODE(PMAP__ENTER) | DBG_FUNC_END, kr);

	return kr;
}

/*
 *	Routine:	pmap_change_wiring
 *	Function:	Change the wiring attribute for a map/virtual-address
 *			pair.
 *	In/out conditions:
 *			The mapping must already exist in the pmap.
 */
MARK_AS_PMAP_TEXT static void
pmap_change_wiring_internal(
	pmap_t pmap,
	vm_map_address_t v,
	boolean_t wired)
{
	pt_entry_t     *pte_p;
	pmap_paddr_t    pa;

	/* Don't bother tracking wiring for kernel PTEs.  We use ARM_PTE_WIRED to track
	 * wired memory statistics for user pmaps, but kernel PTEs are assumed
	 * to be wired in nearly all cases.  For VM layer functionality, the wired
	 * count in vm_page_t is sufficient. */
	if (pmap == kernel_pmap) {
		return;
	}
	VALIDATE_USER_PMAP(pmap);

	PMAP_LOCK(pmap);
	pte_p = pmap_pte(pmap, v);
	assert(pte_p != PT_ENTRY_NULL);
	pa = pte_to_pa(*pte_p);

	while (pa_valid(pa)) {
		pmap_paddr_t new_pa;

		LOCK_PVH((int)pa_index(pa));
		new_pa = pte_to_pa(*pte_p);

		if (pa == new_pa) {
			break;
		}

		UNLOCK_PVH((int)pa_index(pa));
		pa = new_pa;
	}

	if (wired && !pte_is_wired(*pte_p)) {
		pte_set_wired(pte_p, wired);
		OSAddAtomic(+1, (SInt32 *) &pmap->stats.wired_count);
		pmap_ledger_credit(pmap, task_ledgers.wired_mem, PAGE_SIZE);
	} else if (!wired && pte_is_wired(*pte_p)) {
		PMAP_STATS_ASSERTF(pmap->stats.wired_count >= 1, pmap, "stats.wired_count %d", pmap->stats.wired_count);
		pte_set_wired(pte_p, wired);
		OSAddAtomic(-1, (SInt32 *) &pmap->stats.wired_count);
		pmap_ledger_debit(pmap, task_ledgers.wired_mem, PAGE_SIZE);
	}

	if (pa_valid(pa)) {
		UNLOCK_PVH((int)pa_index(pa));
	}

	PMAP_UNLOCK(pmap);
}

void
pmap_change_wiring(
	pmap_t pmap,
	vm_map_address_t v,
	boolean_t wired)
{
#if XNU_MONITOR
	pmap_change_wiring_ppl(pmap, v, wired);

	pmap_ledger_check_balance(pmap);
#else
	pmap_change_wiring_internal(pmap, v, wired);
#endif
}

MARK_AS_PMAP_TEXT static ppnum_t
pmap_find_phys_internal(
	pmap_t pmap,
	addr64_t va)
{
	ppnum_t         ppn = 0;

	VALIDATE_PMAP(pmap);

	if (pmap != kernel_pmap) {
		PMAP_LOCK(pmap);
	}

	ppn = pmap_vtophys(pmap, va);

	if (pmap != kernel_pmap) {
		PMAP_UNLOCK(pmap);
	}

	return ppn;
}

ppnum_t
pmap_find_phys(
	pmap_t pmap,
	addr64_t va)
{
	pmap_paddr_t    pa = 0;

	if (pmap == kernel_pmap) {
		pa = mmu_kvtop(va);
	} else if ((current_thread()->map) && (pmap == vm_map_pmap(current_thread()->map))) {
		pa = mmu_uvtop(va);
	}

	if (pa) {
		return (ppnum_t)(pa >> PAGE_SHIFT);
	}

	if (not_in_kdp) {
#if XNU_MONITOR
		return pmap_find_phys_ppl(pmap, va);
#else
		return pmap_find_phys_internal(pmap, va);
#endif
	} else {
		return pmap_vtophys(pmap, va);
	}
}

pmap_paddr_t
kvtophys(
	vm_offset_t va)
{
	pmap_paddr_t pa;

	pa = mmu_kvtop(va);
	if (pa) {
		return pa;
	}
	pa = ((pmap_paddr_t)pmap_vtophys(kernel_pmap, va)) << PAGE_SHIFT;
	if (pa) {
		pa |= (va & PAGE_MASK);
	}

	return (pmap_paddr_t)pa;
}

ppnum_t
pmap_vtophys(
	pmap_t pmap,
	addr64_t va)
{
	if ((va < pmap->min) || (va >= pmap->max)) {
		return 0;
	}

#if     (__ARM_VMSA__ == 7)
	tt_entry_t     *tte_p, tte;
	pt_entry_t     *pte_p;
	ppnum_t         ppn;

	tte_p = pmap_tte(pmap, va);
	if (tte_p == (tt_entry_t *) NULL) {
		return (ppnum_t) 0;
	}

	tte = *tte_p;
	if ((tte & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
		pte_p = (pt_entry_t *) ttetokv(tte) + ptenum(va);
		ppn = (ppnum_t) atop(pte_to_pa(*pte_p) | (va & ARM_PGMASK));
#if DEVELOPMENT || DEBUG
		if (ppn != 0 &&
		    ARM_PTE_IS_COMPRESSED(*pte_p, pte_p)) {
			panic("pmap_vtophys(%p,0x%llx): compressed pte_p=%p 0x%llx with ppn=0x%x\n",
			    pmap, va, pte_p, (uint64_t) (*pte_p), ppn);
		}
#endif /* DEVELOPMENT || DEBUG */
	} else if ((tte & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_BLOCK) {
		if ((tte & ARM_TTE_BLOCK_SUPER) == ARM_TTE_BLOCK_SUPER) {
			ppn = (ppnum_t) atop(suptte_to_pa(tte) | (va & ARM_TT_L1_SUPER_OFFMASK));
		} else {
			ppn = (ppnum_t) atop(sectte_to_pa(tte) | (va & ARM_TT_L1_BLOCK_OFFMASK));
		}
	} else {
		ppn = 0;
	}
#else
	tt_entry_t              *ttp;
	tt_entry_t              tte;
	ppnum_t                 ppn = 0;

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	/* Level 0 currently unused */

	/* Get first-level (1GB) entry */
	ttp = pmap_tt1e(pmap, va);
	tte = *ttp;
	if ((tte & (ARM_TTE_TYPE_MASK | ARM_TTE_VALID)) != (ARM_TTE_TYPE_TABLE | ARM_TTE_VALID)) {
		return ppn;
	}

	tte = ((tt_entry_t*) phystokv(tte & ARM_TTE_TABLE_MASK))[tt2_index(pmap, pt_attr, va)];

	if ((tte & ARM_TTE_VALID) != (ARM_TTE_VALID)) {
		return ppn;
	}

	if ((tte & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_BLOCK) {
		ppn = (ppnum_t) atop((tte & ARM_TTE_BLOCK_L2_MASK) | (va & ARM_TT_L2_OFFMASK));
		return ppn;
	}
	tte = ((tt_entry_t*) phystokv(tte & ARM_TTE_TABLE_MASK))[tt3_index(pmap, pt_attr, va)];
	ppn = (ppnum_t) atop((tte & ARM_PTE_MASK) | (va & ARM_TT_L3_OFFMASK));
#endif

	return ppn;
}

MARK_AS_PMAP_TEXT static vm_offset_t
pmap_extract_internal(
	pmap_t pmap,
	vm_map_address_t va)
{
	pmap_paddr_t    pa = 0;
	ppnum_t         ppn = 0;

	if (pmap == NULL) {
		return 0;
	}

	VALIDATE_PMAP(pmap);

	PMAP_LOCK(pmap);

	ppn = pmap_vtophys(pmap, va);

	if (ppn != 0) {
		pa = ptoa(ppn) | ((va) & PAGE_MASK);
	}

	PMAP_UNLOCK(pmap);

	return pa;
}

/*
 *	Routine:	pmap_extract
 *	Function:
 *		Extract the physical page address associated
 *		with the given map/virtual_address pair.
 *
 */
vm_offset_t
pmap_extract(
	pmap_t pmap,
	vm_map_address_t va)
{
	pmap_paddr_t    pa = 0;

	if (pmap == kernel_pmap) {
		pa = mmu_kvtop(va);
	} else if (pmap == vm_map_pmap(current_thread()->map)) {
		pa = mmu_uvtop(va);
	}

	if (pa) {
		return pa;
	}

#if XNU_MONITOR
	return pmap_extract_ppl(pmap, va);
#else
	return pmap_extract_internal(pmap, va);
#endif
}

/*
 *	pmap_init_pte_page - Initialize a page table page.
 */
void
pmap_init_pte_page(
	pmap_t pmap,
	pt_entry_t *pte_p,
	vm_offset_t va,
	unsigned int ttlevel,
	boolean_t alloc_ptd,
	boolean_t clear)
{
	pt_desc_t   *ptdp = NULL;
	vm_offset_t *pvh;

	pvh = (vm_offset_t *)(pai_to_pvh(pa_index(ml_static_vtop((vm_offset_t)pte_p))));

	if (pvh_test_type(pvh, PVH_TYPE_NULL)) {
		if (alloc_ptd) {
			/*
			 * This path should only be invoked from arm_vm_init.  If we are emulating 16KB pages
			 * on 4KB hardware, we may already have allocated a page table descriptor for a
			 * bootstrap request, so we check for an existing PTD here.
			 */
			ptdp = ptd_alloc(pmap, true);
			pvh_update_head_unlocked(pvh, ptdp, PVH_TYPE_PTDP);
		} else {
			panic("pmap_init_pte_page(): pte_p %p", pte_p);
		}
	} else if (pvh_test_type(pvh, PVH_TYPE_PTDP)) {
		ptdp = (pt_desc_t*)(pvh_list(pvh));
	} else {
		panic("pmap_init_pte_page(): invalid PVH type for pte_p %p", pte_p);
	}

	if (clear) {
		bzero(pte_p, ARM_PGBYTES);
		// below barrier ensures the page zeroing is visible to PTW before
		// it is linked to the PTE of previous level
		__builtin_arm_dmb(DMB_ISHST);
	}
	ptd_init(ptdp, pmap, va, ttlevel, pte_p);
}

/*
 *	Routine:	pmap_expand
 *
 *	Expands a pmap to be able to map the specified virtual address.
 *
 *	Allocates new memory for the default (COARSE) translation table
 *	entry, initializes all the pte entries to ARM_PTE_TYPE_FAULT and
 *	also allocates space for the corresponding pv entries.
 *
 *	Nothing should be locked.
 */
static kern_return_t
pmap_expand(
	pmap_t pmap,
	vm_map_address_t v,
	unsigned int options,
	unsigned int level)
{
	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

#if     (__ARM_VMSA__ == 7)
	vm_offset_t     pa;
	tt_entry_t              *tte_p;
	tt_entry_t              *tt_p;
	unsigned int    i;

#if DEVELOPMENT || DEBUG
	/*
	 * We no longer support root level expansion; panic in case something
	 * still attempts to trigger it.
	 */
	i = tte_index(pmap, pt_attr, v);

	if (i >= pmap->tte_index_max) {
		panic("%s: index out of range, index=%u, max=%u, "
		    "pmap=%p, addr=%p, options=%u, level=%u",
		    __func__, i, pmap->tte_index_max,
		    pmap, (void *)v, options, level);
	}
#endif /* DEVELOPMENT || DEBUG */

	if (level == 1) {
		return KERN_SUCCESS;
	}

	{
		tt_entry_t     *tte_next_p;

		PMAP_LOCK(pmap);
		pa = 0;
		if (pmap_pte(pmap, v) != PT_ENTRY_NULL) {
			PMAP_UNLOCK(pmap);
			return KERN_SUCCESS;
		}
		tte_p = &pmap->tte[ttenum(v & ~ARM_TT_L1_PT_OFFMASK)];
		for (i = 0, tte_next_p = tte_p; i < 4; i++) {
			if (tte_to_pa(*tte_next_p)) {
				pa = tte_to_pa(*tte_next_p);
				break;
			}
			tte_next_p++;
		}
		pa = pa & ~PAGE_MASK;
		if (pa) {
			tte_p =  &pmap->tte[ttenum(v)];
			*tte_p =  pa_to_tte(pa) | (((v >> ARM_TT_L1_SHIFT) & 0x3) << 10) | ARM_TTE_TYPE_TABLE;
			FLUSH_PTE(tte_p);
			PMAP_TRACE(3, PMAP_CODE(PMAP__TTE), VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE(v & ~ARM_TT_L1_OFFMASK),
			    VM_KERNEL_ADDRHIDE((v & ~ARM_TT_L1_OFFMASK) + ARM_TT_L1_SIZE), *tte_p);
			PMAP_UNLOCK(pmap);
			return KERN_SUCCESS;
		}
		PMAP_UNLOCK(pmap);
	}
	v = v & ~ARM_TT_L1_PT_OFFMASK;


	while (pmap_pte(pmap, v) == PT_ENTRY_NULL) {
		/*
		 *	Allocate a VM page for the level 2 page table entries.
		 */
		while (pmap_tt_allocate(pmap, &tt_p, PMAP_TT_L2_LEVEL, ((options & PMAP_TT_ALLOCATE_NOWAIT)? PMAP_PAGES_ALLOCATE_NOWAIT : 0)) != KERN_SUCCESS) {
			if (options & PMAP_OPTIONS_NOWAIT) {
				return KERN_RESOURCE_SHORTAGE;
			}
			VM_PAGE_WAIT();
		}

		PMAP_LOCK(pmap);
		/*
		 *	See if someone else expanded us first
		 */
		if (pmap_pte(pmap, v) == PT_ENTRY_NULL) {
			tt_entry_t     *tte_next_p;

			pmap_init_pte_page(pmap, (pt_entry_t *) tt_p, v, PMAP_TT_L2_LEVEL, FALSE, TRUE);
			pa = kvtophys((vm_offset_t)tt_p);
			tte_p = &pmap->tte[ttenum(v)];
			for (i = 0, tte_next_p = tte_p; i < 4; i++) {
				*tte_next_p = pa_to_tte(pa) | ARM_TTE_TYPE_TABLE;
				PMAP_TRACE(3, PMAP_CODE(PMAP__TTE), VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE((v & ~ARM_TT_L1_PT_OFFMASK) + (i * ARM_TT_L1_SIZE)),
				    VM_KERNEL_ADDRHIDE((v & ~ARM_TT_L1_PT_OFFMASK) + ((i + 1) * ARM_TT_L1_SIZE)), *tte_p);
				tte_next_p++;
				pa = pa + 0x400;
			}
			FLUSH_PTE_RANGE(tte_p, tte_p + 4);

			pa = 0x0ULL;
			tt_p = (tt_entry_t *)NULL;
		}
		PMAP_UNLOCK(pmap);
		if (tt_p != (tt_entry_t *)NULL) {
			pmap_tt_deallocate(pmap, tt_p, PMAP_TT_L2_LEVEL);
			tt_p = (tt_entry_t *)NULL;
		}
	}
	return KERN_SUCCESS;
#else
	pmap_paddr_t    pa;
	unsigned int    ttlevel = pt_attr_root_level(pt_attr);
	tt_entry_t              *tte_p;
	tt_entry_t              *tt_p;

	pa = 0x0ULL;
	tt_p =  (tt_entry_t *)NULL;

	for (; ttlevel < level; ttlevel++) {
		PMAP_LOCK(pmap);

		if (pmap_ttne(pmap, ttlevel + 1, v) == PT_ENTRY_NULL) {
			PMAP_UNLOCK(pmap);
			while (pmap_tt_allocate(pmap, &tt_p, ttlevel + 1, ((options & PMAP_TT_ALLOCATE_NOWAIT)? PMAP_PAGES_ALLOCATE_NOWAIT : 0)) != KERN_SUCCESS) {
				if (options & PMAP_OPTIONS_NOWAIT) {
					return KERN_RESOURCE_SHORTAGE;
				}
#if XNU_MONITOR
				panic("%s: failed to allocate tt, "
				    "pmap=%p, v=%p, options=0x%x, level=%u",
				    __FUNCTION__,
				    pmap, (void *)v, options, level);
#else
				VM_PAGE_WAIT();
#endif
			}
			PMAP_LOCK(pmap);
			if ((pmap_ttne(pmap, ttlevel + 1, v) == PT_ENTRY_NULL)) {
				pmap_init_pte_page(pmap, (pt_entry_t *) tt_p, v, ttlevel + 1, FALSE, TRUE);
				pa = kvtophys((vm_offset_t)tt_p);
				tte_p = pmap_ttne(pmap, ttlevel, v);
				*tte_p = (pa & ARM_TTE_TABLE_MASK) | ARM_TTE_TYPE_TABLE | ARM_TTE_VALID;
				PMAP_TRACE(ttlevel + 1, PMAP_CODE(PMAP__TTE), VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE(v & ~pt_attr_ln_offmask(pt_attr, ttlevel)),
				    VM_KERNEL_ADDRHIDE((v & ~pt_attr_ln_offmask(pt_attr, ttlevel)) + pt_attr_ln_size(pt_attr, ttlevel)), *tte_p);
				pa = 0x0ULL;
				tt_p = (tt_entry_t *)NULL;
			}
		}

		PMAP_UNLOCK(pmap);

		if (tt_p != (tt_entry_t *)NULL) {
			pmap_tt_deallocate(pmap, tt_p, ttlevel + 1);
			tt_p = (tt_entry_t *)NULL;
		}
	}

	return KERN_SUCCESS;
#endif
}

/*
 *	Routine:	pmap_collect
 *	Function:
 *		Garbage collects the physical map system for
 *		pages which are no longer used.
 *		Success need not be guaranteed -- that is, there
 *		may well be pages which are not referenced, but
 *		others may be collected.
 */
void
pmap_collect(pmap_t pmap)
{
	if (pmap == PMAP_NULL) {
		return;
	}

#if 0
	PMAP_LOCK(pmap);
	if ((pmap->nested == FALSE) && (pmap != kernel_pmap)) {
		/* TODO: Scan for vm page assigned to top level page tables with no reference */
	}
	PMAP_UNLOCK(pmap);
#endif

	return;
}

/*
 *	Routine:	pmap_gc
 *	Function:
 *              Pmap garbage collection
 *		Called by the pageout daemon when pages are scarce.
 *
 */
void
pmap_gc(
	void)
{
#if XNU_MONITOR
	/*
	 * We cannot invoke the scheduler from the PPL, so for now we elide the
	 * GC logic if the PPL is enabled.
	 */
#endif
#if !XNU_MONITOR
	pmap_t  pmap, pmap_next;
	boolean_t       gc_wait;

	if (pmap_gc_allowed &&
	    (pmap_gc_allowed_by_time_throttle ||
	    pmap_gc_forced)) {
		pmap_gc_forced = FALSE;
		pmap_gc_allowed_by_time_throttle = FALSE;
		pmap_simple_lock(&pmaps_lock);
		pmap = CAST_DOWN_EXPLICIT(pmap_t, queue_first(&map_pmap_list));
		while (!queue_end(&map_pmap_list, (queue_entry_t)pmap)) {
			if (!(pmap->gc_status & PMAP_GC_INFLIGHT)) {
				pmap->gc_status |= PMAP_GC_INFLIGHT;
			}
			pmap_simple_unlock(&pmaps_lock);

			pmap_collect(pmap);

			pmap_simple_lock(&pmaps_lock);
			gc_wait = (pmap->gc_status & PMAP_GC_WAIT);
			pmap->gc_status &= ~(PMAP_GC_INFLIGHT | PMAP_GC_WAIT);
			pmap_next = CAST_DOWN_EXPLICIT(pmap_t, queue_next(&pmap->pmaps));
			if (gc_wait) {
				if (!queue_end(&map_pmap_list, (queue_entry_t)pmap_next)) {
					pmap_next->gc_status |= PMAP_GC_INFLIGHT;
				}
				pmap_simple_unlock(&pmaps_lock);
				thread_wakeup((event_t) &pmap->gc_status);
				pmap_simple_lock(&pmaps_lock);
			}
			pmap = pmap_next;
		}
		pmap_simple_unlock(&pmaps_lock);
	}
#endif
}

/*
 * Called by the VM to reclaim pages that we can reclaim quickly and cheaply.
 */
uint64_t
pmap_release_pages_fast(void)
{
#if XNU_MONITOR
	return pmap_release_ppl_pages_to_kernel();
#else /* XNU_MONITOR */
	return 0;
#endif
}

/*
 *      By default, don't attempt pmap GC more frequently
 *      than once / 1 minutes.
 */

void
compute_pmap_gc_throttle(
	void *arg __unused)
{
	pmap_gc_allowed_by_time_throttle = TRUE;
}

/*
 * pmap_attribute_cache_sync(vm_offset_t pa)
 *
 * Invalidates all of the instruction cache on a physical page and
 * pushes any dirty data from the data cache for the same physical page
 */

kern_return_t
pmap_attribute_cache_sync(
	ppnum_t pp,
	vm_size_t size,
	__unused vm_machine_attribute_t attribute,
	__unused vm_machine_attribute_val_t * value)
{
	if (size > PAGE_SIZE) {
		panic("pmap_attribute_cache_sync size: 0x%llx\n", (uint64_t)size);
	} else {
		cache_sync_page(pp);
	}

	return KERN_SUCCESS;
}

/*
 * pmap_sync_page_data_phys(ppnum_t pp)
 *
 * Invalidates all of the instruction cache on a physical page and
 * pushes any dirty data from the data cache for the same physical page
 */
void
pmap_sync_page_data_phys(
	ppnum_t pp)
{
	cache_sync_page(pp);
}

/*
 * pmap_sync_page_attributes_phys(ppnum_t pp)
 *
 * Write back and invalidate all cachelines on a physical page.
 */
void
pmap_sync_page_attributes_phys(
	ppnum_t pp)
{
	flush_dcache((vm_offset_t) (pp << PAGE_SHIFT), PAGE_SIZE, TRUE);
}

#if CONFIG_COREDUMP
/* temporary workaround */
boolean_t
coredumpok(
	vm_map_t map,
	vm_offset_t va)
{
	pt_entry_t     *pte_p;
	pt_entry_t      spte;

	pte_p = pmap_pte(map->pmap, va);
	if (0 == pte_p) {
		return FALSE;
	}
	spte = *pte_p;
	return (spte & ARM_PTE_ATTRINDXMASK) == ARM_PTE_ATTRINDX(CACHE_ATTRINDX_DEFAULT);
}
#endif

void
fillPage(
	ppnum_t pn,
	unsigned int fill)
{
	unsigned int   *addr;
	int             count;

	addr = (unsigned int *) phystokv(ptoa(pn));
	count = PAGE_SIZE / sizeof(unsigned int);
	while (count--) {
		*addr++ = fill;
	}
}

extern void     mapping_set_mod(ppnum_t pn);

void
mapping_set_mod(
	ppnum_t pn)
{
	pmap_set_modify(pn);
}

extern void     mapping_set_ref(ppnum_t pn);

void
mapping_set_ref(
	ppnum_t pn)
{
	pmap_set_reference(pn);
}

/*
 * Clear specified attribute bits.
 *
 * Try to force an arm_fast_fault() for all mappings of
 * the page - to force attributes to be set again at fault time.
 * If the forcing succeeds, clear the cached bits at the head.
 * Otherwise, something must have been wired, so leave the cached
 * attributes alone.
 */
MARK_AS_PMAP_TEXT static void
phys_attribute_clear_internal(
	ppnum_t         pn,
	unsigned int    bits,
	int             options,
	void            *arg)
{
	pmap_paddr_t    pa = ptoa(pn);
	vm_prot_t       allow_mode = VM_PROT_ALL;

#if XNU_MONITOR
	if (bits & PP_ATTR_PPL_OWNED_BITS) {
		panic("%s: illegal request, "
		    "pn=%u, bits=%#x, options=%#x, arg=%p",
		    __FUNCTION__,
		    pn, bits, options, arg);
	}
#endif

	if ((bits & PP_ATTR_MODIFIED) &&
	    (options & PMAP_OPTIONS_NOFLUSH) &&
	    (arg == NULL)) {
		panic("phys_attribute_clear(0x%x,0x%x,0x%x,%p): "
		    "should not clear 'modified' without flushing TLBs\n",
		    pn, bits, options, arg);
	}

	assert(pn != vm_page_fictitious_addr);

	if (options & PMAP_OPTIONS_CLEAR_WRITE) {
		assert(bits == PP_ATTR_MODIFIED);

		pmap_page_protect_options_internal(pn, (VM_PROT_ALL & ~VM_PROT_WRITE), 0);
		/*
		 * We short circuit this case; it should not need to
		 * invoke arm_force_fast_fault, so just clear the modified bit.
		 * pmap_page_protect has taken care of resetting
		 * the state so that we'll see the next write as a fault to
		 * the VM (i.e. we don't want a fast fault).
		 */
		pa_clear_bits(pa, bits);
		return;
	}
	if (bits & PP_ATTR_REFERENCED) {
		allow_mode &= ~(VM_PROT_READ | VM_PROT_EXECUTE);
	}
	if (bits & PP_ATTR_MODIFIED) {
		allow_mode &= ~VM_PROT_WRITE;
	}

	if (bits == PP_ATTR_NOENCRYPT) {
		/*
		 * We short circuit this case; it should not need to
		 * invoke arm_force_fast_fault, so just clear and
		 * return.  On ARM, this bit is just a debugging aid.
		 */
		pa_clear_bits(pa, bits);
		return;
	}

	if (arm_force_fast_fault_internal(pn, allow_mode, options)) {
		pa_clear_bits(pa, bits);
	}
	return;
}

static void
phys_attribute_clear(
	ppnum_t         pn,
	unsigned int    bits,
	int             options,
	void            *arg)
{
	/*
	 * Do we really want this tracepoint?  It will be extremely chatty.
	 * Also, should we have a corresponding trace point for the set path?
	 */
	PMAP_TRACE(3, PMAP_CODE(PMAP__ATTRIBUTE_CLEAR) | DBG_FUNC_START, pn, bits);

#if XNU_MONITOR
	phys_attribute_clear_ppl(pn, bits, options, arg);
#else
	phys_attribute_clear_internal(pn, bits, options, arg);
#endif

	PMAP_TRACE(3, PMAP_CODE(PMAP__ATTRIBUTE_CLEAR) | DBG_FUNC_END);
}

/*
 *	Set specified attribute bits.
 *
 *	Set cached value in the pv head because we have
 *	no per-mapping hardware support for referenced and
 *	modify bits.
 */
MARK_AS_PMAP_TEXT static void
phys_attribute_set_internal(
	ppnum_t pn,
	unsigned int bits)
{
	pmap_paddr_t    pa = ptoa(pn);
	assert(pn != vm_page_fictitious_addr);

#if XNU_MONITOR
	if (bits & PP_ATTR_PPL_OWNED_BITS) {
		panic("%s: illegal request, "
		    "pn=%u, bits=%#x",
		    __FUNCTION__,
		    pn, bits);
	}
#endif

	pa_set_bits(pa, bits);

	return;
}

static void
phys_attribute_set(
	ppnum_t pn,
	unsigned int bits)
{
#if XNU_MONITOR
	phys_attribute_set_ppl(pn, bits);
#else
	phys_attribute_set_internal(pn, bits);
#endif
}


/*
 *	Check specified attribute bits.
 *
 *	use the software cached bits (since no hw support).
 */
static boolean_t
phys_attribute_test(
	ppnum_t pn,
	unsigned int bits)
{
	pmap_paddr_t    pa = ptoa(pn);
	assert(pn != vm_page_fictitious_addr);
	return pa_test_bits(pa, bits);
}


/*
 *	Set the modify/reference bits on the specified physical page.
 */
void
pmap_set_modify(ppnum_t pn)
{
	phys_attribute_set(pn, PP_ATTR_MODIFIED);
}


/*
 *	Clear the modify bits on the specified physical page.
 */
void
pmap_clear_modify(
	ppnum_t pn)
{
	phys_attribute_clear(pn, PP_ATTR_MODIFIED, 0, NULL);
}


/*
 *	pmap_is_modified:
 *
 *	Return whether or not the specified physical page is modified
 *	by any physical maps.
 */
boolean_t
pmap_is_modified(
	ppnum_t pn)
{
	return phys_attribute_test(pn, PP_ATTR_MODIFIED);
}


/*
 *	Set the reference bit on the specified physical page.
 */
static void
pmap_set_reference(
	ppnum_t pn)
{
	phys_attribute_set(pn, PP_ATTR_REFERENCED);
}

/*
 *	Clear the reference bits on the specified physical page.
 */
void
pmap_clear_reference(
	ppnum_t pn)
{
	phys_attribute_clear(pn, PP_ATTR_REFERENCED, 0, NULL);
}


/*
 *	pmap_is_referenced:
 *
 *	Return whether or not the specified physical page is referenced
 *	by any physical maps.
 */
boolean_t
pmap_is_referenced(
	ppnum_t pn)
{
	return phys_attribute_test(pn, PP_ATTR_REFERENCED);
}

/*
 * pmap_get_refmod(phys)
 *  returns the referenced and modified bits of the specified
 *  physical page.
 */
unsigned int
pmap_get_refmod(
	ppnum_t pn)
{
	return ((phys_attribute_test(pn, PP_ATTR_MODIFIED)) ? VM_MEM_MODIFIED : 0)
	       | ((phys_attribute_test(pn, PP_ATTR_REFERENCED)) ? VM_MEM_REFERENCED : 0);
}

/*
 * pmap_clear_refmod(phys, mask)
 *  clears the referenced and modified bits as specified by the mask
 *  of the specified physical page.
 */
void
pmap_clear_refmod_options(
	ppnum_t         pn,
	unsigned int    mask,
	unsigned int    options,
	void            *arg)
{
	unsigned int    bits;

	bits = ((mask & VM_MEM_MODIFIED) ? PP_ATTR_MODIFIED : 0) |
	    ((mask & VM_MEM_REFERENCED) ? PP_ATTR_REFERENCED : 0);
	phys_attribute_clear(pn, bits, options, arg);
}

void
pmap_clear_refmod(
	ppnum_t pn,
	unsigned int mask)
{
	pmap_clear_refmod_options(pn, mask, 0, NULL);
}

unsigned int
pmap_disconnect_options(
	ppnum_t pn,
	unsigned int options,
	void *arg)
{
	if ((options & PMAP_OPTIONS_COMPRESSOR_IFF_MODIFIED)) {
		/*
		 * On ARM, the "modified" bit is managed by software, so
		 * we know up-front if the physical page is "modified",
		 * without having to scan all the PTEs pointing to it.
		 * The caller should have made the VM page "busy" so noone
		 * should be able to establish any new mapping and "modify"
		 * the page behind us.
		 */
		if (pmap_is_modified(pn)) {
			/*
			 * The page has been modified and will be sent to
			 * the VM compressor.
			 */
			options |= PMAP_OPTIONS_COMPRESSOR;
		} else {
			/*
			 * The page hasn't been modified and will be freed
			 * instead of compressed.
			 */
		}
	}

	/* disconnect the page */
	pmap_page_protect_options(pn, 0, options, arg);

	/* return ref/chg status */
	return pmap_get_refmod(pn);
}

/*
 *	Routine:
 *		pmap_disconnect
 *
 *	Function:
 *		Disconnect all mappings for this page and return reference and change status
 *		in generic format.
 *
 */
unsigned int
pmap_disconnect(
	ppnum_t pn)
{
	pmap_page_protect(pn, 0);       /* disconnect the page */
	return pmap_get_refmod(pn);   /* return ref/chg status */
}

boolean_t
pmap_has_managed_page(ppnum_t first, ppnum_t last)
{
	if (ptoa(first) >= vm_last_phys) {
		return FALSE;
	}
	if (ptoa(last) < vm_first_phys) {
		return FALSE;
	}

	return TRUE;
}

/*
 * The state maintained by the noencrypt functions is used as a
 * debugging aid on ARM.  This incurs some overhead on the part
 * of the caller.  A special case check in phys_attribute_clear
 * (the most expensive path) currently minimizes this overhead,
 * but stubbing these functions out on RELEASE kernels yields
 * further wins.
 */
boolean_t
pmap_is_noencrypt(
	ppnum_t pn)
{
#if DEVELOPMENT || DEBUG
	boolean_t result = FALSE;

	if (!pa_valid(ptoa(pn))) {
		return FALSE;
	}

	result = (phys_attribute_test(pn, PP_ATTR_NOENCRYPT));

	return result;
#else
#pragma unused(pn)
	return FALSE;
#endif
}

void
pmap_set_noencrypt(
	ppnum_t pn)
{
#if DEVELOPMENT || DEBUG
	if (!pa_valid(ptoa(pn))) {
		return;
	}

	phys_attribute_set(pn, PP_ATTR_NOENCRYPT);
#else
#pragma unused(pn)
#endif
}

void
pmap_clear_noencrypt(
	ppnum_t pn)
{
#if DEVELOPMENT || DEBUG
	if (!pa_valid(ptoa(pn))) {
		return;
	}

	phys_attribute_clear(pn, PP_ATTR_NOENCRYPT, 0, NULL);
#else
#pragma unused(pn)
#endif
}

#if XNU_MONITOR
boolean_t
pmap_is_monitor(ppnum_t pn)
{
	assert(pa_valid(ptoa(pn)));
	return phys_attribute_test(pn, PP_ATTR_MONITOR);
}
#endif

void
pmap_lock_phys_page(ppnum_t pn)
{
#if !XNU_MONITOR
	int             pai;
	pmap_paddr_t    phys = ptoa(pn);

	if (pa_valid(phys)) {
		pai = (int)pa_index(phys);
		LOCK_PVH(pai);
	} else
#else
	(void)pn;
#endif
	{ simple_lock(&phys_backup_lock, LCK_GRP_NULL);}
}


void
pmap_unlock_phys_page(ppnum_t pn)
{
#if !XNU_MONITOR
	int             pai;
	pmap_paddr_t    phys = ptoa(pn);

	if (pa_valid(phys)) {
		pai = (int)pa_index(phys);
		UNLOCK_PVH(pai);
	} else
#else
	(void)pn;
#endif
	{ simple_unlock(&phys_backup_lock);}
}

MARK_AS_PMAP_TEXT static void
pmap_switch_user_ttb_internal(
	pmap_t pmap)
{
	VALIDATE_PMAP(pmap);
	pmap_cpu_data_t *cpu_data_ptr;
	cpu_data_ptr = pmap_get_cpu_data();

#if     (__ARM_VMSA__ == 7)
	cpu_data_ptr->cpu_user_pmap = pmap;
	cpu_data_ptr->cpu_user_pmap_stamp = pmap->stamp;

#if     MACH_ASSERT && __ARM_USER_PROTECT__
	{
		unsigned int ttbr0_val, ttbr1_val;
		__asm__ volatile ("mrc p15,0,%0,c2,c0,0\n" : "=r"(ttbr0_val));
		__asm__ volatile ("mrc p15,0,%0,c2,c0,1\n" : "=r"(ttbr1_val));
		if (ttbr0_val != ttbr1_val) {
			panic("Misaligned ttbr0  %08X\n", ttbr0_val);
		}
	}
#endif
	if (pmap->tte_index_max == NTTES) {
		/* Setting TTBCR.N for TTBR0 TTBR1 boundary at  0x40000000 */
		__asm__ volatile ("mcr	p15,0,%0,c2,c0,2"  : : "r"(2));
		__builtin_arm_isb(ISB_SY);
#if !__ARM_USER_PROTECT__
		set_mmu_ttb(pmap->ttep);
#endif
	} else {
#if !__ARM_USER_PROTECT__
		set_mmu_ttb(pmap->ttep);
#endif
		/* Setting TTBCR.N for TTBR0 TTBR1 boundary at  0x80000000 */
		__asm__ volatile ("mcr	p15,0,%0,c2,c0,2"  : : "r"(1));
		__builtin_arm_isb(ISB_SY);
#if     MACH_ASSERT && __ARM_USER_PROTECT__
		if (pmap->ttep & 0x1000) {
			panic("Misaligned ttbr0  %08X\n", pmap->ttep);
		}
#endif
	}

#if !__ARM_USER_PROTECT__
	set_context_id(pmap->hw_asid);
#endif

#else /* (__ARM_VMSA__ == 7) */

	if (pmap != kernel_pmap) {
		cpu_data_ptr->cpu_nested_pmap = pmap->nested_pmap;
	}

	if (pmap == kernel_pmap) {
		pmap_clear_user_ttb_internal();
	} else {
		set_mmu_ttb((pmap->ttep & TTBR_BADDR_MASK) | (((uint64_t)pmap->hw_asid) << TTBR_ASID_SHIFT));
	}

#if defined(HAS_APPLE_PAC) && (__APCFG_SUPPORTED__ || __APSTS_SUPPORTED__)
	if (!(BootArgs->bootFlags & kBootFlagsDisableJOP) && !(BootArgs->bootFlags & kBootFlagsDisableUserJOP)) {
		uint64_t sctlr = __builtin_arm_rsr64("SCTLR_EL1");
		bool jop_enabled = sctlr & SCTLR_JOP_KEYS_ENABLED;
		if (!jop_enabled && !pmap->disable_jop) {
			// turn on JOP
			sctlr |= SCTLR_JOP_KEYS_ENABLED;
			__builtin_arm_wsr64("SCTLR_EL1", sctlr);
			// no ISB necessary because this won't take effect until eret returns to EL0
		} else if (jop_enabled && pmap->disable_jop) {
			// turn off JOP
			sctlr &= ~SCTLR_JOP_KEYS_ENABLED;
			__builtin_arm_wsr64("SCTLR_EL1", sctlr);
		}
	}
#endif /* HAS_APPLE_PAC && (__APCFG_SUPPORTED__ || __APSTS_SUPPORTED__) */
#endif /* (__ARM_VMSA__ == 7) */
}

void
pmap_switch_user_ttb(
	pmap_t pmap)
{
	PMAP_TRACE(1, PMAP_CODE(PMAP__SWITCH_USER_TTB) | DBG_FUNC_START, VM_KERNEL_ADDRHIDE(pmap), PMAP_VASID(pmap), pmap->hw_asid);
#if XNU_MONITOR
	pmap_switch_user_ttb_ppl(pmap);
#else
	pmap_switch_user_ttb_internal(pmap);
#endif
	PMAP_TRACE(1, PMAP_CODE(PMAP__SWITCH_USER_TTB) | DBG_FUNC_END);
}

MARK_AS_PMAP_TEXT static void
pmap_clear_user_ttb_internal(void)
{
#if (__ARM_VMSA__ > 7)
	set_mmu_ttb(invalid_ttep & TTBR_BADDR_MASK);
#else
	set_mmu_ttb(kernel_pmap->ttep);
#endif
}

void
pmap_clear_user_ttb(void)
{
#if XNU_MONITOR
	pmap_clear_user_ttb_ppl();
#else
	pmap_clear_user_ttb_internal();
#endif
}

/*
 *	Routine:	arm_force_fast_fault
 *
 *	Function:
 *		Force all mappings for this page to fault according
 *		to the access modes allowed, so we can gather ref/modify
 *		bits again.
 */
MARK_AS_PMAP_TEXT static boolean_t
arm_force_fast_fault_internal(
	ppnum_t         ppnum,
	vm_prot_t       allow_mode,
	int             options)
{
	pmap_paddr_t     phys = ptoa(ppnum);
	pv_entry_t      *pve_p;
	pt_entry_t      *pte_p;
	int              pai;
	boolean_t        result;
	pv_entry_t     **pv_h;
	boolean_t        is_reusable, is_internal;
	boolean_t        tlb_flush_needed = FALSE;
	boolean_t        ref_fault;
	boolean_t        mod_fault;

	assert(ppnum != vm_page_fictitious_addr);

	if (!pa_valid(phys)) {
		return FALSE;   /* Not a managed page. */
	}

	result = TRUE;
	ref_fault = FALSE;
	mod_fault = FALSE;
	pai = (int)pa_index(phys);
	LOCK_PVH(pai);
	pv_h = pai_to_pvh(pai);

	pte_p = PT_ENTRY_NULL;
	pve_p = PV_ENTRY_NULL;
	if (pvh_test_type(pv_h, PVH_TYPE_PTEP)) {
		pte_p = pvh_ptep(pv_h);
	} else if (pvh_test_type(pv_h, PVH_TYPE_PVEP)) {
		pve_p = pvh_list(pv_h);
	}

	is_reusable = IS_REUSABLE_PAGE(pai);
	is_internal = IS_INTERNAL_PAGE(pai);

	while ((pve_p != PV_ENTRY_NULL) || (pte_p != PT_ENTRY_NULL)) {
		vm_map_address_t va;
		pt_entry_t       spte;
		pt_entry_t       tmplate;
		pmap_t           pmap;
		boolean_t        update_pte;

		if (pve_p != PV_ENTRY_NULL) {
			pte_p = pve_get_ptep(pve_p);
		}

		if (pte_p == PT_ENTRY_NULL) {
			panic("pte_p is NULL: pve_p=%p ppnum=0x%x\n", pve_p, ppnum);
		}
#ifdef PVH_FLAG_IOMMU
		if ((vm_offset_t)pte_p & PVH_FLAG_IOMMU) {
			goto fff_skip_pve;
		}
#endif
		if (*pte_p == ARM_PTE_EMPTY) {
			panic("pte is NULL: pte_p=%p ppnum=0x%x\n", pte_p, ppnum);
		}
		if (ARM_PTE_IS_COMPRESSED(*pte_p, pte_p)) {
			panic("pte is COMPRESSED: pte_p=%p ppnum=0x%x\n", pte_p, ppnum);
		}

		pmap = ptep_get_pmap(pte_p);
		va = ptep_get_va(pte_p);

		assert(va >= pmap->min && va < pmap->max);

		if (pte_is_wired(*pte_p) || pmap == kernel_pmap) {
			result = FALSE;
			break;
		}

		spte = *pte_p;
		tmplate = spte;
		update_pte = FALSE;

		if ((allow_mode & VM_PROT_READ) != VM_PROT_READ) {
			/* read protection sets the pte to fault */
			tmplate =  tmplate & ~ARM_PTE_AF;
			update_pte = TRUE;
			ref_fault = TRUE;
		}
		if ((allow_mode & VM_PROT_WRITE) != VM_PROT_WRITE) {
			/* take away write permission if set */
			if (pmap == kernel_pmap) {
				if ((tmplate & ARM_PTE_APMASK) == ARM_PTE_AP(AP_RWNA)) {
					tmplate = ((tmplate & ~ARM_PTE_APMASK) | ARM_PTE_AP(AP_RONA));
					pte_set_was_writeable(tmplate, true);
					update_pte = TRUE;
					mod_fault = TRUE;
				}
			} else {
				if ((tmplate & ARM_PTE_APMASK) == ARM_PTE_AP(AP_RWRW)) {
					tmplate = ((tmplate & ~ARM_PTE_APMASK) | pt_attr_leaf_ro(pmap_get_pt_attr(pmap)));
					pte_set_was_writeable(tmplate, true);
					update_pte = TRUE;
					mod_fault = TRUE;
				}
			}
		}

#if MACH_ASSERT && XNU_MONITOR
		if (is_pte_xprr_protected(spte)) {
			if (pte_to_xprr_perm(spte) != pte_to_xprr_perm(tmplate)) {
				panic("%s: attempted to mutate an xPRR mapping pte_p=%p, pmap=%p, pv_h=%p, pve_p=%p, pte=0x%llx, tmplate=0x%llx, va=0x%llx, "
				    "ppnum=0x%x, options=0x%x, allow_mode=0x%x",
				    __FUNCTION__, pte_p, pmap, pv_h, pve_p, (unsigned long long)spte, (unsigned long long)tmplate, (unsigned long long)va,
				    ppnum, options, allow_mode);
			}
		}
#endif /* MACH_ASSERT && XNU_MONITOR */

		if (update_pte) {
			if (*pte_p != ARM_PTE_TYPE_FAULT &&
			    !ARM_PTE_IS_COMPRESSED(*pte_p, pte_p)) {
				WRITE_PTE_STRONG(pte_p, tmplate);
				pmap_get_pt_ops(pmap)->flush_tlb_region_async(va, PAGE_SIZE, pmap);
				tlb_flush_needed = TRUE;
			} else {
				WRITE_PTE(pte_p, tmplate);
				__builtin_arm_isb(ISB_SY);
			}
		}

		/* update pmap stats and ledgers */
		if (IS_ALTACCT_PAGE(pai, pve_p)) {
			/*
			 * We do not track "reusable" status for
			 * "alternate accounting" mappings.
			 */
		} else if ((options & PMAP_OPTIONS_CLEAR_REUSABLE) &&
		    is_reusable &&
		    is_internal &&
		    pmap != kernel_pmap) {
			/* one less "reusable" */
			PMAP_STATS_ASSERTF(pmap->stats.reusable > 0, pmap, "stats.reusable %d", pmap->stats.reusable);
			OSAddAtomic(-1, &pmap->stats.reusable);
			/* one more "internal" */
			OSAddAtomic(+1, &pmap->stats.internal);
			PMAP_STATS_PEAK(pmap->stats.internal);
			PMAP_STATS_ASSERTF(pmap->stats.internal > 0, pmap, "stats.internal %d", pmap->stats.internal);
			pmap_ledger_credit(pmap, task_ledgers.internal, machine_ptob(1));
			assert(!IS_ALTACCT_PAGE(pai, pve_p));
			assert(IS_INTERNAL_PAGE(pai));
			pmap_ledger_credit(pmap, task_ledgers.phys_footprint, machine_ptob(1));

			/*
			 * Avoid the cost of another trap to handle the fast
			 * fault when we next write to this page:  let's just
			 * handle that now since we already have all the
			 * necessary information.
			 */
			{
				arm_clear_fast_fault(ppnum, VM_PROT_WRITE);
			}
		} else if ((options & PMAP_OPTIONS_SET_REUSABLE) &&
		    !is_reusable &&
		    is_internal &&
		    pmap != kernel_pmap) {
			/* one more "reusable" */
			OSAddAtomic(+1, &pmap->stats.reusable);
			PMAP_STATS_PEAK(pmap->stats.reusable);
			PMAP_STATS_ASSERTF(pmap->stats.reusable > 0, pmap, "stats.reusable %d", pmap->stats.reusable);
			/* one less "internal" */
			PMAP_STATS_ASSERTF(pmap->stats.internal > 0, pmap, "stats.internal %d", pmap->stats.internal);
			OSAddAtomic(-1, &pmap->stats.internal);
			pmap_ledger_debit(pmap, task_ledgers.internal, machine_ptob(1));
			assert(!IS_ALTACCT_PAGE(pai, pve_p));
			assert(IS_INTERNAL_PAGE(pai));
			pmap_ledger_debit(pmap, task_ledgers.phys_footprint, machine_ptob(1));
		}

#ifdef PVH_FLAG_IOMMU
fff_skip_pve:
#endif
		pte_p = PT_ENTRY_NULL;
		if (pve_p != PV_ENTRY_NULL) {
			pve_p = PVE_NEXT_PTR(pve_next(pve_p));
		}
	}

	if (tlb_flush_needed) {
		sync_tlb_flush();
	}

	/* update global "reusable" status for this page */
	if (is_internal) {
		if ((options & PMAP_OPTIONS_CLEAR_REUSABLE) &&
		    is_reusable) {
			CLR_REUSABLE_PAGE(pai);
		} else if ((options & PMAP_OPTIONS_SET_REUSABLE) &&
		    !is_reusable) {
			SET_REUSABLE_PAGE(pai);
		}
	}

	if (mod_fault) {
		SET_MODFAULT_PAGE(pai);
	}
	if (ref_fault) {
		SET_REFFAULT_PAGE(pai);
	}

	UNLOCK_PVH(pai);
	return result;
}

boolean_t
arm_force_fast_fault(
	ppnum_t         ppnum,
	vm_prot_t       allow_mode,
	int             options,
	__unused void   *arg)
{
	pmap_paddr_t    phys = ptoa(ppnum);

	assert(ppnum != vm_page_fictitious_addr);

	if (!pa_valid(phys)) {
		return FALSE;   /* Not a managed page. */
	}

#if XNU_MONITOR
	return arm_force_fast_fault_ppl(ppnum, allow_mode, options);
#else
	return arm_force_fast_fault_internal(ppnum, allow_mode, options);
#endif
}

/*
 *	Routine:	arm_clear_fast_fault
 *
 *	Function:
 *		Clear pending force fault for all mappings for this page based on
 *		the observed fault type, update ref/modify bits.
 */
boolean_t
arm_clear_fast_fault(
	ppnum_t ppnum,
	vm_prot_t fault_type)
{
	pmap_paddr_t    pa = ptoa(ppnum);
	pv_entry_t     *pve_p;
	pt_entry_t     *pte_p;
	int             pai;
	boolean_t       result;
	boolean_t       tlb_flush_needed = FALSE;
	pv_entry_t    **pv_h;

	assert(ppnum != vm_page_fictitious_addr);

	if (!pa_valid(pa)) {
		return FALSE;   /* Not a managed page. */
	}

	result = FALSE;
	pai = (int)pa_index(pa);
	ASSERT_PVH_LOCKED(pai);
	pv_h = pai_to_pvh(pai);

	pte_p = PT_ENTRY_NULL;
	pve_p = PV_ENTRY_NULL;
	if (pvh_test_type(pv_h, PVH_TYPE_PTEP)) {
		pte_p = pvh_ptep(pv_h);
	} else if (pvh_test_type(pv_h, PVH_TYPE_PVEP)) {
		pve_p = pvh_list(pv_h);
	}

	while ((pve_p != PV_ENTRY_NULL) || (pte_p != PT_ENTRY_NULL)) {
		vm_map_address_t va;
		pt_entry_t              spte;
		pt_entry_t      tmplate;
		pmap_t          pmap;

		if (pve_p != PV_ENTRY_NULL) {
			pte_p = pve_get_ptep(pve_p);
		}

		if (pte_p == PT_ENTRY_NULL) {
			panic("pte_p is NULL: pve_p=%p ppnum=0x%x\n", pve_p, ppnum);
		}
#ifdef PVH_FLAG_IOMMU
		if ((vm_offset_t)pte_p & PVH_FLAG_IOMMU) {
			goto cff_skip_pve;
		}
#endif
		if (*pte_p == ARM_PTE_EMPTY) {
			panic("pte is NULL: pte_p=%p ppnum=0x%x\n", pte_p, ppnum);
		}

		pmap = ptep_get_pmap(pte_p);
		va = ptep_get_va(pte_p);

		assert(va >= pmap->min && va < pmap->max);

		spte = *pte_p;
		tmplate = spte;

		if ((fault_type & VM_PROT_WRITE) && (pte_was_writeable(spte))) {
			{
				if (pmap == kernel_pmap) {
					tmplate = ((spte & ~ARM_PTE_APMASK) | ARM_PTE_AP(AP_RWNA));
				} else {
					tmplate = ((spte & ~ARM_PTE_APMASK) | pt_attr_leaf_rw(pmap_get_pt_attr(pmap)));
				}
			}

			tmplate |= ARM_PTE_AF;

			pte_set_was_writeable(tmplate, false);
			pa_set_bits(pa, PP_ATTR_REFERENCED | PP_ATTR_MODIFIED);
		} else if ((fault_type & VM_PROT_READ) && ((spte & ARM_PTE_AF) != ARM_PTE_AF)) {
			tmplate = spte | ARM_PTE_AF;

			{
				pa_set_bits(pa, PP_ATTR_REFERENCED);
			}
		}

#if MACH_ASSERT && XNU_MONITOR
		if (is_pte_xprr_protected(spte)) {
			if (pte_to_xprr_perm(spte) != pte_to_xprr_perm(tmplate)) {
				panic("%s: attempted to mutate an xPRR mapping pte_p=%p, pmap=%p, pv_h=%p, pve_p=%p, pte=0x%llx, tmplate=0x%llx, va=0x%llx, "
				    "ppnum=0x%x, fault_type=0x%x",
				    __FUNCTION__, pte_p, pmap, pv_h, pve_p, (unsigned long long)spte, (unsigned long long)tmplate, (unsigned long long)va,
				    ppnum, fault_type);
			}
		}
#endif /* MACH_ASSERT && XNU_MONITOR */

		if (spte != tmplate) {
			if (spte != ARM_PTE_TYPE_FAULT) {
				WRITE_PTE_STRONG(pte_p, tmplate);
				pmap_get_pt_ops(pmap)->flush_tlb_region_async(va, PAGE_SIZE, pmap);
				tlb_flush_needed = TRUE;
			} else {
				WRITE_PTE(pte_p, tmplate);
				__builtin_arm_isb(ISB_SY);
			}
			result = TRUE;
		}

#ifdef PVH_FLAG_IOMMU
cff_skip_pve:
#endif
		pte_p = PT_ENTRY_NULL;
		if (pve_p != PV_ENTRY_NULL) {
			pve_p = PVE_NEXT_PTR(pve_next(pve_p));
		}
	}
	if (tlb_flush_needed) {
		sync_tlb_flush();
	}
	return result;
}

/*
 * Determine if the fault was induced by software tracking of
 * modify/reference bits.  If so, re-enable the mapping (and set
 * the appropriate bits).
 *
 * Returns KERN_SUCCESS if the fault was induced and was
 * successfully handled.
 *
 * Returns KERN_FAILURE if the fault was not induced and
 * the function was unable to deal with it.
 *
 * Returns KERN_PROTECTION_FAILURE if the pmap layer explictly
 * disallows this type of access.
 */
MARK_AS_PMAP_TEXT static kern_return_t
arm_fast_fault_internal(
	pmap_t pmap,
	vm_map_address_t va,
	vm_prot_t fault_type,
	__unused bool was_af_fault,
	__unused bool from_user)
{
	kern_return_t   result = KERN_FAILURE;
	pt_entry_t     *ptep;
	pt_entry_t      spte = ARM_PTE_TYPE_FAULT;
	int             pai;
	pmap_paddr_t    pa;
	VALIDATE_PMAP(pmap);

	PMAP_LOCK(pmap);

	/*
	 * If the entry doesn't exist, is completely invalid, or is already
	 * valid, we can't fix it here.
	 */

	ptep = pmap_pte(pmap, va);
	if (ptep != PT_ENTRY_NULL) {
		while (true) {
			spte = *ptep;

			pa = pte_to_pa(spte);

			if ((spte == ARM_PTE_TYPE_FAULT) ||
			    ARM_PTE_IS_COMPRESSED(spte, ptep)) {
				PMAP_UNLOCK(pmap);
				return result;
			}

			if (!pa_valid(pa)) {
				PMAP_UNLOCK(pmap);
#if XNU_MONITOR
				if (pmap_cache_attributes((ppnum_t)atop(pa)) & PP_ATTR_MONITOR) {
					return KERN_PROTECTION_FAILURE;
				} else
#endif
				return result;
			}
			pai = (int)pa_index(pa);
			LOCK_PVH(pai);
#if __APRR_SUPPORTED__
			if (*ptep == spte) {
				/*
				 * Double-check the spte value, as we care
				 * about the AF bit.
				 */
				break;
			}
			UNLOCK_PVH(pai);
#else /* !(__APRR_SUPPORTED__*/
			break;
#endif /* !(__APRR_SUPPORTED__*/
		}
	} else {
		PMAP_UNLOCK(pmap);
		return result;
	}

#if __APRR_SUPPORTED__
	/* Check to see if this mapping had APRR restrictions. */
	if (is_pte_xprr_protected(spte)) {
		/*
		 * We have faulted on an XPRR managed mapping; decide if the access should be
		 * reattempted or if it should cause an exception. Now that all JIT entitled
		 * task threads always have MPRR enabled we're only here because of
		 * an AF fault or an actual permission fault. AF faults will have result
		 * changed to KERN_SUCCESS below upon arm_clear_fast_fault return.
		 */
		if (was_af_fault && (spte & ARM_PTE_AF)) {
			result = KERN_SUCCESS;
			goto out;
		} else {
			result = KERN_PROTECTION_FAILURE;
		}
	}
#endif /* __APRR_SUPPORTED__*/

	if ((IS_REFFAULT_PAGE(pai)) ||
	    ((fault_type & VM_PROT_WRITE) && IS_MODFAULT_PAGE(pai))) {
		/*
		 * An attempted access will always clear ref/mod fault state, as
		 * appropriate for the fault type.  arm_clear_fast_fault will
		 * update the associated PTEs for the page as appropriate; if
		 * any PTEs are updated, we redrive the access.  If the mapping
		 * does not actually allow for the attempted access, the
		 * following fault will (hopefully) fail to update any PTEs, and
		 * thus cause arm_fast_fault to decide that it failed to handle
		 * the fault.
		 */
		if (IS_REFFAULT_PAGE(pai)) {
			CLR_REFFAULT_PAGE(pai);
		}
		if ((fault_type & VM_PROT_WRITE) && IS_MODFAULT_PAGE(pai)) {
			CLR_MODFAULT_PAGE(pai);
		}

		if (arm_clear_fast_fault((ppnum_t)atop(pa), fault_type)) {
			/*
			 * Should this preserve KERN_PROTECTION_FAILURE?  The
			 * cost of not doing so is a another fault in a case
			 * that should already result in an exception.
			 */
			result = KERN_SUCCESS;
		}
	}

#if __APRR_SUPPORTED__
out:
#endif /* __APRR_SUPPORTED__*/
	UNLOCK_PVH(pai);
	PMAP_UNLOCK(pmap);
	return result;
}

kern_return_t
arm_fast_fault(
	pmap_t pmap,
	vm_map_address_t va,
	vm_prot_t fault_type,
	bool was_af_fault,
	__unused bool from_user)
{
	kern_return_t   result = KERN_FAILURE;

	if (va < pmap->min || va >= pmap->max) {
		return result;
	}

	PMAP_TRACE(3, PMAP_CODE(PMAP__FAST_FAULT) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE(va), fault_type,
	    from_user);

#if     (__ARM_VMSA__ == 7)
	if (pmap != kernel_pmap) {
		pmap_cpu_data_t *cpu_data_ptr = pmap_get_cpu_data();
		pmap_t          cur_pmap;
		pmap_t          cur_user_pmap;

		cur_pmap = current_pmap();
		cur_user_pmap = cpu_data_ptr->cpu_user_pmap;

		if ((cur_user_pmap == cur_pmap) && (cur_pmap == pmap)) {
			if (cpu_data_ptr->cpu_user_pmap_stamp != pmap->stamp) {
				pmap_set_pmap(pmap, current_thread());
				result = KERN_SUCCESS;
				goto done;
			}
		}
	}
#endif

#if XNU_MONITOR
	result = arm_fast_fault_ppl(pmap, va, fault_type, was_af_fault, from_user);
#else
	result = arm_fast_fault_internal(pmap, va, fault_type, was_af_fault, from_user);
#endif

#if (__ARM_VMSA__ == 7)
done:
#endif

	PMAP_TRACE(3, PMAP_CODE(PMAP__FAST_FAULT) | DBG_FUNC_END, result);

	return result;
}

void
pmap_copy_page(
	ppnum_t psrc,
	ppnum_t pdst)
{
	bcopy_phys((addr64_t) (ptoa(psrc)),
	    (addr64_t) (ptoa(pdst)),
	    PAGE_SIZE);
}


/*
 *	pmap_copy_page copies the specified (machine independent) pages.
 */
void
pmap_copy_part_page(
	ppnum_t psrc,
	vm_offset_t src_offset,
	ppnum_t pdst,
	vm_offset_t dst_offset,
	vm_size_t len)
{
	bcopy_phys((addr64_t) (ptoa(psrc) + src_offset),
	    (addr64_t) (ptoa(pdst) + dst_offset),
	    len);
}


/*
 *	pmap_zero_page zeros the specified (machine independent) page.
 */
void
pmap_zero_page(
	ppnum_t pn)
{
	assert(pn != vm_page_fictitious_addr);
	bzero_phys((addr64_t) ptoa(pn), PAGE_SIZE);
}

/*
 *	pmap_zero_part_page
 *	zeros the specified (machine independent) part of a page.
 */
void
pmap_zero_part_page(
	ppnum_t pn,
	vm_offset_t offset,
	vm_size_t len)
{
	assert(pn != vm_page_fictitious_addr);
	assert(offset + len <= PAGE_SIZE);
	bzero_phys((addr64_t) (ptoa(pn) + offset), len);
}


/*
 * nop in current arm implementation
 */
void
inval_copy_windows(
	__unused thread_t t)
{
}

void
pmap_map_globals(
	void)
{
	pt_entry_t      *ptep, pte;

	ptep = pmap_pte(kernel_pmap, LOWGLOBAL_ALIAS);
	assert(ptep != PT_ENTRY_NULL);
	assert(*ptep == ARM_PTE_EMPTY);

	pte = pa_to_pte(ml_static_vtop((vm_offset_t)&lowGlo)) | AP_RONA | ARM_PTE_NX | ARM_PTE_PNX | ARM_PTE_AF | ARM_PTE_TYPE;
#if __ARM_KERNEL_PROTECT__
	pte |= ARM_PTE_NG;
#endif /* __ARM_KERNEL_PROTECT__ */
	pte |= ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITEBACK);
#if     (__ARM_VMSA__ > 7)
	pte |= ARM_PTE_SH(SH_OUTER_MEMORY);
#else
	pte |= ARM_PTE_SH;
#endif
	*ptep = pte;
	FLUSH_PTE_RANGE(ptep, (ptep + 1));
	PMAP_UPDATE_TLBS(kernel_pmap, LOWGLOBAL_ALIAS, LOWGLOBAL_ALIAS + PAGE_SIZE, false);
}

vm_offset_t
pmap_cpu_windows_copy_addr(int cpu_num, unsigned int index)
{
	if (__improbable(index >= CPUWINDOWS_MAX)) {
		panic("%s: invalid index %u", __func__, index);
	}
	return (vm_offset_t)(CPUWINDOWS_BASE + (PAGE_SIZE * ((CPUWINDOWS_MAX * cpu_num) + index)));
}

MARK_AS_PMAP_TEXT static unsigned int
pmap_map_cpu_windows_copy_internal(
	ppnum_t pn,
	vm_prot_t prot,
	unsigned int wimg_bits)
{
	pt_entry_t      *ptep = NULL, pte;
	pmap_cpu_data_t *pmap_cpu_data = pmap_get_cpu_data();
	unsigned int    cpu_num;
	unsigned int    i;
	vm_offset_t     cpu_copywindow_vaddr = 0;
	bool            need_strong_sync = false;

#if XNU_MONITOR || HAS_MILD_DSB
	unsigned int    cacheattr = (!pa_valid(ptoa(pn)) ? pmap_cache_attributes(pn) : 0);
	need_strong_sync = ((cacheattr & PMAP_IO_RANGE_STRONG_SYNC) != 0);
#endif

#if XNU_MONITOR
#ifdef  __ARM_COHERENT_IO__
	if (pa_valid(ptoa(pn)) && !pmap_ppl_disable) {
		panic("%s: attempted to map a managed page, "
		    "pn=%u, prot=0x%x, wimg_bits=0x%x",
		    __FUNCTION__,
		    pn, prot, wimg_bits);
	}
	if (!pmap_ppl_disable && (cacheattr & PP_ATTR_MONITOR)) {
		panic("%s: attempt to map PPL-protected I/O address 0x%llx", __func__, (uint64_t)ptoa(pn));
	}

#else /* __ARM_COHERENT_IO__ */
#error CPU copy windows are not properly supported with both the PPL and incoherent IO
#endif /* __ARM_COHERENT_IO__ */
#endif /* XNU_MONITOR */
	cpu_num = pmap_cpu_data->cpu_number;

	for (i = 0; i < CPUWINDOWS_MAX; i++) {
		cpu_copywindow_vaddr = pmap_cpu_windows_copy_addr(cpu_num, i);
		ptep = pmap_pte(kernel_pmap, cpu_copywindow_vaddr);
		assert(!ARM_PTE_IS_COMPRESSED(*ptep, ptep));
		if (*ptep == ARM_PTE_TYPE_FAULT) {
			break;
		}
	}
	if (i == CPUWINDOWS_MAX) {
		panic("pmap_map_cpu_windows_copy: out of window\n");
	}

	pte = pa_to_pte(ptoa(pn)) | ARM_PTE_TYPE | ARM_PTE_AF | ARM_PTE_NX | ARM_PTE_PNX;
#if __ARM_KERNEL_PROTECT__
	pte |= ARM_PTE_NG;
#endif /* __ARM_KERNEL_PROTECT__ */

	pte |= wimg_to_pte(wimg_bits);

	if (prot & VM_PROT_WRITE) {
		pte |= ARM_PTE_AP(AP_RWNA);
	} else {
		pte |= ARM_PTE_AP(AP_RONA);
	}

	WRITE_PTE_FAST(ptep, pte);
	/*
	 * Invalidate tlb. Cover nested cpu_copywindow_vaddr usage with the interrupted context
	 * in pmap_unmap_cpu_windows_copy() after clearing the pte and before tlb invalidate.
	 */
	FLUSH_PTE_STRONG(ptep);
	PMAP_UPDATE_TLBS(kernel_pmap, cpu_copywindow_vaddr, cpu_copywindow_vaddr + PAGE_SIZE, pmap_cpu_data->copywindow_strong_sync[i]);
	pmap_cpu_data->copywindow_strong_sync[i] = need_strong_sync;

	return i;
}

unsigned int
pmap_map_cpu_windows_copy(
	ppnum_t pn,
	vm_prot_t prot,
	unsigned int wimg_bits)
{
#if XNU_MONITOR
	return pmap_map_cpu_windows_copy_ppl(pn, prot, wimg_bits);
#else
	return pmap_map_cpu_windows_copy_internal(pn, prot, wimg_bits);
#endif
}

MARK_AS_PMAP_TEXT static void
pmap_unmap_cpu_windows_copy_internal(
	unsigned int index)
{
	pt_entry_t      *ptep;
	unsigned int    cpu_num;
	vm_offset_t     cpu_copywindow_vaddr = 0;
	pmap_cpu_data_t *pmap_cpu_data = pmap_get_cpu_data();

	cpu_num = pmap_cpu_data->cpu_number;

	cpu_copywindow_vaddr = pmap_cpu_windows_copy_addr(cpu_num, index);
	/* Issue full-system DSB to ensure prior operations on the per-CPU window
	 * (which are likely to have been on I/O memory) are complete before
	 * tearing down the mapping. */
	__builtin_arm_dsb(DSB_SY);
	ptep = pmap_pte(kernel_pmap, cpu_copywindow_vaddr);
	WRITE_PTE_STRONG(ptep, ARM_PTE_TYPE_FAULT);
	PMAP_UPDATE_TLBS(kernel_pmap, cpu_copywindow_vaddr, cpu_copywindow_vaddr + PAGE_SIZE, pmap_cpu_data->copywindow_strong_sync[index]);
}

void
pmap_unmap_cpu_windows_copy(
	unsigned int index)
{
#if XNU_MONITOR
	return pmap_unmap_cpu_windows_copy_ppl(index);
#else
	return pmap_unmap_cpu_windows_copy_internal(index);
#endif
}

/*
 * Indicate that a pmap is intended to be used as a nested pmap
 * within one or more larger address spaces.  This must be set
 * before pmap_nest() is called with this pmap as the 'subordinate'.
 */
MARK_AS_PMAP_TEXT static void
pmap_set_nested_internal(
	pmap_t pmap)
{
	VALIDATE_PMAP(pmap);
	pmap->nested = TRUE;
}

void
pmap_set_nested(
	pmap_t pmap)
{
#if XNU_MONITOR
	pmap_set_nested_ppl(pmap);
#else
	pmap_set_nested_internal(pmap);
#endif
}

/*
 * pmap_trim_range(pmap, start, end)
 *
 * pmap  = pmap to operate on
 * start = start of the range
 * end   = end of the range
 *
 * Attempts to deallocate TTEs for the given range in the nested range.
 */
MARK_AS_PMAP_TEXT static void
pmap_trim_range(
	pmap_t pmap,
	addr64_t start,
	addr64_t end)
{
	addr64_t cur;
	addr64_t nested_region_start;
	addr64_t nested_region_end;
	addr64_t adjusted_start;
	addr64_t adjusted_end;
	addr64_t adjust_offmask;
	tt_entry_t * tte_p;
	pt_entry_t * pte_p;
	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	if (__improbable(end < start)) {
		panic("%s: invalid address range, "
		    "pmap=%p, start=%p, end=%p",
		    __func__,
		    pmap, (void*)start, (void*)end);
	}

	nested_region_start = pmap->nested ? pmap->nested_region_subord_addr : pmap->nested_region_subord_addr;
	nested_region_end = nested_region_start + pmap->nested_region_size;

	if (__improbable((start < nested_region_start) || (end > nested_region_end))) {
		panic("%s: range outside nested region %p-%p, "
		    "pmap=%p, start=%p, end=%p",
		    __func__, (void *)nested_region_start, (void *)nested_region_end,
		    pmap, (void*)start, (void*)end);
	}

	/* Contract the range to TT page boundaries. */
	adjust_offmask = pt_attr_leaf_table_offmask(pt_attr);
	adjusted_start = ((start + adjust_offmask) & ~adjust_offmask);
	adjusted_end = end & ~adjust_offmask;
	bool modified = false;

	/* Iterate over the range, trying to remove TTEs. */
	for (cur = adjusted_start; (cur < adjusted_end) && (cur >= adjusted_start); cur += pt_attr_twig_size(pt_attr)) {
		PMAP_LOCK(pmap);

		tte_p = pmap_tte(pmap, cur);

		if (tte_p == (tt_entry_t *) NULL) {
			goto done;
		}

		if ((*tte_p & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
			pte_p = (pt_entry_t *) ttetokv(*tte_p);

			if ((ptep_get_ptd(pte_p)->ptd_info[ARM_PT_DESC_INDEX(pte_p)].refcnt == 0) &&
			    (pmap != kernel_pmap)) {
				if (pmap->nested == TRUE) {
					/* Deallocate for the nested map. */
					pmap_tte_deallocate(pmap, tte_p, pt_attr_twig_level(pt_attr));
				} else {
					/* Just remove for the parent map. */
					pmap_tte_remove(pmap, tte_p, pt_attr_twig_level(pt_attr));
				}

				pmap_get_pt_ops(pmap)->flush_tlb_tte_async(cur, pmap);
				modified = true;
			}
		}

done:
		PMAP_UNLOCK(pmap);
	}

	if (modified) {
		sync_tlb_flush();
	}

#if (__ARM_VMSA__ > 7)
	/* Remove empty L2 TTs. */
	adjusted_start = ((start + ARM_TT_L1_OFFMASK) & ~ARM_TT_L1_OFFMASK);
	adjusted_end = end & ~ARM_TT_L1_OFFMASK;

	for (cur = adjusted_start; (cur < adjusted_end) && (cur >= adjusted_start); cur += ARM_TT_L1_SIZE) {
		/* For each L1 entry in our range... */
		PMAP_LOCK(pmap);

		bool remove_tt1e = true;
		tt_entry_t * tt1e_p = pmap_tt1e(pmap, cur);
		tt_entry_t * tt2e_start;
		tt_entry_t * tt2e_end;
		tt_entry_t * tt2e_p;
		tt_entry_t tt1e;

		if (tt1e_p == NULL) {
			PMAP_UNLOCK(pmap);
			continue;
		}

		tt1e = *tt1e_p;

		if (tt1e == ARM_TTE_TYPE_FAULT) {
			PMAP_UNLOCK(pmap);
			continue;
		}

		tt2e_start = &((tt_entry_t*) phystokv(tt1e & ARM_TTE_TABLE_MASK))[0];
		tt2e_end = &tt2e_start[TTE_PGENTRIES];

		for (tt2e_p = tt2e_start; tt2e_p < tt2e_end; tt2e_p++) {
			if (*tt2e_p != ARM_TTE_TYPE_FAULT) {
				/*
				 * If any TTEs are populated, don't remove the
				 * L1 TT.
				 */
				remove_tt1e = false;
			}
		}

		if (remove_tt1e) {
			pmap_tte_deallocate(pmap, tt1e_p, PMAP_TT_L1_LEVEL);
			PMAP_UPDATE_TLBS(pmap, cur, cur + PAGE_SIZE, false);
		}

		PMAP_UNLOCK(pmap);
	}
#endif /* (__ARM_VMSA__ > 7) */
}

/*
 * pmap_trim_internal(grand, subord, vstart, nstart, size)
 *
 * grand  = pmap subord is nested in
 * subord = nested pmap
 * vstart = start of the used range in grand
 * nstart = start of the used range in nstart
 * size   = size of the used range
 *
 * Attempts to trim the shared region page tables down to only cover the given
 * range in subord and grand.
 */
MARK_AS_PMAP_TEXT static void
pmap_trim_internal(
	pmap_t grand,
	pmap_t subord,
	addr64_t vstart,
	addr64_t nstart,
	uint64_t size)
{
	addr64_t vend, nend;
	addr64_t adjust_offmask;

	if (__improbable(os_add_overflow(vstart, size, &vend))) {
		panic("%s: grand addr wraps around, "
		    "grand=%p, subord=%p, vstart=%p, nstart=%p, size=%#llx",
		    __func__, grand, subord, (void*)vstart, (void*)nstart, size);
	}

	if (__improbable(os_add_overflow(nstart, size, &nend))) {
		panic("%s: nested addr wraps around, "
		    "grand=%p, subord=%p, vstart=%p, nstart=%p, size=%#llx",
		    __func__, grand, subord, (void*)vstart, (void*)nstart, size);
	}

	VALIDATE_PMAP(grand);
	VALIDATE_PMAP(subord);

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(grand);

	PMAP_LOCK(subord);

	if (!subord->nested) {
		panic("%s: subord is not nestable, "
		    "grand=%p, subord=%p, vstart=%p, nstart=%p, size=%#llx",
		    __func__, grand, subord, (void*)vstart, (void*)nstart, size);
	}

	if (grand->nested) {
		panic("%s: grand is nestable, "
		    "grand=%p, subord=%p, vstart=%p, nstart=%p, size=%#llx",
		    __func__, grand, subord, (void*)vstart, (void*)nstart, size);
	}

	if (grand->nested_pmap != subord) {
		panic("%s: grand->nested != subord, "
		    "grand=%p, subord=%p, vstart=%p, nstart=%p, size=%#llx",
		    __func__, grand, subord, (void*)vstart, (void*)nstart, size);
	}

	if (size != 0) {
		if ((vstart < grand->nested_region_grand_addr) || (vend > (grand->nested_region_grand_addr + grand->nested_region_size))) {
			panic("%s: grand range not in nested region, "
			    "grand=%p, subord=%p, vstart=%p, nstart=%p, size=%#llx",
			    __func__, grand, subord, (void*)vstart, (void*)nstart, size);
		}

		if ((nstart < grand->nested_region_grand_addr) || (nend > (grand->nested_region_grand_addr + grand->nested_region_size))) {
			panic("%s: subord range not in nested region, "
			    "grand=%p, subord=%p, vstart=%p, nstart=%p, size=%#llx",
			    __func__, grand, subord, (void*)vstart, (void*)nstart, size);
		}
	}


	if (!grand->nested_has_no_bounds_ref) {
		assert(subord->nested_bounds_set);

		if (!grand->nested_bounds_set) {
			/* Inherit the bounds from subord. */
			grand->nested_region_true_start = (subord->nested_region_true_start - grand->nested_region_subord_addr) + grand->nested_region_grand_addr;
			grand->nested_region_true_end = (subord->nested_region_true_end - grand->nested_region_subord_addr) + grand->nested_region_grand_addr;
			grand->nested_bounds_set = true;
		}

		PMAP_UNLOCK(subord);
		return;
	}

	if ((!subord->nested_bounds_set) && size) {
		adjust_offmask = pt_attr_leaf_table_offmask(pt_attr);

		subord->nested_region_true_start = nstart;
		subord->nested_region_true_end = nend;
		subord->nested_region_true_start &= ~adjust_offmask;

		if (__improbable(os_add_overflow(subord->nested_region_true_end, adjust_offmask, &subord->nested_region_true_end))) {
			panic("%s: padded true end wraps around, "
			    "grand=%p, subord=%p, vstart=%p, nstart=%p, size=%#llx",
			    __func__, grand, subord, (void*)vstart, (void*)nstart, size);
		}

		subord->nested_region_true_end &= ~adjust_offmask;
		subord->nested_bounds_set = true;
	}

	if (subord->nested_bounds_set) {
		/* Inherit the bounds from subord. */
		grand->nested_region_true_start = (subord->nested_region_true_start - grand->nested_region_subord_addr) + grand->nested_region_grand_addr;
		grand->nested_region_true_end = (subord->nested_region_true_end - grand->nested_region_subord_addr) + grand->nested_region_grand_addr;
		grand->nested_bounds_set = true;

		/* If we know the bounds, we can trim the pmap. */
		grand->nested_has_no_bounds_ref = false;
		PMAP_UNLOCK(subord);
	} else {
		/* Don't trim if we don't know the bounds. */
		PMAP_UNLOCK(subord);
		return;
	}

	/* Trim grand to only cover the given range. */
	pmap_trim_range(grand, grand->nested_region_grand_addr, grand->nested_region_true_start);
	pmap_trim_range(grand, grand->nested_region_true_end, (grand->nested_region_grand_addr + grand->nested_region_size));

	/* Try to trim subord. */
	pmap_trim_subord(subord);
}

MARK_AS_PMAP_TEXT static void
pmap_trim_self(pmap_t pmap)
{
	if (pmap->nested_has_no_bounds_ref && pmap->nested_pmap) {
		/* If we have a no bounds ref, we need to drop it. */
		PMAP_LOCK(pmap->nested_pmap);
		pmap->nested_has_no_bounds_ref = false;
		boolean_t nested_bounds_set = pmap->nested_pmap->nested_bounds_set;
		vm_map_offset_t nested_region_true_start = (pmap->nested_pmap->nested_region_true_start - pmap->nested_region_subord_addr) + pmap->nested_region_grand_addr;
		vm_map_offset_t nested_region_true_end = (pmap->nested_pmap->nested_region_true_end - pmap->nested_region_subord_addr) + pmap->nested_region_grand_addr;
		PMAP_UNLOCK(pmap->nested_pmap);

		if (nested_bounds_set) {
			pmap_trim_range(pmap, pmap->nested_region_grand_addr, nested_region_true_start);
			pmap_trim_range(pmap, nested_region_true_end, (pmap->nested_region_grand_addr + pmap->nested_region_size));
		}
		/*
		 * Try trimming the nested pmap, in case we had the
		 * last reference.
		 */
		pmap_trim_subord(pmap->nested_pmap);
	}
}

/*
 * pmap_trim_subord(grand, subord)
 *
 * grand  = pmap that we have nested subord in
 * subord = nested pmap we are attempting to trim
 *
 * Trims subord if possible
 */
MARK_AS_PMAP_TEXT static void
pmap_trim_subord(pmap_t subord)
{
	bool contract_subord = false;

	PMAP_LOCK(subord);

	subord->nested_no_bounds_refcnt--;

	if ((subord->nested_no_bounds_refcnt == 0) && (subord->nested_bounds_set)) {
		/* If this was the last no bounds reference, trim subord. */
		contract_subord = true;
	}

	PMAP_UNLOCK(subord);

	if (contract_subord) {
		pmap_trim_range(subord, subord->nested_region_subord_addr, subord->nested_region_true_start);
		pmap_trim_range(subord, subord->nested_region_true_end, subord->nested_region_subord_addr + subord->nested_region_size);
	}
}

void
pmap_trim(
	pmap_t grand,
	pmap_t subord,
	addr64_t vstart,
	addr64_t nstart,
	uint64_t size)
{
#if XNU_MONITOR
	pmap_trim_ppl(grand, subord, vstart, nstart, size);

	pmap_ledger_check_balance(grand);
	pmap_ledger_check_balance(subord);
#else
	pmap_trim_internal(grand, subord, vstart, nstart, size);
#endif
}

#if HAS_APPLE_PAC && XNU_MONITOR
static void *
pmap_sign_user_ptr_internal(void *value, ptrauth_key key, uint64_t discriminator)
{
	void *res = NULL;
	boolean_t current_intr_state = ml_set_interrupts_enabled(FALSE);

	ml_set_kernelkey_enabled(FALSE);
	switch (key) {
	case ptrauth_key_asia:
		res = ptrauth_sign_unauthenticated(value, ptrauth_key_asia, discriminator);
		break;
	case ptrauth_key_asda:
		res = ptrauth_sign_unauthenticated(value, ptrauth_key_asda, discriminator);
		break;
	default:
		panic("attempt to sign user pointer without process independent key");
	}
	ml_set_kernelkey_enabled(TRUE);

	ml_set_interrupts_enabled(current_intr_state);

	return res;
}

void *
pmap_sign_user_ptr(void *value, ptrauth_key key, uint64_t discriminator)
{
	return pmap_sign_user_ptr_internal(value, key, discriminator);
}

static void *
pmap_auth_user_ptr_internal(void *value, ptrauth_key key, uint64_t discriminator)
{
	if ((key != ptrauth_key_asia) && (key != ptrauth_key_asda)) {
		panic("attempt to auth user pointer without process independent key");
	}

	void *res = NULL;
	boolean_t current_intr_state = ml_set_interrupts_enabled(FALSE);

	ml_set_kernelkey_enabled(FALSE);
	res = ml_auth_ptr_unchecked(value, key, discriminator);
	ml_set_kernelkey_enabled(TRUE);

	ml_set_interrupts_enabled(current_intr_state);

	return res;
}

void *
pmap_auth_user_ptr(void *value, ptrauth_key key, uint64_t discriminator)
{
	return pmap_auth_user_ptr_internal(value, key, discriminator);
}
#endif /* HAS_APPLE_PAC && XNU_MONITOR */

/*
 *	kern_return_t pmap_nest(grand, subord, vstart, size)
 *
 *	grand  = the pmap that we will nest subord into
 *	subord = the pmap that goes into the grand
 *	vstart  = start of range in pmap to be inserted
 *	nstart  = start of range in pmap nested pmap
 *	size   = Size of nest area (up to 16TB)
 *
 *	Inserts a pmap into another.  This is used to implement shared segments.
 *
 */

MARK_AS_PMAP_TEXT static kern_return_t
pmap_nest_internal(
	pmap_t grand,
	pmap_t subord,
	addr64_t vstart,
	addr64_t nstart,
	uint64_t size)
{
	kern_return_t kr = KERN_FAILURE;
	vm_map_offset_t vaddr, nvaddr;
	tt_entry_t     *stte_p;
	tt_entry_t     *gtte_p;
	unsigned int    i;
	unsigned int    num_tte;
	unsigned int    nested_region_asid_bitmap_size;
	unsigned int*   nested_region_asid_bitmap;
	int expand_options = 0;

	addr64_t vend, nend;
	if (__improbable(os_add_overflow(vstart, size, &vend))) {
		panic("%s: %p grand addr wraps around: 0x%llx + 0x%llx", __func__, grand, vstart, size);
	}
	if (__improbable(os_add_overflow(nstart, size, &nend))) {
		panic("%s: %p nested addr wraps around: 0x%llx + 0x%llx", __func__, subord, nstart, size);
	}

	VALIDATE_PMAP(grand);
	VALIDATE_PMAP(subord);

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(grand);
	assert(pmap_get_pt_attr(subord) == pt_attr);

#if XNU_MONITOR
	expand_options |= PMAP_TT_ALLOCATE_NOWAIT;
#endif

	if (((size | vstart | nstart) & (pt_attr_leaf_table_offmask(pt_attr))) != 0x0ULL) {
		panic("pmap_nest() pmap %p unaligned nesting request 0x%llx, 0x%llx, 0x%llx\n", grand, vstart, nstart, size);
	}

	if (!subord->nested) {
		panic("%s: subordinate pmap %p is not nestable", __func__, subord);
	}

	if ((grand->nested_pmap != PMAP_NULL) && (grand->nested_pmap != subord)) {
		panic("pmap_nest() pmap %p has a nested pmap\n", grand);
	}

	if (subord->nested_region_asid_bitmap == NULL) {
		nested_region_asid_bitmap_size  = (unsigned int)(size >> pt_attr_twig_shift(pt_attr)) / (sizeof(unsigned int) * NBBY);

#if XNU_MONITOR
		pmap_paddr_t pa = 0;

		if ((nested_region_asid_bitmap_size * sizeof(unsigned int)) > PAGE_SIZE) {
			panic("%s: nested_region_asid_bitmap_size=%u will not fit in a page, "
			    "grand=%p, subord=%p, vstart=0x%llx, nstart=0x%llx, size=%llx",
			    __FUNCTION__,
			    nested_region_asid_bitmap_size,
			    grand, subord, vstart, nstart, size);
		}

		kr = pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_ALLOCATE_NOWAIT);

		if (kr != KERN_SUCCESS) {
			return kr;
		}

		assert(pa);

		nested_region_asid_bitmap = (unsigned int *)phystokv(pa);
#else
		nested_region_asid_bitmap = kalloc(nested_region_asid_bitmap_size * sizeof(unsigned int));
#endif
		bzero(nested_region_asid_bitmap, nested_region_asid_bitmap_size * sizeof(unsigned int));

		PMAP_LOCK(subord);
		if (subord->nested_region_asid_bitmap == NULL) {
			subord->nested_region_asid_bitmap = nested_region_asid_bitmap;
			subord->nested_region_asid_bitmap_size = nested_region_asid_bitmap_size;
			subord->nested_region_subord_addr = nstart;
			subord->nested_region_size = (mach_vm_offset_t) size;
			nested_region_asid_bitmap = NULL;
		}
		PMAP_UNLOCK(subord);
		if (nested_region_asid_bitmap != NULL) {
#if XNU_MONITOR
			pmap_pages_free(kvtophys((vm_offset_t)nested_region_asid_bitmap), PAGE_SIZE);
#else
			kfree(nested_region_asid_bitmap, nested_region_asid_bitmap_size * sizeof(unsigned int));
#endif
		}
	}
	if ((subord->nested_region_subord_addr + subord->nested_region_size) < nend) {
		uint64_t        new_size;
		unsigned int    new_nested_region_asid_bitmap_size;
		unsigned int*   new_nested_region_asid_bitmap;

		nested_region_asid_bitmap = NULL;
		nested_region_asid_bitmap_size = 0;
		new_size =  nend - subord->nested_region_subord_addr;

		/* We explicitly add 1 to the bitmap allocation size in order to avoid issues with truncation. */
		new_nested_region_asid_bitmap_size  = (unsigned int)((new_size >> pt_attr_twig_shift(pt_attr)) / (sizeof(unsigned int) * NBBY)) + 1;

#if XNU_MONITOR
		pmap_paddr_t pa = 0;

		if ((new_nested_region_asid_bitmap_size * sizeof(unsigned int)) > PAGE_SIZE) {
			panic("%s: new_nested_region_asid_bitmap_size=%u will not fit in a page, "
			    "grand=%p, subord=%p, vstart=0x%llx, nstart=0x%llx, size=%llx",
			    __FUNCTION__,
			    new_nested_region_asid_bitmap_size,
			    grand, subord, vstart, nstart, size);
		}

		kr = pmap_pages_alloc(&pa, PAGE_SIZE, PMAP_PAGES_ALLOCATE_NOWAIT);

		if (kr != KERN_SUCCESS) {
			return kr;
		}

		assert(pa);

		new_nested_region_asid_bitmap = (unsigned int *)phystokv(pa);
#else
		new_nested_region_asid_bitmap = kalloc(new_nested_region_asid_bitmap_size * sizeof(unsigned int));
#endif
		PMAP_LOCK(subord);
		if (subord->nested_region_size < new_size) {
			bzero(new_nested_region_asid_bitmap, new_nested_region_asid_bitmap_size * sizeof(unsigned int));
			bcopy(subord->nested_region_asid_bitmap, new_nested_region_asid_bitmap, subord->nested_region_asid_bitmap_size);
			nested_region_asid_bitmap_size  = subord->nested_region_asid_bitmap_size;
			nested_region_asid_bitmap = subord->nested_region_asid_bitmap;
			subord->nested_region_asid_bitmap = new_nested_region_asid_bitmap;
			subord->nested_region_asid_bitmap_size = new_nested_region_asid_bitmap_size;
			subord->nested_region_size = new_size;
			new_nested_region_asid_bitmap = NULL;
		}
		PMAP_UNLOCK(subord);
		if (nested_region_asid_bitmap != NULL)
#if XNU_MONITOR
		{pmap_pages_free(kvtophys((vm_offset_t)nested_region_asid_bitmap), PAGE_SIZE);}
#else
		{ kfree(nested_region_asid_bitmap, nested_region_asid_bitmap_size * sizeof(unsigned int));}
#endif
		if (new_nested_region_asid_bitmap != NULL)
#if XNU_MONITOR
		{pmap_pages_free(kvtophys((vm_offset_t)new_nested_region_asid_bitmap), PAGE_SIZE);}
#else
		{ kfree(new_nested_region_asid_bitmap, new_nested_region_asid_bitmap_size * sizeof(unsigned int));}
#endif
	}

	PMAP_LOCK(subord);
	if (grand->nested_pmap == PMAP_NULL) {
		grand->nested_pmap = subord;

		if (!subord->nested_bounds_set) {
			/*
			 * We are nesting without the shared regions bounds
			 * being known.  We'll have to trim the pmap later.
			 */
			grand->nested_has_no_bounds_ref = true;
			subord->nested_no_bounds_refcnt++;
		}

		grand->nested_region_grand_addr = vstart;
		grand->nested_region_subord_addr = nstart;
		grand->nested_region_size = (mach_vm_offset_t) size;
	} else {
		if ((grand->nested_region_grand_addr > vstart)) {
			panic("pmap_nest() pmap %p : attempt to nest outside the nested region\n", grand);
		} else if ((grand->nested_region_grand_addr + grand->nested_region_size) < vend) {
			grand->nested_region_size = (mach_vm_offset_t)(vstart - grand->nested_region_grand_addr + size);
		}
	}

#if     (__ARM_VMSA__ == 7)
	nvaddr = (vm_map_offset_t) nstart;
	vaddr = (vm_map_offset_t) vstart;
	num_tte = size >> ARM_TT_L1_SHIFT;

	for (i = 0; i < num_tte; i++) {
		if (((subord->nested_region_true_start) > nvaddr) || ((subord->nested_region_true_end) <= nvaddr)) {
			goto expand_next;
		}

		stte_p = pmap_tte(subord, nvaddr);
		if ((stte_p == (tt_entry_t *)NULL) || (((*stte_p) & ARM_TTE_TYPE_MASK) != ARM_TTE_TYPE_TABLE)) {
			PMAP_UNLOCK(subord);
			kr = pmap_expand(subord, nvaddr, expand_options, PMAP_TT_L2_LEVEL);

			if (kr != KERN_SUCCESS) {
				PMAP_LOCK(grand);
				goto done;
			}

			PMAP_LOCK(subord);
		}
		PMAP_UNLOCK(subord);
		PMAP_LOCK(grand);
		stte_p = pmap_tte(grand, vaddr);
		if (stte_p == (tt_entry_t *)NULL) {
			PMAP_UNLOCK(grand);
			kr = pmap_expand(grand, vaddr, expand_options, PMAP_TT_L1_LEVEL);

			if (kr != KERN_SUCCESS) {
				PMAP_LOCK(grand);
				goto done;
			}
		} else {
			PMAP_UNLOCK(grand);
			kr = KERN_SUCCESS;
		}
		PMAP_LOCK(subord);

expand_next:
		nvaddr += ARM_TT_L1_SIZE;
		vaddr += ARM_TT_L1_SIZE;
	}

#else
	nvaddr = (vm_map_offset_t) nstart;
	num_tte = (unsigned int)(size >> pt_attr_twig_shift(pt_attr));

	for (i = 0; i < num_tte; i++) {
		if (((subord->nested_region_true_start) > nvaddr) || ((subord->nested_region_true_end) <= nvaddr)) {
			goto expand_next;
		}

		stte_p = pmap_tte(subord, nvaddr);
		if (stte_p == PT_ENTRY_NULL || *stte_p == ARM_TTE_EMPTY) {
			PMAP_UNLOCK(subord);
			kr = pmap_expand(subord, nvaddr, expand_options, PMAP_TT_LEAF_LEVEL);

			if (kr != KERN_SUCCESS) {
				PMAP_LOCK(grand);
				goto done;
			}

			PMAP_LOCK(subord);
		}
expand_next:
		nvaddr += pt_attr_twig_size(pt_attr);
	}
#endif
	PMAP_UNLOCK(subord);

	/*
	 * copy tte's from subord pmap into grand pmap
	 */

	PMAP_LOCK(grand);
	nvaddr = (vm_map_offset_t) nstart;
	vaddr = (vm_map_offset_t) vstart;


#if     (__ARM_VMSA__ == 7)
	for (i = 0; i < num_tte; i++) {
		if (((subord->nested_region_true_start) > nvaddr) || ((subord->nested_region_true_end) <= nvaddr)) {
			goto nest_next;
		}

		stte_p = pmap_tte(subord, nvaddr);
		gtte_p = pmap_tte(grand, vaddr);
		*gtte_p = *stte_p;

nest_next:
		nvaddr += ARM_TT_L1_SIZE;
		vaddr += ARM_TT_L1_SIZE;
	}
#else
	for (i = 0; i < num_tte; i++) {
		if (((subord->nested_region_true_start) > nvaddr) || ((subord->nested_region_true_end) <= nvaddr)) {
			goto nest_next;
		}

		stte_p = pmap_tte(subord, nvaddr);
		gtte_p = pmap_tte(grand, vaddr);
		if (gtte_p == PT_ENTRY_NULL) {
			PMAP_UNLOCK(grand);
			kr = pmap_expand(grand, vaddr, expand_options, PMAP_TT_TWIG_LEVEL);
			PMAP_LOCK(grand);

			if (kr != KERN_SUCCESS) {
				goto done;
			}

			gtte_p = pmap_tt2e(grand, vaddr);
		}
		*gtte_p = *stte_p;

nest_next:
		vaddr += pt_attr_twig_size(pt_attr);
		nvaddr += pt_attr_twig_size(pt_attr);
	}
#endif

	kr = KERN_SUCCESS;
done:

	stte_p = pmap_tte(grand, vstart);
	FLUSH_PTE_RANGE_STRONG(stte_p, stte_p + num_tte);

#if     (__ARM_VMSA__ > 7)
	/*
	 * check for overflow on LP64 arch
	 */
	assert((size & 0xFFFFFFFF00000000ULL) == 0);
#endif
	PMAP_UPDATE_TLBS(grand, vstart, vend, false);

	PMAP_UNLOCK(grand);
	return kr;
}

kern_return_t
pmap_nest(
	pmap_t grand,
	pmap_t subord,
	addr64_t vstart,
	addr64_t nstart,
	uint64_t size)
{
	kern_return_t kr = KERN_FAILURE;

	PMAP_TRACE(2, PMAP_CODE(PMAP__NEST) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDE(grand), VM_KERNEL_ADDRHIDE(subord),
	    VM_KERNEL_ADDRHIDE(vstart));

#if XNU_MONITOR
	while ((kr = pmap_nest_ppl(grand, subord, vstart, nstart, size)) == KERN_RESOURCE_SHORTAGE) {
		pmap_alloc_page_for_ppl();
	}

	pmap_ledger_check_balance(grand);
	pmap_ledger_check_balance(subord);
#else
	kr = pmap_nest_internal(grand, subord, vstart, nstart, size);
#endif

	PMAP_TRACE(2, PMAP_CODE(PMAP__NEST) | DBG_FUNC_END, kr);

	return kr;
}

/*
 *	kern_return_t pmap_unnest(grand, vaddr)
 *
 *	grand  = the pmap that will have the virtual range unnested
 *	vaddr  = start of range in pmap to be unnested
 *	size   = size of range in pmap to be unnested
 *
 */

kern_return_t
pmap_unnest(
	pmap_t grand,
	addr64_t vaddr,
	uint64_t size)
{
	return pmap_unnest_options(grand, vaddr, size, 0);
}

MARK_AS_PMAP_TEXT static kern_return_t
pmap_unnest_options_internal(
	pmap_t grand,
	addr64_t vaddr,
	uint64_t size,
	unsigned int option)
{
	vm_map_offset_t start;
	vm_map_offset_t addr;
	tt_entry_t     *tte_p;
	unsigned int    current_index;
	unsigned int    start_index;
	unsigned int    max_index;
	unsigned int    num_tte;
	unsigned int    i;

	addr64_t vend;
	if (__improbable(os_add_overflow(vaddr, size, &vend))) {
		panic("%s: %p vaddr wraps around: 0x%llx + 0x%llx", __func__, grand, vaddr, size);
	}

	VALIDATE_PMAP(grand);

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(grand);

	if (((size | vaddr) & pt_attr_twig_offmask(pt_attr)) != 0x0ULL) {
		panic("pmap_unnest(): unaligned request");
	}

	if ((option & PMAP_UNNEST_CLEAN) == 0) {
		if (grand->nested_pmap == NULL) {
			panic("%s: %p has no nested pmap", __func__, grand);
		}

		if ((vaddr < grand->nested_region_grand_addr) || (vend > (grand->nested_region_grand_addr + grand->nested_region_size))) {
			panic("%s: %p: unnest request to region not-fully-nested region [%p, %p)", __func__, grand, (void*)vaddr, (void*)vend);
		}

		PMAP_LOCK(grand->nested_pmap);

		start = vaddr - grand->nested_region_grand_addr + grand->nested_region_subord_addr;
		start_index = (unsigned int)((vaddr - grand->nested_region_grand_addr)  >> pt_attr_twig_shift(pt_attr));
		max_index = (unsigned int)(start_index + (size >> pt_attr_twig_shift(pt_attr)));
		num_tte = (unsigned int)(size >> pt_attr_twig_shift(pt_attr));

		for (current_index = start_index, addr = start; current_index < max_index; current_index++, addr += pt_attr_twig_size(pt_attr)) {
			pt_entry_t  *bpte, *epte, *cpte;

			if (addr < grand->nested_pmap->nested_region_true_start) {
				/* We haven't reached the interesting range. */
				continue;
			}

			if (addr >= grand->nested_pmap->nested_region_true_end) {
				/* We're done with the interesting range. */
				break;
			}

			bpte = pmap_pte(grand->nested_pmap, addr);
			epte = bpte + (pt_attr_leaf_index_mask(pt_attr) >> pt_attr_leaf_shift(pt_attr));

			if (!testbit(current_index, (int *)grand->nested_pmap->nested_region_asid_bitmap)) {
				setbit(current_index, (int *)grand->nested_pmap->nested_region_asid_bitmap);

				for (cpte = bpte; cpte <= epte; cpte++) {
					pmap_paddr_t    pa;
					int                             pai = 0;
					boolean_t               managed = FALSE;
					pt_entry_t  spte;

					if ((*cpte != ARM_PTE_TYPE_FAULT)
					    && (!ARM_PTE_IS_COMPRESSED(*cpte, cpte))) {
						spte = *cpte;
						while (!managed) {
							pa = pte_to_pa(spte);
							if (!pa_valid(pa)) {
								break;
							}
							pai = (int)pa_index(pa);
							LOCK_PVH(pai);
							spte = *cpte;
							pa = pte_to_pa(spte);
							if (pai == (int)pa_index(pa)) {
								managed = TRUE;
								break; // Leave the PVH locked as we'll unlock it after we update the PTE
							}
							UNLOCK_PVH(pai);
						}

						if (((spte & ARM_PTE_NG) != ARM_PTE_NG)) {
							WRITE_PTE_FAST(cpte, (spte | ARM_PTE_NG));
						}

						if (managed) {
							ASSERT_PVH_LOCKED(pai);
							UNLOCK_PVH(pai);
						}
					}
				}
			}

			FLUSH_PTE_RANGE_STRONG(bpte, epte);
			flush_mmu_tlb_region_asid_async(start, (unsigned)size, grand->nested_pmap);
		}

		sync_tlb_flush();

		PMAP_UNLOCK(grand->nested_pmap);
	}

	PMAP_LOCK(grand);

	/*
	 * invalidate all pdes for segment at vaddr in pmap grand
	 */
	start = vaddr;
	addr = vaddr;

	num_tte = (unsigned int)(size >> pt_attr_twig_shift(pt_attr));

	for (i = 0; i < num_tte; i++, addr += pt_attr_twig_size(pt_attr)) {
		if (addr < grand->nested_pmap->nested_region_true_start) {
			/* We haven't reached the interesting range. */
			continue;
		}

		if (addr >= grand->nested_pmap->nested_region_true_end) {
			/* We're done with the interesting range. */
			break;
		}

		tte_p = pmap_tte(grand, addr);
		*tte_p = ARM_TTE_TYPE_FAULT;
	}

	tte_p = pmap_tte(grand, start);
	FLUSH_PTE_RANGE_STRONG(tte_p, tte_p + num_tte);
	PMAP_UPDATE_TLBS(grand, start, vend, false);

	PMAP_UNLOCK(grand);

	return KERN_SUCCESS;
}

kern_return_t
pmap_unnest_options(
	pmap_t grand,
	addr64_t vaddr,
	uint64_t size,
	unsigned int option)
{
	kern_return_t kr = KERN_FAILURE;

	PMAP_TRACE(2, PMAP_CODE(PMAP__UNNEST) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDE(grand), VM_KERNEL_ADDRHIDE(vaddr));

#if XNU_MONITOR
	kr = pmap_unnest_options_ppl(grand, vaddr, size, option);
#else
	kr = pmap_unnest_options_internal(grand, vaddr, size, option);
#endif

	PMAP_TRACE(2, PMAP_CODE(PMAP__UNNEST) | DBG_FUNC_END, kr);

	return kr;
}

boolean_t
pmap_adjust_unnest_parameters(
	__unused pmap_t p,
	__unused vm_map_offset_t *s,
	__unused vm_map_offset_t *e)
{
	return TRUE; /* to get to log_unnest_badness()... */
}

/*
 * disable no-execute capability on
 * the specified pmap
 */
#if DEVELOPMENT || DEBUG
void
pmap_disable_NX(
	pmap_t pmap)
{
	pmap->nx_enabled = FALSE;
}
#else
void
pmap_disable_NX(
	__unused pmap_t pmap)
{
}
#endif

void
pt_fake_zone_init(
	int zone_index)
{
	pt_fake_zone_index = zone_index;
}

void
pt_fake_zone_info(
	int *count,
	vm_size_t *cur_size, vm_size_t *max_size, vm_size_t *elem_size, vm_size_t *alloc_size,
	uint64_t *sum_size, int *collectable, int *exhaustable, int *caller_acct)
{
	*count      = inuse_pmap_pages_count;
	*cur_size   = PAGE_SIZE * (inuse_pmap_pages_count);
	*max_size   = PAGE_SIZE * (inuse_pmap_pages_count + vm_page_inactive_count + vm_page_active_count + vm_page_free_count);
	*elem_size  = PAGE_SIZE;
	*alloc_size = PAGE_SIZE;
	*sum_size   = (alloc_pmap_pages_count) * PAGE_SIZE;

	*collectable = 1;
	*exhaustable = 0;
	*caller_acct = 1;
}

/*
 * flush a range of hardware TLB entries.
 * NOTE: assumes the smallest TLB entry in use will be for
 * an ARM small page (4K).
 */

#define ARM_FULL_TLB_FLUSH_THRESHOLD 64

#if __ARM_RANGE_TLBI__
#define ARM64_RANGE_TLB_FLUSH_THRESHOLD 1
#define ARM64_FULL_TLB_FLUSH_THRESHOLD  ARM64_16K_TLB_RANGE_PAGES
#else
#define ARM64_FULL_TLB_FLUSH_THRESHOLD  256
#endif // __ARM_RANGE_TLBI__

static void
flush_mmu_tlb_region_asid_async(
	vm_offset_t va,
	unsigned length,
	pmap_t pmap)
{
#if     (__ARM_VMSA__ == 7)
	vm_offset_t     end = va + length;
	uint32_t        asid;

	asid = pmap->hw_asid;

	if (length / ARM_SMALL_PAGE_SIZE > ARM_FULL_TLB_FLUSH_THRESHOLD) {
		boolean_t       flush_all = FALSE;

		if ((asid == 0) || (pmap->nested == TRUE)) {
			flush_all = TRUE;
		}
		if (flush_all) {
			flush_mmu_tlb_async();
		} else {
			flush_mmu_tlb_asid_async(asid);
		}

		return;
	}
	if (pmap->nested == TRUE) {
#if     !__ARM_MP_EXT__
		flush_mmu_tlb();
#else
		va = arm_trunc_page(va);
		while (va < end) {
			flush_mmu_tlb_mva_entries_async(va);
			va += ARM_SMALL_PAGE_SIZE;
		}
#endif
		return;
	}
	va = arm_trunc_page(va) | (asid & 0xff);
	flush_mmu_tlb_entries_async(va, end);

#else
	unsigned    npages = length >> pt_attr_leaf_shift(pmap_get_pt_attr(pmap));
	uint32_t    asid;

	asid = pmap->hw_asid;

	if (npages > ARM64_FULL_TLB_FLUSH_THRESHOLD) {
		boolean_t       flush_all = FALSE;

		if ((asid == 0) || (pmap->nested == TRUE)) {
			flush_all = TRUE;
		}
		if (flush_all) {
			flush_mmu_tlb_async();
		} else {
			flush_mmu_tlb_asid_async((uint64_t)asid << TLBI_ASID_SHIFT);
		}
		return;
	}
#if __ARM_RANGE_TLBI__
	if (npages > ARM64_RANGE_TLB_FLUSH_THRESHOLD) {
		va = generate_rtlbi_param(npages, asid, va);
		if (pmap->nested == TRUE) {
			flush_mmu_tlb_allrange_async(va);
		} else {
			flush_mmu_tlb_range_async(va);
		}
		return;
	}
#endif
	vm_offset_t end = tlbi_asid(asid) | tlbi_addr(va + length);
	va = tlbi_asid(asid) | tlbi_addr(va);
	if (pmap->nested == TRUE) {
		flush_mmu_tlb_allentries_async(va, end);
	} else {
		flush_mmu_tlb_entries_async(va, end);
	}

#endif
}

MARK_AS_PMAP_TEXT static void
flush_mmu_tlb_tte_asid_async(vm_offset_t va, pmap_t pmap)
{
#if     (__ARM_VMSA__ == 7)
	flush_mmu_tlb_entry_async((va & ~ARM_TT_L1_PT_OFFMASK) | (pmap->hw_asid & 0xff));
	flush_mmu_tlb_entry_async(((va & ~ARM_TT_L1_PT_OFFMASK) + ARM_TT_L1_SIZE) | (pmap->hw_asid & 0xff));
	flush_mmu_tlb_entry_async(((va & ~ARM_TT_L1_PT_OFFMASK) + 2 * ARM_TT_L1_SIZE) | (pmap->hw_asid & 0xff));
	flush_mmu_tlb_entry_async(((va & ~ARM_TT_L1_PT_OFFMASK) + 3 * ARM_TT_L1_SIZE) | (pmap->hw_asid & 0xff));
#else
	flush_mmu_tlb_entry_async(tlbi_addr(va & ~pt_attr_twig_offmask(pmap_get_pt_attr(pmap))) | tlbi_asid(pmap->hw_asid));
#endif
}

MARK_AS_PMAP_TEXT static void
flush_mmu_tlb_full_asid_async(pmap_t pmap)
{
#if (__ARM_VMSA__ == 7)
	flush_mmu_tlb_asid_async(pmap->hw_asid);
#else /* (__ARM_VMSA__ == 7) */
	flush_mmu_tlb_asid_async((uint64_t)(pmap->hw_asid) << TLBI_ASID_SHIFT);
#endif /* (__ARM_VMSA__ == 7) */
}

void
flush_mmu_tlb_region(
	vm_offset_t va,
	unsigned length)
{
	flush_mmu_tlb_region_asid_async(va, length, kernel_pmap);
	sync_tlb_flush();
}

static pmap_io_range_t*
pmap_find_io_attr(pmap_paddr_t paddr)
{
	pmap_io_range_t find_range = {.addr = paddr & ~PAGE_MASK, .len = PAGE_SIZE};
	unsigned int begin = 0, end = num_io_rgns - 1;
	if ((num_io_rgns == 0) || (paddr < io_attr_table[begin].addr) ||
	    (paddr >= (io_attr_table[end].addr + io_attr_table[end].len))) {
		return NULL;
	}

	for (;;) {
		unsigned int middle = (begin + end) / 2;
		int cmp = cmp_io_rgns(&find_range, &io_attr_table[middle]);
		if (cmp == 0) {
			return &io_attr_table[middle];
		} else if (begin == end) {
			break;
		} else if (cmp > 0) {
			begin = middle + 1;
		} else {
			end = middle;
		}
	}

	return NULL;
}

unsigned int
pmap_cache_attributes(
	ppnum_t pn)
{
	pmap_paddr_t    paddr;
	int             pai;
	unsigned int    result;
	pp_attr_t       pp_attr_current;

	paddr = ptoa(pn);

	assert(vm_last_phys > vm_first_phys); // Check that pmap has been bootstrapped

	if (!pa_valid(paddr)) {
		pmap_io_range_t *io_rgn = pmap_find_io_attr(paddr);
		return (io_rgn == NULL) ? VM_WIMG_IO : io_rgn->wimg;
	}

	result = VM_WIMG_DEFAULT;

	pai = (int)pa_index(paddr);

	pp_attr_current = pp_attr_table[pai];
	if (pp_attr_current & PP_ATTR_WIMG_MASK) {
		result = pp_attr_current & PP_ATTR_WIMG_MASK;
	}
	return result;
}

MARK_AS_PMAP_TEXT static void
pmap_sync_wimg(ppnum_t pn, unsigned int wimg_bits_prev, unsigned int wimg_bits_new)
{
	if ((wimg_bits_prev != wimg_bits_new)
	    && ((wimg_bits_prev == VM_WIMG_COPYBACK)
	    || ((wimg_bits_prev == VM_WIMG_INNERWBACK)
	    && (wimg_bits_new != VM_WIMG_COPYBACK))
	    || ((wimg_bits_prev == VM_WIMG_WTHRU)
	    && ((wimg_bits_new != VM_WIMG_COPYBACK) || (wimg_bits_new != VM_WIMG_INNERWBACK))))) {
		pmap_sync_page_attributes_phys(pn);
	}

	if ((wimg_bits_new == VM_WIMG_RT) && (wimg_bits_prev != VM_WIMG_RT)) {
		pmap_force_dcache_clean(phystokv(ptoa(pn)), PAGE_SIZE);
	}
}

MARK_AS_PMAP_TEXT static __unused void
pmap_update_compressor_page_internal(ppnum_t pn, unsigned int prev_cacheattr, unsigned int new_cacheattr)
{
	pmap_paddr_t paddr = ptoa(pn);
	int pai = (int)pa_index(paddr);

	if (__improbable(!pa_valid(paddr))) {
		panic("%s called on non-managed page 0x%08x", __func__, pn);
	}

	LOCK_PVH(pai);

#if XNU_MONITOR
	if (__improbable(pa_test_monitor(paddr))) {
		panic("%s invoked on PPL page 0x%08x", __func__, pn);
	}
#endif

	pmap_update_cache_attributes_locked(pn, new_cacheattr);

	UNLOCK_PVH(pai);

	pmap_sync_wimg(pn, prev_cacheattr & VM_WIMG_MASK, new_cacheattr & VM_WIMG_MASK);
}

void *
pmap_map_compressor_page(ppnum_t pn)
{
#if __ARM_PTE_PHYSMAP__
	unsigned int cacheattr = pmap_cache_attributes(pn) & VM_WIMG_MASK;
	if (cacheattr != VM_WIMG_DEFAULT) {
#if XNU_MONITOR
		pmap_update_compressor_page_ppl(pn, cacheattr, VM_WIMG_DEFAULT);
#else
		pmap_update_compressor_page_internal(pn, cacheattr, VM_WIMG_DEFAULT);
#endif
	}
#endif
	return (void*)phystokv(ptoa(pn));
}

void
pmap_unmap_compressor_page(ppnum_t pn __unused, void *kva __unused)
{
#if __ARM_PTE_PHYSMAP__
	unsigned int cacheattr = pmap_cache_attributes(pn) & VM_WIMG_MASK;
	if (cacheattr != VM_WIMG_DEFAULT) {
#if XNU_MONITOR
		pmap_update_compressor_page_ppl(pn, VM_WIMG_DEFAULT, cacheattr);
#else
		pmap_update_compressor_page_internal(pn, VM_WIMG_DEFAULT, cacheattr);
#endif
	}
#endif
}

MARK_AS_PMAP_TEXT static boolean_t
pmap_batch_set_cache_attributes_internal(
	ppnum_t pn,
	unsigned int cacheattr,
	unsigned int page_cnt,
	unsigned int page_index,
	boolean_t doit,
	unsigned int *res)
{
	pmap_paddr_t    paddr;
	int             pai;
	pp_attr_t       pp_attr_current;
	pp_attr_t       pp_attr_template;
	unsigned int    wimg_bits_prev, wimg_bits_new;

	if (cacheattr & VM_WIMG_USE_DEFAULT) {
		cacheattr = VM_WIMG_DEFAULT;
	}

	if ((doit == FALSE) && (*res == 0)) {
		pmap_pin_kernel_pages((vm_offset_t)res, sizeof(*res));
		*res = page_cnt;
		pmap_unpin_kernel_pages((vm_offset_t)res, sizeof(*res));
		if (platform_cache_batch_wimg(cacheattr & (VM_WIMG_MASK), page_cnt << PAGE_SHIFT) == FALSE) {
			return FALSE;
		}
	}

	paddr = ptoa(pn);

	if (!pa_valid(paddr)) {
		panic("pmap_batch_set_cache_attributes(): pn 0x%08x not managed", pn);
	}

	pai = (int)pa_index(paddr);

	if (doit) {
		LOCK_PVH(pai);
#if XNU_MONITOR
		if (pa_test_monitor(paddr)) {
			panic("%s invoked on PPL page 0x%llx", __func__, (uint64_t)paddr);
		}
#endif
	}

	do {
		pp_attr_current = pp_attr_table[pai];
		wimg_bits_prev = VM_WIMG_DEFAULT;
		if (pp_attr_current & PP_ATTR_WIMG_MASK) {
			wimg_bits_prev = pp_attr_current & PP_ATTR_WIMG_MASK;
		}

		pp_attr_template = (pp_attr_current & ~PP_ATTR_WIMG_MASK) | PP_ATTR_WIMG(cacheattr & (VM_WIMG_MASK));

		if (!doit) {
			break;
		}

		/* WIMG bits should only be updated under the PVH lock, but we should do this in a CAS loop
		 * to avoid losing simultaneous updates to other bits like refmod. */
	} while (!OSCompareAndSwap16(pp_attr_current, pp_attr_template, &pp_attr_table[pai]));

	wimg_bits_new = VM_WIMG_DEFAULT;
	if (pp_attr_template & PP_ATTR_WIMG_MASK) {
		wimg_bits_new = pp_attr_template & PP_ATTR_WIMG_MASK;
	}

	if (doit) {
		if (wimg_bits_new != wimg_bits_prev) {
			pmap_update_cache_attributes_locked(pn, cacheattr);
		}
		UNLOCK_PVH(pai);
		if ((wimg_bits_new == VM_WIMG_RT) && (wimg_bits_prev != VM_WIMG_RT)) {
			pmap_force_dcache_clean(phystokv(paddr), PAGE_SIZE);
		}
	} else {
		if (wimg_bits_new == VM_WIMG_COPYBACK) {
			return FALSE;
		}
		if (wimg_bits_prev == wimg_bits_new) {
			pmap_pin_kernel_pages((vm_offset_t)res, sizeof(*res));
			*res = *res - 1;
			pmap_unpin_kernel_pages((vm_offset_t)res, sizeof(*res));
			if (!platform_cache_batch_wimg(wimg_bits_new, (*res) << PAGE_SHIFT)) {
				return FALSE;
			}
		}
		return TRUE;
	}

	if (page_cnt == (page_index + 1)) {
		wimg_bits_prev = VM_WIMG_COPYBACK;
		if (((wimg_bits_prev != wimg_bits_new))
		    && ((wimg_bits_prev == VM_WIMG_COPYBACK)
		    || ((wimg_bits_prev == VM_WIMG_INNERWBACK)
		    && (wimg_bits_new != VM_WIMG_COPYBACK))
		    || ((wimg_bits_prev == VM_WIMG_WTHRU)
		    && ((wimg_bits_new != VM_WIMG_COPYBACK) || (wimg_bits_new != VM_WIMG_INNERWBACK))))) {
			platform_cache_flush_wimg(wimg_bits_new);
		}
	}

	return TRUE;
}

boolean_t
pmap_batch_set_cache_attributes(
	ppnum_t pn,
	unsigned int cacheattr,
	unsigned int page_cnt,
	unsigned int page_index,
	boolean_t doit,
	unsigned int *res)
{
#if XNU_MONITOR
	return pmap_batch_set_cache_attributes_ppl(pn, cacheattr, page_cnt, page_index, doit, res);
#else
	return pmap_batch_set_cache_attributes_internal(pn, cacheattr, page_cnt, page_index, doit, res);
#endif
}

MARK_AS_PMAP_TEXT static void
pmap_set_cache_attributes_priv(
	ppnum_t pn,
	unsigned int cacheattr,
	boolean_t external __unused)
{
	pmap_paddr_t    paddr;
	int             pai;
	pp_attr_t       pp_attr_current;
	pp_attr_t       pp_attr_template;
	unsigned int    wimg_bits_prev, wimg_bits_new;

	paddr = ptoa(pn);

	if (!pa_valid(paddr)) {
		return;                         /* Not a managed page. */
	}

	if (cacheattr & VM_WIMG_USE_DEFAULT) {
		cacheattr = VM_WIMG_DEFAULT;
	}

	pai = (int)pa_index(paddr);

	LOCK_PVH(pai);

#if XNU_MONITOR
	if (external && pa_test_monitor(paddr)) {
		panic("%s invoked on PPL page 0x%llx", __func__, (uint64_t)paddr);
	} else if (!external && !pa_test_monitor(paddr)) {
		panic("%s invoked on non-PPL page 0x%llx", __func__, (uint64_t)paddr);
	}
#endif

	do {
		pp_attr_current = pp_attr_table[pai];
		wimg_bits_prev = VM_WIMG_DEFAULT;
		if (pp_attr_current & PP_ATTR_WIMG_MASK) {
			wimg_bits_prev = pp_attr_current & PP_ATTR_WIMG_MASK;
		}

		pp_attr_template = (pp_attr_current & ~PP_ATTR_WIMG_MASK) | PP_ATTR_WIMG(cacheattr & (VM_WIMG_MASK));

		/* WIMG bits should only be updated under the PVH lock, but we should do this in a CAS loop
		 * to avoid losing simultaneous updates to other bits like refmod. */
	} while (!OSCompareAndSwap16(pp_attr_current, pp_attr_template, &pp_attr_table[pai]));

	wimg_bits_new = VM_WIMG_DEFAULT;
	if (pp_attr_template & PP_ATTR_WIMG_MASK) {
		wimg_bits_new = pp_attr_template & PP_ATTR_WIMG_MASK;
	}

	if (wimg_bits_new != wimg_bits_prev) {
		pmap_update_cache_attributes_locked(pn, cacheattr);
	}

	UNLOCK_PVH(pai);

	pmap_sync_wimg(pn, wimg_bits_prev, wimg_bits_new);
}

MARK_AS_PMAP_TEXT static void
pmap_set_cache_attributes_internal(
	ppnum_t pn,
	unsigned int cacheattr)
{
	pmap_set_cache_attributes_priv(pn, cacheattr, TRUE);
}

void
pmap_set_cache_attributes(
	ppnum_t pn,
	unsigned int cacheattr)
{
#if XNU_MONITOR
	pmap_set_cache_attributes_ppl(pn, cacheattr);
#else
	pmap_set_cache_attributes_internal(pn, cacheattr);
#endif
}

MARK_AS_PMAP_TEXT void
pmap_update_cache_attributes_locked(
	ppnum_t ppnum,
	unsigned attributes)
{
	pmap_paddr_t    phys = ptoa(ppnum);
	pv_entry_t      *pve_p;
	pt_entry_t      *pte_p;
	pv_entry_t      **pv_h;
	pt_entry_t      tmplate;
	unsigned int    pai;
	boolean_t       tlb_flush_needed = FALSE;

	PMAP_TRACE(2, PMAP_CODE(PMAP__UPDATE_CACHING) | DBG_FUNC_START, ppnum, attributes);

#if __ARM_PTE_PHYSMAP__
	vm_offset_t kva = phystokv(phys);
	pte_p = pmap_pte(kernel_pmap, kva);

	tmplate = *pte_p;
	tmplate &= ~(ARM_PTE_ATTRINDXMASK | ARM_PTE_SHMASK);
#if XNU_MONITOR
	tmplate |= (wimg_to_pte(attributes) & ~ARM_PTE_XPRR_MASK);
#else
	tmplate |= wimg_to_pte(attributes);
#endif
#if (__ARM_VMSA__ > 7)
	if (tmplate & ARM_PTE_HINT_MASK) {
		panic("%s: physical aperture PTE %p has hint bit set, va=%p, pte=0x%llx",
		    __FUNCTION__, pte_p, (void *)kva, tmplate);
	}
#endif
	WRITE_PTE_STRONG(pte_p, tmplate);
	flush_mmu_tlb_region_asid_async(kva, PAGE_SIZE, kernel_pmap);
	tlb_flush_needed = TRUE;
#endif

	pai = (unsigned int)pa_index(phys);

	pv_h = pai_to_pvh(pai);

	pte_p = PT_ENTRY_NULL;
	pve_p = PV_ENTRY_NULL;
	if (pvh_test_type(pv_h, PVH_TYPE_PTEP)) {
		pte_p = pvh_ptep(pv_h);
	} else if (pvh_test_type(pv_h, PVH_TYPE_PVEP)) {
		pve_p = pvh_list(pv_h);
		pte_p = PT_ENTRY_NULL;
	}

	while ((pve_p != PV_ENTRY_NULL) || (pte_p != PT_ENTRY_NULL)) {
		vm_map_address_t va;
		pmap_t          pmap;

		if (pve_p != PV_ENTRY_NULL) {
			pte_p = pve_get_ptep(pve_p);
		}
#ifdef PVH_FLAG_IOMMU
		if ((vm_offset_t)pte_p & PVH_FLAG_IOMMU) {
			goto cache_skip_pve;
		}
#endif
		pmap = ptep_get_pmap(pte_p);
		va = ptep_get_va(pte_p);

		tmplate = *pte_p;
		tmplate &= ~(ARM_PTE_ATTRINDXMASK | ARM_PTE_SHMASK);
		tmplate |= pmap_get_pt_ops(pmap)->wimg_to_pte(attributes);

		WRITE_PTE_STRONG(pte_p, tmplate);
		pmap_get_pt_ops(pmap)->flush_tlb_region_async(va, PAGE_SIZE, pmap);
		tlb_flush_needed = TRUE;

#ifdef PVH_FLAG_IOMMU
cache_skip_pve:
#endif
		pte_p = PT_ENTRY_NULL;
		if (pve_p != PV_ENTRY_NULL) {
			pve_p = PVE_NEXT_PTR(pve_next(pve_p));
		}
	}
	if (tlb_flush_needed) {
		sync_tlb_flush();
	}

	PMAP_TRACE(2, PMAP_CODE(PMAP__UPDATE_CACHING) | DBG_FUNC_END, ppnum, attributes);
}

#if     (__ARM_VMSA__ == 7)
vm_map_address_t
pmap_create_sharedpage(
	void)
{
	pmap_paddr_t    pa;
	kern_return_t   kr;

	(void) pmap_pages_alloc(&pa, PAGE_SIZE, 0);
	memset((char *) phystokv(pa), 0, PAGE_SIZE);

	kr = pmap_enter(kernel_pmap, _COMM_PAGE_BASE_ADDRESS, atop(pa), VM_PROT_READ | VM_PROT_WRITE, VM_PROT_NONE, VM_WIMG_USE_DEFAULT, TRUE);
	assert(kr == KERN_SUCCESS);

	return (vm_map_address_t)phystokv(pa);
}
#else
static void
pmap_update_tt3e(
	pmap_t pmap,
	vm_address_t address,
	tt_entry_t template)
{
	tt_entry_t *ptep, pte;

	ptep = pmap_tt3e(pmap, address);
	if (ptep == NULL) {
		panic("%s: no ptep?\n", __FUNCTION__);
	}

	pte = *ptep;
	pte = tte_to_pa(pte) | template;
	WRITE_PTE_STRONG(ptep, pte);
}

/* Note absence of non-global bit */
#define PMAP_COMM_PAGE_PTE_TEMPLATE (ARM_PTE_TYPE_VALID \
	        | ARM_PTE_ATTRINDX(CACHE_ATTRINDX_WRITEBACK) \
	        | ARM_PTE_SH(SH_INNER_MEMORY) | ARM_PTE_NX \
	        | ARM_PTE_PNX | ARM_PTE_AP(AP_RORO) | ARM_PTE_AF)

vm_map_address_t
pmap_create_sharedpage(
	void
	)
{
	kern_return_t   kr;
	pmap_paddr_t    pa = 0;

#if XNU_MONITOR
	pa = pmap_alloc_page_for_kern();
	assert(pa);
#else

	(void) pmap_pages_alloc(&pa, PAGE_SIZE, 0);
#endif

	memset((char *) phystokv(pa), 0, PAGE_SIZE);

#ifdef CONFIG_XNUPOST
	/*
	 * The kernel pmap maintains a user accessible mapping of the commpage
	 * to test PAN.
	 */
	kr = pmap_enter(kernel_pmap, _COMM_HIGH_PAGE64_BASE_ADDRESS, (ppnum_t)atop(pa), VM_PROT_READ, VM_PROT_NONE, VM_WIMG_USE_DEFAULT, TRUE);
	assert(kr == KERN_SUCCESS);

	/*
	 * This mapping should not be global (as we only expect to reference it
	 * during testing).
	 */
	pmap_update_tt3e(kernel_pmap, _COMM_HIGH_PAGE64_BASE_ADDRESS, PMAP_COMM_PAGE_PTE_TEMPLATE | ARM_PTE_NG);

#if KASAN
	kasan_map_shadow(_COMM_HIGH_PAGE64_BASE_ADDRESS, PAGE_SIZE, true);
#endif
#endif /* CONFIG_XNUPOST */

	/*
	 * In order to avoid burning extra pages on mapping the shared page, we
	 * create a dedicated pmap for the shared page.  We forcibly nest the
	 * translation tables from this pmap into other pmaps.  The level we
	 * will nest at depends on the MMU configuration (page size, TTBR range,
	 * etc).
	 *
	 * Note that this is NOT "the nested pmap" (which is used to nest the
	 * shared cache).
	 *
	 * Note that we update parameters of the entry for our unique needs (NG
	 * entry, etc.).
	 */
	sharedpage_pmap = pmap_create_options(NULL, 0x0, 0);
	assert(sharedpage_pmap != NULL);

	/* The user 64-bit mapping... */
	kr = pmap_enter(sharedpage_pmap, _COMM_PAGE64_BASE_ADDRESS, (ppnum_t)atop(pa), VM_PROT_READ, VM_PROT_NONE, VM_WIMG_USE_DEFAULT, TRUE);
	assert(kr == KERN_SUCCESS);
	pmap_update_tt3e(sharedpage_pmap, _COMM_PAGE64_BASE_ADDRESS, PMAP_COMM_PAGE_PTE_TEMPLATE);

	/* ...and the user 32-bit mapping. */
	kr = pmap_enter(sharedpage_pmap, _COMM_PAGE32_BASE_ADDRESS, (ppnum_t)atop(pa), VM_PROT_READ, VM_PROT_NONE, VM_WIMG_USE_DEFAULT, TRUE);
	assert(kr == KERN_SUCCESS);
	pmap_update_tt3e(sharedpage_pmap, _COMM_PAGE32_BASE_ADDRESS, PMAP_COMM_PAGE_PTE_TEMPLATE);

	/* For manipulation in kernel, go straight to physical page */
	return (vm_map_address_t)phystokv(pa);
}

/*
 * Asserts to ensure that the TTEs we nest to map the shared page do not overlap
 * with user controlled TTEs.
 */
#if (ARM_PGSHIFT == 14)
static_assert((_COMM_PAGE64_BASE_ADDRESS & ~ARM_TT_L2_OFFMASK) >= MACH_VM_MAX_ADDRESS);
static_assert((_COMM_PAGE32_BASE_ADDRESS & ~ARM_TT_L2_OFFMASK) >= VM_MAX_ADDRESS);
#elif (ARM_PGSHIFT == 12)
static_assert((_COMM_PAGE64_BASE_ADDRESS & ~ARM_TT_L1_OFFMASK) >= MACH_VM_MAX_ADDRESS);
static_assert((_COMM_PAGE32_BASE_ADDRESS & ~ARM_TT_L1_OFFMASK) >= VM_MAX_ADDRESS);
#else
#error Nested shared page mapping is unsupported on this config
#endif

MARK_AS_PMAP_TEXT static kern_return_t
pmap_insert_sharedpage_internal(
	pmap_t pmap)
{
	kern_return_t kr = KERN_SUCCESS;
	vm_offset_t sharedpage_vaddr;
	pt_entry_t *ttep, *src_ttep;
	int options = 0;

	VALIDATE_PMAP(pmap);
#if XNU_MONITOR
	options |= PMAP_OPTIONS_NOWAIT;
#endif /* XNU_MONITOR */

#if _COMM_PAGE_AREA_LENGTH != PAGE_SIZE
#error We assume a single page.
#endif

	if (pmap_is_64bit(pmap)) {
		sharedpage_vaddr = _COMM_PAGE64_BASE_ADDRESS;
	} else {
		sharedpage_vaddr = _COMM_PAGE32_BASE_ADDRESS;
	}

	PMAP_LOCK(pmap);

	/*
	 * For 4KB pages, we can force the commpage to nest at the level one
	 * page table, as each entry is 1GB (i.e, there will be no overlap
	 * with regular userspace mappings).  For 16KB pages, each level one
	 * entry is 64GB, so we must go to the second level entry (32MB) in
	 * order to nest.
	 */
#if (ARM_PGSHIFT == 12)
	(void)options;

	/* Just slam in the L1 entry.  */
	ttep = pmap_tt1e(pmap, sharedpage_vaddr);

	if (*ttep != ARM_PTE_EMPTY) {
		panic("%s: Found something mapped at the commpage address?!", __FUNCTION__);
	}

	src_ttep = pmap_tt1e(sharedpage_pmap, sharedpage_vaddr);
#elif (ARM_PGSHIFT == 14)
	/* Allocate for the L2 entry if necessary, and slam it into place. */
	/*
	 * As long as we are use a three level page table, the first level
	 * should always exist, so we don't need to check for it.
	 */
	while (*pmap_tt1e(pmap, sharedpage_vaddr) == ARM_PTE_EMPTY) {
		PMAP_UNLOCK(pmap);

		kr = pmap_expand(pmap, sharedpage_vaddr, options, PMAP_TT_L2_LEVEL);

		if (kr != KERN_SUCCESS) {
#if XNU_MONITOR
			if (kr == KERN_RESOURCE_SHORTAGE) {
				return kr;
			} else
#endif
			{
				panic("Failed to pmap_expand for commpage, pmap=%p", pmap);
			}
		}

		PMAP_LOCK(pmap);
	}

	ttep = pmap_tt2e(pmap, sharedpage_vaddr);

	if (*ttep != ARM_PTE_EMPTY) {
		panic("%s: Found something mapped at the commpage address?!", __FUNCTION__);
	}

	src_ttep = pmap_tt2e(sharedpage_pmap, sharedpage_vaddr);
#endif

	*ttep =  *src_ttep;
	FLUSH_PTE_STRONG(ttep);

	/* TODO: Should we flush in the 64-bit case? */
	flush_mmu_tlb_region_asid_async(sharedpage_vaddr, PAGE_SIZE, kernel_pmap);

#if (ARM_PGSHIFT == 12)
	flush_mmu_tlb_entry_async(tlbi_addr(sharedpage_vaddr & ~ARM_TT_L1_OFFMASK) | tlbi_asid(pmap->hw_asid));
#elif (ARM_PGSHIFT == 14)
	flush_mmu_tlb_entry_async(tlbi_addr(sharedpage_vaddr & ~ARM_TT_L2_OFFMASK) | tlbi_asid(pmap->hw_asid));
#endif
	sync_tlb_flush();

	PMAP_UNLOCK(pmap);

	return kr;
}

static void
pmap_unmap_sharedpage(
	pmap_t pmap)
{
	pt_entry_t *ttep;
	vm_offset_t sharedpage_vaddr;

#if _COMM_PAGE_AREA_LENGTH != PAGE_SIZE
#error We assume a single page.
#endif

	if (pmap_is_64bit(pmap)) {
		sharedpage_vaddr = _COMM_PAGE64_BASE_ADDRESS;
	} else {
		sharedpage_vaddr = _COMM_PAGE32_BASE_ADDRESS;
	}

#if (ARM_PGSHIFT == 12)
	ttep = pmap_tt1e(pmap, sharedpage_vaddr);

	if (ttep == NULL) {
		return;
	}

	/* It had better be mapped to the shared page */
	if (*ttep != ARM_TTE_EMPTY && *ttep != *pmap_tt1e(sharedpage_pmap, sharedpage_vaddr)) {
		panic("%s: Something other than commpage mapped in shared page slot?", __FUNCTION__);
	}
#elif (ARM_PGSHIFT == 14)
	ttep = pmap_tt2e(pmap, sharedpage_vaddr);

	if (ttep == NULL) {
		return;
	}

	/* It had better be mapped to the shared page */
	if (*ttep != ARM_TTE_EMPTY && *ttep != *pmap_tt2e(sharedpage_pmap, sharedpage_vaddr)) {
		panic("%s: Something other than commpage mapped in shared page slot?", __FUNCTION__);
	}
#endif

	*ttep = ARM_TTE_EMPTY;
	flush_mmu_tlb_region_asid_async(sharedpage_vaddr, PAGE_SIZE, kernel_pmap);

#if (ARM_PGSHIFT == 12)
	flush_mmu_tlb_entry_async(tlbi_addr(sharedpage_vaddr & ~ARM_TT_L1_OFFMASK) | tlbi_asid(pmap->hw_asid));
#elif (ARM_PGSHIFT == 14)
	flush_mmu_tlb_entry_async(tlbi_addr(sharedpage_vaddr & ~ARM_TT_L2_OFFMASK) | tlbi_asid(pmap->hw_asid));
#endif
	sync_tlb_flush();
}

void
pmap_insert_sharedpage(
	pmap_t pmap)
{
#if XNU_MONITOR
	kern_return_t kr = KERN_FAILURE;

	while ((kr = pmap_insert_sharedpage_ppl(pmap)) == KERN_RESOURCE_SHORTAGE) {
		pmap_alloc_page_for_ppl();
	}

	pmap_ledger_check_balance(pmap);

	if (kr != KERN_SUCCESS) {
		panic("%s: failed to insert the shared page, kr=%d, "
		    "pmap=%p",
		    __FUNCTION__, kr,
		    pmap);
	}
#else
	pmap_insert_sharedpage_internal(pmap);
#endif
}

static boolean_t
pmap_is_64bit(
	pmap_t pmap)
{
	return pmap->is_64bit;
}

#endif

/* ARMTODO -- an implementation that accounts for
 * holes in the physical map, if any.
 */
boolean_t
pmap_valid_page(
	ppnum_t pn)
{
	return pa_valid(ptoa(pn));
}

boolean_t
pmap_bootloader_page(
	ppnum_t pn)
{
	pmap_paddr_t paddr = ptoa(pn);

	if (pa_valid(paddr)) {
		return FALSE;
	}
	pmap_io_range_t *io_rgn = pmap_find_io_attr(paddr);
	return (io_rgn != NULL) && (io_rgn->wimg & PMAP_IO_RANGE_CARVEOUT);
}

MARK_AS_PMAP_TEXT static boolean_t
pmap_is_empty_internal(
	pmap_t pmap,
	vm_map_offset_t va_start,
	vm_map_offset_t va_end)
{
	vm_map_offset_t block_start, block_end;
	tt_entry_t *tte_p;

	if (pmap == NULL) {
		return TRUE;
	}

	VALIDATE_PMAP(pmap);

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);
	unsigned int initial_not_in_kdp = not_in_kdp;

	if ((pmap != kernel_pmap) && (initial_not_in_kdp)) {
		PMAP_LOCK(pmap);
	}

#if     (__ARM_VMSA__ == 7)
	if (tte_index(pmap, pt_attr, va_end) >= pmap->tte_index_max) {
		if ((pmap != kernel_pmap) && (initial_not_in_kdp)) {
			PMAP_UNLOCK(pmap);
		}
		return TRUE;
	}
#endif

	/* TODO: This will be faster if we increment ttep at each level. */
	block_start = va_start;

	while (block_start < va_end) {
		pt_entry_t     *bpte_p, *epte_p;
		pt_entry_t     *pte_p;

		block_end = (block_start + pt_attr_twig_size(pt_attr)) & ~pt_attr_twig_offmask(pt_attr);
		if (block_end > va_end) {
			block_end = va_end;
		}

		tte_p = pmap_tte(pmap, block_start);
		if ((tte_p != PT_ENTRY_NULL)
		    && ((*tte_p & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE)) {
			pte_p = (pt_entry_t *) ttetokv(*tte_p);
			bpte_p = &pte_p[pte_index(pmap, pt_attr, block_start)];
			epte_p = &pte_p[pte_index(pmap, pt_attr, block_end)];

			for (pte_p = bpte_p; pte_p < epte_p; pte_p++) {
				if (*pte_p != ARM_PTE_EMPTY) {
					if ((pmap != kernel_pmap) && (initial_not_in_kdp)) {
						PMAP_UNLOCK(pmap);
					}
					return FALSE;
				}
			}
		}
		block_start = block_end;
	}

	if ((pmap != kernel_pmap) && (initial_not_in_kdp)) {
		PMAP_UNLOCK(pmap);
	}

	return TRUE;
}

boolean_t
pmap_is_empty(
	pmap_t pmap,
	vm_map_offset_t va_start,
	vm_map_offset_t va_end)
{
#if XNU_MONITOR
	return pmap_is_empty_ppl(pmap, va_start, va_end);
#else
	return pmap_is_empty_internal(pmap, va_start, va_end);
#endif
}

vm_map_offset_t
pmap_max_offset(
	boolean_t               is64,
	unsigned int    option)
{
	return (is64) ? pmap_max_64bit_offset(option) : pmap_max_32bit_offset(option);
}

vm_map_offset_t
pmap_max_64bit_offset(
	__unused unsigned int option)
{
	vm_map_offset_t max_offset_ret = 0;

#if defined(__arm64__)
	const vm_map_offset_t min_max_offset = SHARED_REGION_BASE_ARM64 + SHARED_REGION_SIZE_ARM64 + 0x20000000; // end of shared region + 512MB for various purposes
	if (option == ARM_PMAP_MAX_OFFSET_DEFAULT) {
		max_offset_ret = arm64_pmap_max_offset_default;
	} else if (option == ARM_PMAP_MAX_OFFSET_MIN) {
		max_offset_ret = min_max_offset;
	} else if (option == ARM_PMAP_MAX_OFFSET_MAX) {
		max_offset_ret = MACH_VM_MAX_ADDRESS;
	} else if (option == ARM_PMAP_MAX_OFFSET_DEVICE) {
		if (arm64_pmap_max_offset_default) {
			max_offset_ret = arm64_pmap_max_offset_default;
		} else if (max_mem > 0xC0000000) {
			max_offset_ret = min_max_offset + 0x138000000; // Max offset is 13.375GB for devices with > 3GB of memory
		} else if (max_mem > 0x40000000) {
			max_offset_ret = min_max_offset + 0x38000000;  // Max offset is 9.375GB for devices with > 1GB and <= 3GB of memory
		} else {
			max_offset_ret = min_max_offset;
		}
	} else if (option == ARM_PMAP_MAX_OFFSET_JUMBO) {
		if (arm64_pmap_max_offset_default) {
			// Allow the boot-arg to override jumbo size
			max_offset_ret = arm64_pmap_max_offset_default;
		} else {
			max_offset_ret = MACH_VM_MAX_ADDRESS;     // Max offset is 64GB for pmaps with special "jumbo" blessing
		}
	} else {
		panic("pmap_max_64bit_offset illegal option 0x%x\n", option);
	}

	assert(max_offset_ret <= MACH_VM_MAX_ADDRESS);
	assert(max_offset_ret >= min_max_offset);
#else
	panic("Can't run pmap_max_64bit_offset on non-64bit architectures\n");
#endif

	return max_offset_ret;
}

vm_map_offset_t
pmap_max_32bit_offset(
	unsigned int option)
{
	vm_map_offset_t max_offset_ret = 0;

	if (option == ARM_PMAP_MAX_OFFSET_DEFAULT) {
		max_offset_ret = arm_pmap_max_offset_default;
	} else if (option == ARM_PMAP_MAX_OFFSET_MIN) {
		max_offset_ret = 0x80000000;
	} else if (option == ARM_PMAP_MAX_OFFSET_MAX) {
		max_offset_ret = VM_MAX_ADDRESS;
	} else if (option == ARM_PMAP_MAX_OFFSET_DEVICE) {
		if (arm_pmap_max_offset_default) {
			max_offset_ret = arm_pmap_max_offset_default;
		} else if (max_mem > 0x20000000) {
			max_offset_ret = 0x80000000;
		} else {
			max_offset_ret = 0x80000000;
		}
	} else if (option == ARM_PMAP_MAX_OFFSET_JUMBO) {
		max_offset_ret = 0x80000000;
	} else {
		panic("pmap_max_32bit_offset illegal option 0x%x\n", option);
	}

	assert(max_offset_ret <= MACH_VM_MAX_ADDRESS);
	return max_offset_ret;
}

#if CONFIG_DTRACE
/*
 * Constrain DTrace copyin/copyout actions
 */
extern kern_return_t dtrace_copyio_preflight(addr64_t);
extern kern_return_t dtrace_copyio_postflight(addr64_t);

kern_return_t
dtrace_copyio_preflight(
	__unused addr64_t va)
{
	if (current_map() == kernel_map) {
		return KERN_FAILURE;
	} else {
		return KERN_SUCCESS;
	}
}

kern_return_t
dtrace_copyio_postflight(
	__unused addr64_t va)
{
	return KERN_SUCCESS;
}
#endif /* CONFIG_DTRACE */


void
pmap_flush_context_init(__unused pmap_flush_context *pfc)
{
}


void
pmap_flush(
	__unused pmap_flush_context *cpus_to_flush)
{
	/* not implemented yet */
	return;
}

#if XNU_MONITOR

/*
 * Enforce that the address range described by kva and nbytes is not currently
 * PPL-owned, and won't become PPL-owned while pinned.  This is to prevent
 * unintentionally writing to PPL-owned memory.
 */
static void
pmap_pin_kernel_pages(vm_offset_t kva, size_t nbytes)
{
	vm_offset_t end;
	if (os_add_overflow(kva, nbytes, &end)) {
		panic("%s(%p, 0x%llx): overflow", __func__, (void*)kva, (uint64_t)nbytes);
	}
	for (vm_offset_t ckva = kva; ckva < end; ckva = round_page(ckva + 1)) {
		pmap_paddr_t pa = kvtophys(ckva);
		if (!pa_valid(pa)) {
			panic("%s(%p): invalid physical page 0x%llx", __func__, (void*)kva, (uint64_t)pa);
		}
		pp_attr_t attr;
		unsigned int pai = (unsigned int)pa_index(pa);
		if (ckva == phystokv(pa)) {
			panic("%s(%p): attempt to pin static mapping for page 0x%llx", __func__, (void*)kva, (uint64_t)pa);
		}
		do {
			attr = pp_attr_table[pai] & ~PP_ATTR_NO_MONITOR;
			if (attr & PP_ATTR_MONITOR) {
				panic("%s(%p): physical page 0x%llx belongs to PPL", __func__, (void*)kva, (uint64_t)pa);
			}
		} while (!OSCompareAndSwap16(attr, attr | PP_ATTR_NO_MONITOR, &pp_attr_table[pai]));
	}
}

static void
pmap_unpin_kernel_pages(vm_offset_t kva, size_t nbytes)
{
	vm_offset_t end;
	if (os_add_overflow(kva, nbytes, &end)) {
		panic("%s(%p, 0x%llx): overflow", __func__, (void*)kva, (uint64_t)nbytes);
	}
	for (vm_offset_t ckva = kva; ckva < end; ckva = round_page(ckva + 1)) {
		pmap_paddr_t pa = kvtophys(ckva);
		if (!pa_valid(pa)) {
			panic("%s(%p): invalid physical page 0x%llx", __func__, (void*)kva, (uint64_t)pa);
		}
		if (!(pp_attr_table[pa_index(pa)] & PP_ATTR_NO_MONITOR)) {
			panic("%s(%p): physical page 0x%llx not pinned", __func__, (void*)kva, (uint64_t)pa);
		}
		assert(!(pp_attr_table[pa_index(pa)] & PP_ATTR_MONITOR));
		pa_clear_no_monitor(pa);
	}
}

/*
 * Lock down a page, making all mappings read-only, and preventing
 * further mappings or removal of this particular kva's mapping.
 * Effectively, it makes the page at kva immutable.
 */
MARK_AS_PMAP_TEXT static void
pmap_ppl_lockdown_page(vm_address_t kva)
{
	pmap_paddr_t pa = kvtophys(kva);
	unsigned int pai = (unsigned int)pa_index(pa);
	LOCK_PVH(pai);
	pv_entry_t **pv_h  = pai_to_pvh(pai);

	if (pa_test_monitor(pa)) {
		panic("%#lx: page %llx belongs to PPL", kva, pa);
	}

	if (pvh_get_flags(pv_h) & (PVH_FLAG_LOCKDOWN | PVH_FLAG_EXEC)) {
		panic("%#lx: already locked down/executable (%#llx)", kva, pvh_get_flags(pv_h));
	}

	pt_entry_t *pte_p = pmap_pte(kernel_pmap, kva);

	if (pte_p == PT_ENTRY_NULL) {
		panic("%#lx: NULL pte", kva);
	}

	pt_entry_t tmplate = *pte_p;
	if ((tmplate & ARM_PTE_APMASK) != ARM_PTE_AP(AP_RWNA)) {
		panic("%#lx: not a kernel r/w page (%#llx)", kva, tmplate & ARM_PTE_APMASK);
	}

	pvh_set_flags(pv_h, pvh_get_flags(pv_h) | PVH_FLAG_LOCKDOWN);

	pmap_set_ptov_ap(pai, AP_RONA, FALSE);

	UNLOCK_PVH(pai);

	pmap_page_protect_options_internal((ppnum_t)atop(pa), VM_PROT_READ, 0);
}

/*
 * Release a page from being locked down to the PPL, making it writable
 * to the kernel once again.
 */
MARK_AS_PMAP_TEXT static void
pmap_ppl_unlockdown_page(vm_address_t kva)
{
	pmap_paddr_t pa = kvtophys(kva);
	unsigned int pai = (unsigned int)pa_index(pa);
	LOCK_PVH(pai);
	pv_entry_t **pv_h  = pai_to_pvh(pai);

	vm_offset_t pvh_flags = pvh_get_flags(pv_h);

	if (!(pvh_flags & PVH_FLAG_LOCKDOWN)) {
		panic("unlockdown attempt on not locked down virtual %#lx/pai %d", kva, pai);
	}

	pvh_set_flags(pv_h, pvh_flags & ~PVH_FLAG_LOCKDOWN);
	pmap_set_ptov_ap(pai, AP_RWNA, FALSE);
	UNLOCK_PVH(pai);
}

#else /* XNU_MONITOR */

static void __unused
pmap_pin_kernel_pages(vm_offset_t kva __unused, size_t nbytes __unused)
{
}

static void __unused
pmap_unpin_kernel_pages(vm_offset_t kva __unused, size_t nbytes __unused)
{
}

#endif /* !XNU_MONITOR */


#define PMAP_RESIDENT_INVALID   ((mach_vm_size_t)-1)

MARK_AS_PMAP_TEXT static mach_vm_size_t
pmap_query_resident_internal(
	pmap_t                  pmap,
	vm_map_address_t        start,
	vm_map_address_t        end,
	mach_vm_size_t          *compressed_bytes_p)
{
	mach_vm_size_t  resident_bytes = 0;
	mach_vm_size_t  compressed_bytes = 0;

	pt_entry_t     *bpte, *epte;
	pt_entry_t     *pte_p;
	tt_entry_t     *tte_p;

	if (pmap == NULL) {
		return PMAP_RESIDENT_INVALID;
	}

	VALIDATE_PMAP(pmap);

	/* Ensure that this request is valid, and addresses exactly one TTE. */
	if (__improbable((start % ARM_PGBYTES) || (end % ARM_PGBYTES))) {
		panic("%s: address range %p, %p not page-aligned", __func__, (void*)start, (void*)end);
	}

	if (__improbable((end < start) || ((end - start) > (PTE_PGENTRIES * ARM_PGBYTES)))) {
		panic("%s: invalid address range %p, %p", __func__, (void*)start, (void*)end);
	}

	PMAP_LOCK(pmap);
	tte_p = pmap_tte(pmap, start);
	if (tte_p == (tt_entry_t *) NULL) {
		PMAP_UNLOCK(pmap);
		return PMAP_RESIDENT_INVALID;
	}
	if ((*tte_p & ARM_TTE_TYPE_MASK) == ARM_TTE_TYPE_TABLE) {
		__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);
		pte_p = (pt_entry_t *) ttetokv(*tte_p);
		bpte = &pte_p[pte_index(pmap, pt_attr, start)];
		epte = &pte_p[pte_index(pmap, pt_attr, end)];

		for (; bpte < epte; bpte++) {
			if (ARM_PTE_IS_COMPRESSED(*bpte, bpte)) {
				compressed_bytes += ARM_PGBYTES;
			} else if (pa_valid(pte_to_pa(*bpte))) {
				resident_bytes += ARM_PGBYTES;
			}
		}
	}
	PMAP_UNLOCK(pmap);

	if (compressed_bytes_p) {
		pmap_pin_kernel_pages((vm_offset_t)compressed_bytes_p, sizeof(*compressed_bytes_p));
		*compressed_bytes_p += compressed_bytes;
		pmap_unpin_kernel_pages((vm_offset_t)compressed_bytes_p, sizeof(*compressed_bytes_p));
	}

	return resident_bytes;
}

mach_vm_size_t
pmap_query_resident(
	pmap_t                  pmap,
	vm_map_address_t        start,
	vm_map_address_t        end,
	mach_vm_size_t          *compressed_bytes_p)
{
	mach_vm_size_t          total_resident_bytes;
	mach_vm_size_t          compressed_bytes;
	vm_map_address_t        va;


	if (pmap == PMAP_NULL) {
		if (compressed_bytes_p) {
			*compressed_bytes_p = 0;
		}
		return 0;
	}

	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	total_resident_bytes = 0;
	compressed_bytes = 0;

	PMAP_TRACE(3, PMAP_CODE(PMAP__QUERY_RESIDENT) | DBG_FUNC_START,
	    VM_KERNEL_ADDRHIDE(pmap), VM_KERNEL_ADDRHIDE(start),
	    VM_KERNEL_ADDRHIDE(end));

	va = start;
	while (va < end) {
		vm_map_address_t l;
		mach_vm_size_t resident_bytes;

		l = ((va + pt_attr_twig_size(pt_attr)) & ~pt_attr_twig_offmask(pt_attr));

		if (l > end) {
			l = end;
		}
#if XNU_MONITOR
		resident_bytes = pmap_query_resident_ppl(pmap, va, l, compressed_bytes_p);
#else
		resident_bytes = pmap_query_resident_internal(pmap, va, l, compressed_bytes_p);
#endif
		if (resident_bytes == PMAP_RESIDENT_INVALID) {
			break;
		}

		total_resident_bytes += resident_bytes;

		va = l;
	}

	if (compressed_bytes_p) {
		*compressed_bytes_p = compressed_bytes;
	}

	PMAP_TRACE(3, PMAP_CODE(PMAP__QUERY_RESIDENT) | DBG_FUNC_END,
	    total_resident_bytes);

	return total_resident_bytes;
}

#if MACH_ASSERT
static void
pmap_check_ledgers(
	pmap_t pmap)
{
	int     pid;
	char    *procname;

	if (pmap->pmap_pid == 0) {
		/*
		 * This pmap was not or is no longer fully associated
		 * with a task (e.g. the old pmap after a fork()/exec() or
		 * spawn()).  Its "ledger" still points at a task that is
		 * now using a different (and active) address space, so
		 * we can't check that all the pmap ledgers are balanced here.
		 *
		 * If the "pid" is set, that means that we went through
		 * pmap_set_process() in task_terminate_internal(), so
		 * this task's ledger should not have been re-used and
		 * all the pmap ledgers should be back to 0.
		 */
		return;
	}

	pid = pmap->pmap_pid;
	procname = pmap->pmap_procname;

	vm_map_pmap_check_ledgers(pmap, pmap->ledger, pid, procname);

	PMAP_STATS_ASSERTF(pmap->stats.resident_count == 0, pmap, "stats.resident_count %d", pmap->stats.resident_count);
#if 00
	PMAP_STATS_ASSERTF(pmap->stats.wired_count == 0, pmap, "stats.wired_count %d", pmap->stats.wired_count);
#endif
	PMAP_STATS_ASSERTF(pmap->stats.device == 0, pmap, "stats.device %d", pmap->stats.device);
	PMAP_STATS_ASSERTF(pmap->stats.internal == 0, pmap, "stats.internal %d", pmap->stats.internal);
	PMAP_STATS_ASSERTF(pmap->stats.external == 0, pmap, "stats.external %d", pmap->stats.external);
	PMAP_STATS_ASSERTF(pmap->stats.reusable == 0, pmap, "stats.reusable %d", pmap->stats.reusable);
	PMAP_STATS_ASSERTF(pmap->stats.compressed == 0, pmap, "stats.compressed %lld", pmap->stats.compressed);
}
#endif /* MACH_ASSERT */

void
pmap_advise_pagezero_range(__unused pmap_t p, __unused uint64_t a)
{
}


#if CONFIG_PGTRACE
#define PROF_START  uint64_t t, nanot;\
	            t = mach_absolute_time();

#define PROF_END    absolutetime_to_nanoseconds(mach_absolute_time()-t, &nanot);\
	            kprintf("%s: took %llu ns\n", __func__, nanot);

#define PMAP_PGTRACE_LOCK(p)                                \
    do {                                                    \
	*(p) = ml_set_interrupts_enabled(false);            \
	if (simple_lock_try(&(pmap_pgtrace.lock), LCK_GRP_NULL)) break;   \
	ml_set_interrupts_enabled(*(p));                    \
    } while (true)

#define PMAP_PGTRACE_UNLOCK(p)                  \
    do {                                        \
	simple_unlock(&(pmap_pgtrace.lock));    \
	ml_set_interrupts_enabled(*(p));        \
    } while (0)

#define PGTRACE_WRITE_PTE(pte_p, pte_entry) \
    do {                                    \
	*(pte_p) = (pte_entry);             \
	FLUSH_PTE(pte_p);                   \
    } while (0)

#define PGTRACE_MAX_MAP 16      // maximum supported va to same pa

typedef enum {
	UNDEFINED,
	PA_UNDEFINED,
	VA_UNDEFINED,
	DEFINED
} pmap_pgtrace_page_state_t;

typedef struct {
	queue_chain_t   chain;

	/*
	 *   pa              - pa
	 *   maps            - list of va maps to upper pa
	 *   map_pool        - map pool
	 *   map_waste       - waste can
	 *   state           - state
	 */
	pmap_paddr_t    pa;
	queue_head_t    maps;
	queue_head_t    map_pool;
	queue_head_t    map_waste;
	pmap_pgtrace_page_state_t    state;
} pmap_pgtrace_page_t;

static struct {
	/*
	 *   pages       - list of tracing page info
	 */
	queue_head_t    pages;
	decl_simple_lock_data(, lock);
} pmap_pgtrace = {};

static void
pmap_pgtrace_init(void)
{
	queue_init(&(pmap_pgtrace.pages));
	simple_lock_init(&(pmap_pgtrace.lock), 0);

	boolean_t enabled;

	if (PE_parse_boot_argn("pgtrace", &enabled, sizeof(enabled))) {
		pgtrace_enabled = enabled;
	}
}

// find a page with given pa - pmap_pgtrace should be locked
inline static pmap_pgtrace_page_t *
pmap_pgtrace_find_page(pmap_paddr_t pa)
{
	queue_head_t *q = &(pmap_pgtrace.pages);
	pmap_pgtrace_page_t *p;

	queue_iterate(q, p, pmap_pgtrace_page_t *, chain) {
		if (p->state == UNDEFINED) {
			continue;
		}
		if (p->state == PA_UNDEFINED) {
			continue;
		}
		if (p->pa == pa) {
			return p;
		}
	}

	return NULL;
}

// enter clone of given pmap, va page and range - pmap should be locked
static bool
pmap_pgtrace_enter_clone(pmap_t pmap, vm_map_offset_t va_page, vm_map_offset_t start, vm_map_offset_t end)
{
	bool ints;
	queue_head_t *q = &(pmap_pgtrace.pages);
	pmap_paddr_t pa_page;
	pt_entry_t *ptep, *cptep;
	pmap_pgtrace_page_t *p;
	bool found = false;

	PMAP_ASSERT_LOCKED(pmap);
	assert(va_page == arm_trunc_page(va_page));

	PMAP_PGTRACE_LOCK(&ints);

	ptep = pmap_pte(pmap, va_page);

	// target pte should exist
	if (!ptep || !(*ptep & ARM_PTE_TYPE_VALID)) {
		PMAP_PGTRACE_UNLOCK(&ints);
		return false;
	}

	queue_head_t *mapq;
	queue_head_t *mappool;
	pmap_pgtrace_map_t *map = NULL;

	pa_page = pte_to_pa(*ptep);

	// find if we have a page info defined for this
	queue_iterate(q, p, pmap_pgtrace_page_t *, chain) {
		mapq = &(p->maps);
		mappool = &(p->map_pool);

		switch (p->state) {
		case PA_UNDEFINED:
			queue_iterate(mapq, map, pmap_pgtrace_map_t *, chain) {
				if (map->cloned == false && map->pmap == pmap && map->ova == va_page) {
					p->pa = pa_page;
					map->range.start = start;
					map->range.end = end;
					found = true;
					break;
				}
			}
			break;

		case VA_UNDEFINED:
			if (p->pa != pa_page) {
				break;
			}
			queue_iterate(mapq, map, pmap_pgtrace_map_t *, chain) {
				if (map->cloned == false) {
					map->pmap = pmap;
					map->ova = va_page;
					map->range.start = start;
					map->range.end = end;
					found = true;
					break;
				}
			}
			break;

		case DEFINED:
			if (p->pa != pa_page) {
				break;
			}
			queue_iterate(mapq, map, pmap_pgtrace_map_t *, chain) {
				if (map->cloned == true && map->pmap == pmap && map->ova == va_page) {
					kprintf("%s: skip existing mapping at va=%llx\n", __func__, va_page);
					break;
				} else if (map->cloned == true && map->pmap == kernel_pmap && map->cva[1] == va_page) {
					kprintf("%s: skip clone mapping at va=%llx\n", __func__, va_page);
					break;
				} else if (map->cloned == false && map->pmap == pmap && map->ova == va_page) {
					// range should be already defined as well
					found = true;
					break;
				}
			}
			break;

		default:
			panic("invalid state p->state=%x\n", p->state);
		}

		if (found == true) {
			break;
		}
	}

	// do not clone if no page info found
	if (found == false) {
		PMAP_PGTRACE_UNLOCK(&ints);
		return false;
	}

	// copy pre, target and post ptes to clone ptes
	for (int i = 0; i < 3; i++) {
		ptep = pmap_pte(pmap, va_page + (i - 1) * ARM_PGBYTES);
		cptep = pmap_pte(kernel_pmap, map->cva[i]);
		assert(cptep != NULL);
		if (ptep == NULL) {
			PGTRACE_WRITE_PTE(cptep, (pt_entry_t)NULL);
		} else {
			PGTRACE_WRITE_PTE(cptep, *ptep);
		}
		PMAP_UPDATE_TLBS(kernel_pmap, map->cva[i], map->cva[i] + ARM_PGBYTES, false);
	}

	// get ptes for original and clone
	ptep = pmap_pte(pmap, va_page);
	cptep = pmap_pte(kernel_pmap, map->cva[1]);

	// invalidate original pte and mark it as a pgtrace page
	PGTRACE_WRITE_PTE(ptep, (*ptep | ARM_PTE_PGTRACE) & ~ARM_PTE_TYPE_VALID);
	PMAP_UPDATE_TLBS(pmap, map->ova, map->ova + ARM_PGBYTES, false);

	map->cloned = true;
	p->state = DEFINED;

	kprintf("%s: pa_page=%llx va_page=%llx cva[1]=%llx pmap=%p ptep=%p cptep=%p\n", __func__, pa_page, va_page, map->cva[1], pmap, ptep, cptep);

	PMAP_PGTRACE_UNLOCK(&ints);

	return true;
}

// This function removes trace bit and validate pte if applicable. Pmap must be locked.
static void
pmap_pgtrace_remove_clone(pmap_t pmap, pmap_paddr_t pa, vm_map_offset_t va)
{
	bool ints, found = false;
	pmap_pgtrace_page_t *p;
	pt_entry_t *ptep;

	PMAP_PGTRACE_LOCK(&ints);

	// we must have this page info
	p = pmap_pgtrace_find_page(pa);
	if (p == NULL) {
		goto unlock_exit;
	}

	// find matching map
	queue_head_t *mapq = &(p->maps);
	queue_head_t *mappool = &(p->map_pool);
	pmap_pgtrace_map_t *map;

	queue_iterate(mapq, map, pmap_pgtrace_map_t *, chain) {
		if (map->pmap == pmap && map->ova == va) {
			found = true;
			break;
		}
	}

	if (!found) {
		goto unlock_exit;
	}

	if (map->cloned == true) {
		// Restore back the pte to original state
		ptep = pmap_pte(pmap, map->ova);
		assert(ptep);
		PGTRACE_WRITE_PTE(ptep, *ptep | ARM_PTE_TYPE_VALID);
		PMAP_UPDATE_TLBS(pmap, va, va + ARM_PGBYTES, false);

		// revert clone pages
		for (int i = 0; i < 3; i++) {
			ptep = pmap_pte(kernel_pmap, map->cva[i]);
			assert(ptep != NULL);
			PGTRACE_WRITE_PTE(ptep, map->cva_spte[i]);
			PMAP_UPDATE_TLBS(kernel_pmap, map->cva[i], map->cva[i] + ARM_PGBYTES, false);
		}
	}

	queue_remove(mapq, map, pmap_pgtrace_map_t *, chain);
	map->pmap = NULL;
	map->ova = (vm_map_offset_t)NULL;
	map->cloned = false;
	queue_enter_first(mappool, map, pmap_pgtrace_map_t *, chain);

	kprintf("%s: p=%p pa=%llx va=%llx\n", __func__, p, pa, va);

unlock_exit:
	PMAP_PGTRACE_UNLOCK(&ints);
}

// remove all clones of given pa - pmap must be locked
static void
pmap_pgtrace_remove_all_clone(pmap_paddr_t pa)
{
	bool ints;
	pmap_pgtrace_page_t *p;
	pt_entry_t *ptep;

	PMAP_PGTRACE_LOCK(&ints);

	// we must have this page info
	p = pmap_pgtrace_find_page(pa);
	if (p == NULL) {
		PMAP_PGTRACE_UNLOCK(&ints);
		return;
	}

	queue_head_t *mapq = &(p->maps);
	queue_head_t *mappool = &(p->map_pool);
	queue_head_t *mapwaste = &(p->map_waste);
	pmap_pgtrace_map_t *map;

	// move maps to waste
	while (!queue_empty(mapq)) {
		queue_remove_first(mapq, map, pmap_pgtrace_map_t *, chain);
		queue_enter_first(mapwaste, map, pmap_pgtrace_map_t*, chain);
	}

	PMAP_PGTRACE_UNLOCK(&ints);

	// sanitize maps in waste
	queue_iterate(mapwaste, map, pmap_pgtrace_map_t *, chain) {
		if (map->cloned == true) {
			PMAP_LOCK(map->pmap);

			// restore back original pte
			ptep = pmap_pte(map->pmap, map->ova);
			assert(ptep);
			PGTRACE_WRITE_PTE(ptep, *ptep | ARM_PTE_TYPE_VALID);
			PMAP_UPDATE_TLBS(map->pmap, map->ova, map->ova + ARM_PGBYTES, false);

			// revert clone ptes
			for (int i = 0; i < 3; i++) {
				ptep = pmap_pte(kernel_pmap, map->cva[i]);
				assert(ptep != NULL);
				PGTRACE_WRITE_PTE(ptep, map->cva_spte[i]);
				PMAP_UPDATE_TLBS(kernel_pmap, map->cva[i], map->cva[i] + ARM_PGBYTES, false);
			}

			PMAP_UNLOCK(map->pmap);
		}

		map->pmap = NULL;
		map->ova = (vm_map_offset_t)NULL;
		map->cloned = false;
	}

	PMAP_PGTRACE_LOCK(&ints);

	// recycle maps back to map_pool
	while (!queue_empty(mapwaste)) {
		queue_remove_first(mapwaste, map, pmap_pgtrace_map_t *, chain);
		queue_enter_first(mappool, map, pmap_pgtrace_map_t*, chain);
	}

	PMAP_PGTRACE_UNLOCK(&ints);
}

inline static void
pmap_pgtrace_get_search_space(pmap_t pmap, vm_map_offset_t *startp, vm_map_offset_t *endp)
{
	uint64_t tsz;
	vm_map_offset_t end;

	if (pmap == kernel_pmap) {
		tsz = (get_tcr() >> TCR_T1SZ_SHIFT) & TCR_TSZ_MASK;
		*startp = MAX(VM_MIN_KERNEL_ADDRESS, (UINT64_MAX >> (64 - tsz)) << (64 - tsz));
		*endp = VM_MAX_KERNEL_ADDRESS;
	} else {
		tsz = (get_tcr() >> TCR_T0SZ_SHIFT) & TCR_TSZ_MASK;
		if (tsz == 64) {
			end = 0;
		} else {
			end = ((uint64_t)1 << (64 - tsz)) - 1;
		}

		*startp = 0;
		*endp = end;
	}

	assert(*endp > *startp);

	return;
}

// has pa mapped in given pmap? then clone it
static uint64_t
pmap_pgtrace_clone_from_pa(pmap_t pmap, pmap_paddr_t pa, vm_map_offset_t start_offset, vm_map_offset_t end_offset)
{
	uint64_t ret = 0;
	vm_map_offset_t min, max;
	vm_map_offset_t cur_page, end_page;
	pt_entry_t *ptep;
	tt_entry_t *ttep;
	tt_entry_t tte;
	__unused const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	pmap_pgtrace_get_search_space(pmap, &min, &max);

	cur_page = arm_trunc_page(min);
	end_page = arm_trunc_page(max);
	while (cur_page <= end_page) {
		vm_map_offset_t add = 0;

		PMAP_LOCK(pmap);

		// skip uninterested space
		if (pmap == kernel_pmap &&
		    ((vm_kernel_base <= cur_page && cur_page < vm_kernel_top) ||
		    (vm_kext_base <= cur_page && cur_page < vm_kext_top))) {
			add = ARM_PGBYTES;
			goto unlock_continue;
		}

		// check whether we can skip l1
		ttep = pmap_tt1e(pmap, cur_page);
		assert(ttep);
		tte = *ttep;
		if ((tte & (ARM_TTE_TYPE_MASK | ARM_TTE_VALID)) != (ARM_TTE_TYPE_TABLE | ARM_TTE_VALID)) {
			add = ARM_TT_L1_SIZE;
			goto unlock_continue;
		}

		// how about l2
		tte = ((tt_entry_t*) phystokv(tte & ARM_TTE_TABLE_MASK))[tt2_index(pmap, pt_attr, cur_page)];

		if ((tte & (ARM_TTE_TYPE_MASK | ARM_TTE_VALID)) != (ARM_TTE_TYPE_TABLE | ARM_TTE_VALID)) {
			add = ARM_TT_L2_SIZE;
			goto unlock_continue;
		}

		// ptep finally
		ptep = &(((pt_entry_t*) phystokv(tte & ARM_TTE_TABLE_MASK))[tt3_index(pmap, pt_attr, cur_page)]);
		if (ptep == PT_ENTRY_NULL) {
			add = ARM_TT_L3_SIZE;
			goto unlock_continue;
		}

		if (arm_trunc_page(pa) == pte_to_pa(*ptep)) {
			if (pmap_pgtrace_enter_clone(pmap, cur_page, start_offset, end_offset) == true) {
				ret++;
			}
		}

		add = ARM_PGBYTES;

unlock_continue:
		PMAP_UNLOCK(pmap);

		//overflow
		if (cur_page + add < cur_page) {
			break;
		}

		cur_page += add;
	}


	return ret;
}

// search pv table and clone vas of given pa
static uint64_t
pmap_pgtrace_clone_from_pvtable(pmap_paddr_t pa, vm_map_offset_t start_offset, vm_map_offset_t end_offset)
{
	uint64_t ret = 0;
	unsigned long pai;
	pv_entry_t **pvh;
	pt_entry_t *ptep;
	pmap_t pmap;

	typedef struct {
		queue_chain_t chain;
		pmap_t pmap;
		vm_map_offset_t va;
	} pmap_va_t;

	queue_head_t pmapvaq;
	pmap_va_t *pmapva;

	queue_init(&pmapvaq);

	pai = pa_index(pa);
	LOCK_PVH(pai);
	pvh = pai_to_pvh(pai);

	// collect pmap/va pair from pvh
	if (pvh_test_type(pvh, PVH_TYPE_PTEP)) {
		ptep = pvh_ptep(pvh);
		pmap = ptep_get_pmap(ptep);

		pmapva = (pmap_va_t *)kalloc(sizeof(pmap_va_t));
		pmapva->pmap = pmap;
		pmapva->va = ptep_get_va(ptep);

		queue_enter_first(&pmapvaq, pmapva, pmap_va_t *, chain);
	} else if (pvh_test_type(pvh, PVH_TYPE_PVEP)) {
		pv_entry_t *pvep;

		pvep = pvh_list(pvh);
		while (pvep) {
			ptep = pve_get_ptep(pvep);
			pmap = ptep_get_pmap(ptep);

			pmapva = (pmap_va_t *)kalloc(sizeof(pmap_va_t));
			pmapva->pmap = pmap;
			pmapva->va = ptep_get_va(ptep);

			queue_enter_first(&pmapvaq, pmapva, pmap_va_t *, chain);

			pvep = PVE_NEXT_PTR(pve_next(pvep));
		}
	}

	UNLOCK_PVH(pai);

	// clone them while making sure mapping still exists
	queue_iterate(&pmapvaq, pmapva, pmap_va_t *, chain) {
		PMAP_LOCK(pmapva->pmap);
		ptep = pmap_pte(pmapva->pmap, pmapva->va);
		if (pte_to_pa(*ptep) == pa) {
			if (pmap_pgtrace_enter_clone(pmapva->pmap, pmapva->va, start_offset, end_offset) == true) {
				ret++;
			}
		}
		PMAP_UNLOCK(pmapva->pmap);

		kfree(pmapva, sizeof(pmap_va_t));
	}

	return ret;
}

// allocate a page info
static pmap_pgtrace_page_t *
pmap_pgtrace_alloc_page(void)
{
	pmap_pgtrace_page_t *p;
	queue_head_t *mapq;
	queue_head_t *mappool;
	queue_head_t *mapwaste;
	pmap_pgtrace_map_t *map;

	p = kalloc(sizeof(pmap_pgtrace_page_t));
	assert(p);

	p->state = UNDEFINED;

	mapq = &(p->maps);
	mappool = &(p->map_pool);
	mapwaste = &(p->map_waste);
	queue_init(mapq);
	queue_init(mappool);
	queue_init(mapwaste);

	for (int i = 0; i < PGTRACE_MAX_MAP; i++) {
		vm_map_offset_t newcva;
		pt_entry_t *cptep;
		kern_return_t kr;
		vm_map_entry_t entry;

		// get a clone va
		vm_object_reference(kernel_object);
		kr = vm_map_find_space(kernel_map, &newcva, vm_map_round_page(3 * ARM_PGBYTES, PAGE_MASK), 0, 0, VM_MAP_KERNEL_FLAGS_NONE, VM_KERN_MEMORY_DIAG, &entry);
		if (kr != KERN_SUCCESS) {
			panic("%s VM couldn't find any space kr=%d\n", __func__, kr);
		}
		VME_OBJECT_SET(entry, kernel_object);
		VME_OFFSET_SET(entry, newcva);
		vm_map_unlock(kernel_map);

		// fill default clone page info and add to pool
		map = kalloc(sizeof(pmap_pgtrace_map_t));
		for (int j = 0; j < 3; j++) {
			vm_map_offset_t addr = newcva + j * ARM_PGBYTES;

			// pre-expand pmap while preemption enabled
			kr = pmap_expand(kernel_pmap, addr, 0, PMAP_TT_MAX_LEVEL);
			if (kr != KERN_SUCCESS) {
				panic("%s: pmap_expand(kernel_pmap, addr=%llx) returns kr=%d\n", __func__, addr, kr);
			}

			cptep = pmap_pte(kernel_pmap, addr);
			assert(cptep != NULL);

			map->cva[j] = addr;
			map->cva_spte[j] = *cptep;
		}
		map->range.start = map->range.end = 0;
		map->cloned = false;
		queue_enter_first(mappool, map, pmap_pgtrace_map_t *, chain);
	}

	return p;
}

// free a page info
static void
pmap_pgtrace_free_page(pmap_pgtrace_page_t *p)
{
	queue_head_t *mapq;
	queue_head_t *mappool;
	queue_head_t *mapwaste;
	pmap_pgtrace_map_t *map;

	assert(p);

	mapq = &(p->maps);
	mappool = &(p->map_pool);
	mapwaste = &(p->map_waste);

	while (!queue_empty(mapq)) {
		queue_remove_first(mapq, map, pmap_pgtrace_map_t *, chain);
		kfree(map, sizeof(pmap_pgtrace_map_t));
	}

	while (!queue_empty(mappool)) {
		queue_remove_first(mappool, map, pmap_pgtrace_map_t *, chain);
		kfree(map, sizeof(pmap_pgtrace_map_t));
	}

	while (!queue_empty(mapwaste)) {
		queue_remove_first(mapwaste, map, pmap_pgtrace_map_t *, chain);
		kfree(map, sizeof(pmap_pgtrace_map_t));
	}

	kfree(p, sizeof(pmap_pgtrace_page_t));
}

// construct page infos with the given address range
int
pmap_pgtrace_add_page(pmap_t pmap, vm_map_offset_t start, vm_map_offset_t end)
{
	int ret = 0;
	pt_entry_t *ptep;
	queue_head_t *q = &(pmap_pgtrace.pages);
	bool ints;
	vm_map_offset_t cur_page, end_page;

	if (start > end) {
		kprintf("%s: invalid start=%llx > end=%llx\n", __func__, start, end);
		return -1;
	}

	PROF_START

	// add each page in given range
	    cur_page = arm_trunc_page(start);
	end_page = arm_trunc_page(end);
	while (cur_page <= end_page) {
		pmap_paddr_t pa_page = 0;
		uint64_t num_cloned = 0;
		pmap_pgtrace_page_t *p = NULL, *newp;
		bool free_newp = true;
		pmap_pgtrace_page_state_t state;

		// do all allocations outside of spinlocks
		newp = pmap_pgtrace_alloc_page();

		// keep lock orders in pmap, kernel_pmap and pgtrace lock
		if (pmap != NULL) {
			PMAP_LOCK(pmap);
		}
		if (pmap != kernel_pmap) {
			PMAP_LOCK(kernel_pmap);
		}

		// addresses are physical if pmap is null
		if (pmap == NULL) {
			ptep = NULL;
			pa_page = cur_page;
			state = VA_UNDEFINED;
		} else {
			ptep = pmap_pte(pmap, cur_page);
			if (ptep != NULL) {
				pa_page = pte_to_pa(*ptep);
				state = DEFINED;
			} else {
				state = PA_UNDEFINED;
			}
		}

		// search if we have a page info already
		PMAP_PGTRACE_LOCK(&ints);
		if (state != PA_UNDEFINED) {
			p = pmap_pgtrace_find_page(pa_page);
		}

		// add pre-allocated page info if nothing found
		if (p == NULL) {
			queue_enter_first(q, newp, pmap_pgtrace_page_t *, chain);
			p = newp;
			free_newp = false;
		}

		// now p points what we want
		p->state = state;

		queue_head_t *mapq = &(p->maps);
		queue_head_t *mappool = &(p->map_pool);
		pmap_pgtrace_map_t *map;
		vm_map_offset_t start_offset, end_offset;

		// calculate trace offsets in the page
		if (cur_page > start) {
			start_offset = 0;
		} else {
			start_offset = start - cur_page;
		}
		if (cur_page == end_page) {
			end_offset = end - end_page;
		} else {
			end_offset = ARM_PGBYTES - 1;
		}

		kprintf("%s: pmap=%p cur_page=%llx ptep=%p state=%d start_offset=%llx end_offset=%llx\n", __func__, pmap, cur_page, ptep, state, start_offset, end_offset);

		// fill map info
		assert(!queue_empty(mappool));
		queue_remove_first(mappool, map, pmap_pgtrace_map_t *, chain);
		if (p->state == PA_UNDEFINED) {
			map->pmap = pmap;
			map->ova = cur_page;
			map->range.start = start_offset;
			map->range.end = end_offset;
		} else if (p->state == VA_UNDEFINED) {
			p->pa = pa_page;
			map->range.start = start_offset;
			map->range.end = end_offset;
		} else if (p->state == DEFINED) {
			p->pa = pa_page;
			map->pmap = pmap;
			map->ova = cur_page;
			map->range.start = start_offset;
			map->range.end = end_offset;
		} else {
			panic("invalid p->state=%d\n", p->state);
		}

		// not cloned yet
		map->cloned = false;
		queue_enter(mapq, map, pmap_pgtrace_map_t *, chain);

		// unlock locks
		PMAP_PGTRACE_UNLOCK(&ints);
		if (pmap != kernel_pmap) {
			PMAP_UNLOCK(kernel_pmap);
		}
		if (pmap != NULL) {
			PMAP_UNLOCK(pmap);
		}

		// now clone it
		if (pa_valid(pa_page)) {
			num_cloned = pmap_pgtrace_clone_from_pvtable(pa_page, start_offset, end_offset);
		}
		if (pmap == NULL) {
			num_cloned += pmap_pgtrace_clone_from_pa(kernel_pmap, pa_page, start_offset, end_offset);
		} else {
			num_cloned += pmap_pgtrace_clone_from_pa(pmap, pa_page, start_offset, end_offset);
		}

		// free pre-allocations if we didn't add it to the q
		if (free_newp) {
			pmap_pgtrace_free_page(newp);
		}

		if (num_cloned == 0) {
			kprintf("%s: no mapping found for pa_page=%llx but will be added when a page entered\n", __func__, pa_page);
		}

		ret += num_cloned;

		// overflow
		if (cur_page + ARM_PGBYTES < cur_page) {
			break;
		} else {
			cur_page += ARM_PGBYTES;
		}
	}

	PROF_END

	return ret;
}

// delete page infos for given address range
int
pmap_pgtrace_delete_page(pmap_t pmap, vm_map_offset_t start, vm_map_offset_t end)
{
	int ret = 0;
	bool ints;
	queue_head_t *q = &(pmap_pgtrace.pages);
	pmap_pgtrace_page_t *p;
	vm_map_offset_t cur_page, end_page;

	kprintf("%s start=%llx end=%llx\n", __func__, start, end);

	PROF_START

	pt_entry_t *ptep;
	pmap_paddr_t pa_page;

	// remove page info from start to end
	cur_page = arm_trunc_page(start);
	end_page = arm_trunc_page(end);
	while (cur_page <= end_page) {
		p = NULL;

		if (pmap == NULL) {
			pa_page = cur_page;
		} else {
			PMAP_LOCK(pmap);
			ptep = pmap_pte(pmap, cur_page);
			if (ptep == NULL) {
				PMAP_UNLOCK(pmap);
				goto cont;
			}
			pa_page = pte_to_pa(*ptep);
			PMAP_UNLOCK(pmap);
		}

		// remove all clones and validate
		pmap_pgtrace_remove_all_clone(pa_page);

		// find page info and delete
		PMAP_PGTRACE_LOCK(&ints);
		p = pmap_pgtrace_find_page(pa_page);
		if (p != NULL) {
			queue_remove(q, p, pmap_pgtrace_page_t *, chain);
			ret++;
		}
		PMAP_PGTRACE_UNLOCK(&ints);

		// free outside of locks
		if (p != NULL) {
			pmap_pgtrace_free_page(p);
		}

cont:
		// overflow
		if (cur_page + ARM_PGBYTES < cur_page) {
			break;
		} else {
			cur_page += ARM_PGBYTES;
		}
	}

	PROF_END

	return ret;
}

kern_return_t
pmap_pgtrace_fault(pmap_t pmap, vm_map_offset_t va, arm_saved_state_t *ss)
{
	pt_entry_t *ptep;
	pgtrace_run_result_t res;
	pmap_pgtrace_page_t *p;
	bool ints, found = false;
	pmap_paddr_t pa;

	// Quick check if we are interested
	ptep = pmap_pte(pmap, va);
	if (!ptep || !(*ptep & ARM_PTE_PGTRACE)) {
		return KERN_FAILURE;
	}

	PMAP_PGTRACE_LOCK(&ints);

	// Check again since access is serialized
	ptep = pmap_pte(pmap, va);
	if (!ptep || !(*ptep & ARM_PTE_PGTRACE)) {
		PMAP_PGTRACE_UNLOCK(&ints);
		return KERN_FAILURE;
	} else if ((*ptep & ARM_PTE_TYPE_VALID) == ARM_PTE_TYPE_VALID) {
		// Somehow this cpu's tlb has not updated
		kprintf("%s Somehow this cpu's tlb has not updated?\n", __func__);
		PMAP_UPDATE_TLBS(pmap, va, va + ARM_PGBYTES, false);

		PMAP_PGTRACE_UNLOCK(&ints);
		return KERN_SUCCESS;
	}

	// Find if this pa is what we are tracing
	pa = pte_to_pa(*ptep);

	p = pmap_pgtrace_find_page(arm_trunc_page(pa));
	if (p == NULL) {
		panic("%s Can't find va=%llx pa=%llx from tracing pages\n", __func__, va, pa);
	}

	// find if pmap and va are also matching
	queue_head_t *mapq = &(p->maps);
	queue_head_t *mapwaste = &(p->map_waste);
	pmap_pgtrace_map_t *map;

	queue_iterate(mapq, map, pmap_pgtrace_map_t *, chain) {
		if (map->pmap == pmap && map->ova == arm_trunc_page(va)) {
			found = true;
			break;
		}
	}

	// if not found, search map waste as they are still valid
	if (!found) {
		queue_iterate(mapwaste, map, pmap_pgtrace_map_t *, chain) {
			if (map->pmap == pmap && map->ova == arm_trunc_page(va)) {
				found = true;
				break;
			}
		}
	}

	if (!found) {
		panic("%s Can't find va=%llx pa=%llx from tracing pages\n", __func__, va, pa);
	}

	// Decode and run it on the clone map
	bzero(&res, sizeof(res));
	pgtrace_decode_and_run(*(uint32_t *)get_saved_state_pc(ss), // instruction
	    va, map->cva,                                       // fault va and clone page vas
	    ss, &res);

	// write a log if in range
	vm_map_offset_t offset = va - map->ova;
	if (map->range.start <= offset && offset <= map->range.end) {
		pgtrace_write_log(res);
	}

	PMAP_PGTRACE_UNLOCK(&ints);

	// Return to next instruction
	add_saved_state_pc(ss, sizeof(uint32_t));

	return KERN_SUCCESS;
}
#endif

boolean_t
pmap_enforces_execute_only(
#if (__ARM_VMSA__ == 7)
	__unused
#endif
	pmap_t pmap)
{
#if (__ARM_VMSA__ > 7)
	return pmap != kernel_pmap;
#else
	return FALSE;
#endif
}

MARK_AS_PMAP_TEXT void
pmap_set_jit_entitled_internal(
	__unused pmap_t pmap)
{
	return;
}

void
pmap_set_jit_entitled(
	pmap_t pmap)
{
#if XNU_MONITOR
	pmap_set_jit_entitled_ppl(pmap);
#else
	pmap_set_jit_entitled_internal(pmap);
#endif
}

MARK_AS_PMAP_TEXT static kern_return_t
pmap_query_page_info_internal(
	pmap_t          pmap,
	vm_map_offset_t va,
	int             *disp_p)
{
	pmap_paddr_t    pa;
	int             disp;
	int             pai;
	pt_entry_t      *pte;
	pv_entry_t      **pv_h, *pve_p;

	if (pmap == PMAP_NULL || pmap == kernel_pmap) {
		pmap_pin_kernel_pages((vm_offset_t)disp_p, sizeof(*disp_p));
		*disp_p = 0;
		pmap_unpin_kernel_pages((vm_offset_t)disp_p, sizeof(*disp_p));
		return KERN_INVALID_ARGUMENT;
	}

	disp = 0;

	VALIDATE_PMAP(pmap);
	PMAP_LOCK(pmap);

	pte = pmap_pte(pmap, va);
	if (pte == PT_ENTRY_NULL) {
		goto done;
	}

	pa = pte_to_pa(*pte);
	if (pa == 0) {
		if (ARM_PTE_IS_COMPRESSED(*pte, pte)) {
			disp |= PMAP_QUERY_PAGE_COMPRESSED;
			if (*pte & ARM_PTE_COMPRESSED_ALT) {
				disp |= PMAP_QUERY_PAGE_COMPRESSED_ALTACCT;
			}
		}
	} else {
		disp |= PMAP_QUERY_PAGE_PRESENT;
		pai = (int) pa_index(pa);
		if (!pa_valid(pa)) {
			goto done;
		}
		LOCK_PVH(pai);
		pv_h = pai_to_pvh(pai);
		pve_p = PV_ENTRY_NULL;
		if (pvh_test_type(pv_h, PVH_TYPE_PVEP)) {
			pve_p = pvh_list(pv_h);
			while (pve_p != PV_ENTRY_NULL &&
			    pve_get_ptep(pve_p) != pte) {
				pve_p = PVE_NEXT_PTR(pve_next(pve_p));
			}
		}
		if (IS_ALTACCT_PAGE(pai, pve_p)) {
			disp |= PMAP_QUERY_PAGE_ALTACCT;
		} else if (IS_REUSABLE_PAGE(pai)) {
			disp |= PMAP_QUERY_PAGE_REUSABLE;
		} else if (IS_INTERNAL_PAGE(pai)) {
			disp |= PMAP_QUERY_PAGE_INTERNAL;
		}
		UNLOCK_PVH(pai);
	}

done:
	PMAP_UNLOCK(pmap);
	pmap_pin_kernel_pages((vm_offset_t)disp_p, sizeof(*disp_p));
	*disp_p = disp;
	pmap_unpin_kernel_pages((vm_offset_t)disp_p, sizeof(*disp_p));
	return KERN_SUCCESS;
}

kern_return_t
pmap_query_page_info(
	pmap_t          pmap,
	vm_map_offset_t va,
	int             *disp_p)
{
#if XNU_MONITOR
	return pmap_query_page_info_ppl(pmap, va, disp_p);
#else
	return pmap_query_page_info_internal(pmap, va, disp_p);
#endif
}

MARK_AS_PMAP_TEXT kern_return_t
pmap_return_internal(__unused boolean_t do_panic, __unused boolean_t do_recurse)
{

	return KERN_SUCCESS;
}

kern_return_t
pmap_return(boolean_t do_panic, boolean_t do_recurse)
{
#if XNU_MONITOR
	return pmap_return_ppl(do_panic, do_recurse);
#else
	return pmap_return_internal(do_panic, do_recurse);
#endif
}




MARK_AS_PMAP_TEXT static void
pmap_footprint_suspend_internal(
	vm_map_t        map,
	boolean_t       suspend)
{
#if DEVELOPMENT || DEBUG
	if (suspend) {
		current_thread()->pmap_footprint_suspended = TRUE;
		map->pmap->footprint_was_suspended = TRUE;
	} else {
		current_thread()->pmap_footprint_suspended = FALSE;
	}
#else /* DEVELOPMENT || DEBUG */
	(void) map;
	(void) suspend;
#endif /* DEVELOPMENT || DEBUG */
}

void
pmap_footprint_suspend(
	vm_map_t map,
	boolean_t suspend)
{
#if XNU_MONITOR
	pmap_footprint_suspend_ppl(map, suspend);
#else
	pmap_footprint_suspend_internal(map, suspend);
#endif
}

#if defined(__arm64__) && (DEVELOPMENT || DEBUG)

struct page_table_dump_header {
	uint64_t pa;
	uint64_t num_entries;
	uint64_t start_va;
	uint64_t end_va;
};

static size_t
pmap_dump_page_tables_recurse(pmap_t pmap,
    const tt_entry_t *ttp,
    unsigned int cur_level,
    uint64_t start_va,
    void *bufp,
    void *buf_end)
{
	size_t bytes_used = 0;
	uint64_t num_entries = ARM_PGBYTES / sizeof(*ttp);
	const pt_attr_t * const pt_attr = pmap_get_pt_attr(pmap);

	uint64_t size = pt_attr->pta_level_info[cur_level].size;
	uint64_t valid_mask = pt_attr->pta_level_info[cur_level].valid_mask;
	uint64_t type_mask = pt_attr->pta_level_info[cur_level].type_mask;
	uint64_t type_block = pt_attr->pta_level_info[cur_level].type_block;

	if (cur_level == arm64_root_pgtable_level) {
		num_entries = arm64_root_pgtable_num_ttes;
	}

	uint64_t tt_size = num_entries * sizeof(tt_entry_t);
	const tt_entry_t *tt_end = &ttp[num_entries];

	if (((vm_offset_t)buf_end - (vm_offset_t)bufp) < (tt_size + sizeof(struct page_table_dump_header))) {
		return 0;
	}

	struct page_table_dump_header *header = (struct page_table_dump_header*)bufp;
	header->pa = ml_static_vtop((vm_offset_t)ttp);
	header->num_entries = num_entries;
	header->start_va = start_va;
	header->end_va = start_va + (num_entries * size);

	bcopy(ttp, (uint8_t*)bufp + sizeof(*header), tt_size);
	bytes_used += (sizeof(*header) + tt_size);
	uint64_t current_va = start_va;

	for (const tt_entry_t *ttep = ttp; ttep < tt_end; ttep++, current_va += size) {
		tt_entry_t tte = *ttep;

		if (!(tte & valid_mask)) {
			continue;
		}

		if ((tte & type_mask) == type_block) {
			continue;
		} else {
			if (cur_level >= PMAP_TT_MAX_LEVEL) {
				panic("%s: corrupt entry %#llx at %p, "
				    "ttp=%p, cur_level=%u, bufp=%p, buf_end=%p",
				    __FUNCTION__, tte, ttep,
				    ttp, cur_level, bufp, buf_end);
			}

			const tt_entry_t *next_tt = (const tt_entry_t*)phystokv(tte & ARM_TTE_TABLE_MASK);

			size_t recurse_result = pmap_dump_page_tables_recurse(pmap, next_tt, cur_level + 1, current_va, (uint8_t*)bufp + bytes_used, buf_end);

			if (recurse_result == 0) {
				return 0;
			}

			bytes_used += recurse_result;
		}
	}

	return bytes_used;
}

size_t
pmap_dump_page_tables(pmap_t pmap, void *bufp, void *buf_end)
{
	if (not_in_kdp) {
		panic("pmap_dump_page_tables must only be called from kernel debugger context");
	}
	return pmap_dump_page_tables_recurse(pmap, pmap->tte, arm64_root_pgtable_level, pmap->min, bufp, buf_end);
}

#else /* defined(__arm64__) && (DEVELOPMENT || DEBUG) */

size_t
pmap_dump_page_tables(pmap_t pmap __unused, void *bufp __unused, void *buf_end __unused)
{
	return (size_t)-1;
}

#endif /* !defined(__arm64__) */
