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

/* TODO: remove this once zlib adds inherent support for hardware accelerated
crc32 for all architectures. */
typedef unsigned long (*my_crc32_func_t)(unsigned long crc,
                                         const unsigned char *ptr,
                                         unsigned int len);
my_crc32_func_t my_crc32= crc32;

#if __GNUC__ >= 4 && defined(__x86_64__) && defined(HAVE_CLMUL_INSTRUCTION)
/*----------------------------- x86_64 ---------------------------------*/
extern int is_pclmul_enabled();
extern unsigned long crc32_pclmul(unsigned long crc32,
                                  const unsigned char *buf, unsigned int len);
void my_crc32_init()
{
  my_crc32= is_pclmul_enabled() ? crc32_pclmul : crc32;
}
#elif defined(__GNUC__) && defined(__linux__) && defined(HAVE_ARMV8_CRC)
/*----------------------------- aarch64 --------------------------------*/
extern unsigned int crc32_aarch64_available(void);
extern unsigned long crc32_aarch64(unsigned long crc32,
                                   const unsigned char *buf, unsigned int len);

/* Ideally all ARM 64 bit processor should support crc32 but if some model
doesn't support better to find it out through auxillary vector. */
void my_crc32_init()
{
  my_crc32= crc32_aarch64_available() ? crc32_aarch64 : crc32;
}
#elif defined(HAVE_CRC32_VPMSUM)
/*----------------------------- ppc64{,le} ---------------------------------*/
extern unsigned int crc32ieee_vpmsum(unsigned int crc, const unsigned char *p,
                                     unsigned long len);
void my_crc32_init()
{
}
#else
void my_crc32_init()
{
  my_crc32= crc32;
}
#endif

/*
  Calculate a long checksum for a memoryblock.

  SYNOPSIS
    my_checksum()
      crc       start value for crc
      pos       pointer to memory block
      length    length of the block
*/

inline ha_checksum my_checksum(ha_checksum crc, const uchar *pos, size_t length)
{
#if defined(HAVE_CRC32_VPMSUM)
  crc= crc32ieee_vpmsum(crc, pos, length);
#else
  crc= my_crc32((uint)crc, pos, (uint) length);
#endif
  DBUG_PRINT("info", ("crc: %lu", (ulong) crc));
  return crc;
}
