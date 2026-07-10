INCLUDE(${CMAKE_SOURCE_DIR}/storage/duckdb/cmake/duckdb_target_setup.cmake)

SET(DUCKDB_COMMON_SOURCES
  duckdb_types.cc
  duckdb_log.cc
  duckdb_charset_collation.cc
  duckdb_timezone.cc
)

ADD_LIBRARY(duckdb_common STATIC ${DUCKDB_COMMON_SOURCES})
duckdb_setup_target(duckdb_common)

TARGET_INCLUDE_DIRECTORIES(duckdb_common PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
