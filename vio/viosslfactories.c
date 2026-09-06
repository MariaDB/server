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
#include <stdio.h>
#include <errno.h>
#include <string.h>

#ifdef HAVE_OPENSSL
#include <openssl/x509.h>
#ifndef X509_VERSION_3
#define X509_VERSION_3 2
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

static EVP_PKEY *vio_keygen()
{
  EVP_PKEY_CTX *ctx;
  EVP_PKEY *pkey = NULL;

  if (!(ctx= EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, NULL)))
    return NULL;

  if (EVP_PKEY_keygen_init(ctx) <= 0)
    goto end;

  if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 4096) <= 0)
    goto end;

  if (EVP_PKEY_keygen(ctx, &pkey) <= 0)
    pkey= NULL; /* just in case */

end:
  EVP_PKEY_CTX_free(ctx);
  return pkey;
}

static X509 *vio_gencert(EVP_PKEY *pkey)
{
  X509 *x;
  X509_NAME *name;

  if (!(x= X509_new()))
    goto err;

  if (!X509_set_version(x, X509_VERSION_3))
    goto err;
  if (!(name= X509_get_subject_name(x)))
    goto err;
  if (!X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
         (uchar*)STRING_WITH_LEN("MariaDB Server"), -1, 0))
    goto err;
  if (!X509_set_issuer_name(x, name))
    goto err;
  if (!X509_gmtime_adj(X509_get_notBefore(x), 0))
    goto err;
  if (!X509_gmtime_adj(X509_get_notAfter(x), 60*60*24*365*10))
    goto err;
  if (!X509_set_pubkey(x, pkey))
    goto err;
  if (!X509_sign(x, pkey, EVP_sha256()))
    goto err;

  return x;

err:
  X509_free(x);
  return NULL;
}

static int
vio_set_cert_stuff(SSL_CTX *ctx, const char *cert_file, const char *key_file,
                   my_bool is_client, enum enum_ssl_init_error* error)
{
  DBUG_ENTER("vio_set_cert_stuff");
  DBUG_PRINT("enter", ("ctx: %p cert_file: %s  key_file: %s",
		       ctx, cert_file, key_file));

  if (!cert_file && !key_file)
  {
    if (!is_client)
    {
      EVP_PKEY *pkey;
      X509 *x509;
      if (!(pkey= vio_keygen()) || SSL_CTX_use_PrivateKey(ctx, pkey) < 1)
      {
        *error= SSL_INITERR_KEY;
        fprintf(stderr, "SSL error: %s\n", sslGetErrString(*error));
        DBUG_RETURN(1);
      }

      if (!(x509= vio_gencert(pkey)) || SSL_CTX_use_certificate(ctx, x509) < 1)
      {
        *error= SSL_INITERR_CERT;
        fprintf(stderr, "SSL error: %s\n", sslGetErrString(*error));
        DBUG_RETURN(1);
      }
      EVP_PKEY_free(pkey);      /* decrement refcnt */
      X509_free(x509);          /* ditto */
    }
    DBUG_RETURN(0);
  }

  /* cert and key can be combined in one file */
  if (!cert_file)
    cert_file= key_file;
  else if (!key_file)
    key_file= cert_file;

  if (SSL_CTX_use_certificate_chain_file(ctx, cert_file) <= 0)
  {
    *error= SSL_INITERR_CERT;
    DBUG_PRINT("error",("%s from file '%s'", sslGetErrString(*error), cert_file));
    DBUG_EXECUTE("error", ERR_print_errors_fp(DBUG_FILE););
    fprintf(stderr, "SSL error: %s from '%s'\n", sslGetErrString(*error),
            cert_file);
    fflush(stderr);
    DBUG_RETURN(1);
  }

  if (SSL_CTX_use_PrivateKey_file(ctx, key_file, SSL_FILETYPE_PEM) <= 0)
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
    If certificate is used check if private key matches.
    Note, that server side has to use certificate.
  */
  if (!SSL_CTX_check_private_key(ctx))
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

/* Passphrase handlers */

/**
  Read a passphrase from a file
  @param buf      Buffer to store the passphrase
  @param size     Size of the buffer
  @param filename Name of the file to read the passphrase from
  @retval Length of the passphrase
*/
static int passwd_from_file(char* buf, int size, const char* filename)
{
  char *passwd= NULL;
  FILE *fp= fopen(filename, "r");
  if (fp)
  {
    passwd= fgets(buf, size, fp);
    fclose(fp);
  }
  else
  {
    fprintf(stderr,
        "SSL passphrase error: failed to open file '%s': %s (errno %d)\n ",
        filename, strerror(errno), errno);
  }
  return passwd?(int)strlen(passwd):0;
}

/**
  Read a passphrase from a given string
  @param buf      Buffer to store the passphrase
  @param size     Size of the buffer
  @param pass     clear text passphrase
  @retval Length of the passphrase
*/
static int passwd_from_string(char *buf, int size, const char *pass)
{
  int len= (int) (strmake(buf, pass, size) - buf);
  return MY_MIN(len, size - 1);
}

/**
  Read a passphrase from an environment variable
  @param buf      Buffer to store the passphrase
  @param size     Size of the buffer
  @param var      Name of the environment variable
  @retval Length of the passphrase
*/
static int passwd_from_env(char *buf, int size, const char *var)
{
  char *val= getenv(var);
  if (!val)
  {
    fprintf(stderr,
        "SSL passphrase error: environment variable '%s' not found\n", var);
    return 0;
  }
  return passwd_from_string(buf, size, val);
}

/**
  Passphrase callback for SSL_CTX_set_default_passwd_cb

  Depending on the prefix in the command string, it will call the appropriate
  handler to get the passphrase

  Currently supported prefixes are:
  - pass:  - passphrase is given as a string
  - file:  - passphrase is read from a file
  - env:   - passphrase is read from an environment variable

  The meaning is the same as passphrase parameter for openssl command line
  utility (see https://docs.openssl.org/3.4/man1/openssl-passphrase-options/#synopsis)
  Some prefixes are not supported, e.g stdin: or fd:

  @param buf      Buffer to store the passphrase
  @retval length of the passphrase
*/
static int ssl_external_passwd_cb(char *buf, int size, int rw, void *userdata)
{
  /* Prefixes and their handlers */
  struct Handler
  {
    const char *prefix;
    size_t prefix_len;
    int (*handler)(char *, int, const char *);
  };

  static const struct Handler handlers[]=
  {
    {STRING_WITH_LEN("pass:"), passwd_from_string},
    {STRING_WITH_LEN("file:"), passwd_from_file},
    {STRING_WITH_LEN("env:"), passwd_from_env}
  };
  const char *strdata= (const char *) userdata;
  (void) rw; /* unused */
  DBUG_ASSERT(buf);
  DBUG_ASSERT(size > 0);
  DBUG_ASSERT(userdata);

  for (size_t i= 0; i < array_elements(handlers); i++)
  {
    const struct Handler *h= &handlers[i];
    if (strncmp(strdata, h->prefix, h->prefix_len) == 0)
    {
      int len= h->handler(buf, size, strdata + h->prefix_len);
      // Trim trailing newlines
      while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r'))
      {
        buf[--len]= 0;
      }
      return len;
    }
  }
  fprintf(stderr, "SSL passphrase error: ssl-passphrase value must be "
         "prefixed with 'file:', 'env:', or 'pass:'\n");
  return 0; // No matching prefix found
}

/*
  If some optional parameters indicate empty strings, then
  for compatibility with SSL libraries, replace them with NULL,
  otherwise these libraries will try to open files with an empty
  name, etc., and they will return an error code instead of performing
  the necessary operations:
*/
#define fix_value(X) if (X && !X[0]) X= NULL

/************************ VioSSLFd **********************************/
static struct st_VioSSLFd *
new_VioSSLFd(const char *key_file, const char *cert_file, const char *ca_file,
             const char *ca_path, const char *cipher, my_bool is_client_method,
             enum enum_ssl_init_error *error, const char *crl_file,
             const char *crl_path, ulonglong tls_version, const char *passphrase)
{
  struct st_VioSSLFd *ssl_fd;
  long ssl_ctx_options;
  DBUG_ENTER("new_VioSSLFd");

  fix_value(key_file);
  fix_value(cert_file);
  fix_value(ca_file);
  fix_value(ca_path);
  fix_value(crl_file);
  fix_value(crl_path);
  fix_value(cipher);

  DBUG_PRINT("enter",
             ("key_file: '%s'  cert_file: '%s'  ca_file: '%s'  ca_path: '%s'  "
              "cipher: '%s' crl_file: '%s' crl_path: '%s'", key_file,
              cert_file, ca_file, ca_path, cipher, crl_file, crl_path));

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

  if (passphrase)
  {
    SSL_CTX_set_default_passwd_cb_userdata(ssl_fd->ssl_context, (void *)passphrase);
    SSL_CTX_set_default_passwd_cb(ssl_fd->ssl_context, ssl_external_passwd_cb);
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
    NOTE: SSL_CTX_set_ciphersuites/SSL_CTX_set_cipher_list will return 0 if
    none of the provided ciphers could be selected
  */
  if (cipher)
  {
    int cipher_result= 0;

    cipher_result|= SSL_CTX_set_ciphersuites(ssl_fd->ssl_context, cipher);
    cipher_result|= SSL_CTX_set_cipher_list(ssl_fd->ssl_context, cipher);

    if (cipher_result == 0)
    {
      *error= SSL_INITERR_CIPHERS;
      DBUG_PRINT("error", ("%s", sslGetErrString(*error)));
      goto err2;
    }
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

  if (vio_set_cert_stuff(ssl_fd->ssl_context, cert_file, key_file,
                         is_client_method, error))
  {
    DBUG_PRINT("error", ("vio_set_cert_stuff failed"));
    goto err2;
  }

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

int always_ok(int preverify, X509_STORE_CTX* store)
{
    return 1;
}

/************************ VioSSLConnectorFd **********************************/
struct st_VioSSLFd *
new_VioSSLConnectorFd(const char *key_file, const char *cert_file,
                      const char *ca_file, const char *ca_path,
                      const char *cipher, enum enum_ssl_init_error* error,
                      const char *crl_file, const char *crl_path)
{
  struct st_VioSSLFd *ssl_fd;
  int (*cb)(int, X509_STORE_CTX *) = NULL;

  /*
    Don't abort when the certificate cannot be verified if neither
    ca_file nor ca_path were set.
  */
  if ((ca_file == 0 || ca_file[0] == 0) && (ca_path == 0 || ca_path[0] == 0))
    cb= always_ok;

  /* Init the VioSSLFd as a "connector" ie. the client side */
  if (!(ssl_fd= new_VioSSLFd(key_file, cert_file, ca_file, ca_path, cipher,
                             TRUE, error, crl_file, crl_path, 0, NULL)))
  {
    return 0;
  }

  SSL_CTX_set_verify(ssl_fd->ssl_context, SSL_VERIFY_PEER, cb);
  return ssl_fd;
}


/************************ VioSSLAcceptorFd **********************************/
struct st_VioSSLFd *
new_VioSSLAcceptorFd(const char *key_file, const char *cert_file,
		     const char *ca_file, const char *ca_path,
		     const char *cipher, enum enum_ssl_init_error* error,
                     const char *crl_file, const char *crl_path,
                     ulonglong tls_version, const char *passphrase)
{
  struct st_VioSSLFd *ssl_fd;
  int verify= SSL_VERIFY_PEER | SSL_VERIFY_CLIENT_ONCE;

  /* Init the the VioSSLFd as a "acceptor" ie. the server side */
  if (!(ssl_fd= new_VioSSLFd(key_file, cert_file, ca_file, ca_path, cipher,
                             FALSE, error, crl_file, crl_path, tls_version, passphrase)))
  {
    return 0;
  }
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
