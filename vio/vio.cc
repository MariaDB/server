/* Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.
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

/*
  Note that we can't have assertion on file descriptors;  The reason for
  this is that during mysql shutdown, another thread can close a file
  we are working on.  In this case we should just return read errors from
  file descriptor.
*/

#include "vio_priv.h"
#include <ssl_compat.h>
#include <new>

extern "C" {
PSI_memory_key key_memory_vio_ssl_fd;
PSI_memory_key key_memory_vio;
}

static MYSQL_SOCKET transport_dummy_socket= MYSQL_INVALID_SOCKET;

#ifdef HAVE_PSI_INTERFACE
static PSI_memory_info all_vio_memory[]=
{
  {&key_memory_vio_ssl_fd, "ssl_fd", 0},
  {&key_memory_vio, "vio", 0},
};

void init_vio_psi_keys()
{
  const char* category= "vio";
  int count;

  count= array_elements(all_vio_memory);
  mysql_memory_register(category, all_vio_memory, count);
}
#endif

void *st_vio::operator new(size_t size, const std::nothrow_t&) noexcept
{
  return my_malloc(key_memory_vio, size, MYF(MY_WME));
}

void st_vio::operator delete(void *ptr, const std::nothrow_t&) noexcept
{
  my_free(ptr);
}

void st_vio::operator delete(void *ptr) noexcept
{
  my_free(ptr);
}

MYSQL_SOCKET *Vio_transport::mysql_socket_ptr()
{
  return &transport_dummy_socket;
}

Vio_transport::Vio_transport(enum enum_vio_type type, uint flags,
                           const char *description)
  : m_localhost(flags & VIO_LOCALHOST), m_type(type),
    m_state(VIO_STATE_ACTIVE), m_description(description),
    m_read_timeout(-1), m_write_timeout(-1)
{
  memset(&m_local, 0, sizeof(m_local));
  memset(&m_remote, 0, sizeof(m_remote));
}

namespace {

class Vio_client_readahead final : public Vio_filter
{
  uchar m_buffer[VIO_READ_BUFFER_SIZE];
  size_t m_pos{0};
  size_t m_end{0};

public:
  explicit Vio_client_readahead(Vio *underlying)
    : Vio_filter(underlying)
  {
  }

  size_t read(uchar *buf, size_t size) override
  {
    size_t available= m_end - m_pos;
    if (available)
    {
      size_t count= size < available ? size : available;
      memcpy(buf, m_buffer + m_pos, count);
      m_pos+= count;
      return count;
    }
    if (size < VIO_READ_BUFFER_SIZE / 8)
    {
      size_t result= m_underlying->read(m_buffer, sizeof(m_buffer));
      if (!result || result == (size_t) -1)
        return result;
      size_t count= size < result ? size : result;
      memcpy(buf, m_buffer, count);
      m_pos= count;
      m_end= result;
      return count;
    }
    return m_underlying->read(buf, size);
  }

  my_bool has_data() const override
  {
    return m_pos != m_end || m_underlying->has_data();
  }
};

} /* anonymous namespace */


/* Create a new VIO for socket or TCP/IP connection. */

Vio *mysql_socket_vio_new(MYSQL_SOCKET mysql_socket, enum enum_vio_type type, uint flags)
{
  DBUG_ENTER("mysql_socket_vio_new");
  Socket_vio *socket_vio=
    new (std::nothrow) Socket_vio(mysql_socket, type, flags);
  if (!socket_vio)
    DBUG_RETURN(nullptr);
  DBUG_PRINT("enter", ("sd: %d", (int) socket_vio->fd()));
#if defined(__linux__) && !defined(DBUG_OFF)
  DBUG_ASSERT(socket_vio->fd() < 0 ||
              (fcntl(socket_vio->fd(), F_GETFL, 0) & O_NONBLOCK));
#endif
  Vio *vio= socket_vio;
  if (flags & VIO_BUFFERED_READ)
  {
    Vio *readahead= new (std::nothrow) Vio_client_readahead(vio);
    if (readahead)
      vio= readahead;
    /* On allocation failure, fall back to the unbuffered vio as-is. */
  }
  DBUG_RETURN(vio);
}

/* Create a VIO for an existing socket. */

Vio *vio_new(my_socket sd, enum enum_vio_type type, uint flags)
{
  Vio *vio;
  MYSQL_SOCKET mysql_socket= MYSQL_INVALID_SOCKET;
  DBUG_ENTER("vio_new");
  DBUG_PRINT("enter", ("sd: %d", (int)sd));

  mysql_socket_setfd(&mysql_socket, sd);
  vio = mysql_socket_vio_new(mysql_socket, type, flags);

  DBUG_RETURN(vio);
}

#ifdef _WIN32

Vio *vio_new_win32pipe(HANDLE hPipe)
{
  DBUG_ENTER("vio_new_handle");
  Named_pipe_vio *vio=
    new (std::nothrow) Named_pipe_vio(hPipe);
  if (vio && !vio->valid())
  {
    delete vio;
    vio= nullptr;
  }
  DBUG_RETURN(vio);
}


#endif


/**
  Set timeout for a network send or receive operation.

  @remark A negative timeout means an infinite timeout.

  @param vio      A VIO object.
  @param which    Whether timeout is for send (1) or receive (0).
  @param timeout  Timeout interval in seconds.

  @return FALSE on success, TRUE otherwise.
*/

int vio_timeout(Vio *vio, uint which, int timeout_sec)
{
  int timeout_ms;

  /*
    Vio timeouts are measured in milliseconds. Check for a possible
    overflow. In case of overflow, set to infinite.
  */
  if (timeout_sec > INT_MAX/1000)
    timeout_ms= -1;
  else
    timeout_ms= (int) (timeout_sec * 1000);

  return vio_timeout_ms(vio, which, timeout_ms);
}

int vio_timeout_ms(Vio *vio, uint which, int timeout_ms)
{
  return vio->set_timeout(which, timeout_ms);
}

int vio_get_timeout_ms(Vio *vio, uint which)
{
  return vio->get_timeout(which);
}


void vio_delete(Vio* vio)
{
  if (!vio)
    return; /* It must be safe to delete null pointers. */

  vio->close();
  delete vio;
}


/*
  Cleanup memory allocated by vio or the
  components below it when application finish

*/
void vio_end(void)
{
#ifdef HAVE_WOLFSSL
  wolfSSL_Cleanup();
#else
  // This one is needed on the client side
  ERR_remove_state(0);
  ERR_free_strings();
  EVP_cleanup();
  CRYPTO_cleanup_all_ex_data();
#endif
}
