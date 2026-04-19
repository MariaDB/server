FIND_PACKAGE(CURL REQUIRED)

if(NOT TARGET libduckdb)
  MESSAGE(FATAL_ERROR
    "Parquet storage engine requires DuckDB target 'libduckdb'. "
    "Make sure the DuckDB submodule is initialized and configured "
    "(for example: git submodule update --init --recursive storage/duckdb/duckdb).")
endif()

MYSQL_ADD_PLUGIN(parquet
  ha_parquet.cc
  ha_parquet_pushdown.cc
  parquet_cross_engine_scan.cc
  parquet_shared.cc
  parquet_catalog.cc
  parquet_iceberg.cc
  parquet_metadata.cc
  parquet_object_store.cc
  parquet_transaction.cc
  STORAGE_ENGINE
  MANDATORY
)

if(TARGET parquet)
  target_include_directories(parquet PRIVATE
    ${CMAKE_SOURCE_DIR}/storage/columnstore/columnstore/utils/json
    ${CMAKE_SOURCE_DIR}/storage/duckdb/duckdb
    ${CMAKE_SOURCE_DIR}/storage/duckdb/duckdb/third_parties/duckdb/src/include
  )
  target_link_libraries(parquet PRIVATE
    libduckdb
    CURL::libcurl
  )
endif()
