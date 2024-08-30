/*****************************************************************************

Copyright (c) 2024, MariaDB plc

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

/* This is based on the implementation of pmem_persist() in
https://github.com/pmem/pmdk/, Copyright 2014-2020, Intel Corporation,
last revised in libpmem-1.12.0. */

#include "my_global.h"
#include "cache.h"
#include <cstdint>

#if defined __x86_64__ || defined __aarch64__ || defined __powerpc64__
# ifdef __x86_64__
static void pmem_clflush(const void *buf, size_t size)
{
  for (uintptr_t u= uintptr_t(buf) & ~(CPU_LEVEL1_DCACHE_LINESIZE),
         end= uintptr_t(buf) + size;
       u < end; u+= CPU_LEVEL1_DCACHE_LINESIZE)
    __asm__ __volatile__("clflush %0" ::
                         "m"(*reinterpret_cast<const char*>(u)) : "memory");
}

static void pmem_clflushopt(const void *buf, size_t size)
{
  for (uintptr_t u= uintptr_t(buf) & ~(CPU_LEVEL1_DCACHE_LINESIZE),
         end= uintptr_t(buf) + size;
       u < end; u+= CPU_LEVEL1_DCACHE_LINESIZE)
    __asm__ __volatile__(".byte 0x66; clflush %0" /* clflushopt */ ::
                         "m"(*reinterpret_cast<const char*>(u)) : "memory");
  __asm__ __volatile__("sfence" ::: "memory");
}

static void pmem_clwb(const void *buf, size_t size)
{
  for (uintptr_t u= uintptr_t(buf) & ~(CPU_LEVEL1_DCACHE_LINESIZE),
         end= uintptr_t(buf) + size;
       u < end; u+= CPU_LEVEL1_DCACHE_LINESIZE)
    __asm__ __volatile__(".byte 0x66; xsaveopt %0" /* clwb */ ::
                         "m"(*reinterpret_cast<const char*>(u)) : "memory");
  __asm__ __volatile__("sfence" ::: "memory");
}

#  include <cpuid.h>
static decltype(pmem_control::persist) pmem_persist_init()
{
  uint32_t eax= 0, ebx= 0, ecx= 0, edx= 0;
  __cpuid_count(7, 0, eax, ebx, ecx, edx);
  if (ebx & 1U<<24 /* CLWB */)
    return pmem_clwb;
  else if (ebx & 1U<<23 /* CLFLUSHOPT */)
    return pmem_clflushopt;
  else
    return pmem_clflush;
}
# elif defined __aarch64__
static void pmem_cvac(const void* buf, size_t size)
{
  for (uintptr_t u= uintptr_t(buf) & ~(CPU_LEVEL1_DCACHE_LINESIZE),
         end= uintptr_t(buf) + size;
       u < end; u+= CPU_LEVEL1_DCACHE_LINESIZE)
    __asm__ __volatile__("dc cvac, %0" :: "r"(u) : "memory");
  __asm__ __volatile__("dmb ishst" ::: "memory");
}

static void pmem_cvap(const void* buf, size_t size)
{
  for (uintptr_t u= uintptr_t(buf) & ~(CPU_LEVEL1_DCACHE_LINESIZE),
         end= uintptr_t(buf) + size;
       u < end; u+= CPU_LEVEL1_DCACHE_LINESIZE)
    __asm__ __volatile__(".arch armv8.2-a\n dc cvap, %0" :: "r"(u) : "memory");
  __asm__ __volatile__("dmb ishst" ::: "memory");
}

#  include <sys/auxv.h>
#  include <asm/hwcap.h>
#  ifndef HWCAP_DCPOP
#   define HWCAP_DCPOP (1 << 16)
#  endif

static decltype(pmem_control::persist) pmem_persist_init()
{
  return (getauxval(AT_HWCAP) & HWCAP_DCPOP) ? pmem_cvap : pmem_cvac;
}
# elif defined __powerpc64__
static void pmem_phwsync(const void* buf, size_t size)
{
  for (uintptr_t u= uintptr_t(buf) & ~(CPU_LEVEL1_DCACHE_LINESIZE),
         end= uintptr_t(buf) + size;
       u < end; u+= CPU_LEVEL1_DCACHE_LINESIZE)
  {
    /* GCC is just passing the inline asm snippets to the assembler,
    and it does not even define these mnemonics by itself. Clang does,
    and it includes a built-in assembler.

    Let us hope that having a recent enough GCC is an adequate proxy
    for having a recent enough assembler. */
#  if __GNUC__ >= 11 || (defined __clang_major__ && __clang_major__ >= 12)
    __asm__ __volatile__("dcbstps 0,%0" :: "r"(u) : "memory");
#  else
    __asm__ __volatile__(".long (0x7cc000AC | %0 << 11)" :: "r"(u) : "memory");
#  endif
  }

#  if __GNUC__ >= 11 || (defined __clang_major__ && __clang_major__ >= 18)
  __asm__ __volatile__("phwsync" ::: "memory");
#  else
  __asm__ __volatile__(".long 0x7c80040a" ::: "memory");
#  endif
}

#  include <atomic>
static void pmem_fence(const void*, size_t)
{
  std::atomic_thread_fence(std::memory_order_seq_cst);
}

#  include <sys/auxv.h>
#  ifndef AT_HWCAP2
#   define AT_HWCAP2 26
#  endif
#  ifndef PPC_FEATURE2_ARCH_3_1
#   define PPC_FEATURE2_ARCH_3_1 4
#  endif

static decltype(pmem_control::persist) pmem_persist_init()
{
  return (getauxval(AT_HWCAP2) & PPC_FEATURE2_ARCH_3_1)
    ? pmem_phwsync : pmem_fence;
}
# endif

pmem_control::pmem_control() : persist(pmem_persist_init()) {}
const pmem_control pmem;
#else
void pmem_persist(const void *buf, size_t size)
{
# if defined __riscv && __riscv_xlen == 64
  __asm__ __volatile__("fence w,w" ::: "memory");
# elif defined __loongarch64
  __asm__ __volatile__("dbar 0" ::: "memory");
# else
#  error "Missing implementation; recompile with cmake -DWITH_INNODB_PMEM=OFF"
# endif
}
#endif
