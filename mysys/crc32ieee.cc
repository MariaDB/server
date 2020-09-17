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
static unsigned int my_crc32_zlib(unsigned int crc, const void *data,
                                  size_t len)
{
  return (unsigned int) crc32(crc, (const Bytef *)data, (unsigned int) len);
}

#ifdef HAVE_PCLMUL
extern "C" int crc32_pclmul_enabled();
extern "C" unsigned int crc32_pclmul(unsigned int, const void *, size_t);
#elif defined(__GNUC__) && defined(HAVE_ARMV8_CRC)
extern "C" int crc32_aarch64_available();
extern "C" unsigned int crc32_aarch64(unsigned int, const void *, size_t);
#endif


typedef unsigned int (*my_crc32_t)(unsigned int, const void *, size_t);

static my_crc32_t init_crc32()
{
  my_crc32_t func= my_crc32_zlib;
#ifdef HAVE_PCLMUL
  if (crc32_pclmul_enabled())
    func = crc32_pclmul;
#elif defined(__GNUC__) && defined(HAVE_ARMV8_CRC)
  if (crc32_aarch64_available())
    func= crc32_aarch64;
#endif
  return func;
}

static const my_crc32_t my_checksum_func= init_crc32();

#ifndef __powerpc64__
/* For powerpc, my_checksum is defined elsewhere.*/
extern "C" unsigned int my_checksum(unsigned int crc, const void *data, size_t len)
{
  return my_checksum_func(crc, data, len);
}
#endif


