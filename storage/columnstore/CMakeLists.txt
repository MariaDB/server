#set(PLUGIN_COLUMNSTORE "NO" CACHE STRING "Enable ColumnStore engine")

if("NO" STREQUAL "${PLUGIN_COLUMNSTORE}")
  return()
endif()

IF(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
CMAKE_SYSTEM_PROCESSOR STREQUAL "amd64" OR
CMAKE_SYSTEM_PROCESSOR STREQUAL "i386" OR
CMAKE_SYSTEM_PROCESSOR STREQUAL "i686")
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -DCOLUMNSTORE_MATURITY=MariaDB_PLUGIN_MATURITY_BETA")
    add_subdirectory(columnstore)

    IF(TARGET columnstore)
        # Needed to bump the component changes up to the main scope
        SET(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} PARENT_SCOPE)
        IF (RPM)
            SET(CPACK_RPM_columnstore-engine_USER_FILELIST ${CPACK_RPM_columnstore-engine_USER_FILELIST} PARENT_SCOPE)
            # To untie the python mess across distributions MCS supports.
            IF(RPM MATCHES "(rhel|centos)8|(sles|suse)15|fedora")
                SET(CPACK_RPM_columnstore-engine_PACKAGE_REQUIRES "${CPACK_RPM_columnstore-engine_PACKAGE_REQUIRES}, MariaDB-server >= 10.5.4, python3, binutils" PARENT_SCOPE)
            ELSEIF(RPM MATCHES "(centos|rhel)7")
                SET(CPACK_RPM_columnstore-engine_PACKAGE_REQUIRES "${CPACK_RPM_columnstore-engine_PACKAGE_REQUIRES}, MariaDB-server >= 10.5.4, python, binutils" PARENT_SCOPE)
            ELSE()
                SET(CPACK_RPM_columnstore-engine_PACKAGE_REQUIRES "${CPACK_RPM_columnstore-engine_PACKAGE_REQUIRES}, MariaDB-server >= 10.5.4, python or python2 or python3, binutils" PARENT_SCOPE)
            ENDIF()
            SET(CPACK_RPM_columnstore-engine_PRE_INSTALL_SCRIPT_FILE ${CPACK_RPM_columnstore-engine_PRE_INSTALL_SCRIPT_FILE} PARENT_SCOPE)
            SET(CPACK_RPM_columnstore-engine_POST_INSTALL_SCRIPT_FILE ${CPACK_RPM_columnstore-engine_POST_INSTALL_SCRIPT_FILE} PARENT_SCOPE)
            SET(CPACK_RPM_columnstore-engine_PRE_UNINSTALL_SCRIPT_FILE ${CPACK_RPM_columnstore-engine_PRE_UNINSTALL_SCRIPT_FILE} PARENT_SCOPE)
            SET(CPACK_RPM_columnstore-engine_PACKAGE_CONFLICTS "thrift, MariaDB-columnstore-engine, MariaDB-columnstore-platform, MariaDB-columnstore-libs" PARENT_SCOPE)
        ENDIF()
        INSTALL_MYSQL_TEST("${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/" "plugin/columnstore")
    ENDIF()
ENDIF()
