/* Copyright (c) 2002, 2015, Oracle and/or its affiliates.
   Copyright (c) 2012, 2017, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* guess_malloc_library() deduces, to the best of its ability,
   the currently used malloc library and its version */

#include <stddef.h>
#include "my_global.h"
#include <m_string.h>

typedef const char* (*tc_version_type)(int*, int*, const char**);
typedef int (*mallctl_type)(const char*, void*, size_t*, void*, size_t);

char *guess_malloc_library()
{
  tc_version_type tc_version_func;
  mallctl_type mallctl_func;
#ifndef HAVE_DLOPEN
  return (char*) MALLOC_LIBRARY;
#else
  static char buf[128];

  if (strcmp(MALLOC_LIBRARY, "system") != 0)
  {
    return (char*) MALLOC_LIBRARY;
  }

  /* tcmalloc */
  tc_version_func= (tc_version_type) dlsym(RTLD_DEFAULT, "tc_version");
  if (tc_version_func)
  {
    int major, minor;
    const char* ver_str = tc_version_func(&major, &minor, NULL);
    strxnmov(buf, sizeof(buf)-1, "tcmalloc ", ver_str, NULL);
    return buf;
  }

  /* jemalloc */
  mallctl_func= (mallctl_type) dlsym(RTLD_DEFAULT, "mallctl");
  if (mallctl_func)
  {
    char *ver;
    size_t len = sizeof(ver);
    mallctl_func("version", &ver, &len, NULL, 0);
    strxnmov(buf, sizeof(buf)-1, "jemalloc ", ver, NULL);
    return buf;
  }

  return (char*) MALLOC_LIBRARY;
#endif
}

