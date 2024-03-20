#ifndef SQL_TYPE_UUID_V4_INCLUDED
#define SQL_TYPE_UUID_V4_INCLUDED

/* Copyright (c) 2024, Stefano Petrilli

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

/*
  Implements Universal Unique Identifiers version 4, as described in
  draft-ietf-uuidrev-rfc4122bis-14.

    Field                       Octet #          Note
  random_a                       0-5     Random CSPRNG 48 bits.
  ver                            6       The 4 bit version field, set to
                                         0b0100. Occupies bits 48 through 51 of
                                         octet 6.
  random_b                       6-7     Random CSPRNG 12 bits.
  var                            8       The 2 bit variant field, set to 0b10.
                                         Occupies bits 64 and 65 of octet 8.
  random_c                       8-15    Random CSPRNG 62 bits.

  The structure of an UUIDv4 is: llllllll-mmmm-Vhhh-vsss-nnnnnnnnnnnn
  The replacement of the version and variant field bits results in 122
  bits of random data.
*/

#include "my_rnd.h"

#define UUID_VERSION                         0x40
#define UUID_VERSION_MASK                    0x0F
#define UUID_VARIANT                         0x80
#define UUID_VARIANT_MASK                    0x3F

/**
   Create a global unique identifier version 4 (uuidv4)

   @func  my_uuid_v4()
   @param to   Store uuidv4 here. Must be of size MY_UUID_SIZE (16)
   @return 1 in case of success and 0 in case of failure
*/
static inline bool my_uuid_v4(uchar *to)
{
  if (my_random_bytes(to, 16) != MY_AES_OK)
      return 0;

  to[6]= ((to[6] & UUID_VERSION_MASK) | UUID_VERSION);
  to[8]= ((to[8] & UUID_VARIANT_MASK) | UUID_VARIANT);
  return 1;
}

#endif // SQL_TYPE_UUID_V4_INCLUDED
