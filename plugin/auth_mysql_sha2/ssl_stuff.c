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

#include "mysql_sha2.h"
#include <errno.h>
#include <openssl/rsa.h>
#include <openssl/pem.h>
#include <openssl/err.h>

/* openssl rsautl -decrypt -inkey private_key.pem -in src -out dst */
int ssl_decrypt(EVP_PKEY *pkey, unsigned char *src, size_t srclen,
                unsigned char *dst, size_t *dstlen)
{
  int res;
  EVP_PKEY_CTX *ctx= EVP_PKEY_CTX_new(pkey, NULL);
  if (!ctx)
    return 1;
  res= EVP_PKEY_decrypt_init(ctx) <= 0 ||
    EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_OAEP_PADDING) <= 0 ||
    EVP_PKEY_decrypt(ctx, dst, dstlen, src, srclen) <= 0;
  EVP_PKEY_CTX_free(ctx);
  return res;
}

#define FILE_ERROR(op,file)                                     \
  do {                                                          \
    my_printf_error(1, SELF ": failed to " op " %s: %iE",       \
                    ME_ERROR_LOG_ONLY, file, errno);            \
    goto err;                                                   \
  } while(0)

#define SSL_ERROR(op,file)                                      \
  do {                                                          \
    unsigned long e= ERR_get_error();                           \
    my_printf_error(1, SELF ": failed to " op " %s: %lu - %s",  \
       ME_ERROR_LOG_ONLY, file, e, ERR_reason_error_string(e)); \
    goto err;                                                   \
  } while(0)

/*
  openssl genrsa -out private_key.pem 2048
  openssl rsa -in private_key.pem -pubout -out public_key.pem
*/
int ssl_genkeys()
{
  EVP_PKEY *pkey;
  BIO *bio= NULL;

  if (!(pkey= EVP_RSA_gen(2048)))
    goto err;

  if (!(bio= BIO_new_file(private_key_path, "w")))
    FILE_ERROR("write", private_key_path);

  if (PEM_write_bio_PrivateKey(bio, pkey, NULL, NULL, 0, NULL, NULL) != 1)
    SSL_ERROR("write", private_key_path);
  BIO_free(bio);

  if (!(bio= BIO_new_file(public_key_path, "w")))
    SSL_ERROR("write", public_key_path);

  if (PEM_write_bio_PUBKEY(bio, pkey) <= 0)
    SSL_ERROR("write", public_key_path);
  BIO_free(bio);

  EVP_PKEY_free(pkey);
  return 0;

err:
  if (bio)
    BIO_free(bio);
  if (pkey)
    EVP_PKEY_free(pkey);
  return 1;
}

int ssl_loadkeys()
{
  EVP_PKEY *pkey= 0;
  BIO *bio;
  size_t len;

  if (!(bio= BIO_new_file(private_key_path, "r")))
    FILE_ERROR("read", private_key_path);

  if (!(pkey= PEM_read_bio_PrivateKey(bio, NULL, NULL, NULL)))
    SSL_ERROR("read", private_key_path);
  BIO_free(bio);

  if (!(bio= BIO_new_file(public_key_path, "r")))
    FILE_ERROR("read", public_key_path);
  len= BIO_read(bio, public_key, sizeof(public_key));

  if (len == sizeof(public_key))
  {
    my_printf_error(1, SELF ": failed to read %s: larger than %zu",
                    ME_ERROR_LOG_ONLY, private_key_path, sizeof(public_key)-1);
    goto err;
  }
  BIO_free(bio);

  public_key[len]= 0;
  public_key_len= len;
  private_key= pkey;
  return 0;

err:
  if (bio)
    BIO_free(bio);
  if (pkey)
    EVP_PKEY_free(pkey);
  return 1;
}
