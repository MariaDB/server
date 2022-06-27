/* Copyright (c) 2000, 2016, Oracle and/or its affiliates.
   Copyright (c) 2011, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "vio_priv.h"
#include <ssl_compat.h>

#ifdef HAVE_OPENSSL
#include <openssl/dh.h>
#include <openssl/bn.h>

static my_bool     ssl_algorithms_added    = FALSE;
static my_bool     ssl_error_strings_loaded= FALSE;

/* the function below was generated with "openssl dhparam -2 -C 2048" */
#ifndef HAVE_WOLFSSL
static
DH *get_dh2048()
{
    static unsigned char dhp_2048[] = {
        0xA1,0xBB,0x7C,0x20,0xC5,0x5B,0xC0,0x7B,0x21,0x8B,0xD6,0xA8,
        0x15,0xFC,0x3B,0xBA,0xAB,0x9F,0xDF,0x68,0xC4,0x79,0x78,0x0D,
        0xC1,0x12,0x64,0xE4,0x15,0xC9,0x66,0xDB,0xF6,0xCB,0xB3,0x39,
        0x02,0x5B,0x78,0x62,0xFB,0x09,0xAE,0x09,0x6B,0xDD,0xD4,0x5D,
        0x97,0xBC,0xDC,0x7F,0xE6,0xD6,0xF1,0xCB,0xF5,0xEB,0xDA,0xA7,
        0x2E,0x5A,0x43,0x2B,0xE9,0x40,0xE2,0x85,0x00,0x1C,0xC0,0x0A,
        0x98,0x77,0xA9,0x31,0xDE,0x0B,0x75,0x4D,0x1E,0x1F,0x16,0x83,
        0xCA,0xDE,0xBD,0x21,0xFC,0xC1,0x82,0x37,0x36,0x33,0x0B,0x66,
        0x06,0x3C,0xF3,0xAF,0x21,0x57,0x57,0x80,0xF6,0x94,0x1B,0xA9,
        0xD4,0xF6,0x8F,0x18,0x62,0x0E,0xC4,0x22,0xF9,0x5B,0x62,0xCC,
        0x3F,0x19,0x95,0xCF,0x4B,0x00,0xA6,0x6C,0x0B,0xAF,0x9F,0xD5,
        0xFA,0x3D,0x6D,0xDA,0x30,0x83,0x07,0x91,0xAC,0x15,0xFF,0x8F,
        0x59,0x54,0xEA,0x25,0xBC,0x4E,0xEB,0x6A,0x54,0xDF,0x75,0x09,
        0x72,0x0F,0xEF,0x23,0x70,0xE0,0xA8,0x04,0xEA,0xFF,0x90,0x54,
        0xCD,0x84,0x18,0xC0,0x75,0x91,0x99,0x0F,0xA1,0x78,0x0C,0x07,
        0xB7,0xC5,0xDE,0x55,0x06,0x7B,0x95,0x68,0x2C,0x33,0x39,0xBC,
        0x2C,0xD0,0x6D,0xDD,0xFA,0xDC,0xB5,0x8F,0x82,0x39,0xF8,0x67,
        0x44,0xF1,0xD8,0xF7,0x78,0x11,0x9A,0x77,0x9B,0x53,0x47,0xD6,
        0x2B,0x5D,0x67,0xB8,0xB7,0xBC,0xC1,0xD7,0x79,0x62,0x15,0xC2,
        0xC5,0x83,0x97,0xA7,0xF8,0xB4,0x9C,0xF6,0x8F,0x9A,0xC7,0xDA,
        0x1B,0xBB,0x87,0x07,0xA7,0x71,0xAD,0xB2,0x8A,0x50,0xF8,0x26,
        0x12,0xB7,0x3E,0x0B,
    };
    static unsigned char dhg_2048[] = {
        0x02
    };
    DH *dh = DH_new();
    BIGNUM *dhp_bn, *dhg_bn;

    if (dh == NULL)
        return NULL;
    dhp_bn = BN_bin2bn(dhp_2048, sizeof (dhp_2048), NULL);
    dhg_bn = BN_bin2bn(dhg_2048, sizeof (dhg_2048), NULL);
    if (dhp_bn == NULL || dhg_bn == NULL
            || !DH_set0_pqg(dh, dhp_bn, NULL, dhg_bn)) {
        DH_free(dh);
        BN_free(dhp_bn);
        BN_free(dhg_bn);
        return NULL;
    }
    return dh;
}
#endif

static const char*
ssl_error_string[] =
{
  "No error",
  "Unable to get certificate",
  "Unable to get private key",
  "Private key does not match the certificate public key",
  "SSL_CTX_set_default_verify_paths failed",
  "Failed to set ciphers to use",
  "SSL_CTX_new failed",
  "SSL_CTX_set_tmp_dh failed",
  "Unknown TLS version"
};

const char*
sslGetErrString(enum enum_ssl_init_error e)
{
  DBUG_ASSERT(SSL_INITERR_NOERROR < e && e < SSL_INITERR_LASTERR);
  return ssl_error_string[e];
}

static int
vio_set_cert_stuff(SSL_CTX *ctx, const char *cert_file, const char *key_file,
                   enum enum_ssl_init_error* error)
{
  DBUG_ENTER("vio_set_cert_stuff");
  DBUG_PRINT("enter", ("ctx: %p cert_file: %s  key_file: %s",
		       ctx, cert_file, key_file));

  if (!cert_file &&  key_file)
    cert_file= key_file;
  
  if (!key_file &&  cert_file)
    key_file= cert_file;

  if (cert_file &&
      SSL_CTX_use_certificate_chain_file(ctx, cert_file) <= 0)
  {
    *error= SSL_INITERR_CERT;
    DBUG_PRINT("error",("%s from file '%s'", sslGetErrString(*error), cert_file));
    DBUG_EXECUTE("error", ERR_print_errors_fp(DBUG_FILE););
    fprintf(stderr, "SSL error: %s from '%s'\n", sslGetErrString(*error),
            cert_file);
    fflush(stderr);
    DBUG_RETURN(1);
  }

  if (key_file &&
      SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0)
  {
    *error= SSL_INITERR_KEY;
    DBUG_PRINT("error", ("%s from file '%s'", sslGetErrString(*error), key_file));
    DBUG_EXECUTE("error", ERR_print_errors_fp(DBUG_FILE););
    fprintf(stderr, "SSL error: %s from '%s'\n", sslGetErrString(*error),
            key_file);
    fflush(stderr);
    DBUG_RETURN(1);
  }

  /*
    If we are using DSA, we can copy the parameters from the private key
    Now we know that a key and cert have been set against the SSL context
  */
  if (cert_file && !SSL_CTX_check_private_key(ctx))
  {
    *error= SSL_INITERR_NOMATCH;
    DBUG_PRINT("error", ("%s",sslGetErrString(*error)));
    DBUG_EXECUTE("error", ERR_print_errors_fp(DBUG_FILE););
    fprintf(stderr, "SSL error: %s\n", sslGetErrString(*error));
    fflush(stderr);
    DBUG_RETURN(1);
  }

  DBUG_RETURN(0);
}


void vio_check_ssl_init()
{
  if (!ssl_algorithms_added)
  {
    ssl_algorithms_added= TRUE;
    OPENSSL_init_ssl(0, NULL);
  }

  if (!ssl_error_strings_loaded)
  {
    ssl_error_strings_loaded= TRUE;
    SSL_load_error_strings();
  }
}

#ifdef HAVE_WOLFSSL
static int wolfssl_recv(WOLFSSL* ssl, char* buf, int sz, void* vio)
{
  size_t ret;
  (void)ssl;
  ret = vio_read((Vio *)vio, (uchar *)buf, sz);
  /* check if connection was closed */
  if (ret == 0)
    return WOLFSSL_CBIO_ERR_CONN_CLOSE;

  return (int)ret;
}

static int wolfssl_send(WOLFSSL* ssl, char* buf, int sz, void* vio)
{
  return (int)vio_write((Vio *)vio, (unsigned char*)buf, sz);
}
#endif /* HAVE_WOLFSSL */

static long vio_tls_protocol_options(ulonglong tls_version)
{
   long tls_protocol_flags=
#ifdef TLS1_3_VERSION
    SSL_OP_NO_TLSv1_3 |
#endif
#if defined(TLS1_2_VERSION) || defined(HAVE_WOLFSSL)
    SSL_OP_NO_TLSv1_2 |
#endif
    SSL_OP_NO_TLSv1_1 |
    SSL_OP_NO_TLSv1;
   long disabled_tls_protocols= tls_protocol_flags,
        disabled_ssl_protocols= SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3;

  if (!tls_version)
    return disabled_ssl_protocols;

  if (tls_version & VIO_TLSv1_0)
    disabled_tls_protocols&= ~SSL_OP_NO_TLSv1;
  if (tls_version & VIO_TLSv1_1)
    disabled_tls_protocols&= ~SSL_OP_NO_TLSv1_1;
#if defined(TLS1_2_VERSION) || defined(HAVE_WOLFSSL)
  if (tls_version & VIO_TLSv1_2)
    disabled_tls_protocols&= ~SSL_OP_NO_TLSv1_2;
#endif
#ifdef TLS1_3_VERSION
  if (tls_version & VIO_TLSv1_3)
    disabled_tls_protocols&= ~SSL_OP_NO_TLSv1_3;
#endif

  /* some garbage was specified in tls_version option */
  if (tls_protocol_flags == disabled_tls_protocols)
    return -1;
  return (disabled_tls_protocols | disabled_ssl_protocols);
}

/************************ VioSSLFd **********************************/
static struct st_VioSSLFd *
new_VioSSLFd(const char *key_file, const char *cert_file,
             const char *ca_file, const char *ca_path,
             const char *cipher, my_bool is_client_method,
             enum enum_ssl_init_error *error,
             const char *crl_file, const char *crl_path, ulonglong tls_version)
{
  struct st_VioSSLFd *ssl_fd;
  long ssl_ctx_options;
  DBUG_ENTER("new_VioSSLFd");

  /*
    If some optional parameters indicate empty strings, then
    for compatibility with SSL libraries, replace them with NULL,
    otherwise these libraries will try to open files with an empty
    name, etc., and they will return an error code instead performing
    the necessary operations:
  */
  if (ca_file && !ca_file[0])
  {
    ca_file  = NULL;
  }
  if (ca_path && !ca_path[0])
  {
    ca_path  = NULL;
  }
  if (crl_file && !crl_file[0])
  {
    crl_file = NULL;
  }
  if (crl_path && !crl_path[0])
  {
    crl_path = NULL;
  }

  DBUG_PRINT("enter",
             ("key_file: '%s'  cert_file: '%s'  ca_file: '%s'  ca_path: '%s'  "
              "cipher: '%s' crl_file: '%s' crl_path: '%s'",
              key_file ? key_file : "NULL",
              cert_file ? cert_file : "NULL",
              ca_file ? ca_file : "NULL",
              ca_path ? ca_path : "NULL",
              cipher ? cipher : "NULL",
              crl_file ? crl_file : "NULL",
              crl_path ? crl_path : "NULL"));

  vio_check_ssl_init();

  if (!(ssl_fd= ((struct st_VioSSLFd*)
                 my_malloc(key_memory_vio_ssl_fd,
                           sizeof(struct st_VioSSLFd), MYF(0)))))
    goto err0;
  if (!(ssl_fd->ssl_context= SSL_CTX_new(is_client_method ? 
                                         SSLv23_client_method() :
                                         SSLv23_server_method())))
  {
    *error= SSL_INITERR_MEMFAIL;
    DBUG_PRINT("error", ("%s", sslGetErrString(*error)));
    goto err1;
  }

  ssl_ctx_options= vio_tls_protocol_options(tls_version);
  if (ssl_ctx_options == -1)
  {
    *error= SSL_INITERR_PROTOCOL;
    DBUG_PRINT("error", ("%s", sslGetErrString(*error)));
    goto err1;
  }

  SSL_CTX_set_options(ssl_fd->ssl_context, ssl_ctx_options);

  /*
    Set the ciphers that can be used
    NOTE: SSL_CTX_set_cipher_list will return 0 if
    none of the provided ciphers could be selected
  */
  if (cipher &&
      SSL_CTX_set_ciphersuites(ssl_fd->ssl_context, cipher) == 0 &&
      SSL_CTX_set_cipher_list(ssl_fd->ssl_context, cipher) == 0)
  {
    *error= SSL_INITERR_CIPHERS;
    DBUG_PRINT("error", ("%s", sslGetErrString(*error)));
    goto err2;
  }

  /* Load certs from the trusted ca */
  if (SSL_CTX_load_verify_locations(ssl_fd->ssl_context, ca_file, ca_path) <= 0)
  {
    DBUG_PRINT("warning", ("SSL_CTX_load_verify_locations failed"));
    if (ca_file || ca_path)
    {
      /* fail only if ca file or ca path were supplied and looking into 
         them fails. */
      *error= SSL_INITERR_BAD_PATHS;
      DBUG_PRINT("error", ("SSL_CTX_load_verify_locations failed : %s", 
                 sslGetErrString(*error)));
      goto err2;
    }
#ifndef HAVE_WOLFSSL
    /* otherwise go use the defaults */
    if (SSL_CTX_set_default_verify_paths(ssl_fd->ssl_context) == 0)
    {
      *error= SSL_INITERR_BAD_PATHS;
      DBUG_PRINT("error", ("%s", sslGetErrString(*error)));
      goto err2;
    }
#endif
  }

  if (crl_file || crl_path)
  {
#ifdef HAVE_WOLFSSL
    /* CRL does not work with WolfSSL. */
    DBUG_ASSERT(0);
    goto err2;
#else
    X509_STORE *store= SSL_CTX_get_cert_store(ssl_fd->ssl_context);
    /* Load crls from the trusted ca */
    if (X509_STORE_load_locations(store, crl_file, crl_path) == 0 ||
        X509_STORE_set_flags(store,
                             X509_V_FLAG_CRL_CHECK | 
                             X509_V_FLAG_CRL_CHECK_ALL) == 0)
    {
      DBUG_PRINT("warning", ("X509_STORE_load_locations for CRL failed"));
      *error= SSL_INITERR_BAD_PATHS;
      DBUG_PRINT("error", ("%s", sslGetErrString(*error)));
      goto err2;
    }
#endif
  }

  if (vio_set_cert_stuff(ssl_fd->ssl_context, cert_file, key_file, error))
  {
    DBUG_PRINT("error", ("vio_set_cert_stuff failed"));
    goto err2;
  }

#ifndef HAVE_WOLFSSL
  /* DH stuff */
  if (!is_client_method)
  {
    DH *dh= get_dh2048();
    if (!SSL_CTX_set_tmp_dh(ssl_fd->ssl_context, dh))
    {
      *error= SSL_INITERR_DH;
      DH_free(dh);
      goto err2;
    }

    DH_free(dh);
  }
#endif

#ifdef HAVE_WOLFSSL
  /* set IO functions used by wolfSSL */
   wolfSSL_SetIORecv(ssl_fd->ssl_context, wolfssl_recv);
   wolfSSL_SetIOSend(ssl_fd->ssl_context, wolfssl_send);
#endif

  DBUG_PRINT("exit", ("OK 1"));

  DBUG_RETURN(ssl_fd);

err2:
  SSL_CTX_free(ssl_fd->ssl_context);
err1:
  my_free(ssl_fd);
err0:
  DBUG_EXECUTE("error", ERR_print_errors_fp(DBUG_FILE););
  DBUG_RETURN(0);
}


/************************ VioSSLConnectorFd **********************************/
struct st_VioSSLFd *
new_VioSSLConnectorFd(const char *key_file, const char *cert_file,
                      const char *ca_file, const char *ca_path,
                      const char *cipher, enum enum_ssl_init_error* error,
                      const char *crl_file, const char *crl_path)
{
  struct st_VioSSLFd *ssl_fd;
  int verify= SSL_VERIFY_PEER;

  if (ca_file  && ! ca_file[0])  ca_file  = NULL;
  if (ca_path  && ! ca_path[0])  ca_path  = NULL;
  if (crl_file && ! crl_file[0]) crl_file = NULL;
  if (crl_path && ! crl_path[0]) crl_path = NULL;

  /*
    If some optional parameters indicate empty strings, then
    for compatibility with SSL libraries, replace them with NULL,
    otherwise these libraries will try to open files with an empty
    name, etc., and they will return an error code instead performing
    the necessary operations:
  */
  if (ca_file && !ca_file[0])
  {
    ca_file  = NULL;
  }
  if (ca_path && !ca_path[0])
  {
    ca_path  = NULL;
  }
  if (crl_file && !crl_file[0])
  {
    crl_file = NULL;
  }
  if (crl_path && !crl_path[0])
  {
    crl_path = NULL;
  }

  /*
    Turn off verification of servers certificate if both
    ca_file and ca_path is set to NULL
  */
  if (ca_file == 0 && ca_path == 0)
    verify= SSL_VERIFY_NONE;

  if (!(ssl_fd= new_VioSSLFd(key_file, cert_file, ca_file,
                             ca_path, cipher, TRUE, error,
                             crl_file, crl_path, 0)))
  {
    return 0;
  }

  /* Init the VioSSLFd as a "connector" ie. the client side */

  SSL_CTX_set_verify(ssl_fd->ssl_context, verify, NULL);

  return ssl_fd;
}


/************************ VioSSLAcceptorFd **********************************/
struct st_VioSSLFd *
new_VioSSLAcceptorFd(const char *key_file, const char *cert_file,
		     const char *ca_file, const char *ca_path,
		     const char *cipher, enum enum_ssl_init_error* error,
                     const char *crl_file, const char *crl_path,
                     ulonglong tls_version)
{
  struct st_VioSSLFd *ssl_fd;
  int verify= SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;

  /*
    If some optional parameters indicate empty strings, then
    for compatibility with SSL libraries, replace them with NULL,
    otherwise these libraries will try to open files with an empty
    name, etc., and they will return an error code instead performing
    the necessary operations:
  */
  if (ca_file && !ca_file[0])
  {
    ca_file  = NULL;
  }
  if (ca_path && !ca_path[0])
  {
    ca_path  = NULL;
  }
  if (crl_file && !crl_file[0])
  {
    crl_file = NULL;
  }
  if (crl_path && !crl_path[0])
  {
    crl_path = NULL;
  }

  if (!(ssl_fd= new_VioSSLFd(key_file, cert_file, ca_file,
                             ca_path, cipher, FALSE, error,
                             crl_file, crl_path, tls_version)))
  {
    return 0;
  }
  /* Init the the VioSSLFd as a "acceptor" ie. the server side */

  /* Set max number of cached sessions, returns the previous size */
  SSL_CTX_sess_set_cache_size(ssl_fd->ssl_context, 128);

  SSL_CTX_set_verify(ssl_fd->ssl_context, verify, NULL);

  /*
    Set session_id - an identifier for this server session
    Use the ssl_fd pointer
   */
  SSL_CTX_set_session_id_context(ssl_fd->ssl_context,
				 (const unsigned char *)ssl_fd,
				 sizeof(ssl_fd));

  return ssl_fd;
}

void free_vio_ssl_acceptor_fd(struct st_VioSSLFd *fd)
{
  DBUG_ENTER("free_vio_ssl_acceptor_fd");
  SSL_CTX_free(fd->ssl_context);
  my_free(fd);
  DBUG_VOID_RETURN;
}
#endif /* HAVE_OPENSSL */
