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
# module. This mirrors how storage/rocksdb builds rocksdblib, in that a self-contained
# build with no dependency on a system-installed libtidesdb, so a normal
# MariaDB build produces a working engine and stale /usr/local copies cannot
# shadow it.

#
# Compression backends (zstd / lz4 / snappy) via MariaDB's compression
# provider services.
#
# Instead of linking libzstd/liblz4/libsnappy into the engine, libtidesdb's
# compress.c is compiled against MariaDB's provider shims in include/providers/
# (kept on the include path -- unlike storage/rocksdb, which drops them).  Each
# codec call resolves at run time through the matching provider plugin
# (provider_zstd / provider_lz4 / provider_snappy).  When a provider is not
# loaded its service stub returns an error, so compress_data() fails cleanly
# for that algorithm and the engine still runs (TDB_COMPRESS_NONE always
# works) -- no hard dependency on any codec library.
#
# All three code paths are always compiled in; runtime availability is a
# property of which provider plugins are loaded.  LZ4_FAST uses the
# LZ4_compress_fast entry added to the lz4 provider.
#
SET(TIDESDB_COMPRESS_DEFS TIDESDB_HAVE_ZSTD TIDESDB_HAVE_LZ4 TIDESDB_HAVE_SNAPPY)
SET(TIDESDB_HAVE_ZSTD   ON)   # feeds tidesdb_version.h #cmakedefine
SET(TIDESDB_HAVE_LZ4    ON)
SET(TIDESDB_HAVE_SNAPPY ON)

MESSAGE(STATUS "TidesDB: compression routed through provider services (zstd, lz4, snappy)")

#
# Optional S3-compatible object store connector.
#
# Requires libcurl only.  AWS SigV4 request signing uses libtidesdb's own
# bundled SHA256/HMAC (src/sha256.c, src/hmac_sha256.c) rather than OpenSSL, so
# the engine carries no TLS/crypto dependency -- this is what lets the S3
# connector build on Windows.  On by default; degrades gracefully to a non-S3
# build if libcurl is absent.
#
OPTION(TIDESDB_WITH_S3 "Build the TidesDB S3-compatible object store connector" ON)
SET(TIDESDB_S3_LIBS)
IF(TIDESDB_WITH_S3)
  FIND_PACKAGE(CURL)
  IF(CURL_FOUND)
    SET(TIDESDB_S3_LIBS CURL::libcurl)
    MESSAGE(STATUS "TidesDB: S3 object store connector enabled")
  ELSE()
    MESSAGE(STATUS "TidesDB: libcurl not found - building without S3 connector")
    SET(TIDESDB_WITH_S3 OFF)
  ENDIF()
ENDIF()
SET(TIDESDB_HAS_S3 ${TIDESDB_WITH_S3})  # feeds tidesdb_version.h #cmakedefine

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
  ${TIDESDB_LIB_DIR}/src/sha256.c
  ${TIDESDB_LIB_DIR}/src/hmac_sha256.c
  ${TIDESDB_LIB_DIR}/external/xxhash.c
  ${TIDESDB_LIB_DIR}/external/ini.c
)
IF(TIDESDB_WITH_S3)
  LIST(APPEND TIDESDB_CORE_SOURCES ${TIDESDB_LIB_DIR}/src/objstore_s3.c)
ENDIF()

ADD_LIBRARY(tidesdb_embedded STATIC ${TIDESDB_CORE_SOURCES})

# Vendored third-party C -- build position-independent (it links into a module)
# and as C11.  Platform-specific flags mirror libtidesdb's own build:
#
# GCC/Clang -- rather than blanket-suppressing warnings, silence only the
# specific categories libtidesdb trips under MariaDB's stricter C flags:
#   -Wdeclaration-after-statement   the library uses C99/C11 declare-at-use
#                                   style, while MariaDB compiles C as C90;
#   -Wframe-larger-than             a few functions use large on-stack buffers;
#   -Wdiscarded-qualifiers          one const-qualifier drop in the library.
# Any other (new) warning still surfaces and, in maintainer mode, fails the
# build -- so this does not mask regressions the way a blanket -w would.
#
# MSVC -- libtidesdb uses C11 <stdatomic.h>, which MSVC only enables under
# /experimental:c11atomics (without it the build fails with C1189 "C atomic
# support is not enabled").  The Windows build compiles with /WX, so also
# apply the same targeted /wd disables the library uses upstream.
SET_TARGET_PROPERTIES(tidesdb_embedded PROPERTIES
                      C_STANDARD 11
                      POSITION_INDEPENDENT_CODE ON)

TARGET_INCLUDE_DIRECTORIES(tidesdb_embedded PRIVATE
                           ${TIDESDB_LIB_DIR}/src        # internal "tidesdb.h" includes
                           ${TIDESDB_LIB_DIR}/external)

# MYSQL_DYNAMIC_PLUGIN makes the include/providers/ shims resolve the codec
# service pointers the dynamic-plugin way (through the loaded provider), the
# same way MYSQL_ADD_PLUGIN compiles the plugin module.  Without it compress.c
# would reference the server-internal provider_service_*_static symbols, which
# a dynamically loaded engine cannot resolve.
TARGET_COMPILE_DEFINITIONS(tidesdb_embedded PRIVATE
                           _GNU_SOURCE
                           MYSQL_DYNAMIC_PLUGIN
                           ${TIDESDB_COMPRESS_DEFS}
                           $<$<BOOL:${TIDESDB_WITH_S3}>:TIDESDB_WITH_S3>)

IF(MSVC)
  TARGET_COMPILE_OPTIONS(tidesdb_embedded PRIVATE
                         /std:c17 /experimental:c11atomics
                         /wd4311 /wd4244 /wd4267 /wd4047 /wd4189 /wd4101
                         /wd4459 /wd4127 /wd4305 /wd4702 /wd4700 /wd4013)
ELSE()
  TARGET_COMPILE_OPTIONS(tidesdb_embedded PRIVATE
                         -Wno-declaration-after-statement
                         -Wno-frame-larger-than
                         -Wno-discarded-qualifiers)
ENDIF()

# Propagate the runtime dependencies to whatever links the archive (the plugin).
IF(HAVE_GCC_C11_ATOMICS_WITH_LIBATOMIC)
  SET(TIDESDB_ATOMIC_LIBS -latomic)
ENDIF()

# mysqlservices provides the provider_service_{zstd,lz4,snappy} symbols the
# compression shims reference; the server rebinds them to the loaded provider
# plugins at engine load time.  Listed here so they resolve when the archive
# is linked into the plugin module.
TARGET_LINK_LIBRARIES(tidesdb_embedded PUBLIC
                      mysqlservices
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

# Generate libtidesdb's public version/feature header from its template (as the
# upstream build does) so the plugin can report which libtidesdb version is
# vendored (TIDESDB_VERSION) and which optional features were compiled in
# (TIDESDB_HAS_S3, TIDESDB_HAVE_{ZSTD,LZ4,SNAPPY}).  The template substitutes
# @PROJECT_VERSION@; set it to the vendored libtidesdb version here and keep it
# in sync with libtidesdb upstream (project(... VERSION x.y.z)) when re-vendoring.
SET(PROJECT_VERSION "9.3.6")
CONFIGURE_FILE(${TIDESDB_LIB_DIR}/src/tidesdb_version.h.in
               ${TIDESDB_PUBLIC_INC}/tidesdb/tidesdb_version.h
               @ONLY)

TARGET_INCLUDE_DIRECTORIES(tidesdb PRIVATE ${TIDESDB_PUBLIC_INC})
TARGET_LINK_LIBRARIES(tidesdb tidesdb_embedded)

IF(TIDESDB_WITH_S3)
  TARGET_COMPILE_DEFINITIONS(tidesdb PRIVATE TIDESDB_WITH_S3)
ENDIF()
