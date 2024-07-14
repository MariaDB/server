/*
MIT License

Copyright (c) 2023 Sasha Krassovsky

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// https://save-buffer.github.io/bloom_filter.html

#pragma once
#include <cmath>
#include <vector>
#include <algorithm>
#ifdef HAVE_IMMINTRIN_H
#include <immintrin.h>
#if __GNUC__ > 7
#define DEFAULT_IMPLEMENTATION    __attribute__ ((target ("default")))
#define INTEL_SIMD_IMPLEMENTATION __attribute__ ((target ("avx2,avx,fma")))
#endif
#endif
#ifndef DEFAULT_IMPLEMENTATION
#define DEFAULT_IMPLEMENTATION
#endif

template <typename T>
struct PatternedSimdBloomFilter
{
  PatternedSimdBloomFilter(int n, float eps) : n(n), epsilon(eps)
  {
    m = ComputeNumBits();
    int log_num_blocks = my_bit_log2_uint32(m) + 1 - rotate_bits;
    num_blocks = (1ULL << log_num_blocks);
    bv.resize(num_blocks);
  }

  uint32_t ComputeNumBits()
  {
    double bits_per_val = -1.44 * std::log2(epsilon);
    return std::max<uint32_t>(512, static_cast<uint32_t>(bits_per_val * n + 0.5));
  }

#ifdef INTEL_SIMD_IMPLEMENTATION
  INTEL_SIMD_IMPLEMENTATION
  __m256i CalcHash(__m256i vecData)
  {
    // (almost) xxHash parallel version, 64bit input, 64bit output, seed=0
    static constexpr __m256i rotl48={
      0x0504030201000706ULL, 0x0D0C0B0A09080F0EULL,
      0x1514131211101716ULL, 0x1D1C1B1A19181F1EULL
    };
    static constexpr __m256i rotl24={
      0x0201000706050403ULL, 0x0A09080F0E0D0C0BULL,
      0x1211101716151413ULL, 0x1A19181F1E1D1C1BULL,
    };
    static constexpr uint64_t prime_mx2= 0x9FB21C651E98DF25ULL;
    static constexpr uint64_t bitflip= 0xC73AB174C5ECD5A2ULL;
    __m256i step1= _mm256_xor_si256(vecData, _mm256_set1_epi64x(bitflip));
    __m256i step2= _mm256_shuffle_epi8(step1, rotl48);
    __m256i step3= _mm256_shuffle_epi8(step1, rotl24);
    __m256i step4= _mm256_xor_si256(step1, _mm256_xor_si256(step2, step3));
    __m256i step5= _mm256_mul_epi32(step4, _mm256_set1_epi64x(prime_mx2));
    __m256i step6= _mm256_srli_epi64(step5, 35);
    __m256i step7= _mm256_add_epi64(step6, _mm256_set1_epi64x(8));
    __m256i step8= _mm256_xor_si256(step5, step7);
    __m256i step9= _mm256_mul_epi32(step8, _mm256_set1_epi64x(prime_mx2));
    return _mm256_xor_si256(step9, _mm256_srli_epi64(step9, 28));
  }

  INTEL_SIMD_IMPLEMENTATION
  __m256i GetBlockIdx(__m256i vecHash)
  {
    __m256i vecNumBlocksMask = _mm256_set1_epi64x(num_blocks - 1);
    __m256i vecBlockIdx = _mm256_srli_epi64(vecHash, mask_idx_bits + rotate_bits);
    return _mm256_and_si256(vecBlockIdx, vecNumBlocksMask);
  }

  INTEL_SIMD_IMPLEMENTATION
  __m256i ConstructMask(__m256i vecHash)
  {
    __m256i vecMaskIdxMask = _mm256_set1_epi64x((1 << mask_idx_bits) - 1);
    __m256i vecMaskMask = _mm256_set1_epi64x((1ull << bits_per_mask) - 1);
    __m256i vec64 = _mm256_set1_epi64x(64);

    __m256i vecMaskIdx = _mm256_and_si256(vecHash, vecMaskIdxMask);
    __m256i vecMaskByteIdx = _mm256_srli_epi64(vecMaskIdx, 3);
    __m256i vecMaskBitIdx = _mm256_and_si256(vecMaskIdx, _mm256_set1_epi64x(0x7));
    __m256i vecRawMasks = _mm256_i64gather_epi64((const longlong *)masks, vecMaskByteIdx, 1);
    __m256i vecUnrotated = _mm256_and_si256(_mm256_srlv_epi64(vecRawMasks, vecMaskBitIdx), vecMaskMask);

    __m256i vecRotation = _mm256_and_si256(_mm256_srli_epi64(vecHash, mask_idx_bits), _mm256_set1_epi64x((1 << rotate_bits) - 1));
    __m256i vecShiftUp = _mm256_sllv_epi64(vecUnrotated, vecRotation);
    __m256i vecShiftDown = _mm256_srlv_epi64(vecUnrotated, _mm256_sub_epi64(vec64, vecRotation));
    return _mm256_or_si256(vecShiftDown, vecShiftUp);
  }

  INTEL_SIMD_IMPLEMENTATION
  void Insert(const T **data)
  {
    __m256i vecDataA = _mm256_loadu_si256(reinterpret_cast<__m256i *>(data + 0));
    __m256i vecDataB = _mm256_loadu_si256(reinterpret_cast<__m256i *>(data + 4));

    __m256i vecHashA= CalcHash(vecDataA);
    __m256i vecHashB= CalcHash(vecDataB);

    __m256i vecMaskA = ConstructMask(vecHashA);
    __m256i vecMaskB = ConstructMask(vecHashB);

    __m256i vecBlockIdxA = GetBlockIdx(vecHashA);
    __m256i vecBlockIdxB = GetBlockIdx(vecHashB);

    uint64_t block0 = _mm256_extract_epi64(vecBlockIdxA, 0);
    uint64_t block1 = _mm256_extract_epi64(vecBlockIdxA, 1);
    uint64_t block2 = _mm256_extract_epi64(vecBlockIdxA, 2);
    uint64_t block3 = _mm256_extract_epi64(vecBlockIdxA, 3);
    uint64_t block4 = _mm256_extract_epi64(vecBlockIdxB, 0);
    uint64_t block5 = _mm256_extract_epi64(vecBlockIdxB, 1);
    uint64_t block6 = _mm256_extract_epi64(vecBlockIdxB, 2);
    uint64_t block7 = _mm256_extract_epi64(vecBlockIdxB, 3);

    bv[block0] |= _mm256_extract_epi64(vecMaskA, 0);
    bv[block1] |= _mm256_extract_epi64(vecMaskA, 1);
    bv[block2] |= _mm256_extract_epi64(vecMaskA, 2);
    bv[block3] |= _mm256_extract_epi64(vecMaskA, 3);
    bv[block4] |= _mm256_extract_epi64(vecMaskB, 0);
    bv[block5] |= _mm256_extract_epi64(vecMaskB, 1);
    bv[block6] |= _mm256_extract_epi64(vecMaskB, 2);
    bv[block7] |= _mm256_extract_epi64(vecMaskB, 3);
  }

  INTEL_SIMD_IMPLEMENTATION
  uint8_t Query(T **data)
  {
    __m256i vecDataA = _mm256_loadu_si256(reinterpret_cast<__m256i *>(data + 0));
    __m256i vecDataB = _mm256_loadu_si256(reinterpret_cast<__m256i *>(data + 4));

    __m256i vecHashA= CalcHash(vecDataA);
    __m256i vecHashB= CalcHash(vecDataB);

    __m256i vecMaskA = ConstructMask(vecHashA);
    __m256i vecMaskB = ConstructMask(vecHashB);

    __m256i vecBlockIdxA = GetBlockIdx(vecHashA);
    __m256i vecBlockIdxB = GetBlockIdx(vecHashB);

    __m256i vecBloomA = _mm256_i64gather_epi64(bv.data(), vecBlockIdxA, sizeof(longlong));
    __m256i vecBloomB = _mm256_i64gather_epi64(bv.data(), vecBlockIdxB, sizeof(longlong));
    __m256i vecCmpA = _mm256_cmpeq_epi64(_mm256_and_si256(vecMaskA, vecBloomA), vecMaskA);
    __m256i vecCmpB = _mm256_cmpeq_epi64(_mm256_and_si256(vecMaskB, vecBloomB), vecMaskB);
    uint32_t res_a = static_cast<uint32_t>(_mm256_movemask_epi8(vecCmpA));
    uint32_t res_b = static_cast<uint32_t>(_mm256_movemask_epi8(vecCmpB));
    uint64_t res_bytes = res_a | (static_cast<uint64_t>(res_b) << 32);
    uint8_t res_bits = static_cast<uint8_t>(_mm256_movemask_epi8(_mm256_set1_epi64x(res_bytes)) & 0xff);
    return res_bits;
  }
#endif

  /********************************************************
  ********* non-SIMD fallback version ********************/

  uint64_t CalcHash_1(const T* data)
  {
    static constexpr uint64_t prime_mx2= 0x9FB21C651E98DF25ULL;
    static constexpr uint64_t bitflip= 0xC73AB174C5ECD5A2ULL;
    uint64_t step1= ((intptr)data) ^ bitflip;
    uint64_t step2= (step1 >> 48) ^ (step1 << 16);
    uint64_t step3= (step1 >> 24) ^ (step1 << 40);
    uint64_t step4= step1 ^ step2 ^ step3;
    uint64_t step5= step4 * prime_mx2;
    uint64_t step6= step5 >> 35;
    uint64_t step7= step6 + 8;
    uint64_t step8= step5 ^ step7;
    uint64_t step9= step8 * prime_mx2;
    return step9 ^ (step9 >> 28);
  }

  uint64_t GetBlockIdx_1(uint64_t hash)
  {
    uint64_t blockIdx = hash >> (mask_idx_bits + rotate_bits);
    return blockIdx & (num_blocks - 1);
  }

  uint64_t ConstructMask_1(uint64_t hash)
  {
    uint64_t maskIdxMask = (1 << mask_idx_bits) - 1;
    uint64_t maskMask = (1ULL << bits_per_mask) - 1;
    uint64_t maskIdx = hash & maskIdxMask;
    uint64_t maskByteIdx = maskIdx >> 3;
    uint64_t maskBitIdx = maskIdx & 7;
    uint64_t rawMask = *(uint64_t *)(masks + maskByteIdx);
    uint64_t unrotated = (rawMask >> maskBitIdx) & maskMask;
    uint64_t rotation = (hash >> mask_idx_bits) & ((1 << rotate_bits) - 1);
    return (unrotated << rotation) | (unrotated >> (64 - rotation));
  }

  DEFAULT_IMPLEMENTATION
  void Insert(const T **data)
  {
    for (size_t i = 0; i < 8; i++)
    {
      uint64_t hash = CalcHash_1(data[i]);
      uint64_t mask = ConstructMask_1(hash);
      bv[GetBlockIdx_1(hash)] |= mask;
    }
  }

  DEFAULT_IMPLEMENTATION
  uint8_t Query(T **data)
  {
    uint8_t res_bits = 0;
    for (size_t i = 0; i < 8; i++)
    {
      uint64_t hash = CalcHash_1(data[i]);
      uint64_t mask = ConstructMask_1(hash);
      if ((bv[GetBlockIdx_1(hash)] & mask) == mask)
        res_bits |= 1 << i;
    }
    return res_bits;
  }

  int n;
  float epsilon;

  uint64_t num_blocks;
  uint32_t m;
  // calculated from the upstream MaskTable and hard-coded
  static constexpr int log_num_masks = 10;
  static constexpr int bits_per_mask = 57;
  const uint8_t masks[136]= {0x00, 0x04, 0x01, 0x04, 0x00, 0x20, 0x01, 0x00,
    0x00, 0x02, 0x08, 0x00, 0x02, 0x42, 0x00, 0x00, 0x04, 0x00, 0x00, 0x84,
    0x80, 0x00, 0x04, 0x00, 0x02, 0x00, 0x00, 0x21, 0x00, 0x08, 0x00, 0x14,
    0x00, 0x00, 0x40, 0x00, 0x10, 0x00, 0xa8, 0x00, 0x00, 0x00, 0x00, 0x10,
    0x04, 0x40, 0x01, 0x00, 0x40, 0x00, 0x00, 0x08, 0x01, 0x02, 0x80, 0x00,
    0x00, 0x01, 0x00, 0x06, 0x00, 0x00, 0x09, 0x00, 0x00, 0x00, 0x0c, 0x10,
    0x00, 0x10, 0x00, 0x00, 0x10, 0x08, 0x01, 0x10, 0x00, 0x00, 0x10, 0x20,
    0x00, 0x01, 0x20, 0x00, 0x02, 0x40, 0x00, 0x00, 0x02, 0x40, 0x01, 0x00,
    0x40, 0x00, 0x00, 0x0a, 0x00, 0x02, 0x01, 0x80, 0x00, 0x00, 0x10, 0x08,
    0x00, 0x06, 0x00, 0x04, 0x00, 0x00, 0x50, 0x00, 0x08, 0x10, 0x20, 0x00,
    0x00, 0x80, 0x00, 0x10, 0x10, 0x04, 0x04, 0x00, 0x00, 0x00, 0x20, 0x20,
    0x08, 0x08, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00};
  std::vector<longlong> bv;

  static constexpr int mask_idx_bits = log_num_masks;
  static constexpr int rotate_bits = 6;
};
