/*
   Copyright (c) 2001, 2012, Oracle and/or its affiliates
   Copyright (c) 2012, 2026, MariaDB Corporation

   This program is free software; you can redistribute it and/or
   modify it under the terms of the GNU General Public License
   as published by the Free Software Foundation; version 2 of
   the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
   02110-1335  USA */

/*
  Note that we can't have assertion on file descriptors;  The reason for
  this is that during mysql shutdown, another thread can close a file
  we are working on.  In this case we should just return read errors from
  file descriptor.
*/

#include "vio_priv.h"
#ifdef _WIN32
  #include <winsock2.h>
  #include <MSWSock.h>
  #include <mstcpip.h>
  #pragma comment(lib, "ws2_32.lib")
#endif

#ifdef FIONREAD_IN_SYS_FILIO
# include <sys/filio.h>
#endif

/* I/O wait callbacks for performance accounting and the threadpool. */
static void (*before_io_wait)(void)= 0;
static void (*after_io_wait)(void)= 0;

void vio_wait_begin(int timeout)
{
  if (timeout && before_io_wait)
    before_io_wait();
}

void vio_wait_end(int timeout)
{
  if (timeout && after_io_wait)
    after_io_wait();
}

/* Wait callback macros (both performance schema and threadpool */
#define START_SOCKET_WAIT(locker, state_ptr, sock, which, timeout) \
do                                                        \
{                                                         \
  MYSQL_START_SOCKET_WAIT(locker, state_ptr, sock,        \
                            which, 0);                    \
  vio_wait_begin(timeout);                                \
} while(0)


#define END_SOCKET_WAIT(locker,timeout)                  \
do                                                       \
{                                                        \
  MYSQL_END_SOCKET_WAIT(locker, 0);                      \
  vio_wait_end(timeout);                                 \
} while(0)



void vio_set_wait_callback(void (*before_wait)(void),
                                void (*after_wait)(void))
{
  before_io_wait= before_wait;
  after_io_wait= after_wait;
}

int Socket_vio::error() const
{
  /* Mapped to WSAGetLastError() on Win32. */
  return socket_errno;
}

Socket_vio::Socket_vio(MYSQL_SOCKET socket,
                                enum enum_vio_type type, uint flags)
  : Vio_transport(type, flags, type == VIO_TYPE_SOCKET ? "socket" : "TCP/IP"),
    m_socket(socket)
{
  /* Set socket to non-blocking for client/connection sockets, but not
     for listener sockets which handle accepts(). */
  if (set_nonblocking())
  {
    /*
      The only reason it would fail is something like EBADF.
      Reason to abort, even in non-debug builds.
    */
    fprintf(stderr, "FATAL: setting socket to non-blocking mode failed, "
            "socket %d, error %d\n", (int) fd(), error());
    abort();
  }

  /* Ask for low-latency delivery (was vio_fastsend(), only ever called
     once, right after creating the Vio -- no need for it to be a
     separate, externally-callable operation). */
  if (m_type == VIO_TYPE_TCPIP)
  {
#if defined(IPTOS_THROUGHPUT)
    {
      int tos= IPTOS_THROUGHPUT;
      mysql_socket_setsockopt(m_socket, IPPROTO_IP, IP_TOS,
                              (void *) &tos, sizeof(tos));
    }
#endif /* IPTOS_THROUGHPUT */
    nodelay(TRUE);
  }
}

int Socket_vio::set_nonblocking()
{
#ifdef _WIN32
  u_long arg= 1;
  return ioctlsocket(fd(), FIONBIO, &arg);
#else
  int flags= fcntl(fd(), F_GETFL, 0);
  if (flags < 0)
    return -1;
  return fcntl(fd(), F_SETFL, flags | O_NONBLOCK);
#endif
}

static int vio_set_linger(my_socket s, unsigned short timeout_sec)
{
  struct linger s_linger;
  int ret;
  s_linger.l_onoff = 1;
  s_linger.l_linger = timeout_sec;
  ret = setsockopt(s, SOL_SOCKET, SO_LINGER, (const char *)&s_linger, (int)sizeof(s_linger));
  return ret;
}


/**
  Attempt to wait for an I/O event on a socket.

  @param vio      VIO object representing a connected socket.
  @param event    The type of I/O event (read or write) to wait for.

  @return Return value is -1 on failure, 0 on success.
*/

int Socket_vio::io_wait_default(enum enum_vio_io_event event)
{
  int timeout, ret;

  DBUG_ASSERT(event == VIO_IO_EVENT_READ || event == VIO_IO_EVENT_WRITE);

  /* Choose an appropriate timeout. */
  if (event == VIO_IO_EVENT_READ)
    timeout= m_read_timeout;
  else
    timeout= m_write_timeout;

  /* Wait for input data to become available. */
  int wait_result= io_wait(event, timeout);
  if (wait_result < 0)
    ret= -1;
  else if (!wait_result)
  {
    ret= -1;
    vio_set_linger(m_socket.fd, 0);
  }
  else
    ret= 0;

  return ret;
}


#ifndef SOCKET_EAGAIN
#define SOCKET_EAGAIN SOCKET_EWOULDBLOCK
#endif

/*
  returns number of bytes read or -1 in case of an error
*/
size_t Socket_vio::read(uchar *buf, size_t size)
{
  ssize_t ret;
  DBUG_ENTER("vio_read");
  DBUG_PRINT("enter", ("sd: %d  buf: %p  size: %zu",
                       (int)mysql_socket_getfd(m_socket), buf,
                       size));
  while ((ret= mysql_socket_recv(m_socket, (SOCKBUF_T *)buf, size,
                                  0)) == -1)
  {
    int error= socket_errno;

    /* The operation would block? */
    if (error != SOCKET_EAGAIN && error != SOCKET_EWOULDBLOCK)
    {
      vio_set_was_timeout(FALSE);
      break;
    }

    /* Wait for input data to become available. */
    if ((ret= io_wait_default(VIO_IO_EVENT_READ)))
      break;
  }
  if (ret == 0)
    vio_set_was_timeout(FALSE);
#ifndef DBUG_OFF
  if (ret == -1)
  {
    DBUG_PRINT("vio_error", ("Got error %d during read", errno));
  }
#ifndef DEBUG_DATA_PACKETS
  else
  {
    DBUG_DUMP("read_data", buf, ret);
  }
#endif /* DEBUG_DATA_PACKETS */
#endif /* DBUG_OFF */
  DBUG_PRINT("exit", ("%d", (int) ret));
  DBUG_RETURN(ret);
}


size_t Socket_vio::write(const uchar* buf, size_t size)
{
  ssize_t ret;
  DBUG_ENTER("vio_write");
  DBUG_PRINT("enter", ("sd: %d  buf: %p  size: %zu",
                       (int)mysql_socket_getfd(m_socket), buf,
                       size));
  while ((ret= mysql_socket_send(m_socket, (SOCKBUF_T *)buf, size,
                                  0)) == -1)
  {
    int error= socket_errno;

    /* The operation would block? */
    if (error != SOCKET_EAGAIN && error != SOCKET_EWOULDBLOCK)
    {
      vio_set_was_timeout(FALSE);
      break;
    }

    /* Wait for the output buffer to become writable.*/
    if ((ret= io_wait_default(VIO_IO_EVENT_WRITE)))
      break;
  }
  if (ret == 0 && size)
    vio_set_was_timeout(FALSE);
#ifndef DBUG_OFF
  if (ret == -1)
  {
    DBUG_PRINT("vio_error", ("Got error on write: %d",socket_errno));
  }
#endif /* DBUG_OFF */
  DBUG_PRINT("exit", ("%d", (int) ret));
  DBUG_RETURN(ret);
}

int Socket_vio::shutdown(int how)
{
  int ret;
  DBUG_ENTER("vio_socket_shutdown");
  DBUG_PRINT("enter", ("sd: %d", (int)mysql_socket_getfd(m_socket)));

  m_state= VIO_STATE_SHUTDOWN;
  ret= ::shutdown(mysql_socket_getfd(m_socket), how);

#ifdef  _WIN32
  /* Cancel possible IO in progress (shutdown does not do that on Windows). */
  (void) CancelIoEx((HANDLE)mysql_socket_getfd(m_socket), NULL);
#endif
  DBUG_RETURN(ret);
}


int Socket_vio::set_timeout(uint which, int timeout_ms)
{
  if (which)
    m_write_timeout= timeout_ms;
  else
    m_read_timeout= timeout_ms;
  return 0;
}

/* Set TCP_NODELAY (disable Nagle's algorithm */
int Socket_vio::nodelay(my_bool on)
{
  int r;
  int no_delay= MY_TEST(on);
  DBUG_ENTER("vio_nodelay");

  if (m_type == VIO_TYPE_SOCKET)
  {
    DBUG_RETURN(0);
  }

  r = mysql_socket_setsockopt(m_socket, IPPROTO_TCP, TCP_NODELAY,
    IF_WIN((const char*), (void*)) &no_delay,
    sizeof(no_delay));

  if (r)
  {
    DBUG_PRINT("warning",
     ("Couldn't set socket option for fast send, error %d",
      socket_errno));
     r = -1;
  }
  DBUG_PRINT("exit", ("%d", r));
  DBUG_RETURN(r);
}

int Socket_vio::keepalive(my_bool set_keep_alive)
{
  int r=0;
  uint opt = 0;
  DBUG_ENTER("vio_keepalive");
  DBUG_PRINT("enter", ("sd: %d  set_keep_alive: %d",
                       (int)mysql_socket_getfd(m_socket),
                       (int)set_keep_alive));

  if (set_keep_alive)
    opt = 1;
  r = mysql_socket_setsockopt(m_socket, SOL_SOCKET, SO_KEEPALIVE,
                              (char *)&opt, sizeof(opt));
  DBUG_RETURN(r);
}

/*
  Set socket options for keepalive e.g., TCP_KEEPCNT, TCP_KEEPIDLE/TCP_KEEPALIVE, TCP_KEEPINTVL
*/
int Socket_vio::set_keepalive_options(
  const struct vio_keepalive_opts *opts)
{
#if defined _WIN32
  struct tcp_keepalive s;
  DWORD  nbytes;

  if (!opts->idle && !opts->interval)
    return 0;

  s.onoff= 1;
  s.keepalivetime= opts->idle? opts->idle * 1000 : 7200;
  s.keepaliveinterval= opts->interval?opts->interval * 1000 : 1;

  return WSAIoctl(m_socket.fd, SIO_KEEPALIVE_VALS, (LPVOID) &s, sizeof(s),
           NULL, 0, &nbytes, NULL, NULL);

#elif defined (TCP_KEEPIDLE) || defined (TCP_KEEPALIVE)

  int ret= 0;
  if (opts->idle)
  {
#ifdef TCP_KEEPIDLE // Linux only
    ret= mysql_socket_setsockopt(m_socket, IPPROTO_TCP, TCP_KEEPIDLE, (char *)&opts->idle, sizeof(opts->idle));
#elif defined (TCP_KEEPALIVE)
    ret= mysql_socket_setsockopt(m_socket, IPPROTO_TCP, TCP_KEEPALIVE, (char *)&opts->idle, sizeof(opts->idle));
#endif
    if(ret)
      return ret;
  }

#ifdef TCP_KEEPCNT // Linux only
  if(opts->probes)
  {
    ret= mysql_socket_setsockopt(m_socket, IPPROTO_TCP, TCP_KEEPCNT, (char *)&opts->probes, sizeof(opts->probes));
    if(ret)
      return ret;
  }
#endif

#ifdef TCP_KEEPINTVL  // Linux only
  if(opts->interval)
  {
    ret= mysql_socket_setsockopt(m_socket, IPPROTO_TCP, TCP_KEEPINTVL, (char *)&opts->interval, sizeof(opts->interval));
  }
#endif
  return ret;
#else /*TCP_KEEPIDLE || TCP_KEEPALIVE */
  return -1;
#endif
}


int Socket_vio::close()
{
  DBUG_ENTER("vio_close");
  DBUG_PRINT("enter", ("sd: %d", (int)mysql_socket_getfd(m_socket)));

  if (m_type != VIO_CLOSED)
  {
    MYSQL_SOCKET mysql_socket= m_socket;
    DBUG_ASSERT(m_type ==  VIO_TYPE_TCPIP ||
                m_type == VIO_TYPE_SOCKET);


    m_type= VIO_CLOSED;
    m_state= VIO_STATE_CLOSED;
    m_socket= MYSQL_INVALID_SOCKET;

    DBUG_ASSERT(mysql_socket_getfd(mysql_socket) >= 0);
    if (mysql_socket_close(mysql_socket))
    {
      DBUG_PRINT("vio_error", ("close() failed, error: %d",socket_errno));
      /* FIXME: error handling (not critical for MySQL) */
      DBUG_RETURN(-1);
    }
  }
  DBUG_RETURN(0);
}


static const LEX_CSTRING vio_type_names[] =
{
  { STRING_WITH_LEN("") }, // internal threads
  { STRING_WITH_LEN("TCP/IP") },
  { STRING_WITH_LEN("Socket") },
  { STRING_WITH_LEN("Named Pipe") },
  { STRING_WITH_LEN("SSL/TLS") },
  { STRING_WITH_LEN("Shared Memory") }
};

const char *vio_type_name(enum enum_vio_type vio_type, size_t *len)
{
  int index= vio_type >= FIRST_VIO_TYPE && vio_type <= LAST_VIO_TYPE
             ?  vio_type : 0;

  *len= vio_type_names[index].length;
  return vio_type_names[index].str;
}


/**
  Convert a sock-address (AF_INET or AF_INET6) into the "normalized" form,
  which is the IPv4 form for IPv4-mapped or IPv4-compatible IPv6 addresses.

  @note Background: when IPv4 and IPv6 are used simultaneously, IPv4
  addresses may be written in a form of IPv4-mapped or IPv4-compatible IPv6
  addresses. That means, one address (a.b.c.d) can be written in three forms:
    - IPv4: a.b.c.d;
    - IPv4-compatible IPv6: ::a.b.c.d;
    - IPv4-mapped IPv4: ::ffff:a.b.c.d;

  Having three forms of one address makes it a little difficult to compare
  addresses with each other (the IPv4-compatible IPv6-address of foo.bar
  will be different from the IPv4-mapped IPv6-address of foo.bar).

  @note This function can be made public when it's needed.

  @param src        [in] source IP address (AF_INET or AF_INET6).
  @param src_length [in] length of the src.
  @param dst        [out] a buffer to store normalized IP address
                          (sockaddr_storage).
  @param dst_length [out] optional - actual length of the normalized IP address.
*/

void vio_get_normalized_ip(const struct sockaddr *src, size_t src_length,
                                  struct sockaddr *dst)
{
  switch (src->sa_family) {
  case AF_INET:
    memcpy(dst, src, src_length);
    break;

#ifdef HAVE_IPV6
  case AF_INET6:
  {
    const struct sockaddr_in6 *src_addr6= (const struct sockaddr_in6 *) src;
    const struct in6_addr *src_ip6= &(src_addr6->sin6_addr);
    const uint32 *src_ip6_int32= (uint32 *) src_ip6->s6_addr;

    if (IN6_IS_ADDR_V4MAPPED(src_ip6) || IN6_IS_ADDR_V4COMPAT(src_ip6))
    {
      struct sockaddr_in *dst_ip4= (struct sockaddr_in *) dst;

      /*
        This is an IPv4-mapped or IPv4-compatible IPv6 address. It should
        be converted to the IPv4 form.
      */

      memset(dst_ip4, 0, sizeof (struct sockaddr_in));
      dst_ip4->sin_family= AF_INET;
      dst_ip4->sin_port= src_addr6->sin6_port;

      /*
        In an IPv4 mapped or compatible address, the last 32 bits represent
        the IPv4 address. The byte orders for IPv6 and IPv4 addresses are
        the same, so a simple copy is possible.
      */
      dst_ip4->sin_addr.s_addr= src_ip6_int32[3];
    }
    else
    {
      /* This is a "native" IPv6 address. */
      memcpy(dst, src, src_length);
    }

    break;
  }
#endif /* HAVE_IPV6 */
  }
}


/**
  Return the normalized IP address string for a sock-address.

  The idea is to return an IPv4-address for an IPv4-mapped and
  IPv4-compatible IPv6 address.

  The function writes the normalized IP address to the given buffer.
  The buffer should have enough space, otherwise error flag is returned.
  The system constant INET6_ADDRSTRLEN can be used to reserve buffers of
  the right size.

  @param addr           [in]  sockaddr object (AF_INET or AF_INET6).
  @param addr_length    [in]  length of the addr.
  @param ip_string      [out] buffer to write normalized IP address.
  @param ip_string_size [in]  size of the ip_string.

  @return Error status.
  @retval TRUE in case of error (the ip_string buffer is not enough).
  @retval FALSE on success.
*/

my_bool vio_get_normalized_ip_string(const struct sockaddr *addr, size_t addr_length,
                                     char *ip_string,
                                     size_t ip_string_size)
{
  struct sockaddr_storage norm_addr_storage;
  struct sockaddr *norm_addr= (struct sockaddr *) &norm_addr_storage;
  int err_code;

  vio_get_normalized_ip(addr, addr_length, norm_addr);

  err_code= vio_getnameinfo(norm_addr, ip_string, ip_string_size, NULL, 0,
                            NI_NUMERICHOST);

  if (!err_code)
    return FALSE;

  DBUG_PRINT("error", ("getnameinfo() failed with %d (%s).",
                       (int) err_code,
                       (const char *) gai_strerror(err_code)));
  return TRUE;
}


/**
  Return IP address and port of a VIO client socket.

  The function returns an IPv4 address if IPv6 support is disabled.

  The function returns an IPv4 address if the client socket is associated
  with an IPv4-compatible or IPv4-mapped IPv6 address. Otherwise, the native
  IPv6 address is returned.
*/

my_bool Socket_vio::peer_addr(char *ip_buffer, uint16 *port,
                                       size_t ip_buffer_size)
{
  DBUG_ENTER("vio_peer_addr");
  DBUG_PRINT("enter", ("Client socked fd: %d",
            (int)mysql_socket_getfd(m_socket)));

  if (m_localhost)
  {
    /*
      Initialize m_remote and vio->addLen. Set m_remote to IPv4 loopback
      address.
    */
    struct in_addr *ip4= &((struct sockaddr_in *) &(m_remote))->sin_addr;
    m_remote.ss_family= AF_INET;

    ip4->s_addr= htonl(INADDR_LOOPBACK);

    /* Initialize ip_buffer and port. */

    strmov(ip_buffer, "127.0.0.1");
    *port= 0;
  }
  else
  {
    int err_code;
    char port_buffer[NI_MAXSERV];

    struct sockaddr_storage addr_storage;
    struct sockaddr *addr= (struct sockaddr *) &addr_storage;
    size_socket addr_length= sizeof (addr_storage);
    /* Get sockaddr by socked fd. */

    err_code= mysql_socket_getpeername(m_socket, addr, &addr_length);

    if (err_code)
    {
      DBUG_PRINT("exit", ("getpeername() gave error: %d", socket_errno));
      DBUG_RETURN(TRUE);
    }

    /* Normalize IP address. */

    vio_get_normalized_ip(addr, addr_length,
                          (struct sockaddr *) &m_remote);

    /* Get IP address & port number. */

    err_code= vio_getnameinfo((struct sockaddr *) &m_remote,
                              ip_buffer, ip_buffer_size,
                              port_buffer, NI_MAXSERV,
                              NI_NUMERICHOST | NI_NUMERICSERV);

    if (err_code)
    {
      DBUG_PRINT("exit", ("getnameinfo() gave error: %s",
                          gai_strerror(err_code)));
      DBUG_RETURN(TRUE);
    }

    *port= (uint16) strtol(port_buffer, NULL, 10);
  }

  DBUG_PRINT("exit", ("Client IP address: %s; port: %d",
                      (const char *) ip_buffer,
                      (int) *port));
  DBUG_RETURN(FALSE);
}


/**
  Retrieve the amount of data that can be read from a socket.

  @param vio          A VIO object.
  @param bytes[out]   The amount of bytes available.

  @retval FALSE   Success.
  @retval TRUE    Failure.
*/
// WL#4896: Not covered

my_bool Socket_vio::peek_read(uint *bytes)
{
  my_socket sd= mysql_socket_getfd(m_socket);
#if defined(_WIN32)
  u_long len;
  if (ioctlsocket(sd, FIONREAD, &len))
    return TRUE;
  *bytes= len;
  return FALSE;
#elif defined(FIONREAD_IN_SYS_IOCTL) || defined(FIONREAD_IN_SYS_FILIO)
  int len;
  if (ioctl(sd, FIONREAD, &len) < 0)
    return TRUE;
  *bytes= len;
  return FALSE;
#else
  char buf[1024];
  ssize_t res= recv(sd, &buf, sizeof(buf), MSG_PEEK);
  if (res < 0)
    return TRUE;
  *bytes= res;
  return FALSE;
#endif /*_WIN32*/
}

#ifndef _WIN32

/**
  Set of event flags grouped by operations.
*/

/*
  Linux specific flag used to detect connection shutdown. The flag is
  also used for half-closed notification, which here is interpreted as
  if there is data available to be read from the socket.
*/
#ifndef POLLRDHUP
#define POLLRDHUP 0
#endif

/* Data may be read. */
#define MY_POLL_SET_IN      (POLLIN | POLLPRI)
/* Data may be written. */
#define MY_POLL_SET_OUT     (POLLOUT)
/* An error or hangup. */
#define MY_POLL_SET_ERR     (POLLERR | POLLHUP | POLLNVAL)

#endif /* _WIN32 */

/**
  Wait for an I/O event on a VIO socket.

  @param vio      VIO object representing a connected socket.
  @param event    The type of I/O event to wait for.
  @param timeout  Interval (in milliseconds) to wait for an I/O event.
                  A negative timeout value means an infinite timeout.

  @remark socket_errno is set to SOCKET_ETIMEDOUT on timeout.

  @return A three-state value which indicates the operation status.
  @retval -1  Failure, socket_errno indicates the error.
  @retval  0  The wait has timed out.
  @retval  1  The requested I/O event has occurred.
*/

static int remaining_timeout(int timeout, ulonglong start)
{
  if (timeout < 0)
    return -1;
  ulonglong elapsed_ms= (my_hrtime().val - start + 999) / 1000;
  return elapsed_ms >= (ulonglong) timeout ?
         0 : timeout - (int) elapsed_ms;
}

#ifndef _WIN32
int Socket_vio::io_wait(enum enum_vio_io_event event, int timeout)
{
  int ret, wait_timeout= timeout;
  ulonglong start= timeout >= 0 ? my_hrtime().val : 0;
  short revents __attribute__((unused)) = 0;
  struct pollfd pfd;
  my_socket sd= mysql_socket_getfd(m_socket);
  MYSQL_SOCKET_WAIT_VARIABLES(locker, state) /* no ';' */
  DBUG_ENTER("vio_io_wait");
  DBUG_PRINT("enter", ("sd: %d  timeout: %d",
                       (int) mysql_socket_getfd(m_socket),
                       timeout));

  DBUG_ASSERT(m_state != VIO_STATE_CLOSED);
  memset(&pfd, 0, sizeof(pfd));

  pfd.fd= sd;

  /*
    Set the poll bitmask describing the type of events.
    The error flags are only valid in the revents bitmask.
  */
  switch (event)
  {
  case VIO_IO_EVENT_READ:
    pfd.events= MY_POLL_SET_IN;
    revents= MY_POLL_SET_IN | MY_POLL_SET_ERR | POLLRDHUP;
    break;
  case VIO_IO_EVENT_WRITE:
  case VIO_IO_EVENT_CONNECT:
    pfd.events= MY_POLL_SET_OUT;
    revents= MY_POLL_SET_OUT | MY_POLL_SET_ERR;
    break;
  }

  START_SOCKET_WAIT(locker, &state, m_socket, PSI_SOCKET_SELECT, timeout);
  /*
    Wait for the I/O event and return early in case of
    error or timeout.
  */
  do
  {
    ret= poll(&pfd, 1, wait_timeout);
    if (ret != -1 || error() != SOCKET_EINTR)
      break;
    wait_timeout= remaining_timeout(timeout, start);
    if (!wait_timeout)
    {
      ret= 0;
      break;
    }
  } while (true);

  switch (ret)
  {
  case -1:
    DBUG_PRINT("error", ("poll returned -1  errno: %d", error()));
    /* On error, -1 is returned. */
    vio_set_was_timeout(FALSE);
    break;
  case 0:
    /*
      Set errno to indicate a timeout error.
      (This is not compiled in on WIN32.)
    */
    DBUG_PRINT("info", ("poll timeout"));
    errno= SOCKET_ETIMEDOUT;
    vio_set_was_timeout(TRUE);
    break;
  default:
    /* Ensure that the requested I/O event has completed. */
    DBUG_ASSERT(pfd.revents & revents);
    break;
  }

  END_SOCKET_WAIT(locker, timeout);
  DBUG_RETURN(ret);
}

#else

int Socket_vio::io_wait(enum enum_vio_io_event event, int timeout)
{
  int ret;
  struct timeval tm;
  my_socket fd= mysql_socket_getfd(m_socket);
  fd_set readfds, writefds, exceptfds;
  MYSQL_SOCKET_WAIT_VARIABLES(locker, state) /* no ';' */
  DBUG_ENTER("vio_io_wait");
  DBUG_ASSERT(m_state != VIO_STATE_CLOSED);
  if (timeout >= 0)
  {
    tm.tv_sec= timeout / 1000;
    tm.tv_usec= (timeout % 1000) * 1000;
  }

  FD_ZERO(&readfds);
  FD_ZERO(&writefds);
  FD_ZERO(&exceptfds);
  FD_SET(fd, &exceptfds);
  if (event == VIO_IO_EVENT_READ)
    FD_SET(fd, &readfds);
  else
    FD_SET(fd, &writefds);

  START_SOCKET_WAIT(locker, &state, m_socket, PSI_SOCKET_SELECT, timeout);

  /* The first argument is ignored on Windows. Unlike POSIX, a Windows
     select() interrupted by a cross-thread shutdown()/CancelIoEx() must
     NOT be silently retried here -- that would swallow the very signal
     used to wake this wait (see terminate_slave_thread()). */
  ret= select(0, &readfds, &writefds, &exceptfds,
              (timeout >= 0) ? &tm : NULL);

  END_SOCKET_WAIT(locker, timeout);

  /* Set error code to indicate a timeout error. */
  if (ret == 0)
  {
    WSASetLastError(SOCKET_ETIMEDOUT);
    vio_set_was_timeout(TRUE);
  }
  else if (ret < 0)
    vio_set_was_timeout(FALSE);

  /* Error or timeout? */
  if (ret <= 0)
    DBUG_RETURN(ret);

  /* The requested I/O event is ready? */
  switch (event)
  {
  case VIO_IO_EVENT_READ:
    ret= MY_TEST(FD_ISSET(fd, &readfds));
    break;
  case VIO_IO_EVENT_WRITE:
  case VIO_IO_EVENT_CONNECT:
    ret= MY_TEST(FD_ISSET(fd, &writefds));
    break;
  }

  /* Error conditions pending? */
  ret|= MY_TEST(FD_ISSET(fd, &exceptfds));

  /* Not a timeout, ensure that a condition was met. */
  DBUG_ASSERT(ret);

  DBUG_RETURN(ret);
}

#endif /* _WIN32 */


/**
  Connect to a peer address.

  @param vio       A VIO object.
  @param addr      Socket address containing the peer address.
  @param len       Length of socket address.
  @param timeout   Interval (in milliseconds) to wait until a
                   connection is established.

  @retval FALSE   A connection was successfully established.
  @retval TRUE    A fatal error. See socket_errno.
*/

my_bool
Socket_vio::connect(struct sockaddr *addr, socklen_t len, int timeout)
{
  int ret, wait;
  DBUG_ENTER("vio_socket_connect");

  /* Only for socket-based transport types. */
  DBUG_ASSERT(m_type == VIO_TYPE_SOCKET || m_type == VIO_TYPE_TCPIP);

  /* Initiate the connection (the socket is already non-blocking). */
  ret= mysql_socket_connect(m_socket, addr, len);

#ifdef _WIN32
  wait= (ret == SOCKET_ERROR) &&
        (WSAGetLastError() == WSAEINPROGRESS ||
         WSAGetLastError() == WSAEWOULDBLOCK);
#else
  wait= (ret == -1) && (errno == EINPROGRESS || errno == EALREADY);
#endif

  /*
    The connection is in progress. The vio_io_wait() call can be used
    to wait up to a specified period of time for the connection to
    succeed.

    If vio_io_wait() returns 0 (after waiting however many seconds),
    the socket never became writable (host is probably unreachable.)
    Otherwise, if vio_io_wait() returns 1, then one of two conditions
    exist:

    1. An error occurred. Use getsockopt() to check for this.
    2. The connection was set up successfully: getsockopt() will
       return 0 as an error.
  */
  if (wait && (io_wait(VIO_IO_EVENT_CONNECT, timeout) == 1))
  {
    int error;
    IF_WIN(int, socklen_t) optlen= sizeof(error);
    IF_WIN(char, void) *optval= (IF_WIN(char, void) *) &error;

    /*
      At this point, we know that something happened on the socket.
      But this does not means that everything is alright. The connect
      might have failed. We need to retrieve the error code from the
      socket layer. We must return success only if we are sure that
      it was really a success. Otherwise we might prevent the caller
      from trying another address to connect to.
    */
    if (!(ret= mysql_socket_getsockopt(m_socket, SOL_SOCKET,
                                       SO_ERROR, optval, &optlen)))
    {
#ifdef _WIN32
      WSASetLastError(error);
#else
      errno= error;
#endif
      ret= MY_TEST(error);
    }
  }

  DBUG_RETURN(MY_TEST(ret));
}


/**
  Determine if the endpoint of a connection is still available.

  @remark The socket is assumed to be disconnected if an EOF
          condition is encountered.

  @param vio      The VIO object.

  @retval TRUE    EOF condition not found.
  @retval FALSE   EOF condition is signaled.
*/

my_bool Socket_vio::is_connected()
{
  uint bytes= 0;
  DBUG_ENTER("vio_is_connected");
  DBUG_ASSERT(m_state != VIO_STATE_CLOSED);

  /*
    The first step of detecting an EOF condition is verifying
    whether there is data to read. Data in this case would be
    the EOF. An exceptional condition event and/or errors are
    interpreted as if there is data to read.
  */

  if (!io_wait(VIO_IO_EVENT_READ, 0))
    DBUG_RETURN(TRUE);

  /*
    The second step is read() or recv() from the socket returning
    0 (EOF). Unfortunately, it's not possible to call read directly
    as we could inadvertently read meaningful connection data.
    Simulate a read by retrieving the number of bytes available to
    read -- 0 meaning EOF. In the presence of unrecoverable errors,
    the socket is assumed to be disconnected.
  */
  while (peek_read(&bytes))
  {
#ifdef _WIN32
    /*
      SOCKET_EINTR (WSAEINTR) on Windows is never a transient interruption
      worth retrying -- it signals an intentional cross-thread cancellation
      (see Socket_vio::shutdown()) and must be treated as a hard failure.
    */
    DBUG_RETURN(FALSE);
#else
    if (socket_errno != SOCKET_EINTR)
      DBUG_RETURN(FALSE);
#endif
  }

  DBUG_RETURN(bytes ? TRUE : FALSE);
}


/**
  Number of bytes in the read or socket buffer

  @remark An EOF condition might count as one readable byte.

  @return number of bytes in one of the buffers or < 0 if error.
*/

ssize_t Socket_vio::pending()
{
  uint bytes= 0;

  if (peek_read(&bytes))
    return -1;
  return bytes;
}


/**
  Checks if the error code, returned by vio_getnameinfo(), means it was the
  "No-name" error.

  Windows-specific note: getnameinfo() returns WSANO_DATA instead of
  EAI_NODATA or EAI_NONAME when no reverse mapping is available at the host
  (i.e. Windows can't get hostname by IP-address). This error should be
  treated as EAI_NONAME.

  @return if the error code is actually EAI_NONAME.
  @retval true if the error code is EAI_NONAME.
  @retval false otherwise.
*/

my_bool vio_is_no_name_error(int err_code)
{
#ifdef _WIN32

  return err_code == WSANO_DATA || err_code == EAI_NONAME;

#else

  return err_code == EAI_NONAME;

#endif
}


/**
  This is a wrapper for the system getnameinfo(), because different OS
  differ in the getnameinfo() implementation:
    - Solaris 10 requires that the 2nd argument (salen) must match the
      actual size of the struct sockaddr_storage passed to it;
    - Mac OS X has sockaddr_in::sin_len and sockaddr_in6::sin6_len and
      requires them to be filled.
*/

int vio_getnameinfo(const struct sockaddr *sa,
                    char *hostname, size_t hostname_size,
                    char *port, size_t port_size,
                    int flags)
{
  int sa_length= 0;

  switch (sa->sa_family) {
  case AF_INET:
    sa_length= sizeof (struct sockaddr_in);
#ifdef HAVE_SOCKADDR_IN_SIN_LEN
    ((struct sockaddr_in *) sa)->sin_len= sa_length;
#endif /* HAVE_SOCKADDR_IN_SIN_LEN */
    break;

#ifdef HAVE_IPV6
  case AF_INET6:
    sa_length= sizeof (struct sockaddr_in6);
# ifdef HAVE_SOCKADDR_IN6_SIN6_LEN
    ((struct sockaddr_in6 *) sa)->sin6_len= sa_length;
# endif /* HAVE_SOCKADDR_IN6_SIN6_LEN */
    break;
#endif /* HAVE_IPV6 */
  }

  return getnameinfo(sa, sa_length,
                     hostname, (uint)hostname_size,
                     port, (uint)port_size,
                     flags);
}
