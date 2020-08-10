/*
 * Copyright (c) 2007 Apple Inc. All rights reserved.
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
#include <sys/param.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/kauth.h>
#include <sys/namei.h>
#include <sys/mount.h>
#include <sys/mount_internal.h>
#include <sys/uio_internal.h>
#include <sys/xattr.h>
#include "../bsd/sys/fsevents.h"

#include <security/mac_internal.h>

/*
 * Caller holds I/O reference on vnode
 */
int
vnode_label(struct mount *mp, struct vnode *dvp, struct vnode *vp,
    struct componentname *cnp, int flags, vfs_context_t ctx)
{
	int error = 0;
	bool exit_fast;

	/* fast path checks... */

	/* are we labeling vnodes? If not still notify of create */
#if CONFIG_MACF_LAZY_VNODE_LABELS
	exit_fast = true;
#else
	exit_fast = (mac_label_vnodes == 0);
#endif
	if (exit_fast) {
		if (flags & VNODE_LABEL_CREATE) {
			error = mac_vnode_notify_create(ctx,
			    mp, dvp, vp, cnp);
		}
		return 0;
	}

	/* if already VL_LABELED */
	if (vp->v_lflag & VL_LABELED) {
		return 0;
	}

	vnode_lock_spin(vp);

	/*
	 * must revalidate state once we hold the lock
	 * since we could have blocked and someone else
	 * has since labeled this vnode
	 */
	if (vp->v_lflag & VL_LABELED) {
		vnode_unlock(vp);
		return 0;
	}

	if ((vp->v_lflag & VL_LABEL) == 0) {
		vp->v_lflag |= VL_LABEL;

		/* Could sleep on disk I/O, drop lock. */
		vnode_unlock(vp);

		if (vp->v_label == NULL) {
			vp->v_label = mac_vnode_label_alloc();
		}

		if (flags & VNODE_LABEL_CREATE) {
			error = mac_vnode_notify_create(ctx,
			    mp, dvp, vp, cnp);
		} else {
			error = mac_vnode_label_associate(mp, vp, ctx);
		}

		vnode_lock_spin(vp);

		if ((error == 0) && (vp->v_flag & VNCACHEABLE)) {
			vp->v_lflag |= VL_LABELED;
		}
		vp->v_lflag &= ~VL_LABEL;

		if (vp->v_lflag & VL_LABELWAIT) {
			vp->v_lflag &= ~VL_LABELWAIT;
			wakeup(&vp->v_label);
		}
	} else {
		struct timespec ts;

		ts.tv_sec = 10;
		ts.tv_nsec = 0;

		while (vp->v_lflag & VL_LABEL) {
			vp->v_lflag |= VL_LABELWAIT;

			error = msleep(&vp->v_label, &vp->v_lock, PVFS | PDROP,
			    "vnode_label", &ts);
			vnode_lock_spin(vp);

			if (error == EWOULDBLOCK) {
				vprint("vnode label timeout", vp);
				break;
			}
		}
		/* XXX: what should be done if labeling failed (above)? */
	}
	vnode_unlock(vp);

	return error;
}


/*
 * Clear the "labeled" flag on a VNODE.
 * VNODE will have label re-associated upon
 * next call to lookup().
 *
 * Caller verifies vfs_flags(vnode_mount(vp)) & MNT_MULTILABEL
 * Caller holds vnode lock.
 */
void
vnode_relabel(struct vnode *vp)
{
	/* Wait for any other labeling to complete. */
	while (vp->v_lflag & VL_LABEL) {
		vp->v_lflag |= VL_LABELWAIT;
		(void)msleep(&vp->v_label, &vp->v_lock, PVFS, "vnode_relabel", 0);
	}

	/* Clear labeled flag */
	vp->v_lflag &= ~VL_LABELED;

	return;
}

/*
 * VFS XATTR helpers.
 */

int
mac_vnop_setxattr(struct vnode *vp, const char *name, char *buf, size_t len)
{
	vfs_context_t ctx;
	int options = XATTR_NOSECURITY;
	char uio_buf[UIO_SIZEOF(1)];
	uio_t auio;
	int error;

	if (vfs_isrdonly(vp->v_mount)) {
		return EROFS;
	}

	ctx = vfs_context_current();
	auio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_WRITE,
	    &uio_buf[0], sizeof(uio_buf));
	uio_addiov(auio, CAST_USER_ADDR_T(buf), len);

	error = vn_setxattr(vp, name, auio, options, ctx);
#if CONFIG_FSE
	if (error == 0) {
		add_fsevent(FSE_XATTR_MODIFIED, ctx,
		    FSE_ARG_VNODE, vp,
		    FSE_ARG_DONE);
	}
#endif

	return error;
}

int
mac_vnop_getxattr(struct vnode *vp, const char *name, char *buf, size_t len,
    size_t *attrlen)
{
	vfs_context_t ctx = vfs_context_current();
	int options = XATTR_NOSECURITY;
	char uio_buf[UIO_SIZEOF(1)];
	uio_t auio;
	int error;

	auio = uio_createwithbuffer(1, 0, UIO_SYSSPACE, UIO_READ,
	    &uio_buf[0], sizeof(uio_buf));
	uio_addiov(auio, CAST_USER_ADDR_T(buf), len);

	error = vn_getxattr(vp, name, auio, attrlen, options, ctx);
	*attrlen = len - uio_resid(auio);

	return error;
}

int
mac_vnop_removexattr(struct vnode *vp, const char *name)
{
	vfs_context_t ctx = vfs_context_current();
	int options = XATTR_NOSECURITY;
	int error;

	if (vfs_isrdonly(vp->v_mount)) {
		return EROFS;
	}

	error = vn_removexattr(vp, name, options, ctx);
#if CONFIG_FSE
	if (error == 0) {
		add_fsevent(FSE_XATTR_REMOVED, ctx,
		    FSE_ARG_VNODE, vp,
		    FSE_ARG_DONE);
	}
#endif

	return error;
}
