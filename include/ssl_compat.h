/*
 Copyright (c) 2016, 2017 MariaDB Corporation

 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.

 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#include <openssl/opensslv.h>

/* OpenSSL version specific definitions */
#if !defined(HAVE_YASSL) && defined(OPENSSL_VERSION_NUMBER)

#if OPENSSL_VERSION_NUMBER >= 0x10002000L && !defined(LIBRESSL_VERSION_NUMBER)
#define HAVE_X509_check_host 1
#endif

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && !defined(LIBRESSL_VERSION_NUMBER)
#define HAVE_OPENSSL11 1
#define ERR_remove_state(X) ERR_clear_error()
#define EVP_MD_CTX_cleanup(X) EVP_MD_CTX_reset(X)
#define EVP_CIPHER_CTX_SIZE 168
#define EVP_MD_CTX_SIZE 48
#undef EVP_MD_CTX_init
#define EVP_MD_CTX_init(X) do { bzero((X), EVP_MD_CTX_SIZE); EVP_MD_CTX_reset(X); } while(0)
#undef EVP_CIPHER_CTX_init
#define EVP_CIPHER_CTX_init(X) do { bzero((X), EVP_CIPHER_CTX_SIZE); EVP_CIPHER_CTX_reset(X); } while(0)

#else
#define HAVE_OPENSSL10 1
/*
  Unfortunately RAND_bytes manual page does not provide any guarantees
  in relation to blocking behavior. Here we explicitly use SSLeay random
  instead of whatever random engine is currently set in OpenSSL. That way
  we are guaranteed to have a non-blocking random.
*/
#define RAND_OpenSSL() RAND_SSLeay()

#ifdef HAVE_ERR_remove_thread_state
#define ERR_remove_state(X) ERR_remove_thread_state(NULL)
#endif /* HAVE_ERR_remove_thread_state */

#endif /* HAVE_OPENSSL11 */

#elif defined(HAVE_YASSL)
#define BN_free(X) do { } while(0)
#endif /* !defined(HAVE_YASSL) */

#ifndef HAVE_OPENSSL11
#define ASN1_STRING_get0_data(X)        ASN1_STRING_data(X)
#define OPENSSL_init_ssl(X,Y)           SSL_library_init()
#define DH_set0_pqg(D,P,Q,G)            ((D)->p= (P), (D)->g= (G))
#define EVP_CIPHER_CTX_buf_noconst(ctx) ((ctx)->buf)
#define EVP_CIPHER_CTX_encrypting(ctx)  ((ctx)->encrypt)
#define EVP_CIPHER_CTX_SIZE             sizeof(EVP_CIPHER_CTX)
#define EVP_MD_CTX_SIZE                 sizeof(EVP_MD_CTX)
#endif

#ifdef	__cplusplus
extern "C" {
#endif /* __cplusplus */

int check_openssl_compatibility();

#ifdef	__cplusplus
}
#endif
