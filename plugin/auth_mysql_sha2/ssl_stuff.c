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
#ifdef OPENSSL_IS_WOLFSSL
  int ret= 1;
  static RsaKey rsa_key; /* large struct, thus static */
  WC_RNG rng;
  byte der[4096];
  byte pem[4096];
  int der_size, pem_size;
  FILE *f= NULL;

  ret= wc_InitRng(&rng);
  if (ret)
  {
    my_printf_error(
        1, SELF ": failed to generate RSA key: wc_InitRng failed with %d",
        ME_ERROR_LOG_ONLY, ret);
    return ret;
  }

  ret= wc_InitRsaKey(&rsa_key, NULL);
  if (ret)
  {
    my_printf_error(
        1, SELF ": failed to generate RSA key: wc_InitRsaKey failed with %d",
        ME_ERROR_LOG_ONLY, ret);
    return ret;
  }

  ret= wc_MakeRsaKey(&rsa_key, 2048, 65537, &rng);
  if (ret)
    SSL_ERROR("failed to generate RSA key: wc_MakeRsaKey failed", private_key_path);

  der_size= wc_RsaKeyToDer(&rsa_key, der, sizeof(der));
  if (der_size < 0)
    SSL_ERROR("wc_DerToPem", private_key_path);

  pem_size= wc_DerToPem(der, (word32) der_size, pem, sizeof(pem), PRIVATEKEY_TYPE);
  if (pem_size < 0)
    SSL_ERROR("wc_DerToPem", private_key_path);

  if (!(f= fopen(private_key_path, "w")))
    FILE_ERROR("fopen", private_key_path);
  if (fwrite(pem, 1, pem_size, f) < (size_t)pem_size)
    FILE_ERROR("fwrite", private_key_path);
  fclose(f);
  f= NULL;

  der_size= wc_RsaKeyToPublicDer(&rsa_key, der, sizeof(der));
  if (der_size < 0)
    SSL_ERROR("wc_RsaKeyToPublicDer", public_key_path);
  pem_size= wc_DerToPem(der, (word32)der_size, pem, sizeof(pem), PUBLICKEY_TYPE);
  if (pem_size < 0)
    SSL_ERROR("wc_DerToPem", public_key_path);

  if (!(f= fopen(public_key_path, "w")))
    FILE_ERROR("write", public_key_path);
  if (fwrite(pem, 1, pem_size, f) < (size_t)pem_size)
    FILE_ERROR("fwrite", public_key_path);
  fclose(f);
  f= NULL;

  ret= 0;

err:
  wc_FreeRsaKey(&rsa_key);
  wc_FreeRng(&rng);
  if (f)
    fclose(f);
  return ret;
#else
  EVP_PKEY *pkey;
  FILE *f= NULL;

  if (!(pkey= EVP_RSA_gen(2048)))
    goto err;

  if (!(f= fopen(private_key_path, "w")))
    FILE_ERROR("write", private_key_path);

  if (PEM_write_PrivateKey(f, pkey, NULL, NULL, 0, NULL, NULL) != 1)
    SSL_ERROR("write", private_key_path);
  fclose(f);

  if (!(f= fopen(public_key_path, "w")))
    FILE_ERROR("write", public_key_path);

  if (PEM_write_PUBKEY(f, pkey) != 1)
    SSL_ERROR("write", public_key_path);
  fclose(f);

  EVP_PKEY_free(pkey);
  return 0;

err:
  if (f)
    fclose(f);
  if (pkey)
    EVP_PKEY_free(pkey);
  return 1;
#endif
}

int ssl_loadkeys()
{
  EVP_PKEY *pkey= 0;
  FILE *f;
  size_t len;

  if (!(f= fopen(private_key_path, "r")))
    FILE_ERROR("read", private_key_path);

  if (!(pkey= PEM_read_PrivateKey(f, NULL, NULL, NULL)))
    SSL_ERROR("read", private_key_path);
  fclose(f);

  if (!(f= fopen(public_key_path, "r")))
    FILE_ERROR("read", public_key_path);
  len= fread(public_key, 1, sizeof(public_key)-1, f);

  if (!feof(f))
  {
    my_printf_error(1, SELF ": failed to read %s: larger than %zu",
                    ME_ERROR_LOG_ONLY, private_key_path, sizeof(public_key)-1);
    goto err;
  }
  fclose(f);

  public_key[len]= 0;
  public_key_len= len;
  private_key= pkey;
  return 0;

err:
  if (f)
    fclose(f);
  if (pkey)
    EVP_PKEY_free(pkey);
  return 1;
}
