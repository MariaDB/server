/* Copyright (c) 2020, 2021, MariaDB

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

/*
  Implementation of CRC32 (Ethernet) uing Intel PCLMULQDQ
  Ported from Intels work, see https://github.com/intel/soft-crc
*/

/*******************************************************************************
 Copyright (c) 2009-2018, Intel Corporation

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

     * Redistributions of source code must retain the above copyright notice,
       this list of conditions and the following disclaimer.
     * Redistributions in binary form must reproduce the above copyright
       notice, this list of conditions and the following disclaimer in the
       documentation and/or other materials provided with the distribution.
     * Neither the name of Intel Corporation nor the names of its contributors
       may be used to endorse or promote products derived from this software
       without specific prior written permission.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE
 FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*******************************************************************************/


#include <my_global.h>
#include <my_compiler.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __GNUC__
#include <x86intrin.h>
#elif defined(_MSC_VER)
#include <intrin.h>
#else
#error "unknown compiler"
#endif

/**
 * @brief Shifts left 128 bit register by specified number of bytes
 *
 * @param reg 128 bit value
 * @param num number of bytes to shift left \a reg by (0-16)
 *
 * @return \a reg << (\a num * 8)
 */
static inline __m128i xmm_shift_left(__m128i reg, const unsigned int num)
{
  static const MY_ALIGNED(16) uint8_t crc_xmm_shift_tab[48]= {
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
      0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0xff, 0xff, 0xff, 0xff,
      0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

  const __m128i *p= (const __m128i *) (crc_xmm_shift_tab + 16 - num);

  return _mm_shuffle_epi8(reg, _mm_loadu_si128(p));
}

struct crcr_pclmulqdq_ctx
{
  uint64_t rk1;
  uint64_t rk2;
  uint64_t rk5;
  uint64_t rk6;
  uint64_t rk7;
  uint64_t rk8;
};

/**
 * @brief Performs one folding round
 *
 * Logically function operates as follows:
 *     DATA = READ_NEXT_16BYTES();
 *     F1 = LSB8(FOLD)
 *     F2 = MSB8(FOLD)
 *     T1 = CLMUL(F1, RK1)
 *     T2 = CLMUL(F2, RK2)
 *     FOLD = XOR(T1, T2, DATA)
 *
 * @param data_block 16 byte data block
 * @param precomp precomputed rk1 constanst
 * @param fold running 16 byte folded data
 *
 * @return New 16 byte folded data
 */
static inline __m128i crcr32_folding_round(const __m128i data_block,
                                    const __m128i precomp, const __m128i fold)
{
  __m128i tmp0= _mm_clmulepi64_si128(fold, precomp, 0x01);
  __m128i tmp1= _mm_clmulepi64_si128(fold, precomp, 0x10);

  return _mm_xor_si128(tmp1, _mm_xor_si128(data_block, tmp0));
}

/**
 * @brief Performs reduction from 128 bits to 64 bits
 *
 * @param data128 128 bits data to be reduced
 * @param precomp rk5 and rk6 precomputed constants
 *
 * @return data reduced to 64 bits
 */
static inline __m128i crcr32_reduce_128_to_64(__m128i data128, const __m128i precomp)
{
  __m128i tmp0, tmp1, tmp2;

  /* 64b fold */
  tmp0= _mm_clmulepi64_si128(data128, precomp, 0x00);
  tmp1= _mm_srli_si128(data128, 8);
  tmp0= _mm_xor_si128(tmp0, tmp1);

  /* 32b fold */
  tmp2= _mm_slli_si128(tmp0, 4);
  tmp1= _mm_clmulepi64_si128(tmp2, precomp, 0x10);

  return _mm_xor_si128(tmp1, tmp0);
}

/**
 * @brief Performs Barret's reduction from 64 bits to 32 bits
 *
 * @param data64 64 bits data to be reduced
 * @param precomp rk7 precomputed constant
 *
 * @return data reduced to 32 bits
 */
static inline uint32_t crcr32_reduce_64_to_32(__m128i data64, const __m128i precomp)
{
  static const MY_ALIGNED(16) uint32_t mask1[4]= {
      0xffffffff, 0xffffffff, 0x00000000, 0x00000000};
  static const MY_ALIGNED(16) uint32_t mask2[4]= {
      0x00000000, 0xffffffff, 0xffffffff, 0xffffffff};
  __m128i tmp0, tmp1, tmp2;

  tmp0= _mm_and_si128(data64, _mm_load_si128((__m128i *) mask2));

  tmp1= _mm_clmulepi64_si128(tmp0, precomp, 0x00);
  tmp1= _mm_xor_si128(tmp1, tmp0);
  tmp1= _mm_and_si128(tmp1, _mm_load_si128((__m128i *) mask1));

  tmp2= _mm_clmulepi64_si128(tmp1, precomp, 0x10);
  tmp2= _mm_xor_si128(tmp2, tmp1);
  tmp2= _mm_xor_si128(tmp2, tmp0);

  return _mm_extract_epi32(tmp2, 2);
}

/**
 * @brief Calculates reflected 32-bit CRC for given \a data block
 *        by applying folding and reduction methods.
 *
 * Algorithm operates on 32 bit CRCs.
 * Polynomials and initial values may need to be promoted to
 * 32 bits where required.
 *
 * @param crc initial CRC value (32 bit value)
 * @param data pointer to data block
 * @param data_len length of \a data block in bytes
 * @param params pointer to PCLMULQDQ CRC calculation context
 *
 * @return CRC for given \a data block (32 bits wide).
 */
static inline uint32_t crcr32_calc_pclmulqdq(const uint8_t *data, uint32_t data_len,
                                      uint32_t crc,
                                      const struct crcr_pclmulqdq_ctx *params)
{
  __m128i temp, fold, k;
  uint32_t n;

  DBUG_ASSERT(data != NULL || data_len == 0);
  DBUG_ASSERT(params);

  if (unlikely(data_len == 0))
    return crc;

  /**
   * Get CRC init value
   */
  temp= _mm_insert_epi32(_mm_setzero_si128(), crc, 0);

  /**
   * -------------------------------------------------
   * Folding all data into single 16 byte data block
   * Assumes: \a fold holds first 16 bytes of data
   */

  if (unlikely(data_len < 32))
  {
    if (unlikely(data_len == 16))
    {
      /* 16 bytes */
      fold= _mm_loadu_si128((__m128i *) data);
      fold= _mm_xor_si128(fold, temp);
      goto reduction_128_64;
    }
    if (unlikely(data_len < 16))
    {
      /* 0 to 15 bytes */
      MY_ALIGNED(16) uint8_t buffer[16];

      memset(buffer, 0, sizeof(buffer));
      memcpy(buffer, data, data_len);

      fold= _mm_load_si128((__m128i *) buffer);
      fold= _mm_xor_si128(fold, temp);
      if ((data_len < 4))
      {
        fold= xmm_shift_left(fold, 8 - data_len);
        goto barret_reduction;
      }
      fold= xmm_shift_left(fold, 16 - data_len);
      goto reduction_128_64;
    }
    /* 17 to 31 bytes */
    fold= _mm_loadu_si128((__m128i *) data);
    fold= _mm_xor_si128(fold, temp);
    n= 16;
    k= _mm_load_si128((__m128i *) (&params->rk1));
    goto partial_bytes;
  }

  /**
   * At least 32 bytes in the buffer
   */

  /**
   * Apply CRC initial value
   */
  fold= _mm_loadu_si128((const __m128i *) data);
  fold= _mm_xor_si128(fold, temp);

  /**
   * Main folding loop
   * - the last 16 bytes is processed separately
   */
  k= _mm_load_si128((__m128i *) (&params->rk1));
  for (n= 16; (n + 16) <= data_len; n+= 16)
  {
    temp= _mm_loadu_si128((__m128i *) &data[n]);
    fold= crcr32_folding_round(temp, k, fold);
  }

partial_bytes:
  if (likely(n < data_len))
  {
    static const MY_ALIGNED(16) uint32_t mask3[4]= {0x80808080, 0x80808080,
                                                   0x80808080, 0x80808080};
    static const MY_ALIGNED(16) uint8_t shf_table[32]= {
        0x00, 0x81, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a,
        0x8b, 0x8c, 0x8d, 0x8e, 0x8f, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
        0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f};
    __m128i last16, a, b;

    last16= _mm_loadu_si128((const __m128i *) &data[data_len - 16]);

    temp= _mm_loadu_si128((const __m128i *) &shf_table[data_len & 15]);
    a= _mm_shuffle_epi8(fold, temp);

    temp= _mm_xor_si128(temp, _mm_load_si128((const __m128i *) mask3));
    b= _mm_shuffle_epi8(fold, temp);
    b= _mm_blendv_epi8(b, last16, temp);

    /* k = rk1 & rk2 */
    temp= _mm_clmulepi64_si128(a, k, 0x01);
    fold= _mm_clmulepi64_si128(a, k, 0x10);

    fold= _mm_xor_si128(fold, temp);
    fold= _mm_xor_si128(fold, b);
  }

  /**
   * -------------------------------------------------
   * Reduction 128 -> 32
   * Assumes: \a fold holds 128bit folded data
   */
reduction_128_64:
  k= _mm_load_si128((__m128i *) (&params->rk5));
  fold= crcr32_reduce_128_to_64(fold, k);

barret_reduction:
  k= _mm_load_si128((__m128i *) (&params->rk7));
  n= crcr32_reduce_64_to_32(fold, k);
  return n;
}

static const MY_ALIGNED(16) struct crcr_pclmulqdq_ctx ether_crc32_clmul= {
    0xccaa009e,  /**< rk1 */
    0x1751997d0, /**< rk2 */
    0xccaa009e,  /**< rk5 */
    0x163cd6124, /**< rk6 */
    0x1f7011640, /**< rk7 */
    0x1db710641  /**< rk8 */
};

/**
 * @brief Calculates Ethernet CRC32 using PCLMULQDQ method.
 *
 * @param data pointer to data block to calculate CRC for
 * @param data_len size of data block
 *
 * @return New CRC value
 */
unsigned int crc32_pclmul(unsigned int crc32, const void *buf, size_t len)
{
  return ~crcr32_calc_pclmulqdq(buf, (uint32_t)len, ~crc32, &ether_crc32_clmul);
}
