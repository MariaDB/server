/*****************************************************************************

Copyright (c) 1995, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file buf/buf0checksum.cc
Buffer pool checksum functions, also linked from /extra/innochecksum.cc

Created Aug 11, 2011 Vasil Dimov
*******************************************************/

#include "buf0checksum.h"
#include "fil0fil.h"
#include "my_sys.h"

#ifndef UNIV_INNOCHECKSUM
# include "srv0srv.h"
#endif /* !UNIV_INNOCHECKSUM */

/** Calculate the CRC32 checksum of a page. The value is stored to the page
when it is written to a file and also checked for a match when reading from
the file. Note that we must be careful to calculate the same value on all
architectures.
@param[in]	page			buffer page (srv_page_size bytes)
@return	CRC-32C */
uint32_t buf_calc_page_crc32(const byte *page) noexcept
{
	/* Note: innodb_checksum_algorithm=crc32 could and should have
	included the entire page in the checksum, and CRC-32 values
	should be combined with the CRC-32 function, not with
	exclusive OR. We stick to the current algorithm in order to
	remain compatible with old data files. */
	return my_crc32c(0, page + FIL_PAGE_OFFSET,
			 FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
                         - FIL_PAGE_OFFSET)
		^ my_crc32c(0, page + FIL_PAGE_DATA,
                            srv_page_size
                            - (FIL_PAGE_DATA + FIL_PAGE_END_LSN_OLD_CHKSUM));
}

#ifndef UNIV_INNOCHECKSUM
static inline ulint ut_fold_ulint_pair(ulint n1, ulint n2)
{
  return ((((n1 ^ n2 ^ 1653893711) << 8) + n1) ^ 1463735687) + n2;
}

/** Fold a binary string, similar to innodb_checksum_algorithm=innodb.
@return folded value */
ulint ut_fold_binary(const byte *str, size_t len) noexcept
{
  ulint fold= 0;
  for (const byte *const str_end= str + (len & 0xFFFFFFF8); str < str_end;
       str+= 8)
  {
    fold= ut_fold_ulint_pair(fold, str[0]);
    fold= ut_fold_ulint_pair(fold, str[1]);
    fold= ut_fold_ulint_pair(fold, str[2]);
    fold= ut_fold_ulint_pair(fold, str[3]);
    fold= ut_fold_ulint_pair(fold, str[4]);
    fold= ut_fold_ulint_pair(fold, str[5]);
    fold= ut_fold_ulint_pair(fold, str[6]);
    fold= ut_fold_ulint_pair(fold, str[7]);
  }

  switch (len & 0x7) {
  case 7:
    fold= ut_fold_ulint_pair(fold, *str++);
    /* fall through */
  case 6:
    fold= ut_fold_ulint_pair(fold, *str++);
    /* fall through */
  case 5:
    fold= ut_fold_ulint_pair(fold, *str++);
    /* fall through */
  case 4:
    fold= ut_fold_ulint_pair(fold, *str++);
    /* fall through */
  case 3:
    fold= ut_fold_ulint_pair(fold, *str++);
    /* fall through */
  case 2:
    fold= ut_fold_ulint_pair(fold, *str++);
    /* fall through */
  case 1:
    fold= ut_fold_ulint_pair(fold, *str++);
  }
  return fold;
}

/** Calculate a checksum which is stored to the page when it is written
to a file. Note that we must be careful to calculate the same value on
32-bit and 64-bit architectures.
@param[in]	page	file page (srv_page_size bytes)
@return checksum */
uint32_t buf_calc_page_new_checksum(const byte *page) noexcept
{
	ulint checksum;

	/* Since the field FIL_PAGE_FILE_FLUSH_LSN, and in versions <= 4.1.x
	FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, are written outside the buffer pool
	to the first pages of data files, we have to skip them in the page
	checksum calculation.
	We must also skip the field FIL_PAGE_SPACE_OR_CHKSUM where the
	checksum is stored, and also the last 8 bytes of page because
	there we store the old formula checksum. */

	checksum = ut_fold_binary(page + FIL_PAGE_OFFSET,
				  FIL_PAGE_FILE_FLUSH_LSN_OR_KEY_VERSION
				  - FIL_PAGE_OFFSET)
		+ ut_fold_binary(page + FIL_PAGE_DATA,
				 srv_page_size - FIL_PAGE_DATA
				 - FIL_PAGE_END_LSN_OLD_CHKSUM);
	return(static_cast<uint32_t>(checksum));
}
#endif /* !UNIV_INNOCHECKSUM */
