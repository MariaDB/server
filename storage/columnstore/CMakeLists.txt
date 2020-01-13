set(PLUGIN_COLUMNSTORE "NO" CACHE STRING "Enable ColumnStore engine")

if("NO" STREQUAL "${PLUGIN_COLUMNSTORE}")
  return()
endif()

IF(GIT_EXECUTABLE AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/columnstore/.git)
  EXECUTE_PROCESS(
    COMMAND ${GIT_EXECUTABLE} submodule update --init
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/columnstore)
ENDIF()

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCOLUMNSTORE_MATURITY=MariaDB_PLUGIN_MATURITY_GAMMA")
IF(INSTALL_SYSCONF2DIR)
  install(FILES x-columnstore.cnf
    DESTINATION ${INSTALL_SYSCONF2DIR} COMPONENT columnstore-engine)
ENDIF()

add_subdirectory(columnstore)

# Needed to bump the component changes up to the main scope
SET(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} PARENT_SCOPE)
IF (RPM)
    SET(CPACK_RPM_columnstore-platform_USER_FILELIST ${CPACK_RPM_columnstore-platform_USER_FILELIST} PARENT_SCOPE)
    SET(CPACK_RPM_columnstore-libs_USER_FILELIST ${CPACK_RPM_columnstore-libs_USER_FILELIST} PARENT_SCOPE)
    SET(CPACK_RPM_columnstore-engine_USER_FILELIST ${CPACK_RPM_columnstore-engine_USER_FILELIST} PARENT_SCOPE)
    SET(CPACK_RPM_columnstore-platform_PACKAGE_REQUIRES ${CPACK_RPM_columnstore-platform_PACKAGE_REQUIRES} PARENT_SCOPE)
ENDIF ()

# Tests are here and not in the engine tree (or the path thing wouldn't go so well)
GET_FILENAME_COMPONENT(subpath ${CMAKE_CURRENT_SOURCE_DIR} NAME)
INSTALL_MYSQL_TEST("${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/" "plugin/${subpath}")

# FIXME: Sergei added this for MTR, doesn't work in 10.5
#include(symlinks)
#CREATE_MARIADB_SYMLINK_IN_DIR(columnstore/lib/ha_columnstore.so ha_columnstore.so)
