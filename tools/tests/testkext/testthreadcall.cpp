/*
 *  testthreadcall.cpp
 *  testkext
 *
 */

#include "testthreadcall.h"

#include <kern/thread_call.h>
#include <pexpert/pexpert.h>

#define super IOService
OSDefineMetaClassAndStructors(testthreadcall, super);

extern "C" {
static void thread_call_test_func(thread_call_param_t param0,
    thread_call_param_t param1);

static void thread_call_test_func2(thread_call_param_t param0,
    thread_call_param_t param1);
}

static int my_event;

bool
testthreadcall::start( IOService * provider )
{
	boolean_t ret;
	uint64_t deadline;
	int sleepret;
	uint32_t kernel_configuration;

	IOLog("%s\n", __PRETTY_FUNCTION__);

	if (!super::start(provider)) {
		return false;
	}

	kernel_configuration = PE_i_can_has_kernel_configuration();
	IOLog("%s: Assertions %s\n", __PRETTY_FUNCTION__,
	    (kernel_configuration & kPEICanHasAssertions) ? "enabled" : "disabled");
	IOLog("%s: Statistics %s\n", __PRETTY_FUNCTION__,
	    (kernel_configuration & kPEICanHasStatistics) ? "enabled" : "disabled");
	IOLog("%s: Diagnostic API %s\n", __PRETTY_FUNCTION__,
	    (kernel_configuration & kPEICanHasDiagnosticAPI) ? "enabled" : "disabled");

	IOLog("Attempting thread_call_allocate\n");
	tcall = thread_call_allocate(thread_call_test_func, this);
	IOLog("thread_call_t %p\n", tcall);

	tlock = IOSimpleLockAlloc();
	IOLog("tlock %p\n", tlock);

	clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
	IOLog("%d sec deadline is %llu\n", 5, deadline);

	ret = thread_call_enter_delayed(tcall, deadline);

	IOLog("Attempting thread_call_allocate\n");
	tcall2 = thread_call_allocate(thread_call_test_func2, this);
	IOLog("thread_call_t %p\n", tcall);

	tlock2 = IOLockAlloc();
	IOLog("tlock2 %p\n", tlock2);

	clock_interval_to_deadline(2, NSEC_PER_SEC, &deadline);
	IOLog("%d sec deadline is %llu\n", 2, deadline);

	ret = thread_call_enter_delayed(tcall2, deadline);

	IOLockLock(tlock2);

	clock_interval_to_deadline(3, NSEC_PER_SEC, &deadline);
	IOLog("%d sec deadline is %llu\n", 3, deadline);
	sleepret = IOLockSleepDeadline(tlock2, &my_event, deadline, THREAD_INTERRUPTIBLE);
	IOLog("IOLockSleepDeadline(&my_event, %llu) returned %d, expected 0\n", deadline, sleepret);

	IOLockUnlock(tlock2);

	clock_interval_to_deadline(4, NSEC_PER_SEC, &deadline);
	IOLog("%d sec deadline is %llu\n", 4, deadline);

	ret = thread_call_enter_delayed(tcall2, deadline);

	IOLockLock(tlock2);

	clock_interval_to_deadline(3, NSEC_PER_SEC, &deadline);
	IOLog("%d sec deadline is %llu\n", 3, deadline);
	sleepret = IOLockSleepDeadline(tlock2, &my_event, deadline, THREAD_INTERRUPTIBLE);
	IOLog("IOLockSleepDeadline(&my_event, %llu) returned %d, expected 1\n", deadline, sleepret);

	IOLockUnlock(tlock2);

	return true;
}

static void
thread_call_test_func(thread_call_param_t param0,
    thread_call_param_t param1)
{
	testthreadcall *self = (testthreadcall *)param0;

	IOLog("thread_call_test_func %p %p\n", param0, param1);

	IOSimpleLockLock(self->tlock);
	IOSimpleLockUnlock(self->tlock);
}

static void
thread_call_test_func2(thread_call_param_t param0,
    thread_call_param_t param1)
{
	testthreadcall *self = (testthreadcall *)param0;

	IOLog("thread_call_test_func2 %p %p\n", param0, param1);

	IOLockWakeup(self->tlock2, &my_event, false);
}
