# Copyright (c) 2006, 2018, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2018, 2026, MariaDB
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

include(CheckFunctionExists)
include(CheckIncludeFile)
include(CheckTypeSize)

#
# Check to see if we have large file support
#
set(CMAKE_REQUIRED_DEFINITIONS -D_LARGEFILE64_SOURCE=1)
check_type_size(off64_t OFF64_T)
unset(CMAKE_REQUIRED_DEFINITIONS) # clear variable

#
# Check for fseeko
#
check_function_exists(fseeko HAVE_FSEEKO)

#
# Check for stdarg.h
#
check_include_file(stdarg.h HAVE_STDARG_H)

#
# Check for unistd.h
#
check_include_file(unistd.h HAVE_UNISTD_H)

# ============================================================================

configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/zconf.h.cmakein
  ${CMAKE_CURRENT_BINARY_DIR}/zconf.h @ONLY
  )
include_directories(
  SYSTEM ${CMAKE_CURRENT_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}
  )

#============================================================================
# zlib
# ============================================================================

set(ZLIB_PUBLIC_HDRS ${CMAKE_CURRENT_BINARY_DIR}/zconf.h zlib.h)

set(ZLIB_PRIVATE_HDRS
    crc32.h
    deflate.h
    gzguts.h
    inffast.h
    inffixed.h
    inflate.h
    inftrees.h
    trees.h
    zutil.h)

set(ZLIB_SRCS
    adler32.c
    compress.c
    crc32.c
    deflate.c
    gzclose.c
    gzlib.c
    gzread.c
    gzwrite.c
    inflate.c
    infback.c
    inftrees.c
    inffast.c
    trees.c
    uncompr.c
    zutil.c)

ADD_CONVENIENCE_LIBRARY(zlib STATIC
  ${ZLIB_SRCS} ${ZLIB_PUBLIC_HDRS} ${ZLIB_PRIVATE_HDRS})
RESTRICT_SYMBOL_EXPORTS(zlib)
