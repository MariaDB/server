/* Copyright (c) MariaDB 2020, 2024

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

/*
  The following lookup table oriented computation of CRC-32
  is based on the Public Domain / Creative Commons CC0 Perl code from
  http://billauer.co.il/blog/2011/05/perl-crc32-crc-xs-module/
*/

/** Lookup tables */
static uint32 tab_3309[256], tab_castagnoli[256];

/** Initialize a lookup table for a CRC-32 polynomial */
static void init_lookup(uint32 *tab, uint32 polynomial)
{
  unsigned i;
  for (i= 0; i < 256; i++)
  {
    uint32 x= i;
    unsigned j;
    for (j= 0; j < 8; j++)
      if (x & 1)
        x= (x >> 1) ^ polynomial;
      else
        x>>= 1;
    tab[i]= x;
  }
}

/** Compute a CRC-32 one octet at a time based on a lookup table */
static uint crc_(uint32 crc, const void *buf, size_t len, const uint32 *tab)
{
  const unsigned char *b= buf;
  const unsigned char *const end = b + len;
  crc^= 0xffffffff;
  while (b != end)
    crc= ((crc >> 8) & 0xffffff) ^ tab[(crc ^ *b++) & 0xff];
  crc^= 0xffffffff;
  return crc;
}

static uint crc32(uint32 crc, const void *buf, size_t len)
{ return crc_(crc, buf, len, tab_3309); }
static uint crc32c(uint32 crc, const void *buf, size_t len)
{ return crc_(crc, buf, len, tab_castagnoli); }

static char buf[16384];

typedef uint (*check)(uint32, const void*, size_t);

static size_t test_buf(check c1, check c2)
{
  size_t s;
  for (s= sizeof buf; s; s--)
    if (c1(0, buf, s) != c2(0, buf, s))
      break;
  return s;
}

#define DO_TEST_CRC32(crc,str,len)                      \
  ok(crc32(crc,str,len) == my_checksum(crc, str, len),  \
     "crc32(%u,'%.*s')", crc, (int) len, str)

/* Check that CRC-32C calculation returns correct result*/
#define DO_TEST_CRC32C(crc,str,len)                     \
  ok(crc32c(crc,str,len) == my_crc32c(crc, str, len),   \
     "crc32c(%u,'%.*s')", crc, (int) len, str)

static const char STR[]=
  "123456789012345678900212345678901231213123321212123123123123123"
  "..........................................................................."
  "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
  "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyyy"
  "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzzz";

int main(int argc __attribute__((unused)),char *argv[])
{
  MY_INIT(argv[0]);
  init_lookup(tab_3309, 0xedb88320);
  init_lookup(tab_castagnoli, 0x82f63b78);

  plan(36);
  printf("%s\n",my_crc32c_implementation());
  DO_TEST_CRC32(0,STR,0);
  DO_TEST_CRC32(1,STR,0);
  DO_TEST_CRC32(0,STR,3);
  DO_TEST_CRC32(0,STR,5);
  DO_TEST_CRC32(1,STR,5);
  DO_TEST_CRC32(0,STR,15);
  DO_TEST_CRC32(0,STR,16);
  DO_TEST_CRC32(0,STR,19);
  DO_TEST_CRC32(0,STR,32);
  DO_TEST_CRC32(0,STR,63);
  DO_TEST_CRC32(0,STR,64);
  DO_TEST_CRC32(0,STR,65);
  DO_TEST_CRC32(0,STR,255);
  DO_TEST_CRC32(0,STR,256);
  DO_TEST_CRC32(0,STR,257);
  DO_TEST_CRC32(0,STR,(sizeof(STR)-1));
  ok(0 == my_checksum(0, NULL, 0) , "crc32 data = NULL, length = 0");

  DO_TEST_CRC32C(0,STR,0);
  DO_TEST_CRC32C(1,STR,0);
  DO_TEST_CRC32C(0,STR,3);
  DO_TEST_CRC32C(0,STR,5);
  DO_TEST_CRC32C(1,STR,5);
  DO_TEST_CRC32C(0,STR,15);
  DO_TEST_CRC32C(0,STR,16);
  DO_TEST_CRC32C(0,STR,19);
  DO_TEST_CRC32C(0,STR,32);
  DO_TEST_CRC32C(0,STR,63);
  DO_TEST_CRC32C(0,STR,64);
  DO_TEST_CRC32C(0,STR,65);
  DO_TEST_CRC32C(0,STR,255);
  DO_TEST_CRC32C(0,STR,256);
  DO_TEST_CRC32C(0,STR,257);
  DO_TEST_CRC32C(0,STR,(sizeof(STR)-1));
  ok(0 == my_crc32c(0, NULL, 0), "crc32c data = NULL, length = 0");

  for (size_t i = 0; i < sizeof buf; i++)
    buf[i] = i % 251;
  ok(0 == test_buf(my_checksum, crc32), "crc32 with various lengths");
  ok(0 == test_buf(my_crc32c, crc32c), "crc32c with various lengths");

  my_end(0);
  return exit_status();
}
