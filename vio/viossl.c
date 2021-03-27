/* Copyright (c) 2000, 2013, Oracle and/or its affiliates. All rights reserved.

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

#ifdef HAVE_OPENSSL

#define SSL_errno(X,Y) ERR_get_error()

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


/**
  Indicate whether a SSL I/O operation must be retried later.

  @param vio  VIO object representing a SSL connection.
  @param ret  Value returned by a SSL I/O function.
  @param event[out] The type of I/O event to wait/retry.
  @param should_wait[out] whether to wait for 'event'

  @return Whether a SSL I/O operation should be deferred.
  @retval TRUE    Temporary failure, retry operation.
  @retval FALSE   Indeterminate failure.
*/

static my_bool ssl_should_retry(Vio *vio, int ret, enum enum_vio_io_event *event, my_bool *should_wait)
{
  int ssl_error;
  SSL *ssl= vio->ssl_arg;
  my_bool should_retry= TRUE;

#if defined(ERR_LIB_X509) && defined(X509_R_CERT_ALREADY_IN_HASH_TABLE)
  /*
    Ignore error X509_R_CERT_ALREADY_IN_HASH_TABLE.
    This is a workaround for an OpenSSL bug in an older (< 1.1.1)
    OpenSSL version.
  */
  unsigned long err = ERR_peek_error();
  if (ERR_GET_LIB(err) == ERR_LIB_X509 &&
      ERR_GET_REASON(err) == X509_R_CERT_ALREADY_IN_HASH_TABLE)
  {
    ERR_clear_error();
    *should_wait= FALSE;
    return TRUE;
  }
#endif

  /* Retrieve the result for the SSL I/O operation. */
  ssl_error= SSL_get_error(ssl, ret);

  /* Retrieve the result for the SSL I/O operation. */
  switch (ssl_error)
  {
  case SSL_ERROR_WANT_READ:
    *event= VIO_IO_EVENT_READ;
    *should_wait= TRUE;
    break;
  case SSL_ERROR_WANT_WRITE:
    *event= VIO_IO_EVENT_WRITE;
    *should_wait= TRUE;
    break;
  default:
    should_retry= FALSE;
    *should_wait= FALSE;
    ssl_set_sys_error(ssl_error);
    ERR_clear_error();
    break;
  }

  return should_retry;
}


/**
  Handle SSL io error.

  @param[in] vio Vio
  @param[in] ret return from the failed IO operation

  @return  0 - should retry last read/write operation
           1 - some error has occured
*/
static int handle_ssl_io_error(Vio *vio, int ret)
{
  enum enum_vio_io_event event;
  my_bool should_wait;

  /* Process the SSL I/O error. */
  if (!ssl_should_retry(vio, ret, &event, &should_wait))
    return 1;

  if (!should_wait)
    return 1;

  /* Attempt to wait for an I/O event. */
  return vio_socket_io_wait(vio, event);
}


size_t vio_ssl_read(Vio *vio, uchar *buf, size_t size)
{
  int ret;
  SSL *ssl= vio->ssl_arg;
  DBUG_ENTER("vio_ssl_read");
  DBUG_PRINT("enter", ("sd: %d  buf: %p  size: %zu  ssl: %p",
		       (int)mysql_socket_getfd(vio->mysql_socket), buf, size,
                       vio->ssl_arg));


  while ((ret= SSL_read(ssl, buf, (int)size)) < 0)
  {
    if (handle_ssl_io_error(vio, ret))
      break;
  }

  DBUG_PRINT("exit", ("%d", ret));
  DBUG_RETURN(ret < 0 ? -1 : ret);

}


size_t vio_ssl_write(Vio *vio, const uchar *buf, size_t size)
{
  int ret;
  SSL *ssl= vio->ssl_arg;
  DBUG_ENTER("vio_ssl_write");
  DBUG_PRINT("enter", ("sd: %d  buf: %p  size: %zu",
                       (int)mysql_socket_getfd(vio->mysql_socket),
                       buf, size));
  while ((ret= SSL_write(ssl, buf, (int)size)) < 0)
  {
    if (handle_ssl_io_error(vio,ret))
      break;
  }

  DBUG_RETURN(ret < 0 ? -1 : ret);
}

int vio_ssl_close(Vio *vio)
{
  int r= 0;
  SSL *ssl= (SSL*)vio->ssl_arg;
  DBUG_ENTER("vio_ssl_close");

  if (ssl)
  {
    /*
    THE SSL standard says that SSL sockets must send and receive a close_notify
    alert on socket shutdown to avoid truncation attacks. However, this can
    cause problems since we often hold a lock during shutdown and this IO can
    take an unbounded amount of time to complete. Since our packets are self
    describing with length, we aren't vunerable to these attacks. Therefore,
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
  DBUG_RETURN(vio_close(vio));
}


void vio_ssl_delete(Vio *vio)
{
  if (!vio)
    return; /* It must be safe to delete null pointer */

  if (vio->type == VIO_TYPE_SSL)
    vio_ssl_close(vio); /* Still open, close connection first */

  if (vio->ssl_arg)
  {
    SSL_free((SSL*) vio->ssl_arg);
    vio->ssl_arg= 0;
  }

  vio_delete(vio);
}


/** SSL handshake handler. */
typedef int (*ssl_handshake_func_t)(SSL*);


/**
  Loop and wait until a SSL handshake is completed.

  @param vio    VIO object representing a SSL connection.
  @param ssl    SSL structure for the connection.
  @param func   SSL handshake handler.

  @return Return value is 1 on success.
*/

static int ssl_handshake_loop(Vio *vio, SSL *ssl, ssl_handshake_func_t func)
{
  int ret;

  vio->ssl_arg= ssl;

  /* Initiate the SSL handshake. */
  while ((ret= func(ssl)) < 1)
  {
    if (handle_ssl_io_error(vio,ret))
      break;
  }

  vio->ssl_arg= NULL;

  return ret;
}


static int ssl_do(struct st_VioSSLFd *ptr, Vio *vio, long timeout,
                  ssl_handshake_func_t func, unsigned long *errptr)
{
  int r;
  SSL *ssl;
  my_socket sd= mysql_socket_getfd(vio->mysql_socket);
  DBUG_ENTER("ssl_do");
  DBUG_PRINT("enter", ("ptr: %p, sd: %d  ctx: %p",
                       ptr, (int)sd, ptr->ssl_context));


  if (!(ssl= SSL_new(ptr->ssl_context)))
  {
    DBUG_PRINT("error", ("SSL_new failure"));
    *errptr= ERR_get_error();
    DBUG_RETURN(1);
  }
  DBUG_PRINT("info", ("ssl: %p timeout: %ld", ssl, timeout));
  SSL_clear(ssl);
  SSL_SESSION_set_timeout(SSL_get_session(ssl), timeout);
  SSL_set_fd(ssl, (int)sd);

#ifdef HAVE_WOLFSSL
  /* Set first argument of the transport functions. */
  wolfSSL_SetIOReadCtx(ssl, vio);
  wolfSSL_SetIOWriteCtx(ssl, vio);
#endif

#if defined(SSL_OP_NO_COMPRESSION)
  SSL_set_options(ssl, SSL_OP_NO_COMPRESSION);
#endif

  if ((r= ssl_handshake_loop(vio, ssl, func)) < 1)
  {
    DBUG_PRINT("error", ("SSL_connect/accept failure"));
    *errptr= SSL_errno(ssl, r);
    SSL_free(ssl);
    DBUG_RETURN(1);
  }

  /*
    Connection succeeded. Install new function handlers,
    change type, set sd to the fd used when connecting
    and set pointer to the SSL structure
  */
  if (vio_reset(vio, VIO_TYPE_SSL, SSL_get_fd(ssl), ssl, 0))
  {
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


int sslaccept(struct st_VioSSLFd *ptr, Vio *vio, long timeout, unsigned long *errptr)
{
  DBUG_ENTER("sslaccept");
  DBUG_RETURN(ssl_do(ptr, vio, timeout, SSL_accept, errptr));
}


int sslconnect(struct st_VioSSLFd *ptr, Vio *vio, long timeout, unsigned long *errptr)
{
  DBUG_ENTER("sslconnect");
  DBUG_RETURN(ssl_do(ptr, vio, timeout, SSL_connect, errptr));
}


int vio_ssl_blocking(Vio *vio __attribute__((unused)),
		     my_bool set_blocking_mode,
		     my_bool *old_mode)
{
  /* Mode is always blocking */
  *old_mode= 1;
  /* Return error if we try to change to non_blocking mode */
  return (set_blocking_mode ? 0 : 1);
}

my_bool vio_ssl_has_data(Vio *vio)
{
  return SSL_pending(vio->ssl_arg) > 0 ? TRUE : FALSE;
}

#endif /* HAVE_OPENSSL */
