/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#pragma once

#include <my_global.h>
#include <string.h>

/** The following function is used to fetch data from one byte.
@param[in]      b       pointer to a byte to read
@return ulint integer, >= 0, < 256 */
static inline uchar mach_read_from_1(const uchar *b)
{
  return ((uchar) (b[0]));
}

/** Reads a ulint stored in the little-endian format.
 @return unsigned long int */
static inline ulong
mach_read_from_2_little_endian(const uchar *buf) /*!< in: from where to read */
{
  return ((ulong) (buf[0]) | ((ulong) (buf[1]) << 8));
}

/** Reads a ulint stored in the little-endian format.
@param[in] buf      From where to read.
@param[in] buf_size From how many bytes to read.
@return unsigned long int */
static inline ulong mach_read_from_n_little_endian(const uchar *buf,
                                                   ulong buf_size)
{
  ulong n= 0;
  const uchar *ptr;

  ptr= buf + buf_size;

  for (;;)
  {
    ptr--;

    n= n << 8;

    n+= (ulong) (*ptr);

    if (ptr == buf)
    {
      break;
    }
  }

  return (n);
}

/** Reads a >= 5.0.3 format true VARCHAR length, in the MySQL row format, and
 returns a pointer to the data.
 @return pointer to the data, we skip the 1 or 2 bytes at the start
 that are used to store the len */
static inline const uchar *row_mysql_read_true_varchar(
    ulong *len,         /*!< out: variable-length field length */
    const uchar *field, /*!< in: field in the MySQL format */
    ulong lenlen)       /*!< in: storage length of len: either 1
                        or 2 bytes */
{
  if (lenlen == 2)
  {
    *len= mach_read_from_2_little_endian(field);

    return (field + 2);
  }

  *len= mach_read_from_1(field);

  return (field + 1);
}

/** Reads a reference to a BLOB in the MySQL format.
@param[out] len                 BLOB length.
@param[in] ref                  BLOB reference in the MySQL format.
@param[in] col_len              BLOB reference length (not BLOB length).
@return pointer to BLOB data */
static inline const uchar *
row_mysql_read_blob_ref(ulong *len, const uchar *ref, ulong col_len)
{
  uchar *data;

  *len= mach_read_from_n_little_endian(ref, col_len - 8);

  memcpy(&data, ref + col_len - 8, sizeof data);

  return (data);
}
