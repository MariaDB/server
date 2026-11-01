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

  /*
    Floating point macros. The stored bytes are IEEE 754, little-endian
    (float4store etc.) or big-endian (the mi_ variants), on any host.
  */
#define TEST_FLOAT(STORE_NAME, LOAD_NAME, TYPE, VALUE, BYTES)                  \
  {                                                                            \
    TYPE value= VALUE, loaded;                                                 \
    uchar bytes[]= BYTES;                                                      \
    STORE_NAME(aligned, value);                                                \
    ok(!memcmp(aligned, bytes, sizeof(bytes)), "aligned\t\t" #STORE_NAME);     \
    LOAD_NAME(loaded, aligned);                                                \
    ok(!memcmp(&loaded, &value, sizeof(value)), "aligned\t\t" #LOAD_NAME);     \
    STORE_NAME(not_aligned, value);                                            \
    ok(!memcmp(not_aligned, bytes, sizeof(bytes)),                             \
       "not aligned\t" #STORE_NAME);                                           \
    LOAD_NAME(loaded, not_aligned);                                            \
    ok(!memcmp(&loaded, &value, sizeof(value)), "not aligned\t" #LOAD_NAME);   \
  }

  TEST_FLOAT(float4store, float4get, float, 1.0f, ARRAY_4(0, 0, 0x80, 0x3f));
  TEST_FLOAT(float8store, float8get, double, 1.0,
             ARRAY_8(0, 0, 0, 0, 0, 0, 0xf0, 0x3f));
  TEST_FLOAT(float8store, float8get, double, -0.0,
             ARRAY_8(0, 0, 0, 0, 0, 0, 0, 0x80));
  TEST_FLOAT(mi_float4store, mi_float4get, float, 1.0f,
             ARRAY_4(0x3f, 0x80, 0, 0));
  TEST_FLOAT(mi_float4store, mi_float4get, float, -2.0f,
             ARRAY_4(0xc0, 0, 0, 0));
  TEST_FLOAT(mi_float8store, mi_float8get, double, 1.0,
             ARRAY_8(0x3f, 0xf0, 0, 0, 0, 0, 0, 0));
  TEST_FLOAT(mi_float8store, mi_float8get, double, -2.0,
             ARRAY_8(0xc0, 0, 0, 0, 0, 0, 0, 0));

  {
    /* The bits of a NaN must be preserved, too */
    ulonglong bits= 0x7ff8000000000001ULL;
    uchar bytes[]= ARRAY_8(1, 0, 0, 0, 0, 0, 0xf8, 0x7f);
    double nan, loaded;
    memcpy(&nan, &bits, sizeof(nan));
    float8store(aligned, nan);
    ok(!memcmp(aligned, bytes, sizeof(bytes)), "NaN\t\tfloat8store");
    float8get(loaded, aligned);
    ok(!memcmp(&loaded, &nan, sizeof(nan)), "NaN\t\tfloat8get");
  }

  /* Native byte order integers, including sign and zero extension on read */
#define TEST_NATIVE(P, NAME)                                                   \
  {                                                                            \
    int16 s= -2;                                                               \
    uint16 us= 0xfffe;                                                         \
    int32 l= -70000;                                                           \
    int16 s_loaded;                                                            \
    int i_loaded;                                                              \
    int32 l_loaded;                                                            \
    longlong ll_loaded;                                                        \
    shortstore(P, s);                                                          \
    shortget(s_loaded, P);                                                     \
    ok(s_loaded == s, NAME "\tshortstore/shortget");                           \
    shortget(i_loaded, P);                                                     \
    ok(i_loaded == -2, NAME "\tshortget sign extension");                      \
    shortstore(P, us);                                                         \
    ushortget(i_loaded, P);                                                    \
    ok(i_loaded == 0xfffe, NAME "\tushortget zero extension");                 \
    longstore(P, l);                                                           \
    longget(l_loaded, P);                                                      \
    ok(l_loaded == l, NAME "\tlongstore/longget");                             \
    longget(ll_loaded, P);                                                     \
    ok(ll_loaded == -70000, NAME "\tlongget sign extension");                  \
  }

  TEST_NATIVE(aligned, "aligned");
  TEST_NATIVE(not_aligned, "not aligned");

#undef TEST_NATIVE
#undef TEST_FLOAT
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
  plan(108);
  test_byte_order();
  return exit_status();
}
