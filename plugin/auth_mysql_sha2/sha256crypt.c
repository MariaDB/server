/*
  Copyright (c) 2025, MariaDB plc

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

#include <my_alloca.h>
#include "mysql_sha2.h"

/* based on https://www.akkadia.org/drepper/SHA-crypt.txt */

/* SHA256-based Unix crypt implementation.
   Released into the Public Domain by Ulrich Drepper <drepper@redhat.com>.  */

void sha256_crypt_r(const unsigned char *key, size_t key_len,
                    const unsigned char *salt, size_t salt_len,
                    unsigned char *buffer, size_t rounds)
{
  unsigned char tmp[SHA256_DIGEST_LENGTH];
  unsigned char alt[SHA256_DIGEST_LENGTH];
  size_t cnt;
  static const char b64t[64]
#if defined __GNUC__ && __GNUC__ > 7
    __attribute__((nonstring))
#endif
    = "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
  void *ctx = alloca(my_sha256_context_size());
  unsigned char *p_bytes = alloca(key_len);
  unsigned char *s_bytes = alloca(salt_len);

  my_sha256_multi(alt, key, key_len, salt, salt_len, key, key_len, NULL);

  my_sha256_init(ctx);
  my_sha256_input(ctx, key, key_len);
  my_sha256_input(ctx, salt, salt_len);
  /* Add for every byte in the key one byte of the alternate sum.  */
  for (cnt = key_len; cnt > sizeof(alt); cnt -= sizeof(alt))
    my_sha256_input(ctx, alt, sizeof(alt));
  my_sha256_input(ctx, alt, cnt);
  /* Take the binary representation of the length of the key and for every
     1 add the alternate sum, for every 0 the key.  */
  for (cnt = key_len; cnt > 0; cnt >>= 1)
    if ((cnt & 1) != 0)
      my_sha256_input(ctx, alt, sizeof(alt));
    else
      my_sha256_input(ctx, key, key_len);
  my_sha256_result(ctx, alt);

  /* Start computing S byte sequence.  */
  my_sha256_init(ctx);
  for (cnt = 0; cnt < 16u + alt[0]; ++cnt)
    my_sha256_input(ctx, salt, salt_len);
  my_sha256_result(ctx, tmp);

  /* Create byte sequence S.  */
  for (cnt = salt_len; cnt >= sizeof(tmp); cnt -= sizeof(tmp))
    memcpy(s_bytes + salt_len - cnt, tmp, sizeof(tmp));
  memcpy(s_bytes + salt_len - cnt, tmp, cnt);

  /* Start computing P byte sequence.  */
  my_sha256_init(ctx);
  for (cnt = 0; cnt < key_len; ++cnt)
    my_sha256_input(ctx, key, key_len);
  my_sha256_result(ctx, tmp);

  /* Create byte sequence P.  */
  for (cnt = key_len; cnt >= sizeof(tmp); cnt -= sizeof(tmp))
    memcpy(p_bytes + key_len - cnt, tmp, sizeof(tmp));
  memcpy(p_bytes + key_len - cnt, tmp, cnt);

  /* Repeatedly run the collected hash value through SHA256 to burn
     CPU cycles.  */
  for (cnt = 0; cnt < rounds; ++cnt)
  {
    my_sha256_init(ctx);
    if ((cnt & 1) != 0)
      my_sha256_input(ctx, p_bytes, key_len);
    else
      my_sha256_input(ctx, cnt ? tmp : alt, sizeof(tmp));
    if (cnt % 3 != 0)
      my_sha256_input(ctx, s_bytes, salt_len);
    if (cnt % 7 != 0)
      my_sha256_input(ctx, p_bytes, key_len);
    if ((cnt & 1) != 0)
      my_sha256_input(ctx, tmp, sizeof(tmp));
    else
      my_sha256_input(ctx, p_bytes, key_len);
    my_sha256_result(ctx, tmp);
  }

#define b64_from_24bit(B2, B1, B0, N)                     \
  do {                                                    \
    unsigned int w = ((B2) << 16) | ((B1) << 8) | (B0);   \
    int n = (N);                                          \
    while (n-- > 0)                                       \
    {                                                     \
      *buffer++ = b64t[w & 0x3f];                         \
      w >>= 6;                                            \
    }                                                     \
  } while (0)

  b64_from_24bit (tmp[0], tmp[10], tmp[20], 4);
  b64_from_24bit (tmp[21], tmp[1], tmp[11], 4);
  b64_from_24bit (tmp[12], tmp[22], tmp[2], 4);
  b64_from_24bit (tmp[3], tmp[13], tmp[23], 4);
  b64_from_24bit (tmp[24], tmp[4], tmp[14], 4);
  b64_from_24bit (tmp[15], tmp[25], tmp[5], 4);
  b64_from_24bit (tmp[6], tmp[16], tmp[26], 4);
  b64_from_24bit (tmp[27], tmp[7], tmp[17], 4);
  b64_from_24bit (tmp[18], tmp[28], tmp[8], 4);
  b64_from_24bit (tmp[9], tmp[19], tmp[29], 4);
  b64_from_24bit (0, tmp[31], tmp[30], 3);      /* == 43 bytes in total */
}
