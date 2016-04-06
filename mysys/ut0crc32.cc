/*****************************************************************************

Copyright (C) 2009, 2010 Facebook, Inc. All Rights Reserved.
Copyright (c) 2011, 2011, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/***************************************************************//**
@file ut/ut0crc32.cc
CRC32C implementation from Facebook, based on the zlib implementation.

Created Aug 8, 2011, Vasil Dimov, based on mysys/my_crc32.c and
mysys/my_perf.c, contributed by Facebook under the following license.
********************************************************************/

/* Copyright (C) 2009-2010 Facebook, Inc.  All Rights Reserved.

   Dual licensed under BSD license and GPLv2.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions are met:
   1. Redistributions of source code must retain the above copyright notice,
      this list of conditions and the following disclaimer.
   2. Redistributions in binary form must reproduce the above copyright notice,
      this list of conditions and the following disclaimer in the documentation
      and/or other materials provided with the distribution.

   THIS SOFTWARE IS PROVIDED BY FACEBOOK, INC. ``AS IS'' AND ANY EXPRESS OR
   IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
   MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO
   EVENT SHALL FACEBOOK, INC. BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS;
   OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
   WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR
   OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF
   ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

   This program is free software; you can redistribute it and/or modify it
   under the terms of the GNU General Public License as published by the Free
   Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful, but WITHOUT
   ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
   FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
   more details.

   You should have received a copy of the GNU General Public License along with
   this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA */

/* The below CRC32 implementation is based on the implementation included with
 * zlib with modifications to process 8 bytes at a time and using SSE 4.2
 * extentions when available.  The polynomial constant has been changed to
 * match the one used by SSE 4.2 and does not return the same value as the
 * version used by zlib.  This implementation only supports 64-bit
 * little-endian processors.  The original zlib copyright notice follows. */

/* crc32.c -- compute the CRC-32 of a buf stream
 * Copyright (C) 1995-2005 Mark Adler
 * For conditions of distribution and use, see copyright notice in zlib.h
 *
 * Thanks to Rodney Brown <rbrown64@csc.com.au> for his contribution of faster
 * CRC methods: exclusive-oring 32 bits of buf at a time, and pre-computing
 * tables for updating the shift register in one step with three exclusive-ors
 * instead of four steps with four exclusive-ors.  This results in about a
 * factor of two increase in speed on a Power PC G4 (PPC7455) using gcc -O3.
 */

#include "ut0crc32.h"

#if defined(__linux__) && defined(__powerpc__)
/* Used to detect at runtime if we have vpmsum instructions (PowerISA 2.07) */
#include <sys/auxv.h>
#include <bits/hwcap.h>
#endif /* defined(__linux__) && defined(__powerpc__) */

#include <string.h>

ib_ut_crc32_t		ut_crc32c;
ib_ut_crc32_ex_t	ut_crc32c_ex;

ib_ut_crc32_t		ut_crc32;
ib_ut_crc32_ex_t	ut_crc32_ex;

/* Precalculated table used to generate the CRC32 if the CPU does not
have support for it */
static uint32	ut_crc32c_slice8_table[8][256];
static bool	ut_crc32c_slice8_table_initialized = FALSE;
static uint32	ut_crc32_slice8_table[8][256];
static bool	ut_crc32_slice8_table_initialized = FALSE;

/** Text description of CRC32 implementation */
const char *ut_crc32_implementation = NULL;

/********************************************************************//**
Initializes the table that is used to generate the CRC32 if the CPU does
not have support for it. */
static
void
ut_crc32_slice8_table_init(uint32 poly, uint32 slice8_table[8][256])
/*========================*/
{
	uint32			n;
	uint32			k;
	uint32			c;

	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++) {
			c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
		}
		slice8_table[0][n] = c;
	}

	for (n = 0; n < 256; n++) {
		c = slice8_table[0][n];
		for (k = 1; k < 8; k++) {
			c = slice8_table[0][c & 0xFF] ^ (c >> 8);
			slice8_table[k][n] = c;
		}
	}

}

static
void
ut_crc32c_slice8_table_init()
{
	/* bit-reversed poly 0x1EDC6F41 for CRC32C */
	ut_crc32_slice8_table_init(0x82f63b78, ut_crc32c_slice8_table);

	ut_crc32c_slice8_table_initialized = true;
}

static
void
ut_crc32_slice8_table_init()
{
	/* bit reversed poly 0x04C11DB7 for IEEE CRC */
	ut_crc32_slice8_table_init(0xEDB88320, ut_crc32_slice8_table);

	ut_crc32_slice8_table_initialized = true;
}


#if defined(__GNUC__) && defined(__x86_64__)
/********************************************************************//**
Fetches CPU info */
static
void
ut_cpuid(
/*=====*/
	uint32	vend[3],	/*!< out: CPU vendor */
	uint32*	model,		/*!< out: CPU model */
	uint32*	family,		/*!< out: CPU family */
	uint32*	stepping,	/*!< out: CPU stepping */
	uint32*	features_ecx,	/*!< out: CPU features ecx */
	uint32*	features_edx)	/*!< out: CPU features edx */
{
	uint32	sig;
	asm("cpuid" : "=b" (vend[0]), "=c" (vend[2]), "=d" (vend[1]) : "a" (0));
	asm("cpuid" : "=a" (sig), "=c" (*features_ecx), "=d" (*features_edx)
	    : "a" (1)
	    : "ebx");

	*model = ((sig >> 4) & 0xF);
	*family = ((sig >> 8) & 0xF);
	*stepping = (sig & 0xF);

	if (memcmp(vend, "GenuineIntel", 12) == 0
	    || (memcmp(vend, "AuthenticAMD", 12) == 0 && *family == 0xF)) {

		*model += (((sig >> 16) & 0xF) << 4);
		*family += ((sig >> 20) & 0xFF);
	}
}

/* opcodes taken from objdump of "crc32b (%%rdx), %%rcx"
for RHEL4 support (GCC 3 doesn't support this instruction) */
#define ut_crc32c_sse42_byte \
	asm(".byte 0xf2, 0x48, 0x0f, 0x38, 0xf0, 0x0a" \
	    : "=c"(crc) : "c"(crc), "d"(buf)); \
	len--, buf++

/* opcodes taken from objdump of "crc32q (%%rdx), %%rcx"
for RHEL4 support (GCC 3 doesn't support this instruction) */
#define ut_crc32c_sse42_quadword \
	asm(".byte 0xf2, 0x48, 0x0f, 0x38, 0xf1, 0x0a" \
	    : "=c"(crc) : "c"(crc), "d"(buf)); \
	len -= 8, buf += 8
#endif /* defined(__GNUC__) && defined(__x86_64__) */



/********************************************************************//**
Calculates CRC32 using CPU instructions.
@return CRC-32C (polynomial 0x11EDC6F41) */
inline
uint32
ut_crc32c_ex_sse42(
/*===========*/
	uint32		crc_arg,
	const uint8*	buf,	/*!< in: data over which to calculate CRC32 */
	my_ulonglong	len)	/*!< in: data length */
{
#if defined(__GNUC__) && defined(__x86_64__)
	uint64 crc = crc_arg ^(uint32) (-1);

	while (len && ((my_ulonglong) buf & 7)) {
		ut_crc32c_sse42_byte;
	}

	while (len >= 32) {
		ut_crc32c_sse42_quadword;
		ut_crc32c_sse42_quadword;
		ut_crc32c_sse42_quadword;
		ut_crc32c_sse42_quadword;
	}

	while (len >= 8) {
		ut_crc32c_sse42_quadword;
	}

	while (len) {
		ut_crc32c_sse42_byte;
	}

	return((uint32) ((~crc) & 0xFFFFFFFF));
#else
	MY_ASSERT_UNREACHABLE();
	/* silence compiler warning about unused parameters */
	return((uint32) buf[len]);
#endif /* defined(__GNUC__) && defined(__x86_64__) */
}

inline
uint32
ut_crc32c_sse42(
/*===========*/
	const uint8*	buf,	/*!< in: data over which to calculate CRC32 */
	my_ulonglong	len)	/*!< in: data length */
{
	return ut_crc32c_ex_sse42(0UL, buf, len);
}

#define ut_crc32_slice8_byte \
	crc = (crc >> 8) ^ slice8_table[0][(crc ^ *buf++) & 0xFF]; \
	len--

#define ut_crc32_slice8_quadword \
	crc ^= *(uint64*) buf; \
	crc = slice8_table[7][(crc      ) & 0xFF] ^ \
	      slice8_table[6][(crc >>  8) & 0xFF] ^ \
	      slice8_table[5][(crc >> 16) & 0xFF] ^ \
	      slice8_table[4][(crc >> 24) & 0xFF] ^ \
	      slice8_table[3][(crc >> 32) & 0xFF] ^ \
	      slice8_table[2][(crc >> 40) & 0xFF] ^ \
	      slice8_table[1][(crc >> 48) & 0xFF] ^ \
	      slice8_table[0][(crc >> 56)]; \
	len -= 8, buf += 8

/********************************************************************//**
Calculates CRC32 manually.
@return CRC-32C (polynomial 0x11EDC6F41) */

inline
static
uint32
ut_crc32_slice8_common(
/*============*/
        uint32 crc_arg,
        const uint8*    buf,    /*!< in: data over which to calculate CRC32 */
        my_ulonglong    len,    /*!< in: data length */
	uint32		slice8_table[8][256])
{
	uint64 crc = crc_arg ^ (uint32) (-1);

	while (len && ((my_ulonglong) buf & 7)) {
		ut_crc32_slice8_byte;
	}

	while (len >= 32) {
		ut_crc32_slice8_quadword;
		ut_crc32_slice8_quadword;
		ut_crc32_slice8_quadword;
		ut_crc32_slice8_quadword;
	}

	while (len >= 8) {
		ut_crc32_slice8_quadword;
	}

	while (len) {
		ut_crc32_slice8_byte;
	}

	return((uint32) ((~crc) & 0xFFFFFFFF));
}

inline
uint32
ut_crc32c_ex_slice8(
/*============*/
        uint32 crc_arg,
        const uint8*    buf,    /*!< in: data over which to calculate CRC32 */
        my_ulonglong    len)    /*!< in: data length */
{
	DBUG_ASSERT(ut_crc32c_slice8_table_initialized);

	return ut_crc32_slice8_common(crc_arg, buf, len, ut_crc32c_slice8_table);
}

inline
uint32
ut_crc32c_slice8(
/*============*/
        const uint8*    buf,    /*!< in: data over which to calculate CRC32 */
        my_ulonglong    len)    /*!< in: data length */
{
	DBUG_ASSERT(ut_crc32c_slice8_table_initialized);

	return ut_crc32_slice8_common(0UL, buf, len, ut_crc32c_slice8_table);
}

inline
uint32
ut_crc32_ex_slice8(
/*============*/
        uint32 crc_arg,
        const uint8*    buf,    /*!< in: data over which to calculate CRC32 */
        my_ulonglong    len)    /*!< in: data length */
{
	DBUG_ASSERT(ut_crc32_slice8_table_initialized);

	return ut_crc32_slice8_common(crc_arg, buf, len, ut_crc32_slice8_table);
}

inline
uint32
ut_crc32_slice8(
/*============*/
        const uint8*    buf,    /*!< in: data over which to calculate CRC32 */
        my_ulonglong    len)    /*!< in: data length */
{
	DBUG_ASSERT(ut_crc32_slice8_table_initialized);

	return ut_crc32_slice8_common(0UL, buf, len, ut_crc32_slice8_table);
}


#if defined(__powerpc__)
extern "C" {
unsigned int crc32c_vpmsum(unsigned int crc, const unsigned char *p, unsigned long len);
unsigned int crc32_vpmsum(unsigned int crc, const unsigned char *p, unsigned long len);
};
#endif /* __powerpc__ */

inline
uint32
ut_crc32c_power8(
/*===========*/
		 const uint8*		 buf,		 /*!< in: data over which to calculate CRC32 */
		 my_ulonglong 		 len)		 /*!< in: data length */
{
#if defined(__powerpc__) && !defined(WORDS_BIGENDIAN)
		 return crc32c_vpmsum(0, buf, len);
#else
		 MY_ASSERT_UNREACHABLE();
		 /* silence compiler warning about unused parameters */
		 return((uint32) buf[len]);
#endif /* __powerpc__ */
}

inline
uint32
ut_crc32c_ex_power8(
/*===========*/
		 uint32			 crc,
		 const uint8*		 buf,		 /*!< in: data over which to calculate CRC32 */
		 my_ulonglong 		 len)		 /*!< in: data length */
{
#if defined(__powerpc__) && !defined(WORDS_BIGENDIAN)
		 return crc32c_vpmsum(crc, buf, len);
#else
		 MY_ASSERT_UNREACHABLE();
		 /* silence compiler warning about unused parameters */
		 return((uint32) buf[len]);
#endif /* __powerpc__ */
}

inline
uint32
ut_crc32_power8(
/*===========*/
		 const uint8*		 buf,		 /*!< in: data over which to calculate CRC32 */
		 my_ulonglong 		 len)		 /*!< in: data length */
{
#if defined(__powerpc__) && !defined(WORDS_BIGENDIAN)
		 return crc32_vpmsum(0, buf, len);
#else
		 MY_ASSERT_UNREACHABLE();
		 /* silence compiler warning about unused parameters */
		 return((uint32) buf[len]);
#endif /* __powerpc__ */
}

inline
uint32
ut_crc32_ex_power8(
/*===========*/
		 uint32			 crc,
		 const uint8*		 buf,		 /*!< in: data over which to calculate CRC32 */
		 my_ulonglong 		 len)		 /*!< in: data length */
{
#if defined(__powerpc__) && !defined(WORDS_BIGENDIAN)
		 return crc32_vpmsum(crc, buf, len);
#else
		 MY_ASSERT_UNREACHABLE();
		 /* silence compiler warning about unused parameters */
		 return((uint32) buf[len]);
#endif /* __powerpc__ */
}

/********************************************************************//**
Initializes the data structures used by ut_crc32(). Does not do any
allocations, would not hurt if called twice, but would be pointless. */
void
ut_crc32_init()
/*===========*/
{
	bool		ut_crc32_sse2_enabled = false;
	bool		ut_crc32_power8_enabled = false;
#if defined(__GNUC__) && defined(__x86_64__)
	uint32	vend[3];
	uint32	model;
	uint32	family;
	uint32	stepping;
	uint32	features_ecx;
	uint32	features_edx;

	ut_cpuid(vend, &model, &family, &stepping,
		 &features_ecx, &features_edx);

	/* Valgrind does not understand the CRC32 instructions:

	vex amd64->IR: unhandled instruction bytes: 0xF2 0x48 0xF 0x38 0xF0 0xA
	valgrind: Unrecognised instruction at address 0xad3db5.
	Your program just tried to execute an instruction that Valgrind
	did not recognise.  There are two possible reasons for this.
	1. Your program has a bug and erroneously jumped to a non-code
	   location.  If you are running Memcheck and you just saw a
	   warning about a bad jump, it's probably your program's fault.
	2. The instruction is legitimate but Valgrind doesn't handle it,
	   i.e. it's Valgrind's fault.  If you think this is the case or
	   you are not sure, please let us know and we'll try to fix it.
	Either way, Valgrind will now raise a SIGILL signal which will
	probably kill your program.

	*/
#ifndef UNIV_DEBUG_VALGRIND
	ut_crc32_sse2_enabled = (features_ecx >> 20) & 1;
#endif /* UNIV_DEBUG_VALGRIND */

#endif /* defined(__GNUC__) && defined(__x86_64__) */

#if defined(__linux__) && defined(__powerpc__) && defined(AT_HWCAP2) \
        && !defined(WORDS_BIGENDIAN)
	if (getauxval(AT_HWCAP2) & PPC_FEATURE2_ARCH_2_07)
		 ut_crc32_power8_enabled = true;
#endif /* defined(__linux__) && defined(__powerpc__) */

	if (ut_crc32_sse2_enabled) {
		ut_crc32c = ut_crc32c_sse42;
		ut_crc32c_ex = ut_crc32c_ex_sse42;

		ut_crc32_slice8_table_init();
		ut_crc32 = ut_crc32_slice8;
		ut_crc32_ex = ut_crc32_ex_slice8;
		ut_crc32_implementation = "Using SSE2 crc32c instructions";
	} else if (ut_crc32_power8_enabled) {
		ut_crc32c = ut_crc32c_power8;
		ut_crc32c_ex = ut_crc32c_ex_power8;
		ut_crc32 = ut_crc32_power8;
		ut_crc32_ex = ut_crc32_ex_power8;
		ut_crc32_implementation = "Using POWER8 crc32c instructions";
	} else {
		ut_crc32c_slice8_table_init();
		ut_crc32c = ut_crc32c_slice8;
		ut_crc32c_ex = ut_crc32c_ex_slice8;

		ut_crc32_slice8_table_init();
		ut_crc32 = ut_crc32_slice8;
		ut_crc32_ex = ut_crc32_ex_slice8;
		ut_crc32_implementation = "Using generic crc32c instructions";
	}
}
