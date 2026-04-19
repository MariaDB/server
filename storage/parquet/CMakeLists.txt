FIND_PACKAGE(CURL REQUIRED)

MYSQL_ADD_PLUGIN(parquet
  ha_parquet.cc
  ha_parquet_pushdown.cc
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
    ${CMAKE_SOURCE_DIR}/storage/duckdb/duckdb/third_parties/duckdb/src/include
  )
  target_link_libraries(parquet PRIVATE
    ${CMAKE_BINARY_DIR}/storage/duckdb/duckdb/duckdb-build/src/libduckdb.dylib
    CURL::libcurl
  )
endif()
