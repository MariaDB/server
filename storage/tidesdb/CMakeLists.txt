# Copyright (c) 2026 TidesDB Corp.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA

# TidesDB storage engine.
#
# The libtidesdb C library is vendored under libtidesdb/ and compiled into a
# static archive (tidesdb_embedded) that is statically linked into the plugin
# module. This mirrors how storage/rocksdb builds rocksdblib: a self-contained
# build with no dependency on a system-installed libtidesdb, so a normal
# MariaDB build produces a working engine and stale /usr/local copies cannot
# shadow it.

# 
# Do not route compression through MariaDB's loadable provider services.
#
# MariaDB injects include/providers/ globally (CMakeLists.txt), whose lz4.h /
# snappy-c.h shims rewrite the C compression calls to go through provider
# plugins that must be separately installed. libtidesdb expects to call the
# real zstd/lz4/snappy directly, so drop that include dir for this engine and
# link the system libraries instead (mirrors storage/rocksdb).
# 
GET_PROPERTY(dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
LIST(REMOVE_ITEM dirs ${CMAKE_SOURCE_DIR}/include/providers)
SET_PROPERTY(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${dirs}")

#
# Compression backends (zstd / lz4 / snappy) -- each OPTIONAL.
#
# As of libtidesdb 9.3.5 every backend is independent: compress.c pulls in the
# third-party header and compiles its code path only when the matching
# TIDESDB_HAVE_<backend> macro is defined, and a build with none still works
# (TDB_COMPRESS_NONE only).  Auto-detect each dev library and enable the ones
# present.  Use FIND_LIBRARY full paths, not pkg-config: pkg-config returns
# bare names like "zstd" which collide with MariaDB's own "zstd" MODULE target
# (the provider_zstd plugin).
#
SET(TIDESDB_COMPRESS_DEFS "")
SET(TIDESDB_COMPRESS_LIBS "")
SET(TIDESDB_COMPRESS_INCS "")

FIND_LIBRARY(TIDESDB_ZSTD_LIB NAMES zstd)
FIND_PATH(TIDESDB_ZSTD_INC    NAMES zstd.h)
IF(TIDESDB_ZSTD_LIB AND TIDESDB_ZSTD_INC)
  LIST(APPEND TIDESDB_COMPRESS_DEFS TIDESDB_HAVE_ZSTD)
  LIST(APPEND TIDESDB_COMPRESS_LIBS ${TIDESDB_ZSTD_LIB})
  LIST(APPEND TIDESDB_COMPRESS_INCS ${TIDESDB_ZSTD_INC})
ENDIF()

FIND_LIBRARY(TIDESDB_LZ4_LIB NAMES lz4)
FIND_PATH(TIDESDB_LZ4_INC    NAMES lz4.h)
IF(TIDESDB_LZ4_LIB AND TIDESDB_LZ4_INC)
  LIST(APPEND TIDESDB_COMPRESS_DEFS TIDESDB_HAVE_LZ4)
  LIST(APPEND TIDESDB_COMPRESS_LIBS ${TIDESDB_LZ4_LIB})
  LIST(APPEND TIDESDB_COMPRESS_INCS ${TIDESDB_LZ4_INC})
ENDIF()

FIND_LIBRARY(TIDESDB_SNAPPY_LIB NAMES snappy)
FIND_PATH(TIDESDB_SNAPPY_INC    NAMES snappy-c.h)
IF(TIDESDB_SNAPPY_LIB AND TIDESDB_SNAPPY_INC)
  LIST(APPEND TIDESDB_COMPRESS_DEFS TIDESDB_HAVE_SNAPPY)
  LIST(APPEND TIDESDB_COMPRESS_LIBS ${TIDESDB_SNAPPY_LIB})
  LIST(APPEND TIDESDB_COMPRESS_INCS ${TIDESDB_SNAPPY_INC})
ENDIF()

MESSAGE(STATUS "TidesDB: compression backends enabled: ${TIDESDB_COMPRESS_DEFS}")

# 
# Optional S3-compatible object store connector (libcurl + OpenSSL).
# On by default; degrades gracefully to a non-S3 build if the deps are absent.
# 
OPTION(TIDESDB_WITH_S3 "Build the TidesDB S3-compatible object store connector" ON)
SET(TIDESDB_S3_LIBS)
IF(TIDESDB_WITH_S3)
  FIND_PACKAGE(CURL)
  FIND_PACKAGE(OpenSSL)
  IF(CURL_FOUND AND OPENSSL_FOUND)
    SET(TIDESDB_S3_LIBS CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
    MESSAGE(STATUS "TidesDB: S3 object store connector enabled")
  ELSE()
    MESSAGE(STATUS "TidesDB: libcurl/OpenSSL not found - building without S3 connector")
    SET(TIDESDB_WITH_S3 OFF)
  ENDIF()
ENDIF()

# 
# The plugin module. Created first so that, if the engine is not requested
# (-DPLUGIN_TIDESDB=NO), we RETURN before building the vendored library.
# 
MYSQL_ADD_PLUGIN(tidesdb ha_tidesdb.cc
                 STORAGE_ENGINE MODULE_ONLY
                 COMPONENT tidesdb-engine)

IF(NOT TARGET tidesdb)
  RETURN()
ENDIF()

# 
# Vendored libtidesdb -> static archive linked into the plugin.
# 
# The vendored tree keeps libtidesdb's upstream src/ + external/ layout so that
# its internal relative includes (e.g. src/clock_cache.c -> "../external/xxhash.h")
# resolve unchanged.
SET(TIDESDB_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/libtidesdb)

SET(TIDESDB_CORE_SOURCES
  ${TIDESDB_LIB_DIR}/src/tidesdb.c
  ${TIDESDB_LIB_DIR}/src/block_manager.c
  ${TIDESDB_LIB_DIR}/src/skip_list.c
  ${TIDESDB_LIB_DIR}/src/compress.c
  ${TIDESDB_LIB_DIR}/src/bloom_filter.c
  ${TIDESDB_LIB_DIR}/src/manifest.c
  ${TIDESDB_LIB_DIR}/src/clock_cache.c
  ${TIDESDB_LIB_DIR}/src/queue.c
  ${TIDESDB_LIB_DIR}/src/btree.c
  ${TIDESDB_LIB_DIR}/src/alloc.c
  ${TIDESDB_LIB_DIR}/src/objstore_fs.c
  ${TIDESDB_LIB_DIR}/src/local_cache.c
  ${TIDESDB_LIB_DIR}/external/xxhash.c
  ${TIDESDB_LIB_DIR}/external/ini.c
)
IF(TIDESDB_WITH_S3)
  LIST(APPEND TIDESDB_CORE_SOURCES ${TIDESDB_LIB_DIR}/src/objstore_s3.c)
ENDIF()

ADD_LIBRARY(tidesdb_embedded STATIC ${TIDESDB_CORE_SOURCES})

# Vendored third-party C: build position-independent (it links into a module),
# as C11, and silence its warnings.  Against MariaDB's stricter C flag set
# libtidesdb emits declaration-after-statement (C90-style) warnings; -w keeps
# them out of the build and, unlike a bare -Wno-error, also shields the lib
# from maintainer mode's specific -Werror=uninitialized.
SET_TARGET_PROPERTIES(tidesdb_embedded PROPERTIES
                      C_STANDARD 11
                      POSITION_INDEPENDENT_CODE ON)

TARGET_INCLUDE_DIRECTORIES(tidesdb_embedded PRIVATE
                           ${TIDESDB_LIB_DIR}/src        # internal "tidesdb.h" includes
                           ${TIDESDB_LIB_DIR}/external
                           ${TIDESDB_COMPRESS_INCS})

TARGET_COMPILE_DEFINITIONS(tidesdb_embedded PRIVATE
                           _GNU_SOURCE
                           ${TIDESDB_COMPRESS_DEFS}
                           $<$<BOOL:${TIDESDB_WITH_S3}>:TIDESDB_WITH_S3>)

IF(NOT MSVC)
  TARGET_COMPILE_OPTIONS(tidesdb_embedded PRIVATE -w)
ENDIF()

# Propagate the runtime dependencies to whatever links the archive (the plugin).
IF(HAVE_GCC_C11_ATOMICS_WITH_LIBATOMIC)
  SET(TIDESDB_ATOMIC_LIBS -latomic)
ENDIF()

TARGET_LINK_LIBRARIES(tidesdb_embedded PUBLIC
                      ${TIDESDB_COMPRESS_LIBS}
                      ${TIDESDB_S3_LIBS}
                      ${TIDESDB_ATOMIC_LIBS}
                      ${CMAKE_THREAD_LIBS_INIT}
                      ${LIBM})

# 
# Wire the static library into the plugin.
#
# ha_tidesdb.cc uses installed-style includes (<tidesdb/db.h>, <tidesdb/xxhash.h>)
# that expect every public header flattened under a single tidesdb/ directory.
# Assemble that layout in the build tree from the upstream src/ + external/
# headers, then point the plugin's include path at its parent.
# 
SET(TIDESDB_PUBLIC_INC ${CMAKE_CURRENT_BINARY_DIR}/include)
FILE(MAKE_DIRECTORY ${TIDESDB_PUBLIC_INC}/tidesdb)
FILE(COPY ${TIDESDB_LIB_DIR}/src/      DESTINATION ${TIDESDB_PUBLIC_INC}/tidesdb
     FILES_MATCHING PATTERN "*.h")
FILE(COPY ${TIDESDB_LIB_DIR}/external/ DESTINATION ${TIDESDB_PUBLIC_INC}/tidesdb
     FILES_MATCHING PATTERN "*.h")

TARGET_INCLUDE_DIRECTORIES(tidesdb PRIVATE ${TIDESDB_PUBLIC_INC})
TARGET_LINK_LIBRARIES(tidesdb tidesdb_embedded)

IF(TIDESDB_WITH_S3)
  TARGET_COMPILE_DEFINITIONS(tidesdb PRIVATE TIDESDB_WITH_S3)
ENDIF()
