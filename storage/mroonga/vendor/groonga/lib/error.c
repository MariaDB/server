/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2013 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "grn_error.h"

#ifdef HAVE_ERRNO_H
#include <errno.h>
#endif /* HAVE_ERRNO_H */

#include <string.h>

#ifdef WIN32
const char *
grn_current_error_message(void)
{
# define ERROR_MESSAGE_BUFFER_SIZE 4096
  int error_code = GetLastError();
  static char message[ERROR_MESSAGE_BUFFER_SIZE];
  DWORD written_bytes;

  written_bytes = FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM |
                                FORMAT_MESSAGE_IGNORE_INSERTS,
                                NULL,
                                error_code,
                                MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                                message,
                                ERROR_MESSAGE_BUFFER_SIZE,
                                NULL);
  if (written_bytes >= 2) {
    if (message[written_bytes - 1] == '\n') {
      message[written_bytes - 1] = '\0';
      written_bytes--;
    }
    if (message[written_bytes - 1] == '\r') {
      message[written_bytes - 1] = '\0';
      written_bytes--;
    }
  }

  return message;

# undef ERROR_MESSAGE_BUFFER_SIZE
}
#else
const char *
grn_current_error_message(void)
{
  return strerror(errno);
}
#endif
