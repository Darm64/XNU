/*
 * Copyright (c) 2009-2014 Apple Inc. All rights reserved.
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
#include <string.h>
#include <sys/types.h>

#if KERNEL
    #include <libkern/kernel_mach_header.h>
    #include <mach/machine.h>
    #include <mach/vm_param.h>
    #include <mach-o/fat.h>
#else /* !KERNEL */
/* Get machine.h from the kernel source so we can support all platforms
 * that the kernel supports. Otherwise we're at the mercy of the host.
 */
    #include "../../osfmk/mach/machine.h"

    #include <architecture/byte_order.h>
    #include <mach/mach_init.h>
    #include <mach-o/arch.h>
    #include <mach-o/swap.h>
#endif /* KERNEL */

#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/reloc.h>
#include <os/overflow.h>

#define DEBUG_ASSERT_COMPONENT_NAME_STRING "kxld"
#include <AssertMacros.h>

#include "kxld_demangle.h"
#include "kxld_dict.h"
#include "kxld_reloc.h"
#include "kxld_sect.h"
#include "kxld_seg.h"
#include "kxld_srcversion.h"
#include "kxld_symtab.h"
#include "kxld_util.h"
#include "kxld_uuid.h"
#include "kxld_versionmin.h"
#include "kxld_vtable.h"
#include "kxld_splitinfolc.h"

#include "kxld_object.h"

extern boolean_t isSplitKext;
extern boolean_t isOldInterface;

/*******************************************************************************
* Data structures
*******************************************************************************/

struct kxld_object {
	u_char *file;   // used by old interface
	u_long size;    // used by old interface
	const char *name;
	uint32_t filetype;
	cpu_type_t cputype;
	cpu_subtype_t cpusubtype;
	KXLDArray segs;
	KXLDArray sects;
	KXLDArray extrelocs;
	KXLDArray locrelocs;
	KXLDRelocator relocator;
	KXLDuuid uuid;
	KXLDversionmin versionmin;
	KXLDsrcversion srcversion;
	KXLDSymtab *symtab;
	struct dysymtab_command *dysymtab_hdr;
	KXLDsplitinfolc splitinfolc;
	splitKextLinkInfo split_info;
	kxld_addr_t link_addr;
	u_long    output_buffer_size;
	boolean_t is_kernel;
	boolean_t is_final_image;
	boolean_t is_linked;
	boolean_t got_is_created;
#if KXLD_USER_OR_OBJECT
	KXLDArray *section_order;
#endif
#if KXLD_PIC_KEXTS
	boolean_t include_kaslr_relocs;
#endif
#if !KERNEL
	enum NXByteOrder host_order;
	enum NXByteOrder target_order;
#endif
};

/*******************************************************************************
* Prototypes
*******************************************************************************/

static kern_return_t get_target_machine_info(KXLDObject *object,
    cpu_type_t cputype, cpu_subtype_t cpusubtype);
static kern_return_t get_macho_slice_for_arch(KXLDObject *object,
    u_char *file, u_long size);

static u_long get_macho_header_size(const KXLDObject *object);
static u_long get_macho_data_size(const KXLDObject *object) __unused;

static kern_return_t init_from_execute(KXLDObject *object);
static kern_return_t init_from_final_linked_image(KXLDObject *object,
    u_int *filetype_out, struct symtab_command **symtab_hdr_out);

static boolean_t target_supports_protected_segments(const KXLDObject *object)
__attribute__((pure));
static void set_is_object_linked(KXLDObject *object);

#if KXLD_USER_OR_BUNDLE
static boolean_t target_supports_bundle(const KXLDObject *object)
__attribute((pure));
static kern_return_t init_from_bundle(KXLDObject *object);
static kern_return_t process_relocs_from_tables(KXLDObject *object);
static KXLDSeg *get_seg_by_base_addr(KXLDObject *object,
    kxld_addr_t base_addr);
static kern_return_t process_symbol_pointers(KXLDObject *object);
static void add_to_ptr(u_char *symptr, kxld_addr_t val, boolean_t is_32_bit);
#endif /* KXLD_USER_OR_BUNDLE */

#if KXLD_USER_OR_OBJECT
static boolean_t target_supports_object(const KXLDObject *object)
__attribute((pure));
static kern_return_t init_from_object(KXLDObject *object);
static kern_return_t process_relocs_from_sections(KXLDObject *object);
#endif /* KXLD_USER_OR_OBJECT */

#if KXLD_PIC_KEXTS
static boolean_t target_supports_slideable_kexts(const KXLDObject *object);
#endif  /* KXLD_PIC_KEXTS */


static kern_return_t export_macho_header(const KXLDObject *object, u_char *buf,
    u_int ncmds, u_long *header_offset, u_long header_size);
#if KXLD_USER_OR_ILP32
static u_long get_macho_cmd_data_32(u_char *file, u_long offset,
    u_int *filetype, u_int *ncmds);
static kern_return_t export_macho_header_32(const KXLDObject *object,
    u_char *buf, u_int ncmds, u_long *header_offset, u_long header_size);
#endif /* KXLD_USER_OR_ILP32 */
#if KXLD_USER_OR_LP64
static u_long get_macho_cmd_data_64(u_char *file, u_long offset,
    u_int *filetype, u_int *ncmds);
static kern_return_t export_macho_header_64(const KXLDObject *object,
    u_char *buf, u_int ncmds, u_long *header_offset, u_long header_size);
#endif /* KXLD_USER_OR_LP64 */

#if KXLD_USER_OR_GOT || KXLD_USER_OR_COMMON
static kern_return_t add_section(KXLDObject *object, KXLDSect **sect);
#endif /* KXLD_USER_OR_GOT || KXLD_USER_OR_COMMON */

#if KXLD_USER_OR_COMMON
static kern_return_t resolve_common_symbols(KXLDObject *object);
#endif /* KXLD_USER_OR_COMMON */

#if KXLD_USER_OR_GOT
static boolean_t target_has_got(const KXLDObject *object) __attribute__((pure));
static kern_return_t create_got(KXLDObject *object);
static kern_return_t populate_got(KXLDObject *object);
#endif /* KXLD_USER_OR_GOT */

static KXLDSym *get_mutable_sym(const KXLDObject *object, const KXLDSym *sym);

static kern_return_t populate_kmod_info(KXLDObject *object);

/*******************************************************************************
* Prototypes that may need to be exported
*******************************************************************************/
static boolean_t kxld_object_target_needs_swap(const KXLDObject *object __unused);
static KXLDSeg * kxld_object_get_seg_by_name(const KXLDObject *object, const char *segname);
static KXLDSect * kxld_object_get_sect_by_name(const KXLDObject *object, const char *segname,
    const char *sectname);

/*******************************************************************************
*******************************************************************************/
size_t
kxld_object_sizeof(void)
{
	return sizeof(KXLDObject);
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_init_from_macho(KXLDObject *object, u_char *file, u_long size,
    const char *name, KXLDArray *section_order __unused,
    cpu_type_t cputype, cpu_subtype_t cpusubtype, KXLDFlags flags __unused)
{
	kern_return_t       rval    = KERN_FAILURE;
	KXLDSeg           * seg     = NULL;
	u_int               i       = 0;
	u_char *            my_file;

	check(object);
	check(file);
	check(name);

	object->name = name;

#if KXLD_USER_OR_OBJECT
	object->section_order = section_order;
#endif
#if KXLD_PIC_KEXTS
	object->include_kaslr_relocs = ((flags & kKXLDFlagIncludeRelocs) == kKXLDFlagIncludeRelocs);
#endif

	/* Find the local architecture */

	rval = get_target_machine_info(object, cputype, cpusubtype);
	require_noerr(rval, finish);

	/* Find the Mach-O slice for the target architecture */

	rval = get_macho_slice_for_arch(object, file, size);
	require_noerr(rval, finish);

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

	/* Allocate the symbol table */

	if (!object->symtab) {
		object->symtab = kxld_calloc(kxld_symtab_sizeof());
		require_action(object->symtab, finish, rval = KERN_RESOURCE_SHORTAGE);
	}

	/* Build the relocator */

	rval = kxld_relocator_init(&object->relocator,
	    my_file,
	    object->symtab, &object->sects,
	    object->cputype,
	    object->cpusubtype,
	    kxld_object_target_needs_swap(object));
	require_noerr(rval, finish);

	/* There are four types of Mach-O files that we can support:
	 *   1) 32-bit MH_OBJECT      - Snow Leopard and earlier
	 *   2) 32-bit MH_KEXT_BUNDLE - Lion and Later
	 *   3) 64-bit MH_OBJECT      - Unsupported
	 *   4) 64-bit MH_KEXT_BUNDLE - Snow Leopard and Later
	 */

	if (kxld_object_is_32_bit(object)) {
		struct mach_header *mach_hdr = (struct mach_header *) ((void *) my_file);
		object->filetype = mach_hdr->filetype;
	} else {
		struct mach_header_64 *mach_hdr = (struct mach_header_64 *) ((void *) my_file);
		object->filetype = mach_hdr->filetype;
	}

	switch (object->filetype) {
#if KXLD_USER_OR_BUNDLE
	case MH_KEXT_BUNDLE:
		rval = init_from_bundle(object);
		require_noerr(rval, finish);
		break;
#endif /* KXLD_USER_OR_BUNDLE */
#if KXLD_USER_OR_OBJECT
	case MH_OBJECT:
		rval = init_from_object(object);
		require_noerr(rval, finish);
		break;
#endif /* KXLD_USER_OR_OBJECT */
	case MH_EXECUTE:
		object->is_kernel = TRUE;
		rval = init_from_execute(object);
		require_noerr(rval, finish);
		break;
	default:
		rval = KERN_FAILURE;
		kxld_log(kKxldLogLinking, kKxldLogErr,
		    kKxldLogFiletypeNotSupported, object->filetype);
		goto finish;
	}

	if (!kxld_object_is_kernel(object)) {
		for (i = 0; i < object->segs.nitems; ++i) {
			seg = kxld_array_get_item(&object->segs, i);
			kxld_seg_set_vm_protections(seg,
			    target_supports_protected_segments(object));
		}

		seg = kxld_object_get_seg_by_name(object, SEG_LINKEDIT);
		if (seg) {
			(void) kxld_seg_populate_linkedit(seg, object->symtab,
			    kxld_object_is_32_bit(object)
#if KXLD_PIC_KEXTS
			    , &object->locrelocs, &object->extrelocs,
			    target_supports_slideable_kexts(object)
#endif
			    , isOldInterface ? 0 : object->splitinfolc.datasize
			    );
		}
	}

	(void) set_is_object_linked(object);

	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
splitKextLinkInfo *
kxld_object_get_link_info(KXLDObject *object)
{
	check(object);

	return &object->split_info;
}


/*******************************************************************************
*******************************************************************************/
void
kxld_object_set_link_info(KXLDObject *object, splitKextLinkInfo *link_info)
{
	check(object);
	check(link_info);

	object->split_info.vmaddr_TEXT = link_info->vmaddr_TEXT;
	object->split_info.vmaddr_TEXT_EXEC = link_info->vmaddr_TEXT_EXEC;
	object->split_info.vmaddr_DATA = link_info->vmaddr_DATA;
	object->split_info.vmaddr_DATA_CONST = link_info->vmaddr_DATA_CONST;
	object->split_info.vmaddr_LLVM_COV = link_info->vmaddr_LLVM_COV;
	object->split_info.vmaddr_LINKEDIT = link_info->vmaddr_LINKEDIT;

	return;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
get_target_machine_info(KXLDObject *object, cpu_type_t cputype __unused,
    cpu_subtype_t cpusubtype __unused)
{
#if KERNEL

	/* Because the kernel can only link for its own architecture, we know what
	 * the host and target architectures are at compile time, so we can use
	 * a vastly simplified version of this function.
	 */

	check(object);

#if   defined(__x86_64__)
	object->cputype = CPU_TYPE_X86_64;
/* FIXME: we need clang to provide a __x86_64h__ macro for the sub-type. Using
 * __AVX2__ is a temporary solution until this is available. */
#if   defined(__AVX2__)
	object->cpusubtype = CPU_SUBTYPE_X86_64_H;
#else
	object->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
#endif
	return KERN_SUCCESS;
#elif defined(__arm__)
	object->cputype = CPU_TYPE_ARM;
	object->cpusubtype = CPU_SUBTYPE_ARM_ALL;
	return KERN_SUCCESS;
#elif defined(__arm64__)
	object->cputype = CPU_TYPE_ARM64;
	object->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
	return KERN_SUCCESS;
#else
	kxld_log(kKxldLogLinking, kKxldLogErr,
	    kKxldLogArchNotSupported, _mh_execute_header->cputype);
	return KERN_NOT_SUPPORTED;
#endif /* Supported architecture defines */


#else /* !KERNEL */

	/* User-space must look up the architecture it's running on and the target
	 * architecture at run-time.
	 */

	kern_return_t rval = KERN_FAILURE;
	const NXArchInfo *host_arch = NULL;

	check(object);

	host_arch = NXGetLocalArchInfo();
	require_action(host_arch, finish, rval = KERN_FAILURE);

	object->host_order = host_arch->byteorder;

	/* If the user did not specify a cputype, use the local architecture.
	 */

	if (cputype) {
		object->cputype = cputype;
		object->cpusubtype = cpusubtype;
	} else {
		object->cputype = host_arch->cputype;
		object->target_order = object->host_order;

		switch (object->cputype) {
		case CPU_TYPE_I386:
			object->cpusubtype = CPU_SUBTYPE_I386_ALL;
			break;
		case CPU_TYPE_X86_64:
			object->cpusubtype = CPU_SUBTYPE_X86_64_ALL;
			break;
		case CPU_TYPE_ARM:
			object->cpusubtype = CPU_SUBTYPE_ARM_ALL;
			break;
		case CPU_TYPE_ARM64:
			object->cpusubtype = CPU_SUBTYPE_ARM64_ALL;
			break;
		default:
			object->cpusubtype = 0;
			break;
		}
	}

	/* Validate that we support the target architecture and record its
	 * endianness.
	 */

	switch (object->cputype) {
	case CPU_TYPE_ARM:
	case CPU_TYPE_ARM64:
	case CPU_TYPE_I386:
	case CPU_TYPE_X86_64:
		object->target_order = NX_LittleEndian;
		break;
	default:
		rval = KERN_NOT_SUPPORTED;
		kxld_log(kKxldLogLinking, kKxldLogErr,
		    kKxldLogArchNotSupported, object->cputype);
		goto finish;
	}

	rval = KERN_SUCCESS;

finish:
	return rval;
#endif /* KERNEL */
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
get_macho_slice_for_arch(KXLDObject *object, u_char *file, u_long size)
{
	kern_return_t rval = KERN_FAILURE;
	struct mach_header *mach_hdr = NULL;
#if !KERNEL
	struct fat_header *fat = (struct fat_header *) ((void *) file);
	struct fat_arch *archs = (struct fat_arch *) &fat[1];
	boolean_t swap = FALSE;
#endif /* KERNEL */
	u_char *my_file = file;
	u_long my_file_size = size;

	check(object);
	check(file);
	check(size);

	/* We are assuming that we will never receive a fat file in the kernel */

#if !KERNEL
	require_action(size >= sizeof(*fat), finish,
	    rval = KERN_FAILURE;
	    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogTruncatedMachO));

	/* The fat header is always big endian, so swap if necessary */
	if (fat->magic == FAT_CIGAM) {
		(void) swap_fat_header(fat, object->host_order);
		swap = TRUE;
	}

	if (fat->magic == FAT_MAGIC) {
		struct fat_arch *arch = NULL;
		u_long arch_size;
		boolean_t ovr = os_mul_and_add_overflow(fat->nfat_arch, sizeof(*archs), sizeof(*fat), &arch_size);

		require_action(!ovr && size >= arch_size,
		    finish,
		    rval = KERN_FAILURE;
		    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogTruncatedMachO));

		/* Swap the fat_arch structures if necessary */
		if (swap) {
			(void) swap_fat_arch(archs, fat->nfat_arch, object->host_order);
		}

		/* Locate the Mach-O for the requested architecture */

		arch = NXFindBestFatArch(object->cputype, object->cpusubtype, archs, fat->nfat_arch);
		require_action(arch, finish, rval = KERN_FAILURE;
		    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogArchNotFound));
		require_action(size >= arch->offset + arch->size, finish,
		    rval = KERN_FAILURE;
		    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogTruncatedMachO));

		my_file = my_file + arch->offset;
		my_file_size = arch->size;
	}
#endif /* !KERNEL */

	/* Swap the Mach-O's headers to this architecture if necessary */
	if (kxld_object_is_32_bit(object)) {
		rval = validate_and_swap_macho_32(my_file, my_file_size
#if !KERNEL
		    , object->host_order
#endif /* !KERNEL */
		    );
	} else {
		rval = validate_and_swap_macho_64(my_file, my_file_size
#if !KERNEL
		    , object->host_order
#endif /* !KERNEL */
		    );
	}
	require_noerr(rval, finish);

	mach_hdr = (struct mach_header *) ((void *) my_file);
	require_action(object->cputype == mach_hdr->cputype, finish,
	    rval = KERN_FAILURE;
	    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogTruncatedMachO));
	object->cpusubtype = mach_hdr->cpusubtype; /* <rdar://problem/16008438> */

	if (isOldInterface) {
		object->file = my_file;
		object->size = my_file_size;
	} else {
		object->split_info.kextExecutable = my_file;
		object->split_info.kextSize = my_file_size;
	}

	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
init_from_final_linked_image(KXLDObject *object, u_int *filetype_out,
    struct symtab_command **symtab_hdr_out)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSeg *seg = NULL;
	KXLDSect *sect = NULL;
	struct load_command *cmd_hdr = NULL;
	struct symtab_command *symtab_hdr = NULL;
	struct uuid_command *uuid_hdr = NULL;
	struct version_min_command *versionmin_hdr = NULL;
	struct build_version_command *build_version_hdr = NULL;
	struct source_version_command *source_version_hdr = NULL;
	u_long base_offset = 0;
	u_long offset = 0;
	u_long sect_offset = 0;
	u_int filetype = 0;
	u_int i = 0;
	u_int j = 0;
	u_int segi = 0;
	u_int secti = 0;
	u_int nsegs = 0;
	u_int nsects = 0;
	u_int ncmds = 0;
	u_char *my_file;

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

	KXLD_3264_FUNC(kxld_object_is_32_bit(object), base_offset,
	    get_macho_cmd_data_32, get_macho_cmd_data_64,
	    my_file, offset, &filetype, &ncmds);

	/* First pass to count segments and sections */

	offset = base_offset;
	for (i = 0; i < ncmds; ++i, offset += cmd_hdr->cmdsize) {
		cmd_hdr = (struct load_command *) ((void *) (my_file + offset));

		switch (cmd_hdr->cmd) {
#if KXLD_USER_OR_ILP32
		case LC_SEGMENT:
		{
			struct segment_command *seg_hdr =
			    (struct segment_command *) cmd_hdr;

			/* Ignore segments with no vm size */
			if (!seg_hdr->vmsize) {
				continue;
			}

			++nsegs;
			nsects += seg_hdr->nsects;
		}
		break;
#endif /* KXLD_USER_OR_ILP32 */
#if KXLD_USER_OR_LP64
		case LC_SEGMENT_64:
		{
			struct segment_command_64 *seg_hdr =
			    (struct segment_command_64 *) ((void *) cmd_hdr);

			/* Ignore segments with no vm size */
			if (!seg_hdr->vmsize) {
				continue;
			}

			++nsegs;
			nsects += seg_hdr->nsects;
		}
		break;
#endif /* KXLD_USER_OR_LP64 */
		default:
			continue;
		}
	}

	/* Allocate the segments and sections */

	if (nsegs) {
		rval = kxld_array_init(&object->segs, sizeof(KXLDSeg), nsegs);
		require_noerr(rval, finish);

		rval = kxld_array_init(&object->sects, sizeof(KXLDSect), nsects);
		require_noerr(rval, finish);
	}

	/* Initialize the segments and sections */

	offset = base_offset;
	for (i = 0; i < ncmds; ++i, offset += cmd_hdr->cmdsize) {
		cmd_hdr = (struct load_command *) ((void *) (my_file + offset));
		seg = NULL;

		switch (cmd_hdr->cmd) {
#if KXLD_USER_OR_ILP32
		case LC_SEGMENT:
		{
			struct segment_command *seg_hdr =
			    (struct segment_command *) cmd_hdr;

			/* Ignore segments with no vm size */
			if (!seg_hdr->vmsize) {
				continue;
			}

			seg = kxld_array_get_item(&object->segs, segi++);

			rval = kxld_seg_init_from_macho_32(seg, seg_hdr);
			require_noerr(rval, finish);

			sect_offset = offset + sizeof(*seg_hdr);
		}
		break;
#endif /* KXLD_USER_OR_ILP32 */
#if KXLD_USER_OR_LP64
		case LC_SEGMENT_64:
		{
			struct segment_command_64 *seg_hdr =
			    (struct segment_command_64 *) ((void *) cmd_hdr);

			/* Ignore segments with no vm size */
			if (!seg_hdr->vmsize) {
				continue;
			}

			seg = kxld_array_get_item(&object->segs, segi++);

			rval = kxld_seg_init_from_macho_64(seg, seg_hdr);
			require_noerr(rval, finish);

			sect_offset = offset + sizeof(*seg_hdr);
		}
		break;
#endif /* KXLD_USER_OR_LP64 */
		case LC_SYMTAB:
			symtab_hdr = (struct symtab_command *) cmd_hdr;
			break;
		case LC_UUID:
			uuid_hdr = (struct uuid_command *) cmd_hdr;
			kxld_uuid_init_from_macho(&object->uuid, uuid_hdr);
			break;
		case LC_VERSION_MIN_MACOSX:
		case LC_VERSION_MIN_IPHONEOS:
		case LC_VERSION_MIN_TVOS:
		case LC_VERSION_MIN_WATCHOS:
			versionmin_hdr = (struct version_min_command *) cmd_hdr;
			kxld_versionmin_init_from_macho(&object->versionmin, versionmin_hdr);
			break;
		case LC_BUILD_VERSION:
			build_version_hdr = (struct build_version_command *)cmd_hdr;
			kxld_versionmin_init_from_build_cmd(&object->versionmin, build_version_hdr);
			break;
		case LC_SOURCE_VERSION:
			source_version_hdr = (struct source_version_command *) (void *) cmd_hdr;
			kxld_srcversion_init_from_macho(&object->srcversion, source_version_hdr);
			break;
		case LC_DYSYMTAB:
			object->dysymtab_hdr = (struct dysymtab_command *) cmd_hdr;
			rval = kxld_reloc_create_macho(&object->extrelocs, &object->relocator,
			    (struct relocation_info *) ((void *) (my_file + object->dysymtab_hdr->extreloff)),
			    object->dysymtab_hdr->nextrel);
			require_noerr(rval, finish);

			rval = kxld_reloc_create_macho(&object->locrelocs, &object->relocator,
			    (struct relocation_info *) ((void *) (my_file + object->dysymtab_hdr->locreloff)),
			    object->dysymtab_hdr->nlocrel);
			require_noerr(rval, finish);

			break;
		case LC_UNIXTHREAD:
		case LC_MAIN:
			/* Don't need to do anything with UNIXTHREAD or MAIN for the kernel */
			require_action(kxld_object_is_kernel(object),
			    finish, rval = KERN_FAILURE;
			    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
			    "LC_UNIXTHREAD/LC_MAIN segment is not valid in a kext."));
			break;
		case LC_SEGMENT_SPLIT_INFO:
			if (isSplitKext) {
				struct linkedit_data_command *split_info_hdr = NULL;
				split_info_hdr = (struct linkedit_data_command *) (void *) cmd_hdr;
				kxld_splitinfolc_init_from_macho(&object->splitinfolc, split_info_hdr);
			}
			break;
		case LC_NOTE:
			/* binary blob of data */
			break;
		case LC_CODE_SIGNATURE:
		case LC_DYLD_INFO:
		case LC_DYLD_INFO_ONLY:
		case LC_FUNCTION_STARTS:
		case LC_DATA_IN_CODE:
		case LC_DYLIB_CODE_SIGN_DRS:
			/* Various metadata that might be stored in the linkedit segment */
			break;
		default:
			rval = KERN_FAILURE;
			kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
			    "Invalid load command type in MH_KEXT_BUNDLE kext: %u.", cmd_hdr->cmd);
			goto finish;
		}

		if (seg) {
			/* Initialize the sections */
			for (j = 0; j < seg->sects.nitems; ++j, ++secti) {
				sect = kxld_array_get_item(&object->sects, secti);

				KXLD_3264_FUNC(kxld_object_is_32_bit(object), rval,
				    kxld_sect_init_from_macho_32, kxld_sect_init_from_macho_64,
				    sect, my_file, &sect_offset, secti, &object->relocator);
				require_noerr(rval, finish);

				/* Add the section to the segment.  This will also make sure
				 * that the sections and segments have the same segname.
				 */
				rval = kxld_seg_add_section(seg, sect);
				require_noerr(rval, finish);
			}
			rval = kxld_seg_finish_init(seg);
			require_noerr(rval, finish);
		}
	}

	if (filetype_out) {
		*filetype_out = filetype;
	}
	if (symtab_hdr_out) {
		*symtab_hdr_out = symtab_hdr;
	}
	object->is_final_image = TRUE;
	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
init_from_execute(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	struct symtab_command *symtab_hdr = NULL;
	u_int filetype = 0;
	KXLDSeg * kernel_linkedit_seg = NULL; // used if running kernel
#if KXLD_USER_OR_OBJECT
	KXLDSeg *seg = NULL;
	KXLDSect *sect = NULL;
	KXLDSectionName *sname = NULL;
	u_int i = 0, j = 0, k = 0;
#endif /* KXLD_USER_OR_OBJECT */
	u_char *my_file;

	check(object);

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

	require_action(kxld_object_is_kernel(object), finish, rval = KERN_FAILURE);

	rval = init_from_final_linked_image(object, &filetype, &symtab_hdr);
	require_noerr(rval, finish);

	require_action(filetype == MH_EXECUTE, finish, rval = KERN_FAILURE;
	    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
	    "The kernel file is not of type MH_EXECUTE."));

	/* Initialize the symbol table. If this is the running kernel
	 * we  will work from the in-memory linkedit segment;
	 * otherwise we work from the whole mach-o image.
	 */
#if KERNEL
	kernel_linkedit_seg = kxld_object_get_seg_by_name(object, SEG_LINKEDIT);
	require_action(kernel_linkedit_seg, finish, rval = KERN_FAILURE;
	    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO));
#endif

	KXLD_3264_FUNC(kxld_object_is_32_bit(object), rval,
	    kxld_symtab_init_from_macho_32, kxld_symtab_init_from_macho_64,
	    object->symtab, symtab_hdr, my_file, kernel_linkedit_seg);
	require_noerr(rval, finish);

#if KXLD_USER_OR_OBJECT
	/* Save off the order of section names so that we can lay out kext
	 * sections for MH_OBJECT-based systems.
	 */
	if (target_supports_object(object)) {
		rval = kxld_array_init(object->section_order, sizeof(KXLDSectionName),
		    object->sects.nitems);
		require_noerr(rval, finish);

		/* Copy the section names into the section_order array for future kext
		 * section ordering.
		 */
		for (i = 0, k = 0; i < object->segs.nitems; ++i) {
			seg = kxld_array_get_item(&object->segs, i);

			for (j = 0; j < seg->sects.nitems; ++j, ++k) {
				sect = *(KXLDSect **) kxld_array_get_item(&seg->sects, j);
				sname = kxld_array_get_item(object->section_order, k);

				strlcpy(sname->segname, sect->segname, sizeof(sname->segname));
				strlcpy(sname->sectname, sect->sectname, sizeof(sname->sectname));
			}
		}
	}
#endif /* KXLD_USER_OR_OBJECT */

	rval = KERN_SUCCESS;
finish:
	return rval;
}

#if KXLD_USER_OR_BUNDLE
/*******************************************************************************
*******************************************************************************/
static boolean_t
target_supports_bundle(const KXLDObject *object __unused)
{
	return TRUE;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
init_from_bundle(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	struct symtab_command *symtab_hdr = NULL;
	u_int filetype = 0;
	u_char *my_file;

	check(object);

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

	require_action(target_supports_bundle(object), finish,
	    rval = KERN_FAILURE;
	    kxld_log(kKxldLogLinking, kKxldLogErr,
	    kKxldLogFiletypeNotSupported, MH_KEXT_BUNDLE));

	rval = init_from_final_linked_image(object, &filetype, &symtab_hdr);
	require_noerr(rval, finish);

	require_action(filetype == MH_KEXT_BUNDLE, finish,
	    rval = KERN_FAILURE);

	KXLD_3264_FUNC(kxld_object_is_32_bit(object), rval,
	    kxld_symtab_init_from_macho_32, kxld_symtab_init_from_macho_64,
	    object->symtab, symtab_hdr, my_file,
	    /* kernel_linkedit_seg */ NULL);
	require_noerr(rval, finish);

	rval = KERN_SUCCESS;
finish:
	return rval;
}
#endif /* KXLD_USER_OR_BUNDLE */

#if KXLD_USER_OR_OBJECT
/*******************************************************************************
*******************************************************************************/
static boolean_t
target_supports_object(const KXLDObject *object)
{
	return object->cputype == CPU_TYPE_I386;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
init_from_object(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	struct load_command *cmd_hdr = NULL;
	struct symtab_command *symtab_hdr = NULL;
	struct uuid_command *uuid_hdr = NULL;
	KXLDSect *sect = NULL;
	u_long offset = 0;
	u_long sect_offset = 0;
	u_int filetype = 0;
	u_int ncmds = 0;
	u_int nsects = 0;
	u_int i = 0;
	boolean_t has_segment = FALSE;
	u_char *my_file;

	check(object);

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

	require_action(target_supports_object(object),
	    finish, rval = KERN_FAILURE;
	    kxld_log(kKxldLogLinking, kKxldLogErr,
	    kKxldLogFiletypeNotSupported, MH_OBJECT));

	KXLD_3264_FUNC(kxld_object_is_32_bit(object), offset,
	    get_macho_cmd_data_32, get_macho_cmd_data_64,
	    my_file, offset, &filetype, &ncmds);

	require_action(filetype == MH_OBJECT, finish, rval = KERN_FAILURE);

	/* MH_OBJECTs use one unnamed segment to contain all of the sections.  We
	 * loop over all of the load commands to initialize the structures we
	 * expect.  Then, we'll use the unnamed segment to get to all of the
	 * sections, and then use those sections to create the actual segments.
	 */

	for (; i < ncmds; ++i, offset += cmd_hdr->cmdsize) {
		cmd_hdr = (struct load_command *) ((void *) (my_file + offset));

		switch (cmd_hdr->cmd) {
#if KXLD_USER_OR_ILP32
		case LC_SEGMENT:
		{
			struct segment_command *seg_hdr =
			    (struct segment_command *) cmd_hdr;

			/* Ignore segments with no vm size */
			if (!seg_hdr->vmsize) {
				continue;
			}

			/* Ignore LINKEDIT segments */
			if (streq_safe(seg_hdr->segname, SEG_LINKEDIT,
			    const_strlen(SEG_LINKEDIT))) {
				continue;
			}

			require_action(kxld_object_is_32_bit(object), finish, rval = KERN_FAILURE;
			    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
			    "LC_SEGMENT in 64-bit kext."));
			require_action(!has_segment, finish, rval = KERN_FAILURE;
			    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
			    "Multiple segments in an MH_OBJECT kext."));

			nsects = seg_hdr->nsects;
			sect_offset = offset + sizeof(*seg_hdr);
			has_segment = TRUE;
		}
		break;
#endif /* KXLD_USER_OR_ILP32 */
#if KXLD_USER_OR_LP64
		case LC_SEGMENT_64:
		{
			struct segment_command_64 *seg_hdr =
			    (struct segment_command_64 *) ((void *) cmd_hdr);

			/* Ignore segments with no vm size */
			if (!seg_hdr->vmsize) {
				continue;
			}

			/* Ignore LINKEDIT segments */
			if (streq_safe(seg_hdr->segname, SEG_LINKEDIT,
			    const_strlen(SEG_LINKEDIT))) {
				continue;
			}

			require_action(!kxld_object_is_32_bit(object), finish, rval = KERN_FAILURE;
			    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
			    "LC_SEGMENT_64 in a 32-bit kext."));
			require_action(!has_segment, finish, rval = KERN_FAILURE;
			    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
			    "Multiple segments in an MH_OBJECT kext."));

			nsects = seg_hdr->nsects;
			sect_offset = offset + sizeof(*seg_hdr);
			has_segment = TRUE;
		}
		break;
#endif /* KXLD_USER_OR_LP64 */
		case LC_SYMTAB:
			symtab_hdr = (struct symtab_command *) cmd_hdr;

			KXLD_3264_FUNC(kxld_object_is_32_bit(object), rval,
			    kxld_symtab_init_from_macho_32, kxld_symtab_init_from_macho_64,
			    object->symtab, symtab_hdr, my_file,
			    /* kernel_linkedit_seg */ NULL);
			require_noerr(rval, finish);
			break;
		case LC_UUID:
			uuid_hdr = (struct uuid_command *) cmd_hdr;
			kxld_uuid_init_from_macho(&object->uuid, uuid_hdr);
			break;
		case LC_UNIXTHREAD:
		case LC_MAIN:
			/* Don't need to do anything with UNIXTHREAD or MAIN */
			break;
		case LC_CODE_SIGNATURE:
		case LC_DYLD_INFO:
		case LC_DYLD_INFO_ONLY:
		case LC_FUNCTION_STARTS:
		case LC_DATA_IN_CODE:
		case LC_DYLIB_CODE_SIGN_DRS:
			/* Various metadata that might be stored in the linkedit segment */
			break;
		case LC_NOTE:
			/* bag-of-bits carried with the binary: ignore */
			break;
		case LC_BUILD_VERSION:
			/* should be able to ignore build version commands */
			kxld_log(kKxldLogLinking, kKxldLogWarn,
			    "Ignoring LC_BUILD_VERSION (%u) in MH_OBJECT kext: (platform:%d)",
			    cmd_hdr->cmd, ((struct build_version_command *)cmd_hdr)->platform);
			break;
		case LC_VERSION_MIN_MACOSX:
		case LC_VERSION_MIN_IPHONEOS:
		case LC_VERSION_MIN_TVOS:
		case LC_VERSION_MIN_WATCHOS:
		case LC_SOURCE_VERSION:
		/* Not supported for object files, fall through */
		default:
			rval = KERN_FAILURE;
			kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
			    "Invalid load command type in MH_OBJECT kext: %u.", cmd_hdr->cmd);
			goto finish;
		}
	}

	if (has_segment) {
		/* Get the number of sections from the segment and build the section index */

		rval = kxld_array_init(&object->sects, sizeof(KXLDSect), nsects);
		require_noerr(rval, finish);

		/* Loop over all of the sections to initialize the section index */

		for (i = 0; i < nsects; ++i) {
			sect = kxld_array_get_item(&object->sects, i);

			KXLD_3264_FUNC(kxld_object_is_32_bit(object), rval,
			    kxld_sect_init_from_macho_32, kxld_sect_init_from_macho_64,
			    sect, my_file, &sect_offset, i, &object->relocator);
			require_noerr(rval, finish);
		}

		/* Create special sections */

#if KXLD_USER_OR_GOT
		rval = create_got(object);
		require_noerr(rval, finish);
#endif /* KXLD_USER_OR_GOT */

#if KXLD_USER_OR_COMMON
		rval = resolve_common_symbols(object);
		require_noerr(rval, finish);
#endif /* KXLD_USER_OR_COMMON */

		/* Create the segments from the section index */

		rval = kxld_seg_create_seg_from_sections(&object->segs, &object->sects);
		require_noerr(rval, finish);

		rval = kxld_seg_finalize_object_segment(&object->segs,
		    object->section_order, get_macho_header_size(object));
		require_noerr(rval, finish);

		rval = kxld_seg_init_linkedit(&object->segs);
		require_noerr(rval, finish);
	}

	rval = KERN_SUCCESS;
finish:
	return rval;
}
#endif /* KXLD_USER_OR_OBJECT */

#if KXLD_USER_OR_ILP32
/*******************************************************************************
*******************************************************************************/
static u_long
get_macho_cmd_data_32(u_char *file, u_long offset, u_int *filetype, u_int *ncmds)
{
	struct mach_header *mach_hdr = (struct mach_header *) ((void *) (file + offset));

	if (filetype) {
		*filetype = mach_hdr->filetype;
	}
	if (ncmds) {
		*ncmds = mach_hdr->ncmds;
	}

	return sizeof(*mach_hdr);
}

#endif /* KXLD_USER_OR_ILP32 */

#if KXLD_USER_OR_LP64
/*******************************************************************************
*******************************************************************************/
static u_long
get_macho_cmd_data_64(u_char *file, u_long offset, u_int *filetype, u_int *ncmds)
{
	struct mach_header_64 *mach_hdr = (struct mach_header_64 *) ((void *) (file + offset));

	if (filetype) {
		*filetype = mach_hdr->filetype;
	}
	if (ncmds) {
		*ncmds = mach_hdr->ncmds;
	}

	return sizeof(*mach_hdr);
}
#endif /* KXLD_USER_OR_LP64 */

/*******************************************************************************
*******************************************************************************/
static u_long
get_macho_header_size(const KXLDObject *object)
{
	KXLDSeg *seg = NULL;
	u_long header_size = 0;
	u_int i = 0;
	boolean_t   object_is_32_bit = kxld_object_is_32_bit(object);

	check(object);

	/* Mach, segment, symtab, and UUID headers */

	header_size += object_is_32_bit ? sizeof(struct mach_header) : sizeof(struct mach_header_64);

	for (i = 0; i < object->segs.nitems; ++i) {
		seg = kxld_array_get_item(&object->segs, i);
		header_size += kxld_seg_get_macho_header_size(seg, object_is_32_bit);
	}

	header_size += kxld_symtab_get_macho_header_size();

#if KXLD_PIC_KEXTS
	if (target_supports_slideable_kexts(object)) {
		header_size += kxld_reloc_get_macho_header_size();
	}
#endif  /* KXLD_PIC_KEXTS */

	if (object->uuid.has_uuid) {
		header_size += kxld_uuid_get_macho_header_size();
	}

	if (object->versionmin.has_versionmin) {
		header_size += kxld_versionmin_get_macho_header_size(&object->versionmin);
	}

	if (object->srcversion.has_srcversion) {
		header_size += kxld_srcversion_get_macho_header_size();
	}

	if (isSplitKext && object->splitinfolc.has_splitinfolc) {
		header_size += kxld_splitinfolc_get_macho_header_size();
	}

	return header_size;
}

/*******************************************************************************
*******************************************************************************/
static u_long
get_macho_data_size(const KXLDObject *object)
{
	KXLDSeg *seg = NULL;
	u_long data_size = 0;
	u_int i = 0;

	check(object);

	/* total all segment vmsize values */
	for (i = 0; i < object->segs.nitems; ++i) {
		seg = kxld_array_get_item(&object->segs, i);
		data_size += (u_long) kxld_seg_get_vmsize(seg);
	}

#if KXLD_PIC_KEXTS
	{
		/* ensure that when we eventually emit the final linked object,
		 * appending the __DYSYMTAB data after the __LINKEDIT data will
		 * not overflow the space allocated for the __LINKEDIT segment
		 */

		u_long  seg_vmsize = 0;
		u_long  symtab_size = 0;
		u_long  reloc_size = 0;

		/* get current __LINKEDIT sizes */
		seg = kxld_object_get_seg_by_name(object, SEG_LINKEDIT);

		seg_vmsize = (u_long) kxld_seg_get_vmsize(seg);

		/* get size of symbol table data that will eventually be dumped
		 * into the __LINKEDIT segment
		 */
		symtab_size = kxld_symtab_get_macho_data_size(object->symtab, kxld_object_is_32_bit(object));

		if (target_supports_slideable_kexts(object)) {
			/* get size of __DYSYMTAB relocation entries */
			reloc_size = kxld_reloc_get_macho_data_size(&object->locrelocs, &object->extrelocs);
		}

		/* combine, and ensure they'll both fit within the page(s)
		 * allocated for the __LINKEDIT segment. If they'd overflow,
		 * increase the vmsize appropriately so no overflow will occur
		 */
		if ((symtab_size + reloc_size) > seg_vmsize) {
			u_long  overflow = (symtab_size + reloc_size) - seg_vmsize;
			data_size += kxld_round_page_cross_safe(overflow);
		}
	}
#endif  // KXLD_PIC_KEXTS

	return data_size;
}

/*******************************************************************************
*******************************************************************************/
boolean_t
kxld_object_target_needs_swap(const KXLDObject *object __unused)
{
#if KERNEL
	return FALSE;
#else
	return object->target_order != object->host_order;
#endif /* KERNEL */
}

/*******************************************************************************
*******************************************************************************/
KXLDSeg *
kxld_object_get_seg_by_name(const KXLDObject *object, const char *segname)
{
	KXLDSeg *seg = NULL;
	u_int i = 0;

	for (i = 0; i < object->segs.nitems; ++i) {
		seg = kxld_array_get_item(&object->segs, i);

		if (streq_safe(segname, seg->segname, sizeof(seg->segname))) {
			break;
		}

		seg = NULL;
	}

	return seg;
}

/*******************************************************************************
*******************************************************************************/
const KXLDRelocator *
kxld_object_get_relocator(const KXLDObject * object)
{
	check(object);

	return &object->relocator;
}

/*******************************************************************************
*******************************************************************************/
KXLDSect *
kxld_object_get_sect_by_name(const KXLDObject *object, const char *segname,
    const char *sectname)
{
	KXLDSect *sect = NULL;
	u_int i = 0;

	for (i = 0; i < object->sects.nitems; ++i) {
		sect = kxld_array_get_item(&object->sects, i);

		if (streq_safe(segname, sect->segname, sizeof(sect->segname)) &&
		    streq_safe(sectname, sect->sectname, sizeof(sect->sectname))) {
			break;
		}

		sect = NULL;
	}

	return sect;
}

/*******************************************************************************
*******************************************************************************/
const KXLDReloc *
kxld_object_get_reloc_at_symbol(const KXLDObject *object, const KXLDSym *sym)
{
	const KXLDReloc *reloc = NULL;
	const KXLDSect *sect = NULL;
	uint32_t offset = 0;

	check(object);
	check(sym);

	sect = kxld_object_get_section_by_index(object, sym->sectnum);
	require(sect, finish);

	if (kxld_object_is_final_image(object)) {
		reloc = kxld_reloc_get_reloc_by_offset(&object->extrelocs,
		    sym->base_addr);
		if (!reloc) {
			reloc = kxld_reloc_get_reloc_by_offset(&object->locrelocs,
			    sym->base_addr);
		}
	} else {
		offset = kxld_sym_get_section_offset(sym, sect);
		reloc = kxld_reloc_get_reloc_by_offset(&sect->relocs, offset);
	}

finish:
	return reloc;
}

/*******************************************************************************
*******************************************************************************/
const KXLDSym *
kxld_object_get_symbol_of_reloc(const KXLDObject *object,
    const KXLDReloc *reloc, const KXLDSect *sect)
{
	const KXLDSym *sym = NULL;
	u_char *my_file;

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

	if (kxld_object_is_final_image(object)) {
		sym = kxld_reloc_get_symbol(&object->relocator, reloc, my_file);
	} else {
		sym = kxld_reloc_get_symbol(&object->relocator, reloc, sect->data);
	}
	return sym;
}

/*******************************************************************************
*******************************************************************************/
const KXLDSect *
kxld_object_get_section_by_index(const KXLDObject *object, u_int sectnum)
{
	KXLDSect *sect = NULL;

	check(object);

	if (sectnum < object->sects.nitems) {
		sect = kxld_array_get_item(&object->sects, sectnum);
	}

	return sect;
}

/*******************************************************************************
*******************************************************************************/
const KXLDArray  *
kxld_object_get_extrelocs(const KXLDObject *object)
{
	const KXLDArray *rval = NULL;

	check(object);

	if (kxld_object_is_final_image(object)) {
		rval = &object->extrelocs;
	}

	return rval;
}

/*******************************************************************************
*******************************************************************************/
const KXLDSymtab *
kxld_object_get_symtab(const KXLDObject *object)
{
	check(object);

	return object->symtab;
}

#if KXLD_USER_OR_GOT || KXLD_USER_OR_COMMON
/*******************************************************************************
*******************************************************************************/
static kern_return_t
add_section(KXLDObject *object, KXLDSect **sect)
{
	kern_return_t rval = KERN_FAILURE;
	u_int nsects = object->sects.nitems;

	rval = kxld_array_resize(&object->sects, nsects + 1);
	require_noerr(rval, finish);

	*sect = kxld_array_get_item(&object->sects, nsects);

	rval = KERN_SUCCESS;

finish:
	return rval;
}
#endif /* KXLD_USER_OR_GOT || KXLD_USER_OR_COMMON */

#if KXLD_USER_OR_COMMON
/*******************************************************************************
* If there are common symbols, calculate how much space they'll need
* and create/grow the __DATA __common section to accommodate them.
* Then, resolve them against that section.
*******************************************************************************/
static kern_return_t
resolve_common_symbols(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSymtabIterator iter;
	KXLDSym *sym = NULL;
	KXLDSect *sect = NULL;
	kxld_addr_t base_addr = 0;
	kxld_size_t size = 0;
	kxld_size_t total_size = 0;
	u_int align = 0;
	u_int max_align = 0;
	u_int sectnum = 0;

	if (!kxld_object_target_supports_common_symbols(object)) {
		rval = KERN_SUCCESS;
		goto finish;
	}

	/* Iterate over the common symbols to calculate their total aligned size */
	kxld_symtab_iterator_init(&iter, object->symtab, kxld_sym_is_common, FALSE);
	while ((sym = kxld_symtab_iterator_get_next(&iter))) {
		align = kxld_sym_get_common_align(sym);
		size = kxld_sym_get_common_size(sym);

		if (align > max_align) {
			max_align = align;
		}

		total_size = kxld_align_address(total_size, align) + size;
	}

	/* If there are common symbols, grow or create the __DATA __common section
	 * to hold them.
	 */
	if (total_size) {
		sect = kxld_object_get_sect_by_name(object, SEG_DATA, SECT_COMMON);
		if (sect) {
			base_addr = sect->base_addr + sect->size;

			kxld_sect_grow(sect, total_size, max_align);
		} else {
			base_addr = 0;

			rval = add_section(object, &sect);
			require_noerr(rval, finish);

			kxld_sect_init_zerofill(sect, SEG_DATA, SECT_COMMON,
			    total_size, max_align);
		}

		/* Resolve the common symbols against the new section */
		rval = kxld_array_get_index(&object->sects, sect, &sectnum);
		require_noerr(rval, finish);

		kxld_symtab_iterator_reset(&iter);
		while ((sym = kxld_symtab_iterator_get_next(&iter))) {
			align = kxld_sym_get_common_align(sym);
			size = kxld_sym_get_common_size(sym);

			base_addr = kxld_align_address(base_addr, align);
			kxld_sym_resolve_common(sym, sectnum, base_addr);

			base_addr += size;
		}
	}

	rval = KERN_SUCCESS;

finish:
	return rval;
}
#endif /* KXLD_USER_OR_COMMON */

#if KXLD_USER_OR_GOT
/*******************************************************************************
*******************************************************************************/
static boolean_t
target_has_got(const KXLDObject *object)
{
	return FALSE:
}

/*******************************************************************************
* Create and initialize the Global Offset Table
*******************************************************************************/
static kern_return_t
create_got(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSect *sect = NULL;
	u_int ngots = 0;
	u_int i = 0;

	if (!target_has_got(object)) {
		rval = KERN_SUCCESS;
		goto finish;
	}

	for (i = 0; i < object->sects.nitems; ++i) {
		sect = kxld_array_get_item(&object->sects, i);
		ngots += kxld_sect_get_ngots(sect, &object->relocator,
		    object->symtab);
	}

	rval = add_section(object, &sect);
	require_noerr(rval, finish);

	rval = kxld_sect_init_got(sect, ngots);
	require_noerr(rval, finish);

	object->got_is_created = TRUE;
	rval = KERN_SUCCESS;

finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
populate_got(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSect *sect = NULL;
	u_int i = 0;

	if (!target_has_got(object) || !object->got_is_created) {
		rval = KERN_SUCCESS;
		goto finish;
	}

	for (i = 0; i < object->sects.nitems; ++i) {
		sect = kxld_array_get_item(&object->sects, i);
		if (streq_safe(sect->segname, KXLD_SEG_GOT, sizeof(KXLD_SEG_GOT)) &&
		    streq_safe(sect->sectname, KXLD_SECT_GOT, sizeof(KXLD_SECT_GOT))) {
			kxld_sect_populate_got(sect, object->symtab,
			    kxld_object_target_needs_swap(object));
			break;
		}
	}

	require_action(i < object->sects.nitems, finish, rval = KXLD_MISSING_GOT);

	rval = KERN_SUCCESS;

finish:
	return rval;
}
#endif /* KXLD_USER_OR_GOT */

/*******************************************************************************
*******************************************************************************/
static boolean_t
target_supports_protected_segments(const KXLDObject *object)
{
	return object->is_final_image &&
	       (object->cputype == CPU_TYPE_X86_64 ||
	       object->cputype == CPU_TYPE_ARM ||
	       object->cputype == CPU_TYPE_ARM64);
}

/*******************************************************************************
*******************************************************************************/
static void
set_is_object_linked(KXLDObject *object)
{
	u_int i = 0;

	if (kxld_object_is_kernel(object)) {
		object->is_linked = TRUE;
		return;
	}

	if (object->is_final_image) {
		object->is_linked = !object->extrelocs.nitems;
		return;
	}

	object->is_linked = TRUE;
	for (i = 0; i < object->sects.nitems; ++i) {
		KXLDSect *sect = kxld_array_get_item(&object->sects, i);
		if (sect->relocs.nitems) {
			object->is_linked = FALSE;
			break;
		}
	}
}


/*******************************************************************************
*******************************************************************************/
void
kxld_object_clear(KXLDObject *object)
{
	KXLDSeg *seg = NULL;
	KXLDSect *sect = NULL;
	u_int i;
	u_char *my_file;

	check(object);

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

#if !KERNEL
	if (kxld_object_is_kernel(object)) {
		unswap_macho(my_file, object->host_order, object->target_order);
	}
#endif /* !KERNEL */

	for (i = 0; i < object->segs.nitems; ++i) {
		seg = kxld_array_get_item(&object->segs, i);
		kxld_seg_clear(seg);
	}
	kxld_array_reset(&object->segs);

	for (i = 0; i < object->sects.nitems; ++i) {
		sect = kxld_array_get_item(&object->sects, i);
		kxld_sect_clear(sect);
	}
	kxld_array_reset(&object->sects);

	kxld_array_reset(&object->extrelocs);
	kxld_array_reset(&object->locrelocs);
	kxld_relocator_clear(&object->relocator);
	kxld_uuid_clear(&object->uuid);
	kxld_versionmin_clear(&object->versionmin);
	kxld_srcversion_clear(&object->srcversion);

	if (object->symtab) {
		kxld_symtab_clear(object->symtab);
	}

	if (isOldInterface) {
		object->file = NULL;
		object->size = 0;
	} else {
		kxld_splitinfolc_clear(&object->splitinfolc);
		object->split_info.kextExecutable = NULL;
		object->split_info.kextSize = 0;
	}
	object->filetype = 0;
	object->cputype = 0;
	object->cpusubtype = 0;
	object->is_kernel = FALSE;
	object->is_final_image = FALSE;
	object->is_linked = FALSE;
	object->got_is_created = FALSE;

#if KXLD_USER_OR_OBJECT
	object->section_order = NULL;
#endif
#if !KERNEL
	object->host_order = 0;
	object->target_order = 0;
#endif
}

/*******************************************************************************
*******************************************************************************/
void
kxld_object_deinit(KXLDObject *object __unused)
{
	KXLDSeg *seg = NULL;
	KXLDSect *sect = NULL;
	u_int i;
	u_char *my_file;

	check(object);

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

#if !KERNEL
	if (my_file && kxld_object_is_kernel(object)) {
		unswap_macho(my_file, object->host_order, object->target_order);
	}
#endif /* !KERNEL */

	for (i = 0; i < object->segs.maxitems; ++i) {
		seg = kxld_array_get_slot(&object->segs, i);
		kxld_seg_deinit(seg);
	}
	kxld_array_deinit(&object->segs);

	for (i = 0; i < object->sects.maxitems; ++i) {
		sect = kxld_array_get_slot(&object->sects, i);
		kxld_sect_deinit(sect);
	}
	kxld_array_deinit(&object->sects);

	kxld_array_deinit(&object->extrelocs);
	kxld_array_deinit(&object->locrelocs);

	if (object->symtab) {
		kxld_symtab_deinit(object->symtab);
		kxld_free(object->symtab, kxld_symtab_sizeof());
	}

	bzero(object, sizeof(*object));
}

/*******************************************************************************
*******************************************************************************/
const u_char *
kxld_object_get_file(const KXLDObject *object)
{
	const u_char *my_file;

	check(object);

	if (isOldInterface) {
		my_file = object->file;
	} else {
		my_file = object->split_info.kextExecutable;
	}

	return my_file;
}

/*******************************************************************************
*******************************************************************************/
const char *
kxld_object_get_name(const KXLDObject *object)
{
	check(object);

	return object->name;
}

/*******************************************************************************
*******************************************************************************/
boolean_t
kxld_object_is_32_bit(const KXLDObject *object)
{
	check(object);

	return kxld_is_32_bit(object->cputype);
}

/*******************************************************************************
*******************************************************************************/
boolean_t
kxld_object_is_final_image(const KXLDObject *object)
{
	check(object);

	return object->is_final_image;
}

/*******************************************************************************
*******************************************************************************/
boolean_t
kxld_object_is_kernel(const KXLDObject *object)
{
	check(object);

	return object->is_kernel;
}

/*******************************************************************************
*******************************************************************************/
boolean_t
kxld_object_is_linked(const KXLDObject *object)
{
	check(object);

	return object->is_linked;
}

/*******************************************************************************
*******************************************************************************/
boolean_t
kxld_object_target_supports_strict_patching(const KXLDObject *object)
{
	check(object);

	return object->cputype != CPU_TYPE_I386;
}

/*******************************************************************************
*******************************************************************************/
boolean_t
kxld_object_target_supports_common_symbols(const KXLDObject *object)
{
	check(object);

	return object->cputype == CPU_TYPE_I386;
}


/*******************************************************************************
*******************************************************************************/
void
kxld_object_get_vmsize_for_seg_by_name(const KXLDObject *object,
    const char *segname,
    u_long *vmsize)
{
	check(object);
	check(segname);
	check(vmsize);

	KXLDSeg *seg = NULL;
	u_long  my_size = 0;

	/* segment vmsize */
	seg = kxld_object_get_seg_by_name(object, segname);

	my_size = (u_long) kxld_seg_get_vmsize(seg);

#if KXLD_PIC_KEXTS
	if (kxld_seg_is_linkedit_seg(seg)) {
		u_long  reloc_size = 0;

		if (target_supports_slideable_kexts(object)) {
			/* get size of __DYSYMTAB relocation entries */
			reloc_size = kxld_reloc_get_macho_data_size(&object->locrelocs, &object->extrelocs);
			my_size += reloc_size;
		}
	}
#endif

	*vmsize = my_size;
}

/*******************************************************************************
*******************************************************************************/
void
kxld_object_get_vmsize(const KXLDObject *object, u_long *header_size,
    u_long *vmsize)
{
	check(object);
	check(header_size);
	check(vmsize);
	*header_size = 0;
	*vmsize = 0;

	/* vmsize is the padded header page(s) + segment vmsizes */

	*header_size = (object->is_final_image) ?
	    0 : (u_long)kxld_round_page_cross_safe(get_macho_header_size(object));

	*vmsize = *header_size + get_macho_data_size(object);
}

/*******************************************************************************
*******************************************************************************/
void
kxld_object_set_linked_object_size(KXLDObject *object, u_long vmsize)
{
	check(object);

	if (isOldInterface) {
		object->output_buffer_size = vmsize; /* cache this for use later */
	} else {
		object->split_info.linkedKextSize = vmsize;
	}
	return;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_export_linked_object(const KXLDObject *object,
    void *linked_object
    )
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSeg *seg = NULL;
	u_long size = 0;
	u_long header_size = 0;
	u_long header_offset = 0;
	u_long data_offset = 0;
	u_int ncmds = 0;
	u_int i = 0;
	boolean_t   is_32bit_object = kxld_object_is_32_bit(object);
	kxld_addr_t link_addr;
	u_char *my_linked_object;

	check(object);
	check(linked_object);

	if (isOldInterface) {
		size = object->output_buffer_size;
		link_addr = object->link_addr;
		my_linked_object = (u_char *) linked_object;
	} else {
		size = ((splitKextLinkInfo *)linked_object)->linkedKextSize;
		link_addr = ((splitKextLinkInfo *)linked_object)->vmaddr_TEXT;
		my_linked_object = ((splitKextLinkInfo *)linked_object)->linkedKext;
	}

	/* Calculate the size of the headers and data */

	header_size = get_macho_header_size(object);

	/* Copy data to the file */

	ncmds = object->segs.nitems + 1 /* LC_SYMTAB */;

#if KXLD_PIC_KEXTS
	/* don't write out a DYSYMTAB segment for targets that can't digest it
	 */
	if (target_supports_slideable_kexts(object)) {
		ncmds++; /* dysymtab */
	}
#endif  /* KXLD_PIC_KEXTS */

	if (object->uuid.has_uuid == TRUE) {
		ncmds++;
	}

	if (object->versionmin.has_versionmin == TRUE) {
		ncmds++;
	}

	if (object->srcversion.has_srcversion == TRUE) {
		ncmds++;
	}

	if (isSplitKext && object->splitinfolc.has_splitinfolc) {
		ncmds++;
	}

	rval = export_macho_header(object, my_linked_object, ncmds, &header_offset, header_size);
	require_noerr(rval, finish);

	for (i = 0; i < object->segs.nitems; ++i) {
		seg = kxld_array_get_item(&object->segs, i);

		rval = kxld_seg_export_macho_to_vm(seg, my_linked_object, &header_offset,
		    header_size, size, link_addr, is_32bit_object);
		require_noerr(rval, finish);
	}

	seg = kxld_object_get_seg_by_name(object, SEG_LINKEDIT);
	data_offset = (u_long) (seg->link_addr - link_addr);

	// data_offset is used to set the fileoff in the macho header load commands
	rval = kxld_symtab_export_macho(object->symtab,
	    my_linked_object,
	    &header_offset,
	    header_size,
	    &data_offset, size, is_32bit_object);
	require_noerr(rval, finish);

	// data_offset now points past the symbol tab and strings data in the linkedit
	// segment - (it was used to set new values for symoff and stroff)

#if KXLD_PIC_KEXTS
	if (target_supports_slideable_kexts(object)) {
		rval = kxld_reloc_export_macho(&object->relocator,
		    &object->locrelocs,
		    &object->extrelocs,
		    my_linked_object,
		    &header_offset,
		    header_size,
		    &data_offset, size);
		require_noerr(rval, finish);
	}
#endif  /* KXLD_PIC_KEXTS */

	if (object->uuid.has_uuid) {
		rval = kxld_uuid_export_macho(&object->uuid, my_linked_object, &header_offset, header_size);
		require_noerr(rval, finish);
	}

	if (object->versionmin.has_versionmin) {
		rval = kxld_versionmin_export_macho(&object->versionmin, my_linked_object, &header_offset, header_size);
		require_noerr(rval, finish);
	}

	if (object->srcversion.has_srcversion) {
		rval = kxld_srcversion_export_macho(&object->srcversion, my_linked_object, &header_offset, header_size);
		require_noerr(rval, finish);
	}

	if (isSplitKext && object->splitinfolc.has_splitinfolc) {
		rval = kxld_splitinfolc_export_macho(&object->splitinfolc,
		    linked_object,
		    &header_offset,
		    header_size,
		    &data_offset,
		    size);
		require_noerr(rval, finish);
	}

#if !KERNEL
	unswap_macho(my_linked_object, object->host_order, object->target_order);
#endif /* KERNEL */

	rval = KERN_SUCCESS;

finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
export_macho_header(const KXLDObject *object, u_char *buf, u_int ncmds,
    u_long *header_offset, u_long header_size)
{
	kern_return_t rval = KERN_FAILURE;

	check(object);
	check(buf);
	check(header_offset);

	KXLD_3264_FUNC(kxld_object_is_32_bit(object), rval,
	    export_macho_header_32, export_macho_header_64,
	    object, buf, ncmds, header_offset, header_size);
	require_noerr(rval, finish);

	rval = KERN_SUCCESS;

finish:
	return rval;
}

#if KXLD_USER_OR_ILP32
/*******************************************************************************
*******************************************************************************/
static kern_return_t
export_macho_header_32(const KXLDObject *object, u_char *buf, u_int ncmds,
    u_long *header_offset, u_long header_size)
{
	kern_return_t rval = KERN_FAILURE;
	struct mach_header *mach = NULL;

	check(object);
	check(buf);
	check(header_offset);

	require_action(sizeof(*mach) <= header_size - *header_offset, finish,
	    rval = KERN_FAILURE);
	mach = (struct mach_header *) ((void *) (buf + *header_offset));

	mach->magic = MH_MAGIC;
	mach->cputype = object->cputype;
	mach->cpusubtype = object->cpusubtype;
	mach->filetype = object->filetype;
	mach->ncmds = ncmds;
	mach->sizeofcmds = (uint32_t) (header_size - sizeof(*mach));
	mach->flags = MH_NOUNDEFS;

	*header_offset += sizeof(*mach);

	rval = KERN_SUCCESS;

finish:
	return rval;
}
#endif /* KXLD_USER_OR_ILP32 */

#if KXLD_USER_OR_LP64
/*******************************************************************************
*******************************************************************************/
static kern_return_t
export_macho_header_64(const KXLDObject *object, u_char *buf, u_int ncmds,
    u_long *header_offset, u_long header_size)
{
	kern_return_t rval = KERN_FAILURE;
	struct mach_header_64 *mach = NULL;

	check(object);
	check(buf);
	check(header_offset);

	require_action(sizeof(*mach) <= header_size - *header_offset, finish,
	    rval = KERN_FAILURE);
	mach = (struct mach_header_64 *) ((void *) (buf + *header_offset));

	mach->magic = MH_MAGIC_64;
	mach->cputype = object->cputype;
	mach->cpusubtype = object->cpusubtype;
	mach->filetype = object->filetype;
	mach->ncmds = ncmds;
	mach->sizeofcmds = (uint32_t) (header_size - sizeof(*mach));
	mach->flags = MH_NOUNDEFS;

	*header_offset += sizeof(*mach);

#if SPLIT_KEXTS_DEBUG
	{
		kxld_log(kKxldLogLinking, kKxldLogErr,
		    " %p >>> Start of macho header (size %lu) <%s>",
		    (void *) mach,
		    sizeof(*mach),
		    __func__);
		kxld_log(kKxldLogLinking, kKxldLogErr,
		    " %p <<< End of macho header <%s>",
		    (void *) ((u_char *)mach + sizeof(*mach)),
		    __func__);
	}
#endif

	rval = KERN_SUCCESS;

finish:
	return rval;
}
#endif /* KXLD_USER_OR_LP64 */

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_index_symbols_by_name(KXLDObject *object)
{
	return kxld_symtab_index_symbols_by_name(object->symtab);
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_index_cxx_symbols_by_value(KXLDObject *object)
{
	return kxld_symtab_index_cxx_symbols_by_value(object->symtab);
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_relocate(KXLDObject *object, kxld_addr_t link_address)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSeg *seg = NULL;
	u_int i = 0;

	check(object);

	object->link_addr = link_address;

	/* Relocate segments (which relocates the sections) */
	for (i = 0; i < object->segs.nitems; ++i) {
		seg = kxld_array_get_item(&object->segs, i);
		kxld_seg_relocate(seg, link_address);
	} // for...

	/* Relocate symbols */
	rval = kxld_symtab_relocate(object->symtab, &object->sects);
	require_noerr(rval, finish);

	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
static KXLDSym *
get_mutable_sym(const KXLDObject *object, const KXLDSym *sym)
{
	KXLDSym *rval = NULL;
	kern_return_t result = KERN_FAILURE;
	u_int i = 0;

	result = kxld_symtab_get_sym_index(object->symtab, sym, &i);
	require_noerr(result, finish);

	rval = kxld_symtab_get_symbol_by_index(object->symtab, i);
	require_action(rval == sym, finish, rval = NULL);

finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_resolve_symbol(KXLDObject *object,
    const KXLDSym *sym, kxld_addr_t addr)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSym *resolved_sym = NULL;

	resolved_sym = get_mutable_sym(object, sym);
	require_action(resolved_sym, finish, rval = KERN_FAILURE);

	rval = kxld_sym_resolve(resolved_sym, addr);
	require_noerr(rval, finish);

	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_patch_symbol(KXLDObject *object, const struct kxld_sym *sym)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSym *patched_sym = NULL;

	patched_sym = get_mutable_sym(object, sym);
	require_action(patched_sym, finish, rval = KERN_FAILURE);

	(void) kxld_sym_patch(patched_sym);
	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_add_symbol(KXLDObject *object, char *name, kxld_addr_t link_addr,
    const KXLDSym **sym_out)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSym *sym = NULL;

	rval = kxld_symtab_add_symbol(object->symtab, name, link_addr, &sym);
	require_noerr(rval, finish);

	*sym_out = sym;
	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
kern_return_t
kxld_object_process_relocations(KXLDObject *object,
    const KXLDDict *patched_vtables)
{
	kern_return_t rval = KERN_FAILURE;

	(void) kxld_relocator_set_vtables(&object->relocator, patched_vtables);

	/* Process relocation entries and populate the global offset table.
	 *
	 * For final linked images: the relocation entries are contained in a couple
	 * of tables hanging off the end of the symbol table.  The GOT has its own
	 * section created by the linker; we simply need to fill it.
	 *
	 * For object files: the relocation entries are bound to each section.
	 * The GOT, if it exists for the target architecture, is created by kxld,
	 * and we must populate it according to our internal structures.
	 */
	if (object->is_final_image) {
#if KXLD_USER_OR_BUNDLE
		rval = process_symbol_pointers(object);
		require_noerr(rval, finish);

		rval = process_relocs_from_tables(object);
		require_noerr(rval, finish);
#else
		require_action(FALSE, finish, rval = KERN_FAILURE);
#endif /* KXLD_USER_OR_BUNDLE */
	} else {
#if KXLD_USER_OR_GOT
		/* Populate GOT */
		rval = populate_got(object);
		require_noerr(rval, finish);
#endif /* KXLD_USER_OR_GOT */
#if KXLD_USER_OR_OBJECT
		rval = process_relocs_from_sections(object);
		require_noerr(rval, finish);
#else
		require_action(FALSE, finish, rval = KERN_FAILURE);
#endif /* KXLD_USER_OR_OBJECT */
	}

	/* Populate kmod info structure */
	rval = populate_kmod_info(object);
	require_noerr(rval, finish);

	rval = KERN_SUCCESS;
finish:
	return rval;
}

#if KXLD_USER_OR_BUNDLE

#if SPLIT_KEXTS_DEBUG
static boolean_t  kxld_show_ptr_value;
#endif

#define SECT_SYM_PTRS "__nl_symbol_ptr"

/*******************************************************************************
* Final linked images create an __nl_symbol_ptr section for the global offset
* table and for symbol pointer lookups in general.  Rather than use relocation
* entries, the linker creates an "indirect symbol table" which stores indexes
* into the symbol table corresponding to the entries of this section.  This
* function populates the section with the relocated addresses of those symbols.
*******************************************************************************/
static kern_return_t
process_symbol_pointers(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSect *sect = NULL;
	KXLDSym *sym = NULL;
	int32_t *symidx = NULL;
	u_char *symptr = NULL;
	u_long symptrsize = 0;
	u_int nsyms = 0;
	u_int firstsym = 0;
	u_int i = 0;

	check(object);

	require_action(object->is_final_image && object->dysymtab_hdr,
	    finish, rval = KERN_FAILURE);

	/* Get the __DATA,__nl_symbol_ptr section.  If it doesn't exist, we have
	 * nothing to do.
	 */

	sect = kxld_object_get_sect_by_name(object, SEG_DATA, SECT_SYM_PTRS);
	if (!sect || !(sect->flags & S_NON_LAZY_SYMBOL_POINTERS)) {
		rval = KERN_SUCCESS;
		goto finish;
	}

	/* Calculate the table offset and number of entries in the section */

	if (kxld_object_is_32_bit(object)) {
		symptrsize = sizeof(uint32_t);
	} else {
		symptrsize = sizeof(uint64_t);
	}

	nsyms = (u_int) (sect->size / symptrsize);
	firstsym = sect->reserved1;

	require_action(firstsym + nsyms <= object->dysymtab_hdr->nindirectsyms,
	    finish, rval = KERN_FAILURE;
	    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogMalformedMachO
	    "firstsym + nsyms > object->dysymtab_hdr->nindirectsyms"));

	/* Iterate through the indirect symbol table and fill in the section of
	 * symbol pointers.  There are three cases:
	 *   1) A normal symbol - put its value directly in the table
	 *   2) An INDIRECT_SYMBOL_LOCAL - symbols that are local and already have
	 *      their offset from the start of the file in the section.  Simply
	 *      add the file's link address to fill this entry.
	 *   3) An INDIRECT_SYMBOL_ABS - prepopulated absolute symbols.  No
	 *      action is required.
	 */

	if (isOldInterface) {
		symidx = (int32_t *) ((void *) (object->file + object->dysymtab_hdr->indirectsymoff));
	} else {
		symidx = (int32_t *) ((void *) (object->split_info.kextExecutable + object->dysymtab_hdr->indirectsymoff));
	}

	symidx += firstsym;
	symptr = sect->data;
	for (i = 0; i < nsyms; ++i, ++symidx, symptr += symptrsize) {
		if (*symidx & INDIRECT_SYMBOL_LOCAL) {
			if (*symidx & INDIRECT_SYMBOL_ABS) {
				continue;
			}

			if (isOldInterface) {
				add_to_ptr(symptr, object->link_addr, kxld_object_is_32_bit(object));
			} else {
				add_to_ptr(symptr, object->split_info.vmaddr_TEXT, kxld_object_is_32_bit(object));
			}
		} else {
			sym = kxld_symtab_get_symbol_by_index(object->symtab, *symidx);
			require_action(sym, finish, rval = KERN_FAILURE);

			if (isOldInterface) {
				add_to_ptr(symptr, sym->link_addr, kxld_object_is_32_bit(object));
			} else {
				add_to_ptr(symptr, object->split_info.vmaddr_TEXT, kxld_object_is_32_bit(object));
			}
		}
	}

	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
static KXLDSeg *
get_seg_by_base_addr(KXLDObject *object, kxld_addr_t base_addr)
{
	KXLDSeg *seg = NULL;
	kxld_addr_t start = 0;
	kxld_addr_t end = 0;
	u_int i = 0;

	for (i = 0; i < object->segs.nitems; ++i) {
		seg = kxld_array_get_item(&object->segs, i);
		start = seg->base_addr;
		end = seg->base_addr + seg->vmsize;

		if (start <= base_addr && base_addr < end) {
			return seg;
		}
	}

	return NULL;
}

/*******************************************************************************
*******************************************************************************/
static kern_return_t
process_relocs_from_tables(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDReloc *reloc = NULL;
	KXLDSeg *seg = NULL;
	u_int i = 0;

	/* Process external relocations */
	for (i = 0; i < object->extrelocs.nitems; ++i) {
		reloc = kxld_array_get_item(&object->extrelocs, i);

		seg = get_seg_by_base_addr(object, reloc->address);
		require_action(seg, finish, rval = KERN_FAILURE);

		if (isOldInterface) {
			rval = kxld_relocator_process_table_reloc(&object->relocator, reloc,
			    seg, object->link_addr);
		} else {
			kxld_addr_t my_link_addr = object->split_info.vmaddr_TEXT;
			if (isSplitKext) {
				if (kxld_seg_is_text_exec_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_TEXT_EXEC;
				} else if (kxld_seg_is_data_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_DATA;
				} else if (kxld_seg_is_data_const_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_DATA_CONST;
				} else if (kxld_seg_is_llvm_cov_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_LLVM_COV;
				} else if (kxld_seg_is_linkedit_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_LINKEDIT;
				}
			}
			rval = kxld_relocator_process_table_reloc(&object->relocator,
			    reloc,
			    seg,
			    my_link_addr);
		}
		require_noerr(rval, finish);
	}

	/* Process local relocations */
	for (i = 0; i < object->locrelocs.nitems; ++i) {
		reloc = kxld_array_get_item(&object->locrelocs, i);

		seg = get_seg_by_base_addr(object, reloc->address);
		require_action(seg, finish, rval = KERN_FAILURE);

		if (isOldInterface) {
			rval = kxld_relocator_process_table_reloc(&object->relocator, reloc,
			    seg, object->link_addr);
		} else {
			kxld_addr_t my_link_addr = object->split_info.vmaddr_TEXT;
			if (isSplitKext) {
				if (kxld_seg_is_text_exec_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_TEXT_EXEC;
				} else if (kxld_seg_is_data_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_DATA;
				} else if (kxld_seg_is_data_const_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_DATA_CONST;
				} else if (kxld_seg_is_llvm_cov_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_LLVM_COV;
				} else if (kxld_seg_is_linkedit_seg(seg)) {
					my_link_addr = object->split_info.vmaddr_LINKEDIT;
				}
			}
			rval = kxld_relocator_process_table_reloc(&object->relocator,
			    reloc,
			    seg,
			    my_link_addr);
		}
		require_noerr(rval, finish);
	}

	rval = KERN_SUCCESS;
finish:
	return rval;
}

/*******************************************************************************
*******************************************************************************/
static void
add_to_ptr(u_char *symptr, kxld_addr_t val, boolean_t is_32_bit)
{
	if (is_32_bit) {
		uint32_t *ptr = (uint32_t *) ((void *) symptr);

		*ptr += (uint32_t) val;
	} else {
		uint64_t *ptr = (uint64_t *) ((void *) symptr);

		*ptr += (uint64_t) val;
	}

#if SPLIT_KEXTS_DEBUG
	kxld_show_ptr_value = FALSE;
#endif
}
#endif /* KXLD_USER_OR_BUNDLE */

#if KXLD_USER_OR_OBJECT
/*******************************************************************************
*******************************************************************************/
static kern_return_t
process_relocs_from_sections(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSect *sect = NULL;
	u_int i = 0;

	for (i = 0; i < object->sects.nitems; ++i) {
		sect = kxld_array_get_item(&object->sects, i);
		rval = kxld_sect_process_relocs(sect, &object->relocator);
		require_noerr(rval, finish);
	}

	rval = KERN_SUCCESS;
finish:
	return rval;
}
#endif /* KXLD_USER_OR_OBJECT */

/*******************************************************************************
*******************************************************************************/
static kern_return_t
populate_kmod_info(KXLDObject *object)
{
	kern_return_t rval = KERN_FAILURE;
	KXLDSect *kmodsect = NULL;
	KXLDSym *kmodsym = NULL;
	kmod_info_t *kmod_info = NULL;
	u_long kmod_offset = 0;
	u_long header_size;
	u_long size;

	if (kxld_object_is_kernel(object)) {
		rval = KERN_SUCCESS;
		goto finish;
	}

	kxld_object_get_vmsize(object, &header_size, &size);

	kmodsym = kxld_symtab_get_locally_defined_symbol_by_name(object->symtab,
	    KXLD_KMOD_INFO_SYMBOL);
	require_action(kmodsym, finish, rval = KERN_FAILURE;
	    kxld_log(kKxldLogLinking, kKxldLogErr, kKxldLogNoKmodInfo));

	kmodsect = kxld_array_get_item(&object->sects, kmodsym->sectnum);

	kmod_offset = (u_long) (kmodsym->base_addr -  kmodsect->base_addr);
	kmod_info = (kmod_info_t *) ((void *) (kmodsect->data + kmod_offset));

	if (kxld_object_is_32_bit(object)) {
		kmod_info_32_v1_t *kmod = (kmod_info_32_v1_t *) (kmod_info);

		if (isOldInterface) {
			kmod->address = (uint32_t) object->link_addr;
		} else {
			kmod->address = (uint32_t) object->split_info.vmaddr_TEXT;
		}

		kmod->size = (uint32_t) size;
		kmod->hdr_size = (uint32_t) header_size;

#if !KERNEL
		if (kxld_object_target_needs_swap(object)) {
			kmod->address = OSSwapInt32(kmod->address);
			kmod->size = OSSwapInt32(kmod->size);
			kmod->hdr_size = OSSwapInt32(kmod->hdr_size);
		}
#endif /* !KERNEL */
	} else {
		kmod_info_64_v1_t *kmod = (kmod_info_64_v1_t *) (kmod_info);

		if (isOldInterface) {
			kmod->address = object->link_addr;
		} else {
			kmod->address = object->split_info.vmaddr_TEXT;
		}

		kmod->size = size;
		kmod->hdr_size = header_size;

#if !KERNEL
		if (kxld_object_target_needs_swap(object)) {
			kmod->address = OSSwapInt64(kmod->address);
			kmod->size = OSSwapInt64(kmod->size);
			kmod->hdr_size = OSSwapInt64(kmod->hdr_size);
		}
#endif /* !KERNEL */

#if SPLIT_KEXTS_DEBUG
		{
			kxld_log(kKxldLogLinking, kKxldLogErr,
			    " kmodsect %p kmod_info %p = kmodsect->data %p + kmod_offset %lu <%s>",
			    (void *) kmodsect,
			    (void *) kmod_info,
			    (void *) kmodsect->data,
			    kmod_offset,
			    __func__);

			kxld_log(kKxldLogLinking, kKxldLogErr,
			    " kmod_info data: address %p size %llu hdr_size %llu start_addr %p stop_addr %p <%s>",
			    (void *) kmod->address,
			    kmod->size,
			    kmod->hdr_size,
			    (void *) kmod->start_addr,
			    (void *) kmod->stop_addr,
			    __func__);
		}
#endif
	}

	rval = KERN_SUCCESS;

finish:
	return rval;
}

#if KXLD_PIC_KEXTS
/*******************************************************************************
*******************************************************************************/
static boolean_t
target_supports_slideable_kexts(const KXLDObject *object)
{
	check(object);

	return object->cputype != CPU_TYPE_I386 && object->include_kaslr_relocs;
}
#endif  /* KXLD_PIC_KEXTS */
