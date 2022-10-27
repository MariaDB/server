/*
   Copyright (c) 2017, 2021, MariaDB

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

#include <mysql/plugin_auth.h>
#include <mysqld_error.h>
#include "common.h"

#if !defined(__attribute__) && !defined(__GNUC__)
#define __attribute__(A)
#endif

#define PASSWORD_LEN_BUF 44 /* base64 of 32 bytes */
#define PASSWORD_LEN 43     /* we won't store the last byte, padding '=' */

#define CRYPTO_LONGS (CRYPTO_BYTES/sizeof(long))
#define NONCE_LONGS  (NONCE_BYTES/sizeof(long))

/************************** SERVER *************************************/

static int loaded= 0;

static int auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  int pkt_len;
  unsigned long nonce[CRYPTO_LONGS + NONCE_LONGS];
  unsigned char *pkt, *reply= (unsigned char*)nonce;

  info->password_used= PASSWORD_USED_YES;

  /* prepare random nonce */
  if (my_random_bytes((unsigned char *)nonce, (int)sizeof(nonce)))
    return CR_ERROR; // eh? OpenSSL error

  /* send it */
  if (vio->write_packet(vio, reply + CRYPTO_BYTES, NONCE_BYTES))
    return CR_AUTH_HANDSHAKE;

  /* read the signature */
  if ((pkt_len= vio->read_packet(vio, &pkt)) != CRYPTO_BYTES)
    return CR_AUTH_HANDSHAKE;
  memcpy(reply, pkt, CRYPTO_BYTES);

  if (crypto_sign_open(reply, CRYPTO_BYTES + NONCE_BYTES,
                       (unsigned char*)info->auth_string))
    return CR_AUTH_USER_CREDENTIALS; // wrong password provided by the user

  return CR_OK;
}

static int compute_password_digest(const char *pw, size_t pwlen,
                                   char *d, size_t *dlen)
{
  unsigned char pk[CRYPTO_PUBLICKEYBYTES];
  if (*dlen < PASSWORD_LEN || pwlen == 0)
    return 1;
  *dlen= PASSWORD_LEN;
  crypto_sign_keypair(pk, (unsigned char*)pw, pwlen);
  my_base64_encode(pk, CRYPTO_PUBLICKEYBYTES, d);
  return 0;
}

static int digest_to_binary(const char *d, size_t dlen,
                            unsigned char *b, size_t *blen)
{
  char pw[PASSWORD_LEN_BUF];

  if (*blen < CRYPTO_PUBLICKEYBYTES || dlen != PASSWORD_LEN)
  {
    my_printf_error(ER_PASSWD_LENGTH, "Password hash should be %d characters long", 0, PASSWORD_LEN);
    return 1;
  }

  *blen= CRYPTO_PUBLICKEYBYTES;
  memcpy(pw, d, PASSWORD_LEN);
  pw[PASSWORD_LEN]= '=';
  if (my_base64_decode(pw, PASSWORD_LEN_BUF, b, 0, 0) == CRYPTO_PUBLICKEYBYTES)
    return 0;
  my_printf_error(ER_PASSWD_LENGTH, "Password hash should be base64 encoded", 0);
  return 1;
}

static struct st_mysql_auth info =
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  "client_ed25519",
  auth,
  compute_password_digest,
  digest_to_binary
};

static int init(void *p __attribute__((unused)))
{
  loaded= 1;
  return 0;
}

static int deinit(void *p __attribute__((unused)))
{
  loaded= 0;
  return 0;
}

maria_declare_plugin(ed25519)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &info,
  "ed25519",
  "Sergei Golubchik",
  "Elliptic curve ED25519 based authentication",
  PLUGIN_LICENSE_GPL,
  init,
  deinit,
  0x0101,
  NULL,
  NULL,
  "1.1",
  MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;

/************************** UDF ****************************************/
MYSQL_PLUGIN_EXPORT
char *ed25519_password(UDF_INIT *initid __attribute__((unused)),
                       UDF_ARGS *args, char *result, unsigned long *length,
                       char *is_null, char *error __attribute__((unused)))
{
  unsigned char pk[CRYPTO_PUBLICKEYBYTES];

  if ((*is_null= !args->args[0]))
    return NULL;

  *length= PASSWORD_LEN;
  crypto_sign_keypair(pk, (unsigned char*)args->args[0], args->lengths[0]);
  my_base64_encode(pk, CRYPTO_PUBLICKEYBYTES, result);
  return result;
}

/*
  At least one of _init/_deinit is needed unless the server is started
  with --allow_suspicious_udfs.
*/
MYSQL_PLUGIN_EXPORT
my_bool ed25519_password_init(UDF_INIT *initid, UDF_ARGS *args, char *message)
{
  if (args->arg_count != 1 || args->arg_type[0] != STRING_RESULT)
  {
    strcpy(message,"Wrong arguments to ed25519_password()");
    return 1;
  }
  if (!loaded)
  {
    /* cannot work unless the plugin is loaded, we need services. */
    strcpy(message,"Authentication plugin ed25519 is not loaded");
    return 1;
  }
  initid->max_length= PASSWORD_LEN_BUF;
  return 0;
}
