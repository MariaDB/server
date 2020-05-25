/*****************************************************************************

Copyright (c) 2009, 2010 Facebook, Inc. All Rights Reserved.
Copyright (c) 2011, 2015, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2016, 2020, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/***************************************************************//**
@file ut/ut0crc32.cc
CRC32 implementation from Facebook, based on the zlib implementation.

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
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* The below CRC32 implementation is based on the implementation included with
 * zlib with modifications to process 8 bytes at a time and using SSE 4.2
 * extensions when available.  The polynomial constant has been changed to
 * match the one used by SSE 4.2 and does not return the same value as the
 * version used by zlib.  The original zlib copyright notice follows. */

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

// First include (the generated) my_config.h, to get correct platform defines.
#include "my_config.h"
#include <string.h>

#include "ut0crc32.h"

#ifdef _MSC_VER
# include <intrin.h>
#endif

/* CRC32 hardware implementation. */

#ifdef HAVE_CRC32_VPMSUM
extern "C"
unsigned int crc32c_vpmsum(unsigned int crc, const unsigned char *p, unsigned long len);
ut_crc32_func_t ut_crc32_low= crc32c_vpmsum;
const char*	ut_crc32_implementation = "Using POWER8 crc32 instructions";
#else
# if defined(__GNUC__) && defined(__linux__) && defined(HAVE_ARMV8_CRC)
extern "C" {
uint32_t crc32c_aarch64(uint32_t crc, const unsigned char *buffer, uint64_t len);
/* For runtime check  */
unsigned int crc32c_aarch64_available(void);
};
# elif defined(_MSC_VER)
#  define TRY_SSE4_2
# elif defined (__GNUC__)
#  ifdef __x86_64__
#   define TRY_SSE4_2
#  elif defined(__i386__) && (__GNUC__ > 4 || defined __clang__)
#   define TRY_SSE4_2
#  endif
# endif

# ifdef TRY_SSE4_2
/** return whether SSE4.2 instructions are available */
static inline bool has_sse4_2()
{
  /* We assume that the CPUID instruction and its parameter 1 are available.
  We do not support any precursors of the Intel 80486. */
#  ifdef _MSC_VER
  int data[4];
  __cpuid(data, 1);
  return !!(data[2] & 1 << 20);
#  else
  uint32_t eax, ecx;
  asm("cpuid" : "=a"(eax), "=c"(ecx) : "a"(1) : "ebx", "edx");
  return !!(ecx & 1 << 20);
#  endif
}

/** Append 8 bits (1 byte) to a CRC-32C checksum.
@param crc   CRC-32C checksum so far
@param data  data to be checksummed
@return the updated CRC-32C */
static inline ulint ut_crc32c_8(ulint crc, byte data)
{
#  ifdef _MSC_VER
  return _mm_crc32_u8(static_cast<uint32_t>(crc), data);
#  elif __has_feature(memory_sanitizer)
  return __builtin_ia32_crc32qi(static_cast<uint32_t>(crc), data);
#  else
  asm("crc32b %1, %0" : "+r" (crc) : "rm" (data));
  return crc;
#  endif
}

/** Append 64 bits (8 aligned bytes) to a CRC-32C checksum
@param[in] crc    CRC-32C checksum so far
@param[in] data   8 bytes of aligned data
@return the updated CRC-32C */
static inline ulint ut_crc32c_64(ulint crc, uint64_t data)
{
#  ifdef _MSC_VER
#   ifdef _M_X64
  return _mm_crc32_u64(crc, data);
#   elif defined(_M_IX86)
  crc= _mm_crc32_u32(crc, static_cast<uint32_t>(data));
  crc= _mm_crc32_u32(crc, static_cast<uint32_t>(data >> 32));
  return crc;
#   else
#    error Unsupported processor type
#   endif
#  elif __has_feature(memory_sanitizer)
  return __builtin_ia32_crc32di(crc, data);
#  elif defined __x86_64__
  asm("crc32q %1, %0" : "+r" (crc) : "rm" (data));
  return crc;
#  else
  asm("crc32l %1, %0" : "+r" (crc) : "rm" (static_cast<uint32_t>(data)));
  asm("crc32l %1, %0" : "+r" (crc) : "rm" (static_cast<uint32_t>(data >> 32)));
  return crc;
#  endif
}

/** Calculate CRC-32C using dedicated IA-32 or AMD64 instructions
@param crc   current checksum
@param buf   data to append to the checksum
@param len   data length in bytes
@return CRC-32C (polynomial 0x11EDC6F41) */
uint32_t ut_crc32_hw(uint32_t crc, const byte *buf, size_t len)
{
  ulint c= static_cast<uint32_t>(~crc);

  /* Calculate byte-by-byte up to an 8-byte aligned address. After
  this consume the input 8-bytes at a time. */
  while (len > 0 && (reinterpret_cast<uintptr_t>(buf) & 7) != 0)
  {
    c= ut_crc32c_8(c, *buf++);
    len--;
  }

  const uint64_t* b64= reinterpret_cast<const uint64_t*>(buf);

  for (; len >= 128; len-= 128)
  {
    /* This call is repeated 16 times. 16 * 8 = 128. */
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
    c= ut_crc32c_64(c, *b64++);
  }

  for (; len >= 8; len-= 8)
    c= ut_crc32c_64(c, *b64++);

  buf= reinterpret_cast<const byte*>(b64);

  while (len--)
    c= ut_crc32c_8(c, *buf++);

  return ~static_cast<uint32_t>(c);
}
# endif /* (defined(__GNUC__) && defined(__i386__)) || _MSC_VER */

/* CRC32 software implementation. */

/* Precalculated table used to generate the CRC32 if the CPU does not
have support for it */
static uint32_t	ut_crc32_slice8_table[8][256];

/********************************************************************//**
Initializes the table that is used to generate the CRC32 if the CPU does
not have support for it. */
static
void
ut_crc32_slice8_table_init()
/*========================*/
{
	/* bit-reversed poly 0x1EDC6F41 (from SSE42 crc32 instruction) */
	static const uint32_t	poly = 0x82f63b78;
	uint32_t		n;
	uint32_t		k;
	uint32_t		c;

	for (n = 0; n < 256; n++) {
		c = n;
		for (k = 0; k < 8; k++) {
			c = (c & 1) ? (poly ^ (c >> 1)) : (c >> 1);
		}
		ut_crc32_slice8_table[0][n] = c;
	}

	for (n = 0; n < 256; n++) {
		c = ut_crc32_slice8_table[0][n];
		for (k = 1; k < 8; k++) {
			c = ut_crc32_slice8_table[0][c & 0xFF] ^ (c >> 8);
			ut_crc32_slice8_table[k][n] = c;
		}
	}
}

/** Append 8 bits (1 byte) to a CRC-32C checksum.
@param crc   CRC-32C checksum so far
@param data  data to be checksummed
@return the updated CRC-32C */
static inline uint32_t ut_crc32c_8_sw(uint32_t crc, byte data)
{
  const uint8_t i= (crc ^ data) & 0xFF;

  return (crc >> 8) ^ ut_crc32_slice8_table[0][i];
}

/** Append 64 bits (8 aligned bytes) to a CRC-32C checksum
@param[in] crc    CRC-32C checksum so far
@param[in] data   8 bytes of aligned data
@return the updated CRC-32C */
static inline uint32_t ut_crc32c_64_sw(uint32_t crc, uint64_t data)
{
# ifdef WORDS_BIGENDIAN
  data= data << 56 |
    (data & 0x000000000000FF00ULL) << 40 |
    (data & 0x0000000000FF0000ULL) << 24 |
    (data & 0x00000000FF000000ULL) << 8 |
    (data & 0x000000FF00000000ULL) >> 8 |
    (data & 0x0000FF0000000000ULL) >> 24 |
    (data & 0x00FF000000000000ULL) >> 40 |
    data >> 56;
# endif /* WORDS_BIGENDIAN */

  data^= crc;
  return
    ut_crc32_slice8_table[7][(data      ) & 0xFF] ^
    ut_crc32_slice8_table[6][(data >>  8) & 0xFF] ^
    ut_crc32_slice8_table[5][(data >> 16) & 0xFF] ^
    ut_crc32_slice8_table[4][(data >> 24) & 0xFF] ^
    ut_crc32_slice8_table[3][(data >> 32) & 0xFF] ^
    ut_crc32_slice8_table[2][(data >> 40) & 0xFF] ^
    ut_crc32_slice8_table[1][(data >> 48) & 0xFF] ^
    ut_crc32_slice8_table[0][(data >> 56)];
}

/** Calculate CRC-32C using a look-up table.
@param crc   current checksum
@param buf   data to append to the checksum
@param len   data length in bytes
@return CRC-32C (polynomial 0x11EDC6F41) */
uint32_t ut_crc32_sw(uint32_t crc, const byte *buf, size_t len)
{
  crc= ~crc;

  /* Calculate byte-by-byte up to an 8-byte aligned address. After
  this consume the input 8-bytes at a time. */
  while (len > 0 && (reinterpret_cast<uintptr_t>(buf) & 7) != 0)
  {
    crc= ut_crc32c_8_sw(crc, *buf++);
    len--;
  }

  const uint64_t* b64= reinterpret_cast<const uint64_t*>(buf);

  for (; len >= 8; len-= 8)
    crc= ut_crc32c_64_sw(crc, *b64++);

  buf= reinterpret_cast<const byte*>(b64);

  while (len--)
    crc= ut_crc32c_8_sw(crc, *buf++);

  return ~crc;
}

ut_crc32_func_t ut_crc32_low= ut_crc32_sw;
const char *ut_crc32_implementation= "Using generic crc32 instructions";
#endif

/********************************************************************//**
Initializes the data structures used by ut_crc32*(). Does not do any
allocations, would not hurt if called twice, but would be pointless. */
void ut_crc32_init()
{
#ifndef HAVE_CRC32_VPMSUM
# if defined(__GNUC__) && defined(__linux__) && defined(HAVE_ARMV8_CRC)
  if (crc32c_aarch64_available())
  {
    ut_crc32_low= crc32c_aarch64;
    ut_crc32_implementation= "Using ARMv8 crc32 instructions";
    return;
  }
# elif defined(TRY_SSE4_2)
  if (has_sse4_2())
  {
    ut_crc32_low= ut_crc32_hw;
    ut_crc32_implementation= "Using SSE4.2 crc32 instructions";
    return;
  }
# endif
  ut_crc32_slice8_table_init();
#endif /* !HAVE_CRC32_VPMSUM */
}
