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
#define SSL_LIBRARY OpenSSL_version(OPENSSL_VERSION)
#define ERR_remove_state(X) ERR_clear_error()
#define EVP_CIPHER_CTX_SIZE 176
#define EVP_MD_CTX_SIZE 48
#undef EVP_MD_CTX_init
#define EVP_MD_CTX_init(X) do { memset((X), 0, EVP_MD_CTX_SIZE); EVP_MD_CTX_reset(X); } while(0)
#undef EVP_CIPHER_CTX_init
#define EVP_CIPHER_CTX_init(X) do { memset((X), 0, EVP_CIPHER_CTX_SIZE); EVP_CIPHER_CTX_reset(X); } while(0)

/*
  Macros below are deprecated. OpenSSL 1.1 may define them or not,
  depending on how it was built.
*/
#undef ERR_free_strings
#define ERR_free_strings()
#undef EVP_cleanup
#define EVP_cleanup()
#undef CRYPTO_cleanup_all_ex_data
#define CRYPTO_cleanup_all_ex_data()
#undef SSL_load_error_strings
#define SSL_load_error_strings()

#else
#define HAVE_OPENSSL10 1
#define SSL_LIBRARY SSLeay_version(SSLEAY_VERSION)

#ifdef HAVE_ERR_remove_thread_state
#define ERR_remove_state(X) ERR_remove_thread_state(NULL)
#endif /* HAVE_ERR_remove_thread_state */

#endif /* HAVE_OPENSSL11 */

#elif defined(HAVE_YASSL)
#define SSL_LIBRARY "YaSSL " YASSL_VERSION
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

#define EVP_MD_CTX_reset(X) EVP_MD_CTX_cleanup(X)
#define EVP_CIPHER_CTX_reset(X) EVP_CIPHER_CTX_cleanup(X)
#define X509_get0_notBefore(X) X509_get_notBefore(X)
#define X509_get0_notAfter(X) X509_get_notAfter(X)
#endif

#ifndef TLS1_3_VERSION
#define SSL_CTX_set_ciphersuites(X,Y) 0
#endif

#ifdef	__cplusplus
extern "C" {
#endif /* __cplusplus */

int check_openssl_compatibility();

#ifdef	__cplusplus
}
#endif
