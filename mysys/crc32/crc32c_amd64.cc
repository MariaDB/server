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
 * Copyright 2016 Ferry Toth, Exalon Delft BV, The Netherlands
 *  This software is provided 'as-is', without any express or implied
 * warranty.  In no event will the author be held liable for any damages
 * arising from the use of this software.
 *  Permission is granted to anyone to use this software for any purpose,
 * including commercial applications, and to alter it and redistribute it
 * freely, subject to the following restrictions:
 *  1. The origin of this software must not be misrepresented; you must not
 *   claim that you wrote the original software. If you use this software
 *   in a product, an acknowledgment in the product documentation would be
 *   appreciated but is not required.
 * 2. Altered source versions must be plainly marked as such, and must not be
 *   misrepresented as being the original software.
 * 3. This notice may not be removed or altered from any source distribution.
 *  Ferry Toth
 * ftoth@exalondelft.nl
 *
 * https://github.com/htot/crc32c
 *
 * Modified by Facebook
 *
 * Original intel whitepaper:
 * "Fast CRC Computation for iSCSI Polynomial Using CRC32 Instruction"
 * https://www.intel.com/content/dam/www/public/us/en/documents/white-papers/crc-iscsi-polynomial-crc32-instruction-paper.pdf
 *
 * This version is from the folly library, created by Dave Watson <davejwatson@fb.com>
 *
*/

#include <stdint.h>
#include <nmmintrin.h>
#include <wmmintrin.h>


#define CRCtriplet(crc, buf, offset)                  \
  crc##0 = _mm_crc32_u64(crc##0, *(buf##0 + offset)); \
  crc##1 = _mm_crc32_u64(crc##1, *(buf##1 + offset)); \
  crc##2 = _mm_crc32_u64(crc##2, *(buf##2 + offset));

#define CRCduplet(crc, buf, offset)                   \
  crc##0 = _mm_crc32_u64(crc##0, *(buf##0 + offset)); \
  crc##1 = _mm_crc32_u64(crc##1, *(buf##1 + offset));

#define CRCsinglet(crc, buf, offset)                    \
  crc = _mm_crc32_u64(crc, *(uint64_t*)(buf + offset));


// Numbers taken directly from intel whitepaper.
// clang-format off
static const uint64_t clmul_constants alignas(16) [] = {
    0x14cd00bd6, 0x105ec76f0, 0x0ba4fc28e, 0x14cd00bd6,
    0x1d82c63da, 0x0f20c0dfe, 0x09e4addf8, 0x0ba4fc28e,
    0x039d3b296, 0x1384aa63a, 0x102f9b8a2, 0x1d82c63da,
    0x14237f5e6, 0x01c291d04, 0x00d3b6092, 0x09e4addf8,
    0x0c96cfdc0, 0x0740eef02, 0x18266e456, 0x039d3b296,
    0x0daece73e, 0x0083a6eec, 0x0ab7aff2a, 0x102f9b8a2,
    0x1248ea574, 0x1c1733996, 0x083348832, 0x14237f5e6,
    0x12c743124, 0x02ad91c30, 0x0b9e02b86, 0x00d3b6092,
    0x018b33a4e, 0x06992cea2, 0x1b331e26a, 0x0c96cfdc0,
    0x17d35ba46, 0x07e908048, 0x1bf2e8b8a, 0x18266e456,
    0x1a3e0968a, 0x11ed1f9d8, 0x0ce7f39f4, 0x0daece73e,
    0x061d82e56, 0x0f1d0f55e, 0x0d270f1a2, 0x0ab7aff2a,
    0x1c3f5f66c, 0x0a87ab8a8, 0x12ed0daac, 0x1248ea574,
    0x065863b64, 0x08462d800, 0x11eef4f8e, 0x083348832,
    0x1ee54f54c, 0x071d111a8, 0x0b3e32c28, 0x12c743124,
    0x0064f7f26, 0x0ffd852c6, 0x0dd7e3b0c, 0x0b9e02b86,
    0x0f285651c, 0x0dcb17aa4, 0x010746f3c, 0x018b33a4e,
    0x1c24afea4, 0x0f37c5aee, 0x0271d9844, 0x1b331e26a,
    0x08e766a0c, 0x06051d5a2, 0x093a5f730, 0x17d35ba46,
    0x06cb08e5c, 0x11d5ca20e, 0x06b749fb2, 0x1bf2e8b8a,
    0x1167f94f2, 0x021f3d99c, 0x0cec3662e, 0x1a3e0968a,
    0x19329634a, 0x08f158014, 0x0e6fc4e6a, 0x0ce7f39f4,
    0x08227bb8a, 0x1a5e82106, 0x0b0cd4768, 0x061d82e56,
    0x13c2b89c4, 0x188815ab2, 0x0d7a4825c, 0x0d270f1a2,
    0x10f5ff2ba, 0x105405f3e, 0x00167d312, 0x1c3f5f66c,
    0x0f6076544, 0x0e9adf796, 0x026f6a60a, 0x12ed0daac,
    0x1a2adb74e, 0x096638b34, 0x19d34af3a, 0x065863b64,
    0x049c3cc9c, 0x1e50585a0, 0x068bce87a, 0x11eef4f8e,
    0x1524fa6c6, 0x19f1c69dc, 0x16cba8aca, 0x1ee54f54c,
    0x042d98888, 0x12913343e, 0x1329d9f7e, 0x0b3e32c28,
    0x1b1c69528, 0x088f25a3a, 0x02178513a, 0x0064f7f26,
    0x0e0ac139e, 0x04e36f0b0, 0x0170076fa, 0x0dd7e3b0c,
    0x141a1a2e2, 0x0bd6f81f8, 0x16ad828b4, 0x0f285651c,
    0x041d17b64, 0x19425cbba, 0x1fae1cc66, 0x010746f3c,
    0x1a75b4b00, 0x18db37e8a, 0x0f872e54c, 0x1c24afea4,
    0x01e41e9fc, 0x04c144932, 0x086d8e4d2, 0x0271d9844,
    0x160f7af7a, 0x052148f02, 0x05bb8f1bc, 0x08e766a0c,
    0x0a90fd27a, 0x0a3c6f37a, 0x0b3af077a, 0x093a5f730,
    0x04984d782, 0x1d22c238e, 0x0ca6ef3ac, 0x06cb08e5c,
    0x0234e0b26, 0x063ded06a, 0x1d88abd4a, 0x06b749fb2,
    0x04597456a, 0x04d56973c, 0x0e9e28eb4, 0x1167f94f2,
    0x07b3ff57a, 0x19385bf2e, 0x0c9c8b782, 0x0cec3662e,
    0x13a9cba9e, 0x0e417f38a, 0x093e106a4, 0x19329634a,
    0x167001a9c, 0x14e727980, 0x1ddffc5d4, 0x0e6fc4e6a,
    0x00df04680, 0x0d104b8fc, 0x02342001e, 0x08227bb8a,
    0x00a2a8d7e, 0x05b397730, 0x168763fa6, 0x0b0cd4768,
    0x1ed5a407a, 0x0e78eb416, 0x0d2c3ed1a, 0x13c2b89c4,
    0x0995a5724, 0x1641378f0, 0x19b1afbc4, 0x0d7a4825c,
    0x109ffedc0, 0x08d96551c, 0x0f2271e60, 0x10f5ff2ba,
    0x00b0bf8ca, 0x00bf80dd2, 0x123888b7a, 0x00167d312,
    0x1e888f7dc, 0x18dcddd1c, 0x002ee03b2, 0x0f6076544,
    0x183e8d8fe, 0x06a45d2b2, 0x133d7a042, 0x026f6a60a,
    0x116b0f50c, 0x1dd3e10e8, 0x05fabe670, 0x1a2adb74e,
    0x130004488, 0x0de87806c, 0x000bcf5f6, 0x19d34af3a,
    0x18f0c7078, 0x014338754, 0x017f27698, 0x049c3cc9c,
    0x058ca5f00, 0x15e3e77ee, 0x1af900c24, 0x068bce87a,
    0x0b5cfca28, 0x0dd07448e, 0x0ded288f8, 0x1524fa6c6,
    0x059f229bc, 0x1d8048348, 0x06d390dec, 0x16cba8aca,
    0x037170390, 0x0a3e3e02c, 0x06353c1cc, 0x042d98888,
    0x0c4584f5c, 0x0d73c7bea, 0x1f16a3418, 0x1329d9f7e,
    0x0531377e2, 0x185137662, 0x1d8d9ca7c, 0x1b1c69528,
    0x0b25b29f2, 0x18a08b5bc, 0x19fb2a8b0, 0x02178513a,
    0x1a08fe6ac, 0x1da758ae0, 0x045cddf4e, 0x0e0ac139e,
    0x1a91647f2, 0x169cf9eb0, 0x1a0f717c4, 0x0170076fa,
};

// Compute the crc32c value for buffer smaller than 8
static inline void align_to_8(
    size_t len,
    uint64_t& crc0, // crc so far, updated on return
    const unsigned char*& next) { // next data pointer, updated on return
  uint32_t crc32bit = static_cast<uint32_t>(crc0);
  if (len & 0x04) {
    crc32bit = _mm_crc32_u32(crc32bit, *(uint32_t*)next);
    next += sizeof(uint32_t);
  }
  if (len & 0x02) {
    crc32bit = _mm_crc32_u16(crc32bit, *(uint16_t*)next);
    next += sizeof(uint16_t);
  }
  if (len & 0x01) {
    crc32bit = _mm_crc32_u8(crc32bit, *(next));
    next++;
  }
  crc0 = crc32bit;
}

//
// CombineCRC performs pclmulqdq multiplication of 2 partial CRC's and a well
// chosen constant and xor's these with the remaining CRC.
//
static inline uint64_t CombineCRC(
    size_t block_size,
    uint64_t crc0,
    uint64_t crc1,
    uint64_t crc2,
    const uint64_t* next2) {
  const auto multiplier =
      *(reinterpret_cast<const __m128i*>(clmul_constants) + block_size - 1);
  const auto crc0_xmm = _mm_set_epi64x(0, crc0);
  const auto res0 = _mm_clmulepi64_si128(crc0_xmm, multiplier, 0x00);
  const auto crc1_xmm = _mm_set_epi64x(0, crc1);
  const auto res1 = _mm_clmulepi64_si128(crc1_xmm, multiplier, 0x10);
  const auto res = _mm_xor_si128(res0, res1);
  crc0 = _mm_cvtsi128_si64(res);
  crc0 = crc0 ^ *((uint64_t*)next2 - 1);
  crc2 = _mm_crc32_u64(crc2, crc0);
  return crc2;
}

// Compute CRC-32C using the Intel hardware instruction.
extern "C"
uint32_t crc32c_3way(uint32_t crc, const char *buf, size_t len)
{
  const unsigned char* next = (const unsigned char*)buf;
  uint64_t count;
  uint64_t crc0, crc1, crc2;
  crc0 = crc ^ 0xffffffffu;


  if (len >= 8) {
    // if len > 216 then align and use triplets
    if (len > 216) {
      {
        // Work on the bytes (< 8) before the first 8-byte alignment addr starts
        auto align_bytes = (8 - (uintptr_t)next) & 7;
        len -= align_bytes;
        align_to_8(align_bytes, crc0, next);
      }

      // Now work on the remaining blocks
      count = len / 24; // number of triplets
      len %= 24; // bytes remaining
      uint64_t n = count >> 7; // #blocks = first block + full blocks
      uint64_t block_size = count & 127;
      if (block_size == 0) {
        block_size = 128;
      } else {
        n++;
      }
      // points to the first byte of the next block
      const uint64_t* next0 = (uint64_t*)next + block_size;
      const uint64_t* next1 = next0 + block_size;
      const uint64_t* next2 = next1 + block_size;

      crc1 = crc2 = 0;
      // Use Duff's device, a for() loop inside a switch()
      // statement. This needs to execute at least once, round len
      // down to nearest triplet multiple
      switch (block_size) {
        case 128:
          do {
            // jumps here for a full block of len 128
            CRCtriplet(crc, next, -128);
              /* fallthrough */
            case 127:
              // jumps here or below for the first block smaller
              CRCtriplet(crc, next, -127);
              /* fallthrough */
            case 126:
              CRCtriplet(crc, next, -126); // than 128
              /* fallthrough */
            case 125:
              CRCtriplet(crc, next, -125);
              /* fallthrough */
            case 124:
              CRCtriplet(crc, next, -124);
              /* fallthrough */
            case 123:
              CRCtriplet(crc, next, -123);
              /* fallthrough */
            case 122:
              CRCtriplet(crc, next, -122);
              /* fallthrough */
            case 121:
              CRCtriplet(crc, next, -121);
              /* fallthrough */
            case 120:
              CRCtriplet(crc, next, -120);
              /* fallthrough */
            case 119:
              CRCtriplet(crc, next, -119);
              /* fallthrough */
            case 118:
              CRCtriplet(crc, next, -118);
              /* fallthrough */
            case 117:
              CRCtriplet(crc, next, -117);
              /* fallthrough */
            case 116:
              CRCtriplet(crc, next, -116);
              /* fallthrough */
            case 115:
              CRCtriplet(crc, next, -115);
              /* fallthrough */
            case 114:
              CRCtriplet(crc, next, -114);
              /* fallthrough */
            case 113:
              CRCtriplet(crc, next, -113);
              /* fallthrough */
            case 112:
              CRCtriplet(crc, next, -112);
              /* fallthrough */
            case 111:
              CRCtriplet(crc, next, -111);
              /* fallthrough */
            case 110:
              CRCtriplet(crc, next, -110);
              /* fallthrough */
            case 109:
              CRCtriplet(crc, next, -109);
              /* fallthrough */
            case 108:
              CRCtriplet(crc, next, -108);
              /* fallthrough */
            case 107:
              CRCtriplet(crc, next, -107);
              /* fallthrough */
            case 106:
              CRCtriplet(crc, next, -106);
              /* fallthrough */
            case 105:
              CRCtriplet(crc, next, -105);
              /* fallthrough */
            case 104:
              CRCtriplet(crc, next, -104);
              /* fallthrough */
            case 103:
              CRCtriplet(crc, next, -103);
              /* fallthrough */
            case 102:
              CRCtriplet(crc, next, -102);
              /* fallthrough */
            case 101:
              CRCtriplet(crc, next, -101);
              /* fallthrough */
            case 100:
              CRCtriplet(crc, next, -100);
              /* fallthrough */
            case 99:
              CRCtriplet(crc, next, -99);
              /* fallthrough */
            case 98:
              CRCtriplet(crc, next, -98);
              /* fallthrough */
            case 97:
              CRCtriplet(crc, next, -97);
              /* fallthrough */
            case 96:
              CRCtriplet(crc, next, -96);
              /* fallthrough */
            case 95:
              CRCtriplet(crc, next, -95);
              /* fallthrough */
            case 94:
              CRCtriplet(crc, next, -94);
              /* fallthrough */
            case 93:
              CRCtriplet(crc, next, -93);
              /* fallthrough */
            case 92:
              CRCtriplet(crc, next, -92);
              /* fallthrough */
            case 91:
              CRCtriplet(crc, next, -91);
              /* fallthrough */
            case 90:
              CRCtriplet(crc, next, -90);
              /* fallthrough */
            case 89:
              CRCtriplet(crc, next, -89);
              /* fallthrough */
            case 88:
              CRCtriplet(crc, next, -88);
              /* fallthrough */
            case 87:
              CRCtriplet(crc, next, -87);
              /* fallthrough */
            case 86:
              CRCtriplet(crc, next, -86);
              /* fallthrough */
            case 85:
              CRCtriplet(crc, next, -85);
              /* fallthrough */
            case 84:
              CRCtriplet(crc, next, -84);
              /* fallthrough */
            case 83:
              CRCtriplet(crc, next, -83);
              /* fallthrough */
            case 82:
              CRCtriplet(crc, next, -82);
              /* fallthrough */
            case 81:
              CRCtriplet(crc, next, -81);
              /* fallthrough */
            case 80:
              CRCtriplet(crc, next, -80);
              /* fallthrough */
            case 79:
              CRCtriplet(crc, next, -79);
              /* fallthrough */
            case 78:
              CRCtriplet(crc, next, -78);
              /* fallthrough */
            case 77:
              CRCtriplet(crc, next, -77);
              /* fallthrough */
            case 76:
              CRCtriplet(crc, next, -76);
              /* fallthrough */
            case 75:
              CRCtriplet(crc, next, -75);
              /* fallthrough */
            case 74:
              CRCtriplet(crc, next, -74);
              /* fallthrough */
            case 73:
              CRCtriplet(crc, next, -73);
              /* fallthrough */
            case 72:
              CRCtriplet(crc, next, -72);
              /* fallthrough */
            case 71:
              CRCtriplet(crc, next, -71);
              /* fallthrough */
            case 70:
              CRCtriplet(crc, next, -70);
              /* fallthrough */
            case 69:
              CRCtriplet(crc, next, -69);
              /* fallthrough */
            case 68:
              CRCtriplet(crc, next, -68);
              /* fallthrough */
            case 67:
              CRCtriplet(crc, next, -67);
              /* fallthrough */
            case 66:
              CRCtriplet(crc, next, -66);
              /* fallthrough */
            case 65:
              CRCtriplet(crc, next, -65);
              /* fallthrough */
            case 64:
              CRCtriplet(crc, next, -64);
              /* fallthrough */
            case 63:
              CRCtriplet(crc, next, -63);
              /* fallthrough */
            case 62:
              CRCtriplet(crc, next, -62);
              /* fallthrough */
            case 61:
              CRCtriplet(crc, next, -61);
              /* fallthrough */
            case 60:
              CRCtriplet(crc, next, -60);
              /* fallthrough */
            case 59:
              CRCtriplet(crc, next, -59);
              /* fallthrough */
            case 58:
              CRCtriplet(crc, next, -58);
              /* fallthrough */
            case 57:
              CRCtriplet(crc, next, -57);
              /* fallthrough */
            case 56:
              CRCtriplet(crc, next, -56);
              /* fallthrough */
            case 55:
              CRCtriplet(crc, next, -55);
              /* fallthrough */
            case 54:
              CRCtriplet(crc, next, -54);
              /* fallthrough */
            case 53:
              CRCtriplet(crc, next, -53);
              /* fallthrough */
            case 52:
              CRCtriplet(crc, next, -52);
              /* fallthrough */
            case 51:
              CRCtriplet(crc, next, -51);
              /* fallthrough */
            case 50:
              CRCtriplet(crc, next, -50);
              /* fallthrough */
            case 49:
              CRCtriplet(crc, next, -49);
              /* fallthrough */
            case 48:
              CRCtriplet(crc, next, -48);
              /* fallthrough */
            case 47:
              CRCtriplet(crc, next, -47);
              /* fallthrough */
            case 46:
              CRCtriplet(crc, next, -46);
              /* fallthrough */
            case 45:
              CRCtriplet(crc, next, -45);
              /* fallthrough */
            case 44:
              CRCtriplet(crc, next, -44);
              /* fallthrough */
            case 43:
              CRCtriplet(crc, next, -43);
              /* fallthrough */
            case 42:
              CRCtriplet(crc, next, -42);
              /* fallthrough */
            case 41:
              CRCtriplet(crc, next, -41);
              /* fallthrough */
            case 40:
              CRCtriplet(crc, next, -40);
              /* fallthrough */
            case 39:
              CRCtriplet(crc, next, -39);
              /* fallthrough */
            case 38:
              CRCtriplet(crc, next, -38);
              /* fallthrough */
            case 37:
              CRCtriplet(crc, next, -37);
              /* fallthrough */
            case 36:
              CRCtriplet(crc, next, -36);
              /* fallthrough */
            case 35:
              CRCtriplet(crc, next, -35);
              /* fallthrough */
            case 34:
              CRCtriplet(crc, next, -34);
              /* fallthrough */
            case 33:
              CRCtriplet(crc, next, -33);
              /* fallthrough */
            case 32:
              CRCtriplet(crc, next, -32);
              /* fallthrough */
            case 31:
              CRCtriplet(crc, next, -31);
              /* fallthrough */
            case 30:
              CRCtriplet(crc, next, -30);
              /* fallthrough */
            case 29:
              CRCtriplet(crc, next, -29);
              /* fallthrough */
            case 28:
              CRCtriplet(crc, next, -28);
              /* fallthrough */
            case 27:
              CRCtriplet(crc, next, -27);
              /* fallthrough */
            case 26:
              CRCtriplet(crc, next, -26);
              /* fallthrough */
            case 25:
              CRCtriplet(crc, next, -25);
              /* fallthrough */
            case 24:
              CRCtriplet(crc, next, -24);
              /* fallthrough */
            case 23:
              CRCtriplet(crc, next, -23);
              /* fallthrough */
            case 22:
              CRCtriplet(crc, next, -22);
              /* fallthrough */
            case 21:
              CRCtriplet(crc, next, -21);
              /* fallthrough */
            case 20:
              CRCtriplet(crc, next, -20);
              /* fallthrough */
            case 19:
              CRCtriplet(crc, next, -19);
              /* fallthrough */
            case 18:
              CRCtriplet(crc, next, -18);
              /* fallthrough */
            case 17:
              CRCtriplet(crc, next, -17);
              /* fallthrough */
            case 16:
              CRCtriplet(crc, next, -16);
              /* fallthrough */
            case 15:
              CRCtriplet(crc, next, -15);
              /* fallthrough */
            case 14:
              CRCtriplet(crc, next, -14);
              /* fallthrough */
            case 13:
              CRCtriplet(crc, next, -13);
              /* fallthrough */
            case 12:
              CRCtriplet(crc, next, -12);
              /* fallthrough */
            case 11:
              CRCtriplet(crc, next, -11);
              /* fallthrough */
            case 10:
              CRCtriplet(crc, next, -10);
              /* fallthrough */
            case 9:
              CRCtriplet(crc, next, -9);
              /* fallthrough */
            case 8:
              CRCtriplet(crc, next, -8);
              /* fallthrough */
            case 7:
              CRCtriplet(crc, next, -7);
              /* fallthrough */
            case 6:
              CRCtriplet(crc, next, -6);
              /* fallthrough */
            case 5:
              CRCtriplet(crc, next, -5);
              /* fallthrough */
            case 4:
              CRCtriplet(crc, next, -4);
              /* fallthrough */
            case 3:
              CRCtriplet(crc, next, -3);
              /* fallthrough */
            case 2:
              CRCtriplet(crc, next, -2);
              /* fallthrough */
            case 1:
              CRCduplet(crc, next, -1); // the final triplet is actually only 2
              //{ CombineCRC(); }
              crc0 = CombineCRC(block_size, crc0, crc1, crc2, next2);
              if (--n > 0) {
                crc1 = crc2 = 0;
                block_size = 128;
                // points to the first byte of the next block
                next0 = next2 + 128;
                next1 = next0 + 128; // from here on all blocks are 128 long
                next2 = next1 + 128;
              }
              /* fallthrough */
            case 0:;
          } while (n > 0);
      }
      next = (const unsigned char*)next2;
    }
    uint64_t count2 = len >> 3; // 216 of less bytes is 27 or less singlets
    len = len & 7;
    next += (count2 * 8);
    switch (count2) {
      case 27:
        CRCsinglet(crc0, next, -27 * 8);
        /* fallthrough */
      case 26:
        CRCsinglet(crc0, next, -26 * 8);
        /* fallthrough */
      case 25:
        CRCsinglet(crc0, next, -25 * 8);
        /* fallthrough */
      case 24:
        CRCsinglet(crc0, next, -24 * 8);
        /* fallthrough */
      case 23:
        CRCsinglet(crc0, next, -23 * 8);
        /* fallthrough */
      case 22:
        CRCsinglet(crc0, next, -22 * 8);
        /* fallthrough */
      case 21:
        CRCsinglet(crc0, next, -21 * 8);
        /* fallthrough */
      case 20:
        CRCsinglet(crc0, next, -20 * 8);
        /* fallthrough */
      case 19:
        CRCsinglet(crc0, next, -19 * 8);
        /* fallthrough */
      case 18:
        CRCsinglet(crc0, next, -18 * 8);
        /* fallthrough */
      case 17:
        CRCsinglet(crc0, next, -17 * 8);
        /* fallthrough */
      case 16:
        CRCsinglet(crc0, next, -16 * 8);
        /* fallthrough */
      case 15:
        CRCsinglet(crc0, next, -15 * 8);
        /* fallthrough */
      case 14:
        CRCsinglet(crc0, next, -14 * 8);
        /* fallthrough */
      case 13:
        CRCsinglet(crc0, next, -13 * 8);
        /* fallthrough */
      case 12:
        CRCsinglet(crc0, next, -12 * 8);
        /* fallthrough */
      case 11:
        CRCsinglet(crc0, next, -11 * 8);
        /* fallthrough */
      case 10:
        CRCsinglet(crc0, next, -10 * 8);
        /* fallthrough */
      case 9:
        CRCsinglet(crc0, next, -9 * 8);
        /* fallthrough */
      case 8:
        CRCsinglet(crc0, next, -8 * 8);
        /* fallthrough */
      case 7:
        CRCsinglet(crc0, next, -7 * 8);
        /* fallthrough */
      case 6:
        CRCsinglet(crc0, next, -6 * 8);
        /* fallthrough */
      case 5:
        CRCsinglet(crc0, next, -5 * 8);
        /* fallthrough */
      case 4:
        CRCsinglet(crc0, next, -4 * 8);
        /* fallthrough */
      case 3:
        CRCsinglet(crc0, next, -3 * 8);
        /* fallthrough */
      case 2:
        CRCsinglet(crc0, next, -2 * 8);
        /* fallthrough */
      case 1:
        CRCsinglet(crc0, next, -1 * 8);
        /* fallthrough */
      case 0:;
    }
  }
  {
    align_to_8(len, crc0, next);
    return (uint32_t)crc0 ^ 0xffffffffu;
  }
}
