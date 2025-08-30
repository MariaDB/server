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
# if 0 /* So far, we have no environment where this could be tested. */
#  define USE_VPCLMULQDQ /* nothing */
# endif
#else
# include <cpuid.h>
# ifdef __APPLE__ /* AVX512 states are not enabled in XCR0 */
# elif __GNUC__ >= 15
#  define TARGET "pclmul,avx10.1,vpclmulqdq"
#  define USE_VPCLMULQDQ __attribute__((target(TARGET)))
# elif __GNUC__ >= 14 || (defined __clang_major__ && __clang_major__ >= 18)
#  define TARGET "pclmul,evex512,avx512f,avx512dq,avx512bw,avx512vl,vpclmulqdq"
#  define USE_VPCLMULQDQ __attribute__((target(TARGET)))
# elif __GNUC__ >= 11 || (defined __clang_major__ && __clang_major__ >= 9)
/* clang 8 does not support _xgetbv(), which we also need */
#  define TARGET "pclmul,avx512f,avx512dq,avx512bw,avx512vl,vpclmulqdq"
#  define USE_VPCLMULQDQ __attribute__((target(TARGET)))
# endif
#endif

extern "C" unsigned crc32c_sse42(unsigned crc, const void* buf, size_t size);

constexpr uint32_t cpuid_ecx_SSE42= 1U << 20;
constexpr uint32_t cpuid_ecx_SSE42_AND_PCLMUL= cpuid_ecx_SSE42 | 1U << 1;
constexpr uint32_t cpuid_ecx_AVX_AND_XSAVE= 1U << 28 | 1U << 27;

static uint32_t cpuid_ecx()
{
#ifdef __GNUC__
  uint32_t reax= 0, rebx= 0, recx= 0, redx= 0;
  __cpuid(1, reax, rebx, recx, redx);
  return recx;
#elif defined _MSC_VER
  int regs[4];
  __cpuid(regs, 1);
  return regs[2];
#else
# error "unknown compiler"
#endif
}

typedef uint32_t (*my_crc32_t)(uint32_t, const void *, size_t);
extern "C" uint32_t crc32_pclmul(uint32_t, const void *, size_t);
extern "C" uint32_t crc32c_3way(uint32_t, const void *, size_t);

#ifdef USE_VPCLMULQDQ
# include <immintrin.h>

# ifdef _MSC_VER
/* MSVC does not seem to define this intrinsic for vmovdqa */
#  define _mm_load_epi32(x) *reinterpret_cast<const __m128i*>(x)
# endif

/*
  This implementation is based on
  crc32_by16_vclmul_avx512 and crc32_refl_by16_vclmul_avx512
  in https://github.com/intel/intel-ipsec-mb/ with some optimizations.
  The // comments in crc32_avx512() correspond to assembler labels.
*/

/** table of constants corresponding to a CRC polynomial up to degree 32 */
struct alignas(64) crc32_tab
{
  const uint64_t b2048[2], b1024[2];
  alignas(64) const uint64_t b896[6]; /* includes b786, b640 */
  const uint64_t b512[2];
  const uint64_t b384[2], b256[2], b128[2], zeropad_for_b384[2];
  const uint64_t b64[2], b32[2];
};

/** ISO 3309 CRC-32 (reflected polynomial 0x04C11DB7); zlib crc32() */
static const crc32_tab refl32 = {
  { 0x00000000e95c1271, 0x00000000ce3371cb },
  { 0x00000000910eeec1, 0x0000000033fff533 },
  { 0x000000000cbec0ed, 0x0000000031f8303f,
    0x0000000057c54819, 0x00000000df068dc2,
    0x00000000ae0b5394, 0x000000001c279815 },
  { 0x000000001d9513d7, 0x000000008f352d95 },
  { 0x00000000af449247, 0x000000003db1ecdc },
  { 0x0000000081256527, 0x00000000f1da05aa },
  { 0x00000000ccaa009e, 0x00000000ae689191 },
  { 0, 0 },
  { 0x00000000ccaa009e, 0x00000000b8bc6765 },
  { 0x00000001f7011640, 0x00000001db710640 }
};

/** Castagnoli CRC-32C (reflected polynomial 0x1EDC6F41) */
static const crc32_tab refl32c = {
  { 0x00000000b9e02b86, 0x00000000dcb17aa4 },
  { 0x000000000d3b6092, 0x000000006992cea2 },
  { 0x0000000047db8317, 0x000000002ad91c30,
    0x000000000715ce53, 0x00000000c49f4f67,
    0x0000000039d3b296, 0x00000000083a6eec },
  { 0x000000009e4addf8, 0x00000000740eef02 },
  { 0x00000000ddc0152b, 0x000000001c291d04 },
  { 0x00000000ba4fc28e, 0x000000003da6d0cb },
  { 0x00000000493c7d27, 0x00000000f20c0dfe },
  { 0, 0 },
  { 0x00000000493c7d27, 0x00000000dd45aab8 },
  { 0x00000000dea713f0, 0x0000000105ec76f0 }
};

/** Some ternary functions */
class ternary
{
  static constexpr uint8_t A = 0b11110000;
  static constexpr uint8_t B = 0b11001100;
  static constexpr uint8_t C = 0b10101010;
public:
  static constexpr uint8_t XOR3 = A ^ B ^ C;
  static constexpr uint8_t XNOR3 = uint8_t(~(A ^ B ^ C));
  static constexpr uint8_t XOR2_AND = (A ^ B) & C;
};

USE_VPCLMULQDQ
/** @return a^b^c */
static inline __m128i xor3_128(__m128i a, __m128i b, __m128i c)
{
  return _mm_ternarylogic_epi64(a, b, c, ternary::XOR3);
}

USE_VPCLMULQDQ
/** @return ~(a^b^c) */
static inline __m128i xnor3_128(__m128i a, __m128i b, __m128i c)
{
  return _mm_ternarylogic_epi64(a, b, c, ternary::XNOR3);
}

USE_VPCLMULQDQ
/** @return a^b^c */
static inline __m512i xor3_512(__m512i a, __m512i b, __m512i c)
{
  return _mm512_ternarylogic_epi64(a, b, c, ternary::XOR3);
}

USE_VPCLMULQDQ
/** @return (a^b)&c */
static inline __m128i xor2_and_128(__m128i a, __m128i b, __m128i c)
{
  return _mm_ternarylogic_epi64(a, b, c, ternary::XOR2_AND);
}

USE_VPCLMULQDQ
/** Load 64 bytes */
static inline __m512i load512(const char *b) { return _mm512_loadu_epi8(b); }

USE_VPCLMULQDQ
/** Load 16 bytes */
static inline __m128i load128(const char *b) { return _mm_loadu_epi64(b); }

/** Combine 512 data bits with CRC */
USE_VPCLMULQDQ
static inline __m512i combine512(__m512i a, __m512i tab, __m512i b)
{
  return xor3_512(b, _mm512_clmulepi64_epi128(a, tab, 0x01),
                  _mm512_clmulepi64_epi128(a, tab, 0x10));
}

# define xor512(a, b) _mm512_xor_epi64(a, b)
# define xor256(a, b) _mm256_xor_epi64(a, b)
# define xor128(a, b) _mm_xor_epi64(a, b)
# define and128(a, b) _mm_and_si128(a, b)

template<uint8_t bits> USE_VPCLMULQDQ
/** Pick and zero-extend 128 bits of a 512-bit vector (vextracti32x4) */
static inline __m512i extract512_128(__m512i a)
{
  static_assert(bits <= 3, "usage");
  return _mm512_zextsi128_si512(_mm512_extracti64x2_epi64(a, bits));
}

alignas(16) static const uint64_t shuffle128[4] = {
  0x8786858483828100, 0x8f8e8d8c8b8a8988,
  0x0706050403020100, 0x000e0d0c0b0a0908
};

static const __mmask16 size_mask[16] = {
  0x0001, 0x0003, 0x0007, 0x000f, 0x001f, 0x003f, 0x007f, 0x00ff,
  0x01ff, 0x03ff, 0x07ff, 0x0fff, 0x1fff, 0x3fff, 0x7fff, 0xffff
};

alignas(16) static const uint64_t shift128[4] = {
  0x8786858483828100, 0x8f8e8d8c8b8a8988,
  0x0706050403020100, 0x000e0d0c0b0a0908
};

static const char shift_1_to_3_reflect[7 + 11] = {
  -1, -1, -1, -1, -1, -1, -1,
  0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10
};

USE_VPCLMULQDQ
static unsigned crc32_avx512(unsigned crc, const char *buf, size_t size,
                             const crc32_tab &tab)
{
  const __m512i crc_in = _mm512_zextsi128_si512(_mm_cvtsi32_si128(~crc)),
    b512 = _mm512_broadcast_i32x4(_mm_load_epi32(tab.b512));
  __m128i crc_out;
  __m512i lo;

  if (size >= 256) {
    lo = xor512(load512(buf), crc_in);
    __m512i l1 = load512(buf + 64);

    const __m512i b1024 = _mm512_broadcast_i32x4(_mm_load_epi32(&tab.b1024));
    size -= 256;
    if (size >= 256) {
      __m512i h0 = load512(buf + 128),
        hi = load512(buf + 192);
      const __m512i b2048 = _mm512_broadcast_i32x4(_mm_load_epi32(&tab.b2048));
      size -= 256;
      do {
        buf += 256;
        lo = combine512(lo, b2048, load512(buf));
        l1 = combine512(l1, b2048, load512(buf + 64));
        h0 = combine512(h0, b2048, load512(buf + 128));
        hi = combine512(hi, b2048, load512(buf + 192));
        size -= 256;
      } while (ssize_t(size) >= 0);

      buf += 256;
      lo = combine512(lo, b1024, h0);
      l1 = combine512(l1, b1024, hi);
      size += 128;
    } else {
      do {
        buf += 128;
        lo = combine512(lo, b1024, load512(buf));
        l1 = combine512(l1, b1024, load512(buf + 64));
        size -= 128;
      } while (ssize_t(size) >= 0);

      buf += 128;
    }

    if (ssize_t(size) >= -64) {
      size += 128;
      lo = combine512(lo, b512, l1);
      goto fold_64_B_loop;
    }

    const __m512i
      b896 = _mm512_load_epi32(&tab.b896),
      b384 = _mm512_load_epi32(&tab.b384);

    __m512i c4 = xor3_512(_mm512_clmulepi64_epi128(lo, b896, 1),
                          _mm512_clmulepi64_epi128(lo, b896, 0x10),
                          _mm512_clmulepi64_epi128(l1, b384, 1));
    c4 = xor3_512(c4, _mm512_clmulepi64_epi128(l1, b384, 0x10),
                  extract512_128<3>(l1));

    __m256i c2 =
      _mm512_castsi512_si256(_mm512_shuffle_i64x2(c4, c4, 0b01001110));
    c2 = xor256(c2, _mm512_castsi512_si256(c4));
    crc_out = xor128(_mm256_extracti64x2_epi64(c2, 1),
                     _mm256_castsi256_si128(c2));
    size += 128 - 16;
    goto final_reduction;
  }

  __m128i b;

  // less_than_256
  if (size >= 32) {
    if (size >= 64) {
      lo = xor512(load512(buf), crc_in);

      while (buf += 64, (size -= 64) >= 64)
      fold_64_B_loop:
        lo = combine512(lo, b512, load512(buf));

      // reduce_64B
      const __m512i b384 = _mm512_load_epi32(&tab.b384);
      __m512i crc512 =
        xor3_512(_mm512_clmulepi64_epi128(lo, b384, 1),
                 _mm512_clmulepi64_epi128(lo, b384, 0x10),
                 extract512_128<3>(lo));
      crc512 =
	xor512(crc512, _mm512_shuffle_i64x2(crc512, crc512, 0b01001110));
      const __m256i crc256 = _mm512_castsi512_si256(crc512);
      crc_out = xor128(_mm256_extracti64x2_epi64(crc256, 1),
                      _mm256_castsi256_si128(crc256));
      size -= 16;
    } else {
      // less_than_64
      crc_out = xor128(load128(buf),
                       _mm512_castsi512_si128(crc_in));
      buf += 16;
      size -= 32;
    }

  final_reduction:
    b = _mm_load_epi32(&tab.b128);

    while (ssize_t(size) >= 0) {
      // reduction_loop_16B
      crc_out = xor3_128(load128(buf),
                         _mm_clmulepi64_si128(crc_out, b, 1),
                         _mm_clmulepi64_si128(crc_out, b, 0x10));
      buf += 16;
      size -= 16;
    }
    // final_reduction_for_128

    size += 16;
    if (size) {
    get_last_two_xmms:
      const __m128i crc2 = crc_out, d = load128(buf + ssize_t(size) - 16);
      __m128i S = load128(reinterpret_cast<const char*>(shuffle128) + size);
      crc_out = _mm_shuffle_epi8(crc_out, S);
      S = xor128(S, _mm_set1_epi32(0x80808080));
      crc_out = xor3_128(_mm_blendv_epi8(_mm_shuffle_epi8(crc2, S), d, S),
                         _mm_clmulepi64_si128(crc_out, b, 1),
                         _mm_clmulepi64_si128(crc_out, b, 0x10));
    }

  done_128:
    __m128i crc_tmp;
    b = _mm_load_epi32(&tab.b64);
    crc_tmp = xor128(_mm_clmulepi64_si128(crc_out, b, 0x00),
                     _mm_srli_si128(crc_out, 8));
    crc_out = _mm_slli_si128(crc_tmp, 4);
    crc_out = _mm_clmulepi64_si128(crc_out, b, 0x10);
    crc_out = xor128(crc_out, crc_tmp);

  barrett:
    b = _mm_load_epi32(&tab.b32);
    crc_tmp = crc_out;
    crc_out = and128(crc_out, _mm_set_epi64x(~0ULL, ~0xFFFFFFFFULL));
    crc_out = _mm_clmulepi64_si128(crc_out, b, 0);
    crc_out = xor2_and_128(crc_out, crc_tmp, _mm_set_epi64x(0, ~0ULL));
    crc_out = xnor3_128(crc_out, crc_tmp,
                        _mm_clmulepi64_si128(crc_out, b, 0x10));
    return _mm_extract_epi32(crc_out, 2);
  } else {
    // less_than_32
    if (size > 0) {
      if (size > 16) {
        crc_out = xor128(load128(buf),
                         _mm512_castsi512_si128(crc_in));
        buf += 16;
        size -= 16;
        b = _mm_load_epi32(&tab.b128);
        goto get_last_two_xmms;
      } else if (size < 16) {
        crc_out = _mm_maskz_loadu_epi8(size_mask[size - 1], buf);
        crc_out = xor128(crc_out, _mm512_castsi512_si128(crc_in));

        if (size >= 4) {
          crc_out = _mm_shuffle_epi8
            (crc_out,
             load128(reinterpret_cast<const char*>(shift128) + size));
          goto done_128;
        } else {
          // only_less_than_4
          /* Shift, zero-filling 5 to 7 of the 8-byte crc_out */
          crc_out = _mm_shuffle_epi8(crc_out,
                                     load128(shift_1_to_3_reflect + size - 1));
          goto barrett;
        }
      } else {
        crc_out = xor128(load128(buf), _mm512_castsi512_si128(crc_in));
        goto done_128;
      }
    } else
      return crc;
  }
}

#ifdef __GNUC__
__attribute__((target("xsave")))
#endif
static bool os_have_avx512()
{
  // The following flags must be set: SSE, AVX, OPMASK, ZMM_HI256, HI16_ZMM
  return !(~_xgetbv(0 /*_XCR_XFEATURE_ENABLED_MASK*/) & 0xe6);
}

static ATTRIBUTE_NOINLINE bool have_vpclmulqdq(uint32_t cpuid_ecx)
{
  if ((~cpuid_ecx & cpuid_ecx_AVX_AND_XSAVE) || !os_have_avx512())
    return false;
# ifdef _MSC_VER
  int regs[4];
  __cpuidex(regs, 7, 0);
  uint32_t ebx = regs[1], ecx = regs[2];
# else
  uint32_t eax = 0, ebx = 0, ecx = 0, edx = 0;
  __cpuid_count(7, 0, eax, ebx, ecx, edx);
# endif
  return ecx & 1U<<10/*VPCLMULQDQ*/ &&
    !(~ebx & ((1U<<16/*AVX512F*/ | 1U<<17/*AVX512DQ*/ |
               1U<<30/*AVX512BW*/ | 1U<<31/*AVX512VL*/)));
}

static unsigned crc32_vpclmulqdq(unsigned crc, const void *buf, size_t size)
{
  return crc32_avx512(crc, static_cast<const char*>(buf), size, refl32);
}

static unsigned crc32c_vpclmulqdq(unsigned crc, const void *buf, size_t size)
{
  return crc32_avx512(crc, static_cast<const char*>(buf), size, refl32c);
}
#endif

extern "C" my_crc32_t crc32_pclmul_enabled(void)
{
  const uint32_t ecx= cpuid_ecx();
  if (~ecx & cpuid_ecx_SSE42_AND_PCLMUL)
    return nullptr;
#ifdef USE_VPCLMULQDQ
  if (have_vpclmulqdq(ecx))
    return crc32_vpclmulqdq;
#endif
  return crc32_pclmul;
}

extern "C" my_crc32_t crc32c_x86_available(void)
{
  const uint32_t ecx= cpuid_ecx();
#ifdef USE_VPCLMULQDQ
  if (have_vpclmulqdq(ecx))
    return crc32c_vpclmulqdq;
#endif
#if SIZEOF_SIZE_T == 8
  switch (ecx & cpuid_ecx_SSE42_AND_PCLMUL) {
  case cpuid_ecx_SSE42_AND_PCLMUL:
    return crc32c_3way;
  case cpuid_ecx_SSE42:
    return crc32c_sse42;
  }
#else
  if (ecx & cpuid_ecx_SSE42)
    return crc32c_sse42;
#endif
  return nullptr;
}

extern "C" const char *crc32c_x86_impl(my_crc32_t c)
{
#ifdef USE_VPCLMULQDQ
  if (c == crc32c_vpclmulqdq)
    return "Using AVX512 instructions";
#endif
#if SIZEOF_SIZE_T == 8
  if (c == crc32c_3way)
    return "Using crc32 + pclmulqdq instructions";
#endif
  if (c == crc32c_sse42)
    return "Using SSE4.2 crc32 instructions";
  return nullptr;
}
