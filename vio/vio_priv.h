/* Copyright (c) 2003, 2011, Oracle and/or its affiliates.
   Copyright (c) 2012, 2026, MariaDB Corporation

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

#ifndef VIO_PRIV_INCLUDED
#define VIO_PRIV_INCLUDED

/* Structures and functions private to the vio package */

#define DONT_MAP_VIO
#include <my_global.h>
#include <mysql_com.h>
#include <my_sys.h>
#include <m_string.h>
#ifdef __cplusplus
#include <vio.h>
#else
#include <violite.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif
extern PSI_memory_key key_memory_vio;
extern PSI_memory_key key_memory_vio_ssl_fd;
/* Underlying VIO in a filter chain, or nullptr for a transport. */
Vio *vio_get_underlying(Vio *vio);
/* Notify the scheduler around waits which can block the current worker. */
void vio_wait_begin(int timeout);
void vio_wait_end(int timeout);
#ifdef __cplusplus
}
#endif

#ifdef __cplusplus

/* Record how the current thread's most recent failed VIO operation failed. */
void vio_set_was_timeout(my_bool was_timeout);

/*
  Base for VIO layers backed by a real OS transport (socket or named pipe).
  Unlike Vio_filter, a Vio_transport has no underlying VIO of its own.
*/
class Vio_transport : public Vio
{
protected:
  my_bool m_localhost;
  struct sockaddr_storage m_local;
  struct sockaddr_storage m_remote;
  enum enum_vio_type m_type;
  enum enum_vio_state m_state;
  const char *m_description;
  int m_read_timeout;
  int m_write_timeout;

  Vio_transport(enum enum_vio_type type, uint flags, const char *description);

public:
  enum enum_vio_type type() const override
  {
    return m_type;
  }

  enum enum_vio_state state() const override
  {
    return m_state;
  }

  const char *description() const override
  {
    return m_description;
  }

  int get_timeout(uint which) const override
  {
    return which ? m_write_timeout : m_read_timeout;
  }

  my_bool is_local() const override
  {
    return m_localhost;
  }

  struct sockaddr_storage *remote_addr()
  {
    return &m_remote;
  }

  /* A transport is the bottom of the VIO chain. */
  Vio *underlying() const override
  {
    return nullptr;
  }

  /*
    Non-socket transports return a pointer to an invalid, uninstrumented
    dummy MYSQL_SOCKET (e.g. Windows named pipe).
  */
  virtual MYSQL_SOCKET *mysql_socket_ptr();

  /* SSL is applied via an Ssl_vio filter above this. */
  void *ssl_handle() const override
  {
    return nullptr;
  }
};

/* A VIO backed by a TCP/IP or Unix domain socket. */
class Socket_vio final : public Vio_transport
{
  MYSQL_SOCKET m_socket;
  /* io_wait() using this VIO's own read/write timeout for event. */
  int io_wait_default(enum enum_vio_io_event event);
  /* TCP sockets are always non-blocking; Unix domain sockets are local. */
  int set_nonblocking();
  /* Peek the socket to distinguish EOF from data being available. */
  my_bool peek_read(uint *bytes);

public:
  Socket_vio(MYSQL_SOCKET socket, enum enum_vio_type type, uint flags);
  int error() const override;
  size_t read(uchar *buf, size_t size) override;
  size_t write(const uchar *buf, size_t size) override;
  int set_timeout(uint which, int timeout_ms) override;
  int keepalive(my_bool on);
  my_bool peer_addr(char *buf, uint16 *port, size_t buflen);
  int close() override;
  my_bool is_connected() override;
  int shutdown(int how) override;
  my_bool has_data() const override
  {
    return FALSE;
  }
  ssize_t pending() override;
  int io_wait(enum_vio_io_event event, int timeout) override;
  my_socket fd() const override
  {
    return mysql_socket_getfd(m_socket);
  }

  MYSQL_SOCKET *mysql_socket_ptr() override
  {
    return &m_socket;
  }
#ifdef _WIN32
  HANDLE handle() const override
  {
    return reinterpret_cast<HANDLE>(fd());
  }
#endif
  int nodelay(my_bool on);
  int set_keepalive_options(const struct vio_keepalive_opts *opts);
  my_bool connect(struct sockaddr *addr, socklen_t len, int timeout);
};

#ifdef _WIN32
/* A VIO backed by a Windows named pipe, using overlapped I/O. */
class Named_pipe_vio final : public Vio_transport
{
  HANDLE m_pipe;
  OVERLAPPED m_overlapped;
  DWORD m_overlapped_result;
  int m_shutdown_flag;

public:
  explicit Named_pipe_vio(HANDLE pipe);
  ~Named_pipe_vio() override;
  bool valid() const
  {
    return m_overlapped.hEvent != nullptr;
  }

  int error() const override
  {
    return GetLastError();
  }
  size_t read(uchar *buf, size_t size) override;
  size_t write(const uchar *buf, size_t size) override;
  int set_timeout(uint which, int timeout_ms) override;
  int close() override;
  my_bool is_connected() override;
  int shutdown(int how) override;
  my_bool has_data() const override
  {
    return FALSE;
  }
  ssize_t pending() override;
  int io_wait(enum_vio_io_event event, int timeout) override;
  my_socket fd() const override
  {
    return INVALID_SOCKET;
  }

  HANDLE handle() const override
  {
    return m_pipe;
  }
};
#endif

#ifdef HAVE_OPENSSL
/* A VIO filter that speaks TLS over its underlying VIO via m_ssl (SSL*). */
class Ssl_vio final : public Vio_filter
{
  void *m_ssl;

public:
  /* Take ownership of ssl (an already-configured SSL*) and underlying. */
  Ssl_vio(Vio *underlying, void *ssl);
  ~Ssl_vio() override;
  size_t read(uchar *buf, size_t size) override;
  size_t write(const uchar *buf, size_t size) override;
  int close() override;
  my_bool has_data() const override;
  ssize_t pending() override;
  enum enum_vio_type type() const override
  {
    return VIO_TYPE_SSL;
  }

  void *ssl_handle() const override
  {
    return m_ssl;
  }
};
#endif

/* Real transport at the bottom of vio's filter chain. */
Vio_transport *vio_transport(Vio *vio);
/* Real transport at the bottom of vio's filter chain. */
const Vio_transport *vio_transport(const Vio *vio);

#endif /* __cplusplus */
#endif /* VIO_PRIV_INCLUDED */
