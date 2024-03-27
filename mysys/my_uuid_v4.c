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
  implements Universal Unique Identifiers version 4, as described in
  draft-ietf-uuidrev-rfc4122bis-14.

  Field                       Octet #          Note
 random_a                       0-5     Random CSPRNG 48 bits.
 random_b_and_version           6-7     Random CSPRNG 16 bits multiplexed
                                        with the version number.
 random_c_and_variant           8-15    Random CSPRNG 64 bits multiplexed
                                        with the variant number.
*/

#include "mysys_priv.h"
#include <my_rnd.h>
#include <m_string.h>
#include <myisampack.h> /* mi_int2store, mi_int4store */
#include <errmsg.h>

#define UUID_VERSION                         0x4000
#define UUID_VERSION_MASK                    0x0FFF
#define UUID_VARIANT                         0x8000000000000000
#define UUID_VARIANT_MASK                    0x3FFFFFFFFFFFFFFF

static int is_random_bits_generation_successful(unsigned char *rand_var, int size)
{
  return my_random_bytes(rand_var, size) != MY_AES_OK;
}

/**
   Create a global unique identifier version 4 (uuidv4)

   @func  my_uuid_v4()
   @param to   Store uuidv4 here. Must be of size MY_UUID_SIZE (16)
   @return 1 in case of success and 0 in case of failure
*/
int my_uuid_v4(uchar *to)
{
  uint64 random_a, random_c = 0, random_c_and_variant;
  uint16 random_b = 0, random_b_and_version;

  if (is_random_bits_generation_successful((unsigned char *) &random_a,
                                           sizeof(random_a)) ||
      is_random_bits_generation_successful((unsigned char *) &random_b,
                                           sizeof(random_b)) ||
      is_random_bits_generation_successful((unsigned char *) &random_c,
                                           sizeof(random_c)))
  {
    return 0;
  }

  random_b_and_version= (uint16)
    ((random_b & UUID_VERSION_MASK) | UUID_VERSION);
  random_c_and_variant= (uint64)
    ((random_c & UUID_VARIANT_MASK) | UUID_VARIANT);

  mi_int6store(to, random_a);
  mi_int2store(to+6, random_b_and_version);
  mi_int8store(to+8, random_c_and_variant);
  return 1;
}
