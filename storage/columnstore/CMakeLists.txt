#set(PLUGIN_COLUMNSTORE "NO" CACHE STRING "Enable ColumnStore engine")

if("NO" STREQUAL "${PLUGIN_COLUMNSTORE}" OR
   (NOT EXISTS ${CMAKE_CURRENT_LIST_DIR}/columnstore/CMakeLists.txt))
  return()
endif()

add_definitions(-DBOOST_BIND_GLOBAL_PLACEHOLDERS)
IF(NOT PLUGIN_PERFSCHEMA STREQUAL NO)
  add_definitions(-DHAVE_PSI_INTERFACE=)
ENDIF()

# let's temporarily disable the warning, until ColumnStore is fixed
MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-error=suggest-override")
MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-suggest-override")
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -U_GLIBCXX_DEBUG -U_GLIBCXX_ASSERTIONS")

# new gcc is quite buggy re. spurious warnings. let's shut it up
IF(CMAKE_COMPILER_IS_GNUCC AND CMAKE_C_COMPILER_VERSION VERSION_GREATER "12.0.0")
  # 12
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-error=restrict")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-error=use-after-free")
  # 13
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-error=maybe-uninitialized")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-error=dangling-reference")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-error=dangling-pointer")
ENDIF()
MY_CHECK_AND_SET_COMPILER_FLAG("-fno-strict-aliasing")

# let's temporarily disable the warning, until ColumnStore is fixed
MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-error=calloc-transposed-args")

SET(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--as-needed")

STRING(REGEX REPLACE "-Wframe-larger-than=[0-9]*" ""
       CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
STRING(REGEX REPLACE "-Wframe-larger-than=[0-9]*" ""
       CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
STRING(REGEX REPLACE "-Wframe-larger-than=[0-9]*" ""
       CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG}")

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
    MY_CHECK_AND_SET_COMPILER_FLAG("-fno-lto")
    SET(PCRE_INCLUDES "${PCRE_INCLUDE_DIRS}")
    add_subdirectory(columnstore)

    IF(TARGET columnstore)
	# Redo logic in cmake/plugin to prevent the attempted creation of *Symlinks package
	SET(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} columnstore-engineSymlinks)
	SET(CPACK_COMPONENT_COLUMNSTORE-ENGINESYMLINKS_GROUP columnstore-engine PARENT_SCOPE)
        SET(CPACK_COMPONENT_COLUMNSTORE-ENGINE_GROUP columnstore-engine PARENT_SCOPE)
        # Needed to bump the component changes up to the main scope
        APPEND_FOR_CPACK(CPACK_COMPONENTS_ALL)
        IF (RPM)
            APPEND_FOR_CPACK(CPACK_RPM_columnstore-engine_PACKAGE_REQUIRES " binutils net-tools python3 MariaDB-client-compat MariaDB-server-compat")
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
