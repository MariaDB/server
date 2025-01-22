/*****************************************************************************

Copyright (c) 2024 Kristian Nielsen.

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
/******************************************************************//**
@file include/ut0bitop.h
Reading and writing of compressed integers.

Created 2024-10-01 Kristian Nielsen <knielsen@knielsen-hq.org>
*******************************************************/

#include "univ.i"
#include "ut0compr_int.h"

/* Read and write compressed (up to) 64-bit integers. */

/*
  Write compressed unsigned integer, efficient version without assuming
  unaligned writes.
*/
unsigned char *compr_int_write(unsigned char *p, uint64_t v) {
  // Compute bytes needed to store the value v plus 3 bits encoding length.
  uint32_t needed_bits_minus_1= 66 - nlz(v|1);
  uint32_t needed_bytes= (needed_bits_minus_1 >> 3) + 1;

  // Compute the encoding of the length.
  // We need 1-9 bytes. Use 9 bytes instead of 8, so we can encode the
  // length in 3 bits for (1, 2, ..., 7, or 9 bytes).
  uint32_t bytes= needed_bytes | (needed_bytes >> 3);
  uint32_t len= needed_bytes - 1;
  // Encode 1-7 as 0-6, and encode 8,9 both as 8.
  len-= (len >> 3);

  // Compute the first 64-bit word to write.
  uintptr_t offset= (uintptr_t)p & (uintptr_t)7;
  uintptr_t offset_bits= offset << 3;
  uint64_t v1= (len | (v << 3)) << offset_bits;
  uint64_t *p1= (uint64_t *)(p - offset);
  uint64_t mask1= ~(uint64_t)0 << offset_bits;

  // Compute the second word to write (if any).
  uint64_t v2= v >> ((64 - 3) - offset_bits);
  uint64_t *p2= p1 + 1;

  // Write the value into next one or two 64-bit words, as needed.
  // Two words are needed if (offset + bytes) cross into the next word.
#ifdef WORDS_BIGENDIAN
  /*
    ToDo: On big-endian can use more efficient little-endian conversion, since
    we know the pointer is 8-byte aligned.
  */
  int8store((unsigned char *)p1,
            (uint8korr((unsigned char *)p1) & ~mask1) | v1);
#else
  *p1= (*p1 & ~mask1) | v1;
#endif
  if (offset + bytes >= 8) {
#ifdef WORDS_BIGENDIAN
    int8store((unsigned char *)p2, v2);
#else
    *p2= v2;
#endif
  }
  return p + bytes;
}


/*
  Read compressed integer, efficient version without assuming unaligned reads.
  Returns a pair of the value read and the incremented pointer.
*/
std::pair<uint64_t, const unsigned char *>
compr_int_read(const unsigned char *p)
{
  uintptr_t offset= (uintptr_t)p & (uintptr_t)7;
  uintptr_t offset_bits= offset << 3;
  const uint64_t *p_align= (const uint64_t *)((uintptr_t)p & ~(uintptr_t)7);
#ifdef WORDS_BIGENDIAN
  /*
    ToDo: On big-endian can use more efficient little-endian conversion, since
    we know the pointer is 8-byte aligned.
  */
  uint64_t v1= uint8korr((unsigned char *)p_align);
#else
  uint64_t v1= p_align[0];
#endif
  uint32_t len= (v1 >> offset_bits) & 7;
  uint32_t bytes= len + 1;
  bytes+= (bytes >> 3);
  uint64_t v;
  if (offset + bytes > 8) {
    uint64_t mask2= (~(uint64_t)0) >> ((16 - (offset + bytes)) << 3);
#ifdef WORDS_BIGENDIAN
    v= (v1 >> (3 + offset_bits)) |
      ((uint8korr((unsigned char *)(p_align + 1)) & mask2) <<
       (61 - offset_bits));
#else
    v= (v1 >> (3 + offset_bits)) | ((p_align[1] & mask2) << (61 - offset_bits));
#endif
  } else {
    uint64_t mask1= (~(uint64_t)0) >> ((7 - (offset + len)) << 3);
    v= (v1 & mask1) >> (3 + offset_bits);
  }
  return std::pair<uint64_t, const unsigned char *>(v, p + bytes);
}


#ifdef TEST_MAIN
#include <stdlib.h>
#include <string.h>
#include <stdio.h>


// Smaller version that assumes unaligned writes of 8-bit values is ok, and
// that there are up to 8 scratch bytes available after the value written.
unsigned char *compr_int_write_le_unaligned_buffer(unsigned char *p, uint64_t v) {
  // Compute bytes needed to store the value v plus 3 bits encoding length.
  uint32_t needed_bits_minus_1= 66 - nlz(v|1);
  uint32_t needed_bytes= (needed_bits_minus_1 >> 3) + 1;

  // Compute the encoding of the length.
  // We need 1-9 bytes. Use 9 bytes instead of 8, so we can encode the
  // length in 3 bits for (1, 2, ..., 7, or 9 bytes).
  uint32_t bytes= needed_bytes | (needed_bytes >> 3);
  uint32_t len= needed_bytes - 1;
  // Encode 1-7 as 0-6, and encode 8,9 both as 8.
  len-= (len >> 3);

  // Write the (up to) 9 bytes, prefering redundant write to conditional jump.
  *(uint64_t *)p= len | (v << 3);
  *(p+8)= v >> 63;
  return p + bytes;
}

// Generic version without assumptions.
unsigned char *compr_int_write_generic(unsigned char *p, uint64_t v) {
  // Compute bytes needed to store the value v plus 3 bits encoding length.
  uint32_t needed_bits_minus_1= 66 - nlz(v|1);
  uint32_t needed_bytes= (needed_bits_minus_1 >> 3) + 1;

  // Compute the encoding of the length.
  // We need 1-9 bytes. Use 9 bytes instead of 8, so we can encode the
  // length in 3 bits for (1, 2, ..., 7, or 9 bytes).
  uint32_t bytes= needed_bytes | (needed_bytes >> 3);
  uint32_t len= needed_bytes - 1;
  // Encode 1-7 as 0-6, and encode 8,9 both as 8.
  len-= (len >> 3);

  // Write the necessary bytes out.
  *p++= len | (v << 3);
  v >>= 5;
  while (--bytes > 0) {
    *p++= v;
    v>>= 8;
  }
  return p;

}


// Generic read compressed integers.
std::pair<uint64_t, const unsigned char *>
compr_int_read_generic(const unsigned char *p)
{
  uint64_t v= *p++;
  uint32_t bytes= v & 7;
  v>>= 3;
  uint32_t shift= 5;
  bytes+= ((bytes + 1) >> 3);  // A 7 means read 8 bytes more (9 total)
  while (bytes-- > 0) {
    v|= ((uint64_t)(*p++)) << shift;
    shift+= 8;
  }
  return std::pair<uint64_t, const unsigned char *>(v, p);
}


// Read compressed integers assuming little-endian, efficient unaligned
// 64-bit reads, and up to 7 bytes of scratch space after.
std::pair<uint64_t, const unsigned char *>
compr_int_read_le_unaligned_buf(const unsigned char *p)
{
  uint64_t v= *(const uint64_t *)p;
  uint32_t len= v & 7;
  uint64_t mask= (~(uint64_t)0) >> ((7 - len) << 3);
  v= (v & mask) >> 3;
  // Need for extra read is assumed rare, well-predicted conditional jump
  // likely faster.
  uint32_t bytes= len + 1;
  if (__builtin_expect((len == 7), 0)) {
    v|= ((uint64_t)p[8]) << 61;   // Add last 3 bits
    bytes+= 1;                    // 7 means read 9 bytes
  }
  return std::pair<uint64_t, const unsigned char *>(v, p + bytes);
}


int
main(int argc, char *argv[])
{
  int N= (argc > 1 ? atoi(argv[1]) : 1000);
  uint64_t *src= new uint64_t[N];
  unsigned char *buf1= new unsigned char[N*9];
  unsigned char *buf2= new unsigned char[N*9];
  int i;

  // Generate test data.
  for (i= 0; i < N; ++i)
    src[i]= ((uint64_t)1 << (rand() % 64)) + (uint64_t)(rand());
  // Write test data.
  unsigned char *p1= buf1;
  unsigned char *p2= buf2;
  for (i= 0; i < N; ++i) {
    p1= compr_int_write(p1, src[i]);
    p2= compr_int_write_generic(p2, src[i]);
  }
  if ((p1 - buf1) != (p2 - buf2)) {
    fprintf(stderr, "Write error! Mismatch lengths of optimised and generic.\n");
  } else {
    if (memcmp(buf1, buf2, p1 - buf1))
      fprintf(stderr, "Write error! Mismatch data of optimised and generic.\n");
  }
  // Verify written data.
  std::pair<uint64_t, const unsigned char *>q1, q2;
  const unsigned char *c1= buf1;
  const unsigned char *c2= buf2;
  for (i= 0; i < N; ++i) {
    q1= compr_int_read(c1);
    q2= compr_int_read_generic(c2);
    uint64_t v1= q1.first;
    c1= q1.second;
    uint64_t v2= q2.first;
    c2= q2.second;
    if (v1 != v2 || v1 != src[i]) {
      fprintf(stderr, "Read error! mismatch values @ i=%d 0x%llx 0x%llx 0x%llx.\n",
              i, (unsigned long long)v1, (unsigned long long)v2,
              (unsigned long long)(src[i]));
      break;
    }
  }
  return 0;
}

#endif  /* TEST_MAIN */
