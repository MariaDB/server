/*
   Copyright (c) 2017, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */

#include <tap.h>
#include <m_string.h>
#include "common.h"

int main()
{
  uchar sk[CRYPTO_SECRETKEYBYTES], pk[CRYPTO_PUBLICKEYBYTES];
  uchar foobar_sk[CRYPTO_SECRETKEYBYTES]= {195, 171, 143, 241, 55, 32, 232,
    173, 144, 71, 221, 57, 70, 107, 60, 137, 116, 229, 146, 194, 250, 56, 61,
    74, 57, 96, 113, 76, 174, 240, 196, 242, 46, 6, 101, 50, 204, 79, 15, 14,
    186, 168, 176, 159, 25, 100, 110, 224, 133, 74, 171, 60, 128, 170, 80, 53,
    105, 116, 153, 109, 172, 121, 153, 161};
  uchar foobar_sign[CRYPTO_BYTES]= {164, 116, 168, 41, 250, 169, 91, 205, 126,
    71, 253, 70, 233, 228, 79, 70, 43, 157, 221, 169, 35, 130, 101, 62, 133,
    50, 104, 50, 45, 168, 238, 198, 48, 243, 76, 167, 173, 56, 241, 81, 221,
    197, 31, 60, 247, 225, 52, 158, 31, 82, 20, 6, 237, 68, 54, 32, 78, 244,
    91, 49, 194, 238, 117, 5 };

  uchar nonce[NONCE_BYTES];
  uchar reply[NONCE_BYTES+CRYPTO_BYTES];
  unsigned long long reply_len, scramble_len;
  int r;

  plan(6);
  pw_to_sk_and_pk(STRING_WITH_LEN("foobar"), sk, pk);
  ok(!memcmp(sk, foobar_sk, CRYPTO_SECRETKEYBYTES), "foobar sk");

  memset(nonce, 'A', sizeof(nonce));
  crypto_sign(reply, &reply_len, nonce, sizeof(nonce), sk);
  ok(reply_len == sizeof(reply), "reply_len");
  ok(!memcmp(reply, foobar_sign, CRYPTO_BYTES), "foobar sign");

  r= crypto_sign_open(nonce, &scramble_len, reply, reply_len, pk);
  ok(scramble_len == sizeof(nonce), "scramble_len");
  ok(!r, "good nonce");

  reply[CRYPTO_BYTES + 10]='B';
  r= crypto_sign_open(nonce, &scramble_len, reply, reply_len, pk);
  ok(r, "bad nonce");

  return exit_status();
}
