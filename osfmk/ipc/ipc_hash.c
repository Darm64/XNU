/*
 * Copyright (c) 2000-2004 Apple Computer, Inc. All rights reserved.
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
 */
/*
 *	File:	ipc/ipc_hash.c
 *	Author:	Rich Draves
 *	Date:	1989
 *
 *	Entry hash table operations.
 */

#include <mach/boolean.h>
#include <mach/port.h>
#include <kern/kalloc.h>
#include <ipc/port.h>
#include <ipc/ipc_space.h>
#include <ipc/ipc_object.h>
#include <ipc/ipc_entry.h>
#include <ipc/ipc_hash.h>
#include <ipc/ipc_init.h>
#include <os/hash.h>

#include <mach_ipc_debug.h>

#if     MACH_IPC_DEBUG
#include <mach/kern_return.h>
#include <mach_debug/hash_info.h>
#include <vm/vm_map.h>
#include <vm/vm_kern.h>
#endif  /* MACH_IPC_DEBUG */

/*
 * Forward declarations
 */

/* Delete an entry from the local reverse hash table */
void ipc_hash_local_delete(
	ipc_space_t             space,
	ipc_object_t            obj,
	mach_port_index_t       index,
	ipc_entry_t             entry);

/*
 *	Routine:	ipc_hash_lookup
 *	Purpose:
 *		Converts (space, obj) -> (name, entry).
 *		Returns TRUE if an entry was found.
 *	Conditions:
 *		The space must be locked (read or write) throughout.
 */

boolean_t
ipc_hash_lookup(
	ipc_space_t             space,
	ipc_object_t            obj,
	mach_port_name_t        *namep,
	ipc_entry_t             *entryp)
{
	return ipc_hash_table_lookup(space->is_table, space->is_table_size, obj, namep, entryp);
}

/*
 *	Routine:	ipc_hash_insert
 *	Purpose:
 *		Inserts an entry into the appropriate reverse hash table,
 *		so that ipc_hash_lookup will find it.
 *	Conditions:
 *		The space must be write-locked.
 */

void
ipc_hash_insert(
	ipc_space_t             space,
	ipc_object_t            obj,
	mach_port_name_t        name,
	ipc_entry_t             entry)
{
	mach_port_index_t index;

	index = MACH_PORT_INDEX(name);
	space->is_table_hashed++;
	ipc_hash_table_insert(space->is_table, space->is_table_size, obj, index, entry);
}

/*
 *	Routine:	ipc_hash_delete
 *	Purpose:
 *		Deletes an entry from the appropriate reverse hash table.
 *	Conditions:
 *		The space must be write-locked.
 */

void
ipc_hash_delete(
	ipc_space_t             space,
	ipc_object_t            obj,
	mach_port_name_t        name,
	ipc_entry_t             entry)
{
	mach_port_index_t index;

	index = MACH_PORT_INDEX(name);
	space->is_table_hashed--;
	ipc_hash_table_delete(space->is_table, space->is_table_size, obj, index, entry);
}

/*
 *	Each space has a local reverse hash table, which holds
 *	entries from the space's table.  In fact, the hash table
 *	just uses a field (ie_index) in the table itself.
 *
 *	The local hash table is an open-addressing hash table,
 *	which means that when a collision occurs, instead of
 *	throwing the entry into a bucket, the entry is rehashed
 *	to another position in the table.  In this case the rehash
 *	is very simple: linear probing (ie, just increment the position).
 *	This simple rehash makes deletions tractable (they're still a pain),
 *	but it means that collisions tend to build up into clumps.
 *
 *	Because at least one entry in the table (index 0) is always unused,
 *	there will always be room in the reverse hash table.  If a table
 *	with n slots gets completely full, the reverse hash table will
 *	have one giant clump of n-1 slots and one free slot somewhere.
 *	Because entries are only entered into the reverse table if they
 *	are pure send rights (not receive, send-once, port-set,
 *	or dead-name rights), and free entries of course aren't entered,
 *	I expect the reverse hash table won't get unreasonably full.
 *
 *	Ordered hash tables (Amble & Knuth, Computer Journal, v. 17, no. 2,
 *	pp. 135-142.) may be desirable here.  They can dramatically help
 *	unsuccessful lookups.  But unsuccessful lookups are almost always
 *	followed by insertions, and those slow down somewhat.  They
 *	also can help deletions somewhat.  Successful lookups aren't affected.
 *	So possibly a small win; probably nothing significant.
 */

#define IH_TABLE_HASH(obj, size)                                \
	        ((mach_port_index_t)(os_hash_kernel_pointer(obj) % (size)))

/*
 *	Routine:	ipc_hash_table_lookup
 *	Purpose:
 *		Converts (table, obj) -> (name, entry).
 *	Conditions:
 *		Must have read consistency on the table.
 */

boolean_t
ipc_hash_table_lookup(
	ipc_entry_t             table,
	ipc_entry_num_t         size,
	ipc_object_t            obj,
	mach_port_name_t        *namep,
	ipc_entry_t             *entryp)
{
	mach_port_index_t hindex, index, hdist;

	if (obj == IO_NULL) {
		return FALSE;
	}

	hindex = IH_TABLE_HASH(obj, size);
	hdist  = 0;

	/*
	 *	Ideally, table[hindex].ie_index is the name we want.
	 *	However, must check ie_object to verify this,
	 *	because collisions can happen.  In case of a collision,
	 *	search farther along in the clump.
	 */

	while ((index = table[hindex].ie_index) != 0) {
		ipc_entry_t entry = &table[index];

		/*
		 * if our current displacement is strictly larger
		 * than the current slot one, then insertion would
		 * have stolen his place so we can't possibly exist.
		 */
		if (hdist > table[hindex].ie_dist) {
			return FALSE;
		}

		/*
		 * If our current displacement is exactly the current
		 * slot displacement, then it can be a match, let's check.
		 */
		if (hdist == table[hindex].ie_dist) {
			assert(index < size);
			if (entry->ie_object == obj) {
				*entryp = entry;
				*namep = MACH_PORT_MAKE(index,
				    IE_BITS_GEN(entry->ie_bits));
				return TRUE;
			}
		} else {
			assert(entry->ie_object != obj);
		}

		if (hdist < IPC_ENTRY_DIST_MAX) {
			/* peg the displacement distance at IPC_ENTRY_DIST_MAX */
			++hdist;
		}
		if (++hindex == size) {
			hindex = 0;
		}
	}

	return FALSE;
}

/*
 *	Routine:	ipc_hash_table_insert
 *	Purpose:
 *		Inserts an entry into the space's reverse hash table.
 *	Conditions:
 *		The space must be write-locked.
 */

void
ipc_hash_table_insert(
	ipc_entry_t                     table,
	ipc_entry_num_t                 size,
	ipc_object_t                    obj,
	mach_port_index_t               index,
	__assert_only ipc_entry_t       entry)
{
	mach_port_index_t hindex, hdist;

	assert(index != 0);
	assert(obj != IO_NULL);

	hindex = IH_TABLE_HASH(obj, size);
	hdist  = 0;

	assert(entry == &table[index]);
	assert(entry->ie_object == obj);

	/*
	 *	We want to insert at hindex, but there may be collisions.
	 *	If a collision occurs, search for the end of the clump
	 *	and insert there.
	 *
	 *	However, Robin Hood steals from the rich, and as we go
	 *	through the clump, if we go over an item that is less
	 *	displaced than we'd be, we steal his slot and
	 *	keep inserting him in our stead.
	 */
	while (table[hindex].ie_index != 0) {
		if (table[hindex].ie_dist < hdist) {
#define swap(a, b)  ({ typeof(a) _tmp = (b); (b) = (a); (a) = _tmp; })
			swap(hdist, table[hindex].ie_dist);
			swap(index, table[hindex].ie_index);
#undef swap
		}
		if (hdist < IPC_ENTRY_DIST_MAX) {
			/* peg the displacement distance at IPC_ENTRY_DIST_MAX */
			++hdist;
		}
		if (++hindex == size) {
			hindex = 0;
		}
	}

	table[hindex].ie_index = index;
	table[hindex].ie_dist = hdist;
}

/*
 *	Routine:	ipc_hash_table_delete
 *	Purpose:
 *		Deletes an entry from the table's reverse hash.
 *	Conditions:
 *		Exclusive access to the table.
 */

void
ipc_hash_table_delete(
	ipc_entry_t                     table,
	ipc_entry_num_t                 size,
	ipc_object_t                    obj,
	mach_port_index_t               index,
	__assert_only ipc_entry_t       entry)
{
	mach_port_index_t hindex, dindex, dist;

	assert(index != MACH_PORT_NULL);
	assert(obj != IO_NULL);

	hindex = IH_TABLE_HASH(obj, size);

	assert(entry == &table[index]);
	assert(entry->ie_object == obj);

	/*
	 *	First check we have the right hindex for this index.
	 *	In case of collision, we have to search farther
	 *	along in this clump.
	 */

	while (table[hindex].ie_index != index) {
		if (++hindex == size) {
			hindex = 0;
		}
	}

	/*
	 *	Now we want to set table[hindex].ie_index = 0.
	 *	But if we aren't the last index in a clump,
	 *	this might cause problems for lookups of objects
	 *	farther along in the clump that are displaced
	 *	due to collisions.  Searches for them would fail
	 *	at hindex instead of succeeding.
	 *
	 *	So we must check the clump after hindex for objects
	 *	that are so displaced, and move one up to the new hole.
	 *
	 *		hindex - index of new hole in the clump
	 *		dindex - index we are checking for a displaced object
	 *
	 *	When we move a displaced object up into the hole,
	 *	it creates a new hole, and we have to repeat the process
	 *	until we get to the end of the clump.
	 */

	for (;;) {
		dindex = hindex + 1;
		if (dindex == size) {
			dindex = 0;
		}

		/*
		 * If the next element is empty or isn't displaced,
		 * then lookup will end on the next element anyway,
		 * so we can leave the hole right here, we're done
		 */
		index = table[dindex].ie_index;
		dist  = table[dindex].ie_dist;
		if (index == 0 || dist == 0) {
			table[hindex].ie_index = 0;
			table[hindex].ie_dist = 0;
			return;
		}

		/*
		 * Move this object closer to its own slot by occupying the hole.
		 * If its displacement was pegged, recompute it.
		 */
		if (dist-- == IPC_ENTRY_DIST_MAX) {
			uint32_t desired = IH_TABLE_HASH(table[index].ie_object, size);
			if (hindex >= desired) {
				dist = hindex - desired;
			} else {
				dist = hindex + size - desired;
			}
			if (dist > IPC_ENTRY_DIST_MAX) {
				dist = IPC_ENTRY_DIST_MAX;
			}
		}

		/*
		 * Move the displaced element closer to its ideal bucket,
		 * and keep shifting elements back.
		 */
		table[hindex].ie_index = index;
		table[hindex].ie_dist = dist;
		hindex = dindex;
	}
}
