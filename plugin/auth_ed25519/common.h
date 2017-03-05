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

#include <mysql.h>
#include <string.h>

#include "ref10/api.h"
#include "crypto_sign.h"
#include "crypto_hash_sha256.h"

#define NONCE_BYTES 32

static inline void pw_to_sk_and_pk(const char *pw, size_t pwlen,
                                   unsigned char *sk, unsigned char *pk)
{
  crypto_hash_sha256(sk, pw, pwlen);
  crypto_sign_keypair(pk, sk);
}

