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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#include <tap.h>
#include <m_string.h>
#include "common.h"

int main()
{
  uchar pk[CRYPTO_PUBLICKEYBYTES];
  uchar foobar_pk[CRYPTO_PUBLICKEYBYTES]= {170, 253, 166, 27, 161, 214, 10,
    236, 183, 217, 41, 91, 231, 24, 85, 225, 49, 210, 181, 236, 13, 207, 101,
    72, 53, 83, 219, 130, 79, 151, 0, 159};
  uchar foobar_sign[CRYPTO_BYTES]= {232, 61, 201, 63, 67, 63, 51, 53, 86, 73,
    238, 35, 170, 117, 146, 214, 26, 17, 35, 9, 8, 132, 245, 141, 48, 99, 66,
    58, 36, 228, 48, 84, 115, 254, 187, 168, 88, 162, 249, 57, 35, 85, 79, 238,
    167, 106, 68, 117, 56, 135, 171, 47, 20, 14, 133, 79, 15, 229, 124, 160,
    176, 100, 138, 14};

  uchar nonce[NONCE_BYTES];
  uchar reply[NONCE_BYTES+CRYPTO_BYTES];
  int r;

  plan(4);

  crypto_sign_keypair(pk, USTRING_WITH_LEN("foobar"));
  ok(!memcmp(pk, foobar_pk, CRYPTO_PUBLICKEYBYTES), "foobar pk");

  memset(nonce, 'A', sizeof(nonce));
  crypto_sign(reply, nonce, sizeof(nonce), USTRING_WITH_LEN("foobar"));
  ok(!memcmp(reply, foobar_sign, CRYPTO_BYTES), "foobar sign");

  r= crypto_sign_open(reply, sizeof(reply), pk);
  ok(!r, "good nonce");

  crypto_sign(reply, nonce, sizeof(nonce), USTRING_WITH_LEN("foobar"));
  reply[CRYPTO_BYTES + 10]='B';
  r= crypto_sign_open(reply, sizeof(reply), pk);
  ok(r, "bad nonce");

  return exit_status();
}
