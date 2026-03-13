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

#if _WIN32
#include <io.h>
#define access _access
#define F_OK 0
#else
#include <unistd.h>
#endif
#include "mysql_sha2.h"
#include <mysql/plugin_auth.h>
#include <mysqld_error.h>

char *private_key_path, *public_key_path, public_key[1024]={0};
size_t public_key_len=0;
EVP_PKEY *private_key= 0;

static my_bool auto_generate_keys;
static unsigned int digest_rounds;

struct digest {
  unsigned int iterations;
  unsigned char salt[SCRAMBLE_LENGTH];
  unsigned char crypted[SHA256CRYPT_LEN];
};

#define PASSWORD_LEN (SCRAMBLE_LENGTH + SHA256CRYPT_LEN + sizeof("$A$005$")-1)

#define ITERATION_MULTIPLIER 1000

static unsigned char request_public_key = '\2';
static unsigned char perform_full_authentication = '\4';

static void make_salt(unsigned char *to)
{
  unsigned char *end= to + SCRAMBLE_LENGTH;
  my_random_bytes(to, SCRAMBLE_LENGTH);
  for (; to < end; to++)
    *to = (*to % 90) + '$' + 1;
  /* in MySQL: if (*to == '\0' || *to == '$') (*to)++; */
}

static int auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  struct digest *authstr;
  unsigned char to[SHA256CRYPT_LEN];
  unsigned char scramble[SCRAMBLE_LENGTH + 1], *pkt;
  int pkt_len;
  MYSQL_PLUGIN_VIO_INFO vio_info;
  unsigned char plain_text[1025];
  size_t plain_text_len= sizeof(plain_text)-1;

  make_salt(scramble);
  scramble[SCRAMBLE_LENGTH]= 0;
  if (vio->write_packet(vio, scramble, sizeof(scramble)))
    return CR_ERROR;

  if ((pkt_len= vio->read_packet(vio, &pkt)) < 0)
    return CR_ERROR;

  if (!pkt_len || (pkt_len == 1 && *pkt == 0)) /* sic! */
  {
    if (info->auth_string_length == 0)
      return CR_OK;
    return CR_AUTH_USER_CREDENTIALS;
  }
  info->password_used= PASSWORD_USED_YES;

  if (info->auth_string_length == 0)
    return CR_AUTH_USER_CREDENTIALS;

  if (pkt_len != SHA256_DIGEST_LENGTH)
    return CR_ERROR;

  /*
    TODO support caching: user@host -> plaintext password
    but for now - request full auth unconditionally
  */

  if (vio->write_packet(vio, &perform_full_authentication, 1))
    return CR_ERROR;
  
  if ((pkt_len= vio->read_packet(vio, &pkt)) <= 0)
    return CR_ERROR;

  vio->info(vio, &vio_info);
  /* secure transport, as in MySQL. SSL is "secure" even if not verified */
  if (vio_info.protocol ==  MYSQL_VIO_TCP && !vio_info.tls)
  {
    if (!private_key || !public_key_len)
    {
      my_printf_error(1, SELF ": Authentication requires either RSA keys "
                      "or secure transport", ME_ERROR_LOG_ONLY);
      return CR_AUTH_PLUGIN_ERROR;
    }

    if (pkt_len == 1 && *pkt == request_public_key)
    {
      if (vio->write_packet(vio, (unsigned char *)public_key,
                            (int)public_key_len))
        return CR_ERROR;
      if ((pkt_len= vio->read_packet(vio, &pkt)) <= 0)
        return CR_ERROR;
    }

    if (ssl_decrypt(private_key, pkt, pkt_len, plain_text, &plain_text_len))
      return CR_ERROR;

    for (size_t i=0; i < plain_text_len; i++)
      plain_text[i]^= scramble[i % SCRAMBLE_LENGTH];
    pkt= plain_text;
    pkt_len= (int)plain_text_len;
  }
  /* now pkt contains plaintext password */

  authstr= (struct digest*)info->auth_string;
  sha256_crypt_r(pkt, pkt_len-1, authstr->salt, sizeof(authstr->salt),
                 to, authstr->iterations);

  if (memcmp(to, authstr->crypted, SHA256CRYPT_LEN))
    return CR_AUTH_USER_CREDENTIALS;
  return CR_OK;
}

static int password_hash(const char *password, size_t password_length,
                         char *hash, size_t *hash_length)
{
  struct digest authstr;

  if (*hash_length < PASSWORD_LEN)
    return 1;

  if (!password_length)
    return (int)(*hash_length= 0);

  make_salt(authstr.salt);
  sha256_crypt_r((unsigned char*)password, password_length,
                 authstr.salt, sizeof(authstr.salt),
                 authstr.crypted, digest_rounds);
  *hash_length= my_snprintf(hash, *hash_length, "$A$%03X$%.20s%.43s",
                            digest_rounds/ITERATION_MULTIPLIER,
                            authstr.salt, authstr.crypted);
  assert(*hash_length == PASSWORD_LEN);
  return 0;
}

static int digest_to_binary(const char *hash, size_t hash_length,
                            unsigned char *out, size_t *out_length)
{
  struct digest *authstr= (struct digest*)out;
  assert(*out_length > sizeof(*authstr));
  *out_length= sizeof(*authstr);
  memset(out, 0, *out_length);
  if (hash_length != PASSWORD_LEN)
  {
    my_printf_error(ER_PASSWD_LENGTH, "Password hash should be "
                    "%zu characters long", 0, PASSWORD_LEN);
    return 1;
  }
  if (sscanf(hash, "$A$%X$%20c%43c", &authstr->iterations, authstr->salt,
             authstr->crypted) < 3)
  {
    my_printf_error(ER_PASSWD_LENGTH, "Invalid password hash", 0);
    return 1;
  }
  authstr->iterations*= ITERATION_MULTIPLIER;
  return 0;
}

static struct st_mysql_auth info=
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  SELF, auth, password_hash, digest_to_binary
};

static MYSQL_SYSVAR_STR(private_key_path, private_key_path, PLUGIN_VAR_READONLY,
    "A path to the private RSA key used for authentication",
    NULL, NULL, "private_key.pem");

static MYSQL_SYSVAR_STR(public_key_path, public_key_path, PLUGIN_VAR_READONLY,
    "A path to the public RSA key used for authentication",
    NULL, NULL, "public_key.pem");

static MYSQL_SYSVAR_BOOL(auto_generate_rsa_keys, auto_generate_keys,
    PLUGIN_VAR_READONLY | PLUGIN_VAR_OPCMDARG,
    "Auto generate RSA keys at server startup if key paths "
    "are not explicitly set and key files are not present "
    "at their default locations", NULL, NULL, 1);

static MYSQL_SYSVAR_UINT(digest_rounds, digest_rounds, PLUGIN_VAR_READONLY,
  "Number of SHA2 rounds to be performed when computing a password hash",
  NULL, NULL, 5000, 5000, 0xfff * ITERATION_MULTIPLIER, 1);

static int init_keys(void *p)
{
  if (private_key_path == MYSQL_SYSVAR_NAME(private_key_path).def_val &&
      public_key_path == MYSQL_SYSVAR_NAME(public_key_path).def_val &&
      access(private_key_path, F_OK) && access(public_key_path, F_OK) &&
      auto_generate_keys)
    ssl_genkeys();
  ssl_loadkeys();
  return 0;
}

static int free_keys(void *p)
{
  EVP_PKEY_free(private_key);
  return 0;
}

static struct st_mysql_sys_var *sysvars[]=
{
  MYSQL_SYSVAR(private_key_path),
  MYSQL_SYSVAR(public_key_path),
  MYSQL_SYSVAR(auto_generate_rsa_keys),
  MYSQL_SYSVAR(digest_rounds),
  NULL
};

static struct st_mysql_show_var status_variables[]=
{
  {"rsa_public_key", public_key, SHOW_CHAR},
  {NULL, NULL, 0}
};

maria_declare_plugin(auth_mysql_sha2)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &info,
  SELF,
  "Oracle Corporation, Sergei Golubchik",
  "MySQL-compatible SHA2 authentication",
  PLUGIN_LICENSE_GPL,
  init_keys,
  free_keys,
  0x0100,
  status_variables,
  sysvars,
  "1.0",
  MariaDB_PLUGIN_MATURITY_GAMMA
}
maria_declare_plugin_end;
