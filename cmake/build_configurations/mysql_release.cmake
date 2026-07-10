# Copyright (c) 2010, 2011, Oracle and/or its affiliates. All rights reserved.
# Copyright (c) 2011, 2022, MariaDB Corporation.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA 

# This file includes build settings used for MySQL release

INCLUDE(CheckIncludeFiles)
INCLUDE(CheckLibraryExists)
INCLUDE(CheckTypeSize)

# XXX package_name.cmake uses this too, move it somewhere global
CHECK_TYPE_SIZE("void *" SIZEOF_VOIDP)
IF(SIZEOF_VOIDP EQUAL 4)
  SET(32BIT 1)
ENDIF()
IF(SIZEOF_VOIDP EQUAL 8)
  SET(64BIT 1)
ENDIF()

# include aws_key_management plugin in release builds
OPTION(AWS_SDK_EXTERNAL_PROJECT  "Allow download and build AWS C++ SDK" ON)

SET(FEATURE_SET "community" CACHE STRING 
" Selection of features. Options are
 - xsmall : 
 - small: embedded
 - classic: embedded + archive + federated + blackhole 
 - large :  embedded + archive + federated + blackhole + innodb
 - xlarge:  embedded + archive + federated + blackhole + innodb + partition
 - community:  all  features (currently == xlarge)
"
)

SET(FEATURE_SET_xsmall  1)
SET(FEATURE_SET_small   2)
SET(FEATURE_SET_classic 3)
SET(FEATURE_SET_large   5)
SET(FEATURE_SET_xlarge  6)
SET(FEATURE_SET_community 7)

IF(FEATURE_SET)
  STRING(TOLOWER ${FEATURE_SET} feature_set)
  SET(num ${FEATURE_SET_${feature_set}})
  IF(NOT num)
   MESSAGE(FATAL_ERROR "Invalid FEATURE_SET option '${feature_set}'. 
   Should be xsmall, small, classic, large, or community
   ")
  ENDIF()
  SET(PLUGIN_PARTITION "NO")
  IF(num EQUAL FEATURE_SET_xsmall)
    SET(WITH_NONE ON)
  ENDIF()
  
  IF(num GREATER FEATURE_SET_small)
    SET(PLUGIN_ARCHIVE "STATIC")
    SET(PLUGIN_BLACKHOLE "STATIC")
    SET(PLUGIN_FEDERATEDX "STATIC")
    SET(PLUGIN_FEEDBACK "STATIC")
  ENDIF()
  IF(num GREATER FEATURE_SET_classic)
    SET(PLUGIN_INNOBASE "STATIC")
  ENDIF()
  IF(num GREATER FEATURE_SET_large)
    SET(PLUGIN_PARTITION "STATIC")
  ENDIF()
  IF(num GREATER FEATURE_SET_xlarge)
   # OPTION(WITH_ALL ON) 
   # better no set this, otherwise server would be linked 
   # statically with experimental stuff like audit_null
  ENDIF()
ENDIF()

SET(WITH_INNODB_SNAPPY OFF CACHE STRING "")
SET(WITH_NUMA 0 CACHE BOOL "")
SET(CPU_LEVEL1_DCACHE_LINESIZE 0)
# generate SBOMS
SET(SBOM_DEFAULT 1)

IF(NOT EXISTS ${CMAKE_SOURCE_DIR}/.git)
  SET(GIT_EXECUTABLE GIT_EXECUTABLE-NOTFOUND CACHE FILEPATH "")
ENDIF()

IF(WIN32)
  SET(INSTALL_MYSQLTESTDIR "" CACHE STRING "")
  SET(INSTALL_SQLBENCHDIR  "" CACHE STRING "")
  SET(INSTALL_SUPPORTFILESDIR ""  CACHE STRING "")
ELSEIF(CMAKE_SYSTEM_NAME MATCHES "AIX")
  # AIX freesource is RPM, but different than Linux RPM
  SET(WITH_SSL system CACHE STRING "")
  SET(WITH_ZLIB system CACHE STRING "")
ELSEIF(RPM)
  SET(WITH_SSL system CACHE STRING "")
  SET(WITH_ZLIB system CACHE STRING "")
  SET(CHECKMODULE /usr/bin/checkmodule CACHE FILEPATH "")
  SET(SEMODULE_PACKAGE /usr/bin/semodule_package CACHE FILEPATH "")
  SET(PLUGIN_AUTH_SOCKET YES CACHE STRING "")
  SET(WITH_EMBEDDED_SERVER ON CACHE BOOL "")
  SET(WITH_PCRE system CACHE STRING "")
  SET(CLIENT_PLUGIN_ZSTD OFF)
  IF(RPM MATCHES "fedora|centos|rhel|rocky|alma")
    SET(WITH_ROCKSDB_BZip2 OFF CACHE STRING "")
  ENDIF()
  IF(RPM MATCHES "opensuse|sles|centos|rhel|rocky|alma")
    SET(WITH_ROCKSDB_LZ4 OFF CACHE STRING "")
  ENDIF()
ELSEIF(DEB)
  SET(WITH_SSL system CACHE STRING "")
  SET(WITH_ZLIB system CACHE STRING "")
  SET(WITH_LIBWRAP ON)
  SET(HAVE_EMBEDDED_PRIVILEGE_CONTROL ON)
  # On Hurd systems (GNU) there is no auth socket implementation
  IF(NOT CMAKE_SYSTEM_NAME STREQUAL "GNU")
    SET(PLUGIN_AUTH_SOCKET YES CACHE STRING "")
  ENDIF()
  SET(WITH_EMBEDDED_SERVER ON CACHE BOOL "")
  SET(WITH_PCRE system CACHE STRING "")
  SET(CLIENT_PLUGIN_ZSTD OFF)
  SET(WITH_ROCKSDB_BZip2 OFF CACHE STRING "")
ELSE()
  SET(WITH_SSL bundled CACHE STRING "")
  SET(WITH_PCRE bundled CACHE STRING "")
  SET(WITH_ZLIB bundled CACHE STRING "")
  SET(WITH_JEMALLOC static CACHE STRING "")
  SET(PLUGIN_AUTH_SOCKET STATIC CACHE STRING "")
  SET(WITH_STRIPPED_CLIENT ON CACHE BOOL "Strip all client binaries")
  SET(WITH_ROCKSDB_BZip2 OFF CACHE STRING "")
  SET(WITH_ROCKSDB_LZ4 OFF CACHE STRING "")
ENDIF()

IF(NOT COMPILATION_COMMENT)
  SET(COMPILATION_COMMENT "MariaDB Server")
ENDIF()

IF(WIN32)
  IF(NOT CMAKE_USING_VC_FREE_TOOLS)
    # Sign executables with authenticode certificate
    SET(SIGNCODE 1 CACHE BOOL "")
  ENDIF()
ENDIF()

IF(UNIX)
  SET(WITH_EXTRA_CHARSETS all CACHE STRING "")
  SET(PLUGIN_AUTH_PAM YES CACHE BOOL "")

  IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    FIND_PACKAGE(URING)
    FIND_PACKAGE(LIBAIO)
    IF(NOT URING_FOUND AND NOT LIBAIO_FOUND AND NOT IGNORE_AIO_CHECK)
        MESSAGE(FATAL_ERROR "
        Either liburing or libaio is required on Linux.
        You can  install libaio like this:

          Debian/Ubuntu:              apt-get install libaio-dev
          RedHat/Fedora/Oracle Linux: yum install libaio-devel
          SuSE:                       zypper install libaio-devel

          If you really do not want it, pass -DIGNORE_AIO_CHECK=YES to cmake.
        ")
    ENDIF()
  ENDIF()
ENDIF()
