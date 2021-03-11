/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2021, MariaDB Corporation.

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
@file buf/buf0checksum.h
Buffer pool checksum functions, also linked from /extra/innochecksum.cc

Created Aug 11, 2011 Vasil Dimov
*******************************************************/

#pragma once
#include "buf0types.h"

/** Calculate the CRC32 checksum of a page. The value is stored to the page
when it is written to a file and also checked for a match when reading from
the file. Note that we must be careful to calculate the same value on all
architectures.
@param[in]	page			buffer page (srv_page_size bytes)
@return	CRC-32C */
uint32_t buf_calc_page_crc32(const byte* page);

#ifndef UNIV_INNOCHECKSUM
/** Calculate a checksum which is stored to the page when it is written
to a file. Note that we must be careful to calculate the same value on
32-bit and 64-bit architectures.
@param[in]	page	file page (srv_page_size bytes)
@return checksum */
uint32_t
buf_calc_page_new_checksum(const byte* page);

/** In MySQL before 4.0.14 or 4.1.1 there was an InnoDB bug that
the checksum only looked at the first few bytes of the page.
This calculates that old checksum.
NOTE: we must first store the new formula checksum to
FIL_PAGE_SPACE_OR_CHKSUM before calculating and storing this old checksum
because this takes that field as an input!
@param[in]	page	file page (srv_page_size bytes)
@return checksum */
uint32_t
buf_calc_page_old_checksum(const byte* page);
#endif /* !UNIV_INNOCHECKSUM */
