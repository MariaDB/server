IF(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/duckdb/CMakeLists.txt)
  return()
ENDIF()

# libduckdb_bundle.a is built without debug STL wrappers.
# Mismatched _GLIBCXX_DEBUG changes sizeof(std::vector) → SIGSEGV.
SET(CMAKE_CXX_FLAGS_DEBUG
    "${CMAKE_CXX_FLAGS_DEBUG} -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS")

add_subdirectory(duckdb)

IF(TARGET duckdb)
  # MTR discovers plugins at storage/<name>/, not storage/<name>/<submodule>/
  SET_TARGET_PROPERTIES(duckdb PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
  INSTALL_MYSQL_TEST("${CMAKE_CURRENT_SOURCE_DIR}/duckdb/mysql-test/"
                     "plugin/duckdb")
ENDIF()
