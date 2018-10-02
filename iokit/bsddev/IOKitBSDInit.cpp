/*
 * Copyright (c) 1998-2018 Apple Inc. All rights reserved.
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
#include <IOKit/IOBSD.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOCatalogue.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IOPlatformExpert.h>
#include <IOKit/IOUserClient.h>

extern "C" {

#include <pexpert/pexpert.h>
#include <kern/clock.h>
#include <uuid/uuid.h>
#include <sys/vnode_internal.h>
#include <sys/mount.h>

// how long to wait for matching root device, secs
#if DEBUG
#define ROOTDEVICETIMEOUT       120
#else
#define ROOTDEVICETIMEOUT       60
#endif

int panic_on_exception_triage = 0;

extern dev_t mdevadd(int devid, uint64_t base, unsigned int size, int phys);
extern dev_t mdevlookup(int devid);
extern void mdevremoveall(void);
extern int mdevgetrange(int devid, uint64_t *base, uint64_t *size);
extern void di_root_ramfile(IORegistryEntry * entry);


#if CONFIG_EMBEDDED
#define IOPOLLED_COREFILE 	(CONFIG_KDP_INTERACTIVE_DEBUGGING)

#if defined(XNU_TARGET_OS_BRIDGE)
#define kIOCoreDumpSize         150ULL*1024ULL*1024ULL
// leave free space on volume:
#define kIOCoreDumpFreeSize     150ULL*1024ULL*1024ULL
#define kIOCoreDumpPath         "/private/var/internal/kernelcore"
#else
#define kIOCoreDumpSize         350ULL*1024ULL*1024ULL
// leave free space on volume:
#define kIOCoreDumpFreeSize     350ULL*1024ULL*1024ULL
#define kIOCoreDumpPath         "/private/var/vm/kernelcore"
#endif

#elif DEVELOPMENT
#define IOPOLLED_COREFILE  	1
// no sizing
#define kIOCoreDumpSize		0ULL
#define kIOCoreDumpFreeSize	0ULL
#else
#define IOPOLLED_COREFILE  	0
#endif


#if IOPOLLED_COREFILE
static bool 
NewKernelCoreMedia(void * target, void * refCon,
		   IOService * newService,
		   IONotifier * notifier);
#endif /* IOPOLLED_COREFILE */

#if CONFIG_KDP_INTERACTIVE_DEBUGGING
/*
 * Touched by IOFindBSDRoot() if a RAMDisk is used for the root device.
 */
extern uint64_t kdp_core_ramdisk_addr;
extern uint64_t kdp_core_ramdisk_size;
#endif

kern_return_t
IOKitBSDInit( void )
{
    IOService::publishResource("IOBSD");

    return( kIOReturnSuccess );
}

void
IOServicePublishResource( const char * property, boolean_t value )
{
    if ( value)
        IOService::publishResource( property, kOSBooleanTrue );
    else
        IOService::getResourceService()->removeProperty( property );
}

boolean_t
IOServiceWaitForMatchingResource( const char * property, uint64_t timeout )
{
    OSDictionary *	dict = 0;
    IOService *         match = 0;
    boolean_t		found = false;
    
    do {
        
        dict = IOService::resourceMatching( property );
        if( !dict)
            continue;
        match = IOService::waitForMatchingService( dict, timeout );
        if ( match)
            found = true;
        
    } while( false );
    
    if( dict)
        dict->release();
    if( match)
        match->release();
    
    return( found );
}

boolean_t
IOCatalogueMatchingDriversPresent( const char * property )
{
    OSDictionary *	dict = 0;
    OSOrderedSet *	set = 0;
    SInt32		generationCount = 0;
    boolean_t		found = false;
    
    do {
        
        dict = OSDictionary::withCapacity(1);
        if( !dict)
            continue;
        dict->setObject( property, kOSBooleanTrue );
        set = gIOCatalogue->findDrivers( dict, &generationCount );
        if ( set && (set->getCount() > 0))
            found = true;
        
    } while( false );
    
    if( dict)
        dict->release();
    if( set)
        set->release();
    
    return( found );
}

OSDictionary * IOBSDNameMatching( const char * name )
{
    OSDictionary *	dict;
    const OSSymbol *	str = 0;

    do {

	dict = IOService::serviceMatching( gIOServiceKey );
	if( !dict)
	    continue;
        str = OSSymbol::withCString( name );
	if( !str)
	    continue;
        dict->setObject( kIOBSDNameKey, (OSObject *) str );
        str->release();

        return( dict );

    } while( false );

    if( dict)
	dict->release();
    if( str)
	str->release();

    return( 0 );
}

OSDictionary * IOUUIDMatching( void )
{
    return IOService::resourceMatching( "boot-uuid-media" );
}

OSDictionary * IONetworkNamePrefixMatching( const char * prefix )
{
    OSDictionary *	 matching;
    OSDictionary *   propDict = 0;
    const OSSymbol * str      = 0;
	char networkType[128];
	
    do {
        matching = IOService::serviceMatching( "IONetworkInterface" );
        if ( matching == 0 )
            continue;

        propDict = OSDictionary::withCapacity(1);
        if ( propDict == 0 )
            continue;

        str = OSSymbol::withCString( prefix );
        if ( str == 0 )
            continue;

        propDict->setObject( "IOInterfaceNamePrefix", (OSObject *) str );
        str->release();
        str = 0;

		// see if we're contrained to netroot off of specific network type
		if(PE_parse_boot_argn( "network-type", networkType, 128 ))
		{
			str = OSSymbol::withCString( networkType );
			if(str)
			{
				propDict->setObject( "IONetworkRootType", str);
				str->release();
				str = 0;
			}
		}

        if ( matching->setObject( gIOPropertyMatchKey,
                                  (OSObject *) propDict ) != true )
            continue;

        propDict->release();
        propDict = 0;

        return( matching );

    } while ( false );

    if ( matching ) matching->release();
    if ( propDict ) propDict->release();
    if ( str      ) str->release();

    return( 0 );
}

static bool IORegisterNetworkInterface( IOService * netif )
{
    // A network interface is typically named and registered
    // with BSD after receiving a request from a user space
    // "namer". However, for cases when the system needs to
    // root from the network, this registration task must be
    // done inside the kernel and completed before the root
    // device is handed to BSD.

    IOService *    stack;
    OSNumber *     zero    = 0;
    OSString *     path    = 0;
    OSDictionary * dict    = 0;
    char *         pathBuf = 0;
    int            len;
    enum { kMaxPathLen = 512 };

    do {
        stack = IOService::waitForService(
                IOService::serviceMatching("IONetworkStack") );
        if ( stack == 0 ) break;

        dict = OSDictionary::withCapacity(3);
        if ( dict == 0 ) break;

        zero = OSNumber::withNumber((UInt64) 0, 32);
        if ( zero == 0 ) break;

        pathBuf = (char *) IOMalloc( kMaxPathLen );
        if ( pathBuf == 0 ) break;

        len = kMaxPathLen;
        if ( netif->getPath( pathBuf, &len, gIOServicePlane )
             == false ) break;

        path = OSString::withCStringNoCopy( pathBuf );
        if ( path == 0 ) break;

        dict->setObject( "IOInterfaceUnit", zero );
        dict->setObject( kIOPathMatchKey,   path );

        stack->setProperties( dict );
    }
    while ( false );

    if ( zero ) zero->release();
    if ( path ) path->release();
    if ( dict ) dict->release();
    if ( pathBuf ) IOFree(pathBuf, kMaxPathLen);

	return ( netif->getProperty( kIOBSDNameKey ) != 0 );
}

OSDictionary * IOOFPathMatching( const char * path, char * buf, int maxLen )
{
    OSDictionary *	matching = NULL;
    OSString *		str;
    char *		comp;
    int			len;

    do {

	len = strlen( kIODeviceTreePlane ":" );
	maxLen -= len;
	if( maxLen <= 0)
	    continue;

	strlcpy( buf, kIODeviceTreePlane ":", len + 1 );
	comp = buf + len;

	len = strlen( path );
	maxLen -= len;
	if( maxLen <= 0)
	    continue;
	strlcpy( comp, path, len + 1 );

	matching = OSDictionary::withCapacity( 1 );
	if( !matching)
	    continue;

	str = OSString::withCString( buf );
	if( !str)
	    continue;
        matching->setObject( kIOPathMatchKey, str );
	str->release();

	return( matching );

    } while( false );

    if( matching)
        matching->release();

    return( 0 );
}

static int didRam = 0;
enum { kMaxPathBuf = 512, kMaxBootVar = 128 };

kern_return_t IOFindBSDRoot( char * rootName, unsigned int rootNameSize,
				dev_t * root, u_int32_t * oflags )
{
    mach_timespec_t	t;
    IOService *		service;
    IORegistryEntry *	regEntry;
    OSDictionary *	matching = 0;
    OSString *		iostr;
    OSNumber *		off;
    OSData *		data = 0;

    UInt32		flags = 0;
    int			mnr, mjr;
    const char *        mediaProperty = 0;
    char *		rdBootVar;
    char *		str;
    const char *	look = 0;
    int			len;
    bool		debugInfoPrintedOnce = false;
    const char * 	uuidStr = NULL;

    static int		mountAttempts = 0;
				
    int xchar, dchar;

    // stall here for anyone matching on the IOBSD resource to finish (filesystems)
    matching = IOService::serviceMatching(gIOResourcesKey);
    assert(matching);
    matching->setObject(gIOResourceMatchedKey, gIOBSDKey);

	if ((service = IOService::waitForMatchingService(matching, 30ULL * kSecondScale))) {
		service->release();
	} else {
		IOLog("!BSD\n");
	}
    matching->release();
	matching = NULL;

    if( mountAttempts++)
    {
        IOLog("mount(%d) failed\n", mountAttempts);
	IOSleep( 5 * 1000 );
    }

    str = (char *) IOMalloc( kMaxPathBuf + kMaxBootVar );
    if( !str)
	return( kIOReturnNoMemory );
    rdBootVar = str + kMaxPathBuf;

    if (!PE_parse_boot_argn("rd", rdBootVar, kMaxBootVar )
     && !PE_parse_boot_argn("rootdev", rdBootVar, kMaxBootVar ))
	rdBootVar[0] = 0;

    do {
	if( (regEntry = IORegistryEntry::fromPath( "/chosen", gIODTPlane ))) {
	    di_root_ramfile(regEntry);
            data = OSDynamicCast(OSData, regEntry->getProperty( "root-matching" ));
            if (data) {
               matching = OSDynamicCast(OSDictionary, OSUnserializeXML((char *)data->getBytesNoCopy()));
                if (matching) {
                    continue;
                }
            }

	    data = (OSData *) regEntry->getProperty( "boot-uuid" );
	    if( data) {
		uuidStr = (const char*)data->getBytesNoCopy();
		OSString *uuidString = OSString::withCString( uuidStr );

		// match the boot-args boot-uuid processing below
		if( uuidString) {
		    IOLog("rooting via boot-uuid from /chosen: %s\n", uuidStr);
		    IOService::publishResource( "boot-uuid", uuidString );
		    uuidString->release();
		    matching = IOUUIDMatching();
		    mediaProperty = "boot-uuid-media";
		    regEntry->release();
		    continue;
		} else {
		    uuidStr = NULL;
		}
	    }
	    regEntry->release();
	}
    } while( false );

//
//	See if we have a RAMDisk property in /chosen/memory-map.  If so, make it into a device.
//	It will become /dev/mdx, where x is 0-f. 
//

	if(!didRam) {												/* Have we already build this ram disk? */
		didRam = 1;												/* Remember we did this */
		if((regEntry = IORegistryEntry::fromPath( "/chosen/memory-map", gIODTPlane ))) {	/* Find the map node */
			data = (OSData *)regEntry->getProperty("RAMDisk");	/* Find the ram disk, if there */
			if(data) {											/* We found one */
				uintptr_t *ramdParms;
				ramdParms = (uintptr_t *)data->getBytesNoCopy();	/* Point to the ram disk base and size */
				(void)mdevadd(-1, ml_static_ptovirt(ramdParms[0]) >> 12, ramdParms[1] >> 12, 0);	/* Initialize it and pass back the device number */
			}
			regEntry->release();								/* Toss the entry */
		}
	}
	
//
//	Now check if we are trying to root on a memory device
//

	if((rdBootVar[0] == 'm') && (rdBootVar[1] == 'd') && (rdBootVar[3] == 0)) {
		dchar = xchar = rdBootVar[2];							/* Get the actual device */
		if((xchar >= '0') && (xchar <= '9')) xchar = xchar - '0';	/* If digit, convert */
		else {
			xchar = xchar & ~' ';								/* Fold to upper case */
			if((xchar >= 'A') && (xchar <= 'F')) {				/* Is this a valid digit? */
				xchar = (xchar & 0xF) + 9;						/* Convert the hex digit */
				dchar = dchar | ' ';							/* Fold to lower case */
			}
			else xchar = -1;									/* Show bogus */
		}
		if(xchar >= 0) {										/* Do we have a valid memory device name? */
			*root = mdevlookup(xchar);							/* Find the device number */
			if(*root >= 0) {									/* Did we find one? */
				rootName[0] = 'm';								/* Build root name */
				rootName[1] = 'd';								/* Build root name */
				rootName[2] = dchar;							/* Build root name */
				rootName[3] = 0;								/* Build root name */
				IOLog("BSD root: %s, major %d, minor %d\n", rootName, major(*root), minor(*root));
				*oflags = 0;									/* Show that this is not network */

#if CONFIG_KDP_INTERACTIVE_DEBUGGING
                /* retrieve final ramdisk range and initialize KDP variables */
                if (mdevgetrange(xchar, &kdp_core_ramdisk_addr, &kdp_core_ramdisk_size) != 0) {
                    IOLog("Unable to retrieve range for root memory device %d\n", xchar);
                    kdp_core_ramdisk_addr = 0;
                    kdp_core_ramdisk_size = 0;
                }
#endif

				goto iofrootx;									/* Join common exit... */
			}
			panic("IOFindBSDRoot: specified root memory device, %s, has not been configured\n", rdBootVar);	/* Not there */
		}
	}

      if( (!matching) && rdBootVar[0] ) {
	// by BSD name
	look = rdBootVar;
	if( look[0] == '*')
	    look++;
    
	if ( strncmp( look, "en", strlen( "en" )) == 0 ) {
	    matching = IONetworkNamePrefixMatching( "en" );
	} else if ( strncmp( look, "uuid", strlen( "uuid" )) == 0 ) {
            char *uuid;
            OSString *uuidString;

            uuid = (char *)IOMalloc( kMaxBootVar );
                  
            if ( uuid ) {
                if (!PE_parse_boot_argn( "boot-uuid", uuid, kMaxBootVar )) {
                    panic( "rd=uuid but no boot-uuid=<value> specified" ); 
                } 
                uuidString = OSString::withCString( uuid );
                if ( uuidString ) {
                    IOService::publishResource( "boot-uuid", uuidString );
                    uuidString->release();
                    IOLog( "\nWaiting for boot volume with UUID %s\n", uuid );
                    matching = IOUUIDMatching();
                    mediaProperty = "boot-uuid-media";
                }
                IOFree( uuid, kMaxBootVar );
            }
	} else {
	    matching = IOBSDNameMatching( look );
	}
    }

    if( !matching) {
	OSString * astring;
	// Match any HFS media
	
        matching = IOService::serviceMatching( "IOMedia" );
        astring = OSString::withCStringNoCopy("Apple_HFS");
        if ( astring ) {
            matching->setObject("Content", astring);
            astring->release();
        }
    }

    if( gIOKitDebug & kIOWaitQuietBeforeRoot ) {
    	IOLog( "Waiting for matching to complete\n" );
    	IOService::getPlatform()->waitQuiet();
    }

    if( true && matching) {
        OSSerialize * s = OSSerialize::withCapacity( 5 );

        if( matching->serialize( s )) {
            IOLog( "Waiting on %s\n", s->text() );
            s->release();
        }
    }

    do {
        t.tv_sec = ROOTDEVICETIMEOUT;
        t.tv_nsec = 0;
	matching->retain();
        service = IOService::waitForService( matching, &t );
	if( (!service) || (mountAttempts == 10)) {
            PE_display_icon( 0, "noroot");
            IOLog( "Still waiting for root device\n" );

            if( !debugInfoPrintedOnce) {
                debugInfoPrintedOnce = true;
                if( gIOKitDebug & kIOLogDTree) {
                    IOLog("\nDT plane:\n");
                    IOPrintPlane( gIODTPlane );
                }
                if( gIOKitDebug & kIOLogServiceTree) {
                    IOLog("\nService plane:\n");
                    IOPrintPlane( gIOServicePlane );
                }
                if( gIOKitDebug & kIOLogMemory)
                    IOPrintMemory();
            }
	}
    } while( !service);
    matching->release();

    if ( service && mediaProperty ) {
        service = (IOService *)service->getProperty(mediaProperty);
    }

    mjr = 0;
    mnr = 0;

    // If the IOService we matched to is a subclass of IONetworkInterface,
    // then make sure it has been registered with BSD and has a BSD name
    // assigned.

    if ( service
    &&   service->metaCast( "IONetworkInterface" )
    &&   !IORegisterNetworkInterface( service ) )
    {
        service = 0;
    }

    if( service) {

	len = kMaxPathBuf;
	service->getPath( str, &len, gIOServicePlane );
	IOLog( "Got boot device = %s\n", str );

	iostr = (OSString *) service->getProperty( kIOBSDNameKey );
	if( iostr)
	    strlcpy( rootName, iostr->getCStringNoCopy(), rootNameSize );
	off = (OSNumber *) service->getProperty( kIOBSDMajorKey );
	if( off)
	    mjr = off->unsigned32BitValue();
	off = (OSNumber *) service->getProperty( kIOBSDMinorKey );
	if( off)
	    mnr = off->unsigned32BitValue();

	if( service->metaCast( "IONetworkInterface" ))
	    flags |= 1;

    } else {

	IOLog( "Wait for root failed\n" );
        strlcpy( rootName, "en0", rootNameSize );
        flags |= 1;
    }

    IOLog( "BSD root: %s", rootName );
    if( mjr)
	IOLog(", major %d, minor %d\n", mjr, mnr );
    else
	IOLog("\n");

    *root = makedev( mjr, mnr );
    *oflags = flags;

    IOFree( str,  kMaxPathBuf + kMaxBootVar );

iofrootx:
    if( (gIOKitDebug & (kIOLogDTree | kIOLogServiceTree | kIOLogMemory)) && !debugInfoPrintedOnce) {

	IOService::getPlatform()->waitQuiet();
        if( gIOKitDebug & kIOLogDTree) {
            IOLog("\nDT plane:\n");
            IOPrintPlane( gIODTPlane );
        }
        if( gIOKitDebug & kIOLogServiceTree) {
            IOLog("\nService plane:\n");
            IOPrintPlane( gIOServicePlane );
        }
        if( gIOKitDebug & kIOLogMemory)
            IOPrintMemory();
    }

    return( kIOReturnSuccess );
}

bool IORamDiskBSDRoot(void)
{
    char rdBootVar[kMaxBootVar];
    if (PE_parse_boot_argn("rd", rdBootVar, kMaxBootVar )
     || PE_parse_boot_argn("rootdev", rdBootVar, kMaxBootVar )) {
        if((rdBootVar[0] == 'm') && (rdBootVar[1] == 'd') && (rdBootVar[3] == 0)) {
            return true;
        }
    }
    return false;
}

void IOSecureBSDRoot(const char * rootName)
{
#if CONFIG_EMBEDDED
    int              tmpInt;
    IOReturn         result;
    IOPlatformExpert *pe;
    OSDictionary     *matching;
    const OSSymbol   *functionName = OSSymbol::withCStringNoCopy("SecureRootName");
    
    matching = IOService::serviceMatching("IOPlatformExpert");
    assert(matching);
    pe = (IOPlatformExpert *) IOService::waitForMatchingService(matching, 30ULL * kSecondScale);
    matching->release();
    assert(pe);
    // Returns kIOReturnNotPrivileged is the root device is not secure.
    // Returns kIOReturnUnsupported if "SecureRootName" is not implemented.
    result = pe->callPlatformFunction(functionName, false, (void *)rootName, (void *)0, (void *)0, (void *)0);
    functionName->release();
    OSSafeReleaseNULL(pe);
    
    if (result == kIOReturnNotPrivileged) {
        mdevremoveall();
    } else if (result == kIOReturnSuccess) {
        // If we are booting with a secure root, and we have the right
	// boot-arg, we will want to panic on exception triage.  This
	// behavior is intended as a debug aid (we can look at why an
	// exception occured in the kernel debugger).
        if (PE_parse_boot_argn("-panic_on_exception_triage", &tmpInt, sizeof(tmpInt))) {
            panic_on_exception_triage = 1;
        }
    }

#endif  // CONFIG_EMBEDDED
}

void *
IOBSDRegistryEntryForDeviceTree(char * path)
{
    return (IORegistryEntry::fromPath(path, gIODTPlane));
}

void
IOBSDRegistryEntryRelease(void * entry)
{
    IORegistryEntry * regEntry = (IORegistryEntry *)entry;

    if (regEntry)
	regEntry->release();
    return;
}

const void *
IOBSDRegistryEntryGetData(void * entry, char * property_name, 
			  int * packet_length)
{
    OSData *		data;
    IORegistryEntry * 	regEntry = (IORegistryEntry *)entry;

    data = (OSData *) regEntry->getProperty(property_name);
    if (data) {
	*packet_length = data->getLength();
        return (data->getBytesNoCopy());
    }
    return (NULL);
}

kern_return_t IOBSDGetPlatformUUID( uuid_t uuid, mach_timespec_t timeout )
{
    IOService * resources;
    OSString *  string;

    resources = IOService::waitForService( IOService::resourceMatching( kIOPlatformUUIDKey ), ( timeout.tv_sec || timeout.tv_nsec ) ? &timeout : 0 );
    if ( resources == 0 ) return KERN_OPERATION_TIMED_OUT;

    string = ( OSString * ) IOService::getPlatform( )->getProvider( )->getProperty( kIOPlatformUUIDKey );
    if ( string == 0 ) return KERN_NOT_SUPPORTED;

    uuid_parse( string->getCStringNoCopy( ), uuid );

    return KERN_SUCCESS;
}

} /* extern "C" */

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

#include <sys/conf.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <sys/fcntl.h>
#include <IOKit/IOPolledInterface.h>
#include <IOKit/IOBufferMemoryDescriptor.h>

IOPolledFileIOVars * gIOPolledCoreFileVars;

#if IOPOLLED_COREFILE

static IOReturn 
IOOpenPolledCoreFile(const char * filename)
{
    IOReturn err;
    unsigned int debug;

    if (gIOPolledCoreFileVars)                             return (kIOReturnBusy);
    if (!IOPolledInterface::gMetaClass.getInstanceCount()) return (kIOReturnUnsupported);

    debug = 0;
    PE_parse_boot_argn("debug", &debug, sizeof (debug));
    if (DB_DISABLE_LOCAL_CORE & debug)                     return (kIOReturnUnsupported);

    err = IOPolledFileOpen(filename, kIOCoreDumpSize, kIOCoreDumpFreeSize,
			    NULL, 0,
			    &gIOPolledCoreFileVars, NULL, NULL, 0);
    if (kIOReturnSuccess != err)                           return (err);

    err = IOPolledFilePollersSetup(gIOPolledCoreFileVars, kIOPolledPreflightCoreDumpState);
    if (kIOReturnSuccess != err)
    {
	IOPolledFileClose(&gIOPolledCoreFileVars, NULL, NULL, 0, 0, 0);
    }

    return (err);
}

static void 
IOClosePolledCoreFile(void)
{
    IOPolledFilePollersClose(gIOPolledCoreFileVars, kIOPolledPostflightCoreDumpState);
    IOPolledFileClose(&gIOPolledCoreFileVars, NULL, NULL, 0, 0, 0);
}

static thread_call_t gIOOpenPolledCoreFileTC;
static IONotifier  * gIOPolledCoreFileNotifier;
static IONotifier  * gIOPolledCoreFileInterestNotifier;

static IOReturn 
KernelCoreMediaInterest(void * target, void * refCon,
			UInt32 messageType, IOService * provider,
			void * messageArgument, vm_size_t argSize )
{
    if (kIOMessageServiceIsTerminated == messageType)
    {
	gIOPolledCoreFileInterestNotifier->remove();
	gIOPolledCoreFileInterestNotifier = 0;
	IOClosePolledCoreFile();
    }

    return (kIOReturnSuccess);
}

static void
OpenKernelCoreMedia(thread_call_param_t p0, thread_call_param_t p1)
{
    IOService * newService;
    OSString  * string;
    char        filename[16];

    newService = (IOService *) p1;
    do
    {
	if (gIOPolledCoreFileVars) break;
	string = OSDynamicCast(OSString, newService->getProperty(kIOBSDNameKey));
	if (!string) break;
	snprintf(filename, sizeof(filename), "/dev/%s", string->getCStringNoCopy());
	if (kIOReturnSuccess != IOOpenPolledCoreFile(filename)) break;
	gIOPolledCoreFileInterestNotifier = newService->registerInterest(
				gIOGeneralInterest, &KernelCoreMediaInterest, NULL, 0);
    }
    while (false);

    newService->release();
}

static bool 
NewKernelCoreMedia(void * target, void * refCon,
		   IOService * newService,
		   IONotifier * notifier)
{
    static volatile UInt32 onlyOneCorePartition = 0;
    do
    {
	if (!OSCompareAndSwap(0, 1, &onlyOneCorePartition)) break;
	if (gIOPolledCoreFileVars)    break;
        if (!gIOOpenPolledCoreFileTC) break;
        newService = newService->getProvider();
        if (!newService)              break;
        newService->retain();
	thread_call_enter1(gIOOpenPolledCoreFileTC, newService);
    }
    while (false);

    return (false);
}

#endif /* IOPOLLED_COREFILE */

extern "C" void 
IOBSDMountChange(struct mount * mp, uint32_t op)
{
#if IOPOLLED_COREFILE

    OSDictionary * bsdMatching;
    OSDictionary * mediaMatching;
    OSString     * string;

    if (!gIOPolledCoreFileNotifier) do
    {
	if (!gIOOpenPolledCoreFileTC) gIOOpenPolledCoreFileTC = thread_call_allocate(&OpenKernelCoreMedia, NULL);
	bsdMatching = IOService::serviceMatching("IOMediaBSDClient");
	if (!bsdMatching) break;
	mediaMatching = IOService::serviceMatching("IOMedia");
	string = OSString::withCStringNoCopy("5361644D-6163-11AA-AA11-00306543ECAC");
	if (!string || !mediaMatching) break;
	mediaMatching->setObject("Content", string);
	string->release();
	bsdMatching->setObject(gIOParentMatchKey, mediaMatching);
	mediaMatching->release();

	gIOPolledCoreFileNotifier = IOService::addMatchingNotification(
						  gIOFirstMatchNotification, bsdMatching, 
						  &NewKernelCoreMedia, NULL, NULL, -1000);
    }
    while (false);

#if CONFIG_EMBEDDED
    uint64_t flags;
    char path[128];
    int pathLen;
    vnode_t vn;
    int result;

    switch (op)
    {
	case kIOMountChangeMount:
	case kIOMountChangeDidResize:

	    if (gIOPolledCoreFileVars) break;
	    flags = vfs_flags(mp);
	    if (MNT_RDONLY & flags) break;
	    if (!(MNT_LOCAL & flags)) break;

	    vn = vfs_vnodecovered(mp);
	    if (!vn) break;
	    pathLen = sizeof(path);
	    result = vn_getpath(vn, &path[0], &pathLen);
	    vnode_put(vn);
	    if (0 != result) break;
	    if (!pathLen) break;
#if defined(XNU_TARGET_OS_BRIDGE)
	    // on bridgeOS systems we put the core in /private/var/internal. We don't
	    // want to match with /private/var because /private/var/internal is often mounted
	    // over /private/var
	    if ((pathLen - 1) < (int) strlen("/private/var/internal")) break;
#endif
	    if (0 != strncmp(path, kIOCoreDumpPath, pathLen - 1)) break;
	    IOOpenPolledCoreFile(kIOCoreDumpPath);
	    break;

	case kIOMountChangeUnmount:
	case kIOMountChangeWillResize:
	    if (gIOPolledCoreFileVars && (mp == kern_file_mount(gIOPolledCoreFileVars->fileRef)))
	    {
		IOClosePolledCoreFile();
	    }
	    break;
    }
#endif /* CONFIG_EMBEDDED */
#endif /* IOPOLLED_COREFILE */
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * */

extern "C" boolean_t 
IOTaskHasEntitlement(task_t task, const char * entitlement)
{
    OSObject * obj;
    obj = IOUserClient::copyClientEntitlement(task, entitlement);
    if (!obj) return (false);
    obj->release();
    return (obj != kOSBooleanFalse);
}

