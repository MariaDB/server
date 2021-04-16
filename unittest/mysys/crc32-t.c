/* Copyright (c) MariaDB 2020

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License as
   published by the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful, but
   WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include <my_global.h>
#include <my_sys.h>
#include <my_crypt.h>
#include <tap.h>
#include <string.h>
#include <ctype.h>
#include <zlib.h>

/*
  Check that optimized crc32 (ieee, or ethernet polynomical) returns the same
  result as zlib (not so well optimized, yet, but trustworthy)
*/
#define DO_TEST_CRC32(crc,str)  \
    ok(crc32(crc,(const Bytef *)str,(uint)(sizeof(str)-1)) == my_checksum(crc, str, sizeof(str)-1), "crc32 '%s'",str)

/* Check that CRC32-C calculation returns correct result*/
#define DO_TEST_CRC32C(crc,str,expected) \
    do { \
     unsigned int v = my_crc32c(crc, str, sizeof(str)-1); \
     printf("crc32(%u,'%s',%zu)=%u\n",crc,str,sizeof(str)-1,v); \
     ok(expected == my_crc32c(crc, str, sizeof(str)-1),"crc32c '%s'",str); \
    }while(0)


#define LONG_STR "1234567890234568900212345678901231213123321212123123123123123"\
 "............................................................................." \
 "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx" \
 "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy" \
 "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz"

int main(int argc __attribute__((unused)),char *argv[])
{
  MY_INIT(argv[0]);
  plan(14);
  printf("%s\n",my_crc32c_implementation());
  DO_TEST_CRC32(0,"");
  DO_TEST_CRC32(1,"");
  DO_TEST_CRC32(0,"12345");
  DO_TEST_CRC32(1,"12345");
  DO_TEST_CRC32(0,"1234567890123456789");
  DO_TEST_CRC32(0, LONG_STR);
  ok(0 == my_checksum(0, NULL, 0) , "crc32 data = NULL, length = 0");

  DO_TEST_CRC32C(0,"", 0);
  DO_TEST_CRC32C(1,"", 1);
  DO_TEST_CRC32C(0, "12345", 416359221);
  DO_TEST_CRC32C(1, "12345", 549473433);
  DO_TEST_CRC32C(0, "1234567890123456789", 2366987449U);
  DO_TEST_CRC32C(0, LONG_STR, 3009234172U);
  ok(0 == my_crc32c(0, NULL, 0), "crc32c data = NULL, length = 0");

  my_end(0);
  return exit_status();
}
