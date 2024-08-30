/*
 Copyright (c) 2016, 2022, MariaDB Corporation.

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
#if defined(OPENSSL_VERSION_NUMBER)

#if OPENSSL_VERSION_NUMBER >= 0x10100000L && \
	!(defined(LIBRESSL_VERSION_NUMBER) && LIBRESSL_VERSION_NUMBER < 0x30500000L)
#define HAVE_OPENSSL11 1
#define SSL_LIBRARY OpenSSL_version(OPENSSL_VERSION)
#define ERR_remove_state(X) ERR_clear_error()
#define EVP_CIPHER_CTX_SIZE 200
#define EVP_MD_CTX_SIZE 80
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
#ifdef HAVE_WOLFSSL
#define SSL_LIBRARY "WolfSSL " WOLFSSL_VERSION
#else
#define SSL_LIBRARY SSLeay_version(SSLEAY_VERSION)
#endif

#ifdef HAVE_WOLFSSL
#undef ERR_remove_state
#define ERR_remove_state(x) do {} while(0)
#elif defined (HAVE_ERR_remove_thread_state)
#define ERR_remove_state(X) ERR_remove_thread_state(NULL)
#endif /* HAVE_ERR_remove_thread_state */

#endif /* HAVE_OPENSSL11 */
#endif

#ifdef HAVE_WOLFSSL
#define EVP_MD_CTX_SIZE                 sizeof(wc_Md5)
#endif

#ifndef HAVE_OPENSSL11
#ifndef ASN1_STRING_get0_data
#define ASN1_STRING_get0_data(X)        ASN1_STRING_data(X)
#endif
#ifndef EVP_MD_CTX_SIZE
#define EVP_MD_CTX_SIZE                 sizeof(EVP_MD_CTX)
#endif

#ifndef DH_set0_pqg
#define DH_set0_pqg(D,P,Q,G)            ((D)->p= (P), (D)->g= (G))
#endif

#define EVP_CIPHER_CTX_encrypting(ctx)  ((ctx)->encrypt)
#define EVP_CIPHER_CTX_SIZE             sizeof(EVP_CIPHER_CTX)

#ifndef HAVE_WOLFSSL
#define OPENSSL_init_ssl(X,Y)           SSL_library_init()
#define EVP_MD_CTX_reset(X) EVP_MD_CTX_cleanup(X)
#define EVP_CIPHER_CTX_reset(X) EVP_CIPHER_CTX_cleanup(X)
#define X509_get0_notBefore(X) X509_get_notBefore(X)
#define X509_get0_notAfter(X) X509_get_notAfter(X)
#endif
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
