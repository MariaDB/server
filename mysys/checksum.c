/* Copyright (c) 2000, 2010, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


#include <my_global.h>
#include <my_sys.h>
#include <zlib.h>

#if !defined(HAVE_CRC32_VPMSUM)
/* TODO: remove this once zlib adds inherent support for hardware accelerated
crc32 for all architectures. */
static unsigned int my_crc32_zlib(unsigned int crc, const void *data,
                                  size_t len)
{
  return (unsigned int) crc32(crc, data, (unsigned int) len);
}

my_crc32_t my_checksum= my_crc32_zlib;
#endif

#if __GNUC__ >= 4 && defined(__x86_64__) && defined(HAVE_CLMUL_INSTRUCTION)
/*  this define being set means crc32_x86.c with pclmul instructions
    has been included, not necessarily that the pclmul functions are
    available at runtime */

extern int crc32_pclmul_enabled();
extern unsigned int crc32_pclmul(unsigned int, const void *, size_t);

/*----------------------------- x86_64 ---------------------------------*/
void my_checksum_init(void)
{
  if (crc32_pclmul_enabled())
    my_checksum= crc32_pclmul;
}
#elif defined(__GNUC__) && defined(HAVE_ARMV8_CRC)
/*----------------------------- aarch64 --------------------------------*/

extern unsigned int crc32_aarch64(unsigned int, const void *, size_t);

/* Ideally all ARM 64 bit processor should support crc32 but if some model
doesn't support better to find it out through auxillary vector. */
void my_checksum_init(void)
{
  if (crc32_aarch64_available())
    my_checksum= crc32_aarch64;
}
#else
void my_checksum_init(void) {}
#endif
