/*
  Copyright (c) 2024, MariaDB plc

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

#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
#ifdef HAVE_WOLFSSL
#include <openssl/ed25519.h>
#include <wolfcrypt/ed25519.h>
#endif

#include <mysql/plugin_auth.h>
#include <mysql/plugin.h>
#include <mysqld_error.h>
#include "scope.h"

#include <cstring>

typedef unsigned char uchar;
constexpr size_t CHALLENGE_SCRAMBLE_LENGTH= 32;
constexpr size_t CHALLENGE_SALT_LENGTH= 18;
constexpr size_t ED25519_SIG_LENGTH= 64;
constexpr size_t ED25519_KEY_LENGTH= 32;
constexpr size_t PBKDF2_HASH_LENGTH= ED25519_KEY_LENGTH;
constexpr size_t CLIENT_RESPONSE_LENGTH= CHALLENGE_SCRAMBLE_LENGTH
                                         + ED25519_SIG_LENGTH;

constexpr size_t base64_length(size_t input_length)
{
  return ((input_length + 2) / 3) * 4; // with padding
}

constexpr size_t base64_length_raw(size_t input_length)
{
  return (input_length * 4 + 2) / 3; // no padding
}

struct alignas(1) Client_signed_response
{
  union {
    struct {
      uchar client_scramble[CHALLENGE_SCRAMBLE_LENGTH];
      uchar signature[ED25519_SIG_LENGTH];
    };
    uchar start[1];
  };
};

static_assert(sizeof(Client_signed_response) == CLIENT_RESPONSE_LENGTH,
              "Client_signed_response is not aligned.");

struct alignas(1) Passwd_as_stored
{
  char algorithm;
  uchar iterations;
  char colon;
  char salt[base64_length_raw(CHALLENGE_SALT_LENGTH)];
  char colon2;
  char pub_key[base64_length_raw(ED25519_KEY_LENGTH)];
};

struct alignas(1) Passwd_in_memory
{
  char algorithm;
  uchar iterations;
  uchar salt[CHALLENGE_SALT_LENGTH];
  uchar pub_key[ED25519_KEY_LENGTH];
};

int print_ssl_error()
{
  char buf[512];
  unsigned long err= ERR_get_error();
  ERR_error_string_n(err, buf, sizeof buf);
  my_printf_error(err, "parsec: %s", ME_ERROR_LOG_ONLY, buf);
  return 1;
}

static
int compute_derived_key(const char* password, size_t pass_len,
                         const Passwd_in_memory *params, uchar *derived_key)
{
  assert(params->algorithm == 'P');
  int ret = PKCS5_PBKDF2_HMAC(password, (int)pass_len, params->salt,
                              sizeof(params->salt), 1024 << params->iterations,
                              EVP_sha512(), PBKDF2_HASH_LENGTH, derived_key);
  if(ret == 0)
    return print_ssl_error();

  return 0;
}


static
int verify_ed25519(const uchar *public_key, const uchar *signature,
                    const uchar *message, size_t message_len)
{
#ifdef HAVE_WOLFSSL
  int res= wolfSSL_ED25519_verify(message, (unsigned)message_len,
                                  public_key, ED25519_KEY_LENGTH,
                                  signature, ED25519_SIG_LENGTH);
  return res != WOLFSSL_SUCCESS;
#else
  int ret= 1;
  EVP_MD_CTX *mdctx= EVP_MD_CTX_new();
  EVP_PKEY *pkey= EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, NULL,
                                              public_key, 32);

  if (pkey && mdctx && EVP_DigestVerifyInit(mdctx, NULL, NULL, NULL, pkey))
    ret= !EVP_DigestVerify(mdctx, signature, ED25519_SIG_LENGTH,
                           message, message_len);
  EVP_MD_CTX_free(mdctx);
  EVP_PKEY_free(pkey);
  return ret;
#endif
}

static
int ed25519_derive_public_key(const uchar *raw_private_key, uchar *pub_key)
{
#ifdef HAVE_WOLFSSL
  ed25519_key key;

  int ret = wc_ed25519_init(&key);
  if (ret != 0)
    return print_ssl_error();

  SCOPE_EXIT([&key](){ wc_ed25519_free(&key); });

  ret = wc_ed25519_import_private_only(raw_private_key, ED25519_KEY_LENGTH,
                                       &key);
  if (ret != 0)
    return print_ssl_error();

  ret = wc_ed25519_make_public(&key, pub_key, ED25519_KEY_LENGTH);
  if (ret != 0)
    return print_ssl_error();

  return false;
#else
  EVP_PKEY *pkey = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, NULL,
                                                raw_private_key,
                                                ED25519_KEY_LENGTH);
  bool res= pkey != NULL;
  size_t len= ED25519_KEY_LENGTH;
  if (pkey)
    res= EVP_PKEY_get_raw_public_key(pkey, pub_key, &len); // 1 == success

  if (!res)
    print_ssl_error();

  EVP_PKEY_free(pkey);
  return !res;
#endif
}

static
int hash_password(const char *password, size_t password_length,
                  char *hash, size_t *hash_length)
{
  auto stored= (Passwd_as_stored*)hash;
  assert(*hash_length >= sizeof(*stored) + 2); // it should fit the buffer

  Passwd_in_memory memory;
  memory.algorithm= 'P';
  memory.iterations= 0;
  my_random_bytes(memory.salt, sizeof(memory.salt));

  uchar derived_key[PBKDF2_HASH_LENGTH];
  if (compute_derived_key(password, password_length, &memory, derived_key))
    return 1;

  if (ed25519_derive_public_key(derived_key, memory.pub_key))
    return 1;

  stored->algorithm= memory.algorithm;
  stored->iterations= memory.iterations + '0';
  my_base64_encode(memory.salt, sizeof(memory.salt), stored->salt);
  my_base64_encode(memory.pub_key, sizeof(memory.pub_key), stored->pub_key);
  stored->colon= stored->colon2= ':';

  *hash_length = sizeof *stored;
  hash[*hash_length]= 0; // safety

  return 0;
}

static
int digest_to_binary(const char *hash, size_t hash_length,
                    unsigned char *out, size_t *out_length)
{
  auto stored= (Passwd_as_stored*)hash;
  auto memory= (Passwd_in_memory*)out;

  if (hash_length != sizeof (*stored) || *out_length < sizeof(*memory) ||
      stored->algorithm != 'P' ||
      stored->iterations < '0' || stored->iterations > '3' ||
      stored->colon != ':' || stored->colon2 != ':')
  {
    my_printf_error(ER_PASSWD_LENGTH, "Wrong ext-salt format", 0);
    return 1;
  }

  *out_length = sizeof(*memory);
  memory->algorithm= stored->algorithm;
  memory->iterations= stored->iterations - '0';

  static_assert(base64_length(CHALLENGE_SALT_LENGTH) == base64_length_raw(CHALLENGE_SALT_LENGTH),
                "Salt is base64-aligned");
  if (my_base64_decode(stored->salt, base64_length(CHALLENGE_SALT_LENGTH),
                       memory->salt, NULL, 0) < 0)
  {
    my_printf_error(ER_PASSWD_LENGTH,
                    "Password salt should be base64 encoded", 0);
    return 1;
  }

  char buf[base64_length(ED25519_KEY_LENGTH)+1];
  constexpr int pad= (int)base64_length(ED25519_KEY_LENGTH)
                     - (int)base64_length_raw(ED25519_KEY_LENGTH);
  static_assert(pad > 0, "base64 length calculation check");
  memcpy(buf, stored->pub_key, base64_length_raw(ED25519_KEY_LENGTH));
  memset(buf + base64_length_raw(ED25519_KEY_LENGTH), '=', pad);
  if (my_base64_decode(buf, base64_length(ED25519_KEY_LENGTH),
                       memory->pub_key, NULL, 0) < 0)
  {
    my_printf_error(ER_PASSWD_LENGTH,
                    "Password-derived key should be base64 encoded", 0);
    return 1;
  }

  return 0;
}

static
int auth(MYSQL_PLUGIN_VIO *vio, MYSQL_SERVER_AUTH_INFO *info)
{
  union
  {
    struct
    {
      uchar server[CHALLENGE_SCRAMBLE_LENGTH];
      uchar client[CHALLENGE_SCRAMBLE_LENGTH];
    };
    uchar start[1];
  } scramble_pair;

  my_random_bytes(scramble_pair.server, CHALLENGE_SCRAMBLE_LENGTH);

  if (vio->write_packet(vio, scramble_pair.server, sizeof(scramble_pair.server)))
    return CR_ERROR;

  // Begin with reading the handshake packet. It should be empty (for now).
  uchar *dummy;
  int bytes_read= vio->read_packet(vio, &dummy);
  if (bytes_read != 0)
    return CR_ERROR;

  auto passwd= (Passwd_in_memory*)info->auth_string;

  if (vio->write_packet(vio, (uchar*)info->auth_string, 2 + CHALLENGE_SALT_LENGTH))
    return CR_ERROR;

  Client_signed_response *client_response;
  bytes_read= vio->read_packet(vio, (uchar**)&client_response);
  if (bytes_read < 0)
    return CR_ERROR;
  if (bytes_read != sizeof *client_response)
    return CR_AUTH_HANDSHAKE;

  memcpy(scramble_pair.client, client_response->client_scramble,
         CHALLENGE_SCRAMBLE_LENGTH);

  if (verify_ed25519(passwd->pub_key, client_response->signature,
                     scramble_pair.start, sizeof(scramble_pair)))
    return CR_AUTH_HANDSHAKE;

  return CR_OK;
}

static struct st_mysql_auth info =
{
  MYSQL_AUTHENTICATION_INTERFACE_VERSION,
  "parsec",
  auth,
  hash_password,
  digest_to_binary
};


maria_declare_plugin(auth_parsec)
{
  MYSQL_AUTHENTICATION_PLUGIN,
  &info,
  "parsec",
  "Nikita Maliavin",
  "Password Authentication using Response Signed with Elliptic Curve",
  PLUGIN_LICENSE_GPL,
  NULL,
  NULL,
  0x0100,
  NULL,
  NULL,
  "1.0",
  MariaDB_PLUGIN_MATURITY_GAMMA
}
maria_declare_plugin_end;
