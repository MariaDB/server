# Copyright (c) 2013 Percona LLC and/or its affiliates.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.

# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.

# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA


OPTION(WITH_MARIABACKUP "Include mariabackup" ON)
IF(NOT WITH_MARIABACKUP)
  RETURN()
ENDIF()


IF(NOT WIN32)
  CHECK_SYMBOL_EXISTS(regcomp regex.h HAVE_SYSTEM_REGEX)
  IF(HAVE_SYSTEM_REGEX)
    ADD_DEFINITIONS(-DHAVE_SYSTEM_REGEX)
  ENDIF()
ENDIF()

IF(WITH_LIBARCHIVE STREQUAL "STATIC")
  SET(CMAKE_FIND_LIBRARY_SUFFIXES .a .lib)
ENDIF()

FIND_PACKAGE(LibArchive)

IF(NOT DEFINED WITH_LIBARCHIVE)  
  IF(LibArchive_FOUND)
    SET(WITH_LIBARCHIVE_DEFAULT ON)
  ELSE()
    SET(WITH_LIBARCHIVE_DEFAULT OFF)
  ENDIF()
  SET(WITH_LIBARCHIVE ${WITH_LIBARCHIVE_DEFAULT} CACHE STRING "Use libarchive for streaming features (ON, OFF or STATIC)" )
ENDIF()

IF(NOT WITH_LIBARCHIVE MATCHES "^(ON|OFF|STATIC)$")
  MESSAGE(FATAL_ERROR "Invalid value for WITH_LIBARCHIVE: '${WITH_LIBARCHIVE}'. Use one of ON, OFF or STATIC")
ENDIF()

IF(UNIX)
  SET(PIC_FLAG -fPIC)
ENDIF()

IF((NOT WITH_LIBARCHIVE STREQUAL "OFF") AND (NOT LibArchive_FOUND))
  IF(CMAKE_VERSION VERSION_LESS "2.8.12")
    MESSAGE("libarchive can't be built, old cmake")
  ELSE()
    # Build a local version
    INCLUDE(ExternalProject)
    SET(LIBARCHIVE_DIR  ${CMAKE_CURRENT_BINARY_DIR}/libarchive)
    SET(libarchive_PREFIX ${CMAKE_CURRENT_BINARY_DIR}/libarchive)
    SET(libarchive_CMAKE_ARGS
      -DCMAKE_INSTALL_PREFIX:PATH=<INSTALL_DIR> 
      -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
      -DENABLE_ICONV=OFF
      -DENABLE_TAR=ON
      -DENABLE_OPENSSL=OFF
      -DENABLE_TEST=OFF
      "-DCMAKE_C_FLAGS_DEBUG=${CMAKE_C_FLAGS_DEBUG} ${PIC_FLAG}"
      "-DCMAKE_C_FLAGS_RELWITHDEBINFO=${CMAKE_C_FLAGS_RELWITHDEBINFO} ${PIC_FLAG}"
      "-DCMAKE_C_FLAGS_RELEASE=${CMAKE_C_FLAGS_RELEASE} ${PIC_FLAG}"
      "-DCMAKE_C_FLAGS_MINSIZEREL=${CMAKE_C_FLAGS_MINSIZEREL} ${PIC_FLAG}"
    )
    IF(WIN32)
      SET(libarchive_CMAKE_ARGS  ${libarchive_CMAKE_ARGS}    -DWINDOWS_VERSION=WIN7 -DCMAKE_DEBUG_POSTFIX=d)
      SET(LIBARCHIVE_RELEASE_LIB ${LIBARCHIVE_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}archive_static${CMAKE_STATIC_LIBRARY_SUFFIX})
      SET(LIBARCHIVE_DEBUG_LIB ${LIBARCHIVE_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}archive_staticd${CMAKE_STATIC_LIBRARY_SUFFIX})
      SET(byproducts ${LIBARCHIVE_RELEASE_LIB} ${LIBARCHIVE_DEBUG_LIB})
    ELSE()
      SET(LIBARCHIVE_LIB ${LIBARCHIVE_DIR}/lib/${CMAKE_STATIC_LIBRARY_PREFIX}archive${CMAKE_STATIC_LIBRARY_SUFFIX})
      SET(byproducts ${LIBARCHIVE_LIB})
    ENDIF()

    IF(CMAKE_VERSION VERSION_GREATER "3.1")
      SET(byproducts BUILD_BYPRODUCTS ${byproducts})
    ENDIF()

    ExternalProject_Add(libarchive
      PREFIX ${libarchive_PREFIX}
      DOWNLOAD_DIR ${LIBARCHIVE_DIR}
      URL http://www.libarchive.org/downloads/libarchive-3.2.2.tar.gz
      INSTALL_DIR ${LIBARCHIVE_DIR}
      CMAKE_ARGS ${libarchive_CMAKE_ARGS}
      ${byproducts}
    )
    ADD_LIBRARY(archive_static STATIC IMPORTED)
    ADD_DEPENDENCIES(archive_static libarchive)
    IF(WIN32)
      SET_PROPERTY(TARGET archive_static PROPERTY IMPORTED_LOCATION_RELWITHDEBINFO ${LIBARCHIVE_RELEASE_LIB})
      SET_PROPERTY(TARGET archive_static PROPERTY IMPORTED_LOCATION_RELEASE ${LIBARCHIVE_RELEASE_LIB})
      SET_PROPERTY(TARGET archive_static PROPERTY IMPORTED_LOCATION_DEBUG ${LIBARCHIVE_DEBUG_LIB})
      SET_PROPERTY(TARGET archive_static PROPERTY IMPORTED_LOCATION_MINSIZEREL ${LIBARCHIVE_RELEASE_LIB})
    ELSE()
      SET_PROPERTY(TARGET archive_static PROPERTY IMPORTED_LOCATION ${LIBARCHIVE_LIB})
    ENDIF()

    SET(LibArchive_FOUND ON )
    SET(LibArchive_INCLUDE_DIRS ${LIBARCHIVE_DIR}/include )
    SET(LibArchive_LIBRARIES archive_static)
    IF(WIN32)
      SET(LIBARCHIVE_STATIC 1)
    ENDIF()
  ENDIF()
ENDIF()


IF(WITH_LIBARCHIVE AND LibArchive_FOUND)
  ADD_DEFINITIONS(-DHAVE_LIBARCHIVE)
  IF(LIBARCHIVE_STATIC)
    ADD_DEFINITIONS(-DLIBARCHIVE_STATIC)
  ENDIF()
  INCLUDE_DIRECTORIES(${LibArchive_INCLUDE_DIRS})
  LINK_LIBRARIES(${LibArchive_LIBRARIES})
  SET(DS_ARCHIVE_SOURCE ds_archive.c)
ENDIF()

INCLUDE_DIRECTORIES(
  ${CMAKE_SOURCE_DIR}/include
  ${CMAKE_SOURCE_DIR}/storage/xtradb/include
  ${CMAKE_SOURCE_DIR}/sql
  ${CMAKE_CURRENT_SOURCE_DIR}/quicklz
  ${CMAKE_CURRENT_SOURCE_DIR}
  )

IF(NOT HAVE_SYSTEM_REGEX)
  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/pcre)
ENDIF()

ADD_DEFINITIONS(-UMYSQL_SERVER)
########################################################################
# xtrabackup binary
########################################################################

IF(WIN32)
  SET(NT_SERVICE_SOURCE ${PROJECT_SOURCE_DIR}/sql/nt_servc.cc)
ELSE()
  SET(NT_SERVICE_SOURCE)
ENDIF()

ADD_DEFINITIONS(-DPCRE_STATIC=1)

MYSQL_ADD_EXECUTABLE(mariabackup
  xtrabackup.cc
  innobackupex.cc
  changed_page_bitmap.cc
  datasink.c
  ${DS_ARCHIVE_SOURCE}
  ds_buffer.c
  ds_compress.c
  ds_encrypt.c
  ds_local.c
  ds_stdout.c
  ds_tmpfile.c
  ds_xbstream.c
  fil_cur.cc
  quicklz/quicklz.c
  read_filt.cc
  write_filt.cc
  wsrep.cc
  xbcrypt_common.c
  xbcrypt_write.c
  xbstream_write.c
  backup_mysql.cc
  backup_copy.cc
  encryption_plugin.cc
  ${PROJECT_SOURCE_DIR}/libmysql/libmysql.c
  ${PROJECT_SOURCE_DIR}/sql/net_serv.cc
  ${NT_SERVICE_SOURCE}
  COMPONENT backup
  )


# Export all symbols on Unix, for better crash callstacks
SET_TARGET_PROPERTIES(mariabackup PROPERTIES ENABLE_EXPORTS TRUE)


TARGET_LINK_LIBRARIES(mariabackup sql)

IF(NOT HAVE_SYSTEM_REGEX)
  TARGET_LINK_LIBRARIES(mariabackup pcreposix)
ENDIF()


########################################################################
# xbstream binary
########################################################################
MYSQL_ADD_EXECUTABLE(mbstream
  ds_buffer.c
  ds_local.c
  ds_stdout.c
  datasink.c
  xbstream.c
  xbstream_read.c
  xbstream_write.c

  COMPONENT backup
  )


TARGET_LINK_LIBRARIES(mbstream
  mysys
)

IF(MSVC)
  SET_TARGET_PROPERTIES(mbstream PROPERTIES LINK_FLAGS setargv.obj)
ENDIF()
