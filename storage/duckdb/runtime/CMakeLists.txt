INCLUDE(${CMAKE_SOURCE_DIR}/storage/duckdb/cmake/duckdb_target_setup.cmake)

SET(DUCKDB_RUNTIME_SOURCES
  duckdb_config.cc
  duckdb_manager.cc
  duckdb_query.cc
  duckdb_context.cc
  delta_appender.cc
  cross_engine_scan.cc
  duckdb_mysql_compat.cc
  fiber_context.cpp
  fiber_scan.cc
)

ADD_LIBRARY(duckdb_runtime STATIC ${DUCKDB_RUNTIME_SOURCES})
duckdb_setup_target(duckdb_runtime)

TARGET_INCLUDE_DIRECTORIES(duckdb_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
TARGET_LINK_LIBRARIES(duckdb_runtime PUBLIC duckdb_convertor duckdb_common)

IF(DUCKDB_UNIT_TESTS)
  ADD_EXECUTABLE(fiber_context_test fiber_context_test.cc fiber_context.c)
  SET_TARGET_PROPERTIES(fiber_context_test PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON)
  TARGET_INCLUDE_DIRECTORIES(fiber_context_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  TARGET_COMPILE_OPTIONS(fiber_context_test PRIVATE -Wno-frame-larger-than=)
ENDIF()
