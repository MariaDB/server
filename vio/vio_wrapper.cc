/* Copyright (c) 2026, MariaDB Corporation.

   Thin C API wrappers for the C++ VIO hierarchy.
*/

#include "vio_priv.h"

static thread_local my_bool last_vio_io_was_timeout;

void vio_set_was_timeout(my_bool was_timeout)
{
  last_vio_io_was_timeout= was_timeout;
}

Vio_filter::~Vio_filter()
{
  delete m_underlying;
}

Vio *vio_wrap(Vio *vio, Vio_filter *filter)
{
  DBUG_ASSERT(vio);
  DBUG_ASSERT(filter);
  filter->m_underlying= vio;
  return filter;
}

Vio *vio_get_transport(Vio *vio)
{
  while (vio->underlying())
    vio= vio->underlying();
  return vio;
}

const Vio *vio_get_transport(const Vio *vio)
{
  while (vio->underlying())
    vio= vio->underlying();
  return vio;
}

Vio *vio_wrap_transport(Vio *vio, Vio_filter *filter)
{
  Vio_filter *parent= nullptr;
  Vio *transport= vio;

  DBUG_ASSERT(vio);
  DBUG_ASSERT(filter);

  while (transport->underlying())
  {
    parent= static_cast<Vio_filter *>(transport);
    transport= transport->underlying();
  }

  filter->m_underlying= transport;
  if (!parent)
    return filter;

  parent->m_underlying= filter;
  return vio;
}

Vio_transport *vio_transport(Vio *vio)
{
  return static_cast<Vio_transport *>(vio_get_transport(vio));
}

const Vio_transport *vio_transport(const Vio *vio)
{
  return static_cast<const Vio_transport *>(vio_get_transport(vio));
}

static Socket_vio *socket_vio(Vio *vio)
{
  Vio_transport *transport= vio_transport(vio);
  enum enum_vio_type type= transport->type();

  if (type != VIO_TYPE_TCPIP && type != VIO_TYPE_SOCKET)
    return nullptr;
  return static_cast<Socket_vio *>(transport);
}

static Socket_vio *tcp_vio(Vio *vio)
{
  Socket_vio *socket= socket_vio(vio);

  return socket && socket->type() == VIO_TYPE_TCPIP ? socket : nullptr;
}

extern "C" {

Vio *vio_get_underlying(Vio *vio)
{
  return vio ? vio->underlying() : nullptr;
}

int vio_errno(Vio *vio)
{
  return vio->error();
}

size_t vio_read(Vio *vio, uchar *buf, size_t size)
{
  return vio->read(buf, size);
}

size_t vio_write(Vio *vio, const uchar *buf, size_t size)
{
  return vio->write(buf, size);
}

int vio_nodelay(Vio *vio, my_bool on)
{
  Socket_vio *socket= tcp_vio(vio);
  return socket ? socket->nodelay(on) : 0;
}

int vio_keepalive(Vio *vio, my_bool on)
{
  Socket_vio *socket= tcp_vio(vio);
  return socket ? socket->keepalive(on) : 0;
}

int vio_set_keepalive_options(Vio *vio,
                              const struct vio_keepalive_opts *opts)
{
  Socket_vio *socket= tcp_vio(vio);
  return socket ? socket->set_keepalive_options(opts) : 0;
}

my_bool vio_should_retry(Vio *vio)
{
#ifdef _WIN32
  /*
    On Windows, SOCKET_EINTR (WSAEINTR) is never a transient signal
    interruption worth retrying -- it is produced only by an intentional
    cross-thread cancellation (shutdown()+CancelIoEx(), see
    Socket_vio::shutdown()), and must be treated as a hard failure.
  */
  return FALSE;
#else
  return vio_errno(vio) == SOCKET_EINTR;
#endif
}

my_bool vio_was_timeout(Vio *vio)
{
  (void) vio;
  return last_vio_io_was_timeout;
}

int vio_shutdown(Vio *vio, int how)
{
  return vio->shutdown(how);
}

const char *vio_description(Vio *vio)
{
  return vio->description();
}

enum enum_vio_type vio_type(Vio *vio)
{
  return vio->type();
}

my_socket vio_fd(Vio *vio)
{
  return vio->fd();
}

my_bool vio_peer_addr(Vio *vio, char *buf, uint16 *port, size_t buflen)
{
  Socket_vio *socket= socket_vio(vio);
  return socket ? socket->peer_addr(buf, port, buflen) : FALSE;
}

my_bool vio_is_connected(Vio *vio)
{
  return vio->is_connected();
}

my_bool vio_has_data(Vio *vio)
{
  return vio->has_data();
}

ssize_t vio_pending(Vio *vio)
{
  return vio->pending();
}

my_bool vio_socket_connect(Vio *vio, struct sockaddr *addr,
                           socklen_t len, int timeout)
{
  Socket_vio *socket= socket_vio(vio);
  return socket ? socket->connect(addr, len, timeout) : TRUE;
}

enum enum_vio_state vio_state(Vio *vio)
{
  return vio->state();
}

my_bool vio_is_local(Vio *vio)
{
  return vio->is_local();
}

struct sockaddr_storage *vio_remote_addr(Vio *vio)
{
  return vio_transport(vio)->remote_addr();
}

enum enum_vio_type vio_transport_type(Vio* vio)
{
  return vio_transport(vio)->type();
}

MYSQL_SOCKET *vio_mysql_socket_ptr(Vio *vio)
{
  return vio_transport(vio)->mysql_socket_ptr();
}

#ifdef _WIN32
HANDLE vio_handle(Vio *vio)
{
  return vio->handle();
}
#endif

} /* extern "C" */
