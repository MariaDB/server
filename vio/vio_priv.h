/* Copyright (c) 2003, 2011, Oracle and/or its affiliates.
   Copyright (c) 2012, Monty Program Ab

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
#include <violite.h>

extern PSI_memory_key key_memory_vio;
extern PSI_memory_key key_memory_vio_read_buffer;
extern PSI_memory_key key_memory_vio_ssl_fd;

#ifdef _WIN32
size_t vio_read_pipe(Vio *vio, uchar * buf, size_t size);
size_t vio_write_pipe(Vio *vio, const uchar * buf, size_t size);
my_bool vio_is_connected_pipe(Vio *vio);
int vio_close_pipe(Vio * vio);
int cancel_io(HANDLE handle, DWORD thread_id);
int vio_shutdown_pipe(Vio *vio,int how);
uint vio_pending_pipe(Vio* vio);
#endif


int	vio_socket_shutdown(Vio *vio, int how);
my_bool	vio_buff_has_data(Vio *vio);
int	vio_socket_io_wait(Vio *vio, enum enum_vio_io_event event);
int	vio_socket_timeout(Vio *vio, uint which, my_bool old_mode);

#ifdef HAVE_OPENSSL
#include "my_net.h"			/* needed because of struct in_addr */

size_t	vio_ssl_read(Vio *vio,uchar* buf,	size_t size);
size_t	vio_ssl_write(Vio *vio,const uchar* buf, size_t size);

/* When the workday is over... */
int vio_ssl_close(Vio *vio);
void vio_ssl_delete(Vio *vio);
int vio_ssl_blocking(Vio *vio, my_bool set_blocking_mode, my_bool *old_mode);
my_bool vio_ssl_has_data(Vio *vio);

#endif /* HAVE_OPENSSL */
#endif /* VIO_PRIV_INCLUDED */
