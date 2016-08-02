/*****************************************************************************

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

/**************************************************//**
@file include/ut0crc32.h
CRC32 implementation

Created Aug 10, 2011 Vasil Dimov
*******************************************************/

#ifndef ut0crc32_h
#define ut0crc32_h

#include <my_global.h>

#ifdef  __cplusplus
extern "C" {
#endif /* __cplusplus */

/********************************************************************//**
Initializes the data structures used by ut_crc32(). Does not do any
allocations, would not hurt if called twice, but would be pointless. */
void
ut_crc32_init();
/*===========*/

/********************************************************************//**
Calculates CRC32C.
@param ptr	- data over which to calculate CRC32.
@param len	- data length in bytes.
@return CRC32 (CRC-32C, using the GF(2) primitive polynomial 0x11EDC6F41,
or 0x1EDC6F41 without the high-order bit) */
typedef uint32 (*ib_ut_crc32_t)(const uint8* ptr, my_ulonglong len);
typedef uint32 (*ib_ut_crc32_ex_t)(uint32 crc, const uint8* ptr, my_ulonglong len);

extern ib_ut_crc32_t	ut_crc32c;
extern ib_ut_crc32_ex_t	ut_crc32c_ex;

/* IEEE CRC32 functions on primitive polynomial 0x04C11DB7 */
extern ib_ut_crc32_t	ut_crc32;
extern ib_ut_crc32_ex_t	ut_crc32_ex;

/** Text description of CRC32 implementation */
extern const char *ut_crc32_implementation;

#ifdef  __cplusplus
}
#endif /* __cplusplus */

#endif /* ut0crc32_h */
