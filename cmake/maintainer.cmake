# Copyright (c) 2010, 2014, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2011, 2021, MariaDB
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

IF(MSVC OR MYSQL_MAINTAINER_MODE STREQUAL "NO")
  # Windows support is in cmake/os/Windows.cmake, not here
  RETURN()
ENDIF()

# Common warning flags for GCC, G++, Clang and Clang++
SET(MY_WARNING_FLAGS
  -Wall
  -Wdeclaration-after-statement
  -Wenum-compare
  -Wenum-conversion
  -Wextra
  -Wformat-security
  -Wno-format-truncation
  -Wno-init-self
  -Wno-nonnull-compare
  -Wno-null-conversion
  -Wno-unused-parameter
  -Wno-unused-private-field
  -Woverloaded-virtual
  -Wnon-virtual-dtor
  -Wvla
  -Wwrite-strings
  )

FOREACH(F ${MY_WARNING_FLAGS})
  MY_CHECK_AND_SET_COMPILER_FLAG(${F} DEBUG RELWITHDEBINFO)
ENDFOREACH()

SET(MY_ERROR_FLAGS -Werror)

IF(CMAKE_COMPILER_IS_GNUCC AND CMAKE_C_COMPILER_VERSION VERSION_LESS "6.0.0")
  SET(MY_ERROR_FLAGS ${MY_ERROR_FLAGS} -Wno-error=maybe-uninitialized)
  SET(MY_ERROR_FLAGS ${MY_ERROR_FLAGS} -Wno-error=non-virtual-dtor) # gcc bug 7302
ENDIF()

IF(MYSQL_MAINTAINER_MODE MATCHES "OFF|WARN")
  RETURN()
ELSEIF(MYSQL_MAINTAINER_MODE MATCHES "AUTO")
  SET(WHERE DEBUG)
ENDIF()

FOREACH(F ${MY_ERROR_FLAGS})
  MY_CHECK_AND_SET_COMPILER_FLAG(${F} ${WHERE})
ENDFOREACH()

IF(CMAKE_C_COMPILER_ID MATCHES "GNU")
  STRING(REPLACE " -E " " -E -dDI " CMAKE_C_CREATE_PREPROCESSED_SOURCE ${CMAKE_C_CREATE_PREPROCESSED_SOURCE})
ENDIF()
IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
  STRING(REPLACE " -E " " -E -dDI " CMAKE_CXX_CREATE_PREPROCESSED_SOURCE ${CMAKE_CXX_CREATE_PREPROCESSED_SOURCE})
ENDIF()
