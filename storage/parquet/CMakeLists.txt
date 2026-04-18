cmake_minimum_required(VERSION 4.0)
project(MariaDB_Parquet)

MYSQL_ADD_PLUGIN(parquet
  ha_parquet.cc
  ha_parquet_pushdown.cc
  parquet_catalog.cc
  parquet_iceberg.cc
  parquet_metadata.cc
  parquet_object_store.cc
  parquet_transaction.cc
  STORAGE_ENGINE
  MODULE_ONLY
)

if(TARGET parquet)
  if(NOT TARGET libduckdb)
    message(FATAL_ERROR
      "The parquet plugin requires DuckDB support. "
      "Enable the DuckDB build first so libduckdb is available.")
  endif()

  find_package(CURL REQUIRED)

  target_include_directories(parquet PRIVATE
    ${CMAKE_SOURCE_DIR}/storage/columnstore/columnstore/utils/json
    ${CMAKE_SOURCE_DIR}/storage/duckdb/duckdb/third_parties/duckdb/src/include
    ${CMAKE_SOURCE_DIR}/wsrep-lib/include
    ${CMAKE_SOURCE_DIR}/wsrep-lib/wsrep-API/v26
    ${CURL_INCLUDE_DIRS}
  )
  target_link_libraries(parquet libduckdb ${CURL_LIBRARIES})
endif()
