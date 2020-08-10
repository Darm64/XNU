/*
 * Copyright (c) 2000 Apple Computer, Inc. All rights reserved.
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

#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSOrderedSet.h>
#include <libkern/c++/OSLib.h>

#define super OSCollection

OSDefineMetaClassAndStructors(OSOrderedSet, OSCollection)
OSMetaClassDefineReservedUnused(OSOrderedSet, 0);
OSMetaClassDefineReservedUnused(OSOrderedSet, 1);
OSMetaClassDefineReservedUnused(OSOrderedSet, 2);
OSMetaClassDefineReservedUnused(OSOrderedSet, 3);
OSMetaClassDefineReservedUnused(OSOrderedSet, 4);
OSMetaClassDefineReservedUnused(OSOrderedSet, 5);
OSMetaClassDefineReservedUnused(OSOrderedSet, 6);
OSMetaClassDefineReservedUnused(OSOrderedSet, 7);


struct _Element {
	const OSMetaClassBase *             obj;
//    unsigned int	pri;
};

#define EXT_CAST(obj) \
    reinterpret_cast<OSObject *>(const_cast<OSMetaClassBase *>(obj))

bool
OSOrderedSet::
initWithCapacity(unsigned int inCapacity,
    OSOrderFunction inOrdering, void *inOrderingRef)
{
	unsigned int size;

	if (!super::init()) {
		return false;
	}

	if (inCapacity > (UINT_MAX / sizeof(_Element))) {
		return false;
	}

	size = sizeof(_Element) * inCapacity;
	array = (_Element *) kalloc_container(size);
	if (!array) {
		return false;
	}

	count = 0;
	capacity = inCapacity;
	capacityIncrement = (inCapacity)? inCapacity : 16;
	ordering = inOrdering;
	orderingRef = inOrderingRef;

	bzero(array, size);
	OSCONTAINER_ACCUMSIZE(size);

	return true;
}

OSOrderedSet *
OSOrderedSet::
withCapacity(unsigned int capacity,
    OSOrderFunction ordering, void * orderingRef)
{
	OSOrderedSet *me = new OSOrderedSet;

	if (me && !me->initWithCapacity(capacity, ordering, orderingRef)) {
		me->release();
		me = NULL;
	}

	return me;
}

void
OSOrderedSet::free()
{
	(void) super::setOptions(0, kImmutable);
	flushCollection();

	if (array) {
		kfree(array, sizeof(_Element) * capacity);
		OSCONTAINER_ACCUMSIZE( -(sizeof(_Element) * capacity));
	}

	super::free();
}

unsigned int
OSOrderedSet::getCount() const
{
	return count;
}
unsigned int
OSOrderedSet::getCapacity() const
{
	return capacity;
}
unsigned int
OSOrderedSet::getCapacityIncrement() const
{
	return capacityIncrement;
}
unsigned int
OSOrderedSet::setCapacityIncrement(unsigned int increment)
{
	capacityIncrement = (increment)? increment : 16;
	return capacityIncrement;
}

unsigned int
OSOrderedSet::ensureCapacity(unsigned int newCapacity)
{
	_Element *newArray;
	unsigned int finalCapacity;
	vm_size_t    oldSize, newSize;

	if (newCapacity <= capacity) {
		return capacity;
	}

	// round up
	finalCapacity = (((newCapacity - 1) / capacityIncrement) + 1)
	    * capacityIncrement;
	if ((finalCapacity < newCapacity) ||
	    (finalCapacity > (UINT_MAX / sizeof(_Element)))) {
		return capacity;
	}
	newSize = sizeof(_Element) * finalCapacity;

	newArray = (_Element *) kallocp_container(&newSize);
	if (newArray) {
		// use all of the actual allocation size
		finalCapacity = newSize / sizeof(_Element);

		oldSize = sizeof(_Element) * capacity;

		OSCONTAINER_ACCUMSIZE(((size_t)newSize) - ((size_t)oldSize));

		bcopy(array, newArray, oldSize);
		bzero(&newArray[capacity], newSize - oldSize);
		kfree(array, oldSize);
		array = newArray;
		capacity = finalCapacity;
	}

	return capacity;
}

void
OSOrderedSet::flushCollection()
{
	unsigned int i;

	haveUpdated();

	for (i = 0; i < count; i++) {
		array[i].obj->taggedRelease(OSTypeID(OSCollection));
	}

	count = 0;
}

/* internal */
bool
OSOrderedSet::setObject(unsigned int index, const OSMetaClassBase *anObject)
{
	unsigned int i;
	unsigned int newCount = count + 1;

	if ((index > count) || !anObject) {
		return false;
	}

	if (containsObject(anObject)) {
		return false;
	}

	// do we need more space?
	if (newCount > capacity && newCount > ensureCapacity(newCount)) {
		return false;
	}

	haveUpdated();
	if (index != count) {
		for (i = count; i > index; i--) {
			array[i] = array[i - 1];
		}
	}
	array[index].obj = anObject;
//    array[index].pri = pri;
	anObject->taggedRetain(OSTypeID(OSCollection));
	count++;

	return true;
}


bool
OSOrderedSet::setFirstObject(const OSMetaClassBase *anObject)
{
	return setObject(0, anObject);
}

bool
OSOrderedSet::setLastObject(const OSMetaClassBase *anObject)
{
	return setObject( count, anObject);
}


#define ORDER(obj1, obj2) \
    (ordering ? ((*ordering)( (const OSObject *) obj1, (const OSObject *) obj2, orderingRef)) : 0)

bool
OSOrderedSet::setObject(const OSMetaClassBase *anObject )
{
	unsigned int i;

	// queue it behind those with same priority
	for (i = 0;
	    (i < count) && (ORDER(array[i].obj, anObject) >= 0);
	    i++) {
	}

	return setObject(i, anObject);
}

void
OSOrderedSet::removeObject(const OSMetaClassBase *anObject)
{
	bool                deleted = false;
	unsigned int        i;

	for (i = 0; i < count; i++) {
		if (deleted) {
			array[i - 1] = array[i];
		} else if (array[i].obj == anObject) {
			deleted = true;
			haveUpdated(); // Pity we can't flush the log
			array[i].obj->taggedRelease(OSTypeID(OSCollection));
		}
	}

	if (deleted) {
		count--;
	}
}

bool
OSOrderedSet::containsObject(const OSMetaClassBase *anObject) const
{
	return anObject && member(anObject);
}

bool
OSOrderedSet::member(const OSMetaClassBase *anObject) const
{
	unsigned int i;

	for (i = 0;
	    (i < count) && (array[i].obj != anObject);
	    i++) {
	}

	return i < count;
}

/* internal */
OSObject *
OSOrderedSet::getObject( unsigned int index ) const
{
	if (index >= count) {
		return NULL;
	}

//    if( pri)
//	*pri = array[index].pri;

	return const_cast<OSObject *>((const OSObject *) array[index].obj);
}

OSObject *
OSOrderedSet::getFirstObject() const
{
	if (count) {
		return const_cast<OSObject *>((const OSObject *) array[0].obj);
	} else {
		return NULL;
	}
}

OSObject *
OSOrderedSet::getLastObject() const
{
	if (count) {
		return const_cast<OSObject *>((const OSObject *) array[count - 1].obj);
	} else {
		return NULL;
	}
}

SInt32
OSOrderedSet::orderObject( const OSMetaClassBase * anObject )
{
	return ORDER( anObject, NULL );
}

void *
OSOrderedSet::getOrderingRef()
{
	return orderingRef;
}

bool
OSOrderedSet::isEqualTo(const OSOrderedSet *anOrderedSet) const
{
	unsigned int i;

	if (this == anOrderedSet) {
		return true;
	}

	if (count != anOrderedSet->getCount()) {
		return false;
	}

	for (i = 0; i < count; i++) {
		if (!array[i].obj->isEqualTo(anOrderedSet->getObject(i))) {
			return false;
		}
	}

	return true;
}

bool
OSOrderedSet::isEqualTo(const OSMetaClassBase *anObject) const
{
	OSOrderedSet *oSet;

	oSet = OSDynamicCast(OSOrderedSet, anObject);
	if (oSet) {
		return isEqualTo(oSet);
	} else {
		return false;
	}
}

unsigned int
OSOrderedSet::iteratorSize() const
{
	return sizeof(unsigned int);
}

bool
OSOrderedSet::initIterator(void *inIterator) const
{
	unsigned int *iteratorP = (unsigned int *) inIterator;

	*iteratorP = 0;
	return true;
}

bool
OSOrderedSet::
getNextObjectForIterator(void *inIterator, OSObject **ret) const
{
	unsigned int *iteratorP = (unsigned int *) inIterator;
	unsigned int index = (*iteratorP)++;

	if (index < count) {
		*ret = const_cast<OSObject *>((const OSObject *) array[index].obj);
	} else {
		*ret = NULL;
	}

	return *ret != NULL;
}


unsigned
OSOrderedSet::setOptions(unsigned options, unsigned mask, void *)
{
	unsigned old = super::setOptions(options, mask);
	if ((old ^ options) & mask) {
		// Value changed need to recurse over all of the child collections
		for (unsigned i = 0; i < count; i++) {
			OSCollection *coll = OSDynamicCast(OSCollection, array[i].obj);
			if (coll) {
				coll->setOptions(options, mask);
			}
		}
	}

	return old;
}

OSCollection *
OSOrderedSet::copyCollection(OSDictionary *cycleDict)
{
	bool allocDict = !cycleDict;
	OSCollection *ret = NULL;
	OSOrderedSet *newSet = NULL;

	if (allocDict) {
		cycleDict = OSDictionary::withCapacity(16);
		if (!cycleDict) {
			return NULL;
		}
	}

	do {
		// Check for a cycle
		ret = super::copyCollection(cycleDict);
		if (ret) {
			continue;
		}

		// Duplicate the set with no contents
		newSet = OSOrderedSet::withCapacity(capacity, ordering, orderingRef);
		if (!newSet) {
			continue;
		}

		// Insert object into cycle Dictionary
		cycleDict->setObject((const OSSymbol *) this, newSet);

		newSet->capacityIncrement = capacityIncrement;

		// Now copy over the contents to the new duplicate
		for (unsigned int i = 0; i < count; i++) {
			OSObject *obj = EXT_CAST(array[i].obj);
			OSCollection *coll = OSDynamicCast(OSCollection, obj);
			if (coll) {
				OSCollection *newColl = coll->copyCollection(cycleDict);
				if (newColl) {
					obj = newColl; // Rely on cycleDict ref for a bit
					newColl->release();
				} else {
					goto abortCopy;
				}
			}
			;
			newSet->setLastObject(obj);
		}
		;

		ret = newSet;
		newSet = NULL;
	} while (false);

abortCopy:
	if (newSet) {
		newSet->release();
	}

	if (allocDict) {
		cycleDict->release();
	}

	return ret;
}
