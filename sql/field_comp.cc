/* Copyright (C) 2017 MariaDB Foundation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */


#include <my_global.h>
#include "sql_string.h"
#include "sql_class.h"
#include "field_comp.h"
#include <zlib.h>


/**
  Compresses string using zlib

  @param[out]    to         destination buffer for compressed data
  @param[in]     from       data to compress
  @param[in]     length     from length

  Requirement is such that string stored at `to' must not exceed `from' length.
  Otherwise 0 is returned and caller stores string uncompressed.

  `to' must be large enough to hold `length' bytes.

  length == 1 is an edge case that may break stream.avail_out calculation: at
  least 2 bytes required to store metadata.
*/

static uint compress_zlib(THD *thd, char *to, const char *from, uint length)
{
  uint level= thd->variables.column_compression_zlib_level;

  /* Caller takes care of empty strings. */
  DBUG_ASSERT(length);

  if (level > 0 && length > 1)
  {
    z_stream stream;
    int wbits= thd->variables.column_compression_zlib_wrap ? MAX_WBITS :
                                                            -MAX_WBITS;
    uint strategy= thd->variables.column_compression_zlib_strategy;
    /* Store only meaningful bytes of original data length. */
    uchar original_pack_length= number_storage_requirement(length);

    *to= 0x80 + original_pack_length + (wbits < 0 ? 8 : 0);
    store_bigendian(length, (uchar*) to + 1, original_pack_length);

    stream.avail_in= length;
    stream.next_in= (Bytef*) from;

    DBUG_ASSERT(length >= static_cast<uint>(original_pack_length) + 1);
    stream.avail_out= length - original_pack_length - 1;
    stream.next_out= (Bytef*) to + original_pack_length + 1;

    stream.zalloc= 0;
    stream.zfree= 0;
    stream.opaque= 0;

    if (deflateInit2(&stream, level, Z_DEFLATED, wbits, 8, strategy) == Z_OK)
    {
      int res= deflate(&stream, Z_FINISH);
      if (deflateEnd(&stream) == Z_OK && res == Z_STREAM_END)
        return (uint) (stream.next_out - (Bytef*) to);
    }
  }
  return 0;
}


static int uncompress_zlib(String *to, const uchar *from, uint from_length,
                           uint field_length)
{
  z_stream stream;
  uchar original_pack_length;
  int wbits;
  ulonglong avail_out;

  original_pack_length= *from & 0x07;
  wbits= *from & 8 ? -MAX_WBITS : MAX_WBITS;

  from++;
  from_length--;

  if (from_length < original_pack_length)
  {
    my_error(ER_ZLIB_Z_DATA_ERROR, MYF(0));
    return 1;
  }

  avail_out= (ulonglong)read_bigendian(from, original_pack_length);

  if (avail_out > field_length)
  {
    my_error(ER_ZLIB_Z_DATA_ERROR, MYF(0));
    return 1;
  }

  stream.avail_out= (uint)avail_out;
  if (to->alloc(stream.avail_out))
    return 1;

  stream.next_out= (Bytef*) to->ptr();

  stream.avail_in= from_length - original_pack_length;
  stream.next_in= (Bytef*) from + original_pack_length;

  stream.zalloc= 0;
  stream.zfree= 0;
  stream.opaque= 0;

  if (inflateInit2(&stream, wbits) == Z_OK)
  {
    int res= inflate(&stream, Z_FINISH);
    if (inflateEnd(&stream) == Z_OK && res == Z_STREAM_END)
    {
      to->length(stream.total_out);
      return 0;
    }
  }
  my_error(ER_ZLIB_Z_DATA_ERROR, MYF(0));
  return 1;
}


Compression_method compression_methods[MAX_COMPRESSION_METHODS]=
{
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { "zlib", compress_zlib, uncompress_zlib },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 },
  { 0, 0, 0 }
};
