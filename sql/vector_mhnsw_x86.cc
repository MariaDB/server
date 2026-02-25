/* Copyright (c) 2024, MariaDB plc

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
#include <cstddef>
#include <cstdint>

#ifdef _MSC_VER
# include <intrin.h>
#else
# include <cpuid.h>
#endif

constexpr uint32_t cpuid_ecx_AVX_AND_XSAVE= 1U << 28 | 1U << 27;
constexpr uint32_t cpuid_ebx_AVX2= 1U << 5;
constexpr uint32_t cpuid_ebx_AVX512= 1U << 16 | 1U << 30; // AVX512F and AVX512BW


static uint32_t cpuid_ecx()
{
#ifdef __GNUC__
  uint32_t eax, ebx, ecx, edx;
  __cpuid(1, eax, ebx, ecx, edx);
  return ecx;
#elif defined _MSC_VER
  int regs[4];
  __cpuid(regs, 1);
  return regs[2];
#endif
}

static uint32_t cpuid_ebx_7()
{
#ifdef __GNUC__
  uint32_t eax, ebx, ecx, edx;
  __cpuid_count(7, 0, eax, ebx, ecx, edx);
  return ebx;
#elif defined _MSC_VER
  int regs[4];
  __cpuidex(regs, 7, 0);
  return regs[1];
#endif
}

struct FVector;
typedef struct Vector_ops
{
  float (*dot_product)(const int16_t *v1, const int16_t *v2, size_t len);
  size_t (*alloc_size)(size_t n);
  FVector * (*align_ptr)(void *ptr);
  void (*fix_tail)(int16_t *dims, size_t vec_len);
} Vector_ops;

extern "C" float dot_product_avx2(const int16_t*, const int16_t*, size_t);
extern "C" float dot_product_avx512(const int16_t*, const int16_t*, size_t);
extern "C" size_t alloc_size_avx2(size_t);
extern "C" FVector *align_ptr_avx2(void*);
extern "C" void fix_tail_avx2(int16_t*, size_t);
extern "C" size_t alloc_size_avx512(size_t);
extern "C" FVector *align_ptr_avx512(void*);
extern "C" void fix_tail_avx512(int16_t*, size_t);

extern "C" Vector_ops vector_ops_x86_available(void)
{
  const uint32_t ecx = cpuid_ecx();
  if (~ecx & cpuid_ecx_AVX_AND_XSAVE)
    return {nullptr, nullptr, nullptr, nullptr};

  const uint32_t ebx = cpuid_ebx_7();
  if ((ebx & cpuid_ebx_AVX512) == cpuid_ebx_AVX512)
    return {dot_product_avx512, alloc_size_avx512, align_ptr_avx512, fix_tail_avx512};

  if (ebx & cpuid_ebx_AVX2)
    return {dot_product_avx2, alloc_size_avx2, align_ptr_avx2, fix_tail_avx2};

  return {nullptr, nullptr, nullptr, nullptr};
}
