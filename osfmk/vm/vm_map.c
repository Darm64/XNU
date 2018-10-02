/*
 * Copyright (c) 2000-2012 Apple Inc. All rights reserved.
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
 *	File:	vm/vm_map.c
 *	Author:	Avadis Tevanian, Jr., Michael Wayne Young
 *	Date:	1985
 *
 *	Virtual memory mapping module.
 */

#include <task_swapper.h>
#include <mach_assert.h>

#include <vm/vm_options.h>

#include <libkern/OSAtomic.h>

#include <mach/kern_return.h>
#include <mach/port.h>
#include <mach/vm_attributes.h>
#include <mach/vm_param.h>
#include <mach/vm_behavior.h>
#include <mach/vm_statistics.h>
#include <mach/memory_object.h>
#include <mach/mach_vm.h>
#include <machine/cpu_capabilities.h>
#include <mach/sdt.h>

#include <kern/assert.h>
#include <kern/backtrace.h>
#include <kern/counters.h>
#include <kern/kalloc.h>
#include <kern/zalloc.h>

#include <vm/cpm.h>
#include <vm/vm_compressor_pager.h>
#include <vm/vm_init.h>
#include <vm/vm_fault.h>
#include <vm/vm_map.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/vm_kern.h>
#include <ipc/ipc_port.h>
#include <kern/sched_prim.h>
#include <kern/misc_protos.h>
#include <kern/xpr.h>

#include <mach/vm_map_server.h>
#include <mach/mach_host_server.h>
#include <vm/vm_protos.h>
#include <vm/vm_purgeable_internal.h>

#include <vm/vm_protos.h>
#include <vm/vm_shared_region.h>
#include <vm/vm_map_store.h>

#include <san/kasan.h>

#if __arm64__
extern int fourk_binary_compatibility_unsafe;
extern int fourk_binary_compatibility_allow_wx;
#endif /* __arm64__ */
extern int proc_selfpid(void);
extern char *proc_name_address(void *p);

#if VM_MAP_DEBUG_APPLE_PROTECT
int vm_map_debug_apple_protect = 0;
#endif /* VM_MAP_DEBUG_APPLE_PROTECT */
#if VM_MAP_DEBUG_FOURK
int vm_map_debug_fourk = 0;
#endif /* VM_MAP_DEBUG_FOURK */

int vm_map_executable_immutable = 0;
int vm_map_executable_immutable_no_log = 0;

extern u_int32_t random(void);	/* from <libkern/libkern.h> */
/* Internal prototypes
 */

static void vm_map_simplify_range(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end);	/* forward */

static boolean_t	vm_map_range_check(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_map_entry_t	*entry);

static vm_map_entry_t	_vm_map_entry_create(
	struct vm_map_header	*map_header, boolean_t map_locked);

static void		_vm_map_entry_dispose(
	struct vm_map_header	*map_header,
	vm_map_entry_t		entry);

static void		vm_map_pmap_enter(
	vm_map_t		map,
	vm_map_offset_t 	addr,
	vm_map_offset_t		end_addr,
	vm_object_t 		object,
	vm_object_offset_t	offset,
	vm_prot_t		protection);

static void		_vm_map_clip_end(
	struct vm_map_header	*map_header,
	vm_map_entry_t		entry,
	vm_map_offset_t		end);

static void		_vm_map_clip_start(
	struct vm_map_header	*map_header,
	vm_map_entry_t		entry,
	vm_map_offset_t		start);

static void		vm_map_entry_delete(
	vm_map_t	map,
	vm_map_entry_t	entry);

static kern_return_t	vm_map_delete(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	int		flags,
	vm_map_t	zap_map);

static kern_return_t	vm_map_copy_overwrite_unaligned(
	vm_map_t	dst_map,
	vm_map_entry_t	entry,
	vm_map_copy_t	copy,
	vm_map_address_t start,
	boolean_t	discard_on_success);

static kern_return_t	vm_map_copy_overwrite_aligned(
	vm_map_t	dst_map,
	vm_map_entry_t	tmp_entry,
	vm_map_copy_t	copy,
	vm_map_offset_t start,
	pmap_t		pmap);

static kern_return_t	vm_map_copyin_kernel_buffer(
	vm_map_t	src_map,
	vm_map_address_t src_addr,
	vm_map_size_t	len,
	boolean_t	src_destroy,
	vm_map_copy_t	*copy_result);  /* OUT */

static kern_return_t	vm_map_copyout_kernel_buffer(
	vm_map_t	map,
	vm_map_address_t *addr,	/* IN/OUT */
	vm_map_copy_t	copy,
	vm_map_size_t   copy_size,
	boolean_t	overwrite,
	boolean_t	consume_on_success);

static void		vm_map_fork_share(
	vm_map_t	old_map,
	vm_map_entry_t	old_entry,
	vm_map_t	new_map);

static boolean_t	vm_map_fork_copy(
	vm_map_t	old_map,
	vm_map_entry_t	*old_entry_p,
	vm_map_t	new_map,
	int		vm_map_copyin_flags);

static kern_return_t	vm_map_wire_nested(
	vm_map_t		   map,
	vm_map_offset_t		   start,
	vm_map_offset_t		   end,
	vm_prot_t		   caller_prot,
	vm_tag_t		   tag,
	boolean_t		   user_wire,
	pmap_t			   map_pmap,
	vm_map_offset_t		   pmap_addr,
	ppnum_t			   *physpage_p);

static kern_return_t	vm_map_unwire_nested(
	vm_map_t		   map,
	vm_map_offset_t		   start,
	vm_map_offset_t		   end,
	boolean_t		   user_wire,
	pmap_t			   map_pmap,
	vm_map_offset_t		   pmap_addr);

static kern_return_t	vm_map_overwrite_submap_recurse(
	vm_map_t		   dst_map,
	vm_map_offset_t		   dst_addr,
	vm_map_size_t		   dst_size);

static kern_return_t	vm_map_copy_overwrite_nested(
	vm_map_t		   dst_map,
	vm_map_offset_t		   dst_addr,
	vm_map_copy_t		   copy,
	boolean_t		   interruptible,
	pmap_t			   pmap,
	boolean_t		   discard_on_success);

static kern_return_t	vm_map_remap_extract(
	vm_map_t		map,
	vm_map_offset_t		addr,
	vm_map_size_t		size,
	boolean_t		copy,
	struct vm_map_header 	*map_header,
	vm_prot_t		*cur_protection,
	vm_prot_t		*max_protection,
	vm_inherit_t		inheritance,
	boolean_t		pageable,
	boolean_t		same_map,
	vm_map_kernel_flags_t	vmk_flags);

static kern_return_t	vm_map_remap_range_allocate(
	vm_map_t		map,
	vm_map_address_t	*address,
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	int			flags,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		tag,
	vm_map_entry_t		*map_entry);

static void		vm_map_region_look_for_page(
	vm_map_t		   map,
	vm_map_offset_t            va,
	vm_object_t		   object,
	vm_object_offset_t	   offset,
	int                        max_refcnt,
	int                        depth,
	vm_region_extended_info_t  extended,
	mach_msg_type_number_t count);

static int		vm_map_region_count_obj_refs(
	vm_map_entry_t    	   entry,
	vm_object_t       	   object);


static kern_return_t	vm_map_willneed(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end);

static kern_return_t	vm_map_reuse_pages(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end);

static kern_return_t	vm_map_reusable_pages(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end);

static kern_return_t	vm_map_can_reuse(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end);

#if MACH_ASSERT
static kern_return_t	vm_map_pageout(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end);
#endif /* MACH_ASSERT */

pid_t find_largest_process_vm_map_entries(void);

/*
 * Macros to copy a vm_map_entry. We must be careful to correctly
 * manage the wired page count. vm_map_entry_copy() creates a new
 * map entry to the same memory - the wired count in the new entry
 * must be set to zero. vm_map_entry_copy_full() creates a new
 * entry that is identical to the old entry.  This preserves the
 * wire count; it's used for map splitting and zone changing in
 * vm_map_copyout.
 */

#define vm_map_entry_copy(NEW,OLD)	\
MACRO_BEGIN				\
boolean_t _vmec_reserved = (NEW)->from_reserved_zone;	\
	*(NEW) = *(OLD);                \
	(NEW)->is_shared = FALSE;	\
	(NEW)->needs_wakeup = FALSE;    \
	(NEW)->in_transition = FALSE;   \
	(NEW)->wired_count = 0;         \
	(NEW)->user_wired_count = 0;    \
	(NEW)->permanent = FALSE;	\
	(NEW)->used_for_jit = FALSE;	\
	(NEW)->from_reserved_zone = _vmec_reserved;	\
	if ((NEW)->iokit_acct) {			\
	     assertf(!(NEW)->use_pmap, "old %p new %p\n", (OLD), (NEW)); \
	     (NEW)->iokit_acct = FALSE;			\
	     (NEW)->use_pmap = TRUE;			\
	}						\
	(NEW)->vme_resilient_codesign = FALSE; \
	(NEW)->vme_resilient_media = FALSE;	\
	(NEW)->vme_atomic = FALSE; 	\
MACRO_END

#define vm_map_entry_copy_full(NEW,OLD)			\
MACRO_BEGIN						\
boolean_t _vmecf_reserved = (NEW)->from_reserved_zone;	\
(*(NEW) = *(OLD));					\
(NEW)->from_reserved_zone = _vmecf_reserved;			\
MACRO_END

/*
 *	Decide if we want to allow processes to execute from their data or stack areas.
 *	override_nx() returns true if we do.  Data/stack execution can be enabled independently
 *	for 32 and 64 bit processes.  Set the VM_ABI_32 or VM_ABI_64 flags in allow_data_exec
 *	or allow_stack_exec to enable data execution for that type of data area for that particular
 *	ABI (or both by or'ing the flags together).  These are initialized in the architecture
 *	specific pmap files since the default behavior varies according to architecture.  The
 *	main reason it varies is because of the need to provide binary compatibility with old
 *	applications that were written before these restrictions came into being.  In the old
 *	days, an app could execute anything it could read, but this has slowly been tightened
 *	up over time.  The default behavior is:
 *
 *	32-bit PPC apps		may execute from both stack and data areas
 *	32-bit Intel apps	may exeucte from data areas but not stack
 *	64-bit PPC/Intel apps	may not execute from either data or stack
 *
 *	An application on any architecture may override these defaults by explicitly
 *	adding PROT_EXEC permission to the page in question with the mprotect(2)
 *	system call.  This code here just determines what happens when an app tries to
 * 	execute from a page that lacks execute permission.
 *
 *	Note that allow_data_exec or allow_stack_exec may also be modified by sysctl to change the
 *	default behavior for both 32 and 64 bit apps on a system-wide basis. Furthermore,
 *	a Mach-O header flag bit (MH_NO_HEAP_EXECUTION) can be used to forcibly disallow
 *	execution from data areas for a particular binary even if the arch normally permits it. As
 *	a final wrinkle, a posix_spawn attribute flag can be used to negate this opt-in header bit
 *	to support some complicated use cases, notably browsers with out-of-process plugins that
 *	are not all NX-safe.
 */

extern int allow_data_exec, allow_stack_exec;

int
override_nx(vm_map_t map, uint32_t user_tag) /* map unused on arm */
{
	int current_abi;

	if (map->pmap == kernel_pmap) return FALSE;

	/*
	 * Determine if the app is running in 32 or 64 bit mode.
	 */

	if (vm_map_is_64bit(map))
		current_abi = VM_ABI_64;
	else
		current_abi = VM_ABI_32;

	/*
	 * Determine if we should allow the execution based on whether it's a
	 * stack or data area and the current architecture.
	 */

	if (user_tag == VM_MEMORY_STACK)
		return allow_stack_exec & current_abi;

	return (allow_data_exec & current_abi) && (map->map_disallow_data_exec == FALSE);
}


/*
 *	Virtual memory maps provide for the mapping, protection,
 *	and sharing of virtual memory objects.  In addition,
 *	this module provides for an efficient virtual copy of
 *	memory from one map to another.
 *
 *	Synchronization is required prior to most operations.
 *
 *	Maps consist of an ordered doubly-linked list of simple
 *	entries; a single hint is used to speed up lookups.
 *
 *	Sharing maps have been deleted from this version of Mach.
 *	All shared objects are now mapped directly into the respective
 *	maps.  This requires a change in the copy on write strategy;
 *	the asymmetric (delayed) strategy is used for shared temporary
 *	objects instead of the symmetric (shadow) strategy.  All maps
 *	are now "top level" maps (either task map, kernel map or submap
 *	of the kernel map).
 *
 *	Since portions of maps are specified by start/end addreses,
 *	which may not align with existing map entries, all
 *	routines merely "clip" entries to these start/end values.
 *	[That is, an entry is split into two, bordering at a
 *	start or end value.]  Note that these clippings may not
 *	always be necessary (as the two resulting entries are then
 *	not changed); however, the clipping is done for convenience.
 *	No attempt is currently made to "glue back together" two
 *	abutting entries.
 *
 *	The symmetric (shadow) copy strategy implements virtual copy
 *	by copying VM object references from one map to
 *	another, and then marking both regions as copy-on-write.
 *	It is important to note that only one writeable reference
 *	to a VM object region exists in any map when this strategy
 *	is used -- this means that shadow object creation can be
 *	delayed until a write operation occurs.  The symmetric (delayed)
 *	strategy allows multiple maps to have writeable references to
 *	the same region of a vm object, and hence cannot delay creating
 *	its copy objects.  See vm_object_copy_quickly() in vm_object.c.
 *	Copying of permanent objects is completely different; see
 *	vm_object_copy_strategically() in vm_object.c.
 */

static zone_t	vm_map_zone;				/* zone for vm_map structures */
zone_t			vm_map_entry_zone;			/* zone for vm_map_entry structures */
static zone_t	vm_map_entry_reserved_zone;	/* zone with reserve for non-blocking allocations */
static zone_t	vm_map_copy_zone;			/* zone for vm_map_copy structures */
zone_t			vm_map_holes_zone;			/* zone for vm map holes (vm_map_links) structures */


/*
 *	Placeholder object for submap operations.  This object is dropped
 *	into the range by a call to vm_map_find, and removed when
 *	vm_map_submap creates the submap.
 */

vm_object_t	vm_submap_object;

static void		*map_data;
static vm_size_t	map_data_size;
static void		*kentry_data;
static vm_size_t	kentry_data_size;
static void		*map_holes_data;
static vm_size_t	map_holes_data_size;

#if CONFIG_EMBEDDED
#define		NO_COALESCE_LIMIT  0
#else
#define         NO_COALESCE_LIMIT  ((1024 * 128) - 1)
#endif

/* Skip acquiring locks if we're in the midst of a kernel core dump */
unsigned int not_in_kdp = 1;

unsigned int vm_map_set_cache_attr_count = 0;

kern_return_t
vm_map_set_cache_attr(
	vm_map_t	map,
	vm_map_offset_t	va)
{
	vm_map_entry_t	map_entry;
	vm_object_t	object;
	kern_return_t	kr = KERN_SUCCESS;

	vm_map_lock_read(map);

	if (!vm_map_lookup_entry(map, va, &map_entry) ||
	    map_entry->is_sub_map) {
		/*
		 * that memory is not properly mapped
		 */
		kr = KERN_INVALID_ARGUMENT;
		goto done;
	}
	object = VME_OBJECT(map_entry);

	if (object == VM_OBJECT_NULL) {
		/*
		 * there should be a VM object here at this point
		 */
		kr = KERN_INVALID_ARGUMENT;
		goto done;
	}
	vm_object_lock(object);
	object->set_cache_attr = TRUE;
	vm_object_unlock(object);

	vm_map_set_cache_attr_count++;
done:
	vm_map_unlock_read(map);

	return kr;
}


#if CONFIG_CODE_DECRYPTION
/*
 * vm_map_apple_protected:
 * This remaps the requested part of the object with an object backed by
 * the decrypting pager.
 * crypt_info contains entry points and session data for the crypt module.
 * The crypt_info block will be copied by vm_map_apple_protected. The data structures
 * referenced in crypt_info must remain valid until crypt_info->crypt_end() is called.
 */
kern_return_t
vm_map_apple_protected(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_object_offset_t	crypto_backing_offset,
	struct pager_crypt_info *crypt_info)
{
	boolean_t	map_locked;
	kern_return_t	kr;
	vm_map_entry_t	map_entry;
	struct vm_map_entry tmp_entry;
	memory_object_t	unprotected_mem_obj;
	vm_object_t	protected_object;
	vm_map_offset_t	map_addr;
	vm_map_offset_t	start_aligned, end_aligned;
	vm_object_offset_t	crypto_start, crypto_end;
	int		vm_flags;
	vm_map_kernel_flags_t vmk_flags;

	vm_flags = 0;
	vmk_flags = VM_MAP_KERNEL_FLAGS_NONE;

	map_locked = FALSE;
	unprotected_mem_obj = MEMORY_OBJECT_NULL;

	start_aligned = vm_map_trunc_page(start, PAGE_MASK_64);
	end_aligned = vm_map_round_page(end, PAGE_MASK_64);
	start_aligned = vm_map_trunc_page(start_aligned, VM_MAP_PAGE_MASK(map));
	end_aligned = vm_map_round_page(end_aligned, VM_MAP_PAGE_MASK(map));

#if __arm64__
	/*
	 * "start" and "end" might be 4K-aligned but not 16K-aligned,
	 * so we might have to loop and establish up to 3 mappings:
	 *
	 * + the first 16K-page, which might overlap with the previous
	 *   4K-aligned mapping,
	 * + the center,
	 * + the last 16K-page, which might overlap with the next
	 *   4K-aligned mapping.
	 * Each of these mapping might be backed by a vnode pager (if
	 * properly page-aligned) or a "fourk_pager", itself backed by a
	 * vnode pager (if 4K-aligned but not page-aligned).
	 */
#else /* __arm64__ */
	assert(start_aligned == start);
	assert(end_aligned == end);
#endif /* __arm64__ */

	map_addr = start_aligned;
	for (map_addr = start_aligned;
	     map_addr < end;
	     map_addr = tmp_entry.vme_end) {
		vm_map_lock(map);
		map_locked = TRUE;

		/* lookup the protected VM object */
		if (!vm_map_lookup_entry(map,
					 map_addr,
					 &map_entry) ||
		    map_entry->is_sub_map ||
		    VME_OBJECT(map_entry) == VM_OBJECT_NULL ||
		    !(map_entry->protection & VM_PROT_EXECUTE)) {
			/* that memory is not properly mapped */
			kr = KERN_INVALID_ARGUMENT;
			goto done;
		}

		/* get the protected object to be decrypted */
		protected_object = VME_OBJECT(map_entry);
		if (protected_object == VM_OBJECT_NULL) {
			/* there should be a VM object here at this point */
			kr = KERN_INVALID_ARGUMENT;
			goto done;
		}
		/* ensure protected object stays alive while map is unlocked */
		vm_object_reference(protected_object);

		/* limit the map entry to the area we want to cover */
		vm_map_clip_start(map, map_entry, start_aligned);
		vm_map_clip_end(map, map_entry, end_aligned);

		tmp_entry = *map_entry;
		map_entry = VM_MAP_ENTRY_NULL; /* not valid after unlocking map */
		vm_map_unlock(map);
		map_locked = FALSE;

		/*
		 * This map entry might be only partially encrypted
		 * (if not fully "page-aligned").
		 */
		crypto_start = 0;
		crypto_end = tmp_entry.vme_end - tmp_entry.vme_start;
		if (tmp_entry.vme_start < start) {
			if (tmp_entry.vme_start != start_aligned) {
				kr = KERN_INVALID_ADDRESS;
			}
			crypto_start += (start - tmp_entry.vme_start);
		}
		if (tmp_entry.vme_end > end) {
			if (tmp_entry.vme_end != end_aligned) {
				kr = KERN_INVALID_ADDRESS;
			}
			crypto_end -= (tmp_entry.vme_end - end);
		}

		/*
		 * This "extra backing offset" is needed to get the decryption
		 * routine to use the right key.  It adjusts for the possibly
		 * relative offset of an interposed "4K" pager...
		 */
		if (crypto_backing_offset == (vm_object_offset_t) -1) {
			crypto_backing_offset = VME_OFFSET(&tmp_entry);
		}

		/*
		 * Lookup (and create if necessary) the protected memory object
		 * matching that VM object.
		 * If successful, this also grabs a reference on the memory object,
		 * to guarantee that it doesn't go away before we get a chance to map
		 * it.
		 */
		unprotected_mem_obj = apple_protect_pager_setup(
			protected_object,
			VME_OFFSET(&tmp_entry),
			crypto_backing_offset,
			crypt_info,
			crypto_start,
			crypto_end);

		/* release extra ref on protected object */
		vm_object_deallocate(protected_object);

		if (unprotected_mem_obj == NULL) {
			kr = KERN_FAILURE;
			goto done;
		}

		vm_flags = VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE;
		/* can overwrite an immutable mapping */
		vmk_flags.vmkf_overwrite_immutable = TRUE;
#if __arm64__
		if (tmp_entry.used_for_jit &&
		    (VM_MAP_PAGE_SHIFT(map) != FOURK_PAGE_SHIFT ||
		     PAGE_SHIFT != FOURK_PAGE_SHIFT) &&
		    fourk_binary_compatibility_unsafe &&
		    fourk_binary_compatibility_allow_wx) {
			printf("** FOURK_COMPAT [%d]: "
			       "allowing write+execute at 0x%llx\n",
			       proc_selfpid(), tmp_entry.vme_start);
			vmk_flags.vmkf_map_jit = TRUE;
		}
#endif /* __arm64__ */

		/* map this memory object in place of the current one */
		map_addr = tmp_entry.vme_start;
		kr = vm_map_enter_mem_object(map,
					     &map_addr,
					     (tmp_entry.vme_end -
					      tmp_entry.vme_start),
					     (mach_vm_offset_t) 0,
					     vm_flags,
					     vmk_flags,
					     VM_KERN_MEMORY_NONE,
					     (ipc_port_t) unprotected_mem_obj,
					     0,
					     TRUE,
					     tmp_entry.protection,
					     tmp_entry.max_protection,
					     tmp_entry.inheritance);
		assertf(kr == KERN_SUCCESS,
			"kr = 0x%x\n", kr);
		assertf(map_addr == tmp_entry.vme_start,
			"map_addr=0x%llx vme_start=0x%llx tmp_entry=%p\n",
			(uint64_t)map_addr,
			(uint64_t) tmp_entry.vme_start,
			&tmp_entry);

#if VM_MAP_DEBUG_APPLE_PROTECT
		if (vm_map_debug_apple_protect) {
			printf("APPLE_PROTECT: map %p [0x%llx:0x%llx] pager %p:"
			       " backing:[object:%p,offset:0x%llx,"
			       "crypto_backing_offset:0x%llx,"
			       "crypto_start:0x%llx,crypto_end:0x%llx]\n",
			       map,
			       (uint64_t) map_addr,
			       (uint64_t) (map_addr + (tmp_entry.vme_end -
						       tmp_entry.vme_start)),
			       unprotected_mem_obj,
			       protected_object,
			       VME_OFFSET(&tmp_entry),
			       crypto_backing_offset,
			       crypto_start,
			       crypto_end);
		}
#endif /* VM_MAP_DEBUG_APPLE_PROTECT */

		/*
		 * Release the reference obtained by
		 * apple_protect_pager_setup().
		 * The mapping (if it succeeded) is now holding a reference on
		 * the memory object.
		 */
		memory_object_deallocate(unprotected_mem_obj);
		unprotected_mem_obj = MEMORY_OBJECT_NULL;

		/* continue with next map entry */
		crypto_backing_offset += (tmp_entry.vme_end -
					  tmp_entry.vme_start);
		crypto_backing_offset -= crypto_start;
	}
	kr = KERN_SUCCESS;

done:
	if (map_locked) {
		vm_map_unlock(map);
	}
	return kr;
}
#endif	/* CONFIG_CODE_DECRYPTION */


lck_grp_t		vm_map_lck_grp;
lck_grp_attr_t	vm_map_lck_grp_attr;
lck_attr_t		vm_map_lck_attr;
lck_attr_t		vm_map_lck_rw_attr;


/*
 *	vm_map_init:
 *
 *	Initialize the vm_map module.  Must be called before
 *	any other vm_map routines.
 *
 *	Map and entry structures are allocated from zones -- we must
 *	initialize those zones.
 *
 *	There are three zones of interest:
 *
 *	vm_map_zone:		used to allocate maps.
 *	vm_map_entry_zone:	used to allocate map entries.
 *	vm_map_entry_reserved_zone:	fallback zone for kernel map entries
 *
 *	The kernel allocates map entries from a special zone that is initially
 *	"crammed" with memory.  It would be difficult (perhaps impossible) for
 *	the kernel to allocate more memory to a entry zone when it became
 *	empty since the very act of allocating memory implies the creation
 *	of a new entry.
 */
void
vm_map_init(
	void)
{
	vm_size_t entry_zone_alloc_size;
	const char *mez_name = "VM map entries";

	vm_map_zone = zinit((vm_map_size_t) sizeof(struct _vm_map), 40*1024,
			    PAGE_SIZE, "maps");
	zone_change(vm_map_zone, Z_NOENCRYPT, TRUE);
#if	defined(__LP64__)
	entry_zone_alloc_size = PAGE_SIZE * 5;
#else
	entry_zone_alloc_size = PAGE_SIZE * 6;
#endif
	vm_map_entry_zone = zinit((vm_map_size_t) sizeof(struct vm_map_entry),
				  1024*1024, entry_zone_alloc_size,
				  mez_name);
	zone_change(vm_map_entry_zone, Z_NOENCRYPT, TRUE);
	zone_change(vm_map_entry_zone, Z_NOCALLOUT, TRUE);
	zone_change(vm_map_entry_zone, Z_GZALLOC_EXEMPT, TRUE);

	vm_map_entry_reserved_zone = zinit((vm_map_size_t) sizeof(struct vm_map_entry),
				   kentry_data_size * 64, kentry_data_size,
				   "Reserved VM map entries");
	zone_change(vm_map_entry_reserved_zone, Z_NOENCRYPT, TRUE);
	/* Don't quarantine because we always need elements available */
	zone_change(vm_map_entry_reserved_zone, Z_KASAN_QUARANTINE, FALSE);

	vm_map_copy_zone = zinit((vm_map_size_t) sizeof(struct vm_map_copy),
				 16*1024, PAGE_SIZE, "VM map copies");
	zone_change(vm_map_copy_zone, Z_NOENCRYPT, TRUE);

	vm_map_holes_zone = zinit((vm_map_size_t) sizeof(struct vm_map_links),
				 16*1024, PAGE_SIZE, "VM map holes");
	zone_change(vm_map_holes_zone, Z_NOENCRYPT, TRUE);

	/*
	 *	Cram the map and kentry zones with initial data.
	 *	Set reserved_zone non-collectible to aid zone_gc().
	 */
	zone_change(vm_map_zone, Z_COLLECT, FALSE);
	zone_change(vm_map_zone, Z_FOREIGN, TRUE);
        zone_change(vm_map_zone, Z_GZALLOC_EXEMPT, TRUE);

	zone_change(vm_map_entry_reserved_zone, Z_COLLECT, FALSE);
	zone_change(vm_map_entry_reserved_zone, Z_EXPAND, FALSE);
	zone_change(vm_map_entry_reserved_zone, Z_FOREIGN, TRUE);
	zone_change(vm_map_entry_reserved_zone, Z_NOCALLOUT, TRUE);
	zone_change(vm_map_entry_reserved_zone, Z_CALLERACCT, FALSE); /* don't charge caller */
	zone_change(vm_map_copy_zone, Z_CALLERACCT, FALSE); /* don't charge caller */
	zone_change(vm_map_entry_reserved_zone, Z_GZALLOC_EXEMPT, TRUE);

	zone_change(vm_map_holes_zone, Z_COLLECT, TRUE);
	zone_change(vm_map_holes_zone, Z_EXPAND, TRUE);
	zone_change(vm_map_holes_zone, Z_FOREIGN, TRUE);
	zone_change(vm_map_holes_zone, Z_NOCALLOUT, TRUE);
	zone_change(vm_map_holes_zone, Z_CALLERACCT, TRUE);
	zone_change(vm_map_holes_zone, Z_GZALLOC_EXEMPT, TRUE);

	/*
	 * Add the stolen memory to zones, adjust zone size and stolen counts.
	 * zcram only up to the maximum number of pages for each zone chunk.
	 */
	zcram(vm_map_zone, (vm_offset_t)map_data, map_data_size);

	const vm_size_t stride = ZONE_CHUNK_MAXPAGES * PAGE_SIZE;
	for (vm_offset_t off = 0; off < kentry_data_size; off += stride) {
		zcram(vm_map_entry_reserved_zone,
				(vm_offset_t)kentry_data + off,
				MIN(kentry_data_size - off, stride));
	}
	for (vm_offset_t off = 0; off < map_holes_data_size; off += stride) {
		zcram(vm_map_holes_zone,
				(vm_offset_t)map_holes_data + off,
				MIN(map_holes_data_size - off, stride));
	}

	VM_PAGE_MOVE_STOLEN(atop_64(map_data_size) + atop_64(kentry_data_size) + atop_64(map_holes_data_size));

	lck_grp_attr_setdefault(&vm_map_lck_grp_attr);
	lck_grp_init(&vm_map_lck_grp, "vm_map", &vm_map_lck_grp_attr);
	lck_attr_setdefault(&vm_map_lck_attr);

	lck_attr_setdefault(&vm_map_lck_rw_attr);
	lck_attr_cleardebug(&vm_map_lck_rw_attr);

#if VM_MAP_DEBUG_APPLE_PROTECT
	PE_parse_boot_argn("vm_map_debug_apple_protect",
			   &vm_map_debug_apple_protect,
			   sizeof(vm_map_debug_apple_protect));
#endif /* VM_MAP_DEBUG_APPLE_PROTECT */
#if VM_MAP_DEBUG_APPLE_FOURK
	PE_parse_boot_argn("vm_map_debug_fourk",
			   &vm_map_debug_fourk,
			   sizeof(vm_map_debug_fourk));
#endif /* VM_MAP_DEBUG_FOURK */
	PE_parse_boot_argn("vm_map_executable_immutable",
			   &vm_map_executable_immutable,
			   sizeof(vm_map_executable_immutable));
	PE_parse_boot_argn("vm_map_executable_immutable_no_log",
			   &vm_map_executable_immutable_no_log,
			   sizeof(vm_map_executable_immutable_no_log));
}

void
vm_map_steal_memory(
	void)
{
	uint32_t kentry_initial_pages;

	map_data_size = round_page(10 * sizeof(struct _vm_map));
	map_data = pmap_steal_memory(map_data_size);

	/*
	 * kentry_initial_pages corresponds to the number of kernel map entries
	 * required during bootstrap until the asynchronous replenishment
	 * scheme is activated and/or entries are available from the general
	 * map entry pool.
	 */
#if	defined(__LP64__)
	kentry_initial_pages = 10;
#else
	kentry_initial_pages = 6;
#endif

#if CONFIG_GZALLOC
	/* If using the guard allocator, reserve more memory for the kernel
	 * reserved map entry pool.
	*/
	if (gzalloc_enabled())
		kentry_initial_pages *= 1024;
#endif

	kentry_data_size = kentry_initial_pages * PAGE_SIZE;
	kentry_data = pmap_steal_memory(kentry_data_size);

	map_holes_data_size = kentry_data_size;
	map_holes_data = pmap_steal_memory(map_holes_data_size);
}

boolean_t vm_map_supports_hole_optimization = FALSE;

void
vm_kernel_reserved_entry_init(void) {
	zone_prio_refill_configure(vm_map_entry_reserved_zone, (6*PAGE_SIZE)/sizeof(struct vm_map_entry));

	/*
	 * Once we have our replenish thread set up, we can start using the vm_map_holes zone.
	 */
	zone_prio_refill_configure(vm_map_holes_zone, (6*PAGE_SIZE)/sizeof(struct vm_map_links));
	vm_map_supports_hole_optimization = TRUE;
}

void
vm_map_disable_hole_optimization(vm_map_t map)
{
	vm_map_entry_t	head_entry, hole_entry, next_hole_entry;

	if (map->holelistenabled) {

		head_entry = hole_entry = (vm_map_entry_t) map->holes_list;

		while (hole_entry != NULL) {

			next_hole_entry = hole_entry->vme_next;

			hole_entry->vme_next = NULL;
			hole_entry->vme_prev = NULL;
			zfree(vm_map_holes_zone, hole_entry);

			if (next_hole_entry == head_entry) {
				hole_entry = NULL;
			} else {
				hole_entry = next_hole_entry;
			}
		}

		map->holes_list = NULL;
		map->holelistenabled = FALSE;

		map->first_free = vm_map_first_entry(map);
		SAVE_HINT_HOLE_WRITE(map, NULL);
	}
}

boolean_t
vm_kernel_map_is_kernel(vm_map_t map) {
	return (map->pmap == kernel_pmap);
}

/*
 *	vm_map_create:
 *
 *	Creates and returns a new empty VM map with
 *	the given physical map structure, and having
 *	the given lower and upper address bounds.
 */

vm_map_t
vm_map_create(
	pmap_t			pmap,
	vm_map_offset_t	min,
	vm_map_offset_t	max,
	boolean_t		pageable)
{
	static int		color_seed = 0;
	vm_map_t	result;
	struct vm_map_links	*hole_entry = NULL;

	result = (vm_map_t) zalloc(vm_map_zone);
	if (result == VM_MAP_NULL)
		panic("vm_map_create");

	vm_map_first_entry(result) = vm_map_to_entry(result);
	vm_map_last_entry(result)  = vm_map_to_entry(result);
	result->hdr.nentries = 0;
	result->hdr.entries_pageable = pageable;

	vm_map_store_init( &(result->hdr) );

	result->hdr.page_shift = PAGE_SHIFT;

	result->size = 0;
	result->user_wire_limit = MACH_VM_MAX_ADDRESS;	/* default limit is unlimited */
	result->user_wire_size  = 0;
#if __x86_64__
	result->vmmap_high_start = 0;
#endif /* __x86_64__ */
	result->ref_count = 1;
#if	TASK_SWAPPER
	result->res_count = 1;
	result->sw_state = MAP_SW_IN;
#endif	/* TASK_SWAPPER */
	result->pmap = pmap;
	result->min_offset = min;
	result->max_offset = max;
	result->wiring_required = FALSE;
	result->no_zero_fill = FALSE;
	result->mapped_in_other_pmaps = FALSE;
	result->wait_for_space = FALSE;
	result->switch_protect = FALSE;
	result->disable_vmentry_reuse = FALSE;
	result->map_disallow_data_exec = FALSE;
	result->is_nested_map = FALSE;
	result->map_disallow_new_exec = FALSE;
	result->highest_entry_end = 0;
	result->first_free = vm_map_to_entry(result);
	result->hint = vm_map_to_entry(result);
	result->color_rr = (color_seed++) & vm_color_mask;
 	result->jit_entry_exists = FALSE;

	if (vm_map_supports_hole_optimization) {
		hole_entry = zalloc(vm_map_holes_zone);

		hole_entry->start = min;
#if defined(__arm__) || defined(__arm64__)
		hole_entry->end = result->max_offset;
#else
		hole_entry->end = (max > (vm_map_offset_t)MACH_VM_MAX_ADDRESS) ? max : (vm_map_offset_t)MACH_VM_MAX_ADDRESS;
#endif
		result->holes_list = result->hole_hint = hole_entry;
		hole_entry->prev = hole_entry->next = (vm_map_entry_t) hole_entry;
		result->holelistenabled = TRUE;

	} else {

		result->holelistenabled = FALSE;
	}

	vm_map_lock_init(result);
	lck_mtx_init_ext(&result->s_lock, &result->s_lock_ext, &vm_map_lck_grp, &vm_map_lck_attr);

	return(result);
}

/*
 *	vm_map_entry_create:	[ internal use only ]
 *
 *	Allocates a VM map entry for insertion in the
 *	given map (or map copy).  No fields are filled.
 */
#define	vm_map_entry_create(map, map_locked)	_vm_map_entry_create(&(map)->hdr, map_locked)

#define	vm_map_copy_entry_create(copy, map_locked)					\
	_vm_map_entry_create(&(copy)->cpy_hdr, map_locked)
unsigned reserved_zalloc_count, nonreserved_zalloc_count;

static vm_map_entry_t
_vm_map_entry_create(
	struct vm_map_header	*map_header, boolean_t __unused map_locked)
{
	zone_t	zone;
	vm_map_entry_t	entry;

	zone = vm_map_entry_zone;

	assert(map_header->entries_pageable ? !map_locked : TRUE);

	if (map_header->entries_pageable) {
		entry = (vm_map_entry_t) zalloc(zone);
	}
	else {
		entry = (vm_map_entry_t) zalloc_canblock(zone, FALSE);

		if (entry == VM_MAP_ENTRY_NULL) {
			zone = vm_map_entry_reserved_zone;
			entry = (vm_map_entry_t) zalloc(zone);
			OSAddAtomic(1, &reserved_zalloc_count);
		} else
			OSAddAtomic(1, &nonreserved_zalloc_count);
	}

	if (entry == VM_MAP_ENTRY_NULL)
		panic("vm_map_entry_create");
	entry->from_reserved_zone = (zone == vm_map_entry_reserved_zone);

	vm_map_store_update( (vm_map_t) NULL, entry, VM_MAP_ENTRY_CREATE);
#if	MAP_ENTRY_CREATION_DEBUG
	entry->vme_creation_maphdr = map_header;
	backtrace(&entry->vme_creation_bt[0],
	          (sizeof(entry->vme_creation_bt)/sizeof(uintptr_t)));
#endif
	return(entry);
}

/*
 *	vm_map_entry_dispose:	[ internal use only ]
 *
 *	Inverse of vm_map_entry_create.
 *
 * 	write map lock held so no need to
 *	do anything special to insure correctness
 * 	of the stores
 */
#define	vm_map_entry_dispose(map, entry)			\
	_vm_map_entry_dispose(&(map)->hdr, (entry))

#define	vm_map_copy_entry_dispose(map, entry) \
	_vm_map_entry_dispose(&(copy)->cpy_hdr, (entry))

static void
_vm_map_entry_dispose(
	struct vm_map_header	*map_header,
	vm_map_entry_t		entry)
{
	zone_t		zone;

	if (map_header->entries_pageable || !(entry->from_reserved_zone))
		zone = vm_map_entry_zone;
	else
		zone = vm_map_entry_reserved_zone;

	if (!map_header->entries_pageable) {
		if (zone == vm_map_entry_zone)
			OSAddAtomic(-1, &nonreserved_zalloc_count);
		else
			OSAddAtomic(-1, &reserved_zalloc_count);
	}

	zfree(zone, entry);
}

#if MACH_ASSERT
static boolean_t first_free_check = FALSE;
boolean_t
first_free_is_valid(
	vm_map_t	map)
{
	if (!first_free_check)
		return TRUE;

	return( first_free_is_valid_store( map ));
}
#endif /* MACH_ASSERT */


#define vm_map_copy_entry_link(copy, after_where, entry)		\
	_vm_map_store_entry_link(&(copy)->cpy_hdr, after_where, (entry))

#define vm_map_copy_entry_unlink(copy, entry)				\
	_vm_map_store_entry_unlink(&(copy)->cpy_hdr, (entry))

#if	MACH_ASSERT && TASK_SWAPPER
/*
 *	vm_map_res_reference:
 *
 *	Adds another valid residence count to the given map.
 *
 *	Map is locked so this function can be called from
 *	vm_map_swapin.
 *
 */
void vm_map_res_reference(vm_map_t map)
{
	/* assert map is locked */
	assert(map->res_count >= 0);
	assert(map->ref_count >= map->res_count);
	if (map->res_count == 0) {
		lck_mtx_unlock(&map->s_lock);
		vm_map_lock(map);
		vm_map_swapin(map);
		lck_mtx_lock(&map->s_lock);
		++map->res_count;
		vm_map_unlock(map);
	} else
		++map->res_count;
}

/*
 *	vm_map_reference_swap:
 *
 *	Adds valid reference and residence counts to the given map.
 *
 *	The map may not be in memory (i.e. zero residence count).
 *
 */
void vm_map_reference_swap(vm_map_t map)
{
	assert(map != VM_MAP_NULL);
	lck_mtx_lock(&map->s_lock);
	assert(map->res_count >= 0);
	assert(map->ref_count >= map->res_count);
	map->ref_count++;
	vm_map_res_reference(map);
	lck_mtx_unlock(&map->s_lock);
}

/*
 *	vm_map_res_deallocate:
 *
 *	Decrement residence count on a map; possibly causing swapout.
 *
 *	The map must be in memory (i.e. non-zero residence count).
 *
 *	The map is locked, so this function is callable from vm_map_deallocate.
 *
 */
void vm_map_res_deallocate(vm_map_t map)
{
	assert(map->res_count > 0);
	if (--map->res_count == 0) {
		lck_mtx_unlock(&map->s_lock);
		vm_map_lock(map);
		vm_map_swapout(map);
		vm_map_unlock(map);
		lck_mtx_lock(&map->s_lock);
	}
	assert(map->ref_count >= map->res_count);
}
#endif	/* MACH_ASSERT && TASK_SWAPPER */

/*
 *	vm_map_destroy:
 *
 *	Actually destroy a map.
 */
void
vm_map_destroy(
	vm_map_t	map,
	int		flags)
{
	vm_map_lock(map);

	/* final cleanup: no need to unnest shared region */
	flags |= VM_MAP_REMOVE_NO_UNNESTING;
	/* final cleanup: ok to remove immutable mappings */
	flags |= VM_MAP_REMOVE_IMMUTABLE;

	/* clean up regular map entries */
	(void) vm_map_delete(map, map->min_offset, map->max_offset,
			     flags, VM_MAP_NULL);
	/* clean up leftover special mappings (commpage, etc...) */
#if	!defined(__arm__) && !defined(__arm64__)
	(void) vm_map_delete(map, 0x0, 0xFFFFFFFFFFFFF000ULL,
			     flags, VM_MAP_NULL);
#endif /* !__arm__ && !__arm64__ */

	vm_map_disable_hole_optimization(map);
	vm_map_unlock(map);

	assert(map->hdr.nentries == 0);

	if(map->pmap)
		pmap_destroy(map->pmap);

	if (vm_map_lck_attr.lck_attr_val & LCK_ATTR_DEBUG) {
		/*
		 * If lock debugging is enabled the mutexes get tagged as LCK_MTX_TAG_INDIRECT.
		 * And this is regardless of whether the lck_mtx_ext_t is embedded in the
		 * structure or kalloc'ed via lck_mtx_init.
		 * An example is s_lock_ext within struct _vm_map.
		 *
		 * A lck_mtx_destroy on such a mutex will attempt a kfree and panic. We
		 * can add another tag to detect embedded vs alloc'ed indirect external
		 * mutexes but that'll be additional checks in the lock path and require
		 * updating dependencies for the old vs new tag.
		 *
		 * Since the kfree() is for LCK_MTX_TAG_INDIRECT mutexes and that tag is applied
		 * just when lock debugging is ON, we choose to forego explicitly destroying
		 * the vm_map mutex and rw lock and, as a consequence, will overflow the reference
		 * count on vm_map_lck_grp, which has no serious side-effect.
		 */
	} else {
		lck_rw_destroy(&(map)->lock, &vm_map_lck_grp);
		lck_mtx_destroy(&(map)->s_lock, &vm_map_lck_grp);
	}

	zfree(vm_map_zone, map);
}

/*
 * Returns pid of the task with the largest number of VM map entries.
 * Used in the zone-map-exhaustion jetsam path.
 */
pid_t
find_largest_process_vm_map_entries(void)
{
	pid_t victim_pid = -1;
	int max_vm_map_entries = 0;
	task_t task = TASK_NULL;
	queue_head_t *task_list = &tasks;

	lck_mtx_lock(&tasks_threads_lock);
	queue_iterate(task_list, task, task_t, tasks) {
		if (task == kernel_task || !task->active)
			continue;

		vm_map_t task_map = task->map;
		if (task_map != VM_MAP_NULL) {
			int task_vm_map_entries = task_map->hdr.nentries;
			if (task_vm_map_entries > max_vm_map_entries) {
				max_vm_map_entries = task_vm_map_entries;
				victim_pid = pid_from_task(task);
			}
		}
	}
	lck_mtx_unlock(&tasks_threads_lock);

	printf("zone_map_exhaustion: victim pid %d, vm region count: %d\n", victim_pid, max_vm_map_entries);
	return victim_pid;
}

#if	TASK_SWAPPER
/*
 * vm_map_swapin/vm_map_swapout
 *
 * Swap a map in and out, either referencing or releasing its resources.
 * These functions are internal use only; however, they must be exported
 * because they may be called from macros, which are exported.
 *
 * In the case of swapout, there could be races on the residence count,
 * so if the residence count is up, we return, assuming that a
 * vm_map_deallocate() call in the near future will bring us back.
 *
 * Locking:
 *	-- We use the map write lock for synchronization among races.
 *	-- The map write lock, and not the simple s_lock, protects the
 *	   swap state of the map.
 *	-- If a map entry is a share map, then we hold both locks, in
 *	   hierarchical order.
 *
 * Synchronization Notes:
 *	1) If a vm_map_swapin() call happens while swapout in progress, it
 *	will block on the map lock and proceed when swapout is through.
 *	2) A vm_map_reference() call at this time is illegal, and will
 *	cause a panic.  vm_map_reference() is only allowed on resident
 *	maps, since it refuses to block.
 *	3) A vm_map_swapin() call during a swapin will block, and
 *	proceeed when the first swapin is done, turning into a nop.
 *	This is the reason the res_count is not incremented until
 *	after the swapin is complete.
 *	4) There is a timing hole after the checks of the res_count, before
 *	the map lock is taken, during which a swapin may get the lock
 *	before a swapout about to happen.  If this happens, the swapin
 *	will detect the state and increment the reference count, causing
 *	the swapout to be a nop, thereby delaying it until a later
 *	vm_map_deallocate.  If the swapout gets the lock first, then
 *	the swapin will simply block until the swapout is done, and
 *	then proceed.
 *
 * Because vm_map_swapin() is potentially an expensive operation, it
 * should be used with caution.
 *
 * Invariants:
 *	1) A map with a residence count of zero is either swapped, or
 *	   being swapped.
 *	2) A map with a non-zero residence count is either resident,
 *	   or being swapped in.
 */

int vm_map_swap_enable = 1;

void vm_map_swapin (vm_map_t map)
{
	vm_map_entry_t entry;

	if (!vm_map_swap_enable)	/* debug */
		return;

	/*
	 * Map is locked
	 * First deal with various races.
	 */
	if (map->sw_state == MAP_SW_IN)
		/*
		 * we raced with swapout and won.  Returning will incr.
		 * the res_count, turning the swapout into a nop.
		 */
		return;

	/*
	 * The residence count must be zero.  If we raced with another
	 * swapin, the state would have been IN; if we raced with a
	 * swapout (after another competing swapin), we must have lost
	 * the race to get here (see above comment), in which case
	 * res_count is still 0.
	 */
	assert(map->res_count == 0);

	/*
	 * There are no intermediate states of a map going out or
	 * coming in, since the map is locked during the transition.
	 */
	assert(map->sw_state == MAP_SW_OUT);

	/*
	 * We now operate upon each map entry.  If the entry is a sub-
	 * or share-map, we call vm_map_res_reference upon it.
	 * If the entry is an object, we call vm_object_res_reference
	 * (this may iterate through the shadow chain).
	 * Note that we hold the map locked the entire time,
	 * even if we get back here via a recursive call in
	 * vm_map_res_reference.
	 */
	entry = vm_map_first_entry(map);

	while (entry != vm_map_to_entry(map)) {
		if (VME_OBJECT(entry) != VM_OBJECT_NULL) {
			if (entry->is_sub_map) {
				vm_map_t lmap = VME_SUBMAP(entry);
				lck_mtx_lock(&lmap->s_lock);
				vm_map_res_reference(lmap);
				lck_mtx_unlock(&lmap->s_lock);
			} else {
				vm_object_t object = VME_OBEJCT(entry);
				vm_object_lock(object);
				/*
				 * This call may iterate through the
				 * shadow chain.
				 */
				vm_object_res_reference(object);
				vm_object_unlock(object);
			}
		}
		entry = entry->vme_next;
	}
	assert(map->sw_state == MAP_SW_OUT);
	map->sw_state = MAP_SW_IN;
}

void vm_map_swapout(vm_map_t map)
{
	vm_map_entry_t entry;

	/*
	 * Map is locked
	 * First deal with various races.
	 * If we raced with a swapin and lost, the residence count
	 * will have been incremented to 1, and we simply return.
	 */
	lck_mtx_lock(&map->s_lock);
	if (map->res_count != 0) {
		lck_mtx_unlock(&map->s_lock);
		return;
	}
	lck_mtx_unlock(&map->s_lock);

	/*
	 * There are no intermediate states of a map going out or
	 * coming in, since the map is locked during the transition.
	 */
	assert(map->sw_state == MAP_SW_IN);

	if (!vm_map_swap_enable)
		return;

	/*
	 * We now operate upon each map entry.  If the entry is a sub-
	 * or share-map, we call vm_map_res_deallocate upon it.
	 * If the entry is an object, we call vm_object_res_deallocate
	 * (this may iterate through the shadow chain).
	 * Note that we hold the map locked the entire time,
	 * even if we get back here via a recursive call in
	 * vm_map_res_deallocate.
	 */
	entry = vm_map_first_entry(map);

	while (entry != vm_map_to_entry(map)) {
		if (VME_OBJECT(entry) != VM_OBJECT_NULL) {
			if (entry->is_sub_map) {
				vm_map_t lmap = VME_SUBMAP(entry);
				lck_mtx_lock(&lmap->s_lock);
				vm_map_res_deallocate(lmap);
				lck_mtx_unlock(&lmap->s_lock);
			} else {
				vm_object_t object = VME_OBJECT(entry);
				vm_object_lock(object);
				/*
				 * This call may take a long time,
				 * since it could actively push
				 * out pages (if we implement it
				 * that way).
				 */
				vm_object_res_deallocate(object);
				vm_object_unlock(object);
			}
		}
		entry = entry->vme_next;
	}
	assert(map->sw_state == MAP_SW_IN);
	map->sw_state = MAP_SW_OUT;
}

#endif	/* TASK_SWAPPER */

/*
 *	vm_map_lookup_entry:	[ internal use only ]
 *
 *	Calls into the vm map store layer to find the map
 *	entry containing (or immediately preceding) the
 *	specified address in the given map; the entry is returned
 *	in the "entry" parameter.  The boolean
 *	result indicates whether the address is
 *	actually contained in the map.
 */
boolean_t
vm_map_lookup_entry(
	vm_map_t		map,
	vm_map_offset_t	address,
	vm_map_entry_t		*entry)		/* OUT */
{
	return ( vm_map_store_lookup_entry( map, address, entry ));
}

/*
 *	Routine:	vm_map_find_space
 *	Purpose:
 *		Allocate a range in the specified virtual address map,
 *		returning the entry allocated for that range.
 *		Used by kmem_alloc, etc.
 *
 *		The map must be NOT be locked. It will be returned locked
 *		on KERN_SUCCESS, unlocked on failure.
 *
 *		If an entry is allocated, the object/offset fields
 *		are initialized to zero.
 */
kern_return_t
vm_map_find_space(
	vm_map_t	map,
	vm_map_offset_t		*address,	/* OUT */
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	int			flags __unused,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		tag,
	vm_map_entry_t		*o_entry)	/* OUT */
{
	vm_map_entry_t			entry, new_entry;
	vm_map_offset_t	start;
	vm_map_offset_t	end;
	vm_map_entry_t			hole_entry;

	if (size == 0) {
		*address = 0;
		return KERN_INVALID_ARGUMENT;
	}

	if (vmk_flags.vmkf_guard_after) {
		/* account for the back guard page in the size */
		size += VM_MAP_PAGE_SIZE(map);
	}

	new_entry = vm_map_entry_create(map, FALSE);

	/*
	 *	Look for the first possible address; if there's already
	 *	something at this address, we have to start after it.
	 */

	vm_map_lock(map);

	if( map->disable_vmentry_reuse == TRUE) {
		VM_MAP_HIGHEST_ENTRY(map, entry, start);
	} else {
		if (map->holelistenabled) {
			hole_entry = (vm_map_entry_t)map->holes_list;

			if (hole_entry == NULL) {
				/*
				 * No more space in the map?
				 */
				vm_map_entry_dispose(map, new_entry);
				vm_map_unlock(map);
				return(KERN_NO_SPACE);
			}

			entry = hole_entry;
			start = entry->vme_start;
		} else {
			assert(first_free_is_valid(map));
			if ((entry = map->first_free) == vm_map_to_entry(map))
				start = map->min_offset;
			else
				start = entry->vme_end;
		}
	}

	/*
	 *	In any case, the "entry" always precedes
	 *	the proposed new region throughout the loop:
	 */

	while (TRUE) {
		vm_map_entry_t	next;

		/*
		 *	Find the end of the proposed new region.
		 *	Be sure we didn't go beyond the end, or
		 *	wrap around the address.
		 */

		if (vmk_flags.vmkf_guard_before) {
			/* reserve space for the front guard page */
			start += VM_MAP_PAGE_SIZE(map);
		}
		end = ((start + mask) & ~mask);

		if (end < start) {
			vm_map_entry_dispose(map, new_entry);
			vm_map_unlock(map);
			return(KERN_NO_SPACE);
		}
		start = end;
		end += size;

		if ((end > map->max_offset) || (end < start)) {
			vm_map_entry_dispose(map, new_entry);
			vm_map_unlock(map);
			return(KERN_NO_SPACE);
		}

		next = entry->vme_next;

		if (map->holelistenabled) {
			if (entry->vme_end >= end)
				break;
		} else {
			/*
			 *	If there are no more entries, we must win.
			 *
			 *	OR
			 *
			 *	If there is another entry, it must be
			 *	after the end of the potential new region.
			 */

			if (next == vm_map_to_entry(map))
				break;

			if (next->vme_start >= end)
				break;
		}

		/*
		 *	Didn't fit -- move to the next entry.
		 */

		entry = next;

		if (map->holelistenabled) {
			if (entry == (vm_map_entry_t) map->holes_list) {
				/*
				 * Wrapped around
				 */
				vm_map_entry_dispose(map, new_entry);
				vm_map_unlock(map);
				return(KERN_NO_SPACE);
			}
			start = entry->vme_start;
		} else {
			start = entry->vme_end;
		}
	}

	if (map->holelistenabled) {
		if (vm_map_lookup_entry(map, entry->vme_start, &entry)) {
			panic("Found an existing entry (%p) instead of potential hole at address: 0x%llx.\n", entry, (unsigned long long)entry->vme_start);
		}
	}

	/*
	 *	At this point,
	 *		"start" and "end" should define the endpoints of the
	 *			available new range, and
	 *		"entry" should refer to the region before the new
	 *			range, and
	 *
	 *		the map should be locked.
	 */

	if (vmk_flags.vmkf_guard_before) {
		/* go back for the front guard page */
		start -= VM_MAP_PAGE_SIZE(map);
	}
	*address = start;

	assert(start < end);
	new_entry->vme_start = start;
	new_entry->vme_end = end;
	assert(page_aligned(new_entry->vme_start));
	assert(page_aligned(new_entry->vme_end));
	assert(VM_MAP_PAGE_ALIGNED(new_entry->vme_start,
				   VM_MAP_PAGE_MASK(map)));
	assert(VM_MAP_PAGE_ALIGNED(new_entry->vme_end,
				   VM_MAP_PAGE_MASK(map)));

	new_entry->is_shared = FALSE;
	new_entry->is_sub_map = FALSE;
	new_entry->use_pmap = TRUE;
	VME_OBJECT_SET(new_entry, VM_OBJECT_NULL);
	VME_OFFSET_SET(new_entry, (vm_object_offset_t) 0);

	new_entry->needs_copy = FALSE;

	new_entry->inheritance = VM_INHERIT_DEFAULT;
	new_entry->protection = VM_PROT_DEFAULT;
	new_entry->max_protection = VM_PROT_ALL;
	new_entry->behavior = VM_BEHAVIOR_DEFAULT;
	new_entry->wired_count = 0;
	new_entry->user_wired_count = 0;

	new_entry->in_transition = FALSE;
	new_entry->needs_wakeup = FALSE;
	new_entry->no_cache = FALSE;
	new_entry->permanent = FALSE;
	new_entry->superpage_size = FALSE;
	if (VM_MAP_PAGE_SHIFT(map) != PAGE_SHIFT) {
		new_entry->map_aligned = TRUE;
	} else {
		new_entry->map_aligned = FALSE;
	}

	new_entry->used_for_jit = FALSE;
	new_entry->zero_wired_pages = FALSE;
	new_entry->iokit_acct = FALSE;
	new_entry->vme_resilient_codesign = FALSE;
	new_entry->vme_resilient_media = FALSE;
	if (vmk_flags.vmkf_atomic_entry)
		new_entry->vme_atomic = TRUE;
	else
		new_entry->vme_atomic = FALSE;

	VME_ALIAS_SET(new_entry, tag);

	/*
	 *	Insert the new entry into the list
	 */

	vm_map_store_entry_link(map, entry, new_entry);

	map->size += size;

	/*
	 *	Update the lookup hint
	 */
	SAVE_HINT_MAP_WRITE(map, new_entry);

	*o_entry = new_entry;
	return(KERN_SUCCESS);
}

int vm_map_pmap_enter_print = FALSE;
int vm_map_pmap_enter_enable = FALSE;

/*
 *	Routine:	vm_map_pmap_enter [internal only]
 *
 *	Description:
 *		Force pages from the specified object to be entered into
 *		the pmap at the specified address if they are present.
 *		As soon as a page not found in the object the scan ends.
 *
 *	Returns:
 *		Nothing.
 *
 *	In/out conditions:
 *		The source map should not be locked on entry.
 */
__unused static void
vm_map_pmap_enter(
	vm_map_t		map,
	vm_map_offset_t		addr,
	vm_map_offset_t		end_addr,
	vm_object_t		object,
	vm_object_offset_t	offset,
	vm_prot_t		protection)
{
	int			type_of_fault;
	kern_return_t		kr;

	if(map->pmap == 0)
		return;

	while (addr < end_addr) {
		vm_page_t	m;


		/*
   		 * TODO:
		 * From vm_map_enter(), we come into this function without the map
		 * lock held or the object lock held.
		 * We haven't taken a reference on the object either.
		 * We should do a proper lookup on the map to make sure
		 * that things are sane before we go locking objects that
		 * could have been deallocated from under us.
		 */

		vm_object_lock(object);

		m = vm_page_lookup(object, offset);

		if (m == VM_PAGE_NULL || m->busy || m->fictitious ||
		    (m->unusual && ( m->error || m->restart || m->absent))) {
			vm_object_unlock(object);
			return;
		}

		if (vm_map_pmap_enter_print) {
			printf("vm_map_pmap_enter:");
			printf("map: %p, addr: %llx, object: %p, offset: %llx\n",
			       map, (unsigned long long)addr, object, (unsigned long long)offset);
		}
		type_of_fault = DBG_CACHE_HIT_FAULT;
		kr = vm_fault_enter(m, map->pmap, addr, protection, protection,
						    VM_PAGE_WIRED(m),
						    FALSE, /* change_wiring */
						    VM_KERN_MEMORY_NONE, /* tag - not wiring */
						    FALSE, /* no_cache */
						    FALSE, /* cs_bypass */
						    0,     /* XXX need user tag / alias? */
						    0,     /* pmap_options */
						    NULL,  /* need_retry */
						    &type_of_fault);

		vm_object_unlock(object);

		offset += PAGE_SIZE_64;
		addr += PAGE_SIZE;
	}
}

boolean_t vm_map_pmap_is_empty(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t end);
boolean_t vm_map_pmap_is_empty(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
#ifdef MACHINE_PMAP_IS_EMPTY
	return pmap_is_empty(map->pmap, start, end);
#else 	/* MACHINE_PMAP_IS_EMPTY */
	vm_map_offset_t	offset;
	ppnum_t		phys_page;

	if (map->pmap == NULL) {
		return TRUE;
	}

	for (offset = start;
	     offset < end;
	     offset += PAGE_SIZE) {
		phys_page = pmap_find_phys(map->pmap, offset);
		if (phys_page) {
			kprintf("vm_map_pmap_is_empty(%p,0x%llx,0x%llx): "
				"page %d at 0x%llx\n",
				map, (long long)start, (long long)end,
				phys_page, (long long)offset);
			return FALSE;
		}
	}
	return TRUE;
#endif	/* MACHINE_PMAP_IS_EMPTY */
}

#define MAX_TRIES_TO_GET_RANDOM_ADDRESS	1000
kern_return_t
vm_map_random_address_for_size(
	vm_map_t	map,
	vm_map_offset_t	*address,
	vm_map_size_t	size)
{
	kern_return_t	kr = KERN_SUCCESS;
	int		tries = 0;
	vm_map_offset_t	random_addr = 0;
	vm_map_offset_t hole_end;

	vm_map_entry_t	next_entry = VM_MAP_ENTRY_NULL;
	vm_map_entry_t	prev_entry = VM_MAP_ENTRY_NULL;
	vm_map_size_t	vm_hole_size = 0;
	vm_map_size_t	addr_space_size;

	addr_space_size = vm_map_max(map) - vm_map_min(map);

	assert(page_aligned(size));

	while (tries < MAX_TRIES_TO_GET_RANDOM_ADDRESS) {
		random_addr = ((vm_map_offset_t)random()) << PAGE_SHIFT;
		random_addr = vm_map_trunc_page(
			vm_map_min(map) +(random_addr % addr_space_size),
			VM_MAP_PAGE_MASK(map));

		if (vm_map_lookup_entry(map, random_addr, &prev_entry) == FALSE) {
			if (prev_entry == vm_map_to_entry(map)) {
				next_entry = vm_map_first_entry(map);
			} else {
				next_entry = prev_entry->vme_next;
			}
			if (next_entry == vm_map_to_entry(map)) {
				hole_end = vm_map_max(map);
			} else {
				hole_end = next_entry->vme_start;
			}
			vm_hole_size = hole_end - random_addr;
			if (vm_hole_size >= size) {
				*address = random_addr;
				break;
			}
		}
		tries++;
	}

	if (tries == MAX_TRIES_TO_GET_RANDOM_ADDRESS) {
		kr = KERN_NO_SPACE;
	}
	return kr;
}

/*
 *	Routine:	vm_map_enter
 *
 *	Description:
 *		Allocate a range in the specified virtual address map.
 *		The resulting range will refer to memory defined by
 *		the given memory object and offset into that object.
 *
 *		Arguments are as defined in the vm_map call.
 */
int _map_enter_debug = 0;
static unsigned int vm_map_enter_restore_successes = 0;
static unsigned int vm_map_enter_restore_failures = 0;
kern_return_t
vm_map_enter(
	vm_map_t		map,
	vm_map_offset_t		*address,	/* IN/OUT */
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	int			flags,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		alias,
	vm_object_t		object,
	vm_object_offset_t	offset,
	boolean_t		needs_copy,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_inherit_t		inheritance)
{
	vm_map_entry_t		entry, new_entry;
	vm_map_offset_t		start, tmp_start, tmp_offset;
	vm_map_offset_t		end, tmp_end;
	vm_map_offset_t		tmp2_start, tmp2_end;
	vm_map_offset_t		step;
	kern_return_t		result = KERN_SUCCESS;
	vm_map_t		zap_old_map = VM_MAP_NULL;
	vm_map_t		zap_new_map = VM_MAP_NULL;
	boolean_t		map_locked = FALSE;
	boolean_t		pmap_empty = TRUE;
	boolean_t		new_mapping_established = FALSE;
	boolean_t		keep_map_locked = vmk_flags.vmkf_keep_map_locked;
	boolean_t		anywhere = ((flags & VM_FLAGS_ANYWHERE) != 0);
	boolean_t		purgable = ((flags & VM_FLAGS_PURGABLE) != 0);
	boolean_t		overwrite = ((flags & VM_FLAGS_OVERWRITE) != 0);
	boolean_t		no_cache = ((flags & VM_FLAGS_NO_CACHE) != 0);
	boolean_t		is_submap = vmk_flags.vmkf_submap;
	boolean_t		permanent = vmk_flags.vmkf_permanent;
	boolean_t		entry_for_jit = vmk_flags.vmkf_map_jit;
	boolean_t		iokit_acct = vmk_flags.vmkf_iokit_acct;
	boolean_t		resilient_codesign = ((flags & VM_FLAGS_RESILIENT_CODESIGN) != 0);
	boolean_t		resilient_media = ((flags & VM_FLAGS_RESILIENT_MEDIA) != 0);
	boolean_t		random_address = ((flags & VM_FLAGS_RANDOM_ADDR) != 0);
	unsigned int		superpage_size = ((flags & VM_FLAGS_SUPERPAGE_MASK) >> VM_FLAGS_SUPERPAGE_SHIFT);
	vm_tag_t        	user_alias;
	vm_map_offset_t		effective_min_offset, effective_max_offset;
	kern_return_t		kr;
	boolean_t		clear_map_aligned = FALSE;
	vm_map_entry_t		hole_entry;
	vm_map_size_t		chunk_size = 0;

	assertf(vmk_flags.__vmkf_unused == 0, "vmk_flags unused=0x%x\n", vmk_flags.__vmkf_unused);

	if (flags & VM_FLAGS_4GB_CHUNK) {
#if defined(__LP64__)
		chunk_size = (4ULL * 1024 * 1024 * 1024); /* max. 4GB chunks for the new allocation */
#else /* __LP64__ */
		chunk_size = ANON_CHUNK_SIZE;
#endif /* __LP64__ */
	} else {
		chunk_size = ANON_CHUNK_SIZE;
	}

	if (superpage_size) {
		switch (superpage_size) {
			/*
			 * Note that the current implementation only supports
			 * a single size for superpages, SUPERPAGE_SIZE, per
			 * architecture. As soon as more sizes are supposed
			 * to be supported, SUPERPAGE_SIZE has to be replaced
			 * with a lookup of the size depending on superpage_size.
			 */
#ifdef __x86_64__
			case SUPERPAGE_SIZE_ANY:
				/* handle it like 2 MB and round up to page size */
				size = (size + 2*1024*1024 - 1) & ~(2*1024*1024 - 1);
			case SUPERPAGE_SIZE_2MB:
				break;
#endif
			default:
				return KERN_INVALID_ARGUMENT;
		}
		mask = SUPERPAGE_SIZE-1;
		if (size & (SUPERPAGE_SIZE-1))
			return KERN_INVALID_ARGUMENT;
		inheritance = VM_INHERIT_NONE;	/* fork() children won't inherit superpages */
	}


#if CONFIG_EMBEDDED
	if (cur_protection & VM_PROT_WRITE){
		if ((cur_protection & VM_PROT_EXECUTE) && !entry_for_jit){
			printf("EMBEDDED: %s: curprot cannot be write+execute. "
			       "turning off execute\n",
			       __FUNCTION__);
			cur_protection &= ~VM_PROT_EXECUTE;
		}
	}
#endif /* CONFIG_EMBEDDED */

	/*
	 * If the task has requested executable lockdown,
	 * deny any new executable mapping.
	 */
	if (map->map_disallow_new_exec == TRUE) {
		if (cur_protection & VM_PROT_EXECUTE) {
			return KERN_PROTECTION_FAILURE;
		}
	}

	if (resilient_codesign || resilient_media) {
		if ((cur_protection & (VM_PROT_WRITE | VM_PROT_EXECUTE)) ||
		    (max_protection & (VM_PROT_WRITE | VM_PROT_EXECUTE))) {
			return KERN_PROTECTION_FAILURE;
		}
	}

	if (is_submap) {
		if (purgable) {
			/* submaps can not be purgeable */
			return KERN_INVALID_ARGUMENT;
		}
		if (object == VM_OBJECT_NULL) {
			/* submaps can not be created lazily */
			return KERN_INVALID_ARGUMENT;
		}
	}
	if (vmk_flags.vmkf_already) {
		/*
		 * VM_FLAGS_ALREADY says that it's OK if the same mapping
		 * is already present.  For it to be meaningul, the requested
		 * mapping has to be at a fixed address (!VM_FLAGS_ANYWHERE) and
		 * we shouldn't try and remove what was mapped there first
		 * (!VM_FLAGS_OVERWRITE).
		 */
		if ((flags & VM_FLAGS_ANYWHERE) ||
		    (flags & VM_FLAGS_OVERWRITE)) {
			return KERN_INVALID_ARGUMENT;
		}
	}

	effective_min_offset = map->min_offset;

	if (vmk_flags.vmkf_beyond_max) {
		/*
		 * Allow an insertion beyond the map's max offset.
		 */
#if	!defined(__arm__) && !defined(__arm64__)
		if (vm_map_is_64bit(map))
			effective_max_offset = 0xFFFFFFFFFFFFF000ULL;
		else
#endif	/* __arm__ */
			effective_max_offset = 0x00000000FFFFF000ULL;
	} else {
		effective_max_offset = map->max_offset;
	}

	if (size == 0 ||
	    (offset & PAGE_MASK_64) != 0) {
		*address = 0;
		return KERN_INVALID_ARGUMENT;
	}

	if (map->pmap == kernel_pmap) {
		user_alias = VM_KERN_MEMORY_NONE;
	} else {
		user_alias = alias;
	}

#define	RETURN(value)	{ result = value; goto BailOut; }

	assert(page_aligned(*address));
	assert(page_aligned(size));

	if (!VM_MAP_PAGE_ALIGNED(size, VM_MAP_PAGE_MASK(map))) {
		/*
		 * In most cases, the caller rounds the size up to the
		 * map's page size.
		 * If we get a size that is explicitly not map-aligned here,
		 * we'll have to respect the caller's wish and mark the
		 * mapping as "not map-aligned" to avoid tripping the
		 * map alignment checks later.
		 */
		clear_map_aligned = TRUE;
	}
	if (!anywhere &&
	    !VM_MAP_PAGE_ALIGNED(*address, VM_MAP_PAGE_MASK(map))) {
		/*
		 * We've been asked to map at a fixed address and that
		 * address is not aligned to the map's specific alignment.
		 * The caller should know what it's doing (i.e. most likely
		 * mapping some fragmented copy map, transferring memory from
		 * a VM map with a different alignment), so clear map_aligned
		 * for this new VM map entry and proceed.
		 */
		clear_map_aligned = TRUE;
	}

	/*
	 * Only zero-fill objects are allowed to be purgable.
	 * LP64todo - limit purgable objects to 32-bits for now
	 */
	if (purgable &&
	    (offset != 0 ||
	     (object != VM_OBJECT_NULL &&
	      (object->vo_size != size ||
	       object->purgable == VM_PURGABLE_DENY))
	     || size > ANON_MAX_SIZE)) /* LP64todo: remove when dp capable */
		return KERN_INVALID_ARGUMENT;

	if (!anywhere && overwrite) {
		/*
		 * Create a temporary VM map to hold the old mappings in the
		 * affected area while we create the new one.
		 * This avoids releasing the VM map lock in
		 * vm_map_entry_delete() and allows atomicity
		 * when we want to replace some mappings with a new one.
		 * It also allows us to restore the old VM mappings if the
		 * new mapping fails.
		 */
		zap_old_map = vm_map_create(PMAP_NULL,
					    *address,
					    *address + size,
					    map->hdr.entries_pageable);
		vm_map_set_page_shift(zap_old_map, VM_MAP_PAGE_SHIFT(map));
		vm_map_disable_hole_optimization(zap_old_map);
	}

StartAgain: ;

	start = *address;

	if (anywhere) {
		vm_map_lock(map);
		map_locked = TRUE;

		if (entry_for_jit) {
			if (map->jit_entry_exists) {
				result = KERN_INVALID_ARGUMENT;
				goto BailOut;
			}
			random_address = TRUE;
		}

		if (random_address) {
			/*
			 * Get a random start address.
			 */
			result = vm_map_random_address_for_size(map, address, size);
			if (result != KERN_SUCCESS) {
				goto BailOut;
			}
			start = *address;
		}
#if __x86_64__
		else if ((start == 0 || start == vm_map_min(map)) &&
			 !map->disable_vmentry_reuse &&
			 map->vmmap_high_start != 0) {
			start = map->vmmap_high_start;
		}
#endif /* __x86_64__ */


		/*
		 *	Calculate the first possible address.
		 */

		if (start < effective_min_offset)
			start = effective_min_offset;
		if (start > effective_max_offset)
			RETURN(KERN_NO_SPACE);

		/*
		 *	Look for the first possible address;
		 *	if there's already something at this
		 *	address, we have to start after it.
		 */

		if( map->disable_vmentry_reuse == TRUE) {
			VM_MAP_HIGHEST_ENTRY(map, entry, start);
		} else {

			if (map->holelistenabled) {
				hole_entry = (vm_map_entry_t)map->holes_list;

				if (hole_entry == NULL) {
					/*
					 * No more space in the map?
					 */
					result = KERN_NO_SPACE;
					goto BailOut;
				} else {

					boolean_t found_hole = FALSE;

					do {
						if (hole_entry->vme_start >= start) {
							start = hole_entry->vme_start;
							found_hole = TRUE;
							break;
						}

						if (hole_entry->vme_end > start) {
							found_hole = TRUE;
							break;
						}
						hole_entry = hole_entry->vme_next;

					} while (hole_entry != (vm_map_entry_t) map->holes_list);

					if (found_hole == FALSE) {
						result = KERN_NO_SPACE;
						goto BailOut;
					}

					entry = hole_entry;

					if (start == 0)
						start += PAGE_SIZE_64;
				}
			} else {
				assert(first_free_is_valid(map));

				entry = map->first_free;

				if (entry == vm_map_to_entry(map)) {
					entry = NULL;
				} else {
				       if (entry->vme_next == vm_map_to_entry(map)){
					       /*
						* Hole at the end of the map.
						*/
						entry = NULL;
				       } else {
						if (start < (entry->vme_next)->vme_start ) {
							start = entry->vme_end;
							start = vm_map_round_page(start,
										  VM_MAP_PAGE_MASK(map));
						} else {
							/*
							 * Need to do a lookup.
							 */
							entry = NULL;
						}
				       }
				}

				if (entry == NULL) {
					vm_map_entry_t	tmp_entry;
					if (vm_map_lookup_entry(map, start, &tmp_entry)) {
						assert(!entry_for_jit);
						start = tmp_entry->vme_end;
						start = vm_map_round_page(start,
									  VM_MAP_PAGE_MASK(map));
					}
					entry = tmp_entry;
				}
			}
		}

		/*
		 *	In any case, the "entry" always precedes
		 *	the proposed new region throughout the
		 *	loop:
		 */

		while (TRUE) {
			vm_map_entry_t	next;

			/*
			 *	Find the end of the proposed new region.
			 *	Be sure we didn't go beyond the end, or
			 *	wrap around the address.
			 */

			end = ((start + mask) & ~mask);
			end = vm_map_round_page(end,
						VM_MAP_PAGE_MASK(map));
			if (end < start)
				RETURN(KERN_NO_SPACE);
			start = end;
			assert(VM_MAP_PAGE_ALIGNED(start,
						   VM_MAP_PAGE_MASK(map)));
			end += size;

			if ((end > effective_max_offset) || (end < start)) {
				if (map->wait_for_space) {
					assert(!keep_map_locked);
					if (size <= (effective_max_offset -
						     effective_min_offset)) {
						assert_wait((event_t)map,
							    THREAD_ABORTSAFE);
						vm_map_unlock(map);
						map_locked = FALSE;
						thread_block(THREAD_CONTINUE_NULL);
						goto StartAgain;
					}
				}
				RETURN(KERN_NO_SPACE);
			}

			next = entry->vme_next;

			if (map->holelistenabled) {
				if (entry->vme_end >= end)
					break;
			} else {
				/*
				 *	If there are no more entries, we must win.
				 *
				 *	OR
				 *
				 *	If there is another entry, it must be
				 *	after the end of the potential new region.
				 */

				if (next == vm_map_to_entry(map))
					break;

				if (next->vme_start >= end)
					break;
			}

			/*
			 *	Didn't fit -- move to the next entry.
			 */

			entry = next;

			if (map->holelistenabled) {
				if (entry == (vm_map_entry_t) map->holes_list) {
					/*
					 * Wrapped around
					 */
					result = KERN_NO_SPACE;
					goto BailOut;
				}
				start = entry->vme_start;
			} else {
				start = entry->vme_end;
			}

			start = vm_map_round_page(start,
						  VM_MAP_PAGE_MASK(map));
		}

		if (map->holelistenabled) {
			if (vm_map_lookup_entry(map, entry->vme_start, &entry)) {
				panic("Found an existing entry (%p) instead of potential hole at address: 0x%llx.\n", entry, (unsigned long long)entry->vme_start);
			}
		}

		*address = start;
		assert(VM_MAP_PAGE_ALIGNED(*address,
					   VM_MAP_PAGE_MASK(map)));
	} else {
		/*
		 *	Verify that:
		 *		the address doesn't itself violate
		 *		the mask requirement.
		 */

		vm_map_lock(map);
		map_locked = TRUE;
		if ((start & mask) != 0)
			RETURN(KERN_NO_SPACE);

		/*
		 *	...	the address is within bounds
		 */

		end = start + size;

		if ((start < effective_min_offset) ||
		    (end > effective_max_offset) ||
		    (start >= end)) {
			RETURN(KERN_INVALID_ADDRESS);
		}

		if (overwrite && zap_old_map != VM_MAP_NULL) {
			int remove_flags;
			/*
			 * Fixed mapping and "overwrite" flag: attempt to
			 * remove all existing mappings in the specified
			 * address range, saving them in our "zap_old_map".
			 */
			remove_flags = VM_MAP_REMOVE_SAVE_ENTRIES;
			remove_flags |= VM_MAP_REMOVE_NO_MAP_ALIGN;
			if (vmk_flags.vmkf_overwrite_immutable) {
				/* we can overwrite immutable mappings */
				remove_flags |= VM_MAP_REMOVE_IMMUTABLE;
			}
			(void) vm_map_delete(map, start, end,
					     remove_flags,
					     zap_old_map);
		}

		/*
		 *	...	the starting address isn't allocated
		 */

		if (vm_map_lookup_entry(map, start, &entry)) {
			if (! (vmk_flags.vmkf_already)) {
				RETURN(KERN_NO_SPACE);
			}
			/*
			 * Check if what's already there is what we want.
			 */
			tmp_start = start;
			tmp_offset = offset;
			if (entry->vme_start < start) {
				tmp_start -= start - entry->vme_start;
				tmp_offset -= start - entry->vme_start;

			}
			for (; entry->vme_start < end;
			     entry = entry->vme_next) {
				/*
				 * Check if the mapping's attributes
				 * match the existing map entry.
				 */
				if (entry == vm_map_to_entry(map) ||
				    entry->vme_start != tmp_start ||
				    entry->is_sub_map != is_submap ||
				    VME_OFFSET(entry) != tmp_offset ||
				    entry->needs_copy != needs_copy ||
				    entry->protection != cur_protection ||
				    entry->max_protection != max_protection ||
				    entry->inheritance != inheritance ||
				    entry->iokit_acct != iokit_acct ||
				    VME_ALIAS(entry) != alias) {
					/* not the same mapping ! */
					RETURN(KERN_NO_SPACE);
				}
				/*
				 * Check if the same object is being mapped.
				 */
				if (is_submap) {
					if (VME_SUBMAP(entry) !=
					    (vm_map_t) object) {
						/* not the same submap */
						RETURN(KERN_NO_SPACE);
					}
				} else {
					if (VME_OBJECT(entry) != object) {
						/* not the same VM object... */
						vm_object_t obj2;

						obj2 = VME_OBJECT(entry);
						if ((obj2 == VM_OBJECT_NULL ||
						     obj2->internal) &&
						    (object == VM_OBJECT_NULL ||
						     object->internal)) {
							/*
							 * ... but both are
							 * anonymous memory,
							 * so equivalent.
							 */
						} else {
							RETURN(KERN_NO_SPACE);
						}
					}
				}

				tmp_offset += entry->vme_end - entry->vme_start;
				tmp_start += entry->vme_end - entry->vme_start;
				if (entry->vme_end >= end) {
					/* reached the end of our mapping */
					break;
				}
			}
			/* it all matches:  let's use what's already there ! */
			RETURN(KERN_MEMORY_PRESENT);
		}

		/*
		 *	...	the next region doesn't overlap the
		 *		end point.
		 */

		if ((entry->vme_next != vm_map_to_entry(map)) &&
		    (entry->vme_next->vme_start < end))
			RETURN(KERN_NO_SPACE);
	}

	/*
	 *	At this point,
	 *		"start" and "end" should define the endpoints of the
	 *			available new range, and
	 *		"entry" should refer to the region before the new
	 *			range, and
	 *
	 *		the map should be locked.
	 */

	/*
	 *	See whether we can avoid creating a new entry (and object) by
	 *	extending one of our neighbors.  [So far, we only attempt to
	 *	extend from below.]  Note that we can never extend/join
	 *	purgable objects because they need to remain distinct
	 *	entities in order to implement their "volatile object"
	 *	semantics.
	 */

	if (purgable || entry_for_jit) {
		if (object == VM_OBJECT_NULL) {

			object = vm_object_allocate(size);
			object->copy_strategy = MEMORY_OBJECT_COPY_NONE;
			object->true_share = TRUE;
			if (purgable) {
				task_t owner;
				object->purgable = VM_PURGABLE_NONVOLATILE;
				if (map->pmap == kernel_pmap) {
					/*
					 * Purgeable mappings made in a kernel
					 * map are "owned" by the kernel itself
					 * rather than the current user task
					 * because they're likely to be used by
					 * more than this user task (see
					 * execargs_purgeable_allocate(), for
					 * example).
					 */
					owner = kernel_task;
				} else {
					owner = current_task();
				}
				assert(object->vo_purgeable_owner == NULL);
				assert(object->resident_page_count == 0);
				assert(object->wired_page_count == 0);
				vm_object_lock(object);
				vm_purgeable_nonvolatile_enqueue(object, owner);
				vm_object_unlock(object);
			}
			offset = (vm_object_offset_t)0;
		}
	} else if ((is_submap == FALSE) &&
		   (object == VM_OBJECT_NULL) &&
		   (entry != vm_map_to_entry(map)) &&
		   (entry->vme_end == start) &&
		   (!entry->is_shared) &&
		   (!entry->is_sub_map) &&
		   (!entry->in_transition) &&
		   (!entry->needs_wakeup) &&
		   (entry->behavior == VM_BEHAVIOR_DEFAULT) &&
		   (entry->protection == cur_protection) &&
		   (entry->max_protection == max_protection) &&
		   (entry->inheritance == inheritance) &&
		   ((user_alias == VM_MEMORY_REALLOC) ||
		    (VME_ALIAS(entry) == alias)) &&
		   (entry->no_cache == no_cache) &&
		   (entry->permanent == permanent) &&
		   /* no coalescing for immutable executable mappings */
		   !((entry->protection & VM_PROT_EXECUTE) &&
		     entry->permanent) &&
		   (!entry->superpage_size && !superpage_size) &&
		   /*
		    * No coalescing if not map-aligned, to avoid propagating
		    * that condition any further than needed:
		    */
		   (!entry->map_aligned || !clear_map_aligned) &&
		   (!entry->zero_wired_pages) &&
		   (!entry->used_for_jit && !entry_for_jit) &&
		   (entry->iokit_acct == iokit_acct) &&
		   (!entry->vme_resilient_codesign) &&
		   (!entry->vme_resilient_media) &&
		   (!entry->vme_atomic) &&

		   ((entry->vme_end - entry->vme_start) + size <=
		    (user_alias == VM_MEMORY_REALLOC ?
		     ANON_CHUNK_SIZE :
		     NO_COALESCE_LIMIT)) &&

		   (entry->wired_count == 0)) { /* implies user_wired_count == 0 */
		if (vm_object_coalesce(VME_OBJECT(entry),
				       VM_OBJECT_NULL,
				       VME_OFFSET(entry),
				       (vm_object_offset_t) 0,
				       (vm_map_size_t)(entry->vme_end - entry->vme_start),
				       (vm_map_size_t)(end - entry->vme_end))) {

			/*
			 *	Coalesced the two objects - can extend
			 *	the previous map entry to include the
			 *	new range.
			 */
			map->size += (end - entry->vme_end);
			assert(entry->vme_start < end);
			assert(VM_MAP_PAGE_ALIGNED(end,
						   VM_MAP_PAGE_MASK(map)));
			if (__improbable(vm_debug_events))
				DTRACE_VM5(map_entry_extend, vm_map_t, map, vm_map_entry_t, entry, vm_address_t, entry->vme_start, vm_address_t, entry->vme_end, vm_address_t, end);
			entry->vme_end = end;
			if (map->holelistenabled) {
				vm_map_store_update_first_free(map, entry, TRUE);
			} else {
				vm_map_store_update_first_free(map, map->first_free, TRUE);
			}
			new_mapping_established = TRUE;
			RETURN(KERN_SUCCESS);
		}
	}

	step = superpage_size ? SUPERPAGE_SIZE : (end - start);
	new_entry = NULL;

	for (tmp2_start = start; tmp2_start<end; tmp2_start += step) {
		tmp2_end = tmp2_start + step;
		/*
		 *	Create a new entry
		 *
		 * XXX FBDP
		 * The reserved "page zero" in each process's address space can
		 * be arbitrarily large.  Splitting it into separate objects and
		 * therefore different VM map entries serves no purpose and just
		 * slows down operations on the VM map, so let's not split the
		 * allocation into chunks if the max protection is NONE.  That
		 * memory should never be accessible, so it will never get to the
		 * default pager.
		 */
		tmp_start = tmp2_start;
		if (object == VM_OBJECT_NULL &&
		    size > chunk_size &&
		    max_protection != VM_PROT_NONE &&
		    superpage_size == 0)
			tmp_end = tmp_start + chunk_size;
		else
			tmp_end = tmp2_end;
		do {
			new_entry = vm_map_entry_insert(
				map, entry, tmp_start, tmp_end,
				object,	offset, needs_copy,
				FALSE, FALSE,
				cur_protection, max_protection,
				VM_BEHAVIOR_DEFAULT,
				(entry_for_jit)? VM_INHERIT_NONE: inheritance,
				0,
				no_cache,
				permanent,
				superpage_size,
				clear_map_aligned,
				is_submap,
				entry_for_jit,
				alias);

			assert((object != kernel_object) || (VM_KERN_MEMORY_NONE != alias));

			if (resilient_codesign &&
			    ! ((cur_protection | max_protection) &
			       (VM_PROT_WRITE | VM_PROT_EXECUTE))) {
				new_entry->vme_resilient_codesign = TRUE;
			}

			if (resilient_media &&
			    ! ((cur_protection | max_protection) &
			       (VM_PROT_WRITE | VM_PROT_EXECUTE))) {
				new_entry->vme_resilient_media = TRUE;
			}

			assert(!new_entry->iokit_acct);
			if (!is_submap &&
			    object != VM_OBJECT_NULL &&
			    object->purgable != VM_PURGABLE_DENY) {
				assert(new_entry->use_pmap);
				assert(!new_entry->iokit_acct);
				/*
				 * Turn off pmap accounting since
				 * purgeable objects have their
				 * own ledgers.
				 */
				new_entry->use_pmap = FALSE;
			} else if (!is_submap &&
				   iokit_acct &&
				   object != VM_OBJECT_NULL &&
				   object->internal) {
				/* alternate accounting */
				assert(!new_entry->iokit_acct);
				assert(new_entry->use_pmap);
				new_entry->iokit_acct = TRUE;
				new_entry->use_pmap = FALSE;
				DTRACE_VM4(
					vm_map_iokit_mapped_region,
					vm_map_t, map,
					vm_map_offset_t, new_entry->vme_start,
					vm_map_offset_t, new_entry->vme_end,
					int, VME_ALIAS(new_entry));
				vm_map_iokit_mapped_region(
					map,
					(new_entry->vme_end -
					 new_entry->vme_start));
			} else if (!is_submap) {
				assert(!new_entry->iokit_acct);
				assert(new_entry->use_pmap);
			}

			if (is_submap) {
				vm_map_t	submap;
				boolean_t	submap_is_64bit;
				boolean_t	use_pmap;

				assert(new_entry->is_sub_map);
				assert(!new_entry->use_pmap);
				assert(!new_entry->iokit_acct);
				submap = (vm_map_t) object;
				submap_is_64bit = vm_map_is_64bit(submap);
				use_pmap = (user_alias == VM_MEMORY_SHARED_PMAP);
#ifndef NO_NESTED_PMAP
				if (use_pmap && submap->pmap == NULL) {
					ledger_t ledger = map->pmap->ledger;
					/* we need a sub pmap to nest... */
					submap->pmap = pmap_create(ledger, 0,
					    submap_is_64bit);
					if (submap->pmap == NULL) {
						/* let's proceed without nesting... */
					}
#if	defined(__arm__) || defined(__arm64__)
					else {
						pmap_set_nested(submap->pmap);
					}
#endif
				}
				if (use_pmap && submap->pmap != NULL) {
					kr = pmap_nest(map->pmap,
						       submap->pmap,
						       tmp_start,
						       tmp_start,
						       tmp_end - tmp_start);
					if (kr != KERN_SUCCESS) {
						printf("vm_map_enter: "
						       "pmap_nest(0x%llx,0x%llx) "
						       "error 0x%x\n",
						       (long long)tmp_start,
						       (long long)tmp_end,
						       kr);
					} else {
						/* we're now nested ! */
						new_entry->use_pmap = TRUE;
						pmap_empty = FALSE;
					}
				}
#endif /* NO_NESTED_PMAP */
			}
			entry = new_entry;

			if (superpage_size) {
				vm_page_t pages, m;
				vm_object_t sp_object;
				vm_object_offset_t sp_offset;

				VME_OFFSET_SET(entry, 0);

				/* allocate one superpage */
				kr = cpm_allocate(SUPERPAGE_SIZE, &pages, 0, SUPERPAGE_NBASEPAGES-1, TRUE, 0);
				if (kr != KERN_SUCCESS) {
					/* deallocate whole range... */
					new_mapping_established = TRUE;
					/* ... but only up to "tmp_end" */
					size -= end - tmp_end;
					RETURN(kr);
				}

				/* create one vm_object per superpage */
				sp_object = vm_object_allocate((vm_map_size_t)(entry->vme_end - entry->vme_start));
				sp_object->phys_contiguous = TRUE;
				sp_object->vo_shadow_offset = (vm_object_offset_t)VM_PAGE_GET_PHYS_PAGE(pages)*PAGE_SIZE;
				VME_OBJECT_SET(entry, sp_object);
				assert(entry->use_pmap);

				/* enter the base pages into the object */
				vm_object_lock(sp_object);
				for (sp_offset = 0;
				     sp_offset < SUPERPAGE_SIZE;
				     sp_offset += PAGE_SIZE) {
					m = pages;
					pmap_zero_page(VM_PAGE_GET_PHYS_PAGE(m));
					pages = NEXT_PAGE(m);
					*(NEXT_PAGE_PTR(m)) = VM_PAGE_NULL;
					vm_page_insert_wired(m, sp_object, sp_offset, VM_KERN_MEMORY_OSFMK);
				}
				vm_object_unlock(sp_object);
			}
		} while (tmp_end != tmp2_end &&
			 (tmp_start = tmp_end) &&
			 (tmp_end = (tmp2_end - tmp_end > chunk_size) ?
			  tmp_end + chunk_size : tmp2_end));
	}

	new_mapping_established = TRUE;

BailOut:
	assert(map_locked == TRUE);

	if (result == KERN_SUCCESS) {
		vm_prot_t pager_prot;
		memory_object_t pager;

#if DEBUG
		if (pmap_empty &&
		    !(vmk_flags.vmkf_no_pmap_check)) {
			assert(vm_map_pmap_is_empty(map,
						    *address,
						    *address+size));
		}
#endif /* DEBUG */

		/*
		 * For "named" VM objects, let the pager know that the
		 * memory object is being mapped.  Some pagers need to keep
		 * track of this, to know when they can reclaim the memory
		 * object, for example.
		 * VM calls memory_object_map() for each mapping (specifying
		 * the protection of each mapping) and calls
		 * memory_object_last_unmap() when all the mappings are gone.
		 */
		pager_prot = max_protection;
		if (needs_copy) {
			/*
			 * Copy-On-Write mapping: won't modify
			 * the memory object.
			 */
			pager_prot &= ~VM_PROT_WRITE;
		}
		if (!is_submap &&
		    object != VM_OBJECT_NULL &&
		    object->named &&
		    object->pager != MEMORY_OBJECT_NULL) {
			vm_object_lock(object);
			pager = object->pager;
			if (object->named &&
			    pager != MEMORY_OBJECT_NULL) {
				assert(object->pager_ready);
				vm_object_mapping_wait(object, THREAD_UNINT);
				vm_object_mapping_begin(object);
				vm_object_unlock(object);

				kr = memory_object_map(pager, pager_prot);
				assert(kr == KERN_SUCCESS);

				vm_object_lock(object);
				vm_object_mapping_end(object);
			}
			vm_object_unlock(object);
		}
	}

	assert(map_locked == TRUE);

	if (!keep_map_locked) {
		vm_map_unlock(map);
		map_locked = FALSE;
	}

	/*
	 * We can't hold the map lock if we enter this block.
	 */

	if (result == KERN_SUCCESS) {

		/*	Wire down the new entry if the user
		 *	requested all new map entries be wired.
		 */
		if ((map->wiring_required)||(superpage_size)) {
			assert(!keep_map_locked);
			pmap_empty = FALSE; /* pmap won't be empty */
			kr = vm_map_wire_kernel(map, start, end,
					     new_entry->protection, VM_KERN_MEMORY_MLOCK,
					     TRUE);
			result = kr;
		}

	}

	if (result != KERN_SUCCESS) {
		if (new_mapping_established) {
			/*
			 * We have to get rid of the new mappings since we
			 * won't make them available to the user.
			 * Try and do that atomically, to minimize the risk
			 * that someone else create new mappings that range.
			 */
			zap_new_map = vm_map_create(PMAP_NULL,
						    *address,
						    *address + size,
						    map->hdr.entries_pageable);
			vm_map_set_page_shift(zap_new_map,
					      VM_MAP_PAGE_SHIFT(map));
			vm_map_disable_hole_optimization(zap_new_map);

			if (!map_locked) {
				vm_map_lock(map);
				map_locked = TRUE;
			}
			(void) vm_map_delete(map, *address, *address+size,
					     (VM_MAP_REMOVE_SAVE_ENTRIES |
					      VM_MAP_REMOVE_NO_MAP_ALIGN),
					     zap_new_map);
		}
		if (zap_old_map != VM_MAP_NULL &&
		    zap_old_map->hdr.nentries != 0) {
			vm_map_entry_t	entry1, entry2;

			/*
			 * The new mapping failed.  Attempt to restore
			 * the old mappings, saved in the "zap_old_map".
			 */
			if (!map_locked) {
				vm_map_lock(map);
				map_locked = TRUE;
			}

			/* first check if the coast is still clear */
			start = vm_map_first_entry(zap_old_map)->vme_start;
			end = vm_map_last_entry(zap_old_map)->vme_end;
			if (vm_map_lookup_entry(map, start, &entry1) ||
			    vm_map_lookup_entry(map, end, &entry2) ||
			    entry1 != entry2) {
				/*
				 * Part of that range has already been
				 * re-mapped:  we can't restore the old
				 * mappings...
				 */
				vm_map_enter_restore_failures++;
			} else {
				/*
				 * Transfer the saved map entries from
				 * "zap_old_map" to the original "map",
				 * inserting them all after "entry1".
				 */
				for (entry2 = vm_map_first_entry(zap_old_map);
				     entry2 != vm_map_to_entry(zap_old_map);
				     entry2 = vm_map_first_entry(zap_old_map)) {
					vm_map_size_t entry_size;

					entry_size = (entry2->vme_end -
						      entry2->vme_start);
					vm_map_store_entry_unlink(zap_old_map,
							    entry2);
					zap_old_map->size -= entry_size;
					vm_map_store_entry_link(map, entry1, entry2);
					map->size += entry_size;
					entry1 = entry2;
				}
				if (map->wiring_required) {
					/*
					 * XXX TODO: we should rewire the
					 * old pages here...
					 */
				}
				vm_map_enter_restore_successes++;
			}
		}
	}

	/*
	 * The caller is responsible for releasing the lock if it requested to
	 * keep the map locked.
	 */
	if (map_locked && !keep_map_locked) {
		vm_map_unlock(map);
	}

	/*
	 * Get rid of the "zap_maps" and all the map entries that
	 * they may still contain.
	 */
	if (zap_old_map != VM_MAP_NULL) {
		vm_map_destroy(zap_old_map, VM_MAP_REMOVE_NO_PMAP_CLEANUP);
		zap_old_map = VM_MAP_NULL;
	}
	if (zap_new_map != VM_MAP_NULL) {
		vm_map_destroy(zap_new_map, VM_MAP_REMOVE_NO_PMAP_CLEANUP);
		zap_new_map = VM_MAP_NULL;
	}

	return result;

#undef	RETURN
}

#if __arm64__
extern const struct memory_object_pager_ops fourk_pager_ops;
kern_return_t
vm_map_enter_fourk(
	vm_map_t		map,
	vm_map_offset_t		*address,	/* IN/OUT */
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	int			flags,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		alias,
	vm_object_t		object,
	vm_object_offset_t	offset,
	boolean_t		needs_copy,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_inherit_t		inheritance)
{
	vm_map_entry_t		entry, new_entry;
	vm_map_offset_t		start, fourk_start;
	vm_map_offset_t		end, fourk_end;
	vm_map_size_t		fourk_size;
	kern_return_t		result = KERN_SUCCESS;
	vm_map_t		zap_old_map = VM_MAP_NULL;
	vm_map_t		zap_new_map = VM_MAP_NULL;
	boolean_t		map_locked = FALSE;
	boolean_t		pmap_empty = TRUE;
	boolean_t		new_mapping_established = FALSE;
	boolean_t		keep_map_locked = vmk_flags.vmkf_keep_map_locked;
	boolean_t		anywhere = ((flags & VM_FLAGS_ANYWHERE) != 0);
	boolean_t		purgable = ((flags & VM_FLAGS_PURGABLE) != 0);
	boolean_t		overwrite = ((flags & VM_FLAGS_OVERWRITE) != 0);
	boolean_t		no_cache = ((flags & VM_FLAGS_NO_CACHE) != 0);
	boolean_t		is_submap = vmk_flags.vmkf_submap;
	boolean_t		permanent = vmk_flags.vmkf_permanent;
	boolean_t		entry_for_jit = vmk_flags.vmkf_map_jit;
//	boolean_t		iokit_acct = vmk_flags.vmkf_iokit_acct;
	unsigned int		superpage_size = ((flags & VM_FLAGS_SUPERPAGE_MASK) >> VM_FLAGS_SUPERPAGE_SHIFT);
	vm_map_offset_t		effective_min_offset, effective_max_offset;
	kern_return_t		kr;
	boolean_t		clear_map_aligned = FALSE;
	memory_object_t		fourk_mem_obj;
	vm_object_t		fourk_object;
	vm_map_offset_t		fourk_pager_offset;
	int			fourk_pager_index_start, fourk_pager_index_num;
	int			cur_idx;
	boolean_t		fourk_copy;
	vm_object_t		copy_object;
	vm_object_offset_t	copy_offset;

	fourk_mem_obj = MEMORY_OBJECT_NULL;
	fourk_object = VM_OBJECT_NULL;

	if (superpage_size) {
		return KERN_NOT_SUPPORTED;
	}

#if CONFIG_EMBEDDED
	if (cur_protection & VM_PROT_WRITE) {
		if ((cur_protection & VM_PROT_EXECUTE) &&
		    !entry_for_jit) {
			printf("EMBEDDED: %s: curprot cannot be write+execute. "
			       "turning off execute\n",
			       __FUNCTION__);
			cur_protection &= ~VM_PROT_EXECUTE;
		}
	}
#endif /* CONFIG_EMBEDDED */

	/*
	 * If the task has requested executable lockdown,
	 * deny any new executable mapping.
	 */
	if (map->map_disallow_new_exec == TRUE) {
		if (cur_protection & VM_PROT_EXECUTE) {
			return KERN_PROTECTION_FAILURE;
		}
	}

	if (is_submap) {
		return KERN_NOT_SUPPORTED;
	}
	if (vmk_flags.vmkf_already) {
		return KERN_NOT_SUPPORTED;
	}
	if (purgable || entry_for_jit) {
		return KERN_NOT_SUPPORTED;
	}

	effective_min_offset = map->min_offset;

	if (vmk_flags.vmkf_beyond_max) {
		return KERN_NOT_SUPPORTED;
	} else {
		effective_max_offset = map->max_offset;
	}

	if (size == 0 ||
	    (offset & FOURK_PAGE_MASK) != 0) {
		*address = 0;
		return KERN_INVALID_ARGUMENT;
	}

#define	RETURN(value)	{ result = value; goto BailOut; }

	assert(VM_MAP_PAGE_ALIGNED(*address, FOURK_PAGE_MASK));
	assert(VM_MAP_PAGE_ALIGNED(size, FOURK_PAGE_MASK));

	if (!anywhere && overwrite) {
		return KERN_NOT_SUPPORTED;
	}
	if (!anywhere && overwrite) {
		/*
		 * Create a temporary VM map to hold the old mappings in the
		 * affected area while we create the new one.
		 * This avoids releasing the VM map lock in
		 * vm_map_entry_delete() and allows atomicity
		 * when we want to replace some mappings with a new one.
		 * It also allows us to restore the old VM mappings if the
		 * new mapping fails.
		 */
		zap_old_map = vm_map_create(PMAP_NULL,
					    *address,
					    *address + size,
					    map->hdr.entries_pageable);
		vm_map_set_page_shift(zap_old_map, VM_MAP_PAGE_SHIFT(map));
		vm_map_disable_hole_optimization(zap_old_map);
	}

	fourk_start = *address;
	fourk_size = size;
	fourk_end = fourk_start + fourk_size;

	start = vm_map_trunc_page(*address, VM_MAP_PAGE_MASK(map));
	end = vm_map_round_page(fourk_end, VM_MAP_PAGE_MASK(map));
	size = end - start;

	if (anywhere) {
		return KERN_NOT_SUPPORTED;
	} else {
		/*
		 *	Verify that:
		 *		the address doesn't itself violate
		 *		the mask requirement.
		 */

		vm_map_lock(map);
		map_locked = TRUE;
		if ((start & mask) != 0) {
			RETURN(KERN_NO_SPACE);
		}

		/*
		 *	...	the address is within bounds
		 */

		end = start + size;

		if ((start < effective_min_offset) ||
		    (end > effective_max_offset) ||
		    (start >= end)) {
			RETURN(KERN_INVALID_ADDRESS);
		}

		if (overwrite && zap_old_map != VM_MAP_NULL) {
			/*
			 * Fixed mapping and "overwrite" flag: attempt to
			 * remove all existing mappings in the specified
			 * address range, saving them in our "zap_old_map".
			 */
			(void) vm_map_delete(map, start, end,
					     (VM_MAP_REMOVE_SAVE_ENTRIES |
					      VM_MAP_REMOVE_NO_MAP_ALIGN),
					     zap_old_map);
		}

		/*
		 *	...	the starting address isn't allocated
		 */
		if (vm_map_lookup_entry(map, start, &entry)) {
			vm_object_t cur_object, shadow_object;

			/*
			 * We might already some 4K mappings
			 * in a 16K page here.
			 */

			if (entry->vme_end - entry->vme_start
			    != SIXTEENK_PAGE_SIZE) {
				RETURN(KERN_NO_SPACE);
			}
			if (entry->is_sub_map) {
				RETURN(KERN_NO_SPACE);
			}
			if (VME_OBJECT(entry) == VM_OBJECT_NULL) {
				RETURN(KERN_NO_SPACE);
			}

			/* go all the way down the shadow chain */
			cur_object = VME_OBJECT(entry);
			vm_object_lock(cur_object);
			while (cur_object->shadow != VM_OBJECT_NULL) {
				shadow_object = cur_object->shadow;
				vm_object_lock(shadow_object);
				vm_object_unlock(cur_object);
				cur_object = shadow_object;
				shadow_object = VM_OBJECT_NULL;
			}
			if (cur_object->internal ||
			    cur_object->pager == NULL) {
				vm_object_unlock(cur_object);
				RETURN(KERN_NO_SPACE);
			}
			if (cur_object->pager->mo_pager_ops
			    != &fourk_pager_ops) {
				vm_object_unlock(cur_object);
				RETURN(KERN_NO_SPACE);
			}
			fourk_object = cur_object;
			fourk_mem_obj = fourk_object->pager;

			/* keep the "4K" object alive */
			vm_object_reference_locked(fourk_object);
			vm_object_unlock(fourk_object);

			/* merge permissions */
			entry->protection |= cur_protection;
			entry->max_protection |= max_protection;
			if ((entry->protection & (VM_PROT_WRITE |
						  VM_PROT_EXECUTE)) ==
			    (VM_PROT_WRITE | VM_PROT_EXECUTE) &&
			    fourk_binary_compatibility_unsafe &&
			    fourk_binary_compatibility_allow_wx) {
				/* write+execute: need to be "jit" */
				entry->used_for_jit = TRUE;
			}

			goto map_in_fourk_pager;
		}

		/*
		 *	...	the next region doesn't overlap the
		 *		end point.
		 */

		if ((entry->vme_next != vm_map_to_entry(map)) &&
		    (entry->vme_next->vme_start < end)) {
			RETURN(KERN_NO_SPACE);
		}
	}

	/*
	 *	At this point,
	 *		"start" and "end" should define the endpoints of the
	 *			available new range, and
	 *		"entry" should refer to the region before the new
	 *			range, and
	 *
	 *		the map should be locked.
	 */

	/* create a new "4K" pager */
	fourk_mem_obj = fourk_pager_create();
	fourk_object = fourk_pager_to_vm_object(fourk_mem_obj);
	assert(fourk_object);

	/* keep the "4" object alive */
	vm_object_reference(fourk_object);

	/* create a "copy" object, to map the "4K" object copy-on-write */
	fourk_copy = TRUE;
	result = vm_object_copy_strategically(fourk_object,
					      0,
					      end - start,
					      &copy_object,
					      &copy_offset,
					      &fourk_copy);
	assert(result == KERN_SUCCESS);
	assert(copy_object != VM_OBJECT_NULL);
	assert(copy_offset == 0);

	/* take a reference on the copy object, for this mapping */
	vm_object_reference(copy_object);

	/* map the "4K" pager's copy object */
	new_entry =
		vm_map_entry_insert(map, entry,
				    vm_map_trunc_page(start,
						      VM_MAP_PAGE_MASK(map)),
				    vm_map_round_page(end,
						      VM_MAP_PAGE_MASK(map)),
				    copy_object,
				    0, /* offset */
				    FALSE, /* needs_copy */
				    FALSE, FALSE,
				    cur_protection, max_protection,
				    VM_BEHAVIOR_DEFAULT,
				    ((entry_for_jit)
				     ? VM_INHERIT_NONE
				     : inheritance),
				    0,
				    no_cache,
				    permanent,
				    superpage_size,
				    clear_map_aligned,
				    is_submap,
				    FALSE, /* jit */
				    alias);
	entry = new_entry;

#if VM_MAP_DEBUG_FOURK
	if (vm_map_debug_fourk) {
		printf("FOURK_PAGER: map %p [0x%llx:0x%llx] new pager %p\n",
		       map,
		       (uint64_t) entry->vme_start,
		       (uint64_t) entry->vme_end,
		       fourk_mem_obj);
	}
#endif /* VM_MAP_DEBUG_FOURK */

	new_mapping_established = TRUE;

map_in_fourk_pager:
	/* "map" the original "object" where it belongs in the "4K" pager */
	fourk_pager_offset = (fourk_start & SIXTEENK_PAGE_MASK);
	fourk_pager_index_start = (int) (fourk_pager_offset / FOURK_PAGE_SIZE);
	if (fourk_size > SIXTEENK_PAGE_SIZE) {
		fourk_pager_index_num = 4;
	} else {
		fourk_pager_index_num = (int) (fourk_size / FOURK_PAGE_SIZE);
	}
	if (fourk_pager_index_start + fourk_pager_index_num > 4) {
		fourk_pager_index_num = 4 - fourk_pager_index_start;
	}
	for (cur_idx = 0;
	     cur_idx < fourk_pager_index_num;
	     cur_idx++) {
		vm_object_t		old_object;
		vm_object_offset_t	old_offset;

		kr = fourk_pager_populate(fourk_mem_obj,
					  TRUE, /* overwrite */
					  fourk_pager_index_start + cur_idx,
					  object,
					  (object
					   ? (offset +
					      (cur_idx * FOURK_PAGE_SIZE))
					   : 0),
					  &old_object,
					  &old_offset);
#if VM_MAP_DEBUG_FOURK
		if (vm_map_debug_fourk) {
			if (old_object == (vm_object_t) -1 &&
			    old_offset == (vm_object_offset_t) -1) {
				printf("FOURK_PAGER: map %p [0x%llx:0x%llx] "
				       "pager [%p:0x%llx] "
				       "populate[%d] "
				       "[object:%p,offset:0x%llx]\n",
				       map,
				       (uint64_t) entry->vme_start,
				       (uint64_t) entry->vme_end,
				       fourk_mem_obj,
				       VME_OFFSET(entry),
				       fourk_pager_index_start + cur_idx,
				       object,
				       (object
					? (offset + (cur_idx * FOURK_PAGE_SIZE))
					: 0));
			} else {
				printf("FOURK_PAGER: map %p [0x%llx:0x%llx] "
				       "pager [%p:0x%llx] "
				       "populate[%d] [object:%p,offset:0x%llx] "
				       "old [%p:0x%llx]\n",
				       map,
				       (uint64_t) entry->vme_start,
				       (uint64_t) entry->vme_end,
				       fourk_mem_obj,
				       VME_OFFSET(entry),
				       fourk_pager_index_start + cur_idx,
				       object,
				       (object
					? (offset + (cur_idx * FOURK_PAGE_SIZE))
					: 0),
				       old_object,
				       old_offset);
			}
		}
#endif /* VM_MAP_DEBUG_FOURK */

		assert(kr == KERN_SUCCESS);
		if (object != old_object &&
		    object != VM_OBJECT_NULL &&
		    object != (vm_object_t) -1) {
			vm_object_reference(object);
		}
		if (object != old_object &&
		    old_object != VM_OBJECT_NULL &&
		    old_object != (vm_object_t) -1) {
			vm_object_deallocate(old_object);
		}
	}

BailOut:
	assert(map_locked == TRUE);

	if (fourk_object != VM_OBJECT_NULL) {
		vm_object_deallocate(fourk_object);
		fourk_object = VM_OBJECT_NULL;
		fourk_mem_obj = MEMORY_OBJECT_NULL;
	}

	if (result == KERN_SUCCESS) {
		vm_prot_t pager_prot;
		memory_object_t pager;

#if DEBUG
		if (pmap_empty &&
		    !(vmk_flags.vmkf_no_pmap_check)) {
			assert(vm_map_pmap_is_empty(map,
						    *address,
						    *address+size));
		}
#endif /* DEBUG */

		/*
		 * For "named" VM objects, let the pager know that the
		 * memory object is being mapped.  Some pagers need to keep
		 * track of this, to know when they can reclaim the memory
		 * object, for example.
		 * VM calls memory_object_map() for each mapping (specifying
		 * the protection of each mapping) and calls
		 * memory_object_last_unmap() when all the mappings are gone.
		 */
		pager_prot = max_protection;
		if (needs_copy) {
			/*
			 * Copy-On-Write mapping: won't modify
			 * the memory object.
			 */
			pager_prot &= ~VM_PROT_WRITE;
		}
		if (!is_submap &&
		    object != VM_OBJECT_NULL &&
		    object->named &&
		    object->pager != MEMORY_OBJECT_NULL) {
			vm_object_lock(object);
			pager = object->pager;
			if (object->named &&
			    pager != MEMORY_OBJECT_NULL) {
				assert(object->pager_ready);
				vm_object_mapping_wait(object, THREAD_UNINT);
				vm_object_mapping_begin(object);
				vm_object_unlock(object);

				kr = memory_object_map(pager, pager_prot);
				assert(kr == KERN_SUCCESS);

				vm_object_lock(object);
				vm_object_mapping_end(object);
			}
			vm_object_unlock(object);
		}
		if (!is_submap &&
		    fourk_object != VM_OBJECT_NULL &&
		    fourk_object->named &&
		    fourk_object->pager != MEMORY_OBJECT_NULL) {
			vm_object_lock(fourk_object);
			pager = fourk_object->pager;
			if (fourk_object->named &&
			    pager != MEMORY_OBJECT_NULL) {
				assert(fourk_object->pager_ready);
				vm_object_mapping_wait(fourk_object,
						       THREAD_UNINT);
				vm_object_mapping_begin(fourk_object);
				vm_object_unlock(fourk_object);

				kr = memory_object_map(pager, VM_PROT_READ);
				assert(kr == KERN_SUCCESS);

				vm_object_lock(fourk_object);
				vm_object_mapping_end(fourk_object);
			}
			vm_object_unlock(fourk_object);
		}
	}

	assert(map_locked == TRUE);

	if (!keep_map_locked) {
		vm_map_unlock(map);
		map_locked = FALSE;
	}

	/*
	 * We can't hold the map lock if we enter this block.
	 */

	if (result == KERN_SUCCESS) {

		/*	Wire down the new entry if the user
		 *	requested all new map entries be wired.
		 */
		if ((map->wiring_required)||(superpage_size)) {
			assert(!keep_map_locked);
			pmap_empty = FALSE; /* pmap won't be empty */
			kr = vm_map_wire_kernel(map, start, end,
					     new_entry->protection, VM_KERN_MEMORY_MLOCK,
					     TRUE);
			result = kr;
		}

	}

	if (result != KERN_SUCCESS) {
		if (new_mapping_established) {
			/*
			 * We have to get rid of the new mappings since we
			 * won't make them available to the user.
			 * Try and do that atomically, to minimize the risk
			 * that someone else create new mappings that range.
			 */
			zap_new_map = vm_map_create(PMAP_NULL,
						    *address,
						    *address + size,
						    map->hdr.entries_pageable);
			vm_map_set_page_shift(zap_new_map,
					      VM_MAP_PAGE_SHIFT(map));
			vm_map_disable_hole_optimization(zap_new_map);

			if (!map_locked) {
				vm_map_lock(map);
				map_locked = TRUE;
			}
			(void) vm_map_delete(map, *address, *address+size,
					     (VM_MAP_REMOVE_SAVE_ENTRIES |
					      VM_MAP_REMOVE_NO_MAP_ALIGN),
					     zap_new_map);
		}
		if (zap_old_map != VM_MAP_NULL &&
		    zap_old_map->hdr.nentries != 0) {
			vm_map_entry_t	entry1, entry2;

			/*
			 * The new mapping failed.  Attempt to restore
			 * the old mappings, saved in the "zap_old_map".
			 */
			if (!map_locked) {
				vm_map_lock(map);
				map_locked = TRUE;
			}

			/* first check if the coast is still clear */
			start = vm_map_first_entry(zap_old_map)->vme_start;
			end = vm_map_last_entry(zap_old_map)->vme_end;
			if (vm_map_lookup_entry(map, start, &entry1) ||
			    vm_map_lookup_entry(map, end, &entry2) ||
			    entry1 != entry2) {
				/*
				 * Part of that range has already been
				 * re-mapped:  we can't restore the old
				 * mappings...
				 */
				vm_map_enter_restore_failures++;
			} else {
				/*
				 * Transfer the saved map entries from
				 * "zap_old_map" to the original "map",
				 * inserting them all after "entry1".
				 */
				for (entry2 = vm_map_first_entry(zap_old_map);
				     entry2 != vm_map_to_entry(zap_old_map);
				     entry2 = vm_map_first_entry(zap_old_map)) {
					vm_map_size_t entry_size;

					entry_size = (entry2->vme_end -
						      entry2->vme_start);
					vm_map_store_entry_unlink(zap_old_map,
							    entry2);
					zap_old_map->size -= entry_size;
					vm_map_store_entry_link(map, entry1, entry2);
					map->size += entry_size;
					entry1 = entry2;
				}
				if (map->wiring_required) {
					/*
					 * XXX TODO: we should rewire the
					 * old pages here...
					 */
				}
				vm_map_enter_restore_successes++;
			}
		}
	}

	/*
	 * The caller is responsible for releasing the lock if it requested to
	 * keep the map locked.
	 */
	if (map_locked && !keep_map_locked) {
		vm_map_unlock(map);
	}

	/*
	 * Get rid of the "zap_maps" and all the map entries that
	 * they may still contain.
	 */
	if (zap_old_map != VM_MAP_NULL) {
		vm_map_destroy(zap_old_map, VM_MAP_REMOVE_NO_PMAP_CLEANUP);
		zap_old_map = VM_MAP_NULL;
	}
	if (zap_new_map != VM_MAP_NULL) {
		vm_map_destroy(zap_new_map, VM_MAP_REMOVE_NO_PMAP_CLEANUP);
		zap_new_map = VM_MAP_NULL;
	}

	return result;

#undef	RETURN
}
#endif /* __arm64__ */

/*
 * Counters for the prefault optimization.
 */
int64_t vm_prefault_nb_pages = 0;
int64_t vm_prefault_nb_bailout = 0;

static kern_return_t
vm_map_enter_mem_object_helper(
	vm_map_t		target_map,
	vm_map_offset_t		*address,
	vm_map_size_t		initial_size,
	vm_map_offset_t		mask,
	int			flags,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		tag,
	ipc_port_t		port,
	vm_object_offset_t	offset,
	boolean_t		copy,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_inherit_t		inheritance,
	upl_page_list_ptr_t	page_list,
	unsigned int		page_list_count)
{
	vm_map_address_t	map_addr;
	vm_map_size_t		map_size;
	vm_object_t		object;
	vm_object_size_t	size;
	kern_return_t		result;
	boolean_t		mask_cur_protection, mask_max_protection;
	boolean_t		kernel_prefault, try_prefault = (page_list_count != 0);
	vm_map_offset_t		offset_in_mapping = 0;
#if __arm64__
	boolean_t		fourk = vmk_flags.vmkf_fourk;
#endif /* __arm64__ */

	assertf(vmk_flags.__vmkf_unused == 0, "vmk_flags unused=0x%x\n", vmk_flags.__vmkf_unused);

	mask_cur_protection = cur_protection & VM_PROT_IS_MASK;
	mask_max_protection = max_protection & VM_PROT_IS_MASK;
	cur_protection &= ~VM_PROT_IS_MASK;
	max_protection &= ~VM_PROT_IS_MASK;

	/*
	 * Check arguments for validity
	 */
	if ((target_map == VM_MAP_NULL) ||
	    (cur_protection & ~VM_PROT_ALL) ||
	    (max_protection & ~VM_PROT_ALL) ||
	    (inheritance > VM_INHERIT_LAST_VALID) ||
	    (try_prefault && (copy || !page_list)) ||
	    initial_size == 0) {
		return KERN_INVALID_ARGUMENT;
	}

#if __arm64__
	if (fourk) {
		map_addr = vm_map_trunc_page(*address, FOURK_PAGE_MASK);
		map_size = vm_map_round_page(initial_size, FOURK_PAGE_MASK);
	} else
#endif /* __arm64__ */
	{
		map_addr = vm_map_trunc_page(*address,
					     VM_MAP_PAGE_MASK(target_map));
		map_size = vm_map_round_page(initial_size,
					     VM_MAP_PAGE_MASK(target_map));
	}
	size = vm_object_round_page(initial_size);

	/*
	 * Find the vm object (if any) corresponding to this port.
	 */
	if (!IP_VALID(port)) {
		object = VM_OBJECT_NULL;
		offset = 0;
		copy = FALSE;
	} else if (ip_kotype(port) == IKOT_NAMED_ENTRY) {
		vm_named_entry_t	named_entry;

		named_entry = (vm_named_entry_t) port->ip_kobject;

		if (flags & (VM_FLAGS_RETURN_DATA_ADDR |
			     VM_FLAGS_RETURN_4K_DATA_ADDR)) {
			offset += named_entry->data_offset;
		}

		/* a few checks to make sure user is obeying rules */
		if (size == 0) {
			if (offset >= named_entry->size)
				return KERN_INVALID_RIGHT;
			size = named_entry->size - offset;
		}
		if (mask_max_protection) {
			max_protection &= named_entry->protection;
		}
		if (mask_cur_protection) {
			cur_protection &= named_entry->protection;
		}
		if ((named_entry->protection & max_protection) !=
		    max_protection)
			return KERN_INVALID_RIGHT;
		if ((named_entry->protection & cur_protection) !=
		    cur_protection)
			return KERN_INVALID_RIGHT;
		if (offset + size < offset) {
			/* overflow */
			return KERN_INVALID_ARGUMENT;
		}
		if (named_entry->size < (offset + initial_size)) {
			return KERN_INVALID_ARGUMENT;
		}

		if (named_entry->is_copy) {
			/* for a vm_map_copy, we can only map it whole */
			if ((size != named_entry->size) &&
			    (vm_map_round_page(size,
					       VM_MAP_PAGE_MASK(target_map)) ==
			     named_entry->size)) {
				/* XXX FBDP use the rounded size... */
				size = vm_map_round_page(
					size,
					VM_MAP_PAGE_MASK(target_map));
			}

			if (!(flags & VM_FLAGS_ANYWHERE) &&
			    (offset != 0 ||
			     size != named_entry->size)) {
				/*
				 * XXX for a mapping at a "fixed" address,
				 * we can't trim after mapping the whole
				 * memory entry, so reject a request for a
				 * partial mapping.
				 */
				return KERN_INVALID_ARGUMENT;
			}
		}

		/* the callers parameter offset is defined to be the */
		/* offset from beginning of named entry offset in object */
		offset = offset + named_entry->offset;

		if (! VM_MAP_PAGE_ALIGNED(size,
					  VM_MAP_PAGE_MASK(target_map))) {
			/*
			 * Let's not map more than requested;
			 * vm_map_enter() will handle this "not map-aligned"
			 * case.
			 */
			map_size = size;
		}

		named_entry_lock(named_entry);
		if (named_entry->is_sub_map) {
			vm_map_t		submap;

			if (flags & (VM_FLAGS_RETURN_DATA_ADDR |
				     VM_FLAGS_RETURN_4K_DATA_ADDR)) {
				panic("VM_FLAGS_RETURN_DATA_ADDR not expected for submap.");
			}

			submap = named_entry->backing.map;
			vm_map_lock(submap);
			vm_map_reference(submap);
			vm_map_unlock(submap);
			named_entry_unlock(named_entry);

			vmk_flags.vmkf_submap = TRUE;

			result = vm_map_enter(target_map,
					      &map_addr,
					      map_size,
					      mask,
					      flags,
					      vmk_flags,
					      tag,
					      (vm_object_t) submap,
					      offset,
					      copy,
					      cur_protection,
					      max_protection,
					      inheritance);
			if (result != KERN_SUCCESS) {
				vm_map_deallocate(submap);
			} else {
				/*
				 * No need to lock "submap" just to check its
				 * "mapped" flag: that flag is never reset
				 * once it's been set and if we race, we'll
				 * just end up setting it twice, which is OK.
				 */
				if (submap->mapped_in_other_pmaps == FALSE &&
				    vm_map_pmap(submap) != PMAP_NULL &&
				    vm_map_pmap(submap) !=
				    vm_map_pmap(target_map)) {
					/*
					 * This submap is being mapped in a map
					 * that uses a different pmap.
					 * Set its "mapped_in_other_pmaps" flag
					 * to indicate that we now need to
					 * remove mappings from all pmaps rather
					 * than just the submap's pmap.
					 */
					vm_map_lock(submap);
					submap->mapped_in_other_pmaps = TRUE;
					vm_map_unlock(submap);
				}
				*address = map_addr;
			}
			return result;

		} else if (named_entry->is_copy) {
			kern_return_t	kr;
			vm_map_copy_t	copy_map;
			vm_map_entry_t	copy_entry;
			vm_map_offset_t	copy_addr;

			if (flags & ~(VM_FLAGS_FIXED |
				      VM_FLAGS_ANYWHERE |
				      VM_FLAGS_OVERWRITE |
				      VM_FLAGS_RETURN_4K_DATA_ADDR |
				      VM_FLAGS_RETURN_DATA_ADDR |
				      VM_FLAGS_ALIAS_MASK)) {
				named_entry_unlock(named_entry);
				return KERN_INVALID_ARGUMENT;
			}

			if (flags & (VM_FLAGS_RETURN_DATA_ADDR |
				     VM_FLAGS_RETURN_4K_DATA_ADDR)) {
				offset_in_mapping = offset - vm_object_trunc_page(offset);
				if (flags & VM_FLAGS_RETURN_4K_DATA_ADDR)
					offset_in_mapping &= ~((signed)(0xFFF));
				offset = vm_object_trunc_page(offset);
				map_size = vm_object_round_page(offset + offset_in_mapping + initial_size) - offset;
			}

			copy_map = named_entry->backing.copy;
			assert(copy_map->type == VM_MAP_COPY_ENTRY_LIST);
			if (copy_map->type != VM_MAP_COPY_ENTRY_LIST) {
				/* unsupported type; should not happen */
				printf("vm_map_enter_mem_object: "
				       "memory_entry->backing.copy "
				       "unsupported type 0x%x\n",
				       copy_map->type);
				named_entry_unlock(named_entry);
				return KERN_INVALID_ARGUMENT;
			}

			/* reserve a contiguous range */
			kr = vm_map_enter(target_map,
					  &map_addr,
					  /* map whole mem entry, trim later: */
					  named_entry->size,
					  mask,
					  flags & (VM_FLAGS_ANYWHERE |
						   VM_FLAGS_OVERWRITE |
						   VM_FLAGS_RETURN_4K_DATA_ADDR |
						   VM_FLAGS_RETURN_DATA_ADDR),
					  vmk_flags,
					  tag,
					  VM_OBJECT_NULL,
					  0,
					  FALSE, /* copy */
					  cur_protection,
					  max_protection,
					  inheritance);
			if (kr != KERN_SUCCESS) {
				named_entry_unlock(named_entry);
				return kr;
			}

			copy_addr = map_addr;

			for (copy_entry = vm_map_copy_first_entry(copy_map);
			     copy_entry != vm_map_copy_to_entry(copy_map);
			     copy_entry = copy_entry->vme_next) {
				int			remap_flags;
				vm_map_kernel_flags_t	vmk_remap_flags;
				vm_map_t		copy_submap;
				vm_object_t		copy_object;
				vm_map_size_t		copy_size;
				vm_object_offset_t	copy_offset;
				int			copy_vm_alias;

				remap_flags = 0;
				vmk_remap_flags = VM_MAP_KERNEL_FLAGS_NONE;

				copy_object = VME_OBJECT(copy_entry);
				copy_offset = VME_OFFSET(copy_entry);
				copy_size = (copy_entry->vme_end -
					     copy_entry->vme_start);
				VM_GET_FLAGS_ALIAS(flags, copy_vm_alias);
				if (copy_vm_alias == 0) {
					/*
					 * Caller does not want a specific
					 * alias for this new mapping:  use
					 * the alias of the original mapping.
					 */
					copy_vm_alias = VME_ALIAS(copy_entry);
				}

				/* sanity check */
				if ((copy_addr + copy_size) >
				    (map_addr +
				     named_entry->size /* XXX full size */ )) {
					/* over-mapping too much !? */
					kr = KERN_INVALID_ARGUMENT;
					/* abort */
					break;
				}

				/* take a reference on the object */
				if (copy_entry->is_sub_map) {
					vmk_remap_flags.vmkf_submap = TRUE;
					copy_submap = VME_SUBMAP(copy_entry);
					vm_map_lock(copy_submap);
					vm_map_reference(copy_submap);
					vm_map_unlock(copy_submap);
					copy_object = (vm_object_t) copy_submap;
				} else if (!copy &&
					   copy_object != VM_OBJECT_NULL &&
					   (copy_entry->needs_copy ||
					    copy_object->shadowed ||
					    (!copy_object->true_share &&
					     !copy_entry->is_shared &&
					     copy_object->vo_size > copy_size))) {
					/*
					 * We need to resolve our side of this
					 * "symmetric" copy-on-write now; we
					 * need a new object to map and share,
					 * instead of the current one which
					 * might still be shared with the
					 * original mapping.
					 *
					 * Note: A "vm_map_copy_t" does not
					 * have a lock but we're protected by
					 * the named entry's lock here.
					 */
					// assert(copy_object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC);
					VME_OBJECT_SHADOW(copy_entry, copy_size);
					if (!copy_entry->needs_copy &&
					    copy_entry->protection & VM_PROT_WRITE) {
						vm_prot_t prot;

						prot = copy_entry->protection & ~VM_PROT_WRITE;
						vm_object_pmap_protect(copy_object,
								       copy_offset,
								       copy_size,
								       PMAP_NULL,
								       0,
								       prot);
					}

					copy_entry->needs_copy = FALSE;
					copy_entry->is_shared = TRUE;
					copy_object = VME_OBJECT(copy_entry);
					copy_offset = VME_OFFSET(copy_entry);
					vm_object_lock(copy_object);
					vm_object_reference_locked(copy_object);
					if (copy_object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC) {
						/* we're about to make a shared mapping of this object */
						copy_object->copy_strategy = MEMORY_OBJECT_COPY_DELAY;
						copy_object->true_share = TRUE;
					}
					vm_object_unlock(copy_object);
				} else {
					/*
					 * We already have the right object
					 * to map.
					 */
					copy_object = VME_OBJECT(copy_entry);
					vm_object_reference(copy_object);
				}

				/* over-map the object into destination */
				remap_flags |= flags;
				remap_flags |= VM_FLAGS_FIXED;
				remap_flags |= VM_FLAGS_OVERWRITE;
				remap_flags &= ~VM_FLAGS_ANYWHERE;
				if (!copy && !copy_entry->is_sub_map) {
					/*
					 * copy-on-write should have been
					 * resolved at this point, or we would
					 * end up sharing instead of copying.
					 */
					assert(!copy_entry->needs_copy);
				}
				kr = vm_map_enter(target_map,
						  &copy_addr,
						  copy_size,
						  (vm_map_offset_t) 0,
						  remap_flags,
						  vmk_remap_flags,
						  copy_vm_alias,
						  copy_object,
						  copy_offset,
						  copy,
						  cur_protection,
						  max_protection,
						  inheritance);
				if (kr != KERN_SUCCESS) {
					if (copy_entry->is_sub_map) {
						vm_map_deallocate(copy_submap);
					} else {
						vm_object_deallocate(copy_object);
					}
					/* abort */
					break;
				}

				/* next mapping */
				copy_addr += copy_size;
			}

			if (kr == KERN_SUCCESS) {
				if (flags & (VM_FLAGS_RETURN_DATA_ADDR |
					     VM_FLAGS_RETURN_4K_DATA_ADDR)) {
					*address = map_addr + offset_in_mapping;
				} else {
					*address = map_addr;
				}

				if (offset) {
					/*
					 * Trim in front, from 0 to "offset".
					 */
					vm_map_remove(target_map,
						      map_addr,
						      map_addr + offset,
						      0);
					*address += offset;
				}
				if (offset + map_size < named_entry->size) {
					/*
					 * Trim in back, from
					 * "offset + map_size" to
					 * "named_entry->size".
					 */
					vm_map_remove(target_map,
						      (map_addr +
						       offset + map_size),
						      (map_addr +
						       named_entry->size),
						      0);
				}
			}
			named_entry_unlock(named_entry);

			if (kr != KERN_SUCCESS) {
				if (! (flags & VM_FLAGS_OVERWRITE)) {
					/* deallocate the contiguous range */
					(void) vm_deallocate(target_map,
							     map_addr,
							     map_size);
				}
			}

			return kr;

		} else {
			unsigned int	access;
			vm_prot_t	protections;
			unsigned int	wimg_mode;

			/* we are mapping a VM object */

			protections = named_entry->protection & VM_PROT_ALL;
			access = GET_MAP_MEM(named_entry->protection);

			if (flags & (VM_FLAGS_RETURN_DATA_ADDR |
				     VM_FLAGS_RETURN_4K_DATA_ADDR)) {
				offset_in_mapping = offset - vm_object_trunc_page(offset);
				if (flags & VM_FLAGS_RETURN_4K_DATA_ADDR)
					offset_in_mapping &= ~((signed)(0xFFF));
				offset = vm_object_trunc_page(offset);
				map_size = vm_object_round_page(offset + offset_in_mapping + initial_size) - offset;
			}

			object = named_entry->backing.object;
			assert(object != VM_OBJECT_NULL);
			vm_object_lock(object);
			named_entry_unlock(named_entry);

			vm_object_reference_locked(object);

			wimg_mode = object->wimg_bits;
                        vm_prot_to_wimg(access, &wimg_mode);
			if (object->wimg_bits != wimg_mode)
				vm_object_change_wimg_mode(object, wimg_mode);

			vm_object_unlock(object);
		}
	} else if (ip_kotype(port) == IKOT_MEMORY_OBJECT) {
		/*
		 * JMM - This is temporary until we unify named entries
		 * and raw memory objects.
		 *
		 * Detected fake ip_kotype for a memory object.  In
		 * this case, the port isn't really a port at all, but
		 * instead is just a raw memory object.
		 */
		if (flags & (VM_FLAGS_RETURN_DATA_ADDR |
			     VM_FLAGS_RETURN_4K_DATA_ADDR)) {
			panic("VM_FLAGS_RETURN_DATA_ADDR not expected for raw memory object.");
		}

		object = memory_object_to_vm_object((memory_object_t)port);
		if (object == VM_OBJECT_NULL)
			return KERN_INVALID_OBJECT;
		vm_object_reference(object);

		/* wait for object (if any) to be ready */
		if (object != VM_OBJECT_NULL) {
			if (object == kernel_object) {
				printf("Warning: Attempt to map kernel object"
					" by a non-private kernel entity\n");
				return KERN_INVALID_OBJECT;
			}
			if (!object->pager_ready) {
				vm_object_lock(object);

				while (!object->pager_ready) {
					vm_object_wait(object,
						       VM_OBJECT_EVENT_PAGER_READY,
						       THREAD_UNINT);
					vm_object_lock(object);
				}
				vm_object_unlock(object);
			}
		}
	} else {
		return KERN_INVALID_OBJECT;
	}

	if (object != VM_OBJECT_NULL &&
	    object->named &&
	    object->pager != MEMORY_OBJECT_NULL &&
	    object->copy_strategy != MEMORY_OBJECT_COPY_NONE) {
		memory_object_t pager;
		vm_prot_t	pager_prot;
		kern_return_t	kr;

		/*
		 * For "named" VM objects, let the pager know that the
		 * memory object is being mapped.  Some pagers need to keep
		 * track of this, to know when they can reclaim the memory
		 * object, for example.
		 * VM calls memory_object_map() for each mapping (specifying
		 * the protection of each mapping) and calls
		 * memory_object_last_unmap() when all the mappings are gone.
		 */
		pager_prot = max_protection;
		if (copy) {
			/*
			 * Copy-On-Write mapping: won't modify the
			 * memory object.
			 */
			pager_prot &= ~VM_PROT_WRITE;
		}
		vm_object_lock(object);
		pager = object->pager;
		if (object->named &&
		    pager != MEMORY_OBJECT_NULL &&
		    object->copy_strategy != MEMORY_OBJECT_COPY_NONE) {
			assert(object->pager_ready);
			vm_object_mapping_wait(object, THREAD_UNINT);
			vm_object_mapping_begin(object);
			vm_object_unlock(object);

			kr = memory_object_map(pager, pager_prot);
			assert(kr == KERN_SUCCESS);

			vm_object_lock(object);
			vm_object_mapping_end(object);
		}
		vm_object_unlock(object);
	}

	/*
	 *	Perform the copy if requested
	 */

	if (copy) {
		vm_object_t		new_object;
		vm_object_offset_t	new_offset;

		result = vm_object_copy_strategically(object, offset,
						      map_size,
						      &new_object, &new_offset,
						      &copy);


		if (result == KERN_MEMORY_RESTART_COPY) {
			boolean_t success;
			boolean_t src_needs_copy;

			/*
			 * XXX
			 * We currently ignore src_needs_copy.
			 * This really is the issue of how to make
			 * MEMORY_OBJECT_COPY_SYMMETRIC safe for
			 * non-kernel users to use. Solution forthcoming.
			 * In the meantime, since we don't allow non-kernel
			 * memory managers to specify symmetric copy,
			 * we won't run into problems here.
			 */
			new_object = object;
			new_offset = offset;
			success = vm_object_copy_quickly(&new_object,
							 new_offset,
							 map_size,
							 &src_needs_copy,
							 &copy);
			assert(success);
			result = KERN_SUCCESS;
		}
		/*
		 *	Throw away the reference to the
		 *	original object, as it won't be mapped.
		 */

		vm_object_deallocate(object);

		if (result != KERN_SUCCESS) {
			return result;
		}

		object = new_object;
		offset = new_offset;
	}

	/*
	 * If non-kernel users want to try to prefault pages, the mapping and prefault
	 * needs to be atomic.
	 */
	kernel_prefault = (try_prefault && vm_kernel_map_is_kernel(target_map));
	vmk_flags.vmkf_keep_map_locked = (try_prefault && !kernel_prefault);

#if __arm64__
	if (fourk) {
		/* map this object in a "4K" pager */
		result = vm_map_enter_fourk(target_map,
					    &map_addr,
					    map_size,
					    (vm_map_offset_t) mask,
					    flags,
					    vmk_flags,
					    tag,
					    object,
					    offset,
					    copy,
					    cur_protection,
					    max_protection,
					    inheritance);
	} else
#endif /* __arm64__ */
	{
		result = vm_map_enter(target_map,
				      &map_addr, map_size,
				      (vm_map_offset_t)mask,
				      flags,
				      vmk_flags,
				      tag,
				      object, offset,
				      copy,
				      cur_protection, max_protection,
				      inheritance);
	}
	if (result != KERN_SUCCESS)
		vm_object_deallocate(object);

	/*
	 * Try to prefault, and do not forget to release the vm map lock.
	 */
	if (result == KERN_SUCCESS && try_prefault) {
		mach_vm_address_t va = map_addr;
		kern_return_t kr = KERN_SUCCESS;
		unsigned int i = 0;
		int pmap_options;

		pmap_options = kernel_prefault ? 0 : PMAP_OPTIONS_NOWAIT;
		if (object->internal) {
			pmap_options |= PMAP_OPTIONS_INTERNAL;
		}

		for (i = 0; i < page_list_count; ++i) {
			if (!UPL_VALID_PAGE(page_list, i)) {
				if (kernel_prefault) {
					assertf(FALSE, "kernel_prefault && !UPL_VALID_PAGE");
					result = KERN_MEMORY_ERROR;
					break;
				}
			} else {
				/*
				 * If this function call failed, we should stop
				 * trying to optimize, other calls are likely
				 * going to fail too.
				 *
				 * We are not gonna report an error for such
				 * failure though. That's an optimization, not
				 * something critical.
				 */
				kr = pmap_enter_options(target_map->pmap,
				                        va, UPL_PHYS_PAGE(page_list, i),
				                        cur_protection, VM_PROT_NONE,
				                        0, TRUE, pmap_options, NULL);
				if (kr != KERN_SUCCESS) {
					OSIncrementAtomic64(&vm_prefault_nb_bailout);
					if (kernel_prefault) {
						result = kr;
					}
					break;
				}
				OSIncrementAtomic64(&vm_prefault_nb_pages);
			}

			/* Next virtual address */
			va += PAGE_SIZE;
		}
		if (vmk_flags.vmkf_keep_map_locked) {
			vm_map_unlock(target_map);
		}
	}

	if (flags & (VM_FLAGS_RETURN_DATA_ADDR |
		     VM_FLAGS_RETURN_4K_DATA_ADDR)) {
		*address = map_addr + offset_in_mapping;
	} else {
		*address = map_addr;
	}
	return result;
}

kern_return_t
vm_map_enter_mem_object(
	vm_map_t		target_map,
	vm_map_offset_t		*address,
	vm_map_size_t		initial_size,
	vm_map_offset_t		mask,
	int			flags,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		tag,
	ipc_port_t		port,
	vm_object_offset_t	offset,
	boolean_t		copy,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_inherit_t		inheritance)
{
	kern_return_t ret;

	ret = vm_map_enter_mem_object_helper(target_map,
					     address,
					     initial_size,
					     mask,
					     flags,
					     vmk_flags,
					     tag,
					     port,
					     offset,
					     copy,
					     cur_protection,
					     max_protection,
					     inheritance,
					     NULL,
					     0);

#if KASAN
	if (ret == KERN_SUCCESS && address && target_map->pmap == kernel_pmap) {
		kasan_notify_address(*address, initial_size);
	}
#endif

	return ret;
}

kern_return_t
vm_map_enter_mem_object_prefault(
	vm_map_t		target_map,
	vm_map_offset_t		*address,
	vm_map_size_t		initial_size,
	vm_map_offset_t		mask,
	int			flags,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		tag,
	ipc_port_t		port,
	vm_object_offset_t	offset,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	upl_page_list_ptr_t	page_list,
	unsigned int		page_list_count)
{
	kern_return_t ret;

	ret = vm_map_enter_mem_object_helper(target_map,
					     address,
					     initial_size,
					     mask,
					     flags,
					     vmk_flags,
					     tag,
					     port,
					     offset,
					     FALSE,
					     cur_protection,
					     max_protection,
					     VM_INHERIT_DEFAULT,
					     page_list,
					     page_list_count);

#if KASAN
	if (ret == KERN_SUCCESS && address && target_map->pmap == kernel_pmap) {
		kasan_notify_address(*address, initial_size);
	}
#endif

	return ret;
}


kern_return_t
vm_map_enter_mem_object_control(
	vm_map_t		target_map,
	vm_map_offset_t		*address,
	vm_map_size_t		initial_size,
	vm_map_offset_t		mask,
	int			flags,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		tag,
	memory_object_control_t	control,
	vm_object_offset_t	offset,
	boolean_t		copy,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_inherit_t		inheritance)
{
	vm_map_address_t	map_addr;
	vm_map_size_t		map_size;
	vm_object_t		object;
	vm_object_size_t	size;
	kern_return_t		result;
	memory_object_t		pager;
	vm_prot_t		pager_prot;
	kern_return_t		kr;
#if __arm64__
	boolean_t		fourk = vmk_flags.vmkf_fourk;
#endif /* __arm64__ */

	/*
	 * Check arguments for validity
	 */
	if ((target_map == VM_MAP_NULL) ||
	    (cur_protection & ~VM_PROT_ALL) ||
	    (max_protection & ~VM_PROT_ALL) ||
	    (inheritance > VM_INHERIT_LAST_VALID) ||
	    initial_size == 0) {
		return KERN_INVALID_ARGUMENT;
	}

#if __arm64__
	if (fourk) {
		map_addr = vm_map_trunc_page(*address,
					     FOURK_PAGE_MASK);
		map_size = vm_map_round_page(initial_size,
					     FOURK_PAGE_MASK);
	} else
#endif /* __arm64__ */
	{
		map_addr = vm_map_trunc_page(*address,
					     VM_MAP_PAGE_MASK(target_map));
		map_size = vm_map_round_page(initial_size,
					     VM_MAP_PAGE_MASK(target_map));
	}
	size = vm_object_round_page(initial_size);

	object = memory_object_control_to_vm_object(control);

	if (object == VM_OBJECT_NULL)
		return KERN_INVALID_OBJECT;

	if (object == kernel_object) {
		printf("Warning: Attempt to map kernel object"
		       " by a non-private kernel entity\n");
		return KERN_INVALID_OBJECT;
	}

	vm_object_lock(object);
	object->ref_count++;
	vm_object_res_reference(object);

	/*
	 * For "named" VM objects, let the pager know that the
	 * memory object is being mapped.  Some pagers need to keep
	 * track of this, to know when they can reclaim the memory
	 * object, for example.
	 * VM calls memory_object_map() for each mapping (specifying
	 * the protection of each mapping) and calls
	 * memory_object_last_unmap() when all the mappings are gone.
	 */
	pager_prot = max_protection;
	if (copy) {
		pager_prot &= ~VM_PROT_WRITE;
	}
	pager = object->pager;
	if (object->named &&
	    pager != MEMORY_OBJECT_NULL &&
	    object->copy_strategy != MEMORY_OBJECT_COPY_NONE) {
		assert(object->pager_ready);
		vm_object_mapping_wait(object, THREAD_UNINT);
		vm_object_mapping_begin(object);
		vm_object_unlock(object);

		kr = memory_object_map(pager, pager_prot);
		assert(kr == KERN_SUCCESS);

		vm_object_lock(object);
		vm_object_mapping_end(object);
	}
	vm_object_unlock(object);

	/*
	 *	Perform the copy if requested
	 */

	if (copy) {
		vm_object_t		new_object;
		vm_object_offset_t	new_offset;

		result = vm_object_copy_strategically(object, offset, size,
						      &new_object, &new_offset,
						      &copy);


		if (result == KERN_MEMORY_RESTART_COPY) {
			boolean_t success;
			boolean_t src_needs_copy;

			/*
			 * XXX
			 * We currently ignore src_needs_copy.
			 * This really is the issue of how to make
			 * MEMORY_OBJECT_COPY_SYMMETRIC safe for
			 * non-kernel users to use. Solution forthcoming.
			 * In the meantime, since we don't allow non-kernel
			 * memory managers to specify symmetric copy,
			 * we won't run into problems here.
			 */
			new_object = object;
			new_offset = offset;
			success = vm_object_copy_quickly(&new_object,
							 new_offset, size,
							 &src_needs_copy,
							 &copy);
			assert(success);
			result = KERN_SUCCESS;
		}
		/*
		 *	Throw away the reference to the
		 *	original object, as it won't be mapped.
		 */

		vm_object_deallocate(object);

		if (result != KERN_SUCCESS) {
			return result;
		}

		object = new_object;
		offset = new_offset;
	}

#if __arm64__
	if (fourk) {
		result = vm_map_enter_fourk(target_map,
					    &map_addr,
					    map_size,
					    (vm_map_offset_t)mask,
					    flags,
					    vmk_flags,
					    tag,
					    object, offset,
					    copy,
					    cur_protection, max_protection,
					    inheritance);
	} else
#endif /* __arm64__ */
	{
		result = vm_map_enter(target_map,
				      &map_addr, map_size,
				      (vm_map_offset_t)mask,
				      flags,
				      vmk_flags,
				      tag,
				      object, offset,
				      copy,
				      cur_protection, max_protection,
				      inheritance);
	}
	if (result != KERN_SUCCESS)
		vm_object_deallocate(object);
	*address = map_addr;

	return result;
}


#if	VM_CPM

#ifdef MACH_ASSERT
extern pmap_paddr_t	avail_start, avail_end;
#endif

/*
 *	Allocate memory in the specified map, with the caveat that
 *	the memory is physically contiguous.  This call may fail
 *	if the system can't find sufficient contiguous memory.
 *	This call may cause or lead to heart-stopping amounts of
 *	paging activity.
 *
 *	Memory obtained from this call should be freed in the
 *	normal way, viz., via vm_deallocate.
 */
kern_return_t
vm_map_enter_cpm(
	vm_map_t		map,
	vm_map_offset_t	*addr,
	vm_map_size_t		size,
	int			flags)
{
	vm_object_t		cpm_obj;
	pmap_t			pmap;
	vm_page_t		m, pages;
	kern_return_t		kr;
	vm_map_offset_t		va, start, end, offset;
#if	MACH_ASSERT
	vm_map_offset_t		prev_addr = 0;
#endif	/* MACH_ASSERT */

	boolean_t		anywhere = ((VM_FLAGS_ANYWHERE & flags) != 0);
	vm_tag_t tag;

	VM_GET_FLAGS_ALIAS(flags, tag);

	if (size == 0) {
		*addr = 0;
		return KERN_SUCCESS;
	}
	if (anywhere)
		*addr = vm_map_min(map);
	else
		*addr = vm_map_trunc_page(*addr,
					  VM_MAP_PAGE_MASK(map));
	size = vm_map_round_page(size,
				 VM_MAP_PAGE_MASK(map));

	/*
	 * LP64todo - cpm_allocate should probably allow
	 * allocations of >4GB, but not with the current
	 * algorithm, so just cast down the size for now.
	 */
	if (size > VM_MAX_ADDRESS)
		return KERN_RESOURCE_SHORTAGE;
	if ((kr = cpm_allocate(CAST_DOWN(vm_size_t, size),
			       &pages, 0, 0, TRUE, flags)) != KERN_SUCCESS)
		return kr;

	cpm_obj = vm_object_allocate((vm_object_size_t)size);
	assert(cpm_obj != VM_OBJECT_NULL);
	assert(cpm_obj->internal);
	assert(cpm_obj->vo_size == (vm_object_size_t)size);
	assert(cpm_obj->can_persist == FALSE);
	assert(cpm_obj->pager_created == FALSE);
	assert(cpm_obj->pageout == FALSE);
	assert(cpm_obj->shadow == VM_OBJECT_NULL);

	/*
	 *	Insert pages into object.
	 */

	vm_object_lock(cpm_obj);
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		m = pages;
		pages = NEXT_PAGE(m);
		*(NEXT_PAGE_PTR(m)) = VM_PAGE_NULL;

		assert(!m->gobbled);
		assert(!m->wanted);
		assert(!m->pageout);
		assert(!m->tabled);
		assert(VM_PAGE_WIRED(m));
		assert(m->busy);
		assert(VM_PAGE_GET_PHYS_PAGE(m)>=(avail_start>>PAGE_SHIFT) && VM_PAGE_GET_PHYS_PAGE(m)<=(avail_end>>PAGE_SHIFT));

		m->busy = FALSE;
		vm_page_insert(m, cpm_obj, offset);
	}
	assert(cpm_obj->resident_page_count == size / PAGE_SIZE);
	vm_object_unlock(cpm_obj);

	/*
	 *	Hang onto a reference on the object in case a
	 *	multi-threaded application for some reason decides
	 *	to deallocate the portion of the address space into
	 *	which we will insert this object.
	 *
	 *	Unfortunately, we must insert the object now before
	 *	we can talk to the pmap module about which addresses
	 *	must be wired down.  Hence, the race with a multi-
	 *	threaded app.
	 */
	vm_object_reference(cpm_obj);

	/*
	 *	Insert object into map.
	 */

	kr = vm_map_enter(
		map,
		addr,
		size,
		(vm_map_offset_t)0,
		flags,
		VM_MAP_KERNEL_FLAGS_NONE,
		cpm_obj,
		(vm_object_offset_t)0,
		FALSE,
		VM_PROT_ALL,
		VM_PROT_ALL,
		VM_INHERIT_DEFAULT);

	if (kr != KERN_SUCCESS) {
		/*
		 *	A CPM object doesn't have can_persist set,
		 *	so all we have to do is deallocate it to
		 *	free up these pages.
		 */
		assert(cpm_obj->pager_created == FALSE);
		assert(cpm_obj->can_persist == FALSE);
		assert(cpm_obj->pageout == FALSE);
		assert(cpm_obj->shadow == VM_OBJECT_NULL);
		vm_object_deallocate(cpm_obj); /* kill acquired ref */
		vm_object_deallocate(cpm_obj); /* kill creation ref */
	}

	/*
	 *	Inform the physical mapping system that the
	 *	range of addresses may not fault, so that
	 *	page tables and such can be locked down as well.
	 */
	start = *addr;
	end = start + size;
	pmap = vm_map_pmap(map);
	pmap_pageable(pmap, start, end, FALSE);

	/*
	 *	Enter each page into the pmap, to avoid faults.
	 *	Note that this loop could be coded more efficiently,
	 *	if the need arose, rather than looking up each page
	 *	again.
	 */
	for (offset = 0, va = start; offset < size;
	     va += PAGE_SIZE, offset += PAGE_SIZE) {
	        int type_of_fault;

		vm_object_lock(cpm_obj);
		m = vm_page_lookup(cpm_obj, (vm_object_offset_t)offset);
		assert(m != VM_PAGE_NULL);

		vm_page_zero_fill(m);

		type_of_fault = DBG_ZERO_FILL_FAULT;

		vm_fault_enter(m, pmap, va, VM_PROT_ALL, VM_PROT_WRITE,
						VM_PAGE_WIRED(m),
						FALSE, /* change_wiring */
						VM_KERN_MEMORY_NONE, /* tag - not wiring */
						FALSE, /* no_cache */
						FALSE, /* cs_bypass */
						0,     /* user_tag */
					    0,     /* pmap_options */
						NULL,  /* need_retry */
						&type_of_fault);

		vm_object_unlock(cpm_obj);
	}

#if	MACH_ASSERT
	/*
	 *	Verify ordering in address space.
	 */
	for (offset = 0; offset < size; offset += PAGE_SIZE) {
		vm_object_lock(cpm_obj);
		m = vm_page_lookup(cpm_obj, (vm_object_offset_t)offset);
		vm_object_unlock(cpm_obj);
		if (m == VM_PAGE_NULL)
			panic("vm_allocate_cpm:  obj %p off 0x%llx no page",
			      cpm_obj, (uint64_t)offset);
		assert(m->tabled);
		assert(!m->busy);
		assert(!m->wanted);
		assert(!m->fictitious);
		assert(!m->private);
		assert(!m->absent);
		assert(!m->error);
		assert(!m->cleaning);
		assert(!m->laundry);
		assert(!m->precious);
		assert(!m->clustered);
		if (offset != 0) {
			if (VM_PAGE_GET_PHYS_PAGE(m) != prev_addr + 1) {
				printf("start 0x%llx end 0x%llx va 0x%llx\n",
				       (uint64_t)start, (uint64_t)end, (uint64_t)va);
				printf("obj %p off 0x%llx\n", cpm_obj, (uint64_t)offset);
				printf("m %p prev_address 0x%llx\n", m, (uint64_t)prev_addr);
				panic("vm_allocate_cpm:  pages not contig!");
			}
		}
		prev_addr = VM_PAGE_GET_PHYS_PAGE(m);
	}
#endif	/* MACH_ASSERT */

	vm_object_deallocate(cpm_obj); /* kill extra ref */

	return kr;
}


#else	/* VM_CPM */

/*
 *	Interface is defined in all cases, but unless the kernel
 *	is built explicitly for this option, the interface does
 *	nothing.
 */

kern_return_t
vm_map_enter_cpm(
	__unused vm_map_t	map,
	__unused vm_map_offset_t	*addr,
	__unused vm_map_size_t	size,
	__unused int		flags)
{
	return KERN_FAILURE;
}
#endif /* VM_CPM */

/* Not used without nested pmaps */
#ifndef NO_NESTED_PMAP
/*
 * Clip and unnest a portion of a nested submap mapping.
 */


static void
vm_map_clip_unnest(
	vm_map_t	map,
	vm_map_entry_t	entry,
	vm_map_offset_t	start_unnest,
	vm_map_offset_t	end_unnest)
{
	vm_map_offset_t old_start_unnest = start_unnest;
	vm_map_offset_t old_end_unnest = end_unnest;

	assert(entry->is_sub_map);
	assert(VME_SUBMAP(entry) != NULL);
	assert(entry->use_pmap);

	/*
	 * Query the platform for the optimal unnest range.
	 * DRK: There's some duplication of effort here, since
	 * callers may have adjusted the range to some extent. This
	 * routine was introduced to support 1GiB subtree nesting
	 * for x86 platforms, which can also nest on 2MiB boundaries
	 * depending on size/alignment.
	 */
	if (pmap_adjust_unnest_parameters(map->pmap, &start_unnest, &end_unnest)) {
		assert(VME_SUBMAP(entry)->is_nested_map);
		assert(!VME_SUBMAP(entry)->disable_vmentry_reuse);
		log_unnest_badness(map,
				   old_start_unnest,
				   old_end_unnest,
				   VME_SUBMAP(entry)->is_nested_map,
				   (entry->vme_start +
				    VME_SUBMAP(entry)->lowest_unnestable_start -
				    VME_OFFSET(entry)));
	}

	if (entry->vme_start > start_unnest ||
	    entry->vme_end < end_unnest) {
		panic("vm_map_clip_unnest(0x%llx,0x%llx): "
		      "bad nested entry: start=0x%llx end=0x%llx\n",
		      (long long)start_unnest, (long long)end_unnest,
		      (long long)entry->vme_start, (long long)entry->vme_end);
	}

	if (start_unnest > entry->vme_start) {
		_vm_map_clip_start(&map->hdr,
				   entry,
				   start_unnest);
		if (map->holelistenabled) {
			vm_map_store_update_first_free(map, NULL, FALSE);
		} else {
			vm_map_store_update_first_free(map, map->first_free, FALSE);
		}
	}
	if (entry->vme_end > end_unnest) {
		_vm_map_clip_end(&map->hdr,
				 entry,
				 end_unnest);
		if (map->holelistenabled) {
			vm_map_store_update_first_free(map, NULL, FALSE);
		} else {
			vm_map_store_update_first_free(map, map->first_free, FALSE);
		}
	}

	pmap_unnest(map->pmap,
		    entry->vme_start,
		    entry->vme_end - entry->vme_start);
	if ((map->mapped_in_other_pmaps) && (map->ref_count)) {
		/* clean up parent map/maps */
		vm_map_submap_pmap_clean(
			map, entry->vme_start,
			entry->vme_end,
			VME_SUBMAP(entry),
			VME_OFFSET(entry));
	}
	entry->use_pmap = FALSE;
	if ((map->pmap != kernel_pmap) &&
	    (VME_ALIAS(entry) == VM_MEMORY_SHARED_PMAP)) {
		VME_ALIAS_SET(entry, VM_MEMORY_UNSHARED_PMAP);
	}
}
#endif	/* NO_NESTED_PMAP */

/*
 *	vm_map_clip_start:	[ internal use only ]
 *
 *	Asserts that the given entry begins at or after
 *	the specified address; if necessary,
 *	it splits the entry into two.
 */
void
vm_map_clip_start(
	vm_map_t	map,
	vm_map_entry_t	entry,
	vm_map_offset_t	startaddr)
{
#ifndef NO_NESTED_PMAP
	if (entry->is_sub_map &&
	    entry->use_pmap &&
	    startaddr >= entry->vme_start) {
		vm_map_offset_t	start_unnest, end_unnest;

		/*
		 * Make sure "startaddr" is no longer in a nested range
		 * before we clip.  Unnest only the minimum range the platform
		 * can handle.
		 * vm_map_clip_unnest may perform additional adjustments to
		 * the unnest range.
		 */
		start_unnest = startaddr & ~(pmap_nesting_size_min - 1);
		end_unnest = start_unnest + pmap_nesting_size_min;
		vm_map_clip_unnest(map, entry, start_unnest, end_unnest);
	}
#endif /* NO_NESTED_PMAP */
	if (startaddr > entry->vme_start) {
		if (VME_OBJECT(entry) &&
		    !entry->is_sub_map &&
		    VME_OBJECT(entry)->phys_contiguous) {
			pmap_remove(map->pmap,
				    (addr64_t)(entry->vme_start),
				    (addr64_t)(entry->vme_end));
		}
		if (entry->vme_atomic) {
			panic("Attempting to clip an atomic VM entry! (map: %p, entry: %p)\n", map, entry);
		}
		_vm_map_clip_start(&map->hdr, entry, startaddr);
		if (map->holelistenabled) {
			vm_map_store_update_first_free(map, NULL, FALSE);
		} else {
			vm_map_store_update_first_free(map, map->first_free, FALSE);
		}
	}
}


#define vm_map_copy_clip_start(copy, entry, startaddr) \
	MACRO_BEGIN \
	if ((startaddr) > (entry)->vme_start) \
		_vm_map_clip_start(&(copy)->cpy_hdr,(entry),(startaddr)); \
	MACRO_END

/*
 *	This routine is called only when it is known that
 *	the entry must be split.
 */
static void
_vm_map_clip_start(
	struct vm_map_header	*map_header,
	vm_map_entry_t		entry,
	vm_map_offset_t		start)
{
	vm_map_entry_t	new_entry;

	/*
	 *	Split off the front portion --
	 *	note that we must insert the new
	 *	entry BEFORE this one, so that
	 *	this entry has the specified starting
	 *	address.
	 */

	if (entry->map_aligned) {
		assert(VM_MAP_PAGE_ALIGNED(start,
					   VM_MAP_HDR_PAGE_MASK(map_header)));
	}

	new_entry = _vm_map_entry_create(map_header, !map_header->entries_pageable);
	vm_map_entry_copy_full(new_entry, entry);

	new_entry->vme_end = start;
	assert(new_entry->vme_start < new_entry->vme_end);
	VME_OFFSET_SET(entry, VME_OFFSET(entry) + (start - entry->vme_start));
	assert(start < entry->vme_end);
	entry->vme_start = start;

	_vm_map_store_entry_link(map_header, entry->vme_prev, new_entry);

	if (entry->is_sub_map)
		vm_map_reference(VME_SUBMAP(new_entry));
	else
		vm_object_reference(VME_OBJECT(new_entry));
}


/*
 *	vm_map_clip_end:	[ internal use only ]
 *
 *	Asserts that the given entry ends at or before
 *	the specified address; if necessary,
 *	it splits the entry into two.
 */
void
vm_map_clip_end(
	vm_map_t	map,
	vm_map_entry_t	entry,
	vm_map_offset_t	endaddr)
{
	if (endaddr > entry->vme_end) {
		/*
		 * Within the scope of this clipping, limit "endaddr" to
		 * the end of this map entry...
		 */
		endaddr = entry->vme_end;
	}
#ifndef NO_NESTED_PMAP
	if (entry->is_sub_map && entry->use_pmap) {
		vm_map_offset_t	start_unnest, end_unnest;

		/*
		 * Make sure the range between the start of this entry and
		 * the new "endaddr" is no longer nested before we clip.
		 * Unnest only the minimum range the platform can handle.
		 * vm_map_clip_unnest may perform additional adjustments to
		 * the unnest range.
		 */
		start_unnest = entry->vme_start;
		end_unnest =
			(endaddr + pmap_nesting_size_min - 1) &
			~(pmap_nesting_size_min - 1);
		vm_map_clip_unnest(map, entry, start_unnest, end_unnest);
	}
#endif /* NO_NESTED_PMAP */
	if (endaddr < entry->vme_end) {
		if (VME_OBJECT(entry) &&
		    !entry->is_sub_map &&
		    VME_OBJECT(entry)->phys_contiguous) {
			pmap_remove(map->pmap,
				    (addr64_t)(entry->vme_start),
				    (addr64_t)(entry->vme_end));
		}
		if (entry->vme_atomic) {
			panic("Attempting to clip an atomic VM entry! (map: %p, entry: %p)\n", map, entry);
		}
		_vm_map_clip_end(&map->hdr, entry, endaddr);
		if (map->holelistenabled) {
			vm_map_store_update_first_free(map, NULL, FALSE);
		} else {
			vm_map_store_update_first_free(map, map->first_free, FALSE);
		}
	}
}


#define vm_map_copy_clip_end(copy, entry, endaddr) \
	MACRO_BEGIN \
	if ((endaddr) < (entry)->vme_end) \
		_vm_map_clip_end(&(copy)->cpy_hdr,(entry),(endaddr)); \
	MACRO_END

/*
 *	This routine is called only when it is known that
 *	the entry must be split.
 */
static void
_vm_map_clip_end(
	struct vm_map_header	*map_header,
	vm_map_entry_t		entry,
	vm_map_offset_t		end)
{
	vm_map_entry_t	new_entry;

	/*
	 *	Create a new entry and insert it
	 *	AFTER the specified entry
	 */

	if (entry->map_aligned) {
		assert(VM_MAP_PAGE_ALIGNED(end,
					   VM_MAP_HDR_PAGE_MASK(map_header)));
	}

	new_entry = _vm_map_entry_create(map_header, !map_header->entries_pageable);
	vm_map_entry_copy_full(new_entry, entry);

	assert(entry->vme_start < end);
	new_entry->vme_start = entry->vme_end = end;
	VME_OFFSET_SET(new_entry,
		       VME_OFFSET(new_entry) + (end - entry->vme_start));
	assert(new_entry->vme_start < new_entry->vme_end);

	_vm_map_store_entry_link(map_header, entry, new_entry);

	if (entry->is_sub_map)
		vm_map_reference(VME_SUBMAP(new_entry));
	else
		vm_object_reference(VME_OBJECT(new_entry));
}


/*
 *	VM_MAP_RANGE_CHECK:	[ internal use only ]
 *
 *	Asserts that the starting and ending region
 *	addresses fall within the valid range of the map.
 */
#define	VM_MAP_RANGE_CHECK(map, start, end)	\
	MACRO_BEGIN				\
	if (start < vm_map_min(map))		\
		start = vm_map_min(map);	\
	if (end > vm_map_max(map))		\
		end = vm_map_max(map);		\
	if (start > end)			\
		start = end;			\
	MACRO_END

/*
 *	vm_map_range_check:	[ internal use only ]
 *
 *	Check that the region defined by the specified start and
 *	end addresses are wholly contained within a single map
 *	entry or set of adjacent map entries of the spacified map,
 *	i.e. the specified region contains no unmapped space.
 *	If any or all of the region is unmapped, FALSE is returned.
 *	Otherwise, TRUE is returned and if the output argument 'entry'
 *	is not NULL it points to the map entry containing the start
 *	of the region.
 *
 *	The map is locked for reading on entry and is left locked.
 */
static boolean_t
vm_map_range_check(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_map_entry_t		*entry)
{
	vm_map_entry_t		cur;
	vm_map_offset_t		prev;

	/*
	 * 	Basic sanity checks first
	 */
	if (start < vm_map_min(map) || end > vm_map_max(map) || start > end)
		return (FALSE);

	/*
	 * 	Check first if the region starts within a valid
	 *	mapping for the map.
	 */
	if (!vm_map_lookup_entry(map, start, &cur))
		return (FALSE);

	/*
	 *	Optimize for the case that the region is contained
	 *	in a single map entry.
	 */
	if (entry != (vm_map_entry_t *) NULL)
		*entry = cur;
	if (end <= cur->vme_end)
		return (TRUE);

	/*
	 * 	If the region is not wholly contained within a
	 * 	single entry, walk the entries looking for holes.
	 */
	prev = cur->vme_end;
	cur = cur->vme_next;
	while ((cur != vm_map_to_entry(map)) && (prev == cur->vme_start)) {
		if (end <= cur->vme_end)
			return (TRUE);
		prev = cur->vme_end;
		cur = cur->vme_next;
	}
	return (FALSE);
}

/*
 *	vm_map_submap:		[ kernel use only ]
 *
 *	Mark the given range as handled by a subordinate map.
 *
 *	This range must have been created with vm_map_find using
 *	the vm_submap_object, and no other operations may have been
 *	performed on this range prior to calling vm_map_submap.
 *
 *	Only a limited number of operations can be performed
 *	within this rage after calling vm_map_submap:
 *		vm_fault
 *	[Don't try vm_map_copyin!]
 *
 *	To remove a submapping, one must first remove the
 *	range from the superior map, and then destroy the
 *	submap (if desired).  [Better yet, don't try it.]
 */
kern_return_t
vm_map_submap(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_map_t	submap,
	vm_map_offset_t	offset,
#ifdef NO_NESTED_PMAP
	__unused
#endif	/* NO_NESTED_PMAP */
	boolean_t	use_pmap)
{
	vm_map_entry_t		entry;
	kern_return_t		result = KERN_INVALID_ARGUMENT;
	vm_object_t		object;

	vm_map_lock(map);

	if (! vm_map_lookup_entry(map, start, &entry)) {
		entry = entry->vme_next;
	}

	if (entry == vm_map_to_entry(map) ||
	    entry->is_sub_map) {
		vm_map_unlock(map);
		return KERN_INVALID_ARGUMENT;
	}

	vm_map_clip_start(map, entry, start);
	vm_map_clip_end(map, entry, end);

	if ((entry->vme_start == start) && (entry->vme_end == end) &&
	    (!entry->is_sub_map) &&
	    ((object = VME_OBJECT(entry)) == vm_submap_object) &&
	    (object->resident_page_count == 0) &&
	    (object->copy == VM_OBJECT_NULL) &&
	    (object->shadow == VM_OBJECT_NULL) &&
	    (!object->pager_created)) {
		VME_OFFSET_SET(entry, (vm_object_offset_t)offset);
		VME_OBJECT_SET(entry, VM_OBJECT_NULL);
		vm_object_deallocate(object);
		entry->is_sub_map = TRUE;
		entry->use_pmap = FALSE;
		VME_SUBMAP_SET(entry, submap);
		vm_map_reference(submap);
		if (submap->mapped_in_other_pmaps == FALSE &&
		    vm_map_pmap(submap) != PMAP_NULL &&
		    vm_map_pmap(submap) != vm_map_pmap(map)) {
			/*
			 * This submap is being mapped in a map
			 * that uses a different pmap.
			 * Set its "mapped_in_other_pmaps" flag
			 * to indicate that we now need to
			 * remove mappings from all pmaps rather
			 * than just the submap's pmap.
			 */
			submap->mapped_in_other_pmaps = TRUE;
		}

#ifndef NO_NESTED_PMAP
		if (use_pmap) {
			/* nest if platform code will allow */
			if(submap->pmap == NULL) {
				ledger_t ledger = map->pmap->ledger;
				submap->pmap = pmap_create(ledger,
						(vm_map_size_t) 0, FALSE);
				if(submap->pmap == PMAP_NULL) {
					vm_map_unlock(map);
					return(KERN_NO_SPACE);
				}
#if	defined(__arm__) || defined(__arm64__)
				pmap_set_nested(submap->pmap);
#endif
			}
			result = pmap_nest(map->pmap,
					   (VME_SUBMAP(entry))->pmap,
					   (addr64_t)start,
					   (addr64_t)start,
					   (uint64_t)(end - start));
			if(result)
				panic("vm_map_submap: pmap_nest failed, rc = %08X\n", result);
			entry->use_pmap = TRUE;
		}
#else	/* NO_NESTED_PMAP */
		pmap_remove(map->pmap, (addr64_t)start, (addr64_t)end);
#endif	/* NO_NESTED_PMAP */
		result = KERN_SUCCESS;
	}
	vm_map_unlock(map);

	return(result);
}

#if CONFIG_EMBEDDED && (DEVELOPMENT || DEBUG)
#include <sys/codesign.h>
extern int proc_selfcsflags(void);
extern int panic_on_unsigned_execute;
#endif /* CONFIG_EMBEDDED && (DEVELOPMENT || DEBUG) */

/*
 *	vm_map_protect:
 *
 *	Sets the protection of the specified address
 *	region in the target map.  If "set_max" is
 *	specified, the maximum protection is to be set;
 *	otherwise, only the current protection is affected.
 */
kern_return_t
vm_map_protect(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_prot_t	new_prot,
	boolean_t	set_max)
{
	vm_map_entry_t			current;
	vm_map_offset_t			prev;
	vm_map_entry_t			entry;
	vm_prot_t			new_max;
	int				pmap_options = 0;
	kern_return_t			kr;

	XPR(XPR_VM_MAP,
	    "vm_map_protect, 0x%X start 0x%X end 0x%X, new 0x%X %d",
	    map, start, end, new_prot, set_max);

	if (new_prot & VM_PROT_COPY) {
		vm_map_offset_t		new_start;
		vm_prot_t		cur_prot, max_prot;
		vm_map_kernel_flags_t	kflags;

		/* LP64todo - see below */
		if (start >= map->max_offset) {
			return KERN_INVALID_ADDRESS;
		}

		kflags = VM_MAP_KERNEL_FLAGS_NONE;
		kflags.vmkf_remap_prot_copy = TRUE;
		new_start = start;
		kr = vm_map_remap(map,
				  &new_start,
				  end - start,
				  0, /* mask */
				  VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE,
				  kflags,
				  0,
				  map,
				  start,
				  TRUE, /* copy-on-write remapping! */
				  &cur_prot,
				  &max_prot,
				  VM_INHERIT_DEFAULT);
		if (kr != KERN_SUCCESS) {
			return kr;
		}
		new_prot &= ~VM_PROT_COPY;
	}

	vm_map_lock(map);

	/* LP64todo - remove this check when vm_map_commpage64()
	 * no longer has to stuff in a map_entry for the commpage
	 * above the map's max_offset.
	 */
	if (start >= map->max_offset) {
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	while(1) {
		/*
		 * 	Lookup the entry.  If it doesn't start in a valid
		 *	entry, return an error.
		 */
		if (! vm_map_lookup_entry(map, start, &entry)) {
			vm_map_unlock(map);
			return(KERN_INVALID_ADDRESS);
		}

		if (entry->superpage_size && (start & (SUPERPAGE_SIZE-1))) { /* extend request to whole entry */
			start = SUPERPAGE_ROUND_DOWN(start);
			continue;
		}
		break;
 	}
	if (entry->superpage_size)
 		end = SUPERPAGE_ROUND_UP(end);

	/*
	 *	Make a first pass to check for protection and address
	 *	violations.
	 */

	current = entry;
	prev = current->vme_start;
	while ((current != vm_map_to_entry(map)) &&
	       (current->vme_start < end)) {

		/*
		 * If there is a hole, return an error.
		 */
		if (current->vme_start != prev) {
			vm_map_unlock(map);
			return(KERN_INVALID_ADDRESS);
		}

		new_max = current->max_protection;
		if ((new_prot & new_max) != new_prot) {
			vm_map_unlock(map);
			return(KERN_PROTECTION_FAILURE);
		}

#if CONFIG_EMBEDDED
		if (new_prot & VM_PROT_WRITE) {
			if ((new_prot & VM_PROT_EXECUTE) && !(current->used_for_jit)) {
				printf("EMBEDDED: %s can't have both write and exec at the same time\n", __FUNCTION__);
				new_prot &= ~VM_PROT_EXECUTE;
			}
		}
#endif

		/*
		 * If the task has requested executable lockdown,
		 * deny both:
		 * - adding executable protections OR
		 * - adding write protections to an existing executable mapping.
		 */
		if (map->map_disallow_new_exec == TRUE) {
			if ((new_prot & VM_PROT_EXECUTE) ||
			    ((current->protection & VM_PROT_EXECUTE) && (new_prot & VM_PROT_WRITE))) {
				vm_map_unlock(map);
				return(KERN_PROTECTION_FAILURE);
			}
		}

		prev = current->vme_end;
		current = current->vme_next;
	}

#if __arm64__
	if (end > prev &&
	    end == vm_map_round_page(prev, VM_MAP_PAGE_MASK(map))) {
		vm_map_entry_t prev_entry;

		prev_entry = current->vme_prev;
		if (prev_entry != vm_map_to_entry(map) &&
		    !prev_entry->map_aligned &&
		    (vm_map_round_page(prev_entry->vme_end,
				       VM_MAP_PAGE_MASK(map))
		     == end)) {
			/*
			 * The last entry in our range is not "map-aligned"
			 * but it would have reached all the way to "end"
			 * if it had been map-aligned, so this is not really
			 * a hole in the range and we can proceed.
			 */
			prev = end;
		}
	}
#endif /* __arm64__ */

	if (end > prev) {
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	/*
	 *	Go back and fix up protections.
	 *	Clip to start here if the range starts within
	 *	the entry.
	 */

	current = entry;
	if (current != vm_map_to_entry(map)) {
		/* clip and unnest if necessary */
		vm_map_clip_start(map, current, start);
	}

	while ((current != vm_map_to_entry(map)) &&
	       (current->vme_start < end)) {

		vm_prot_t	old_prot;

		vm_map_clip_end(map, current, end);

		if (current->is_sub_map) {
			/* clipping did unnest if needed */
			assert(!current->use_pmap);
		}

		old_prot = current->protection;

		if (set_max) {
			current->max_protection = new_prot;
			current->protection = new_prot & old_prot;
		} else {
			current->protection = new_prot;
		}

		/*
		 *	Update physical map if necessary.
		 *	If the request is to turn off write protection,
		 *	we won't do it for real (in pmap). This is because
		 *	it would cause copy-on-write to fail.  We've already
		 *	set, the new protection in the map, so if a
		 *	write-protect fault occurred, it will be fixed up
		 *	properly, COW or not.
		 */
		if (current->protection != old_prot) {
			/* Look one level in we support nested pmaps */
			/* from mapped submaps which are direct entries */
			/* in our map */

			vm_prot_t prot;

			prot = current->protection;
			if (current->is_sub_map || (VME_OBJECT(current) == NULL) || (VME_OBJECT(current) != compressor_object)) {
			        prot &= ~VM_PROT_WRITE;
                        } else {
                                assert(!VME_OBJECT(current)->code_signed);
                                assert(VME_OBJECT(current)->copy_strategy == MEMORY_OBJECT_COPY_NONE);
			}

			if (override_nx(map, VME_ALIAS(current)) && prot)
			        prot |= VM_PROT_EXECUTE;

#if CONFIG_EMBEDDED && (DEVELOPMENT || DEBUG)
			if (!(old_prot & VM_PROT_EXECUTE) &&
			    (prot & VM_PROT_EXECUTE) &&
			    (proc_selfcsflags() & CS_KILL) &&
			    panic_on_unsigned_execute) {
				panic("vm_map_protect(%p,0x%llx,0x%llx) old=0x%x new=0x%x - <rdar://23770418> code-signing bypass?\n", map, (uint64_t)current->vme_start, (uint64_t)current->vme_end, old_prot, prot);
			}
#endif /* CONFIG_EMBEDDED && (DEVELOPMENT || DEBUG) */

			if (pmap_has_prot_policy(prot)) {
				if (current->wired_count) {
					panic("vm_map_protect(%p,0x%llx,0x%llx) new=0x%x wired=%x\n",
					      map, (uint64_t)current->vme_start, (uint64_t)current->vme_end, prot, current->wired_count);
				}

				/* If the pmap layer cares about this
				 * protection type, force a fault for
				 * each page so that vm_fault will
				 * repopulate the page with the full
				 * set of protections.
				 */
				/*
				 * TODO: We don't seem to need this,
				 * but this is due to an internal
				 * implementation detail of
				 * pmap_protect.  Do we want to rely
				 * on this?
				 */
				prot = VM_PROT_NONE;
			}

			if (current->is_sub_map && current->use_pmap) {
				pmap_protect(VME_SUBMAP(current)->pmap,
					     current->vme_start,
					     current->vme_end,
					     prot);
			} else {
				if (prot & VM_PROT_WRITE) {
					if (VME_OBJECT(current) == compressor_object) {
						/*
						 * For write requests on the
						 * compressor, we wil ask the
						 * pmap layer to prevent us from
						 * taking a write fault when we
						 * attempt to access the mapping
						 * next.
						 */
						pmap_options |= PMAP_OPTIONS_PROTECT_IMMEDIATE;
					}
				}

				pmap_protect_options(map->pmap,
						     current->vme_start,
						     current->vme_end,
						     prot,
						     pmap_options,
						     NULL);
			}
		}
		current = current->vme_next;
	}

	current = entry;
	while ((current != vm_map_to_entry(map)) &&
	       (current->vme_start <= end)) {
		vm_map_simplify_entry(map, current);
		current = current->vme_next;
	}

	vm_map_unlock(map);
	return(KERN_SUCCESS);
}

/*
 *	vm_map_inherit:
 *
 *	Sets the inheritance of the specified address
 *	range in the target map.  Inheritance
 *	affects how the map will be shared with
 *	child maps at the time of vm_map_fork.
 */
kern_return_t
vm_map_inherit(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_inherit_t	new_inheritance)
{
	vm_map_entry_t	entry;
	vm_map_entry_t	temp_entry;

	vm_map_lock(map);

	VM_MAP_RANGE_CHECK(map, start, end);

	if (vm_map_lookup_entry(map, start, &temp_entry)) {
		entry = temp_entry;
	}
	else {
		temp_entry = temp_entry->vme_next;
		entry = temp_entry;
	}

	/* first check entire range for submaps which can't support the */
	/* given inheritance. */
	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
		if(entry->is_sub_map) {
			if(new_inheritance == VM_INHERIT_COPY) {
				vm_map_unlock(map);
				return(KERN_INVALID_ARGUMENT);
			}
		}

		entry = entry->vme_next;
	}

	entry = temp_entry;
	if (entry != vm_map_to_entry(map)) {
		/* clip and unnest if necessary */
		vm_map_clip_start(map, entry, start);
	}

	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
		vm_map_clip_end(map, entry, end);
		if (entry->is_sub_map) {
			/* clip did unnest if needed */
			assert(!entry->use_pmap);
		}

		entry->inheritance = new_inheritance;

		entry = entry->vme_next;
	}

	vm_map_unlock(map);
	return(KERN_SUCCESS);
}

/*
 * Update the accounting for the amount of wired memory in this map.  If the user has
 * exceeded the defined limits, then we fail.  Wiring on behalf of the kernel never fails.
 */

static kern_return_t
add_wire_counts(
	vm_map_t	map,
	vm_map_entry_t	entry,
	boolean_t	user_wire)
{
	vm_map_size_t	size;

	if (user_wire) {
		unsigned int total_wire_count =  vm_page_wire_count + vm_lopage_free_count;

		/*
		 * We're wiring memory at the request of the user.  Check if this is the first time the user is wiring
		 * this map entry.
		 */

		if (entry->user_wired_count == 0) {
			size = entry->vme_end - entry->vme_start;

			/*
			 * Since this is the first time the user is wiring this map entry, check to see if we're
			 * exceeding the user wire limits.  There is a per map limit which is the smaller of either
			 * the process's rlimit or the global vm_user_wire_limit which caps this value.  There is also
			 * a system-wide limit on the amount of memory all users can wire.  If the user is over either
			 * limit, then we fail.
			 */

			if(size + map->user_wire_size > MIN(map->user_wire_limit, vm_user_wire_limit) ||
			   size + ptoa_64(total_wire_count) > vm_global_user_wire_limit ||
		    	   size + ptoa_64(total_wire_count) > max_mem - vm_global_no_user_wire_amount)
				return KERN_RESOURCE_SHORTAGE;

			/*
			 * The first time the user wires an entry, we also increment the wired_count and add this to
			 * the total that has been wired in the map.
			 */

			if (entry->wired_count >= MAX_WIRE_COUNT)
				return KERN_FAILURE;

			entry->wired_count++;
			map->user_wire_size += size;
		}

		if (entry->user_wired_count >= MAX_WIRE_COUNT)
			return KERN_FAILURE;

		entry->user_wired_count++;

	} else {

		/*
		 * The kernel's wiring the memory.  Just bump the count and continue.
		 */

		if (entry->wired_count >= MAX_WIRE_COUNT)
			panic("vm_map_wire: too many wirings");

		entry->wired_count++;
	}

	return KERN_SUCCESS;
}

/*
 * Update the memory wiring accounting now that the given map entry is being unwired.
 */

static void
subtract_wire_counts(
	vm_map_t	map,
	vm_map_entry_t	entry,
	boolean_t	user_wire)
{

	if (user_wire) {

		/*
		 * We're unwiring memory at the request of the user.  See if we're removing the last user wire reference.
		 */

		if (entry->user_wired_count == 1) {

			/*
			 * We're removing the last user wire reference.  Decrement the wired_count and the total
			 * user wired memory for this map.
			 */

			assert(entry->wired_count >= 1);
			entry->wired_count--;
			map->user_wire_size -= entry->vme_end - entry->vme_start;
		}

		assert(entry->user_wired_count >= 1);
		entry->user_wired_count--;

	} else {

		/*
		 * The kernel is unwiring the memory.   Just update the count.
		 */

		assert(entry->wired_count >= 1);
		entry->wired_count--;
	}
}

#if CONFIG_EMBEDDED
int cs_executable_wire = 0;
#endif /* CONFIG_EMBEDDED */

/*
 *	vm_map_wire:
 *
 *	Sets the pageability of the specified address range in the
 *	target map as wired.  Regions specified as not pageable require
 *	locked-down physical memory and physical page maps.  The
 *	access_type variable indicates types of accesses that must not
 *	generate page faults.  This is checked against protection of
 *	memory being locked-down.
 *
 *	The map must not be locked, but a reference must remain to the
 *	map throughout the call.
 */
static kern_return_t
vm_map_wire_nested(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_prot_t		caller_prot,
	vm_tag_t		tag,
	boolean_t		user_wire,
	pmap_t			map_pmap,
	vm_map_offset_t		pmap_addr,
	ppnum_t			*physpage_p)
{
	vm_map_entry_t		entry;
	vm_prot_t		access_type;
	struct vm_map_entry	*first_entry, tmp_entry;
	vm_map_t		real_map;
	vm_map_offset_t		s,e;
	kern_return_t		rc;
	boolean_t		need_wakeup;
	boolean_t		main_map = FALSE;
	wait_interrupt_t	interruptible_state;
	thread_t		cur_thread;
	unsigned int		last_timestamp;
	vm_map_size_t		size;
	boolean_t		wire_and_extract;

	access_type = (caller_prot & VM_PROT_ALL);

	wire_and_extract = FALSE;
	if (physpage_p != NULL) {
		/*
		 * The caller wants the physical page number of the
		 * wired page.  We return only one physical page number
		 * so this works for only one page at a time.
		 */
		if ((end - start) != PAGE_SIZE) {
			return KERN_INVALID_ARGUMENT;
		}
		wire_and_extract = TRUE;
		*physpage_p = 0;
	}

	vm_map_lock(map);
	if(map_pmap == NULL)
		main_map = TRUE;
	last_timestamp = map->timestamp;

	VM_MAP_RANGE_CHECK(map, start, end);
	assert(page_aligned(start));
	assert(page_aligned(end));
	assert(VM_MAP_PAGE_ALIGNED(start, VM_MAP_PAGE_MASK(map)));
	assert(VM_MAP_PAGE_ALIGNED(end, VM_MAP_PAGE_MASK(map)));
	if (start == end) {
		/* We wired what the caller asked for, zero pages */
		vm_map_unlock(map);
		return KERN_SUCCESS;
	}

	need_wakeup = FALSE;
	cur_thread = current_thread();

	s = start;
	rc = KERN_SUCCESS;

	if (vm_map_lookup_entry(map, s, &first_entry)) {
		entry = first_entry;
		/*
		 * vm_map_clip_start will be done later.
		 * We don't want to unnest any nested submaps here !
		 */
	} else {
		/* Start address is not in map */
		rc = KERN_INVALID_ADDRESS;
		goto done;
	}

	while ((entry != vm_map_to_entry(map)) && (s < end)) {
		/*
		 * At this point, we have wired from "start" to "s".
		 * We still need to wire from "s" to "end".
		 *
		 * "entry" hasn't been clipped, so it could start before "s"
		 * and/or end after "end".
		 */

		/* "e" is how far we want to wire in this entry */
		e = entry->vme_end;
		if (e > end)
			e = end;

		/*
		 * If another thread is wiring/unwiring this entry then
		 * block after informing other thread to wake us up.
		 */
		if (entry->in_transition) {
			wait_result_t wait_result;

			/*
			 * We have not clipped the entry.  Make sure that
			 * the start address is in range so that the lookup
			 * below will succeed.
			 * "s" is the current starting point: we've already
			 * wired from "start" to "s" and we still have
			 * to wire from "s" to "end".
			 */

			entry->needs_wakeup = TRUE;

			/*
			 * wake up anybody waiting on entries that we have
			 * already wired.
			 */
			if (need_wakeup) {
				vm_map_entry_wakeup(map);
				need_wakeup = FALSE;
			}
			/*
			 * User wiring is interruptible
			 */
			wait_result = vm_map_entry_wait(map,
							(user_wire) ? THREAD_ABORTSAFE :
							THREAD_UNINT);
			if (user_wire && wait_result ==	THREAD_INTERRUPTED) {
				/*
				 * undo the wirings we have done so far
				 * We do not clear the needs_wakeup flag,
				 * because we cannot tell if we were the
				 * only one waiting.
				 */
				rc = KERN_FAILURE;
				goto done;
			}

			/*
			 * Cannot avoid a lookup here. reset timestamp.
			 */
			last_timestamp = map->timestamp;

			/*
			 * The entry could have been clipped, look it up again.
			 * Worse that can happen is, it may not exist anymore.
			 */
			if (!vm_map_lookup_entry(map, s, &first_entry)) {
				/*
				 * User: undo everything upto the previous
				 * entry.  let vm_map_unwire worry about
				 * checking the validity of the range.
				 */
				rc = KERN_FAILURE;
				goto done;
			}
			entry = first_entry;
			continue;
		}

		if (entry->is_sub_map) {
			vm_map_offset_t	sub_start;
			vm_map_offset_t	sub_end;
			vm_map_offset_t	local_start;
			vm_map_offset_t	local_end;
			pmap_t		pmap;

			if (wire_and_extract) {
				/*
				 * Wiring would result in copy-on-write
				 * which would not be compatible with
				 * the sharing we have with the original
				 * provider of this memory.
				 */
				rc = KERN_INVALID_ARGUMENT;
				goto done;
			}

			vm_map_clip_start(map, entry, s);
			vm_map_clip_end(map, entry, end);

			sub_start = VME_OFFSET(entry);
			sub_end = entry->vme_end;
			sub_end += VME_OFFSET(entry) - entry->vme_start;

			local_end = entry->vme_end;
			if(map_pmap == NULL) {
				vm_object_t		object;
				vm_object_offset_t	offset;
				vm_prot_t		prot;
				boolean_t		wired;
				vm_map_entry_t		local_entry;
				vm_map_version_t	 version;
				vm_map_t		lookup_map;

				if(entry->use_pmap) {
					pmap = VME_SUBMAP(entry)->pmap;
					/* ppc implementation requires that */
					/* submaps pmap address ranges line */
					/* up with parent map */
#ifdef notdef
					pmap_addr = sub_start;
#endif
					pmap_addr = s;
				} else {
					pmap = map->pmap;
					pmap_addr = s;
				}

				if (entry->wired_count) {
					if ((rc = add_wire_counts(map, entry, user_wire)) != KERN_SUCCESS)
						goto done;

					/*
					 * The map was not unlocked:
					 * no need to goto re-lookup.
					 * Just go directly to next entry.
					 */
					entry = entry->vme_next;
					s = entry->vme_start;
					continue;

				}

				/* call vm_map_lookup_locked to */
				/* cause any needs copy to be   */
				/* evaluated */
				local_start = entry->vme_start;
				lookup_map = map;
				vm_map_lock_write_to_read(map);
				if(vm_map_lookup_locked(
					   &lookup_map, local_start,
					   access_type | VM_PROT_COPY,
					   OBJECT_LOCK_EXCLUSIVE,
					   &version, &object,
					   &offset, &prot, &wired,
					   NULL,
					   &real_map)) {

					vm_map_unlock_read(lookup_map);
					assert(map_pmap == NULL);
					vm_map_unwire(map, start,
						      s, user_wire);
					return(KERN_FAILURE);
				}
				vm_object_unlock(object);
				if(real_map != lookup_map)
					vm_map_unlock(real_map);
				vm_map_unlock_read(lookup_map);
				vm_map_lock(map);

				/* we unlocked, so must re-lookup */
				if (!vm_map_lookup_entry(map,
							 local_start,
							 &local_entry)) {
					rc = KERN_FAILURE;
					goto done;
				}

				/*
				 * entry could have been "simplified",
				 * so re-clip
				 */
				entry = local_entry;
				assert(s == local_start);
				vm_map_clip_start(map, entry, s);
				vm_map_clip_end(map, entry, end);
				/* re-compute "e" */
				e = entry->vme_end;
				if (e > end)
					e = end;

				/* did we have a change of type? */
				if (!entry->is_sub_map) {
					last_timestamp = map->timestamp;
					continue;
				}
			} else {
				local_start = entry->vme_start;
				pmap = map_pmap;
			}

			if ((rc = add_wire_counts(map, entry, user_wire)) != KERN_SUCCESS)
				goto done;

			entry->in_transition = TRUE;

			vm_map_unlock(map);
			rc = vm_map_wire_nested(VME_SUBMAP(entry),
						sub_start, sub_end,
						caller_prot, tag,
						user_wire, pmap, pmap_addr,
						NULL);
			vm_map_lock(map);

			/*
			 * Find the entry again.  It could have been clipped
			 * after we unlocked the map.
			 */
			if (!vm_map_lookup_entry(map, local_start,
						 &first_entry))
				panic("vm_map_wire: re-lookup failed");
			entry = first_entry;

			assert(local_start == s);
			/* re-compute "e" */
			e = entry->vme_end;
			if (e > end)
				e = end;

			last_timestamp = map->timestamp;
			while ((entry != vm_map_to_entry(map)) &&
			       (entry->vme_start < e)) {
				assert(entry->in_transition);
				entry->in_transition = FALSE;
				if (entry->needs_wakeup) {
					entry->needs_wakeup = FALSE;
					need_wakeup = TRUE;
				}
				if (rc != KERN_SUCCESS) {/* from vm_*_wire */
					subtract_wire_counts(map, entry, user_wire);
				}
				entry = entry->vme_next;
			}
			if (rc != KERN_SUCCESS) {	/* from vm_*_wire */
				goto done;
			}

			/* no need to relookup again */
			s = entry->vme_start;
			continue;
		}

		/*
		 * If this entry is already wired then increment
		 * the appropriate wire reference count.
		 */
		if (entry->wired_count) {

			if ((entry->protection & access_type) != access_type) {
				/* found a protection problem */

				/*
				 * XXX FBDP
				 * We should always return an error
				 * in this case but since we didn't
				 * enforce it before, let's do
				 * it only for the new "wire_and_extract"
				 * code path for now...
				 */
				if (wire_and_extract) {
					rc = KERN_PROTECTION_FAILURE;
					goto done;
				}
			}

			/*
			 * entry is already wired down, get our reference
			 * after clipping to our range.
			 */
			vm_map_clip_start(map, entry, s);
			vm_map_clip_end(map, entry, end);

			if ((rc = add_wire_counts(map, entry, user_wire)) != KERN_SUCCESS)
				goto done;

			if (wire_and_extract) {
				vm_object_t		object;
				vm_object_offset_t	offset;
				vm_page_t		m;

				/*
				 * We don't have to "wire" the page again
				 * bit we still have to "extract" its
				 * physical page number, after some sanity
				 * checks.
				 */
				assert((entry->vme_end - entry->vme_start)
				       == PAGE_SIZE);
				assert(!entry->needs_copy);
				assert(!entry->is_sub_map);
				assert(VME_OBJECT(entry));
				if (((entry->vme_end - entry->vme_start)
				     != PAGE_SIZE) ||
				    entry->needs_copy ||
				    entry->is_sub_map ||
				    VME_OBJECT(entry) == VM_OBJECT_NULL) {
					rc = KERN_INVALID_ARGUMENT;
					goto done;
				}

				object = VME_OBJECT(entry);
				offset = VME_OFFSET(entry);
				/* need exclusive lock to update m->dirty */
				if (entry->protection & VM_PROT_WRITE) {
					vm_object_lock(object);
				} else {
					vm_object_lock_shared(object);
				}
				m = vm_page_lookup(object, offset);
				assert(m != VM_PAGE_NULL);
				assert(VM_PAGE_WIRED(m));
				if (m != VM_PAGE_NULL && VM_PAGE_WIRED(m)) {
					*physpage_p = VM_PAGE_GET_PHYS_PAGE(m);
					if (entry->protection & VM_PROT_WRITE) {
						vm_object_lock_assert_exclusive(
							object);
						m->dirty = TRUE;
					}
				} else {
					/* not already wired !? */
					*physpage_p = 0;
				}
				vm_object_unlock(object);
			}

			/* map was not unlocked: no need to relookup */
			entry = entry->vme_next;
			s = entry->vme_start;
			continue;
		}

		/*
		 * Unwired entry or wire request transmitted via submap
		 */

#if CONFIG_EMBEDDED
		/*
		 * Wiring would copy the pages to the shadow object.
		 * The shadow object would not be code-signed so
		 * attempting to execute code from these copied pages
		 * would trigger a code-signing violation.
		 */
		if (entry->protection & VM_PROT_EXECUTE) {
#if MACH_ASSERT
			printf("pid %d[%s] wiring executable range from "
			       "0x%llx to 0x%llx: rejected to preserve "
			       "code-signing\n",
			       proc_selfpid(),
			       (current_task()->bsd_info
				? proc_name_address(current_task()->bsd_info)
				: "?"),
			       (uint64_t) entry->vme_start,
			       (uint64_t) entry->vme_end);
#endif /* MACH_ASSERT */
			DTRACE_VM2(cs_executable_wire,
				   uint64_t, (uint64_t)entry->vme_start,
				   uint64_t, (uint64_t)entry->vme_end);
			cs_executable_wire++;
			rc = KERN_PROTECTION_FAILURE;
			goto done;
		}
#endif /* CONFIG_EMBEDDED */


		/*
		 * Perform actions of vm_map_lookup that need the write
		 * lock on the map: create a shadow object for a
		 * copy-on-write region, or an object for a zero-fill
		 * region.
		 */
		size = entry->vme_end - entry->vme_start;
		/*
		 * If wiring a copy-on-write page, we need to copy it now
		 * even if we're only (currently) requesting read access.
		 * This is aggressive, but once it's wired we can't move it.
		 */
		if (entry->needs_copy) {
			if (wire_and_extract) {
				/*
				 * We're supposed to share with the original
				 * provider so should not be "needs_copy"
				 */
				rc = KERN_INVALID_ARGUMENT;
				goto done;
			}

			VME_OBJECT_SHADOW(entry, size);
			entry->needs_copy = FALSE;
		} else if (VME_OBJECT(entry) == VM_OBJECT_NULL) {
			if (wire_and_extract) {
				/*
				 * We're supposed to share with the original
				 * provider so should already have an object.
				 */
				rc = KERN_INVALID_ARGUMENT;
				goto done;
			}
			VME_OBJECT_SET(entry, vm_object_allocate(size));
			VME_OFFSET_SET(entry, (vm_object_offset_t)0);
			assert(entry->use_pmap);
		}

		vm_map_clip_start(map, entry, s);
		vm_map_clip_end(map, entry, end);

		/* re-compute "e" */
		e = entry->vme_end;
		if (e > end)
			e = end;

		/*
		 * Check for holes and protection mismatch.
		 * Holes: Next entry should be contiguous unless this
		 *	  is the end of the region.
		 * Protection: Access requested must be allowed, unless
		 *	wiring is by protection class
		 */
		if ((entry->vme_end < end) &&
		    ((entry->vme_next == vm_map_to_entry(map)) ||
		     (entry->vme_next->vme_start > entry->vme_end))) {
			/* found a hole */
			rc = KERN_INVALID_ADDRESS;
			goto done;
		}
		if ((entry->protection & access_type) != access_type) {
			/* found a protection problem */
			rc = KERN_PROTECTION_FAILURE;
			goto done;
		}

		assert(entry->wired_count == 0 && entry->user_wired_count == 0);

		if ((rc = add_wire_counts(map, entry, user_wire)) != KERN_SUCCESS)
			goto done;

		entry->in_transition = TRUE;

		/*
		 * This entry might get split once we unlock the map.
		 * In vm_fault_wire(), we need the current range as
		 * defined by this entry.  In order for this to work
		 * along with a simultaneous clip operation, we make a
		 * temporary copy of this entry and use that for the
		 * wiring.  Note that the underlying objects do not
		 * change during a clip.
		 */
		tmp_entry = *entry;

		/*
		 * The in_transition state guarentees that the entry
		 * (or entries for this range, if split occured) will be
		 * there when the map lock is acquired for the second time.
		 */
		vm_map_unlock(map);

		if (!user_wire && cur_thread != THREAD_NULL)
			interruptible_state = thread_interrupt_level(THREAD_UNINT);
		else
			interruptible_state = THREAD_UNINT;

		if(map_pmap)
			rc = vm_fault_wire(map,
					   &tmp_entry, caller_prot, tag, map_pmap, pmap_addr,
					   physpage_p);
		else
			rc = vm_fault_wire(map,
					   &tmp_entry, caller_prot, tag, map->pmap,
					   tmp_entry.vme_start,
					   physpage_p);

		if (!user_wire && cur_thread != THREAD_NULL)
			thread_interrupt_level(interruptible_state);

		vm_map_lock(map);

		if (last_timestamp+1 != map->timestamp) {
			/*
			 * Find the entry again.  It could have been clipped
			 * after we unlocked the map.
			 */
			if (!vm_map_lookup_entry(map, tmp_entry.vme_start,
						 &first_entry))
				panic("vm_map_wire: re-lookup failed");

			entry = first_entry;
		}

		last_timestamp = map->timestamp;

		while ((entry != vm_map_to_entry(map)) &&
		       (entry->vme_start < tmp_entry.vme_end)) {
			assert(entry->in_transition);
			entry->in_transition = FALSE;
			if (entry->needs_wakeup) {
				entry->needs_wakeup = FALSE;
				need_wakeup = TRUE;
			}
			if (rc != KERN_SUCCESS) {	/* from vm_*_wire */
				subtract_wire_counts(map, entry, user_wire);
			}
			entry = entry->vme_next;
		}

		if (rc != KERN_SUCCESS) {		/* from vm_*_wire */
			goto done;
		}

		if ((entry != vm_map_to_entry(map)) && /* we still have entries in the map */
		    (tmp_entry.vme_end != end) &&    /* AND, we are not at the end of the requested range */
		    (entry->vme_start != tmp_entry.vme_end)) { /* AND, the next entry is not contiguous. */
			/* found a "new" hole */
			s = tmp_entry.vme_end;
			rc = KERN_INVALID_ADDRESS;
			goto done;
		}

		s = entry->vme_start;

	} /* end while loop through map entries */

done:
	if (rc == KERN_SUCCESS) {
		/* repair any damage we may have made to the VM map */
		vm_map_simplify_range(map, start, end);
	}

	vm_map_unlock(map);

	/*
	 * wake up anybody waiting on entries we wired.
	 */
	if (need_wakeup)
		vm_map_entry_wakeup(map);

	if (rc != KERN_SUCCESS) {
		/* undo what has been wired so far */
		vm_map_unwire_nested(map, start, s, user_wire,
				     map_pmap, pmap_addr);
		if (physpage_p) {
			*physpage_p = 0;
		}
	}

	return rc;

}

kern_return_t
vm_map_wire_external(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_prot_t		caller_prot,
	boolean_t		user_wire)
{
	kern_return_t	kret;

	kret = vm_map_wire_nested(map, start, end, caller_prot, vm_tag_bt(),
				  user_wire, (pmap_t)NULL, 0, NULL);
	return kret;
}

kern_return_t
vm_map_wire_kernel(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_prot_t		caller_prot,
	vm_tag_t		tag,
	boolean_t		user_wire)
{
	kern_return_t	kret;

	kret = vm_map_wire_nested(map, start, end, caller_prot, tag,
				  user_wire, (pmap_t)NULL, 0, NULL);
	return kret;
}

kern_return_t
vm_map_wire_and_extract_external(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_prot_t	caller_prot,
	boolean_t	user_wire,
	ppnum_t		*physpage_p)
{
	kern_return_t	kret;

	kret = vm_map_wire_nested(map,
				  start,
				  start+VM_MAP_PAGE_SIZE(map),
				  caller_prot,
				  vm_tag_bt(),
				  user_wire,
				  (pmap_t)NULL,
				  0,
				  physpage_p);
	if (kret != KERN_SUCCESS &&
	    physpage_p != NULL) {
		*physpage_p = 0;
	}
	return kret;
}

kern_return_t
vm_map_wire_and_extract_kernel(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_prot_t	caller_prot,
	vm_tag_t	tag,
	boolean_t	user_wire,
	ppnum_t		*physpage_p)
{
	kern_return_t	kret;

	kret = vm_map_wire_nested(map,
				  start,
				  start+VM_MAP_PAGE_SIZE(map),
				  caller_prot,
				  tag,
				  user_wire,
				  (pmap_t)NULL,
				  0,
				  physpage_p);
	if (kret != KERN_SUCCESS &&
	    physpage_p != NULL) {
		*physpage_p = 0;
	}
	return kret;
}

/*
 *	vm_map_unwire:
 *
 *	Sets the pageability of the specified address range in the target
 *	as pageable.  Regions specified must have been wired previously.
 *
 *	The map must not be locked, but a reference must remain to the map
 *	throughout the call.
 *
 *	Kernel will panic on failures.  User unwire ignores holes and
 *	unwired and intransition entries to avoid losing memory by leaving
 *	it unwired.
 */
static kern_return_t
vm_map_unwire_nested(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	boolean_t		user_wire,
	pmap_t			map_pmap,
	vm_map_offset_t		pmap_addr)
{
	vm_map_entry_t		entry;
	struct vm_map_entry	*first_entry, tmp_entry;
	boolean_t		need_wakeup;
	boolean_t		main_map = FALSE;
	unsigned int		last_timestamp;

	vm_map_lock(map);
	if(map_pmap == NULL)
		main_map = TRUE;
	last_timestamp = map->timestamp;

	VM_MAP_RANGE_CHECK(map, start, end);
	assert(page_aligned(start));
	assert(page_aligned(end));
	assert(VM_MAP_PAGE_ALIGNED(start, VM_MAP_PAGE_MASK(map)));
	assert(VM_MAP_PAGE_ALIGNED(end, VM_MAP_PAGE_MASK(map)));

	if (start == end) {
		/* We unwired what the caller asked for: zero pages */
		vm_map_unlock(map);
		return KERN_SUCCESS;
	}

	if (vm_map_lookup_entry(map, start, &first_entry)) {
		entry = first_entry;
		/*
		 * vm_map_clip_start will be done later.
		 * We don't want to unnest any nested sub maps here !
		 */
	}
	else {
		if (!user_wire) {
			panic("vm_map_unwire: start not found");
		}
		/*	Start address is not in map. */
		vm_map_unlock(map);
		return(KERN_INVALID_ADDRESS);
	}

	if (entry->superpage_size) {
		/* superpages are always wired */
		vm_map_unlock(map);
		return KERN_INVALID_ADDRESS;
	}

	need_wakeup = FALSE;
	while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
		if (entry->in_transition) {
			/*
			 * 1)
			 * Another thread is wiring down this entry. Note
			 * that if it is not for the other thread we would
			 * be unwiring an unwired entry.  This is not
			 * permitted.  If we wait, we will be unwiring memory
			 * we did not wire.
			 *
			 * 2)
			 * Another thread is unwiring this entry.  We did not
			 * have a reference to it, because if we did, this
			 * entry will not be getting unwired now.
			 */
			if (!user_wire) {
				/*
				 * XXX FBDP
				 * This could happen:  there could be some
				 * overlapping vslock/vsunlock operations
				 * going on.
				 * We should probably just wait and retry,
				 * but then we have to be careful that this
				 * entry could get "simplified" after
				 * "in_transition" gets unset and before
				 * we re-lookup the entry, so we would
				 * have to re-clip the entry to avoid
				 * re-unwiring what we have already unwired...
				 * See vm_map_wire_nested().
				 *
				 * Or we could just ignore "in_transition"
				 * here and proceed to decement the wired
				 * count(s) on this entry.  That should be fine
				 * as long as "wired_count" doesn't drop all
				 * the way to 0 (and we should panic if THAT
				 * happens).
				 */
				panic("vm_map_unwire: in_transition entry");
			}

			entry = entry->vme_next;
			continue;
		}

		if (entry->is_sub_map) {
			vm_map_offset_t	sub_start;
			vm_map_offset_t	sub_end;
			vm_map_offset_t	local_end;
			pmap_t		pmap;

			vm_map_clip_start(map, entry, start);
			vm_map_clip_end(map, entry, end);

			sub_start = VME_OFFSET(entry);
			sub_end = entry->vme_end - entry->vme_start;
			sub_end += VME_OFFSET(entry);
			local_end = entry->vme_end;
			if(map_pmap == NULL) {
				if(entry->use_pmap) {
					pmap = VME_SUBMAP(entry)->pmap;
					pmap_addr = sub_start;
				} else {
					pmap = map->pmap;
					pmap_addr = start;
				}
				if (entry->wired_count == 0 ||
				    (user_wire && entry->user_wired_count == 0)) {
					if (!user_wire)
						panic("vm_map_unwire: entry is unwired");
					entry = entry->vme_next;
					continue;
				}

				/*
				 * Check for holes
				 * Holes: Next entry should be contiguous unless
				 * this is the end of the region.
				 */
				if (((entry->vme_end < end) &&
				     ((entry->vme_next == vm_map_to_entry(map)) ||
				      (entry->vme_next->vme_start
				       > entry->vme_end)))) {
					if (!user_wire)
						panic("vm_map_unwire: non-contiguous region");
/*
					entry = entry->vme_next;
					continue;
*/
				}

				subtract_wire_counts(map, entry, user_wire);

				if (entry->wired_count != 0) {
					entry = entry->vme_next;
					continue;
				}

				entry->in_transition = TRUE;
				tmp_entry = *entry;/* see comment in vm_map_wire() */

				/*
				 * We can unlock the map now. The in_transition state
				 * guarantees existance of the entry.
				 */
				vm_map_unlock(map);
				vm_map_unwire_nested(VME_SUBMAP(entry),
						     sub_start, sub_end, user_wire, pmap, pmap_addr);
				vm_map_lock(map);

				if (last_timestamp+1 != map->timestamp) {
					/*
					 * Find the entry again.  It could have been
					 * clipped or deleted after we unlocked the map.
					 */
					if (!vm_map_lookup_entry(map,
								 tmp_entry.vme_start,
								 &first_entry)) {
						if (!user_wire)
							panic("vm_map_unwire: re-lookup failed");
						entry = first_entry->vme_next;
					} else
						entry = first_entry;
				}
				last_timestamp = map->timestamp;

				/*
				 * clear transition bit for all constituent entries
				 * that were in the original entry (saved in
				 * tmp_entry).  Also check for waiters.
				 */
				while ((entry != vm_map_to_entry(map)) &&
				       (entry->vme_start < tmp_entry.vme_end)) {
					assert(entry->in_transition);
					entry->in_transition = FALSE;
					if (entry->needs_wakeup) {
						entry->needs_wakeup = FALSE;
						need_wakeup = TRUE;
					}
					entry = entry->vme_next;
				}
				continue;
			} else {
				vm_map_unlock(map);
				vm_map_unwire_nested(VME_SUBMAP(entry),
						     sub_start, sub_end, user_wire, map_pmap,
						     pmap_addr);
				vm_map_lock(map);

				if (last_timestamp+1 != map->timestamp) {
					/*
					 * Find the entry again.  It could have been
					 * clipped or deleted after we unlocked the map.
					 */
					if (!vm_map_lookup_entry(map,
								 tmp_entry.vme_start,
								 &first_entry)) {
						if (!user_wire)
							panic("vm_map_unwire: re-lookup failed");
						entry = first_entry->vme_next;
					} else
						entry = first_entry;
				}
				last_timestamp = map->timestamp;
			}
		}


		if ((entry->wired_count == 0) ||
		    (user_wire && entry->user_wired_count == 0)) {
			if (!user_wire)
				panic("vm_map_unwire: entry is unwired");

			entry = entry->vme_next;
			continue;
		}

		assert(entry->wired_count > 0 &&
		       (!user_wire || entry->user_wired_count > 0));

		vm_map_clip_start(map, entry, start);
		vm_map_clip_end(map, entry, end);

		/*
		 * Check for holes
		 * Holes: Next entry should be contiguous unless
		 *	  this is the end of the region.
		 */
		if (((entry->vme_end < end) &&
		     ((entry->vme_next == vm_map_to_entry(map)) ||
		      (entry->vme_next->vme_start > entry->vme_end)))) {

			if (!user_wire)
				panic("vm_map_unwire: non-contiguous region");
			entry = entry->vme_next;
			continue;
		}

		subtract_wire_counts(map, entry, user_wire);

		if (entry->wired_count != 0) {
			entry = entry->vme_next;
			continue;
		}

		if(entry->zero_wired_pages) {
			entry->zero_wired_pages = FALSE;
		}

		entry->in_transition = TRUE;
		tmp_entry = *entry;	/* see comment in vm_map_wire() */

		/*
		 * We can unlock the map now. The in_transition state
		 * guarantees existance of the entry.
		 */
		vm_map_unlock(map);
		if(map_pmap) {
			vm_fault_unwire(map,
					&tmp_entry, FALSE, map_pmap, pmap_addr);
		} else {
			vm_fault_unwire(map,
					&tmp_entry, FALSE, map->pmap,
					tmp_entry.vme_start);
		}
		vm_map_lock(map);

		if (last_timestamp+1 != map->timestamp) {
			/*
			 * Find the entry again.  It could have been clipped
			 * or deleted after we unlocked the map.
			 */
			if (!vm_map_lookup_entry(map, tmp_entry.vme_start,
						 &first_entry)) {
				if (!user_wire)
					panic("vm_map_unwire: re-lookup failed");
				entry = first_entry->vme_next;
			} else
				entry = first_entry;
		}
		last_timestamp = map->timestamp;

		/*
		 * clear transition bit for all constituent entries that
		 * were in the original entry (saved in tmp_entry).  Also
		 * check for waiters.
		 */
		while ((entry != vm_map_to_entry(map)) &&
		       (entry->vme_start < tmp_entry.vme_end)) {
			assert(entry->in_transition);
			entry->in_transition = FALSE;
			if (entry->needs_wakeup) {
				entry->needs_wakeup = FALSE;
				need_wakeup = TRUE;
			}
			entry = entry->vme_next;
		}
	}

	/*
	 * We might have fragmented the address space when we wired this
	 * range of addresses.  Attempt to re-coalesce these VM map entries
	 * with their neighbors now that they're no longer wired.
	 * Under some circumstances, address space fragmentation can
	 * prevent VM object shadow chain collapsing, which can cause
	 * swap space leaks.
	 */
	vm_map_simplify_range(map, start, end);

	vm_map_unlock(map);
	/*
	 * wake up anybody waiting on entries that we have unwired.
	 */
	if (need_wakeup)
		vm_map_entry_wakeup(map);
	return(KERN_SUCCESS);

}

kern_return_t
vm_map_unwire(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	boolean_t		user_wire)
{
	return vm_map_unwire_nested(map, start, end,
				    user_wire, (pmap_t)NULL, 0);
}


/*
 *	vm_map_entry_delete:	[ internal use only ]
 *
 *	Deallocate the given entry from the target map.
 */
static void
vm_map_entry_delete(
	vm_map_t	map,
	vm_map_entry_t	entry)
{
	vm_map_offset_t	s, e;
	vm_object_t	object;
	vm_map_t	submap;

	s = entry->vme_start;
	e = entry->vme_end;
	assert(page_aligned(s));
	assert(page_aligned(e));
	if (entry->map_aligned == TRUE) {
		assert(VM_MAP_PAGE_ALIGNED(s, VM_MAP_PAGE_MASK(map)));
		assert(VM_MAP_PAGE_ALIGNED(e, VM_MAP_PAGE_MASK(map)));
	}
	assert(entry->wired_count == 0);
	assert(entry->user_wired_count == 0);
	assert(!entry->permanent);

	if (entry->is_sub_map) {
		object = NULL;
		submap = VME_SUBMAP(entry);
	} else {
		submap = NULL;
		object = VME_OBJECT(entry);
	}

	vm_map_store_entry_unlink(map, entry);
	map->size -= e - s;

	vm_map_entry_dispose(map, entry);

	vm_map_unlock(map);
	/*
	 *	Deallocate the object only after removing all
	 *	pmap entries pointing to its pages.
	 */
	if (submap)
		vm_map_deallocate(submap);
	else
		vm_object_deallocate(object);

}

void
vm_map_submap_pmap_clean(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_map_t	sub_map,
	vm_map_offset_t	offset)
{
	vm_map_offset_t	submap_start;
	vm_map_offset_t	submap_end;
	vm_map_size_t	remove_size;
	vm_map_entry_t	entry;

	submap_end = offset + (end - start);
	submap_start = offset;

	vm_map_lock_read(sub_map);
	if(vm_map_lookup_entry(sub_map, offset, &entry)) {

		remove_size = (entry->vme_end - entry->vme_start);
		if(offset > entry->vme_start)
			remove_size -= offset - entry->vme_start;


		if(submap_end < entry->vme_end) {
			remove_size -=
				entry->vme_end - submap_end;
		}
		if(entry->is_sub_map) {
			vm_map_submap_pmap_clean(
				sub_map,
				start,
				start + remove_size,
				VME_SUBMAP(entry),
				VME_OFFSET(entry));
		} else {

			if((map->mapped_in_other_pmaps) && (map->ref_count)
			   && (VME_OBJECT(entry) != NULL)) {
				vm_object_pmap_protect_options(
					VME_OBJECT(entry),
					(VME_OFFSET(entry) +
					 offset -
					 entry->vme_start),
					remove_size,
					PMAP_NULL,
					entry->vme_start,
					VM_PROT_NONE,
					PMAP_OPTIONS_REMOVE);
			} else {
				pmap_remove(map->pmap,
					    (addr64_t)start,
					    (addr64_t)(start + remove_size));
			}
		}
	}

	entry = entry->vme_next;

	while((entry != vm_map_to_entry(sub_map))
	      && (entry->vme_start < submap_end)) {
		remove_size = (entry->vme_end - entry->vme_start);
		if(submap_end < entry->vme_end) {
			remove_size -= entry->vme_end - submap_end;
		}
		if(entry->is_sub_map) {
			vm_map_submap_pmap_clean(
				sub_map,
				(start + entry->vme_start) - offset,
				((start + entry->vme_start) - offset) + remove_size,
				VME_SUBMAP(entry),
				VME_OFFSET(entry));
		} else {
			if((map->mapped_in_other_pmaps) && (map->ref_count)
			   && (VME_OBJECT(entry) != NULL)) {
				vm_object_pmap_protect_options(
					VME_OBJECT(entry),
					VME_OFFSET(entry),
					remove_size,
					PMAP_NULL,
					entry->vme_start,
					VM_PROT_NONE,
					PMAP_OPTIONS_REMOVE);
			} else {
				pmap_remove(map->pmap,
					    (addr64_t)((start + entry->vme_start)
						       - offset),
					    (addr64_t)(((start + entry->vme_start)
							- offset) + remove_size));
			}
		}
		entry = entry->vme_next;
	}
	vm_map_unlock_read(sub_map);
	return;
}

/*
 *	vm_map_delete:	[ internal use only ]
 *
 *	Deallocates the given address range from the target map.
 *	Removes all user wirings. Unwires one kernel wiring if
 *	VM_MAP_REMOVE_KUNWIRE is set.  Waits for kernel wirings to go
 *	away if VM_MAP_REMOVE_WAIT_FOR_KWIRE is set.  Sleeps
 *	interruptibly if VM_MAP_REMOVE_INTERRUPTIBLE is set.
 *
 *	This routine is called with map locked and leaves map locked.
 */
static kern_return_t
vm_map_delete(
	vm_map_t		map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	int			flags,
	vm_map_t		zap_map)
{
	vm_map_entry_t		entry, next;
	struct	 vm_map_entry	*first_entry, tmp_entry;
	vm_map_offset_t		s;
	vm_object_t		object;
	boolean_t		need_wakeup;
	unsigned int		last_timestamp = ~0; /* unlikely value */
	int			interruptible;

	interruptible = (flags & VM_MAP_REMOVE_INTERRUPTIBLE) ?
		THREAD_ABORTSAFE : THREAD_UNINT;

	/*
	 * All our DMA I/O operations in IOKit are currently done by
	 * wiring through the map entries of the task requesting the I/O.
	 * Because of this, we must always wait for kernel wirings
	 * to go away on the entries before deleting them.
	 *
	 * Any caller who wants to actually remove a kernel wiring
	 * should explicitly set the VM_MAP_REMOVE_KUNWIRE flag to
	 * properly remove one wiring instead of blasting through
	 * them all.
	 */
	flags |= VM_MAP_REMOVE_WAIT_FOR_KWIRE;

	while(1) {
		/*
		 *	Find the start of the region, and clip it
		 */
		if (vm_map_lookup_entry(map, start, &first_entry)) {
			entry = first_entry;
			if (map == kalloc_map &&
			    (entry->vme_start != start ||
			     entry->vme_end != end)) {
				panic("vm_map_delete(%p,0x%llx,0x%llx): "
				      "mismatched entry %p [0x%llx:0x%llx]\n",
				      map,
				      (uint64_t)start,
				      (uint64_t)end,
				      entry,
				      (uint64_t)entry->vme_start,
				      (uint64_t)entry->vme_end);
			}
			if (entry->superpage_size && (start & ~SUPERPAGE_MASK)) { /* extend request to whole entry */				start = SUPERPAGE_ROUND_DOWN(start);
				start = SUPERPAGE_ROUND_DOWN(start);
				continue;
			}
			if (start == entry->vme_start) {
				/*
				 * No need to clip.  We don't want to cause
				 * any unnecessary unnesting in this case...
				 */
			} else {
				if ((flags & VM_MAP_REMOVE_NO_MAP_ALIGN) &&
				    entry->map_aligned &&
				    !VM_MAP_PAGE_ALIGNED(
					    start,
					    VM_MAP_PAGE_MASK(map))) {
					/*
					 * The entry will no longer be
					 * map-aligned after clipping
					 * and the caller said it's OK.
					 */
					entry->map_aligned = FALSE;
				}
				if (map == kalloc_map) {
					panic("vm_map_delete(%p,0x%llx,0x%llx):"
					      " clipping %p at 0x%llx\n",
					      map,
					      (uint64_t)start,
					      (uint64_t)end,
					      entry,
					      (uint64_t)start);
				}
				vm_map_clip_start(map, entry, start);
			}

			/*
			 *	Fix the lookup hint now, rather than each
			 *	time through the loop.
			 */
			SAVE_HINT_MAP_WRITE(map, entry->vme_prev);
		} else {
			if (map->pmap == kernel_pmap &&
			    map->ref_count != 0) {
				panic("vm_map_delete(%p,0x%llx,0x%llx): "
				      "no map entry at 0x%llx\n",
				      map,
				      (uint64_t)start,
				      (uint64_t)end,
				      (uint64_t)start);
			}
			entry = first_entry->vme_next;
		}
		break;
	}
	if (entry->superpage_size)
		end = SUPERPAGE_ROUND_UP(end);

	need_wakeup = FALSE;
	/*
	 *	Step through all entries in this region
	 */
	s = entry->vme_start;
	while ((entry != vm_map_to_entry(map)) && (s < end)) {
		/*
		 * At this point, we have deleted all the memory entries
		 * between "start" and "s".  We still need to delete
		 * all memory entries between "s" and "end".
		 * While we were blocked and the map was unlocked, some
		 * new memory entries could have been re-allocated between
		 * "start" and "s" and we don't want to mess with those.
		 * Some of those entries could even have been re-assembled
		 * with an entry after "s" (in vm_map_simplify_entry()), so
		 * we may have to vm_map_clip_start() again.
		 */

		if (entry->vme_start >= s) {
			/*
			 * This entry starts on or after "s"
			 * so no need to clip its start.
			 */
		} else {
			/*
			 * This entry has been re-assembled by a
			 * vm_map_simplify_entry().  We need to
			 * re-clip its start.
			 */
			if ((flags & VM_MAP_REMOVE_NO_MAP_ALIGN) &&
			    entry->map_aligned &&
			    !VM_MAP_PAGE_ALIGNED(s,
						 VM_MAP_PAGE_MASK(map))) {
				/*
				 * The entry will no longer be map-aligned
				 * after clipping and the caller said it's OK.
				 */
				entry->map_aligned = FALSE;
			}
			if (map == kalloc_map) {
				panic("vm_map_delete(%p,0x%llx,0x%llx): "
				      "clipping %p at 0x%llx\n",
				      map,
				      (uint64_t)start,
				      (uint64_t)end,
				      entry,
				      (uint64_t)s);
			}
			vm_map_clip_start(map, entry, s);
		}
		if (entry->vme_end <= end) {
			/*
			 * This entry is going away completely, so no need
			 * to clip and possibly cause an unnecessary unnesting.
			 */
		} else {
			if ((flags & VM_MAP_REMOVE_NO_MAP_ALIGN) &&
			    entry->map_aligned &&
			    !VM_MAP_PAGE_ALIGNED(end,
						 VM_MAP_PAGE_MASK(map))) {
				/*
				 * The entry will no longer be map-aligned
				 * after clipping and the caller said it's OK.
				 */
				entry->map_aligned = FALSE;
			}
			if (map == kalloc_map) {
				panic("vm_map_delete(%p,0x%llx,0x%llx): "
				      "clipping %p at 0x%llx\n",
				      map,
				      (uint64_t)start,
				      (uint64_t)end,
				      entry,
				      (uint64_t)end);
			}
			vm_map_clip_end(map, entry, end);
		}

		if (entry->permanent) {
			if (map->pmap == kernel_pmap) {
				panic("%s(%p,0x%llx,0x%llx): "
				      "attempt to remove permanent "
				      "VM map entry "
				      "%p [0x%llx:0x%llx]\n",
				      __FUNCTION__,
				      map,
				      (uint64_t) start,
				      (uint64_t) end,
				      entry,
				      (uint64_t) entry->vme_start,
				      (uint64_t) entry->vme_end);
			} else if (flags & VM_MAP_REMOVE_IMMUTABLE) {
//				printf("FBDP %d[%s] removing permanent entry %p [0x%llx:0x%llx] prot 0x%x/0x%x\n", proc_selfpid(), (current_task()->bsd_info ? proc_name_address(current_task()->bsd_info) : "?"), entry, (uint64_t)entry->vme_start, (uint64_t)entry->vme_end, entry->protection, entry->max_protection);
				entry->permanent = FALSE;
			} else {
				if (!vm_map_executable_immutable_no_log) {
					printf("%d[%s] %s(0x%llx,0x%llx): "
						   "permanent entry [0x%llx:0x%llx] "
						   "prot 0x%x/0x%x\n",
						   proc_selfpid(),
						   (current_task()->bsd_info
							? proc_name_address(current_task()->bsd_info)
							: "?"),
						   __FUNCTION__,
						   (uint64_t) start,
						   (uint64_t) end,
						   (uint64_t)entry->vme_start,
						   (uint64_t)entry->vme_end,
						   entry->protection,
						   entry->max_protection);
				}
				/*
				 * dtrace -n 'vm_map_delete_permanent { print("start=0x%llx end=0x%llx prot=0x%x/0x%x\n", arg0, arg1, arg2, arg3); stack(); ustack(); }'
				 */
				DTRACE_VM5(vm_map_delete_permanent,
					   vm_map_offset_t, entry->vme_start,
					   vm_map_offset_t, entry->vme_end,
					   vm_prot_t, entry->protection,
					   vm_prot_t, entry->max_protection,
					   int, VME_ALIAS(entry));
			}
		}


		if (entry->in_transition) {
			wait_result_t wait_result;

			/*
			 * Another thread is wiring/unwiring this entry.
			 * Let the other thread know we are waiting.
			 */
			assert(s == entry->vme_start);
			entry->needs_wakeup = TRUE;

			/*
			 * wake up anybody waiting on entries that we have
			 * already unwired/deleted.
			 */
			if (need_wakeup) {
				vm_map_entry_wakeup(map);
				need_wakeup = FALSE;
			}

			wait_result = vm_map_entry_wait(map, interruptible);

			if (interruptible &&
			    wait_result == THREAD_INTERRUPTED) {
				/*
				 * We do not clear the needs_wakeup flag,
				 * since we cannot tell if we were the only one.
				 */
				return KERN_ABORTED;
			}

			/*
			 * The entry could have been clipped or it
			 * may not exist anymore.  Look it up again.
			 */
			if (!vm_map_lookup_entry(map, s, &first_entry)) {
				/*
				 * User: use the next entry
				 */
				entry = first_entry->vme_next;
				s = entry->vme_start;
			} else {
				entry = first_entry;
				SAVE_HINT_MAP_WRITE(map, entry->vme_prev);
			}
			last_timestamp = map->timestamp;
			continue;
		} /* end in_transition */

		if (entry->wired_count) {
			boolean_t	user_wire;

			user_wire = entry->user_wired_count > 0;

			/*
			 * 	Remove a kernel wiring if requested
			 */
			if (flags & VM_MAP_REMOVE_KUNWIRE) {
				entry->wired_count--;
			}

			/*
			 *	Remove all user wirings for proper accounting
			 */
			if (entry->user_wired_count > 0) {
				while (entry->user_wired_count)
					subtract_wire_counts(map, entry, user_wire);
			}

			if (entry->wired_count != 0) {
				assert(map != kernel_map);
				/*
				 * Cannot continue.  Typical case is when
				 * a user thread has physical io pending on
				 * on this page.  Either wait for the
				 * kernel wiring to go away or return an
				 * error.
				 */
				if (flags & VM_MAP_REMOVE_WAIT_FOR_KWIRE) {
					wait_result_t wait_result;

					assert(s == entry->vme_start);
					entry->needs_wakeup = TRUE;
					wait_result = vm_map_entry_wait(map,
									interruptible);

					if (interruptible &&
					    wait_result == THREAD_INTERRUPTED) {
						/*
						 * We do not clear the
						 * needs_wakeup flag, since we
						 * cannot tell if we were the
						 * only one.
						 */
						return KERN_ABORTED;
					}

					/*
					 * The entry could have been clipped or
					 * it may not exist anymore.  Look it
					 * up again.
					 */
					if (!vm_map_lookup_entry(map, s,
								 &first_entry)) {
						assert(map != kernel_map);
						/*
						 * User: use the next entry
						 */
						entry = first_entry->vme_next;
						s = entry->vme_start;
					} else {
						entry = first_entry;
						SAVE_HINT_MAP_WRITE(map, entry->vme_prev);
					}
					last_timestamp = map->timestamp;
					continue;
				}
				else {
					return KERN_FAILURE;
				}
			}

			entry->in_transition = TRUE;
			/*
			 * copy current entry.  see comment in vm_map_wire()
			 */
			tmp_entry = *entry;
			assert(s == entry->vme_start);

			/*
			 * We can unlock the map now. The in_transition
			 * state guarentees existance of the entry.
			 */
			vm_map_unlock(map);

			if (tmp_entry.is_sub_map) {
				vm_map_t sub_map;
				vm_map_offset_t sub_start, sub_end;
				pmap_t pmap;
				vm_map_offset_t pmap_addr;


				sub_map = VME_SUBMAP(&tmp_entry);
				sub_start = VME_OFFSET(&tmp_entry);
				sub_end = sub_start + (tmp_entry.vme_end -
						       tmp_entry.vme_start);
				if (tmp_entry.use_pmap) {
					pmap = sub_map->pmap;
					pmap_addr = tmp_entry.vme_start;
				} else {
					pmap = map->pmap;
					pmap_addr = tmp_entry.vme_start;
				}
				(void) vm_map_unwire_nested(sub_map,
							    sub_start, sub_end,
							    user_wire,
							    pmap, pmap_addr);
			} else {

				if (VME_OBJECT(&tmp_entry) == kernel_object) {
					pmap_protect_options(
						map->pmap,
						tmp_entry.vme_start,
						tmp_entry.vme_end,
						VM_PROT_NONE,
						PMAP_OPTIONS_REMOVE,
						NULL);
				}
				vm_fault_unwire(map, &tmp_entry,
						VME_OBJECT(&tmp_entry) == kernel_object,
						map->pmap, tmp_entry.vme_start);
			}

			vm_map_lock(map);

			if (last_timestamp+1 != map->timestamp) {
				/*
				 * Find the entry again.  It could have
				 * been clipped after we unlocked the map.
				 */
				if (!vm_map_lookup_entry(map, s, &first_entry)){
					assert((map != kernel_map) &&
					       (!entry->is_sub_map));
					first_entry = first_entry->vme_next;
					s = first_entry->vme_start;
				} else {
					SAVE_HINT_MAP_WRITE(map, entry->vme_prev);
				}
			} else {
				SAVE_HINT_MAP_WRITE(map, entry->vme_prev);
				first_entry = entry;
			}

			last_timestamp = map->timestamp;

			entry = first_entry;
			while ((entry != vm_map_to_entry(map)) &&
			       (entry->vme_start < tmp_entry.vme_end)) {
				assert(entry->in_transition);
				entry->in_transition = FALSE;
				if (entry->needs_wakeup) {
					entry->needs_wakeup = FALSE;
					need_wakeup = TRUE;
				}
				entry = entry->vme_next;
			}
			/*
			 * We have unwired the entry(s).  Go back and
			 * delete them.
			 */
			entry = first_entry;
			continue;
		}

		/* entry is unwired */
		assert(entry->wired_count == 0);
		assert(entry->user_wired_count == 0);

		assert(s == entry->vme_start);

		if (flags & VM_MAP_REMOVE_NO_PMAP_CLEANUP) {
			/*
			 * XXX with the VM_MAP_REMOVE_SAVE_ENTRIES flag to
			 * vm_map_delete(), some map entries might have been
			 * transferred to a "zap_map", which doesn't have a
			 * pmap.  The original pmap has already been flushed
			 * in the vm_map_delete() call targeting the original
			 * map, but when we get to destroying the "zap_map",
			 * we don't have any pmap to flush, so let's just skip
			 * all this.
			 */
		} else if (entry->is_sub_map) {
			if (entry->use_pmap) {
#ifndef NO_NESTED_PMAP
				int pmap_flags;

				if (flags & VM_MAP_REMOVE_NO_UNNESTING) {
					/*
					 * This is the final cleanup of the
					 * address space being terminated.
					 * No new mappings are expected and
					 * we don't really need to unnest the
					 * shared region (and lose the "global"
					 * pmap mappings, if applicable).
					 *
					 * Tell the pmap layer that we're
					 * "clean" wrt nesting.
					 */
					pmap_flags = PMAP_UNNEST_CLEAN;
				} else {
					/*
					 * We're unmapping part of the nested
					 * shared region, so we can't keep the
					 * nested pmap.
					 */
					pmap_flags = 0;
				}
				pmap_unnest_options(
					map->pmap,
					(addr64_t)entry->vme_start,
					entry->vme_end - entry->vme_start,
					pmap_flags);
#endif	/* NO_NESTED_PMAP */
				if ((map->mapped_in_other_pmaps) && (map->ref_count)) {
					/* clean up parent map/maps */
					vm_map_submap_pmap_clean(
						map, entry->vme_start,
						entry->vme_end,
						VME_SUBMAP(entry),
						VME_OFFSET(entry));
				}
			} else {
				vm_map_submap_pmap_clean(
					map, entry->vme_start, entry->vme_end,
					VME_SUBMAP(entry),
					VME_OFFSET(entry));
			}
		} else if (VME_OBJECT(entry) != kernel_object &&
			   VME_OBJECT(entry) != compressor_object) {
			object = VME_OBJECT(entry);
			if ((map->mapped_in_other_pmaps) && (map->ref_count)) {
				vm_object_pmap_protect_options(
					object, VME_OFFSET(entry),
					entry->vme_end - entry->vme_start,
					PMAP_NULL,
					entry->vme_start,
					VM_PROT_NONE,
					PMAP_OPTIONS_REMOVE);
			} else if ((VME_OBJECT(entry) != VM_OBJECT_NULL) ||
				   (map->pmap == kernel_pmap)) {
				/* Remove translations associated
				 * with this range unless the entry
				 * does not have an object, or
				 * it's the kernel map or a descendant
				 * since the platform could potentially
				 * create "backdoor" mappings invisible
				 * to the VM. It is expected that
				 * objectless, non-kernel ranges
				 * do not have such VM invisible
				 * translations.
				 */
				pmap_remove_options(map->pmap,
						    (addr64_t)entry->vme_start,
						    (addr64_t)entry->vme_end,
						    PMAP_OPTIONS_REMOVE);
			}
		}

		if (entry->iokit_acct) {
			/* alternate accounting */
			DTRACE_VM4(vm_map_iokit_unmapped_region,
				   vm_map_t, map,
				   vm_map_offset_t, entry->vme_start,
				   vm_map_offset_t, entry->vme_end,
				   int, VME_ALIAS(entry));
			vm_map_iokit_unmapped_region(map,
						     (entry->vme_end -
						      entry->vme_start));
			entry->iokit_acct = FALSE;
			entry->use_pmap = FALSE;
		}

		/*
		 * All pmap mappings for this map entry must have been
		 * cleared by now.
		 */
#if DEBUG
		assert(vm_map_pmap_is_empty(map,
					    entry->vme_start,
					    entry->vme_end));
#endif /* DEBUG */

		next = entry->vme_next;

		if (map->pmap == kernel_pmap &&
		    map->ref_count != 0 &&
		    entry->vme_end < end &&
		    (next == vm_map_to_entry(map) ||
		     next->vme_start != entry->vme_end)) {
			panic("vm_map_delete(%p,0x%llx,0x%llx): "
			      "hole after %p at 0x%llx\n",
			      map,
			      (uint64_t)start,
			      (uint64_t)end,
			      entry,
			      (uint64_t)entry->vme_end);
		}

		s = next->vme_start;
		last_timestamp = map->timestamp;

		if (entry->permanent) {
			/*
			 * A permanent entry can not be removed, so leave it
			 * in place but remove all access permissions.
			 */
			entry->protection = VM_PROT_NONE;
			entry->max_protection = VM_PROT_NONE;
		} else if ((flags & VM_MAP_REMOVE_SAVE_ENTRIES) &&
			   zap_map != VM_MAP_NULL) {
			vm_map_size_t entry_size;
			/*
			 * The caller wants to save the affected VM map entries
			 * into the "zap_map".  The caller will take care of
			 * these entries.
			 */
			/* unlink the entry from "map" ... */
			vm_map_store_entry_unlink(map, entry);
			/* ... and add it to the end of the "zap_map" */
			vm_map_store_entry_link(zap_map,
					  vm_map_last_entry(zap_map),
					  entry);
			entry_size = entry->vme_end - entry->vme_start;
			map->size -= entry_size;
			zap_map->size += entry_size;
			/* we didn't unlock the map, so no timestamp increase */
			last_timestamp--;
		} else {
			vm_map_entry_delete(map, entry);
			/* vm_map_entry_delete unlocks the map */
			vm_map_lock(map);
		}

		entry = next;

		if(entry == vm_map_to_entry(map)) {
			break;
		}
		if (last_timestamp+1 != map->timestamp) {
			/*
			 * we are responsible for deleting everything
			 * from the give space, if someone has interfered
			 * we pick up where we left off, back fills should
			 * be all right for anyone except map_delete and
			 * we have to assume that the task has been fully
			 * disabled before we get here
			 */
        		if (!vm_map_lookup_entry(map, s, &entry)){
	               		entry = entry->vme_next;
				s = entry->vme_start;
        		} else {
				SAVE_HINT_MAP_WRITE(map, entry->vme_prev);
       		 	}
			/*
			 * others can not only allocate behind us, we can
			 * also see coalesce while we don't have the map lock
			 */
			if(entry == vm_map_to_entry(map)) {
				break;
			}
		}
		last_timestamp = map->timestamp;
	}

	if (map->wait_for_space)
		thread_wakeup((event_t) map);
	/*
	 * wake up anybody waiting on entries that we have already deleted.
	 */
	if (need_wakeup)
		vm_map_entry_wakeup(map);

	return KERN_SUCCESS;
}

/*
 *	vm_map_remove:
 *
 *	Remove the given address range from the target map.
 *	This is the exported form of vm_map_delete.
 */
kern_return_t
vm_map_remove(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	 boolean_t	flags)
{
	kern_return_t	result;

	vm_map_lock(map);
	VM_MAP_RANGE_CHECK(map, start, end);
	/*
	 * For the zone_map, the kernel controls the allocation/freeing of memory.
	 * Any free to the zone_map should be within the bounds of the map and
	 * should free up memory. If the VM_MAP_RANGE_CHECK() silently converts a
	 * free to the zone_map into a no-op, there is a problem and we should
	 * panic.
	 */
	if ((map == zone_map) && (start == end))
		panic("Nothing being freed to the zone_map. start = end = %p\n", (void *)start);
	result = vm_map_delete(map, start, end, flags, VM_MAP_NULL);
	vm_map_unlock(map);

	return(result);
}

/*
 *	vm_map_remove_locked:
 *
 *	Remove the given address range from the target locked map.
 *	This is the exported form of vm_map_delete.
 */
kern_return_t
vm_map_remove_locked(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	boolean_t	flags)
{
	kern_return_t	result;

	VM_MAP_RANGE_CHECK(map, start, end);
	result = vm_map_delete(map, start, end, flags, VM_MAP_NULL);
	return(result);
}


/*
 *	Routine:	vm_map_copy_discard
 *
 *	Description:
 *		Dispose of a map copy object (returned by
 *		vm_map_copyin).
 */
void
vm_map_copy_discard(
	vm_map_copy_t	copy)
{
	if (copy == VM_MAP_COPY_NULL)
		return;

	switch (copy->type) {
	case VM_MAP_COPY_ENTRY_LIST:
		while (vm_map_copy_first_entry(copy) !=
		       vm_map_copy_to_entry(copy)) {
			vm_map_entry_t	entry = vm_map_copy_first_entry(copy);

			vm_map_copy_entry_unlink(copy, entry);
			if (entry->is_sub_map) {
				vm_map_deallocate(VME_SUBMAP(entry));
			} else {
				vm_object_deallocate(VME_OBJECT(entry));
			}
			vm_map_copy_entry_dispose(copy, entry);
		}
		break;
        case VM_MAP_COPY_OBJECT:
		vm_object_deallocate(copy->cpy_object);
		break;
	case VM_MAP_COPY_KERNEL_BUFFER:

		/*
		 * The vm_map_copy_t and possibly the data buffer were
		 * allocated by a single call to kalloc(), i.e. the
		 * vm_map_copy_t was not allocated out of the zone.
		 */
		if (copy->size > msg_ool_size_small || copy->offset)
			panic("Invalid vm_map_copy_t sz:%lld, ofst:%lld",
			      (long long)copy->size, (long long)copy->offset);
		kfree(copy, copy->size + cpy_kdata_hdr_sz);
		return;
	}
	zfree(vm_map_copy_zone, copy);
}

/*
 *	Routine:	vm_map_copy_copy
 *
 *	Description:
 *			Move the information in a map copy object to
 *			a new map copy object, leaving the old one
 *			empty.
 *
 *			This is used by kernel routines that need
 *			to look at out-of-line data (in copyin form)
 *			before deciding whether to return SUCCESS.
 *			If the routine returns FAILURE, the original
 *			copy object will be deallocated; therefore,
 *			these routines must make a copy of the copy
 *			object and leave the original empty so that
 *			deallocation will not fail.
 */
vm_map_copy_t
vm_map_copy_copy(
	vm_map_copy_t	copy)
{
	vm_map_copy_t	new_copy;

	if (copy == VM_MAP_COPY_NULL)
		return VM_MAP_COPY_NULL;

	/*
	 * Allocate a new copy object, and copy the information
	 * from the old one into it.
	 */

	new_copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
	new_copy->c_u.hdr.rb_head_store.rbh_root = (void*)(int)SKIP_RB_TREE;
	*new_copy = *copy;

	if (copy->type == VM_MAP_COPY_ENTRY_LIST) {
		/*
		 * The links in the entry chain must be
		 * changed to point to the new copy object.
		 */
		vm_map_copy_first_entry(copy)->vme_prev
			= vm_map_copy_to_entry(new_copy);
		vm_map_copy_last_entry(copy)->vme_next
			= vm_map_copy_to_entry(new_copy);
	}

	/*
	 * Change the old copy object into one that contains
	 * nothing to be deallocated.
	 */
	copy->type = VM_MAP_COPY_OBJECT;
	copy->cpy_object = VM_OBJECT_NULL;

	/*
	 * Return the new object.
	 */
	return new_copy;
}

static kern_return_t
vm_map_overwrite_submap_recurse(
	vm_map_t	dst_map,
	vm_map_offset_t	dst_addr,
	vm_map_size_t	dst_size)
{
	vm_map_offset_t	dst_end;
	vm_map_entry_t	tmp_entry;
	vm_map_entry_t	entry;
	kern_return_t	result;
	boolean_t	encountered_sub_map = FALSE;



	/*
	 *	Verify that the destination is all writeable
	 *	initially.  We have to trunc the destination
	 *	address and round the copy size or we'll end up
	 *	splitting entries in strange ways.
	 */

	dst_end = vm_map_round_page(dst_addr + dst_size,
				    VM_MAP_PAGE_MASK(dst_map));
	vm_map_lock(dst_map);

start_pass_1:
	if (!vm_map_lookup_entry(dst_map, dst_addr, &tmp_entry)) {
		vm_map_unlock(dst_map);
		return(KERN_INVALID_ADDRESS);
	}

	vm_map_clip_start(dst_map,
			  tmp_entry,
			  vm_map_trunc_page(dst_addr,
					    VM_MAP_PAGE_MASK(dst_map)));
	if (tmp_entry->is_sub_map) {
		/* clipping did unnest if needed */
		assert(!tmp_entry->use_pmap);
	}

	for (entry = tmp_entry;;) {
		vm_map_entry_t	next;

		next = entry->vme_next;
		while(entry->is_sub_map) {
			vm_map_offset_t	sub_start;
			vm_map_offset_t	sub_end;
			vm_map_offset_t	local_end;

			if (entry->in_transition) {
				/*
				 * Say that we are waiting, and wait for entry.
				 */
                        	entry->needs_wakeup = TRUE;
                        	vm_map_entry_wait(dst_map, THREAD_UNINT);

				goto start_pass_1;
			}

			encountered_sub_map = TRUE;
			sub_start = VME_OFFSET(entry);

			if(entry->vme_end < dst_end)
				sub_end = entry->vme_end;
			else
				sub_end = dst_end;
			sub_end -= entry->vme_start;
			sub_end += VME_OFFSET(entry);
			local_end = entry->vme_end;
			vm_map_unlock(dst_map);

			result = vm_map_overwrite_submap_recurse(
				VME_SUBMAP(entry),
				sub_start,
				sub_end - sub_start);

			if(result != KERN_SUCCESS)
				return result;
			if (dst_end <= entry->vme_end)
				return KERN_SUCCESS;
			vm_map_lock(dst_map);
			if(!vm_map_lookup_entry(dst_map, local_end,
						&tmp_entry)) {
				vm_map_unlock(dst_map);
				return(KERN_INVALID_ADDRESS);
			}
			entry = tmp_entry;
			next = entry->vme_next;
		}

		if ( ! (entry->protection & VM_PROT_WRITE)) {
			vm_map_unlock(dst_map);
			return(KERN_PROTECTION_FAILURE);
		}

		/*
		 *	If the entry is in transition, we must wait
		 *	for it to exit that state.  Anything could happen
		 *	when we unlock the map, so start over.
		 */
                if (entry->in_transition) {

                        /*
                         * Say that we are waiting, and wait for entry.
                         */
                        entry->needs_wakeup = TRUE;
                        vm_map_entry_wait(dst_map, THREAD_UNINT);

			goto start_pass_1;
		}

/*
 *		our range is contained completely within this map entry
 */
		if (dst_end <= entry->vme_end) {
			vm_map_unlock(dst_map);
			return KERN_SUCCESS;
		}
/*
 *		check that range specified is contiguous region
 */
		if ((next == vm_map_to_entry(dst_map)) ||
		    (next->vme_start != entry->vme_end)) {
			vm_map_unlock(dst_map);
			return(KERN_INVALID_ADDRESS);
		}

		/*
		 *	Check for permanent objects in the destination.
		 */
		if ((VME_OBJECT(entry) != VM_OBJECT_NULL) &&
		    ((!VME_OBJECT(entry)->internal) ||
		     (VME_OBJECT(entry)->true_share))) {
			if(encountered_sub_map) {
				vm_map_unlock(dst_map);
				return(KERN_FAILURE);
			}
		}


		entry = next;
	}/* for */
	vm_map_unlock(dst_map);
	return(KERN_SUCCESS);
}

/*
 *	Routine:	vm_map_copy_overwrite
 *
 *	Description:
 *		Copy the memory described by the map copy
 *		object (copy; returned by vm_map_copyin) onto
 *		the specified destination region (dst_map, dst_addr).
 *		The destination must be writeable.
 *
 *		Unlike vm_map_copyout, this routine actually
 *		writes over previously-mapped memory.  If the
 *		previous mapping was to a permanent (user-supplied)
 *		memory object, it is preserved.
 *
 *		The attributes (protection and inheritance) of the
 *		destination region are preserved.
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 *
 *	Implementation notes:
 *		To overwrite aligned temporary virtual memory, it is
 *		sufficient to remove the previous mapping and insert
 *		the new copy.  This replacement is done either on
 *		the whole region (if no permanent virtual memory
 *		objects are embedded in the destination region) or
 *		in individual map entries.
 *
 *		To overwrite permanent virtual memory , it is necessary
 *		to copy each page, as the external memory management
 *		interface currently does not provide any optimizations.
 *
 *		Unaligned memory also has to be copied.  It is possible
 *		to use 'vm_trickery' to copy the aligned data.  This is
 *		not done but not hard to implement.
 *
 *		Once a page of permanent memory has been overwritten,
 *		it is impossible to interrupt this function; otherwise,
 *		the call would be neither atomic nor location-independent.
 *		The kernel-state portion of a user thread must be
 *		interruptible.
 *
 *		It may be expensive to forward all requests that might
 *		overwrite permanent memory (vm_write, vm_copy) to
 *		uninterruptible kernel threads.  This routine may be
 *		called by interruptible threads; however, success is
 *		not guaranteed -- if the request cannot be performed
 *		atomically and interruptibly, an error indication is
 *		returned.
 */

static kern_return_t
vm_map_copy_overwrite_nested(
	vm_map_t		dst_map,
	vm_map_address_t	dst_addr,
	vm_map_copy_t		copy,
	boolean_t		interruptible,
	pmap_t			pmap,
	boolean_t		discard_on_success)
{
	vm_map_offset_t		dst_end;
	vm_map_entry_t		tmp_entry;
	vm_map_entry_t		entry;
	kern_return_t		kr;
	boolean_t		aligned = TRUE;
	boolean_t		contains_permanent_objects = FALSE;
	boolean_t		encountered_sub_map = FALSE;
	vm_map_offset_t		base_addr;
	vm_map_size_t		copy_size;
	vm_map_size_t		total_size;


	/*
	 *	Check for null copy object.
	 */

	if (copy == VM_MAP_COPY_NULL)
		return(KERN_SUCCESS);

	/*
	 *	Check for special kernel buffer allocated
	 *	by new_ipc_kmsg_copyin.
	 */

	if (copy->type == VM_MAP_COPY_KERNEL_BUFFER) {
		return(vm_map_copyout_kernel_buffer(
			       dst_map, &dst_addr,
			       copy, copy->size, TRUE, discard_on_success));
	}

	/*
	 *      Only works for entry lists at the moment.  Will
	 *	support page lists later.
	 */

	assert(copy->type == VM_MAP_COPY_ENTRY_LIST);

	if (copy->size == 0) {
		if (discard_on_success)
			vm_map_copy_discard(copy);
		return(KERN_SUCCESS);
	}

	/*
	 *	Verify that the destination is all writeable
	 *	initially.  We have to trunc the destination
	 *	address and round the copy size or we'll end up
	 *	splitting entries in strange ways.
	 */

	if (!VM_MAP_PAGE_ALIGNED(copy->size,
				 VM_MAP_PAGE_MASK(dst_map)) ||
	    !VM_MAP_PAGE_ALIGNED(copy->offset,
				 VM_MAP_PAGE_MASK(dst_map)) ||
	    !VM_MAP_PAGE_ALIGNED(dst_addr,
				 VM_MAP_PAGE_MASK(dst_map)))
	{
		aligned = FALSE;
		dst_end = vm_map_round_page(dst_addr + copy->size,
					    VM_MAP_PAGE_MASK(dst_map));
	} else {
		dst_end = dst_addr + copy->size;
	}

	vm_map_lock(dst_map);

	/* LP64todo - remove this check when vm_map_commpage64()
	 * no longer has to stuff in a map_entry for the commpage
	 * above the map's max_offset.
	 */
	if (dst_addr >= dst_map->max_offset) {
		vm_map_unlock(dst_map);
		return(KERN_INVALID_ADDRESS);
	}

start_pass_1:
	if (!vm_map_lookup_entry(dst_map, dst_addr, &tmp_entry)) {
		vm_map_unlock(dst_map);
		return(KERN_INVALID_ADDRESS);
	}
	vm_map_clip_start(dst_map,
			  tmp_entry,
			  vm_map_trunc_page(dst_addr,
					    VM_MAP_PAGE_MASK(dst_map)));
	for (entry = tmp_entry;;) {
		vm_map_entry_t	next = entry->vme_next;

		while(entry->is_sub_map) {
			vm_map_offset_t	sub_start;
			vm_map_offset_t	sub_end;
			vm_map_offset_t	local_end;

                	if (entry->in_transition) {

				/*
				 * Say that we are waiting, and wait for entry.
				 */
                        	entry->needs_wakeup = TRUE;
                        	vm_map_entry_wait(dst_map, THREAD_UNINT);

				goto start_pass_1;
			}

			local_end = entry->vme_end;
		        if (!(entry->needs_copy)) {
				/* if needs_copy we are a COW submap */
				/* in such a case we just replace so */
				/* there is no need for the follow-  */
				/* ing check.                        */
				encountered_sub_map = TRUE;
				sub_start = VME_OFFSET(entry);

				if(entry->vme_end < dst_end)
					sub_end = entry->vme_end;
				else
					sub_end = dst_end;
				sub_end -= entry->vme_start;
				sub_end += VME_OFFSET(entry);
				vm_map_unlock(dst_map);

				kr = vm_map_overwrite_submap_recurse(
					VME_SUBMAP(entry),
					sub_start,
					sub_end - sub_start);
				if(kr != KERN_SUCCESS)
					return kr;
				vm_map_lock(dst_map);
			}

			if (dst_end <= entry->vme_end)
				goto start_overwrite;
			if(!vm_map_lookup_entry(dst_map, local_end,
						&entry)) {
				vm_map_unlock(dst_map);
				return(KERN_INVALID_ADDRESS);
			}
			next = entry->vme_next;
		}

		if ( ! (entry->protection & VM_PROT_WRITE)) {
			vm_map_unlock(dst_map);
			return(KERN_PROTECTION_FAILURE);
		}

		/*
		 *	If the entry is in transition, we must wait
		 *	for it to exit that state.  Anything could happen
		 *	when we unlock the map, so start over.
		 */
                if (entry->in_transition) {

                        /*
                         * Say that we are waiting, and wait for entry.
                         */
                        entry->needs_wakeup = TRUE;
                        vm_map_entry_wait(dst_map, THREAD_UNINT);

			goto start_pass_1;
		}

/*
 *		our range is contained completely within this map entry
 */
		if (dst_end <= entry->vme_end)
			break;
/*
 *		check that range specified is contiguous region
 */
		if ((next == vm_map_to_entry(dst_map)) ||
		    (next->vme_start != entry->vme_end)) {
			vm_map_unlock(dst_map);
			return(KERN_INVALID_ADDRESS);
		}


		/*
		 *	Check for permanent objects in the destination.
		 */
		if ((VME_OBJECT(entry) != VM_OBJECT_NULL) &&
		    ((!VME_OBJECT(entry)->internal) ||
		     (VME_OBJECT(entry)->true_share))) {
			contains_permanent_objects = TRUE;
		}

		entry = next;
	}/* for */

start_overwrite:
	/*
	 *	If there are permanent objects in the destination, then
	 *	the copy cannot be interrupted.
	 */

	if (interruptible && contains_permanent_objects) {
		vm_map_unlock(dst_map);
		return(KERN_FAILURE);	/* XXX */
	}

	/*
 	 *
	 *	Make a second pass, overwriting the data
	 *	At the beginning of each loop iteration,
	 *	the next entry to be overwritten is "tmp_entry"
	 *	(initially, the value returned from the lookup above),
	 *	and the starting address expected in that entry
	 *	is "start".
	 */

	total_size = copy->size;
	if(encountered_sub_map) {
		copy_size = 0;
		/* re-calculate tmp_entry since we've had the map */
		/* unlocked */
		if (!vm_map_lookup_entry( dst_map, dst_addr, &tmp_entry)) {
			vm_map_unlock(dst_map);
			return(KERN_INVALID_ADDRESS);
		}
	} else {
		copy_size = copy->size;
	}

	base_addr = dst_addr;
	while(TRUE) {
		/* deconstruct the copy object and do in parts */
		/* only in sub_map, interruptable case */
		vm_map_entry_t	copy_entry;
		vm_map_entry_t	previous_prev = VM_MAP_ENTRY_NULL;
		vm_map_entry_t	next_copy = VM_MAP_ENTRY_NULL;
		int		nentries;
		int		remaining_entries = 0;
		vm_map_offset_t	new_offset = 0;

		for (entry = tmp_entry; copy_size == 0;) {
			vm_map_entry_t	next;

			next = entry->vme_next;

			/* tmp_entry and base address are moved along */
			/* each time we encounter a sub-map.  Otherwise */
			/* entry can outpase tmp_entry, and the copy_size */
			/* may reflect the distance between them */
			/* if the current entry is found to be in transition */
			/* we will start over at the beginning or the last */
			/* encounter of a submap as dictated by base_addr */
			/* we will zero copy_size accordingly. */
			if (entry->in_transition) {
                       		/*
                       		 * Say that we are waiting, and wait for entry.
                       		 */
                       		entry->needs_wakeup = TRUE;
                       		vm_map_entry_wait(dst_map, THREAD_UNINT);

				if(!vm_map_lookup_entry(dst_map, base_addr,
							&tmp_entry)) {
					vm_map_unlock(dst_map);
					return(KERN_INVALID_ADDRESS);
				}
				copy_size = 0;
				entry = tmp_entry;
				continue;
			}
			if (entry->is_sub_map) {
				vm_map_offset_t	sub_start;
				vm_map_offset_t	sub_end;
				vm_map_offset_t	local_end;

		        	if (entry->needs_copy) {
					/* if this is a COW submap */
					/* just back the range with a */
					/* anonymous entry */
					if(entry->vme_end < dst_end)
						sub_end = entry->vme_end;
					else
						sub_end = dst_end;
					if(entry->vme_start < base_addr)
						sub_start = base_addr;
					else
						sub_start = entry->vme_start;
					vm_map_clip_end(
						dst_map, entry, sub_end);
					vm_map_clip_start(
						dst_map, entry, sub_start);
					assert(!entry->use_pmap);
					assert(!entry->iokit_acct);
					entry->use_pmap = TRUE;
					entry->is_sub_map = FALSE;
					vm_map_deallocate(
						VME_SUBMAP(entry));
					VME_OBJECT_SET(entry, NULL);
					VME_OFFSET_SET(entry, 0);
					entry->is_shared = FALSE;
					entry->needs_copy = FALSE;
					entry->protection = VM_PROT_DEFAULT;
					entry->max_protection = VM_PROT_ALL;
					entry->wired_count = 0;
					entry->user_wired_count = 0;
					if(entry->inheritance
					   == VM_INHERIT_SHARE)
						entry->inheritance = VM_INHERIT_COPY;
					continue;
				}
				/* first take care of any non-sub_map */
				/* entries to send */
				if(base_addr < entry->vme_start) {
					/* stuff to send */
					copy_size =
						entry->vme_start - base_addr;
					break;
				}
				sub_start = VME_OFFSET(entry);

				if(entry->vme_end < dst_end)
					sub_end = entry->vme_end;
				else
					sub_end = dst_end;
				sub_end -= entry->vme_start;
				sub_end += VME_OFFSET(entry);
				local_end = entry->vme_end;
				vm_map_unlock(dst_map);
				copy_size = sub_end - sub_start;

				/* adjust the copy object */
				if (total_size > copy_size) {
					vm_map_size_t	local_size = 0;
					vm_map_size_t	entry_size;

					nentries = 1;
					new_offset = copy->offset;
					copy_entry = vm_map_copy_first_entry(copy);
					while(copy_entry !=
					      vm_map_copy_to_entry(copy)){
						entry_size = copy_entry->vme_end -
							copy_entry->vme_start;
						if((local_size < copy_size) &&
						   ((local_size + entry_size)
						    >= copy_size)) {
							vm_map_copy_clip_end(copy,
									     copy_entry,
									     copy_entry->vme_start +
									     (copy_size - local_size));
							entry_size = copy_entry->vme_end -
								copy_entry->vme_start;
							local_size += entry_size;
							new_offset += entry_size;
						}
						if(local_size >= copy_size) {
							next_copy = copy_entry->vme_next;
							copy_entry->vme_next =
								vm_map_copy_to_entry(copy);
							previous_prev =
								copy->cpy_hdr.links.prev;
							copy->cpy_hdr.links.prev = copy_entry;
							copy->size = copy_size;
							remaining_entries =
								copy->cpy_hdr.nentries;
							remaining_entries -= nentries;
							copy->cpy_hdr.nentries = nentries;
							break;
						} else {
							local_size += entry_size;
							new_offset += entry_size;
							nentries++;
						}
						copy_entry = copy_entry->vme_next;
					}
				}

				if((entry->use_pmap) && (pmap == NULL)) {
					kr = vm_map_copy_overwrite_nested(
						VME_SUBMAP(entry),
						sub_start,
						copy,
						interruptible,
						VME_SUBMAP(entry)->pmap,
						TRUE);
				} else if (pmap != NULL) {
					kr = vm_map_copy_overwrite_nested(
						VME_SUBMAP(entry),
						sub_start,
						copy,
						interruptible, pmap,
						TRUE);
				} else {
					kr = vm_map_copy_overwrite_nested(
						VME_SUBMAP(entry),
						sub_start,
						copy,
						interruptible,
						dst_map->pmap,
						TRUE);
				}
				if(kr != KERN_SUCCESS) {
					if(next_copy != NULL) {
						copy->cpy_hdr.nentries +=
							remaining_entries;
						copy->cpy_hdr.links.prev->vme_next =
							next_copy;
						copy->cpy_hdr.links.prev
							= previous_prev;
						copy->size = total_size;
					}
					return kr;
				}
				if (dst_end <= local_end) {
					return(KERN_SUCCESS);
				}
				/* otherwise copy no longer exists, it was */
				/* destroyed after successful copy_overwrite */
			        copy = (vm_map_copy_t)
					zalloc(vm_map_copy_zone);
				copy->c_u.hdr.rb_head_store.rbh_root = (void*)(int)SKIP_RB_TREE;
				vm_map_copy_first_entry(copy) =
					vm_map_copy_last_entry(copy) =
					vm_map_copy_to_entry(copy);
				copy->type = VM_MAP_COPY_ENTRY_LIST;
				copy->offset = new_offset;

				/*
				 * XXX FBDP
				 * this does not seem to deal with
				 * the VM map store (R&B tree)
				 */

				total_size -= copy_size;
				copy_size = 0;
				/* put back remainder of copy in container */
				if(next_copy != NULL) {
					copy->cpy_hdr.nentries = remaining_entries;
					copy->cpy_hdr.links.next = next_copy;
					copy->cpy_hdr.links.prev = previous_prev;
					copy->size = total_size;
					next_copy->vme_prev =
						vm_map_copy_to_entry(copy);
					next_copy = NULL;
				}
				base_addr = local_end;
				vm_map_lock(dst_map);
				if(!vm_map_lookup_entry(dst_map,
							local_end, &tmp_entry)) {
					vm_map_unlock(dst_map);
					return(KERN_INVALID_ADDRESS);
				}
				entry = tmp_entry;
				continue;
			}
			if (dst_end <= entry->vme_end) {
				copy_size = dst_end - base_addr;
				break;
			}

			if ((next == vm_map_to_entry(dst_map)) ||
			    (next->vme_start != entry->vme_end)) {
				vm_map_unlock(dst_map);
				return(KERN_INVALID_ADDRESS);
			}

			entry = next;
		}/* for */

		next_copy = NULL;
		nentries = 1;

		/* adjust the copy object */
		if (total_size > copy_size) {
			vm_map_size_t	local_size = 0;
			vm_map_size_t	entry_size;

			new_offset = copy->offset;
			copy_entry = vm_map_copy_first_entry(copy);
			while(copy_entry != vm_map_copy_to_entry(copy)) {
				entry_size = copy_entry->vme_end -
					copy_entry->vme_start;
				if((local_size < copy_size) &&
				   ((local_size + entry_size)
				    >= copy_size)) {
					vm_map_copy_clip_end(copy, copy_entry,
							     copy_entry->vme_start +
							     (copy_size - local_size));
					entry_size = copy_entry->vme_end -
						copy_entry->vme_start;
					local_size += entry_size;
					new_offset += entry_size;
				}
				if(local_size >= copy_size) {
					next_copy = copy_entry->vme_next;
					copy_entry->vme_next =
						vm_map_copy_to_entry(copy);
					previous_prev =
						copy->cpy_hdr.links.prev;
					copy->cpy_hdr.links.prev = copy_entry;
					copy->size = copy_size;
					remaining_entries =
						copy->cpy_hdr.nentries;
					remaining_entries -= nentries;
					copy->cpy_hdr.nentries = nentries;
					break;
				} else {
					local_size += entry_size;
					new_offset += entry_size;
					nentries++;
				}
				copy_entry = copy_entry->vme_next;
			}
		}

		if (aligned) {
			pmap_t	local_pmap;

			if(pmap)
				local_pmap = pmap;
			else
				local_pmap = dst_map->pmap;

			if ((kr =  vm_map_copy_overwrite_aligned(
				     dst_map, tmp_entry, copy,
				     base_addr, local_pmap)) != KERN_SUCCESS) {
				if(next_copy != NULL) {
					copy->cpy_hdr.nentries +=
						remaining_entries;
				        copy->cpy_hdr.links.prev->vme_next =
						next_copy;
			       		copy->cpy_hdr.links.prev =
						previous_prev;
					copy->size += copy_size;
				}
				return kr;
			}
			vm_map_unlock(dst_map);
		} else {
			/*
			 * Performance gain:
			 *
			 * if the copy and dst address are misaligned but the same
			 * offset within the page we can copy_not_aligned the
			 * misaligned parts and copy aligned the rest.  If they are
			 * aligned but len is unaligned we simply need to copy
			 * the end bit unaligned.  We'll need to split the misaligned
			 * bits of the region in this case !
			 */
			/* ALWAYS UNLOCKS THE dst_map MAP */
			kr = vm_map_copy_overwrite_unaligned(
				dst_map,
				tmp_entry,
				copy,
				base_addr,
				discard_on_success);
			if (kr != KERN_SUCCESS) {
				if(next_copy != NULL) {
					copy->cpy_hdr.nentries +=
						remaining_entries;
			       		copy->cpy_hdr.links.prev->vme_next =
						next_copy;
			       		copy->cpy_hdr.links.prev =
						previous_prev;
					copy->size += copy_size;
				}
				return kr;
			}
		}
		total_size -= copy_size;
		if(total_size == 0)
			break;
		base_addr += copy_size;
		copy_size = 0;
		copy->offset = new_offset;
		if(next_copy != NULL) {
			copy->cpy_hdr.nentries = remaining_entries;
			copy->cpy_hdr.links.next = next_copy;
			copy->cpy_hdr.links.prev = previous_prev;
			next_copy->vme_prev = vm_map_copy_to_entry(copy);
			copy->size = total_size;
		}
		vm_map_lock(dst_map);
		while(TRUE) {
			if (!vm_map_lookup_entry(dst_map,
						 base_addr, &tmp_entry)) {
				vm_map_unlock(dst_map);
				return(KERN_INVALID_ADDRESS);
			}
                	if (tmp_entry->in_transition) {
                       		entry->needs_wakeup = TRUE;
                       		vm_map_entry_wait(dst_map, THREAD_UNINT);
			} else {
				break;
			}
		}
		vm_map_clip_start(dst_map,
				  tmp_entry,
				  vm_map_trunc_page(base_addr,
						    VM_MAP_PAGE_MASK(dst_map)));

		entry = tmp_entry;
	} /* while */

	/*
	 *	Throw away the vm_map_copy object
	 */
	if (discard_on_success)
		vm_map_copy_discard(copy);

	return(KERN_SUCCESS);
}/* vm_map_copy_overwrite */

kern_return_t
vm_map_copy_overwrite(
	vm_map_t	dst_map,
	vm_map_offset_t	dst_addr,
	vm_map_copy_t	copy,
	boolean_t	interruptible)
{
	vm_map_size_t	head_size, tail_size;
	vm_map_copy_t	head_copy, tail_copy;
	vm_map_offset_t	head_addr, tail_addr;
	vm_map_entry_t	entry;
	kern_return_t	kr;
	vm_map_offset_t	effective_page_mask, effective_page_size;

	head_size = 0;
	tail_size = 0;
	head_copy = NULL;
	tail_copy = NULL;
	head_addr = 0;
	tail_addr = 0;

	if (interruptible ||
	    copy == VM_MAP_COPY_NULL ||
	    copy->type != VM_MAP_COPY_ENTRY_LIST) {
		/*
		 * We can't split the "copy" map if we're interruptible
		 * or if we don't have a "copy" map...
		 */
	blunt_copy:
		return vm_map_copy_overwrite_nested(dst_map,
						    dst_addr,
						    copy,
						    interruptible,
						    (pmap_t) NULL,
						    TRUE);
	}

	effective_page_mask = MAX(VM_MAP_PAGE_MASK(dst_map), PAGE_MASK);
	effective_page_mask = MAX(VM_MAP_COPY_PAGE_MASK(copy),
				  effective_page_mask);
	effective_page_size = effective_page_mask + 1;

	if (copy->size < 3 * effective_page_size) {
		/*
		 * Too small to bother with optimizing...
		 */
		goto blunt_copy;
	}

	if ((dst_addr & effective_page_mask) !=
	    (copy->offset & effective_page_mask)) {
		/*
		 * Incompatible mis-alignment of source and destination...
		 */
		goto blunt_copy;
	}

	/*
	 * Proper alignment or identical mis-alignment at the beginning.
	 * Let's try and do a small unaligned copy first (if needed)
	 * and then an aligned copy for the rest.
	 */
	if (!vm_map_page_aligned(dst_addr, effective_page_mask)) {
		head_addr = dst_addr;
		head_size = (effective_page_size -
			     (copy->offset & effective_page_mask));
		head_size = MIN(head_size, copy->size);
	}
	if (!vm_map_page_aligned(copy->offset + copy->size,
				  effective_page_mask)) {
		/*
		 * Mis-alignment at the end.
		 * Do an aligned copy up to the last page and
		 * then an unaligned copy for the remaining bytes.
		 */
		tail_size = ((copy->offset + copy->size) &
			     effective_page_mask);
		tail_size = MIN(tail_size, copy->size);
		tail_addr = dst_addr + copy->size - tail_size;
		assert(tail_addr >= head_addr + head_size);
	}
	assert(head_size + tail_size <= copy->size);

	if (head_size + tail_size == copy->size) {
		/*
		 * It's all unaligned, no optimization possible...
		 */
		goto blunt_copy;
	}

	/*
	 * Can't optimize if there are any submaps in the
	 * destination due to the way we free the "copy" map
	 * progressively in vm_map_copy_overwrite_nested()
	 * in that case.
	 */
	vm_map_lock_read(dst_map);
	if (! vm_map_lookup_entry(dst_map, dst_addr, &entry)) {
		vm_map_unlock_read(dst_map);
		goto blunt_copy;
	}
	for (;
	     (entry != vm_map_copy_to_entry(copy) &&
	      entry->vme_start < dst_addr + copy->size);
	     entry = entry->vme_next) {
		if (entry->is_sub_map) {
			vm_map_unlock_read(dst_map);
			goto blunt_copy;
		}
	}
	vm_map_unlock_read(dst_map);

	if (head_size) {
		/*
		 * Unaligned copy of the first "head_size" bytes, to reach
		 * a page boundary.
		 */

		/*
		 * Extract "head_copy" out of "copy".
		 */
		head_copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
		head_copy->c_u.hdr.rb_head_store.rbh_root = (void*)(int)SKIP_RB_TREE;
		vm_map_copy_first_entry(head_copy) =
			vm_map_copy_to_entry(head_copy);
		vm_map_copy_last_entry(head_copy) =
			vm_map_copy_to_entry(head_copy);
		head_copy->type = VM_MAP_COPY_ENTRY_LIST;
		head_copy->cpy_hdr.nentries = 0;
		head_copy->cpy_hdr.entries_pageable =
			copy->cpy_hdr.entries_pageable;
		vm_map_store_init(&head_copy->cpy_hdr);

		entry = vm_map_copy_first_entry(copy);
		if (entry->vme_end < copy->offset + head_size) {
			head_size = entry->vme_end - copy->offset;
		}

		head_copy->offset = copy->offset;
		head_copy->size = head_size;
		copy->offset += head_size;
		copy->size -= head_size;

		vm_map_copy_clip_end(copy, entry, copy->offset);
		vm_map_copy_entry_unlink(copy, entry);
		vm_map_copy_entry_link(head_copy,
				       vm_map_copy_to_entry(head_copy),
				       entry);

		/*
		 * Do the unaligned copy.
		 */
		kr = vm_map_copy_overwrite_nested(dst_map,
						  head_addr,
						  head_copy,
						  interruptible,
						  (pmap_t) NULL,
						  FALSE);
		if (kr != KERN_SUCCESS)
			goto done;
	}

	if (tail_size) {
		/*
		 * Extract "tail_copy" out of "copy".
		 */
		tail_copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
		tail_copy->c_u.hdr.rb_head_store.rbh_root = (void*)(int)SKIP_RB_TREE;
		vm_map_copy_first_entry(tail_copy) =
			vm_map_copy_to_entry(tail_copy);
		vm_map_copy_last_entry(tail_copy) =
			vm_map_copy_to_entry(tail_copy);
		tail_copy->type = VM_MAP_COPY_ENTRY_LIST;
		tail_copy->cpy_hdr.nentries = 0;
		tail_copy->cpy_hdr.entries_pageable =
			copy->cpy_hdr.entries_pageable;
		vm_map_store_init(&tail_copy->cpy_hdr);

		tail_copy->offset = copy->offset + copy->size - tail_size;
		tail_copy->size = tail_size;

		copy->size -= tail_size;

		entry = vm_map_copy_last_entry(copy);
		vm_map_copy_clip_start(copy, entry, tail_copy->offset);
		entry = vm_map_copy_last_entry(copy);
		vm_map_copy_entry_unlink(copy, entry);
		vm_map_copy_entry_link(tail_copy,
				       vm_map_copy_last_entry(tail_copy),
				       entry);
	}

	/*
	 * Copy most (or possibly all) of the data.
	 */
	kr = vm_map_copy_overwrite_nested(dst_map,
					  dst_addr + head_size,
					  copy,
					  interruptible,
					  (pmap_t) NULL,
					  FALSE);
	if (kr != KERN_SUCCESS) {
		goto done;
	}

	if (tail_size) {
		kr = vm_map_copy_overwrite_nested(dst_map,
						  tail_addr,
						  tail_copy,
						  interruptible,
						  (pmap_t) NULL,
						  FALSE);
	}

done:
	assert(copy->type == VM_MAP_COPY_ENTRY_LIST);
	if (kr == KERN_SUCCESS) {
		/*
		 * Discard all the copy maps.
		 */
		if (head_copy) {
			vm_map_copy_discard(head_copy);
			head_copy = NULL;
		}
		vm_map_copy_discard(copy);
		if (tail_copy) {
			vm_map_copy_discard(tail_copy);
			tail_copy = NULL;
		}
	} else {
		/*
		 * Re-assemble the original copy map.
		 */
		if (head_copy) {
			entry = vm_map_copy_first_entry(head_copy);
			vm_map_copy_entry_unlink(head_copy, entry);
			vm_map_copy_entry_link(copy,
					       vm_map_copy_to_entry(copy),
					       entry);
			copy->offset -= head_size;
			copy->size += head_size;
			vm_map_copy_discard(head_copy);
			head_copy = NULL;
		}
		if (tail_copy) {
			entry = vm_map_copy_last_entry(tail_copy);
			vm_map_copy_entry_unlink(tail_copy, entry);
			vm_map_copy_entry_link(copy,
					       vm_map_copy_last_entry(copy),
					       entry);
			copy->size += tail_size;
			vm_map_copy_discard(tail_copy);
			tail_copy = NULL;
		}
	}
	return kr;
}


/*
 *	Routine: vm_map_copy_overwrite_unaligned	[internal use only]
 *
 *	Decription:
 *	Physically copy unaligned data
 *
 *	Implementation:
 *	Unaligned parts of pages have to be physically copied.  We use
 *	a modified form of vm_fault_copy (which understands none-aligned
 *	page offsets and sizes) to do the copy.  We attempt to copy as
 *	much memory in one go as possibly, however vm_fault_copy copies
 *	within 1 memory object so we have to find the smaller of "amount left"
 *	"source object data size" and "target object data size".  With
 *	unaligned data we don't need to split regions, therefore the source
 *	(copy) object should be one map entry, the target range may be split
 *	over multiple map entries however.  In any event we are pessimistic
 *	about these assumptions.
 *
 *	Assumptions:
 *	dst_map is locked on entry and is return locked on success,
 *	unlocked on error.
 */

static kern_return_t
vm_map_copy_overwrite_unaligned(
	vm_map_t	dst_map,
	vm_map_entry_t	entry,
	vm_map_copy_t	copy,
	vm_map_offset_t	start,
	boolean_t	discard_on_success)
{
	vm_map_entry_t		copy_entry;
	vm_map_entry_t		copy_entry_next;
	vm_map_version_t	version;
	vm_object_t		dst_object;
	vm_object_offset_t	dst_offset;
	vm_object_offset_t	src_offset;
	vm_object_offset_t	entry_offset;
	vm_map_offset_t		entry_end;
	vm_map_size_t		src_size,
				dst_size,
				copy_size,
				amount_left;
	kern_return_t		kr = KERN_SUCCESS;


	copy_entry = vm_map_copy_first_entry(copy);

	vm_map_lock_write_to_read(dst_map);

	src_offset = copy->offset - vm_object_trunc_page(copy->offset);
	amount_left = copy->size;
/*
 *	unaligned so we never clipped this entry, we need the offset into
 *	the vm_object not just the data.
 */
	while (amount_left > 0) {

		if (entry == vm_map_to_entry(dst_map)) {
			vm_map_unlock_read(dst_map);
			return KERN_INVALID_ADDRESS;
		}

		/* "start" must be within the current map entry */
		assert ((start>=entry->vme_start) && (start<entry->vme_end));

		dst_offset = start - entry->vme_start;

		dst_size = entry->vme_end - start;

		src_size = copy_entry->vme_end -
			(copy_entry->vme_start + src_offset);

		if (dst_size < src_size) {
/*
 *			we can only copy dst_size bytes before
 *			we have to get the next destination entry
 */
			copy_size = dst_size;
		} else {
/*
 *			we can only copy src_size bytes before
 *			we have to get the next source copy entry
 */
			copy_size = src_size;
		}

		if (copy_size > amount_left) {
			copy_size = amount_left;
		}
/*
 *		Entry needs copy, create a shadow shadow object for
 *		Copy on write region.
 */
		if (entry->needs_copy &&
		    ((entry->protection & VM_PROT_WRITE) != 0))
		{
			if (vm_map_lock_read_to_write(dst_map)) {
				vm_map_lock_read(dst_map);
				goto RetryLookup;
			}
			VME_OBJECT_SHADOW(entry,
					  (vm_map_size_t)(entry->vme_end
							  - entry->vme_start));
			entry->needs_copy = FALSE;
			vm_map_lock_write_to_read(dst_map);
		}
		dst_object = VME_OBJECT(entry);
/*
 *		unlike with the virtual (aligned) copy we're going
 *		to fault on it therefore we need a target object.
 */
                if (dst_object == VM_OBJECT_NULL) {
			if (vm_map_lock_read_to_write(dst_map)) {
				vm_map_lock_read(dst_map);
				goto RetryLookup;
			}
			dst_object = vm_object_allocate((vm_map_size_t)
							entry->vme_end - entry->vme_start);
			VME_OBJECT(entry) = dst_object;
			VME_OFFSET_SET(entry, 0);
			assert(entry->use_pmap);
			vm_map_lock_write_to_read(dst_map);
		}
/*
 *		Take an object reference and unlock map. The "entry" may
 *		disappear or change when the map is unlocked.
 */
		vm_object_reference(dst_object);
		version.main_timestamp = dst_map->timestamp;
		entry_offset = VME_OFFSET(entry);
		entry_end = entry->vme_end;
		vm_map_unlock_read(dst_map);
/*
 *		Copy as much as possible in one pass
 */
		kr = vm_fault_copy(
			VME_OBJECT(copy_entry),
			VME_OFFSET(copy_entry) + src_offset,
			&copy_size,
			dst_object,
			entry_offset + dst_offset,
			dst_map,
			&version,
			THREAD_UNINT );

		start += copy_size;
		src_offset += copy_size;
		amount_left -= copy_size;
/*
 *		Release the object reference
 */
		vm_object_deallocate(dst_object);
/*
 *		If a hard error occurred, return it now
 */
		if (kr != KERN_SUCCESS)
			return kr;

		if ((copy_entry->vme_start + src_offset) == copy_entry->vme_end
		    || amount_left == 0)
		{
/*
 *			all done with this copy entry, dispose.
 */
			copy_entry_next = copy_entry->vme_next;

			if (discard_on_success) {
				vm_map_copy_entry_unlink(copy, copy_entry);
				assert(!copy_entry->is_sub_map);
				vm_object_deallocate(VME_OBJECT(copy_entry));
				vm_map_copy_entry_dispose(copy, copy_entry);
			}

			if (copy_entry_next == vm_map_copy_to_entry(copy) &&
			    amount_left) {
/*
 *				not finished copying but run out of source
 */
				return KERN_INVALID_ADDRESS;
			}

			copy_entry = copy_entry_next;

			src_offset = 0;
		}

		if (amount_left == 0)
			return KERN_SUCCESS;

		vm_map_lock_read(dst_map);
		if (version.main_timestamp == dst_map->timestamp) {
			if (start == entry_end) {
/*
 *				destination region is split.  Use the version
 *				information to avoid a lookup in the normal
 *				case.
 */
				entry = entry->vme_next;
/*
 *				should be contiguous. Fail if we encounter
 *				a hole in the destination.
 */
				if (start != entry->vme_start) {
					vm_map_unlock_read(dst_map);
					return KERN_INVALID_ADDRESS ;
				}
			}
		} else {
/*
 *			Map version check failed.
 *			we must lookup the entry because somebody
 *			might have changed the map behind our backs.
 */
		RetryLookup:
			if (!vm_map_lookup_entry(dst_map, start, &entry))
			{
				vm_map_unlock_read(dst_map);
				return KERN_INVALID_ADDRESS ;
			}
		}
	}/* while */

	return KERN_SUCCESS;
}/* vm_map_copy_overwrite_unaligned */

/*
 *	Routine: vm_map_copy_overwrite_aligned	[internal use only]
 *
 *	Description:
 *	Does all the vm_trickery possible for whole pages.
 *
 *	Implementation:
 *
 *	If there are no permanent objects in the destination,
 *	and the source and destination map entry zones match,
 *	and the destination map entry is not shared,
 *	then the map entries can be deleted and replaced
 *	with those from the copy.  The following code is the
 *	basic idea of what to do, but there are lots of annoying
 *	little details about getting protection and inheritance
 *	right.  Should add protection, inheritance, and sharing checks
 *	to the above pass and make sure that no wiring is involved.
 */

int vm_map_copy_overwrite_aligned_src_not_internal = 0;
int vm_map_copy_overwrite_aligned_src_not_symmetric = 0;
int vm_map_copy_overwrite_aligned_src_large = 0;

static kern_return_t
vm_map_copy_overwrite_aligned(
	vm_map_t	dst_map,
	vm_map_entry_t	tmp_entry,
	vm_map_copy_t	copy,
	vm_map_offset_t	start,
	__unused pmap_t	pmap)
{
	vm_object_t	object;
	vm_map_entry_t	copy_entry;
	vm_map_size_t	copy_size;
	vm_map_size_t	size;
	vm_map_entry_t	entry;

	while ((copy_entry = vm_map_copy_first_entry(copy))
	       != vm_map_copy_to_entry(copy))
	{
		copy_size = (copy_entry->vme_end - copy_entry->vme_start);

		entry = tmp_entry;
		if (entry->is_sub_map) {
			/* unnested when clipped earlier */
			assert(!entry->use_pmap);
		}
		if (entry == vm_map_to_entry(dst_map)) {
			vm_map_unlock(dst_map);
			return KERN_INVALID_ADDRESS;
		}
		size = (entry->vme_end - entry->vme_start);
		/*
		 *	Make sure that no holes popped up in the
		 *	address map, and that the protection is
		 *	still valid, in case the map was unlocked
		 *	earlier.
		 */

		if ((entry->vme_start != start) || ((entry->is_sub_map)
						    && !entry->needs_copy)) {
			vm_map_unlock(dst_map);
			return(KERN_INVALID_ADDRESS);
		}
		assert(entry != vm_map_to_entry(dst_map));

		/*
		 *	Check protection again
		 */

		if ( ! (entry->protection & VM_PROT_WRITE)) {
			vm_map_unlock(dst_map);
			return(KERN_PROTECTION_FAILURE);
		}

		/*
		 *	Adjust to source size first
		 */

		if (copy_size < size) {
			if (entry->map_aligned &&
			    !VM_MAP_PAGE_ALIGNED(entry->vme_start + copy_size,
						 VM_MAP_PAGE_MASK(dst_map))) {
				/* no longer map-aligned */
				entry->map_aligned = FALSE;
			}
			vm_map_clip_end(dst_map, entry, entry->vme_start + copy_size);
			size = copy_size;
		}

		/*
		 *	Adjust to destination size
		 */

		if (size < copy_size) {
			vm_map_copy_clip_end(copy, copy_entry,
					     copy_entry->vme_start + size);
			copy_size = size;
		}

		assert((entry->vme_end - entry->vme_start) == size);
		assert((tmp_entry->vme_end - tmp_entry->vme_start) == size);
		assert((copy_entry->vme_end - copy_entry->vme_start) == size);

		/*
		 *	If the destination contains temporary unshared memory,
		 *	we can perform the copy by throwing it away and
		 *	installing the source data.
		 */

		object = VME_OBJECT(entry);
		if ((!entry->is_shared &&
		     ((object == VM_OBJECT_NULL) ||
		      (object->internal && !object->true_share))) ||
		    entry->needs_copy) {
			vm_object_t	old_object = VME_OBJECT(entry);
			vm_object_offset_t	old_offset = VME_OFFSET(entry);
			vm_object_offset_t	offset;

			/*
			 * Ensure that the source and destination aren't
			 * identical
			 */
			if (old_object == VME_OBJECT(copy_entry) &&
			    old_offset == VME_OFFSET(copy_entry)) {
				vm_map_copy_entry_unlink(copy, copy_entry);
				vm_map_copy_entry_dispose(copy, copy_entry);

				if (old_object != VM_OBJECT_NULL)
					vm_object_deallocate(old_object);

				start = tmp_entry->vme_end;
				tmp_entry = tmp_entry->vme_next;
				continue;
			}

#if !CONFIG_EMBEDDED
#define __TRADEOFF1_OBJ_SIZE (64 * 1024 * 1024)	/* 64 MB */
#define __TRADEOFF1_COPY_SIZE (128 * 1024)	/* 128 KB */
			if (VME_OBJECT(copy_entry) != VM_OBJECT_NULL &&
			    VME_OBJECT(copy_entry)->vo_size >= __TRADEOFF1_OBJ_SIZE &&
			    copy_size <= __TRADEOFF1_COPY_SIZE) {
				/*
				 * Virtual vs. Physical copy tradeoff #1.
				 *
				 * Copying only a few pages out of a large
				 * object:  do a physical copy instead of
				 * a virtual copy, to avoid possibly keeping
				 * the entire large object alive because of
				 * those few copy-on-write pages.
				 */
				vm_map_copy_overwrite_aligned_src_large++;
				goto slow_copy;
			}
#endif /* !CONFIG_EMBEDDED */

			if ((dst_map->pmap != kernel_pmap) &&
			    (VME_ALIAS(entry) >= VM_MEMORY_MALLOC) &&
			    (VME_ALIAS(entry) <= VM_MEMORY_MALLOC_LARGE_REUSED)) {
				vm_object_t new_object, new_shadow;

				/*
				 * We're about to map something over a mapping
				 * established by malloc()...
				 */
				new_object = VME_OBJECT(copy_entry);
				if (new_object != VM_OBJECT_NULL) {
					vm_object_lock_shared(new_object);
				}
				while (new_object != VM_OBJECT_NULL &&
#if !CONFIG_EMBEDDED
				       !new_object->true_share &&
				       new_object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC &&
#endif /* !CONFIG_EMBEDDED */
				       new_object->internal) {
					new_shadow = new_object->shadow;
					if (new_shadow == VM_OBJECT_NULL) {
						break;
					}
					vm_object_lock_shared(new_shadow);
					vm_object_unlock(new_object);
					new_object = new_shadow;
				}
				if (new_object != VM_OBJECT_NULL) {
					if (!new_object->internal) {
						/*
						 * The new mapping is backed
						 * by an external object.  We
						 * don't want malloc'ed memory
						 * to be replaced with such a
						 * non-anonymous mapping, so
						 * let's go off the optimized
						 * path...
						 */
						vm_map_copy_overwrite_aligned_src_not_internal++;
						vm_object_unlock(new_object);
						goto slow_copy;
					}
#if !CONFIG_EMBEDDED
					if (new_object->true_share ||
					    new_object->copy_strategy != MEMORY_OBJECT_COPY_SYMMETRIC) {
						/*
						 * Same if there's a "true_share"
						 * object in the shadow chain, or
						 * an object with a non-default
						 * (SYMMETRIC) copy strategy.
						 */
						vm_map_copy_overwrite_aligned_src_not_symmetric++;
						vm_object_unlock(new_object);
						goto slow_copy;
					}
#endif /* !CONFIG_EMBEDDED */
					vm_object_unlock(new_object);
				}
				/*
				 * The new mapping is still backed by
				 * anonymous (internal) memory, so it's
				 * OK to substitute it for the original
				 * malloc() mapping.
				 */
			}

			if (old_object != VM_OBJECT_NULL) {
				if(entry->is_sub_map) {
					if(entry->use_pmap) {
#ifndef NO_NESTED_PMAP
						pmap_unnest(dst_map->pmap,
							    (addr64_t)entry->vme_start,
							    entry->vme_end - entry->vme_start);
#endif	/* NO_NESTED_PMAP */
						if(dst_map->mapped_in_other_pmaps) {
							/* clean up parent */
							/* map/maps */
							vm_map_submap_pmap_clean(
								dst_map, entry->vme_start,
								entry->vme_end,
								VME_SUBMAP(entry),
								VME_OFFSET(entry));
						}
					} else {
						vm_map_submap_pmap_clean(
							dst_map, entry->vme_start,
							entry->vme_end,
							VME_SUBMAP(entry),
							VME_OFFSET(entry));
					}
				   	vm_map_deallocate(VME_SUBMAP(entry));
			   	} else {
					if(dst_map->mapped_in_other_pmaps) {
						vm_object_pmap_protect_options(
							VME_OBJECT(entry),
							VME_OFFSET(entry),
							entry->vme_end
							- entry->vme_start,
							PMAP_NULL,
							entry->vme_start,
							VM_PROT_NONE,
							PMAP_OPTIONS_REMOVE);
					} else {
						pmap_remove_options(
							dst_map->pmap,
							(addr64_t)(entry->vme_start),
							(addr64_t)(entry->vme_end),
							PMAP_OPTIONS_REMOVE);
					}
					vm_object_deallocate(old_object);
			   	}
			}

			if (entry->iokit_acct) {
				/* keep using iokit accounting */
				entry->use_pmap = FALSE;
			} else {
				/* use pmap accounting */
				entry->use_pmap = TRUE;
			}
			entry->is_sub_map = FALSE;
			VME_OBJECT_SET(entry, VME_OBJECT(copy_entry));
			object = VME_OBJECT(entry);
			entry->needs_copy = copy_entry->needs_copy;
			entry->wired_count = 0;
			entry->user_wired_count = 0;
			offset = VME_OFFSET(copy_entry);
			VME_OFFSET_SET(entry, offset);

			vm_map_copy_entry_unlink(copy, copy_entry);
			vm_map_copy_entry_dispose(copy, copy_entry);

			/*
			 * we could try to push pages into the pmap at this point, BUT
			 * this optimization only saved on average 2 us per page if ALL
			 * the pages in the source were currently mapped
			 * and ALL the pages in the dest were touched, if there were fewer
			 * than 2/3 of the pages touched, this optimization actually cost more cycles
			 * it also puts a lot of pressure on the pmap layer w/r to mapping structures
			 */

			/*
			 *	Set up for the next iteration.  The map
			 *	has not been unlocked, so the next
			 *	address should be at the end of this
			 *	entry, and the next map entry should be
			 *	the one following it.
			 */

			start = tmp_entry->vme_end;
			tmp_entry = tmp_entry->vme_next;
		} else {
			vm_map_version_t	version;
			vm_object_t		dst_object;
			vm_object_offset_t	dst_offset;
			kern_return_t		r;

		slow_copy:
			if (entry->needs_copy) {
				VME_OBJECT_SHADOW(entry,
						  (entry->vme_end -
						   entry->vme_start));
				entry->needs_copy = FALSE;
			}

			dst_object = VME_OBJECT(entry);
			dst_offset = VME_OFFSET(entry);

			/*
			 *	Take an object reference, and record
			 *	the map version information so that the
			 *	map can be safely unlocked.
			 */

			if (dst_object == VM_OBJECT_NULL) {
				/*
				 * We would usually have just taken the
				 * optimized path above if the destination
				 * object has not been allocated yet.  But we
				 * now disable that optimization if the copy
				 * entry's object is not backed by anonymous
				 * memory to avoid replacing malloc'ed
				 * (i.e. re-usable) anonymous memory with a
				 * not-so-anonymous mapping.
				 * So we have to handle this case here and
				 * allocate a new VM object for this map entry.
				 */
				dst_object = vm_object_allocate(
					entry->vme_end - entry->vme_start);
				dst_offset = 0;
				VME_OBJECT_SET(entry, dst_object);
				VME_OFFSET_SET(entry, dst_offset);
				assert(entry->use_pmap);

			}

			vm_object_reference(dst_object);

			/* account for unlock bumping up timestamp */
			version.main_timestamp = dst_map->timestamp + 1;

			vm_map_unlock(dst_map);

			/*
			 *	Copy as much as possible in one pass
			 */

			copy_size = size;
			r = vm_fault_copy(
				VME_OBJECT(copy_entry),
				VME_OFFSET(copy_entry),
				&copy_size,
				dst_object,
				dst_offset,
				dst_map,
				&version,
				THREAD_UNINT );

			/*
			 *	Release the object reference
			 */

			vm_object_deallocate(dst_object);

			/*
			 *	If a hard error occurred, return it now
			 */

			if (r != KERN_SUCCESS)
				return(r);

			if (copy_size != 0) {
				/*
				 *	Dispose of the copied region
				 */

				vm_map_copy_clip_end(copy, copy_entry,
						     copy_entry->vme_start + copy_size);
				vm_map_copy_entry_unlink(copy, copy_entry);
				vm_object_deallocate(VME_OBJECT(copy_entry));
				vm_map_copy_entry_dispose(copy, copy_entry);
			}

			/*
			 *	Pick up in the destination map where we left off.
			 *
			 *	Use the version information to avoid a lookup
			 *	in the normal case.
			 */

			start += copy_size;
			vm_map_lock(dst_map);
			if (version.main_timestamp == dst_map->timestamp &&
			    copy_size != 0) {
				/* We can safely use saved tmp_entry value */

				if (tmp_entry->map_aligned &&
				    !VM_MAP_PAGE_ALIGNED(
					    start,
					    VM_MAP_PAGE_MASK(dst_map))) {
					/* no longer map-aligned */
					tmp_entry->map_aligned = FALSE;
				}
				vm_map_clip_end(dst_map, tmp_entry, start);
				tmp_entry = tmp_entry->vme_next;
			} else {
				/* Must do lookup of tmp_entry */

				if (!vm_map_lookup_entry(dst_map, start, &tmp_entry)) {
					vm_map_unlock(dst_map);
					return(KERN_INVALID_ADDRESS);
				}
				if (tmp_entry->map_aligned &&
				    !VM_MAP_PAGE_ALIGNED(
					    start,
					    VM_MAP_PAGE_MASK(dst_map))) {
					/* no longer map-aligned */
					tmp_entry->map_aligned = FALSE;
				}
				vm_map_clip_start(dst_map, tmp_entry, start);
			}
		}
	}/* while */

	return(KERN_SUCCESS);
}/* vm_map_copy_overwrite_aligned */

/*
 *	Routine: vm_map_copyin_kernel_buffer [internal use only]
 *
 *	Description:
 *		Copy in data to a kernel buffer from space in the
 *		source map. The original space may be optionally
 *		deallocated.
 *
 *		If successful, returns a new copy object.
 */
static kern_return_t
vm_map_copyin_kernel_buffer(
	vm_map_t	src_map,
	vm_map_offset_t	src_addr,
	vm_map_size_t	len,
	boolean_t	src_destroy,
	vm_map_copy_t	*copy_result)
{
	kern_return_t kr;
	vm_map_copy_t copy;
	vm_size_t kalloc_size;

	if (len > msg_ool_size_small)
		return KERN_INVALID_ARGUMENT;

	kalloc_size = (vm_size_t)(cpy_kdata_hdr_sz + len);

	copy = (vm_map_copy_t)kalloc(kalloc_size);
	if (copy == VM_MAP_COPY_NULL)
		return KERN_RESOURCE_SHORTAGE;
	copy->type = VM_MAP_COPY_KERNEL_BUFFER;
	copy->size = len;
	copy->offset = 0;

	kr = copyinmap(src_map, src_addr, copy->cpy_kdata, (vm_size_t)len);
	if (kr != KERN_SUCCESS) {
		kfree(copy, kalloc_size);
		return kr;
	}
	if (src_destroy) {
		(void) vm_map_remove(
			src_map,
			vm_map_trunc_page(src_addr,
					  VM_MAP_PAGE_MASK(src_map)),
			vm_map_round_page(src_addr + len,
					  VM_MAP_PAGE_MASK(src_map)),
			(VM_MAP_REMOVE_INTERRUPTIBLE |
			 VM_MAP_REMOVE_WAIT_FOR_KWIRE |
			 ((src_map == kernel_map) ? VM_MAP_REMOVE_KUNWIRE : 0)));
	}
	*copy_result = copy;
	return KERN_SUCCESS;
}

/*
 *	Routine: vm_map_copyout_kernel_buffer	[internal use only]
 *
 *	Description:
 *		Copy out data from a kernel buffer into space in the
 *		destination map. The space may be otpionally dynamically
 *		allocated.
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 */
static int vm_map_copyout_kernel_buffer_failures = 0;
static kern_return_t
vm_map_copyout_kernel_buffer(
	vm_map_t		map,
	vm_map_address_t	*addr,	/* IN/OUT */
	vm_map_copy_t		copy,
	vm_map_size_t		copy_size,
	boolean_t		overwrite,
	boolean_t		consume_on_success)
{
	kern_return_t kr = KERN_SUCCESS;
	thread_t thread = current_thread();

	assert(copy->size == copy_size);

	/*
	 * check for corrupted vm_map_copy structure
	 */
	if (copy_size > msg_ool_size_small || copy->offset)
		panic("Invalid vm_map_copy_t sz:%lld, ofst:%lld",
		      (long long)copy->size, (long long)copy->offset);

	if (!overwrite) {

		/*
		 * Allocate space in the target map for the data
		 */
		*addr = 0;
		kr = vm_map_enter(map,
				  addr,
				  vm_map_round_page(copy_size,
						    VM_MAP_PAGE_MASK(map)),
				  (vm_map_offset_t) 0,
				  VM_FLAGS_ANYWHERE,
				  VM_MAP_KERNEL_FLAGS_NONE,
				  VM_KERN_MEMORY_NONE,
				  VM_OBJECT_NULL,
				  (vm_object_offset_t) 0,
				  FALSE,
				  VM_PROT_DEFAULT,
				  VM_PROT_ALL,
				  VM_INHERIT_DEFAULT);
		if (kr != KERN_SUCCESS)
			return kr;
#if KASAN
		if (map->pmap == kernel_pmap) {
			kasan_notify_address(*addr, copy->size);
		}
#endif
	}

	/*
	 * Copyout the data from the kernel buffer to the target map.
	 */
	if (thread->map == map) {

		/*
		 * If the target map is the current map, just do
		 * the copy.
		 */
		assert((vm_size_t)copy_size == copy_size);
		if (copyout(copy->cpy_kdata, *addr, (vm_size_t)copy_size)) {
			kr = KERN_INVALID_ADDRESS;
		}
	}
	else {
		vm_map_t oldmap;

		/*
		 * If the target map is another map, assume the
		 * target's address space identity for the duration
		 * of the copy.
		 */
		vm_map_reference(map);
		oldmap = vm_map_switch(map);

		assert((vm_size_t)copy_size == copy_size);
		if (copyout(copy->cpy_kdata, *addr, (vm_size_t)copy_size)) {
			vm_map_copyout_kernel_buffer_failures++;
			kr = KERN_INVALID_ADDRESS;
		}

		(void) vm_map_switch(oldmap);
		vm_map_deallocate(map);
	}

	if (kr != KERN_SUCCESS) {
		/* the copy failed, clean up */
		if (!overwrite) {
			/*
			 * Deallocate the space we allocated in the target map.
			 */
			(void) vm_map_remove(
				map,
				vm_map_trunc_page(*addr,
						  VM_MAP_PAGE_MASK(map)),
				vm_map_round_page((*addr +
						   vm_map_round_page(copy_size,
								     VM_MAP_PAGE_MASK(map))),
						  VM_MAP_PAGE_MASK(map)),
				VM_MAP_NO_FLAGS);
			*addr = 0;
		}
	} else {
		/* copy was successful, dicard the copy structure */
		if (consume_on_success) {
			kfree(copy, copy_size + cpy_kdata_hdr_sz);
		}
	}

	return kr;
}

/*
 *	Macro:		vm_map_copy_insert
 *
 *	Description:
 *		Link a copy chain ("copy") into a map at the
 *		specified location (after "where").
 *	Side effects:
 *		The copy chain is destroyed.
 *	Warning:
 *		The arguments are evaluated multiple times.
 */
#define	vm_map_copy_insert(map, where, copy)				\
MACRO_BEGIN								\
	vm_map_store_copy_insert(map, where, copy);	  \
	zfree(vm_map_copy_zone, copy);		\
MACRO_END

void
vm_map_copy_remap(
	vm_map_t	map,
	vm_map_entry_t	where,
	vm_map_copy_t	copy,
	vm_map_offset_t	adjustment,
	vm_prot_t	cur_prot,
	vm_prot_t	max_prot,
	vm_inherit_t	inheritance)
{
	vm_map_entry_t	copy_entry, new_entry;

	for (copy_entry = vm_map_copy_first_entry(copy);
	     copy_entry != vm_map_copy_to_entry(copy);
	     copy_entry = copy_entry->vme_next) {
		/* get a new VM map entry for the map */
		new_entry = vm_map_entry_create(map,
						!map->hdr.entries_pageable);
		/* copy the "copy entry" to the new entry */
		vm_map_entry_copy(new_entry, copy_entry);
		/* adjust "start" and "end" */
		new_entry->vme_start += adjustment;
		new_entry->vme_end += adjustment;
		/* clear some attributes */
		new_entry->inheritance = inheritance;
		new_entry->protection = cur_prot;
		new_entry->max_protection = max_prot;
		new_entry->behavior = VM_BEHAVIOR_DEFAULT;
		/* take an extra reference on the entry's "object" */
		if (new_entry->is_sub_map) {
			assert(!new_entry->use_pmap); /* not nested */
			vm_map_lock(VME_SUBMAP(new_entry));
			vm_map_reference(VME_SUBMAP(new_entry));
			vm_map_unlock(VME_SUBMAP(new_entry));
		} else {
			vm_object_reference(VME_OBJECT(new_entry));
		}
		/* insert the new entry in the map */
		vm_map_store_entry_link(map, where, new_entry);
		/* continue inserting the "copy entries" after the new entry */
		where = new_entry;
	}
}


/*
 * Returns true if *size matches (or is in the range of) copy->size.
 * Upon returning true, the *size field is updated with the actual size of the
 * copy object (may be different for VM_MAP_COPY_ENTRY_LIST types)
 */
boolean_t
vm_map_copy_validate_size(
	vm_map_t		dst_map,
	vm_map_copy_t		copy,
	vm_map_size_t		*size)
{
	if (copy == VM_MAP_COPY_NULL)
		return FALSE;
	vm_map_size_t copy_sz = copy->size;
	vm_map_size_t sz = *size;
	switch (copy->type) {
	case VM_MAP_COPY_OBJECT:
	case VM_MAP_COPY_KERNEL_BUFFER:
		if (sz == copy_sz)
			return TRUE;
		break;
	case VM_MAP_COPY_ENTRY_LIST:
		/*
		 * potential page-size rounding prevents us from exactly
		 * validating this flavor of vm_map_copy, but we can at least
		 * assert that it's within a range.
		 */
		if (copy_sz >= sz &&
		    copy_sz <= vm_map_round_page(sz, VM_MAP_PAGE_MASK(dst_map))) {
			*size = copy_sz;
			return TRUE;
		}
		break;
	default:
		break;
	}
	return FALSE;
}

/*
 *	Routine:	vm_map_copyout_size
 *
 *	Description:
 *		Copy out a copy chain ("copy") into newly-allocated
 *		space in the destination map. Uses a prevalidated
 *		size for the copy object (vm_map_copy_validate_size).
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 */
kern_return_t
vm_map_copyout_size(
	vm_map_t		dst_map,
	vm_map_address_t	*dst_addr,	/* OUT */
	vm_map_copy_t		copy,
	vm_map_size_t		copy_size)
{
	return vm_map_copyout_internal(dst_map, dst_addr, copy, copy_size,
	                               TRUE, /* consume_on_success */
	                               VM_PROT_DEFAULT,
	                               VM_PROT_ALL,
	                               VM_INHERIT_DEFAULT);
}

/*
 *	Routine:	vm_map_copyout
 *
 *	Description:
 *		Copy out a copy chain ("copy") into newly-allocated
 *		space in the destination map.
 *
 *		If successful, consumes the copy object.
 *		Otherwise, the caller is responsible for it.
 */
kern_return_t
vm_map_copyout(
	vm_map_t		dst_map,
	vm_map_address_t	*dst_addr,	/* OUT */
	vm_map_copy_t		copy)
{
	return vm_map_copyout_internal(dst_map, dst_addr, copy, copy ? copy->size : 0,
	                               TRUE, /* consume_on_success */
	                               VM_PROT_DEFAULT,
	                               VM_PROT_ALL,
	                               VM_INHERIT_DEFAULT);
}

kern_return_t
vm_map_copyout_internal(
	vm_map_t		dst_map,
	vm_map_address_t	*dst_addr,	/* OUT */
	vm_map_copy_t		copy,
	vm_map_size_t		copy_size,
	boolean_t		consume_on_success,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_inherit_t		inheritance)
{
	vm_map_size_t		size;
	vm_map_size_t		adjustment;
	vm_map_offset_t		start;
	vm_object_offset_t	vm_copy_start;
	vm_map_entry_t		last;
	vm_map_entry_t		entry;
	vm_map_entry_t		hole_entry;

	/*
	 *	Check for null copy object.
	 */

	if (copy == VM_MAP_COPY_NULL) {
		*dst_addr = 0;
		return(KERN_SUCCESS);
	}

	if (copy->size != copy_size) {
		*dst_addr = 0;
		return KERN_FAILURE;
	}

	/*
	 *	Check for special copy object, created
	 *	by vm_map_copyin_object.
	 */

	if (copy->type == VM_MAP_COPY_OBJECT) {
		vm_object_t 		object = copy->cpy_object;
		kern_return_t 		kr;
		vm_object_offset_t	offset;

		offset = vm_object_trunc_page(copy->offset);
		size = vm_map_round_page((copy_size +
					  (vm_map_size_t)(copy->offset -
							  offset)),
					 VM_MAP_PAGE_MASK(dst_map));
		*dst_addr = 0;
		kr = vm_map_enter(dst_map, dst_addr, size,
				  (vm_map_offset_t) 0, VM_FLAGS_ANYWHERE,
				  VM_MAP_KERNEL_FLAGS_NONE,
				  VM_KERN_MEMORY_NONE,
				  object, offset, FALSE,
				  VM_PROT_DEFAULT, VM_PROT_ALL,
				  VM_INHERIT_DEFAULT);
		if (kr != KERN_SUCCESS)
			return(kr);
		/* Account for non-pagealigned copy object */
		*dst_addr += (vm_map_offset_t)(copy->offset - offset);
		if (consume_on_success)
			zfree(vm_map_copy_zone, copy);
		return(KERN_SUCCESS);
	}

	/*
	 *	Check for special kernel buffer allocated
	 *	by new_ipc_kmsg_copyin.
	 */

	if (copy->type == VM_MAP_COPY_KERNEL_BUFFER) {
		return vm_map_copyout_kernel_buffer(dst_map, dst_addr,
						    copy, copy_size, FALSE,
						    consume_on_success);
	}


	/*
	 *	Find space for the data
	 */

	vm_copy_start = vm_map_trunc_page((vm_map_size_t)copy->offset,
					  VM_MAP_COPY_PAGE_MASK(copy));
	size = vm_map_round_page((vm_map_size_t)copy->offset + copy_size,
				 VM_MAP_COPY_PAGE_MASK(copy))
		- vm_copy_start;


StartAgain: ;

	vm_map_lock(dst_map);
	if( dst_map->disable_vmentry_reuse == TRUE) {
		VM_MAP_HIGHEST_ENTRY(dst_map, entry, start);
		last = entry;
	} else {
		if (dst_map->holelistenabled) {
			hole_entry = (vm_map_entry_t)dst_map->holes_list;

			if (hole_entry == NULL) {
				/*
				 * No more space in the map?
				 */
				vm_map_unlock(dst_map);
				return(KERN_NO_SPACE);
			}

			last = hole_entry;
			start = last->vme_start;
		} else {
			assert(first_free_is_valid(dst_map));
			start = ((last = dst_map->first_free) == vm_map_to_entry(dst_map)) ?
			vm_map_min(dst_map) : last->vme_end;
		}
		start = vm_map_round_page(start,
					  VM_MAP_PAGE_MASK(dst_map));
	}

	while (TRUE) {
		vm_map_entry_t	next = last->vme_next;
		vm_map_offset_t	end = start + size;

		if ((end > dst_map->max_offset) || (end < start)) {
			if (dst_map->wait_for_space) {
				if (size <= (dst_map->max_offset - dst_map->min_offset)) {
					assert_wait((event_t) dst_map,
						    THREAD_INTERRUPTIBLE);
					vm_map_unlock(dst_map);
					thread_block(THREAD_CONTINUE_NULL);
					goto StartAgain;
				}
			}
			vm_map_unlock(dst_map);
			return(KERN_NO_SPACE);
		}

		if (dst_map->holelistenabled) {
			if (last->vme_end >= end)
				break;
		} else {
			/*
			 *	If there are no more entries, we must win.
			 *
			 *	OR
			 *
			 *	If there is another entry, it must be
			 *	after the end of the potential new region.
			 */

			if (next == vm_map_to_entry(dst_map))
				break;

			if (next->vme_start >= end)
				break;
		}

		last = next;

		if (dst_map->holelistenabled) {
			if (last == (vm_map_entry_t) dst_map->holes_list) {
				/*
				 * Wrapped around
				 */
				vm_map_unlock(dst_map);
				return(KERN_NO_SPACE);
			}
			start = last->vme_start;
		} else {
			start = last->vme_end;
		}
		start = vm_map_round_page(start,
					  VM_MAP_PAGE_MASK(dst_map));
	}

	if (dst_map->holelistenabled) {
		if (vm_map_lookup_entry(dst_map, last->vme_start, &last)) {
			panic("Found an existing entry (%p) instead of potential hole at address: 0x%llx.\n", last, (unsigned long long)last->vme_start);
		}
	}


	adjustment = start - vm_copy_start;
	if (! consume_on_success) {
		/*
		 * We're not allowed to consume "copy", so we'll have to
		 * copy its map entries into the destination map below.
		 * No need to re-allocate map entries from the correct
		 * (pageable or not) zone, since we'll get new map entries
		 * during the transfer.
		 * We'll also adjust the map entries's "start" and "end"
		 * during the transfer, to keep "copy"'s entries consistent
		 * with its "offset".
		 */
		goto after_adjustments;
	}

	/*
	 *	Since we're going to just drop the map
	 *	entries from the copy into the destination
	 *	map, they must come from the same pool.
	 */

	if (copy->cpy_hdr.entries_pageable != dst_map->hdr.entries_pageable) {
		/*
		 * Mismatches occur when dealing with the default
		 * pager.
		 */
		zone_t		old_zone;
		vm_map_entry_t	next, new;

		/*
		 * Find the zone that the copies were allocated from
		 */

		entry = vm_map_copy_first_entry(copy);

		/*
		 * Reinitialize the copy so that vm_map_copy_entry_link
		 * will work.
		 */
		vm_map_store_copy_reset(copy, entry);
		copy->cpy_hdr.entries_pageable = dst_map->hdr.entries_pageable;

		/*
		 * Copy each entry.
		 */
		while (entry != vm_map_copy_to_entry(copy)) {
			new = vm_map_copy_entry_create(copy, !copy->cpy_hdr.entries_pageable);
			vm_map_entry_copy_full(new, entry);
			assert(!new->iokit_acct);
			if (new->is_sub_map) {
				/* clr address space specifics */
				new->use_pmap = FALSE;
			}
			vm_map_copy_entry_link(copy,
					       vm_map_copy_last_entry(copy),
					       new);
			next = entry->vme_next;
			old_zone = entry->from_reserved_zone ? vm_map_entry_reserved_zone : vm_map_entry_zone;
			zfree(old_zone, entry);
			entry = next;
		}
	}

	/*
	 *	Adjust the addresses in the copy chain, and
	 *	reset the region attributes.
	 */

	for (entry = vm_map_copy_first_entry(copy);
	     entry != vm_map_copy_to_entry(copy);
	     entry = entry->vme_next) {
		if (VM_MAP_PAGE_SHIFT(dst_map) == PAGE_SHIFT) {
			/*
			 * We're injecting this copy entry into a map that
			 * has the standard page alignment, so clear
			 * "map_aligned" (which might have been inherited
			 * from the original map entry).
			 */
			entry->map_aligned = FALSE;
		}

		entry->vme_start += adjustment;
		entry->vme_end += adjustment;

		if (entry->map_aligned) {
			assert(VM_MAP_PAGE_ALIGNED(entry->vme_start,
						   VM_MAP_PAGE_MASK(dst_map)));
			assert(VM_MAP_PAGE_ALIGNED(entry->vme_end,
						   VM_MAP_PAGE_MASK(dst_map)));
		}

		entry->inheritance = VM_INHERIT_DEFAULT;
		entry->protection = VM_PROT_DEFAULT;
		entry->max_protection = VM_PROT_ALL;
		entry->behavior = VM_BEHAVIOR_DEFAULT;

		/*
		 * If the entry is now wired,
		 * map the pages into the destination map.
		 */
		if (entry->wired_count != 0) {
			vm_map_offset_t va;
			vm_object_offset_t	 offset;
			vm_object_t object;
			vm_prot_t prot;
			int	type_of_fault;

			object = VME_OBJECT(entry);
			offset = VME_OFFSET(entry);
			va = entry->vme_start;

			pmap_pageable(dst_map->pmap,
				      entry->vme_start,
				      entry->vme_end,
				      TRUE);

			while (va < entry->vme_end) {
				vm_page_t	m;

				/*
				 * Look up the page in the object.
				 * Assert that the page will be found in the
				 * top object:
				 * either
				 *	the object was newly created by
				 *	vm_object_copy_slowly, and has
				 *	copies of all of the pages from
				 *	the source object
				 * or
				 *	the object was moved from the old
				 *	map entry; because the old map
				 *	entry was wired, all of the pages
				 *	were in the top-level object.
				 *	(XXX not true if we wire pages for
				 *	 reading)
				 */
				vm_object_lock(object);

				m = vm_page_lookup(object, offset);
				if (m == VM_PAGE_NULL || !VM_PAGE_WIRED(m) ||
				    m->absent)
					panic("vm_map_copyout: wiring %p", m);

				prot = entry->protection;

				if (override_nx(dst_map, VME_ALIAS(entry)) &&
				    prot)
				        prot |= VM_PROT_EXECUTE;

				type_of_fault = DBG_CACHE_HIT_FAULT;

				vm_fault_enter(m, dst_map->pmap, va, prot, prot,
								VM_PAGE_WIRED(m),
								FALSE, /* change_wiring */
								VM_KERN_MEMORY_NONE, /* tag - not wiring */
								FALSE, /* no_cache */
								FALSE, /* cs_bypass */
								VME_ALIAS(entry),
								((entry->iokit_acct ||
								 (!entry->is_sub_map &&
								  !entry->use_pmap))
								? PMAP_OPTIONS_ALT_ACCT
								: 0),  /* pmap_options */
								NULL,  /* need_retry */
								&type_of_fault);

				vm_object_unlock(object);

				offset += PAGE_SIZE_64;
				va += PAGE_SIZE;
			}
		}
	}

after_adjustments:

	/*
	 *	Correct the page alignment for the result
	 */

	*dst_addr = start + (copy->offset - vm_copy_start);

#if KASAN
	kasan_notify_address(*dst_addr, size);
#endif

	/*
	 *	Update the hints and the map size
	 */

	if (consume_on_success) {
		SAVE_HINT_MAP_WRITE(dst_map, vm_map_copy_last_entry(copy));
	} else {
		SAVE_HINT_MAP_WRITE(dst_map, last);
	}

	dst_map->size += size;

	/*
	 *	Link in the copy
	 */

	if (consume_on_success) {
		vm_map_copy_insert(dst_map, last, copy);
	} else {
		vm_map_copy_remap(dst_map, last, copy, adjustment,
				  cur_protection, max_protection,
				  inheritance);
	}

	vm_map_unlock(dst_map);

	/*
	 * XXX	If wiring_required, call vm_map_pageable
	 */

	return(KERN_SUCCESS);
}

/*
 *	Routine:	vm_map_copyin
 *
 *	Description:
 *		see vm_map_copyin_common.  Exported via Unsupported.exports.
 *
 */

#undef vm_map_copyin

kern_return_t
vm_map_copyin(
	vm_map_t			src_map,
	vm_map_address_t	src_addr,
	vm_map_size_t		len,
	boolean_t			src_destroy,
	vm_map_copy_t		*copy_result)	/* OUT */
{
	return(vm_map_copyin_common(src_map, src_addr, len, src_destroy,
					FALSE, copy_result, FALSE));
}

/*
 *	Routine:	vm_map_copyin_common
 *
 *	Description:
 *		Copy the specified region (src_addr, len) from the
 *		source address space (src_map), possibly removing
 *		the region from the source address space (src_destroy).
 *
 *	Returns:
 *		A vm_map_copy_t object (copy_result), suitable for
 *		insertion into another address space (using vm_map_copyout),
 *		copying over another address space region (using
 *		vm_map_copy_overwrite).  If the copy is unused, it
 *		should be destroyed (using vm_map_copy_discard).
 *
 *	In/out conditions:
 *		The source map should not be locked on entry.
 */

typedef struct submap_map {
	vm_map_t	parent_map;
	vm_map_offset_t	base_start;
	vm_map_offset_t	base_end;
	vm_map_size_t	base_len;
	struct submap_map *next;
} submap_map_t;

kern_return_t
vm_map_copyin_common(
	vm_map_t	src_map,
	vm_map_address_t src_addr,
	vm_map_size_t	len,
	boolean_t	src_destroy,
	__unused boolean_t	src_volatile,
	vm_map_copy_t	*copy_result,	/* OUT */
	boolean_t	use_maxprot)
{
	int flags;

	flags = 0;
	if (src_destroy) {
		flags |= VM_MAP_COPYIN_SRC_DESTROY;
	}
	if (use_maxprot) {
		flags |= VM_MAP_COPYIN_USE_MAXPROT;
	}
	return vm_map_copyin_internal(src_map,
				      src_addr,
				      len,
				      flags,
				      copy_result);
}
kern_return_t
vm_map_copyin_internal(
	vm_map_t	src_map,
	vm_map_address_t src_addr,
	vm_map_size_t	len,
	int		flags,
	vm_map_copy_t	*copy_result)	/* OUT */
{
	vm_map_entry_t	tmp_entry;	/* Result of last map lookup --
					 * in multi-level lookup, this
					 * entry contains the actual
					 * vm_object/offset.
					 */
	vm_map_entry_t	new_entry = VM_MAP_ENTRY_NULL;	/* Map entry for copy */

	vm_map_offset_t	src_start;	/* Start of current entry --
					 * where copy is taking place now
					 */
	vm_map_offset_t	src_end;	/* End of entire region to be
					 * copied */
	vm_map_offset_t src_base;
	vm_map_t	base_map = src_map;
	boolean_t	map_share=FALSE;
	submap_map_t	*parent_maps = NULL;

	vm_map_copy_t	copy;		/* Resulting copy */
	vm_map_address_t copy_addr;
	vm_map_size_t	copy_size;
	boolean_t	src_destroy;
	boolean_t	use_maxprot;
	boolean_t	preserve_purgeable;
	boolean_t	entry_was_shared;
	vm_map_entry_t	saved_src_entry;

	if (flags & ~VM_MAP_COPYIN_ALL_FLAGS) {
		return KERN_INVALID_ARGUMENT;
	}

	src_destroy = (flags & VM_MAP_COPYIN_SRC_DESTROY) ? TRUE : FALSE;
	use_maxprot = (flags & VM_MAP_COPYIN_USE_MAXPROT) ? TRUE : FALSE;
	preserve_purgeable =
		(flags & VM_MAP_COPYIN_PRESERVE_PURGEABLE) ? TRUE : FALSE;

	/*
	 *	Check for copies of zero bytes.
	 */

	if (len == 0) {
		*copy_result = VM_MAP_COPY_NULL;
		return(KERN_SUCCESS);
	}

	/*
	 *	Check that the end address doesn't overflow
	 */
	src_end = src_addr + len;
	if (src_end < src_addr)
		return KERN_INVALID_ADDRESS;

	/*
	 *	Compute (page aligned) start and end of region
	 */
	src_start = vm_map_trunc_page(src_addr,
				      VM_MAP_PAGE_MASK(src_map));
	src_end = vm_map_round_page(src_end,
				    VM_MAP_PAGE_MASK(src_map));

	/*
	 * If the copy is sufficiently small, use a kernel buffer instead
	 * of making a virtual copy.  The theory being that the cost of
	 * setting up VM (and taking C-O-W faults) dominates the copy costs
	 * for small regions.
	 */
	if ((len < msg_ool_size_small) &&
	    !use_maxprot &&
	    !preserve_purgeable &&
	    !(flags & VM_MAP_COPYIN_ENTRY_LIST) &&
	    /*
	     * Since the "msg_ool_size_small" threshold was increased and
	     * vm_map_copyin_kernel_buffer() doesn't handle accesses beyond the
	     * address space limits, we revert to doing a virtual copy if the
	     * copied range goes beyond those limits.  Otherwise, mach_vm_read()
	     * of the commpage would now fail when it used to work.
	     */
	    (src_start >= vm_map_min(src_map) &&
	     src_start < vm_map_max(src_map) &&
	     src_end >= vm_map_min(src_map) &&
	     src_end < vm_map_max(src_map)))
		return vm_map_copyin_kernel_buffer(src_map, src_addr, len,
						   src_destroy, copy_result);

	XPR(XPR_VM_MAP, "vm_map_copyin_common map 0x%x addr 0x%x len 0x%x dest %d\n", src_map, src_addr, len, src_destroy, 0);

	/*
	 *	Allocate a header element for the list.
	 *
	 *	Use the start and end in the header to
	 *	remember the endpoints prior to rounding.
	 */

	copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
	copy->c_u.hdr.rb_head_store.rbh_root = (void*)(int)SKIP_RB_TREE;
	vm_map_copy_first_entry(copy) =
		vm_map_copy_last_entry(copy) = vm_map_copy_to_entry(copy);
	copy->type = VM_MAP_COPY_ENTRY_LIST;
	copy->cpy_hdr.nentries = 0;
	copy->cpy_hdr.entries_pageable = TRUE;
#if 00
	copy->cpy_hdr.page_shift = src_map->hdr.page_shift;
#else
	/*
	 * The copy entries can be broken down for a variety of reasons,
	 * so we can't guarantee that they will remain map-aligned...
	 * Will need to adjust the first copy_entry's "vme_start" and
	 * the last copy_entry's "vme_end" to be rounded to PAGE_MASK
	 * rather than the original map's alignment.
	 */
	copy->cpy_hdr.page_shift = PAGE_SHIFT;
#endif

	vm_map_store_init( &(copy->cpy_hdr) );

	copy->offset = src_addr;
	copy->size = len;

	new_entry = vm_map_copy_entry_create(copy, !copy->cpy_hdr.entries_pageable);

#define	RETURN(x)						\
	MACRO_BEGIN						\
	vm_map_unlock(src_map);					\
	if(src_map != base_map)					\
		vm_map_deallocate(src_map);			\
	if (new_entry != VM_MAP_ENTRY_NULL)			\
		vm_map_copy_entry_dispose(copy,new_entry);	\
	vm_map_copy_discard(copy);				\
	{							\
		submap_map_t	*_ptr;				\
								\
		for(_ptr = parent_maps; _ptr != NULL; _ptr = parent_maps) { \
			parent_maps=parent_maps->next;		\
			if (_ptr->parent_map != base_map)	\
				vm_map_deallocate(_ptr->parent_map);	\
			kfree(_ptr, sizeof(submap_map_t));	\
		}						\
	}							\
	MACRO_RETURN(x);					\
	MACRO_END

	/*
	 *	Find the beginning of the region.
	 */

 	vm_map_lock(src_map);

	/*
	 * Lookup the original "src_addr" rather than the truncated
	 * "src_start", in case "src_start" falls in a non-map-aligned
	 * map entry *before* the map entry that contains "src_addr"...
	 */
	if (!vm_map_lookup_entry(src_map, src_addr, &tmp_entry))
		RETURN(KERN_INVALID_ADDRESS);
	if(!tmp_entry->is_sub_map) {
		/*
		 * ... but clip to the map-rounded "src_start" rather than
		 * "src_addr" to preserve map-alignment.  We'll adjust the
		 * first copy entry at the end, if needed.
		 */
		vm_map_clip_start(src_map, tmp_entry, src_start);
	}
	if (src_start < tmp_entry->vme_start) {
		/*
		 * Move "src_start" up to the start of the
		 * first map entry to copy.
		 */
		src_start = tmp_entry->vme_start;
	}
	/* set for later submap fix-up */
	copy_addr = src_start;

	/*
	 *	Go through entries until we get to the end.
	 */

	while (TRUE) {
		vm_map_entry_t	src_entry = tmp_entry;	/* Top-level entry */
		vm_map_size_t	src_size;		/* Size of source
							 * map entry (in both
							 * maps)
							 */

		vm_object_t		src_object;	/* Object to copy */
		vm_object_offset_t	src_offset;

		boolean_t	src_needs_copy;		/* Should source map
							 * be made read-only
							 * for copy-on-write?
							 */

		boolean_t	new_entry_needs_copy;	/* Will new entry be COW? */

		boolean_t	was_wired;		/* Was source wired? */
		vm_map_version_t version;		/* Version before locks
							 * dropped to make copy
							 */
		kern_return_t	result;			/* Return value from
							 * copy_strategically.
							 */
		while(tmp_entry->is_sub_map) {
			vm_map_size_t submap_len;
			submap_map_t *ptr;

			ptr = (submap_map_t *)kalloc(sizeof(submap_map_t));
			ptr->next = parent_maps;
			parent_maps = ptr;
			ptr->parent_map = src_map;
			ptr->base_start = src_start;
			ptr->base_end = src_end;
			submap_len = tmp_entry->vme_end - src_start;
			if(submap_len > (src_end-src_start))
				submap_len = src_end-src_start;
			ptr->base_len = submap_len;

			src_start -= tmp_entry->vme_start;
			src_start += VME_OFFSET(tmp_entry);
			src_end = src_start + submap_len;
			src_map = VME_SUBMAP(tmp_entry);
			vm_map_lock(src_map);
			/* keep an outstanding reference for all maps in */
			/* the parents tree except the base map */
			vm_map_reference(src_map);
			vm_map_unlock(ptr->parent_map);
			if (!vm_map_lookup_entry(
				    src_map, src_start, &tmp_entry))
				RETURN(KERN_INVALID_ADDRESS);
			map_share = TRUE;
			if(!tmp_entry->is_sub_map)
				vm_map_clip_start(src_map, tmp_entry, src_start);
			src_entry = tmp_entry;
		}
		/* we are now in the lowest level submap... */

		if ((VME_OBJECT(tmp_entry) != VM_OBJECT_NULL) &&
		    (VME_OBJECT(tmp_entry)->phys_contiguous)) {
			/* This is not, supported for now.In future */
			/* we will need to detect the phys_contig   */
			/* condition and then upgrade copy_slowly   */
			/* to do physical copy from the device mem  */
			/* based object. We can piggy-back off of   */
			/* the was wired boolean to set-up the      */
			/* proper handling */
			RETURN(KERN_PROTECTION_FAILURE);
		}
		/*
		 *	Create a new address map entry to hold the result.
		 *	Fill in the fields from the appropriate source entries.
		 *	We must unlock the source map to do this if we need
		 *	to allocate a map entry.
		 */
		if (new_entry == VM_MAP_ENTRY_NULL) {
			version.main_timestamp = src_map->timestamp;
			vm_map_unlock(src_map);

			new_entry = vm_map_copy_entry_create(copy, !copy->cpy_hdr.entries_pageable);

			vm_map_lock(src_map);
			if ((version.main_timestamp + 1) != src_map->timestamp) {
				if (!vm_map_lookup_entry(src_map, src_start,
							 &tmp_entry)) {
					RETURN(KERN_INVALID_ADDRESS);
				}
				if (!tmp_entry->is_sub_map)
					vm_map_clip_start(src_map, tmp_entry, src_start);
				continue; /* restart w/ new tmp_entry */
			}
		}

		/*
		 *	Verify that the region can be read.
		 */
		if (((src_entry->protection & VM_PROT_READ) == VM_PROT_NONE &&
		     !use_maxprot) ||
		    (src_entry->max_protection & VM_PROT_READ) == 0)
			RETURN(KERN_PROTECTION_FAILURE);

		/*
		 *	Clip against the endpoints of the entire region.
		 */

		vm_map_clip_end(src_map, src_entry, src_end);

		src_size = src_entry->vme_end - src_start;
		src_object = VME_OBJECT(src_entry);
		src_offset = VME_OFFSET(src_entry);
		was_wired = (src_entry->wired_count != 0);

		vm_map_entry_copy(new_entry, src_entry);
		if (new_entry->is_sub_map) {
			/* clr address space specifics */
			new_entry->use_pmap = FALSE;
		} else {
			/*
			 * We're dealing with a copy-on-write operation,
			 * so the resulting mapping should not inherit the
			 * original mapping's accounting settings.
			 * "iokit_acct" should have been cleared in
			 * vm_map_entry_copy().
			 * "use_pmap" should be reset to its default (TRUE)
			 * so that the new mapping gets accounted for in
			 * the task's memory footprint.
			 */
			assert(!new_entry->iokit_acct);
			new_entry->use_pmap = TRUE;
		}

		/*
		 *	Attempt non-blocking copy-on-write optimizations.
		 */

		if (src_destroy &&
		    (src_object == VM_OBJECT_NULL ||
		     (src_object->internal &&
		      src_object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC &&
		      !map_share))) {
			/*
			 * If we are destroying the source, and the object
			 * is internal, we can move the object reference
			 * from the source to the copy.  The copy is
			 * copy-on-write only if the source is.
			 * We make another reference to the object, because
			 * destroying the source entry will deallocate it.
			 */
			vm_object_reference(src_object);

			/*
			 * Copy is always unwired.  vm_map_copy_entry
			 * set its wired count to zero.
			 */

			goto CopySuccessful;
		}


	RestartCopy:
		XPR(XPR_VM_MAP, "vm_map_copyin_common src_obj 0x%x ent 0x%x obj 0x%x was_wired %d\n",
		    src_object, new_entry, VME_OBJECT(new_entry),
		    was_wired, 0);
		if ((src_object == VM_OBJECT_NULL ||
		     (!was_wired && !map_share && !tmp_entry->is_shared)) &&
		    vm_object_copy_quickly(
			    &VME_OBJECT(new_entry),
			    src_offset,
			    src_size,
			    &src_needs_copy,
			    &new_entry_needs_copy)) {

			new_entry->needs_copy = new_entry_needs_copy;

			/*
			 *	Handle copy-on-write obligations
			 */

			if (src_needs_copy && !tmp_entry->needs_copy) {
			        vm_prot_t prot;

				prot = src_entry->protection & ~VM_PROT_WRITE;

				if (override_nx(src_map, VME_ALIAS(src_entry))
				    && prot)
				        prot |= VM_PROT_EXECUTE;

				vm_object_pmap_protect(
					src_object,
					src_offset,
					src_size,
			      		(src_entry->is_shared ?
					 PMAP_NULL
					 : src_map->pmap),
					src_entry->vme_start,
					prot);

				assert(tmp_entry->wired_count == 0);
				tmp_entry->needs_copy = TRUE;
			}

			/*
			 *	The map has never been unlocked, so it's safe
			 *	to move to the next entry rather than doing
			 *	another lookup.
			 */

			goto CopySuccessful;
		}

		entry_was_shared = tmp_entry->is_shared;

		/*
		 *	Take an object reference, so that we may
		 *	release the map lock(s).
		 */

		assert(src_object != VM_OBJECT_NULL);
		vm_object_reference(src_object);

		/*
		 *	Record the timestamp for later verification.
		 *	Unlock the map.
		 */

		version.main_timestamp = src_map->timestamp;
		vm_map_unlock(src_map);	/* Increments timestamp once! */
		saved_src_entry = src_entry;
		tmp_entry = VM_MAP_ENTRY_NULL;
		src_entry = VM_MAP_ENTRY_NULL;

		/*
		 *	Perform the copy
		 */

		if (was_wired) {
		CopySlowly:
			vm_object_lock(src_object);
			result = vm_object_copy_slowly(
				src_object,
				src_offset,
				src_size,
				THREAD_UNINT,
				&VME_OBJECT(new_entry));
			VME_OFFSET_SET(new_entry, 0);
			new_entry->needs_copy = FALSE;
		}
		else if (src_object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC &&
			 (entry_was_shared  || map_share)) {
		  	vm_object_t new_object;

			vm_object_lock_shared(src_object);
			new_object = vm_object_copy_delayed(
				src_object,
				src_offset,
				src_size,
				TRUE);
			if (new_object == VM_OBJECT_NULL)
			  	goto CopySlowly;

			VME_OBJECT_SET(new_entry, new_object);
			assert(new_entry->wired_count == 0);
			new_entry->needs_copy = TRUE;
			assert(!new_entry->iokit_acct);
			assert(new_object->purgable == VM_PURGABLE_DENY);
			assertf(new_entry->use_pmap, "src_map %p new_entry %p\n", src_map, new_entry);
			result = KERN_SUCCESS;

		} else {
			vm_object_offset_t new_offset;
			new_offset = VME_OFFSET(new_entry);
			result = vm_object_copy_strategically(src_object,
							      src_offset,
							      src_size,
							      &VME_OBJECT(new_entry),
							      &new_offset,
							      &new_entry_needs_copy);
			if (new_offset != VME_OFFSET(new_entry)) {
				VME_OFFSET_SET(new_entry, new_offset);
			}

			new_entry->needs_copy = new_entry_needs_copy;
		}

		if (result == KERN_SUCCESS &&
		    preserve_purgeable &&
		    src_object->purgable != VM_PURGABLE_DENY) {
			vm_object_t	new_object;

			new_object = VME_OBJECT(new_entry);
			assert(new_object != src_object);
			vm_object_lock(new_object);
			assert(new_object->ref_count == 1);
			assert(new_object->shadow == VM_OBJECT_NULL);
			assert(new_object->copy == VM_OBJECT_NULL);
			assert(new_object->vo_purgeable_owner == NULL);

			new_object->copy_strategy = MEMORY_OBJECT_COPY_NONE;
			new_object->true_share = TRUE;
			/* start as non-volatile with no owner... */
			new_object->purgable = VM_PURGABLE_NONVOLATILE;
			vm_purgeable_nonvolatile_enqueue(new_object, NULL);
			/* ... and move to src_object's purgeable state */
			if (src_object->purgable != VM_PURGABLE_NONVOLATILE) {
				int state;
				state = src_object->purgable;
				vm_object_purgable_control(
					new_object,
					VM_PURGABLE_SET_STATE_FROM_KERNEL,
					&state);
			}
			vm_object_unlock(new_object);
			new_object = VM_OBJECT_NULL;
			/* no pmap accounting for purgeable objects */
			new_entry->use_pmap = FALSE;
		}

		if (result != KERN_SUCCESS &&
		    result != KERN_MEMORY_RESTART_COPY) {
			vm_map_lock(src_map);
			RETURN(result);
		}

		/*
		 *	Throw away the extra reference
		 */

		vm_object_deallocate(src_object);

		/*
		 *	Verify that the map has not substantially
		 *	changed while the copy was being made.
		 */

		vm_map_lock(src_map);

		if ((version.main_timestamp + 1) == src_map->timestamp) {
			/* src_map hasn't changed: src_entry is still valid */
			src_entry = saved_src_entry;
			goto VerificationSuccessful;
		}

		/*
		 *	Simple version comparison failed.
		 *
		 *	Retry the lookup and verify that the
		 *	same object/offset are still present.
		 *
		 *	[Note: a memory manager that colludes with
		 *	the calling task can detect that we have
		 *	cheated.  While the map was unlocked, the
		 *	mapping could have been changed and restored.]
		 */

		if (!vm_map_lookup_entry(src_map, src_start, &tmp_entry)) {
			if (result != KERN_MEMORY_RESTART_COPY) {
				vm_object_deallocate(VME_OBJECT(new_entry));
				VME_OBJECT_SET(new_entry, VM_OBJECT_NULL);
				/* reset accounting state */
				new_entry->iokit_acct = FALSE;
				new_entry->use_pmap = TRUE;
			}
			RETURN(KERN_INVALID_ADDRESS);
		}

		src_entry = tmp_entry;
		vm_map_clip_start(src_map, src_entry, src_start);

		if ((((src_entry->protection & VM_PROT_READ) == VM_PROT_NONE) &&
		     !use_maxprot) ||
		    ((src_entry->max_protection & VM_PROT_READ) == 0))
			goto VerificationFailed;

		if (src_entry->vme_end < new_entry->vme_end) {
			/*
			 * This entry might have been shortened
			 * (vm_map_clip_end) or been replaced with
			 * an entry that ends closer to "src_start"
			 * than before.
			 * Adjust "new_entry" accordingly; copying
			 * less memory would be correct but we also
			 * redo the copy (see below) if the new entry
			 * no longer points at the same object/offset.
			 */
			assert(VM_MAP_PAGE_ALIGNED(src_entry->vme_end,
						   VM_MAP_COPY_PAGE_MASK(copy)));
			new_entry->vme_end = src_entry->vme_end;
			src_size = new_entry->vme_end - src_start;
		} else if (src_entry->vme_end > new_entry->vme_end) {
			/*
			 * This entry might have been extended
			 * (vm_map_entry_simplify() or coalesce)
			 * or been replaced with an entry that ends farther
			 * from "src_start" than before.
			 *
			 * We've called vm_object_copy_*() only on
			 * the previous <start:end> range, so we can't
			 * just extend new_entry.  We have to re-do
			 * the copy based on the new entry as if it was
			 * pointing at a different object/offset (see
			 * "Verification failed" below).
			 */
		}

		if ((VME_OBJECT(src_entry) != src_object) ||
		    (VME_OFFSET(src_entry) != src_offset) ||
		    (src_entry->vme_end > new_entry->vme_end)) {

			/*
			 *	Verification failed.
			 *
			 *	Start over with this top-level entry.
			 */

		VerificationFailed: ;

			vm_object_deallocate(VME_OBJECT(new_entry));
			tmp_entry = src_entry;
			continue;
		}

		/*
		 *	Verification succeeded.
		 */

	VerificationSuccessful: ;

		if (result == KERN_MEMORY_RESTART_COPY)
			goto RestartCopy;

		/*
		 *	Copy succeeded.
		 */

	CopySuccessful: ;

		/*
		 *	Link in the new copy entry.
		 */

		vm_map_copy_entry_link(copy, vm_map_copy_last_entry(copy),
				       new_entry);

		/*
		 *	Determine whether the entire region
		 *	has been copied.
		 */
		src_base = src_start;
		src_start = new_entry->vme_end;
		new_entry = VM_MAP_ENTRY_NULL;
		while ((src_start >= src_end) && (src_end != 0)) {
			submap_map_t	*ptr;

			if (src_map == base_map) {
				/* back to the top */
				break;
			}

			ptr = parent_maps;
			assert(ptr != NULL);
			parent_maps = parent_maps->next;

			/* fix up the damage we did in that submap */
			vm_map_simplify_range(src_map,
					      src_base,
					      src_end);

			vm_map_unlock(src_map);
			vm_map_deallocate(src_map);
			vm_map_lock(ptr->parent_map);
			src_map = ptr->parent_map;
			src_base = ptr->base_start;
			src_start = ptr->base_start + ptr->base_len;
			src_end = ptr->base_end;
			if (!vm_map_lookup_entry(src_map,
						 src_start,
						 &tmp_entry) &&
			    (src_end > src_start)) {
				RETURN(KERN_INVALID_ADDRESS);
			}
			kfree(ptr, sizeof(submap_map_t));
			if (parent_maps == NULL)
				map_share = FALSE;
			src_entry = tmp_entry->vme_prev;
		}

		if ((VM_MAP_PAGE_SHIFT(src_map) != PAGE_SHIFT) &&
		    (src_start >= src_addr + len) &&
		    (src_addr + len != 0)) {
			/*
			 * Stop copying now, even though we haven't reached
			 * "src_end".  We'll adjust the end of the last copy
			 * entry at the end, if needed.
			 *
			 * If src_map's aligment is different from the
			 * system's page-alignment, there could be
			 * extra non-map-aligned map entries between
			 * the original (non-rounded) "src_addr + len"
			 * and the rounded "src_end".
			 * We do not want to copy those map entries since
			 * they're not part of the copied range.
			 */
			break;
		}

		if ((src_start >= src_end) && (src_end != 0))
			break;

		/*
		 *	Verify that there are no gaps in the region
		 */

		tmp_entry = src_entry->vme_next;
		if ((tmp_entry->vme_start != src_start) ||
		    (tmp_entry == vm_map_to_entry(src_map))) {
			RETURN(KERN_INVALID_ADDRESS);
		}
	}

	/*
	 * If the source should be destroyed, do it now, since the
	 * copy was successful.
	 */
	if (src_destroy) {
		(void) vm_map_delete(
			src_map,
			vm_map_trunc_page(src_addr,
					  VM_MAP_PAGE_MASK(src_map)),
			src_end,
			((src_map == kernel_map) ?
			 VM_MAP_REMOVE_KUNWIRE :
			 VM_MAP_NO_FLAGS),
			VM_MAP_NULL);
	} else {
		/* fix up the damage we did in the base map */
		vm_map_simplify_range(
			src_map,
			vm_map_trunc_page(src_addr,
					  VM_MAP_PAGE_MASK(src_map)),
			vm_map_round_page(src_end,
					  VM_MAP_PAGE_MASK(src_map)));
	}

	vm_map_unlock(src_map);
	tmp_entry = VM_MAP_ENTRY_NULL;

	if (VM_MAP_PAGE_SHIFT(src_map) != PAGE_SHIFT) {
		vm_map_offset_t original_start, original_offset, original_end;

		assert(VM_MAP_COPY_PAGE_MASK(copy) == PAGE_MASK);

		/* adjust alignment of first copy_entry's "vme_start" */
		tmp_entry = vm_map_copy_first_entry(copy);
		if (tmp_entry != vm_map_copy_to_entry(copy)) {
			vm_map_offset_t adjustment;

			original_start = tmp_entry->vme_start;
			original_offset = VME_OFFSET(tmp_entry);

			/* map-align the start of the first copy entry... */
			adjustment = (tmp_entry->vme_start -
				      vm_map_trunc_page(
					      tmp_entry->vme_start,
					      VM_MAP_PAGE_MASK(src_map)));
			tmp_entry->vme_start -= adjustment;
			VME_OFFSET_SET(tmp_entry,
				       VME_OFFSET(tmp_entry) - adjustment);
			copy_addr -= adjustment;
			assert(tmp_entry->vme_start < tmp_entry->vme_end);
			/* ... adjust for mis-aligned start of copy range */
			adjustment =
				(vm_map_trunc_page(copy->offset,
						   PAGE_MASK) -
				 vm_map_trunc_page(copy->offset,
						   VM_MAP_PAGE_MASK(src_map)));
			if (adjustment) {
				assert(page_aligned(adjustment));
				assert(adjustment < VM_MAP_PAGE_SIZE(src_map));
				tmp_entry->vme_start += adjustment;
				VME_OFFSET_SET(tmp_entry,
					       (VME_OFFSET(tmp_entry) +
						adjustment));
				copy_addr += adjustment;
				assert(tmp_entry->vme_start < tmp_entry->vme_end);
			}

			/*
			 * Assert that the adjustments haven't exposed
			 * more than was originally copied...
			 */
			assert(tmp_entry->vme_start >= original_start);
			assert(VME_OFFSET(tmp_entry) >= original_offset);
			/*
			 * ... and that it did not adjust outside of a
			 * a single 16K page.
			 */
			assert(vm_map_trunc_page(tmp_entry->vme_start,
						 VM_MAP_PAGE_MASK(src_map)) ==
			       vm_map_trunc_page(original_start,
						 VM_MAP_PAGE_MASK(src_map)));
		}

		/* adjust alignment of last copy_entry's "vme_end" */
		tmp_entry = vm_map_copy_last_entry(copy);
		if (tmp_entry != vm_map_copy_to_entry(copy)) {
			vm_map_offset_t adjustment;

			original_end = tmp_entry->vme_end;

			/* map-align the end of the last copy entry... */
			tmp_entry->vme_end =
				vm_map_round_page(tmp_entry->vme_end,
						  VM_MAP_PAGE_MASK(src_map));
			/* ... adjust for mis-aligned end of copy range */
			adjustment =
				(vm_map_round_page((copy->offset +
						    copy->size),
						   VM_MAP_PAGE_MASK(src_map)) -
				 vm_map_round_page((copy->offset +
						    copy->size),
						   PAGE_MASK));
			if (adjustment) {
				assert(page_aligned(adjustment));
				assert(adjustment < VM_MAP_PAGE_SIZE(src_map));
				tmp_entry->vme_end -= adjustment;
				assert(tmp_entry->vme_start < tmp_entry->vme_end);
			}

			/*
			 * Assert that the adjustments haven't exposed
			 * more than was originally copied...
			 */
			assert(tmp_entry->vme_end <= original_end);
			/*
			 * ... and that it did not adjust outside of a
			 * a single 16K page.
			 */
			assert(vm_map_round_page(tmp_entry->vme_end,
						 VM_MAP_PAGE_MASK(src_map)) ==
			       vm_map_round_page(original_end,
						 VM_MAP_PAGE_MASK(src_map)));
		}
	}

	/* Fix-up start and end points in copy.  This is necessary */
	/* when the various entries in the copy object were picked */
	/* up from different sub-maps */

	tmp_entry = vm_map_copy_first_entry(copy);
	copy_size = 0; /* compute actual size */
	while (tmp_entry != vm_map_copy_to_entry(copy)) {
		assert(VM_MAP_PAGE_ALIGNED(
			       copy_addr + (tmp_entry->vme_end -
					    tmp_entry->vme_start),
			       VM_MAP_COPY_PAGE_MASK(copy)));
		assert(VM_MAP_PAGE_ALIGNED(
			       copy_addr,
			       VM_MAP_COPY_PAGE_MASK(copy)));

		/*
		 * The copy_entries will be injected directly into the
		 * destination map and might not be "map aligned" there...
		 */
		tmp_entry->map_aligned = FALSE;

		tmp_entry->vme_end = copy_addr +
			(tmp_entry->vme_end - tmp_entry->vme_start);
		tmp_entry->vme_start = copy_addr;
		assert(tmp_entry->vme_start < tmp_entry->vme_end);
		copy_addr += tmp_entry->vme_end - tmp_entry->vme_start;
		copy_size += tmp_entry->vme_end - tmp_entry->vme_start;
		tmp_entry = (struct vm_map_entry *)tmp_entry->vme_next;
	}

	if (VM_MAP_PAGE_SHIFT(src_map) != PAGE_SHIFT &&
	    copy_size < copy->size) {
		/*
		 * The actual size of the VM map copy is smaller than what
		 * was requested by the caller.  This must be because some
		 * PAGE_SIZE-sized pages are missing at the end of the last
		 * VM_MAP_PAGE_SIZE(src_map)-sized chunk of the range.
		 * The caller might not have been aware of those missing
		 * pages and might not want to be aware of it, which is
		 * fine as long as they don't try to access (and crash on)
		 * those missing pages.
		 * Let's adjust the size of the "copy", to avoid failing
		 * in vm_map_copyout() or vm_map_copy_overwrite().
		 */
		assert(vm_map_round_page(copy_size,
					 VM_MAP_PAGE_MASK(src_map)) ==
		       vm_map_round_page(copy->size,
					 VM_MAP_PAGE_MASK(src_map)));
		copy->size = copy_size;
	}

	*copy_result = copy;
	return(KERN_SUCCESS);

#undef	RETURN
}

kern_return_t
vm_map_copy_extract(
	vm_map_t		src_map,
	vm_map_address_t	src_addr,
	vm_map_size_t		len,
	vm_map_copy_t		*copy_result,	/* OUT */
	vm_prot_t		*cur_prot,	/* OUT */
	vm_prot_t		*max_prot)
{
	vm_map_offset_t	src_start, src_end;
	vm_map_copy_t	copy;
	kern_return_t	kr;

	/*
	 *	Check for copies of zero bytes.
	 */

	if (len == 0) {
		*copy_result = VM_MAP_COPY_NULL;
		return(KERN_SUCCESS);
	}

	/*
	 *	Check that the end address doesn't overflow
	 */
	src_end = src_addr + len;
	if (src_end < src_addr)
		return KERN_INVALID_ADDRESS;

	/*
	 *	Compute (page aligned) start and end of region
	 */
	src_start = vm_map_trunc_page(src_addr, PAGE_MASK);
	src_end = vm_map_round_page(src_end, PAGE_MASK);

	/*
	 *	Allocate a header element for the list.
	 *
	 *	Use the start and end in the header to
	 *	remember the endpoints prior to rounding.
	 */

	copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
	copy->c_u.hdr.rb_head_store.rbh_root = (void*)(int)SKIP_RB_TREE;
	vm_map_copy_first_entry(copy) =
		vm_map_copy_last_entry(copy) = vm_map_copy_to_entry(copy);
	copy->type = VM_MAP_COPY_ENTRY_LIST;
	copy->cpy_hdr.nentries = 0;
	copy->cpy_hdr.entries_pageable = TRUE;

	vm_map_store_init(&copy->cpy_hdr);

	copy->offset = 0;
	copy->size = len;

	kr = vm_map_remap_extract(src_map,
				  src_addr,
				  len,
				  FALSE, /* copy */
				  &copy->cpy_hdr,
				  cur_prot,
				  max_prot,
				  VM_INHERIT_SHARE,
				  TRUE, /* pageable */
				  FALSE, /* same_map */
				  VM_MAP_KERNEL_FLAGS_NONE);
	if (kr != KERN_SUCCESS) {
		vm_map_copy_discard(copy);
		return kr;
	}

	*copy_result = copy;
	return KERN_SUCCESS;
}

/*
 *	vm_map_copyin_object:
 *
 *	Create a copy object from an object.
 *	Our caller donates an object reference.
 */

kern_return_t
vm_map_copyin_object(
	vm_object_t		object,
	vm_object_offset_t	offset,	/* offset of region in object */
	vm_object_size_t	size,	/* size of region in object */
	vm_map_copy_t	*copy_result)	/* OUT */
{
	vm_map_copy_t	copy;		/* Resulting copy */

	/*
	 *	We drop the object into a special copy object
	 *	that contains the object directly.
	 */

	copy = (vm_map_copy_t) zalloc(vm_map_copy_zone);
	copy->c_u.hdr.rb_head_store.rbh_root = (void*)(int)SKIP_RB_TREE;
	copy->type = VM_MAP_COPY_OBJECT;
	copy->cpy_object = object;
	copy->offset = offset;
	copy->size = size;

	*copy_result = copy;
	return(KERN_SUCCESS);
}

static void
vm_map_fork_share(
	vm_map_t	old_map,
	vm_map_entry_t	old_entry,
	vm_map_t	new_map)
{
	vm_object_t 	object;
	vm_map_entry_t 	new_entry;

	/*
	 *	New sharing code.  New map entry
	 *	references original object.  Internal
	 *	objects use asynchronous copy algorithm for
	 *	future copies.  First make sure we have
	 *	the right object.  If we need a shadow,
	 *	or someone else already has one, then
	 *	make a new shadow and share it.
	 */

	object = VME_OBJECT(old_entry);
	if (old_entry->is_sub_map) {
		assert(old_entry->wired_count == 0);
#ifndef NO_NESTED_PMAP
		if(old_entry->use_pmap) {
			kern_return_t	result;

			result = pmap_nest(new_map->pmap,
					   (VME_SUBMAP(old_entry))->pmap,
					   (addr64_t)old_entry->vme_start,
					   (addr64_t)old_entry->vme_start,
					   (uint64_t)(old_entry->vme_end - old_entry->vme_start));
			if(result)
				panic("vm_map_fork_share: pmap_nest failed!");
		}
#endif	/* NO_NESTED_PMAP */
	} else if (object == VM_OBJECT_NULL) {
		object = vm_object_allocate((vm_map_size_t)(old_entry->vme_end -
							    old_entry->vme_start));
		VME_OFFSET_SET(old_entry, 0);
		VME_OBJECT_SET(old_entry, object);
		old_entry->use_pmap = TRUE;
//		assert(!old_entry->needs_copy);
	} else if (object->copy_strategy !=
		   MEMORY_OBJECT_COPY_SYMMETRIC) {

		/*
		 *	We are already using an asymmetric
		 *	copy, and therefore we already have
		 *	the right object.
		 */

		assert(! old_entry->needs_copy);
	}
	else if (old_entry->needs_copy ||	/* case 1 */
		 object->shadowed ||		/* case 2 */
		 (!object->true_share && 	/* case 3 */
		  !old_entry->is_shared &&
		  (object->vo_size >
		   (vm_map_size_t)(old_entry->vme_end -
				   old_entry->vme_start)))) {

		/*
		 *	We need to create a shadow.
		 *	There are three cases here.
		 *	In the first case, we need to
		 *	complete a deferred symmetrical
		 *	copy that we participated in.
		 *	In the second and third cases,
		 *	we need to create the shadow so
		 *	that changes that we make to the
		 *	object do not interfere with
		 *	any symmetrical copies which
		 *	have occured (case 2) or which
		 *	might occur (case 3).
		 *
		 *	The first case is when we had
		 *	deferred shadow object creation
		 *	via the entry->needs_copy mechanism.
		 *	This mechanism only works when
		 *	only one entry points to the source
		 *	object, and we are about to create
		 *	a second entry pointing to the
		 *	same object. The problem is that
		 *	there is no way of mapping from
		 *	an object to the entries pointing
		 *	to it. (Deferred shadow creation
		 *	works with one entry because occurs
		 *	at fault time, and we walk from the
		 *	entry to the object when handling
		 *	the fault.)
		 *
		 *	The second case is when the object
		 *	to be shared has already been copied
		 *	with a symmetric copy, but we point
		 *	directly to the object without
		 *	needs_copy set in our entry. (This
		 *	can happen because different ranges
		 *	of an object can be pointed to by
		 *	different entries. In particular,
		 *	a single entry pointing to an object
		 *	can be split by a call to vm_inherit,
		 *	which, combined with task_create, can
		 *	result in the different entries
		 *	having different needs_copy values.)
		 *	The shadowed flag in the object allows
		 *	us to detect this case. The problem
		 *	with this case is that if this object
		 *	has or will have shadows, then we
		 *	must not perform an asymmetric copy
		 *	of this object, since such a copy
		 *	allows the object to be changed, which
		 *	will break the previous symmetrical
		 *	copies (which rely upon the object
		 *	not changing). In a sense, the shadowed
		 *	flag says "don't change this object".
		 *	We fix this by creating a shadow
		 *	object for this object, and sharing
		 *	that. This works because we are free
		 *	to change the shadow object (and thus
		 *	to use an asymmetric copy strategy);
		 *	this is also semantically correct,
		 *	since this object is temporary, and
		 *	therefore a copy of the object is
		 *	as good as the object itself. (This
		 *	is not true for permanent objects,
		 *	since the pager needs to see changes,
		 *	which won't happen if the changes
		 *	are made to a copy.)
		 *
		 *	The third case is when the object
		 *	to be shared has parts sticking
		 *	outside of the entry we're working
		 *	with, and thus may in the future
		 *	be subject to a symmetrical copy.
		 *	(This is a preemptive version of
		 *	case 2.)
		 */
		VME_OBJECT_SHADOW(old_entry,
				  (vm_map_size_t) (old_entry->vme_end -
						   old_entry->vme_start));

		/*
		 *	If we're making a shadow for other than
		 *	copy on write reasons, then we have
		 *	to remove write permission.
		 */

		if (!old_entry->needs_copy &&
		    (old_entry->protection & VM_PROT_WRITE)) {
		        vm_prot_t prot;

			assert(!pmap_has_prot_policy(old_entry->protection));

			prot = old_entry->protection & ~VM_PROT_WRITE;

			assert(!pmap_has_prot_policy(prot));

			if (override_nx(old_map, VME_ALIAS(old_entry)) && prot)
			        prot |= VM_PROT_EXECUTE;


			if (old_map->mapped_in_other_pmaps) {
				vm_object_pmap_protect(
					VME_OBJECT(old_entry),
					VME_OFFSET(old_entry),
					(old_entry->vme_end -
					 old_entry->vme_start),
					PMAP_NULL,
					old_entry->vme_start,
					prot);
			} else {
				pmap_protect(old_map->pmap,
					     old_entry->vme_start,
					     old_entry->vme_end,
					     prot);
			}
		}

		old_entry->needs_copy = FALSE;
		object = VME_OBJECT(old_entry);
	}


	/*
	 *	If object was using a symmetric copy strategy,
	 *	change its copy strategy to the default
	 *	asymmetric copy strategy, which is copy_delay
	 *	in the non-norma case and copy_call in the
	 *	norma case. Bump the reference count for the
	 *	new entry.
	 */

	if(old_entry->is_sub_map) {
		vm_map_lock(VME_SUBMAP(old_entry));
		vm_map_reference(VME_SUBMAP(old_entry));
		vm_map_unlock(VME_SUBMAP(old_entry));
	} else {
		vm_object_lock(object);
		vm_object_reference_locked(object);
		if (object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC) {
			object->copy_strategy = MEMORY_OBJECT_COPY_DELAY;
		}
		vm_object_unlock(object);
	}

	/*
	 *	Clone the entry, using object ref from above.
	 *	Mark both entries as shared.
	 */

	new_entry = vm_map_entry_create(new_map, FALSE); /* Never the kernel
							  * map or descendants */
	vm_map_entry_copy(new_entry, old_entry);
	old_entry->is_shared = TRUE;
	new_entry->is_shared = TRUE;

	/*
	 * We're dealing with a shared mapping, so the resulting mapping
	 * should inherit some of the original mapping's accounting settings.
	 * "iokit_acct" should have been cleared in vm_map_entry_copy().
	 * "use_pmap" should stay the same as before (if it hasn't been reset
	 * to TRUE when we cleared "iokit_acct").
	 */
	assert(!new_entry->iokit_acct);

	/*
	 *	If old entry's inheritence is VM_INHERIT_NONE,
	 *	the new entry is for corpse fork, remove the
	 *	write permission from the new entry.
	 */
	if (old_entry->inheritance == VM_INHERIT_NONE) {

		new_entry->protection &= ~VM_PROT_WRITE;
		new_entry->max_protection &= ~VM_PROT_WRITE;
	}

	/*
	 *	Insert the entry into the new map -- we
	 *	know we're inserting at the end of the new
	 *	map.
	 */

	vm_map_store_entry_link(new_map, vm_map_last_entry(new_map), new_entry);

	/*
	 *	Update the physical map
	 */

	if (old_entry->is_sub_map) {
		/* Bill Angell pmap support goes here */
	} else {
		pmap_copy(new_map->pmap, old_map->pmap, new_entry->vme_start,
			  old_entry->vme_end - old_entry->vme_start,
			  old_entry->vme_start);
	}
}

static boolean_t
vm_map_fork_copy(
	vm_map_t	old_map,
	vm_map_entry_t	*old_entry_p,
	vm_map_t	new_map,
	int		vm_map_copyin_flags)
{
	vm_map_entry_t old_entry = *old_entry_p;
	vm_map_size_t entry_size = old_entry->vme_end - old_entry->vme_start;
	vm_map_offset_t start = old_entry->vme_start;
	vm_map_copy_t copy;
	vm_map_entry_t last = vm_map_last_entry(new_map);

	vm_map_unlock(old_map);
	/*
	 *	Use maxprot version of copyin because we
	 *	care about whether this memory can ever
	 *	be accessed, not just whether it's accessible
	 *	right now.
	 */
	vm_map_copyin_flags |= VM_MAP_COPYIN_USE_MAXPROT;
	if (vm_map_copyin_internal(old_map, start, entry_size,
				   vm_map_copyin_flags, &copy)
	    != KERN_SUCCESS) {
		/*
		 *	The map might have changed while it
		 *	was unlocked, check it again.  Skip
		 *	any blank space or permanently
		 *	unreadable region.
		 */
		vm_map_lock(old_map);
		if (!vm_map_lookup_entry(old_map, start, &last) ||
		    (last->max_protection & VM_PROT_READ) == VM_PROT_NONE) {
			last = last->vme_next;
		}
		*old_entry_p = last;

		/*
		 * XXX	For some error returns, want to
		 * XXX	skip to the next element.  Note
		 *	that INVALID_ADDRESS and
		 *	PROTECTION_FAILURE are handled above.
		 */

		return FALSE;
	}

	/*
	 *	Insert the copy into the new map
	 */

	vm_map_copy_insert(new_map, last, copy);

	/*
	 *	Pick up the traversal at the end of
	 *	the copied region.
	 */

	vm_map_lock(old_map);
	start += entry_size;
	if (! vm_map_lookup_entry(old_map, start, &last)) {
		last = last->vme_next;
	} else {
		if (last->vme_start == start) {
			/*
			 * No need to clip here and we don't
			 * want to cause any unnecessary
			 * unnesting...
			 */
		} else {
			vm_map_clip_start(old_map, last, start);
		}
	}
	*old_entry_p = last;

	return TRUE;
}

/*
 *	vm_map_fork:
 *
 *	Create and return a new map based on the old
 *	map, according to the inheritance values on the
 *	regions in that map and the options.
 *
 *	The source map must not be locked.
 */
vm_map_t
vm_map_fork(
	ledger_t	ledger,
	vm_map_t	old_map,
	int		options)
{
	pmap_t		new_pmap;
	vm_map_t	new_map;
	vm_map_entry_t	old_entry;
	vm_map_size_t	new_size = 0, entry_size;
	vm_map_entry_t	new_entry;
	boolean_t	src_needs_copy;
	boolean_t	new_entry_needs_copy;
	boolean_t	pmap_is64bit;
	int		vm_map_copyin_flags;

	if (options & ~(VM_MAP_FORK_SHARE_IF_INHERIT_NONE |
			VM_MAP_FORK_PRESERVE_PURGEABLE)) {
		/* unsupported option */
		return VM_MAP_NULL;
	}

	pmap_is64bit =
#if defined(__i386__) || defined(__x86_64__)
			       old_map->pmap->pm_task_map != TASK_MAP_32BIT;
#elif defined(__arm64__)
			       old_map->pmap->max == MACH_VM_MAX_ADDRESS;
#elif defined(__arm__)
			       FALSE;
#else
#error Unknown architecture.
#endif

	new_pmap = pmap_create(ledger, (vm_map_size_t) 0, pmap_is64bit);

	vm_map_reference_swap(old_map);
	vm_map_lock(old_map);

	new_map = vm_map_create(new_pmap,
				old_map->min_offset,
				old_map->max_offset,
				old_map->hdr.entries_pageable);
	vm_map_lock(new_map);
	vm_commit_pagezero_status(new_map);
	/* inherit the parent map's page size */
	vm_map_set_page_shift(new_map, VM_MAP_PAGE_SHIFT(old_map));
	for (
		old_entry = vm_map_first_entry(old_map);
		old_entry != vm_map_to_entry(old_map);
		) {

		entry_size = old_entry->vme_end - old_entry->vme_start;

		switch (old_entry->inheritance) {
		case VM_INHERIT_NONE:
			/*
			 * Skip making a share entry if VM_MAP_FORK_SHARE_IF_INHERIT_NONE
			 * is not passed or it is backed by a device pager.
			 */
			if ((!(options & VM_MAP_FORK_SHARE_IF_INHERIT_NONE)) ||
				(!old_entry->is_sub_map &&
				VME_OBJECT(old_entry) != NULL &&
				VME_OBJECT(old_entry)->pager != NULL &&
				is_device_pager_ops(VME_OBJECT(old_entry)->pager->mo_pager_ops))) {
				break;
			}
			/* FALLTHROUGH */

		case VM_INHERIT_SHARE:
			vm_map_fork_share(old_map, old_entry, new_map);
			new_size += entry_size;
			break;

		case VM_INHERIT_COPY:

			/*
			 *	Inline the copy_quickly case;
			 *	upon failure, fall back on call
			 *	to vm_map_fork_copy.
			 */

			if(old_entry->is_sub_map)
				break;
			if ((old_entry->wired_count != 0) ||
			    ((VME_OBJECT(old_entry) != NULL) &&
			     (VME_OBJECT(old_entry)->true_share))) {
				goto slow_vm_map_fork_copy;
			}

			new_entry = vm_map_entry_create(new_map, FALSE); /* never the kernel map or descendants */
			vm_map_entry_copy(new_entry, old_entry);
			if (new_entry->is_sub_map) {
				/* clear address space specifics */
				new_entry->use_pmap = FALSE;
			} else {
				/*
				 * We're dealing with a copy-on-write operation,
				 * so the resulting mapping should not inherit
				 * the original mapping's accounting settings.
				 * "iokit_acct" should have been cleared in
				 * vm_map_entry_copy().
				 * "use_pmap" should be reset to its default
				 * (TRUE) so that the new mapping gets
				 * accounted for in the task's memory footprint.
				 */
				assert(!new_entry->iokit_acct);
				new_entry->use_pmap = TRUE;
			}

			if (! vm_object_copy_quickly(
				    &VME_OBJECT(new_entry),
				    VME_OFFSET(old_entry),
				    (old_entry->vme_end -
				     old_entry->vme_start),
				    &src_needs_copy,
				    &new_entry_needs_copy)) {
				vm_map_entry_dispose(new_map, new_entry);
				goto slow_vm_map_fork_copy;
			}

			/*
			 *	Handle copy-on-write obligations
			 */

			if (src_needs_copy && !old_entry->needs_copy) {
			        vm_prot_t prot;

				assert(!pmap_has_prot_policy(old_entry->protection));

				prot = old_entry->protection & ~VM_PROT_WRITE;

				if (override_nx(old_map, VME_ALIAS(old_entry))
				    && prot)
				        prot |= VM_PROT_EXECUTE;

				assert(!pmap_has_prot_policy(prot));

				vm_object_pmap_protect(
					VME_OBJECT(old_entry),
					VME_OFFSET(old_entry),
					(old_entry->vme_end -
					 old_entry->vme_start),
					((old_entry->is_shared
					  || old_map->mapped_in_other_pmaps)
					 ? PMAP_NULL :
					 old_map->pmap),
					old_entry->vme_start,
					prot);

				assert(old_entry->wired_count == 0);
				old_entry->needs_copy = TRUE;
			}
			new_entry->needs_copy = new_entry_needs_copy;

			/*
			 *	Insert the entry at the end
			 *	of the map.
			 */

			vm_map_store_entry_link(new_map, vm_map_last_entry(new_map),
					  new_entry);
			new_size += entry_size;
			break;

		slow_vm_map_fork_copy:
			vm_map_copyin_flags = 0;
			if (options & VM_MAP_FORK_PRESERVE_PURGEABLE) {
				vm_map_copyin_flags |=
					VM_MAP_COPYIN_PRESERVE_PURGEABLE;
			}
			if (vm_map_fork_copy(old_map,
					     &old_entry,
					     new_map,
					     vm_map_copyin_flags)) {
				new_size += entry_size;
			}
			continue;
		}
		old_entry = old_entry->vme_next;
	}

#if defined(__arm64__)
	pmap_insert_sharedpage(new_map->pmap);
#endif

	new_map->size = new_size;
	vm_map_unlock(new_map);
	vm_map_unlock(old_map);
	vm_map_deallocate(old_map);

	return(new_map);
}

/*
 * vm_map_exec:
 *
 * 	Setup the "new_map" with the proper execution environment according
 *	to the type of executable (platform, 64bit, chroot environment).
 *	Map the comm page and shared region, etc...
 */
kern_return_t
vm_map_exec(
	vm_map_t	new_map,
	task_t		task,
	boolean_t	is64bit,
	void		*fsroot,
	cpu_type_t	cpu)
{
	SHARED_REGION_TRACE_DEBUG(
		("shared_region: task %p: vm_map_exec(%p,%p,%p,0x%x): ->\n",
		 (void *)VM_KERNEL_ADDRPERM(current_task()),
		 (void *)VM_KERNEL_ADDRPERM(new_map),
		 (void *)VM_KERNEL_ADDRPERM(task),
		 (void *)VM_KERNEL_ADDRPERM(fsroot),
		 cpu));
	(void) vm_commpage_enter(new_map, task, is64bit);
	(void) vm_shared_region_enter(new_map, task, is64bit, fsroot, cpu);
	SHARED_REGION_TRACE_DEBUG(
		("shared_region: task %p: vm_map_exec(%p,%p,%p,0x%x): <-\n",
		 (void *)VM_KERNEL_ADDRPERM(current_task()),
		 (void *)VM_KERNEL_ADDRPERM(new_map),
		 (void *)VM_KERNEL_ADDRPERM(task),
		 (void *)VM_KERNEL_ADDRPERM(fsroot),
		 cpu));
	return KERN_SUCCESS;
}

/*
 *	vm_map_lookup_locked:
 *
 *	Finds the VM object, offset, and
 *	protection for a given virtual address in the
 *	specified map, assuming a page fault of the
 *	type specified.
 *
 *	Returns the (object, offset, protection) for
 *	this address, whether it is wired down, and whether
 *	this map has the only reference to the data in question.
 *	In order to later verify this lookup, a "version"
 *	is returned.
 *
 *	The map MUST be locked by the caller and WILL be
 *	locked on exit.  In order to guarantee the
 *	existence of the returned object, it is returned
 *	locked.
 *
 *	If a lookup is requested with "write protection"
 *	specified, the map may be changed to perform virtual
 *	copying operations, although the data referenced will
 *	remain the same.
 */
kern_return_t
vm_map_lookup_locked(
	vm_map_t		*var_map,	/* IN/OUT */
	vm_map_offset_t		vaddr,
	vm_prot_t		fault_type,
	int			object_lock_type,
	vm_map_version_t	*out_version,	/* OUT */
	vm_object_t		*object,	/* OUT */
	vm_object_offset_t	*offset,	/* OUT */
	vm_prot_t		*out_prot,	/* OUT */
	boolean_t		*wired,		/* OUT */
	vm_object_fault_info_t	fault_info,	/* OUT */
	vm_map_t		*real_map)
{
	vm_map_entry_t			entry;
	vm_map_t			map = *var_map;
	vm_map_t			old_map = *var_map;
	vm_map_t			cow_sub_map_parent = VM_MAP_NULL;
	vm_map_offset_t			cow_parent_vaddr = 0;
	vm_map_offset_t			old_start = 0;
	vm_map_offset_t			old_end = 0;
	vm_prot_t			prot;
	boolean_t			mask_protections;
	boolean_t			force_copy;
	vm_prot_t			original_fault_type;

	/*
	 * VM_PROT_MASK means that the caller wants us to use "fault_type"
	 * as a mask against the mapping's actual protections, not as an
	 * absolute value.
	 */
	mask_protections = (fault_type & VM_PROT_IS_MASK) ? TRUE : FALSE;
	force_copy = (fault_type & VM_PROT_COPY) ? TRUE : FALSE;
	fault_type &= VM_PROT_ALL;
	original_fault_type = fault_type;

	*real_map = map;

RetryLookup:
	fault_type = original_fault_type;

	/*
	 *	If the map has an interesting hint, try it before calling
	 *	full blown lookup routine.
	 */
	entry = map->hint;

	if ((entry == vm_map_to_entry(map)) ||
	    (vaddr < entry->vme_start) || (vaddr >= entry->vme_end)) {
		vm_map_entry_t	tmp_entry;

		/*
		 *	Entry was either not a valid hint, or the vaddr
		 *	was not contained in the entry, so do a full lookup.
		 */
		if (!vm_map_lookup_entry(map, vaddr, &tmp_entry)) {
			if((cow_sub_map_parent) && (cow_sub_map_parent != map))
				vm_map_unlock(cow_sub_map_parent);
			if((*real_map != map)
			   && (*real_map != cow_sub_map_parent))
				vm_map_unlock(*real_map);
			return KERN_INVALID_ADDRESS;
		}

		entry = tmp_entry;
	}
	if(map == old_map) {
		old_start = entry->vme_start;
		old_end = entry->vme_end;
	}

	/*
	 *	Handle submaps.  Drop lock on upper map, submap is
	 *	returned locked.
	 */

submap_recurse:
	if (entry->is_sub_map) {
		vm_map_offset_t		local_vaddr;
		vm_map_offset_t		end_delta;
		vm_map_offset_t		start_delta;
		vm_map_entry_t		submap_entry;
		vm_prot_t		subentry_protection;
		vm_prot_t		subentry_max_protection;
		boolean_t		mapped_needs_copy=FALSE;

		local_vaddr = vaddr;

		if ((entry->use_pmap &&
		     ! ((fault_type & VM_PROT_WRITE) ||
			force_copy))) {
			/* if real_map equals map we unlock below */
			if ((*real_map != map) &&
			    (*real_map != cow_sub_map_parent))
				vm_map_unlock(*real_map);
			*real_map = VME_SUBMAP(entry);
		}

		if(entry->needs_copy &&
		   ((fault_type & VM_PROT_WRITE) ||
		    force_copy)) {
			if (!mapped_needs_copy) {
				if (vm_map_lock_read_to_write(map)) {
					vm_map_lock_read(map);
					*real_map = map;
					goto RetryLookup;
				}
				vm_map_lock_read(VME_SUBMAP(entry));
				*var_map = VME_SUBMAP(entry);
				cow_sub_map_parent = map;
				/* reset base to map before cow object */
				/* this is the map which will accept   */
				/* the new cow object */
				old_start = entry->vme_start;
				old_end = entry->vme_end;
				cow_parent_vaddr = vaddr;
				mapped_needs_copy = TRUE;
			} else {
				vm_map_lock_read(VME_SUBMAP(entry));
				*var_map = VME_SUBMAP(entry);
				if((cow_sub_map_parent != map) &&
				   (*real_map != map))
					vm_map_unlock(map);
			}
		} else {
			vm_map_lock_read(VME_SUBMAP(entry));
			*var_map = VME_SUBMAP(entry);
			/* leave map locked if it is a target */
			/* cow sub_map above otherwise, just  */
			/* follow the maps down to the object */
			/* here we unlock knowing we are not  */
			/* revisiting the map.  */
			if((*real_map != map) && (map != cow_sub_map_parent))
				vm_map_unlock_read(map);
		}

		map = *var_map;

		/* calculate the offset in the submap for vaddr */
		local_vaddr = (local_vaddr - entry->vme_start) + VME_OFFSET(entry);

	RetrySubMap:
		if(!vm_map_lookup_entry(map, local_vaddr, &submap_entry)) {
			if((cow_sub_map_parent) && (cow_sub_map_parent != map)){
				vm_map_unlock(cow_sub_map_parent);
			}
			if((*real_map != map)
			   && (*real_map != cow_sub_map_parent)) {
				vm_map_unlock(*real_map);
			}
			*real_map = map;
			return KERN_INVALID_ADDRESS;
		}

		/* find the attenuated shadow of the underlying object */
		/* on our target map */

		/* in english the submap object may extend beyond the     */
		/* region mapped by the entry or, may only fill a portion */
		/* of it.  For our purposes, we only care if the object   */
		/* doesn't fill.  In this case the area which will        */
		/* ultimately be clipped in the top map will only need    */
		/* to be as big as the portion of the underlying entry    */
		/* which is mapped */
		start_delta = submap_entry->vme_start > VME_OFFSET(entry) ?
			submap_entry->vme_start - VME_OFFSET(entry) : 0;

		end_delta =
			(VME_OFFSET(entry) + start_delta + (old_end - old_start)) <=
			submap_entry->vme_end ?
			0 : (VME_OFFSET(entry) +
			     (old_end - old_start))
			- submap_entry->vme_end;

		old_start += start_delta;
		old_end -= end_delta;

		if(submap_entry->is_sub_map) {
			entry = submap_entry;
			vaddr = local_vaddr;
			goto submap_recurse;
		}

		if (((fault_type & VM_PROT_WRITE) ||
		     force_copy)
		    && cow_sub_map_parent) {

			vm_object_t	sub_object, copy_object;
			vm_object_offset_t copy_offset;
			vm_map_offset_t	local_start;
			vm_map_offset_t	local_end;
			boolean_t		copied_slowly = FALSE;

			if (vm_map_lock_read_to_write(map)) {
				vm_map_lock_read(map);
				old_start -= start_delta;
				old_end += end_delta;
				goto RetrySubMap;
			}


			sub_object = VME_OBJECT(submap_entry);
			if (sub_object == VM_OBJECT_NULL) {
				sub_object =
					vm_object_allocate(
						(vm_map_size_t)
						(submap_entry->vme_end -
						 submap_entry->vme_start));
				VME_OBJECT_SET(submap_entry, sub_object);
				VME_OFFSET_SET(submap_entry, 0);
				assert(!submap_entry->is_sub_map);
				assert(submap_entry->use_pmap);
			}
			local_start =  local_vaddr -
				(cow_parent_vaddr - old_start);
			local_end = local_vaddr +
				(old_end - cow_parent_vaddr);
			vm_map_clip_start(map, submap_entry, local_start);
			vm_map_clip_end(map, submap_entry, local_end);
			if (submap_entry->is_sub_map) {
				/* unnesting was done when clipping */
				assert(!submap_entry->use_pmap);
			}

			/* This is the COW case, lets connect */
			/* an entry in our space to the underlying */
			/* object in the submap, bypassing the  */
			/* submap. */


			if(submap_entry->wired_count != 0 ||
			   (sub_object->copy_strategy ==
			    MEMORY_OBJECT_COPY_NONE)) {
				vm_object_lock(sub_object);
				vm_object_copy_slowly(sub_object,
						      VME_OFFSET(submap_entry),
						      (submap_entry->vme_end -
						       submap_entry->vme_start),
						      FALSE,
						      &copy_object);
				copied_slowly = TRUE;
			} else {

				/* set up shadow object */
				copy_object = sub_object;
				vm_object_lock(sub_object);
				vm_object_reference_locked(sub_object);
				sub_object->shadowed = TRUE;
				vm_object_unlock(sub_object);

				assert(submap_entry->wired_count == 0);
				submap_entry->needs_copy = TRUE;

				prot = submap_entry->protection;
				assert(!pmap_has_prot_policy(prot));
				prot = prot & ~VM_PROT_WRITE;
				assert(!pmap_has_prot_policy(prot));

				if (override_nx(old_map,
						VME_ALIAS(submap_entry))
				    && prot)
				        prot |= VM_PROT_EXECUTE;

				vm_object_pmap_protect(
					sub_object,
					VME_OFFSET(submap_entry),
					submap_entry->vme_end -
					submap_entry->vme_start,
					(submap_entry->is_shared
					 || map->mapped_in_other_pmaps) ?
					PMAP_NULL : map->pmap,
					submap_entry->vme_start,
					prot);
			}

			/*
			 * Adjust the fault offset to the submap entry.
			 */
			copy_offset = (local_vaddr -
				       submap_entry->vme_start +
				       VME_OFFSET(submap_entry));

			/* This works diffently than the   */
			/* normal submap case. We go back  */
			/* to the parent of the cow map and*/
			/* clip out the target portion of  */
			/* the sub_map, substituting the   */
			/* new copy object,                */

			subentry_protection = submap_entry->protection;
			subentry_max_protection = submap_entry->max_protection;
			vm_map_unlock(map);
			submap_entry = NULL; /* not valid after map unlock */

			local_start = old_start;
			local_end = old_end;
			map = cow_sub_map_parent;
			*var_map = cow_sub_map_parent;
			vaddr = cow_parent_vaddr;
			cow_sub_map_parent = NULL;

			if(!vm_map_lookup_entry(map,
						vaddr, &entry)) {
				vm_object_deallocate(
					copy_object);
				vm_map_lock_write_to_read(map);
				return KERN_INVALID_ADDRESS;
			}

			/* clip out the portion of space */
			/* mapped by the sub map which   */
			/* corresponds to the underlying */
			/* object */

			/*
			 * Clip (and unnest) the smallest nested chunk
			 * possible around the faulting address...
			 */
			local_start = vaddr & ~(pmap_nesting_size_min - 1);
			local_end = local_start + pmap_nesting_size_min;
			/*
			 * ... but don't go beyond the "old_start" to "old_end"
			 * range, to avoid spanning over another VM region
			 * with a possibly different VM object and/or offset.
			 */
			if (local_start < old_start) {
				local_start = old_start;
			}
			if (local_end > old_end) {
				local_end = old_end;
			}
			/*
			 * Adjust copy_offset to the start of the range.
			 */
			copy_offset -= (vaddr - local_start);

			vm_map_clip_start(map, entry, local_start);
			vm_map_clip_end(map, entry, local_end);
			if (entry->is_sub_map) {
				/* unnesting was done when clipping */
				assert(!entry->use_pmap);
			}

			/* substitute copy object for */
			/* shared map entry           */
			vm_map_deallocate(VME_SUBMAP(entry));
			assert(!entry->iokit_acct);
			entry->is_sub_map = FALSE;
			entry->use_pmap = TRUE;
			VME_OBJECT_SET(entry, copy_object);

			/* propagate the submap entry's protections */
			entry->protection |= subentry_protection;
			entry->max_protection |= subentry_max_protection;

#if CONFIG_EMBEDDED
			if (entry->protection & VM_PROT_WRITE) {
				if ((entry->protection & VM_PROT_EXECUTE) && !(entry->used_for_jit)) {
					printf("EMBEDDED: %s can't have both write and exec at the same time\n", __FUNCTION__);
					entry->protection &= ~VM_PROT_EXECUTE;
				}
			}
#endif

			if(copied_slowly) {
				VME_OFFSET_SET(entry, local_start - old_start);
				entry->needs_copy = FALSE;
				entry->is_shared = FALSE;
			} else {
				VME_OFFSET_SET(entry, copy_offset);
				assert(entry->wired_count == 0);
				entry->needs_copy = TRUE;
				if(entry->inheritance == VM_INHERIT_SHARE)
					entry->inheritance = VM_INHERIT_COPY;
				if (map != old_map)
					entry->is_shared = TRUE;
			}
			if(entry->inheritance == VM_INHERIT_SHARE)
				entry->inheritance = VM_INHERIT_COPY;

			vm_map_lock_write_to_read(map);
		} else {
			if((cow_sub_map_parent)
			   && (cow_sub_map_parent != *real_map)
			   && (cow_sub_map_parent != map)) {
				vm_map_unlock(cow_sub_map_parent);
			}
			entry = submap_entry;
			vaddr = local_vaddr;
		}
	}

	/*
	 *	Check whether this task is allowed to have
	 *	this page.
	 */

	prot = entry->protection;

	if (override_nx(old_map, VME_ALIAS(entry)) && prot) {
	        /*
		 * HACK -- if not a stack, then allow execution
		 */
	        prot |= VM_PROT_EXECUTE;
	}

	if (mask_protections) {
		fault_type &= prot;
		if (fault_type == VM_PROT_NONE) {
			goto protection_failure;
		}
	}
	if (((fault_type & prot) != fault_type)
#if __arm64__
	    /* prefetch abort in execute-only page */
	    && !(prot == VM_PROT_EXECUTE && fault_type == (VM_PROT_READ | VM_PROT_EXECUTE))
#endif
	    ) {
	protection_failure:
		if (*real_map != map) {
			vm_map_unlock(*real_map);
		}
		*real_map = map;

		if ((fault_type & VM_PROT_EXECUTE) && prot)
		        log_stack_execution_failure((addr64_t)vaddr, prot);

		DTRACE_VM2(prot_fault, int, 1, (uint64_t *), NULL);
		return KERN_PROTECTION_FAILURE;
	}

	/*
	 *	If this page is not pageable, we have to get
	 *	it for all possible accesses.
	 */

	*wired = (entry->wired_count != 0);
	if (*wired)
	        fault_type = prot;

	/*
	 *	If the entry was copy-on-write, we either ...
	 */

	if (entry->needs_copy) {
	    	/*
		 *	If we want to write the page, we may as well
		 *	handle that now since we've got the map locked.
		 *
		 *	If we don't need to write the page, we just
		 *	demote the permissions allowed.
		 */

		if ((fault_type & VM_PROT_WRITE) || *wired || force_copy) {
			/*
			 *	Make a new object, and place it in the
			 *	object chain.  Note that no new references
			 *	have appeared -- one just moved from the
			 *	map to the new object.
			 */

			if (vm_map_lock_read_to_write(map)) {
				vm_map_lock_read(map);
				goto RetryLookup;
			}

			if (VME_OBJECT(entry)->shadowed == FALSE) {
				vm_object_lock(VME_OBJECT(entry));
				VME_OBJECT(entry)->shadowed = TRUE;
				vm_object_unlock(VME_OBJECT(entry));
			}
			VME_OBJECT_SHADOW(entry,
					  (vm_map_size_t) (entry->vme_end -
							   entry->vme_start));
			entry->needs_copy = FALSE;

			vm_map_lock_write_to_read(map);
		}
		if ((fault_type & VM_PROT_WRITE) == 0 && *wired == 0) {
			/*
			 *	We're attempting to read a copy-on-write
			 *	page -- don't allow writes.
			 */

			prot &= (~VM_PROT_WRITE);
		}
	}

	/*
	 *	Create an object if necessary.
	 */
	if (VME_OBJECT(entry) == VM_OBJECT_NULL) {

		if (vm_map_lock_read_to_write(map)) {
			vm_map_lock_read(map);
			goto RetryLookup;
		}

		VME_OBJECT_SET(entry,
			       vm_object_allocate(
				       (vm_map_size_t)(entry->vme_end -
						       entry->vme_start)));
		VME_OFFSET_SET(entry, 0);
		assert(entry->use_pmap);
		vm_map_lock_write_to_read(map);
	}

	/*
	 *	Return the object/offset from this entry.  If the entry
	 *	was copy-on-write or empty, it has been fixed up.  Also
	 *	return the protection.
	 */

        *offset = (vaddr - entry->vme_start) + VME_OFFSET(entry);
        *object = VME_OBJECT(entry);
	*out_prot = prot;

	if (fault_info) {
		fault_info->interruptible = THREAD_UNINT; /* for now... */
		/* ... the caller will change "interruptible" if needed */
	        fault_info->cluster_size = 0;
		fault_info->user_tag = VME_ALIAS(entry);
		fault_info->pmap_options = 0;
		if (entry->iokit_acct ||
		    (!entry->is_sub_map && !entry->use_pmap)) {
			fault_info->pmap_options |= PMAP_OPTIONS_ALT_ACCT;
		}
	        fault_info->behavior = entry->behavior;
		fault_info->lo_offset = VME_OFFSET(entry);
		fault_info->hi_offset =
			(entry->vme_end - entry->vme_start) + VME_OFFSET(entry);
		fault_info->no_cache  = entry->no_cache;
		fault_info->stealth = FALSE;
		fault_info->io_sync = FALSE;
		if (entry->used_for_jit ||
		    entry->vme_resilient_codesign) {
			fault_info->cs_bypass = TRUE;
		} else {
			fault_info->cs_bypass = FALSE;
		}
		fault_info->mark_zf_absent = FALSE;
		fault_info->batch_pmap_op = FALSE;
	}

	/*
	 *	Lock the object to prevent it from disappearing
	 */
	if (object_lock_type == OBJECT_LOCK_EXCLUSIVE)
	        vm_object_lock(*object);
	else
	        vm_object_lock_shared(*object);

	/*
	 *	Save the version number
	 */

	out_version->main_timestamp = map->timestamp;

	return KERN_SUCCESS;
}


/*
 *	vm_map_verify:
 *
 *	Verifies that the map in question has not changed
 *	since the given version. The map has to be locked
 *	("shared" mode is fine) before calling this function
 *	and it will be returned locked too.
 */
boolean_t
vm_map_verify(
	vm_map_t		map,
	vm_map_version_t	*version)	/* REF */
{
	boolean_t	result;

	vm_map_lock_assert_held(map);
	result = (map->timestamp == version->main_timestamp);

	return(result);
}

/*
 *	TEMPORARYTEMPORARYTEMPORARYTEMPORARYTEMPORARYTEMPORARY
 *	Goes away after regular vm_region_recurse function migrates to
 *	64 bits
 *	vm_region_recurse: A form of vm_region which follows the
 *	submaps in a target map
 *
 */

kern_return_t
vm_map_region_recurse_64(
	vm_map_t		 map,
	vm_map_offset_t	*address,		/* IN/OUT */
	vm_map_size_t		*size,			/* OUT */
	natural_t	 	*nesting_depth,	/* IN/OUT */
	vm_region_submap_info_64_t	submap_info,	/* IN/OUT */
	mach_msg_type_number_t	*count)	/* IN/OUT */
{
	mach_msg_type_number_t	original_count;
	vm_region_extended_info_data_t	extended;
	vm_map_entry_t			tmp_entry;
	vm_map_offset_t			user_address;
	unsigned int			user_max_depth;

	/*
	 * "curr_entry" is the VM map entry preceding or including the
	 * address we're looking for.
	 * "curr_map" is the map or sub-map containing "curr_entry".
	 * "curr_address" is the equivalent of the top map's "user_address"
	 * in the current map.
	 * "curr_offset" is the cumulated offset of "curr_map" in the
	 * target task's address space.
	 * "curr_depth" is the depth of "curr_map" in the chain of
	 * sub-maps.
	 *
	 * "curr_max_below" and "curr_max_above" limit the range (around
	 * "curr_address") we should take into account in the current (sub)map.
	 * They limit the range to what's visible through the map entries
	 * we've traversed from the top map to the current map.

	 */
	vm_map_entry_t			curr_entry;
	vm_map_address_t		curr_address;
	vm_map_offset_t			curr_offset;
	vm_map_t			curr_map;
	unsigned int			curr_depth;
	vm_map_offset_t			curr_max_below, curr_max_above;
	vm_map_offset_t			curr_skip;

	/*
	 * "next_" is the same as "curr_" but for the VM region immediately
	 * after the address we're looking for.  We need to keep track of this
	 * too because we want to return info about that region if the
	 * address we're looking for is not mapped.
	 */
	vm_map_entry_t			next_entry;
	vm_map_offset_t			next_offset;
	vm_map_offset_t			next_address;
	vm_map_t			next_map;
	unsigned int			next_depth;
	vm_map_offset_t			next_max_below, next_max_above;
	vm_map_offset_t			next_skip;

	boolean_t			look_for_pages;
	vm_region_submap_short_info_64_t short_info;
	boolean_t			do_region_footprint;

	if (map == VM_MAP_NULL) {
		/* no address space to work on */
		return KERN_INVALID_ARGUMENT;
	}


	if (*count < VM_REGION_SUBMAP_SHORT_INFO_COUNT_64) {
		/*
		 * "info" structure is not big enough and
		 * would overflow
		 */
		return KERN_INVALID_ARGUMENT;
	}

	do_region_footprint = task_self_region_footprint();
	original_count = *count;

	if (original_count < VM_REGION_SUBMAP_INFO_V0_COUNT_64) {
		*count = VM_REGION_SUBMAP_SHORT_INFO_COUNT_64;
		look_for_pages = FALSE;
		short_info = (vm_region_submap_short_info_64_t) submap_info;
		submap_info = NULL;
	} else {
		look_for_pages = TRUE;
		*count = VM_REGION_SUBMAP_INFO_V0_COUNT_64;
		short_info = NULL;

		if (original_count >= VM_REGION_SUBMAP_INFO_V1_COUNT_64) {
			*count = VM_REGION_SUBMAP_INFO_V1_COUNT_64;
		}
	}

	user_address = *address;
	user_max_depth = *nesting_depth;

	if (not_in_kdp) {
		vm_map_lock_read(map);
	}

recurse_again:
	curr_entry = NULL;
	curr_map = map;
	curr_address = user_address;
	curr_offset = 0;
	curr_skip = 0;
	curr_depth = 0;
	curr_max_above = ((vm_map_offset_t) -1) - curr_address;
	curr_max_below = curr_address;

	next_entry = NULL;
	next_map = NULL;
	next_address = 0;
	next_offset = 0;
	next_skip = 0;
	next_depth = 0;
	next_max_above = (vm_map_offset_t) -1;
	next_max_below = (vm_map_offset_t) -1;

	for (;;) {
		if (vm_map_lookup_entry(curr_map,
					curr_address,
					&tmp_entry)) {
			/* tmp_entry contains the address we're looking for */
			curr_entry = tmp_entry;
		} else {
			vm_map_offset_t skip;
			/*
			 * The address is not mapped.  "tmp_entry" is the
			 * map entry preceding the address.  We want the next
			 * one, if it exists.
			 */
			curr_entry = tmp_entry->vme_next;

			if (curr_entry == vm_map_to_entry(curr_map) ||
			    (curr_entry->vme_start >=
			     curr_address + curr_max_above)) {
				/* no next entry at this level: stop looking */
				if (not_in_kdp) {
					vm_map_unlock_read(curr_map);
				}
				curr_entry = NULL;
				curr_map = NULL;
				curr_skip = 0;
				curr_offset = 0;
				curr_depth = 0;
				curr_max_above = 0;
				curr_max_below = 0;
				break;
			}

			/* adjust current address and offset */
			skip = curr_entry->vme_start - curr_address;
			curr_address = curr_entry->vme_start;
			curr_skip += skip;
			curr_offset += skip;
			curr_max_above -= skip;
			curr_max_below = 0;
		}

		/*
		 * Is the next entry at this level closer to the address (or
		 * deeper in the submap chain) than the one we had
		 * so far ?
		 */
		tmp_entry = curr_entry->vme_next;
		if (tmp_entry == vm_map_to_entry(curr_map)) {
			/* no next entry at this level */
		} else if (tmp_entry->vme_start >=
			   curr_address + curr_max_above) {
			/*
			 * tmp_entry is beyond the scope of what we mapped of
			 * this submap in the upper level: ignore it.
			 */
		} else if ((next_entry == NULL) ||
			   (tmp_entry->vme_start + curr_offset <=
			    next_entry->vme_start + next_offset)) {
			/*
			 * We didn't have a "next_entry" or this one is
			 * closer to the address we're looking for:
			 * use this "tmp_entry" as the new "next_entry".
			 */
			if (next_entry != NULL) {
				/* unlock the last "next_map" */
				if (next_map != curr_map && not_in_kdp) {
					vm_map_unlock_read(next_map);
				}
			}
			next_entry = tmp_entry;
			next_map = curr_map;
			next_depth = curr_depth;
			next_address = next_entry->vme_start;
			next_skip = curr_skip;
			next_skip += (next_address - curr_address);
			next_offset = curr_offset;
			next_offset += (next_address - curr_address);
			next_max_above = MIN(next_max_above, curr_max_above);
			next_max_above = MIN(next_max_above,
					     next_entry->vme_end - next_address);
			next_max_below = MIN(next_max_below, curr_max_below);
			next_max_below = MIN(next_max_below,
					     next_address - next_entry->vme_start);
		}

		/*
		 * "curr_max_{above,below}" allow us to keep track of the
		 * portion of the submap that is actually mapped at this level:
		 * the rest of that submap is irrelevant to us, since it's not
		 * mapped here.
		 * The relevant portion of the map starts at
		 * "VME_OFFSET(curr_entry)" up to the size of "curr_entry".
		 */
		curr_max_above = MIN(curr_max_above,
				     curr_entry->vme_end - curr_address);
		curr_max_below = MIN(curr_max_below,
				     curr_address - curr_entry->vme_start);

		if (!curr_entry->is_sub_map ||
		    curr_depth >= user_max_depth) {
			/*
			 * We hit a leaf map or we reached the maximum depth
			 * we could, so stop looking.  Keep the current map
			 * locked.
			 */
			break;
		}

		/*
		 * Get down to the next submap level.
		 */

		/*
		 * Lock the next level and unlock the current level,
		 * unless we need to keep it locked to access the "next_entry"
		 * later.
		 */
		if (not_in_kdp) {
			vm_map_lock_read(VME_SUBMAP(curr_entry));
		}
		if (curr_map == next_map) {
			/* keep "next_map" locked in case we need it */
		} else {
			/* release this map */
			if (not_in_kdp)
				vm_map_unlock_read(curr_map);
		}

		/*
		 * Adjust the offset.  "curr_entry" maps the submap
		 * at relative address "curr_entry->vme_start" in the
		 * curr_map but skips the first "VME_OFFSET(curr_entry)"
		 * bytes of the submap.
		 * "curr_offset" always represents the offset of a virtual
		 * address in the curr_map relative to the absolute address
		 * space (i.e. the top-level VM map).
		 */
		curr_offset +=
			(VME_OFFSET(curr_entry) - curr_entry->vme_start);
		curr_address = user_address + curr_offset;
		/* switch to the submap */
		curr_map = VME_SUBMAP(curr_entry);
		curr_depth++;
		curr_entry = NULL;
	}

// LP64todo: all the current tools are 32bit, obviously never worked for 64b
// so probably should be a real 32b ID vs. ptr.
// Current users just check for equality

	if (curr_entry == NULL) {
		/* no VM region contains the address... */

		if (do_region_footprint && /* we want footprint numbers */
		    next_entry == NULL && /* & there are no more regions */
		    /* & we haven't already provided our fake region: */
		    user_address <= vm_map_last_entry(map)->vme_end) {
			ledger_amount_t nonvol, nonvol_compressed;
			/*
			 * Add a fake memory region to account for
			 * purgeable memory that counts towards this
			 * task's memory footprint, i.e. the resident
			 * compressed pages of non-volatile objects
			 * owned by that task.
			 */
			ledger_get_balance(
				map->pmap->ledger,
				task_ledgers.purgeable_nonvolatile,
				&nonvol);
			ledger_get_balance(
				map->pmap->ledger,
				task_ledgers.purgeable_nonvolatile_compressed,
				&nonvol_compressed);
			if (nonvol + nonvol_compressed == 0) {
				/* no purgeable memory usage to report */
				return KERN_INVALID_ADDRESS;
			}
			/* fake region to show nonvolatile footprint */
			if (look_for_pages) {
				submap_info->protection = VM_PROT_DEFAULT;
				submap_info->max_protection = VM_PROT_DEFAULT;
				submap_info->inheritance = VM_INHERIT_DEFAULT;
				submap_info->offset = 0;
				submap_info->user_tag = -1;
				submap_info->pages_resident = (unsigned int) (nonvol / PAGE_SIZE);
				submap_info->pages_shared_now_private = 0;
				submap_info->pages_swapped_out = (unsigned int) (nonvol_compressed / PAGE_SIZE);
				submap_info->pages_dirtied = submap_info->pages_resident;
				submap_info->ref_count = 1;
				submap_info->shadow_depth = 0;
				submap_info->external_pager = 0;
				submap_info->share_mode = SM_PRIVATE;
				submap_info->is_submap = 0;
				submap_info->behavior = VM_BEHAVIOR_DEFAULT;
				submap_info->object_id = INFO_MAKE_FAKE_OBJECT_ID(map, task_ledgers.purgeable_nonvolatile);
				submap_info->user_wired_count = 0;
				submap_info->pages_reusable = 0;
			} else {
				short_info->user_tag = -1;
				short_info->offset = 0;
				short_info->protection = VM_PROT_DEFAULT;
				short_info->inheritance = VM_INHERIT_DEFAULT;
				short_info->max_protection = VM_PROT_DEFAULT;
				short_info->behavior = VM_BEHAVIOR_DEFAULT;
				short_info->user_wired_count = 0;
				short_info->is_submap = 0;
				short_info->object_id = INFO_MAKE_FAKE_OBJECT_ID(map, task_ledgers.purgeable_nonvolatile);
				short_info->external_pager = 0;
				short_info->shadow_depth = 0;
				short_info->share_mode = SM_PRIVATE;
				short_info->ref_count = 1;
			}
			*nesting_depth = 0;
			*size = (vm_map_size_t) (nonvol + nonvol_compressed);
//			*address = user_address;
			*address = vm_map_last_entry(map)->vme_end;
			return KERN_SUCCESS;
		}

		if (next_entry == NULL) {
			/* ... and no VM region follows it either */
			return KERN_INVALID_ADDRESS;
		}
		/* ... gather info about the next VM region */
		curr_entry = next_entry;
		curr_map = next_map;	/* still locked ... */
		curr_address = next_address;
		curr_skip = next_skip;
		curr_offset = next_offset;
		curr_depth = next_depth;
		curr_max_above = next_max_above;
		curr_max_below = next_max_below;
	} else {
		/* we won't need "next_entry" after all */
		if (next_entry != NULL) {
			/* release "next_map" */
			if (next_map != curr_map && not_in_kdp) {
				vm_map_unlock_read(next_map);
			}
		}
	}
	next_entry = NULL;
	next_map = NULL;
	next_offset = 0;
	next_skip = 0;
	next_depth = 0;
	next_max_below = -1;
	next_max_above = -1;

	if (curr_entry->is_sub_map &&
	    curr_depth < user_max_depth) {
		/*
		 * We're not as deep as we could be:  we must have
		 * gone back up after not finding anything mapped
		 * below the original top-level map entry's.
		 * Let's move "curr_address" forward and recurse again.
		 */
		user_address = curr_address;
		goto recurse_again;
	}

	*nesting_depth = curr_depth;
	*size = curr_max_above + curr_max_below;
	*address = user_address + curr_skip - curr_max_below;

// LP64todo: all the current tools are 32bit, obviously never worked for 64b
// so probably should be a real 32b ID vs. ptr.
// Current users just check for equality
#define INFO_MAKE_OBJECT_ID(p)	((uint32_t)(uintptr_t)VM_KERNEL_ADDRPERM(p))

	if (look_for_pages) {
		submap_info->user_tag = VME_ALIAS(curr_entry);
		submap_info->offset = VME_OFFSET(curr_entry);
		submap_info->protection = curr_entry->protection;
		submap_info->inheritance = curr_entry->inheritance;
		submap_info->max_protection = curr_entry->max_protection;
		submap_info->behavior = curr_entry->behavior;
		submap_info->user_wired_count = curr_entry->user_wired_count;
		submap_info->is_submap = curr_entry->is_sub_map;
		submap_info->object_id = INFO_MAKE_OBJECT_ID(VME_OBJECT(curr_entry));
	} else {
		short_info->user_tag = VME_ALIAS(curr_entry);
		short_info->offset = VME_OFFSET(curr_entry);
		short_info->protection = curr_entry->protection;
		short_info->inheritance = curr_entry->inheritance;
		short_info->max_protection = curr_entry->max_protection;
		short_info->behavior = curr_entry->behavior;
		short_info->user_wired_count = curr_entry->user_wired_count;
		short_info->is_submap = curr_entry->is_sub_map;
		short_info->object_id = INFO_MAKE_OBJECT_ID(VME_OBJECT(curr_entry));
	}

	extended.pages_resident = 0;
	extended.pages_swapped_out = 0;
	extended.pages_shared_now_private = 0;
	extended.pages_dirtied = 0;
	extended.pages_reusable = 0;
	extended.external_pager = 0;
	extended.shadow_depth = 0;
	extended.share_mode = SM_EMPTY;
	extended.ref_count = 0;

	if (not_in_kdp) {
		if (!curr_entry->is_sub_map) {
			vm_map_offset_t range_start, range_end;
			range_start = MAX((curr_address - curr_max_below),
					  curr_entry->vme_start);
			range_end = MIN((curr_address + curr_max_above),
					curr_entry->vme_end);
			vm_map_region_walk(curr_map,
					   range_start,
					   curr_entry,
					   (VME_OFFSET(curr_entry) +
					    (range_start -
					     curr_entry->vme_start)),
					   range_end - range_start,
					   &extended,
					   look_for_pages, VM_REGION_EXTENDED_INFO_COUNT);
			if (extended.external_pager &&
			    extended.ref_count == 2 &&
			    extended.share_mode == SM_SHARED) {
				extended.share_mode = SM_PRIVATE;
			}
		} else {
			if (curr_entry->use_pmap) {
				extended.share_mode = SM_TRUESHARED;
			} else {
				extended.share_mode = SM_PRIVATE;
			}
			extended.ref_count = VME_SUBMAP(curr_entry)->ref_count;
		}
	}

	if (look_for_pages) {
		submap_info->pages_resident = extended.pages_resident;
		submap_info->pages_swapped_out = extended.pages_swapped_out;
		submap_info->pages_shared_now_private =
			extended.pages_shared_now_private;
		submap_info->pages_dirtied = extended.pages_dirtied;
		submap_info->external_pager = extended.external_pager;
		submap_info->shadow_depth = extended.shadow_depth;
		submap_info->share_mode = extended.share_mode;
		submap_info->ref_count = extended.ref_count;

		if (original_count >= VM_REGION_SUBMAP_INFO_V1_COUNT_64) {
			submap_info->pages_reusable = extended.pages_reusable;
		}
	} else {
		short_info->external_pager = extended.external_pager;
		short_info->shadow_depth = extended.shadow_depth;
		short_info->share_mode = extended.share_mode;
		short_info->ref_count = extended.ref_count;
	}

	if (not_in_kdp) {
		vm_map_unlock_read(curr_map);
	}

	return KERN_SUCCESS;
}

/*
 *	vm_region:
 *
 *	User call to obtain information about a region in
 *	a task's address map. Currently, only one flavor is
 *	supported.
 *
 *	XXX The reserved and behavior fields cannot be filled
 *	    in until the vm merge from the IK is completed, and
 *	    vm_reserve is implemented.
 */

kern_return_t
vm_map_region(
	vm_map_t		 map,
	vm_map_offset_t	*address,		/* IN/OUT */
	vm_map_size_t		*size,			/* OUT */
	vm_region_flavor_t	 flavor,		/* IN */
	vm_region_info_t	 info,			/* OUT */
	mach_msg_type_number_t	*count,	/* IN/OUT */
	mach_port_t		*object_name)		/* OUT */
{
	vm_map_entry_t		tmp_entry;
	vm_map_entry_t		entry;
	vm_map_offset_t		start;

	if (map == VM_MAP_NULL)
		return(KERN_INVALID_ARGUMENT);

	switch (flavor) {

	case VM_REGION_BASIC_INFO:
		/* legacy for old 32-bit objects info */
	{
		vm_region_basic_info_t	basic;

		if (*count < VM_REGION_BASIC_INFO_COUNT)
			return(KERN_INVALID_ARGUMENT);

		basic = (vm_region_basic_info_t) info;
		*count = VM_REGION_BASIC_INFO_COUNT;

		vm_map_lock_read(map);

		start = *address;
		if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
			if ((entry = tmp_entry->vme_next) == vm_map_to_entry(map)) {
				vm_map_unlock_read(map);
				return(KERN_INVALID_ADDRESS);
			}
		} else {
			entry = tmp_entry;
		}

		start = entry->vme_start;

		basic->offset = (uint32_t)VME_OFFSET(entry);
		basic->protection = entry->protection;
		basic->inheritance = entry->inheritance;
		basic->max_protection = entry->max_protection;
		basic->behavior = entry->behavior;
		basic->user_wired_count = entry->user_wired_count;
		basic->reserved = entry->is_sub_map;
		*address = start;
		*size = (entry->vme_end - start);

		if (object_name) *object_name = IP_NULL;
		if (entry->is_sub_map) {
			basic->shared = FALSE;
		} else {
			basic->shared = entry->is_shared;
		}

		vm_map_unlock_read(map);
		return(KERN_SUCCESS);
	}

	case VM_REGION_BASIC_INFO_64:
	{
		vm_region_basic_info_64_t	basic;

		if (*count < VM_REGION_BASIC_INFO_COUNT_64)
			return(KERN_INVALID_ARGUMENT);

		basic = (vm_region_basic_info_64_t) info;
		*count = VM_REGION_BASIC_INFO_COUNT_64;

		vm_map_lock_read(map);

		start = *address;
		if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
			if ((entry = tmp_entry->vme_next) == vm_map_to_entry(map)) {
				vm_map_unlock_read(map);
				return(KERN_INVALID_ADDRESS);
			}
		} else {
			entry = tmp_entry;
		}

		start = entry->vme_start;

		basic->offset = VME_OFFSET(entry);
		basic->protection = entry->protection;
		basic->inheritance = entry->inheritance;
		basic->max_protection = entry->max_protection;
		basic->behavior = entry->behavior;
		basic->user_wired_count = entry->user_wired_count;
		basic->reserved = entry->is_sub_map;
		*address = start;
		*size = (entry->vme_end - start);

		if (object_name) *object_name = IP_NULL;
		if (entry->is_sub_map) {
			basic->shared = FALSE;
		} else {
			basic->shared = entry->is_shared;
		}

		vm_map_unlock_read(map);
		return(KERN_SUCCESS);
	}
	case VM_REGION_EXTENDED_INFO:
		if (*count < VM_REGION_EXTENDED_INFO_COUNT)
			return(KERN_INVALID_ARGUMENT);
		/*fallthru*/
	case VM_REGION_EXTENDED_INFO__legacy:
		if (*count < VM_REGION_EXTENDED_INFO_COUNT__legacy)
			return KERN_INVALID_ARGUMENT;

	{
		vm_region_extended_info_t	extended;
		mach_msg_type_number_t original_count;

		extended = (vm_region_extended_info_t) info;

		vm_map_lock_read(map);

		start = *address;
		if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
			if ((entry = tmp_entry->vme_next) == vm_map_to_entry(map)) {
				vm_map_unlock_read(map);
				return(KERN_INVALID_ADDRESS);
			}
		} else {
			entry = tmp_entry;
		}
		start = entry->vme_start;

		extended->protection = entry->protection;
		extended->user_tag = VME_ALIAS(entry);
		extended->pages_resident = 0;
		extended->pages_swapped_out = 0;
		extended->pages_shared_now_private = 0;
		extended->pages_dirtied = 0;
		extended->external_pager = 0;
		extended->shadow_depth = 0;

		original_count = *count;
		if (flavor == VM_REGION_EXTENDED_INFO__legacy) {
			*count = VM_REGION_EXTENDED_INFO_COUNT__legacy;
		} else {
			extended->pages_reusable = 0;
			*count = VM_REGION_EXTENDED_INFO_COUNT;
		}

		vm_map_region_walk(map, start, entry, VME_OFFSET(entry), entry->vme_end - start, extended, TRUE, *count);

		if (extended->external_pager && extended->ref_count == 2 && extended->share_mode == SM_SHARED)
			extended->share_mode = SM_PRIVATE;

		if (object_name)
			*object_name = IP_NULL;
		*address = start;
		*size = (entry->vme_end - start);

		vm_map_unlock_read(map);
		return(KERN_SUCCESS);
	}
	case VM_REGION_TOP_INFO:
	{
		vm_region_top_info_t	top;

		if (*count < VM_REGION_TOP_INFO_COUNT)
			return(KERN_INVALID_ARGUMENT);

		top = (vm_region_top_info_t) info;
		*count = VM_REGION_TOP_INFO_COUNT;

		vm_map_lock_read(map);

		start = *address;
		if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
			if ((entry = tmp_entry->vme_next) == vm_map_to_entry(map)) {
				vm_map_unlock_read(map);
				return(KERN_INVALID_ADDRESS);
			}
		} else {
			entry = tmp_entry;

		}
		start = entry->vme_start;

		top->private_pages_resident = 0;
		top->shared_pages_resident = 0;

		vm_map_region_top_walk(entry, top);

		if (object_name)
			*object_name = IP_NULL;
		*address = start;
		*size = (entry->vme_end - start);

		vm_map_unlock_read(map);
		return(KERN_SUCCESS);
	}
	default:
		return(KERN_INVALID_ARGUMENT);
	}
}

#define OBJ_RESIDENT_COUNT(obj, entry_size)				\
	MIN((entry_size),						\
	    ((obj)->all_reusable ?					\
	     (obj)->wired_page_count :					\
	     (obj)->resident_page_count - (obj)->reusable_page_count))

void
vm_map_region_top_walk(
        vm_map_entry_t		   entry,
	vm_region_top_info_t       top)
{

	if (VME_OBJECT(entry) == 0 || entry->is_sub_map) {
		top->share_mode = SM_EMPTY;
		top->ref_count = 0;
		top->obj_id = 0;
		return;
	}

	{
	        struct	vm_object *obj, *tmp_obj;
		int		ref_count;
		uint32_t	entry_size;

		entry_size = (uint32_t) ((entry->vme_end - entry->vme_start) / PAGE_SIZE_64);

		obj = VME_OBJECT(entry);

		vm_object_lock(obj);

		if ((ref_count = obj->ref_count) > 1 && obj->paging_in_progress)
			ref_count--;

		assert(obj->reusable_page_count <= obj->resident_page_count);
		if (obj->shadow) {
			if (ref_count == 1)
				top->private_pages_resident =
					OBJ_RESIDENT_COUNT(obj, entry_size);
			else
				top->shared_pages_resident =
					OBJ_RESIDENT_COUNT(obj, entry_size);
			top->ref_count  = ref_count;
			top->share_mode = SM_COW;

			while ((tmp_obj = obj->shadow)) {
				vm_object_lock(tmp_obj);
				vm_object_unlock(obj);
				obj = tmp_obj;

				if ((ref_count = obj->ref_count) > 1 && obj->paging_in_progress)
					ref_count--;

				assert(obj->reusable_page_count <= obj->resident_page_count);
				top->shared_pages_resident +=
					OBJ_RESIDENT_COUNT(obj, entry_size);
				top->ref_count += ref_count - 1;
			}
		} else {
			if (entry->superpage_size) {
				top->share_mode = SM_LARGE_PAGE;
				top->shared_pages_resident = 0;
				top->private_pages_resident = entry_size;
			} else if (entry->needs_copy) {
				top->share_mode = SM_COW;
				top->shared_pages_resident =
					OBJ_RESIDENT_COUNT(obj, entry_size);
			} else {
				if (ref_count == 1 ||
				    (ref_count == 2 && !(obj->pager_trusted) && !(obj->internal))) {
					top->share_mode = SM_PRIVATE;
						top->private_pages_resident =
							OBJ_RESIDENT_COUNT(obj,
									   entry_size);
				} else {
					top->share_mode = SM_SHARED;
					top->shared_pages_resident =
						OBJ_RESIDENT_COUNT(obj,
								  entry_size);
				}
			}
			top->ref_count = ref_count;
		}
		/* XXX K64: obj_id will be truncated */
		top->obj_id = (unsigned int) (uintptr_t)VM_KERNEL_ADDRPERM(obj);

		vm_object_unlock(obj);
	}
}

void
vm_map_region_walk(
	vm_map_t		   	map,
	vm_map_offset_t			va,
	vm_map_entry_t			entry,
	vm_object_offset_t		offset,
	vm_object_size_t		range,
	vm_region_extended_info_t	extended,
	boolean_t			look_for_pages,
	mach_msg_type_number_t count)
{
        struct vm_object *obj, *tmp_obj;
	vm_map_offset_t       last_offset;
	int               i;
	int               ref_count;
	struct vm_object	*shadow_object;
	int			shadow_depth;
	boolean_t	  do_region_footprint;

	do_region_footprint = task_self_region_footprint();

	if ((VME_OBJECT(entry) == 0) ||
	    (entry->is_sub_map) ||
	    (VME_OBJECT(entry)->phys_contiguous &&
	     !entry->superpage_size)) {
		extended->share_mode = SM_EMPTY;
		extended->ref_count = 0;
		return;
	}

	if (entry->superpage_size) {
		extended->shadow_depth = 0;
		extended->share_mode = SM_LARGE_PAGE;
		extended->ref_count = 1;
		extended->external_pager = 0;
		extended->pages_resident = (unsigned int)(range >> PAGE_SHIFT);
		extended->shadow_depth = 0;
		return;
	}

	obj = VME_OBJECT(entry);

	vm_object_lock(obj);

	if ((ref_count = obj->ref_count) > 1 && obj->paging_in_progress)
		ref_count--;

	if (look_for_pages) {
		for (last_offset = offset + range;
		     offset < last_offset;
		     offset += PAGE_SIZE_64, va += PAGE_SIZE) {

			if (do_region_footprint) {
				int disp;

				disp = 0;
				pmap_query_page_info(map->pmap, va, &disp);
				if (disp & PMAP_QUERY_PAGE_PRESENT) {
					extended->pages_resident++;
					if (disp & PMAP_QUERY_PAGE_REUSABLE) {
						extended->pages_reusable++;
					} else if (!(disp & PMAP_QUERY_PAGE_INTERNAL) ||
						   (disp & PMAP_QUERY_PAGE_ALTACCT)) {
						/* alternate accounting */
					} else {
						extended->pages_dirtied++;
					}
				} else if (disp & PMAP_QUERY_PAGE_COMPRESSED) {
					if (disp & PMAP_QUERY_PAGE_COMPRESSED_ALTACCT) {
						/* alternate accounting */
					} else {
						extended->pages_swapped_out++;
					}
				}
				/* deal with alternate accounting */
				if (obj->purgable != VM_PURGABLE_DENY) {
					/*
					 * Pages from purgeable objects
					 * will be reported as dirty 
					 * appropriately in an extra
					 * fake memory region at the end of
					 * the address space.
					 */
				} else if (entry->iokit_acct) {
					/*
					 * IOKit mappings are considered
					 * as fully dirty for footprint's
					 * sake.
					 */
					extended->pages_dirtied++;
				}
				continue;
			}

			vm_map_region_look_for_page(map, va, obj,
						    offset, ref_count,
						    0, extended, count);
		}

		if (do_region_footprint) {
			goto collect_object_info;
		}

	} else {
	collect_object_info:
		shadow_object = obj->shadow;
		shadow_depth = 0;

		if ( !(obj->pager_trusted) && !(obj->internal))
			extended->external_pager = 1;

		if (shadow_object != VM_OBJECT_NULL) {
			vm_object_lock(shadow_object);
			for (;
			     shadow_object != VM_OBJECT_NULL;
			     shadow_depth++) {
				vm_object_t	next_shadow;

				if ( !(shadow_object->pager_trusted) &&
				     !(shadow_object->internal))
					extended->external_pager = 1;

				next_shadow = shadow_object->shadow;
				if (next_shadow) {
					vm_object_lock(next_shadow);
				}
				vm_object_unlock(shadow_object);
				shadow_object = next_shadow;
			}
		}
		extended->shadow_depth = shadow_depth;
	}

	if (extended->shadow_depth || entry->needs_copy)
		extended->share_mode = SM_COW;
	else {
		if (ref_count == 1)
			extended->share_mode = SM_PRIVATE;
		else {
			if (obj->true_share)
				extended->share_mode = SM_TRUESHARED;
			else
				extended->share_mode = SM_SHARED;
		}
	}
	extended->ref_count = ref_count - extended->shadow_depth;

	for (i = 0; i < extended->shadow_depth; i++) {
		if ((tmp_obj = obj->shadow) == 0)
			break;
		vm_object_lock(tmp_obj);
		vm_object_unlock(obj);

		if ((ref_count = tmp_obj->ref_count) > 1 && tmp_obj->paging_in_progress)
			ref_count--;

		extended->ref_count += ref_count;
		obj = tmp_obj;
	}
	vm_object_unlock(obj);

	if (extended->share_mode == SM_SHARED) {
		vm_map_entry_t	     cur;
		vm_map_entry_t	     last;
		int      my_refs;

		obj = VME_OBJECT(entry);
		last = vm_map_to_entry(map);
		my_refs = 0;

		if ((ref_count = obj->ref_count) > 1 && obj->paging_in_progress)
			ref_count--;
		for (cur = vm_map_first_entry(map); cur != last; cur = cur->vme_next)
			my_refs += vm_map_region_count_obj_refs(cur, obj);

		if (my_refs == ref_count)
			extended->share_mode = SM_PRIVATE_ALIASED;
		else if (my_refs > 1)
			extended->share_mode = SM_SHARED_ALIASED;
	}
}


/* object is locked on entry and locked on return */


static void
vm_map_region_look_for_page(
	__unused vm_map_t		map,
	__unused vm_map_offset_t	va,
	vm_object_t			object,
	vm_object_offset_t		offset,
	int				max_refcnt,
	int				depth,
	vm_region_extended_info_t	extended,
	mach_msg_type_number_t count)
{
        vm_page_t	p;
        vm_object_t	shadow;
	int		ref_count;
	vm_object_t	caller_object;

	shadow = object->shadow;
	caller_object = object;


	while (TRUE) {

		if ( !(object->pager_trusted) && !(object->internal))
			extended->external_pager = 1;

		if ((p = vm_page_lookup(object, offset)) != VM_PAGE_NULL) {
	        	if (shadow && (max_refcnt == 1))
		    		extended->pages_shared_now_private++;

			if (!p->fictitious &&
			    (p->dirty || pmap_is_modified(VM_PAGE_GET_PHYS_PAGE(p))))
		    		extended->pages_dirtied++;
			else if (count >= VM_REGION_EXTENDED_INFO_COUNT) {
				if (p->reusable || object->all_reusable) {
					extended->pages_reusable++;
				}
			}

			extended->pages_resident++;

			if(object != caller_object)
				vm_object_unlock(object);

			return;
		}
		if (object->internal &&
		    object->alive &&
		    !object->terminating &&
		    object->pager_ready) {

			if (VM_COMPRESSOR_PAGER_STATE_GET(object, offset)
			    == VM_EXTERNAL_STATE_EXISTS) {
				/* the pager has that page */
				extended->pages_swapped_out++;
				if (object != caller_object)
					vm_object_unlock(object);
				return;
			}
		}

		if (shadow) {
			vm_object_lock(shadow);

			if ((ref_count = shadow->ref_count) > 1 && shadow->paging_in_progress)
			        ref_count--;

	    		if (++depth > extended->shadow_depth)
	        		extended->shadow_depth = depth;

	    		if (ref_count > max_refcnt)
	        		max_refcnt = ref_count;

			if(object != caller_object)
				vm_object_unlock(object);

			offset = offset + object->vo_shadow_offset;
			object = shadow;
			shadow = object->shadow;
			continue;
		}
		if(object != caller_object)
			vm_object_unlock(object);
		break;
	}
}

static int
vm_map_region_count_obj_refs(
        vm_map_entry_t    entry,
	vm_object_t       object)
{
        int ref_count;
	vm_object_t chk_obj;
	vm_object_t tmp_obj;

	if (VME_OBJECT(entry) == 0)
		return(0);

        if (entry->is_sub_map)
		return(0);
	else {
		ref_count = 0;

		chk_obj = VME_OBJECT(entry);
		vm_object_lock(chk_obj);

		while (chk_obj) {
			if (chk_obj == object)
				ref_count++;
			tmp_obj = chk_obj->shadow;
			if (tmp_obj)
				vm_object_lock(tmp_obj);
			vm_object_unlock(chk_obj);

			chk_obj = tmp_obj;
		}
	}
	return(ref_count);
}


/*
 *	Routine:	vm_map_simplify
 *
 *	Description:
 *		Attempt to simplify the map representation in
 *		the vicinity of the given starting address.
 *	Note:
 *		This routine is intended primarily to keep the
 *		kernel maps more compact -- they generally don't
 *		benefit from the "expand a map entry" technology
 *		at allocation time because the adjacent entry
 *		is often wired down.
 */
void
vm_map_simplify_entry(
	vm_map_t	map,
	vm_map_entry_t	this_entry)
{
	vm_map_entry_t	prev_entry;

	counter(c_vm_map_simplify_entry_called++);

	prev_entry = this_entry->vme_prev;

	if ((this_entry != vm_map_to_entry(map)) &&
	    (prev_entry != vm_map_to_entry(map)) &&

	    (prev_entry->vme_end == this_entry->vme_start) &&

	    (prev_entry->is_sub_map == this_entry->is_sub_map) &&
	    (VME_OBJECT(prev_entry) == VME_OBJECT(this_entry)) &&
	    ((VME_OFFSET(prev_entry) + (prev_entry->vme_end -
				    prev_entry->vme_start))
	     == VME_OFFSET(this_entry)) &&

	    (prev_entry->behavior == this_entry->behavior) &&
	    (prev_entry->needs_copy == this_entry->needs_copy) &&
	    (prev_entry->protection == this_entry->protection) &&
	    (prev_entry->max_protection == this_entry->max_protection) &&
	    (prev_entry->inheritance == this_entry->inheritance) &&
	    (prev_entry->use_pmap == this_entry->use_pmap) &&
	    (VME_ALIAS(prev_entry) == VME_ALIAS(this_entry)) &&
	    (prev_entry->no_cache == this_entry->no_cache) &&
	    (prev_entry->permanent == this_entry->permanent) &&
	    (prev_entry->map_aligned == this_entry->map_aligned) &&
	    (prev_entry->zero_wired_pages == this_entry->zero_wired_pages) &&
	    (prev_entry->used_for_jit == this_entry->used_for_jit) &&
	    /* from_reserved_zone: OK if that field doesn't match */
	    (prev_entry->iokit_acct == this_entry->iokit_acct) &&
	    (prev_entry->vme_resilient_codesign ==
	     this_entry->vme_resilient_codesign) &&
	    (prev_entry->vme_resilient_media ==
	     this_entry->vme_resilient_media) &&

	    (prev_entry->wired_count == this_entry->wired_count) &&
	    (prev_entry->user_wired_count == this_entry->user_wired_count) &&

	    ((prev_entry->vme_atomic == FALSE) && (this_entry->vme_atomic == FALSE)) &&
	    (prev_entry->in_transition == FALSE) &&
	    (this_entry->in_transition == FALSE) &&
	    (prev_entry->needs_wakeup == FALSE) &&
	    (this_entry->needs_wakeup == FALSE) &&
	    (prev_entry->is_shared == FALSE) &&
	    (this_entry->is_shared == FALSE) &&
	    (prev_entry->superpage_size == FALSE) &&
	    (this_entry->superpage_size == FALSE)
		) {
		vm_map_store_entry_unlink(map, prev_entry);
		assert(prev_entry->vme_start < this_entry->vme_end);
		if (prev_entry->map_aligned)
			assert(VM_MAP_PAGE_ALIGNED(prev_entry->vme_start,
						   VM_MAP_PAGE_MASK(map)));
		this_entry->vme_start = prev_entry->vme_start;
		VME_OFFSET_SET(this_entry, VME_OFFSET(prev_entry));

		if (map->holelistenabled) {
			vm_map_store_update_first_free(map, this_entry, TRUE);
		}

		if (prev_entry->is_sub_map) {
			vm_map_deallocate(VME_SUBMAP(prev_entry));
		} else {
			vm_object_deallocate(VME_OBJECT(prev_entry));
		}
		vm_map_entry_dispose(map, prev_entry);
		SAVE_HINT_MAP_WRITE(map, this_entry);
		counter(c_vm_map_simplified++);
	}
}

void
vm_map_simplify(
	vm_map_t	map,
	vm_map_offset_t	start)
{
	vm_map_entry_t	this_entry;

	vm_map_lock(map);
	if (vm_map_lookup_entry(map, start, &this_entry)) {
		vm_map_simplify_entry(map, this_entry);
		vm_map_simplify_entry(map, this_entry->vme_next);
	}
	counter(c_vm_map_simplify_called++);
	vm_map_unlock(map);
}

static void
vm_map_simplify_range(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	vm_map_entry_t	entry;

	/*
	 * The map should be locked (for "write") by the caller.
	 */

	if (start >= end) {
		/* invalid address range */
		return;
	}

	start = vm_map_trunc_page(start,
				  VM_MAP_PAGE_MASK(map));
	end = vm_map_round_page(end,
				VM_MAP_PAGE_MASK(map));

	if (!vm_map_lookup_entry(map, start, &entry)) {
		/* "start" is not mapped and "entry" ends before "start" */
		if (entry == vm_map_to_entry(map)) {
			/* start with first entry in the map */
			entry = vm_map_first_entry(map);
		} else {
			/* start with next entry */
			entry = entry->vme_next;
		}
	}

	while (entry != vm_map_to_entry(map) &&
	       entry->vme_start <= end) {
		/* try and coalesce "entry" with its previous entry */
		vm_map_simplify_entry(map, entry);
		entry = entry->vme_next;
	}
}


/*
 *	Routine:	vm_map_machine_attribute
 *	Purpose:
 *		Provide machine-specific attributes to mappings,
 *		such as cachability etc. for machines that provide
 *		them.  NUMA architectures and machines with big/strange
 *		caches will use this.
 *	Note:
 *		Responsibilities for locking and checking are handled here,
 *		everything else in the pmap module. If any non-volatile
 *		information must be kept, the pmap module should handle
 *		it itself. [This assumes that attributes do not
 *		need to be inherited, which seems ok to me]
 */
kern_return_t
vm_map_machine_attribute(
	vm_map_t			map,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_machine_attribute_t	attribute,
	vm_machine_attribute_val_t* value)		/* IN/OUT */
{
	kern_return_t	ret;
	vm_map_size_t sync_size;
	vm_map_entry_t entry;

	if (start < vm_map_min(map) || end > vm_map_max(map))
		return KERN_INVALID_ADDRESS;

	/* Figure how much memory we need to flush (in page increments) */
	sync_size = end - start;

	vm_map_lock(map);

	if (attribute != MATTR_CACHE) {
		/* If we don't have to find physical addresses, we */
		/* don't have to do an explicit traversal here.    */
		ret = pmap_attribute(map->pmap, start, end-start,
				     attribute, value);
		vm_map_unlock(map);
		return ret;
	}

	ret = KERN_SUCCESS;										/* Assume it all worked */

	while(sync_size) {
		if (vm_map_lookup_entry(map, start, &entry)) {
			vm_map_size_t	sub_size;
			if((entry->vme_end - start) > sync_size) {
				sub_size = sync_size;
				sync_size = 0;
			} else {
				sub_size = entry->vme_end - start;
				sync_size -= sub_size;
			}
			if(entry->is_sub_map) {
				vm_map_offset_t sub_start;
				vm_map_offset_t sub_end;

				sub_start = (start - entry->vme_start)
					+ VME_OFFSET(entry);
				sub_end = sub_start + sub_size;
				vm_map_machine_attribute(
					VME_SUBMAP(entry),
					sub_start,
					sub_end,
					attribute, value);
			} else {
				if (VME_OBJECT(entry)) {
					vm_page_t		m;
					vm_object_t		object;
					vm_object_t		base_object;
					vm_object_t		last_object;
					vm_object_offset_t	offset;
					vm_object_offset_t	base_offset;
					vm_map_size_t		range;
					range = sub_size;
					offset = (start - entry->vme_start)
						+ VME_OFFSET(entry);
					base_offset = offset;
					object = VME_OBJECT(entry);
					base_object = object;
					last_object = NULL;

					vm_object_lock(object);

					while (range) {
						m = vm_page_lookup(
							object, offset);

						if (m && !m->fictitious) {
						        ret =
								pmap_attribute_cache_sync(
									VM_PAGE_GET_PHYS_PAGE(m),
									PAGE_SIZE,
									attribute, value);

						} else if (object->shadow) {
						        offset = offset + object->vo_shadow_offset;
							last_object = object;
							object = object->shadow;
							vm_object_lock(last_object->shadow);
							vm_object_unlock(last_object);
							continue;
						}
						range -= PAGE_SIZE;

						if (base_object != object) {
						        vm_object_unlock(object);
							vm_object_lock(base_object);
							object = base_object;
						}
						/* Bump to the next page */
						base_offset += PAGE_SIZE;
						offset = base_offset;
					}
					vm_object_unlock(object);
				}
			}
			start += sub_size;
		} else {
			vm_map_unlock(map);
			return KERN_FAILURE;
		}

	}

	vm_map_unlock(map);

	return ret;
}

/*
 *	vm_map_behavior_set:
 *
 *	Sets the paging reference behavior of the specified address
 *	range in the target map.  Paging reference behavior affects
 *	how pagein operations resulting from faults on the map will be
 *	clustered.
 */
kern_return_t
vm_map_behavior_set(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end,
	vm_behavior_t	new_behavior)
{
	vm_map_entry_t	entry;
	vm_map_entry_t	temp_entry;

	XPR(XPR_VM_MAP,
	    "vm_map_behavior_set, 0x%X start 0x%X end 0x%X behavior %d",
	    map, start, end, new_behavior, 0);

	if (start > end ||
	    start < vm_map_min(map) ||
	    end > vm_map_max(map)) {
		return KERN_NO_SPACE;
	}

	switch (new_behavior) {

	/*
	 * This first block of behaviors all set a persistent state on the specified
	 * memory range.  All we have to do here is to record the desired behavior
	 * in the vm_map_entry_t's.
	 */

	case VM_BEHAVIOR_DEFAULT:
	case VM_BEHAVIOR_RANDOM:
	case VM_BEHAVIOR_SEQUENTIAL:
	case VM_BEHAVIOR_RSEQNTL:
	case VM_BEHAVIOR_ZERO_WIRED_PAGES:
		vm_map_lock(map);

		/*
		 *	The entire address range must be valid for the map.
		 * 	Note that vm_map_range_check() does a
		 *	vm_map_lookup_entry() internally and returns the
		 *	entry containing the start of the address range if
		 *	the entire range is valid.
		 */
		if (vm_map_range_check(map, start, end, &temp_entry)) {
			entry = temp_entry;
			vm_map_clip_start(map, entry, start);
		}
		else {
			vm_map_unlock(map);
			return(KERN_INVALID_ADDRESS);
		}

		while ((entry != vm_map_to_entry(map)) && (entry->vme_start < end)) {
			vm_map_clip_end(map, entry, end);
			if (entry->is_sub_map) {
				assert(!entry->use_pmap);
			}

			if( new_behavior == VM_BEHAVIOR_ZERO_WIRED_PAGES ) {
				entry->zero_wired_pages = TRUE;
			} else {
				entry->behavior = new_behavior;
			}
			entry = entry->vme_next;
		}

		vm_map_unlock(map);
		break;

	/*
	 * The rest of these are different from the above in that they cause
	 * an immediate action to take place as opposed to setting a behavior that
	 * affects future actions.
	 */

	case VM_BEHAVIOR_WILLNEED:
		return vm_map_willneed(map, start, end);

	case VM_BEHAVIOR_DONTNEED:
		return vm_map_msync(map, start, end - start, VM_SYNC_DEACTIVATE | VM_SYNC_CONTIGUOUS);

	case VM_BEHAVIOR_FREE:
		return vm_map_msync(map, start, end - start, VM_SYNC_KILLPAGES | VM_SYNC_CONTIGUOUS);

	case VM_BEHAVIOR_REUSABLE:
		return vm_map_reusable_pages(map, start, end);

	case VM_BEHAVIOR_REUSE:
		return vm_map_reuse_pages(map, start, end);

	case VM_BEHAVIOR_CAN_REUSE:
		return vm_map_can_reuse(map, start, end);

#if MACH_ASSERT
	case VM_BEHAVIOR_PAGEOUT:
		return vm_map_pageout(map, start, end);
#endif /* MACH_ASSERT */

	default:
		return(KERN_INVALID_ARGUMENT);
	}

	return(KERN_SUCCESS);
}


/*
 * Internals for madvise(MADV_WILLNEED) system call.
 *
 * The present implementation is to do a read-ahead if the mapping corresponds
 * to a mapped regular file.  If it's an anonymous mapping, then we do nothing
 * and basically ignore the "advice" (which we are always free to do).
 */


static kern_return_t
vm_map_willneed(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end
)
{
	vm_map_entry_t 			entry;
	vm_object_t			object;
	memory_object_t			pager;
	struct vm_object_fault_info	fault_info;
	kern_return_t			kr;
	vm_object_size_t		len;
	vm_object_offset_t		offset;

	/*
	 * Fill in static values in fault_info.  Several fields get ignored by the code
	 * we call, but we'll fill them in anyway since uninitialized fields are bad
	 * when it comes to future backwards compatibility.
	 */

	fault_info.interruptible = THREAD_UNINT;		/* ignored value */
	fault_info.behavior      = VM_BEHAVIOR_SEQUENTIAL;
	fault_info.no_cache      = FALSE;			/* ignored value */
	fault_info.stealth	 = TRUE;
	fault_info.io_sync = FALSE;
	fault_info.cs_bypass = FALSE;
	fault_info.mark_zf_absent = FALSE;
	fault_info.batch_pmap_op = FALSE;

	/*
	 * The MADV_WILLNEED operation doesn't require any changes to the
	 * vm_map_entry_t's, so the read lock is sufficient.
	 */

	vm_map_lock_read(map);

	/*
	 * The madvise semantics require that the address range be fully
	 * allocated with no holes.  Otherwise, we're required to return
	 * an error.
	 */

	if (! vm_map_range_check(map, start, end, &entry)) {
		vm_map_unlock_read(map);
		return KERN_INVALID_ADDRESS;
	}

	/*
	 * Examine each vm_map_entry_t in the range.
	 */
	for (; entry != vm_map_to_entry(map) && start < end; ) {

		/*
		 * The first time through, the start address could be anywhere
		 * within the vm_map_entry we found.  So adjust the offset to
		 * correspond.  After that, the offset will always be zero to
		 * correspond to the beginning of the current vm_map_entry.
		 */
		offset = (start - entry->vme_start) + VME_OFFSET(entry);

		/*
		 * Set the length so we don't go beyond the end of the
		 * map_entry or beyond the end of the range we were given.
		 * This range could span also multiple map entries all of which
		 * map different files, so make sure we only do the right amount
		 * of I/O for each object.  Note that it's possible for there
		 * to be multiple map entries all referring to the same object
		 * but with different page permissions, but it's not worth
		 * trying to optimize that case.
		 */
		len = MIN(entry->vme_end - start, end - start);

		if ((vm_size_t) len != len) {
			/* 32-bit overflow */
			len = (vm_size_t) (0 - PAGE_SIZE);
		}
		fault_info.cluster_size = (vm_size_t) len;
		fault_info.lo_offset    = offset;
		fault_info.hi_offset    = offset + len;
		fault_info.user_tag     = VME_ALIAS(entry);
		fault_info.pmap_options = 0;
		if (entry->iokit_acct ||
		    (!entry->is_sub_map && !entry->use_pmap)) {
			fault_info.pmap_options |= PMAP_OPTIONS_ALT_ACCT;
		}

		/*
		 * If there's no read permission to this mapping, then just
		 * skip it.
		 */
		if ((entry->protection & VM_PROT_READ) == 0) {
			entry = entry->vme_next;
			start = entry->vme_start;
			continue;
		}

		/*
		 * Find the file object backing this map entry.  If there is
		 * none, then we simply ignore the "will need" advice for this
		 * entry and go on to the next one.
		 */
		if ((object = find_vnode_object(entry)) == VM_OBJECT_NULL) {
			entry = entry->vme_next;
			start = entry->vme_start;
			continue;
		}

		/*
		 * The data_request() could take a long time, so let's
		 * release the map lock to avoid blocking other threads.
		 */
		vm_map_unlock_read(map);

		vm_object_paging_begin(object);
		pager = object->pager;
		vm_object_unlock(object);

		/*
		 * Get the data from the object asynchronously.
		 *
		 * Note that memory_object_data_request() places limits on the
		 * amount of I/O it will do.  Regardless of the len we
		 * specified, it won't do more than MAX_UPL_TRANSFER_BYTES and it
		 * silently truncates the len to that size.  This isn't
		 * necessarily bad since madvise shouldn't really be used to
		 * page in unlimited amounts of data.  Other Unix variants
		 * limit the willneed case as well.  If this turns out to be an
		 * issue for developers, then we can always adjust the policy
		 * here and still be backwards compatible since this is all
		 * just "advice".
		 */
		kr = memory_object_data_request(
			pager,
			offset + object->paging_offset,
			0,	/* ignored */
			VM_PROT_READ,
			(memory_object_fault_info_t)&fault_info);

		vm_object_lock(object);
		vm_object_paging_end(object);
		vm_object_unlock(object);

		/*
		 * If we couldn't do the I/O for some reason, just give up on
		 * the madvise.  We still return success to the user since
		 * madvise isn't supposed to fail when the advice can't be
		 * taken.
		 */
		if (kr != KERN_SUCCESS) {
			return KERN_SUCCESS;
		}

		start += len;
		if (start >= end) {
			/* done */
			return KERN_SUCCESS;
		}

		/* look up next entry */
		vm_map_lock_read(map);
		if (! vm_map_lookup_entry(map, start, &entry)) {
			/*
			 * There's a new hole in the address range.
			 */
			vm_map_unlock_read(map);
			return KERN_INVALID_ADDRESS;
		}
	}

	vm_map_unlock_read(map);
	return KERN_SUCCESS;
}

static boolean_t
vm_map_entry_is_reusable(
	vm_map_entry_t entry)
{
	/* Only user map entries */

	vm_object_t object;

	if (entry->is_sub_map) {
		return FALSE;
	}

	switch (VME_ALIAS(entry)) {
	case VM_MEMORY_MALLOC:
	case VM_MEMORY_MALLOC_SMALL:
	case VM_MEMORY_MALLOC_LARGE:
	case VM_MEMORY_REALLOC:
	case VM_MEMORY_MALLOC_TINY:
	case VM_MEMORY_MALLOC_LARGE_REUSABLE:
	case VM_MEMORY_MALLOC_LARGE_REUSED:
		/*
		 * This is a malloc() memory region: check if it's still
		 * in its original state and can be re-used for more
		 * malloc() allocations.
		 */
		break;
	default:
		/*
		 * Not a malloc() memory region: let the caller decide if
		 * it's re-usable.
		 */
		return TRUE;
	}

	if (entry->is_shared ||
	    entry->is_sub_map ||
	    entry->in_transition ||
	    entry->protection != VM_PROT_DEFAULT ||
	    entry->max_protection != VM_PROT_ALL ||
	    entry->inheritance != VM_INHERIT_DEFAULT ||
	    entry->no_cache ||
	    entry->permanent ||
	    entry->superpage_size != FALSE ||
	    entry->zero_wired_pages ||
	    entry->wired_count != 0 ||
	    entry->user_wired_count != 0) {
		return FALSE;
	}

	object = VME_OBJECT(entry);
	if (object == VM_OBJECT_NULL) {
		return TRUE;
	}
	if (
#if 0
		/*
		 * Let's proceed even if the VM object is potentially
		 * shared.
		 * We check for this later when processing the actual
		 * VM pages, so the contents will be safe if shared.
		 *
		 * But we can still mark this memory region as "reusable" to
		 * acknowledge that the caller did let us know that the memory
		 * could be re-used and should not be penalized for holding
		 * on to it.  This allows its "resident size" to not include
		 * the reusable range.
		 */
	    object->ref_count == 1 &&
#endif
	    object->wired_page_count == 0 &&
	    object->copy == VM_OBJECT_NULL &&
	    object->shadow == VM_OBJECT_NULL &&
	    object->copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC &&
	    object->internal &&
	    !object->true_share &&
	    object->wimg_bits == VM_WIMG_USE_DEFAULT &&
	    !object->code_signed) {
		return TRUE;
	}
	return FALSE;


}

static kern_return_t
vm_map_reuse_pages(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	vm_map_entry_t 			entry;
	vm_object_t			object;
	vm_object_offset_t		start_offset, end_offset;

	/*
	 * The MADV_REUSE operation doesn't require any changes to the
	 * vm_map_entry_t's, so the read lock is sufficient.
	 */

	vm_map_lock_read(map);
	assert(map->pmap != kernel_pmap);	/* protect alias access */

	/*
	 * The madvise semantics require that the address range be fully
	 * allocated with no holes.  Otherwise, we're required to return
	 * an error.
	 */

	if (!vm_map_range_check(map, start, end, &entry)) {
		vm_map_unlock_read(map);
		vm_page_stats_reusable.reuse_pages_failure++;
		return KERN_INVALID_ADDRESS;
	}

	/*
	 * Examine each vm_map_entry_t in the range.
	 */
	for (; entry != vm_map_to_entry(map) && entry->vme_start < end;
	     entry = entry->vme_next) {
		/*
		 * Sanity check on the VM map entry.
		 */
		if (! vm_map_entry_is_reusable(entry)) {
			vm_map_unlock_read(map);
			vm_page_stats_reusable.reuse_pages_failure++;
			return KERN_INVALID_ADDRESS;
		}

		/*
		 * The first time through, the start address could be anywhere
		 * within the vm_map_entry we found.  So adjust the offset to
		 * correspond.
		 */
		if (entry->vme_start < start) {
			start_offset = start - entry->vme_start;
		} else {
			start_offset = 0;
		}
		end_offset = MIN(end, entry->vme_end) - entry->vme_start;
		start_offset += VME_OFFSET(entry);
		end_offset += VME_OFFSET(entry);

		assert(!entry->is_sub_map);
		object = VME_OBJECT(entry);
		if (object != VM_OBJECT_NULL) {
			vm_object_lock(object);
			vm_object_reuse_pages(object, start_offset, end_offset,
					      TRUE);
			vm_object_unlock(object);
		}

		if (VME_ALIAS(entry) == VM_MEMORY_MALLOC_LARGE_REUSABLE) {
			/*
			 * XXX
			 * We do not hold the VM map exclusively here.
			 * The "alias" field is not that critical, so it's
			 * safe to update it here, as long as it is the only
			 * one that can be modified while holding the VM map
			 * "shared".
			 */
			VME_ALIAS_SET(entry, VM_MEMORY_MALLOC_LARGE_REUSED);
		}
	}

	vm_map_unlock_read(map);
	vm_page_stats_reusable.reuse_pages_success++;
	return KERN_SUCCESS;
}


static kern_return_t
vm_map_reusable_pages(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	vm_map_entry_t 			entry;
	vm_object_t			object;
	vm_object_offset_t		start_offset, end_offset;
	vm_map_offset_t			pmap_offset;

	/*
	 * The MADV_REUSABLE operation doesn't require any changes to the
	 * vm_map_entry_t's, so the read lock is sufficient.
	 */

	vm_map_lock_read(map);
	assert(map->pmap != kernel_pmap);	/* protect alias access */

	/*
	 * The madvise semantics require that the address range be fully
	 * allocated with no holes.  Otherwise, we're required to return
	 * an error.
	 */

	if (!vm_map_range_check(map, start, end, &entry)) {
		vm_map_unlock_read(map);
		vm_page_stats_reusable.reusable_pages_failure++;
		return KERN_INVALID_ADDRESS;
	}

	/*
	 * Examine each vm_map_entry_t in the range.
	 */
	for (; entry != vm_map_to_entry(map) && entry->vme_start < end;
	     entry = entry->vme_next) {
		int kill_pages = 0;

		/*
		 * Sanity check on the VM map entry.
		 */
		if (! vm_map_entry_is_reusable(entry)) {
			vm_map_unlock_read(map);
			vm_page_stats_reusable.reusable_pages_failure++;
			return KERN_INVALID_ADDRESS;
		}

		if (! (entry->protection & VM_PROT_WRITE) && !entry->used_for_jit) {
			/* not writable: can't discard contents */
			vm_map_unlock_read(map);
			vm_page_stats_reusable.reusable_nonwritable++;
			vm_page_stats_reusable.reusable_pages_failure++;
			return KERN_PROTECTION_FAILURE;
		}

		/*
		 * The first time through, the start address could be anywhere
		 * within the vm_map_entry we found.  So adjust the offset to
		 * correspond.
		 */
		if (entry->vme_start < start) {
			start_offset = start - entry->vme_start;
			pmap_offset = start;
		} else {
			start_offset = 0;
			pmap_offset = entry->vme_start;
		}
		end_offset = MIN(end, entry->vme_end) - entry->vme_start;
		start_offset += VME_OFFSET(entry);
		end_offset += VME_OFFSET(entry);

		assert(!entry->is_sub_map);
		object = VME_OBJECT(entry);
		if (object == VM_OBJECT_NULL)
			continue;


		vm_object_lock(object);
		if (((object->ref_count == 1) ||
		     (object->copy_strategy != MEMORY_OBJECT_COPY_SYMMETRIC &&
		      object->copy == VM_OBJECT_NULL)) &&
		    object->shadow == VM_OBJECT_NULL &&
		    /*
		     * "iokit_acct" entries are billed for their virtual size
		     * (rather than for their resident pages only), so they
		     * wouldn't benefit from making pages reusable, and it
		     * would be hard to keep track of pages that are both
		     * "iokit_acct" and "reusable" in the pmap stats and
		     * ledgers.
		     */
		    !(entry->iokit_acct ||
		      (!entry->is_sub_map && !entry->use_pmap))) {
			if (object->ref_count != 1) {
				vm_page_stats_reusable.reusable_shared++;
			}
			kill_pages = 1;
		} else {
			kill_pages = -1;
		}
		if (kill_pages != -1) {
			vm_object_deactivate_pages(object,
						   start_offset,
						   end_offset - start_offset,
						   kill_pages,
						   TRUE /*reusable_pages*/,
						   map->pmap,
						   pmap_offset);
		} else {
			vm_page_stats_reusable.reusable_pages_shared++;
		}
		vm_object_unlock(object);

		if (VME_ALIAS(entry) == VM_MEMORY_MALLOC_LARGE ||
		    VME_ALIAS(entry) == VM_MEMORY_MALLOC_LARGE_REUSED) {
			/*
			 * XXX
			 * We do not hold the VM map exclusively here.
			 * The "alias" field is not that critical, so it's
			 * safe to update it here, as long as it is the only
			 * one that can be modified while holding the VM map
			 * "shared".
			 */
			VME_ALIAS_SET(entry, VM_MEMORY_MALLOC_LARGE_REUSABLE);
		}
	}

	vm_map_unlock_read(map);
	vm_page_stats_reusable.reusable_pages_success++;
	return KERN_SUCCESS;
}


static kern_return_t
vm_map_can_reuse(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	vm_map_entry_t 			entry;

	/*
	 * The MADV_REUSABLE operation doesn't require any changes to the
	 * vm_map_entry_t's, so the read lock is sufficient.
	 */

	vm_map_lock_read(map);
	assert(map->pmap != kernel_pmap);	/* protect alias access */

	/*
	 * The madvise semantics require that the address range be fully
	 * allocated with no holes.  Otherwise, we're required to return
	 * an error.
	 */

	if (!vm_map_range_check(map, start, end, &entry)) {
		vm_map_unlock_read(map);
		vm_page_stats_reusable.can_reuse_failure++;
		return KERN_INVALID_ADDRESS;
	}

	/*
	 * Examine each vm_map_entry_t in the range.
	 */
	for (; entry != vm_map_to_entry(map) && entry->vme_start < end;
	     entry = entry->vme_next) {
		/*
		 * Sanity check on the VM map entry.
		 */
		if (! vm_map_entry_is_reusable(entry)) {
			vm_map_unlock_read(map);
			vm_page_stats_reusable.can_reuse_failure++;
			return KERN_INVALID_ADDRESS;
		}
	}

	vm_map_unlock_read(map);
	vm_page_stats_reusable.can_reuse_success++;
	return KERN_SUCCESS;
}


#if MACH_ASSERT
static kern_return_t
vm_map_pageout(
	vm_map_t	map,
	vm_map_offset_t	start,
	vm_map_offset_t	end)
{
	vm_map_entry_t 			entry;

	/*
	 * The MADV_PAGEOUT operation doesn't require any changes to the
	 * vm_map_entry_t's, so the read lock is sufficient.
	 */

	vm_map_lock_read(map);

	/*
	 * The madvise semantics require that the address range be fully
	 * allocated with no holes.  Otherwise, we're required to return
	 * an error.
	 */

	if (!vm_map_range_check(map, start, end, &entry)) {
		vm_map_unlock_read(map);
		return KERN_INVALID_ADDRESS;
	}

	/*
	 * Examine each vm_map_entry_t in the range.
	 */
	for (; entry != vm_map_to_entry(map) && entry->vme_start < end;
	     entry = entry->vme_next) {
		vm_object_t	object;

		/*
		 * Sanity check on the VM map entry.
		 */
		if (entry->is_sub_map) {
			vm_map_t submap;
			vm_map_offset_t submap_start;
			vm_map_offset_t submap_end;
			vm_map_entry_t submap_entry;

			submap = VME_SUBMAP(entry);
			submap_start = VME_OFFSET(entry);
			submap_end = submap_start + (entry->vme_end -
						     entry->vme_start);

			vm_map_lock_read(submap);

			if (! vm_map_range_check(submap,
						 submap_start,
						 submap_end,
						 &submap_entry)) {
				vm_map_unlock_read(submap);
				vm_map_unlock_read(map);
				return KERN_INVALID_ADDRESS;
			}

			object = VME_OBJECT(submap_entry);
			if (submap_entry->is_sub_map ||
			    object == VM_OBJECT_NULL ||
			    !object->internal) {
				vm_map_unlock_read(submap);
				continue;
			}

			vm_object_pageout(object);

			vm_map_unlock_read(submap);
			submap = VM_MAP_NULL;
			submap_entry = VM_MAP_ENTRY_NULL;
			continue;
		}

		object = VME_OBJECT(entry);
		if (entry->is_sub_map ||
		    object == VM_OBJECT_NULL ||
		    !object->internal) {
			continue;
		}

		vm_object_pageout(object);
	}

	vm_map_unlock_read(map);
	return KERN_SUCCESS;
}
#endif /* MACH_ASSERT */


/*
 *	Routine:	vm_map_entry_insert
 *
 *	Descritpion:	This routine inserts a new vm_entry in a locked map.
 */
vm_map_entry_t
vm_map_entry_insert(
	vm_map_t		map,
	vm_map_entry_t		insp_entry,
	vm_map_offset_t		start,
	vm_map_offset_t		end,
	vm_object_t		object,
	vm_object_offset_t	offset,
	boolean_t		needs_copy,
	boolean_t		is_shared,
	boolean_t		in_transition,
	vm_prot_t		cur_protection,
	vm_prot_t		max_protection,
	vm_behavior_t		behavior,
	vm_inherit_t		inheritance,
	unsigned		wired_count,
	boolean_t		no_cache,
	boolean_t		permanent,
	unsigned int		superpage_size,
	boolean_t		clear_map_aligned,
	boolean_t		is_submap,
	boolean_t		used_for_jit,
	int			alias)
{
	vm_map_entry_t	new_entry;

	assert(insp_entry != (vm_map_entry_t)0);

#if DEVELOPMENT || DEBUG
	vm_object_offset_t	end_offset = 0;
	assertf(!os_add_overflow(end - start, offset, &end_offset), "size 0x%llx, offset 0x%llx caused overflow", (uint64_t)(end - start), offset);
#endif /* DEVELOPMENT || DEBUG */

	new_entry = vm_map_entry_create(map, !map->hdr.entries_pageable);

	if (VM_MAP_PAGE_SHIFT(map) != PAGE_SHIFT) {
		new_entry->map_aligned = TRUE;
	} else {
		new_entry->map_aligned = FALSE;
	}
	if (clear_map_aligned &&
	    (! VM_MAP_PAGE_ALIGNED(start, VM_MAP_PAGE_MASK(map)) ||
	     ! VM_MAP_PAGE_ALIGNED(end, VM_MAP_PAGE_MASK(map)))) {
		new_entry->map_aligned = FALSE;
	}

	new_entry->vme_start = start;
	new_entry->vme_end = end;
	assert(page_aligned(new_entry->vme_start));
	assert(page_aligned(new_entry->vme_end));
	if (new_entry->map_aligned) {
		assert(VM_MAP_PAGE_ALIGNED(new_entry->vme_start,
					   VM_MAP_PAGE_MASK(map)));
		assert(VM_MAP_PAGE_ALIGNED(new_entry->vme_end,
					   VM_MAP_PAGE_MASK(map)));
	}
	assert(new_entry->vme_start < new_entry->vme_end);

	VME_OBJECT_SET(new_entry, object);
	VME_OFFSET_SET(new_entry, offset);
	new_entry->is_shared = is_shared;
	new_entry->is_sub_map = is_submap;
	new_entry->needs_copy = needs_copy;
	new_entry->in_transition = in_transition;
	new_entry->needs_wakeup = FALSE;
	new_entry->inheritance = inheritance;
	new_entry->protection = cur_protection;
	new_entry->max_protection = max_protection;
	new_entry->behavior = behavior;
	new_entry->wired_count = wired_count;
	new_entry->user_wired_count = 0;
	if (is_submap) {
		/*
		 * submap: "use_pmap" means "nested".
		 * default: false.
		 */
		new_entry->use_pmap = FALSE;
	} else {
		/*
		 * object: "use_pmap" means "use pmap accounting" for footprint.
		 * default: true.
		 */
		new_entry->use_pmap = TRUE;
	}
	VME_ALIAS_SET(new_entry, alias);
	new_entry->zero_wired_pages = FALSE;
	new_entry->no_cache = no_cache;
	new_entry->permanent = permanent;
	if (superpage_size)
		new_entry->superpage_size = TRUE;
	else
		new_entry->superpage_size = FALSE;
	if (used_for_jit){
		if (!(map->jit_entry_exists)){
			new_entry->used_for_jit = TRUE;
			map->jit_entry_exists = TRUE;

			/* Tell the pmap that it supports JIT. */
			pmap_set_jit_entitled(map->pmap);
		}
	} else {
		new_entry->used_for_jit = FALSE;
	}
	new_entry->iokit_acct = FALSE;
	new_entry->vme_resilient_codesign = FALSE;
	new_entry->vme_resilient_media = FALSE;
	new_entry->vme_atomic = FALSE;

	/*
	 *	Insert the new entry into the list.
	 */

	vm_map_store_entry_link(map, insp_entry, new_entry);
	map->size += end - start;

	/*
	 *	Update the free space hint and the lookup hint.
	 */

	SAVE_HINT_MAP_WRITE(map, new_entry);
	return new_entry;
}

/*
 *	Routine:	vm_map_remap_extract
 *
 *	Descritpion:	This routine returns a vm_entry list from a map.
 */
static kern_return_t
vm_map_remap_extract(
	vm_map_t		map,
	vm_map_offset_t		addr,
	vm_map_size_t		size,
	boolean_t		copy,
	struct vm_map_header	*map_header,
	vm_prot_t		*cur_protection,
	vm_prot_t		*max_protection,
	/* What, no behavior? */
	vm_inherit_t		inheritance,
	boolean_t		pageable,
	boolean_t		same_map,
	vm_map_kernel_flags_t	vmk_flags)
{
	kern_return_t		result;
	vm_map_size_t		mapped_size;
	vm_map_size_t		tmp_size;
	vm_map_entry_t		src_entry;     /* result of last map lookup */
	vm_map_entry_t		new_entry;
	vm_object_offset_t	offset;
	vm_map_offset_t		map_address;
	vm_map_offset_t		src_start;     /* start of entry to map */
	vm_map_offset_t		src_end;       /* end of region to be mapped */
	vm_object_t		object;
	vm_map_version_t	version;
	boolean_t		src_needs_copy;
	boolean_t		new_entry_needs_copy;
	vm_map_entry_t		saved_src_entry;
	boolean_t		src_entry_was_wired;

	assert(map != VM_MAP_NULL);
	assert(size != 0);
	assert(size == vm_map_round_page(size, PAGE_MASK));
	assert(inheritance == VM_INHERIT_NONE ||
	       inheritance == VM_INHERIT_COPY ||
	       inheritance == VM_INHERIT_SHARE);

	/*
	 *	Compute start and end of region.
	 */
	src_start = vm_map_trunc_page(addr, PAGE_MASK);
	src_end = vm_map_round_page(src_start + size, PAGE_MASK);


	/*
	 *	Initialize map_header.
	 */
	map_header->links.next = (struct vm_map_entry *)&map_header->links;
	map_header->links.prev = (struct vm_map_entry *)&map_header->links;
	map_header->nentries = 0;
	map_header->entries_pageable = pageable;
	map_header->page_shift = PAGE_SHIFT;

	vm_map_store_init( map_header );

	*cur_protection = VM_PROT_ALL;
	*max_protection = VM_PROT_ALL;

	map_address = 0;
	mapped_size = 0;
	result = KERN_SUCCESS;

	/*
	 *	The specified source virtual space might correspond to
	 *	multiple map entries, need to loop on them.
	 */
	vm_map_lock(map);
	while (mapped_size != size) {
		vm_map_size_t	entry_size;

		/*
		 *	Find the beginning of the region.
		 */
		if (! vm_map_lookup_entry(map, src_start, &src_entry)) {
			result = KERN_INVALID_ADDRESS;
			break;
		}

		if (src_start < src_entry->vme_start ||
		    (mapped_size && src_start != src_entry->vme_start)) {
			result = KERN_INVALID_ADDRESS;
			break;
		}

		tmp_size = size - mapped_size;
		if (src_end > src_entry->vme_end)
			tmp_size -= (src_end - src_entry->vme_end);

		entry_size = (vm_map_size_t)(src_entry->vme_end -
					     src_entry->vme_start);

		if(src_entry->is_sub_map) {
			vm_map_reference(VME_SUBMAP(src_entry));
			object = VM_OBJECT_NULL;
		} else {
			object = VME_OBJECT(src_entry);
			if (src_entry->iokit_acct) {
				/*
				 * This entry uses "IOKit accounting".
				 */
			} else if (object != VM_OBJECT_NULL &&
				   object->purgable != VM_PURGABLE_DENY) {
				/*
				 * Purgeable objects have their own accounting:
				 * no pmap accounting for them.
				 */
				assertf(!src_entry->use_pmap,
					"map=%p src_entry=%p [0x%llx:0x%llx] 0x%x/0x%x %d",
					map,
					src_entry,
					(uint64_t)src_entry->vme_start,
					(uint64_t)src_entry->vme_end,
					src_entry->protection,
					src_entry->max_protection,
					VME_ALIAS(src_entry));
			} else {
				/*
				 * Not IOKit or purgeable:
				 * must be accounted by pmap stats.
				 */
				assertf(src_entry->use_pmap,
					"map=%p src_entry=%p [0x%llx:0x%llx] 0x%x/0x%x %d",
					map,
					src_entry,
					(uint64_t)src_entry->vme_start,
					(uint64_t)src_entry->vme_end,
					src_entry->protection,
					src_entry->max_protection,
					VME_ALIAS(src_entry));
			}

			if (object == VM_OBJECT_NULL) {
				object = vm_object_allocate(entry_size);
				VME_OFFSET_SET(src_entry, 0);
				VME_OBJECT_SET(src_entry, object);
				assert(src_entry->use_pmap);
			} else if (object->copy_strategy !=
				   MEMORY_OBJECT_COPY_SYMMETRIC) {
				/*
				 *	We are already using an asymmetric
				 *	copy, and therefore we already have
				 *	the right object.
				 */
				assert(!src_entry->needs_copy);
			} else if (src_entry->needs_copy || object->shadowed ||
				   (object->internal && !object->true_share &&
				    !src_entry->is_shared &&
				    object->vo_size > entry_size)) {

				VME_OBJECT_SHADOW(src_entry, entry_size);
				assert(src_entry->use_pmap);

				if (!src_entry->needs_copy &&
				    (src_entry->protection & VM_PROT_WRITE)) {
				        vm_prot_t prot;

					assert(!pmap_has_prot_policy(src_entry->protection));

				        prot = src_entry->protection & ~VM_PROT_WRITE;

					if (override_nx(map,
							VME_ALIAS(src_entry))
					    && prot)
					        prot |= VM_PROT_EXECUTE;

					assert(!pmap_has_prot_policy(prot));

					if(map->mapped_in_other_pmaps) {
						vm_object_pmap_protect(
							VME_OBJECT(src_entry),
							VME_OFFSET(src_entry),
							entry_size,
							PMAP_NULL,
							src_entry->vme_start,
							prot);
					} else {
						pmap_protect(vm_map_pmap(map),
							     src_entry->vme_start,
							     src_entry->vme_end,
							     prot);
					}
				}

				object = VME_OBJECT(src_entry);
				src_entry->needs_copy = FALSE;
			}


			vm_object_lock(object);
			vm_object_reference_locked(object); /* object ref. for new entry */
			if (object->copy_strategy ==
			    MEMORY_OBJECT_COPY_SYMMETRIC) {
				object->copy_strategy =
					MEMORY_OBJECT_COPY_DELAY;
			}
			vm_object_unlock(object);
		}

		offset = (VME_OFFSET(src_entry) +
			  (src_start - src_entry->vme_start));

		new_entry = _vm_map_entry_create(map_header, !map_header->entries_pageable);
		vm_map_entry_copy(new_entry, src_entry);
		if (new_entry->is_sub_map) {
			/* clr address space specifics */
			new_entry->use_pmap = FALSE;
		} else if (copy) {
			/*
			 * We're dealing with a copy-on-write operation,
			 * so the resulting mapping should not inherit the
			 * original mapping's accounting settings.
			 * "use_pmap" should be reset to its default (TRUE)
			 * so that the new mapping gets accounted for in
			 * the task's memory footprint.
			 */
			new_entry->use_pmap = TRUE;
		}
		/* "iokit_acct" was cleared in vm_map_entry_copy() */
		assert(!new_entry->iokit_acct);

		new_entry->map_aligned = FALSE;

		new_entry->vme_start = map_address;
		new_entry->vme_end = map_address + tmp_size;
		assert(new_entry->vme_start < new_entry->vme_end);
		if (copy && vmk_flags.vmkf_remap_prot_copy) {
			/*
			 * Remapping for vm_map_protect(VM_PROT_COPY)
			 * to convert a read-only mapping into a
			 * copy-on-write version of itself but
			 * with write access:
			 * keep the original inheritance and add 
			 * VM_PROT_WRITE to the max protection.
			 */
			new_entry->inheritance = src_entry->inheritance;
			new_entry->max_protection |= VM_PROT_WRITE;
		} else {
			new_entry->inheritance = inheritance;
		}
		VME_OFFSET_SET(new_entry, offset);
		
		/*
		 * The new region has to be copied now if required.
		 */
	RestartCopy:
		if (!copy) {
			/*
			 * Cannot allow an entry describing a JIT
			 * region to be shared across address spaces.
			 */
			if (src_entry->used_for_jit == TRUE && !same_map) {
				result = KERN_INVALID_ARGUMENT;
				break;
			}
			src_entry->is_shared = TRUE;
			new_entry->is_shared = TRUE;
			if (!(new_entry->is_sub_map))
				new_entry->needs_copy = FALSE;

		} else if (src_entry->is_sub_map) {
			/* make this a COW sub_map if not already */
			assert(new_entry->wired_count == 0);
			new_entry->needs_copy = TRUE;
			object = VM_OBJECT_NULL;
		} else if (src_entry->wired_count == 0 &&
			   vm_object_copy_quickly(&VME_OBJECT(new_entry),
						  VME_OFFSET(new_entry),
						  (new_entry->vme_end -
						   new_entry->vme_start),
						  &src_needs_copy,
						  &new_entry_needs_copy)) {

			new_entry->needs_copy = new_entry_needs_copy;
			new_entry->is_shared = FALSE;
			assertf(new_entry->use_pmap, "map %p new_entry %p\n", map, new_entry);

			/*
			 * Handle copy_on_write semantics.
			 */
			if (src_needs_copy && !src_entry->needs_copy) {
			        vm_prot_t prot;

				assert(!pmap_has_prot_policy(src_entry->protection));

				prot = src_entry->protection & ~VM_PROT_WRITE;

				if (override_nx(map,
						VME_ALIAS(src_entry))
				    && prot)
				        prot |= VM_PROT_EXECUTE;

				assert(!pmap_has_prot_policy(prot));

				vm_object_pmap_protect(object,
						       offset,
						       entry_size,
						       ((src_entry->is_shared
							 || map->mapped_in_other_pmaps) ?
							PMAP_NULL : map->pmap),
						       src_entry->vme_start,
						       prot);

				assert(src_entry->wired_count == 0);
				src_entry->needs_copy = TRUE;
			}
			/*
			 * Throw away the old object reference of the new entry.
			 */
			vm_object_deallocate(object);

		} else {
			new_entry->is_shared = FALSE;
			assertf(new_entry->use_pmap, "map %p new_entry %p\n", map, new_entry);

			src_entry_was_wired = (src_entry->wired_count > 0);
			saved_src_entry = src_entry;
			src_entry = VM_MAP_ENTRY_NULL;

			/*
			 * The map can be safely unlocked since we
			 * already hold a reference on the object.
			 *
			 * Record the timestamp of the map for later
			 * verification, and unlock the map.
			 */
			version.main_timestamp = map->timestamp;
			vm_map_unlock(map); 	/* Increments timestamp once! */

			/*
			 * Perform the copy.
			 */
			if (src_entry_was_wired > 0) {
				vm_object_lock(object);
				result = vm_object_copy_slowly(
					object,
					offset,
					(new_entry->vme_end -
					new_entry->vme_start),
					THREAD_UNINT,
					&VME_OBJECT(new_entry));

				VME_OFFSET_SET(new_entry, 0);
				new_entry->needs_copy = FALSE;
			} else {
				vm_object_offset_t new_offset;

				new_offset = VME_OFFSET(new_entry);
				result = vm_object_copy_strategically(
					object,
					offset,
					(new_entry->vme_end -
					new_entry->vme_start),
					&VME_OBJECT(new_entry),
					&new_offset,
					&new_entry_needs_copy);
				if (new_offset != VME_OFFSET(new_entry)) {
					VME_OFFSET_SET(new_entry, new_offset);
				}

				new_entry->needs_copy = new_entry_needs_copy;
			}

			/*
			 * Throw away the old object reference of the new entry.
			 */
			vm_object_deallocate(object);

			if (result != KERN_SUCCESS &&
			    result != KERN_MEMORY_RESTART_COPY) {
				_vm_map_entry_dispose(map_header, new_entry);
				vm_map_lock(map);
				break;
			}

			/*
			 * Verify that the map has not substantially
			 * changed while the copy was being made.
			 */

			vm_map_lock(map);
			if (version.main_timestamp + 1 != map->timestamp) {
				/*
				 * Simple version comparison failed.
				 *
				 * Retry the lookup and verify that the
				 * same object/offset are still present.
				 */
				saved_src_entry = VM_MAP_ENTRY_NULL;
				vm_object_deallocate(VME_OBJECT(new_entry));
				_vm_map_entry_dispose(map_header, new_entry);
				if (result == KERN_MEMORY_RESTART_COPY)
					result = KERN_SUCCESS;
				continue;
			}
			/* map hasn't changed: src_entry is still valid */
			src_entry = saved_src_entry;
			saved_src_entry = VM_MAP_ENTRY_NULL;

			if (result == KERN_MEMORY_RESTART_COPY) {
				vm_object_reference(object);
				goto RestartCopy;
			}
		}

		_vm_map_store_entry_link(map_header,
				   map_header->links.prev, new_entry);

		/*Protections for submap mapping are irrelevant here*/
		if( !src_entry->is_sub_map ) {
			*cur_protection &= src_entry->protection;
			*max_protection &= src_entry->max_protection;
		}
		map_address += tmp_size;
		mapped_size += tmp_size;
		src_start += tmp_size;

	} /* end while */

	vm_map_unlock(map);
	if (result != KERN_SUCCESS) {
		/*
		 * Free all allocated elements.
		 */
		for (src_entry = map_header->links.next;
		     src_entry != (struct vm_map_entry *)&map_header->links;
		     src_entry = new_entry) {
			new_entry = src_entry->vme_next;
			_vm_map_store_entry_unlink(map_header, src_entry);
			if (src_entry->is_sub_map) {
				vm_map_deallocate(VME_SUBMAP(src_entry));
			} else {
				vm_object_deallocate(VME_OBJECT(src_entry));
			}
			_vm_map_entry_dispose(map_header, src_entry);
		}
	}
	return result;
}

/*
 *	Routine:	vm_remap
 *
 *			Map portion of a task's address space.
 *			Mapped region must not overlap more than
 *			one vm memory object. Protections and
 *			inheritance attributes remain the same
 *			as in the original task and are	out parameters.
 *			Source and Target task can be identical
 *			Other attributes are identical as for vm_map()
 */
kern_return_t
vm_map_remap(
	vm_map_t		target_map,
	vm_map_address_t	*address,
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	int			flags,
	vm_map_kernel_flags_t	vmk_flags,
	vm_tag_t		tag,
	vm_map_t		src_map,
	vm_map_offset_t		memory_address,
	boolean_t		copy,
	vm_prot_t		*cur_protection,
	vm_prot_t		*max_protection,
	vm_inherit_t		inheritance)
{
	kern_return_t		result;
	vm_map_entry_t		entry;
	vm_map_entry_t		insp_entry = VM_MAP_ENTRY_NULL;
	vm_map_entry_t		new_entry;
	struct vm_map_header	map_header;
	vm_map_offset_t		offset_in_mapping;

	if (target_map == VM_MAP_NULL)
		return KERN_INVALID_ARGUMENT;

	switch (inheritance) {
	case VM_INHERIT_NONE:
	case VM_INHERIT_COPY:
	case VM_INHERIT_SHARE:
		if (size != 0 && src_map != VM_MAP_NULL)
			break;
		/*FALL THRU*/
	default:
		return KERN_INVALID_ARGUMENT;
	}

	/*
	 * If the user is requesting that we return the address of the
	 * first byte of the data (rather than the base of the page),
	 * then we use different rounding semantics: specifically,
	 * we assume that (memory_address, size) describes a region
	 * all of whose pages we must cover, rather than a base to be truncated
	 * down and a size to be added to that base.  So we figure out
	 * the highest page that the requested region includes and make
	 * sure that the size will cover it.
	 *
 	 * The key example we're worried about it is of the form:
	 *
	 * 		memory_address = 0x1ff0, size = 0x20
	 *
	 * With the old semantics, we round down the memory_address to 0x1000
	 * and round up the size to 0x1000, resulting in our covering *only*
	 * page 0x1000.  With the new semantics, we'd realize that the region covers
	 * 0x1ff0-0x2010, and compute a size of 0x2000.  Thus, we cover both page
	 * 0x1000 and page 0x2000 in the region we remap.
	 */
	if ((flags & VM_FLAGS_RETURN_DATA_ADDR) != 0) {
		offset_in_mapping = memory_address - vm_map_trunc_page(memory_address, PAGE_MASK);
		size = vm_map_round_page(memory_address + size - vm_map_trunc_page(memory_address, PAGE_MASK), PAGE_MASK);
	} else {
		size = vm_map_round_page(size, PAGE_MASK);
	}
	if (size == 0) {
		return KERN_INVALID_ARGUMENT;
	}

	result = vm_map_remap_extract(src_map, memory_address,
				      size, copy, &map_header,
				      cur_protection,
				      max_protection,
				      inheritance,
				      target_map->hdr.entries_pageable,
				      src_map == target_map,
				      vmk_flags);

	if (result != KERN_SUCCESS) {
		return result;
	}

	/*
	 * Allocate/check a range of free virtual address
	 * space for the target
	 */
	*address = vm_map_trunc_page(*address,
				     VM_MAP_PAGE_MASK(target_map));
	vm_map_lock(target_map);
	result = vm_map_remap_range_allocate(target_map, address, size,
					     mask, flags, vmk_flags, tag,
					     &insp_entry);

	for (entry = map_header.links.next;
	     entry != (struct vm_map_entry *)&map_header.links;
	     entry = new_entry) {
		new_entry = entry->vme_next;
		_vm_map_store_entry_unlink(&map_header, entry);
		if (result == KERN_SUCCESS) {
			if (flags & VM_FLAGS_RESILIENT_CODESIGN) {
				/* no codesigning -> read-only access */
				assert(!entry->used_for_jit);
				entry->max_protection = VM_PROT_READ;
				entry->protection = VM_PROT_READ;
				entry->vme_resilient_codesign = TRUE;
			}
			entry->vme_start += *address;
			entry->vme_end += *address;
			assert(!entry->map_aligned);
			vm_map_store_entry_link(target_map, insp_entry, entry);
			insp_entry = entry;
		} else {
			if (!entry->is_sub_map) {
				vm_object_deallocate(VME_OBJECT(entry));
			} else {
				vm_map_deallocate(VME_SUBMAP(entry));
			}
			_vm_map_entry_dispose(&map_header, entry);
		}
	}

	if (flags & VM_FLAGS_RESILIENT_CODESIGN) {
		*cur_protection = VM_PROT_READ;
		*max_protection = VM_PROT_READ;
	}

	if( target_map->disable_vmentry_reuse == TRUE) {
		assert(!target_map->is_nested_map);
		if( target_map->highest_entry_end < insp_entry->vme_end ){
			target_map->highest_entry_end = insp_entry->vme_end;
		}
	}

	if (result == KERN_SUCCESS) {
		target_map->size += size;
		SAVE_HINT_MAP_WRITE(target_map, insp_entry);

	}
	vm_map_unlock(target_map);

	if (result == KERN_SUCCESS && target_map->wiring_required)
		result = vm_map_wire_kernel(target_map, *address,
				     *address + size, *cur_protection, VM_KERN_MEMORY_MLOCK,
				     TRUE);

	/*
	 * If requested, return the address of the data pointed to by the
	 * request, rather than the base of the resulting page.
	 */
	if ((flags & VM_FLAGS_RETURN_DATA_ADDR) != 0) {
		*address += offset_in_mapping;
	}

	return result;
}

/*
 *	Routine:	vm_map_remap_range_allocate
 *
 *	Description:
 *		Allocate a range in the specified virtual address map.
 *		returns the address and the map entry just before the allocated
 *		range
 *
 *	Map must be locked.
 */

static kern_return_t
vm_map_remap_range_allocate(
	vm_map_t		map,
	vm_map_address_t	*address,	/* IN/OUT */
	vm_map_size_t		size,
	vm_map_offset_t		mask,
	int			flags,
	__unused vm_map_kernel_flags_t	vmk_flags,
	__unused vm_tag_t       tag,
	vm_map_entry_t		*map_entry)	/* OUT */
{
	vm_map_entry_t	entry;
	vm_map_offset_t	start;
	vm_map_offset_t	end;
	kern_return_t	kr;
	vm_map_entry_t		hole_entry;

StartAgain: ;

	start = *address;

	if (flags & VM_FLAGS_ANYWHERE)
	{
		if (flags & VM_FLAGS_RANDOM_ADDR)
		{
			/*
			 * Get a random start address.
			 */
			kr = vm_map_random_address_for_size(map, address, size);
			if (kr != KERN_SUCCESS) {
				return(kr);
			}
			start = *address;
		}

		/*
		 *	Calculate the first possible address.
		 */

		if (start < map->min_offset)
			start = map->min_offset;
		if (start > map->max_offset)
			return(KERN_NO_SPACE);

		/*
		 *	Look for the first possible address;
		 *	if there's already something at this
		 *	address, we have to start after it.
		 */

		if( map->disable_vmentry_reuse == TRUE) {
			VM_MAP_HIGHEST_ENTRY(map, entry, start);
		} else {

			if (map->holelistenabled) {
				hole_entry = (vm_map_entry_t)map->holes_list;

				if (hole_entry == NULL) {
					/*
					 * No more space in the map?
					 */
					return(KERN_NO_SPACE);
				} else {

					boolean_t found_hole = FALSE;

					do {
						if (hole_entry->vme_start >= start) {
							start = hole_entry->vme_start;
							found_hole = TRUE;
							break;
						}

						if (hole_entry->vme_end > start) {
							found_hole = TRUE;
							break;
						}
						hole_entry = hole_entry->vme_next;

					} while (hole_entry != (vm_map_entry_t) map->holes_list);

					if (found_hole == FALSE) {
						return (KERN_NO_SPACE);
					}

					entry = hole_entry;
				}
			} else {
				assert(first_free_is_valid(map));
				if (start == map->min_offset) {
					if ((entry = map->first_free) != vm_map_to_entry(map))
						start = entry->vme_end;
				} else {
					vm_map_entry_t	tmp_entry;
					if (vm_map_lookup_entry(map, start, &tmp_entry))
						start = tmp_entry->vme_end;
					entry = tmp_entry;
				}
			}
			start = vm_map_round_page(start,
						  VM_MAP_PAGE_MASK(map));
		}

		/*
		 *	In any case, the "entry" always precedes
		 *	the proposed new region throughout the
		 *	loop:
		 */

		while (TRUE) {
			vm_map_entry_t	next;

			/*
			 *	Find the end of the proposed new region.
			 *	Be sure we didn't go beyond the end, or
			 *	wrap around the address.
			 */

			end = ((start + mask) & ~mask);
			end = vm_map_round_page(end,
						VM_MAP_PAGE_MASK(map));
			if (end < start)
				return(KERN_NO_SPACE);
			start = end;
			end += size;

			if ((end > map->max_offset) || (end < start)) {
				if (map->wait_for_space) {
					if (size <= (map->max_offset -
						     map->min_offset)) {
						assert_wait((event_t) map, THREAD_INTERRUPTIBLE);
						vm_map_unlock(map);
						thread_block(THREAD_CONTINUE_NULL);
						vm_map_lock(map);
						goto StartAgain;
					}
				}

				return(KERN_NO_SPACE);
			}

			next = entry->vme_next;

			if (map->holelistenabled) {
				if (entry->vme_end >= end)
					break;
			} else {
				/*
			 	 *	If there are no more entries, we must win.
				 *
				 *	OR
				 *
				 *	If there is another entry, it must be
				 *	after the end of the potential new region.
				 */

				if (next == vm_map_to_entry(map))
					break;

				if (next->vme_start >= end)
					break;
			}

			/*
			 *	Didn't fit -- move to the next entry.
			 */

			entry = next;

			if (map->holelistenabled) {
				if (entry == (vm_map_entry_t) map->holes_list) {
					/*
					 * Wrapped around
					 */
					return(KERN_NO_SPACE);
				}
				start = entry->vme_start;
			} else {
				start = entry->vme_end;
			}
		}

		if (map->holelistenabled) {

			if (vm_map_lookup_entry(map, entry->vme_start, &entry)) {
				panic("Found an existing entry (%p) instead of potential hole at address: 0x%llx.\n", entry, (unsigned long long)entry->vme_start);
			}
		}

		*address = start;

	} else {
		vm_map_entry_t		temp_entry;

		/*
		 *	Verify that:
		 *		the address doesn't itself violate
		 *		the mask requirement.
		 */

		if ((start & mask) != 0)
			return(KERN_NO_SPACE);


		/*
		 *	...	the address is within bounds
		 */

		end = start + size;

		if ((start < map->min_offset) ||
		    (end > map->max_offset) ||
		    (start >= end)) {
			return(KERN_INVALID_ADDRESS);
		}

		/*
		 * If we're asked to overwrite whatever was mapped in that
		 * range, first deallocate that range.
		 */
		if (flags & VM_FLAGS_OVERWRITE) {
			vm_map_t zap_map;

			/*
			 * We use a "zap_map" to avoid having to unlock
			 * the "map" in vm_map_delete(), which would compromise
			 * the atomicity of the "deallocate" and then "remap"
			 * combination.
			 */
			zap_map = vm_map_create(PMAP_NULL,
						start,
						end,
						map->hdr.entries_pageable);
			if (zap_map == VM_MAP_NULL) {
				return KERN_RESOURCE_SHORTAGE;
			}
			vm_map_set_page_shift(zap_map, VM_MAP_PAGE_SHIFT(map));
			vm_map_disable_hole_optimization(zap_map);

			kr = vm_map_delete(map, start, end,
					   (VM_MAP_REMOVE_SAVE_ENTRIES |
					    VM_MAP_REMOVE_NO_MAP_ALIGN),
					   zap_map);
			if (kr == KERN_SUCCESS) {
				vm_map_destroy(zap_map,
					       VM_MAP_REMOVE_NO_PMAP_CLEANUP);
				zap_map = VM_MAP_NULL;
			}
		}

		/*
		 *	...	the starting address isn't allocated
		 */

		if (vm_map_lookup_entry(map, start, &temp_entry))
			return(KERN_NO_SPACE);

		entry = temp_entry;

		/*
		 *	...	the next region doesn't overlap the
		 *		end point.
		 */

		if ((entry->vme_next != vm_map_to_entry(map)) &&
		    (entry->vme_next->vme_start < end))
			return(KERN_NO_SPACE);
	}
	*map_entry = entry;
	return(KERN_SUCCESS);
}

/*
 *	vm_map_switch:
 *
 *	Set the address map for the current thread to the specified map
 */

vm_map_t
vm_map_switch(
	vm_map_t	map)
{
	int		mycpu;
	thread_t	thread = current_thread();
	vm_map_t	oldmap = thread->map;

	mp_disable_preemption();
	mycpu = cpu_number();

	/*
	 *	Deactivate the current map and activate the requested map
	 */
	PMAP_SWITCH_USER(thread, map, mycpu);

	mp_enable_preemption();
	return(oldmap);
}


/*
 *	Routine:	vm_map_write_user
 *
 *	Description:
 *		Copy out data from a kernel space into space in the
 *		destination map. The space must already exist in the
 *		destination map.
 *		NOTE:  This routine should only be called by threads
 *		which can block on a page fault. i.e. kernel mode user
 *		threads.
 *
 */
kern_return_t
vm_map_write_user(
	vm_map_t		map,
	void			*src_p,
	vm_map_address_t	dst_addr,
	vm_size_t		size)
{
	kern_return_t	kr = KERN_SUCCESS;

	if(current_map() == map) {
		if (copyout(src_p, dst_addr, size)) {
			kr = KERN_INVALID_ADDRESS;
		}
	} else {
		vm_map_t	oldmap;

		/* take on the identity of the target map while doing */
		/* the transfer */

		vm_map_reference(map);
		oldmap = vm_map_switch(map);
		if (copyout(src_p, dst_addr, size)) {
			kr = KERN_INVALID_ADDRESS;
		}
		vm_map_switch(oldmap);
		vm_map_deallocate(map);
	}
	return kr;
}

/*
 *	Routine:	vm_map_read_user
 *
 *	Description:
 *		Copy in data from a user space source map into the
 *		kernel map. The space must already exist in the
 *		kernel map.
 *		NOTE:  This routine should only be called by threads
 *		which can block on a page fault. i.e. kernel mode user
 *		threads.
 *
 */
kern_return_t
vm_map_read_user(
	vm_map_t		map,
	vm_map_address_t	src_addr,
	void			*dst_p,
	vm_size_t		size)
{
	kern_return_t	kr = KERN_SUCCESS;

	if(current_map() == map) {
		if (copyin(src_addr, dst_p, size)) {
			kr = KERN_INVALID_ADDRESS;
		}
	} else {
		vm_map_t	oldmap;

		/* take on the identity of the target map while doing */
		/* the transfer */

		vm_map_reference(map);
		oldmap = vm_map_switch(map);
		if (copyin(src_addr, dst_p, size)) {
			kr = KERN_INVALID_ADDRESS;
		}
		vm_map_switch(oldmap);
		vm_map_deallocate(map);
	}
	return kr;
}


/*
 *	vm_map_check_protection:
 *
 *	Assert that the target map allows the specified
 *	privilege on the entire address region given.
 *	The entire region must be allocated.
 */
boolean_t
vm_map_check_protection(vm_map_t map, vm_map_offset_t start,
			vm_map_offset_t end, vm_prot_t protection)
{
	vm_map_entry_t entry;
	vm_map_entry_t tmp_entry;

	vm_map_lock(map);

	if (start < vm_map_min(map) || end > vm_map_max(map) || start > end)
	{
		vm_map_unlock(map);
		return (FALSE);
	}

	if (!vm_map_lookup_entry(map, start, &tmp_entry)) {
		vm_map_unlock(map);
		return(FALSE);
	}

	entry = tmp_entry;

	while (start < end) {
		if (entry == vm_map_to_entry(map)) {
			vm_map_unlock(map);
			return(FALSE);
		}

		/*
		 *	No holes allowed!
		 */

		if (start < entry->vme_start) {
			vm_map_unlock(map);
			return(FALSE);
		}

		/*
		 * Check protection associated with entry.
		 */

		if ((entry->protection & protection) != protection) {
			vm_map_unlock(map);
			return(FALSE);
		}

		/* go to next entry */

		start = entry->vme_end;
		entry = entry->vme_next;
	}
	vm_map_unlock(map);
	return(TRUE);
}

kern_return_t
vm_map_purgable_control(
	vm_map_t		map,
	vm_map_offset_t		address,
	vm_purgable_t		control,
	int			*state)
{
	vm_map_entry_t		entry;
	vm_object_t		object;
	kern_return_t		kr;
	boolean_t		was_nonvolatile;

	/*
	 * Vet all the input parameters and current type and state of the
	 * underlaying object.  Return with an error if anything is amiss.
	 */
	if (map == VM_MAP_NULL)
		return(KERN_INVALID_ARGUMENT);

	if (control != VM_PURGABLE_SET_STATE &&
	    control != VM_PURGABLE_GET_STATE &&
	    control != VM_PURGABLE_PURGE_ALL &&
	    control != VM_PURGABLE_SET_STATE_FROM_KERNEL)
		return(KERN_INVALID_ARGUMENT);

	if (control == VM_PURGABLE_PURGE_ALL) {
		vm_purgeable_object_purge_all();
		return KERN_SUCCESS;
	}

	if ((control == VM_PURGABLE_SET_STATE ||
	     control == VM_PURGABLE_SET_STATE_FROM_KERNEL) &&
	    (((*state & ~(VM_PURGABLE_ALL_MASKS)) != 0) ||
	     ((*state & VM_PURGABLE_STATE_MASK) > VM_PURGABLE_STATE_MASK)))
		return(KERN_INVALID_ARGUMENT);

	vm_map_lock_read(map);

	if (!vm_map_lookup_entry(map, address, &entry) || entry->is_sub_map) {

		/*
		 * Must pass a valid non-submap address.
		 */
		vm_map_unlock_read(map);
		return(KERN_INVALID_ADDRESS);
	}

	if ((entry->protection & VM_PROT_WRITE) == 0) {
		/*
		 * Can't apply purgable controls to something you can't write.
		 */
		vm_map_unlock_read(map);
		return(KERN_PROTECTION_FAILURE);
	}

	object = VME_OBJECT(entry);
	if (object == VM_OBJECT_NULL ||
	    object->purgable == VM_PURGABLE_DENY) {
		/*
		 * Object must already be present and be purgeable.
		 */
		vm_map_unlock_read(map);
		return KERN_INVALID_ARGUMENT;
	}

	vm_object_lock(object);

#if 00
	if (VME_OFFSET(entry) != 0 ||
	    entry->vme_end - entry->vme_start != object->vo_size) {
		/*
		 * Can only apply purgable controls to the whole (existing)
		 * object at once.
		 */
		vm_map_unlock_read(map);
		vm_object_unlock(object);
		return KERN_INVALID_ARGUMENT;
	}
#endif

	assert(!entry->is_sub_map);
	assert(!entry->use_pmap); /* purgeable has its own accounting */

	vm_map_unlock_read(map);

	was_nonvolatile = (object->purgable == VM_PURGABLE_NONVOLATILE);

	kr = vm_object_purgable_control(object, control, state);

	if (was_nonvolatile &&
	    object->purgable != VM_PURGABLE_NONVOLATILE &&
	    map->pmap == kernel_pmap) {
#if DEBUG
		object->vo_purgeable_volatilizer = kernel_task;
#endif /* DEBUG */
	}

	vm_object_unlock(object);

	return kr;
}

kern_return_t
vm_map_page_query_internal(
	vm_map_t	target_map,
	vm_map_offset_t	offset,
	int		*disposition,
	int		*ref_count)
{
	kern_return_t			kr;
	vm_page_info_basic_data_t	info;
	mach_msg_type_number_t		count;

	count = VM_PAGE_INFO_BASIC_COUNT;
	kr = vm_map_page_info(target_map,
			      offset,
			      VM_PAGE_INFO_BASIC,
			      (vm_page_info_t) &info,
			      &count);
	if (kr == KERN_SUCCESS) {
		*disposition = info.disposition;
		*ref_count = info.ref_count;
	} else {
		*disposition = 0;
		*ref_count = 0;
	}

	return kr;
}

kern_return_t
vm_map_page_info(
	vm_map_t		map,
	vm_map_offset_t		offset,
	vm_page_info_flavor_t	flavor,
	vm_page_info_t		info,
	mach_msg_type_number_t	*count)
{
	return (vm_map_page_range_info_internal(map,
				       offset, /* start of range */
				       (offset + 1), /* this will get rounded in the call to the page boundary */
				       flavor,
				       info,
				       count));
}

kern_return_t
vm_map_page_range_info_internal(
	vm_map_t		map,
	vm_map_offset_t		start_offset,
	vm_map_offset_t		end_offset,
	vm_page_info_flavor_t	flavor,
	vm_page_info_t		info,
	mach_msg_type_number_t	*count)
{
	vm_map_entry_t		map_entry = VM_MAP_ENTRY_NULL;
	vm_object_t		object = VM_OBJECT_NULL, curr_object = VM_OBJECT_NULL;
	vm_page_t		m = VM_PAGE_NULL;
	kern_return_t		retval = KERN_SUCCESS;
	int			disposition = 0;
	int 			ref_count = 0;
	int			depth = 0, info_idx = 0;
	vm_page_info_basic_t	basic_info = 0;
	vm_map_offset_t		offset_in_page = 0, offset_in_object = 0, curr_offset_in_object = 0;
	vm_map_offset_t		start = 0, end = 0, curr_s_offset = 0, curr_e_offset = 0;
	boolean_t		do_region_footprint;

	switch (flavor) {
	case VM_PAGE_INFO_BASIC:
		if (*count != VM_PAGE_INFO_BASIC_COUNT) {
			/*
			 * The "vm_page_info_basic_data" structure was not
			 * properly padded, so allow the size to be off by
			 * one to maintain backwards binary compatibility...
			 */
			if (*count != VM_PAGE_INFO_BASIC_COUNT - 1)
				return KERN_INVALID_ARGUMENT;
		}
		break;
	default:
		return KERN_INVALID_ARGUMENT;
	}

	do_region_footprint = task_self_region_footprint();
	disposition = 0;
	ref_count = 0;
	depth = 0;
	info_idx = 0; /* Tracks the next index within the info structure to be filled.*/
	retval = KERN_SUCCESS;

	offset_in_page = start_offset & PAGE_MASK;
	start = vm_map_trunc_page(start_offset, PAGE_MASK);
	end = vm_map_round_page(end_offset, PAGE_MASK);

	assert ((end - start) <= MAX_PAGE_RANGE_QUERY);

	vm_map_lock_read(map);

	for (curr_s_offset = start; curr_s_offset < end;) {
		/*
		 * New lookup needs reset of these variables.
		 */
		curr_object = object = VM_OBJECT_NULL;
		offset_in_object = 0;
		ref_count = 0;
		depth = 0;

		if (do_region_footprint &&
		    curr_s_offset >= vm_map_last_entry(map)->vme_end) {
			ledger_amount_t nonvol_compressed;

			/*
			 * Request for "footprint" info about a page beyond
			 * the end of address space: this must be for
			 * the fake region vm_map_region_recurse_64()
			 * reported to account for non-volatile purgeable
			 * memory owned by this task.
			 */
			disposition = 0;
			nonvol_compressed = 0;
			ledger_get_balance(
				map->pmap->ledger,
				task_ledgers.purgeable_nonvolatile_compressed,
				&nonvol_compressed);
			if (curr_s_offset - vm_map_last_entry(map)->vme_end <=
			    (unsigned) nonvol_compressed) {
				/*
				 * We haven't reported all the "non-volatile
				 * compressed" pages yet, so report this fake
				 * page as "compressed".
				 */
				disposition |= VM_PAGE_QUERY_PAGE_PAGED_OUT;
			} else {
				/*
				 * We've reported all the non-volatile
				 * compressed page but not all the non-volatile
				 * pages , so report this fake page as
				 * "resident dirty".
				 */
				disposition |= VM_PAGE_QUERY_PAGE_PRESENT;
				disposition |= VM_PAGE_QUERY_PAGE_DIRTY;
				disposition |= VM_PAGE_QUERY_PAGE_REF;
			}
			switch (flavor) {
			case VM_PAGE_INFO_BASIC:
				basic_info = (vm_page_info_basic_t) (((uintptr_t) info) + (info_idx * sizeof(struct vm_page_info_basic)));
				basic_info->disposition = disposition;
				basic_info->ref_count = 1;
				basic_info->object_id = INFO_MAKE_FAKE_OBJECT_ID(map, task_ledgers.purgeable_nonvolatile);
				basic_info->offset = 0;
				basic_info->depth = 0;

				info_idx++;
				break;
			}
			curr_s_offset += PAGE_SIZE;
			continue;
		}

		/*
		 * First, find the map entry covering "curr_s_offset", going down
		 * submaps if necessary.
		 */
		if (!vm_map_lookup_entry(map, curr_s_offset, &map_entry)) {
			/* no entry -> no object -> no page */

			if (curr_s_offset < vm_map_min(map)) {
				/*
				 * Illegal address that falls below map min.
				 */
				curr_e_offset = MIN(end, vm_map_min(map));

			} else if (curr_s_offset >= vm_map_max(map)) {
				/*
				 * Illegal address that falls on/after map max.
				 */
				curr_e_offset = end;

			} else if (map_entry == vm_map_to_entry(map)) {
				/*
				 * Hit a hole.
				 */
				if (map_entry->vme_next == vm_map_to_entry(map)) {
					/*
					 * Empty map.
					 */
					curr_e_offset = MIN(map->max_offset, end);
				} else {
					/*
				 	 * Hole at start of the map.
				 	 */
					curr_e_offset = MIN(map_entry->vme_next->vme_start, end);
				}
			} else {
				if (map_entry->vme_next == vm_map_to_entry(map)) {
					/*
					 * Hole at the end of the map.
					 */
					curr_e_offset = MIN(map->max_offset, end);
				} else {
					curr_e_offset = MIN(map_entry->vme_next->vme_start, end);
				}
			}

			assert(curr_e_offset >= curr_s_offset);

			uint64_t num_pages = (curr_e_offset - curr_s_offset) >> PAGE_SHIFT;

			void *info_ptr = (void*) (((uintptr_t) info) + (info_idx * sizeof(struct vm_page_info_basic)));

			bzero(info_ptr, num_pages * sizeof(struct vm_page_info_basic));

			curr_s_offset = curr_e_offset;

			info_idx += num_pages;

			continue;
		}

		/* compute offset from this map entry's start */
		offset_in_object = curr_s_offset - map_entry->vme_start;

		/* compute offset into this map entry's object (or submap) */
		offset_in_object += VME_OFFSET(map_entry);

		if (map_entry->is_sub_map) {
			vm_map_t sub_map = VM_MAP_NULL;
			vm_page_info_t submap_info = 0;
			vm_map_offset_t submap_s_offset = 0, submap_e_offset = 0, range_len = 0;

			range_len = MIN(map_entry->vme_end, end) - curr_s_offset;

			submap_s_offset = offset_in_object;
			submap_e_offset = submap_s_offset + range_len;

			sub_map = VME_SUBMAP(map_entry);

			vm_map_reference(sub_map);
			vm_map_unlock_read(map);

			submap_info = (vm_page_info_t) (((uintptr_t) info) + (info_idx * sizeof(struct vm_page_info_basic)));

			retval = vm_map_page_range_info_internal(sub_map,
					      submap_s_offset,
					      submap_e_offset,
					      VM_PAGE_INFO_BASIC,
					      (vm_page_info_t) submap_info,
					      count);

			assert(retval == KERN_SUCCESS);

			vm_map_lock_read(map);
			vm_map_deallocate(sub_map);

			/* Move the "info" index by the number of pages we inspected.*/
			info_idx += range_len >> PAGE_SHIFT;

			/* Move our current offset by the size of the range we inspected.*/
			curr_s_offset += range_len;

			continue;
		}

		object = VME_OBJECT(map_entry);
		if (object == VM_OBJECT_NULL) {

			/*
			 * We don't have an object here and, hence,
			 * no pages to inspect. We'll fill up the
			 * info structure appropriately.
			 */

			curr_e_offset = MIN(map_entry->vme_end, end);

			uint64_t num_pages = (curr_e_offset - curr_s_offset) >> PAGE_SHIFT;

			void *info_ptr = (void*) (((uintptr_t) info) + (info_idx * sizeof(struct vm_page_info_basic)));

			bzero(info_ptr, num_pages * sizeof(struct vm_page_info_basic));

			curr_s_offset = curr_e_offset;

			info_idx += num_pages;

			continue;
		}

		if (do_region_footprint) {
			int pmap_disp;

			disposition = 0;
			pmap_disp = 0;
			pmap_query_page_info(map->pmap, curr_s_offset, &pmap_disp);
			if (map_entry->iokit_acct &&
			    object->internal &&
			    object->purgable == VM_PURGABLE_DENY) {
				/*
				 * Non-purgeable IOKit memory: phys_footprint
				 * includes the entire virtual mapping.
				 */
				assertf(!map_entry->use_pmap, "offset 0x%llx map_entry %p", (uint64_t) curr_s_offset, map_entry);
				disposition |= VM_PAGE_QUERY_PAGE_PRESENT;
				disposition |= VM_PAGE_QUERY_PAGE_DIRTY;
			} else if (pmap_disp & (PMAP_QUERY_PAGE_ALTACCT |
						PMAP_QUERY_PAGE_COMPRESSED_ALTACCT)) {
				/* alternate accounting */
				assertf(!map_entry->use_pmap, "offset 0x%llx map_entry %p", (uint64_t) curr_s_offset, map_entry);
				pmap_disp = 0;
			} else {
				if (pmap_disp & PMAP_QUERY_PAGE_PRESENT) {
					assertf(map_entry->use_pmap, "offset 0x%llx map_entry %p", (uint64_t) curr_s_offset, map_entry);
					disposition |= VM_PAGE_QUERY_PAGE_PRESENT;
					disposition |= VM_PAGE_QUERY_PAGE_REF;
					if (pmap_disp & PMAP_QUERY_PAGE_INTERNAL) {
						disposition |= VM_PAGE_QUERY_PAGE_DIRTY;
					} else {
						disposition |= VM_PAGE_QUERY_PAGE_EXTERNAL;
					}
				} else if (pmap_disp & PMAP_QUERY_PAGE_COMPRESSED) {
					assertf(map_entry->use_pmap, "offset 0x%llx map_entry %p", (uint64_t) curr_s_offset, map_entry);
					disposition |= VM_PAGE_QUERY_PAGE_PAGED_OUT;
				}
			}
			switch (flavor) {
			case VM_PAGE_INFO_BASIC:
				basic_info = (vm_page_info_basic_t) (((uintptr_t) info) + (info_idx * sizeof(struct vm_page_info_basic)));
				basic_info->disposition = disposition;
				basic_info->ref_count = 1;
				basic_info->object_id = INFO_MAKE_FAKE_OBJECT_ID(map, task_ledgers.purgeable_nonvolatile);
				basic_info->offset = 0;
				basic_info->depth = 0;

				info_idx++;
				break;
			}
			curr_s_offset += PAGE_SIZE;
			continue;
		}

		vm_object_reference(object);
		/*
		 * Shared mode -- so we can allow other readers
		 * to grab the lock too.
		 */
		vm_object_lock_shared(object);

		curr_e_offset = MIN(map_entry->vme_end, end);

		vm_map_unlock_read(map);

		map_entry = NULL; /* map is unlocked, the entry is no longer valid. */

		curr_object = object;

		for (; curr_s_offset < curr_e_offset;) {

			if (object == curr_object) {
				ref_count = curr_object->ref_count - 1; /* account for our object reference above. */
			} else {
				ref_count = curr_object->ref_count;
			}

			curr_offset_in_object = offset_in_object;

			for (;;) {
				m = vm_page_lookup(curr_object, curr_offset_in_object);

				if (m != VM_PAGE_NULL) {

					disposition |= VM_PAGE_QUERY_PAGE_PRESENT;
					break;

				} else {
					if (curr_object->internal &&
					    curr_object->alive &&
					    !curr_object->terminating &&
					    curr_object->pager_ready) {

						if (VM_COMPRESSOR_PAGER_STATE_GET(curr_object, curr_offset_in_object)
						    == VM_EXTERNAL_STATE_EXISTS) {
							/* the pager has that page */
							disposition |= VM_PAGE_QUERY_PAGE_PAGED_OUT;
							break;
						}
					}
		
					/*
					 * Go down the VM object shadow chain until we find the page
					 * we're looking for.
					 */

					if (curr_object->shadow != VM_OBJECT_NULL) {
						vm_object_t shadow = VM_OBJECT_NULL;

						curr_offset_in_object += curr_object->vo_shadow_offset;
						shadow = curr_object->shadow;

						vm_object_lock_shared(shadow);
						vm_object_unlock(curr_object);

						curr_object = shadow;
						depth++;
						continue;
					} else {

						break;
					}
				}
			}

			/* The ref_count is not strictly accurate, it measures the number   */
			/* of entities holding a ref on the object, they may not be mapping */
			/* the object or may not be mapping the section holding the         */
			/* target page but its still a ball park number and though an over- */
			/* count, it picks up the copy-on-write cases                       */

			/* We could also get a picture of page sharing from pmap_attributes */
			/* but this would under count as only faulted-in mappings would     */
			/* show up.							    */

			if ((curr_object == object) && curr_object->shadow)
				disposition |= VM_PAGE_QUERY_PAGE_COPIED;

			if (! curr_object->internal)
				disposition |= VM_PAGE_QUERY_PAGE_EXTERNAL;

			if (m != VM_PAGE_NULL) {

				if (m->fictitious) {

					disposition |= VM_PAGE_QUERY_PAGE_FICTITIOUS;

				} else {
					if (m->dirty || pmap_is_modified(VM_PAGE_GET_PHYS_PAGE(m)))
						disposition |= VM_PAGE_QUERY_PAGE_DIRTY;

					if (m->reference || pmap_is_referenced(VM_PAGE_GET_PHYS_PAGE(m)))
						disposition |= VM_PAGE_QUERY_PAGE_REF;

					if (m->vm_page_q_state == VM_PAGE_ON_SPECULATIVE_Q)
						disposition |= VM_PAGE_QUERY_PAGE_SPECULATIVE;

					if (m->cs_validated)
						disposition |= VM_PAGE_QUERY_PAGE_CS_VALIDATED;
					if (m->cs_tainted)
						disposition |= VM_PAGE_QUERY_PAGE_CS_TAINTED;
					if (m->cs_nx)
						disposition |= VM_PAGE_QUERY_PAGE_CS_NX;
				}
			}

			switch (flavor) {
			case VM_PAGE_INFO_BASIC:
				basic_info = (vm_page_info_basic_t) (((uintptr_t) info) + (info_idx * sizeof(struct vm_page_info_basic)));
				basic_info->disposition = disposition;
				basic_info->ref_count = ref_count;
				basic_info->object_id = (vm_object_id_t) (uintptr_t)
					VM_KERNEL_ADDRPERM(curr_object);
				basic_info->offset =
					(memory_object_offset_t) curr_offset_in_object + offset_in_page;
				basic_info->depth = depth;

				info_idx++;
				break;
			}

			disposition = 0;
			offset_in_page = 0; // This doesn't really make sense for any offset other than the starting offset.

			/*
			 * Move to next offset in the range and in our object.
			 */
			curr_s_offset += PAGE_SIZE;	
			offset_in_object += PAGE_SIZE;
			curr_offset_in_object = offset_in_object;

			if (curr_object != object) {

				vm_object_unlock(curr_object);

				curr_object = object;

				vm_object_lock_shared(curr_object);
			} else {

				vm_object_lock_yield_shared(curr_object);
			}
		}

		vm_object_unlock(curr_object);
		vm_object_deallocate(curr_object);

		vm_map_lock_read(map);
	}

	vm_map_unlock_read(map);
	return retval;
}

/*
 *	vm_map_msync
 *
 *	Synchronises the memory range specified with its backing store
 *	image by either flushing or cleaning the contents to the appropriate
 *	memory manager engaging in a memory object synchronize dialog with
 *	the manager.  The client doesn't return until the manager issues
 *	m_o_s_completed message.  MIG Magically converts user task parameter
 *	to the task's address map.
 *
 *	interpretation of sync_flags
 *	VM_SYNC_INVALIDATE	- discard pages, only return precious
 *				  pages to manager.
 *
 *	VM_SYNC_INVALIDATE & (VM_SYNC_SYNCHRONOUS | VM_SYNC_ASYNCHRONOUS)
 *				- discard pages, write dirty or precious
 *				  pages back to memory manager.
 *
 *	VM_SYNC_SYNCHRONOUS | VM_SYNC_ASYNCHRONOUS
 *				- write dirty or precious pages back to
 *				  the memory manager.
 *
 *	VM_SYNC_CONTIGUOUS	- does everything normally, but if there
 *				  is a hole in the region, and we would
 *				  have returned KERN_SUCCESS, return
 *				  KERN_INVALID_ADDRESS instead.
 *
 *	NOTE
 *	The memory object attributes have not yet been implemented, this
 *	function will have to deal with the invalidate attribute
 *
 *	RETURNS
 *	KERN_INVALID_TASK		Bad task parameter
 *	KERN_INVALID_ARGUMENT		both sync and async were specified.
 *	KERN_SUCCESS			The usual.
 *	KERN_INVALID_ADDRESS		There was a hole in the region.
 */

kern_return_t
vm_map_msync(
	vm_map_t		map,
	vm_map_address_t	address,
	vm_map_size_t		size,
	vm_sync_t		sync_flags)
{
	vm_map_entry_t		entry;
	vm_map_size_t		amount_left;
	vm_object_offset_t	offset;
	boolean_t		do_sync_req;
	boolean_t		had_hole = FALSE;
	vm_map_offset_t		pmap_offset;

	if ((sync_flags & VM_SYNC_ASYNCHRONOUS) &&
	    (sync_flags & VM_SYNC_SYNCHRONOUS))
		return(KERN_INVALID_ARGUMENT);

	/*
	 * align address and size on page boundaries
	 */
	size = (vm_map_round_page(address + size,
				  VM_MAP_PAGE_MASK(map)) -
		vm_map_trunc_page(address,
				  VM_MAP_PAGE_MASK(map)));
	address = vm_map_trunc_page(address,
				    VM_MAP_PAGE_MASK(map));

        if (map == VM_MAP_NULL)
                return(KERN_INVALID_TASK);

	if (size == 0)
		return(KERN_SUCCESS);

	amount_left = size;

	while (amount_left > 0) {
		vm_object_size_t	flush_size;
		vm_object_t		object;

		vm_map_lock(map);
		if (!vm_map_lookup_entry(map,
					 address,
					 &entry)) {

			vm_map_size_t	skip;

			/*
			 * hole in the address map.
			 */
			had_hole = TRUE;

			if (sync_flags & VM_SYNC_KILLPAGES) {
				/*
				 * For VM_SYNC_KILLPAGES, there should be
				 * no holes in the range, since we couldn't
				 * prevent someone else from allocating in
				 * that hole and we wouldn't want to "kill"
				 * their pages.
				 */
				vm_map_unlock(map);
				break;
			}

			/*
			 * Check for empty map.
			 */
			if (entry == vm_map_to_entry(map) &&
			    entry->vme_next == entry) {
				vm_map_unlock(map);
				break;
			}
			/*
			 * Check that we don't wrap and that
			 * we have at least one real map entry.
			 */
			if ((map->hdr.nentries == 0) ||
			    (entry->vme_next->vme_start < address)) {
				vm_map_unlock(map);
				break;
			}
			/*
			 * Move up to the next entry if needed
			 */
			skip = (entry->vme_next->vme_start - address);
			if (skip >= amount_left)
				amount_left = 0;
			else
				amount_left -= skip;
			address = entry->vme_next->vme_start;
			vm_map_unlock(map);
			continue;
		}

		offset = address - entry->vme_start;
		pmap_offset = address;

		/*
		 * do we have more to flush than is contained in this
		 * entry ?
		 */
		if (amount_left + entry->vme_start + offset > entry->vme_end) {
			flush_size = entry->vme_end -
				(entry->vme_start + offset);
		} else {
			flush_size = amount_left;
		}
		amount_left -= flush_size;
		address += flush_size;

		if (entry->is_sub_map == TRUE) {
			vm_map_t	local_map;
			vm_map_offset_t	local_offset;

			local_map = VME_SUBMAP(entry);
			local_offset = VME_OFFSET(entry);
			vm_map_unlock(map);
			if (vm_map_msync(
				    local_map,
				    local_offset,
				    flush_size,
				    sync_flags) == KERN_INVALID_ADDRESS) {
				had_hole = TRUE;
			}
			continue;
		}
		object = VME_OBJECT(entry);

		/*
		 * We can't sync this object if the object has not been
		 * created yet
		 */
		if (object == VM_OBJECT_NULL) {
			vm_map_unlock(map);
			continue;
		}
		offset += VME_OFFSET(entry);

                vm_object_lock(object);

		if (sync_flags & (VM_SYNC_KILLPAGES | VM_SYNC_DEACTIVATE)) {
		        int kill_pages = 0;
			boolean_t reusable_pages = FALSE;

			if (sync_flags & VM_SYNC_KILLPAGES) {
			        if (((object->ref_count == 1) ||
				     ((object->copy_strategy !=
				       MEMORY_OBJECT_COPY_SYMMETRIC) &&
				      (object->copy == VM_OBJECT_NULL))) &&
				    (object->shadow == VM_OBJECT_NULL)) {
					if (object->ref_count != 1) {
						vm_page_stats_reusable.free_shared++;
					}
				        kill_pages = 1;
				} else {
				        kill_pages = -1;
				}
			}
			if (kill_pages != -1)
			        vm_object_deactivate_pages(
					object,
					offset,
					(vm_object_size_t) flush_size,
					kill_pages,
					reusable_pages,
					map->pmap,
					pmap_offset);
			vm_object_unlock(object);
			vm_map_unlock(map);
			continue;
		}
		/*
		 * We can't sync this object if there isn't a pager.
		 * Don't bother to sync internal objects, since there can't
		 * be any "permanent" storage for these objects anyway.
		 */
		if ((object->pager == MEMORY_OBJECT_NULL) ||
		    (object->internal) || (object->private)) {
			vm_object_unlock(object);
			vm_map_unlock(map);
			continue;
		}
		/*
		 * keep reference on the object until syncing is done
		 */
		vm_object_reference_locked(object);
		vm_object_unlock(object);

		vm_map_unlock(map);

		do_sync_req = vm_object_sync(object,
					     offset,
					     flush_size,
					     sync_flags & VM_SYNC_INVALIDATE,
					     ((sync_flags & VM_SYNC_SYNCHRONOUS) ||
					      (sync_flags & VM_SYNC_ASYNCHRONOUS)),
					     sync_flags & VM_SYNC_SYNCHRONOUS);

		if ((sync_flags & VM_SYNC_INVALIDATE) && object->resident_page_count == 0) {
		        /*
			 * clear out the clustering and read-ahead hints
			 */
		        vm_object_lock(object);

			object->pages_created = 0;
			object->pages_used = 0;
			object->sequential = 0;
			object->last_alloc = 0;

			vm_object_unlock(object);
		}
		vm_object_deallocate(object);
	} /* while */

	/* for proper msync() behaviour */
	if (had_hole == TRUE && (sync_flags & VM_SYNC_CONTIGUOUS))
		return(KERN_INVALID_ADDRESS);

	return(KERN_SUCCESS);
}/* vm_msync */

/*
 *	Routine:	convert_port_entry_to_map
 *	Purpose:
 *		Convert from a port specifying an entry or a task
 *		to a map. Doesn't consume the port ref; produces a map ref,
 *		which may be null.  Unlike convert_port_to_map, the
 *		port may be task or a named entry backed.
 *	Conditions:
 *		Nothing locked.
 */


vm_map_t
convert_port_entry_to_map(
	ipc_port_t	port)
{
	vm_map_t map;
	vm_named_entry_t	named_entry;
	uint32_t	try_failed_count = 0;

	if(IP_VALID(port) && (ip_kotype(port) == IKOT_NAMED_ENTRY)) {
		while(TRUE) {
			ip_lock(port);
			if(ip_active(port) && (ip_kotype(port)
					       == IKOT_NAMED_ENTRY)) {
				named_entry =
					(vm_named_entry_t)port->ip_kobject;
				if (!(lck_mtx_try_lock(&(named_entry)->Lock))) {
                       			ip_unlock(port);

					try_failed_count++;
                       			mutex_pause(try_failed_count);
                       			continue;
                		}
				named_entry->ref_count++;
				lck_mtx_unlock(&(named_entry)->Lock);
				ip_unlock(port);
				if ((named_entry->is_sub_map) &&
				    (named_entry->protection
				     & VM_PROT_WRITE)) {
					map = named_entry->backing.map;
				} else {
					mach_destroy_memory_entry(port);
					return VM_MAP_NULL;
				}
				vm_map_reference_swap(map);
				mach_destroy_memory_entry(port);
				break;
			}
			else
				return VM_MAP_NULL;
		}
	}
	else
		map = convert_port_to_map(port);

	return map;
}

/*
 *	Routine:	convert_port_entry_to_object
 *	Purpose:
 *		Convert from a port specifying a named entry to an
 *		object. Doesn't consume the port ref; produces a map ref,
 *		which may be null.
 *	Conditions:
 *		Nothing locked.
 */


vm_object_t
convert_port_entry_to_object(
	ipc_port_t	port)
{
	vm_object_t		object = VM_OBJECT_NULL;
	vm_named_entry_t	named_entry;
	uint32_t		try_failed_count = 0;

	if (IP_VALID(port) &&
	    (ip_kotype(port) == IKOT_NAMED_ENTRY)) {
	try_again:
		ip_lock(port);
		if (ip_active(port) &&
		    (ip_kotype(port) == IKOT_NAMED_ENTRY)) {
			named_entry = (vm_named_entry_t)port->ip_kobject;
			if (!(lck_mtx_try_lock(&(named_entry)->Lock))) {
				ip_unlock(port);
				try_failed_count++;
				mutex_pause(try_failed_count);
                       		goto try_again;
			}
			named_entry->ref_count++;
			lck_mtx_unlock(&(named_entry)->Lock);
			ip_unlock(port);
			if (!(named_entry->is_sub_map) &&
			    !(named_entry->is_copy) &&
			    (named_entry->protection & VM_PROT_WRITE)) {
				object = named_entry->backing.object;
				vm_object_reference(object);
			}
			mach_destroy_memory_entry(port);
		}
	}

	return object;
}

/*
 * Export routines to other components for the things we access locally through
 * macros.
 */
#undef current_map
vm_map_t
current_map(void)
{
	return (current_map_fast());
}

/*
 *	vm_map_reference:
 *
 *	Most code internal to the osfmk will go through a
 *	macro defining this.  This is always here for the
 *	use of other kernel components.
 */
#undef vm_map_reference
void
vm_map_reference(
	vm_map_t	map)
{
	if (map == VM_MAP_NULL)
		return;

	lck_mtx_lock(&map->s_lock);
#if	TASK_SWAPPER
	assert(map->res_count > 0);
	assert(map->ref_count >= map->res_count);
	map->res_count++;
#endif
	map->ref_count++;
	lck_mtx_unlock(&map->s_lock);
}

/*
 *	vm_map_deallocate:
 *
 *	Removes a reference from the specified map,
 *	destroying it if no references remain.
 *	The map should not be locked.
 */
void
vm_map_deallocate(
	vm_map_t	map)
{
	unsigned int		ref;

	if (map == VM_MAP_NULL)
		return;

	lck_mtx_lock(&map->s_lock);
	ref = --map->ref_count;
	if (ref > 0) {
		vm_map_res_deallocate(map);
		lck_mtx_unlock(&map->s_lock);
		return;
	}
	assert(map->ref_count == 0);
	lck_mtx_unlock(&map->s_lock);

#if	TASK_SWAPPER
	/*
	 * The map residence count isn't decremented here because
	 * the vm_map_delete below will traverse the entire map,
	 * deleting entries, and the residence counts on objects
	 * and sharing maps will go away then.
	 */
#endif

	vm_map_destroy(map, VM_MAP_NO_FLAGS);
}


void
vm_map_disable_NX(vm_map_t map)
{
        if (map == NULL)
	        return;
        if (map->pmap == NULL)
	        return;

        pmap_disable_NX(map->pmap);
}

void
vm_map_disallow_data_exec(vm_map_t map)
{
    if (map == NULL)
        return;

    map->map_disallow_data_exec = TRUE;
}

/* XXX Consider making these constants (VM_MAX_ADDRESS and MACH_VM_MAX_ADDRESS)
 * more descriptive.
 */
void
vm_map_set_32bit(vm_map_t map)
{
#if defined(__arm__) || defined(__arm64__)
	map->max_offset = pmap_max_offset(FALSE, ARM_PMAP_MAX_OFFSET_DEVICE);
#else
	map->max_offset = (vm_map_offset_t)VM_MAX_ADDRESS;
#endif
}


void
vm_map_set_64bit(vm_map_t map)
{
#if defined(__arm__) || defined(__arm64__)
	map->max_offset = pmap_max_offset(TRUE, ARM_PMAP_MAX_OFFSET_DEVICE);
#else
	map->max_offset = (vm_map_offset_t)MACH_VM_MAX_ADDRESS;
#endif
}

/*
 * Expand the maximum size of an existing map.
 */
void
vm_map_set_jumbo(vm_map_t map)
{
#if defined (__arm64__)
	vm_map_offset_t old_max_offset = map->max_offset;
	map->max_offset = pmap_max_offset(TRUE, ARM_PMAP_MAX_OFFSET_JUMBO);
	if (map->holes_list->prev->vme_end == pmap_max_offset(TRUE, ARM_PMAP_MAX_OFFSET_DEVICE)) {
		/*
		 * There is already a hole at the end of the map; simply make it bigger.
		 */
		map->holes_list->prev->vme_end = map->max_offset;
	} else {
		/*
		 * There is no hole at the end, so we need to create a new hole
		 * for the new empty space we're creating.
		 */
		struct vm_map_links *new_hole = zalloc(vm_map_holes_zone);
		new_hole->start = old_max_offset;
		new_hole->end = map->max_offset;
		new_hole->prev = map->holes_list->prev;
		new_hole->next = (struct vm_map_entry *)map->holes_list;
		map->holes_list->prev->links.next = (struct vm_map_entry *)new_hole;
		map->holes_list->prev = (struct vm_map_entry *)new_hole;
	}
#else /* arm64 */
	(void) map;
#endif
}

vm_map_offset_t
vm_compute_max_offset(boolean_t is64)
{
#if defined(__arm__) || defined(__arm64__)
	return (pmap_max_offset(is64, ARM_PMAP_MAX_OFFSET_DEVICE));
#else
	return (is64 ? (vm_map_offset_t)MACH_VM_MAX_ADDRESS : (vm_map_offset_t)VM_MAX_ADDRESS);
#endif
}

void
vm_map_get_max_aslr_slide_section(
		vm_map_t                map __unused,    
		int64_t                 *max_sections,
		int64_t                 *section_size)
{
#if defined(__arm64__)
	*max_sections = 3;
	*section_size = ARM_TT_TWIG_SIZE;
#else
	*max_sections = 1;
	*section_size = 0;
#endif
}

uint64_t
vm_map_get_max_aslr_slide_pages(vm_map_t map)
{
#if defined(__arm64__)
	/* Limit arm64 slide to 16MB to conserve contiguous VA space in the more
	 * limited embedded address space; this is also meant to minimize pmap
	 * memory usage on 16KB page systems.
	 */
	return (1 << (24 - VM_MAP_PAGE_SHIFT(map)));
#else
	return (1 << (vm_map_is_64bit(map) ? 16 : 8));
#endif
}

uint64_t
vm_map_get_max_loader_aslr_slide_pages(vm_map_t map)
{
#if defined(__arm64__)
	/* We limit the loader slide to 4MB, in order to ensure at least 8 bits
	 * of independent entropy on 16KB page systems.
	 */
	return (1 << (22 - VM_MAP_PAGE_SHIFT(map)));
#else
	return (1 << (vm_map_is_64bit(map) ? 16 : 8));
#endif
}

#ifndef	__arm__
boolean_t
vm_map_is_64bit(
		vm_map_t map)
{
	return map->max_offset > ((vm_map_offset_t)VM_MAX_ADDRESS);
}
#endif

boolean_t
vm_map_has_hard_pagezero(
		vm_map_t 	map,
		vm_map_offset_t	pagezero_size)
{
	/*
	 * XXX FBDP
	 * We should lock the VM map (for read) here but we can get away
	 * with it for now because there can't really be any race condition:
	 * the VM map's min_offset is changed only when the VM map is created
	 * and when the zero page is established (when the binary gets loaded),
	 * and this routine gets called only when the task terminates and the
	 * VM map is being torn down, and when a new map is created via
	 * load_machfile()/execve().
	 */
	return (map->min_offset >= pagezero_size);
}

/*
 * Raise a VM map's maximun offset.
 */
kern_return_t
vm_map_raise_max_offset(
	vm_map_t	map,
	vm_map_offset_t	new_max_offset)
{
	kern_return_t	ret;

	vm_map_lock(map);
	ret = KERN_INVALID_ADDRESS;

	if (new_max_offset >= map->max_offset) {
		if (!vm_map_is_64bit(map)) {
			if (new_max_offset <= (vm_map_offset_t)VM_MAX_ADDRESS) {
				map->max_offset = new_max_offset;
				ret = KERN_SUCCESS;
			}
		} else {
			if (new_max_offset <= (vm_map_offset_t)MACH_VM_MAX_ADDRESS) {
				map->max_offset = new_max_offset;
				ret = KERN_SUCCESS;
			}
		}
	}

	vm_map_unlock(map);
	return ret;
}


/*
 * Raise a VM map's minimum offset.
 * To strictly enforce "page zero" reservation.
 */
kern_return_t
vm_map_raise_min_offset(
	vm_map_t	map,
	vm_map_offset_t	new_min_offset)
{
	vm_map_entry_t	first_entry;

	new_min_offset = vm_map_round_page(new_min_offset,
					   VM_MAP_PAGE_MASK(map));

	vm_map_lock(map);

	if (new_min_offset < map->min_offset) {
		/*
		 * Can't move min_offset backwards, as that would expose
		 * a part of the address space that was previously, and for
		 * possibly good reasons, inaccessible.
		 */
		vm_map_unlock(map);
		return KERN_INVALID_ADDRESS;
	}
	if (new_min_offset >= map->max_offset) {
		/* can't go beyond the end of the address space */
		vm_map_unlock(map);
		return KERN_INVALID_ADDRESS;
	}

	first_entry = vm_map_first_entry(map);
	if (first_entry != vm_map_to_entry(map) &&
	    first_entry->vme_start < new_min_offset) {
		/*
		 * Some memory was already allocated below the new
		 * minimun offset.  It's too late to change it now...
		 */
		vm_map_unlock(map);
		return KERN_NO_SPACE;
	}

	map->min_offset = new_min_offset;

	assert(map->holes_list);
	map->holes_list->start = new_min_offset;
	assert(new_min_offset < map->holes_list->end);

	vm_map_unlock(map);

	return KERN_SUCCESS;
}

/*
 * Set the limit on the maximum amount of user wired memory allowed for this map.
 * This is basically a copy of the MEMLOCK rlimit value maintained by the BSD side of
 * the kernel.  The limits are checked in the mach VM side, so we keep a copy so we
 * don't have to reach over to the BSD data structures.
 */

void
vm_map_set_user_wire_limit(vm_map_t 	map,
			   vm_size_t	limit)
{
	map->user_wire_limit = limit;
}


void vm_map_switch_protect(vm_map_t	map,
			   boolean_t	val)
{
	vm_map_lock(map);
	map->switch_protect=val;
	vm_map_unlock(map);
}

/*
 * IOKit has mapped a region into this map; adjust the pmap's ledgers appropriately.
 * phys_footprint is a composite limit consisting of iokit + physmem, so we need to
 * bump both counters.
 */
void
vm_map_iokit_mapped_region(vm_map_t map, vm_size_t bytes)
{
	pmap_t pmap = vm_map_pmap(map);

	ledger_credit(pmap->ledger, task_ledgers.iokit_mapped, bytes);
	ledger_credit(pmap->ledger, task_ledgers.phys_footprint, bytes);
}

void
vm_map_iokit_unmapped_region(vm_map_t map, vm_size_t bytes)
{
	pmap_t pmap = vm_map_pmap(map);

	ledger_debit(pmap->ledger, task_ledgers.iokit_mapped, bytes);
	ledger_debit(pmap->ledger, task_ledgers.phys_footprint, bytes);
}

/* Add (generate) code signature for memory range */
#if CONFIG_DYNAMIC_CODE_SIGNING
kern_return_t vm_map_sign(vm_map_t map,
		 vm_map_offset_t start,
		 vm_map_offset_t end)
{
	vm_map_entry_t entry;
	vm_page_t m;
	vm_object_t object;

	/*
	 * Vet all the input parameters and current type and state of the
	 * underlaying object.  Return with an error if anything is amiss.
	 */
	if (map == VM_MAP_NULL)
		return(KERN_INVALID_ARGUMENT);

	vm_map_lock_read(map);

	if (!vm_map_lookup_entry(map, start, &entry) || entry->is_sub_map) {
		/*
		 * Must pass a valid non-submap address.
		 */
		vm_map_unlock_read(map);
		return(KERN_INVALID_ADDRESS);
	}

	if((entry->vme_start > start) || (entry->vme_end < end)) {
		/*
		 * Map entry doesn't cover the requested range. Not handling
		 * this situation currently.
		 */
		vm_map_unlock_read(map);
		return(KERN_INVALID_ARGUMENT);
	}

	object = VME_OBJECT(entry);
	if (object == VM_OBJECT_NULL) {
		/*
		 * Object must already be present or we can't sign.
		 */
		vm_map_unlock_read(map);
		return KERN_INVALID_ARGUMENT;
	}

	vm_object_lock(object);
	vm_map_unlock_read(map);

	while(start < end) {
		uint32_t refmod;

		m = vm_page_lookup(object,
				   start - entry->vme_start + VME_OFFSET(entry));
		if (m==VM_PAGE_NULL) {
			/* shoud we try to fault a page here? we can probably
			 * demand it exists and is locked for this request */
			vm_object_unlock(object);
			return KERN_FAILURE;
		}
		/* deal with special page status */
		if (m->busy ||
		    (m->unusual && (m->error || m->restart || m->private || m->absent))) {
			vm_object_unlock(object);
			return KERN_FAILURE;
		}

		/* Page is OK... now "validate" it */
		/* This is the place where we'll call out to create a code
		 * directory, later */
		m->cs_validated = TRUE;

		/* The page is now "clean" for codesigning purposes. That means
		 * we don't consider it as modified (wpmapped) anymore. But
		 * we'll disconnect the page so we note any future modification
		 * attempts. */
		m->wpmapped = FALSE;
		refmod = pmap_disconnect(VM_PAGE_GET_PHYS_PAGE(m));

		/* Pull the dirty status from the pmap, since we cleared the
		 * wpmapped bit */
		if ((refmod & VM_MEM_MODIFIED) && !m->dirty) {
			SET_PAGE_DIRTY(m, FALSE);
		}

		/* On to the next page */
		start += PAGE_SIZE;
	}
	vm_object_unlock(object);

	return KERN_SUCCESS;
}
#endif

kern_return_t vm_map_partial_reap(vm_map_t map, unsigned int *reclaimed_resident, unsigned int *reclaimed_compressed)
{
	vm_map_entry_t	entry = VM_MAP_ENTRY_NULL;
	vm_map_entry_t next_entry;
	kern_return_t	kr = KERN_SUCCESS;
	vm_map_t 	zap_map;

	vm_map_lock(map);

	/*
	 * We use a "zap_map" to avoid having to unlock
	 * the "map" in vm_map_delete().
	 */
	zap_map = vm_map_create(PMAP_NULL,
				map->min_offset,
				map->max_offset,
				map->hdr.entries_pageable);

	if (zap_map == VM_MAP_NULL) {
		return KERN_RESOURCE_SHORTAGE;
	}

	vm_map_set_page_shift(zap_map,
			      VM_MAP_PAGE_SHIFT(map));
	vm_map_disable_hole_optimization(zap_map);

	for (entry = vm_map_first_entry(map);
	     entry != vm_map_to_entry(map);
	     entry = next_entry) {
		next_entry = entry->vme_next;

		if (VME_OBJECT(entry) &&
		    !entry->is_sub_map &&
		    (VME_OBJECT(entry)->internal == TRUE) &&
		    (VME_OBJECT(entry)->ref_count == 1)) {

			*reclaimed_resident += VME_OBJECT(entry)->resident_page_count;
			*reclaimed_compressed += vm_compressor_pager_get_count(VME_OBJECT(entry)->pager);

			(void)vm_map_delete(map,
					    entry->vme_start,
					    entry->vme_end,
					    VM_MAP_REMOVE_SAVE_ENTRIES,
					    zap_map);
		}
	}

	vm_map_unlock(map);

        /*
	 * Get rid of the "zap_maps" and all the map entries that
         * they may still contain.
         */
        if (zap_map != VM_MAP_NULL) {
                vm_map_destroy(zap_map, VM_MAP_REMOVE_NO_PMAP_CLEANUP);
                zap_map = VM_MAP_NULL;
        }

	return kr;
}


#if DEVELOPMENT || DEBUG

int
vm_map_disconnect_page_mappings(
	vm_map_t map,
	boolean_t do_unnest)
{
	vm_map_entry_t entry;
	int	page_count = 0;

	if (do_unnest == TRUE) {
#ifndef NO_NESTED_PMAP
		vm_map_lock(map);

		for (entry = vm_map_first_entry(map);
		     entry != vm_map_to_entry(map);
		     entry = entry->vme_next) {

			if (entry->is_sub_map && entry->use_pmap) {
				/*
				 * Make sure the range between the start of this entry and
				 * the end of this entry is no longer nested, so that
				 * we will only remove mappings from the pmap in use by this
				 * this task
				 */
				vm_map_clip_unnest(map, entry, entry->vme_start, entry->vme_end);
			}
		}
		vm_map_unlock(map);
#endif
	}
	vm_map_lock_read(map);

	page_count = map->pmap->stats.resident_count;

	for (entry = vm_map_first_entry(map);
	     entry != vm_map_to_entry(map);
	     entry = entry->vme_next) {

		if (!entry->is_sub_map && ((VME_OBJECT(entry) == 0) ||
					   (VME_OBJECT(entry)->phys_contiguous))) {
			continue;
		}
		if (entry->is_sub_map)
			assert(!entry->use_pmap);

		pmap_remove_options(map->pmap, entry->vme_start, entry->vme_end, 0);
	}
	vm_map_unlock_read(map);

	return page_count;
}

#endif


#if CONFIG_FREEZE


int c_freezer_swapout_count;
int c_freezer_compression_count = 0;
AbsoluteTime c_freezer_last_yield_ts = 0;

kern_return_t vm_map_freeze(
             	vm_map_t map,
             	unsigned int *purgeable_count,
             	unsigned int *wired_count,
             	unsigned int *clean_count,
             	unsigned int *dirty_count,
             	__unused unsigned int dirty_budget,
             	boolean_t *has_shared)
{
	vm_map_entry_t	entry2 = VM_MAP_ENTRY_NULL;
	kern_return_t	kr = KERN_SUCCESS;

	*purgeable_count = *wired_count = *clean_count = *dirty_count = 0;
	*has_shared = FALSE;

	/*
	 * We need the exclusive lock here so that we can
	 * block any page faults or lookups while we are
	 * in the middle of freezing this vm map.
	 */
	vm_map_lock(map);

	assert(VM_CONFIG_COMPRESSOR_IS_PRESENT);

	if (vm_compressor_low_on_space() || vm_swap_low_on_space()) {
		kr = KERN_NO_SPACE;
		goto done;
	}

	c_freezer_compression_count = 0;
	clock_get_uptime(&c_freezer_last_yield_ts);

	for (entry2 = vm_map_first_entry(map);
	     entry2 != vm_map_to_entry(map);
	     entry2 = entry2->vme_next) {

		vm_object_t	src_object = VME_OBJECT(entry2);

		if (src_object &&
		    !entry2->is_sub_map &&
		    !src_object->phys_contiguous) {
			/* If eligible, scan the entry, moving eligible pages over to our parent object */

			if (src_object->internal == TRUE) {

				if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
					/*
					 * Pages belonging to this object could be swapped to disk.
					 * Make sure it's not a shared object because we could end
					 * up just bringing it back in again.
					 */
					if (src_object->ref_count > 1) {
						continue;
					}
				}
				vm_object_compressed_freezer_pageout(src_object);

				if (vm_compressor_low_on_space() || vm_swap_low_on_space()) {
					kr = KERN_NO_SPACE;
					break;
				}
			}
		}
	}
done:
	vm_map_unlock(map);

	vm_object_compressed_freezer_done();

	if (VM_CONFIG_FREEZER_SWAP_IS_ACTIVE) {
		/*
		 * reset the counter tracking the # of swapped c_segs
		 * because we are now done with this freeze session and task.
		 */
		c_freezer_swapout_count = 0;
	}
	return kr;
}

#endif

/*
 * vm_map_entry_should_cow_for_true_share:
 *
 * Determines if the map entry should be clipped and setup for copy-on-write
 * to avoid applying "true_share" to a large VM object when only a subset is
 * targeted.
 *
 * For now, we target only the map entries created for the Objective C
 * Garbage Collector, which initially have the following properties:
 *	- alias == VM_MEMORY_MALLOC
 * 	- wired_count == 0
 * 	- !needs_copy
 * and a VM object with:
 * 	- internal
 * 	- copy_strategy == MEMORY_OBJECT_COPY_SYMMETRIC
 * 	- !true_share
 * 	- vo_size == ANON_CHUNK_SIZE
 *
 * Only non-kernel map entries.
 */
boolean_t
vm_map_entry_should_cow_for_true_share(
	vm_map_entry_t	entry)
{
	vm_object_t	object;

	if (entry->is_sub_map) {
		/* entry does not point at a VM object */
		return FALSE;
	}

	if (entry->needs_copy) {
		/* already set for copy_on_write: done! */
		return FALSE;
	}

	if (VME_ALIAS(entry) != VM_MEMORY_MALLOC &&
	    VME_ALIAS(entry) != VM_MEMORY_MALLOC_SMALL) {
		/* not a malloc heap or Obj-C Garbage Collector heap */
		return FALSE;
	}

	if (entry->wired_count) {
		/* wired: can't change the map entry... */
		vm_counters.should_cow_but_wired++;
		return FALSE;
	}

	object = VME_OBJECT(entry);

	if (object == VM_OBJECT_NULL) {
		/* no object yet... */
		return FALSE;
	}

	if (!object->internal) {
		/* not an internal object */
		return FALSE;
	}

	if (object->copy_strategy != MEMORY_OBJECT_COPY_SYMMETRIC) {
		/* not the default copy strategy */
		return FALSE;
	}

	if (object->true_share) {
		/* already true_share: too late to avoid it */
		return FALSE;
	}

	if (VME_ALIAS(entry) == VM_MEMORY_MALLOC &&
	    object->vo_size != ANON_CHUNK_SIZE) {
		/* ... not an object created for the ObjC Garbage Collector */
		return FALSE;
	}

	if (VME_ALIAS(entry) == VM_MEMORY_MALLOC_SMALL &&
	    object->vo_size != 2048 * 4096) {
		/* ... not a "MALLOC_SMALL" heap */
		return FALSE;
	}

	/*
	 * All the criteria match: we have a large object being targeted for "true_share".
	 * To limit the adverse side-effects linked with "true_share", tell the caller to
	 * try and avoid setting up the entire object for "true_share" by clipping the
	 * targeted range and setting it up for copy-on-write.
	 */
	return TRUE;
}

vm_map_offset_t
vm_map_round_page_mask(
 	vm_map_offset_t	offset,
	vm_map_offset_t	mask)
{
	return VM_MAP_ROUND_PAGE(offset, mask);
}

vm_map_offset_t
vm_map_trunc_page_mask(
	vm_map_offset_t	offset,
	vm_map_offset_t	mask)
{
	return VM_MAP_TRUNC_PAGE(offset, mask);
}

boolean_t
vm_map_page_aligned(
	vm_map_offset_t	offset,
	vm_map_offset_t	mask)
{
	return ((offset) & mask) == 0;
}

int
vm_map_page_shift(
	vm_map_t map)
{
	return VM_MAP_PAGE_SHIFT(map);
}

int
vm_map_page_size(
	vm_map_t map)
{
	return VM_MAP_PAGE_SIZE(map);
}

vm_map_offset_t
vm_map_page_mask(
	vm_map_t map)
{
	return VM_MAP_PAGE_MASK(map);
}

kern_return_t
vm_map_set_page_shift(
	vm_map_t  	map,
	int		pageshift)
{
	if (map->hdr.nentries != 0) {
		/* too late to change page size */
		return KERN_FAILURE;
	}

	map->hdr.page_shift = pageshift;

	return KERN_SUCCESS;
}

kern_return_t
vm_map_query_volatile(
	vm_map_t	map,
	mach_vm_size_t	*volatile_virtual_size_p,
	mach_vm_size_t	*volatile_resident_size_p,
	mach_vm_size_t	*volatile_compressed_size_p,
	mach_vm_size_t	*volatile_pmap_size_p,
	mach_vm_size_t	*volatile_compressed_pmap_size_p)
{
	mach_vm_size_t	volatile_virtual_size;
	mach_vm_size_t	volatile_resident_count;
	mach_vm_size_t	volatile_compressed_count;
	mach_vm_size_t	volatile_pmap_count;
	mach_vm_size_t	volatile_compressed_pmap_count;
	mach_vm_size_t	resident_count;
	vm_map_entry_t	entry;
	vm_object_t	object;

	/* map should be locked by caller */

	volatile_virtual_size = 0;
	volatile_resident_count = 0;
	volatile_compressed_count = 0;
	volatile_pmap_count = 0;
	volatile_compressed_pmap_count = 0;

	for (entry = vm_map_first_entry(map);
	     entry != vm_map_to_entry(map);
	     entry = entry->vme_next) {
		mach_vm_size_t	pmap_resident_bytes, pmap_compressed_bytes;

		if (entry->is_sub_map) {
			continue;
		}
		if (! (entry->protection & VM_PROT_WRITE)) {
			continue;
		}
		object = VME_OBJECT(entry);
		if (object == VM_OBJECT_NULL) {
			continue;
		}
		if (object->purgable != VM_PURGABLE_VOLATILE &&
		    object->purgable != VM_PURGABLE_EMPTY) {
			continue;
		}
		if (VME_OFFSET(entry)) {
			/*
			 * If the map entry has been split and the object now
			 * appears several times in the VM map, we don't want
			 * to count the object's resident_page_count more than
			 * once.  We count it only for the first one, starting
			 * at offset 0 and ignore the other VM map entries.
			 */
			continue;
		}
		resident_count = object->resident_page_count;
		if ((VME_OFFSET(entry) / PAGE_SIZE) >= resident_count) {
			resident_count = 0;
		} else {
			resident_count -= (VME_OFFSET(entry) / PAGE_SIZE);
		}

		volatile_virtual_size += entry->vme_end - entry->vme_start;
		volatile_resident_count += resident_count;
		if (object->pager) {
			volatile_compressed_count +=
				vm_compressor_pager_get_count(object->pager);
		}
		pmap_compressed_bytes = 0;
		pmap_resident_bytes =
			pmap_query_resident(map->pmap,
					    entry->vme_start,
					    entry->vme_end,
					    &pmap_compressed_bytes);
		volatile_pmap_count += (pmap_resident_bytes / PAGE_SIZE);
		volatile_compressed_pmap_count += (pmap_compressed_bytes
						   / PAGE_SIZE);
	}

	/* map is still locked on return */

	*volatile_virtual_size_p = volatile_virtual_size;
	*volatile_resident_size_p = volatile_resident_count * PAGE_SIZE;
	*volatile_compressed_size_p = volatile_compressed_count * PAGE_SIZE;
	*volatile_pmap_size_p = volatile_pmap_count * PAGE_SIZE;
	*volatile_compressed_pmap_size_p = volatile_compressed_pmap_count * PAGE_SIZE;

	return KERN_SUCCESS;
}

void
vm_map_sizes(vm_map_t map,
		vm_map_size_t * psize,
		vm_map_size_t * pfree,
		vm_map_size_t * plargest_free)
{
    vm_map_entry_t  entry;
    vm_map_offset_t prev;
    vm_map_size_t   free, total_free, largest_free;
    boolean_t       end;

    if (!map)
    {
        *psize = *pfree = *plargest_free = 0;
        return;
    }
    total_free = largest_free = 0;

    vm_map_lock_read(map);
    if (psize) *psize = map->max_offset - map->min_offset;

    prev = map->min_offset;
    for (entry = vm_map_first_entry(map);; entry = entry->vme_next)
    {
	end = (entry == vm_map_to_entry(map));

	if (end) free = entry->vme_end   - prev;
	else     free = entry->vme_start - prev;

	total_free += free;
	if (free > largest_free) largest_free = free;

	if (end) break;
	prev = entry->vme_end;
    }
    vm_map_unlock_read(map);
    if (pfree)         *pfree = total_free;
    if (plargest_free) *plargest_free = largest_free;
}

#if VM_SCAN_FOR_SHADOW_CHAIN
int vm_map_shadow_max(vm_map_t map);
int vm_map_shadow_max(
	vm_map_t map)
{
	int		shadows, shadows_max;
	vm_map_entry_t	entry;
	vm_object_t	object, next_object;

	if (map == NULL)
		return 0;

	shadows_max = 0;

	vm_map_lock_read(map);

	for (entry = vm_map_first_entry(map);
	     entry != vm_map_to_entry(map);
	     entry = entry->vme_next) {
		if (entry->is_sub_map) {
			continue;
		}
		object = VME_OBJECT(entry);
		if (object == NULL) {
			continue;
		}
		vm_object_lock_shared(object);
		for (shadows = 0;
		     object->shadow != NULL;
		     shadows++, object = next_object) {
			next_object = object->shadow;
			vm_object_lock_shared(next_object);
			vm_object_unlock(object);
		}
		vm_object_unlock(object);
		if (shadows > shadows_max) {
			shadows_max = shadows;
		}
	}

	vm_map_unlock_read(map);

	return shadows_max;
}
#endif /* VM_SCAN_FOR_SHADOW_CHAIN */

void vm_commit_pagezero_status(vm_map_t lmap) {
	pmap_advise_pagezero_range(lmap->pmap, lmap->min_offset);
}

#if __x86_64__
void
vm_map_set_high_start(
	vm_map_t	map,
	vm_map_offset_t	high_start)
{
	map->vmmap_high_start = high_start;
}
#endif /* __x86_64__ */
