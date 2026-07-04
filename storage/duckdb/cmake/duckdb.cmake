# Copyright (c) 2026, MariaDB Foundation.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA

#
# Build DuckDB static libraries from submodule source.
#
# The upstream DuckDB repo lives at storage/duckdb/third_parties/duckdb/
# as a git submodule.  We build it via ExternalProject so its CMake targets
# (including one named "duckdb") don't clash with the MariaDB plugin target
# of the same name created by MYSQL_ADD_PLUGIN().
#

SET(DUCKDB_SUBMODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_parties/duckdb")
SET(DUCKDB_INCLUDE_DIR   "${DUCKDB_SUBMODULE_DIR}/src/include")

INCLUDE(ExternalProject)

# Map MariaDB build type to a DuckDB-friendly one.
IF(CMAKE_BUILD_TYPE MATCHES "[Dd]ebug")
  SET(_DUCKDB_BUILD_TYPE "Debug")
ELSE()
  SET(_DUCKDB_BUILD_TYPE "Release")
ENDIF()

SET(_DUCKDB_BUILD_DIR "${CMAKE_CURRENT_BINARY_DIR}/duckdb-build")

# The individual static archives DuckDB produces (for the three extensions
# listed in cmake/duckdb_extensions.cmake: core_functions, icu, json).
SET(_DUCKDB_STATIC_LIBS
  "${_DUCKDB_BUILD_DIR}/src/libduckdb_static.a"
  "${_DUCKDB_BUILD_DIR}/extension/libduckdb_generated_extension_loader.a"
  "${_DUCKDB_BUILD_DIR}/extension/core_functions/libcore_functions_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/icu/libicu_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/jemalloc/libjemalloc_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/parquet/libparquet_extension.a"
  "${_DUCKDB_BUILD_DIR}/extension/json/libjson_extension.a"
)

MESSAGE(STATUS "=== Building DuckDB from submodule (${DUCKDB_SUBMODULE_DIR}) ===")
ADD_SUBMODULE(third_parties/duckdb)

ExternalProject_Add(duckdb_build
  PREFIX          "${_DUCKDB_BUILD_DIR}"
  SOURCE_DIR      "${DUCKDB_SUBMODULE_DIR}"
  BINARY_DIR      "${_DUCKDB_BUILD_DIR}"
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${_DUCKDB_BUILD_TYPE}
    -DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}
    -DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON
    -DBUILD_SHELL=OFF
    -DBUILD_UNITTESTS=OFF
    -DENABLE_UNITTEST_CPP_TESTS=OFF
    -DBUILD_PYTHON=OFF
    -DBUILD_BENCHMARKS=OFF
    -DBUILD_TPCE=OFF
    -DEXTENSION_STATIC_BUILD=1
    "-DDUCKDB_EXTENSION_CONFIGS=${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_extensions.cmake"
    # Upstream sets DUCKDB_EXTENSION_JEMALLOC_LINKED via add_extension_definitions(),
    # which runs in extension/ but NOT in src/, so allocator.cpp (in duckdb_static)
    # compiles the glibc malloc() path even though libjemalloc_extension.a is linked.
    # Define it globally + add the jemalloc header dir so the USE_JEMALLOC branch is active.
    "-DCMAKE_CXX_FLAGS=-DDUCKDB_EXTENSION_JEMALLOC_LINKED=1 -I${DUCKDB_SUBMODULE_DIR}/extension/jemalloc/include"
    -DENABLE_SANITIZER=FALSE
    -DENABLE_UBSAN=OFF
    -DOVERRIDE_GIT_DESCRIBE=v1.5.4-0-g0000000000
  INSTALL_COMMAND  ""
  BUILD_BYPRODUCTS ${_DUCKDB_STATIC_LIBS}
  USES_TERMINAL_BUILD ON
)

# Expose all DuckDB archives as a single INTERFACE target so the rest of the
# cmake tree links against "libduckdb" unchanged.
ADD_LIBRARY(libduckdb INTERFACE)
TARGET_LINK_LIBRARIES(libduckdb INTERFACE -Wl,--start-group ${_DUCKDB_STATIC_LIBS} -Wl,--end-group)
ADD_DEPENDENCIES(libduckdb duckdb_build)

MESSAGE(STATUS "DuckDB include: ${DUCKDB_INCLUDE_DIR}")
MESSAGE(STATUS "DuckDB libs:    ${_DUCKDB_STATIC_LIBS}")

INCLUDE_DIRECTORIES(BEFORE SYSTEM "${DUCKDB_INCLUDE_DIR}")
INCLUDE_DIRECTORIES(BEFORE SYSTEM "${DUCKDB_SUBMODULE_DIR}/third_party/re2")
SET(DUCKDB_LIBRARY libduckdb)
