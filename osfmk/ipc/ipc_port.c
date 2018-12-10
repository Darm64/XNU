/*
 * Copyright (c) 2000-2007 Apple Inc. All rights reserved.
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
 * @OSF_FREE_COPYRIGHT@
 */
/* 
 * Mach Operating System
 * Copyright (c) 1991,1990,1989 Carnegie Mellon University
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
 * NOTICE: This file was modified by McAfee Research in 2004 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */
/*
 */
/*
 *	File:	ipc/ipc_port.c
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Functions to manipulate IPC ports.
 */

#include <zone_debug.h>
#include <mach_assert.h>

#include <mach/port.h>
#include <mach/kern_return.h>
#include <kern/ipc_kobject.h>
#include <kern/thread.h>
#include <kern/misc_protos.h>
#include <kern/waitq.h>
#include <kern/policy_internal.h>
#include <kern/debug.h>
#include <kern/kcdata.h>
#include <ipc/ipc_entry.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_port.h>
#include <ipc/ipc_pset.h>
#include <ipc/ipc_kmsg.h>
#include <ipc/ipc_mqueue.h>
#include <ipc/ipc_notify.h>
#include <ipc/ipc_table.h>
#include <ipc/ipc_importance.h>
#include <machine/machlimits.h>
#include <kern/turnstile.h>

#include <security/mac_mach_internal.h>

#include <string.h>

decl_lck_spin_data(,	ipc_port_multiple_lock_data)
ipc_port_timestamp_t	ipc_port_timestamp_data;
int ipc_portbt;

#if	MACH_ASSERT
void	ipc_port_init_debug(
		ipc_port_t	port,
		uintptr_t	*callstack,
		unsigned int	callstack_max);

void	ipc_port_callstack_init_debug(
		uintptr_t	*callstack,
		unsigned int	callstack_max);
	
#endif	/* MACH_ASSERT */

void
ipc_port_release(ipc_port_t port)
{
	ip_release(port);
}

void
ipc_port_reference(ipc_port_t port)
{
	ip_reference(port);
}

/*
 *	Routine:	ipc_port_timestamp
 *	Purpose:
 *		Retrieve a timestamp value.
 */

ipc_port_timestamp_t
ipc_port_timestamp(void)
{
	return OSIncrementAtomic(&ipc_port_timestamp_data);
}

/*
 *	Routine:	ipc_port_request_alloc
 *	Purpose:
 *		Try to allocate a request slot.
 *		If successful, returns the request index.
 *		Otherwise returns zero.
 *	Conditions:
 *		The port is locked and active.
 *	Returns:
 *		KERN_SUCCESS		A request index was found.
 *		KERN_NO_SPACE		No index allocated.
 */

#if IMPORTANCE_INHERITANCE
kern_return_t
ipc_port_request_alloc(
	ipc_port_t			port,
	mach_port_name_t		name,
	ipc_port_t			soright,
	boolean_t			send_possible,
	boolean_t			immediate,
	ipc_port_request_index_t	*indexp,
	boolean_t			*importantp)
#else
kern_return_t
ipc_port_request_alloc(
	ipc_port_t			port,
	mach_port_name_t		name,
	ipc_port_t			soright,
	boolean_t			send_possible,
	boolean_t			immediate,
	ipc_port_request_index_t	*indexp)
#endif /* IMPORTANCE_INHERITANCE */
{
	ipc_port_request_t ipr, table;
	ipc_port_request_index_t index;
	uintptr_t mask = 0;

#if IMPORTANCE_INHERITANCE
	*importantp = FALSE;
#endif /* IMPORTANCE_INHERITANCE */

	assert(ip_active(port));
	assert(name != MACH_PORT_NULL);
	assert(soright != IP_NULL);

	table = port->ip_requests;

	if (table == IPR_NULL)
		return KERN_NO_SPACE;

	index = table->ipr_next;
	if (index == 0)
		return KERN_NO_SPACE;

	ipr = &table[index];
	assert(ipr->ipr_name == MACH_PORT_NULL);

	table->ipr_next = ipr->ipr_next;
	ipr->ipr_name = name;
	
	if (send_possible) {
		mask |= IPR_SOR_SPREQ_MASK;
		if (immediate) {
			mask |= IPR_SOR_SPARM_MASK;
			if (port->ip_sprequests == 0) {
				port->ip_sprequests = 1;
#if IMPORTANCE_INHERITANCE
				/* TODO: Live importance support in send-possible */
				if (port->ip_impdonation != 0 &&
				    port->ip_spimportant == 0 &&
				    (task_is_importance_donor(current_task()))) {
					*importantp = TRUE;
				}
#endif /* IMPORTANCE_INHERTANCE */
			}
		}
	}
	ipr->ipr_soright = IPR_SOR_MAKE(soright, mask);

	*indexp = index;

	return KERN_SUCCESS;
}

/*
 *	Routine:	ipc_port_request_grow
 *	Purpose:
 *		Grow a port's table of requests.
 *	Conditions:
 *		The port must be locked and active.
 *		Nothing else locked; will allocate memory.
 *		Upon return the port is unlocked.
 *	Returns:
 *		KERN_SUCCESS		Grew the table.
 *		KERN_SUCCESS		Somebody else grew the table.
 *		KERN_SUCCESS		The port died.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate new table.
 *		KERN_NO_SPACE		Couldn't grow to desired size
 */

kern_return_t
ipc_port_request_grow(
	ipc_port_t		port,
	ipc_table_elems_t 	target_size)
{
	ipc_table_size_t its;
	ipc_port_request_t otable, ntable;

	assert(ip_active(port));

	otable = port->ip_requests;
	if (otable == IPR_NULL)
		its = &ipc_table_requests[0];
	else
		its = otable->ipr_size + 1;

	if (target_size != ITS_SIZE_NONE) {
		if ((otable != IPR_NULL) &&
		    (target_size <= otable->ipr_size->its_size)) {
			ip_unlock(port);
			return KERN_SUCCESS;
	        }
		while ((its->its_size) && (its->its_size < target_size)) {
			its++;
		}
		if (its->its_size == 0) {
			ip_unlock(port);
			return KERN_NO_SPACE;
		}
	}

	ip_reference(port);
	ip_unlock(port);

	if ((its->its_size == 0) ||
	    ((ntable = it_requests_alloc(its)) == IPR_NULL)) {
		ip_release(port);
		return KERN_RESOURCE_SHORTAGE;
	}

	ip_lock(port);

	/*
	 *	Check that port is still active and that nobody else
	 *	has slipped in and grown the table on us.  Note that
	 *	just checking if the current table pointer == otable
	 *	isn't sufficient; must check ipr_size.
	 */

	if (ip_active(port) && (port->ip_requests == otable) &&
	    ((otable == IPR_NULL) || (otable->ipr_size+1 == its))) {
		ipc_table_size_t oits;
		ipc_table_elems_t osize, nsize;
		ipc_port_request_index_t free, i;

		/* copy old table to new table */

		if (otable != IPR_NULL) {
			oits = otable->ipr_size;
			osize = oits->its_size;
			free = otable->ipr_next;

			(void) memcpy((void *)(ntable + 1),
			      (const void *)(otable + 1),
			      (osize - 1) * sizeof(struct ipc_port_request));
		} else {
			osize = 1;
			oits = 0;
			free = 0;
		}

		nsize = its->its_size;
		assert(nsize > osize);

		/* add new elements to the new table's free list */

		for (i = osize; i < nsize; i++) {
			ipc_port_request_t ipr = &ntable[i];

			ipr->ipr_name = MACH_PORT_NULL;
			ipr->ipr_next = free;
			free = i;
		}

		ntable->ipr_next = free;
		ntable->ipr_size = its;
		port->ip_requests = ntable;
		ip_unlock(port);
		ip_release(port);

		if (otable != IPR_NULL) {
			it_requests_free(oits, otable);
	        }
	} else {
		ip_unlock(port);
		ip_release(port);
		it_requests_free(its, ntable);
	}

	return KERN_SUCCESS;
}
 
/*
 *	Routine:	ipc_port_request_sparm
 *	Purpose:
 *		Arm delayed send-possible request.
 *	Conditions:
 *		The port must be locked and active.
 *
 *		Returns TRUE if the request was armed
 *		(or armed with importance in that version).
 */

boolean_t
ipc_port_request_sparm(
	ipc_port_t			port,
	__assert_only mach_port_name_t	name,
	ipc_port_request_index_t	index,
	mach_msg_option_t       option,
	mach_msg_priority_t override)
{
	if (index != IE_REQ_NONE) {
		ipc_port_request_t ipr, table;

		assert(ip_active(port));
	
		table = port->ip_requests;
		assert(table != IPR_NULL);

		ipr = &table[index];
		assert(ipr->ipr_name == name);

		/* Is there a valid destination? */
		if (IPR_SOR_SPREQ(ipr->ipr_soright)) {
			ipr->ipr_soright = IPR_SOR_MAKE(ipr->ipr_soright, IPR_SOR_SPARM_MASK);
			port->ip_sprequests = 1;

			if (option & MACH_SEND_OVERRIDE) {
				/* apply override to message queue */
				ipc_mqueue_override_send(&port->ip_messages, override);
			}

#if IMPORTANCE_INHERITANCE
			if (((option & MACH_SEND_NOIMPORTANCE) == 0) &&
			    (port->ip_impdonation != 0) &&
			    (port->ip_spimportant == 0) &&
			    (((option & MACH_SEND_IMPORTANCE) != 0) ||
			     (task_is_importance_donor(current_task())))) {
				return TRUE;
			}
#else
			return TRUE;
#endif /* IMPORTANCE_INHERITANCE */
		} 
	}
	return FALSE;
}

/*
 *	Routine:	ipc_port_request_type
 *	Purpose:
 *		Determine the type(s) of port requests enabled for a name.
 *	Conditions:
 *		The port must be locked or inactive (to avoid table growth).
 *		The index must not be IE_REQ_NONE and for the name in question.
 */
mach_port_type_t
ipc_port_request_type(
	ipc_port_t			port,
	__assert_only mach_port_name_t	name,
	ipc_port_request_index_t	index)
{
	ipc_port_request_t ipr, table;
	mach_port_type_t type = 0;

	table = port->ip_requests;
	assert (table != IPR_NULL);

	assert(index != IE_REQ_NONE);
	ipr = &table[index];
	assert(ipr->ipr_name == name);

	if (IP_VALID(IPR_SOR_PORT(ipr->ipr_soright))) {
		type |= MACH_PORT_TYPE_DNREQUEST;

		if (IPR_SOR_SPREQ(ipr->ipr_soright)) {
			type |= MACH_PORT_TYPE_SPREQUEST;

			if (!IPR_SOR_SPARMED(ipr->ipr_soright)) {
				type |= MACH_PORT_TYPE_SPREQUEST_DELAYED;
			}
		}
	}
	return type;
}

/*
 *	Routine:	ipc_port_request_cancel
 *	Purpose:
 *		Cancel a dead-name/send-possible request and return the send-once right.
 *	Conditions:
 *		The port must be locked and active.
 *		The index must not be IPR_REQ_NONE and must correspond with name.
 */

ipc_port_t
ipc_port_request_cancel(
	ipc_port_t			port,
	__assert_only mach_port_name_t	name,
	ipc_port_request_index_t	index)
{
	ipc_port_request_t ipr, table;
	ipc_port_t request = IP_NULL;

	assert(ip_active(port));
	table = port->ip_requests;
	assert(table != IPR_NULL);

	assert (index != IE_REQ_NONE);
	ipr = &table[index];
	assert(ipr->ipr_name == name);
	request = IPR_SOR_PORT(ipr->ipr_soright);

	/* return ipr to the free list inside the table */
	ipr->ipr_name = MACH_PORT_NULL;
	ipr->ipr_next = table->ipr_next;
	table->ipr_next = index;

	return request;
}

/*
 *	Routine:	ipc_port_pdrequest
 *	Purpose:
 *		Make a port-deleted request, returning the
 *		previously registered send-once right.
 *		Just cancels the previous request if notify is IP_NULL.
 *	Conditions:
 *		The port is locked and active.  It is unlocked.
 *		Consumes a ref for notify (if non-null), and
 *		returns previous with a ref (if non-null).
 */

void
ipc_port_pdrequest(
	ipc_port_t	port,
	ipc_port_t	notify,
	ipc_port_t	*previousp)
{
	ipc_port_t previous;

	assert(ip_active(port));

	previous = port->ip_pdrequest;
	port->ip_pdrequest = notify;
	ip_unlock(port);

	*previousp = previous;
}

/*
 *	Routine:	ipc_port_nsrequest
 *	Purpose:
 *		Make a no-senders request, returning the
 *		previously registered send-once right.
 *		Just cancels the previous request if notify is IP_NULL.
 *	Conditions:
 *		The port is locked and active.  It is unlocked.
 *		Consumes a ref for notify (if non-null), and
 *		returns previous with a ref (if non-null).
 */

void
ipc_port_nsrequest(
	ipc_port_t		port,
	mach_port_mscount_t	sync,
	ipc_port_t		notify,
	ipc_port_t		*previousp)
{
	ipc_port_t previous;
	mach_port_mscount_t mscount;

	assert(ip_active(port));

	previous = port->ip_nsrequest;
	mscount = port->ip_mscount;

	if ((port->ip_srights == 0) && (sync <= mscount) &&
	    (notify != IP_NULL)) {
		port->ip_nsrequest = IP_NULL;
		ip_unlock(port);
		ipc_notify_no_senders(notify, mscount);
	} else {
		port->ip_nsrequest = notify;
		ip_unlock(port);
	}

	*previousp = previous;
}


/*
 *	Routine:	ipc_port_clear_receiver
 *	Purpose:
 *		Prepares a receive right for transmission/destruction,
 *		optionally performs mqueue destruction (with port lock held)
 *
 *	Conditions:
 *		The port is locked and active.
 *	Returns:
 *		If should_destroy is TRUE, then the return value indicates
 *		whether the caller needs to reap kmsg structures that should
 *		be destroyed (by calling ipc_kmsg_reap_delayed)
 *
 * 		If should_destroy is FALSE, this always returns FALSE
 */

boolean_t
ipc_port_clear_receiver(
	ipc_port_t	port,
	boolean_t	should_destroy)
{
	ipc_mqueue_t	mqueue = &port->ip_messages;
	boolean_t	reap_messages = FALSE;

	/*
	 * Pull ourselves out of any sets to which we belong.
	 * We hold the port locked, so even though this acquires and releases
	 * the mqueue lock, we know we won't be added to any other sets.
	 */
	if (port->ip_in_pset != 0) {
		ipc_pset_remove_from_all(port);
		assert(port->ip_in_pset == 0);
	}

	/*
	 * Send anyone waiting on the port's queue directly away.
	 * Also clear the mscount and seqno.
	 */
	imq_lock(mqueue);
	ipc_mqueue_changed(mqueue);
	port->ip_mscount = 0;
	mqueue->imq_seqno = 0;
	port->ip_context = port->ip_guarded = port->ip_strict_guard = 0;

	if (should_destroy) {
		/*
		 * Mark the mqueue invalid, preventing further send/receive
		 * operations from succeeding. It's important for this to be
		 * done under the same lock hold as the ipc_mqueue_changed
		 * call to avoid additional threads blocking on an mqueue
		 * that's being destroyed.
		 */
		reap_messages = ipc_mqueue_destroy_locked(mqueue);
	}

	imq_unlock(&port->ip_messages);

	return reap_messages;
}

/*
 *	Routine:	ipc_port_init
 *	Purpose:
 *		Initializes a newly-allocated port.
 *		Doesn't touch the ip_object fields.
 */

void
ipc_port_init(
	ipc_port_t		port,
	ipc_space_t		space,
	mach_port_name_t	name)
{
	/* port->ip_kobject doesn't have to be initialized */

	port->ip_receiver = space;
	port->ip_receiver_name = name;

	port->ip_mscount = 0;
	port->ip_srights = 0;
	port->ip_sorights = 0;

	port->ip_nsrequest = IP_NULL;
	port->ip_pdrequest = IP_NULL;
	port->ip_requests = IPR_NULL;

	port->ip_premsg = IKM_NULL;
	port->ip_context = 0;

	port->ip_sprequests  = 0;
	port->ip_spimportant = 0;
	port->ip_impdonation = 0;
	port->ip_tempowner   = 0;

	port->ip_guarded      = 0;
	port->ip_strict_guard = 0;
	port->ip_impcount    = 0;

	port->ip_specialreply = 0;
	port->ip_sync_link_state = PORT_SYNC_LINK_ANY;

	reset_ip_srp_bits(port);

	port->ip_send_turnstile = TURNSTILE_NULL;

	ipc_mqueue_init(&port->ip_messages,
			FALSE /* !set */);
}

/*
 *	Routine:	ipc_port_alloc
 *	Purpose:
 *		Allocate a port.
 *	Conditions:
 *		Nothing locked.  If successful, the port is returned
 *		locked.  (The caller doesn't have a reference.)
 *	Returns:
 *		KERN_SUCCESS		The port is allocated.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_NO_SPACE		No room for an entry in the space.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate memory.
 */

kern_return_t
ipc_port_alloc(
	ipc_space_t		space,
	mach_port_name_t	*namep,
	ipc_port_t		*portp)
{
	ipc_port_t port;
	mach_port_name_t name;
	kern_return_t kr;

#if     MACH_ASSERT
	uintptr_t buf[IP_CALLSTACK_MAX];
	ipc_port_callstack_init_debug(&buf[0], IP_CALLSTACK_MAX);
#endif /* MACH_ASSERT */
	    
	kr = ipc_object_alloc(space, IOT_PORT,
			      MACH_PORT_TYPE_RECEIVE, 0,
			      &name, (ipc_object_t *) &port);
	if (kr != KERN_SUCCESS)
		return kr;

	/* port and space are locked */
	ipc_port_init(port, space, name);

#if     MACH_ASSERT
	ipc_port_init_debug(port, &buf[0], IP_CALLSTACK_MAX);
#endif  /* MACH_ASSERT */

	/* unlock space after init */
	is_write_unlock(space);

	*namep = name;
	*portp = port;

	return KERN_SUCCESS;
}

/*
 *	Routine:	ipc_port_alloc_name
 *	Purpose:
 *		Allocate a port, with a specific name.
 *	Conditions:
 *		Nothing locked.  If successful, the port is returned
 *		locked.  (The caller doesn't have a reference.)
 *	Returns:
 *		KERN_SUCCESS		The port is allocated.
 *		KERN_INVALID_TASK	The space is dead.
 *		KERN_NAME_EXISTS	The name already denotes a right.
 *		KERN_RESOURCE_SHORTAGE	Couldn't allocate memory.
 */

kern_return_t
ipc_port_alloc_name(
	ipc_space_t		space,
	mach_port_name_t	name,
	ipc_port_t		*portp)
{
	ipc_port_t port;
	kern_return_t kr;

#if     MACH_ASSERT
	uintptr_t buf[IP_CALLSTACK_MAX];
	ipc_port_callstack_init_debug(&buf[0], IP_CALLSTACK_MAX);
#endif /* MACH_ASSERT */	

	kr = ipc_object_alloc_name(space, IOT_PORT,
				   MACH_PORT_TYPE_RECEIVE, 0,
				   name, (ipc_object_t *) &port);
	if (kr != KERN_SUCCESS)
		return kr;

	/* port is locked */

	ipc_port_init(port, space, name);

#if     MACH_ASSERT
	ipc_port_init_debug(port, &buf[0], IP_CALLSTACK_MAX);
#endif  /* MACH_ASSERT */	

	*portp = port;

	return KERN_SUCCESS;
}

/*
 * 	Routine:	ipc_port_spnotify
 *	Purpose:
 *		Generate send-possible port notifications.
 *	Conditions:
 *		Nothing locked, reference held on port.
 */
void
ipc_port_spnotify(
	ipc_port_t	port)
{
	ipc_port_request_index_t index = 0;
	ipc_table_elems_t size = 0;

	/*
	 * If the port has no send-possible request
	 * armed, don't bother to lock the port.
	 */
	if (port->ip_sprequests == 0)
		return;

	ip_lock(port);
	
#if IMPORTANCE_INHERITANCE
	if (port->ip_spimportant != 0) {
		port->ip_spimportant = 0;
		if (ipc_port_importance_delta(port, IPID_OPTION_NORMAL, -1) == TRUE) {
			ip_lock(port);
		}
	}
#endif /* IMPORTANCE_INHERITANCE */

	if (port->ip_sprequests == 0) {
		ip_unlock(port);
		return;
	}
	port->ip_sprequests = 0;

revalidate:
	if (ip_active(port)) {
		ipc_port_request_t requests;

		/* table may change each time port unlocked (reload) */
		requests = port->ip_requests;
		assert(requests != IPR_NULL);

		/*
		 * no need to go beyond table size when first
		 * we entered - those are future notifications.
		 */
		if (size == 0)
			size = requests->ipr_size->its_size;

		/* no need to backtrack either */
		while (++index < size) {
			ipc_port_request_t ipr = &requests[index];
			mach_port_name_t name = ipr->ipr_name;
			ipc_port_t soright = IPR_SOR_PORT(ipr->ipr_soright);
			boolean_t armed = IPR_SOR_SPARMED(ipr->ipr_soright);

			if (MACH_PORT_VALID(name) && armed && IP_VALID(soright)) {
				/* claim send-once right - slot still inuse */
				ipr->ipr_soright = IP_NULL;
				ip_unlock(port);

				ipc_notify_send_possible(soright, name);

				ip_lock(port);
				goto revalidate;
			}
		}
	}
	ip_unlock(port);
	return;
}

/*
 * 	Routine:	ipc_port_dnnotify
 *	Purpose:
 *		Generate dead name notifications for
 *		all outstanding dead-name and send-
 *		possible requests.
 *	Conditions:
 *		Nothing locked.
 *		Port must be inactive.
 *		Reference held on port.
 */
void
ipc_port_dnnotify(
	ipc_port_t	port)
{
	ipc_port_request_t requests = port->ip_requests;

	assert(!ip_active(port));
	if (requests != IPR_NULL) {
		ipc_table_size_t its = requests->ipr_size;
		ipc_table_elems_t size = its->its_size;
		ipc_port_request_index_t index;
		for (index = 1; index < size; index++) {
			ipc_port_request_t ipr = &requests[index];
			mach_port_name_t name = ipr->ipr_name;
			ipc_port_t soright = IPR_SOR_PORT(ipr->ipr_soright);

			if (MACH_PORT_VALID(name) && IP_VALID(soright)) {
				ipc_notify_dead_name(soright, name);
			}
		}
	}
}


/*
 *	Routine:	ipc_port_destroy
 *	Purpose:
 *		Destroys a port.  Cleans up queued messages.
 *
 *		If the port has a backup, it doesn't get destroyed,
 *		but is sent in a port-destroyed notification to the backup.
 *	Conditions:
 *		The port is locked and alive; nothing else locked.
 *		The caller has a reference, which is consumed.
 *		Afterwards, the port is unlocked and dead.
 */

void
ipc_port_destroy(ipc_port_t port)
{
	ipc_port_t pdrequest, nsrequest;
	ipc_mqueue_t mqueue;
	ipc_kmsg_t kmsg;
	boolean_t special_reply = port->ip_specialreply;

#if IMPORTANCE_INHERITANCE
	ipc_importance_task_t release_imp_task = IIT_NULL;
	thread_t self = current_thread();
	boolean_t top = (self->ith_assertions == 0);
	natural_t assertcnt = 0;
#endif /* IMPORTANCE_INHERITANCE */

	assert(ip_active(port));
	/* port->ip_receiver_name is garbage */
	/* port->ip_receiver/port->ip_destination is garbage */

	/* check for a backup port */
	pdrequest = port->ip_pdrequest;

#if IMPORTANCE_INHERITANCE
	/* determine how many assertions to drop and from whom */
	if (port->ip_tempowner != 0) {
		assert(top);
		release_imp_task = port->ip_imp_task;
		if (IIT_NULL != release_imp_task) {
			port->ip_imp_task = IIT_NULL;
			assertcnt = port->ip_impcount;
		}
		/* Otherwise, nothing to drop */
	} else {
		assertcnt = port->ip_impcount;
		if (pdrequest != IP_NULL)
			/* mark in limbo for the journey */
			port->ip_tempowner = 1;
	}

	if (top)
		self->ith_assertions = assertcnt;
#endif /* IMPORTANCE_INHERITANCE */

	if (pdrequest != IP_NULL) {
		/* clear receiver, don't destroy the port */
		(void)ipc_port_clear_receiver(port, FALSE);
		assert(port->ip_in_pset == 0);
		assert(port->ip_mscount == 0);

		/* we assume the ref for pdrequest */
		port->ip_pdrequest = IP_NULL;

		/* make port be in limbo */
		imq_lock(&port->ip_messages);
		port->ip_receiver_name = MACH_PORT_NULL;
		port->ip_destination = IP_NULL;
		imq_unlock(&port->ip_messages);
		ip_unlock(port);

		if (special_reply) {
			ipc_port_adjust_special_reply_port(port,
				IPC_PORT_ADJUST_SR_ALLOW_SYNC_LINKAGE, FALSE);
		}
		/* consumes our refs for port and pdrequest */
		ipc_notify_port_destroyed(pdrequest, port);

		goto drop_assertions;
	}

	/* port active bit needs to be guarded under mqueue lock for turnstiles */
	imq_lock(&port->ip_messages);
	port->ip_object.io_bits &= ~IO_BITS_ACTIVE;
	port->ip_timestamp = ipc_port_timestamp();
	imq_unlock(&port->ip_messages);
	nsrequest = port->ip_nsrequest;

	/*
	 * The mach_msg_* paths don't hold a port lock, they only hold a
	 * reference to the port object. If a thread raced us and is now
	 * blocked waiting for message reception on this mqueue (or waiting
	 * for ipc_mqueue_full), it will never be woken up. We call
	 * ipc_port_clear_receiver() here, _after_ the port has been marked
	 * inactive, to wakeup any threads which may be blocked and ensure
	 * that no other thread can get lost waiting for a wake up on a
	 * port/mqueue that's been destroyed.
	 */
	boolean_t reap_msgs = FALSE;
	reap_msgs = ipc_port_clear_receiver(port, TRUE); /* marks mqueue inactive */
	assert(port->ip_in_pset == 0);
	assert(port->ip_mscount == 0);

	/*
	 * If the port has a preallocated message buffer and that buffer
	 * is not inuse, free it.  If it has an inuse one, then the kmsg
	 * free will detect that we freed the association and it can free it
	 * like a normal buffer.
	 *
	 * Once the port is marked inactive we don't need to keep it locked.
	 */
	if (IP_PREALLOC(port)) {
		ipc_port_t inuse_port;

		kmsg = port->ip_premsg;
		assert(kmsg != IKM_NULL);
		inuse_port = ikm_prealloc_inuse_port(kmsg);
		ipc_kmsg_clear_prealloc(kmsg, port);
		ip_unlock(port);
		if (inuse_port != IP_NULL) {
			assert(inuse_port == port);
		} else {
			ipc_kmsg_free(kmsg);
		}
	} else {
		ip_unlock(port);
	}

	/* unlink the kmsg from special reply port */
	if (special_reply) {
		ipc_port_adjust_special_reply_port(port,
			IPC_PORT_ADJUST_SR_ALLOW_SYNC_LINKAGE, FALSE);
	}

	/* throw away no-senders request */
	if (nsrequest != IP_NULL)
		ipc_notify_send_once(nsrequest); /* consumes ref */

	/*
	 * Reap any kmsg objects waiting to be destroyed.
	 * This must be done after we've released the port lock.
	 */
	if (reap_msgs)
		ipc_kmsg_reap_delayed();

	mqueue = &port->ip_messages;

	/* cleanup waitq related resources */
	ipc_mqueue_deinit(mqueue);

	/* generate dead-name notifications */
	ipc_port_dnnotify(port);

	ipc_kobject_destroy(port);

	ip_release(port); /* consume caller's ref */

 drop_assertions:
#if IMPORTANCE_INHERITANCE
	if (release_imp_task != IIT_NULL) {
		if (assertcnt > 0) {
			assert(top);
			self->ith_assertions = 0;
			assert(ipc_importance_task_is_any_receiver_type(release_imp_task));
			ipc_importance_task_drop_internal_assertion(release_imp_task, assertcnt);
		}
		ipc_importance_task_release(release_imp_task);

	} else if (assertcnt > 0) {
		if (top) {
			self->ith_assertions = 0;
			release_imp_task = current_task()->task_imp_base;
			if (ipc_importance_task_is_any_receiver_type(release_imp_task)) {
				ipc_importance_task_drop_internal_assertion(release_imp_task, assertcnt);
			}
		}
	}
#endif /* IMPORTANCE_INHERITANCE */
}

/*
 *	Routine:	ipc_port_check_circularity
 *	Purpose:
 *		Check if queueing "port" in a message for "dest"
 *		would create a circular group of ports and messages.
 *
 *		If no circularity (FALSE returned), then "port"
 *		is changed from "in limbo" to "in transit".
 *
 *		That is, we want to set port->ip_destination == dest,
 *		but guaranteeing that this doesn't create a circle
 *		port->ip_destination->ip_destination->... == port
 *
 *	Conditions:
 *		No ports locked.  References held for "port" and "dest".
 */

boolean_t
ipc_port_check_circularity(
	ipc_port_t	port,
	ipc_port_t	dest)
{
#if IMPORTANCE_INHERITANCE
	/* adjust importance counts at the same time */
	return ipc_importance_check_circularity(port, dest);
#else
	ipc_port_t base;

	assert(port != IP_NULL);
	assert(dest != IP_NULL);

	if (port == dest)
		return TRUE;
	base = dest;

	/* Check if destination needs a turnstile */
	ipc_port_send_turnstile_prepare(dest);

	/*
	 *	First try a quick check that can run in parallel.
	 *	No circularity if dest is not in transit.
	 */
	ip_lock(port);
	if (ip_lock_try(dest)) {
		if (!ip_active(dest) ||
		    (dest->ip_receiver_name != MACH_PORT_NULL) ||
		    (dest->ip_destination == IP_NULL))
			goto not_circular;

		/* dest is in transit; further checking necessary */

		ip_unlock(dest);
	}
	ip_unlock(port);

	ipc_port_multiple_lock(); /* massive serialization */

	/*
	 *	Search for the end of the chain (a port not in transit),
	 *	acquiring locks along the way.
	 */

	for (;;) {
		ip_lock(base);

		if (!ip_active(base) ||
		    (base->ip_receiver_name != MACH_PORT_NULL) ||
		    (base->ip_destination == IP_NULL))
			break;

		base = base->ip_destination;
	}

	/* all ports in chain from dest to base, inclusive, are locked */

	if (port == base) {
		/* circularity detected! */

		ipc_port_multiple_unlock();

		/* port (== base) is in limbo */

		assert(ip_active(port));
		assert(port->ip_receiver_name == MACH_PORT_NULL);
		assert(port->ip_destination == IP_NULL);

		base = dest;
		while (base != IP_NULL) {
			ipc_port_t next;

			/* dest is in transit or in limbo */

			assert(ip_active(base));
			assert(base->ip_receiver_name == MACH_PORT_NULL);

			next = base->ip_destination;
			ip_unlock(base);
			base = next;
		}

		ipc_port_send_turnstile_complete(dest);
		return TRUE;
	}

	/*
	 *	The guarantee:  lock port while the entire chain is locked.
	 *	Once port is locked, we can take a reference to dest,
	 *	add port to the chain, and unlock everything.
	 */

	ip_lock(port);
	ipc_port_multiple_unlock();

not_circular:
	imq_lock(&port->ip_messages);

	/* port is in limbo */

	assert(ip_active(port));
	assert(port->ip_receiver_name == MACH_PORT_NULL);
	assert(port->ip_destination == IP_NULL);

	ip_reference(dest);
	port->ip_destination = dest;

	/* Setup linkage for source port if it has sync ipc push */
	struct turnstile *send_turnstile = TURNSTILE_NULL;
	if (port_send_turnstile(port)) {
		send_turnstile = turnstile_prepare((uintptr_t)port,
			port_send_turnstile_address(port),
			TURNSTILE_NULL, TURNSTILE_SYNC_IPC);

		turnstile_update_inheritor(send_turnstile, port_send_turnstile(dest),
			(TURNSTILE_INHERITOR_TURNSTILE | TURNSTILE_IMMEDIATE_UPDATE));

		/* update complete and turnstile complete called after dropping all locks */
	}
	imq_unlock(&port->ip_messages);

	/* now unlock chain */

	ip_unlock(port);

	for (;;) {
		ipc_port_t next;

		if (dest == base)
			break;

		/* port is in transit */

		assert(ip_active(dest));
		assert(dest->ip_receiver_name == MACH_PORT_NULL);
		assert(dest->ip_destination != IP_NULL);

		next = dest->ip_destination;
		ip_unlock(dest);
		dest = next;
	}

	/* base is not in transit */
	assert(!ip_active(base) ||
	       (base->ip_receiver_name != MACH_PORT_NULL) ||
	       (base->ip_destination == IP_NULL));

	ip_unlock(base);

	/* All locks dropped, call turnstile_update_inheritor_complete for source port's turnstile */
	if (send_turnstile) {
		turnstile_update_inheritor_complete(send_turnstile, TURNSTILE_INTERLOCK_NOT_HELD);

		/* Take the mq lock to call turnstile complete */
		imq_lock(&port->ip_messages);
		turnstile_complete((uintptr_t)port, port_send_turnstile_address(port), NULL);
		send_turnstile = TURNSTILE_NULL;
		imq_unlock(&port->ip_messages);
		turnstile_cleanup();
	}

	return FALSE;
#endif /* !IMPORTANCE_INHERITANCE */
}

struct turnstile *
ipc_port_get_inheritor(ipc_port_t port)
{
	ipc_mqueue_t mqueue = &port->ip_messages;
	struct knote *kn;

	assert(imq_held(mqueue));

	if (!IMQ_KLIST_VALID(mqueue)) {
		return IMQ_INHERITOR(mqueue);
	}

	SLIST_FOREACH(kn, &port->ip_messages.imq_klist, kn_selnext) {
		if ((kn->kn_sfflags & MACH_RCV_MSG) && (kn->kn_status & KN_DISPATCH)) {
			return filt_machport_kqueue_turnstile(kn);
		}
	}

	return TURNSTILE_NULL;
}

/*
 *	Routine:	ipc_port_send_turnstile_prepare
 *	Purpose:
 *		Get a reference on port's send turnstile, if
 *		port does not have a send turnstile then allocate one.
 *
 *	Conditions:
 *		Nothing is locked.
 */
void
ipc_port_send_turnstile_prepare(ipc_port_t port)
{
	struct turnstile *turnstile = TURNSTILE_NULL;
	struct turnstile *inheritor = TURNSTILE_NULL;
	struct turnstile *send_turnstile = TURNSTILE_NULL;

retry_alloc:
	imq_lock(&port->ip_messages);

	if (port_send_turnstile(port) == NULL ||
	    port_send_turnstile(port)->ts_port_ref == 0) {

		if (turnstile == TURNSTILE_NULL) {
			imq_unlock(&port->ip_messages);
			turnstile = turnstile_alloc();
			goto retry_alloc;
		}

		send_turnstile = turnstile_prepare((uintptr_t)port,
			port_send_turnstile_address(port),
			turnstile, TURNSTILE_SYNC_IPC);
		turnstile = TURNSTILE_NULL;

		/*
		 * if port in transit, setup linkage for its turnstile,
		 * otherwise the link it to WL turnstile.
		 */
		if (ip_active(port) &&
		    port->ip_receiver_name == MACH_PORT_NULL &&
		    port->ip_destination != IP_NULL) {
			assert(port->ip_receiver_name == MACH_PORT_NULL);
			assert(port->ip_destination != IP_NULL);

			inheritor = port_send_turnstile(port->ip_destination);
		} else {
			inheritor = ipc_port_get_inheritor(port);
		}
		turnstile_update_inheritor(send_turnstile, inheritor,
			TURNSTILE_INHERITOR_TURNSTILE | TURNSTILE_IMMEDIATE_UPDATE);
		/* turnstile complete will be called in ipc_port_send_turnstile_complete */
	}

	/* Increment turnstile counter */
	port_send_turnstile(port)->ts_port_ref++;
	imq_unlock(&port->ip_messages);

	if (send_turnstile) {
		turnstile_update_inheritor_complete(send_turnstile,
			TURNSTILE_INTERLOCK_NOT_HELD);
	}
	if (turnstile != TURNSTILE_NULL) {
		turnstile_deallocate(turnstile);
	}
}


/*
 *	Routine:	ipc_port_send_turnstile_complete
 *	Purpose:
 *		Drop a ref on the port's send turnstile, if the
 *		ref becomes zero, deallocate the turnstile.
 *
 *	Conditions:
 *		The space might be locked, use safe deallocate.
 */
void
ipc_port_send_turnstile_complete(ipc_port_t port)
{
	struct turnstile *turnstile = TURNSTILE_NULL;

	/* Drop turnstile count on dest port */
	imq_lock(&port->ip_messages);

	port_send_turnstile(port)->ts_port_ref--;
	if (port_send_turnstile(port)->ts_port_ref == 0) {
		turnstile_complete((uintptr_t)port, port_send_turnstile_address(port),
				&turnstile);
		assert(turnstile != TURNSTILE_NULL);
	}
	imq_unlock(&port->ip_messages);
	turnstile_cleanup();

	if (turnstile != TURNSTILE_NULL) {
		turnstile_deallocate_safe(turnstile);
		turnstile = TURNSTILE_NULL;
	}
}


/*
 *	Routine:	ipc_port_rcv_turnstile_waitq
 *	Purpose:
 *		Given the mqueue's waitq, find the port's
 *              rcv turnstile and return its waitq.
 *
 *	Conditions:
 *		mqueue locked or thread waiting on turnstile is locked.
 */
struct waitq *
ipc_port_rcv_turnstile_waitq(struct waitq *waitq)
{
	struct waitq *safeq;

	ipc_mqueue_t mqueue = imq_from_waitq(waitq);
	ipc_port_t port = ip_from_mq(mqueue);
	struct turnstile *rcv_turnstile = ipc_port_rcv_turnstile(port);

	/* Check if the port has a rcv turnstile */
	if (rcv_turnstile != TURNSTILE_NULL) {
		safeq = &rcv_turnstile->ts_waitq;
	} else {
		safeq = global_eventq(waitq);
	}
	return safeq;
}


/*
 *	Routine:	ipc_port_rcv_turnstile
 *	Purpose:
 *		Get the port's receive turnstile
 *
 *	Conditions:
 *		mqueue locked or thread waiting on turnstile is locked.
 */
struct turnstile *
ipc_port_rcv_turnstile(ipc_port_t port)
{
	return turnstile_lookup_by_proprietor((uintptr_t)port);
}


/*
 *	Routine:	ipc_port_link_special_reply_port
 *	Purpose:
 *		Link the special reply port with the destination port.
 *              Allocates turnstile to dest port.
 *
 *	Conditions:
 *		Nothing is locked.
 */
void
ipc_port_link_special_reply_port(
	ipc_port_t special_reply_port,
	ipc_port_t dest_port)
{
	boolean_t drop_turnstile_ref = FALSE;

	/* Check if dest_port needs a turnstile */
	ipc_port_send_turnstile_prepare(dest_port);

	/* Lock the special reply port and establish the linkage */
	ip_lock(special_reply_port);
	imq_lock(&special_reply_port->ip_messages);

	/* Check if we need to drop the acquired turnstile ref on dest port */
	if (!special_reply_port->ip_specialreply ||
	    special_reply_port->ip_sync_link_state != PORT_SYNC_LINK_ANY ||
	    special_reply_port->ip_sync_inheritor_port != IPC_PORT_NULL) {
		drop_turnstile_ref = TRUE;
	} else {
		/* take a reference on dest_port */
		ip_reference(dest_port);
		special_reply_port->ip_sync_inheritor_port = dest_port;
		special_reply_port->ip_sync_link_state = PORT_SYNC_LINK_PORT;
	}

	imq_unlock(&special_reply_port->ip_messages);
	ip_unlock(special_reply_port);

	if (drop_turnstile_ref) {
		ipc_port_send_turnstile_complete(dest_port);
	}

	return;
}

#if DEVELOPMENT || DEBUG
inline void
reset_ip_srp_bits(ipc_port_t special_reply_port)
{
	special_reply_port->ip_srp_lost_link = 0;
	special_reply_port->ip_srp_msg_sent = 0;
}

inline void
reset_ip_srp_msg_sent(ipc_port_t special_reply_port)
{
	if (special_reply_port->ip_specialreply == 1) {
		special_reply_port->ip_srp_msg_sent = 0;
	}
}

inline void
set_ip_srp_msg_sent(ipc_port_t special_reply_port)
{
	if (special_reply_port->ip_specialreply == 1) {
		special_reply_port->ip_srp_msg_sent = 1;
	}
}

inline void
set_ip_srp_lost_link(ipc_port_t special_reply_port)
{
	if (special_reply_port->ip_specialreply == 1 && special_reply_port->ip_srp_msg_sent == 0) {
		special_reply_port->ip_srp_lost_link = 1;
	}
}

#else /* DEVELOPMENT || DEBUG */
inline void
reset_ip_srp_bits(__unused ipc_port_t special_reply_port)
{
	return;
}

inline void
reset_ip_srp_msg_sent(__unused ipc_port_t special_reply_port)
{
	return;
}

inline void
set_ip_srp_msg_sent(__unused ipc_port_t special_reply_port)
{
	return;
}

inline void
set_ip_srp_lost_link(__unused ipc_port_t special_reply_port)
{
	return;
}
#endif /* DEVELOPMENT || DEBUG */

/*
 *	Routine:	ipc_port_adjust_special_reply_port_locked
 *	Purpose:
 *		If the special port has a turnstile, update it's inheritor.
 *	Condition:
 *		Special reply port locked on entry.
 *		Special reply port unlocked on return.
 *	Returns:
 *		None.
 */
void
ipc_port_adjust_special_reply_port_locked(
	ipc_port_t special_reply_port,
	struct knote *kn,
	uint8_t flags,
	boolean_t get_turnstile)
{
	ipc_port_t dest_port = IPC_PORT_NULL;
	int sync_link_state = PORT_SYNC_LINK_NO_LINKAGE;
	turnstile_inheritor_t inheritor = TURNSTILE_INHERITOR_NULL;
	struct turnstile *dest_ts = TURNSTILE_NULL, *ts = TURNSTILE_NULL;

	imq_lock(&special_reply_port->ip_messages);

	if (flags & IPC_PORT_ADJUST_SR_RECEIVED_MSG) {
		reset_ip_srp_msg_sent(special_reply_port);
	}

	/* Check if the special reply port is marked non-special */
	if (special_reply_port->ip_specialreply == 0 ||
			special_reply_port->ip_sync_link_state == PORT_SYNC_LINK_ANY) {
		if (get_turnstile) {
			turnstile_complete((uintptr_t)special_reply_port,
				port_rcv_turnstile_address(special_reply_port),
			NULL);
		}
		imq_unlock(&special_reply_port->ip_messages);
		ip_unlock(special_reply_port);
		if (get_turnstile) {
			turnstile_cleanup();
		}
		return;
	}

	/* Clear thread's special reply port and clear linkage */
	if (flags & IPC_PORT_ADJUST_SR_CLEAR_SPECIAL_REPLY) {
		/* This option should only be specified by a non blocking thread */
		assert(get_turnstile == FALSE);
		special_reply_port->ip_specialreply = 0;

		reset_ip_srp_bits(special_reply_port);

		/* Check if need to break linkage */
		if (special_reply_port->ip_sync_link_state == PORT_SYNC_LINK_NO_LINKAGE) {
			imq_unlock(&special_reply_port->ip_messages);
			ip_unlock(special_reply_port);
			return;
		}
	} else if (flags & IPC_PORT_ADJUST_SR_LINK_WORKLOOP) {
		if (special_reply_port->ip_sync_link_state == PORT_SYNC_LINK_ANY ||
		    special_reply_port->ip_sync_link_state == PORT_SYNC_LINK_PORT) {
			if (ITH_KNOTE_VALID(kn, MACH_MSG_TYPE_PORT_SEND_ONCE)) {
				inheritor = filt_machport_stash_port(kn, special_reply_port,
						&sync_link_state);
			}
		}
	} else if (flags & IPC_PORT_ADJUST_SR_ALLOW_SYNC_LINKAGE) {
		sync_link_state = PORT_SYNC_LINK_ANY;
	}

	switch (special_reply_port->ip_sync_link_state) {
	case PORT_SYNC_LINK_PORT:
		dest_port = special_reply_port->ip_sync_inheritor_port;
		special_reply_port->ip_sync_inheritor_port = IPC_PORT_NULL;
		break;
	case PORT_SYNC_LINK_WORKLOOP_KNOTE:
		special_reply_port->ip_sync_inheritor_knote = NULL;
		break;
	case PORT_SYNC_LINK_WORKLOOP_STASH:
		dest_ts = special_reply_port->ip_sync_inheritor_ts;
		special_reply_port->ip_sync_inheritor_ts = NULL;
		break;
	}

	special_reply_port->ip_sync_link_state = sync_link_state;

	switch (sync_link_state) {
	case PORT_SYNC_LINK_WORKLOOP_KNOTE:
		special_reply_port->ip_sync_inheritor_knote = kn;
		break;
	case PORT_SYNC_LINK_WORKLOOP_STASH:
		turnstile_reference(inheritor);
		special_reply_port->ip_sync_inheritor_ts = inheritor;
		break;
	case PORT_SYNC_LINK_NO_LINKAGE:
		if (flags & IPC_PORT_ADJUST_SR_ENABLE_EVENT) {
			set_ip_srp_lost_link(special_reply_port);
		}
		break;
	}

	/* Get thread's turnstile donated to special reply port */
	if (get_turnstile) {
		turnstile_complete((uintptr_t)special_reply_port,
			port_rcv_turnstile_address(special_reply_port),
			NULL);
	} else {
		ts = ipc_port_rcv_turnstile(special_reply_port);
		if (ts) {
			turnstile_reference(ts);
			turnstile_update_inheritor(ts, inheritor,
					(TURNSTILE_INHERITOR_TURNSTILE | TURNSTILE_IMMEDIATE_UPDATE));
		}
	}

	imq_unlock(&special_reply_port->ip_messages);
	ip_unlock(special_reply_port);

	if (get_turnstile) {
		turnstile_cleanup();
	} else if (ts) {
		/* Call turnstile cleanup after dropping the interlock */
		turnstile_update_inheritor_complete(ts, TURNSTILE_INTERLOCK_NOT_HELD);
		turnstile_deallocate_safe(ts);
	}

	/* Release the ref on the dest port and it's turnstile */
	if (dest_port) {
		ipc_port_send_turnstile_complete(dest_port);
		/* release the reference on the dest port */
		ip_release(dest_port);
	}

	if (dest_ts) {
		turnstile_deallocate_safe(dest_ts);
	}
}

/*
 *	Routine:	ipc_port_adjust_special_reply_port
 *	Purpose:
 *		If the special port has a turnstile, update it's inheritor.
 *	Condition:
 *		Nothing locked.
 *	Returns:
 *		None.
 */
void
ipc_port_adjust_special_reply_port(
	ipc_port_t special_reply_port,
	uint8_t flags,
	boolean_t get_turnstile)
{
	ip_lock(special_reply_port);
	ipc_port_adjust_special_reply_port_locked(special_reply_port, NULL, flags, get_turnstile);
	/* special_reply_port unlocked */
}

/*
 *	Routine:	ipc_port_get_special_reply_port_inheritor
 *	Purpose:
 *		Returns the current inheritor of the special reply port
 *	Condition:
 *		mqueue is locked, port is a special reply port
 *	Returns:
 *		the current inheritor
 */
turnstile_inheritor_t
ipc_port_get_special_reply_port_inheritor(
	ipc_port_t port)
{
	assert(port->ip_specialreply);
	imq_held(&port->ip_messages);

	switch (port->ip_sync_link_state) {
	case PORT_SYNC_LINK_PORT:
		if (port->ip_sync_inheritor_port != NULL) {
			return port_send_turnstile(port->ip_sync_inheritor_port);
		}
		break;
	case PORT_SYNC_LINK_WORKLOOP_KNOTE:
		return filt_machport_stashed_special_reply_port_turnstile(port);
	case PORT_SYNC_LINK_WORKLOOP_STASH:
		return port->ip_sync_inheritor_ts;
	}
	return TURNSTILE_INHERITOR_NULL;
}

/*
 *	Routine:	ipc_port_impcount_delta
 *	Purpose:
 *		Adjust only the importance count associated with a port.
 *		If there are any adjustments to be made to receiver task,
 *		those are handled elsewhere.
 *
 *		For now, be defensive during deductions to make sure the
 *		impcount for the port doesn't underflow zero.  This will
 *		go away when the port boost addition is made atomic (see
 *		note in ipc_port_importance_delta()).
 *	Conditions:
 *		The port is referenced and locked.
 *		Nothing else is locked.
 */
mach_port_delta_t
ipc_port_impcount_delta(
	ipc_port_t        port,
	mach_port_delta_t delta,
	ipc_port_t        __unused base)
{
	mach_port_delta_t absdelta; 

	if (!ip_active(port)) {
		return 0;
	}

	/* adding/doing nothing is easy */
	if (delta >= 0) {
		port->ip_impcount += delta;
		return delta;
	}

	absdelta = 0 - delta;		
	if (port->ip_impcount >= absdelta) {
		port->ip_impcount -= absdelta;
		return delta;
	}

#if (DEVELOPMENT || DEBUG)
	if (port->ip_receiver_name != MACH_PORT_NULL) {
		task_t target_task = port->ip_receiver->is_task;
		ipc_importance_task_t target_imp = target_task->task_imp_base;
		const char *target_procname;
		int target_pid;

		if (target_imp != IIT_NULL) {
			target_procname = target_imp->iit_procname;
			target_pid = target_imp->iit_bsd_pid;
		} else {
			target_procname = "unknown";
			target_pid = -1;
		}
		printf("Over-release of importance assertions for port 0x%x receiver pid %d (%s), "
		       "dropping %d assertion(s) but port only has %d remaining.\n",
		       port->ip_receiver_name, 
		       target_pid, target_procname,
		       absdelta, port->ip_impcount);

	} else if (base != IP_NULL) {
		task_t target_task = base->ip_receiver->is_task;
		ipc_importance_task_t target_imp = target_task->task_imp_base;
		const char *target_procname;
		int target_pid;

		if (target_imp != IIT_NULL) {
			target_procname = target_imp->iit_procname;
			target_pid = target_imp->iit_bsd_pid;
		} else {
			target_procname = "unknown";
			target_pid = -1;
		}
		printf("Over-release of importance assertions for port 0x%lx "
		       "enqueued on port 0x%x with receiver pid %d (%s), "
		       "dropping %d assertion(s) but port only has %d remaining.\n",
		       (unsigned long)VM_KERNEL_UNSLIDE_OR_PERM((uintptr_t)port),
		       base->ip_receiver_name,
		       target_pid, target_procname,
		       absdelta, port->ip_impcount);
	}
#endif

	delta = 0 - port->ip_impcount;
	port->ip_impcount = 0;
	return delta;
}

/*
 *	Routine:	ipc_port_importance_delta_internal
 *	Purpose:
 *		Adjust the importance count through the given port.
 *		If the port is in transit, apply the delta throughout
 *		the chain. Determine if the there is a task at the
 *		base of the chain that wants/needs to be adjusted,
 *		and if so, apply the delta.
 *	Conditions:
 *		The port is referenced and locked on entry.
 *		Importance may be locked.
 *		Nothing else is locked.
 *		The lock may be dropped on exit.
 *		Returns TRUE if lock was dropped.
 */
#if IMPORTANCE_INHERITANCE

boolean_t
ipc_port_importance_delta_internal(
	ipc_port_t 		port,
	natural_t		options,
	mach_port_delta_t	*deltap,
	ipc_importance_task_t	*imp_task)
{
	ipc_port_t next, base;
	boolean_t dropped = FALSE;

	*imp_task = IIT_NULL;

	if (*deltap == 0)
		return FALSE;

	assert(options == IPID_OPTION_NORMAL || options == IPID_OPTION_SENDPOSSIBLE);

	base = port;

	/* if port is in transit, have to search for end of chain */
	if (ip_active(port) &&
	    port->ip_destination != IP_NULL &&
	    port->ip_receiver_name == MACH_PORT_NULL) {

		dropped = TRUE;

		ip_unlock(port);
		ipc_port_multiple_lock(); /* massive serialization */
		ip_lock(base);

		while(ip_active(base) &&
		      base->ip_destination != IP_NULL &&
		      base->ip_receiver_name == MACH_PORT_NULL) {

			base = base->ip_destination;
			ip_lock(base);
		}
		ipc_port_multiple_unlock();
	}

	/*
	 * If the port lock is dropped b/c the port is in transit, there is a
	 * race window where another thread can drain messages and/or fire a
	 * send possible notification before we get here.
	 *
	 * We solve this race by checking to see if our caller armed the send
	 * possible notification, whether or not it's been fired yet, and
	 * whether or not we've already set the port's ip_spimportant bit. If
	 * we don't need a send-possible boost, then we'll just apply a
	 * harmless 0-boost to the port.
	 */
	if (options & IPID_OPTION_SENDPOSSIBLE) {
		assert(*deltap == 1);
		if (port->ip_sprequests && port->ip_spimportant == 0)
			port->ip_spimportant = 1;
		else
			*deltap = 0;
	}

	/* unlock down to the base, adjusting boost(s) at each level */
	for (;;) {
		*deltap = ipc_port_impcount_delta(port, *deltap, base);

		if (port == base) {
			break;
		}

		/* port is in transit */
		assert(port->ip_tempowner == 0);
		next = port->ip_destination;
		ip_unlock(port);
		port = next;
	}

	/* find the task (if any) to boost according to the base */
	if (ip_active(base)) {
		if (base->ip_tempowner != 0) {
			if (IIT_NULL != base->ip_imp_task)
				*imp_task = base->ip_imp_task;
			/* otherwise don't boost */

		} else if (base->ip_receiver_name != MACH_PORT_NULL) {
			ipc_space_t space = base->ip_receiver;

			/* only spaces with boost-accepting tasks */
			if (space->is_task != TASK_NULL &&
			    ipc_importance_task_is_any_receiver_type(space->is_task->task_imp_base)) {
				*imp_task = space->is_task->task_imp_base;
			}
		}
	}

	/*
	 * Only the base is locked.  If we have to hold or drop task
	 * importance assertions, we'll have to drop that lock as well.
	 */
	if (*imp_task != IIT_NULL) {
		/* take a reference before unlocking base */
		ipc_importance_task_reference(*imp_task);
	}

	if (dropped == TRUE) {
		ip_unlock(base);
	}

	return dropped;
}
#endif /* IMPORTANCE_INHERITANCE */

/*
 *	Routine:	ipc_port_importance_delta
 *	Purpose:
 *		Adjust the importance count through the given port.
 *		If the port is in transit, apply the delta throughout
 *		the chain.
 *
 *		If there is a task at the base of the chain that wants/needs
 *		to be adjusted, apply the delta.
 *	Conditions:
 *		The port is referenced and locked on entry.
 *		Nothing else is locked.
 *		The lock may be dropped on exit.
 *		Returns TRUE if lock was dropped.
 */
#if IMPORTANCE_INHERITANCE

boolean_t
ipc_port_importance_delta(
	ipc_port_t 		port,
	natural_t		options,
	mach_port_delta_t	delta)
{
	ipc_importance_task_t imp_task = IIT_NULL;
	boolean_t dropped;

	dropped = ipc_port_importance_delta_internal(port, options, &delta, &imp_task);

	if (IIT_NULL == imp_task || delta == 0)
		return dropped;

	if (!dropped)
		ip_unlock(port);

	assert(ipc_importance_task_is_any_receiver_type(imp_task));

	if (delta > 0)
		ipc_importance_task_hold_internal_assertion(imp_task, delta);
	else
		ipc_importance_task_drop_internal_assertion(imp_task, -delta);

	ipc_importance_task_release(imp_task);
	return TRUE;
}
#endif /* IMPORTANCE_INHERITANCE */

/*
 *	Routine:	ipc_port_lookup_notify
 *	Purpose:
 *		Make a send-once notify port from a receive right.
 *		Returns IP_NULL if name doesn't denote a receive right.
 *	Conditions:
 *		The space must be locked (read or write) and active.
 *  		Being the active space, we can rely on thread server_id
 *		context to give us the proper server level sub-order
 *		within the space.
 */

ipc_port_t
ipc_port_lookup_notify(
	ipc_space_t		space,
	mach_port_name_t	name)
{
	ipc_port_t port;
	ipc_entry_t entry;

	assert(is_active(space));

	entry = ipc_entry_lookup(space, name);
	if (entry == IE_NULL)
		return IP_NULL;
	if ((entry->ie_bits & MACH_PORT_TYPE_RECEIVE) == 0)
		return IP_NULL;

	__IGNORE_WCASTALIGN(port = (ipc_port_t) entry->ie_object);
	assert(port != IP_NULL);

	ip_lock(port);
	assert(ip_active(port));
	assert(port->ip_receiver_name == name);
	assert(port->ip_receiver == space);

	ip_reference(port);
	port->ip_sorights++;
	ip_unlock(port);

	return port;
}

/*
 *	Routine:	ipc_port_make_send_locked
 *	Purpose:
 *		Make a naked send right from a receive right.
 *
 *	Conditions:
 *		port locked and active.
 */
ipc_port_t
ipc_port_make_send_locked(
	ipc_port_t	port)
{
	assert(ip_active(port));
	port->ip_mscount++;
	port->ip_srights++;
	ip_reference(port);
	return port;
}

/*
 *	Routine:	ipc_port_make_send
 *	Purpose:
 *		Make a naked send right from a receive right.
 */

ipc_port_t
ipc_port_make_send(
	ipc_port_t	port)
{
	
	if (!IP_VALID(port))
		return port;

	ip_lock(port);
	if (ip_active(port)) {
		port->ip_mscount++;
		port->ip_srights++;
		ip_reference(port);
		ip_unlock(port);
		return port;
	}
	ip_unlock(port);
	return IP_DEAD;
}

/*
 *	Routine:	ipc_port_copy_send
 *	Purpose:
 *		Make a naked send right from another naked send right.
 *			IP_NULL		-> IP_NULL
 *			IP_DEAD		-> IP_DEAD
 *			dead port	-> IP_DEAD
 *			live port	-> port + ref
 *	Conditions:
 *		Nothing locked except possibly a space.
 */

ipc_port_t
ipc_port_copy_send(
	ipc_port_t	port)
{
	ipc_port_t sright;

	if (!IP_VALID(port))
		return port;

	ip_lock(port);
	if (ip_active(port)) {
		assert(port->ip_srights > 0);

		ip_reference(port);
		port->ip_srights++;
		sright = port;
	} else
		sright = IP_DEAD;
	ip_unlock(port);

	return sright;
}

/*
 *	Routine:	ipc_port_copyout_send
 *	Purpose:
 *		Copyout a naked send right (possibly null/dead),
 *		or if that fails, destroy the right.
 *	Conditions:
 *		Nothing locked.
 */

mach_port_name_t
ipc_port_copyout_send(
	ipc_port_t	sright,
	ipc_space_t	space)
{
	mach_port_name_t name;

	if (IP_VALID(sright)) {
		kern_return_t kr;

		kr = ipc_object_copyout(space, (ipc_object_t) sright,
					MACH_MSG_TYPE_PORT_SEND, TRUE, &name);
		if (kr != KERN_SUCCESS) {
			ipc_port_release_send(sright);

			if (kr == KERN_INVALID_CAPABILITY)
				name = MACH_PORT_DEAD;
			else
				name = MACH_PORT_NULL;
		}
	} else
		name = CAST_MACH_PORT_TO_NAME(sright);

	return name;
}

/*
 *	Routine:	ipc_port_copyout_name_send
 *	Purpose:
 *		Copyout a naked send right (possibly null/dead) to given name,
 *		or if that fails, destroy the right.
 *	Conditions:
 *		Nothing locked.
 */

mach_port_name_t
ipc_port_copyout_name_send(
	ipc_port_t	sright,
	ipc_space_t	space,
	mach_port_name_t name)
{
	if (IP_VALID(sright)) {
		kern_return_t kr;

		kr = ipc_object_copyout_name(space, (ipc_object_t) sright,
					MACH_MSG_TYPE_PORT_SEND, TRUE, name);
		if (kr != KERN_SUCCESS) {
			ipc_port_release_send(sright);

			if (kr == KERN_INVALID_CAPABILITY)
				name = MACH_PORT_DEAD;
			else
				name = MACH_PORT_NULL;
		}
	} else
		name = CAST_MACH_PORT_TO_NAME(sright);

	return name;
}

/*
 *	Routine:	ipc_port_release_send
 *	Purpose:
 *		Release a naked send right.
 *		Consumes a ref for the port.
 *	Conditions:
 *		Nothing locked.
 */

void
ipc_port_release_send(
	ipc_port_t	port)
{
	ipc_port_t nsrequest = IP_NULL;
	mach_port_mscount_t mscount;

	if (!IP_VALID(port))
		return;

	ip_lock(port);

	assert(port->ip_srights > 0);
	if (port->ip_srights == 0) {
		panic("Over-release of port %p send right!", port);
	}

	port->ip_srights--;

	if (!ip_active(port)) {
		ip_unlock(port);
		ip_release(port);
		return;
	}

	if (port->ip_srights == 0 &&
	    port->ip_nsrequest != IP_NULL) {
		nsrequest = port->ip_nsrequest;
		port->ip_nsrequest = IP_NULL;
		mscount = port->ip_mscount;
		ip_unlock(port);
		ip_release(port);
		ipc_notify_no_senders(nsrequest, mscount);
	} else {
		ip_unlock(port);
		ip_release(port);
	}
}

/*
 *	Routine:	ipc_port_make_sonce_locked
 *	Purpose:
 *		Make a naked send-once right from a receive right.
 *	Conditions:
 *		The port is locked and active.
 */

ipc_port_t
ipc_port_make_sonce_locked(
	ipc_port_t	port)
{
	assert(ip_active(port));
	port->ip_sorights++;
	ip_reference(port);
	return port;
}

/*
 *	Routine:	ipc_port_make_sonce
 *	Purpose:
 *		Make a naked send-once right from a receive right.
 *	Conditions:
 *		The port is not locked.
 */

ipc_port_t
ipc_port_make_sonce(
	ipc_port_t	port)
{
	if (!IP_VALID(port))
		return port;

	ip_lock(port);
	if (ip_active(port)) {
		port->ip_sorights++;
		ip_reference(port);
		ip_unlock(port);
		return port;
	}
	ip_unlock(port);
	return IP_DEAD;
}

/*
 *	Routine:	ipc_port_release_sonce
 *	Purpose:
 *		Release a naked send-once right.
 *		Consumes a ref for the port.
 *
 *		In normal situations, this is never used.
 *		Send-once rights are only consumed when
 *		a message (possibly a send-once notification)
 *		is sent to them.
 *	Conditions:
 *		Nothing locked except possibly a space.
 */

void
ipc_port_release_sonce(
	ipc_port_t	port)
{
	if (!IP_VALID(port))
		return;

	ipc_port_adjust_special_reply_port(port, IPC_PORT_ADJUST_SR_NONE, FALSE);

	ip_lock(port);

	assert(port->ip_sorights > 0);
	if (port->ip_sorights == 0) {
		panic("Over-release of port %p send-once right!", port);
	}

	port->ip_sorights--;

	ip_unlock(port);
	ip_release(port);
}

/*
 *	Routine:	ipc_port_release_receive
 *	Purpose:
 *		Release a naked (in limbo or in transit) receive right.
 *		Consumes a ref for the port; destroys the port.
 *	Conditions:
 *		Nothing locked.
 */

void
ipc_port_release_receive(
	ipc_port_t	port)
{
	ipc_port_t dest;

	if (!IP_VALID(port))
		return;

	ip_lock(port);
	assert(ip_active(port));
	assert(port->ip_receiver_name == MACH_PORT_NULL);
	dest = port->ip_destination;

	ipc_port_destroy(port); /* consumes ref, unlocks */

	if (dest != IP_NULL) {
		ipc_port_send_turnstile_complete(dest);
		ip_release(dest);
	}
}

/*
 *	Routine:	ipc_port_alloc_special
 *	Purpose:
 *		Allocate a port in a special space.
 *		The new port is returned with one ref.
 *		If unsuccessful, IP_NULL is returned.
 *	Conditions:
 *		Nothing locked.
 */

ipc_port_t
ipc_port_alloc_special(
	ipc_space_t	space)
{
	ipc_port_t port;

	__IGNORE_WCASTALIGN(port = (ipc_port_t) io_alloc(IOT_PORT));
	if (port == IP_NULL)
		return IP_NULL;

#if     MACH_ASSERT
	uintptr_t buf[IP_CALLSTACK_MAX];
	ipc_port_callstack_init_debug(&buf[0], IP_CALLSTACK_MAX);
#endif /* MACH_ASSERT */	

	bzero((char *)port, sizeof(*port));
	io_lock_init(&port->ip_object);
	port->ip_references = 1;
	port->ip_object.io_bits = io_makebits(TRUE, IOT_PORT, 0);

	ipc_port_init(port, space, 1);

#if     MACH_ASSERT
	ipc_port_init_debug(port, &buf[0], IP_CALLSTACK_MAX);
#endif  /* MACH_ASSERT */		

	return port;
}

/*
 *	Routine:	ipc_port_dealloc_special
 *	Purpose:
 *		Deallocate a port in a special space.
 *		Consumes one ref for the port.
 *	Conditions:
 *		Nothing locked.
 */

void
ipc_port_dealloc_special(
	ipc_port_t			port,
	__assert_only ipc_space_t	space)
{
	ip_lock(port);
	assert(ip_active(port));
//	assert(port->ip_receiver_name != MACH_PORT_NULL);
	assert(port->ip_receiver == space);

	/*
	 *	We clear ip_receiver_name and ip_receiver to simplify
	 *	the ipc_space_kernel check in ipc_mqueue_send.
	 */

	imq_lock(&port->ip_messages);
	port->ip_receiver_name = MACH_PORT_NULL;
	port->ip_receiver = IS_NULL;
	imq_unlock(&port->ip_messages);

	/* relevant part of ipc_port_clear_receiver */
	ipc_port_set_mscount(port, 0);
	port->ip_messages.imq_seqno = 0;

	ipc_port_destroy(port);
}

/*
 *	Routine:	ipc_port_finalize
 *	Purpose:
 *		Called on last reference deallocate to
 *		free any remaining data associated with the
 *		port.
 *	Conditions:
 *		Nothing locked.
 */
void
ipc_port_finalize(
	ipc_port_t		port)
{
	ipc_port_request_t requests = port->ip_requests;

	assert(port_send_turnstile(port) == TURNSTILE_NULL);
	assert(ipc_port_rcv_turnstile(port) == TURNSTILE_NULL);

	if (ip_active(port)) {
		panic("Trying to free an active port. port %p", port);
	}

	if (requests != IPR_NULL) {
		ipc_table_size_t its = requests->ipr_size;
		it_requests_free(its, requests);
		port->ip_requests = IPR_NULL;
	}

	ipc_mqueue_deinit(&port->ip_messages);
	
#if	MACH_ASSERT
	ipc_port_track_dealloc(port);
#endif	/* MACH_ASSERT */
}

/*
 *	Routine:	kdp_mqueue_send_find_owner
 *	Purpose:
 *		Discover the owner of the ipc_mqueue that contains the input
 *		waitq object. The thread blocked on the waitq should be
 *		waiting for an IPC_MQUEUE_FULL event.
 *	Conditions:
 *		The 'waitinfo->wait_type' value should already be set to
 *		kThreadWaitPortSend.
 *	Note:
 *		If we find out that the containing port is actually in
 *		transit, we reset the wait_type field to reflect this.
 */
void
kdp_mqueue_send_find_owner(struct waitq * waitq, __assert_only event64_t event, thread_waitinfo_t * waitinfo)
{
	struct turnstile *turnstile;
	assert(waitinfo->wait_type == kThreadWaitPortSend);
	assert(event == IPC_MQUEUE_FULL);
	assert(waitq_is_turnstile_queue(waitq));

	turnstile = waitq_to_turnstile(waitq);
	ipc_port_t port     = (ipc_port_t)turnstile->ts_proprietor; /* we are blocking on send */
	assert(kdp_is_in_zone(port, "ipc ports"));

	waitinfo->owner = 0;
	waitinfo->context  = VM_KERNEL_UNSLIDE_OR_PERM(port);
	if (ip_lock_held_kdp(port)) {
		/*
		 * someone has the port locked: it may be in an
		 * inconsistent state: bail
		 */
		waitinfo->owner = STACKSHOT_WAITOWNER_PORT_LOCKED;
		return;
	}

	if (ip_active(port)) {
		if (port->ip_tempowner) {
			if (port->ip_imp_task != IIT_NULL && port->ip_imp_task->iit_task != NULL) {
				/* port is held by a tempowner */
				waitinfo->owner = pid_from_task(port->ip_imp_task->iit_task);
			} else {
				waitinfo->owner = STACKSHOT_WAITOWNER_INTRANSIT;
			}
		} else if (port->ip_receiver_name) {
			/* port in a space */
			if (port->ip_receiver == ipc_space_kernel) {
				/*
				 * The kernel pid is 0, make this
				 * distinguishable from no-owner and
				 * inconsistent port state.
				 */
				waitinfo->owner = STACKSHOT_WAITOWNER_KERNEL;
			} else {
				waitinfo->owner = pid_from_task(port->ip_receiver->is_task);
			}
		} else if (port->ip_destination != IP_NULL) {
			/* port in transit */
			waitinfo->wait_type = kThreadWaitPortSendInTransit;
			waitinfo->owner     = VM_KERNEL_UNSLIDE_OR_PERM(port->ip_destination);
		}
	}
}

/*
 *	Routine:	kdp_mqueue_recv_find_owner
 *	Purpose:
 *		Discover the "owner" of the ipc_mqueue that contains the input
 *		waitq object. The thread blocked on the waitq is trying to
 *		receive on the mqueue.
 *	Conditions:
 *		The 'waitinfo->wait_type' value should already be set to
 *		kThreadWaitPortReceive.
 *	Note:
 *		If we find that we are actualy waiting on a port set, we reset
 *		the wait_type field to reflect this.
 */
void
kdp_mqueue_recv_find_owner(struct waitq * waitq, __assert_only event64_t event, thread_waitinfo_t * waitinfo)
{
	assert(waitinfo->wait_type == kThreadWaitPortReceive);
	assert(event == IPC_MQUEUE_RECEIVE);

	ipc_mqueue_t mqueue = imq_from_waitq(waitq);
	waitinfo->owner     = 0;
	if (imq_is_set(mqueue)) { /* we are waiting on a port set */
		ipc_pset_t set = ips_from_mq(mqueue);
		assert(kdp_is_in_zone(set, "ipc port sets"));

		/* Reset wait type to specify waiting on port set receive */
		waitinfo->wait_type = kThreadWaitPortSetReceive;
		waitinfo->context   = VM_KERNEL_UNSLIDE_OR_PERM(set);
		if (ips_lock_held_kdp(set)) {
			waitinfo->owner = STACKSHOT_WAITOWNER_PSET_LOCKED;
		}
		/* There is no specific owner "at the other end" of a port set, so leave unset. */
	} else {
		ipc_port_t port   = ip_from_mq(mqueue);
		assert(kdp_is_in_zone(port, "ipc ports"));

		waitinfo->context = VM_KERNEL_UNSLIDE_OR_PERM(port);
		if (ip_lock_held_kdp(port)) {
			waitinfo->owner = STACKSHOT_WAITOWNER_PORT_LOCKED;
			return;
		}

		if (ip_active(port)) {
			if (port->ip_receiver_name != MACH_PORT_NULL) {
				waitinfo->owner = port->ip_receiver_name;
			} else {
				waitinfo->owner = STACKSHOT_WAITOWNER_INTRANSIT;
			}
		}
	}
}

#if	MACH_ASSERT
#include <kern/machine.h>

/*
 *	Keep a list of all allocated ports.
 *	Allocation is intercepted via ipc_port_init;
 *	deallocation is intercepted via io_free.
 */
#if 0
queue_head_t	port_alloc_queue;
lck_spin_t	port_alloc_queue_lock;
#endif

unsigned long	port_count = 0;
unsigned long	port_count_warning = 20000;
unsigned long	port_timestamp = 0;

void		db_port_stack_trace(
			ipc_port_t	port);
void		db_ref(
			int		refs);
int		db_port_walk(
			unsigned int	verbose,
			unsigned int	display,
			unsigned int	ref_search,
			unsigned int	ref_target);

/*
 *	Initialize global state needed for run-time
 *	port debugging.
 */
void
ipc_port_debug_init(void)
{
#if 0
	queue_init(&port_alloc_queue);
	lck_spin_init(&port_alloc_queue_lock, &ipc_lck_grp, &ipc_lck_attr);
#endif

	if (!PE_parse_boot_argn("ipc_portbt", &ipc_portbt, sizeof (ipc_portbt)))
		ipc_portbt = 0;
}

#ifdef MACH_BSD
extern int proc_pid(struct proc*);
#endif /* MACH_BSD */

/*
 *	Initialize all of the debugging state in a port.
 *	Insert the port into a global list of all allocated ports.
 */
void
ipc_port_init_debug(
	ipc_port_t	port,
	uintptr_t 	*callstack,
	unsigned int	callstack_max)
{
	unsigned int	i;

	port->ip_thread = current_thread();
	port->ip_timetrack = port_timestamp++;
	for (i = 0; i < callstack_max; ++i)
		port->ip_callstack[i] = callstack[i];	
	for (i = 0; i < IP_NSPARES; ++i)
		port->ip_spares[i] = 0;	

#ifdef MACH_BSD
	task_t task = current_task();
	if (task != TASK_NULL) {
		struct proc* proc = (struct proc*) get_bsdtask_info(task);
		if (proc)
			port->ip_spares[0] = proc_pid(proc);
	}
#endif /* MACH_BSD */

#if 0
	lck_spin_lock(&port_alloc_queue_lock);
	++port_count;
	if (port_count_warning > 0 && port_count >= port_count_warning)
		assert(port_count < port_count_warning);
	queue_enter(&port_alloc_queue, port, ipc_port_t, ip_port_links);
	lck_spin_unlock(&port_alloc_queue_lock);
#endif
}

/*
 *	Routine:	ipc_port_callstack_init_debug
 *	Purpose:
 *		Calls the machine-dependent routine to
 *		fill in an array with up to IP_CALLSTACK_MAX
 *		levels of return pc information
 *	Conditions:
 *		May block (via copyin)
 */
void
ipc_port_callstack_init_debug(
	uintptr_t	*callstack,
	unsigned int	callstack_max)
{
	unsigned int	i;

	/* guarantee the callstack is initialized */
	for (i=0; i < callstack_max; i++)
		callstack[i] = 0;	

	if (ipc_portbt)
		machine_callstack(callstack, callstack_max);
}

/*
 *	Remove a port from the queue of allocated ports.
 *	This routine should be invoked JUST prior to
 *	deallocating the actual memory occupied by the port.
 */
#if 1
void
ipc_port_track_dealloc(
	__unused ipc_port_t	port)
{
}
#else
void
ipc_port_track_dealloc(
	ipc_port_t		port)
{
	lck_spin_lock(&port_alloc_queue_lock);
	assert(port_count > 0);
	--port_count;
	queue_remove(&port_alloc_queue, port, ipc_port_t, ip_port_links);
	lck_spin_unlock(&port_alloc_queue_lock);
}
#endif


#endif	/* MACH_ASSERT */
