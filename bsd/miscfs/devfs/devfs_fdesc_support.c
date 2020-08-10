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
/* Copyright (c) 1995 NeXT Computer, Inc. All Rights Reserved */
/*
 * Copyright (c) 1992, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software donated to Berkeley by
 * Jan-Simon Pendry.
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
 *	@(#)fdesc_vnops.c	8.17 (Berkeley) 5/22/95
 *
 */

/*
 * /dev/fd Filesystem
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/proc_internal.h>
#include <sys/kernel.h> /* boottime */
#include <sys/resourcevar.h>
#include <sys/filedesc.h>
#include <sys/kauth.h>
#include <sys/vnode_internal.h>
#include <sys/malloc.h>
#include <sys/file_internal.h>
#include <sys/stat.h>
#include <sys/mount_internal.h>
#include <sys/namei.h>
#include <sys/dirent.h>
#include <sys/ubc.h>
#include <sys/socketvar.h>
#include <sys/pipe.h>
#include <sys/uio_internal.h>
#include <vfs/vfs_support.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/fdesc.h>
#include <miscfs/devfs/devfs.h>
#include <miscfs/devfs/devfsdefs.h>

#define FDL_WANT        0x01
#define FDL_LOCKED      0x02
static int fdcache_lock;


#if (FD_STDIN != FD_STDOUT - 1) || (FD_STDOUT != FD_STDERR - 1)
FD_STDIN, FD_STDOUT, FD_STDERR must be a sequence n, n + 1, n + 2
#endif

#define NFDCACHE 3

#define FD_NHASH(ix) \
	(&fdhashtbl[(ix) & fdhash])
LIST_HEAD(fdhashhead, fdescnode) * fdhashtbl;
u_long fdhash;

static int fdesc_attr(int fd, struct vnode_attr *vap, vfs_context_t a_context);

lck_mtx_t fdesc_mtx;
lck_grp_t *fdesc_lckgrp;

static void
fdesc_lock(void)
{
	lck_mtx_lock(&fdesc_mtx);
}

static void
fdesc_unlock(void)
{
	lck_mtx_unlock(&fdesc_mtx);
}


/*
 * Initialise cache headers, create the devfs node
 */
int
devfs_fdesc_init()
{
	int error = 0;
	devnode_t *rootdir = dev_root->de_dnp;
	devdirent_t *direntp;

	/* XXX Make sure you have the right path... */
	fdhashtbl = hashinit(NFDCACHE, M_CACHE, &fdhash);
	fdesc_lckgrp = lck_grp_alloc_init("fdesc", NULL);
	lck_mtx_init(&fdesc_mtx, fdesc_lckgrp, NULL);

	DEVFS_LOCK();
	dev_add_entry("fd", rootdir, DEV_DEVFD, NULL, NULL, NULL, &direntp);
	devfs_fdesc_makelinks();
	DEVFS_UNLOCK();

	return error;
}

/*
 * Called during early startup, no need to synchronize
 */
int
devfs_fdesc_makelinks()
{
	int error = 0;
	devdirent_t *stdin_ent = NULL, *stdout_ent = NULL, *stderr_ent = NULL;
	devnode_t *root_devnode = dev_root->de_dnp;

	/* We do this ugliness to get around some "const" warnings */
	char in[] = "stdin";
	char out[] = "stdout";
	char err[] = "stderr";
	char zero[] = "fd/0";
	char one[] = "fd/1";
	char two[] = "fd/2";

	if ((error = devfs_make_symlink(root_devnode, in, 0555, zero, &stdin_ent))) {
		printf("Couldn't make stdin, err %d.\n", error);
		goto bad;
	}

	if ((error = devfs_make_symlink(root_devnode, out, 0555, one, &stdout_ent))) {
		printf("Couldn't make stdout, err %d.\n", error);
		goto bad;
	}

	if ((error = devfs_make_symlink(root_devnode, err, 0555, two, &stderr_ent))) {
		printf("Couldn't make stderr, err %d.\n", error);
		goto bad;
	}

	return 0;

bad:
	if (stdin_ent) {
		dev_free_name(stdin_ent);
	}
	if (stdout_ent) {
		dev_free_name(stdout_ent);
	}
	if (stderr_ent) {
		dev_free_name(stderr_ent);
	}

	return error;
}

int
fdesc_allocvp(fdntype ftype, int ix, struct mount *mp, struct vnode **vpp, enum vtype vtype, int fdno)
{
	struct fdhashhead *fc;
	struct fdescnode *fd;
	int error = 0;
	int vid = 0;
	struct vnode_fsparam vfsp;

	fdesc_lock();

	fc = FD_NHASH(ix);
loop:
	for (fd = fc->lh_first; fd != 0; fd = fd->fd_hash.le_next) {
		if (fd->fd_ix == ix && vnode_mount(fd->fd_vnode) == mp) {
			vid = vnode_vid(fd->fd_vnode);
			fdesc_unlock();

			if (vnode_getwithvid(fd->fd_vnode, vid)) {
				fdesc_lock();
				goto loop;
			}

			*vpp = fd->fd_vnode;
			(*vpp)->v_type = vtype;

			return error;
		}
	}

	/* Only one thread can add to the hash at a time */
	if (fdcache_lock & FDL_LOCKED) {
		fdcache_lock |= FDL_WANT;
		msleep((caddr_t) &fdcache_lock, &fdesc_mtx, PINOD, "fdesc_allocvp", NULL);
		goto loop;
	}

	fdcache_lock |= FDL_LOCKED;
	fdesc_unlock();

	MALLOC(fd, void *, sizeof(struct fdescnode), M_TEMP, M_WAITOK);

	vfsp.vnfs_mp = mp;
	vfsp.vnfs_vtype = vtype;
	vfsp.vnfs_str = "fdesc";
	vfsp.vnfs_dvp = NULL;
	vfsp.vnfs_fsnode = fd;
	vfsp.vnfs_cnp = NULL;
	vfsp.vnfs_vops = fdesc_vnodeop_p;
	vfsp.vnfs_rdev = 0;
	vfsp.vnfs_filesize = 0;
	vfsp.vnfs_flags = VNFS_NOCACHE | VNFS_CANTCACHE;
	vfsp.vnfs_marksystem = 0;
	vfsp.vnfs_markroot = 0;

	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, vpp);
	if (error) {
		FREE(fd, M_TEMP);
		fdesc_lock();
		goto out;
	}

	(*vpp)->v_tag = VT_FDESC;
	fd->fd_vnode = *vpp;
	fd->fd_type = ftype;
	fd->fd_fd = -1;
	fd->fd_link = NULL;
	fd->fd_ix = ix;
	fd->fd_fd = fdno;

	fdesc_lock();

	LIST_INSERT_HEAD(fc, fd, fd_hash);
out:
	/* Hold the lock when we get here */
	fdcache_lock &= ~FDL_LOCKED;

	if (fdcache_lock & FDL_WANT) {
		fdcache_lock &= ~FDL_WANT;
		wakeup((caddr_t) &fdcache_lock);
	}

	fdesc_unlock();

	return error;
}

/*
 * vp is the current namei directory
 * ndp is the name to locate in that directory...
 *
 * This vnop should only be called on the special directory /dev/fd.
 */
int
devfs_devfd_lookup(struct vnop_lookup_args *ap)
{
	struct vnode **vpp = ap->a_vpp;
	struct vnode *dvp = ap->a_dvp;
	struct componentname *cnp = ap->a_cnp;
	char *pname = cnp->cn_nameptr;
	struct proc *p = vfs_context_proc(ap->a_context);
	int numfiles = p->p_fd->fd_nfiles;
	int fd;
	int error;
	struct vnode *fvp;

	if (cnp->cn_namelen == 1 && *pname == '.') {
		*vpp = dvp;

		if ((error = vnode_get(dvp))) {
			return error;
		}
		return 0;
	}

	fd = 0;
	while (*pname >= '0' && *pname <= '9') {
		fd = 10 * fd + *pname++ - '0';
		if (fd >= numfiles) {
			break;
		}
	}

	if (*pname != '\0') {
		error = ENOENT;
		goto bad;
	}

	if (fd < 0 || fd >= numfiles ||
	    *fdfile(p, fd) == NULL ||
	    (*fdflags(p, fd) & UF_RESERVED)) {
		error = EBADF;
		goto bad;
	}

	error = fdesc_allocvp(Fdesc, FD_DESC + fd, dvp->v_mount, &fvp, VNON, fd);
	if (error) {
		goto bad;
	}
	*vpp = fvp;
	return 0;

bad:
	*vpp = NULL;
	return error;
}

int
fdesc_open(struct vnop_open_args *ap)
{
	struct vnode *vp = ap->a_vp;
	thread_t thr = vfs_context_thread(ap->a_context);
	uthread_t uu;
	int error = 0;

	if (thr == NULL) {
		return EINVAL;
	}

	uu = get_bsdthread_info(thr);

	switch (VTOFDESC(vp)->fd_type) {
	case Fdesc:
		/*
		 * XXX Kludge: set uu->uu_dupfd to contain the value of the
		 * the file descriptor being sought for duplication. The error
		 * return ensures that the vnode for this device will be
		 * released by vn_open. Open will detect this special error and
		 * take the actions in dupfdopen.  Other callers of vn_open or
		 * vnop_open will simply report the error.
		 */
		uu->uu_dupfd = VTOFDESC(vp)->fd_fd;     /* XXX */
		error = ENODEV;
		break;
	default:
		panic("Invalid type for fdesc node!");
		break;
	}

	return error;
}

static int
fdesc_attr(int fd, struct vnode_attr *vap, vfs_context_t a_context)
{
	struct fileproc *fp;
	struct proc *p = vfs_context_proc(a_context);
	struct stat stb;
	int error;

	if ((error = fp_lookup(p, fd, &fp, 0))) {
		return error;
	}
	switch (FILEGLOB_DTYPE(fp->f_fglob)) {
	case DTYPE_VNODE:
		if ((error = vnode_getwithref((struct vnode *) fp->f_fglob->fg_data)) != 0) {
			break;
		}
		if ((error = vnode_authorize((struct vnode *)fp->f_fglob->fg_data,
		    NULL,
		    KAUTH_VNODE_READ_ATTRIBUTES | KAUTH_VNODE_READ_SECURITY,
		    a_context)) == 0) {
			error = vnode_getattr((struct vnode *)fp->f_fglob->fg_data, vap, a_context);
		}
		if (error == 0 && vap->va_type == VDIR) {
			/*
			 * directories can cause loops in the namespace,
			 * so turn off the 'x' bits to avoid trouble.
			 *
			 * XXX ACLs break this, of course
			 */
			vap->va_mode &= ~((VEXEC) | (VEXEC >> 3) | (VEXEC >> 6));
		}
		(void)vnode_put((struct vnode *) fp->f_fglob->fg_data);
		break;

	case DTYPE_SOCKET:
	case DTYPE_PIPE:
#if SOCKETS
		if (FILEGLOB_DTYPE(fp->f_fglob) == DTYPE_SOCKET) {
			error = soo_stat((struct socket *)fp->f_fglob->fg_data, (void *)&stb, 0);
		} else
#endif /* SOCKETS */
		error = pipe_stat((struct pipe *)fp->f_fglob->fg_data, (void *)&stb, 0);

		if (error == 0) {
			if (FILEGLOB_DTYPE(fp->f_fglob) == DTYPE_SOCKET) {
				VATTR_RETURN(vap, va_type, VSOCK);
			} else {
				VATTR_RETURN(vap, va_type, VFIFO);
			}

			VATTR_RETURN(vap, va_mode, stb.st_mode);
			VATTR_RETURN(vap, va_nlink, stb.st_nlink);
			VATTR_RETURN(vap, va_uid, stb.st_uid);
			VATTR_RETURN(vap, va_gid, stb.st_gid);
			VATTR_RETURN(vap, va_fsid, stb.st_dev);
			VATTR_RETURN(vap, va_fileid, stb.st_ino);
			VATTR_RETURN(vap, va_data_size, stb.st_size);
			VATTR_RETURN(vap, va_access_time, stb.st_atimespec);
			VATTR_RETURN(vap, va_modify_time, stb.st_mtimespec);
			VATTR_RETURN(vap, va_change_time, stb.st_ctimespec);
			VATTR_RETURN(vap, va_gen, stb.st_gen);
			VATTR_RETURN(vap, va_flags, stb.st_flags);
			VATTR_RETURN(vap, va_rdev, stb.st_rdev);
			VATTR_RETURN(vap, va_total_alloc, stb.st_blocks * stb.st_blksize);
			VATTR_RETURN(vap, va_acl, NULL);
		}
		break;

	default:
		error = EBADF;
	}

	fp_drop(p, fd, fp, 0);
	return error;
}

int
fdesc_getattr(struct vnop_getattr_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct vnode_attr *vap = ap->a_vap;
	unsigned fd;
	int error = 0;

	switch (VTOFDESC(vp)->fd_type) {
	case Fdesc:
		fd = VTOFDESC(vp)->fd_fd;
		error = fdesc_attr(fd, vap, ap->a_context);
		break;

	default:
		panic("Invalid type for an fdesc node!\n");
		break;
	}

	/*
	 * Yes, we do this without locking, but this value is always just
	 * a snapshot.
	 */
	if (error == 0) {
		vp->v_type = vap->va_type;

		/* We need an inactive to reset type to VNON */
		vnode_setneedinactive(vp);
	}

	return error;
}

int
fdesc_setattr(struct vnop_setattr_args *ap)
{
	struct fileproc *fp;
	unsigned fd;
	int error;
	struct proc * p = vfs_context_proc(ap->a_context);

	/*
	 * Can't mess with the root vnode
	 */
	switch (VTOFDESC(ap->a_vp)->fd_type) {
	case Fdesc:
		break;
	default:
		panic("Invalid type for an fdesc node!\n");
		return EACCES;
	}

	fd = VTOFDESC(ap->a_vp)->fd_fd;
	if ((error = fp_lookup(vfs_context_proc(ap->a_context), fd, &fp, 0))) {
		return error;
	}

	/*
	 * Can setattr the underlying vnode, but not sockets!
	 */
	switch (FILEGLOB_DTYPE(fp->f_fglob)) {
	case DTYPE_VNODE:
	{
		if ((error = vnode_getwithref((struct vnode *) fp->f_fglob->fg_data)) != 0) {
			break;
		}
		error = vnode_setattr((struct vnode *) fp->f_fglob->fg_data, ap->a_vap, ap->a_context);
		(void)vnode_put((struct vnode *) fp->f_fglob->fg_data);
		break;
	}

	case DTYPE_SOCKET:
	case DTYPE_PIPE:
		error = 0;
		break;

	default:
		error = EBADF;
		break;
	}

	fp_drop(p, fd, fp, 0);
	return error;
}

#define UIO_MX 16

/*
 *  static struct dirtmp {
 *       u_int32_t d_fileno;
 *       u_short d_reclen;
 *       u_short d_namlen;
 *       char d_name[8];
 *  } rootent[] = {
 *       { FD_DEVFD, UIO_MX, 2, "fd" },
 *       { FD_STDIN, UIO_MX, 5, "stdin" },
 *       { FD_STDOUT, UIO_MX, 6, "stdout" },
 *       { FD_STDERR, UIO_MX, 6, "stderr" },
 *       { 0, 0, 0, "" }
 *  };
 */

/* Only called on /dev/fd */
int
devfs_devfd_readdir(struct vnop_readdir_args *ap)
{
	struct uio *uio = ap->a_uio;
	struct proc *p = current_proc();
	int i, error;

	/*
	 * We don't allow exporting fdesc mounts, and currently local
	 * requests do not need cookies.
	 */
	if (ap->a_flags & (VNODE_READDIR_EXTENDED | VNODE_READDIR_REQSEEKOFF)) {
		return EINVAL;
	}

	/*
	 * There needs to be space for at least one entry.
	 */
	if (uio_resid(uio) < UIO_MX) {
		return EINVAL;
	}

	i = uio->uio_offset / UIO_MX;
	error = 0;
	while (uio_resid(uio) >= UIO_MX) {
		if (i >= p->p_fd->fd_nfiles) {
			break;
		}

		if (*fdfile(p, i) != NULL && !(*fdflags(p, i) & UF_RESERVED)) {
			struct dirent d;
			struct dirent *dp = &d;

			bzero((caddr_t) dp, UIO_MX);

			dp->d_namlen = scnprintf(dp->d_name, sizeof(dp->d_name),
			    "%d", i);
			dp->d_reclen = UIO_MX;
			dp->d_type = DT_UNKNOWN;
			dp->d_fileno = i + FD_STDIN;
			/*
			 * And ship to userland
			 */
			error = uiomove((caddr_t) dp, UIO_MX, uio);
			if (error) {
				break;
			}
		}
		i++;
	}

	uio->uio_offset = i * UIO_MX;
	return error;
}

int
fdesc_read(__unused struct vnop_read_args *ap)
{
	return ENOTSUP;
}

int
fdesc_write(__unused struct vnop_write_args *ap)
{
	return ENOTSUP;
}

int
fdesc_ioctl(__unused struct vnop_ioctl_args *ap)
{
	return ENOTSUP;
}

int
fdesc_select(__unused struct vnop_select_args *ap)
{
	return ENOTSUP;
}

int
fdesc_inactive(struct vnop_inactive_args *ap)
{
	struct vnode *vp = ap->a_vp;

	/*
	 * Clear out the v_type field to avoid
	 * nasty things happening in vgone().
	 */
	vp->v_type = VNON;

	return 0;
}

int
fdesc_reclaim(struct vnop_reclaim_args *ap)
{
	struct vnode *vp = ap->a_vp;
	struct fdescnode *fd = VTOFDESC(vp);

	fdesc_lock();

	LIST_REMOVE(fd, fd_hash);
	FREE(vp->v_data, M_TEMP);
	vp->v_data = NULL;

	fdesc_unlock();

	return 0;
}

/*
 * Return POSIX pathconf information applicable to special devices.
 */
int
fdesc_pathconf(struct vnop_pathconf_args *ap)
{
	switch (ap->a_name) {
	case _PC_LINK_MAX:
		*ap->a_retval = LINK_MAX;
		return 0;
	case _PC_MAX_CANON:
		*ap->a_retval = MAX_CANON;
		return 0;
	case _PC_MAX_INPUT:
		*ap->a_retval = MAX_INPUT;
		return 0;
	case _PC_PIPE_BUF:
		*ap->a_retval = PIPE_BUF;
		return 0;
	case _PC_CHOWN_RESTRICTED:
		*ap->a_retval = 200112;         /* _POSIX_CHOWN_RESTRICTED */
		return 0;
	case _PC_VDISABLE:
		*ap->a_retval = _POSIX_VDISABLE;
		return 0;
	default:
		return EINVAL;
	}
	/* NOTREACHED */
}

/*
 * /dev/fd "should never get here" operation
 */
int
fdesc_badop(void)
{
	return ENOTSUP;
	/* NOTREACHED */
}

#define VOPFUNC int (*)(void *)

#define fdesc_create (int (*) (struct  vnop_create_args *))eopnotsupp
#define fdesc_mknod (int (*) (struct  vnop_mknod_args *))eopnotsupp
#define fdesc_close (int (*) (struct  vnop_close_args *))nullop
#define fdesc_access (int (*) (struct  vnop_access_args *))nullop
#define fdesc_mmap (int (*) (struct  vnop_mmap_args *))eopnotsupp
#define fdesc_revoke nop_revoke
#define fdesc_fsync (int (*) (struct  vnop_fsync_args *))nullop
#define fdesc_remove (int (*) (struct  vnop_remove_args *))eopnotsupp
#define fdesc_link (int (*) (struct  vnop_link_args *))eopnotsupp
#define fdesc_rename (int (*) (struct  vnop_rename_args *))eopnotsupp
#define fdesc_mkdir (int (*) (struct  vnop_mkdir_args *))eopnotsupp
#define fdesc_rmdir (int (*) (struct  vnop_rmdir_args *))eopnotsupp
#define fdesc_symlink (int (*) (struct vnop_symlink_args *))eopnotsupp
#define fdesc_strategy (int (*) (struct  vnop_strategy_args *))fdesc_badop
#define fdesc_advlock (int (*) (struct vnop_advlock_args *))eopnotsupp
#define fdesc_bwrite (int (*) (struct  vnop_bwrite_args *))eopnotsupp
#define fdesc_blktooff (int (*) (struct  vnop_blktooff_args *))eopnotsupp
#define fdesc_offtoblk (int (*) (struct  vnop_offtoblk_args *))eopnotsupp
#define fdesc_blockmap (int (*) (struct  vnop_blockmap_args *))eopnotsupp

int(**fdesc_vnodeop_p)(void *);
const struct vnodeopv_entry_desc devfs_fdesc_vnodeop_entries[] = {
	{ .opve_op = &vnop_default_desc, .opve_impl = (VOPFUNC)vn_default_error },
	{ .opve_op = &vnop_lookup_desc, .opve_impl = (VOPFUNC)vn_default_error},        /* lookup */
	{ .opve_op = &vnop_create_desc, .opve_impl = (VOPFUNC)fdesc_create },   /* create */
	{ .opve_op = &vnop_mknod_desc, .opve_impl = (VOPFUNC)fdesc_mknod },     /* mknod */
	{ .opve_op = &vnop_open_desc, .opve_impl = (VOPFUNC)fdesc_open },       /* open */
	{ .opve_op = &vnop_close_desc, .opve_impl = (VOPFUNC)fdesc_close },     /* close */
	{ .opve_op = &vnop_access_desc, .opve_impl = (VOPFUNC)fdesc_access },   /* access */
	{ .opve_op = &vnop_getattr_desc, .opve_impl = (VOPFUNC)fdesc_getattr }, /* getattr */
	{ .opve_op = &vnop_setattr_desc, .opve_impl = (VOPFUNC)fdesc_setattr }, /* setattr */
	{ .opve_op = &vnop_read_desc, .opve_impl = (VOPFUNC)fdesc_read },       /* read */
	{ .opve_op = &vnop_write_desc, .opve_impl = (VOPFUNC)fdesc_write },     /* write */
	{ .opve_op = &vnop_ioctl_desc, .opve_impl = (VOPFUNC)fdesc_ioctl },     /* ioctl */
	{ .opve_op = &vnop_select_desc, .opve_impl = (VOPFUNC)fdesc_select },   /* select */
	{ .opve_op = &vnop_revoke_desc, .opve_impl = (VOPFUNC)fdesc_revoke },   /* revoke */
	{ .opve_op = &vnop_mmap_desc, .opve_impl = (VOPFUNC)fdesc_mmap },       /* mmap */
	{ .opve_op = &vnop_fsync_desc, .opve_impl = (VOPFUNC)fdesc_fsync },     /* fsync */
	{ .opve_op = &vnop_remove_desc, .opve_impl = (VOPFUNC)fdesc_remove },   /* remove */
	{ .opve_op = &vnop_link_desc, .opve_impl = (VOPFUNC)fdesc_link },       /* link */
	{ .opve_op = &vnop_rename_desc, .opve_impl =  (VOPFUNC)fdesc_rename },   /* rename */
	{ .opve_op = &vnop_mkdir_desc, .opve_impl = (VOPFUNC)fdesc_mkdir },     /* mkdir */
	{ .opve_op = &vnop_rmdir_desc, .opve_impl = (VOPFUNC)fdesc_rmdir },     /* rmdir */
	{ .opve_op = &vnop_symlink_desc, .opve_impl = (VOPFUNC)fdesc_symlink }, /* symlink */
	{ .opve_op = &vnop_readdir_desc, .opve_impl = (VOPFUNC)vn_default_error},/* readdir */
	{ .opve_op = &vnop_readlink_desc, .opve_impl = (VOPFUNC)err_readlink}, /* readlink */
	{ .opve_op = &vnop_inactive_desc, .opve_impl = (VOPFUNC)fdesc_inactive },/* inactive */
	{ .opve_op = &vnop_reclaim_desc, .opve_impl = (VOPFUNC)fdesc_reclaim }, /* reclaim */
	{ .opve_op = &vnop_strategy_desc, .opve_impl = (VOPFUNC)fdesc_strategy },       /* strategy */
	{ .opve_op = &vnop_pathconf_desc, .opve_impl = (VOPFUNC)fdesc_pathconf },       /* pathconf */
	{ .opve_op = &vnop_advlock_desc, .opve_impl = (VOPFUNC)fdesc_advlock }, /* advlock */
	{ .opve_op = &vnop_bwrite_desc, .opve_impl = (VOPFUNC)fdesc_bwrite },   /* bwrite */
	{ .opve_op = &vnop_pagein_desc, .opve_impl = (VOPFUNC)err_pagein },     /* pagein */
	{ .opve_op = &vnop_pageout_desc, .opve_impl = (VOPFUNC)err_pageout },   /* pageout */
	{ .opve_op = &vnop_copyfile_desc, .opve_impl = (VOPFUNC)err_copyfile }, /* Copyfile */
	{ .opve_op = &vnop_blktooff_desc, .opve_impl = (VOPFUNC)fdesc_blktooff },       /* blktooff */
	{ .opve_op = &vnop_blktooff_desc, .opve_impl = (VOPFUNC)fdesc_offtoblk },       /* offtoblk */
	{ .opve_op = &vnop_blockmap_desc, .opve_impl = (VOPFUNC)fdesc_blockmap },       /* blockmap */
	{ .opve_op = (struct vnodeop_desc*)NULL, .opve_impl = (VOPFUNC)NULL }
};

const struct vnodeopv_desc devfs_fdesc_vnodeop_opv_desc =
{ .opv_desc_vector_p = &fdesc_vnodeop_p, .opv_desc_ops = devfs_fdesc_vnodeop_entries };
