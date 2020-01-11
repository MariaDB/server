/* Copyright (c) 2003, 2006, 2007 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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

#define DO_TEST(mode, nopad, slen, fill, dlen, hash)                    \
  SKIP_BLOCK_IF(mode == 0xDEADBEAF, nopad ? 4 : 5, #mode " not supported")     \
  {                                                                     \
    memset(src, fill, src_len= slen);                                   \
    ok(my_aes_crypt(mode, nopad | ENCRYPTION_FLAG_ENCRYPT,              \
                    src, src_len, dst, &dst_len,                        \
                    key, sizeof(key), iv, sizeof(iv)) == MY_AES_OK,     \
      "encrypt " #mode " %u %s", src_len, nopad ? "nopad" : "pad");     \
    if (!nopad)                                                         \
      ok (dst_len == my_aes_get_size(mode, src_len), "my_aes_get_size");\
    my_md5(md5, (char*)dst, dst_len);                                   \
    ok(dst_len == dlen && memcmp(md5, hash, sizeof(md5)) == 0, "md5");  \
    ok(my_aes_crypt(mode, nopad | ENCRYPTION_FLAG_DECRYPT,              \
                    dst, dst_len, ddst, &ddst_len,                      \
                    key, sizeof(key), iv, sizeof(iv)) == MY_AES_OK,     \
       "decrypt " #mode " %u", dst_len);                                \
    ok(ddst_len == src_len && memcmp(src, ddst, src_len) == 0, "memcmp"); \
  }

#define DO_TEST_P(M,S,F,D,H) DO_TEST(M,0,S,F,D,H)
#define DO_TEST_N(M,S,F,D,H) DO_TEST(M,ENCRYPTION_FLAG_NOPAD,S,F,D,H)

/* useful macro for debugging */
#define PRINT_MD5()                                     \
  do {                                                  \
    uint i;                                             \
    printf("\"");                                       \
    for (i=0; i < sizeof(md5); i++)                     \
      printf("\\x%02x", md5[i]);                        \
    printf("\"\n");                                     \
  } while(0);

#ifndef HAVE_EncryptAes128Ctr
const uint MY_AES_CTR=0xDEADBEAF;
#endif
#ifndef HAVE_EncryptAes128Gcm
const uint MY_AES_GCM=0xDEADBEAF;
#endif

int
main(int argc __attribute__((unused)),char *argv[])
{
  uchar key[16]= {1,2,3,4,5,6,7,8,9,0,1,2,3,4,5,6};
  uchar iv[16]=  {2,3,4,5,6,7,8,9,0,1,2,3,4,5,6,7};
  uchar src[1000], dst[1100], ddst[1000];
  uchar md5[MY_MD5_HASH_SIZE];
  uint src_len, dst_len, ddst_len;

  MY_INIT(argv[0]);

  plan(87);
  DO_TEST_P(MY_AES_ECB, 200, '.', 208, "\xd8\x73\x8e\x3a\xbc\x66\x99\x13\x7f\x90\x23\x52\xee\x97\x6f\x9a");
  DO_TEST_P(MY_AES_ECB, 128, '?', 144, "\x19\x58\x33\x85\x4c\xaa\x7f\x06\xd1\xb2\xec\xd7\xb7\x6a\xa9\x5b");
  DO_TEST_P(MY_AES_CBC, 159, '%', 160, "\x4b\x03\x18\x3d\xf1\xa7\xcd\xa1\x46\xb3\xc6\x8a\x92\xc0\x0f\xc9");
  DO_TEST_P(MY_AES_CBC, 192, '@', 208, "\x54\xc4\x75\x1d\xff\xe0\xf6\x80\xf0\x85\xbb\x8b\xda\x07\x21\x17");
  DO_TEST_N(MY_AES_ECB, 200, '.', 200, "\xbf\xec\x43\xd1\x66\x8d\x01\xad\x3a\x25\xee\xa6\x3d\xc6\xc4\x68");
  DO_TEST_N(MY_AES_ECB, 128, '?', 128, "\x5b\x44\x20\xf3\xd9\xb4\x9d\x74\x5e\xb7\x5a\x0a\xe7\x32\x35\xc3");
  DO_TEST_N(MY_AES_CBC, 159, '%', 159, "\xf3\x6e\x40\x00\x3c\x08\xa0\xb1\x2d\x1f\xcf\xce\x54\xc9\x73\x83");
  DO_TEST_N(MY_AES_CBC, 192, '@', 192, "\x30\xe5\x28\x8c\x4a\x3b\x02\xd7\x56\x40\x59\x25\xac\x58\x09\x22");
  DO_TEST_P(MY_AES_CTR, 200, '.', 200, "\x5a\x77\x19\xea\x67\x50\xe3\xab\x7f\x39\x6f\xc4\xa8\x09\xc5\x88");
  DO_TEST_P(MY_AES_GCM, 128, '?', 144, "\x54\x6a\x7c\xa2\x04\xdc\x6e\x80\x1c\xcd\x5f\x7a\x7b\x08\x9e\x9d");

  /* test short inputs (less that one block) */
  DO_TEST_P(MY_AES_ECB, 1, '.', 16, "\x6c\xd7\x66\x5b\x1b\x1e\x3a\x04\xfd\xb1\x91\x8d\x0e\xfd\xf1\x86");
  DO_TEST_P(MY_AES_ECB, 2, '?', 16, "\xdb\x84\x9e\xaf\x5f\xcc\xdb\x6b\xf2\x1c\xeb\x53\x75\xa3\x53\x5e");
  DO_TEST_P(MY_AES_CBC, 3, '%', 16, "\x60\x8e\x45\x9a\x07\x39\x63\xce\x02\x19\xdd\x52\xe3\x09\x2a\x66");
  DO_TEST_P(MY_AES_CBC, 4, '@', 16, "\x90\xc2\x6b\xf8\x84\x79\x83\xbd\xc1\x60\x71\x04\x55\x6a\xce\x9e");
  DO_TEST_N(MY_AES_ECB, 5, '.',  5, "\x6b\x60\xdc\xa4\x24\x9b\x02\xbb\x24\x41\x9b\xb0\xd1\x01\xcd\xba");
  DO_TEST_N(MY_AES_ECB, 6, '?',  6, "\x35\x8f\xb7\x9d\xd9\x61\x21\xcf\x25\x66\xd5\x9e\x91\xc1\x42\x7e");
  DO_TEST_N(MY_AES_CBC, 7, '%',  7, "\x94\x5e\x80\x71\x41\x7a\x64\x5d\x6f\x2e\x5b\x66\x9b\x5a\x3d\xda");
  DO_TEST_N(MY_AES_CBC, 8, '@',  8, "\xb8\x53\x97\xb9\x40\xa6\x98\xaf\x0c\x7b\x9a\xac\xad\x7e\x3c\xe0");
  DO_TEST_P(MY_AES_GCM, 9, '?', 25, "\x5e\x05\xfd\xb2\x8e\x17\x04\x1e\xff\x6d\x71\x81\xcd\x85\x8d\xb5");

  my_end(0);
  return exit_status();
}
