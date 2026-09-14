/* Copyright (c) 2020, 2022, MariaDB

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

typedef unsigned int (*my_crc32_t)(unsigned int, const void *, size_t);

#if defined _M_IX86 || defined _M_X64 || defined __i386__ || defined __x86_64__
extern "C" my_crc32_t crc32_pclmul_enabled();
#elif defined HAVE_ARMV8_CRC
extern "C" int crc32_aarch64_available();
extern "C" unsigned int crc32_aarch64(unsigned int, const void *, size_t);
#elif defined HAVE_RISCV_ZBC
extern "C" int rv_zbc_supported(void *);
extern "C" unsigned int crc32_riscv_zbc(unsigned int, const void *, size_t);
#endif

#ifdef __powerpc64__
# error "my_checksum() is defined in mysys/crc32/crc32_ppc64.c"
#endif

#if defined HAVE_RISCV_ZBC
/* my_checksum() is dispatched by an indirect function on RISC-V, mirroring
   my_crc32c() in crc32c.cc. That file documents the two constraints on the
   resolver: it runs at load time, so it may only call code that is safe
   there, and it must not return NULL. */
extern "C" { static my_crc32_t rv_my_checksum_resolver(unsigned long long,
                                                      void *hwprobe,
                                                      void *)
{
  return rv_zbc_supported(hwprobe) ? crc32_riscv_zbc : my_crc32_zlib;
} }

extern "C" uint32 my_checksum(uint32, const void *, size_t)
  __attribute__((ifunc("rv_my_checksum_resolver")));
#else
static my_crc32_t init_crc32()
{
#if defined _M_IX86 || defined _M_X64 || defined __i386__ || defined __x86_64__
  if (my_crc32_t crc= crc32_pclmul_enabled())
    return crc;
#elif defined HAVE_ARMV8_CRC
  if (crc32_aarch64_available())
    return crc32_aarch64;
#endif
  return my_crc32_zlib;
}

static const my_crc32_t my_checksum_func= init_crc32();

extern "C"
uint32 my_checksum(uint32 crc, const void *data, size_t len)
{
  return my_checksum_func(crc, data, len);
}
#endif
