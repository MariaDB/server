/*****************************************************************************

Copyright (c) 2024 Kristian Nielsen.

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
/******************************************************************//**
@file include/ut0bitop.h
Reading and writing of compressed integers.

Created 2024-10-01 Kristian Nielsen <knielsen@knielsen-hq.org>
*******************************************************/

#ifndef INNOBASE_UT0COMPR_INT_H
#define INNOBASE_UT0COMPR_INT_H

#include "ut0bitop.h"
#include <stdint.h>
#include <utility>


/*
  Read and write compressed (up to) 64-bit integers.

  A 64-bit number is encoded with 1-9 bytes. The 3 first bits stores a tag
  that determines the number of bytes used, and the encoding is written in
  little-endian format as (TAG | (NUMBER << 3)). The tag is the number of
  bytes used minus 1, except that 7 denotes 9 bytes used (numbers are never
  encoded with 8 bytes). For example:

    Number             Encoding
         0              0x00
      0x1f              0xf8       (0 | (0x1f << 3))
      0x20              0x01 0x01
      0xf6              0xb1 0x07
    0xd34a              0x52 0x9a 0x06
      0x1fffffffffffff  0xfe 0xff 0xff 0xff 0xff 0xff 0xff
      0x20000000000000  0x07 0x00 0x00 0x00 0x00 0x00 0x00 0x01 0x00
    0xffffffffffffffff  0xff 0xff 0xff 0xff 0xff 0xff 0xff 0xff 0x07

  The main advantage over something like base-128 compression (also called
  varint) is that the encoding and decoding can happen with just a single
  conditional jump to determine if one or two 64-bit words are involved (or
  even no or only well-predicted conditional jump if unaligned reads/writes
  and buffer padding can be assumed).
*/

#define COMPR_INT_MAX32 5
#define COMPR_INT_MAX64 9
#define COMPR_INT_MAX COMPR_INT_MAX_64

/* Write compressed unsigned integer */
extern unsigned char *compr_int_write(unsigned char *p, uint64_t v);
/*
  Read compressed integer.
  Returns a pair of the value read and the incremented pointer.
*/
extern std::pair<uint64_t, const unsigned char *>
  compr_int_read(const unsigned char *p);

#endif /* INNOBASE_UT0COMPR_INT_H */
