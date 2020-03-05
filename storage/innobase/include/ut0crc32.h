/*****************************************************************************

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

/**************************************************//**
@file include/ut0crc32.h
CRC32 implementation

Created Aug 10, 2011 Vasil Dimov
*******************************************************/

#ifndef ut0crc32_h
#define ut0crc32_h

#include "univ.i"

/********************************************************************//**
Initializes the data structures used by ut_crc32*(). Does not do any
allocations, would not hurt if called twice, but would be pointless. */
void ut_crc32_init();

/** Append data to a CRC-32C checksum.
@param crc   current checksum
@param s     data to append to the checksum
@param size  data length in bytes
@return CRC-32C, using the GF(2) primitive polynomial 0x11EDC6F41,
or 0x1EDC6F41 without the highest degree term */
typedef uint32_t (*ut_crc32_func_t)(uint32_t crc, const byte *s, size_t size);

/** Pointer to CRC32 calculation function. */
extern ut_crc32_func_t ut_crc32_low;

/** Text description of CRC32 implementation */
extern const char*	ut_crc32_implementation;

/** Compute CRC-32C over a string of bytes.
@param s     data
@param len   data length in bytes
@return the CRC-32C of the data */
static inline uint32_t ut_crc32(const byte *s, size_t size)
{
  return ut_crc32_low(0, s, size);
}

#endif /* ut0crc32_h */
