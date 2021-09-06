/* Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2012, 2020, MariaDB Corporation.

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

/* Simple vio interface in C;  The functions are implemented in violite.c */

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
my_bool vio_reset(Vio* vio, enum enum_vio_type type,
                  my_socket sd, void *ssl, uint flags);
size_t	vio_read(Vio *vio, uchar *	buf, size_t size);
size_t  vio_read_buff(Vio *vio, uchar * buf, size_t size);
size_t	vio_write(Vio *vio, const uchar * buf, size_t size);
int	vio_blocking(Vio *vio, my_bool onoff, my_bool *old_mode);
my_bool	vio_is_blocking(Vio *vio);
/* setsockopt TCP_NODELAY at IPPROTO_TCP level, when possible */
int vio_nodelay(Vio *vio, my_bool on);
int	vio_fastsend(Vio *vio);
/* setsockopt SO_KEEPALIVE at SOL_SOCKET level, when possible */
int	vio_keepalive(Vio *vio, my_bool	onoff);
int	vio_set_keepalive_options(Vio * vio, const struct vio_keepalive_opts *opts);
/* Whenever we should retry the last read/write operation. */
my_bool	vio_should_retry(Vio *vio);
/* Check that operation was timed out */
my_bool vio_was_timeout(Vio *vio);
/* Short text description of the socket for those, who are curious.. */
const char* vio_description(Vio *vio);
/* Return the type of the connection */
enum enum_vio_type vio_type(Vio* vio);
/* Return last error number */
int	vio_errno(Vio*vio);
/* Get socket number */
my_socket vio_fd(Vio*vio);
/* Remote peer's address and name in text form */
my_bool vio_peer_addr(Vio *vio, char *buf, uint16 *port, size_t buflen);
/* Wait for an I/O event notification. */
int vio_io_wait(Vio *vio, enum enum_vio_io_event event, int timeout);
my_bool vio_is_connected(Vio *vio);
ssize_t vio_pending(Vio *vio);
/* Set timeout for a network operation. */
extern int vio_timeout(Vio *vio, uint which, int timeout_sec);
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
/* apple deprecated openssl in MacOSX Lion */
#ifdef __APPLE__
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

#define HEADER_DES_LOCL_H dummy_something
#define YASSL_MYSQL_COMPATIBLE
#ifndef YASSL_PREFIX
#define YASSL_PREFIX
#endif
/* Set yaSSL to use same type as MySQL do for socket handles */
typedef my_socket YASSL_SOCKET_T;
#define YASSL_SOCKET_T_DEFINED
#define template _template /* bug in WolfSSL 4.4.0, see also my_crypt.cc */
#include <openssl/ssl.h>
#undef template
#include <openssl/err.h>
#ifdef DEPRECATED
#undef DEPRECATED
#endif

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
  SSL_CTX *ssl_context;
};

int sslaccept(struct st_VioSSLFd*, Vio *, long timeout, unsigned long *errptr);
int sslconnect(struct st_VioSSLFd*, Vio *, long timeout, unsigned long *errptr);

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
		      ulonglong tls_version);
void free_vio_ssl_acceptor_fd(struct st_VioSSLFd *fd);
#endif /* HAVE_OPENSSL */

void vio_end(void);

const char *vio_type_name(enum enum_vio_type vio_type, size_t *len);

#ifdef	__cplusplus
}
#endif

#if !defined(DONT_MAP_VIO)
#define vio_delete(vio) 			(vio)->viodelete(vio)
#define vio_errno(vio)	 			(vio)->vioerrno(vio)
#define vio_read(vio, buf, size)                ((vio)->read)(vio,buf,size)
#define vio_write(vio, buf, size)               ((vio)->write)(vio, buf, size)
#define vio_blocking(vio, set_blocking_mode, old_mode)\
 	(vio)->vioblocking(vio, set_blocking_mode, old_mode)
#define vio_is_blocking(vio) 			(vio)->is_blocking(vio)
#define vio_fastsend(vio)			(vio)->fastsend(vio)
#define vio_keepalive(vio, set_keep_alive)	(vio)->viokeepalive(vio, set_keep_alive)
#define vio_should_retry(vio) 			(vio)->should_retry(vio)
#define vio_was_timeout(vio)                    (vio)->was_timeout(vio)
#define vio_close(vio)				((vio)->vioclose)(vio)
#define vio_shutdown(vio,how)			((vio)->shutdown)(vio,how)
#define vio_peer_addr(vio, buf, prt, buflen)	(vio)->peer_addr(vio, buf, prt, buflen)
#define vio_io_wait(vio, event, timeout)        (vio)->io_wait(vio, event, timeout)
#define vio_is_connected(vio)                   (vio)->is_connected(vio)
#endif /* !defined(DONT_MAP_VIO) */

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

/* HFTODO - hide this if we don't want client in embedded server */
/* This structure is for every connection on both sides */
struct st_vio
{
  MYSQL_SOCKET  mysql_socket;     /* Instrumented socket */
  my_bool		localhost;	/* Are we from localhost? */
  int			fcntl_mode;	/* Buffered fcntl(sd,F_GETFL) */
  struct sockaddr_storage local;	/* Local internet address */
  struct sockaddr_storage remote;	/* Remote internet address */
  enum enum_vio_type	type;		/* Type of connection */
  const char		*desc;		/* String description */
  char                  *read_buffer;   /* buffer for vio_read_buff */
  char                  *read_pos;      /* start of unfetched data in the
                                           read buffer */
  char                  *read_end;      /* end of unfetched data */
  int                   read_timeout;   /* Timeout value (ms) for read ops. */
  int                   write_timeout;  /* Timeout value (ms) for write ops. */
  /* function pointers. They are similar for socket/SSL/whatever */
  void    (*viodelete)(Vio*);
  int     (*vioerrno)(Vio*);
  size_t  (*read)(Vio*, uchar *, size_t);
  size_t  (*write)(Vio*, const uchar *, size_t);
  int     (*timeout)(Vio*, uint, my_bool);
  int     (*vioblocking)(Vio*, my_bool, my_bool *);
  my_bool (*is_blocking)(Vio*);
  int     (*viokeepalive)(Vio*, my_bool);
  int     (*fastsend)(Vio*);
  my_bool (*peer_addr)(Vio*, char *, uint16*, size_t);
  void    (*in_addr)(Vio*, struct sockaddr_storage*);
  my_bool (*should_retry)(Vio*);
  my_bool (*was_timeout)(Vio*);
  int     (*vioclose)(Vio*);
  my_bool (*is_connected)(Vio*);
  int (*shutdown)(Vio *, int);
  my_bool (*has_data) (Vio*);
  int (*io_wait)(Vio*, enum enum_vio_io_event, int);
  my_bool (*connect)(Vio*, struct sockaddr *, socklen_t, int);
#ifdef HAVE_OPENSSL
  void	  *ssl_arg;
#endif
#ifdef _WIN32
  HANDLE hPipe;
  OVERLAPPED overlapped;
  int shutdown_flag;
  void *tp_ctx; /* threadpool context */
#endif
};
#endif /* vio_violite_h_ */
