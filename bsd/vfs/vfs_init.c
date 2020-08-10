/*
 * Copyright (c) 2000-2010 Apple Inc. All rights reserved.
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
 * Copyright (c) 1989, 1993
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed
 * to Berkeley by John Heidemann of the UCLA Ficus project.
 *
 * Source: * @(#)i405_init.c 2.10 92/04/27 UCLA Ficus project
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
 *	@(#)vfs_init.c	8.5 (Berkeley) 5/11/95
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */


#include <sys/param.h>
#include <sys/mount_internal.h>
#include <sys/time.h>
#include <sys/vm.h>
#include <sys/vnode_internal.h>
#include <sys/stat.h>
#include <sys/namei.h>
#include <sys/ucred.h>
#include <sys/errno.h>
#include <sys/malloc.h>
#include <sys/decmpfs.h>

#if CONFIG_MACF
#include <security/mac_framework.h>
#include <sys/kauth.h>
#endif
#if QUOTA
#include <sys/quota.h>
#endif

/*
 * Sigh, such primitive tools are these...
 */
#if 0
#define DODEBUG(A) A
#else
#define DODEBUG(A)
#endif

__private_extern__ void vntblinit(void);

extern const struct vnodeopv_desc *vfs_opv_descs[];
/* a list of lists of vnodeops defns */
extern struct vnodeop_desc *vfs_op_descs[];
/* and the operations they perform */
/*
 * This code doesn't work if the defn is **vnodop_defns with cc.
 * The problem is because of the compiler sometimes putting in an
 * extra level of indirection for arrays.  It's an interesting
 * "feature" of C.
 */
int vfs_opv_numops;

typedef int (*PFIvp)(void *);

/*
 * A miscellaneous routine.
 * A generic "default" routine that just returns an error.
 */
int
vn_default_error(void)
{
	return ENOTSUP;
}

/*
 * vfs_init.c
 *
 * Allocate and fill in operations vectors.
 *
 * An undocumented feature of this approach to defining operations is that
 * there can be multiple entries in vfs_opv_descs for the same operations
 * vector. This allows third parties to extend the set of operations
 * supported by another layer in a binary compatibile way. For example,
 * assume that NFS needed to be modified to support Ficus. NFS has an entry
 * (probably nfs_vnopdeop_decls) declaring all the operations NFS supports by
 * default. Ficus could add another entry (ficus_nfs_vnodeop_decl_entensions)
 * listing those new operations Ficus adds to NFS, all without modifying the
 * NFS code. (Of couse, the OTW NFS protocol still needs to be munged, but
 * that is a(whole)nother story.) This is a feature.
 */
void
vfs_opv_init(void)
{
	int i, j, k;
	int(***opv_desc_vector_p)(void *);
	int(**opv_desc_vector)(void *);
	const struct vnodeopv_entry_desc *opve_descp;

	/*
	 * Allocate the dynamic vectors and fill them in.
	 */
	for (i = 0; vfs_opv_descs[i]; i++) {
		opv_desc_vector_p = vfs_opv_descs[i]->opv_desc_vector_p;
		/*
		 * Allocate and init the vector, if it needs it.
		 * Also handle backwards compatibility.
		 */
		if (*opv_desc_vector_p == NULL) {
			MALLOC(*opv_desc_vector_p, PFIvp*,
			    vfs_opv_numops * sizeof(PFIvp), M_TEMP, M_WAITOK);
			bzero(*opv_desc_vector_p, vfs_opv_numops * sizeof(PFIvp));
			DODEBUG(printf("vector at %x allocated\n",
			    opv_desc_vector_p));
		}
		opv_desc_vector = *opv_desc_vector_p;
		for (j = 0; vfs_opv_descs[i]->opv_desc_ops[j].opve_op; j++) {
			opve_descp = &(vfs_opv_descs[i]->opv_desc_ops[j]);

			/* Silently skip known-disabled operations */
			if (opve_descp->opve_op->vdesc_flags & VDESC_DISABLED) {
				printf("vfs_fsadd: Ignoring reference in %p to disabled operation %s.\n",
				    vfs_opv_descs[i], opve_descp->opve_op->vdesc_name);
				continue;
			}

			/*
			 * Sanity check:  is this operation listed
			 * in the list of operations?  We check this
			 * by seeing if its offest is zero.  Since
			 * the default routine should always be listed
			 * first, it should be the only one with a zero
			 * offset.  Any other operation with a zero
			 * offset is probably not listed in
			 * vfs_op_descs, and so is probably an error.
			 *
			 * A panic here means the layer programmer
			 * has committed the all-too common bug
			 * of adding a new operation to the layer's
			 * list of vnode operations but
			 * not adding the operation to the system-wide
			 * list of supported operations.
			 */
			if (opve_descp->opve_op->vdesc_offset == 0 &&
			    opve_descp->opve_op !=
			    VDESC(vnop_default)) {
				printf("operation %s not listed in %s.\n",
				    opve_descp->opve_op->vdesc_name,
				    "vfs_op_descs");
				panic("vfs_opv_init: bad operation");
			}
			/*
			 * Fill in this entry.
			 */
			opv_desc_vector[opve_descp->opve_op->vdesc_offset] =
			    opve_descp->opve_impl;
		}
	}
	/*
	 * Finally, go back and replace unfilled routines
	 * with their default.  (Sigh, an O(n^3) algorithm.  I
	 * could make it better, but that'd be work, and n is small.)
	 */
	for (i = 0; vfs_opv_descs[i]; i++) {
		opv_desc_vector = *(vfs_opv_descs[i]->opv_desc_vector_p);
		/*
		 * Force every operations vector to have a default routine.
		 */
		if (opv_desc_vector[VOFFSET(vnop_default)] == NULL) {
			panic("vfs_opv_init: operation vector without default routine.");
		}
		for (k = 0; k < vfs_opv_numops; k++) {
			if (opv_desc_vector[k] == NULL) {
				opv_desc_vector[k] =
				    opv_desc_vector[VOFFSET(vnop_default)];
			}
		}
	}
}

/*
 * Initialize known vnode operations vectors.
 */
void
vfs_op_init(void)
{
	int i;

	DODEBUG(printf("Vnode_interface_init.\n"));
	/*
	 * Set all vnode vectors to a well known value.
	 */
	for (i = 0; vfs_opv_descs[i]; i++) {
		*(vfs_opv_descs[i]->opv_desc_vector_p) = NULL;
	}
	/*
	 * Figure out how many ops there are by counting the table,
	 * and assign each its offset.
	 */
	for (vfs_opv_numops = 0, i = 0; vfs_op_descs[i]; i++) {
		/* Silently skip known-disabled operations */
		if (vfs_op_descs[i]->vdesc_flags & VDESC_DISABLED) {
			continue;
		}
		vfs_op_descs[i]->vdesc_offset = vfs_opv_numops;
		vfs_opv_numops++;
	}
	DODEBUG(printf("vfs_opv_numops=%d\n", vfs_opv_numops));
}

/*
 * Routines having to do with the management of the vnode table.
 */
extern struct vnodeops dead_vnodeops;
extern struct vnodeops spec_vnodeops;

/* vars for vnode lock */
lck_grp_t * vnode_lck_grp;
lck_grp_attr_t * vnode_lck_grp_attr;
lck_attr_t * vnode_lck_attr;

#if CONFIG_TRIGGERS
/* vars for vnode trigger resolver */
lck_grp_t * trigger_vnode_lck_grp;
lck_grp_attr_t * trigger_vnode_lck_grp_attr;
lck_attr_t * trigger_vnode_lck_attr;
#endif

lck_grp_t * fd_vn_lck_grp;
lck_grp_attr_t * fd_vn_lck_grp_attr;
lck_attr_t * fd_vn_lck_attr;

/* vars for vnode list lock */
lck_grp_t * vnode_list_lck_grp;
lck_grp_attr_t * vnode_list_lck_grp_attr;
lck_attr_t * vnode_list_lck_attr;
lck_spin_t * vnode_list_spin_lock;
lck_mtx_t * spechash_mtx_lock;

/* vars for vfsconf lock */
lck_grp_t * fsconf_lck_grp;
lck_grp_attr_t * fsconf_lck_grp_attr;
lck_attr_t * fsconf_lck_attr;


/* vars for mount lock */
lck_grp_t * mnt_lck_grp;
lck_grp_attr_t * mnt_lck_grp_attr;
lck_attr_t * mnt_lck_attr;

/* vars for mount list lock */
lck_grp_t * mnt_list_lck_grp;
lck_grp_attr_t * mnt_list_lck_grp_attr;
lck_attr_t * mnt_list_lck_attr;
lck_mtx_t * mnt_list_mtx_lock;

/* vars for sync mutex */
lck_grp_t * sync_mtx_lck_grp;
lck_grp_attr_t * sync_mtx_lck_grp_attr;
lck_attr_t * sync_mtx_lck_attr;
lck_mtx_t * sync_mtx_lck;

lck_mtx_t *pkg_extensions_lck;

struct mount * dead_mountp;

/*
 * Initialize the vnode structures and initialize each file system type.
 */
void
vfsinit(void)
{
	struct vfstable *vfsp;
	int i, maxtypenum;
	struct mount * mp;

	/* Allocate vnode list lock group attribute and group */
	vnode_list_lck_grp_attr = lck_grp_attr_alloc_init();

	vnode_list_lck_grp = lck_grp_alloc_init("vnode list", vnode_list_lck_grp_attr);

	/* Allocate vnode list lock attribute */
	vnode_list_lck_attr = lck_attr_alloc_init();

	/* Allocate vnode list lock */
	vnode_list_spin_lock = lck_spin_alloc_init(vnode_list_lck_grp, vnode_list_lck_attr);

	/* Allocate spec hash list lock */
	spechash_mtx_lock = lck_mtx_alloc_init(vnode_list_lck_grp, vnode_list_lck_attr);

	/* Allocate the package extensions table lock */
	pkg_extensions_lck = lck_mtx_alloc_init(vnode_list_lck_grp, vnode_list_lck_attr);

	/* allocate vnode lock group attribute and group */
	vnode_lck_grp_attr = lck_grp_attr_alloc_init();

	vnode_lck_grp = lck_grp_alloc_init("vnode", vnode_lck_grp_attr);

	/* Allocate vnode lock attribute */
	vnode_lck_attr = lck_attr_alloc_init();

#if CONFIG_TRIGGERS
	trigger_vnode_lck_grp_attr = lck_grp_attr_alloc_init();
	trigger_vnode_lck_grp = lck_grp_alloc_init("trigger_vnode", trigger_vnode_lck_grp_attr);
	trigger_vnode_lck_attr = lck_attr_alloc_init();
#endif
	/* Allocate per fd vnode data lock attribute and group */
	fd_vn_lck_grp_attr = lck_grp_attr_alloc_init();
	fd_vn_lck_grp = lck_grp_alloc_init("fd_vnode_data", fd_vn_lck_grp_attr);
	fd_vn_lck_attr = lck_attr_alloc_init();

	/* Allocate fs config lock group attribute and group */
	fsconf_lck_grp_attr = lck_grp_attr_alloc_init();

	fsconf_lck_grp = lck_grp_alloc_init("fs conf", fsconf_lck_grp_attr);

	/* Allocate fs config lock attribute */
	fsconf_lck_attr = lck_attr_alloc_init();

	/* Allocate mount point related lock structures  */

	/* Allocate mount list lock group attribute and group */
	mnt_list_lck_grp_attr = lck_grp_attr_alloc_init();

	mnt_list_lck_grp = lck_grp_alloc_init("mount list", mnt_list_lck_grp_attr);

	/* Allocate mount list lock attribute */
	mnt_list_lck_attr = lck_attr_alloc_init();

	/* Allocate mount list lock */
	mnt_list_mtx_lock = lck_mtx_alloc_init(mnt_list_lck_grp, mnt_list_lck_attr);


	/* allocate mount lock group attribute and group */
	mnt_lck_grp_attr = lck_grp_attr_alloc_init();

	mnt_lck_grp = lck_grp_alloc_init("mount", mnt_lck_grp_attr);

	/* Allocate mount lock attribute */
	mnt_lck_attr = lck_attr_alloc_init();

	/* Allocate sync lock */
	sync_mtx_lck_grp_attr =  lck_grp_attr_alloc_init();
	sync_mtx_lck_grp =       lck_grp_alloc_init("sync thread", sync_mtx_lck_grp_attr);
	sync_mtx_lck_attr =      lck_attr_alloc_init();
	sync_mtx_lck =           lck_mtx_alloc_init(sync_mtx_lck_grp, sync_mtx_lck_attr);

	/*
	 * Initialize the vnode table
	 */
	vntblinit();
	/*
	 * Initialize the filesystem event mechanism.
	 */
	vfs_event_init();
	/*
	 * Initialize the vnode name cache
	 */
	nchinit();

	/*
	 * Build vnode operation vectors.
	 */
	vfs_op_init();
	vfs_opv_init();   /* finish the job */
	/*
	 * Initialize each file system type in the static list,
	 * until the first NULL ->vfs_vfsops is encountered.
	 */
	maxtypenum = VT_NON;
	for (vfsp = vfsconf, i = 0; i < maxvfsslots; i++, vfsp++) {
		struct vfsconf vfsc;
		if (vfsp->vfc_vfsops == (struct vfsops *)0) {
			break;
		}
		if (i) {
			vfsconf[i - 1].vfc_next = vfsp;
		}
		if (maxtypenum <= vfsp->vfc_typenum) {
			maxtypenum = vfsp->vfc_typenum + 1;
		}

		bzero(&vfsc, sizeof(struct vfsconf));
		vfsc.vfc_reserved1 = 0;
		bcopy(vfsp->vfc_name, vfsc.vfc_name, sizeof(vfsc.vfc_name));
		vfsc.vfc_typenum = vfsp->vfc_typenum;
		vfsc.vfc_refcount = vfsp->vfc_refcount;
		vfsc.vfc_flags = vfsp->vfc_flags;
		vfsc.vfc_reserved2 = 0;
		vfsc.vfc_reserved3 = 0;

		if (vfsp->vfc_vfsops->vfs_sysctl) {
			struct sysctl_oid *oidp = NULL;
			struct sysctl_oid oid = SYSCTL_STRUCT_INIT(_vfs, vfsp->vfc_typenum, , CTLTYPE_NODE | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, vfs_sysctl_node, "-", "");

			MALLOC(oidp, struct sysctl_oid *, sizeof(struct sysctl_oid), M_TEMP, M_WAITOK);
			*oidp = oid;

			/* Memory for VFS oid held by vfsentry forever */
			vfsp->vfc_sysctl = oidp;
			oidp->oid_name = vfsp->vfc_name;
			sysctl_register_oid(vfsp->vfc_sysctl);
		}

		(*vfsp->vfc_vfsops->vfs_init)(&vfsc);

		numused_vfsslots++;
		numregistered_fses++;
	}
	/* next vfc_typenum to be used */
	maxvfstypenum = maxtypenum;

	/*
	 * Initialize the vnop authorization scope.
	 */
	vnode_authorize_init();

	/*
	 * Initialiize the quota system.
	 */
#if QUOTA
	dqinit();
#endif

	/*
	 * create a mount point for dead vnodes
	 */
	MALLOC_ZONE(mp, struct mount *, sizeof(struct mount),
	    M_MOUNT, M_WAITOK);
	bzero((char *)mp, sizeof(struct mount));
	/* Initialize the default IO constraints */
	mp->mnt_maxreadcnt = mp->mnt_maxwritecnt = MAXPHYS;
	mp->mnt_segreadcnt = mp->mnt_segwritecnt = 32;
	mp->mnt_maxsegreadsize = mp->mnt_maxreadcnt;
	mp->mnt_maxsegwritesize = mp->mnt_maxwritecnt;
	mp->mnt_devblocksize = DEV_BSIZE;
	mp->mnt_alignmentmask = PAGE_MASK;
	mp->mnt_ioqueue_depth = MNT_DEFAULT_IOQUEUE_DEPTH;
	mp->mnt_ioscale = 1;
	mp->mnt_ioflags = 0;
	mp->mnt_realrootvp = NULLVP;
	mp->mnt_authcache_ttl = CACHED_LOOKUP_RIGHT_TTL;

	TAILQ_INIT(&mp->mnt_vnodelist);
	TAILQ_INIT(&mp->mnt_workerqueue);
	TAILQ_INIT(&mp->mnt_newvnodes);
	mp->mnt_flag = MNT_LOCAL;
	mp->mnt_lflag = MNT_LDEAD;
	mount_lock_init(mp);

#if CONFIG_MACF
	mac_mount_label_init(mp);
	mac_mount_label_associate(vfs_context_kernel(), mp);
#endif
	dead_mountp = mp;

#if FS_COMPRESSION
	decmpfs_init();
#endif

	nspace_resolver_init();
}

void
vnode_list_lock(void)
{
	lck_spin_lock_grp(vnode_list_spin_lock, vnode_list_lck_grp);
}

void
vnode_list_unlock(void)
{
	lck_spin_unlock(vnode_list_spin_lock);
}

void
mount_list_lock(void)
{
	lck_mtx_lock(mnt_list_mtx_lock);
}

void
mount_list_unlock(void)
{
	lck_mtx_unlock(mnt_list_mtx_lock);
}

void
mount_lock_init(mount_t mp)
{
	lck_mtx_init(&mp->mnt_mlock, mnt_lck_grp, mnt_lck_attr);
	lck_mtx_init(&mp->mnt_iter_lock, mnt_lck_grp, mnt_lck_attr);
	lck_mtx_init(&mp->mnt_renamelock, mnt_lck_grp, mnt_lck_attr);
	lck_rw_init(&mp->mnt_rwlock, mnt_lck_grp, mnt_lck_attr);
}

void
mount_lock_destroy(mount_t mp)
{
	lck_mtx_destroy(&mp->mnt_mlock, mnt_lck_grp);
	lck_mtx_destroy(&mp->mnt_iter_lock, mnt_lck_grp);
	lck_mtx_destroy(&mp->mnt_renamelock, mnt_lck_grp);
	lck_rw_destroy(&mp->mnt_rwlock, mnt_lck_grp);
}


/*
 * Name:	vfstable_add
 *
 * Description:	Add a filesystem to the vfsconf list at the first
 *		unused slot.  If no slots are available, return an
 *		error.
 *
 * Parameter:	nvfsp		vfsconf for VFS to add
 *
 * Returns:	0		Success
 *		-1		Failure
 *
 * Notes:	The vfsconf should be treated as a linked list by
 *		all external references, as the implementation is
 *		expected to change in the future.  The linkage is
 *		through ->vfc_next, and the list is NULL terminated.
 *
 * Warning:	This code assumes that vfsconf[0] is non-empty.
 */
struct vfstable *
vfstable_add(struct vfstable  *nvfsp)
{
	int slot;
	struct vfstable *slotp, *allocated = NULL;
	struct sysctl_oid *oidp = NULL;


	if (nvfsp->vfc_vfsops->vfs_sysctl) {
		struct sysctl_oid oid = SYSCTL_STRUCT_INIT(_vfs, nvfsp->vfc_typenum, , CTLTYPE_NODE | CTLFLAG_KERN | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, vfs_sysctl_node, "-", "");

		MALLOC(oidp, struct sysctl_oid *, sizeof(struct sysctl_oid), M_TEMP, M_WAITOK);
		*oidp = oid;
	}

	/*
	 * Find the next empty slot; we recognize an empty slot by a
	 * NULL-valued ->vfc_vfsops, so if we delete a VFS, we must
	 * ensure we set the entry back to NULL.
	 */
findslot:
	mount_list_lock();
	for (slot = 0; slot < maxvfsslots; slot++) {
		if (vfsconf[slot].vfc_vfsops == NULL) {
			break;
		}
	}
	if (slot == maxvfsslots) {
		if (allocated == NULL) {
			mount_list_unlock();
			/* out of static slots; allocate one instead */
			MALLOC(allocated, struct vfstable *, sizeof(struct vfstable),
			    M_TEMP, M_WAITOK);
			goto findslot;
		} else {
			slotp = allocated;
		}
	} else {
		slotp = &vfsconf[slot];
	}

	/*
	 * Replace the contents of the next empty slot with the contents
	 * of the provided nvfsp.
	 *
	 * Note; Takes advantage of the fact that 'slot' was left
	 * with the value of 'maxvfslots' in the allocation case.
	 */
	bcopy(nvfsp, slotp, sizeof(struct vfstable));
	if (slot != 0) {
		slotp->vfc_next = vfsconf[slot - 1].vfc_next;
		vfsconf[slot - 1].vfc_next = slotp;
	} else {
		slotp->vfc_next = NULL;
	}

	if (slotp != allocated) {
		/* used a statically allocated slot */
		numused_vfsslots++;
	}
	numregistered_fses++;

	if (oidp) {
		/* Memory freed in vfstable_del after unregistration */
		slotp->vfc_sysctl = oidp;
		oidp->oid_name = slotp->vfc_name;
		sysctl_register_oid(slotp->vfc_sysctl);
	}

	mount_list_unlock();

	if (allocated && allocated != slotp) {
		/* did allocation, but ended up using static slot */
		FREE(allocated, M_TEMP);
	}

	return slotp;
}

/*
 * Name:	vfstable_del
 *
 * Description:	Remove a filesystem from the vfsconf list by name.
 *		If no such filesystem exists, return an error.
 *
 * Parameter:	fs_name		name of VFS to remove
 *
 * Returns:	0		Success
 *		-1		Failure
 *
 * Notes:	Hopefully all filesystems have unique names.
 */
int
vfstable_del(struct vfstable  * vtbl)
{
	struct vfstable **vcpp;
	struct vfstable *vcdelp;

#if DEBUG
	lck_mtx_assert(mnt_list_mtx_lock, LCK_MTX_ASSERT_OWNED);
#endif /* DEBUG */

	/*
	 * Traverse the list looking for vtbl; if found, *vcpp
	 * will contain the address of the pointer to the entry to
	 * be removed.
	 */
	for (vcpp = &vfsconf; *vcpp; vcpp = &(*vcpp)->vfc_next) {
		if (*vcpp == vtbl) {
			break;
		}
	}

	if (*vcpp == NULL) {
		return ESRCH; /* vtbl not on vfsconf list */
	}
	if ((*vcpp)->vfc_sysctl) {
		sysctl_unregister_oid((*vcpp)->vfc_sysctl);
		(*vcpp)->vfc_sysctl->oid_name = NULL;
		FREE((*vcpp)->vfc_sysctl, M_TEMP);
		(*vcpp)->vfc_sysctl = NULL;
	}

	/* Unlink entry */
	vcdelp = *vcpp;
	*vcpp = (*vcpp)->vfc_next;

	/*
	 * Is this an entry from our static table?  We find out by
	 * seeing if the pointer to the object to be deleted places
	 * the object in the address space containing the table (or not).
	 */
	if (vcdelp >= vfsconf && vcdelp < (vfsconf + maxvfsslots)) {    /* Y */
		/* Mark as empty for vfscon_add() */
		bzero(vcdelp, sizeof(struct vfstable));
		numregistered_fses--;
		numused_vfsslots--;
	} else {                                                        /* N */
		/*
		 * This entry was dynamically allocated; we must free it;
		 * we would prefer to have just linked the caller's
		 * vfsconf onto our list, but it may not be persistent
		 * because of the previous (copying) implementation.
		 */
		numregistered_fses--;
		mount_list_unlock();
		FREE(vcdelp, M_TEMP);
		mount_list_lock();
	}

#if DEBUG
	lck_mtx_assert(mnt_list_mtx_lock, LCK_MTX_ASSERT_OWNED);
#endif /* DEBUG */

	return 0;
}

void
SPECHASH_LOCK(void)
{
	lck_mtx_lock(spechash_mtx_lock);
}

void
SPECHASH_UNLOCK(void)
{
	lck_mtx_unlock(spechash_mtx_lock);
}
