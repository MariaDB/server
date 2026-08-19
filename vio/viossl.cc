/* Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.
   Copyright (c) 2026, MariaDB Corporation

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

/*
  Note that we can't have assertion on file descriptors;  The reason for
  this is that during mysql shutdown, another thread can close a file
  we are working on.  In this case we should just return read errors from
  the file descriptior.
*/

#include "vio_priv.h"
#include <new>

#ifdef HAVE_OPENSSL
#include <ssl_compat.h>

#define SSL_errno(X,Y) ERR_get_error()

#ifndef HAVE_WOLFSSL

static int vio_bio_read(BIO *bio, char *buf, int size)
{
  Vio *filter;
#ifdef HAVE_OPENSSL11
  filter= (Vio *)BIO_get_data(bio);
#else
  filter= (Vio *)bio->ptr;
#endif
  Vio *underlying= vio_get_underlying(filter);
  if (!underlying)
    return -1;
  ssize_t ret= (ssize_t) vio_read(underlying, (uchar*)buf, size);
  return ret < 0 ? -1 : (int) ret;
}

static int vio_bio_write(BIO *bio, const char *buf, int size)
{
  Vio *filter;
#ifdef HAVE_OPENSSL11
  filter= (Vio *)BIO_get_data(bio);
#else
  filter= (Vio *)bio->ptr;
#endif
  Vio *underlying= vio_get_underlying(filter);
  if (!underlying)
    return -1;
  ssize_t ret= (ssize_t) vio_write(underlying, (const uchar*)buf, size);
  return ret < 0 ? -1 : (int) ret;
}

static long vio_bio_ctrl(BIO *bio __attribute__((unused)), int cmd,
                         long arg __attribute__((unused)),
                         void *ptr __attribute__((unused)))
{
  return cmd == BIO_CTRL_FLUSH ? 1 : 0;
}

static int vio_bio_create(BIO *bio)
{
#ifdef HAVE_OPENSSL11
  BIO_set_data(bio, NULL);
  BIO_set_init(bio, 1);
#else
  bio->ptr= NULL;
  bio->init= 1;
#endif
  return 1;
}

static int vio_bio_destroy(BIO *bio)
{
  if (!bio)
    return 0;
#ifdef HAVE_OPENSSL11
  BIO_set_data(bio, NULL);
  BIO_set_init(bio, 0);
#else
  bio->ptr= NULL;
  bio->init= 0;
#endif
  return 1;
}

#ifdef HAVE_OPENSSL11
static BIO_METHOD *vio_bio_method_instance;
static my_pthread_once_t vio_bio_method_once= MY_PTHREAD_ONCE_INIT;

static void vio_bio_method_init(void)
{
  BIO_METHOD *method= BIO_meth_new(BIO_TYPE_SOURCE_SINK, "MariaDB VIO");
  if (method)
  {
    BIO_meth_set_read(method, vio_bio_read);
    BIO_meth_set_write(method, vio_bio_write);
    BIO_meth_set_ctrl(method, vio_bio_ctrl);
    BIO_meth_set_create(method, vio_bio_create);
    BIO_meth_set_destroy(method, vio_bio_destroy);
  }
  vio_bio_method_instance= method;
}

static BIO_METHOD *vio_bio_method(void)
{
  my_pthread_once(&vio_bio_method_once, vio_bio_method_init);
  return vio_bio_method_instance;
}
#else
static BIO_METHOD *vio_bio_method(void)
{
  static BIO_METHOD method=
  {
    BIO_TYPE_SOURCE_SINK, "MariaDB VIO", vio_bio_write, vio_bio_read,
    NULL, NULL, vio_bio_ctrl, vio_bio_create, vio_bio_destroy, NULL
  };
  return &method;
}
#endif

static int ssl_set_vio_bio(SSL *ssl, void *vio)
{
  BIO *bio;
  BIO_METHOD *method= vio_bio_method();
  if (!method || !(bio= BIO_new(method)))
    return 1;
#ifdef HAVE_OPENSSL11
  BIO_set_data(bio, vio);
#else
  bio->ptr= vio;
#endif
  SSL_set_bio(ssl, bio, bio);
  return 0;
}
#endif /* !HAVE_WOLFSSL */

/**
  Obtain the equivalent system error status for the last SSL I/O operation.

  @param ssl_error  The result code of the failed TLS/SSL I/O operation.
*/

static void ssl_set_sys_error(int ssl_error)
{
  int error= 0;

  switch (ssl_error)
  {
  case SSL_ERROR_ZERO_RETURN:
    error= SOCKET_ECONNRESET;
    break;
  case SSL_ERROR_WANT_READ:
  case SSL_ERROR_WANT_WRITE:
#ifdef SSL_ERROR_WANT_CONNECT
  case SSL_ERROR_WANT_CONNECT:
#endif
#ifdef SSL_ERROR_WANT_ACCEPT
  case SSL_ERROR_WANT_ACCEPT:
#endif
    error= SOCKET_EWOULDBLOCK;
    break;
  case SSL_ERROR_SSL:
    /* Protocol error. */
#ifdef EPROTO
    error= EPROTO;
#else
    error= SOCKET_ECONNRESET;
#endif
    break;
  case SSL_ERROR_SYSCALL:
  case SSL_ERROR_NONE:
  default:
    break;
  };

  /* Set error status to a equivalent of the SSL error. */
  if (error)
  {
#ifdef _WIN32
    WSASetLastError(error);
#else
    errno= error;
#endif
  }
}


static void ssl_set_io_error(SSL *ssl, int ret)
{
  int ssl_error= SSL_get_error(ssl, ret);

  /* SSL_ERROR_SYSCALL preserves the classification made by the transport. */
  if (ssl_error != SSL_ERROR_SYSCALL)
    vio_set_was_timeout(FALSE);
  ssl_set_sys_error(ssl_error);
  ERR_clear_error();
}

Ssl_vio::Ssl_vio(Vio *underlying, void *ssl)
  : Vio_filter(underlying), m_ssl(ssl)
{
}

Ssl_vio::~Ssl_vio()
{
  if (m_ssl)
    SSL_free(static_cast<SSL *>(m_ssl));
}

size_t Ssl_vio::read(uchar *buf, size_t size)
{
  int ret;
  SSL *ssl= static_cast<SSL *>(m_ssl);
  DBUG_ENTER("Ssl_vio::read");
  DBUG_PRINT("enter", ("sd: %d  buf: %p  size: %zu  ssl: %p",
		       (int)m_underlying->fd(), buf, size, m_ssl));


  ret= SSL_read(ssl, buf, (int)size);
  if (ret < 0)
    ssl_set_io_error(ssl, ret);
  else if (!ret)
    vio_set_was_timeout(FALSE);

  DBUG_PRINT("exit", ("%d", ret));
  DBUG_RETURN(ret < 0 ? -1 : ret);

}


size_t Ssl_vio::write(const uchar *buf, size_t size)
{
  int ret;
  SSL *ssl= static_cast<SSL *>(m_ssl);
  DBUG_ENTER("Ssl_vio::write");
  DBUG_PRINT("enter", ("sd: %d  buf: %p  size: %zu",
                       (int)m_underlying->fd(),
                       buf, size));
  ret= SSL_write(ssl, buf, (int)size);
  if (ret < 0)
    ssl_set_io_error(ssl, ret);
  else if (!ret && size)
    vio_set_was_timeout(FALSE);

  DBUG_RETURN(ret < 0 ? -1 : ret);
}

int Ssl_vio::close()
{
  int r= 0;
  SSL *ssl= static_cast<SSL *>(m_ssl);
  DBUG_ENTER("Ssl_vio::close");

  if (ssl)
  {
    /*
    THE SSL standard says that SSL sockets must send and receive a close_notify
    alert on socket shutdown to avoid truncation attacks. However, this can
    cause problems since we often hold a lock during shutdown and this IO can
    take an unbounded amount of time to complete. Since our packets are self
    describing with length, we aren't vulnerable to these attacks. Therefore,
    we just shutdown by closing the socket (quiet shutdown).
    */
    SSL_set_quiet_shutdown(ssl, 1); 
    
    switch ((r= SSL_shutdown(ssl))) {
    case 1:
      /* Shutdown successful */
      break;
    case 0:
      /*
        Shutdown not yet finished - since the socket is going to
        be closed there is no need to call SSL_shutdown() a second
        time to wait for the other side to respond
      */
      break;
    default: /* Shutdown failed */
      DBUG_PRINT("vio_error", ("SSL_shutdown() failed, error: %d",
                               SSL_get_error(ssl, r)));
      break;
    }
  }
  (void) m_underlying->close();
  DBUG_RETURN(r);
}

/** SSL handshake handler. */
typedef int (*ssl_handshake_func_t)(SSL*);


/**
  Loop and wait until a SSL handshake is completed.

  @param ssl    SSL structure for the connection.
  @param func   SSL handshake handler.

  @return Return value is 1 on success.
*/

static int ssl_handshake(SSL *ssl, ssl_handshake_func_t func)
{
  int ret= func(ssl);
  if (ret < 1)
    ssl_set_io_error(ssl, ret);

  return ret;
}


static int ssl_do(struct st_VioSSLFd *ptr, Vio **vio_ptr, long timeout,
                  ssl_handshake_func_t func, unsigned long *errptr)
{
  int r;
  SSL *ssl;
  Ssl_vio *ssl_vio;
  DBUG_ENTER("ssl_do");
  DBUG_PRINT("enter", ("ptr: %p, sd: %d  ctx: %p",
                       ptr, (int)(*vio_ptr)->fd(), ptr->ssl_context));


  if (!(ssl= SSL_new(static_cast<SSL_CTX *>(ptr->ssl_context))))
  {
    DBUG_PRINT("error", ("SSL_new failure"));
    *errptr= ERR_get_error();
    DBUG_RETURN(1);
  }
  DBUG_PRINT("info", ("ssl: %p timeout: %ld", ssl, timeout));
  SSL_clear(ssl);
  SSL_SESSION_set_timeout(SSL_get_session(ssl), timeout);
  if (!(ssl_vio= new (std::nothrow) Ssl_vio(*vio_ptr, ssl)))
  {
    SSL_free(ssl);
    DBUG_RETURN(1);
  }
  *vio_ptr= ssl_vio;

#ifdef HAVE_WOLFSSL
  /* wolfSSL invokes the VIO transport callbacks directly. */
  wolfSSL_SetIOReadCtx(ssl, ssl_vio);
  wolfSSL_SetIOWriteCtx(ssl, ssl_vio);
#else
  if (ssl_set_vio_bio(ssl, ssl_vio))
    DBUG_RETURN(1);
#endif

#if defined(SSL_OP_NO_COMPRESSION)
  SSL_set_options(ssl, SSL_OP_NO_COMPRESSION);
#endif

  if ((r= ssl_handshake(ssl, func)) < 1)
  {
    DBUG_PRINT("error", ("SSL_connect/accept failure"));
    *errptr= SSL_errno(ssl, r);
    DBUG_RETURN(1);
  }

#ifndef DBUG_OFF
  {
    /* Print some info about the peer */
    X509 *cert;
    char buf[512];

    DBUG_PRINT("info",("SSL connection succeeded"));
    DBUG_PRINT("info",("Using cipher: '%s'" , SSL_get_cipher_name(ssl)));

    if ((cert= SSL_get_peer_certificate (ssl)))
    {
      DBUG_PRINT("info",("Peer certificate:"));
      X509_NAME_oneline(X509_get_subject_name(cert), buf, sizeof(buf));
      DBUG_PRINT("info",("\t subject: '%s'", buf));
      X509_NAME_oneline(X509_get_issuer_name(cert), buf, sizeof(buf));
      DBUG_PRINT("info",("\t issuer: '%s'", buf));
      X509_free(cert);
    }
    else
      DBUG_PRINT("info",("Peer does not have certificate."));

    if (SSL_get_shared_ciphers(ssl, buf, sizeof(buf)))
    {
      DBUG_PRINT("info",("shared_ciphers: '%s'", buf));
    }
    else
      DBUG_PRINT("info",("no shared ciphers!"));
  }
#endif

  DBUG_RETURN(0);
}


int sslaccept(struct st_VioSSLFd *ptr, Vio **vio, long timeout,
              unsigned long *errptr)
{
  DBUG_ENTER("sslaccept");
  DBUG_RETURN(ssl_do(ptr, vio, timeout, SSL_accept, errptr));
}


int sslconnect(struct st_VioSSLFd *ptr, Vio **vio, long timeout,
               unsigned long *errptr)
{
  DBUG_ENTER("sslconnect");
  DBUG_RETURN(ssl_do(ptr, vio, timeout, SSL_connect, errptr));
}


my_bool Ssl_vio::has_data() const
{
  return SSL_pending(static_cast<SSL *>(m_ssl)) > 0 ||
         m_underlying->has_data();
}

ssize_t Ssl_vio::pending()
{
  int bytes= SSL_pending(static_cast<SSL *>(m_ssl));
  return bytes ? bytes : m_underlying->pending();
}

void *vio_ssl_handle(Vio *vio)
{
  return vio ? vio->ssl_handle() : nullptr;
}

#endif /* HAVE_OPENSSL */
