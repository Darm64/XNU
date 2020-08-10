/*
 * Copyright (c) 2009 Apple Inc. All rights reserved.
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
#include <unistd.h>
#include <stdio.h>
#include <math.h>
#include <sys/kdebug.h>
#include <stdlib.h>
#include <pthread.h>
#include <errno.h>
#include <err.h>
#include <string.h>
#include <assert.h>
#include <sysexits.h>
#include <sys/sysctl.h>
#include <getopt.h>

#include <spawn.h>
#include <spawn_private.h>
#include <sys/spawn_internal.h>
#include <mach-o/dyld.h>

#include <mach/mach_time.h>
#include <mach/mach.h>
#include <mach/task.h>
#include <mach/semaphore.h>

#include <pthread/qos_private.h>

#include <sys/resource.h>

#include <stdatomic.h>

#include <os/tsd.h>

typedef enum wake_type { WAKE_BROADCAST_ONESEM, WAKE_BROADCAST_PERTHREAD, WAKE_CHAIN, WAKE_HOP } wake_type_t;
typedef enum my_policy_type { MY_POLICY_REALTIME, MY_POLICY_TIMESHARE, MY_POLICY_FIXEDPRI } my_policy_type_t;

#define mach_assert_zero(error)        do { if ((error) != 0) { fprintf(stderr, "[FAIL] error %d (%s) ", (error), mach_error_string(error)); assert(error == 0); } } while (0)
#define mach_assert_zero_t(tid, error) do { if ((error) != 0) { fprintf(stderr, "[FAIL] Thread %d error %d (%s) ", (tid), (error), mach_error_string(error)); assert(error == 0); } } while (0)
#define assert_zero_t(tid, error)      do { if ((error) != 0) { fprintf(stderr, "[FAIL] Thread %d error %d ", (tid), (error)); assert(error == 0); } } while (0)

#define CONSTRAINT_NANOS        (20000000ll)    /* 20 ms */
#define COMPUTATION_NANOS       (10000000ll)    /* 10 ms */
#define RT_CHURN_COMP_NANOS     ( 1000000ll)    /*  1 ms */
#define TRACEWORTHY_NANOS       (10000000ll)    /* 10 ms */
#define TRACEWORTHY_NANOS_TEST  ( 2000000ll)    /*  2 ms */

#if DEBUG
#define debug_log(args ...) printf(args)
#else
#define debug_log(args ...) do { } while(0)
#endif

/* Declarations */
static void*                    worker_thread(void *arg);
static void                     usage();
static int                      thread_setup(uint32_t my_id);
static my_policy_type_t         parse_thread_policy(const char *str);
static void                     selfexec_with_apptype(int argc, char *argv[]);
static void                     parse_args(int argc, char *argv[]);

static __attribute__((aligned(128))) _Atomic uint32_t   g_done_threads;
static __attribute__((aligned(128))) _Atomic boolean_t  g_churn_stop = FALSE;
static __attribute__((aligned(128))) _Atomic uint64_t   g_churn_stopped_at = 0;

/* Global variables (general) */
static uint32_t                 g_numcpus;
static uint32_t                 g_nphysicalcpu;
static uint32_t                 g_nlogicalcpu;
static uint32_t                 g_numthreads;
static wake_type_t              g_waketype;
static policy_t                 g_policy;
static uint32_t                 g_iterations;
static struct mach_timebase_info g_mti;
static semaphore_t              g_main_sem;
static uint64_t                *g_thread_endtimes_abs;
static boolean_t                g_verbose       = FALSE;
static boolean_t                g_do_affinity   = FALSE;
static uint64_t                 g_starttime_abs;
static uint32_t                 g_iteration_sleeptime_us = 0;
static uint32_t                 g_priority = 0;
static uint32_t                 g_churn_pri = 0;
static uint32_t                 g_churn_count = 0;
static uint32_t                 g_rt_churn_count = 0;

static pthread_t*               g_churn_threads = NULL;
static pthread_t*               g_rt_churn_threads = NULL;

/* Threshold for dropping a 'bad run' tracepoint */
static uint64_t                 g_traceworthy_latency_ns = TRACEWORTHY_NANOS;

/* Have we re-execed to set apptype? */
static boolean_t                g_seen_apptype = FALSE;

/* usleep in betweeen iterations */
static boolean_t                g_do_sleep      = TRUE;

/* Every thread spins until all threads have checked in */
static boolean_t                g_do_all_spin = FALSE;

/* Every thread backgrounds temporarily before parking */
static boolean_t                g_drop_priority = FALSE;

/* Test whether realtime threads are scheduled on the separate CPUs */
static boolean_t                g_test_rt = FALSE;

static boolean_t                g_rt_churn = FALSE;

/* On SMT machines, test whether realtime threads are scheduled on the correct CPUs */
static boolean_t                g_test_rt_smt = FALSE;

/* Test whether realtime threads are successfully avoiding CPU 0 on Intel */
static boolean_t                g_test_rt_avoid0 = FALSE;

/* Print a histgram showing how many threads ran on each CPU */
static boolean_t                g_histogram = FALSE;

/* One randomly chosen thread holds up the train for a certain duration. */
static boolean_t                g_do_one_long_spin = FALSE;
static uint32_t                 g_one_long_spin_id = 0;
static uint64_t                 g_one_long_spin_length_abs = 0;
static uint64_t                 g_one_long_spin_length_ns = 0;

/* Each thread spins for a certain duration after waking up before blocking again. */
static boolean_t                g_do_each_spin = FALSE;
static uint64_t                 g_each_spin_duration_abs = 0;
static uint64_t                 g_each_spin_duration_ns = 0;

/* Global variables (broadcast) */
static semaphore_t              g_broadcastsem;
static semaphore_t              g_leadersem;
static semaphore_t              g_readysem;
static semaphore_t              g_donesem;
static semaphore_t              g_rt_churn_sem;
static semaphore_t              g_rt_churn_start_sem;

/* Global variables (chain) */
static semaphore_t             *g_semarr;

typedef struct {
	__attribute__((aligned(128))) uint32_t current;
	uint32_t accum;
} histogram_t;

static histogram_t             *g_cpu_histogram;
static _Atomic uint64_t        *g_cpu_map;

static uint64_t
abs_to_nanos(uint64_t abstime)
{
	return (uint64_t)(abstime * (((double)g_mti.numer) / ((double)g_mti.denom)));
}

static uint64_t
nanos_to_abs(uint64_t ns)
{
	return (uint64_t)(ns * (((double)g_mti.denom) / ((double)g_mti.numer)));
}

inline static void
yield(void)
{
#if defined(__arm__) || defined(__arm64__)
	asm volatile ("yield");
#elif defined(__x86_64__) || defined(__i386__)
	asm volatile ("pause");
#else
#error Unrecognized architecture
#endif
}

static void *
churn_thread(__unused void *arg)
{
	uint64_t spin_count = 0;

	/*
	 * As a safety measure to avoid wedging, we will bail on the spin if
	 * it's been more than 1s after the most recent run start
	 */

	while (g_churn_stop == FALSE &&
	    mach_absolute_time() < (g_starttime_abs + NSEC_PER_SEC)) {
		spin_count++;
		yield();
	}

	/* This is totally racy, but only here to detect if anyone stops early */
	atomic_fetch_add_explicit(&g_churn_stopped_at, spin_count, memory_order_relaxed);

	return NULL;
}

static void
create_churn_threads()
{
	if (g_churn_count == 0) {
		g_churn_count = g_numcpus - 1;
	}

	errno_t err;

	struct sched_param param = { .sched_priority = (int)g_churn_pri };
	pthread_attr_t attr;

	/* Array for churn threads */
	g_churn_threads = (pthread_t*) valloc(sizeof(pthread_t) * g_churn_count);
	assert(g_churn_threads);

	if ((err = pthread_attr_init(&attr))) {
		errc(EX_OSERR, err, "pthread_attr_init");
	}

	if ((err = pthread_attr_setschedparam(&attr, &param))) {
		errc(EX_OSERR, err, "pthread_attr_setschedparam");
	}

	if ((err = pthread_attr_setschedpolicy(&attr, SCHED_RR))) {
		errc(EX_OSERR, err, "pthread_attr_setschedpolicy");
	}

	for (uint32_t i = 0; i < g_churn_count; i++) {
		pthread_t new_thread;

		if ((err = pthread_create(&new_thread, &attr, churn_thread, NULL))) {
			errc(EX_OSERR, err, "pthread_create");
		}
		g_churn_threads[i] = new_thread;
	}

	if ((err = pthread_attr_destroy(&attr))) {
		errc(EX_OSERR, err, "pthread_attr_destroy");
	}
}

static void
join_churn_threads(void)
{
	if (atomic_load_explicit(&g_churn_stopped_at, memory_order_seq_cst) != 0) {
		printf("Warning: Some of the churn threads may have stopped early: %lld\n",
		    g_churn_stopped_at);
	}

	atomic_store_explicit(&g_churn_stop, TRUE, memory_order_seq_cst);

	/* Rejoin churn threads */
	for (uint32_t i = 0; i < g_churn_count; i++) {
		errno_t err = pthread_join(g_churn_threads[i], NULL);
		if (err) {
			errc(EX_OSERR, err, "pthread_join %d", i);
		}
	}
}

/*
 * Set policy
 */
static int
rt_churn_thread_setup(void)
{
	kern_return_t kr;
	thread_time_constraint_policy_data_t pol;

	/* Hard-coded realtime parameters (similar to what Digi uses) */
	pol.period      = 100000;
	pol.constraint  = (uint32_t) nanos_to_abs(CONSTRAINT_NANOS * 2);
	pol.computation = (uint32_t) nanos_to_abs(RT_CHURN_COMP_NANOS * 2);
	pol.preemptible = 0;         /* Ignored by OS */

	kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
	    (thread_policy_t) &pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
	mach_assert_zero_t(0, kr);

	return 0;
}

static void *
rt_churn_thread(__unused void *arg)
{
	rt_churn_thread_setup();

	for (uint32_t i = 0; i < g_iterations; i++) {
		kern_return_t kr = semaphore_wait_signal(g_rt_churn_start_sem, g_rt_churn_sem);
		mach_assert_zero_t(0, kr);

		volatile double x = 0.0;
		volatile double y = 0.0;

		uint64_t endspin = mach_absolute_time() + nanos_to_abs(RT_CHURN_COMP_NANOS);
		while (mach_absolute_time() < endspin) {
			y = y + 1.5 + x;
			x = sqrt(y);
		}
	}

	kern_return_t kr = semaphore_signal(g_rt_churn_sem);
	mach_assert_zero_t(0, kr);

	return NULL;
}

static void
wait_for_rt_churn_threads(void)
{
	for (uint32_t i = 0; i < g_rt_churn_count; i++) {
		kern_return_t kr = semaphore_wait(g_rt_churn_sem);
		mach_assert_zero_t(0, kr);
	}
}

static void
start_rt_churn_threads(void)
{
	for (uint32_t i = 0; i < g_rt_churn_count; i++) {
		kern_return_t kr = semaphore_signal(g_rt_churn_start_sem);
		mach_assert_zero_t(0, kr);
	}
}

static void
create_rt_churn_threads(void)
{
	if (g_rt_churn_count == 0) {
		/* Leave 1 CPU to ensure that the main thread can make progress */
		g_rt_churn_count = g_numcpus - 1;
	}

	errno_t err;

	struct sched_param param = { .sched_priority = (int)g_churn_pri };
	pthread_attr_t attr;

	/* Array for churn threads */
	g_rt_churn_threads = (pthread_t*) valloc(sizeof(pthread_t) * g_rt_churn_count);
	assert(g_rt_churn_threads);

	if ((err = pthread_attr_init(&attr))) {
		errc(EX_OSERR, err, "pthread_attr_init");
	}

	if ((err = pthread_attr_setschedparam(&attr, &param))) {
		errc(EX_OSERR, err, "pthread_attr_setschedparam");
	}

	if ((err = pthread_attr_setschedpolicy(&attr, SCHED_RR))) {
		errc(EX_OSERR, err, "pthread_attr_setschedpolicy");
	}

	for (uint32_t i = 0; i < g_rt_churn_count; i++) {
		pthread_t new_thread;

		if ((err = pthread_create(&new_thread, &attr, rt_churn_thread, NULL))) {
			errc(EX_OSERR, err, "pthread_create");
		}
		g_rt_churn_threads[i] = new_thread;
	}

	if ((err = pthread_attr_destroy(&attr))) {
		errc(EX_OSERR, err, "pthread_attr_destroy");
	}

	/* Wait until all threads have checked in */
	wait_for_rt_churn_threads();
}

static void
join_rt_churn_threads(void)
{
	/* Rejoin rt churn threads */
	for (uint32_t i = 0; i < g_rt_churn_count; i++) {
		errno_t err = pthread_join(g_rt_churn_threads[i], NULL);
		if (err) {
			errc(EX_OSERR, err, "pthread_join %d", i);
		}
	}
}

/*
 * Figure out what thread policy to use
 */
static my_policy_type_t
parse_thread_policy(const char *str)
{
	if (strcmp(str, "timeshare") == 0) {
		return MY_POLICY_TIMESHARE;
	} else if (strcmp(str, "realtime") == 0) {
		return MY_POLICY_REALTIME;
	} else if (strcmp(str, "fixed") == 0) {
		return MY_POLICY_FIXEDPRI;
	} else {
		errx(EX_USAGE, "Invalid thread policy \"%s\"", str);
	}
}

/*
 * Figure out what wakeup pattern to use
 */
static wake_type_t
parse_wakeup_pattern(const char *str)
{
	if (strcmp(str, "chain") == 0) {
		return WAKE_CHAIN;
	} else if (strcmp(str, "hop") == 0) {
		return WAKE_HOP;
	} else if (strcmp(str, "broadcast-single-sem") == 0) {
		return WAKE_BROADCAST_ONESEM;
	} else if (strcmp(str, "broadcast-per-thread") == 0) {
		return WAKE_BROADCAST_PERTHREAD;
	} else {
		errx(EX_USAGE, "Invalid wakeup pattern \"%s\"", str);
	}
}

/*
 * Set policy
 */
static int
thread_setup(uint32_t my_id)
{
	kern_return_t kr;
	errno_t ret;
	thread_time_constraint_policy_data_t pol;

	if (g_priority) {
		int policy = SCHED_OTHER;
		if (g_policy == MY_POLICY_FIXEDPRI) {
			policy = SCHED_RR;
		}

		struct sched_param param = {.sched_priority = (int)g_priority};
		if ((ret = pthread_setschedparam(pthread_self(), policy, &param))) {
			errc(EX_OSERR, ret, "pthread_setschedparam: %d", my_id);
		}
	}

	switch (g_policy) {
	case MY_POLICY_TIMESHARE:
		break;
	case MY_POLICY_REALTIME:
		/* Hard-coded realtime parameters (similar to what Digi uses) */
		pol.period      = 100000;
		pol.constraint  = (uint32_t) nanos_to_abs(CONSTRAINT_NANOS);
		pol.computation = (uint32_t) nanos_to_abs(COMPUTATION_NANOS);
		pol.preemptible = 0;         /* Ignored by OS */

		kr = thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
		    (thread_policy_t) &pol, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
		mach_assert_zero_t(my_id, kr);
		break;
	case MY_POLICY_FIXEDPRI:
		ret = pthread_set_fixedpriority_self();
		if (ret) {
			errc(EX_OSERR, ret, "pthread_set_fixedpriority_self");
		}
		break;
	default:
		errx(EX_USAGE, "invalid policy type %d", g_policy);
	}

	if (g_do_affinity) {
		thread_affinity_policy_data_t affinity;

		affinity.affinity_tag = my_id % 2;

		kr = thread_policy_set(mach_thread_self(), THREAD_AFFINITY_POLICY,
		    (thread_policy_t)&affinity, THREAD_AFFINITY_POLICY_COUNT);
		mach_assert_zero_t(my_id, kr);
	}

	return 0;
}

/*
 * Wait for a wakeup, potentially wake up another of the "0-N" threads,
 * and notify the main thread when done.
 */
static void*
worker_thread(void *arg)
{
	uint32_t my_id = (uint32_t)(uintptr_t)arg;
	kern_return_t kr;

	volatile double x = 0.0;
	volatile double y = 0.0;

	/* Set policy and so forth */
	thread_setup(my_id);

	for (uint32_t i = 0; i < g_iterations; i++) {
		if (my_id == 0) {
			/*
			 * Leader thread either wakes everyone up or starts the chain going.
			 */

			/* Give the worker threads undisturbed time to finish before waiting on them */
			if (g_do_sleep) {
				usleep(g_iteration_sleeptime_us);
			}

			debug_log("%d Leader thread wait for ready\n", i);

			/*
			 * Wait for everyone else to declare ready
			 * Is there a better way to do this that won't interfere with the rest of the chain?
			 * TODO: Invent 'semaphore wait for N signals'
			 */

			for (uint32_t j = 0; j < g_numthreads - 1; j++) {
				kr = semaphore_wait(g_readysem);
				mach_assert_zero_t(my_id, kr);
			}

			debug_log("%d Leader thread wait\n", i);

			if (i > 0) {
				for (int cpuid = 0; cpuid < g_numcpus; cpuid++) {
					if (g_cpu_histogram[cpuid].current == 1) {
						atomic_fetch_or_explicit(&g_cpu_map[i - 1], (1UL << cpuid), memory_order_relaxed);
						g_cpu_histogram[cpuid].current = 0;
					}
				}
			}

			/* Signal main thread and wait for start of iteration */

			kr = semaphore_wait_signal(g_leadersem, g_main_sem);
			mach_assert_zero_t(my_id, kr);

			g_thread_endtimes_abs[my_id] = mach_absolute_time();

			debug_log("%d Leader thread go\n", i);

			assert_zero_t(my_id, atomic_load_explicit(&g_done_threads, memory_order_relaxed));

			switch (g_waketype) {
			case WAKE_BROADCAST_ONESEM:
				kr = semaphore_signal_all(g_broadcastsem);
				mach_assert_zero_t(my_id, kr);
				break;
			case WAKE_BROADCAST_PERTHREAD:
				for (uint32_t j = 1; j < g_numthreads; j++) {
					kr = semaphore_signal(g_semarr[j]);
					mach_assert_zero_t(my_id, kr);
				}
				break;
			case WAKE_CHAIN:
				kr = semaphore_signal(g_semarr[my_id + 1]);
				mach_assert_zero_t(my_id, kr);
				break;
			case WAKE_HOP:
				kr = semaphore_wait_signal(g_donesem, g_semarr[my_id + 1]);
				mach_assert_zero_t(my_id, kr);
				break;
			}
		} else {
			/*
			 * Everyone else waits to be woken up,
			 * records when she wakes up, and possibly
			 * wakes up a friend.
			 */
			switch (g_waketype) {
			case WAKE_BROADCAST_ONESEM:
				kr = semaphore_wait_signal(g_broadcastsem, g_readysem);
				mach_assert_zero_t(my_id, kr);

				g_thread_endtimes_abs[my_id] = mach_absolute_time();
				break;

			case WAKE_BROADCAST_PERTHREAD:
				kr = semaphore_wait_signal(g_semarr[my_id], g_readysem);
				mach_assert_zero_t(my_id, kr);

				g_thread_endtimes_abs[my_id] = mach_absolute_time();
				break;

			case WAKE_CHAIN:
				kr = semaphore_wait_signal(g_semarr[my_id], g_readysem);
				mach_assert_zero_t(my_id, kr);

				/* Signal the next thread *after* recording wake time */

				g_thread_endtimes_abs[my_id] = mach_absolute_time();

				if (my_id < (g_numthreads - 1)) {
					kr = semaphore_signal(g_semarr[my_id + 1]);
					mach_assert_zero_t(my_id, kr);
				}

				break;

			case WAKE_HOP:
				kr = semaphore_wait_signal(g_semarr[my_id], g_readysem);
				mach_assert_zero_t(my_id, kr);

				/* Signal the next thread *after* recording wake time */

				g_thread_endtimes_abs[my_id] = mach_absolute_time();

				if (my_id < (g_numthreads - 1)) {
					kr = semaphore_wait_signal(g_donesem, g_semarr[my_id + 1]);
					mach_assert_zero_t(my_id, kr);
				} else {
					kr = semaphore_signal_all(g_donesem);
					mach_assert_zero_t(my_id, kr);
				}

				break;
			}
		}

		unsigned int cpuid =  _os_cpu_number();
		assert(cpuid < g_numcpus);
		debug_log("Thread %p woke up on CPU %d for iteration %d.\n", pthread_self(), cpuid, i);
		g_cpu_histogram[cpuid].current = 1;
		g_cpu_histogram[cpuid].accum++;

		if (g_do_one_long_spin && g_one_long_spin_id == my_id) {
			/* One randomly chosen thread holds up the train for a while. */

			uint64_t endspin = g_starttime_abs + g_one_long_spin_length_abs;
			while (mach_absolute_time() < endspin) {
				y = y + 1.5 + x;
				x = sqrt(y);
			}
		}

		if (g_do_each_spin) {
			/* Each thread spins for a certain duration after waking up before blocking again. */

			uint64_t endspin = mach_absolute_time() + g_each_spin_duration_abs;
			while (mach_absolute_time() < endspin) {
				y = y + 1.5 + x;
				x = sqrt(y);
			}
		}

		uint32_t done_threads;
		done_threads = atomic_fetch_add_explicit(&g_done_threads, 1, memory_order_relaxed) + 1;

		debug_log("Thread %p new value is %d, iteration %d\n", pthread_self(), done_threads, i);

		if (g_drop_priority) {
			/* Drop priority to BG momentarily */
			errno_t ret = setpriority(PRIO_DARWIN_THREAD, 0, PRIO_DARWIN_BG);
			if (ret) {
				errc(EX_OSERR, ret, "setpriority PRIO_DARWIN_BG");
			}
		}

		if (g_do_all_spin) {
			/* Everyone spins until the last thread checks in. */

			while (atomic_load_explicit(&g_done_threads, memory_order_relaxed) < g_numthreads) {
				y = y + 1.5 + x;
				x = sqrt(y);
			}
		}

		if (g_drop_priority) {
			/* Restore normal priority */
			errno_t ret = setpriority(PRIO_DARWIN_THREAD, 0, 0);
			if (ret) {
				errc(EX_OSERR, ret, "setpriority 0");
			}
		}

		debug_log("Thread %p done spinning, iteration %d\n", pthread_self(), i);
	}

	if (my_id == 0) {
		/* Give the worker threads undisturbed time to finish before waiting on them */
		if (g_do_sleep) {
			usleep(g_iteration_sleeptime_us);
		}

		/* Wait for the worker threads to finish */
		for (uint32_t i = 0; i < g_numthreads - 1; i++) {
			kr = semaphore_wait(g_readysem);
			mach_assert_zero_t(my_id, kr);
		}

		/* Tell everyone and the main thread that the last iteration is done */
		debug_log("%d Leader thread done\n", g_iterations - 1);

		for (int cpuid = 0; cpuid < g_numcpus; cpuid++) {
			if (g_cpu_histogram[cpuid].current == 1) {
				atomic_fetch_or_explicit(&g_cpu_map[g_iterations - 1], (1UL << cpuid), memory_order_relaxed);
				g_cpu_histogram[cpuid].current = 0;
			}
		}

		kr = semaphore_signal_all(g_main_sem);
		mach_assert_zero_t(my_id, kr);
	} else {
		/* Hold up thread teardown so it doesn't affect the last iteration */
		kr = semaphore_wait_signal(g_main_sem, g_readysem);
		mach_assert_zero_t(my_id, kr);
	}

	return 0;
}

/*
 * Given an array of uint64_t values, compute average, max, min, and standard deviation
 */
static void
compute_stats(uint64_t *values, uint64_t count, float *averagep, uint64_t *maxp, uint64_t *minp, float *stddevp)
{
	uint32_t i;
	uint64_t _sum = 0;
	uint64_t _max = 0;
	uint64_t _min = UINT64_MAX;
	float    _avg = 0;
	float    _dev = 0;

	for (i = 0; i < count; i++) {
		_sum += values[i];
		_max = values[i] > _max ? values[i] : _max;
		_min = values[i] < _min ? values[i] : _min;
	}

	_avg = ((float)_sum) / ((float)count);

	_dev = 0;
	for (i = 0; i < count; i++) {
		_dev += powf((((float)values[i]) - _avg), 2);
	}

	_dev /= count;
	_dev = sqrtf(_dev);

	*averagep = _avg;
	*maxp = _max;
	*minp = _min;
	*stddevp = _dev;
}

int
main(int argc, char **argv)
{
	errno_t ret;
	kern_return_t kr;

	pthread_t       *threads;
	uint64_t        *worst_latencies_ns;
	uint64_t        *worst_latencies_from_first_ns;
	uint64_t        max, min;
	float           avg, stddev;

	bool test_fail = false;

	for (int i = 0; i < argc; i++) {
		if (strcmp(argv[i], "--switched_apptype") == 0) {
			g_seen_apptype = TRUE;
		}
	}

	if (!g_seen_apptype) {
		selfexec_with_apptype(argc, argv);
	}

	parse_args(argc, argv);

	srand((unsigned int)time(NULL));

	mach_timebase_info(&g_mti);

	size_t ncpu_size = sizeof(g_numcpus);
	ret = sysctlbyname("hw.ncpu", &g_numcpus, &ncpu_size, NULL, 0);
	if (ret) {
		err(EX_OSERR, "Failed sysctlbyname(hw.ncpu)");
	}
	assert(g_numcpus <= 64); /* g_cpu_map needs to be extended for > 64 cpus */

	size_t physicalcpu_size = sizeof(g_nphysicalcpu);
	ret = sysctlbyname("hw.physicalcpu", &g_nphysicalcpu, &physicalcpu_size, NULL, 0);
	if (ret) {
		err(EX_OSERR, "Failed sysctlbyname(hw.physicalcpu)");
	}

	size_t logicalcpu_size = sizeof(g_nlogicalcpu);
	ret = sysctlbyname("hw.logicalcpu", &g_nlogicalcpu, &logicalcpu_size, NULL, 0);
	if (ret) {
		err(EX_OSERR, "Failed sysctlbyname(hw.logicalcpu)");
	}

	if (g_test_rt) {
		if (g_numthreads == 0) {
			g_numthreads = g_numcpus;
		}
		g_policy = MY_POLICY_REALTIME;
		g_do_all_spin = TRUE;
		g_histogram = true;
		/* Don't change g_traceworthy_latency_ns if it's explicity been set to something other than the default */
		if (g_traceworthy_latency_ns == TRACEWORTHY_NANOS) {
			g_traceworthy_latency_ns = TRACEWORTHY_NANOS_TEST;
		}
	} else if (g_test_rt_smt) {
		if (g_nlogicalcpu != 2 * g_nphysicalcpu) {
			/* Not SMT */
			printf("Attempt to run --test-rt-smt on a non-SMT device\n");
			exit(0);
		}

		if (g_numthreads == 0) {
			g_numthreads = g_nphysicalcpu;
		}
		g_policy = MY_POLICY_REALTIME;
		g_do_all_spin = TRUE;
		g_histogram = true;
	} else if (g_test_rt_avoid0) {
#if defined(__x86_64__) || defined(__i386__)
		if (g_numthreads == 0) {
			g_numthreads = g_nphysicalcpu - 1;
		}
		if (g_numthreads == 0) {
			printf("Attempt to run --test-rt-avoid0 on a uniprocessor\n");
			exit(0);
		}
		g_policy = MY_POLICY_REALTIME;
		g_do_all_spin = TRUE;
		g_histogram = true;
#else
		printf("Attempt to run --test-rt-avoid0 on a non-Intel device\n");
		exit(0);
#endif
	} else if (g_numthreads == 0) {
		g_numthreads = g_numcpus;
	}

	if (g_do_each_spin) {
		g_each_spin_duration_abs = nanos_to_abs(g_each_spin_duration_ns);
	}

	/* Configure the long-spin thread to take up half of its computation */
	if (g_do_one_long_spin) {
		g_one_long_spin_length_ns = COMPUTATION_NANOS / 2;
		g_one_long_spin_length_abs = nanos_to_abs(g_one_long_spin_length_ns);
	}

	/* Estimate the amount of time the cleanup phase needs to back off */
	g_iteration_sleeptime_us = g_numthreads * 20;

	uint32_t threads_per_core = (g_numthreads / g_numcpus) + 1;
	if (g_do_each_spin) {
		g_iteration_sleeptime_us += threads_per_core * (g_each_spin_duration_ns / NSEC_PER_USEC);
	}
	if (g_do_one_long_spin) {
		g_iteration_sleeptime_us += g_one_long_spin_length_ns / NSEC_PER_USEC;
	}

	/* Arrays for threads and their wakeup times */
	threads = (pthread_t*) valloc(sizeof(pthread_t) * g_numthreads);
	assert(threads);

	size_t endtimes_size = sizeof(uint64_t) * g_numthreads;

	g_thread_endtimes_abs = (uint64_t*) valloc(endtimes_size);
	assert(g_thread_endtimes_abs);

	/* Ensure the allocation is pre-faulted */
	ret = memset_s(g_thread_endtimes_abs, endtimes_size, 0, endtimes_size);
	if (ret) {
		errc(EX_OSERR, ret, "memset_s endtimes");
	}

	size_t latencies_size = sizeof(uint64_t) * g_iterations;

	worst_latencies_ns = (uint64_t*) valloc(latencies_size);
	assert(worst_latencies_ns);

	/* Ensure the allocation is pre-faulted */
	ret = memset_s(worst_latencies_ns, latencies_size, 0, latencies_size);
	if (ret) {
		errc(EX_OSERR, ret, "memset_s latencies");
	}

	worst_latencies_from_first_ns = (uint64_t*) valloc(latencies_size);
	assert(worst_latencies_from_first_ns);

	/* Ensure the allocation is pre-faulted */
	ret = memset_s(worst_latencies_from_first_ns, latencies_size, 0, latencies_size);
	if (ret) {
		errc(EX_OSERR, ret, "memset_s latencies_from_first");
	}

	size_t histogram_size = sizeof(histogram_t) * g_numcpus;
	g_cpu_histogram = (histogram_t *)valloc(histogram_size);
	assert(g_cpu_histogram);
	/* Ensure the allocation is pre-faulted */
	ret = memset_s(g_cpu_histogram, histogram_size, 0, histogram_size);
	if (ret) {
		errc(EX_OSERR, ret, "memset_s g_cpu_histogram");
	}

	size_t map_size = sizeof(uint64_t) * g_iterations;
	g_cpu_map = (_Atomic uint64_t *)valloc(map_size);
	assert(g_cpu_map);
	/* Ensure the allocation is pre-faulted */
	ret = memset_s(g_cpu_map, map_size, 0, map_size);
	if (ret) {
		errc(EX_OSERR, ret, "memset_s g_cpu_map");
	}

	kr = semaphore_create(mach_task_self(), &g_main_sem, SYNC_POLICY_FIFO, 0);
	mach_assert_zero(kr);

	/* Either one big semaphore or one per thread */
	if (g_waketype == WAKE_CHAIN ||
	    g_waketype == WAKE_BROADCAST_PERTHREAD ||
	    g_waketype == WAKE_HOP) {
		g_semarr = valloc(sizeof(semaphore_t) * g_numthreads);
		assert(g_semarr);

		for (uint32_t i = 0; i < g_numthreads; i++) {
			kr = semaphore_create(mach_task_self(), &g_semarr[i], SYNC_POLICY_FIFO, 0);
			mach_assert_zero(kr);
		}

		g_leadersem = g_semarr[0];
	} else {
		kr = semaphore_create(mach_task_self(), &g_broadcastsem, SYNC_POLICY_FIFO, 0);
		mach_assert_zero(kr);
		kr = semaphore_create(mach_task_self(), &g_leadersem, SYNC_POLICY_FIFO, 0);
		mach_assert_zero(kr);
	}

	if (g_waketype == WAKE_HOP) {
		kr = semaphore_create(mach_task_self(), &g_donesem, SYNC_POLICY_FIFO, 0);
		mach_assert_zero(kr);
	}

	kr = semaphore_create(mach_task_self(), &g_readysem, SYNC_POLICY_FIFO, 0);
	mach_assert_zero(kr);

	kr = semaphore_create(mach_task_self(), &g_rt_churn_sem, SYNC_POLICY_FIFO, 0);
	mach_assert_zero(kr);

	kr = semaphore_create(mach_task_self(), &g_rt_churn_start_sem, SYNC_POLICY_FIFO, 0);
	mach_assert_zero(kr);

	atomic_store_explicit(&g_done_threads, 0, memory_order_relaxed);

	/* Create the threads */
	for (uint32_t i = 0; i < g_numthreads; i++) {
		ret = pthread_create(&threads[i], NULL, worker_thread, (void*)(uintptr_t)i);
		if (ret) {
			errc(EX_OSERR, ret, "pthread_create %d", i);
		}
	}

	ret = setpriority(PRIO_DARWIN_ROLE, 0, PRIO_DARWIN_ROLE_UI_FOCAL);
	if (ret) {
		errc(EX_OSERR, ret, "setpriority");
	}

	thread_setup(0);

	g_starttime_abs = mach_absolute_time();

	if (g_churn_pri) {
		create_churn_threads();
	}
	if (g_rt_churn) {
		create_rt_churn_threads();
	}

	/* Let everyone get settled */
	kr = semaphore_wait(g_main_sem);
	mach_assert_zero(kr);

	/* Give the system a bit more time to settle */
	if (g_do_sleep) {
		usleep(g_iteration_sleeptime_us);
	}

	/* Go! */
	for (uint32_t i = 0; i < g_iterations; i++) {
		uint32_t j;
		uint64_t worst_abs = 0, best_abs = UINT64_MAX;

		if (g_do_one_long_spin) {
			g_one_long_spin_id = (uint32_t)rand() % g_numthreads;
		}

		if (g_rt_churn) {
			start_rt_churn_threads();
			usleep(100);
		}

		debug_log("%d Main thread reset\n", i);

		atomic_store_explicit(&g_done_threads, 0, memory_order_seq_cst);

		g_starttime_abs = mach_absolute_time();

		/* Fire them off and wait for worker threads to finish */
		kr = semaphore_wait_signal(g_main_sem, g_leadersem);
		mach_assert_zero(kr);

		debug_log("%d Main thread return\n", i);

		assert(atomic_load_explicit(&g_done_threads, memory_order_relaxed) == g_numthreads);

		if (g_rt_churn) {
			wait_for_rt_churn_threads();
		}

		/*
		 * We report the worst latencies relative to start time
		 * and relative to the lead worker thread.
		 */
		for (j = 0; j < g_numthreads; j++) {
			uint64_t latency_abs;

			latency_abs = g_thread_endtimes_abs[j] - g_starttime_abs;
			worst_abs = worst_abs < latency_abs ? latency_abs : worst_abs;
		}

		worst_latencies_ns[i] = abs_to_nanos(worst_abs);

		worst_abs = 0;
		for (j = 1; j < g_numthreads; j++) {
			uint64_t latency_abs;

			latency_abs = g_thread_endtimes_abs[j] - g_thread_endtimes_abs[0];
			worst_abs = worst_abs < latency_abs ? latency_abs : worst_abs;
			best_abs = best_abs > latency_abs ? latency_abs : best_abs;
		}

		worst_latencies_from_first_ns[i] = abs_to_nanos(worst_abs);

		/*
		 * In the event of a bad run, cut a trace point.
		 */
		if (worst_latencies_from_first_ns[i] > g_traceworthy_latency_ns) {
			/* Ariadne's ad-hoc test signpost */
			kdebug_trace(ARIADNEDBG_CODE(0, 0), worst_latencies_from_first_ns[i], g_traceworthy_latency_ns, 0, 0);

			if (g_verbose) {
				printf("Worst on this round was %.2f us.\n", ((float)worst_latencies_from_first_ns[i]) / 1000.0);
			}
		}

		/* Give the system a bit more time to settle */
		if (g_do_sleep) {
			usleep(g_iteration_sleeptime_us);
		}
	}

	/* Rejoin threads */
	for (uint32_t i = 0; i < g_numthreads; i++) {
		ret = pthread_join(threads[i], NULL);
		if (ret) {
			errc(EX_OSERR, ret, "pthread_join %d", i);
		}
	}

	if (g_rt_churn) {
		join_rt_churn_threads();
	}

	if (g_churn_pri) {
		join_churn_threads();
	}

	compute_stats(worst_latencies_ns, g_iterations, &avg, &max, &min, &stddev);
	printf("Results (from a stop):\n");
	printf("Max:\t\t%.2f us\n", ((float)max) / 1000.0);
	printf("Min:\t\t%.2f us\n", ((float)min) / 1000.0);
	printf("Avg:\t\t%.2f us\n", avg / 1000.0);
	printf("Stddev:\t\t%.2f us\n", stddev / 1000.0);

	putchar('\n');

	compute_stats(worst_latencies_from_first_ns, g_iterations, &avg, &max, &min, &stddev);
	printf("Results (relative to first thread):\n");
	printf("Max:\t\t%.2f us\n", ((float)max) / 1000.0);
	printf("Min:\t\t%.2f us\n", ((float)min) / 1000.0);
	printf("Avg:\t\t%.2f us\n", avg / 1000.0);
	printf("Stddev:\t\t%.2f us\n", stddev / 1000.0);

#if 0
	for (uint32_t i = 0; i < g_iterations; i++) {
		printf("Iteration %d: %f us\n", i, worst_latencies_ns[i] / 1000.0);
	}
#endif

	if (g_histogram) {
		putchar('\n');

		for (uint32_t i = 0; i < g_numcpus; i++) {
			printf("%d\t%d\n", i, g_cpu_histogram[i].accum);
		}
	}

	if (g_test_rt || g_test_rt_smt || g_test_rt_avoid0) {
#define PRIMARY   0x5555555555555555ULL
#define SECONDARY 0xaaaaaaaaaaaaaaaaULL

		int fail_count = 0;

		for (uint32_t i = 0; i < g_iterations; i++) {
			bool secondary = false;
			bool fail = false;
			uint64_t map = g_cpu_map[i];
			if (g_test_rt_smt) {
				/* Test for one or more threads running on secondary cores unexpectedly (WARNING) */
				secondary = (map & SECONDARY);
				/* Test for threads running on both primary and secondary cpus of the same core (FAIL) */
				fail = ((map & PRIMARY) & ((map & SECONDARY) >> 1));
			} else if (g_test_rt) {
				fail = (__builtin_popcountll(map) != g_numthreads) && (worst_latencies_ns[i] > g_traceworthy_latency_ns);
			} else if (g_test_rt_avoid0) {
				fail = ((map & 0x1) == 0x1);
			}
			if (secondary || fail) {
				printf("Iteration %d: 0x%llx%s%s\n", i, map,
				    secondary ? " SECONDARY" : "",
				    fail ? " FAIL" : "");
			}
			test_fail |= fail;
			fail_count += fail;
		}

		if (test_fail && (g_iterations >= 100) && (fail_count <= g_iterations / 100)) {
			printf("99%% or better success rate\n");
			test_fail = 0;
		}
	}

	free(threads);
	free(g_thread_endtimes_abs);
	free(worst_latencies_ns);
	free(worst_latencies_from_first_ns);
	free(g_cpu_histogram);
	free(g_cpu_map);

	return test_fail;
}

/*
 * WARNING: This is SPI specifically intended for use by launchd to start UI
 * apps. We use it here for a test tool only to opt into QoS using the same
 * policies. Do not use this outside xnu or libxpc/launchd.
 */
static void
selfexec_with_apptype(int argc, char *argv[])
{
	int ret;
	posix_spawnattr_t attr;
	extern char **environ;
	char *new_argv[argc + 1 + 1 /* NULL */];
	int i;
	char prog[PATH_MAX];
	uint32_t prog_size = PATH_MAX;

	ret = _NSGetExecutablePath(prog, &prog_size);
	if (ret) {
		err(EX_OSERR, "_NSGetExecutablePath");
	}

	for (i = 0; i < argc; i++) {
		new_argv[i] = argv[i];
	}

	new_argv[i]   = "--switched_apptype";
	new_argv[i + 1] = NULL;

	ret = posix_spawnattr_init(&attr);
	if (ret) {
		errc(EX_OSERR, ret, "posix_spawnattr_init");
	}

	ret = posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETEXEC);
	if (ret) {
		errc(EX_OSERR, ret, "posix_spawnattr_setflags");
	}

	ret = posix_spawnattr_setprocesstype_np(&attr, POSIX_SPAWN_PROC_TYPE_APP_DEFAULT);
	if (ret) {
		errc(EX_OSERR, ret, "posix_spawnattr_setprocesstype_np");
	}

	ret = posix_spawn(NULL, prog, NULL, &attr, new_argv, environ);
	if (ret) {
		errc(EX_OSERR, ret, "posix_spawn");
	}
}

/*
 * Admittedly not very attractive.
 */
static void __attribute__((noreturn))
usage()
{
	errx(EX_USAGE, "Usage: %s <threads> <chain | hop | broadcast-single-sem | broadcast-per-thread> "
	    "<realtime | timeshare | fixed> <iterations>\n\t\t"
	    "[--trace <traceworthy latency in ns>] "
	    "[--verbose] [--spin-one] [--spin-all] [--spin-time <nanos>] [--affinity]\n\t\t"
	    "[--no-sleep] [--drop-priority] [--churn-pri <pri>] [--churn-count <n>]",
	    getprogname());
}

static struct option* g_longopts;
static int option_index;

static uint32_t
read_dec_arg()
{
	char *cp;
	/* char* optarg is a magic global */

	uint32_t arg_val = (uint32_t)strtoull(optarg, &cp, 10);

	if (cp == optarg || *cp) {
		errx(EX_USAGE, "arg --%s requires a decimal number, found \"%s\"",
		    g_longopts[option_index].name, optarg);
	}

	return arg_val;
}

static void
parse_args(int argc, char *argv[])
{
	enum {
		OPT_GETOPT = 0,
		OPT_SPIN_TIME,
		OPT_TRACE,
		OPT_PRIORITY,
		OPT_CHURN_PRI,
		OPT_CHURN_COUNT,
		OPT_RT_CHURN_COUNT,
	};

	static struct option longopts[] = {
		/* BEGIN IGNORE CODESTYLE */
		{ "spin-time",          required_argument,      NULL,                           OPT_SPIN_TIME },
		{ "trace",              required_argument,      NULL,                           OPT_TRACE     },
		{ "priority",           required_argument,      NULL,                           OPT_PRIORITY  },
		{ "churn-pri",          required_argument,      NULL,                           OPT_CHURN_PRI },
		{ "churn-count",        required_argument,      NULL,                           OPT_CHURN_COUNT },
		{ "rt-churn-count",     required_argument,      NULL,                           OPT_RT_CHURN_COUNT },
		{ "switched_apptype",   no_argument,            (int*)&g_seen_apptype,          TRUE },
		{ "spin-one",           no_argument,            (int*)&g_do_one_long_spin,      TRUE },
		{ "spin-all",           no_argument,            (int*)&g_do_all_spin,           TRUE },
		{ "affinity",           no_argument,            (int*)&g_do_affinity,           TRUE },
		{ "no-sleep",           no_argument,            (int*)&g_do_sleep,              FALSE },
		{ "drop-priority",      no_argument,            (int*)&g_drop_priority,         TRUE },
		{ "test-rt",            no_argument,            (int*)&g_test_rt,               TRUE },
		{ "test-rt-smt",        no_argument,            (int*)&g_test_rt_smt,           TRUE },
		{ "test-rt-avoid0",     no_argument,            (int*)&g_test_rt_avoid0,        TRUE },
		{ "rt-churn",           no_argument,            (int*)&g_rt_churn,              TRUE },
		{ "histogram",          no_argument,            (int*)&g_histogram,             TRUE },
		{ "verbose",            no_argument,            (int*)&g_verbose,               TRUE },
		{ "help",               no_argument,            NULL,                           'h' },
		{ NULL,                 0,                      NULL,                           0 }
		/* END IGNORE CODESTYLE */
	};

	g_longopts = longopts;
	int ch = 0;

	while ((ch = getopt_long(argc, argv, "h", longopts, &option_index)) != -1) {
		switch (ch) {
		case OPT_GETOPT:
			/* getopt_long set a variable */
			break;
		case OPT_SPIN_TIME:
			g_do_each_spin = TRUE;
			g_each_spin_duration_ns = read_dec_arg();
			break;
		case OPT_TRACE:
			g_traceworthy_latency_ns = read_dec_arg();
			break;
		case OPT_PRIORITY:
			g_priority = read_dec_arg();
			break;
		case OPT_CHURN_PRI:
			g_churn_pri = read_dec_arg();
			break;
		case OPT_CHURN_COUNT:
			g_churn_count = read_dec_arg();
			break;
		case OPT_RT_CHURN_COUNT:
			g_rt_churn_count = read_dec_arg();
			break;
		case '?':
		case 'h':
		default:
			usage();
			/* NORETURN */
		}
	}

	/*
	 * getopt_long reorders all the options to the beginning of the argv array.
	 * Jump past them to the non-option arguments.
	 */

	argc -= optind;
	argv += optind;

	if (argc > 4) {
		warnx("Too many non-option arguments passed");
		usage();
	}

	if (argc != 4) {
		warnx("Missing required <threads> <waketype> <policy> <iterations> arguments");
		usage();
	}

	char *cp;

	/* How many threads? */
	g_numthreads = (uint32_t)strtoull(argv[0], &cp, 10);

	if (cp == argv[0] || *cp) {
		errx(EX_USAGE, "numthreads requires a decimal number, found \"%s\"", argv[0]);
	}

	/* What wakeup pattern? */
	g_waketype = parse_wakeup_pattern(argv[1]);

	/* Policy */
	g_policy = parse_thread_policy(argv[2]);

	/* Iterations */
	g_iterations = (uint32_t)strtoull(argv[3], &cp, 10);

	if (cp == argv[3] || *cp) {
		errx(EX_USAGE, "numthreads requires a decimal number, found \"%s\"", argv[3]);
	}

	if (g_iterations < 1) {
		errx(EX_USAGE, "Must have at least one iteration");
	}

	if (g_numthreads == 1 && g_waketype == WAKE_CHAIN) {
		errx(EX_USAGE, "chain mode requires more than one thread");
	}

	if (g_numthreads == 1 && g_waketype == WAKE_HOP) {
		errx(EX_USAGE, "hop mode requires more than one thread");
	}
}
