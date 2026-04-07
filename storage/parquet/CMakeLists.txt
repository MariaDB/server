  SET(PARQUET_DUCKDB_INCLUDE_DIR
    "${CMAKE_SOURCE_DIR}/storage/duckdb/duckdb/third_parties/duckdb/src/include")

  ADD_EXECUTABLE(parquet_test
    parquet_test.cc
    ha_parquet.cc
    ../../unittest/sql/dummy_builtins.cc
  )

  TARGET_INCLUDE_DIRECTORIES(parquet_test PRIVATE
    ${CMAKE_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/sql
    ${CMAKE_SOURCE_DIR}/include
    ${CMAKE_SOURCE_DIR}/unittest/mytap
  )

  TARGET_COMPILE_OPTIONS(parquet_test PRIVATE -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS)
  TARGET_LINK_LIBRARIES(parquet_test sql mytap)

  IF(TARGET libduckdb)
    MYSQL_ADD_PLUGIN(parquet
      ha_parquet.cc
      STORAGE_ENGINE
      MANDATORY
      LINK_LIBRARIES libduckdb
    )

    IF(TARGET parquet)
      TARGET_INCLUDE_DIRECTORIES(parquet BEFORE SYSTEM PRIVATE
        ${PARQUET_DUCKDB_INCLUDE_DIR})
      TARGET_COMPILE_OPTIONS(parquet PRIVATE -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS)
    ENDIF()
  ELSE()
    MESSAGE(STATUS "DuckDB target not available; skipping parquet storage engine plugin")
  ENDIF()

  MYSQL_ADD_PLUGIN(parquet
  ha_parquet.cc
  STORAGE_ENGINE
  MANDATORY
)