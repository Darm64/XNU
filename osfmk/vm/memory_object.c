/*
 * Copyright (c) 2000-2019 Apple Inc. All rights reserved.
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
 *	File:	vm/memory_object.c
 *	Author:	Michael Wayne Young
 *
 *	External memory management interface control functions.
 */

/*
 *	Interface dependencies:
 */

#include <mach/std_types.h>     /* For pointer_t */
#include <mach/mach_types.h>

#include <mach/mig.h>
#include <mach/kern_return.h>
#include <mach/memory_object.h>
#include <mach/memory_object_default.h>
#include <mach/memory_object_control_server.h>
#include <mach/host_priv_server.h>
#include <mach/boolean.h>
#include <mach/vm_prot.h>
#include <mach/message.h>

/*
 *	Implementation dependencies:
 */
#include <string.h>             /* For memcpy() */

#include <kern/host.h>
#include <kern/thread.h>        /* For current_thread() */
#include <kern/ipc_mig.h>
#include <kern/misc_protos.h>

#include <vm/vm_object.h>
#include <vm/vm_fault.h>
#include <vm/memory_object.h>
#include <vm/vm_page.h>
#include <vm/vm_pageout.h>
#include <vm/pmap.h>            /* For pmap_clear_modify */
#include <vm/vm_kern.h>         /* For kernel_map, vm_move */
#include <vm/vm_map.h>          /* For vm_map_pageable */
#include <vm/vm_purgeable_internal.h>   /* Needed by some vm_page.h macros */
#include <vm/vm_shared_region.h>

#include <vm/vm_external.h>

#include <vm/vm_protos.h>

memory_object_default_t memory_manager_default = MEMORY_OBJECT_DEFAULT_NULL;
decl_lck_mtx_data(, memory_manager_default_lock);


/*
 *	Routine:	memory_object_should_return_page
 *
 *	Description:
 *		Determine whether the given page should be returned,
 *		based on the page's state and on the given return policy.
 *
 *		We should return the page if one of the following is true:
 *
 *		1. Page is dirty and should_return is not RETURN_NONE.
 *		2. Page is precious and should_return is RETURN_ALL.
 *		3. Should_return is RETURN_ANYTHING.
 *
 *		As a side effect, m->vmp_dirty will be made consistent
 *		with pmap_is_modified(m), if should_return is not
 *		MEMORY_OBJECT_RETURN_NONE.
 */

#define memory_object_should_return_page(m, should_return) \
    (should_return != MEMORY_OBJECT_RETURN_NONE && \
     (((m)->vmp_dirty || ((m)->vmp_dirty = pmap_is_modified(VM_PAGE_GET_PHYS_PAGE(m)))) || \
      ((m)->vmp_precious && (should_return) == MEMORY_OBJECT_RETURN_ALL) || \
      (should_return) == MEMORY_OBJECT_RETURN_ANYTHING))

typedef int     memory_object_lock_result_t;

#define MEMORY_OBJECT_LOCK_RESULT_DONE                  0
#define MEMORY_OBJECT_LOCK_RESULT_MUST_BLOCK            1
#define MEMORY_OBJECT_LOCK_RESULT_MUST_RETURN           2
#define MEMORY_OBJECT_LOCK_RESULT_MUST_FREE             3

memory_object_lock_result_t memory_object_lock_page(
	vm_page_t               m,
	memory_object_return_t  should_return,
	boolean_t               should_flush,
	vm_prot_t               prot);

/*
 *	Routine:	memory_object_lock_page
 *
 *	Description:
 *		Perform the appropriate lock operations on the
 *		given page.  See the description of
 *		"memory_object_lock_request" for the meanings
 *		of the arguments.
 *
 *		Returns an indication that the operation
 *		completed, blocked, or that the page must
 *		be cleaned.
 */
memory_object_lock_result_t
memory_object_lock_page(
	vm_page_t               m,
	memory_object_return_t  should_return,
	boolean_t               should_flush,
	vm_prot_t               prot)
{
	if (m->vmp_busy || m->vmp_cleaning) {
		return MEMORY_OBJECT_LOCK_RESULT_MUST_BLOCK;
	}

	if (m->vmp_laundry) {
		vm_pageout_steal_laundry(m, FALSE);
	}

	/*
	 *	Don't worry about pages for which the kernel
	 *	does not have any data.
	 */
	if (m->vmp_absent || m->vmp_error || m->vmp_restart) {
		if (m->vmp_error && should_flush && !VM_PAGE_WIRED(m)) {
			/*
			 * dump the page, pager wants us to
			 * clean it up and there is no
			 * relevant data to return
			 */
			return MEMORY_OBJECT_LOCK_RESULT_MUST_FREE;
		}
		return MEMORY_OBJECT_LOCK_RESULT_DONE;
	}
	assert(!m->vmp_fictitious);

	if (VM_PAGE_WIRED(m)) {
		/*
		 * The page is wired... just clean or return the page if needed.
		 * Wired pages don't get flushed or disconnected from the pmap.
		 */
		if (memory_object_should_return_page(m, should_return)) {
			return MEMORY_OBJECT_LOCK_RESULT_MUST_RETURN;
		}

		return MEMORY_OBJECT_LOCK_RESULT_DONE;
	}

	if (should_flush) {
		/*
		 * must do the pmap_disconnect before determining the
		 * need to return the page... otherwise it's possible
		 * for the page to go from the clean to the dirty state
		 * after we've made our decision
		 */
		if (pmap_disconnect(VM_PAGE_GET_PHYS_PAGE(m)) & VM_MEM_MODIFIED) {
			SET_PAGE_DIRTY(m, FALSE);
		}
	} else {
		/*
		 * If we are decreasing permission, do it now;
		 * let the fault handler take care of increases
		 * (pmap_page_protect may not increase protection).
		 */
		if (prot != VM_PROT_NO_CHANGE) {
			pmap_page_protect(VM_PAGE_GET_PHYS_PAGE(m), VM_PROT_ALL & ~prot);
		}
	}
	/*
	 *	Handle returning dirty or precious pages
	 */
	if (memory_object_should_return_page(m, should_return)) {
		/*
		 * we use to do a pmap_disconnect here in support
		 * of memory_object_lock_request, but that routine
		 * no longer requires this...  in any event, in
		 * our world, it would turn into a big noop since
		 * we don't lock the page in any way and as soon
		 * as we drop the object lock, the page can be
		 * faulted back into an address space
		 *
		 *	if (!should_flush)
		 *		pmap_disconnect(VM_PAGE_GET_PHYS_PAGE(m));
		 */
		return MEMORY_OBJECT_LOCK_RESULT_MUST_RETURN;
	}

	/*
	 *	Handle flushing clean pages
	 */
	if (should_flush) {
		return MEMORY_OBJECT_LOCK_RESULT_MUST_FREE;
	}

	/*
	 * we use to deactivate clean pages at this point,
	 * but we do not believe that an msync should change
	 * the 'age' of a page in the cache... here is the
	 * original comment and code concerning this...
	 *
	 *	XXX Make clean but not flush a paging hint,
	 *	and deactivate the pages.  This is a hack
	 *	because it overloads flush/clean with
	 *	implementation-dependent meaning.  This only
	 *	happens to pages that are already clean.
	 *
	 *   if (vm_page_deactivate_hint && (should_return != MEMORY_OBJECT_RETURN_NONE))
	 *	return (MEMORY_OBJECT_LOCK_RESULT_MUST_DEACTIVATE);
	 */

	return MEMORY_OBJECT_LOCK_RESULT_DONE;
}



/*
 *	Routine:	memory_object_lock_request [user interface]
 *
 *	Description:
 *		Control use of the data associated with the given
 *		memory object.  For each page in the given range,
 *		perform the following operations, in order:
 *			1)  restrict access to the page (disallow
 *			    forms specified by "prot");
 *			2)  return data to the manager (if "should_return"
 *			    is RETURN_DIRTY and the page is dirty, or
 *                          "should_return" is RETURN_ALL and the page
 *			    is either dirty or precious); and,
 *			3)  flush the cached copy (if "should_flush"
 *			    is asserted).
 *		The set of pages is defined by a starting offset
 *		("offset") and size ("size").  Only pages with the
 *		same page alignment as the starting offset are
 *		considered.
 *
 *		A single acknowledgement is sent (to the "reply_to"
 *		port) when these actions are complete.  If successful,
 *		the naked send right for reply_to is consumed.
 */

kern_return_t
memory_object_lock_request(
	memory_object_control_t         control,
	memory_object_offset_t          offset,
	memory_object_size_t            size,
	memory_object_offset_t  *       resid_offset,
	int                     *       io_errno,
	memory_object_return_t          should_return,
	int                             flags,
	vm_prot_t                       prot)
{
	vm_object_t     object;

	/*
	 *	Check for bogus arguments.
	 */
	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	if ((prot & ~VM_PROT_ALL) != 0 && prot != VM_PROT_NO_CHANGE) {
		return KERN_INVALID_ARGUMENT;
	}

	size = round_page_64(size);

	/*
	 *	Lock the object, and acquire a paging reference to
	 *	prevent the memory_object reference from being released.
	 */
	vm_object_lock(object);
	vm_object_paging_begin(object);

	if (flags & MEMORY_OBJECT_DATA_FLUSH_ALL) {
		if ((should_return != MEMORY_OBJECT_RETURN_NONE) || offset || object->copy) {
			flags &= ~MEMORY_OBJECT_DATA_FLUSH_ALL;
			flags |= MEMORY_OBJECT_DATA_FLUSH;
		}
	}
	offset -= object->paging_offset;

	if (flags & MEMORY_OBJECT_DATA_FLUSH_ALL) {
		vm_object_reap_pages(object, REAP_DATA_FLUSH);
	} else {
		(void)vm_object_update(object, offset, size, resid_offset,
		    io_errno, should_return, flags, prot);
	}

	vm_object_paging_end(object);
	vm_object_unlock(object);

	return KERN_SUCCESS;
}

/*
 *	memory_object_release_name:  [interface]
 *
 *	Enforces name semantic on memory_object reference count decrement
 *	This routine should not be called unless the caller holds a name
 *	reference gained through the memory_object_named_create or the
 *	memory_object_rename call.
 *	If the TERMINATE_IDLE flag is set, the call will return if the
 *	reference count is not 1. i.e. idle with the only remaining reference
 *	being the name.
 *	If the decision is made to proceed the name field flag is set to
 *	false and the reference count is decremented.  If the RESPECT_CACHE
 *	flag is set and the reference count has gone to zero, the
 *	memory_object is checked to see if it is cacheable otherwise when
 *	the reference count is zero, it is simply terminated.
 */

kern_return_t
memory_object_release_name(
	memory_object_control_t control,
	int                             flags)
{
	vm_object_t     object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	return vm_object_release_name(object, flags);
}



/*
 *	Routine:	memory_object_destroy [user interface]
 *	Purpose:
 *		Shut down a memory object, despite the
 *		presence of address map (or other) references
 *		to the vm_object.
 */
kern_return_t
memory_object_destroy(
	memory_object_control_t control,
	kern_return_t           reason)
{
	vm_object_t             object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	return vm_object_destroy(object, reason);
}

/*
 *	Routine:	vm_object_sync
 *
 *	Kernel internal function to synch out pages in a given
 *	range within an object to its memory manager.  Much the
 *	same as memory_object_lock_request but page protection
 *	is not changed.
 *
 *	If the should_flush and should_return flags are true pages
 *	are flushed, that is dirty & precious pages are written to
 *	the memory manager and then discarded.  If should_return
 *	is false, only precious pages are returned to the memory
 *	manager.
 *
 *	If should flush is false and should_return true, the memory
 *	manager's copy of the pages is updated.  If should_return
 *	is also false, only the precious pages are updated.  This
 *	last option is of limited utility.
 *
 *	Returns:
 *	FALSE		if no pages were returned to the pager
 *	TRUE		otherwise.
 */

boolean_t
vm_object_sync(
	vm_object_t             object,
	vm_object_offset_t      offset,
	vm_object_size_t        size,
	boolean_t               should_flush,
	boolean_t               should_return,
	boolean_t               should_iosync)
{
	boolean_t       rv;
	int             flags;

	/*
	 * Lock the object, and acquire a paging reference to
	 * prevent the memory_object and control ports from
	 * being destroyed.
	 */
	vm_object_lock(object);
	vm_object_paging_begin(object);

	if (should_flush) {
		flags = MEMORY_OBJECT_DATA_FLUSH;
		/*
		 * This flush is from an msync(), not a truncate(), so the
		 * contents of the file are not affected.
		 * MEMORY_OBECT_DATA_NO_CHANGE lets vm_object_update() know
		 * that the data is not changed and that there's no need to
		 * push the old contents to a copy object.
		 */
		flags |= MEMORY_OBJECT_DATA_NO_CHANGE;
	} else {
		flags = 0;
	}

	if (should_iosync) {
		flags |= MEMORY_OBJECT_IO_SYNC;
	}

	rv = vm_object_update(object, offset, (vm_object_size_t)size, NULL, NULL,
	    (should_return) ?
	    MEMORY_OBJECT_RETURN_ALL :
	    MEMORY_OBJECT_RETURN_NONE,
	    flags,
	    VM_PROT_NO_CHANGE);


	vm_object_paging_end(object);
	vm_object_unlock(object);
	return rv;
}



#define LIST_REQ_PAGEOUT_PAGES(object, data_cnt, po, ro, ioerr, iosync)    \
MACRO_BEGIN                                                             \
                                                                        \
	int			upl_flags;                              \
	memory_object_t		pager;                                  \
                                                                        \
	if ((pager = (object)->pager) != MEMORY_OBJECT_NULL) {          \
	        vm_object_paging_begin(object);                         \
	        vm_object_unlock(object);                               \
                                                                        \
	        if (iosync)                                             \
	                upl_flags = UPL_MSYNC | UPL_IOSYNC;             \
	        else                                                    \
	                upl_flags = UPL_MSYNC;                          \
                                                                        \
	        (void) memory_object_data_return(pager,                 \
	                po,                                             \
	                (memory_object_cluster_size_t)data_cnt,         \
	                ro,                                             \
	                ioerr,                                          \
	                FALSE,                                          \
	                FALSE,                                          \
	                upl_flags);                                     \
                                                                        \
	        vm_object_lock(object);                                 \
	        vm_object_paging_end(object);                           \
	}                                                               \
MACRO_END

extern struct vnode *
vnode_pager_lookup_vnode(memory_object_t);

static int
vm_object_update_extent(
	vm_object_t             object,
	vm_object_offset_t      offset,
	vm_object_offset_t      offset_end,
	vm_object_offset_t      *offset_resid,
	int                     *io_errno,
	boolean_t               should_flush,
	memory_object_return_t  should_return,
	boolean_t               should_iosync,
	vm_prot_t               prot)
{
	vm_page_t       m;
	int             retval = 0;
	vm_object_offset_t      paging_offset = 0;
	vm_object_offset_t      next_offset = offset;
	memory_object_lock_result_t     page_lock_result;
	memory_object_cluster_size_t    data_cnt = 0;
	struct vm_page_delayed_work     dw_array[DEFAULT_DELAYED_WORK_LIMIT];
	struct vm_page_delayed_work     *dwp;
	int             dw_count;
	int             dw_limit;
	int             dirty_count;

	dwp = &dw_array[0];
	dw_count = 0;
	dw_limit = DELAYED_WORK_LIMIT(DEFAULT_DELAYED_WORK_LIMIT);
	dirty_count = 0;

	for (;
	    offset < offset_end && object->resident_page_count;
	    offset += PAGE_SIZE_64) {
		/*
		 * Limit the number of pages to be cleaned at once to a contiguous
		 * run, or at most MAX_UPL_TRANSFER_BYTES
		 */
		if (data_cnt) {
			if ((data_cnt >= MAX_UPL_TRANSFER_BYTES) || (next_offset != offset)) {
				if (dw_count) {
					vm_page_do_delayed_work(object, VM_KERN_MEMORY_NONE, &dw_array[0], dw_count);
					dwp = &dw_array[0];
					dw_count = 0;
				}
				LIST_REQ_PAGEOUT_PAGES(object, data_cnt,
				    paging_offset, offset_resid, io_errno, should_iosync);
				data_cnt = 0;
			}
		}
		while ((m = vm_page_lookup(object, offset)) != VM_PAGE_NULL) {
			dwp->dw_mask = 0;

			page_lock_result = memory_object_lock_page(m, should_return, should_flush, prot);

			if (data_cnt && page_lock_result != MEMORY_OBJECT_LOCK_RESULT_MUST_RETURN) {
				/*
				 *	End of a run of dirty/precious pages.
				 */
				if (dw_count) {
					vm_page_do_delayed_work(object, VM_KERN_MEMORY_NONE, &dw_array[0], dw_count);
					dwp = &dw_array[0];
					dw_count = 0;
				}
				LIST_REQ_PAGEOUT_PAGES(object, data_cnt,
				    paging_offset, offset_resid, io_errno, should_iosync);
				/*
				 * LIST_REQ_PAGEOUT_PAGES will drop the object lock which will
				 * allow the state of page 'm' to change... we need to re-lookup
				 * the current offset
				 */
				data_cnt = 0;
				continue;
			}

			switch (page_lock_result) {
			case MEMORY_OBJECT_LOCK_RESULT_DONE:
				break;

			case MEMORY_OBJECT_LOCK_RESULT_MUST_FREE:
				if (m->vmp_dirty == TRUE) {
					dirty_count++;
				}
				dwp->dw_mask |= DW_vm_page_free;
				break;

			case MEMORY_OBJECT_LOCK_RESULT_MUST_BLOCK:
				PAGE_SLEEP(object, m, THREAD_UNINT);
				continue;

			case MEMORY_OBJECT_LOCK_RESULT_MUST_RETURN:
				if (data_cnt == 0) {
					paging_offset = offset;
				}

				data_cnt += PAGE_SIZE;
				next_offset = offset + PAGE_SIZE_64;

				/*
				 * wired pages shouldn't be flushed and
				 * since they aren't on any queue,
				 * no need to remove them
				 */
				if (!VM_PAGE_WIRED(m)) {
					if (should_flush) {
						/*
						 * add additional state for the flush
						 */
						m->vmp_free_when_done = TRUE;
					}
					/*
					 * we use to remove the page from the queues at this
					 * point, but we do not believe that an msync
					 * should cause the 'age' of a page to be changed
					 *
					 *    else
					 *	dwp->dw_mask |= DW_VM_PAGE_QUEUES_REMOVE;
					 */
				}
				retval = 1;
				break;
			}
			if (dwp->dw_mask) {
				VM_PAGE_ADD_DELAYED_WORK(dwp, m, dw_count);

				if (dw_count >= dw_limit) {
					vm_page_do_delayed_work(object, VM_KERN_MEMORY_NONE, &dw_array[0], dw_count);
					dwp = &dw_array[0];
					dw_count = 0;
				}
			}
			break;
		}
	}

	if (object->pager) {
		task_update_logical_writes(current_task(), (dirty_count * PAGE_SIZE), TASK_WRITE_INVALIDATED, vnode_pager_lookup_vnode(object->pager));
	}
	/*
	 *	We have completed the scan for applicable pages.
	 *	Clean any pages that have been saved.
	 */
	if (dw_count) {
		vm_page_do_delayed_work(object, VM_KERN_MEMORY_NONE, &dw_array[0], dw_count);
	}

	if (data_cnt) {
		LIST_REQ_PAGEOUT_PAGES(object, data_cnt,
		    paging_offset, offset_resid, io_errno, should_iosync);
	}
	return retval;
}



/*
 *	Routine:	vm_object_update
 *	Description:
 *		Work function for m_o_lock_request(), vm_o_sync().
 *
 *		Called with object locked and paging ref taken.
 */
kern_return_t
vm_object_update(
	vm_object_t             object,
	vm_object_offset_t      offset,
	vm_object_size_t        size,
	vm_object_offset_t      *resid_offset,
	int                     *io_errno,
	memory_object_return_t  should_return,
	int                     flags,
	vm_prot_t               protection)
{
	vm_object_t             copy_object = VM_OBJECT_NULL;
	boolean_t               data_returned = FALSE;
	boolean_t               update_cow;
	boolean_t               should_flush = (flags & MEMORY_OBJECT_DATA_FLUSH) ? TRUE : FALSE;
	boolean_t               should_iosync = (flags & MEMORY_OBJECT_IO_SYNC) ? TRUE : FALSE;
	vm_fault_return_t       result;
	int                     num_of_extents;
	int                     n;
#define MAX_EXTENTS     8
#define EXTENT_SIZE     (1024 * 1024 * 256)
#define RESIDENT_LIMIT  (1024 * 32)
	struct extent {
		vm_object_offset_t e_base;
		vm_object_offset_t e_min;
		vm_object_offset_t e_max;
	} extents[MAX_EXTENTS];

	/*
	 *	To avoid blocking while scanning for pages, save
	 *	dirty pages to be cleaned all at once.
	 *
	 *	XXXO A similar strategy could be used to limit the
	 *	number of times that a scan must be restarted for
	 *	other reasons.  Those pages that would require blocking
	 *	could be temporarily collected in another list, or
	 *	their offsets could be recorded in a small array.
	 */

	/*
	 * XXX	NOTE: May want to consider converting this to a page list
	 * XXX	vm_map_copy interface.  Need to understand object
	 * XXX	coalescing implications before doing so.
	 */

	update_cow = ((flags & MEMORY_OBJECT_DATA_FLUSH)
	    && (!(flags & MEMORY_OBJECT_DATA_NO_CHANGE) &&
	    !(flags & MEMORY_OBJECT_DATA_PURGE)))
	    || (flags & MEMORY_OBJECT_COPY_SYNC);

	if (update_cow || (flags & (MEMORY_OBJECT_DATA_PURGE | MEMORY_OBJECT_DATA_SYNC))) {
		int collisions = 0;

		while ((copy_object = object->copy) != VM_OBJECT_NULL) {
			/*
			 * need to do a try here since we're swimming upstream
			 * against the normal lock ordering... however, we need
			 * to hold the object stable until we gain control of the
			 * copy object so we have to be careful how we approach this
			 */
			if (vm_object_lock_try(copy_object)) {
				/*
				 * we 'won' the lock on the copy object...
				 * no need to hold the object lock any longer...
				 * take a real reference on the copy object because
				 * we're going to call vm_fault_page on it which may
				 * under certain conditions drop the lock and the paging
				 * reference we're about to take... the reference
				 * will keep the copy object from going away if that happens
				 */
				vm_object_unlock(object);
				vm_object_reference_locked(copy_object);
				break;
			}
			vm_object_unlock(object);

			collisions++;
			mutex_pause(collisions);

			vm_object_lock(object);
		}
	}
	if ((copy_object != VM_OBJECT_NULL && update_cow) || (flags & MEMORY_OBJECT_DATA_SYNC)) {
		vm_map_size_t           i;
		vm_map_size_t           copy_size;
		vm_map_offset_t         copy_offset;
		vm_prot_t               prot;
		vm_page_t               page;
		vm_page_t               top_page;
		kern_return_t           error = 0;
		struct vm_object_fault_info fault_info = {};

		if (copy_object != VM_OBJECT_NULL) {
			/*
			 * translate offset with respect to shadow's offset
			 */
			copy_offset = (offset >= copy_object->vo_shadow_offset) ?
			    (vm_map_offset_t)(offset - copy_object->vo_shadow_offset) :
			    (vm_map_offset_t) 0;

			if (copy_offset > copy_object->vo_size) {
				copy_offset = copy_object->vo_size;
			}

			/*
			 * clip size with respect to shadow offset
			 */
			if (offset >= copy_object->vo_shadow_offset) {
				copy_size = size;
			} else if (size >= copy_object->vo_shadow_offset - offset) {
				copy_size = size - (copy_object->vo_shadow_offset - offset);
			} else {
				copy_size = 0;
			}

			if (copy_offset + copy_size > copy_object->vo_size) {
				if (copy_object->vo_size >= copy_offset) {
					copy_size = copy_object->vo_size - copy_offset;
				} else {
					copy_size = 0;
				}
			}
			copy_size += copy_offset;
		} else {
			copy_object = object;

			copy_size   = offset + size;
			copy_offset = offset;
		}
		fault_info.interruptible = THREAD_UNINT;
		fault_info.behavior  = VM_BEHAVIOR_SEQUENTIAL;
		fault_info.lo_offset = copy_offset;
		fault_info.hi_offset = copy_size;
		fault_info.stealth = TRUE;
		assert(fault_info.cs_bypass == FALSE);
		assert(fault_info.pmap_cs_associated == FALSE);

		vm_object_paging_begin(copy_object);

		for (i = copy_offset; i < copy_size; i += PAGE_SIZE) {
RETRY_COW_OF_LOCK_REQUEST:
			fault_info.cluster_size = (vm_size_t) (copy_size - i);
			assert(fault_info.cluster_size == copy_size - i);

			prot =  VM_PROT_WRITE | VM_PROT_READ;
			page = VM_PAGE_NULL;
			result = vm_fault_page(copy_object, i,
			    VM_PROT_WRITE | VM_PROT_READ,
			    FALSE,
			    FALSE,                    /* page not looked up */
			    &prot,
			    &page,
			    &top_page,
			    (int *)0,
			    &error,
			    FALSE,
			    FALSE, &fault_info);

			switch (result) {
			case VM_FAULT_SUCCESS:
				if (top_page) {
					vm_fault_cleanup(
						VM_PAGE_OBJECT(page), top_page);
					vm_object_lock(copy_object);
					vm_object_paging_begin(copy_object);
				}
				if ((!VM_PAGE_NON_SPECULATIVE_PAGEABLE(page))) {
					vm_page_lockspin_queues();

					if ((!VM_PAGE_NON_SPECULATIVE_PAGEABLE(page))) {
						vm_page_deactivate(page);
					}
					vm_page_unlock_queues();
				}
				PAGE_WAKEUP_DONE(page);
				break;
			case VM_FAULT_RETRY:
				prot =  VM_PROT_WRITE | VM_PROT_READ;
				vm_object_lock(copy_object);
				vm_object_paging_begin(copy_object);
				goto RETRY_COW_OF_LOCK_REQUEST;
			case VM_FAULT_INTERRUPTED:
				prot =  VM_PROT_WRITE | VM_PROT_READ;
				vm_object_lock(copy_object);
				vm_object_paging_begin(copy_object);
				goto RETRY_COW_OF_LOCK_REQUEST;
			case VM_FAULT_MEMORY_SHORTAGE:
				VM_PAGE_WAIT();
				prot =  VM_PROT_WRITE | VM_PROT_READ;
				vm_object_lock(copy_object);
				vm_object_paging_begin(copy_object);
				goto RETRY_COW_OF_LOCK_REQUEST;
			case VM_FAULT_SUCCESS_NO_VM_PAGE:
				/* success but no VM page: fail */
				vm_object_paging_end(copy_object);
				vm_object_unlock(copy_object);
			/*FALLTHROUGH*/
			case VM_FAULT_MEMORY_ERROR:
				if (object != copy_object) {
					vm_object_deallocate(copy_object);
				}
				vm_object_lock(object);
				goto BYPASS_COW_COPYIN;
			default:
				panic("vm_object_update: unexpected error 0x%x"
				    " from vm_fault_page()\n", result);
			}
		}
		vm_object_paging_end(copy_object);
	}
	if ((flags & (MEMORY_OBJECT_DATA_SYNC | MEMORY_OBJECT_COPY_SYNC))) {
		if (copy_object != VM_OBJECT_NULL && copy_object != object) {
			vm_object_unlock(copy_object);
			vm_object_deallocate(copy_object);
			vm_object_lock(object);
		}
		return KERN_SUCCESS;
	}
	if (copy_object != VM_OBJECT_NULL && copy_object != object) {
		if ((flags & MEMORY_OBJECT_DATA_PURGE)) {
			vm_object_lock_assert_exclusive(copy_object);
			copy_object->shadow_severed = TRUE;
			copy_object->shadowed = FALSE;
			copy_object->shadow = NULL;
			/*
			 * delete the ref the COW was holding on the target object
			 */
			vm_object_deallocate(object);
		}
		vm_object_unlock(copy_object);
		vm_object_deallocate(copy_object);
		vm_object_lock(object);
	}
BYPASS_COW_COPYIN:

	/*
	 * when we have a really large range to check relative
	 * to the number of actual resident pages, we'd like
	 * to use the resident page list to drive our checks
	 * however, the object lock will get dropped while processing
	 * the page which means the resident queue can change which
	 * means we can't walk the queue as we process the pages
	 * we also want to do the processing in offset order to allow
	 * 'runs' of pages to be collected if we're being told to
	 * flush to disk... the resident page queue is NOT ordered.
	 *
	 * a temporary solution (until we figure out how to deal with
	 * large address spaces more generically) is to pre-flight
	 * the resident page queue (if it's small enough) and develop
	 * a collection of extents (that encompass actual resident pages)
	 * to visit.  This will at least allow us to deal with some of the
	 * more pathological cases in a more efficient manner.  The current
	 * worst case (a single resident page at the end of an extremely large
	 * range) can take minutes to complete for ranges in the terrabyte
	 * category... since this routine is called when truncating a file,
	 * and we currently support files up to 16 Tbytes in size, this
	 * is not a theoretical problem
	 */

	if ((object->resident_page_count < RESIDENT_LIMIT) &&
	    (atop_64(size) > (unsigned)(object->resident_page_count / (8 * MAX_EXTENTS)))) {
		vm_page_t               next;
		vm_object_offset_t      start;
		vm_object_offset_t      end;
		vm_object_size_t        e_mask;
		vm_page_t               m;

		start = offset;
		end   = offset + size;
		num_of_extents = 0;
		e_mask = ~((vm_object_size_t)(EXTENT_SIZE - 1));

		m = (vm_page_t) vm_page_queue_first(&object->memq);

		while (!vm_page_queue_end(&object->memq, (vm_page_queue_entry_t) m)) {
			next = (vm_page_t) vm_page_queue_next(&m->vmp_listq);

			if ((m->vmp_offset >= start) && (m->vmp_offset < end)) {
				/*
				 * this is a page we're interested in
				 * try to fit it into a current extent
				 */
				for (n = 0; n < num_of_extents; n++) {
					if ((m->vmp_offset & e_mask) == extents[n].e_base) {
						/*
						 * use (PAGE_SIZE - 1) to determine the
						 * max offset so that we don't wrap if
						 * we're at the last page of the space
						 */
						if (m->vmp_offset < extents[n].e_min) {
							extents[n].e_min = m->vmp_offset;
						} else if ((m->vmp_offset + (PAGE_SIZE - 1)) > extents[n].e_max) {
							extents[n].e_max = m->vmp_offset + (PAGE_SIZE - 1);
						}
						break;
					}
				}
				if (n == num_of_extents) {
					/*
					 * didn't find a current extent that can encompass
					 * this page
					 */
					if (n < MAX_EXTENTS) {
						/*
						 * if we still have room,
						 * create a new extent
						 */
						extents[n].e_base = m->vmp_offset & e_mask;
						extents[n].e_min  = m->vmp_offset;
						extents[n].e_max  = m->vmp_offset + (PAGE_SIZE - 1);

						num_of_extents++;
					} else {
						/*
						 * no room to create a new extent...
						 * fall back to a single extent based
						 * on the min and max page offsets
						 * we find in the range we're interested in...
						 * first, look through the extent list and
						 * develop the overall min and max for the
						 * pages we've looked at up to this point
						 */
						for (n = 1; n < num_of_extents; n++) {
							if (extents[n].e_min < extents[0].e_min) {
								extents[0].e_min = extents[n].e_min;
							}
							if (extents[n].e_max > extents[0].e_max) {
								extents[0].e_max = extents[n].e_max;
							}
						}
						/*
						 * now setup to run through the remaining pages
						 * to determine the overall min and max
						 * offset for the specified range
						 */
						extents[0].e_base = 0;
						e_mask = 0;
						num_of_extents = 1;

						/*
						 * by continuing, we'll reprocess the
						 * page that forced us to abandon trying
						 * to develop multiple extents
						 */
						continue;
					}
				}
			}
			m = next;
		}
	} else {
		extents[0].e_min = offset;
		extents[0].e_max = offset + (size - 1);

		num_of_extents = 1;
	}
	for (n = 0; n < num_of_extents; n++) {
		if (vm_object_update_extent(object, extents[n].e_min, extents[n].e_max, resid_offset, io_errno,
		    should_flush, should_return, should_iosync, protection)) {
			data_returned = TRUE;
		}
	}
	return data_returned;
}


static kern_return_t
vm_object_set_attributes_common(
	vm_object_t     object,
	boolean_t       may_cache,
	memory_object_copy_strategy_t copy_strategy)
{
	boolean_t       object_became_ready;

	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	/*
	 *	Verify the attributes of importance
	 */

	switch (copy_strategy) {
	case MEMORY_OBJECT_COPY_NONE:
	case MEMORY_OBJECT_COPY_DELAY:
		break;
	default:
		return KERN_INVALID_ARGUMENT;
	}

	if (may_cache) {
		may_cache = TRUE;
	}

	vm_object_lock(object);

	/*
	 *	Copy the attributes
	 */
	assert(!object->internal);
	object_became_ready = !object->pager_ready;
	object->copy_strategy = copy_strategy;
	object->can_persist = may_cache;

	/*
	 *	Wake up anyone waiting for the ready attribute
	 *	to become asserted.
	 */

	if (object_became_ready) {
		object->pager_ready = TRUE;
		vm_object_wakeup(object, VM_OBJECT_EVENT_PAGER_READY);
	}

	vm_object_unlock(object);

	return KERN_SUCCESS;
}


kern_return_t
memory_object_synchronize_completed(
	__unused    memory_object_control_t control,
	__unused    memory_object_offset_t  offset,
	__unused    memory_object_size_t    length)
{
	panic("memory_object_synchronize_completed no longer supported\n");
	return KERN_FAILURE;
}


/*
 *	Set the memory object attribute as provided.
 *
 *	XXX This routine cannot be completed until the vm_msync, clean
 *	     in place, and cluster work is completed. See ifdef notyet
 *	     below and note that vm_object_set_attributes_common()
 *	     may have to be expanded.
 */
kern_return_t
memory_object_change_attributes(
	memory_object_control_t         control,
	memory_object_flavor_t          flavor,
	memory_object_info_t            attributes,
	mach_msg_type_number_t          count)
{
	vm_object_t                     object;
	kern_return_t                   result = KERN_SUCCESS;
	boolean_t                       may_cache;
	boolean_t                       invalidate;
	memory_object_copy_strategy_t   copy_strategy;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	vm_object_lock(object);

	may_cache = object->can_persist;
	copy_strategy = object->copy_strategy;
#if notyet
	invalidate = object->invalidate;
#endif
	vm_object_unlock(object);

	switch (flavor) {
	case OLD_MEMORY_OBJECT_BEHAVIOR_INFO:
	{
		old_memory_object_behave_info_t     behave;

		if (count != OLD_MEMORY_OBJECT_BEHAVE_INFO_COUNT) {
			result = KERN_INVALID_ARGUMENT;
			break;
		}

		behave = (old_memory_object_behave_info_t) attributes;

		invalidate = behave->invalidate;
		copy_strategy = behave->copy_strategy;

		break;
	}

	case MEMORY_OBJECT_BEHAVIOR_INFO:
	{
		memory_object_behave_info_t     behave;

		if (count != MEMORY_OBJECT_BEHAVE_INFO_COUNT) {
			result = KERN_INVALID_ARGUMENT;
			break;
		}

		behave = (memory_object_behave_info_t) attributes;

		invalidate = behave->invalidate;
		copy_strategy = behave->copy_strategy;
		break;
	}

	case MEMORY_OBJECT_PERFORMANCE_INFO:
	{
		memory_object_perf_info_t       perf;

		if (count != MEMORY_OBJECT_PERF_INFO_COUNT) {
			result = KERN_INVALID_ARGUMENT;
			break;
		}

		perf = (memory_object_perf_info_t) attributes;

		may_cache = perf->may_cache;

		break;
	}

	case OLD_MEMORY_OBJECT_ATTRIBUTE_INFO:
	{
		old_memory_object_attr_info_t   attr;

		if (count != OLD_MEMORY_OBJECT_ATTR_INFO_COUNT) {
			result = KERN_INVALID_ARGUMENT;
			break;
		}

		attr = (old_memory_object_attr_info_t) attributes;

		may_cache = attr->may_cache;
		copy_strategy = attr->copy_strategy;

		break;
	}

	case MEMORY_OBJECT_ATTRIBUTE_INFO:
	{
		memory_object_attr_info_t       attr;

		if (count != MEMORY_OBJECT_ATTR_INFO_COUNT) {
			result = KERN_INVALID_ARGUMENT;
			break;
		}

		attr = (memory_object_attr_info_t) attributes;

		copy_strategy = attr->copy_strategy;
		may_cache = attr->may_cache_object;

		break;
	}

	default:
		result = KERN_INVALID_ARGUMENT;
		break;
	}

	if (result != KERN_SUCCESS) {
		return result;
	}

	if (copy_strategy == MEMORY_OBJECT_COPY_TEMPORARY) {
		copy_strategy = MEMORY_OBJECT_COPY_DELAY;
	}

	/*
	 * XXX	may_cache may become a tri-valued variable to handle
	 * XXX	uncache if not in use.
	 */
	return vm_object_set_attributes_common(object,
	           may_cache,
	           copy_strategy);
}

kern_return_t
memory_object_get_attributes(
	memory_object_control_t control,
	memory_object_flavor_t  flavor,
	memory_object_info_t    attributes,     /* pointer to OUT array */
	mach_msg_type_number_t  *count)         /* IN/OUT */
{
	kern_return_t           ret = KERN_SUCCESS;
	vm_object_t             object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	vm_object_lock(object);

	switch (flavor) {
	case OLD_MEMORY_OBJECT_BEHAVIOR_INFO:
	{
		old_memory_object_behave_info_t behave;

		if (*count < OLD_MEMORY_OBJECT_BEHAVE_INFO_COUNT) {
			ret = KERN_INVALID_ARGUMENT;
			break;
		}

		behave = (old_memory_object_behave_info_t) attributes;
		behave->copy_strategy = object->copy_strategy;
		behave->temporary = FALSE;
#if notyet      /* remove when vm_msync complies and clean in place fini */
		behave->invalidate = object->invalidate;
#else
		behave->invalidate = FALSE;
#endif

		*count = OLD_MEMORY_OBJECT_BEHAVE_INFO_COUNT;
		break;
	}

	case MEMORY_OBJECT_BEHAVIOR_INFO:
	{
		memory_object_behave_info_t     behave;

		if (*count < MEMORY_OBJECT_BEHAVE_INFO_COUNT) {
			ret = KERN_INVALID_ARGUMENT;
			break;
		}

		behave = (memory_object_behave_info_t) attributes;
		behave->copy_strategy = object->copy_strategy;
		behave->temporary = FALSE;
#if notyet      /* remove when vm_msync complies and clean in place fini */
		behave->invalidate = object->invalidate;
#else
		behave->invalidate = FALSE;
#endif
		behave->advisory_pageout = FALSE;
		behave->silent_overwrite = FALSE;
		*count = MEMORY_OBJECT_BEHAVE_INFO_COUNT;
		break;
	}

	case MEMORY_OBJECT_PERFORMANCE_INFO:
	{
		memory_object_perf_info_t       perf;

		if (*count < MEMORY_OBJECT_PERF_INFO_COUNT) {
			ret = KERN_INVALID_ARGUMENT;
			break;
		}

		perf = (memory_object_perf_info_t) attributes;
		perf->cluster_size = PAGE_SIZE;
		perf->may_cache = object->can_persist;

		*count = MEMORY_OBJECT_PERF_INFO_COUNT;
		break;
	}

	case OLD_MEMORY_OBJECT_ATTRIBUTE_INFO:
	{
		old_memory_object_attr_info_t       attr;

		if (*count < OLD_MEMORY_OBJECT_ATTR_INFO_COUNT) {
			ret = KERN_INVALID_ARGUMENT;
			break;
		}

		attr = (old_memory_object_attr_info_t) attributes;
		attr->may_cache = object->can_persist;
		attr->copy_strategy = object->copy_strategy;

		*count = OLD_MEMORY_OBJECT_ATTR_INFO_COUNT;
		break;
	}

	case MEMORY_OBJECT_ATTRIBUTE_INFO:
	{
		memory_object_attr_info_t       attr;

		if (*count < MEMORY_OBJECT_ATTR_INFO_COUNT) {
			ret = KERN_INVALID_ARGUMENT;
			break;
		}

		attr = (memory_object_attr_info_t) attributes;
		attr->copy_strategy = object->copy_strategy;
		attr->cluster_size = PAGE_SIZE;
		attr->may_cache_object = object->can_persist;
		attr->temporary = FALSE;

		*count = MEMORY_OBJECT_ATTR_INFO_COUNT;
		break;
	}

	default:
		ret = KERN_INVALID_ARGUMENT;
		break;
	}

	vm_object_unlock(object);

	return ret;
}


kern_return_t
memory_object_iopl_request(
	ipc_port_t              port,
	memory_object_offset_t  offset,
	upl_size_t              *upl_size,
	upl_t                   *upl_ptr,
	upl_page_info_array_t   user_page_list,
	unsigned int            *page_list_count,
	upl_control_flags_t     *flags,
	vm_tag_t                tag)
{
	vm_object_t             object;
	kern_return_t           ret;
	upl_control_flags_t     caller_flags;

	caller_flags = *flags;

	if (caller_flags & ~UPL_VALID_FLAGS) {
		/*
		 * For forward compatibility's sake,
		 * reject any unknown flag.
		 */
		return KERN_INVALID_VALUE;
	}

	if (ip_kotype(port) == IKOT_NAMED_ENTRY) {
		vm_named_entry_t        named_entry;

		named_entry = (vm_named_entry_t)port->ip_kobject;
		/* a few checks to make sure user is obeying rules */
		if (*upl_size == 0) {
			if (offset >= named_entry->size) {
				return KERN_INVALID_RIGHT;
			}
			*upl_size = (upl_size_t)(named_entry->size - offset);
			if (*upl_size != named_entry->size - offset) {
				return KERN_INVALID_ARGUMENT;
			}
		}
		if (caller_flags & UPL_COPYOUT_FROM) {
			if ((named_entry->protection & VM_PROT_READ)
			    != VM_PROT_READ) {
				return KERN_INVALID_RIGHT;
			}
		} else {
			if ((named_entry->protection &
			    (VM_PROT_READ | VM_PROT_WRITE))
			    != (VM_PROT_READ | VM_PROT_WRITE)) {
				return KERN_INVALID_RIGHT;
			}
		}
		if (named_entry->size < (offset + *upl_size)) {
			return KERN_INVALID_ARGUMENT;
		}

		/* the callers parameter offset is defined to be the */
		/* offset from beginning of named entry offset in object */
		offset = offset + named_entry->offset;

		if (named_entry->is_sub_map ||
		    named_entry->is_copy) {
			return KERN_INVALID_ARGUMENT;
		}

		named_entry_lock(named_entry);

		object = named_entry->backing.object;
		vm_object_reference(object);
		named_entry_unlock(named_entry);
	} else if (ip_kotype(port) == IKOT_MEM_OBJ_CONTROL) {
		memory_object_control_t control;
		control = (memory_object_control_t) port;
		if (control == NULL) {
			return KERN_INVALID_ARGUMENT;
		}
		object = memory_object_control_to_vm_object(control);
		if (object == VM_OBJECT_NULL) {
			return KERN_INVALID_ARGUMENT;
		}
		vm_object_reference(object);
	} else {
		return KERN_INVALID_ARGUMENT;
	}
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	if (!object->private) {
		if (object->phys_contiguous) {
			*flags = UPL_PHYS_CONTIG;
		} else {
			*flags = 0;
		}
	} else {
		*flags = UPL_DEV_MEMORY | UPL_PHYS_CONTIG;
	}

	ret = vm_object_iopl_request(object,
	    offset,
	    *upl_size,
	    upl_ptr,
	    user_page_list,
	    page_list_count,
	    caller_flags,
	    tag);
	vm_object_deallocate(object);
	return ret;
}

/*
 *	Routine:	memory_object_upl_request [interface]
 *	Purpose:
 *		Cause the population of a portion of a vm_object.
 *		Depending on the nature of the request, the pages
 *		returned may be contain valid data or be uninitialized.
 *
 */

kern_return_t
memory_object_upl_request(
	memory_object_control_t control,
	memory_object_offset_t  offset,
	upl_size_t              size,
	upl_t                   *upl_ptr,
	upl_page_info_array_t   user_page_list,
	unsigned int            *page_list_count,
	int                     cntrl_flags,
	int                     tag)
{
	vm_object_t             object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_TERMINATED;
	}

	return vm_object_upl_request(object,
	           offset,
	           size,
	           upl_ptr,
	           user_page_list,
	           page_list_count,
	           (upl_control_flags_t)(unsigned int) cntrl_flags,
	           tag);
}

/*
 *	Routine:	memory_object_super_upl_request [interface]
 *	Purpose:
 *		Cause the population of a portion of a vm_object
 *		in much the same way as memory_object_upl_request.
 *		Depending on the nature of the request, the pages
 *		returned may be contain valid data or be uninitialized.
 *		However, the region may be expanded up to the super
 *		cluster size provided.
 */

kern_return_t
memory_object_super_upl_request(
	memory_object_control_t control,
	memory_object_offset_t  offset,
	upl_size_t              size,
	upl_size_t              super_cluster,
	upl_t                   *upl,
	upl_page_info_t         *user_page_list,
	unsigned int            *page_list_count,
	int                     cntrl_flags,
	int                     tag)
{
	vm_object_t             object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	return vm_object_super_upl_request(object,
	           offset,
	           size,
	           super_cluster,
	           upl,
	           user_page_list,
	           page_list_count,
	           (upl_control_flags_t)(unsigned int) cntrl_flags,
	           tag);
}

kern_return_t
memory_object_cluster_size(
	memory_object_control_t control,
	memory_object_offset_t  *start,
	vm_size_t               *length,
	uint32_t                *io_streaming,
	memory_object_fault_info_t mo_fault_info)
{
	vm_object_t             object;
	vm_object_fault_info_t  fault_info;

	object = memory_object_control_to_vm_object(control);

	if (object == VM_OBJECT_NULL || object->paging_offset > *start) {
		return KERN_INVALID_ARGUMENT;
	}

	*start -= object->paging_offset;

	fault_info = (vm_object_fault_info_t)(uintptr_t) mo_fault_info;
	vm_object_cluster_size(object,
	    (vm_object_offset_t *)start,
	    length,
	    fault_info,
	    io_streaming);

	*start += object->paging_offset;

	return KERN_SUCCESS;
}


/*
 *	Routine:	host_default_memory_manager [interface]
 *	Purpose:
 *		set/get the default memory manager port and default cluster
 *		size.
 *
 *		If successful, consumes the supplied naked send right.
 */
kern_return_t
host_default_memory_manager(
	host_priv_t             host_priv,
	memory_object_default_t *default_manager,
	__unused memory_object_cluster_size_t cluster_size)
{
	memory_object_default_t current_manager;
	memory_object_default_t new_manager;
	memory_object_default_t returned_manager;
	kern_return_t result = KERN_SUCCESS;

	if (host_priv == HOST_PRIV_NULL) {
		return KERN_INVALID_HOST;
	}

	assert(host_priv == &realhost);

	new_manager = *default_manager;
	lck_mtx_lock(&memory_manager_default_lock);
	current_manager = memory_manager_default;
	returned_manager = MEMORY_OBJECT_DEFAULT_NULL;

	if (new_manager == MEMORY_OBJECT_DEFAULT_NULL) {
		/*
		 *	Retrieve the current value.
		 */
		returned_manager = current_manager;
		memory_object_default_reference(returned_manager);
	} else {
		/*
		 *	Only allow the kernel to change the value.
		 */
		extern task_t kernel_task;
		if (current_task() != kernel_task) {
			result = KERN_NO_ACCESS;
			goto out;
		}

		/*
		 *	If this is the first non-null manager, start
		 *	up the internal pager support.
		 */
		if (current_manager == MEMORY_OBJECT_DEFAULT_NULL) {
			result = vm_pageout_internal_start();
			if (result != KERN_SUCCESS) {
				goto out;
			}
		}

		/*
		 *	Retrieve the current value,
		 *	and replace it with the supplied value.
		 *	We return the old reference to the caller
		 *	but we have to take a reference on the new
		 *	one.
		 */
		returned_manager = current_manager;
		memory_manager_default = new_manager;
		memory_object_default_reference(new_manager);

		/*
		 *	In case anyone's been waiting for a memory
		 *	manager to be established, wake them up.
		 */

		thread_wakeup((event_t) &memory_manager_default);

		/*
		 * Now that we have a default pager for anonymous memory,
		 * reactivate all the throttled pages (i.e. dirty pages with
		 * no pager).
		 */
		if (current_manager == MEMORY_OBJECT_DEFAULT_NULL) {
			vm_page_reactivate_all_throttled();
		}
	}
out:
	lck_mtx_unlock(&memory_manager_default_lock);

	*default_manager = returned_manager;
	return result;
}

/*
 *	Routine:	memory_manager_default_reference
 *	Purpose:
 *		Returns a naked send right for the default
 *		memory manager.  The returned right is always
 *		valid (not IP_NULL or IP_DEAD).
 */

__private_extern__ memory_object_default_t
memory_manager_default_reference(void)
{
	memory_object_default_t current_manager;

	lck_mtx_lock(&memory_manager_default_lock);
	current_manager = memory_manager_default;
	while (current_manager == MEMORY_OBJECT_DEFAULT_NULL) {
		wait_result_t res;

		res = lck_mtx_sleep(&memory_manager_default_lock,
		    LCK_SLEEP_DEFAULT,
		    (event_t) &memory_manager_default,
		    THREAD_UNINT);
		assert(res == THREAD_AWAKENED);
		current_manager = memory_manager_default;
	}
	memory_object_default_reference(current_manager);
	lck_mtx_unlock(&memory_manager_default_lock);

	return current_manager;
}

/*
 *	Routine:	memory_manager_default_check
 *
 *	Purpose:
 *		Check whether a default memory manager has been set
 *		up yet, or not. Returns KERN_SUCCESS if dmm exists,
 *		and KERN_FAILURE if dmm does not exist.
 *
 *		If there is no default memory manager, log an error,
 *		but only the first time.
 *
 */
__private_extern__ kern_return_t
memory_manager_default_check(void)
{
	memory_object_default_t current;

	lck_mtx_lock(&memory_manager_default_lock);
	current = memory_manager_default;
	if (current == MEMORY_OBJECT_DEFAULT_NULL) {
		static boolean_t logged;        /* initialized to 0 */
		boolean_t       complain = !logged;
		logged = TRUE;
		lck_mtx_unlock(&memory_manager_default_lock);
		if (complain) {
			printf("Warning: No default memory manager\n");
		}
		return KERN_FAILURE;
	} else {
		lck_mtx_unlock(&memory_manager_default_lock);
		return KERN_SUCCESS;
	}
}

__private_extern__ void
memory_manager_default_init(void)
{
	memory_manager_default = MEMORY_OBJECT_DEFAULT_NULL;
	lck_mtx_init(&memory_manager_default_lock, &vm_object_lck_grp, &vm_object_lck_attr);
}



/* Allow manipulation of individual page state.  This is actually part of */
/* the UPL regimen but takes place on the object rather than on a UPL */

kern_return_t
memory_object_page_op(
	memory_object_control_t control,
	memory_object_offset_t  offset,
	int                     ops,
	ppnum_t                 *phys_entry,
	int                     *flags)
{
	vm_object_t             object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	return vm_object_page_op(object, offset, ops, phys_entry, flags);
}

/*
 * memory_object_range_op offers performance enhancement over
 * memory_object_page_op for page_op functions which do not require page
 * level state to be returned from the call.  Page_op was created to provide
 * a low-cost alternative to page manipulation via UPLs when only a single
 * page was involved.  The range_op call establishes the ability in the _op
 * family of functions to work on multiple pages where the lack of page level
 * state handling allows the caller to avoid the overhead of the upl structures.
 */

kern_return_t
memory_object_range_op(
	memory_object_control_t control,
	memory_object_offset_t  offset_beg,
	memory_object_offset_t  offset_end,
	int                     ops,
	int                     *range)
{
	vm_object_t             object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	return vm_object_range_op(object,
	           offset_beg,
	           offset_end,
	           ops,
	           (uint32_t *) range);
}


void
memory_object_mark_used(
	memory_object_control_t control)
{
	vm_object_t             object;

	if (control == NULL) {
		return;
	}

	object = memory_object_control_to_vm_object(control);

	if (object != VM_OBJECT_NULL) {
		vm_object_cache_remove(object);
	}
}


void
memory_object_mark_unused(
	memory_object_control_t control,
	__unused boolean_t      rage)
{
	vm_object_t             object;

	if (control == NULL) {
		return;
	}

	object = memory_object_control_to_vm_object(control);

	if (object != VM_OBJECT_NULL) {
		vm_object_cache_add(object);
	}
}

void
memory_object_mark_io_tracking(
	memory_object_control_t control)
{
	vm_object_t             object;

	if (control == NULL) {
		return;
	}
	object = memory_object_control_to_vm_object(control);

	if (object != VM_OBJECT_NULL) {
		vm_object_lock(object);
		object->io_tracking = TRUE;
		vm_object_unlock(object);
	}
}

void
memory_object_mark_trusted(
	memory_object_control_t control)
{
	vm_object_t             object;

	if (control == NULL) {
		return;
	}
	object = memory_object_control_to_vm_object(control);

	if (object != VM_OBJECT_NULL) {
		vm_object_lock(object);
		object->pager_trusted = TRUE;
		vm_object_unlock(object);
	}
}

#if CONFIG_SECLUDED_MEMORY
void
memory_object_mark_eligible_for_secluded(
	memory_object_control_t control,
	boolean_t               eligible_for_secluded)
{
	vm_object_t             object;

	if (control == NULL) {
		return;
	}
	object = memory_object_control_to_vm_object(control);

	if (object == VM_OBJECT_NULL) {
		return;
	}

	vm_object_lock(object);
	if (eligible_for_secluded &&
	    secluded_for_filecache && /* global boot-arg */
	    !object->eligible_for_secluded) {
		object->eligible_for_secluded = TRUE;
		vm_page_secluded.eligible_for_secluded += object->resident_page_count;
	} else if (!eligible_for_secluded &&
	    object->eligible_for_secluded) {
		object->eligible_for_secluded = FALSE;
		vm_page_secluded.eligible_for_secluded -= object->resident_page_count;
		if (object->resident_page_count) {
			/* XXX FBDP TODO: flush pages from secluded queue? */
			// printf("FBDP TODO: flush %d pages from %p from secluded queue\n", object->resident_page_count, object);
		}
	}
	vm_object_unlock(object);
}
#endif /* CONFIG_SECLUDED_MEMORY */

kern_return_t
memory_object_pages_resident(
	memory_object_control_t control,
	boolean_t                       *       has_pages_resident)
{
	vm_object_t             object;

	*has_pages_resident = FALSE;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	if (object->resident_page_count) {
		*has_pages_resident = TRUE;
	}

	return KERN_SUCCESS;
}

kern_return_t
memory_object_signed(
	memory_object_control_t control,
	boolean_t               is_signed)
{
	vm_object_t     object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return KERN_INVALID_ARGUMENT;
	}

	vm_object_lock(object);
	object->code_signed = is_signed;
	vm_object_unlock(object);

	return KERN_SUCCESS;
}

boolean_t
memory_object_is_signed(
	memory_object_control_t control)
{
	boolean_t       is_signed;
	vm_object_t     object;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return FALSE;
	}

	vm_object_lock_shared(object);
	is_signed = object->code_signed;
	vm_object_unlock(object);

	return is_signed;
}

boolean_t
memory_object_is_shared_cache(
	memory_object_control_t control)
{
	vm_object_t     object = VM_OBJECT_NULL;

	object = memory_object_control_to_vm_object(control);
	if (object == VM_OBJECT_NULL) {
		return FALSE;
	}

	return object->object_is_shared_cache;
}

static zone_t mem_obj_control_zone;

__private_extern__ void
memory_object_control_bootstrap(void)
{
	int     i;

	i = (vm_size_t) sizeof(struct memory_object_control);
	mem_obj_control_zone = zinit(i, 8192 * i, 4096, "mem_obj_control");
	zone_change(mem_obj_control_zone, Z_CALLERACCT, FALSE);
	zone_change(mem_obj_control_zone, Z_NOENCRYPT, TRUE);
	return;
}

__private_extern__ memory_object_control_t
memory_object_control_allocate(
	vm_object_t             object)
{
	memory_object_control_t control;

	control = (memory_object_control_t)zalloc(mem_obj_control_zone);
	if (control != MEMORY_OBJECT_CONTROL_NULL) {
		control->moc_object = object;
		control->moc_ikot = IKOT_MEM_OBJ_CONTROL; /* fake ip_kotype */
	}
	return control;
}

__private_extern__ void
memory_object_control_collapse(
	memory_object_control_t control,
	vm_object_t             object)
{
	assert((control->moc_object != VM_OBJECT_NULL) &&
	    (control->moc_object != object));
	control->moc_object = object;
}

__private_extern__ vm_object_t
memory_object_control_to_vm_object(
	memory_object_control_t control)
{
	if (control == MEMORY_OBJECT_CONTROL_NULL ||
	    control->moc_ikot != IKOT_MEM_OBJ_CONTROL) {
		return VM_OBJECT_NULL;
	}

	return control->moc_object;
}

__private_extern__ vm_object_t
memory_object_to_vm_object(
	memory_object_t mem_obj)
{
	memory_object_control_t mo_control;

	if (mem_obj == MEMORY_OBJECT_NULL) {
		return VM_OBJECT_NULL;
	}
	mo_control = mem_obj->mo_control;
	if (mo_control == NULL) {
		return VM_OBJECT_NULL;
	}
	return memory_object_control_to_vm_object(mo_control);
}

memory_object_control_t
convert_port_to_mo_control(
	__unused mach_port_t    port)
{
	return MEMORY_OBJECT_CONTROL_NULL;
}


mach_port_t
convert_mo_control_to_port(
	__unused memory_object_control_t        control)
{
	return MACH_PORT_NULL;
}

void
memory_object_control_reference(
	__unused memory_object_control_t        control)
{
	return;
}

/*
 * We only every issue one of these references, so kill it
 * when that gets released (should switch the real reference
 * counting in true port-less EMMI).
 */
void
memory_object_control_deallocate(
	memory_object_control_t control)
{
	zfree(mem_obj_control_zone, control);
}

void
memory_object_control_disable(
	memory_object_control_t control)
{
	assert(control->moc_object != VM_OBJECT_NULL);
	control->moc_object = VM_OBJECT_NULL;
}

void
memory_object_default_reference(
	memory_object_default_t dmm)
{
	ipc_port_make_send(dmm);
}

void
memory_object_default_deallocate(
	memory_object_default_t dmm)
{
	ipc_port_release_send(dmm);
}

memory_object_t
convert_port_to_memory_object(
	__unused mach_port_t    port)
{
	return MEMORY_OBJECT_NULL;
}


mach_port_t
convert_memory_object_to_port(
	__unused memory_object_t        object)
{
	return MACH_PORT_NULL;
}


/* Routine memory_object_reference */
void
memory_object_reference(
	memory_object_t memory_object)
{
	(memory_object->mo_pager_ops->memory_object_reference)(
		memory_object);
}

/* Routine memory_object_deallocate */
void
memory_object_deallocate(
	memory_object_t memory_object)
{
	(memory_object->mo_pager_ops->memory_object_deallocate)(
		memory_object);
}


/* Routine memory_object_init */
kern_return_t
memory_object_init
(
	memory_object_t memory_object,
	memory_object_control_t memory_control,
	memory_object_cluster_size_t memory_object_page_size
)
{
	return (memory_object->mo_pager_ops->memory_object_init)(
		memory_object,
		memory_control,
		memory_object_page_size);
}

/* Routine memory_object_terminate */
kern_return_t
memory_object_terminate
(
	memory_object_t memory_object
)
{
	return (memory_object->mo_pager_ops->memory_object_terminate)(
		memory_object);
}

/* Routine memory_object_data_request */
kern_return_t
memory_object_data_request
(
	memory_object_t memory_object,
	memory_object_offset_t offset,
	memory_object_cluster_size_t length,
	vm_prot_t desired_access,
	memory_object_fault_info_t fault_info
)
{
	return (memory_object->mo_pager_ops->memory_object_data_request)(
		memory_object,
		offset,
		length,
		desired_access,
		fault_info);
}

/* Routine memory_object_data_return */
kern_return_t
memory_object_data_return
(
	memory_object_t memory_object,
	memory_object_offset_t offset,
	memory_object_cluster_size_t size,
	memory_object_offset_t *resid_offset,
	int     *io_error,
	boolean_t dirty,
	boolean_t kernel_copy,
	int     upl_flags
)
{
	return (memory_object->mo_pager_ops->memory_object_data_return)(
		memory_object,
		offset,
		size,
		resid_offset,
		io_error,
		dirty,
		kernel_copy,
		upl_flags);
}

/* Routine memory_object_data_initialize */
kern_return_t
memory_object_data_initialize
(
	memory_object_t memory_object,
	memory_object_offset_t offset,
	memory_object_cluster_size_t size
)
{
	return (memory_object->mo_pager_ops->memory_object_data_initialize)(
		memory_object,
		offset,
		size);
}

/* Routine memory_object_data_unlock */
kern_return_t
memory_object_data_unlock
(
	memory_object_t memory_object,
	memory_object_offset_t offset,
	memory_object_size_t size,
	vm_prot_t desired_access
)
{
	return (memory_object->mo_pager_ops->memory_object_data_unlock)(
		memory_object,
		offset,
		size,
		desired_access);
}

/* Routine memory_object_synchronize */
kern_return_t
memory_object_synchronize
(
	memory_object_t memory_object,
	memory_object_offset_t offset,
	memory_object_size_t size,
	vm_sync_t sync_flags
)
{
	panic("memory_object_syncrhonize no longer supported\n");

	return (memory_object->mo_pager_ops->memory_object_synchronize)(
		memory_object,
		offset,
		size,
		sync_flags);
}


/*
 * memory_object_map() is called by VM (in vm_map_enter() and its variants)
 * each time a "named" VM object gets mapped directly or indirectly
 * (copy-on-write mapping).  A "named" VM object has an extra reference held
 * by the pager to keep it alive until the pager decides that the
 * memory object (and its VM object) can be reclaimed.
 * VM calls memory_object_last_unmap() (in vm_object_deallocate()) when all
 * the mappings of that memory object have been removed.
 *
 * For a given VM object, calls to memory_object_map() and memory_object_unmap()
 * are serialized (through object->mapping_in_progress), to ensure that the
 * pager gets a consistent view of the mapping status of the memory object.
 *
 * This allows the pager to keep track of how many times a memory object
 * has been mapped and with which protections, to decide when it can be
 * reclaimed.
 */

/* Routine memory_object_map */
kern_return_t
memory_object_map
(
	memory_object_t memory_object,
	vm_prot_t prot
)
{
	return (memory_object->mo_pager_ops->memory_object_map)(
		memory_object,
		prot);
}

/* Routine memory_object_last_unmap */
kern_return_t
memory_object_last_unmap
(
	memory_object_t memory_object
)
{
	return (memory_object->mo_pager_ops->memory_object_last_unmap)(
		memory_object);
}

/* Routine memory_object_data_reclaim */
kern_return_t
memory_object_data_reclaim
(
	memory_object_t memory_object,
	boolean_t       reclaim_backing_store
)
{
	if (memory_object->mo_pager_ops->memory_object_data_reclaim == NULL) {
		return KERN_NOT_SUPPORTED;
	}
	return (memory_object->mo_pager_ops->memory_object_data_reclaim)(
		memory_object,
		reclaim_backing_store);
}

upl_t
convert_port_to_upl(
	ipc_port_t      port)
{
	upl_t upl;

	ip_lock(port);
	if (!ip_active(port) || (ip_kotype(port) != IKOT_UPL)) {
		ip_unlock(port);
		return (upl_t)NULL;
	}
	upl = (upl_t) port->ip_kobject;
	ip_unlock(port);
	upl_lock(upl);
	upl->ref_count += 1;
	upl_unlock(upl);
	return upl;
}

mach_port_t
convert_upl_to_port(
	__unused upl_t          upl)
{
	return MACH_PORT_NULL;
}

__private_extern__ void
upl_no_senders(
	__unused ipc_port_t                             port,
	__unused mach_port_mscount_t    mscount)
{
	return;
}
