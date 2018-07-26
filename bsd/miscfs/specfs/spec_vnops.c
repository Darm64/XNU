/*
 * Copyright (c) 2000-2016 Apple Computer, Inc. All rights reserved.
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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1989, 1993, 1995
 *	The Regents of the University of California.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by the University of
 *	California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 *	@(#)spec_vnops.c	8.14 (Berkeley) 5/21/95
 */

#include <sys/param.h>
#include <sys/proc_internal.h>
#include <sys/kauth.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/buf_internal.h>
#include <sys/mount_internal.h>
#include <sys/vnode_internal.h>
#include <sys/file_internal.h>
#include <sys/namei.h>
#include <sys/stat.h>
#include <sys/errno.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/user.h>
#include <sys/malloc.h>
#include <sys/disk.h>
#include <sys/uio_internal.h>
#include <sys/resource.h>
#include <machine/machine_routines.h>
#include <miscfs/specfs/specdev.h>
#include <vfs/vfs_support.h>
#include <vfs/vfs_disk_conditioner.h>

#include <kern/assert.h>
#include <kern/task.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <kern/policy_internal.h>
#include <kern/timer_call.h>
#include <kern/waitq.h>

#include <pexpert/pexpert.h>

#include <sys/kdebug.h>
#include <libkern/section_keywords.h>

/* XXX following three prototypes should be in a header file somewhere */
extern dev_t	chrtoblk(dev_t dev);
extern boolean_t	iskmemdev(dev_t dev);
extern int	bpfkqfilter(dev_t dev, struct knote *kn);
extern int ptsd_kqfilter(dev_t, struct knote *);
extern int ptmx_kqfilter(dev_t, struct knote *);

struct vnode *speclisth[SPECHSZ];

/* symbolic sleep message strings for devices */
char	devopn[] = "devopn";
char	devio[] = "devio";
char	devwait[] = "devwait";
char	devin[] = "devin";
char	devout[] = "devout";
char	devioc[] = "devioc";
char	devcls[] = "devcls";

#define VOPFUNC int (*)(void *)

int (**spec_vnodeop_p)(void *);
struct vnodeopv_entry_desc spec_vnodeop_entries[] = {
	{ &vnop_default_desc, (VOPFUNC)vn_default_error },
	{ &vnop_lookup_desc, (VOPFUNC)spec_lookup },		/* lookup */
	{ &vnop_create_desc, (VOPFUNC)err_create },		/* create */
	{ &vnop_mknod_desc, (VOPFUNC)err_mknod },		/* mknod */
	{ &vnop_open_desc, (VOPFUNC)spec_open },			/* open */
	{ &vnop_close_desc, (VOPFUNC)spec_close },		/* close */
	{ &vnop_access_desc, (VOPFUNC)spec_access },		/* access */
	{ &vnop_getattr_desc, (VOPFUNC)spec_getattr },		/* getattr */
	{ &vnop_setattr_desc, (VOPFUNC)spec_setattr },		/* setattr */
	{ &vnop_read_desc, (VOPFUNC)spec_read },			/* read */
	{ &vnop_write_desc, (VOPFUNC)spec_write },		/* write */
	{ &vnop_ioctl_desc, (VOPFUNC)spec_ioctl },		/* ioctl */
	{ &vnop_select_desc, (VOPFUNC)spec_select },		/* select */
	{ &vnop_revoke_desc, (VOPFUNC)nop_revoke },		/* revoke */
	{ &vnop_mmap_desc, (VOPFUNC)err_mmap },			/* mmap */
	{ &vnop_fsync_desc, (VOPFUNC)spec_fsync },		/* fsync */
	{ &vnop_remove_desc, (VOPFUNC)err_remove },		/* remove */
	{ &vnop_link_desc, (VOPFUNC)err_link },			/* link */
	{ &vnop_rename_desc, (VOPFUNC)err_rename },		/* rename */
	{ &vnop_mkdir_desc, (VOPFUNC)err_mkdir },		/* mkdir */
	{ &vnop_rmdir_desc, (VOPFUNC)err_rmdir },		/* rmdir */
	{ &vnop_symlink_desc, (VOPFUNC)err_symlink },		/* symlink */
	{ &vnop_readdir_desc, (VOPFUNC)err_readdir },		/* readdir */
	{ &vnop_readlink_desc, (VOPFUNC)err_readlink },		/* readlink */
	{ &vnop_inactive_desc, (VOPFUNC)nop_inactive },		/* inactive */
	{ &vnop_reclaim_desc, (VOPFUNC)nop_reclaim },		/* reclaim */
	{ &vnop_strategy_desc, (VOPFUNC)spec_strategy },		/* strategy */
	{ &vnop_pathconf_desc, (VOPFUNC)spec_pathconf },		/* pathconf */
	{ &vnop_advlock_desc, (VOPFUNC)err_advlock },		/* advlock */
	{ &vnop_bwrite_desc, (VOPFUNC)spec_bwrite },		/* bwrite */
	{ &vnop_pagein_desc, (VOPFUNC)err_pagein },		/* Pagein */
	{ &vnop_pageout_desc, (VOPFUNC)err_pageout },		/* Pageout */
        { &vnop_copyfile_desc, (VOPFUNC)err_copyfile },		/* Copyfile */
	{ &vnop_blktooff_desc, (VOPFUNC)spec_blktooff },		/* blktooff */
	{ &vnop_offtoblk_desc, (VOPFUNC)spec_offtoblk },		/* offtoblk */
	{ &vnop_blockmap_desc, (VOPFUNC)spec_blockmap },		/* blockmap */
	{ (struct vnodeop_desc*)NULL, (int(*)(void *))NULL }
};
struct vnodeopv_desc spec_vnodeop_opv_desc =
	{ &spec_vnodeop_p, spec_vnodeop_entries };


static void set_blocksize(vnode_t, dev_t);

#define LOWPRI_TIER1_WINDOW_MSECS	  25
#define LOWPRI_TIER2_WINDOW_MSECS	  100
#define LOWPRI_TIER3_WINDOW_MSECS	  500

#define LOWPRI_TIER1_IO_PERIOD_MSECS	  40
#define LOWPRI_TIER2_IO_PERIOD_MSECS	  85
#define LOWPRI_TIER3_IO_PERIOD_MSECS	  200

#define LOWPRI_TIER1_IO_PERIOD_SSD_MSECS  5
#define LOWPRI_TIER2_IO_PERIOD_SSD_MSECS  15
#define LOWPRI_TIER3_IO_PERIOD_SSD_MSECS  25


int	throttle_windows_msecs[THROTTLE_LEVEL_END + 1] = {
	0,
	LOWPRI_TIER1_WINDOW_MSECS,
	LOWPRI_TIER2_WINDOW_MSECS,
	LOWPRI_TIER3_WINDOW_MSECS,
};

int	throttle_io_period_msecs[THROTTLE_LEVEL_END + 1] = {
	0,
	LOWPRI_TIER1_IO_PERIOD_MSECS,
	LOWPRI_TIER2_IO_PERIOD_MSECS,
	LOWPRI_TIER3_IO_PERIOD_MSECS,
};

int	throttle_io_period_ssd_msecs[THROTTLE_LEVEL_END + 1] = {
	0,
	LOWPRI_TIER1_IO_PERIOD_SSD_MSECS,
	LOWPRI_TIER2_IO_PERIOD_SSD_MSECS,
	LOWPRI_TIER3_IO_PERIOD_SSD_MSECS,
};


int	throttled_count[THROTTLE_LEVEL_END + 1];

struct _throttle_io_info_t {
        lck_mtx_t       throttle_lock;

	struct timeval	throttle_last_write_timestamp;
	struct timeval	throttle_min_timer_deadline;
	struct timeval	throttle_window_start_timestamp[THROTTLE_LEVEL_END + 1]; /* window starts at both the beginning and completion of an I/O */
	struct timeval	throttle_last_IO_timestamp[THROTTLE_LEVEL_END + 1];
	pid_t 		throttle_last_IO_pid[THROTTLE_LEVEL_END + 1];
	struct timeval	throttle_start_IO_period_timestamp[THROTTLE_LEVEL_END + 1];
	int32_t throttle_inflight_count[THROTTLE_LEVEL_END + 1];

	TAILQ_HEAD( , uthread) throttle_uthlist[THROTTLE_LEVEL_END + 1]; 	/* Lists of throttled uthreads */
	int		throttle_next_wake_level;

        thread_call_t   throttle_timer_call;
        int32_t throttle_timer_ref;
        int32_t throttle_timer_active;

        int32_t throttle_io_count;
        int32_t throttle_io_count_begin;
        int    *throttle_io_periods;
	uint32_t throttle_io_period_num;

	int32_t throttle_refcnt;
	int32_t throttle_alloc;
	int32_t throttle_disabled;
	int32_t throttle_is_fusion_with_priority;
};

struct _throttle_io_info_t _throttle_io_info[LOWPRI_MAX_NUM_DEV];


int	lowpri_throttle_enabled = 1;


static void throttle_info_end_io_internal(struct _throttle_io_info_t *info, int throttle_level);
static int throttle_info_update_internal(struct _throttle_io_info_t *info, uthread_t ut, int flags, boolean_t isssd, boolean_t inflight, struct bufattr *bap);
static int throttle_get_thread_throttle_level(uthread_t ut);
static int throttle_get_thread_throttle_level_internal(uthread_t ut, int io_tier);
void throttle_info_mount_reset_period(mount_t mp, int isssd);

/*
 * Trivial lookup routine that always fails.
 */
int
spec_lookup(struct vnop_lookup_args *ap)
{

	*ap->a_vpp = NULL;
	return (ENOTDIR);
}

static void
set_blocksize(struct vnode *vp, dev_t dev)
{
    int (*size)(dev_t);
    int rsize;

    if ((major(dev) < nblkdev) && (size = bdevsw[major(dev)].d_psize)) {
        rsize = (*size)(dev);
	if (rsize <= 0)        /* did size fail? */
	    vp->v_specsize = DEV_BSIZE;
	else
	    vp->v_specsize = rsize;
    }
    else
	    vp->v_specsize = DEV_BSIZE;
}

void
set_fsblocksize(struct vnode *vp)
{
	
	if (vp->v_type == VBLK) {
		dev_t dev = (dev_t)vp->v_rdev;
		int maj = major(dev);

		if ((u_int)maj >= (u_int)nblkdev)
			return;

		vnode_lock(vp);
		set_blocksize(vp, dev);
		vnode_unlock(vp);
	}

}


/*
 * Open a special file.
 */
int
spec_open(struct vnop_open_args *ap)
{
	struct proc *p = vfs_context_proc(ap->a_context);
	kauth_cred_t cred = vfs_context_ucred(ap->a_context);
	struct vnode *vp = ap->a_vp;
	dev_t bdev, dev = (dev_t)vp->v_rdev;
	int maj = major(dev);
	int error;

	/*
	 * Don't allow open if fs is mounted -nodev.
	 */
	if (vp->v_mount && (vp->v_mount->mnt_flag & MNT_NODEV))
		return (ENXIO);

	switch (vp->v_type) {

	case VCHR:
		if ((u_int)maj >= (u_int)nchrdev)
			return (ENXIO);
		if (cred != FSCRED && (ap->a_mode & FWRITE)) {
			/*
			 * When running in very secure mode, do not allow
			 * opens for writing of any disk character devices.
			 */
			if (securelevel >= 2 && isdisk(dev, VCHR))
				return (EPERM);

			/* Never allow writing to /dev/mem or /dev/kmem */
			if (iskmemdev(dev))
				return (EPERM);
			/*
			 * When running in secure mode, do not allow opens for
			 * writing of character devices whose corresponding block
			 * devices are currently mounted.
			 */
			if (securelevel >= 1) {
				if ((bdev = chrtoblk(dev)) != NODEV && check_mountedon(bdev, VBLK, &error))
					return (error);
			}
		}

		devsw_lock(dev, S_IFCHR);
		error = (*cdevsw[maj].d_open)(dev, ap->a_mode, S_IFCHR, p);

		if (error == 0) {
			vp->v_specinfo->si_opencount++;
		}

		devsw_unlock(dev, S_IFCHR);

		if (error == 0 && cdevsw[maj].d_type == D_DISK && !vp->v_un.vu_specinfo->si_initted) {
			int	isssd = 0;
			uint64_t throttle_mask = 0;
			uint32_t devbsdunit = 0;

			if (VNOP_IOCTL(vp, DKIOCGETTHROTTLEMASK, (caddr_t)&throttle_mask, 0, NULL) == 0) {
				
				if (throttle_mask != 0 &&
				    VNOP_IOCTL(vp, DKIOCISSOLIDSTATE, (caddr_t)&isssd, 0, ap->a_context) == 0) {
					/*
					 * as a reasonable approximation, only use the lowest bit of the mask
					 * to generate a disk unit number
					 */
					devbsdunit = num_trailing_0(throttle_mask);

					vnode_lock(vp);
					
					vp->v_un.vu_specinfo->si_isssd = isssd;
					vp->v_un.vu_specinfo->si_devbsdunit = devbsdunit;
					vp->v_un.vu_specinfo->si_throttle_mask = throttle_mask;
					vp->v_un.vu_specinfo->si_throttleable = 1;
					vp->v_un.vu_specinfo->si_initted = 1;

					vnode_unlock(vp);
				}
			}
			if (vp->v_un.vu_specinfo->si_initted == 0) {
				vnode_lock(vp);
				vp->v_un.vu_specinfo->si_initted = 1;
				vnode_unlock(vp);
			}
		}
		return (error);

	case VBLK:
		if ((u_int)maj >= (u_int)nblkdev)
			return (ENXIO);
		/*
		 * When running in very secure mode, do not allow
		 * opens for writing of any disk block devices.
		 */
		if (securelevel >= 2 && cred != FSCRED &&
		    (ap->a_mode & FWRITE) && bdevsw[maj].d_type == D_DISK)
			return (EPERM);
		/*
		 * Do not allow opens of block devices that are
		 * currently mounted.
		 */
		if ( (error = vfs_mountedon(vp)) )
			return (error);

		devsw_lock(dev, S_IFBLK);
		error = (*bdevsw[maj].d_open)(dev, ap->a_mode, S_IFBLK, p);
		if (!error) {
			vp->v_specinfo->si_opencount++;
		}
		devsw_unlock(dev, S_IFBLK);

		if (!error) {
		    u_int64_t blkcnt;
		    u_int32_t blksize;
			int setsize = 0;
			u_int32_t size512 = 512;


		    if (!VNOP_IOCTL(vp, DKIOCGETBLOCKSIZE, (caddr_t)&blksize, 0, ap->a_context)) {
				/* Switch to 512 byte sectors (temporarily) */

				if (!VNOP_IOCTL(vp, DKIOCSETBLOCKSIZE, (caddr_t)&size512, FWRITE, ap->a_context)) {
			    	/* Get the number of 512 byte physical blocks. */
			    	if (!VNOP_IOCTL(vp, DKIOCGETBLOCKCOUNT, (caddr_t)&blkcnt, 0, ap->a_context)) {
						setsize = 1;
			    	}
				}
				/* If it doesn't set back, we can't recover */
				if (VNOP_IOCTL(vp, DKIOCSETBLOCKSIZE, (caddr_t)&blksize, FWRITE, ap->a_context))
			    	error = ENXIO;
		    }


			vnode_lock(vp);
		    set_blocksize(vp, dev);

		    /*
		     * Cache the size in bytes of the block device for later
		     * use by spec_write().
		     */
			if (setsize)
				vp->v_specdevsize = blkcnt * (u_int64_t)size512;
			else
		    	vp->v_specdevsize = (u_int64_t)0;	/* Default: Can't get */
			
			vnode_unlock(vp);

		}
		return(error);
	default:
	        panic("spec_open type");
	}
	return (0);
}

/*
 * Vnode op for read
 */
int
spec_read(struct vnop_read_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct buf *bp;
	daddr64_t bn, nextbn;
	long bsize, bscale;
	int devBlockSize=0;
	int n, on;
	int error = 0;
	dev_t dev;

#if DIAGNOSTIC
	if (uio->uio_rw != UIO_READ)
		panic("spec_read mode");
	if (UIO_SEG_IS_USER_SPACE(uio->uio_segflg))
		panic("spec_read proc");
#endif
	if (uio_resid(uio) == 0)
		return (0);

	switch (vp->v_type) {

	case VCHR:
		{
			struct _throttle_io_info_t *throttle_info = NULL;
			int thread_throttle_level;
                if (cdevsw[major(vp->v_rdev)].d_type == D_DISK && vp->v_un.vu_specinfo->si_throttleable) {
			throttle_info = &_throttle_io_info[vp->v_un.vu_specinfo->si_devbsdunit];
				thread_throttle_level = throttle_info_update_internal(throttle_info, NULL, 0, vp->v_un.vu_specinfo->si_isssd, TRUE, NULL);
                }
		error = (*cdevsw[major(vp->v_rdev)].d_read)
			(vp->v_rdev, uio, ap->a_ioflag);

			if (throttle_info) {
				throttle_info_end_io_internal(throttle_info, thread_throttle_level);
			}

		return (error);
		}

	case VBLK:
		if (uio->uio_offset < 0)
			return (EINVAL);

		dev = vp->v_rdev;

		devBlockSize = vp->v_specsize;

		if (devBlockSize > PAGE_SIZE) 
			return (EINVAL);

	        bscale = PAGE_SIZE / devBlockSize;
		bsize = bscale * devBlockSize;

		do {
			on = uio->uio_offset % bsize;

			bn = (daddr64_t)((uio->uio_offset / devBlockSize) &~ (bscale - 1));
			
			if (vp->v_speclastr + bscale == bn) {
			        nextbn = bn + bscale;
				error = buf_breadn(vp, bn, (int)bsize, &nextbn,
					       (int *)&bsize, 1, NOCRED, &bp);
			} else
			        error = buf_bread(vp, bn, (int)bsize, NOCRED, &bp);

			vnode_lock(vp);
			vp->v_speclastr = bn;
			vnode_unlock(vp);

			n = bsize - buf_resid(bp);
			if ((on > n) || error) {
			        if (!error)
				        error = EINVAL;
				buf_brelse(bp);
				return (error);
			}
			n = min((unsigned)(n  - on), uio_resid(uio));

			error = uiomove((char *)buf_dataptr(bp) + on, n, uio);
			if (n + on == bsize)
				buf_markaged(bp);
			buf_brelse(bp);
		} while (error == 0 && uio_resid(uio) > 0 && n != 0);
		return (error);

	default:
		panic("spec_read type");
	}
	/* NOTREACHED */

	return (0);
}

/*
 * Vnode op for write
 */
int
spec_write(struct vnop_write_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct uio *uio = ap->a_uio;
	struct buf *bp;
	daddr64_t bn;
	int bsize, blkmask, bscale;
	int io_sync;
	int devBlockSize=0;
	int n, on;
	int error = 0;
	dev_t dev;

#if DIAGNOSTIC
	if (uio->uio_rw != UIO_WRITE)
		panic("spec_write mode");
	if (UIO_SEG_IS_USER_SPACE(uio->uio_segflg))
		panic("spec_write proc");
#endif

	switch (vp->v_type) {

	case VCHR:
		{
			struct _throttle_io_info_t *throttle_info = NULL;
			int thread_throttle_level;
                if (cdevsw[major(vp->v_rdev)].d_type == D_DISK && vp->v_un.vu_specinfo->si_throttleable) {
			throttle_info = &_throttle_io_info[vp->v_un.vu_specinfo->si_devbsdunit];

				thread_throttle_level = throttle_info_update_internal(throttle_info, NULL, 0, vp->v_un.vu_specinfo->si_isssd, TRUE, NULL);

			microuptime(&throttle_info->throttle_last_write_timestamp);
                }
		error = (*cdevsw[major(vp->v_rdev)].d_write)
			(vp->v_rdev, uio, ap->a_ioflag);

			if (throttle_info) {
				throttle_info_end_io_internal(throttle_info, thread_throttle_level);
			}

		return (error);
		}

	case VBLK:
		if (uio_resid(uio) == 0)
			return (0);
		if (uio->uio_offset < 0)
			return (EINVAL);

		io_sync = (ap->a_ioflag & IO_SYNC);

		dev = (vp->v_rdev);

		devBlockSize = vp->v_specsize;
		if (devBlockSize > PAGE_SIZE)
			return(EINVAL);

	        bscale = PAGE_SIZE / devBlockSize;
		blkmask = bscale - 1;
		bsize = bscale * devBlockSize;
		

		do {
			bn = (daddr64_t)((uio->uio_offset / devBlockSize) &~ blkmask);
			on = uio->uio_offset % bsize;

			n = min((unsigned)(bsize - on), uio_resid(uio));

			/*
			 * Use buf_getblk() as an optimization IFF:
			 *
			 * 1)	We are reading exactly a block on a block
			 *	aligned boundary
			 * 2)	We know the size of the device from spec_open
			 * 3)	The read doesn't span the end of the device
			 *
			 * Otherwise, we fall back on buf_bread().
			 */
			if (n == bsize &&
			    vp->v_specdevsize != (u_int64_t)0 &&
			    (uio->uio_offset + (u_int64_t)n) > vp->v_specdevsize) {
			    /* reduce the size of the read to what is there */
			    n = (uio->uio_offset + (u_int64_t)n) - vp->v_specdevsize;
			}

			if (n == bsize)
			        bp = buf_getblk(vp, bn, bsize, 0, 0, BLK_WRITE);
			else
			        error = (int)buf_bread(vp, bn, bsize, NOCRED, &bp);

			/* Translate downstream error for upstream, if needed */
			if (!error)
				error = (int)buf_error(bp);
			if (error) {
				buf_brelse(bp);
				return (error);
			}
			n = min(n, bsize - buf_resid(bp));

			error = uiomove((char *)buf_dataptr(bp) + on, n, uio);
			if (error) {
				buf_brelse(bp);
				return (error);
			}
			buf_markaged(bp);

			if (io_sync) 
			        error = buf_bwrite(bp);
			else {
			        if ((n + on) == bsize)
				        error = buf_bawrite(bp);
				else
				        error = buf_bdwrite(bp);
			}
		} while (error == 0 && uio_resid(uio) > 0 && n != 0);
		return (error);

	default:
		panic("spec_write type");
	}
	/* NOTREACHED */

	return (0);
}

/*
 * Device ioctl operation.
 */
int
spec_ioctl(struct vnop_ioctl_args *ap)
{
	proc_t p = vfs_context_proc(ap->a_context);
	dev_t dev = ap->a_vp->v_rdev;
	int	retval = 0;

	KERNEL_DEBUG_CONSTANT(FSDBG_CODE(DBG_IOCTL, 0) | DBG_FUNC_START,
		dev, ap->a_command, ap->a_fflag, ap->a_vp->v_type, 0);

	switch (ap->a_vp->v_type) {

	case VCHR:
		retval = (*cdevsw[major(dev)].d_ioctl)(dev, ap->a_command, ap->a_data,
						       ap->a_fflag, p);
		break;

	case VBLK:
		retval = (*bdevsw[major(dev)].d_ioctl)(dev, ap->a_command, ap->a_data, ap->a_fflag, p);
		if (!retval && ap->a_command == DKIOCSETBLOCKSIZE)
			ap->a_vp->v_specsize = *(uint32_t *)ap->a_data;
		break;

	default:
		panic("spec_ioctl");
		/* NOTREACHED */
	}
	KERNEL_DEBUG_CONSTANT(FSDBG_CODE(DBG_IOCTL, 0) | DBG_FUNC_END,
		dev, ap->a_command, ap->a_fflag, retval, 0);

	return (retval);
}

int
spec_select(struct vnop_select_args *ap)
{
	proc_t p = vfs_context_proc(ap->a_context);
	dev_t dev;

	switch (ap->a_vp->v_type) {

	default:
		return (1);		/* XXX */

	case VCHR:
		dev = ap->a_vp->v_rdev;
		return (*cdevsw[major(dev)].d_select)(dev, ap->a_which, ap->a_wql, p);
	}
}

static int filt_specattach(struct knote *kn, struct kevent_internal_s *kev);

int
spec_kqfilter(vnode_t vp, struct knote *kn, struct kevent_internal_s *kev)
{
	dev_t dev;

	assert(vnode_ischr(vp));

	dev = vnode_specrdev(vp);

#if NETWORKING
	/*
	 * Try a bpf device, as defined in bsd/net/bpf.c
	 * If it doesn't error out the attach, then it
	 * claimed it. Otherwise, fall through and try
	 * other attaches.
	 */
	int32_t tmp_flags = kn->kn_flags;
	int64_t tmp_data = kn->kn_data;
	int res;

	res = bpfkqfilter(dev, kn);
	if ((kn->kn_flags & EV_ERROR) == 0) {
		return res;
	}
	kn->kn_flags = tmp_flags;
	kn->kn_data = tmp_data;
#endif

	if (major(dev) > nchrdev) {
		knote_set_error(kn, ENXIO);
		return 0;
	}

	kn->kn_vnode_kqok = !!(cdevsw_flags[major(dev)] & CDEVSW_SELECT_KQUEUE);
	kn->kn_vnode_use_ofst = !!(cdevsw_flags[major(dev)] & CDEVSW_USE_OFFSET);

	if (cdevsw_flags[major(dev)] & CDEVSW_IS_PTS) {
		kn->kn_filtid = EVFILTID_PTSD;
		return ptsd_kqfilter(dev, kn);
	} else if (cdevsw_flags[major(dev)] & CDEVSW_IS_PTC) {
		kn->kn_filtid = EVFILTID_PTMX;
		return ptmx_kqfilter(dev, kn);
	} else if (cdevsw[major(dev)].d_type == D_TTY && kn->kn_vnode_kqok) {
		/*
		 * TTYs from drivers that use struct ttys use their own filter
		 * routines.  The PTC driver doesn't use the tty for character
		 * counts, so it must go through the select fallback.
		 */
		kn->kn_filtid = EVFILTID_TTY;
		return knote_fops(kn)->f_attach(kn, kev);
	}

	/* Try to attach to other char special devices */
	return filt_specattach(kn, kev);
}

/*
 * Synch buffers associated with a block device
 */
int
spec_fsync_internal(vnode_t vp, int waitfor, __unused vfs_context_t context)
{
	if (vp->v_type == VCHR)
		return (0);
	/*
	 * Flush all dirty buffers associated with a block device.
	 */
	buf_flushdirtyblks(vp, (waitfor == MNT_WAIT || waitfor == MNT_DWAIT), 0, "spec_fsync");

	return (0);
}

int
spec_fsync(struct vnop_fsync_args *ap)
{
	return spec_fsync_internal(ap->a_vp, ap->a_waitfor, ap->a_context);
}


/*
 * Just call the device strategy routine
 */
void throttle_init(void);


#if 0 
#define DEBUG_ALLOC_THROTTLE_INFO(format, debug_info, args...)	\
        do {                                                    \
               if ((debug_info)->alloc)                           \
               printf("%s: "format, __FUNCTION__, ## args);     \
       } while(0)

#else 
#define DEBUG_ALLOC_THROTTLE_INFO(format, debug_info, args...)
#endif


SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier1_window_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_windows_msecs[THROTTLE_LEVEL_TIER1], 0, "");
SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier2_window_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_windows_msecs[THROTTLE_LEVEL_TIER2], 0, "");
SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier3_window_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_windows_msecs[THROTTLE_LEVEL_TIER3], 0, "");

SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier1_io_period_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_io_period_msecs[THROTTLE_LEVEL_TIER1], 0, "");
SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier2_io_period_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_io_period_msecs[THROTTLE_LEVEL_TIER2], 0, "");
SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier3_io_period_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_io_period_msecs[THROTTLE_LEVEL_TIER3], 0, "");

SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier1_io_period_ssd_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_io_period_ssd_msecs[THROTTLE_LEVEL_TIER1], 0, "");
SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier2_io_period_ssd_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_io_period_ssd_msecs[THROTTLE_LEVEL_TIER2], 0, "");
SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_tier3_io_period_ssd_msecs, CTLFLAG_RW | CTLFLAG_LOCKED, &throttle_io_period_ssd_msecs[THROTTLE_LEVEL_TIER3], 0, "");

SYSCTL_INT(_debug, OID_AUTO, lowpri_throttle_enabled, CTLFLAG_RW | CTLFLAG_LOCKED, &lowpri_throttle_enabled, 0, "");


static lck_grp_t        *throttle_lock_grp;
static lck_attr_t       *throttle_lock_attr;
static lck_grp_attr_t   *throttle_lock_grp_attr;


/*
 * throttled I/O helper function
 * convert the index of the lowest set bit to a device index
 */
int
num_trailing_0(uint64_t n)
{
	/*
	 * since in most cases the number of trailing 0s is very small,
	 * we simply counting sequentially from the lowest bit
	 */
	if (n == 0)
		return sizeof(n) * 8;
	int count = 0;
	while (!ISSET(n, 1)) {
		n >>= 1;
		++count;
	}
	return count;
}


/*
 * Release the reference and if the item was allocated and this is the last
 * reference then free it.
 *
 * This routine always returns the old value.
 */
static int
throttle_info_rel(struct _throttle_io_info_t *info)
{
	SInt32 oldValue = OSDecrementAtomic(&info->throttle_refcnt);

	DEBUG_ALLOC_THROTTLE_INFO("refcnt = %d info = %p\n", 
		info, (int)(oldValue -1), info );

	/* The reference count just went negative, very bad */
	if (oldValue == 0)
		panic("throttle info ref cnt went negative!");

	/* 
	 * Once reference count is zero, no one else should be able to take a 
	 * reference 
	 */
	if ((info->throttle_refcnt == 0) && (info->throttle_alloc)) {
		DEBUG_ALLOC_THROTTLE_INFO("Freeing info = %p\n", info);
		
		lck_mtx_destroy(&info->throttle_lock, throttle_lock_grp);
		FREE(info, M_TEMP); 
	}
	return oldValue;
}


/*
 * Just take a reference on the throttle info structure.
 *
 * This routine always returns the old value.
 */
static SInt32
throttle_info_ref(struct _throttle_io_info_t *info)
{
	SInt32 oldValue = OSIncrementAtomic(&info->throttle_refcnt);

	DEBUG_ALLOC_THROTTLE_INFO("refcnt = %d info = %p\n", 
		info, (int)(oldValue -1), info );
	/* Allocated items should never have a reference of zero */
	if (info->throttle_alloc && (oldValue == 0))
		panic("Taking a reference without calling create throttle info!\n");

	return oldValue;
}

/*
 * on entry the throttle_lock is held...
 * this function is responsible for taking
 * and dropping the reference on the info
 * structure which will keep it from going
 * away while the timer is running if it
 * happens to have been dynamically allocated by
 * a network fileystem kext which is now trying
 * to free it
 */
static uint32_t
throttle_timer_start(struct _throttle_io_info_t *info, boolean_t update_io_count, int wakelevel)
{	
	struct timeval  elapsed;
	struct timeval  now;
	struct timeval  period;
	uint64_t	elapsed_msecs;
	int		throttle_level;
	int		level;
	int		msecs;
	boolean_t	throttled = FALSE;
	boolean_t	need_timer = FALSE;

	microuptime(&now);

	if (update_io_count == TRUE) {
		info->throttle_io_count_begin = info->throttle_io_count;
		info->throttle_io_period_num++;

		while (wakelevel >= THROTTLE_LEVEL_THROTTLED)
			info->throttle_start_IO_period_timestamp[wakelevel--] = now;

		info->throttle_min_timer_deadline = now;

		msecs = info->throttle_io_periods[THROTTLE_LEVEL_THROTTLED];
		period.tv_sec = msecs / 1000;
		period.tv_usec = (msecs % 1000) * 1000;

		timevaladd(&info->throttle_min_timer_deadline, &period);
	}
	for (throttle_level = THROTTLE_LEVEL_START; throttle_level < THROTTLE_LEVEL_END; throttle_level++) {

		elapsed = now;
		timevalsub(&elapsed, &info->throttle_window_start_timestamp[throttle_level]);
		elapsed_msecs = (uint64_t)elapsed.tv_sec * (uint64_t)1000 + (elapsed.tv_usec / 1000);

		for (level = throttle_level + 1; level <= THROTTLE_LEVEL_END; level++) {

			if (!TAILQ_EMPTY(&info->throttle_uthlist[level])) {

				if (elapsed_msecs < (uint64_t)throttle_windows_msecs[level] || info->throttle_inflight_count[throttle_level]) {
					/*
					 * we had an I/O occur at a higher priority tier within
					 * this tier's throttle window
					 */
					throttled = TRUE;
				}
				/*
				 * we assume that the windows are the same or longer
				 * as we drop through the throttling tiers...  thus
				 * we can stop looking once we run into a tier with
				 * threads to schedule regardless of whether it's
				 * still in its throttling window or not
				 */
				break;
			}
		}
		if (throttled == TRUE)
			break;
	}
	if (throttled == TRUE) {
		uint64_t	deadline = 0;
		struct timeval  target;
		struct timeval  min_target;

	        /*
		 * we've got at least one tier still in a throttled window
		 * so we need a timer running... compute the next deadline
		 * and schedule it
		 */
		for (level = throttle_level+1; level <= THROTTLE_LEVEL_END; level++) {

			if (TAILQ_EMPTY(&info->throttle_uthlist[level]))
				continue;

			target = info->throttle_start_IO_period_timestamp[level];

			msecs = info->throttle_io_periods[level];
			period.tv_sec = msecs / 1000;
			period.tv_usec = (msecs % 1000) * 1000;

			timevaladd(&target, &period);
			
			if (need_timer == FALSE || timevalcmp(&target, &min_target, <)) {
				min_target = target;
				need_timer = TRUE;
			}
		}
		if (timevalcmp(&info->throttle_min_timer_deadline, &now, >)) {
		        if (timevalcmp(&info->throttle_min_timer_deadline, &min_target, >))
			        min_target = info->throttle_min_timer_deadline;
		}

		if (info->throttle_timer_active) {
			if (thread_call_cancel(info->throttle_timer_call) == FALSE) {
				/*
				 * couldn't kill the timer because it's already
				 * been dispatched, so don't try to start a new
				 * one... once we drop the lock, the timer will
				 * proceed and eventually re-run this function
				 */
				need_timer = FALSE;
			} else
				info->throttle_timer_active = 0;
		}
		if (need_timer == TRUE) {
			/*
			 * This is defined as an int (32-bit) rather than a 64-bit
			 * value because it would need a really big period in the
			 * order of ~500 days to overflow this. So, we let this be
			 * 32-bit which allows us to use the clock_interval_to_deadline()
			 * routine.
			 */
			int	target_msecs;

			if (info->throttle_timer_ref == 0) {
				/*
				 * take a reference for the timer
				 */
				throttle_info_ref(info);

				info->throttle_timer_ref = 1;
			}
			elapsed = min_target;
			timevalsub(&elapsed, &now);
			target_msecs = elapsed.tv_sec * 1000 + elapsed.tv_usec / 1000;

			if (target_msecs <= 0) {
				/*
				 * we may have computed a deadline slightly in the past
				 * due to various factors... if so, just set the timer
				 * to go off in the near future (we don't need to be precise)
				 */
				target_msecs = 1;
			}
			clock_interval_to_deadline(target_msecs, 1000000, &deadline);

			thread_call_enter_delayed(info->throttle_timer_call, deadline);
			info->throttle_timer_active = 1;
		}
	}
	return (throttle_level);
}


static void
throttle_timer(struct _throttle_io_info_t *info)
{
	uthread_t       ut, utlist;
	struct timeval	elapsed;
	struct timeval	now;
	uint64_t	elapsed_msecs;
	int		throttle_level;
	int		level;
	int		wake_level;
	caddr_t		wake_address = NULL;
        boolean_t	update_io_count = FALSE;
	boolean_t	need_wakeup = FALSE;
	boolean_t	need_release = FALSE;

	ut = NULL;
        lck_mtx_lock(&info->throttle_lock);

	info->throttle_timer_active = 0;
	microuptime(&now);

	elapsed = now;
	timevalsub(&elapsed, &info->throttle_start_IO_period_timestamp[THROTTLE_LEVEL_THROTTLED]);
	elapsed_msecs = (uint64_t)elapsed.tv_sec * (uint64_t)1000 + (elapsed.tv_usec / 1000);

	if (elapsed_msecs >= (uint64_t)info->throttle_io_periods[THROTTLE_LEVEL_THROTTLED]) {

		wake_level = info->throttle_next_wake_level;

		for (level = THROTTLE_LEVEL_START; level < THROTTLE_LEVEL_END; level++) {

			elapsed = now;
			timevalsub(&elapsed, &info->throttle_start_IO_period_timestamp[wake_level]);
			elapsed_msecs = (uint64_t)elapsed.tv_sec * (uint64_t)1000 + (elapsed.tv_usec / 1000);

			if (elapsed_msecs >= (uint64_t)info->throttle_io_periods[wake_level] && !TAILQ_EMPTY(&info->throttle_uthlist[wake_level])) {
				/*
				 * we're closing out the current IO period...
				 * if we have a waiting thread, wake it up
				 * after we have reset the I/O window info
				 */
				need_wakeup = TRUE;
				update_io_count = TRUE;

				info->throttle_next_wake_level = wake_level - 1;

				if (info->throttle_next_wake_level == THROTTLE_LEVEL_START)
					info->throttle_next_wake_level = THROTTLE_LEVEL_END;

				break;
			}
			wake_level--;

			if (wake_level == THROTTLE_LEVEL_START)
				wake_level = THROTTLE_LEVEL_END;
		}
	}
	if (need_wakeup == TRUE) {
		if (!TAILQ_EMPTY(&info->throttle_uthlist[wake_level])) {

			ut = (uthread_t)TAILQ_FIRST(&info->throttle_uthlist[wake_level]);
			TAILQ_REMOVE(&info->throttle_uthlist[wake_level], ut, uu_throttlelist);
			ut->uu_on_throttlelist = THROTTLE_LEVEL_NONE;
			ut->uu_is_throttled = FALSE;

			wake_address = (caddr_t)&ut->uu_on_throttlelist;
		}
	} else
		wake_level = THROTTLE_LEVEL_START;

        throttle_level = throttle_timer_start(info, update_io_count, wake_level);

	if (wake_address != NULL)
		wakeup(wake_address);

	for (level = THROTTLE_LEVEL_THROTTLED; level <= throttle_level; level++) {

		TAILQ_FOREACH_SAFE(ut, &info->throttle_uthlist[level], uu_throttlelist, utlist) {

			TAILQ_REMOVE(&info->throttle_uthlist[level], ut, uu_throttlelist);
			ut->uu_on_throttlelist = THROTTLE_LEVEL_NONE;
			ut->uu_is_throttled = FALSE;

			wakeup(&ut->uu_on_throttlelist);
		}
	}
	if (info->throttle_timer_active == 0 && info->throttle_timer_ref) {
		info->throttle_timer_ref = 0;
		need_release = TRUE;
	}
        lck_mtx_unlock(&info->throttle_lock);

	if (need_release == TRUE)
		throttle_info_rel(info);
}


static int
throttle_add_to_list(struct _throttle_io_info_t *info, uthread_t ut, int mylevel, boolean_t insert_tail)
{
	boolean_t start_timer = FALSE;
	int level = THROTTLE_LEVEL_START;

	if (TAILQ_EMPTY(&info->throttle_uthlist[mylevel])) {
		info->throttle_start_IO_period_timestamp[mylevel] = info->throttle_last_IO_timestamp[mylevel];
		start_timer = TRUE;
	}

	if (insert_tail == TRUE)
		TAILQ_INSERT_TAIL(&info->throttle_uthlist[mylevel], ut, uu_throttlelist);
	else
		TAILQ_INSERT_HEAD(&info->throttle_uthlist[mylevel], ut, uu_throttlelist);

	ut->uu_on_throttlelist = mylevel;

	if (start_timer == TRUE) {
		/* we may need to start or rearm the timer */
		level = throttle_timer_start(info, FALSE, THROTTLE_LEVEL_START);

		if (level == THROTTLE_LEVEL_END) {
			if (ut->uu_on_throttlelist >= THROTTLE_LEVEL_THROTTLED) {
				TAILQ_REMOVE(&info->throttle_uthlist[ut->uu_on_throttlelist], ut, uu_throttlelist);

				ut->uu_on_throttlelist = THROTTLE_LEVEL_NONE;
			}
		}
	}
	return (level);
}

static void
throttle_init_throttle_window(void)
{
	int throttle_window_size;

	/*
	 * The hierarchy of throttle window values is as follows:
	 * - Global defaults
	 * - Device tree properties
	 * - Boot-args
	 * All values are specified in msecs.
	 */

	/* Override global values with device-tree properties */
	if (PE_get_default("kern.io_throttle_window_tier1", &throttle_window_size, sizeof(throttle_window_size)))
		throttle_windows_msecs[THROTTLE_LEVEL_TIER1] = throttle_window_size;

	if (PE_get_default("kern.io_throttle_window_tier2", &throttle_window_size, sizeof(throttle_window_size)))
		throttle_windows_msecs[THROTTLE_LEVEL_TIER2] = throttle_window_size;

	if (PE_get_default("kern.io_throttle_window_tier3", &throttle_window_size, sizeof(throttle_window_size)))
		throttle_windows_msecs[THROTTLE_LEVEL_TIER3] = throttle_window_size;
	
	/* Override with boot-args */
	if (PE_parse_boot_argn("io_throttle_window_tier1", &throttle_window_size, sizeof(throttle_window_size)))
		throttle_windows_msecs[THROTTLE_LEVEL_TIER1] = throttle_window_size;

	if (PE_parse_boot_argn("io_throttle_window_tier2", &throttle_window_size, sizeof(throttle_window_size)))
		throttle_windows_msecs[THROTTLE_LEVEL_TIER2] = throttle_window_size;
	
	if (PE_parse_boot_argn("io_throttle_window_tier3", &throttle_window_size, sizeof(throttle_window_size)))
		throttle_windows_msecs[THROTTLE_LEVEL_TIER3] = throttle_window_size;
}

static void
throttle_init_throttle_period(struct _throttle_io_info_t *info, boolean_t isssd)
{
	int throttle_period_size;

	/*
	 * The hierarchy of throttle period values is as follows:
	 * - Global defaults
	 * - Device tree properties
	 * - Boot-args
	 * All values are specified in msecs.
	 */

	/* Assign global defaults */
	if ((isssd == TRUE) && (info->throttle_is_fusion_with_priority == 0))
		info->throttle_io_periods = &throttle_io_period_ssd_msecs[0];
	else
		info->throttle_io_periods = &throttle_io_period_msecs[0];

	/* Override global values with device-tree properties */
	if (PE_get_default("kern.io_throttle_period_tier1", &throttle_period_size, sizeof(throttle_period_size)))
		info->throttle_io_periods[THROTTLE_LEVEL_TIER1] = throttle_period_size;
	
	if (PE_get_default("kern.io_throttle_period_tier2", &throttle_period_size, sizeof(throttle_period_size)))
		info->throttle_io_periods[THROTTLE_LEVEL_TIER2] = throttle_period_size;

	if (PE_get_default("kern.io_throttle_period_tier3", &throttle_period_size, sizeof(throttle_period_size)))
		info->throttle_io_periods[THROTTLE_LEVEL_TIER3] = throttle_period_size;
	
	/* Override with boot-args */
	if (PE_parse_boot_argn("io_throttle_period_tier1", &throttle_period_size, sizeof(throttle_period_size)))
		info->throttle_io_periods[THROTTLE_LEVEL_TIER1] = throttle_period_size;
	
	if (PE_parse_boot_argn("io_throttle_period_tier2", &throttle_period_size, sizeof(throttle_period_size)))
		info->throttle_io_periods[THROTTLE_LEVEL_TIER2] = throttle_period_size;

	if (PE_parse_boot_argn("io_throttle_period_tier3", &throttle_period_size, sizeof(throttle_period_size)))
		info->throttle_io_periods[THROTTLE_LEVEL_TIER3] = throttle_period_size;

}

#if CONFIG_IOSCHED
extern	void vm_io_reprioritize_init(void);
int	iosched_enabled = 1;
#endif

void
throttle_init(void)
{
        struct _throttle_io_info_t *info;
        int	i;
	int	level;
#if CONFIG_IOSCHED
	int 	iosched;
#endif
	/*                                                                                                                                    
         * allocate lock group attribute and group                                                                                            
         */
        throttle_lock_grp_attr = lck_grp_attr_alloc_init();
        throttle_lock_grp = lck_grp_alloc_init("throttle I/O", throttle_lock_grp_attr);

	/* Update throttle parameters based on device tree configuration */
	throttle_init_throttle_window();

        /*                                                                                                                                    
         * allocate the lock attribute                                                                                                        
         */
        throttle_lock_attr = lck_attr_alloc_init();

	for (i = 0; i < LOWPRI_MAX_NUM_DEV; i++) {
	        info = &_throttle_io_info[i];
	  
	        lck_mtx_init(&info->throttle_lock, throttle_lock_grp, throttle_lock_attr);
		info->throttle_timer_call = thread_call_allocate((thread_call_func_t)throttle_timer, (thread_call_param_t)info);

		for (level = 0; level <= THROTTLE_LEVEL_END; level++) {
			TAILQ_INIT(&info->throttle_uthlist[level]);
			info->throttle_last_IO_pid[level] = 0;
			info->throttle_inflight_count[level] = 0;
		}
		info->throttle_next_wake_level = THROTTLE_LEVEL_END;
		info->throttle_disabled = 0;
		info->throttle_is_fusion_with_priority = 0;
	}
#if CONFIG_IOSCHED
	if (PE_parse_boot_argn("iosched", &iosched, sizeof(iosched))) {
		iosched_enabled = iosched;
	}
	if (iosched_enabled) {
		/* Initialize I/O Reprioritization mechanism */
		vm_io_reprioritize_init();
	}
#endif
}

void
sys_override_io_throttle(int flag)
{
	if (flag == THROTTLE_IO_ENABLE)
		lowpri_throttle_enabled = 1;

	if (flag == THROTTLE_IO_DISABLE)
		lowpri_throttle_enabled = 0;
}

int rethrottle_wakeups = 0;

/*
 * the uu_rethrottle_lock is used to synchronize this function
 * with "throttle_lowpri_io" which is where a throttled thread
 * will block... that function will grab this lock before beginning
 * it's decision making process concerning the need to block, and
 * hold it through the assert_wait.  When that thread is awakened
 * for any reason (timer or rethrottle), it will reacquire the
 * uu_rethrottle_lock before determining if it really is ok for
 * it to now run.  This is the point at which the thread could
 * enter a different throttling queue and reblock or return from
 * the throttle w/o having waited out it's entire throttle if
 * the rethrottle has now moved it out of any currently
 * active throttle window.
 *
 *
 * NOTES:
 * 1 - This may be called with the task lock held.
 * 2 - This may be called with preemption and interrupts disabled
 *     in the kqueue wakeup path so we can't take the throttle_lock which is a mutex
 * 3 - This cannot safely dereference uu_throttle_info, as it may
 *     get deallocated out from under us
 */

void
rethrottle_thread(uthread_t ut)
{
	/*
	 * If uthread doesn't have throttle state, then there's no chance
	 * of it needing a rethrottle.
	 */
	if (ut->uu_throttle_info == NULL)
		return;

	boolean_t s = ml_set_interrupts_enabled(FALSE);
	lck_spin_lock(&ut->uu_rethrottle_lock);

	if (ut->uu_is_throttled == FALSE)
		ut->uu_was_rethrottled = TRUE;
	else {
		int my_new_level = throttle_get_thread_throttle_level(ut);

		if (my_new_level != ut->uu_on_throttlelist) {
			/*
			 * ut is currently blocked (as indicated by
			 * ut->uu_is_throttled == TRUE)
			 * and we're changing it's throttle level, so
			 * we need to wake it up.
			 */
			ut->uu_is_throttled = FALSE;
			wakeup(&ut->uu_on_throttlelist);

			rethrottle_wakeups++;
			KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_FSRW, 102)), thread_tid(ut->uu_thread), ut->uu_on_throttlelist, my_new_level, 0, 0);
		}
	}
	lck_spin_unlock(&ut->uu_rethrottle_lock);
	ml_set_interrupts_enabled(s);
}


/*
 * KPI routine
 *
 * Create and take a reference on a throttle info structure and return a
 * pointer for the file system to use when calling throttle_info_update.
 * Calling file system must have a matching release for every create.
 */
void *
throttle_info_create(void)
{
	struct _throttle_io_info_t *info; 
	int	level;

	MALLOC(info, struct _throttle_io_info_t *, sizeof(*info), M_TEMP, M_ZERO | M_WAITOK);
	/* Should never happen but just in case */
	if (info == NULL)
		return NULL;
	/* Mark that this one was allocated and needs to be freed */
	DEBUG_ALLOC_THROTTLE_INFO("Creating info = %p\n", info, info );
	info->throttle_alloc = TRUE;

	lck_mtx_init(&info->throttle_lock, throttle_lock_grp, throttle_lock_attr);
	info->throttle_timer_call = thread_call_allocate((thread_call_func_t)throttle_timer, (thread_call_param_t)info);

	for (level = 0; level <= THROTTLE_LEVEL_END; level++) {
		TAILQ_INIT(&info->throttle_uthlist[level]);
	}
	info->throttle_next_wake_level = THROTTLE_LEVEL_END;

	/* Take a reference */
	OSIncrementAtomic(&info->throttle_refcnt);
	return info;
}

/*
 * KPI routine
 *
 * Release the throttle info pointer if all the reference are gone. Should be 
 * called to release reference taken by throttle_info_create 
 */ 
void
throttle_info_release(void *throttle_info)
{
	DEBUG_ALLOC_THROTTLE_INFO("Releaseing info = %p\n",
		(struct _throttle_io_info_t *)throttle_info,
		(struct _throttle_io_info_t *)throttle_info);
	if (throttle_info) /* Just to be careful */
		throttle_info_rel(throttle_info);
}

/*
 * KPI routine
 *
 * File Systems that create an info structure, need to call this routine in
 * their mount routine (used by cluster code). File Systems that call this in
 * their mount routines must call throttle_info_mount_rel in their unmount
 * routines. 
 */
void 
throttle_info_mount_ref(mount_t mp, void *throttle_info)
{
	if ((throttle_info == NULL) || (mp == NULL))
		return;
	throttle_info_ref(throttle_info);

	/*
	 * We already have a reference release it before adding the new one
	 */
	if (mp->mnt_throttle_info)
		throttle_info_rel(mp->mnt_throttle_info);
	mp->mnt_throttle_info = throttle_info;
}

/*
 * Private KPI routine
 *
 * return a handle for accessing throttle_info given a throttle_mask.  The
 * handle must be released by throttle_info_rel_by_mask
 */
int
throttle_info_ref_by_mask(uint64_t throttle_mask, throttle_info_handle_t *throttle_info_handle)
{
	int	dev_index;
	struct _throttle_io_info_t *info;

	if (throttle_info_handle == NULL)
		return EINVAL;
	
	dev_index = num_trailing_0(throttle_mask);
	info = &_throttle_io_info[dev_index];
	throttle_info_ref(info);
	*(struct _throttle_io_info_t**)throttle_info_handle = info;

	return 0;
}

/*
 * Private KPI routine
 *
 * release the handle obtained by throttle_info_ref_by_mask
 */
void
throttle_info_rel_by_mask(throttle_info_handle_t throttle_info_handle)
{
	/*
	 * for now the handle is just a pointer to _throttle_io_info_t
	 */
	throttle_info_rel((struct _throttle_io_info_t*)throttle_info_handle);
}

/*
 * KPI routine
 *
 * File Systems that throttle_info_mount_ref, must call this routine in their
 * umount routine.
 */ 
void
throttle_info_mount_rel(mount_t mp)
{
	if (mp->mnt_throttle_info)
		throttle_info_rel(mp->mnt_throttle_info);
	mp->mnt_throttle_info = NULL;
}

/*
 * Reset throttling periods for the given mount point
 *
 * private interface used by disk conditioner to reset
 * throttling periods when 'is_ssd' status changes
 */
void
throttle_info_mount_reset_period(mount_t mp, int isssd)
{
	struct _throttle_io_info_t *info;

	if (mp == NULL)
		info = &_throttle_io_info[LOWPRI_MAX_NUM_DEV - 1];
	else if (mp->mnt_throttle_info == NULL)
		info = &_throttle_io_info[mp->mnt_devbsdunit];
	else
		info = mp->mnt_throttle_info;

	throttle_init_throttle_period(info, isssd);
}

void
throttle_info_get_last_io_time(mount_t mp, struct timeval *tv)
{
    	struct _throttle_io_info_t *info;

	if (mp == NULL)
		info = &_throttle_io_info[LOWPRI_MAX_NUM_DEV - 1];
	else if (mp->mnt_throttle_info == NULL)
		info = &_throttle_io_info[mp->mnt_devbsdunit];
	else
		info = mp->mnt_throttle_info;

	*tv = info->throttle_last_write_timestamp;
}

void
update_last_io_time(mount_t mp)
{
    	struct _throttle_io_info_t *info;
		
	if (mp == NULL)
		info = &_throttle_io_info[LOWPRI_MAX_NUM_DEV - 1];
	else if (mp->mnt_throttle_info == NULL)
		info = &_throttle_io_info[mp->mnt_devbsdunit];
	else
		info = mp->mnt_throttle_info;

	microuptime(&info->throttle_last_write_timestamp);
	if (mp != NULL)
		mp->mnt_last_write_completed_timestamp = info->throttle_last_write_timestamp;
}

int
throttle_get_io_policy(uthread_t *ut)
{
	if (ut != NULL)
		*ut = get_bsdthread_info(current_thread());

	return (proc_get_effective_thread_policy(current_thread(), TASK_POLICY_IO));
}

int
throttle_get_passive_io_policy(uthread_t *ut)
{
	if (ut != NULL)
		*ut = get_bsdthread_info(current_thread());

	return (proc_get_effective_thread_policy(current_thread(), TASK_POLICY_PASSIVE_IO));
}


static int
throttle_get_thread_throttle_level(uthread_t ut)
{
	uthread_t *ut_p = (ut == NULL) ? &ut : NULL;
	int io_tier = throttle_get_io_policy(ut_p);

	return throttle_get_thread_throttle_level_internal(ut, io_tier);
}

/*
 * Return a throttle level given an existing I/O tier (such as returned by throttle_get_io_policy)
 */
static int
throttle_get_thread_throttle_level_internal(uthread_t ut, int io_tier) {
	int thread_throttle_level = io_tier;
	int user_idle_level;

	assert(ut != NULL);

	/* Bootcache misses should always be throttled */
	if (ut->uu_throttle_bc == TRUE)
		thread_throttle_level = THROTTLE_LEVEL_TIER3;

	/*
	 * Issue tier3 I/O as tier2 when the user is idle
	 * to allow maintenance tasks to make more progress.
	 *
	 * Assume any positive idle level is enough... for now it's
	 * only ever 0 or 128 but this is not defined anywhere.
	 */
	if (thread_throttle_level >= THROTTLE_LEVEL_TIER3) {
		user_idle_level = timer_get_user_idle_level();
		if (user_idle_level > 0) {
			thread_throttle_level--;
		}
	}

	return (thread_throttle_level);
}

/*
 * I/O will be throttled if either of the following are true:
 *   - Higher tiers have in-flight I/O
 *   - The time delta since the last start/completion of a higher tier is within the throttle window interval
 *
 * In-flight I/O is bookended by throttle_info_update_internal/throttle_info_end_io_internal
 */
static int
throttle_io_will_be_throttled_internal(void * throttle_info, int * mylevel, int * throttling_level)
{
    	struct _throttle_io_info_t *info = throttle_info;
	struct timeval elapsed;
	struct timeval now;
	uint64_t elapsed_msecs;
	int	thread_throttle_level;
	int	throttle_level;

	if ((thread_throttle_level = throttle_get_thread_throttle_level(NULL)) < THROTTLE_LEVEL_THROTTLED)
		return (THROTTLE_DISENGAGED);

	microuptime(&now);

	for (throttle_level = THROTTLE_LEVEL_START; throttle_level < thread_throttle_level; throttle_level++) {
		if (info->throttle_inflight_count[throttle_level]) {
			break;
		}
		elapsed = now;
		timevalsub(&elapsed, &info->throttle_window_start_timestamp[throttle_level]);
		elapsed_msecs = (uint64_t)elapsed.tv_sec * (uint64_t)1000 + (elapsed.tv_usec / 1000);

		if (elapsed_msecs < (uint64_t)throttle_windows_msecs[thread_throttle_level])
			break;
	}
	if (throttle_level >= thread_throttle_level) {
		/*
		 * we're beyond all of the throttle windows
		 * that affect the throttle level of this thread,
		 * so go ahead and treat as normal I/O
		 */
		return (THROTTLE_DISENGAGED);
	}
	if (mylevel)
		*mylevel = thread_throttle_level;
	if (throttling_level)
		*throttling_level = throttle_level;

	if (info->throttle_io_count != info->throttle_io_count_begin) {
		/*
		 * we've already issued at least one throttleable I/O
		 * in the current I/O window, so avoid issuing another one
		 */
		return (THROTTLE_NOW);
	}
	/*
	 * we're in the throttle window, so
	 * cut the I/O size back
	 */
	return (THROTTLE_ENGAGED);
}

/* 
 * If we have a mount point and it has a throttle info pointer then
 * use it to do the check, otherwise use the device unit number to find
 * the correct throttle info array element.
 */
int
throttle_io_will_be_throttled(__unused int lowpri_window_msecs, mount_t mp)
{
    	struct _throttle_io_info_t	*info;

	/*
	 * Should we just return zero if no mount point
	 */
	if (mp == NULL)
	        info = &_throttle_io_info[LOWPRI_MAX_NUM_DEV - 1];
	else if (mp->mnt_throttle_info == NULL)
	        info = &_throttle_io_info[mp->mnt_devbsdunit];
	else
	        info = mp->mnt_throttle_info;

	if (info->throttle_is_fusion_with_priority) {
		uthread_t ut = get_bsdthread_info(current_thread());
		if (ut->uu_lowpri_window == 0)
			return (THROTTLE_DISENGAGED);
	}

	if (info->throttle_disabled)
		return (THROTTLE_DISENGAGED);
	else
		return throttle_io_will_be_throttled_internal(info, NULL, NULL);
}

/* 
 * Routine to increment I/O throttling counters maintained in the proc
 */

static void 
throttle_update_proc_stats(pid_t throttling_pid, int count)
{
	proc_t throttling_proc;
	proc_t throttled_proc = current_proc();

	/* The throttled_proc is always the current proc; so we are not concerned with refs */
	OSAddAtomic64(count, &(throttled_proc->was_throttled));
	
	/* The throttling pid might have exited by now */
	throttling_proc = proc_find(throttling_pid);
	if (throttling_proc != PROC_NULL) {
		OSAddAtomic64(count, &(throttling_proc->did_throttle));
		proc_rele(throttling_proc);
	}
}

/*
 * Block until woken up by the throttle timer or by a rethrottle call.
 * As long as we hold the throttle_lock while querying the throttle tier, we're
 * safe against seeing an old throttle tier after a rethrottle.
 */
uint32_t
throttle_lowpri_io(int sleep_amount)
{
	uthread_t ut;
	struct _throttle_io_info_t *info;
	int	throttle_type = 0;
	int	mylevel = 0;
	int	throttling_level = THROTTLE_LEVEL_NONE;
	int	sleep_cnt = 0;
	uint32_t  throttle_io_period_num = 0;
	boolean_t insert_tail = TRUE;
	boolean_t s;

	ut = get_bsdthread_info(current_thread());

	if (ut->uu_lowpri_window == 0)
		return (0);

	info = ut->uu_throttle_info;

	if (info == NULL) {
		ut->uu_throttle_bc = FALSE;
		ut->uu_lowpri_window = 0;
		return (0);
	}
	lck_mtx_lock(&info->throttle_lock);
	assert(ut->uu_on_throttlelist < THROTTLE_LEVEL_THROTTLED);

	if (sleep_amount == 0)
		goto done;

	if (sleep_amount == 1 && ut->uu_throttle_bc == FALSE)
		sleep_amount = 0;

	throttle_io_period_num = info->throttle_io_period_num;

	ut->uu_was_rethrottled = FALSE;

	while ( (throttle_type = throttle_io_will_be_throttled_internal(info, &mylevel, &throttling_level)) ) {

		if (throttle_type == THROTTLE_ENGAGED) {
			if (sleep_amount == 0)
				break;			
			if (info->throttle_io_period_num < throttle_io_period_num)
				break;
			if ((info->throttle_io_period_num - throttle_io_period_num) >= (uint32_t)sleep_amount)
				break;
		}
		/*
		 * keep the same position in the list if "rethrottle_thread" changes our throttle level  and
		 * then puts us back to the original level before we get a chance to run
		 */
		if (ut->uu_on_throttlelist >= THROTTLE_LEVEL_THROTTLED && ut->uu_on_throttlelist != mylevel) {
			/*
			 * must have been awakened via "rethrottle_thread" (the timer pulls us off the list)
			 * and we've changed our throttling level, so pull ourselves off of the appropriate list
			 * and make sure we get put on the tail of the new list since we're starting anew w/r to
			 * the throttling engine
			 */
			TAILQ_REMOVE(&info->throttle_uthlist[ut->uu_on_throttlelist], ut, uu_throttlelist);
			ut->uu_on_throttlelist = THROTTLE_LEVEL_NONE;
			insert_tail = TRUE;
		}
		if (ut->uu_on_throttlelist < THROTTLE_LEVEL_THROTTLED) {
			if (throttle_add_to_list(info, ut, mylevel, insert_tail) == THROTTLE_LEVEL_END)
				goto done;
		}
		assert(throttling_level >= THROTTLE_LEVEL_START && throttling_level <= THROTTLE_LEVEL_END);

		s = ml_set_interrupts_enabled(FALSE);
		lck_spin_lock(&ut->uu_rethrottle_lock);

		/*
		 * this is the critical section w/r to our interaction
		 * with "rethrottle_thread"
		 */
		if (ut->uu_was_rethrottled == TRUE) {

			lck_spin_unlock(&ut->uu_rethrottle_lock);
			ml_set_interrupts_enabled(s);
			lck_mtx_yield(&info->throttle_lock);

			KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_FSRW, 103)), thread_tid(ut->uu_thread), ut->uu_on_throttlelist, 0, 0, 0);

			ut->uu_was_rethrottled = FALSE;
			continue;
		}
		KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_THROTTLE, PROCESS_THROTTLED)) | DBG_FUNC_NONE,
				info->throttle_last_IO_pid[throttling_level], throttling_level, proc_selfpid(), mylevel, 0);
		
		if (sleep_cnt == 0) {
			KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_FSRW, 97)) | DBG_FUNC_START,
					      throttle_windows_msecs[mylevel], info->throttle_io_periods[mylevel], info->throttle_io_count, 0, 0);
			throttled_count[mylevel]++;
		}
		ut->uu_wmesg = "throttle_lowpri_io";

		assert_wait((caddr_t)&ut->uu_on_throttlelist, THREAD_UNINT);

		ut->uu_is_throttled = TRUE;
		lck_spin_unlock(&ut->uu_rethrottle_lock);
		ml_set_interrupts_enabled(s);

		lck_mtx_unlock(&info->throttle_lock);

		thread_block(THREAD_CONTINUE_NULL);

		ut->uu_wmesg = NULL;

		ut->uu_is_throttled = FALSE;
		ut->uu_was_rethrottled = FALSE;

		lck_mtx_lock(&info->throttle_lock);

		sleep_cnt++;
		
		if (sleep_amount == 0)
			insert_tail = FALSE;
		else if (info->throttle_io_period_num < throttle_io_period_num ||
			 (info->throttle_io_period_num - throttle_io_period_num) >= (uint32_t)sleep_amount) {
			insert_tail = FALSE;
			sleep_amount = 0;
		}
	}
done:
	if (ut->uu_on_throttlelist >= THROTTLE_LEVEL_THROTTLED) {
		TAILQ_REMOVE(&info->throttle_uthlist[ut->uu_on_throttlelist], ut, uu_throttlelist);
		ut->uu_on_throttlelist = THROTTLE_LEVEL_NONE;
	}
	lck_mtx_unlock(&info->throttle_lock);

	if (sleep_cnt) {
		KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_FSRW, 97)) | DBG_FUNC_END,
				      throttle_windows_msecs[mylevel], info->throttle_io_periods[mylevel], info->throttle_io_count, 0, 0);
		/*
		 * We update the stats for the last pid which opened a throttle window for the throttled thread.
		 * This might not be completely accurate since the multiple throttles seen by the lower tier pid
		 * might have been caused by various higher prio pids. However, updating these stats accurately 
		 * means doing a proc_find while holding the throttle lock which leads to deadlock.
		 */
		throttle_update_proc_stats(info->throttle_last_IO_pid[throttling_level], sleep_cnt);
	}

	ut->uu_throttle_info = NULL;
	ut->uu_throttle_bc = FALSE;
	ut->uu_lowpri_window = 0;

	throttle_info_rel(info);

	return (sleep_cnt);
}

/*
 * KPI routine
 *
 * set a kernel thread's IO policy.  policy can be:
 * IOPOL_NORMAL, IOPOL_THROTTLE, IOPOL_PASSIVE, IOPOL_UTILITY, IOPOL_STANDARD
 *
 * explanations about these policies are in the man page of setiopolicy_np
 */
void throttle_set_thread_io_policy(int policy)
{
	proc_set_thread_policy(current_thread(), TASK_POLICY_INTERNAL, TASK_POLICY_IOPOL, policy);
}

void throttle_info_reset_window(uthread_t ut)
{
	struct _throttle_io_info_t *info;

	if (ut == NULL) 
		ut = get_bsdthread_info(current_thread());

	if ( (info = ut->uu_throttle_info) ) {
		throttle_info_rel(info);

		ut->uu_throttle_info = NULL;
		ut->uu_lowpri_window = 0;
		ut->uu_throttle_bc = FALSE;
	}
}

static
void throttle_info_set_initial_window(uthread_t ut, struct _throttle_io_info_t *info, boolean_t BC_throttle, boolean_t isssd)
{
	if (lowpri_throttle_enabled == 0 || info->throttle_disabled)
		return;

	if (info->throttle_io_periods == 0) {
		throttle_init_throttle_period(info, isssd);
	}
	if (ut->uu_throttle_info == NULL) {

		ut->uu_throttle_info = info;
		throttle_info_ref(info);
		DEBUG_ALLOC_THROTTLE_INFO("updating info = %p\n", info, info );

		ut->uu_lowpri_window = 1;
		ut->uu_throttle_bc = BC_throttle;
	}
}

/*
 * Update inflight IO count and throttling window
 * Should be called when an IO is done
 *
 * Only affects IO that was sent through spec_strategy
 */
void throttle_info_end_io(buf_t bp) {
	mount_t mp;
	struct bufattr *bap;
	struct _throttle_io_info_t *info;
	int io_tier;

	bap = &bp->b_attr;
	if (!ISSET(bap->ba_flags, BA_STRATEGY_TRACKED_IO)) {
		return;
	}
	CLR(bap->ba_flags, BA_STRATEGY_TRACKED_IO);

	mp = buf_vnode(bp)->v_mount;
	if (mp != NULL) {
		info = &_throttle_io_info[mp->mnt_devbsdunit];
	} else {
		info = &_throttle_io_info[LOWPRI_MAX_NUM_DEV - 1];
	}

	io_tier = GET_BUFATTR_IO_TIER(bap);
	if (ISSET(bap->ba_flags, BA_IO_TIER_UPGRADE)) {
		io_tier--;
	}

	throttle_info_end_io_internal(info, io_tier);
}

/*
 * Decrement inflight count initially incremented by throttle_info_update_internal
 */
static
void throttle_info_end_io_internal(struct _throttle_io_info_t *info, int throttle_level) {
	if (throttle_level == THROTTLE_LEVEL_NONE) {
		return;
	}

	microuptime(&info->throttle_window_start_timestamp[throttle_level]);
	OSDecrementAtomic(&info->throttle_inflight_count[throttle_level]);
	assert(info->throttle_inflight_count[throttle_level] >= 0);
}

/*
 * If inflight is TRUE and bap is NULL then the caller is responsible for calling
 * throttle_info_end_io_internal to avoid leaking in-flight I/O.
 */
static
int throttle_info_update_internal(struct _throttle_io_info_t *info, uthread_t ut, int flags, boolean_t isssd, boolean_t inflight, struct bufattr *bap)
{
	int	thread_throttle_level;

	if (lowpri_throttle_enabled == 0 || info->throttle_disabled)
		return THROTTLE_LEVEL_NONE;

	if (ut == NULL)
		ut = get_bsdthread_info(current_thread());

	if (bap && inflight && !ut->uu_throttle_bc) {
		thread_throttle_level = GET_BUFATTR_IO_TIER(bap);
		if (ISSET(bap->ba_flags, BA_IO_TIER_UPGRADE)) {
			thread_throttle_level--;
		}
	} else {
		thread_throttle_level = throttle_get_thread_throttle_level(ut);
	}

	if (thread_throttle_level != THROTTLE_LEVEL_NONE) {
        if(!ISSET(flags, B_PASSIVE)) {
			info->throttle_last_IO_pid[thread_throttle_level] = proc_selfpid();
			if (inflight && !ut->uu_throttle_bc) {
				if (NULL != bap) {
					SET(bap->ba_flags, BA_STRATEGY_TRACKED_IO);
				}
				OSIncrementAtomic(&info->throttle_inflight_count[thread_throttle_level]);
			} else {
				microuptime(&info->throttle_window_start_timestamp[thread_throttle_level]);
			}
			KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_THROTTLE, OPEN_THROTTLE_WINDOW)) | DBG_FUNC_NONE,
					current_proc()->p_pid, thread_throttle_level, 0, 0, 0);
		}
		microuptime(&info->throttle_last_IO_timestamp[thread_throttle_level]);
	}


	if (thread_throttle_level >= THROTTLE_LEVEL_THROTTLED) {
		/*
		 * I'd really like to do the IOSleep here, but
		 * we may be holding all kinds of filesystem related locks
		 * and the pages for this I/O marked 'busy'...
		 * we don't want to cause a normal task to block on
		 * one of these locks while we're throttling a task marked
		 * for low priority I/O... we'll mark the uthread and
		 * do the delay just before we return from the system
		 * call that triggered this I/O or from vnode_pagein
		 */
	        OSAddAtomic(1, &info->throttle_io_count);

		throttle_info_set_initial_window(ut, info, FALSE, isssd);
	}

	return thread_throttle_level;
}

void *throttle_info_update_by_mount(mount_t mp)
{
	struct _throttle_io_info_t *info;
	uthread_t ut;
	boolean_t isssd = FALSE;

	ut = get_bsdthread_info(current_thread());

	if (mp != NULL) {
		if (disk_conditioner_mount_is_ssd(mp))
			isssd = TRUE;
		info = &_throttle_io_info[mp->mnt_devbsdunit];
	} else
		info = &_throttle_io_info[LOWPRI_MAX_NUM_DEV - 1];

	if (!ut->uu_lowpri_window)
		throttle_info_set_initial_window(ut, info, FALSE, isssd);

	return info;
}


/*
 * KPI routine
 *
 * this is usually called before every I/O, used for throttled I/O
 * book keeping.  This routine has low overhead and does not sleep
 */
void throttle_info_update(void *throttle_info, int flags)
{
        if (throttle_info)
		throttle_info_update_internal(throttle_info, NULL, flags, FALSE, FALSE, NULL);
}

/*
 * KPI routine
 *
 * this is usually called before every I/O, used for throttled I/O
 * book keeping.  This routine has low overhead and does not sleep
 */
void throttle_info_update_by_mask(void *throttle_info_handle, int flags)
{
	void *throttle_info = throttle_info_handle;

	/*
	 * for now we only use the lowest bit of the throttle mask, so the
	 * handle is the same as the throttle_info.  Later if we store a
	 * set of throttle infos in the handle, we will want to loop through
	 * them and call throttle_info_update in a loop
	 */
	throttle_info_update(throttle_info, flags);
}
/*
 * KPI routine
 * 
 * This routine marks the throttle info as disabled. Used for mount points which 
 * support I/O scheduling.
 */

void throttle_info_disable_throttle(int devno, boolean_t isfusion)
{
	struct _throttle_io_info_t *info;

	if (devno < 0 || devno >= LOWPRI_MAX_NUM_DEV) 
		panic("Illegal devno (%d) passed into throttle_info_disable_throttle()", devno);

	info = &_throttle_io_info[devno];
	// don't disable software throttling on devices that are part of a fusion device
	// and override the software throttle periods to use HDD periods
	if (isfusion) {
		info->throttle_is_fusion_with_priority = isfusion;
		throttle_init_throttle_period(info, FALSE);
	}
	info->throttle_disabled = !info->throttle_is_fusion_with_priority;
	return;
} 


/*
 * KPI routine (private)
 * Called to determine if this IO is being throttled to this level so that it can be treated specially
 */
int throttle_info_io_will_be_throttled(void * throttle_info, int policy)
{
    	struct _throttle_io_info_t *info = throttle_info;
	struct timeval elapsed;
	uint64_t elapsed_msecs;
	int	throttle_level;
	int	thread_throttle_level;

        switch (policy) {

        case IOPOL_THROTTLE:
                thread_throttle_level = THROTTLE_LEVEL_TIER3;
                break;
        case IOPOL_UTILITY:
                thread_throttle_level = THROTTLE_LEVEL_TIER2;
                break;
        case IOPOL_STANDARD:
                thread_throttle_level = THROTTLE_LEVEL_TIER1;
                break;
        default:
                thread_throttle_level = THROTTLE_LEVEL_TIER0;
		break;
	}
	for (throttle_level = THROTTLE_LEVEL_START; throttle_level < thread_throttle_level; throttle_level++) {
		if (info->throttle_inflight_count[throttle_level]) {
			break;
		}

		microuptime(&elapsed);
		timevalsub(&elapsed, &info->throttle_window_start_timestamp[throttle_level]);
		elapsed_msecs = (uint64_t)elapsed.tv_sec * (uint64_t)1000 + (elapsed.tv_usec / 1000);

		if (elapsed_msecs < (uint64_t)throttle_windows_msecs[thread_throttle_level])
			break;
	}
	if (throttle_level >= thread_throttle_level) {
		/*
		 * we're beyond all of the throttle windows
		 * so go ahead and treat as normal I/O
		 */
		return (THROTTLE_DISENGAGED);
	}
	/*
	 * we're in the throttle window
	 */
	return (THROTTLE_ENGAGED);
}

int throttle_lowpri_window(void)
{
	struct uthread *ut = get_bsdthread_info(current_thread());
	return ut->uu_lowpri_window;
}


#if CONFIG_IOSCHED
int upl_get_cached_tier(void *);
#endif

int
spec_strategy(struct vnop_strategy_args *ap)
{
	buf_t	bp;
	int	bflags;
	int	io_tier;
	int	passive;
	dev_t	bdev;
	uthread_t ut;
	mount_t mp;
	struct	bufattr *bap;
	int	strategy_ret;
	struct _throttle_io_info_t *throttle_info;
	boolean_t isssd = FALSE;
	boolean_t inflight = FALSE;
	boolean_t upgrade = FALSE;
	int code = 0;

#if !CONFIG_EMBEDDED
	proc_t curproc = current_proc();
#endif /* !CONFIG_EMBEDDED */

        bp = ap->a_bp;
	bdev = buf_device(bp);
	mp = buf_vnode(bp)->v_mount;
	bap = &bp->b_attr;

#if CONFIG_IOSCHED
       if (bp->b_flags & B_CLUSTER) {

               io_tier = upl_get_cached_tier(bp->b_upl);

               if (io_tier == -1)
                       io_tier = throttle_get_io_policy(&ut);
#if DEVELOPMENT || DEBUG
               else {
                       int my_io_tier = throttle_get_io_policy(&ut);

                       if (io_tier != my_io_tier)
                               KERNEL_DEBUG_CONSTANT((FSDBG_CODE(DBG_THROTTLE, IO_TIER_UPL_MISMATCH)) | DBG_FUNC_NONE, buf_kernel_addrperm_addr(bp), my_io_tier, io_tier, 0, 0);
               }
#endif
       } else
               io_tier = throttle_get_io_policy(&ut);
#else
	io_tier = throttle_get_io_policy(&ut);
#endif
	passive = throttle_get_passive_io_policy(&ut);

	/*
	 * Mark if the I/O was upgraded by throttle_get_thread_throttle_level
	 * while preserving the original issued tier (throttle_get_io_policy
	 * does not return upgraded tiers)
	 */
	if (mp && io_tier > throttle_get_thread_throttle_level_internal(ut, io_tier)) {
#if CONFIG_IOSCHED
		if (!(mp->mnt_ioflags & MNT_IOFLAGS_IOSCHED_SUPPORTED)) {
			upgrade = TRUE;
		}
#else /* CONFIG_IOSCHED */
		upgrade = TRUE;
#endif /* CONFIG_IOSCHED */
	}

	if (bp->b_flags & B_META)
		bap->ba_flags |= BA_META;

#if CONFIG_IOSCHED
	/* 
	 * For I/O Scheduling, we currently do not have a way to track and expedite metadata I/Os.
	 * To ensure we dont get into priority inversions due to metadata I/Os, we use the following rules:
	 * For metadata reads, ceil all I/Os to IOSCHED_METADATA_TIER & mark them passive if the I/O tier was upgraded
	 * For metadata writes, unconditionally mark them as IOSCHED_METADATA_TIER and passive
	 */
	if (bap->ba_flags & BA_META) {
		if (mp && (mp->mnt_ioflags & MNT_IOFLAGS_IOSCHED_SUPPORTED)) {
			if (bp->b_flags & B_READ) {
				if (io_tier > IOSCHED_METADATA_TIER) {
					io_tier = IOSCHED_METADATA_TIER;
					passive = 1;
				}
			} else {
				io_tier = IOSCHED_METADATA_TIER;
				passive = 1;
			}
		}
	}
#endif /* CONFIG_IOSCHED */
			
	SET_BUFATTR_IO_TIER(bap, io_tier);

	if (passive) {
		bp->b_flags |= B_PASSIVE;
		bap->ba_flags |= BA_PASSIVE;
	}

#if !CONFIG_EMBEDDED
	if ((curproc != NULL) && ((curproc->p_flag & P_DELAYIDLESLEEP) == P_DELAYIDLESLEEP))
		bap->ba_flags |= BA_DELAYIDLESLEEP;
#endif /* !CONFIG_EMBEDDED */
		
	bflags = bp->b_flags;

	if (((bflags & B_READ) == 0) && ((bflags & B_ASYNC) == 0))
		bufattr_markquickcomplete(bap);

	if (bflags & B_READ)
	        code |= DKIO_READ;
	if (bflags & B_ASYNC)
	        code |= DKIO_ASYNC;

	if (bap->ba_flags & BA_META)
	        code |= DKIO_META;
	else if (bflags & B_PAGEIO)
	        code |= DKIO_PAGING;

	if (io_tier != 0)
		code |= DKIO_THROTTLE;

	code |= ((io_tier << DKIO_TIER_SHIFT) & DKIO_TIER_MASK);

	if (bflags & B_PASSIVE)
		code |= DKIO_PASSIVE;

	if (bap->ba_flags & BA_NOCACHE)
		code |= DKIO_NOCACHE;

	if (upgrade) {
		code |= DKIO_TIER_UPGRADE;
		SET(bap->ba_flags, BA_IO_TIER_UPGRADE);
	}

	if (kdebug_enable) {
		KERNEL_DEBUG_CONSTANT_IST(KDEBUG_COMMON, FSDBG_CODE(DBG_DKRW, code) | DBG_FUNC_NONE,
					  buf_kernel_addrperm_addr(bp), bdev, (int)buf_blkno(bp), buf_count(bp), 0);
        }

	thread_update_io_stats(current_thread(), buf_count(bp), code);

	if (mp != NULL) {
		if (disk_conditioner_mount_is_ssd(mp))
			isssd = TRUE;
		/*
		 * Partially initialized mounts don't have a final devbsdunit and should not be tracked.
		 * Verify that devbsdunit is initialized (non-zero) or that 0 is the correct initialized value
		 * (mnt_throttle_mask is initialized and num_trailing_0 would be 0)
		 */
		if (mp->mnt_devbsdunit || (mp->mnt_throttle_mask != LOWPRI_MAX_NUM_DEV - 1 && mp->mnt_throttle_mask & 0x1)) {
			inflight = TRUE;
		}
		throttle_info = &_throttle_io_info[mp->mnt_devbsdunit];

	} else
		throttle_info = &_throttle_io_info[LOWPRI_MAX_NUM_DEV - 1];

	throttle_info_update_internal(throttle_info, ut, bflags, isssd, inflight, bap);

	if ((bflags & B_READ) == 0) {
		microuptime(&throttle_info->throttle_last_write_timestamp);

		if (mp) {
			mp->mnt_last_write_issued_timestamp = throttle_info->throttle_last_write_timestamp;
			INCR_PENDING_IO(buf_count(bp), mp->mnt_pending_write_size);
		}
	} else if (mp) {
		INCR_PENDING_IO(buf_count(bp), mp->mnt_pending_read_size);
	}
	/*
	 * The BootCache may give us special information about
	 * the IO, so it returns special values that we check
	 * for here.
	 *
	 * IO_SATISFIED_BY_CACHE
	 * The read has been satisfied by the boot cache. Don't
	 * throttle the thread unnecessarily.
	 *
	 * IO_SHOULD_BE_THROTTLED
	 * The boot cache is playing back a playlist and this IO
	 * cut through. Throttle it so we're not cutting through
	 * the boot cache too often.
	 *
	 * Note that typical strategy routines are defined with
	 * a void return so we'll get garbage here. In the 
	 * unlikely case the garbage matches our special return
	 * value, it's not a big deal since we're only adjusting
	 * the throttling delay.
 	 */
#define IO_SATISFIED_BY_CACHE  ((int)0xcafefeed)
#define IO_SHOULD_BE_THROTTLED ((int)0xcafebeef)
	typedef	int strategy_fcn_ret_t(struct buf *bp);
	
	strategy_ret = (*(strategy_fcn_ret_t*)bdevsw[major(bdev)].d_strategy)(bp);

	// disk conditioner needs to track when this I/O actually starts
	// which means track it after `strategy` which may include delays
	// from inflight I/Os
	microuptime(&bp->b_timestamp_tv);
	
	if (IO_SATISFIED_BY_CACHE == strategy_ret) {
		/*
		 * If this was a throttled IO satisfied by the boot cache,
		 * don't delay the thread.
		 */
		throttle_info_reset_window(ut);

	} else if (IO_SHOULD_BE_THROTTLED == strategy_ret) {
		/*
		 * If the boot cache indicates this IO should be throttled,
		 * delay the thread.
		 */
		throttle_info_set_initial_window(ut, throttle_info, TRUE, isssd);
	}
	return (0);
}


/*
 * This is a noop, simply returning what one has been given.
 */
int
spec_blockmap(__unused struct vnop_blockmap_args *ap)
{
	return (ENOTSUP);
}


/*
 * Device close routine
 */
int
spec_close(struct vnop_close_args *ap)
{
	struct vnode *vp = ap->a_vp;
	dev_t dev = vp->v_rdev;
	int error = 0;
	int flags = ap->a_fflag;
	struct proc *p = vfs_context_proc(ap->a_context);
	struct session *sessp;

	switch (vp->v_type) {

	case VCHR:
		/*
		 * Hack: a tty device that is a controlling terminal
		 * has a reference from the session structure.
		 * We cannot easily tell that a character device is
		 * a controlling terminal, unless it is the closing
		 * process' controlling terminal.  In that case,
		 * if the reference count is 1 (this is the very
		 * last close)
		 */
		sessp = proc_session(p);
		devsw_lock(dev, S_IFCHR);
		if (sessp != SESSION_NULL) {
			if (vp == sessp->s_ttyvp && vcount(vp) == 1) {
				struct tty *tp = TTY_NULL;

				devsw_unlock(dev, S_IFCHR);
				session_lock(sessp);
				if (vp == sessp->s_ttyvp) {
					tp = SESSION_TP(sessp);
					sessp->s_ttyvp = NULL;
					sessp->s_ttyvid = 0;
					sessp->s_ttyp = TTY_NULL;
					sessp->s_ttypgrpid = NO_PID;
				} 
				session_unlock(sessp);

				if (tp != TTY_NULL) {
					/*
					 * We may have won a race with a proc_exit
					 * of the session leader, the winner
					 * clears the flag (even if not set)
					 */
					tty_lock(tp);
					ttyclrpgrphup(tp);
					tty_unlock(tp);

					ttyfree(tp);
				}
				devsw_lock(dev, S_IFCHR);
			}
			session_rele(sessp);
		}

		if (--vp->v_specinfo->si_opencount < 0)
			panic("negative open count (c, %u, %u)", major(dev), minor(dev));

		/*
		 * close on last reference or on vnode revoke call
		 */
		if (vcount(vp) == 0 || (flags & IO_REVOKE) != 0)
			error = cdevsw[major(dev)].d_close(dev, flags, S_IFCHR, p);

		devsw_unlock(dev, S_IFCHR);
		break;

	case VBLK:
		/*
		 * If there is more than one outstanding open, don't
		 * send the close to the device.
		 */
		devsw_lock(dev, S_IFBLK);
		if (vcount(vp) > 1) {
			vp->v_specinfo->si_opencount--;
			devsw_unlock(dev, S_IFBLK);
			return (0);
		}
		devsw_unlock(dev, S_IFBLK);

		/*
		 * On last close of a block device (that isn't mounted)
		 * we must invalidate any in core blocks, so that
		 * we can, for instance, change floppy disks.
		 */
	        if ((error = spec_fsync_internal(vp, MNT_WAIT, ap->a_context)))
		        return (error);

		error = buf_invalidateblks(vp, BUF_WRITE_DATA, 0, 0);
		if (error)
			return (error);

		devsw_lock(dev, S_IFBLK);

		if (--vp->v_specinfo->si_opencount < 0)
			panic("negative open count (b, %u, %u)", major(dev), minor(dev));

		if (vcount(vp) == 0)
			error = bdevsw[major(dev)].d_close(dev, flags, S_IFBLK, p);

		devsw_unlock(dev, S_IFBLK);
		break;

	default:
		panic("spec_close: not special");
		return(EBADF);
	}

	return error;
}

/*
 * Return POSIX pathconf information applicable to special devices.
 */
int
spec_pathconf(struct vnop_pathconf_args *ap)
{

	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = LINK_MAX;
		return (0);
	case _PC_MAX_CANON:
		*ap->a_retval = MAX_CANON;
		return (0);
	case _PC_MAX_INPUT:
		*ap->a_retval = MAX_INPUT;
		return (0);
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		return (0);
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 200112;		/* _POSIX_CHOWN_RESTRICTED */
		return (0);
	case _PC_VDISABLE:
		*ap->a_retval = _POSIX_VDISABLE;
		return (0);
	default:
		return (EINVAL);
	}
	/* NOTREACHED */
}

/*
 * Special device failed operation
 */
int
spec_ebadf(__unused void *dummy)
{

	return (EBADF);
}

/* Blktooff derives file offset from logical block number */
int
spec_blktooff(struct vnop_blktooff_args *ap)
{
	struct vnode *vp = ap->a_vp;

	switch (vp->v_type) {
	case VCHR:
		*ap->a_offset = (off_t)-1; /* failure */
		return (ENOTSUP);

	case VBLK:
		printf("spec_blktooff: not implemented for VBLK\n");
		*ap->a_offset = (off_t)-1; /* failure */
		return (ENOTSUP);

	default:
		panic("spec_blktooff type");
	}
	/* NOTREACHED */

	return (0);
}

/* Offtoblk derives logical block number from file offset */
int
spec_offtoblk(struct vnop_offtoblk_args *ap)
{
	struct vnode *vp = ap->a_vp;

	switch (vp->v_type) {
	case VCHR:
		*ap->a_lblkno = (daddr64_t)-1; /* failure */
		return (ENOTSUP);

	case VBLK:
		printf("spec_offtoblk: not implemented for VBLK\n");
		*ap->a_lblkno = (daddr64_t)-1; /* failure */
		return (ENOTSUP);

	default:
		panic("spec_offtoblk type");
	}
	/* NOTREACHED */

	return (0);
}

static void filt_specdetach(struct knote *kn);
static int filt_specevent(struct knote *kn, long hint);
static int filt_spectouch(struct knote *kn, struct kevent_internal_s *kev);
static int filt_specprocess(struct knote *kn, struct filt_process_s *data, struct kevent_internal_s *kev);
static unsigned filt_specpeek(struct knote *kn);

SECURITY_READ_ONLY_EARLY(struct filterops) spec_filtops = {
	.f_isfd    = 1,
	.f_attach  = filt_specattach,
	.f_detach  = filt_specdetach,
	.f_event   = filt_specevent,
	.f_touch   = filt_spectouch,
	.f_process = filt_specprocess,
	.f_peek    = filt_specpeek
};


/*
 * Given a waitq that is assumed to be embedded within a selinfo structure,
 * return the containing selinfo structure. While 'wq' is not really a queue
 * element, this macro simply does the offset_of calculation to get back to a
 * containing struct given the struct type and member name.
 */
#define selinfo_from_waitq(wq) \
	qe_element((wq), struct selinfo, si_waitq)

static int
spec_knote_select_and_link(struct knote *kn)
{
	uthread_t uth;
	vfs_context_t ctx;
	vnode_t vp;
	struct waitq_set *old_wqs;
	uint64_t rsvd, rsvd_arg;
	uint64_t *rlptr = NULL;
	struct selinfo *si = NULL;
	int selres = 0;

	uth = get_bsdthread_info(current_thread());

	ctx = vfs_context_current();
	vp = (vnode_t)kn->kn_fp->f_fglob->fg_data;

	int error = vnode_getwithvid(vp, kn->kn_hookid);
	if (error != 0) {
		knote_set_error(kn, ENOENT);
		return 0;
	}

	/*
	 * This function may be called many times to link or re-link the
	 * underlying vnode to the kqueue.  If we've already linked the two,
	 * we will have a valid kn_hook_data which ties us to the underlying
	 * device's waitq via a the waitq's prepost table object. However,
	 * devices can abort any select action by calling selthreadclear().
	 * This is OK because the table object will be invalidated by the
	 * driver (through a call to selthreadclear), so any attempt to access
	 * the associated waitq will fail because the table object is invalid.
	 *
	 * Even if we've already registered, we need to pass a pointer
	 * to a reserved link structure. Otherwise, selrecord() will
	 * infer that we're in the second pass of select() and won't
	 * actually do anything!
	 */
	rsvd = rsvd_arg = waitq_link_reserve(NULL);
	rlptr = (void *)&rsvd_arg;

	/*
	 * Trick selrecord() into hooking kqueue's wait queue set
	 * set into device's selinfo wait queue
	 */
	old_wqs = uth->uu_wqset;
	uth->uu_wqset = &(knote_get_kq(kn)->kq_wqs);
	selres = VNOP_SELECT(vp, knote_get_seltype(kn), 0, rlptr, ctx);
	uth->uu_wqset = old_wqs;

	/*
	 * make sure to cleanup the reserved link - this guards against
	 * drivers that may not actually call selrecord().
	 */
	waitq_link_release(rsvd);
	if (rsvd != rsvd_arg) {
		/* the driver / handler called selrecord() */
		struct waitq *wq;
		memcpy(&wq, rlptr, sizeof(void *));

		/*
		 * The waitq is part of the selinfo structure managed by the
		 * driver. For certain drivers, we want to hook the knote into
		 * the selinfo structure's si_note field so selwakeup can call
		 * KNOTE.
		 */
		si = selinfo_from_waitq(wq);

		/*
		 * The waitq_get_prepost_id() function will (potentially)
		 * allocate a prepost table object for the waitq and return
		 * the table object's ID to us.  It will also set the
		 * waitq_prepost_id field within the waitq structure.
		 *
		 * We can just overwrite kn_hook_data because it's simply a
		 * table ID used to grab a reference when needed.
		 *
		 * We have a reference on the vnode, so we know that the
		 * device won't go away while we get this ID.
		 */
		kn->kn_hook_data = waitq_get_prepost_id(wq);
	} else {
		assert(selres != 0);
	}

	vnode_put(vp);

	return selres;
}

static void filt_spec_common(struct knote *kn, int selres)
{
	if (kn->kn_vnode_use_ofst) {
		if (kn->kn_fp->f_fglob->fg_offset >= (uint32_t)selres) {
			kn->kn_data = 0;
		} else {
			kn->kn_data = ((uint32_t)selres) - kn->kn_fp->f_fglob->fg_offset;
		}
	} else {
		kn->kn_data = selres;
	}
}

static int
filt_specattach(struct knote *kn, __unused struct kevent_internal_s *kev)
{
	vnode_t vp;
	dev_t dev;

	vp = (vnode_t)kn->kn_fp->f_fglob->fg_data; /* Already have iocount, and vnode is alive */

	assert(vnode_ischr(vp));

	dev = vnode_specrdev(vp);

	/*
	 * For a few special kinds of devices, we can attach knotes with
	 * no restrictions because their "select" vectors return the amount
	 * of data available.  Others require an explicit NOTE_LOWAT with
	 * data of 1, indicating that the caller doesn't care about actual
	 * data counts, just an indication that the device has data.
	 */
	if (!kn->kn_vnode_kqok &&
	    ((kn->kn_sfflags & NOTE_LOWAT) == 0 || kn->kn_sdata != 1)) {
		knote_set_error(kn, EINVAL);
		return 0;
	}

	/*
	 * This forces the select fallback to call through VNOP_SELECT and hook
	 * up selinfo on every filter routine.
	 *
	 * Pseudo-terminal controllers are opted out of native kevent support --
	 * remove this when they get their own EVFILTID.
	 */
	if (cdevsw_flags[major(dev)] & CDEVSW_IS_PTC) {
		kn->kn_vnode_kqok = 0;
	}

	kn->kn_filtid = EVFILTID_SPEC;
	kn->kn_hook_data = 0;
	kn->kn_hookid = vnode_vid(vp);

	knote_markstayactive(kn);
	return spec_knote_select_and_link(kn);
}

static void
filt_specdetach(struct knote *kn)
{
	knote_clearstayactive(kn);

	/*
	 * This is potentially tricky: the device's selinfo waitq that was
	 * tricked into being part of this knote's waitq set may not be a part
	 * of any other set, and the device itself may have revoked the memory
	 * in which the waitq was held. We use the knote's kn_hook_data field
	 * to keep the ID of the waitq's prepost table object. This
	 * object keeps a pointer back to the waitq, and gives us a safe way
	 * to decouple the dereferencing of driver allocated memory: if the
	 * driver goes away (taking the waitq with it) then the prepost table
	 * object will be invalidated. The waitq details are handled in the
	 * waitq API invoked here.
	 */
	if (kn->kn_hook_data) {
		waitq_unlink_by_prepost_id(kn->kn_hook_data, &(knote_get_kq(kn)->kq_wqs));
		kn->kn_hook_data = 0;
	}
}

static int
filt_specevent(struct knote *kn, __unused long hint)
{
	/*
	 * Nothing should call knote or knote_vanish on this knote.
	 */
	panic("filt_specevent(%p)", kn);
	return 0;
}

static int
filt_spectouch(struct knote *kn, struct kevent_internal_s *kev)
{
	kn->kn_sdata = kev->data;
	kn->kn_sfflags = kev->fflags;
	if ((kn->kn_status & KN_UDATA_SPECIFIC) == 0)
		kn->kn_udata = kev->udata;

	if (kev->flags & EV_ENABLE) {
		return spec_knote_select_and_link(kn);
	}

	return 0;
}

static int
filt_specprocess(struct knote *kn, struct filt_process_s *data, struct kevent_internal_s *kev)
{
#pragma unused(data)
	vnode_t vp;
	uthread_t uth;
	vfs_context_t ctx;
	int res;
	int selres;
	int error;

	uth = get_bsdthread_info(current_thread());
	ctx = vfs_context_current();
	vp = (vnode_t)kn->kn_fp->f_fglob->fg_data;

	/* FIXME JMM - locking against touches? */

	error = vnode_getwithvid(vp, kn->kn_hookid);
	if (error != 0) {
		kn->kn_flags |= (EV_EOF | EV_ONESHOT);
		*kev = kn->kn_kevent;
		return 1;
	}

	selres = spec_knote_select_and_link(kn);
	filt_spec_common(kn, selres);

	vnode_put(vp);

	res = ((kn->kn_sfflags & NOTE_LOWAT) != 0) ?
		(kn->kn_data >= kn->kn_sdata) : kn->kn_data;

	if (res) {
		*kev = kn->kn_kevent;
		if (kn->kn_flags & EV_CLEAR) {
			kn->kn_fflags = 0;
			kn->kn_data = 0;
		}
	}

	return res;
}

static unsigned
filt_specpeek(struct knote *kn)
{
	int selres = 0;

	selres = spec_knote_select_and_link(kn);
	filt_spec_common(kn, selres);

	return kn->kn_data;
}

