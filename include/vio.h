/* Copyright (c) 2026, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA */

#pragma once

#include <new>
#include <violite.h>

/*
  C++ VIO interface. C translation units see only the incomplete
  struct tag and operate on it through the API declared in violite.h.
*/
struct st_vio
{
public:
  st_vio()= default;
  st_vio(const st_vio &)= delete;
  st_vio &operator=(const st_vio &)= delete;
  virtual ~st_vio()= default;

  /*
    VIO allocations go through my_malloc() so they are accounted with
    key_memory_vio. VIO objects are allocated with new (std::nothrow).
  */
  static void *operator new(size_t size, const std::nothrow_t&) noexcept;
  static void operator delete(void *ptr, const std::nothrow_t&) noexcept;
  static void operator delete(void *ptr) noexcept;

  /* Last error in errno/GetLastError() form. */
  virtual int error() const= 0;
  /* Read bytes; returns bytes read, 0 on EOF, or (size_t)-1 on error. */
  virtual size_t read(uchar *buf, size_t size)= 0;
  /* Write bytes; returns bytes written or (size_t)-1 on error. */
  virtual size_t write(const uchar *buf, size_t size)= 0;
  /* Set a read (which=0) or write (which=1) timeout in milliseconds. */
  virtual int set_timeout(uint which, int timeout_ms)= 0;
  /* Close this VIO layer and, where applicable, the underlying VIO. */
  virtual int close()= 0;
  /* True if the VIO still refers to a live connection. */
  virtual my_bool is_connected()= 0;
  /* Shut down reading and/or writing. */
  virtual int shutdown(int how)= 0;
  /* True if data can be read without blocking. */
  virtual my_bool has_data() const= 0;
  /* Bytes available without blocking, or -1 on error. */
  virtual ssize_t pending()= 0;
  /* VIO_TYPE_* of this layer. */
  virtual enum enum_vio_type type() const= 0;
  /* Current lifecycle state. */
  virtual enum enum_vio_state state() const= 0;
  /* Short human-readable description. */
  virtual const char *description() const= 0;
  /* Socket descriptor, or an invalid dummy value for non-socket transports. */
  virtual my_socket fd() const= 0;
  /* Read (which=0) or write (which=1) timeout in milliseconds. */
  virtual int get_timeout(uint which) const= 0;
  /* True if the peer is local. */
  virtual my_bool is_local() const= 0;
  /* Wrapped VIO, or nullptr for a transport. */
  virtual st_vio *underlying() const= 0;
  /* Opaque TLS implementation state, or nullptr if TLS is not active. */
  virtual void *ssl_handle() const= 0;
#ifdef _WIN32
  /* Underlying Windows socket or named-pipe handle. */
  virtual HANDLE handle() const= 0;
#endif
};

/* Base for VIO layers that delegate to an underlying VIO. */
class Vio_filter : public Vio
{
  friend Vio *vio_wrap(Vio *, Vio_filter *);
  friend Vio *vio_wrap_transport(Vio *, Vio_filter *);
  friend Vio *vio_unwrap(Vio_filter *);

protected:
  /* Owned by this filter and destroyed by its destructor. */
  Vio *m_underlying;

  explicit Vio_filter(Vio *underlying= nullptr)
    : m_underlying(underlying)
  {
  }

public:
  ~Vio_filter() override;

  /* Underlying VIO wrapped by this filter. */
  Vio *underlying() const override
  {
    return m_underlying;
  }

  int error() const override
  {
    return m_underlying->error();
  }

  size_t read(uchar *buf, size_t size) override
  {
    return m_underlying->read(buf, size);
  }

  size_t write(const uchar *buf, size_t size) override
  {
    return m_underlying->write(buf, size);
  }

  int set_timeout(uint which, int timeout_ms) override
  {
    return m_underlying->set_timeout(which, timeout_ms);
  }

  int close() override
  {
    return m_underlying->close();
  }

  my_bool is_connected() override
  {
    return m_underlying->is_connected();
  }

  int shutdown(int how) override
  {
    return m_underlying->shutdown(how);
  }

  my_bool has_data() const override
  {
    return m_underlying->has_data();
  }

  ssize_t pending() override
  {
    return m_underlying->pending();
  }

  enum enum_vio_type type() const override
  {
    return m_underlying->type();
  }

  enum enum_vio_state state() const override
  {
    return m_underlying->state();
  }

  const char *description() const override
  {
    return m_underlying->description();
  }

  my_socket fd() const override
  {
    return m_underlying->fd();
  }

  int get_timeout(uint which) const override
  {
    return m_underlying->get_timeout(which);
  }

  my_bool is_local() const override
  {
    return m_underlying->is_local();
  }

  void *ssl_handle() const override
  {
    return m_underlying->ssl_handle();
  }
#ifdef _WIN32
  HANDLE handle() const override
  {
    return m_underlying->handle();
  }
#endif
};

/*
  Make filter the outermost layer above vio.

  Takes ownership of both objects and returns filter.
*/
Vio *vio_wrap(Vio *vio, Vio_filter *filter);

/*
  Undo vio_wrap(): detach and return filter's underlying VIO, releasing
  ownership of it. Used to unwind a partially-constructed filter without
  destroying the VIO it was about to wrap.
*/
Vio *vio_unwrap(Vio_filter *filter);

/* Terminal transport at the bottom of the VIO stack. */
Vio *vio_get_transport(Vio *vio);
const Vio *vio_get_transport(const Vio *vio);

/*
  Insert filter immediately above the terminal transport in vio.

  Takes ownership of filter and preserves the existing outer layers.
*/
Vio *vio_wrap_transport(Vio *vio, Vio_filter *filter);
