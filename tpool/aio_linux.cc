/* Copyright (C) 2025 MariaDB Corporation.

This program is free software; you can redistribute itand /or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111 - 1301 USA*/

/*
   This file exports create_linux_aio() function which is used to create
   an asynchronous IO implementation for Linux (currently either libaio or
   uring).
 */

#include "tpool.h"
#include <stdio.h>
namespace tpool
{

// Forward declarations of aio implementations
#ifdef HAVE_LIBAIO
// defined in aio_libaio.cc
aio *create_libaio(thread_pool *pool, int max_io);
#endif
#if defined HAVE_URING
// defined in aio_uring.cc
aio *create_uring(thread_pool *pool, int max_io);
#endif

/*
  @brief
  Choose native linux aio implementation based on availability and user
  preference.

  @param pool - thread pool to use for aio operations
  @param max_io - maximum number of concurrent io operations
  @param impl - implementation to use, can be one of the following:

  @returns
  A pointer to the aio implementation object, or nullptr if no suitable
  implementation is available.

  If impl is OS_IO_DEFAULT, it will try uring first, fallback to libaio
  If impl is OS_IO_URING or OS_IO_LIBAIO, it won't fallback
*/
aio *create_linux_aio(thread_pool *pool, int max_io, aio_implementation impl)
{
#ifdef HAVE_URING
  if (impl != OS_IO_LIBAIO)
  {
    aio *ret= create_uring(pool, max_io);
    if (ret)
      return ret;
    else if (impl != OS_IO_DEFAULT)
      return nullptr; // uring is not available
    else
      fprintf(stderr, "create_uring failed: falling back to libaio\n");
  }
#endif
#ifdef HAVE_LIBAIO
  if (impl != OS_IO_URING)
    return create_libaio(pool, max_io);
#endif
  return nullptr;
}
} // namespace tpool
