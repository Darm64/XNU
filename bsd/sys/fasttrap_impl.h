/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */

/*
 * Copyright 2008 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */

#ifndef	_FASTTRAP_IMPL_H
#define	_FASTTRAP_IMPL_H

#include <sys/types.h>
#include <sys/dtrace.h>
#include <sys/proc.h>
#include <sys/user.h>
#include <sys/fasttrap.h>
#include <sys/fasttrap_isa.h>

/* Solaris proc_t is the struct. Darwin's proc_t is a pointer to it. */
#define proc_t struct proc /* Steer clear of the Darwin typedef for proc_t */

#ifdef	__cplusplus
extern "C" {
#endif

/*
 * Fasttrap Providers, Probes and Tracepoints
 *
 * Each Solaris process can have multiple providers -- the pid provider as
 * well as any number of user-level statically defined tracing (USDT)
 * providers. Those providers are each represented by a fasttrap_provider_t.
 * All providers for a given process have a pointer to a shared
 * fasttrap_proc_t. The fasttrap_proc_t has two states: active or defunct.
 * When the count of active providers goes to zero it becomes defunct; a
 * provider drops its active count when it is removed individually or as part
 * of a mass removal when a process exits or performs an exec.
 *
 * Each probe is represented by a fasttrap_probe_t which has a pointer to
 * its associated provider as well as a list of fasttrap_id_tp_t structures
 * which are tuples combining a fasttrap_id_t and a fasttrap_tracepoint_t.
 * A fasttrap_tracepoint_t represents the actual point of instrumentation
 * and it contains two lists of fasttrap_id_t structures (to be fired pre-
 * and post-instruction emulation) that identify the probes attached to the
 * tracepoint. Tracepoints also have a pointer to the fasttrap_proc_t for the
 * process they trace which is used when looking up a tracepoint both when a
 * probe fires and when enabling and disabling probes.
 *
 * It's important to note that probes are preallocated with the necessary
 * number of tracepoints, but that tracepoints can be shared by probes and
 * swapped between probes. If a probe's preallocated tracepoint is enabled
 * (and, therefore, the associated probe is enabled), and that probe is
 * then disabled, ownership of that tracepoint may be exchanged for an
 * unused tracepoint belonging to another probe that was attached to the
 * enabled tracepoint.
 */

/*
 * APPLE NOTE: All kmutex_t's have been converted to lck_mtx_t
 */

typedef struct fasttrap_proc {
	pid_t ftpc_pid;				/* process ID for this proc */
	uint64_t ftpc_acount;			/* count of active providers */
	uint64_t ftpc_rcount;			/* count of extant providers */
	lck_mtx_t ftpc_mtx;			/* lock on all but acount */
	struct fasttrap_proc *ftpc_next;	/* next proc in hash chain */
} fasttrap_proc_t;

typedef struct fasttrap_provider {
	pid_t ftp_pid;				/* process ID for this prov */
	fasttrap_provider_type_t ftp_provider_type;	/* type of this provider (usdt, pid, objc, oneshot) */
	char ftp_name[DTRACE_PROVNAMELEN];	/* prov name (w/o the pid) */
	dtrace_provider_id_t ftp_provid;	/* DTrace provider handle */
	uint_t ftp_marked;			/* mark for possible removal */
	uint_t ftp_retired;			/* mark when retired */
	lck_mtx_t ftp_mtx;			/* provider lock */
	lck_mtx_t ftp_cmtx;			/* lock on creating probes */
	uint64_t ftp_pcount;			/* probes in provider count */
	uint64_t ftp_rcount;			/* enabled probes ref count */
	uint64_t ftp_ccount;			/* consumers creating probes */
	uint64_t ftp_mcount;			/* meta provider count */
	fasttrap_proc_t *ftp_proc;		/* shared proc for all provs */
	struct fasttrap_provider *ftp_next;	/* next prov in hash chain */
} fasttrap_provider_t;

typedef struct fasttrap_id fasttrap_id_t;
typedef struct fasttrap_probe fasttrap_probe_t;
typedef struct fasttrap_tracepoint fasttrap_tracepoint_t;

struct fasttrap_id {
	fasttrap_probe_t *fti_probe;		/* referrring probe */
	fasttrap_id_t *fti_next;		/* enabled probe list on tp */
	fasttrap_probe_type_t fti_ptype;	/* probe type */
};

typedef struct fasttrap_id_tp {
	fasttrap_id_t fit_id;
	fasttrap_tracepoint_t *fit_tp;
} fasttrap_id_tp_t;

struct fasttrap_probe {
	dtrace_id_t ftp_id;			/* DTrace probe identifier */
	pid_t ftp_pid;				/* pid for this probe */
	fasttrap_provider_t *ftp_prov;		/* this probe's provider */
	user_addr_t ftp_faddr;			/* associated function's addr */
	size_t ftp_fsize;			/* associated function's size */
	uint64_t ftp_gen;			/* modification generation */
	uint64_t ftp_ntps;			/* number of tracepoints */
	uint8_t *ftp_argmap;			/* native to translated args */
	uint8_t ftp_nargs;			/* translated argument count */
	uint8_t ftp_enabled;			/* is this probe enabled */
	uint8_t ftp_triggered;
	char *ftp_xtypes;			/* translated types index */
	char *ftp_ntypes;			/* native types index */
	fasttrap_id_tp_t ftp_tps[1];		/* flexible array */
};

#define	FASTTRAP_ID_INDEX(id)	\
((fasttrap_id_tp_t *)(((char *)(id) - offsetof(fasttrap_id_tp_t, fit_id))) - \
&(id)->fti_probe->ftp_tps[0])

struct fasttrap_tracepoint {
	fasttrap_proc_t *ftt_proc;		/* associated process struct */
	user_addr_t ftt_pc;			/* address of tracepoint */
	pid_t ftt_pid;				/* pid of tracepoint */
	fasttrap_machtp_t ftt_mtp;		/* ISA-specific portion */
	fasttrap_id_t *ftt_ids;			/* NULL-terminated list */
	fasttrap_id_t *ftt_retids;		/* NULL-terminated list */
	fasttrap_tracepoint_t *ftt_next;	/* link in global hash */
};

typedef struct fasttrap_bucket {
	lck_mtx_t ftb_mtx;			/* bucket lock */
	void *ftb_data;				/* data payload */

	uint8_t ftb_pad[64 - sizeof (lck_mtx_t) - sizeof (void *)];
} fasttrap_bucket_t;

typedef struct fasttrap_hash {
	ulong_t fth_nent;			/* power-of-2 num. of entries */
	ulong_t fth_mask;			/* fth_nent - 1 */
	fasttrap_bucket_t *fth_table;		/* array of buckets */
} fasttrap_hash_t;

/*
 * If at some future point these assembly functions become observable by
 * DTrace, then these defines should become separate functions so that the
 * fasttrap provider doesn't trigger probes during internal operations.
 */
#define	fasttrap_copyout	copyout
#define	fasttrap_fuword32	fuword32
#define	fasttrap_suword32	suword32

/*
 * APPLE NOTE: xnu supports both 32bit and 64bit user processes.
 * We need to make size explicit.
 */
#define	fasttrap_fuword64	fuword64
#define	fasttrap_suword64	suword64
#define fasttrap_fuword64_noerr	fuword64_noerr
#define fasttrap_fuword32_noerr	fuword32_noerr

extern void fasttrap_sigtrap(proc_t *, uthread_t, user_addr_t);

extern dtrace_id_t 		fasttrap_probe_id;
extern fasttrap_hash_t		fasttrap_tpoints;

#define	FASTTRAP_TPOINTS_INDEX(pid, pc) \
	(((pc) / sizeof (fasttrap_instr_t) + (pid)) & fasttrap_tpoints.fth_mask)

extern void fasttrap_tracepoint_retire(proc_t *p, fasttrap_tracepoint_t *tp);

/*
 * Must be implemented by fasttrap_isa.c
 */
extern int fasttrap_tracepoint_init(proc_t *, fasttrap_tracepoint_t *,
    user_addr_t, fasttrap_probe_type_t);
extern int fasttrap_tracepoint_install(proc_t *, fasttrap_tracepoint_t *);
extern int fasttrap_tracepoint_remove(proc_t *, fasttrap_tracepoint_t *);

#if defined(__x86_64__)
extern int fasttrap_pid_probe(x86_saved_state_t *regs);
extern int fasttrap_return_probe(x86_saved_state_t* regs);
#elif defined(__arm__) || defined(__arm64__)
extern int fasttrap_pid_probe(arm_saved_state_t *rp);
extern int fasttrap_return_probe(arm_saved_state_t *regs);
#else
#error architecture not supported
#endif

extern uint64_t fasttrap_pid_getarg(void *, dtrace_id_t, void *, int, int);
extern uint64_t fasttrap_usdt_getarg(void *, dtrace_id_t, void *, int, int);


#ifdef	__cplusplus
}
#endif

#undef proc_t

#endif	/* _FASTTRAP_IMPL_H */
