INCLUDE(${CMAKE_SOURCE_DIR}/storage/duckdb/cmake/duckdb_target_setup.cmake)

SET(DUCKDB_CONVERTOR_SOURCES
  ddl_convertor.cc
  dml_convertor.cc
  duckdb_select.cc
)

ADD_LIBRARY(duckdb_convertor STATIC ${DUCKDB_CONVERTOR_SOURCES})
duckdb_setup_target(duckdb_convertor)

TARGET_INCLUDE_DIRECTORIES(duckdb_convertor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
TARGET_LINK_LIBRARIES(duckdb_convertor PUBLIC duckdb_common)
