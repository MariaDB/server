/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2012, 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/*
 * Vio Lite.
 * Purpose: include file for Vio that will work with C and C++
 */

#ifndef vio_violite_h_
#define	vio_violite_h_

#include "my_net.h"   /* needed because of struct in_addr */
#include <mysql/psi/mysql_socket.h>
/* C entry points implemented as thin wrappers over the C++ VIO hierarchy. */

#ifdef	__cplusplus
extern "C" {
#endif /* __cplusplus */

#ifdef __cplusplus
typedef struct st_vio Vio;
#endif /* __cplusplus */

enum enum_vio_type
{
  VIO_CLOSED, VIO_TYPE_TCPIP, VIO_TYPE_SOCKET, VIO_TYPE_NAMEDPIPE,
  VIO_TYPE_SSL
  /* see also vio_type_names[] */
};

enum enum_vio_state
{
  VIO_STATE_NOT_INITIALIZED, VIO_STATE_ACTIVE, VIO_STATE_SHUTDOWN,
  VIO_STATE_CLOSED
};

#define FIRST_VIO_TYPE VIO_CLOSED
#define LAST_VIO_TYPE VIO_TYPE_SSL

/**
  VIO I/O events.
*/
enum enum_vio_io_event
{
  VIO_IO_EVENT_READ,
  VIO_IO_EVENT_WRITE,
  VIO_IO_EVENT_CONNECT
};

struct vio_keepalive_opts
{
  int interval;
  int idle;
  int probes;
};


#define VIO_TLSv1_0   1
#define VIO_TLSv1_1   2
#define VIO_TLSv1_2   4
#define VIO_TLSv1_3   8

#define VIO_LOCALHOST 1U                        /* a localhost connection */
#define VIO_BUFFERED_READ 2U                    /* use buffered read */
#define VIO_READ_BUFFER_SIZE 16384U             /* size of read buffer */
#define VIO_DESCRIPTION_SIZE 30                 /* size of description */

Vio* vio_new(my_socket sd, enum enum_vio_type type, uint flags);
Vio*  mysql_socket_vio_new(MYSQL_SOCKET mysql_socket, enum enum_vio_type type, uint flags);
#ifdef _WIN32
Vio* vio_new_win32pipe(HANDLE hPipe);
#else
#define HANDLE void *
#endif /* _WIN32 */

void	vio_delete(Vio* vio);
int	vio_close(Vio* vio);
size_t	vio_read(Vio *vio, uchar *	buf, size_t size);
size_t	vio_write(Vio *vio, const uchar * buf, size_t size);
/* setsockopt TCP_NODELAY at IPPROTO_TCP level, when possible */
int vio_nodelay(Vio *vio, my_bool on);
/* setsockopt SO_KEEPALIVE at SOL_SOCKET level, when possible */
int	vio_keepalive(Vio *vio, my_bool	onoff);
int	vio_set_keepalive_options(Vio * vio, const struct vio_keepalive_opts *opts);
/*
  Whether the last read/write failure (SOCKET_EINTR) is worth retrying.
  Always FALSE on Windows: there, SOCKET_EINTR only means an intentional
  cross-thread cancellation, never a transient signal interruption.
*/
my_bool	vio_should_retry(Vio *vio);
/*
  Return whether the most recent failed VIO I/O operation in the current
  thread failed because of a timeout. Meaningful only immediately after an
  operation reports failure.
*/
my_bool vio_was_timeout(Vio *vio);
/* Short text description of the socket for those, who are curious.. */
const char* vio_description(Vio *vio);
/* Return the type of the connection */
enum enum_vio_type vio_type(Vio* vio);
/*
  Return the transport type of the connection.
  Will differ from vio_type for SSL connections.
*/
enum enum_vio_type vio_transport_type(Vio *vio);

/* Return last error number */
int	vio_errno(Vio*vio);
/* Get socket number */
my_socket vio_fd(Vio*vio);
/* Remote peer's address and name in text form */
my_bool vio_peer_addr(Vio *vio, char *buf, uint16 *port, size_t buflen);
/* Wait for an I/O event notification. */
int vio_io_wait(Vio *vio, enum enum_vio_io_event event, int timeout);
my_bool vio_is_connected(Vio *vio);
my_bool vio_has_data(Vio *vio);
int vio_shutdown(Vio *vio, int how);
ssize_t vio_pending(Vio *vio);
/* Set timeout for a network operation. */
extern int vio_timeout(Vio *vio, uint which, int timeout_sec);
extern int vio_timeout_ms(Vio *vio, uint which, int timeout_ms);
extern int vio_get_timeout_ms(Vio *vio, uint which);
extern void vio_set_wait_callback(void (*before_wait)(void),
                                void (*after_wait)(void));
/* Connect to a peer. */
my_bool vio_socket_connect(Vio *vio, struct sockaddr *addr, socklen_t len,
                           int timeout);

void vio_get_normalized_ip(const struct sockaddr *src, size_t src_length, struct sockaddr *dst);

my_bool vio_get_normalized_ip_string(const struct sockaddr *addr, size_t addr_length,
                                     char *ip_string, size_t ip_string_size);

my_bool vio_is_no_name_error(int err_code);

int vio_getnameinfo(const struct sockaddr *sa,
                    char *hostname, size_t hostname_size,
                    char *port, size_t port_size,
                    int flags);

#ifdef HAVE_OPENSSL
enum enum_ssl_init_error
{
  SSL_INITERR_NOERROR= 0, SSL_INITERR_CERT, SSL_INITERR_KEY,
  SSL_INITERR_NOMATCH, SSL_INITERR_BAD_PATHS, SSL_INITERR_CIPHERS,
  SSL_INITERR_MEMFAIL, SSL_INITERR_DH, SSL_INITERR_PROTOCOL,
  SSL_INITERR_LASTERR
};
const char* sslGetErrString(enum enum_ssl_init_error err);

struct st_VioSSLFd
{
  /* Opaque SSL context; implementation files may cast it to SSL_CTX*. */
  void *ssl_context;
};

int sslaccept(struct st_VioSSLFd*, Vio **, long timeout, unsigned long *errptr);
int sslconnect(struct st_VioSSLFd*, Vio **, long timeout, unsigned long *errptr);
/* Opaque SSL state (castable to SSL*), or nullptr if SSL isn't active. */
void *vio_ssl_handle(Vio *vio);

void vio_check_ssl_init();

struct st_VioSSLFd
*new_VioSSLConnectorFd(const char *key_file, const char *cert_file,
		       const char *ca_file,  const char *ca_path,
		       const char *cipher, enum enum_ssl_init_error *error,
                       const char *crl_file, const char *crl_path);
struct st_VioSSLFd
*new_VioSSLAcceptorFd(const char *key_file, const char *cert_file,
		      const char *ca_file,const char *ca_path,
		      const char *cipher, enum enum_ssl_init_error *error,
		      const char *crl_file, const char *crl_path,
		      ulonglong tls_version, const char *passphrase);
void free_vio_ssl_acceptor_fd(struct st_VioSSLFd *fd);
#endif /* HAVE_OPENSSL */

void vio_end(void);

enum enum_vio_state vio_state(Vio *vio);
my_bool vio_is_local(Vio *vio);
struct sockaddr_storage *vio_remote_addr(Vio *vio);

/*
  The returned pointer is owned by the VIO transport. For socket transports
  it points to the real MYSQL_SOCKET; for non-socket transports (e.g.
  Windows named pipe) it points to an invalid, uninstrumented dummy
  MYSQL_SOCKET.
*/
MYSQL_SOCKET *vio_mysql_socket_ptr(Vio *vio);

#ifdef _WIN32
/*
  Get the underlying HANDLE for a Vio
  For socket transport, this is the underlying SOCKET handle.
  For named pipe, this is the underlying HANDLE to the pipe.
*/
HANDLE vio_handle(Vio *vio);
#endif

const char *vio_type_name(enum enum_vio_type vio_type, size_t *len);

#ifdef	__cplusplus
}
#endif

#ifdef _WIN32

/* shutdown(2) flags */
#ifndef SHUT_RD
#define SHUT_RD SD_RECEIVE
#endif

#endif

/* This enumerator is used in parser - should be always visible */
enum SSL_type
{
  SSL_TYPE_NOT_SPECIFIED= -1,
  SSL_TYPE_NONE,
  SSL_TYPE_ANY,
  SSL_TYPE_X509,
  SSL_TYPE_SPECIFIED
};

#endif /* vio_violite_h_ */
