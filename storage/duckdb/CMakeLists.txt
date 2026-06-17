IF("NO" STREQUAL "${PLUGIN_DUCKDB}")
  return()
ENDIF()

IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  MESSAGE_ONCE(duckdb "DuckDB: not Linux, skipping")
  return()
ENDIF()

IF(NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
        CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64"))
  MESSAGE_ONCE(duckdb "DuckDB: not x86_64 or aarch64, skipping")
  return()
ENDIF()

# Check C++17 compiler support
MY_CHECK_CXX_COMPILER_FLAG("-std=c++17")
IF(NOT have_CXX__std_c__17)
  MESSAGE_ONCE(duckdb "DuckDB: C++ compiler does not support -std=c++17, skipping")
  RETURN()
ENDIF()

# libduckdb_bundle.a is built without debug STL wrappers.
# Mismatched _GLIBCXX_DEBUG changes sizeof(std::vector) → SIGSEGV.
SET(CMAKE_CXX_FLAGS_DEBUG
    "${CMAKE_CXX_FLAGS_DEBUG} -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS")

INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb.cmake)
INCLUDE(${CMAKE_CURRENT_SOURCE_DIR}/cmake/duckdb_target_setup.cmake)

# Provide MariaDB server include paths to sub-libraries.
# MYSQL_ADD_PLUGIN normally does this, but it runs after ADD_SUBDIRECTORY.
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/sql
                    ${CMAKE_BINARY_DIR}/include
                    ${PCRE_INCLUDE_DIRS}
                    ${SSL_INCLUDE_DIRS}
                    ${ZLIB_INCLUDE_DIRS})

# Build sub-libraries.
ADD_SUBDIRECTORY(common)
ADD_SUBDIRECTORY(convertor)
ADD_SUBDIRECTORY(runtime)

# Plugin sources (handler layer + UDFs whose extern "C" symbols must be
# in the final .so for dlsym() loading).
SET(DUCKDB_PLUGIN_SOURCES
  ha_duckdb.cc
  ha_duckdb_pushdown.cc
  duckdb_udf.cc
)

MYSQL_ADD_PLUGIN(duckdb ${DUCKDB_PLUGIN_SOURCES}
  STORAGE_ENGINE
  MODULE_ONLY
  LINK_LIBRARIES duckdb_runtime duckdb_convertor duckdb_common ${DUCKDB_LIBRARY}
)

IF(TARGET duckdb)
  duckdb_setup_target(duckdb)
  TARGET_INCLUDE_DIRECTORIES(duckdb PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/common
    ${CMAKE_CURRENT_SOURCE_DIR}/convertor
    ${CMAKE_CURRENT_SOURCE_DIR}/runtime
  )
ENDIF()
