/*
 * Copyright (c) 2000-2014 Apple Inc. All rights reserved.
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
 * Copyright 1997,1998 Julian Elischer.  All rights reserved.
 * julian@freebsd.org
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *  1. Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 *  2. Redistributions in binary form must reproduce the above copyright notice,
 *     this list of conditions and the following disclaimer in the documentation
 *     and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDER ``AS IS'' AND ANY EXPRESS
 * OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED.  IN NO EVENT SHALL THE HOLDER OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 * devfs_tree.c
 */
/*
 * NOTICE: This file was modified by SPARTA, Inc. in 2005 to introduce
 * support for mandatory and extensible security protections.  This notice
 * is included in support of clause 2.2 (b) of the Apple Public License,
 * Version 2.0.
 */

/*
 * HISTORY
 *  Dieter Siegmund (dieter@apple.com) Thu Apr  8 14:08:19 PDT 1999
 *  - removed mounting of "hidden" mountpoint
 *  - fixed problem in which devnode->dn_vn pointer was not
 *    updated with the vnode returned from checkalias()
 *  - replaced devfs_vntodn() with a macro VTODN()
 *  - rewrote dev_finddir() to not use recursion
 *  - added locking to avoid data structure corruption (DEVFS_(UN)LOCK())
 *  Dieter Siegmund (dieter@apple.com) Wed Jul 14 13:37:59 PDT 1999
 *  - fixed problem with devfs_dntovn() checking the v_id against the
 *    value cached in the device node; a union mount on top of us causes
 *    the v_id to get incremented thus, we would end up returning a new
 *    vnode instead of the existing one that has the mounted_here
 *    field filled in; the net effect was that the filesystem mounted
 *    on top of us would never show up
 *  - added devfs_stats to store how many data structures are actually
 *    allocated
 */

/* SPLIT_DEVS means each devfs uses a different devnode for the same device */
/* Otherwise the same device always ends up at the same vnode even if  */
/* reached througgh a different devfs instance. The practical difference */
/* is that with the same vnode, chmods and chowns show up on all instances of */
/* a device. (etc) */

#define SPLIT_DEVS 1 /* maybe make this an option */
/*#define SPLIT_DEVS 1*/

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/malloc.h>
#include <sys/mount_internal.h>
#include <sys/proc.h>
#include <sys/vnode_internal.h>
#include <stdarg.h>
#include <libkern/OSAtomic.h>
#include <os/refcnt.h>
#define BSD_KERNEL_PRIVATE      1       /* devfs_make_link() prototype */
#include "devfs.h"
#include "devfsdefs.h"

#if CONFIG_MACF
#include <security/mac_framework.h>
#endif

#if FDESC
#include "fdesc.h"
#endif

typedef struct devfs_vnode_event {
	vnode_t                 dve_vp;
	uint32_t                dve_vid;
	uint32_t                dve_events;
} *devfs_vnode_event_t;

/*
 * Size of stack buffer (fast path) for notifications.  If
 * the number of mounts is small, no need to malloc a buffer.
 */
#define NUM_STACK_ENTRIES 5

typedef struct devfs_event_log {
	size_t                  del_max;
	size_t                  del_used;
	devfs_vnode_event_t     del_entries;
} *devfs_event_log_t;


static void     dev_free_hier(devdirent_t *);
static int      devfs_propogate(devdirent_t *, devdirent_t *, devfs_event_log_t);
static int      dev_finddir(const char *, devnode_t *, int, devnode_t **, devfs_event_log_t);
static int      dev_dup_entry(devnode_t *, devdirent_t *, devdirent_t **, struct devfsmount *);
void            devfs_ref_node(devnode_t *);
void            devfs_rele_node(devnode_t *);
static void     devfs_record_event(devfs_event_log_t, devnode_t*, uint32_t);
static int      devfs_init_event_log(devfs_event_log_t, uint32_t, devfs_vnode_event_t);
static void     devfs_release_event_log(devfs_event_log_t, int);
static void     devfs_bulk_notify(devfs_event_log_t);
static devdirent_t *devfs_make_node_internal(dev_t, devfstype_t type, uid_t, gid_t, int,
    int (*clone)(dev_t dev, int action), const char *fmt, va_list ap);


lck_grp_t       * devfs_lck_grp;
lck_grp_attr_t  * devfs_lck_grp_attr;
lck_attr_t      * devfs_lck_attr;
lck_mtx_t         devfs_mutex;
lck_mtx_t         devfs_attr_mutex;

os_refgrp_decl(static, devfs_refgrp, "devfs", NULL);

devdirent_t *           dev_root = NULL;        /* root of backing tree */
struct devfs_stats      devfs_stats;            /* hold stats */

static ino_t            devfs_unique_fileno = 0;

#ifdef HIDDEN_MOUNTPOINT
static struct mount *devfs_hidden_mount;
#endif /* HIDDEN_MOINTPOINT */

static int devfs_ready = 0;
static uint32_t devfs_nmountplanes = 0; /* The first plane is not used for a mount */

#define DEVFS_NOCREATE  FALSE
#define DEVFS_CREATE    TRUE

/*
 * Set up the root directory node in the backing plane
 * This is happenning before the vfs system has been
 * set up yet, so be careful about what we reference..
 * Notice that the ops are by indirection.. as they haven't
 * been set up yet!
 * DEVFS has a hidden mountpoint that is used as the anchor point
 * for the internal 'blueprint' version of the dev filesystem tree.
 */
/*proto*/
int
devfs_sinit(void)
{
	int error;

	devfs_lck_grp_attr = lck_grp_attr_alloc_init();
	devfs_lck_grp = lck_grp_alloc_init("devfs_lock", devfs_lck_grp_attr);

	devfs_lck_attr = lck_attr_alloc_init();

	lck_mtx_init(&devfs_mutex, devfs_lck_grp, devfs_lck_attr);
	lck_mtx_init(&devfs_attr_mutex, devfs_lck_grp, devfs_lck_attr);

	DEVFS_LOCK();
	error = dev_add_entry("root", NULL, DEV_DIR, NULL, NULL, NULL, &dev_root);
	DEVFS_UNLOCK();

	if (error) {
		printf("devfs_sinit: dev_add_entry failed ");
		return ENOTSUP;
	}
#ifdef HIDDEN_MOUNTPOINT
	MALLOC(devfs_hidden_mount, struct mount *, sizeof(struct mount),
	    M_MOUNT, M_WAITOK);
	bzero(devfs_hidden_mount, sizeof(struct mount));
	mount_lock_init(devfs_hidden_mount);
	TAILQ_INIT(&devfs_hidden_mount->mnt_vnodelist);
	TAILQ_INIT(&devfs_hidden_mount->mnt_workerqueue);
	TAILQ_INIT(&devfs_hidden_mount->mnt_newvnodes);
#if CONFIG_MACF
	mac_mount_label_init(devfs_hidden_mount);
	mac_mount_label_associate(vfs_context_kernel(), devfs_hidden_mount);
#endif

	/* Initialize the default IO constraints */
	mp->mnt_maxreadcnt = mp->mnt_maxwritecnt = MAXPHYS;
	mp->mnt_segreadcnt = mp->mnt_segwritecnt = 32;
	mp->mnt_ioflags = 0;
	mp->mnt_realrootvp = NULLVP;
	mp->mnt_authcache_ttl = CACHED_LOOKUP_RIGHT_TTL;

	devfs_mount(devfs_hidden_mount, "dummy", NULL, NULL, NULL);
	dev_root->de_dnp->dn_dvm
	        = (struct devfsmount *)devfs_hidden_mount->mnt_data;
#endif /* HIDDEN_MOUNTPOINT */
#if CONFIG_MACF
	mac_devfs_label_associate_directory("/", strlen("/"),
	    dev_root->de_dnp, "/");
#endif
	devfs_ready = 1;
	return 0;
}

/***********************************************************************\
*************************************************************************
*	Routines used to find our way to a point in the tree		*
*************************************************************************
\***********************************************************************/



/***************************************************************
* Search down the linked list off a dir to find "name"
* return the devnode_t * for that node.
*
* called with DEVFS_LOCK held
***************************************************************/
devdirent_t *
dev_findname(devnode_t * dir, const char *name)
{
	devdirent_t * newfp;
	if (dir->dn_type != DEV_DIR) {
		return 0;                     /*XXX*/ /* printf?*/
	}
	if (name[0] == '.') {
		if (name[1] == 0) {
			return dir->dn_typeinfo.Dir.myname;
		}
		if ((name[1] == '.') && (name[2] == 0)) {
			/* for root, .. == . */
			return dir->dn_typeinfo.Dir.parent->dn_typeinfo.Dir.myname;
		}
	}
	newfp = dir->dn_typeinfo.Dir.dirlist;

	while (newfp) {
		if (!(strncmp(name, newfp->de_name, sizeof(newfp->de_name)))) {
			return newfp;
		}
		newfp = newfp->de_next;
	}
	return NULL;
}

/***********************************************************************
* Given a starting node (0 for root) and a pathname, return the node
* for the end item on the path. It MUST BE A DIRECTORY. If the 'DEVFS_CREATE'
* option is true, then create any missing nodes in the path and create
* and return the final node as well.
* This is used to set up a directory, before making nodes in it..
*
* called with DEVFS_LOCK held
***********************************************************************/
static int
dev_finddir(const char * path,
    devnode_t * dirnode,
    int create,
    devnode_t * * dn_pp,
    devfs_event_log_t delp)
{
	devnode_t *     dnp = NULL;
	int             error = 0;
	const char *            scan;
#if CONFIG_MACF
	char            fullpath[DEVMAXPATHSIZE];
#endif


	if (!dirnode) { /* dirnode == NULL means start at root */
		dirnode = dev_root->de_dnp;
	}

	if (dirnode->dn_type != DEV_DIR) {
		return ENOTDIR;
	}

	if (strlen(path) > (DEVMAXPATHSIZE - 1)) {
		return ENAMETOOLONG;
	}

#if CONFIG_MACF
	strlcpy(fullpath, path, DEVMAXPATHSIZE);
#endif
	scan = path;

	while (*scan == '/') {
		scan++;
	}

	*dn_pp = NULL;

	while (1) {
		char                component[DEVMAXPATHSIZE];
		devdirent_t *       dirent_p;
		const char *        start;

		if (*scan == 0) {
			/* we hit the end of the string, we're done */
			*dn_pp = dirnode;
			break;
		}
		start = scan;
		while (*scan != '/' && *scan) {
			scan++;
		}

		strlcpy(component, start, (scan - start) + 1);
		if (*scan == '/') {
			scan++;
		}

		dirent_p = dev_findname(dirnode, component);
		if (dirent_p) {
			dnp = dirent_p->de_dnp;
			if (dnp->dn_type != DEV_DIR) {
				error = ENOTDIR;
				break;
			}
		} else {
			if (!create) {
				error = ENOENT;
				break;
			}
			error = dev_add_entry(component, dirnode,
			    DEV_DIR, NULL, NULL, NULL, &dirent_p);
			if (error) {
				break;
			}
			dnp = dirent_p->de_dnp;
#if CONFIG_MACF
			mac_devfs_label_associate_directory(
				dirnode->dn_typeinfo.Dir.myname->de_name,
				strlen(dirnode->dn_typeinfo.Dir.myname->de_name),
				dnp, fullpath);
#endif
			devfs_propogate(dirnode->dn_typeinfo.Dir.myname, dirent_p, delp);
		}
		dirnode = dnp; /* continue relative to this directory */
	}
	return error;
}


/***********************************************************************
* Add a new NAME element to the devfs
* If we're creating a root node, then dirname is NULL
* Basically this creates a new namespace entry for the device node
*
* Creates a name node, and links it to the supplied node
*
* called with DEVFS_LOCK held
***********************************************************************/
int
dev_add_name(const char * name, devnode_t * dirnode, __unused devdirent_t * back,
    devnode_t * dnp, devdirent_t * *dirent_pp)
{
	devdirent_t *   dirent_p = NULL;

	if (dirnode != NULL) {
		if (dirnode->dn_type != DEV_DIR) {
			return ENOTDIR;
		}

		if (dev_findname(dirnode, name)) {
			return EEXIST;
		}
	}
	/*
	 * make sure the name is legal
	 * slightly misleading in the case of NULL
	 */
	if (!name || (strlen(name) > (DEVMAXNAMESIZE - 1))) {
		return ENAMETOOLONG;
	}

	/*
	 * Allocate and fill out a new directory entry
	 */
	MALLOC(dirent_p, devdirent_t *, sizeof(devdirent_t),
	    M_DEVFSNAME, M_WAITOK);
	if (!dirent_p) {
		return ENOMEM;
	}
	bzero(dirent_p, sizeof(devdirent_t));

	/* inherrit our parent's mount info */ /*XXX*/
	/* a kludge but.... */
	if (dirnode && (dnp->dn_dvm == NULL)) {
		dnp->dn_dvm = dirnode->dn_dvm;
		/* if(!dnp->dn_dvm) printf("parent had null dvm "); */
	}

	/*
	 * Link the two together
	 * include the implicit link in the count of links to the devnode..
	 * this stops it from being accidentally freed later.
	 */
	dirent_p->de_dnp = dnp;
	dnp->dn_links++;  /* implicit from our own name-node */

	/*
	 * Make sure that we can find all the links that reference a node
	 * so that we can get them all if we need to zap the node.
	 */
	if (dnp->dn_linklist) {
		dirent_p->de_nextlink = dnp->dn_linklist;
		dirent_p->de_prevlinkp = dirent_p->de_nextlink->de_prevlinkp;
		dirent_p->de_nextlink->de_prevlinkp = &(dirent_p->de_nextlink);
		*dirent_p->de_prevlinkp = dirent_p;
	} else {
		dirent_p->de_nextlink = dirent_p;
		dirent_p->de_prevlinkp = &(dirent_p->de_nextlink);
	}
	dnp->dn_linklist = dirent_p;

	/*
	 * If the node is a directory, then we need to handle the
	 * creation of the .. link.
	 * A NULL dirnode indicates a root node, so point to ourself.
	 */
	if (dnp->dn_type == DEV_DIR) {
		dnp->dn_typeinfo.Dir.myname = dirent_p;
		/*
		 * If we are unlinking from an old dir, decrement its links
		 * as we point our '..' elsewhere
		 * Note: it's up to the calling code to remove the
		 * us from the original directory's list
		 */
		if (dnp->dn_typeinfo.Dir.parent) {
			dnp->dn_typeinfo.Dir.parent->dn_links--;
		}
		if (dirnode) {
			dnp->dn_typeinfo.Dir.parent = dirnode;
		} else {
			dnp->dn_typeinfo.Dir.parent = dnp;
		}
		dnp->dn_typeinfo.Dir.parent->dn_links++; /* account for the new '..' */
	}

	/*
	 * put the name into the directory entry.
	 */
	strlcpy(dirent_p->de_name, name, DEVMAXNAMESIZE);


	/*
	 * Check if we are not making a root node..
	 * (i.e. have parent)
	 */
	if (dirnode) {
		/*
		 * Put it on the END of the linked list of directory entries
		 */
		dirent_p->de_parent = dirnode; /* null for root */
		dirent_p->de_prevp = dirnode->dn_typeinfo.Dir.dirlast;
		dirent_p->de_next = *(dirent_p->de_prevp); /* should be NULL */
		                                           /*right?*/
		*(dirent_p->de_prevp) = dirent_p;
		dirnode->dn_typeinfo.Dir.dirlast = &(dirent_p->de_next);
		dirnode->dn_typeinfo.Dir.entrycount++;
		dirnode->dn_len += strlen(name) + 8;/*ok, ok?*/
	}

	*dirent_pp = dirent_p;
	DEVFS_INCR_ENTRIES();
	return 0;
}


/***********************************************************************
* Add a new element to the devfs plane.
*
* Creates a new dev_node to go with it if the prototype should not be
* reused. (Is a DIR, or we select SPLIT_DEVS at compile time)
* typeinfo gives us info to make our node if we don't have a prototype.
* If typeinfo is null and proto exists, then the typeinfo field of
* the proto is used intead in the DEVFS_CREATE case.
* note the 'links' count is 0 (except if a dir)
* but it is only cleared on a transition
* so this is ok till we link it to something
* Even in SPLIT_DEVS mode,
* if the node already exists on the wanted plane, just return it
*
* called with DEVFS_LOCK held
***********************************************************************/
int
dev_add_node(int entrytype, devnode_type_t * typeinfo, devnode_t * proto,
    devnode_t * *dn_pp, struct devfsmount *dvm)
{
	devnode_t *     dnp = NULL;
	int     error = 0;

#if defined SPLIT_DEVS
	/*
	 * If we have a prototype, then check if there is already a sibling
	 * on the mount plane we are looking at, if so, just return it.
	 */
	if (proto) {
		dnp = proto->dn_nextsibling;
		while (dnp != proto) {
			if (dnp->dn_dvm == dvm) {
				*dn_pp = dnp;
				return 0;
			}
			dnp = dnp->dn_nextsibling;
		}
		if (typeinfo == NULL) {
			typeinfo = &(proto->dn_typeinfo);
		}
	}
#else   /* SPLIT_DEVS */
	if (proto) {
		switch (proto->type) {
		case DEV_BDEV:
		case DEV_CDEV:
			*dn_pp = proto;
			return 0;
		}
	}
#endif  /* SPLIT_DEVS */
	MALLOC(dnp, devnode_t *, sizeof(devnode_t), M_DEVFSNODE, M_WAITOK);
	if (!dnp) {
		return ENOMEM;
	}

	/*
	 * If we have a proto, that means that we are duplicating some
	 * other device, which can only happen if we are not at the back plane
	 */
	if (proto) {
		bcopy(proto, dnp, sizeof(devnode_t));
		dnp->dn_links = 0;
		dnp->dn_linklist = NULL;
		dnp->dn_vn = NULL;
		dnp->dn_len = 0;
		/* add to END of siblings list */
		dnp->dn_prevsiblingp = proto->dn_prevsiblingp;
		*(dnp->dn_prevsiblingp) = dnp;
		dnp->dn_nextsibling = proto;
		proto->dn_prevsiblingp = &(dnp->dn_nextsibling);
#if CONFIG_MACF
		mac_devfs_label_init(dnp);
		mac_devfs_label_copy(proto->dn_label, dnp->dn_label);
#endif
	} else {
		struct timeval tv;

		/*
		 * We have no prototype, so start off with a clean slate
		 */
		microtime(&tv);
		bzero(dnp, sizeof(devnode_t));
		dnp->dn_type = entrytype;
		dnp->dn_nextsibling = dnp;
		dnp->dn_prevsiblingp = &(dnp->dn_nextsibling);
		dnp->dn_atime.tv_sec = tv.tv_sec;
		dnp->dn_mtime.tv_sec = tv.tv_sec;
		dnp->dn_ctime.tv_sec = tv.tv_sec;
#if CONFIG_MACF
		mac_devfs_label_init(dnp);
#endif
	}
	dnp->dn_dvm = dvm;

	/* Note: this inits the reference count to 1, this is considered unreferenced */
	os_ref_init_raw(&dnp->dn_refcount, &devfs_refgrp);
	dnp->dn_ino = devfs_unique_fileno;
	devfs_unique_fileno++;

	/*
	 * fill out the dev node according to type
	 */
	switch (entrytype) {
	case DEV_DIR:
		/*
		 * As it's a directory, make sure
		 * it has a null entries list
		 */
		dnp->dn_typeinfo.Dir.dirlast = &(dnp->dn_typeinfo.Dir.dirlist);
		dnp->dn_typeinfo.Dir.dirlist = (devdirent_t *)0;
		dnp->dn_typeinfo.Dir.entrycount = 0;
		/*  until we know better, it has a null parent pointer*/
		dnp->dn_typeinfo.Dir.parent = NULL;
		dnp->dn_links++; /* for .*/
		dnp->dn_typeinfo.Dir.myname = NULL;
		/*
		 * make sure that the ops associated with it are the ops
		 * that we use (by default) for directories
		 */
		dnp->dn_ops = &devfs_vnodeop_p;
		dnp->dn_mode |= 0555;   /* default perms */
		break;
	case DEV_SLNK:
		/*
		 * As it's a symlink allocate and store the link info
		 * Symlinks should only ever be created by the user,
		 * so they are not on the back plane and should not be
		 * propogated forward.. a bit like directories in that way..
		 * A symlink only exists on one plane and has its own
		 * node.. therefore we might be on any random plane.
		 */
		MALLOC(dnp->dn_typeinfo.Slnk.name, char *,
		    typeinfo->Slnk.namelen + 1,
		    M_DEVFSNODE, M_WAITOK);
		if (!dnp->dn_typeinfo.Slnk.name) {
			error = ENOMEM;
			break;
		}
		strlcpy(dnp->dn_typeinfo.Slnk.name, typeinfo->Slnk.name,
		    typeinfo->Slnk.namelen + 1);
		dnp->dn_typeinfo.Slnk.namelen = typeinfo->Slnk.namelen;
		DEVFS_INCR_STRINGSPACE(dnp->dn_typeinfo.Slnk.namelen + 1);
		dnp->dn_ops = &devfs_vnodeop_p;
		dnp->dn_mode |= 0555;   /* default perms */
		break;
	case DEV_CDEV:
	case DEV_BDEV:
		/*
		 * Make sure it has DEVICE type ops
		 * and device specific fields are correct
		 */
		dnp->dn_ops = &devfs_spec_vnodeop_p;
		dnp->dn_typeinfo.dev = typeinfo->dev;
		break;

	#if FDESC
	/* /dev/fd is special */
	case DEV_DEVFD:
		dnp->dn_ops = &devfs_devfd_vnodeop_p;
		dnp->dn_mode |= 0555;   /* default perms */
		break;

	#endif /* FDESC */
	default:
		error = EINVAL;
	}

	if (error) {
		FREE(dnp, M_DEVFSNODE);
	} else {
		*dn_pp = dnp;
		DEVFS_INCR_NODES();
	}

	return error;
}


/***********************************************************************
 * called with DEVFS_LOCK held
 **********************************************************************/
void
devnode_free(devnode_t * dnp)
{
#if CONFIG_MACF
	mac_devfs_label_destroy(dnp);
#endif
	if (dnp->dn_type == DEV_SLNK) {
		DEVFS_DECR_STRINGSPACE(dnp->dn_typeinfo.Slnk.namelen + 1);
		FREE(dnp->dn_typeinfo.Slnk.name, M_DEVFSNODE);
	}
	DEVFS_DECR_NODES();
	FREE(dnp, M_DEVFSNODE);
}


/***********************************************************************
 * called with DEVFS_LOCK held
 **********************************************************************/
static void
devfs_dn_free(devnode_t * dnp)
{
	if (--dnp->dn_links <= 0) { /* can be -1 for initial free, on error */
		/*probably need to do other cleanups XXX */
		if (dnp->dn_nextsibling != dnp) {
			devnode_t * *   prevp = dnp->dn_prevsiblingp;
			*prevp = dnp->dn_nextsibling;
			dnp->dn_nextsibling->dn_prevsiblingp = prevp;
		}

		/* Can only free if there are no references; otherwise, wait for last vnode to be reclaimed */
		os_ref_count_t rc = os_ref_get_count_raw(&dnp->dn_refcount);
		if (rc == 1) {
			/* release final reference from dev_add_node */
			(void) os_ref_release_locked_raw(&dnp->dn_refcount, &devfs_refgrp);
			devnode_free(dnp);
		} else {
			dnp->dn_lflags |= DN_DELETE;
		}
	}
}

/***********************************************************************\
*	Front Node Operations						*
*	Add or delete a chain of front nodes				*
\***********************************************************************/


/***********************************************************************
* Given a directory backing node, and a child backing node, add the
* appropriate front nodes to the front nodes of the directory to
* represent the child node to the user
*
* on failure, front nodes will either be correct or not exist for each
* front dir, however dirs completed will not be stripped of completed
* frontnodes on failure of a later frontnode
*
* This allows a new node to be propogated through all mounted planes
*
* called with DEVFS_LOCK held
***********************************************************************/
static int
devfs_propogate(devdirent_t * parent, devdirent_t * child, devfs_event_log_t delp)
{
	int     error;
	devdirent_t * newnmp;
	devnode_t *     dnp = child->de_dnp;
	devnode_t *     pdnp = parent->de_dnp;
	devnode_t *     adnp = parent->de_dnp;
	int type = child->de_dnp->dn_type;
	uint32_t events;

	events = (dnp->dn_type == DEV_DIR ? VNODE_EVENT_DIR_CREATED : VNODE_EVENT_FILE_CREATED);
	if (delp != NULL) {
		devfs_record_event(delp, pdnp, events);
	}

	/***********************************************
	* Find the other instances of the parent node
	***********************************************/
	for (adnp = pdnp->dn_nextsibling;
	    adnp != pdnp;
	    adnp = adnp->dn_nextsibling) {
		/*
		 * Make the node, using the original as a prototype)
		 * if the node already exists on that plane it won't be
		 * re-made..
		 */
		if ((error = dev_add_entry(child->de_name, adnp, type,
		    NULL, dnp, adnp->dn_dvm,
		    &newnmp)) != 0) {
			printf("duplicating %s failed\n", child->de_name);
		} else {
			if (delp != NULL) {
				devfs_record_event(delp, adnp, events);

				/*
				 * Slightly subtle.  We're guaranteed that there will
				 * only be a vnode hooked into this devnode if we're creating
				 * a new link to an existing node; otherwise, the devnode is new
				 * and no one can have looked it up yet. If we're making a link,
				 * then the buffer is large enough for two nodes in each
				 * plane; otherwise, there's no vnode and this call will
				 * do nothing.
				 */
				devfs_record_event(delp, newnmp->de_dnp, VNODE_EVENT_LINK);
			}
		}
	}
	return 0;       /* for now always succeed */
}

static uint32_t
remove_notify_count(devnode_t *dnp)
{
	uint32_t notify_count = 0;
	devnode_t *dnp2;

	/*
	 * Could need to notify for one removed node on each mount and
	 * one parent for each such node.
	 */
	notify_count = devfs_nmountplanes;
	notify_count += dnp->dn_links;
	for (dnp2 = dnp->dn_nextsibling; dnp2 != dnp; dnp2 = dnp2->dn_nextsibling) {
		notify_count += dnp2->dn_links;
	}

	return notify_count;
}

/***********************************************************************
* remove all instances of this devicename [for backing nodes..]
* note.. if there is another link to the node (non dir nodes only)
* then the devfs_node will still exist as the ref count will be non-0
* removing a directory node will remove all sup-nodes on all planes (ZAP)
*
* Used by device drivers to remove nodes that are no longer relevant
* The argument is the 'cookie' they were given when they created the node
* this function is exported.. see devfs.h
***********************************************************************/
void
devfs_remove(void *dirent_p)
{
	devnode_t * dnp = ((devdirent_t *)dirent_p)->de_dnp;
	devnode_t * dnp2;
	boolean_t   lastlink;
	struct devfs_event_log event_log;
	uint32_t    log_count = 0;
	int         do_notify = 0;
	int         need_free = 0;
	struct devfs_vnode_event stackbuf[NUM_STACK_ENTRIES];

	DEVFS_LOCK();

	if (!devfs_ready) {
		printf("devfs_remove: not ready for devices!\n");
		goto out;
	}

	log_count = remove_notify_count(dnp);

	if (log_count > NUM_STACK_ENTRIES) {
		uint32_t new_count;
wrongsize:
		DEVFS_UNLOCK();
		if (devfs_init_event_log(&event_log, log_count, NULL) == 0) {
			do_notify = 1;
			need_free = 1;
		}
		DEVFS_LOCK();

		new_count = remove_notify_count(dnp);
		if (need_free && (new_count > log_count)) {
			devfs_release_event_log(&event_log, 1);
			need_free = 0;
			do_notify = 0;
			log_count = log_count * 2;
			goto wrongsize;
		}
	} else {
		if (devfs_init_event_log(&event_log, NUM_STACK_ENTRIES, &stackbuf[0]) == 0) {
			do_notify = 1;
		}
	}

	/* This file has been deleted */
	if (do_notify != 0) {
		devfs_record_event(&event_log, dnp, VNODE_EVENT_DELETE);
	}

	/* keep removing the next sibling till only we exist. */
	while ((dnp2 = dnp->dn_nextsibling) != dnp) {
		/*
		 * Keep removing the next front node till no more exist
		 */
		dnp->dn_nextsibling = dnp2->dn_nextsibling;
		dnp->dn_nextsibling->dn_prevsiblingp = &(dnp->dn_nextsibling);
		dnp2->dn_nextsibling = dnp2;
		dnp2->dn_prevsiblingp = &(dnp2->dn_nextsibling);

		/* This file has been deleted in this plane */
		if (do_notify != 0) {
			devfs_record_event(&event_log, dnp2, VNODE_EVENT_DELETE);
		}

		if (dnp2->dn_linklist) {
			do {
				lastlink = (1 == dnp2->dn_links);
				/* Each parent of a link to this file has lost a child in this plane */
				if (do_notify != 0) {
					devfs_record_event(&event_log, dnp2->dn_linklist->de_parent, VNODE_EVENT_FILE_REMOVED);
				}
				dev_free_name(dnp2->dn_linklist);
			} while (!lastlink);
		}
	}

	/*
	 * then free the main node
	 * If we are not running in SPLIT_DEVS mode, then
	 * THIS is what gets rid of the propogated nodes.
	 */
	if (dnp->dn_linklist) {
		do {
			lastlink = (1 == dnp->dn_links);
			/* Each parent of a link to this file has lost a child */
			if (do_notify != 0) {
				devfs_record_event(&event_log, dnp->dn_linklist->de_parent, VNODE_EVENT_FILE_REMOVED);
			}
			dev_free_name(dnp->dn_linklist);
		} while (!lastlink);
	}
out:
	DEVFS_UNLOCK();
	if (do_notify != 0) {
		devfs_bulk_notify(&event_log);
		devfs_release_event_log(&event_log, need_free);
	}

	return;
}



/***************************************************************
 * duplicate the backing tree into a tree of nodes hung off the
 * mount point given as the argument. Do this by
 * calling dev_dup_entry which recurses all the way
 * up the tree..
 *
 * called with DEVFS_LOCK held
 **************************************************************/
int
dev_dup_plane(struct devfsmount *devfs_mp_p)
{
	devdirent_t *   new;
	int             error = 0;

	if ((error = dev_dup_entry(NULL, dev_root, &new, devfs_mp_p))) {
		return error;
	}
	devfs_mp_p->plane_root = new;
	devfs_nmountplanes++;
	return error;
}



/***************************************************************
* Free a whole plane
*
* called with DEVFS_LOCK held
***************************************************************/
void
devfs_free_plane(struct devfsmount *devfs_mp_p)
{
	devdirent_t * dirent_p;

	dirent_p = devfs_mp_p->plane_root;
	if (dirent_p) {
		dev_free_hier(dirent_p);
		dev_free_name(dirent_p);
	}
	devfs_mp_p->plane_root = NULL;
	devfs_nmountplanes--;

	if (devfs_nmountplanes > (devfs_nmountplanes + 1)) {
		panic("plane count wrapped around.\n");
	}
}


/***************************************************************
* Create and link in a new front element..
* Parent can be 0 for a root node
* Not presently usable to make a symlink XXX
* (Ok, symlinks don't propogate)
* recursively will create subnodes corresponding to equivalent
* child nodes in the base level
*
* called with DEVFS_LOCK held
***************************************************************/
static int
dev_dup_entry(devnode_t * parent, devdirent_t * back, devdirent_t * *dnm_pp,
    struct devfsmount *dvm)
{
	devdirent_t *   entry_p = NULL;
	devdirent_t *   newback;
	devdirent_t *   newfront;
	int     error;
	devnode_t *     dnp = back->de_dnp;
	int type = dnp->dn_type;

	/*
	 * go get the node made (if we need to)
	 * use the back one as a prototype
	 */
	error = dev_add_entry(back->de_name, parent, type, NULL, dnp,
	    parent?parent->dn_dvm:dvm, &entry_p);
	if (!error && (entry_p == NULL)) {
		error = ENOMEM; /* Really can't happen, but make static analyzer happy */
	}
	if (error != 0) {
		printf("duplicating %s failed\n", back->de_name);
		goto out;
	}

	/*
	 * If we have just made the root, then insert the pointer to the
	 * mount information
	 */
	if (dvm) {
		entry_p->de_dnp->dn_dvm = dvm;
	}

	/*
	 * If it is a directory, then recurse down all the other
	 * subnodes in it....
	 * note that this time we don't pass on the mount info..
	 */
	if (type == DEV_DIR) {
		for (newback = back->de_dnp->dn_typeinfo.Dir.dirlist;
		    newback; newback = newback->de_next) {
			if ((error = dev_dup_entry(entry_p->de_dnp,
			    newback, &newfront, NULL)) != 0) {
				break; /* back out with an error */
			}
		}
	}
out:
	*dnm_pp = entry_p;
	return error;
}


/***************************************************************
* Free a name node
* remember that if there are other names pointing to the
* dev_node then it may not get freed yet
* can handle if there is no dnp
*
* called with DEVFS_LOCK held
***************************************************************/

int
dev_free_name(devdirent_t * dirent_p)
{
	devnode_t *     parent = dirent_p->de_parent;
	devnode_t *     dnp = dirent_p->de_dnp;

	if (dnp) {
		if (dnp->dn_type == DEV_DIR) {
			devnode_t * p;

			if (dnp->dn_typeinfo.Dir.dirlist) {
				return ENOTEMPTY;
			}
			p = dnp->dn_typeinfo.Dir.parent;
			devfs_dn_free(dnp);     /* account for '.' */
			devfs_dn_free(p);       /* '..' */
		}
		/*
		 * unlink us from the list of links for this node
		 * If we are the only link, it's easy!
		 * if we are a DIR of course there should not be any
		 * other links.
		 */
		if (dirent_p->de_nextlink == dirent_p) {
			dnp->dn_linklist = NULL;
		} else {
			if (dnp->dn_linklist == dirent_p) {
				dnp->dn_linklist = dirent_p->de_nextlink;
			}
		}
		devfs_dn_free(dnp);
	}

	dirent_p->de_nextlink->de_prevlinkp = dirent_p->de_prevlinkp;
	*(dirent_p->de_prevlinkp) = dirent_p->de_nextlink;

	/*
	 * unlink ourselves from the directory on this plane
	 */
	if (parent) { /* if not fs root */
		if ((*dirent_p->de_prevp = dirent_p->de_next)) {/* yes, assign */
			dirent_p->de_next->de_prevp = dirent_p->de_prevp;
		} else {
			parent->dn_typeinfo.Dir.dirlast
			        = dirent_p->de_prevp;
		}
		parent->dn_typeinfo.Dir.entrycount--;
		parent->dn_len -= strlen(dirent_p->de_name) + 8;
	}

	DEVFS_DECR_ENTRIES();
	FREE(dirent_p, M_DEVFSNAME);
	return 0;
}


/***************************************************************
* Free a hierarchy starting at a directory node name
* remember that if there are other names pointing to the
* dev_node then it may not get freed yet
* can handle if there is no dnp
* leave the node itself allocated.
*
* called with DEVFS_LOCK held
***************************************************************/

static void
dev_free_hier(devdirent_t * dirent_p)
{
	devnode_t *     dnp = dirent_p->de_dnp;

	if (dnp) {
		if (dnp->dn_type == DEV_DIR) {
			while (dnp->dn_typeinfo.Dir.dirlist) {
				dev_free_hier(dnp->dn_typeinfo.Dir.dirlist);
				dev_free_name(dnp->dn_typeinfo.Dir.dirlist);
			}
		}
	}
}


/***************************************************************
 * given a dev_node, find the appropriate vnode if one is already
 * associated, or get a new one and associate it with the dev_node
 *
 * called with DEVFS_LOCK held
 *
 * If an error is returned, then the dnp may have been freed (we
 * raced with a delete and lost).  A devnode should not be accessed
 * after devfs_dntovn() fails.
 ****************************************************************/
int
devfs_dntovn(devnode_t * dnp, struct vnode **vn_pp, __unused struct proc * p)
{
	struct vnode *vn_p;
	int error = 0;
	struct vnode_fsparam vfsp;
	enum vtype vtype = 0;
	int markroot = 0;
	int nretries = 0;
	int n_minor = DEVFS_CLONE_ALLOC; /* new minor number for clone device */

	/*
	 * We should never come in and find that our devnode has been marked for delete.
	 * The lookup should have held the lock from entry until now; it should not have
	 * been able to find a removed entry. Any other pathway would have just created
	 * the devnode and come here without dropping the devfs lock, so no one would
	 * have a chance to delete.
	 */
	if (dnp->dn_lflags & DN_DELETE) {
		panic("devfs_dntovn: DN_DELETE set on a devnode upon entry.");
	}

	devfs_ref_node(dnp);

retry:
	*vn_pp = NULL;
	vn_p = dnp->dn_vn;

	if (vn_p) { /* already has a vnode */
		uint32_t vid;

		vid = vnode_vid(vn_p);

		DEVFS_UNLOCK();

		/*
		 * We want to use the drainok variant of vnode_getwithvid
		 * because we _don't_ want to get an iocount if the vnode is
		 * is blocked in vnode_drain as it can cause infinite
		 * loops in vn_open_auth. While in use vnodes are typically
		 * only reclaimed on forced unmounts, In use devfs tty vnodes
		 * can  be quite frequently reclaimed by revoke(2) or by the
		 * exit of a controlling process.
		 */
		error = vnode_getwithvid_drainok(vn_p, vid);

		DEVFS_LOCK();

		if (dnp->dn_lflags & DN_DELETE) {
			/*
			 * our BUSY node got marked for
			 * deletion while the DEVFS lock
			 * was dropped...
			 */
			if (error == 0) {
				/*
				 * vnode_getwithvid returned a valid ref
				 * which we need to drop
				 */
				vnode_put(vn_p);
			}

			/*
			 * This entry is no longer in the namespace.  This is only
			 * possible for lookup: no other path would not find an existing
			 * vnode.  Therefore, ENOENT is a valid result.
			 */
			error = ENOENT;
		} else if (error == ENODEV) {
			/*
			 * The Filesystem is getting unmounted.
			 */
			error = ENOENT;
		} else if (error && (nretries < DEV_MAX_VNODE_RETRY)) {
			/*
			 * If we got an error from vnode_getwithvid, it means
			 * we raced with a recycle and lost i.e. we asked for
			 * an iocount only after vnode_drain had been entered
			 * for the vnode and returned with an error only after
			 * devfs_reclaim was called on the vnode.  devfs_reclaim
			 * sets dn_vn to NULL but while we were waiting to
			 * reacquire DEVFS_LOCK, another vnode might have gotten
			 * associated with the dnp. In either case, we need to
			 * retry otherwise we will end up returning an ENOENT
			 * for this lookup but the next lookup will  succeed
			 * because it creates a new vnode (or a racing  lookup
			 * created a new vnode already).
			 */
			error = 0;
			nretries++;
			goto retry;
		}
		if (!error) {
			*vn_pp = vn_p;
		}

		goto out;
	}

	/*
	 * If we get here, then we've beaten any deletes;
	 * if someone sets DN_DELETE during a subsequent drop
	 * of the devfs lock, we'll still vend a vnode.
	 */

	if (dnp->dn_lflags & DN_CREATE) {
		dnp->dn_lflags |= DN_CREATEWAIT;
		msleep(&dnp->dn_lflags, &devfs_mutex, PRIBIO, 0, 0);
		goto retry;
	}

	dnp->dn_lflags |= DN_CREATE;

	switch (dnp->dn_type) {
	case    DEV_SLNK:
		vtype = VLNK;
		break;
	case    DEV_DIR:
		if (dnp->dn_typeinfo.Dir.parent == dnp) {
			markroot = 1;
		}
		vtype = VDIR;
		break;
	case    DEV_BDEV:
	case    DEV_CDEV:
		vtype = (dnp->dn_type == DEV_BDEV) ? VBLK : VCHR;
		break;
#if FDESC
	case    DEV_DEVFD:
		vtype = VDIR;
		break;
#endif /* FDESC */
	}
	vfsp.vnfs_mp = dnp->dn_dvm->mount;
	vfsp.vnfs_vtype = vtype;
	vfsp.vnfs_str = "devfs";
	vfsp.vnfs_dvp = 0;
	vfsp.vnfs_fsnode = dnp;
	vfsp.vnfs_cnp = 0;
	vfsp.vnfs_vops = *(dnp->dn_ops);

	if (vtype == VBLK || vtype == VCHR) {
		/*
		 * Ask the clone minor number function for a new minor number
		 * to use for the next device instance.  If an administative
		 * limit has been reached, this function will return -1.
		 */
		if (dnp->dn_clone != NULL) {
			int     n_major = major(dnp->dn_typeinfo.dev);

			n_minor = (*dnp->dn_clone)(dnp->dn_typeinfo.dev, DEVFS_CLONE_ALLOC);
			if (n_minor == -1) {
				error = ENOMEM;
				goto out;
			}

			vfsp.vnfs_rdev = makedev(n_major, n_minor);;
		} else {
			vfsp.vnfs_rdev = dnp->dn_typeinfo.dev;
		}
	} else {
		vfsp.vnfs_rdev = 0;
	}
	vfsp.vnfs_filesize = 0;
	vfsp.vnfs_flags = VNFS_NOCACHE | VNFS_CANTCACHE;
	/* Tag system files */
	vfsp.vnfs_marksystem = 0;
	vfsp.vnfs_markroot = markroot;

	DEVFS_UNLOCK();

	error = vnode_create(VNCREATE_FLAVOR, VCREATESIZE, &vfsp, &vn_p);

	/* Do this before grabbing the lock */
	if (error == 0) {
		vnode_setneedinactive(vn_p);
	}

	DEVFS_LOCK();

	if (error == 0) {
		vnode_settag(vn_p, VT_DEVFS);

		if ((dnp->dn_clone != NULL) && (dnp->dn_vn != NULLVP)) {
			panic("devfs_dntovn: cloning device with a vnode?\n");
		}

		*vn_pp = vn_p;

		/*
		 * Another vnode that has this devnode as its v_data.
		 * This reference, unlike the one taken at the start
		 * of the function, persists until a VNOP_RECLAIM
		 * comes through for this vnode.
		 */
		devfs_ref_node(dnp);

		/*
		 * A cloned vnode is not hooked into the devnode; every lookup
		 * gets a new vnode.
		 */
		if (dnp->dn_clone == NULL) {
			dnp->dn_vn = vn_p;
		}
	} else if (n_minor != DEVFS_CLONE_ALLOC) {
		/*
		 * If we failed the create, we need to release the cloned minor
		 * back to the free list.  In general, this is only useful if
		 * the clone function results in a state change in the cloned
		 * device for which the minor number was obtained.  If we get
		 * past this point withouth falling into this case, it's
		 * assumed that any state to be released will be released when
		 * the vnode is dropped, instead.
		 */
		(void)(*dnp->dn_clone)(dnp->dn_typeinfo.dev, DEVFS_CLONE_FREE);
	}

	dnp->dn_lflags &= ~DN_CREATE;
	if (dnp->dn_lflags & DN_CREATEWAIT) {
		dnp->dn_lflags &= ~DN_CREATEWAIT;
		wakeup(&dnp->dn_lflags);
	}

out:
	/*
	 * Release the reference we took to prevent deletion while we weren't holding the lock.
	 * If not returning success, then dropping this reference could delete the devnode;
	 * no one should access a devnode after a call to devfs_dntovn fails.
	 */
	devfs_rele_node(dnp);

	return error;
}

/*
 * Increment refcount on a devnode; prevents free of the node
 * while the devfs lock is not held.
 */
void
devfs_ref_node(devnode_t *dnp)
{
	os_ref_retain_locked_raw(&dnp->dn_refcount, &devfs_refgrp);
}

/*
 * Release a reference on a devnode.  If the devnode is marked for
 * free and the refcount is dropped to one, do the free.
 */
void
devfs_rele_node(devnode_t *dnp)
{
	os_ref_count_t rc = os_ref_release_locked_raw(&dnp->dn_refcount, &devfs_refgrp);
	if (rc < 1) {
		panic("devfs_rele_node: devnode without a refcount!\n");
	} else if ((rc == 1) && (dnp->dn_lflags & DN_DELETE)) {
		/* release final reference from dev_add_node */
		(void) os_ref_release_locked_raw(&dnp->dn_refcount, &devfs_refgrp);
		devnode_free(dnp);
	}
}

/***********************************************************************
* add a whole device, with no prototype.. make name element and node
* Used for adding the original device entries
*
* called with DEVFS_LOCK held
***********************************************************************/
int
dev_add_entry(const char *name, devnode_t * parent, int type, devnode_type_t * typeinfo,
    devnode_t * proto, struct devfsmount *dvm, devdirent_t * *nm_pp)
{
	devnode_t *     dnp;
	int     error = 0;

	if ((error = dev_add_node(type, typeinfo, proto, &dnp,
	    (parent?parent->dn_dvm:dvm))) != 0) {
		printf("devfs: %s: base node allocation failed (Errno=%d)\n",
		    name, error);
		return error;
	}
	if ((error = dev_add_name(name, parent, NULL, dnp, nm_pp)) != 0) {
		devfs_dn_free(dnp); /* 1->0 for dir, 0->(-1) for other */
		printf("devfs: %s: name slot allocation failed (Errno=%d)\n",
		    name, error);
	}
	return error;
}

static void
devfs_bulk_notify(devfs_event_log_t delp)
{
	uint32_t i;
	for (i = 0; i < delp->del_used; i++) {
		devfs_vnode_event_t dvep = &delp->del_entries[i];
		if (vnode_getwithvid(dvep->dve_vp, dvep->dve_vid) == 0) {
			vnode_notify(dvep->dve_vp, dvep->dve_events, NULL);
			vnode_put(dvep->dve_vp);
		}
	}
}

static void
devfs_record_event(devfs_event_log_t delp, devnode_t *dnp, uint32_t events)
{
	if (delp->del_used >= delp->del_max) {
		panic("devfs event log overflowed.\n");
	}

	/* Can only notify for nodes that have an associated vnode */
	if (dnp->dn_vn != NULLVP && vnode_ismonitored(dnp->dn_vn)) {
		devfs_vnode_event_t dvep = &delp->del_entries[delp->del_used];
		dvep->dve_vp = dnp->dn_vn;
		dvep->dve_vid = vnode_vid(dnp->dn_vn);
		dvep->dve_events = events;
		delp->del_used++;
	}
}

static int
devfs_init_event_log(devfs_event_log_t delp, uint32_t count, devfs_vnode_event_t buf)
{
	devfs_vnode_event_t dvearr;

	if (buf == NULL) {
		MALLOC(dvearr, devfs_vnode_event_t, count * sizeof(struct devfs_vnode_event), M_TEMP, M_WAITOK | M_ZERO);
		if (dvearr == NULL) {
			return ENOMEM;
		}
	} else {
		dvearr = buf;
	}

	delp->del_max = count;
	delp->del_used = 0;
	delp->del_entries = dvearr;
	return 0;
}

static void
devfs_release_event_log(devfs_event_log_t delp, int need_free)
{
	if (delp->del_entries == NULL) {
		panic("Free of devfs notify info that has not been intialized.\n");
	}

	if (need_free) {
		FREE(delp->del_entries, M_TEMP);
	}

	delp->del_entries = NULL;
}

/*
 * Function: devfs_make_node
 *
 * Purpose
 *   Create a device node with the given pathname in the devfs namespace.
 *
 * Parameters:
 *   dev        - the dev_t value to associate
 *   chrblk	- block or character device (DEVFS_CHAR or DEVFS_BLOCK)
 *   uid, gid	- ownership
 *   perms	- permissions
 *   clone	- minor number cloning function
 *   fmt, ...	- path format string with printf args to format the path name
 * Returns:
 *   A handle to a device node if successful, NULL otherwise.
 */
void *
devfs_make_node_clone(dev_t dev, int chrblk, uid_t uid,
    gid_t gid, int perms, int (*clone)(dev_t dev, int action),
    const char *fmt, ...)
{
	devdirent_t *   new_dev = NULL;
	devfstype_t     type;
	va_list ap;

	switch (chrblk) {
	case DEVFS_CHAR:
		type = DEV_CDEV;
		break;
	case DEVFS_BLOCK:
		type = DEV_BDEV;
		break;
	default:
		goto out;
	}

	va_start(ap, fmt);
	new_dev = devfs_make_node_internal(dev, type, uid, gid, perms, clone, fmt, ap);
	va_end(ap);
out:
	return new_dev;
}


/*
 * Function: devfs_make_node
 *
 * Purpose
 *   Create a device node with the given pathname in the devfs namespace.
 *
 * Parameters:
 *   dev        - the dev_t value to associate
 *   chrblk	- block or character device (DEVFS_CHAR or DEVFS_BLOCK)
 *   uid, gid	- ownership
 *   perms	- permissions
 *   fmt, ...	- path format string with printf args to format the path name
 * Returns:
 *   A handle to a device node if successful, NULL otherwise.
 */
void *
devfs_make_node(dev_t dev, int chrblk, uid_t uid,
    gid_t gid, int perms, const char *fmt, ...)
{
	devdirent_t *   new_dev = NULL;
	devfstype_t type;
	va_list ap;

	if (chrblk != DEVFS_CHAR && chrblk != DEVFS_BLOCK) {
		goto out;
	}

	type = (chrblk == DEVFS_BLOCK ? DEV_BDEV : DEV_CDEV);

	va_start(ap, fmt);
	new_dev = devfs_make_node_internal(dev, type, uid, gid, perms, NULL, fmt, ap);
	va_end(ap);

out:
	return new_dev;
}

static devdirent_t *
devfs_make_node_internal(dev_t dev, devfstype_t type, uid_t uid,
    gid_t gid, int perms, int (*clone)(dev_t dev, int action), const char *fmt, va_list ap)
{
	devdirent_t *   new_dev = NULL;
	devnode_t * dnp;
	devnode_type_t  typeinfo;

	char            *name, buf[256]; /* XXX */
	const char      *path;
#if CONFIG_MACF
	char buff[sizeof(buf)];
#endif
	int             i;
	uint32_t        log_count;
	struct devfs_event_log event_log;
	struct devfs_vnode_event stackbuf[NUM_STACK_ENTRIES];
	int             need_free = 0;

	vsnprintf(buf, sizeof(buf), fmt, ap);

#if CONFIG_MACF
	bcopy(buf, buff, sizeof(buff));
	buff[sizeof(buff) - 1] = 0;
#endif
	name = NULL;

	for (i = strlen(buf); i > 0; i--) {
		if (buf[i] == '/') {
			name = &buf[i];
			buf[i] = 0;
			break;
		}
	}

	if (name) {
		*name++ = '\0';
		path = buf;
	} else {
		name = buf;
		path = "/";
	}

	log_count = devfs_nmountplanes;
	if (log_count > NUM_STACK_ENTRIES) {
wrongsize:
		need_free = 1;
		if (devfs_init_event_log(&event_log, log_count, NULL) != 0) {
			return NULL;
		}
	} else {
		need_free = 0;
		log_count = NUM_STACK_ENTRIES;
		if (devfs_init_event_log(&event_log, log_count, &stackbuf[0]) != 0) {
			return NULL;
		}
	}

	DEVFS_LOCK();
	if (log_count < devfs_nmountplanes) {
		DEVFS_UNLOCK();
		devfs_release_event_log(&event_log, need_free);
		log_count = log_count * 2;
		goto wrongsize;
	}

	if (!devfs_ready) {
		printf("devfs_make_node: not ready for devices!\n");
		goto out;
	}

	/* find/create directory path ie. mkdir -p */
	if (dev_finddir(path, NULL, DEVFS_CREATE, &dnp, &event_log) == 0) {
		typeinfo.dev = dev;
		if (dev_add_entry(name, dnp, type, &typeinfo, NULL, NULL, &new_dev) == 0) {
			new_dev->de_dnp->dn_gid = gid;
			new_dev->de_dnp->dn_uid = uid;
			new_dev->de_dnp->dn_mode |= perms;
			new_dev->de_dnp->dn_clone = clone;
#if CONFIG_MACF
			mac_devfs_label_associate_device(dev, new_dev->de_dnp, buff);
#endif
			devfs_propogate(dnp->dn_typeinfo.Dir.myname, new_dev, &event_log);
		}
	}

out:
	DEVFS_UNLOCK();

	devfs_bulk_notify(&event_log);
	devfs_release_event_log(&event_log, need_free);
	return new_dev;
}

/*
 * Function: devfs_make_link
 *
 * Purpose:
 *   Create a link to a previously created device node.
 *
 * Returns:
 *   0 if successful, -1 if failed
 */
int
devfs_make_link(void *original, char *fmt, ...)
{
	devdirent_t *   new_dev = NULL;
	devdirent_t *   orig = (devdirent_t *) original;
	devnode_t *     dirnode;        /* devnode for parent directory */
	struct devfs_event_log event_log;
	uint32_t        log_count;

	va_list ap;
	char *p, buf[256]; /* XXX */
	int i;

	DEVFS_LOCK();

	if (!devfs_ready) {
		DEVFS_UNLOCK();
		printf("devfs_make_link: not ready for devices!\n");
		return -1;
	}
	DEVFS_UNLOCK();

	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	p = NULL;

	for (i = strlen(buf); i > 0; i--) {
		if (buf[i] == '/') {
			p = &buf[i];
			buf[i] = 0;
			break;
		}
	}

	/*
	 * One slot for each directory, one for each devnode
	 * whose link count changes
	 */
	log_count = devfs_nmountplanes * 2;
wrongsize:
	if (devfs_init_event_log(&event_log, log_count, NULL) != 0) {
		/* No lock held, no allocations done, can just return */
		return -1;
	}

	DEVFS_LOCK();

	if (log_count < devfs_nmountplanes) {
		DEVFS_UNLOCK();
		devfs_release_event_log(&event_log, 1);
		log_count = log_count * 2;
		goto wrongsize;
	}

	if (p) {
		*p++ = '\0';

		if (dev_finddir(buf, NULL, DEVFS_CREATE, &dirnode, &event_log)
		    || dev_add_name(p, dirnode, NULL, orig->de_dnp, &new_dev)) {
			goto fail;
		}
	} else {
		if (dev_finddir("", NULL, DEVFS_CREATE, &dirnode, &event_log)
		    || dev_add_name(buf, dirnode, NULL, orig->de_dnp, &new_dev)) {
			goto fail;
		}
	}
	devfs_propogate(dirnode->dn_typeinfo.Dir.myname, new_dev, &event_log);
fail:
	DEVFS_UNLOCK();
	devfs_bulk_notify(&event_log);
	devfs_release_event_log(&event_log, 1);

	return (new_dev != NULL) ? 0 : -1;
}
