/*
 * Copyright (c) 2000-2016 Apple Inc. All rights reserved.
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
/*
 * @OSF_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989,1988,1987 Carnegie Mellon University
 * All Rights Reserved.
 * 
 * Permission to use, copy, modify and distribute this software and its
 * documentation is hereby granted, provided that both the copyright
 * notice and this permission notice appear in all copies of the
 * software, derivative works or modified versions, and any portions
 * thereof, and that both notices appear in supporting documentation.
 * 
 * CARNEGIE MELLON ALLOWS FREE USE OF THIS SOFTWARE IN ITS "AS IS"
 * CONDITION.  CARNEGIE MELLON DISCLAIMS ANY LIABILITY OF ANY KIND FOR
 * ANY DAMAGES WHATSOEVER RESULTING FROM THE USE OF THIS SOFTWARE.
 * 
 * Carnegie Mellon requests users of this software to return to
 * 
 *  Software Distribution Coordinator  or  Software.Distribution@CS.CMU.EDU
 *  School of Computer Science
 *  Carnegie Mellon University
 *  Pittsburgh PA 15213-3890
 * 
 * any improvements or extensions that they make and grant Carnegie Mellon
 * the rights to redistribute these changes.
 */
/*
 */
/*
 *	File:	mach/vm_statistics.h
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young, David Golub
 *
 *	Virtual memory statistics structure.
 *
 */

#ifndef	_MACH_VM_STATISTICS_H_
#define	_MACH_VM_STATISTICS_H_

#include <mach/machine/vm_types.h>


/*
 * vm_statistics
 *
 * History:
 *	rev0 - 	original structure.
 *	rev1 - 	added purgable info (purgable_count and purges).
 *	rev2 - 	added speculative_count.
 *
 * Note: you cannot add any new fields to this structure. Add them below in
 * 	 vm_statistics64.
 */

struct vm_statistics {
	natural_t	free_count;		/* # of pages free */
	natural_t	active_count;		/* # of pages active */
	natural_t	inactive_count;		/* # of pages inactive */
	natural_t	wire_count;		/* # of pages wired down */
	natural_t	zero_fill_count;	/* # of zero fill pages */
	natural_t	reactivations;		/* # of pages reactivated */
	natural_t	pageins;		/* # of pageins */
	natural_t	pageouts;		/* # of pageouts */
	natural_t	faults;			/* # of faults */
	natural_t	cow_faults;		/* # of copy-on-writes */
	natural_t	lookups;		/* object cache lookups */
	natural_t	hits;			/* object cache hits */

	/* added for rev1 */
	natural_t	purgeable_count;	/* # of pages purgeable */
	natural_t	purges;			/* # of pages purged */

	/* added for rev2 */
	/*
	 * NB: speculative pages are already accounted for in "free_count",
	 * so "speculative_count" is the number of "free" pages that are
	 * used to hold data that was read speculatively from disk but
	 * haven't actually been used by anyone so far.
	 */
	natural_t	speculative_count;	/* # of pages speculative */
};

/* Used by all architectures */
typedef struct vm_statistics	*vm_statistics_t;
typedef struct vm_statistics	vm_statistics_data_t;

/* 
 * vm_statistics64
 *
 * History:
 *	rev0 - 	original structure.
 *	rev1 - 	added purgable info (purgable_count and purges).
 *	rev2 - 	added speculative_count.
 *	   ----
 *	rev3 - 	changed name to vm_statistics64.
 *		changed some fields in structure to 64-bit on 
 *		arm, i386 and x86_64 architectures.
 *	rev4 -  require 64-bit alignment for efficient access
 *		in the kernel. No change to reported data.
 *
 */

struct vm_statistics64 {
	natural_t	free_count;		/* # of pages free */
	natural_t	active_count;		/* # of pages active */
	natural_t	inactive_count;		/* # of pages inactive */
	natural_t	wire_count;		/* # of pages wired down */
	uint64_t	zero_fill_count;	/* # of zero fill pages */
	uint64_t	reactivations;		/* # of pages reactivated */
	uint64_t	pageins;		/* # of pageins */
	uint64_t	pageouts;		/* # of pageouts */
	uint64_t	faults;			/* # of faults */
	uint64_t	cow_faults;		/* # of copy-on-writes */
	uint64_t	lookups;		/* object cache lookups */
	uint64_t	hits;			/* object cache hits */
	uint64_t	purges;			/* # of pages purged */
	natural_t	purgeable_count;	/* # of pages purgeable */
	/*
	 * NB: speculative pages are already accounted for in "free_count",
	 * so "speculative_count" is the number of "free" pages that are
	 * used to hold data that was read speculatively from disk but
	 * haven't actually been used by anyone so far.
	 */
	natural_t	speculative_count;	/* # of pages speculative */

	/* added for rev1 */
	uint64_t	decompressions;		/* # of pages decompressed */
	uint64_t	compressions;		/* # of pages compressed */
	uint64_t	swapins;		/* # of pages swapped in (via compression segments) */
	uint64_t	swapouts;		/* # of pages swapped out (via compression segments) */
	natural_t	compressor_page_count;	/* # of pages used by the compressed pager to hold all the compressed data */
	natural_t	throttled_count;	/* # of pages throttled */
	natural_t	external_page_count;	/* # of pages that are file-backed (non-swap) */
	natural_t	internal_page_count;	/* # of pages that are anonymous */
	uint64_t	total_uncompressed_pages_in_compressor; /* # of pages (uncompressed) held within the compressor. */
} __attribute__((aligned(8)));

typedef struct vm_statistics64	*vm_statistics64_t;
typedef struct vm_statistics64	vm_statistics64_data_t;

/* 
 * VM_STATISTICS_TRUNCATE_TO_32_BIT
 *
 * This is used by host_statistics() to truncate and peg the 64-bit in-kernel values from
 * vm_statistics64 to the 32-bit values of the older structure above (vm_statistics).
 */
#define VM_STATISTICS_TRUNCATE_TO_32_BIT(value) ((uint32_t)(((value) > UINT32_MAX ) ? UINT32_MAX : (value)))

/* 
 * vm_extmod_statistics
 *
 * Structure to record modifications to a task by an
 * external agent.
 *
 * History:
 *	rev0 - 	original structure.
 */

struct vm_extmod_statistics {
	int64_t	task_for_pid_count;			/* # of times task port was looked up */
	int64_t task_for_pid_caller_count;	/* # of times this task called task_for_pid */
	int64_t	thread_creation_count;		/* # of threads created in task */
	int64_t	thread_creation_caller_count;	/* # of threads created by task */
	int64_t	thread_set_state_count;		/* # of register state sets in task */
	int64_t	thread_set_state_caller_count;	/* # of register state sets by task */
} __attribute__((aligned(8)));

typedef struct vm_extmod_statistics *vm_extmod_statistics_t;
typedef struct vm_extmod_statistics vm_extmod_statistics_data_t;

typedef struct vm_purgeable_stat {
	uint64_t	count;
	uint64_t	size;
}vm_purgeable_stat_t;

struct vm_purgeable_info {
	vm_purgeable_stat_t fifo_data[8];
	vm_purgeable_stat_t obsolete_data;
	vm_purgeable_stat_t lifo_data[8];
};

typedef struct vm_purgeable_info	*vm_purgeable_info_t;

/* included for the vm_map_page_query call */

#define VM_PAGE_QUERY_PAGE_PRESENT      0x1
#define VM_PAGE_QUERY_PAGE_FICTITIOUS   0x2
#define VM_PAGE_QUERY_PAGE_REF          0x4
#define VM_PAGE_QUERY_PAGE_DIRTY        0x8
#define VM_PAGE_QUERY_PAGE_PAGED_OUT    0x10
#define VM_PAGE_QUERY_PAGE_COPIED       0x20
#define VM_PAGE_QUERY_PAGE_SPECULATIVE	0x40
#define VM_PAGE_QUERY_PAGE_EXTERNAL	0x80
#define VM_PAGE_QUERY_PAGE_CS_VALIDATED	0x100
#define VM_PAGE_QUERY_PAGE_CS_TAINTED	0x200
#define VM_PAGE_QUERY_PAGE_CS_NX	0x400

#ifdef	MACH_KERNEL_PRIVATE

/*
 *	Each machine dependent implementation is expected to
 *	keep certain statistics.  They may do this anyway they
 *	so choose, but are expected to return the statistics
 *	in the following structure.
 */

struct pmap_statistics {
	integer_t	resident_count;	/* # of pages mapped (total)*/
	integer_t	resident_max;	/* # of pages mapped (peak) */
	integer_t	wired_count;	/* # of pages wired */

	integer_t	device;
	integer_t	device_peak;
	integer_t	internal;
	integer_t	internal_peak;
	integer_t	external;
	integer_t	external_peak;
	integer_t	reusable;
	integer_t	reusable_peak;
	uint64_t	compressed __attribute__((aligned(8)));
	uint64_t	compressed_peak __attribute__((aligned(8)));
	uint64_t	compressed_lifetime __attribute__((aligned(8)));
};

typedef struct pmap_statistics	*pmap_statistics_t;

#define PMAP_STATS_PEAK(field)			\
	MACRO_BEGIN				\
	if (field > field##_peak) {		\
		field##_peak = field;		\
	}					\
	MACRO_END

#endif	/* MACH_KERNEL_PRIVATE */

/*
 * VM allocation flags:
 * 
 * VM_FLAGS_FIXED
 * 	(really the absence of VM_FLAGS_ANYWHERE)
 *	Allocate new VM region at the specified virtual address, if possible.
 * 
 * VM_FLAGS_ANYWHERE
 *	Allocate new VM region anywhere it would fit in the address space.
 *
 * VM_FLAGS_PURGABLE
 *	Create a purgable VM object for that new VM region.
 *
 * VM_FLAGS_NO_PMAP_CHECK
 *	(for DEBUG kernel config only, ignored for other configs)
 *	Do not check that there is no stale pmap mapping for the new VM region.
 *	This is useful for kernel memory allocations at bootstrap when building
 *	the initial kernel address space while some memory is already in use.
 *
 * VM_FLAGS_OVERWRITE
 *	The new VM region can replace existing VM regions if necessary
 *	(to be used in combination with VM_FLAGS_FIXED).
 *
 * VM_FLAGS_NO_CACHE
 *	Pages brought in to this VM region are placed on the speculative
 *	queue instead of the active queue.  In other words, they are not
 *	cached so that they will be stolen first if memory runs low.
 */

#define VM_FLAGS_FIXED		0x0000
#define VM_FLAGS_ANYWHERE	0x0001
#define VM_FLAGS_PURGABLE	0x0002
#define VM_FLAGS_RANDOM_ADDR	0x0008
#define VM_FLAGS_NO_CACHE	0x0010
#define VM_FLAGS_RESILIENT_CODESIGN	0x0020
#define VM_FLAGS_RESILIENT_MEDIA	0x0040
#define VM_FLAGS_OVERWRITE	0x4000	/* delete any existing mappings first */
/*
 * VM_FLAGS_SUPERPAGE_MASK
 *	3 bits that specify whether large pages should be used instead of
 *	base pages (!=0), as well as the requested page size.
 */
#define VM_FLAGS_SUPERPAGE_MASK	0x70000	/* bits 0x10000, 0x20000, 0x40000 */
#define VM_FLAGS_RETURN_DATA_ADDR	0x100000 /* Return address of target data, rather than base of page */
#define VM_FLAGS_RETURN_4K_DATA_ADDR	0x800000 /* Return 4K aligned address of target data */
#define VM_FLAGS_ALIAS_MASK	0xFF000000
#define VM_GET_FLAGS_ALIAS(flags, alias)			\
		(alias) = ((flags) & VM_FLAGS_ALIAS_MASK) >> 24	
#if !XNU_KERNEL_PRIVATE
#define VM_SET_FLAGS_ALIAS(flags, alias)			\
		(flags) = (((flags) & ~VM_FLAGS_ALIAS_MASK) |	\
		(((alias) & ~VM_FLAGS_ALIAS_MASK) << 24))
#endif /* !XNU_KERNEL_PRIVATE */

/* These are the flags that we accept from user-space */
#define VM_FLAGS_USER_ALLOCATE	(VM_FLAGS_FIXED |		\
				 VM_FLAGS_ANYWHERE |		\
				 VM_FLAGS_PURGABLE |		\
				 VM_FLAGS_RANDOM_ADDR |		\
				 VM_FLAGS_NO_CACHE |		\
				 VM_FLAGS_OVERWRITE |		\
				 VM_FLAGS_SUPERPAGE_MASK |	\
				 VM_FLAGS_ALIAS_MASK)
#define VM_FLAGS_USER_MAP	(VM_FLAGS_USER_ALLOCATE |	\
				 VM_FLAGS_RETURN_4K_DATA_ADDR |	\
				 VM_FLAGS_RETURN_DATA_ADDR)
#define VM_FLAGS_USER_REMAP	(VM_FLAGS_FIXED |    \
				 VM_FLAGS_ANYWHERE | \
				 VM_FLAGS_RANDOM_ADDR | \
				 VM_FLAGS_OVERWRITE| \
				 VM_FLAGS_RETURN_DATA_ADDR |\
				 VM_FLAGS_RESILIENT_CODESIGN)

#define VM_FLAGS_SUPERPAGE_SHIFT 16
#define SUPERPAGE_NONE			0	/* no superpages, if all bits are 0 */
#define SUPERPAGE_SIZE_ANY		1
#define VM_FLAGS_SUPERPAGE_NONE     (SUPERPAGE_NONE     << VM_FLAGS_SUPERPAGE_SHIFT)
#define VM_FLAGS_SUPERPAGE_SIZE_ANY (SUPERPAGE_SIZE_ANY << VM_FLAGS_SUPERPAGE_SHIFT)
#if defined(__x86_64__) || !defined(KERNEL)
#define SUPERPAGE_SIZE_2MB		2
#define VM_FLAGS_SUPERPAGE_SIZE_2MB (SUPERPAGE_SIZE_2MB<<VM_FLAGS_SUPERPAGE_SHIFT)
#endif


#ifdef KERNEL_PRIVATE
typedef struct {
	unsigned int
		vmkf_atomic_entry:1,
		vmkf_permanent:1,
		vmkf_guard_after:1,
		vmkf_guard_before:1,
		vmkf_submap:1,
		vmkf_already:1,
		vmkf_beyond_max:1,
		vmkf_no_pmap_check:1,
		vmkf_map_jit:1,
		vmkf_iokit_acct:1,
		vmkf_keep_map_locked:1,
		vmkf_fourk:1,
		vmkf_overwrite_immutable:1,
		vmkf_remap_prot_copy:1,
		__vmkf_unused:18;
} vm_map_kernel_flags_t;
#define VM_MAP_KERNEL_FLAGS_NONE (vm_map_kernel_flags_t) {		\
	.vmkf_atomic_entry = 0,	/* keep entry atomic (no coalescing) */ \
	.vmkf_permanent = 0,	/* mapping can NEVER be unmapped */	\
	.vmkf_guard_after = 0,	/* guard page after the mapping */	\
	.vmkf_guard_before = 0,	/* guard page before the mapping */	\
	.vmkf_submap = 0,	/* mapping a VM submap */		\
	.vmkf_already = 0,	/* OK if same mapping already exists */	\
	.vmkf_beyond_max = 0,	/* map beyond the map's max offset */	\
	.vmkf_no_pmap_check = 0, /* do not check that pmap is empty */	\
	.vmkf_map_jit = 0,	/* mark entry as JIT region */		\
	.vmkf_iokit_acct = 0,	/* IOKit accounting */			\
	.vmkf_keep_map_locked = 0, /* keep map locked when returning from vm_map_enter() */ \
	.vmkf_fourk = 0,	/* use fourk pager */			\
	.vmkf_overwrite_immutable = 0,	/* can overwrite immutable mappings */ \
	.vmkf_remap_prot_copy = 0, /* vm_remap for VM_PROT_COPY */ 	\
	.__vmkf_unused = 0						\
}
#endif /* KERNEL_PRIVATE */



#define VM_MEMORY_MALLOC 1
#define VM_MEMORY_MALLOC_SMALL 2
#define VM_MEMORY_MALLOC_LARGE 3
#define VM_MEMORY_MALLOC_HUGE 4
#define VM_MEMORY_SBRK 5// uninteresting -- no one should call
#define VM_MEMORY_REALLOC 6
#define VM_MEMORY_MALLOC_TINY 7
#define VM_MEMORY_MALLOC_LARGE_REUSABLE 8
#define VM_MEMORY_MALLOC_LARGE_REUSED 9

#define VM_MEMORY_ANALYSIS_TOOL 10

#define VM_MEMORY_MALLOC_NANO 11

#define VM_MEMORY_MACH_MSG 20
#define VM_MEMORY_IOKIT	21
#define VM_MEMORY_STACK  30
#define VM_MEMORY_GUARD  31
#define	VM_MEMORY_SHARED_PMAP 32
/* memory containing a dylib */
#define VM_MEMORY_DYLIB	33
#define VM_MEMORY_OBJC_DISPATCHERS 34

/* Was a nested pmap (VM_MEMORY_SHARED_PMAP) which has now been unnested */
#define	VM_MEMORY_UNSHARED_PMAP	35


// Placeholders for now -- as we analyze the libraries and find how they
// use memory, we can make these labels more specific.
#define VM_MEMORY_APPKIT 40
#define VM_MEMORY_FOUNDATION 41
#define VM_MEMORY_COREGRAPHICS 42
#define VM_MEMORY_CORESERVICES 43
#define VM_MEMORY_CARBON VM_MEMORY_CORESERVICES
#define VM_MEMORY_JAVA 44
#define VM_MEMORY_COREDATA 45
#define VM_MEMORY_COREDATA_OBJECTIDS 46
#define VM_MEMORY_ATS 50
#define VM_MEMORY_LAYERKIT 51
#define VM_MEMORY_CGIMAGE 52
#define VM_MEMORY_TCMALLOC 53

/* private raster data (i.e. layers, some images, QGL allocator) */
#define	VM_MEMORY_COREGRAPHICS_DATA	54

/* shared image and font caches */
#define VM_MEMORY_COREGRAPHICS_SHARED	55

/* Memory used for virtual framebuffers, shadowing buffers, etc... */
#define	VM_MEMORY_COREGRAPHICS_FRAMEBUFFERS	56

/* Window backing stores, custom shadow data, and compressed backing stores */
#define VM_MEMORY_COREGRAPHICS_BACKINGSTORES	57

/* x-alloc'd memory */
#define VM_MEMORY_COREGRAPHICS_XALLOC 58

/* catch-all for other uses, such as the read-only shared data page */
#define VM_MEMORY_COREGRAPHICS_MISC VM_MEMORY_COREGRAPHICS

/* memory allocated by the dynamic loader for itself */
#define VM_MEMORY_DYLD 60
/* malloc'd memory created by dyld */
#define VM_MEMORY_DYLD_MALLOC 61

/* Used for sqlite page cache */
#define VM_MEMORY_SQLITE 62

/* JavaScriptCore heaps */
#define VM_MEMORY_JAVASCRIPT_CORE 63
#define VM_MEMORY_WEBASSEMBLY VM_MEMORY_JAVASCRIPT_CORE
/* memory allocated for the JIT */
#define VM_MEMORY_JAVASCRIPT_JIT_EXECUTABLE_ALLOCATOR 64
#define VM_MEMORY_JAVASCRIPT_JIT_REGISTER_FILE 65

/* memory allocated for GLSL */
#define VM_MEMORY_GLSL  66

/* memory allocated for OpenCL.framework */
#define VM_MEMORY_OPENCL    67

/* memory allocated for QuartzCore.framework */
#define VM_MEMORY_COREIMAGE 68

/* memory allocated for WebCore Purgeable Buffers */
#define VM_MEMORY_WEBCORE_PURGEABLE_BUFFERS 69

/* ImageIO memory */
#define VM_MEMORY_IMAGEIO	70

/* CoreProfile memory */
#define VM_MEMORY_COREPROFILE	71

/* assetsd / MobileSlideShow memory */
#define VM_MEMORY_ASSETSD	72

/* libsystem_kernel os_once_alloc */
#define VM_MEMORY_OS_ALLOC_ONCE 73

/* libdispatch internal allocator */
#define VM_MEMORY_LIBDISPATCH 74

/* Accelerate.framework image backing stores */
#define VM_MEMORY_ACCELERATE 75

/* CoreUI image block data */
#define VM_MEMORY_COREUI 76

/* CoreUI image file */
#define VM_MEMORY_COREUIFILE 77

/* Genealogy buffers */
#define VM_MEMORY_GENEALOGY 78

/* RawCamera VM allocated memory */
#define VM_MEMORY_RAWCAMERA 79

/* corpse info for dead process */
#define VM_MEMORY_CORPSEINFO 80

/* Apple System Logger (ASL) messages */
#define VM_MEMORY_ASL 81

/* Swift runtime */
#define VM_MEMORY_SWIFT_RUNTIME 82

/* Swift metadata */
#define VM_MEMORY_SWIFT_METADATA 83

/* DHMM data */
#define VM_MEMORY_DHMM 84


/* memory allocated by SceneKit.framework */
#define VM_MEMORY_SCENEKIT 86

/* memory allocated by skywalk networking */
#define VM_MEMORY_SKYWALK 87

#define VM_MEMORY_IOSURFACE 88

#define VM_MEMORY_LIBNETWORK 89

#define VM_MEMORY_AUDIO 90

#define VM_MEMORY_VIDEOBITSTREAM 91

/* Reserve 240-255 for application */
#define VM_MEMORY_APPLICATION_SPECIFIC_1 240
#define VM_MEMORY_APPLICATION_SPECIFIC_16 255

#if !XNU_KERNEL_PRIVATE
#define VM_MAKE_TAG(tag) ((tag) << 24)
#endif /* XNU_KERNEL_PRIVATE */


#if KERNEL_PRIVATE

/* kernel map tags */
/* please add new definition strings to zprint */

#define VM_KERN_MEMORY_NONE		0

#define VM_KERN_MEMORY_OSFMK		1
#define VM_KERN_MEMORY_BSD		2
#define VM_KERN_MEMORY_IOKIT		3
#define VM_KERN_MEMORY_LIBKERN		4
#define VM_KERN_MEMORY_OSKEXT		5
#define VM_KERN_MEMORY_KEXT		6
#define VM_KERN_MEMORY_IPC		7
#define VM_KERN_MEMORY_STACK		8
#define VM_KERN_MEMORY_CPU		9
#define VM_KERN_MEMORY_PMAP		10
#define VM_KERN_MEMORY_PTE		11
#define VM_KERN_MEMORY_ZONE		12
#define VM_KERN_MEMORY_KALLOC		13
#define VM_KERN_MEMORY_COMPRESSOR	14
#define VM_KERN_MEMORY_COMPRESSED_DATA	15
#define VM_KERN_MEMORY_PHANTOM_CACHE	16
#define VM_KERN_MEMORY_WAITQ		17
#define VM_KERN_MEMORY_DIAG		18
#define VM_KERN_MEMORY_LOG		19
#define VM_KERN_MEMORY_FILE		20
#define VM_KERN_MEMORY_MBUF		21
#define VM_KERN_MEMORY_UBC		22
#define VM_KERN_MEMORY_SECURITY		23
#define VM_KERN_MEMORY_MLOCK		24
#define VM_KERN_MEMORY_REASON		25
#define VM_KERN_MEMORY_SKYWALK		26
#define VM_KERN_MEMORY_LTABLE		27

#define VM_KERN_MEMORY_FIRST_DYNAMIC	28
/* out of tags: */
#define VM_KERN_MEMORY_ANY		255
#define VM_KERN_MEMORY_COUNT		256

/* end kernel map tags */

// mach_memory_info.flags
#define VM_KERN_SITE_TYPE		0x000000FF
#define VM_KERN_SITE_TAG		0x00000000
#define VM_KERN_SITE_KMOD		0x00000001
#define VM_KERN_SITE_KERNEL		0x00000002
#define VM_KERN_SITE_COUNTER		0x00000003
#define VM_KERN_SITE_WIRED		0x00000100	/* add to wired count */
#define VM_KERN_SITE_HIDE		0x00000200	/* no zprint */
#define VM_KERN_SITE_NAMED		0x00000400
#define VM_KERN_SITE_ZONE		0x00000800

#define VM_KERN_COUNT_MANAGED		0
#define VM_KERN_COUNT_RESERVED		1
#define VM_KERN_COUNT_WIRED		2
#define VM_KERN_COUNT_WIRED_MANAGED 	3
#define VM_KERN_COUNT_STOLEN		4
#define VM_KERN_COUNT_LOPAGE		5
#define VM_KERN_COUNT_MAP_KERNEL	6
#define VM_KERN_COUNT_MAP_ZONE		7
#define VM_KERN_COUNT_MAP_KALLOC	8

#define VM_KERN_COUNT_WIRED_BOOT    9

#define VM_KERN_COUNT_BOOT_STOLEN	10

#define VM_KERN_COUNTER_COUNT		11

#endif /* KERNEL_PRIVATE */

#endif	/* _MACH_VM_STATISTICS_H_ */
