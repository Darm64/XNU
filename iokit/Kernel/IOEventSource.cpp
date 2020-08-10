/*
 * Copyright (c) 1998-2000, 2009 Apple Inc. All rights reserved.
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
 *  Copyright (c) 1998 Apple Computer, Inc.  All rights reserved.
 *
 *  HISTORY
 *   1998-7-13	Godfrey van der Linden(gvdl)
 *       Created.
 *  ]*/
#include <IOKit/IOLib.h>

#include <IOKit/IOEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <libkern/Block.h>

#define super OSObject

OSDefineMetaClassAndAbstractStructors(IOEventSource, OSObject)

OSMetaClassDefineReservedUnused(IOEventSource, 0);
OSMetaClassDefineReservedUnused(IOEventSource, 1);
OSMetaClassDefineReservedUnused(IOEventSource, 2);
OSMetaClassDefineReservedUnused(IOEventSource, 3);
OSMetaClassDefineReservedUnused(IOEventSource, 4);
OSMetaClassDefineReservedUnused(IOEventSource, 5);
OSMetaClassDefineReservedUnused(IOEventSource, 6);
OSMetaClassDefineReservedUnused(IOEventSource, 7);

bool
IOEventSource::checkForWork()
{
	return false;
}

/* inline function implementations */

#if IOKITSTATS

#define IOStatisticsRegisterCounter() \
do { \
	reserved->counter = IOStatistics::registerEventSource(inOwner); \
} while (0)

#define IOStatisticsUnregisterCounter() \
do { \
	if (reserved) \
	        IOStatistics::unregisterEventSource(reserved->counter); \
} while (0)

#define IOStatisticsOpenGate() \
do { \
	IOStatistics::countOpenGate(reserved->counter); \
} while (0)

#define IOStatisticsCloseGate() \
do { \
	IOStatistics::countCloseGate(reserved->counter); \
} while (0)

#else

#define IOStatisticsRegisterCounter()
#define IOStatisticsUnregisterCounter()
#define IOStatisticsOpenGate()
#define IOStatisticsCloseGate()

#endif /* IOKITSTATS */

void
IOEventSource::signalWorkAvailable()
{
	workLoop->signalWorkAvailable();
}

void
IOEventSource::openGate()
{
	IOStatisticsOpenGate();
	workLoop->openGate();
}

void
IOEventSource::closeGate()
{
	workLoop->closeGate();
	IOStatisticsCloseGate();
}

bool
IOEventSource::tryCloseGate()
{
	bool res;
	if ((res = workLoop->tryCloseGate())) {
		IOStatisticsCloseGate();
	}
	return res;
}

int
IOEventSource::sleepGate(void *event, UInt32 type)
{
	int res;
	IOStatisticsOpenGate();
	res = workLoop->sleepGate(event, type);
	IOStatisticsCloseGate();
	return res;
}

int
IOEventSource::sleepGate(void *event, AbsoluteTime deadline, UInt32 type)
{
	int res;
	IOStatisticsOpenGate();
	res = workLoop->sleepGate(event, deadline, type);
	IOStatisticsCloseGate();
	return res;
}

void
IOEventSource::wakeupGate(void *event, bool oneThread)
{
	workLoop->wakeupGate(event, oneThread);
}


bool
IOEventSource::init(OSObject *inOwner,
    Action inAction)
{
	if (!inOwner) {
		return false;
	}

	owner = inOwner;

	if (!super::init()) {
		return false;
	}

	(void) setAction(inAction);
	enabled = true;

	if (!reserved) {
		reserved = IONew(ExpansionData, 1);
		if (!reserved) {
			return false;
		}
	}

	IOStatisticsRegisterCounter();

	return true;
}

void
IOEventSource::free( void )
{
	IOStatisticsUnregisterCounter();

	if ((kActionBlock & flags) && actionBlock) {
		Block_release(actionBlock);
	}

	if (reserved) {
		IODelete(reserved, ExpansionData, 1);
	}

	super::free();
}

void
IOEventSource::setRefcon(void *newrefcon)
{
	refcon = newrefcon;
}

void *
IOEventSource::getRefcon() const
{
	return refcon;
}

IOEventSource::Action
IOEventSource::getAction() const
{
	if (kActionBlock & flags) {
		return NULL;
	}
	return action;
}

IOEventSource::ActionBlock
IOEventSource::getActionBlock(ActionBlock) const
{
	if (kActionBlock & flags) {
		return actionBlock;
	}
	return NULL;
}

void
IOEventSource::setAction(Action inAction)
{
	if ((kActionBlock & flags) && actionBlock) {
		Block_release(actionBlock);
	}
	action = inAction;
}

void
IOEventSource::setActionBlock(ActionBlock block)
{
	if ((kActionBlock & flags) && actionBlock) {
		Block_release(actionBlock);
	}
	actionBlock = Block_copy(block);
	flags |= kActionBlock;
}

IOEventSource *
IOEventSource::getNext() const
{
	return eventChainNext;
};

void
IOEventSource::setNext(IOEventSource *inNext)
{
	eventChainNext = inNext;
}

void
IOEventSource::enable()
{
	enabled = true;
	if (workLoop) {
		return signalWorkAvailable();
	}
}

void
IOEventSource::disable()
{
	enabled = false;
}

bool
IOEventSource::isEnabled() const
{
	return enabled;
}

void
IOEventSource::setWorkLoop(IOWorkLoop *inWorkLoop)
{
	if (!inWorkLoop) {
		disable();
	}
	workLoop = inWorkLoop;
}

IOWorkLoop *
IOEventSource::getWorkLoop() const
{
	return workLoop;
}

bool
IOEventSource::onThread() const
{
	return (workLoop != NULL) && workLoop->onThread();
}
