/*
 * Copyright (c) 2008 Apple Inc. All rights reserved.
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
#include "kdp_serial.h"
#include <libkern/zlib.h>
#include <stdint.h>
#include <stdbool.h>

#define SKDP_START_CHAR 0xFA
#define SKDP_END_CHAR 0xFB
#define SKDP_ESC_CHAR 0xFE

static enum {DS_WAITSTART, DS_READING, DS_ESCAPED} dsState;
static unsigned char dsBuffer[1518];
static int dsPos;
static uint32_t dsCRC;
static bool dsHaveCRC;


static void
kdp_serial_out(unsigned char byte, void (*outFunc)(char))
{
	//need to escape '\n' because the kernel serial output turns it into a cr/lf
	if (byte == SKDP_START_CHAR || byte == SKDP_END_CHAR || byte == SKDP_ESC_CHAR || byte == '\n') {
		outFunc(SKDP_ESC_CHAR);
		byte = ~byte;
	}
	outFunc(byte);
}

void
kdp_serialize_packet(unsigned char *packet, unsigned int len, void (*outFunc)(char))
{
	unsigned int  index;
	unsigned char byte;
	uint32_t      crc;

	// insert the CRC between back to back STARTs which is compatible with old clients
	crc = (uint32_t) z_crc32(0, packet, len);
	outFunc(SKDP_START_CHAR);
	kdp_serial_out((crc >> 0), outFunc);
	kdp_serial_out((crc >> 8), outFunc);
	kdp_serial_out((crc >> 16), outFunc);
	kdp_serial_out((crc >> 24), outFunc);

	outFunc(SKDP_START_CHAR);
	for (index = 0; index < len; index++) {
		byte = *packet++;
		kdp_serial_out(byte, outFunc);
	}
	outFunc(SKDP_END_CHAR);
}

unsigned char *
kdp_unserialize_packet(unsigned char byte, unsigned int *len)
{
	uint32_t crc;

	switch (dsState) {
	case DS_WAITSTART:
		if (byte == SKDP_START_CHAR) {
//				printf("got start char\n");
			dsState = DS_READING;
			dsPos = 0;
			*len = SERIALIZE_READING;
			dsHaveCRC = false;
			return 0;
		}
		*len = SERIALIZE_WAIT_START;
		break;
	case DS_READING:
		if (byte == SKDP_ESC_CHAR) {
			dsState = DS_ESCAPED;
			*len = SERIALIZE_READING;
			return 0;
		}
		if (byte == SKDP_START_CHAR) {
			if (dsPos >= 4) {
				dsHaveCRC = true;
				dsCRC = dsBuffer[0] | (dsBuffer[1] << 8) | (dsBuffer[2] << 16) | (dsBuffer[3] << 24);
			}
			//else				printf("unexpected start char, resetting\n");
			dsPos = 0;
			*len = SERIALIZE_READING;
			return 0;
		}
		if (byte == SKDP_END_CHAR) {
			dsState = DS_WAITSTART;
			if (dsHaveCRC) {
				crc = (uint32_t) z_crc32(0, &dsBuffer[0], dsPos);
				if (crc != dsCRC) {
//						printf("bad packet crc 0x%x != 0x%x\n", crc, dsCRC);
					dsPos = 0;
					*len = SERIALIZE_WAIT_START;
					return 0;
				}
			}
			*len = dsPos;
			dsPos = 0;
			return dsBuffer;
		}
		dsBuffer[dsPos++] = byte;
		break;
	case DS_ESCAPED:
//			printf("unescaping %02x to %02x\n", byte, ~byte);
		dsBuffer[dsPos++] = ~byte;
		dsState = DS_READING;
		*len = SERIALIZE_READING;
		break;
	}
	if (dsPos == sizeof(dsBuffer)) { //too much data...forget this packet
		dsState = DS_WAITSTART;
		dsPos = 0;
		*len = SERIALIZE_WAIT_START;
	}

	return 0;
}
