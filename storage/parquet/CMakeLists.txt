cmake_minimum_required(VERSION 4.0)
project(MariaDB_Parquet)

MYSQL_ADD_PLUGIN(parquet
  ha_parquet.cc
  STORAGE_ENGINE
  MANDATORY
)

if(TARGET parquet)
  if(NOT TARGET libduckdb)
    message(FATAL_ERROR
      "The parquet plugin requires DuckDB support. "
      "Enable the DuckDB build first so libduckdb is available.")
  endif()

  target_include_directories(parquet PRIVATE
    ${CMAKE_SOURCE_DIR}/storage/duckdb/duckdb/third_parties/duckdb/src/include
  )
  target_link_libraries(parquet PRIVATE libduckdb)
endif()
