/* Copyright (c) 2019, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/**
  @file

  Unit tests for serialization and deserialization functions
*/

#include "tap.h"

#include "my_byteorder.h"
#include "myisampack.h"
#include "m_string.h"

void test_byte_order()
{
  MY_ALIGNED(CPU_LEVEL1_DCACHE_LINESIZE)
  uchar buf[CPU_LEVEL1_DCACHE_LINESIZE * 2];

  uchar *aligned= buf;
  uchar *not_aligned= buf + CPU_LEVEL1_DCACHE_LINESIZE - 1;

#define TEST(STORE_NAME, LOAD_NAME, TYPE, VALUE, BYTES)                        \
  {                                                                            \
    TYPE value= VALUE;                                                         \
    uchar bytes[]= BYTES;                                                      \
    STORE_NAME(aligned, value);                                                \
    ok(!memcmp(aligned, bytes, sizeof(bytes)), "aligned\t\t" #STORE_NAME);     \
    ok(LOAD_NAME(aligned) == value, "aligned\t\t" #LOAD_NAME);                 \
    STORE_NAME(not_aligned, value);                                            \
    ok(!memcmp(not_aligned, bytes, sizeof(bytes)),                             \
       "not aligned\t" #STORE_NAME);                                           \
    ok(LOAD_NAME(not_aligned) == value, "not aligned\t" #LOAD_NAME);           \
  }

#define ARRAY_2(A, B) {A, B}
#define ARRAY_3(A, B, C) {A, B, C}
#define ARRAY_4(A, B, C, D) {A, B, C, D}
#define ARRAY_5(A, B, C, D, E) {A, B, C, D, E}
#define ARRAY_6(A, B, C, D, E, F) {A, B, C, D, E, F}
#define ARRAY_7(A, B, C, D, E, F, G) {A, B, C, D, E, F, G}
#define ARRAY_8(A, B, C, D, E, F, G, H) {A, B, C, D, E, F, G, H}

  TEST(int2store, sint2korr, int16, 0x0201, ARRAY_2(1, 2));
  TEST(int3store, sint3korr, int32, 0xffffffff, ARRAY_3(0xff, 0xff, 0xff));
  TEST(int3store, sint3korr, int32, 0x030201, ARRAY_3(1, 2, 3));
  TEST(int4store, sint4korr, int32, 0xffffffff,
       ARRAY_4(0xff, 0xff, 0xff, 0xff));
  TEST(int4store, sint4korr, int32, 0x04030201, ARRAY_4(1, 2, 3, 4));
  TEST(int8store, sint8korr, longlong, 0x0807060504030201,
       ARRAY_8(1, 2, 3, 4, 5, 6, 7, 8));
  TEST(int8store, sint8korr, longlong, 0xffffffffffffffff,
       ARRAY_8(0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff));

  TEST(int2store, uint2korr, uint16, 0x0201, ARRAY_2(1, 2));
  TEST(int3store, uint3korr, uint32, 0x030201, ARRAY_3(1, 2, 3));
  TEST(int4store, uint4korr, uint32, 0x04030201, ARRAY_4(1, 2, 3, 4));
  TEST(int5store, uint5korr, ulonglong, 0x0504030201, ARRAY_5(1, 2, 3, 4, 5));
  TEST(int6store, uint6korr, ulonglong, 0x060504030201,
       ARRAY_6(1, 2, 3, 4, 5, 6));
  TEST(int8store, uint8korr, ulonglong, 0x0807060504030201,
       ARRAY_8(1, 2, 3, 4, 5, 6, 7, 8));

  TEST(mi_int5store, mi_uint5korr, ulonglong, 0x0504030201,
       ARRAY_5(5, 4, 3, 2, 1));
  TEST(mi_int6store, mi_uint6korr, ulonglong, 0x060504030201,
       ARRAY_6(6, 5, 4, 3, 2, 1));
  TEST(mi_int7store, mi_uint7korr, ulonglong, 0x07060504030201,
       ARRAY_7(7, 6, 5, 4, 3, 2, 1));
  TEST(mi_int8store, mi_uint8korr, ulonglong, 0x0807060504030201,
       ARRAY_8(8, 7, 6, 5, 4, 3, 2, 1));

#undef ARRAY_8
#undef ARRAY_7
#undef ARRAY_6
#undef ARRAY_5
#undef ARRAY_4
#undef ARRAY_3
#undef ARRAY_2

#undef TEST
}

int main(int argc __attribute__((unused)), char **argv __attribute__((unused)))
{
  plan(68);
  test_byte_order();
  return exit_status();
}
