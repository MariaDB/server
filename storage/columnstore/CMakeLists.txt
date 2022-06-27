#set(PLUGIN_COLUMNSTORE "NO" CACHE STRING "Enable ColumnStore engine")

if("NO" STREQUAL "${PLUGIN_COLUMNSTORE}")
  return()
endif()

add_definitions(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)
IF(NOT PLUGIN_PERFSCHEMA STREQUAL NO)
  add_definitions(-DHAVE_PSI_INTERFACE=)
ENDIF()


# this does everything, gets the var from the correct scope, appends new
# values, sets in the correct scope
macro(APPEND_FOR_CPACK V)
  get_directory_property(var DIRECTORY columnstore DEFINITION ${V})
  set(${V} "${var}${ARGN}" PARENT_SCOPE)
endmacro()

# don't use compression providers, there are standalone executables below
GET_PROPERTY(dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
LIST(REMOVE_ITEM dirs ${CMAKE_SOURCE_DIR}/include/providers)
SET_PROPERTY(DIRECTORY PROPERTY INCLUDE_DIRECTORIES "${dirs}")

IF(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
CMAKE_SYSTEM_PROCESSOR STREQUAL "amd64" OR
CMAKE_SYSTEM_PROCESSOR STREQUAL "aarch64")
    add_subdirectory(columnstore)

    IF(TARGET columnstore)
        # Needed to bump the component changes up to the main scope
        APPEND_FOR_CPACK(CPACK_COMPONENTS_ALL)
        IF (RPM)
            APPEND_FOR_CPACK(CPACK_RPM_columnstore-engine_PACKAGE_REQUIRES " binutils net-tools python3")
            APPEND_FOR_CPACK(CPACK_RPM_columnstore-engine_PACKAGE_RECOMMENDS " jemalloc")
            APPEND_FOR_CPACK(CPACK_RPM_columnstore-engine_USER_FILELIST ";%ignore /var/lib;%ignore /var")
            APPEND_FOR_CPACK(CPACK_RPM_columnstore-engine_PACKAGE_CONFLICTS " thrift MariaDB-columnstore-platform MariaDB-columnstore-libs")
            # these three don't have the list semantics, so no append here
            SET(CPACK_RPM_columnstore-engine_PRE_INSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/columnstore/build/preInstall_storage_engine.sh PARENT_SCOPE)
            SET(CPACK_RPM_columnstore-engine_POST_INSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/columnstore/build/postInstall_storage_engine.sh PARENT_SCOPE)
            SET(CPACK_RPM_columnstore-engine_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_CURRENT_SOURCE_DIR}/columnstore/build/preUn_storage_engine.sh PARENT_SCOPE)
            APPEND_FOR_CPACK(CPACK_RPM_columnstore-engine_PACKAGE_VERSION  "")
            SET(CPACK_RPM_columnstore-engine_PACKAGE_SUMMARY "MariaDB ColumnStore storage engine" PARENT_SCOPE)
            SET(CPACK_RPM_columnstore-engine_PACKAGE_DESCRIPTION "The MariaDB ColumnStore storage engine is a high-performance columnar analytical engine, aimed at rapid processing of analytical queries on very large amounts of data." PARENT_SCOPE)
        ENDIF()
        INSTALL_MYSQL_TEST("${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/" "plugin/columnstore")
    ENDIF()
ENDIF()
