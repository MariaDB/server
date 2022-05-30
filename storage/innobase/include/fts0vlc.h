/**

Copyright (c) 2021, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA
**/
/**
@file include/fts0vlc.h
Full text variable length integer encoding/decoding.

Created 2021-10-19 Thirunarayanan Balathandayuthapani
**/

/** Return length of val if it were encoded using our VLC scheme.
@param	val	value to encode
@return length of value encoded, in bytes */
inline size_t fts_get_encoded_len(doc_id_t val)
{
  if (val < static_cast<doc_id_t>(1) << 7)
    return 1;
  if (val < static_cast<doc_id_t>(1) << 14)
    return 2;
  if (val < static_cast<doc_id_t>(1) << 21)
    return 3;
  if (val < static_cast<doc_id_t>(1) << 28)
    return 4;
  if (val < static_cast<doc_id_t>(1) << 35)
    return 5;
  if (val < static_cast<doc_id_t>(1) << 42)
    return 6;
  if (val < static_cast<doc_id_t>(1) << 49)
    return 7;
  if (val < static_cast<doc_id_t>(1) << 56)
    return 8;
  if (val < static_cast<doc_id_t>(1) << 63)
    return 9;
  return 10;
}

/** Encode an integer using our VLC scheme and return the
length in bytes.
@param	val	value to encode
@param	buf	buffer, must have enough space
@return length of value encoded, in bytes */
inline byte *fts_encode_int(doc_id_t val, byte *buf)
{
  if (val < static_cast<doc_id_t>(1) << 7)
    goto add_1;
  if (val < static_cast<doc_id_t>(1) << 14)
    goto add_2;
  if (val < static_cast<doc_id_t>(1) << 21)
    goto add_3;
  if (val < static_cast<doc_id_t>(1) << 28)
    goto add_4;
  if (val < static_cast<doc_id_t>(1) << 35)
    goto add_5;
  if (val < static_cast<doc_id_t>(1) << 42)
    goto add_6;
  if (val < static_cast<doc_id_t>(1) << 49)
    goto add_7;
  if (val < static_cast<doc_id_t>(1) << 56)
    goto add_8;
  if (val < static_cast<doc_id_t>(1) << 63)
    goto add_9;

  *buf++= static_cast<byte>(val >> 63);
add_9:
  *buf++= static_cast<byte>(val >> 56) & 0x7F;
add_8:
  *buf++= static_cast<byte>(val >> 49) & 0x7F;
add_7:
  *buf++= static_cast<byte>(val >> 42) & 0x7F;
add_6:
  *buf++= static_cast<byte>(val >> 35) & 0x7F;
add_5:
  *buf++= static_cast<byte>(val >> 28) & 0x7F;
add_4:
  *buf++= static_cast<byte>(val >> 21) & 0x7F;
add_3:
  *buf++= static_cast<byte>(val >> 14) & 0x7F;
add_2:
  *buf++= static_cast<byte>(val >> 7) & 0x7F;
add_1:
  *buf++= static_cast<byte>(val) | 0x80;
  return buf;
}

/** Decode and return the integer that was encoded using
our VLC scheme.
@param	ptr 	pointer to decode from, this ptr is
		incremented by the number of bytes decoded
@return value decoded */
inline doc_id_t fts_decode_vlc(const byte **ptr)
{
  ut_d(const byte *const start= *ptr);
  ut_ad(*start);

  doc_id_t val= 0;
  for (;;)
  {
    byte b= *(*ptr)++;
    val|= (b & 0x7F);

    /* High-bit on means "last byte in the encoded integer". */
    if (b & 0x80)
      break;
    ut_ad(val < static_cast<doc_id_t>(1) << (64 - 7));
    val <<= 7;
  }

  ut_ad(*ptr - start <= 10);

  return(val);
}
