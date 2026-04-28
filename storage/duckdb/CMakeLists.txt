IF(NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/duckdb/CMakeLists.txt)
  return()
ENDIF()

IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
ENDIF()

IF(NOT (CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
        CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64"))
  return()
ENDIF()

# Check C++17 compiler support
MY_CHECK_CXX_COMPILER_FLAG("-std=c++17")
IF(NOT have_CXX__std_c__17)
  MESSAGE(STATUS "DuckDB: C++ compiler does not support -std=c++17, skipping")
  RETURN()
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

  # MYSQL_ADD_PLUGIN runs in storage/duckdb/duckdb/ and bumps CPACK_* vars
  # one level up (here). Re-bump them to the root scope so include(CPack)
  # sees them. Without this, no MariaDB-duckdb-engine.spec is generated.
  SET(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} PARENT_SCOPE)
  SET(CPACK_COMPONENT_DUCKDB-ENGINE_GROUP
      ${CPACK_COMPONENT_DUCKDB-ENGINE_GROUP} PARENT_SCOPE)
  SET(CPACK_COMPONENT_DUCKDB-ENGINESYMLINKS_GROUP
      ${CPACK_COMPONENT_DUCKDB-ENGINESYMLINKS_GROUP} PARENT_SCOPE)
  SET(CPACK_RPM_duckdb-engine_PACKAGE_REQUIRES
      ${CPACK_RPM_duckdb-engine_PACKAGE_REQUIRES} PARENT_SCOPE)

  IF(RPM)
    SET(CPACK_RPM_duckdb-engine_PACKAGE_SUMMARY
        "MariaDB DuckDB storage engine" PARENT_SCOPE)
    SET(CPACK_RPM_duckdb-engine_PACKAGE_DESCRIPTION
        "The MariaDB DuckDB storage engine embeds DuckDB, an in-process analytical database, as a MariaDB storage engine for fast OLAP queries over local data." PARENT_SCOPE)

    # Mark common parent directories as %ignore so the duckdb-engine RPM
    # does not claim ownership of dirs owned by MariaDB-server/-common.
    # The "%define ignore \#" macro is already set by cmake/cpack_rpm.cmake.
    SET(CPACK_RPM_duckdb-engine_USER_FILELIST
      "%ignore /etc"
      "%ignore /etc/my.cnf.d"
      "%ignore /usr"
      "%ignore /usr/lib64"
      "%ignore /usr/lib64/mysql"
      "%ignore /usr/lib64/mysql/plugin"
      "%ignore /usr/share"
      "%ignore /usr/share/mysql"
      PARENT_SCOPE)

    # Apply DuckDB-specific CPack overrides at packaging time.
    # CPACK_PROJECT_CONFIG_FILE is included by CPack after CPackConfig.cmake,
    # letting these settings override the main project's.
    SET(CPACK_PROJECT_CONFIG_FILE
      "${CMAKE_CURRENT_SOURCE_DIR}/duckdb/cmake/cpack_overrides.cmake"
      CACHE FILEPATH "DuckDB CPack overrides" FORCE)
  ENDIF()
ENDIF()
