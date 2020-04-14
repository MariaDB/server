# Copyright (c) 2006, 2017, Oracle and/or its affiliates.
# Copyright (c) 2011, 2017, MariaDB Corporation
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

# Build comp_sql - used for embedding SQL in C or C++ programs

MACRO(INSTALL_LINK old new destination component)
  EXECUTE_PROCESS(
    COMMAND ${CMAKE_COMMAND} -E create_symlink ${old} ${new}
    WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  )
  INSTALL(
    PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/${new}
    DESTINATION ${destination}
    COMPONENT ${component}
  )
ENDMACRO()

IF(NOT CMAKE_CROSSCOMPILING)
 INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include)
 ADD_EXECUTABLE(comp_sql comp_sql.c)
 TARGET_LINK_LIBRARIES(comp_sql)
ENDIF()

# Build mysql_fix_privilege_tables.sql (concatenate 3 sql scripts)
IF(NOT WIN32 OR CMAKE_CROSSCOMPILING)
  FIND_PROGRAM(CAT_EXECUTABLE cat DOC "path to the executable")
  MARK_AS_ADVANCED(CAT_EXECUTABLE)
ENDIF()

IF(CAT_EXECUTABLE)
  SET(CAT_COMMAND COMMAND
    ${CMAKE_COMMAND} -E chdir  ${CMAKE_CURRENT_SOURCE_DIR}
    ${CAT_EXECUTABLE} mysql_system_tables_fix.sql mysql_system_tables.sql mysql_performance_tables.sql >
    ${CMAKE_CURRENT_BINARY_DIR}/mysql_fix_privilege_tables.sql
  )
ELSEIF(WIN32)
  FILE(TO_NATIVE_PATH ${CMAKE_CURRENT_BINARY_DIR}/mysql_fix_privilege_tables.sql
   native_outfile )
  SET(CAT_COMMAND
   COMMAND ${CMAKE_COMMAND} -E chdir ${CMAKE_CURRENT_SOURCE_DIR}
   cmd /c copy /b mysql_system_tables_fix.sql + mysql_system_tables.sql +  mysql_performance_tables.sql
   ${native_outfile} )
ELSE()
  MESSAGE(FATAL_ERROR "Cannot concatenate files")
ENDIF()

# Build mysql_fix_privilege_tables.c
ADD_CUSTOM_COMMAND(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/mysql_fix_privilege_tables_sql.c
  ${CAT_COMMAND}
  COMMAND comp_sql
  mysql_fix_privilege_tables
  mysql_fix_privilege_tables.sql
  mysql_fix_privilege_tables_sql.c
  WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
  DEPENDS comp_sql
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_system_tables.sql
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_system_tables_fix.sql
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_performance_tables.sql
)

# Add target for the above to be built
ADD_CUSTOM_TARGET(GenFixPrivs
  ALL
  DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/mysql_fix_privilege_tables_sql.c
)

IF(UNIX)
 FILE(WRITE ${CMAKE_CURRENT_BINARY_DIR}/make_binary_distribution
   "cd ${CMAKE_BINARY_DIR} && '${CMAKE_CPACK_COMMAND}' -G TGZ --config CPackConfig.cmake\n" )
 EXECUTE_PROCESS(
  COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/make_binary_distribution
)
ENDIF()

# Configure two scripts from one 'in' file.
# The maria_add_gis_sp.sql - to be sent to 'mysql' tool
# and the maria_add_gis_sp_bootstrap.sql, that can be sent to
# the server as a bootstrap command.

SET(ADD_GIS_SP_SET_DELIMITER "delimiter |")
SET(ADD_GIS_SP_RESET_DELIMITER "delimiter ;")
SET(ADD_GIS_SP_EOL "|")
CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/maria_add_gis_sp.sql.in
  ${CMAKE_CURRENT_BINARY_DIR}/maria_add_gis_sp.sql ESCAPE_QUOTES @ONLY)

SET(ADD_GIS_SP_SET_DELIMITER "")
SET(ADD_GIS_SP_RESET_DELIMITER "")
SET(ADD_GIS_SP_EOL ";")
CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/maria_add_gis_sp.sql.in
  ${CMAKE_CURRENT_BINARY_DIR}/maria_add_gis_sp_bootstrap.sql ESCAPE_QUOTES @ONLY)

IF (NOT WITHOUT_SERVER)
INSTALL(FILES
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_system_tables.sql
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_system_tables_data.sql
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_performance_tables.sql
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_test_db.sql
  ${CMAKE_CURRENT_SOURCE_DIR}/fill_help_tables.sql
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_test_data_timezone.sql
  ${CMAKE_CURRENT_SOURCE_DIR}/mysql_to_mariadb.sql
  ${CMAKE_CURRENT_BINARY_DIR}/maria_add_gis_sp.sql
  ${CMAKE_CURRENT_BINARY_DIR}/maria_add_gis_sp_bootstrap.sql
  ${FIX_PRIVILEGES_SQL}
  DESTINATION ${INSTALL_MYSQLSHAREDIR} COMPONENT Server
)
ENDIF()

# TCMalloc hacks
IF(MALLOC_LIB)
  MESSAGE("Using tcmalloc '${MALLOC_LIB}'")
  INSTALL(FILES ${MALLOC_LIB} DESTINATION ${INSTALL_LIBDIR} OPTIONAL)
ENDIF()

IF(CMAKE_GENERATOR MATCHES "Makefiles|Ninja")
  FOREACH(ARCH ${CMAKE_OSX_ARCHITECTURES})
    SET(CFLAGS   "${CFLAGS} -arch ${ARCH}")
    SET(CXXFLAGS "${CXXFLAGS} -arch ${ARCH}")
  ENDFOREACH()
ENDIF()

IF(CMAKE_SYSTEM_NAME MATCHES "FreeBSD")
  SET (PERL_PATH "/usr/local/bin/perl")
ELSE()
  SET (PERL_PATH "/usr/bin/perl")
ENDIF()

IF(UNIX)
  # FIND_PROC and CHECK_PID are used by mysqld_safe
IF(CMAKE_SYSTEM_NAME MATCHES "Linux")
  SET (FIND_PROC
    "ps wwwp $PID | grep -v mysqld_safe | grep -- $MYSQLD > /dev/null")
ENDIF()
IF(NOT FIND_PROC AND CMAKE_SYSTEM_NAME MATCHES "SunOS")
  SET (FIND_PROC
    "ps -p $PID | grep -v mysqld_safe | grep -- $MYSQLD > /dev/null")
ENDIF()

IF(NOT FIND_PROC)
  # BSD style
  EXECUTE_PROCESS(COMMAND ps -uaxww OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
  IF(result MATCHES 0)
     SET( FIND_PROC
       "ps -uaxww | grep -v mysqld_safe | grep -- $MYSQLD | grep $PID > /dev/null")
  ENDIF()
ENDIF()

IF(NOT FIND_PROC)
  # SysV style
  EXECUTE_PROCESS(COMMAND ps -ef OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
  IF(result MATCHES 0)
    SET( FIND_PROC "ps -ef | grep -v mysqld_safe | grep -- $MYSQLD | grep $PID > /dev/null")
  ENDIF()
ENDIF()

EXECUTE_PROCESS(COMMAND sh -c "kill -0 $$" OUTPUT_QUIET ERROR_QUIET RESULT_VARIABLE result)
IF(result MATCHES 0)
  SET(CHECK_PID "kill -0 $PID > /dev/null 2> /dev/null")
ELSE()
  SET(CHECK_PID "kill -s SIGCONT $PID  > /dev/null 2> /dev/null")
ENDIF()

SET(HOSTNAME "hostname")
SET(MYSQLD_USER "mysql")
ENDIF(UNIX)

# Really ugly, one script, "mysql_install_db", needs prefix set to ".",
# i.e. makes access relative the current directory. This matches
# the documentation, so better not change this.

IF(INSTALL_LAYOUT MATCHES "STANDALONE")
  SET(prefix ".")
  SET(bindir ${prefix}/${INSTALL_BINDIR})
  SET(sbindir ${prefix}/${INSTALL_SBINDIR})
  SET(scriptdir ${prefix}/${INSTALL_BINDIR})
  SET(libexecdir ${prefix}/${INSTALL_SBINDIR})
  SET(pkgdatadir ${prefix}/${INSTALL_MYSQLSHAREDIR})
  SET(pkgplugindir ${prefix}/${INSTALL_PLUGINDIR})
  SET(localstatedir ${prefix}/data)
ELSE()
  SET(prefix "${CMAKE_INSTALL_PREFIX}")
  SET(bindir ${INSTALL_BINDIRABS})
  SET(sbindir ${INSTALL_SBINDIRABS})
  SET(scriptdir ${INSTALL_BINDIRABS})
  SET(libexecdir ${INSTALL_SBINDIRABS})
  SET(pkgdatadir ${INSTALL_MYSQLSHAREDIRABS})
  SET(pkgplugindir ${INSTALL_PLUGINDIRABS})
  SET(localstatedir ${MYSQL_DATADIR})
ENDIF()

SET(resolveip_locations "$basedir/${INSTALL_BINDIR} $basedir/bin")
SET(mysqld_locations "$basedir/${INSTALL_SBINDIR} $basedir/libexec $basedir/sbin $basedir/bin")
SET(errmsg_locations "$basedir/${INSTALL_MYSQLSHAREDIR}/english $basedir/share/english $basedir/share/mysql/english")
SET(pkgdata_locations "$basedir/${INSTALL_MYSQLSHAREDIR} $basedir/share $basedir/share/mysql")

# install mysql_install_db.sh at this point since it needs
# different values for the above variables that will change
# afterwards
IF(UNIX)
  CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/mysql_install_db.sh
    ${CMAKE_CURRENT_BINARY_DIR}/mariadb-install-db ESCAPE_QUOTES @ONLY)
  EXECUTE_PROCESS(COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/mariadb-install-db)
  INSTALL_SCRIPT(
    "${CMAKE_CURRENT_BINARY_DIR}/mariadb-install-db"
    DESTINATION ${INSTALL_SCRIPTDIR}
    COMPONENT Server)

  INSTALL_LINK(mariadb-install-db mysql_install_db ${INSTALL_SCRIPTDIR} Server)
ENDIF()

SET(prefix "${CMAKE_INSTALL_PREFIX}")
IF(INSTALL_SYSCONFDIR)
  SET(sysconfdir ${DEFAULT_SYSCONFDIR})
ELSE()
  SET(sysconfdir "/etc")
ENDIF()
SET(bindir ${INSTALL_BINDIRABS})
SET(libexecdir ${INSTALL_SBINDIRABS})
SET(scriptdir ${INSTALL_BINDIRABS})
SET(datadir ${INSTALL_MYSQLSHAREDIRABS})
SET(pkgdatadir ${INSTALL_MYSQLSHAREDIRABS})
SET(libsubdir  ${INSTALL_LIBDIR})
SET(pkgincludedir ${INSTALL_INCLUDEDIRABS})
SET(pkglibdir ${INSTALL_LIBDIRABS})
SET(pkgplugindir ${INSTALL_PLUGINDIRABS})
SET(localstatedir ${MYSQL_DATADIR})

SET(RPATH_OPTION "")
IF(CMAKE_SYSTEM_NAME STREQUAL "SunOS")
  SET(RPATH_OPTION "-R$pkglibdir")
ENDIF()

# some scripts use  @TARGET_LINUX@
IF(CMAKE_SYSTEM_NAME MATCHES "Linux")
  SET(TARGET_LINUX "true")
ELSE()
  SET(TARGET_LINUX "false")
ENDIF()

SET(mysql_config_COMPONENT COMPONENT Development)
SET(msql2mysql_COMPONENT COMPONENT Client)
SET(mysqlaccess_COMPONENT COMPONENT Client)
SET(mysql_find_rows_COMPONENT COMPONENT Client)
SET(mytop_COMPONENT Mytop)

IF(WIN32)
  # On Windows, some .sh and some .pl.in files are configured
  # The resulting files will have .pl extension (those are perl scripts)

  # Input files with pl.in extension
  SET(PLIN_FILES mysql_config)
  # Input files with .sh extension

  SET(SH_FILES mysql_convert_table_format mysqld_multi mysqldumpslow
    mysqlhotcopy)

  FOREACH(file ${PLIN_FILES})
    IF(NOT ${file}_COMPONENT)
      SET(${file}_COMPONENT Server_Scripts)
    ENDIF()
    CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${file}.pl.in
      ${CMAKE_CURRENT_BINARY_DIR}/${file}.pl ESCAPE_QUOTES @ONLY)
    INSTALL_SCRIPT(${CMAKE_CURRENT_BINARY_DIR}/${file}.pl COMPONENT ${${file}_COMPONENT})
  ENDFOREACH()

  FOREACH(file ${SH_FILES})
    CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${file}.sh
       ${CMAKE_CURRENT_BINARY_DIR}/${file}.pl ESCAPE_QUOTES @ONLY)
    INSTALL_SCRIPT(${CMAKE_CURRENT_BINARY_DIR}/${file}.pl COMPONENT Server_Scripts)
  ENDFOREACH()
ELSE()
  IF(WITH_WSREP)
    SET(WSREP_SCRIPTS
      wsrep_sst_mysqldump
      wsrep_sst_rsync
      wsrep_sst_mariabackup
    )
    # The following script is sourced from other SST scripts, so it should
    # not be made executable.
    SET(WSREP_SOURCE
      wsrep_sst_common
    )
  ENDIF()
  IF (NOT WITHOUT_SERVER)
    SET(SERVER_SCRIPTS
      mariadb-fix-extensions
      mariadbd-multi
      mariadbd-safe
      mariadb-dumpslow
    )
  ENDIF()
  # Configure this one, for testing, but do not install it.
  CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/mysql_config.pl.in
    ${CMAKE_CURRENT_BINARY_DIR}/mysql_config.pl ESCAPE_QUOTES @ONLY)
  # On Unix, most of the files end up in the bin directory
  SET(BIN_SCRIPTS
    msql2mysql
    mariadb-setpermission
    mariadb-secure-installation
    mariadb-access
    mariadb-convert-table-format
    mariadb-find-rows
    mysql_config
    mytop
    mariadb-hotcopy
    ${SERVER_SCRIPTS}
    ${WSREP_SCRIPTS}
    ${SYSTEMD_SCRIPTS}
  )

  FOREACH(file ${BIN_SCRIPTS})
    # set name of executable
    GET_SYMLINK(${file} binname)

    if("${binname}" STREQUAL "")
      set(binname ${file})
    endif()

    IF(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${binname}.sh)
      CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${binname}.sh
        ${CMAKE_CURRENT_BINARY_DIR}/${file} ESCAPE_QUOTES @ONLY)
    ELSEIF(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/${binname})
      CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${binname}
        ${CMAKE_CURRENT_BINARY_DIR}/${file} COPYONLY)
    ELSE()
      MESSAGE(FATAL_ERROR "Cannot find ${binname}.sh or ${binname} in "
        "${CMAKE_CURRENT_SOURCE_DIR}" )
    ENDIF()

    IF(NOT ${file}_COMPONENT)
      SET(${file}_COMPONENT Server)
    ENDIF()

    # install script
    INSTALL_SCRIPT(
      ${CMAKE_CURRENT_BINARY_DIR}/${file}
      DESTINATION ${INSTALL_BINDIR}
      COMPONENT ${${file}_COMPONENT}
    )

    # make scripts executable in build dir
    IF(EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${binname})
      EXECUTE_PROCESS(COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/${binname})
    ELSEIF(EXISTS ${CMAKE_CURRENT_BINARY_DIR}/${file})
      EXECUTE_PROCESS(COMMAND chmod +x ${CMAKE_CURRENT_BINARY_DIR}/${file})
    ENDIF()

    # Create symlink
    IF (NOT ${binname} STREQUAL ${file})
      INSTALL_LINK(${file} ${binname} ${INSTALL_BINDIR} ${${file}_COMPONENT})
    ENDIF()
  ENDFOREACH()

  INSTALL_LINK(wsrep_sst_rsync wsrep_sst_rsync_wan ${INSTALL_BINDIR} Server)
  FOREACH(file ${WSREP_SOURCE})
    CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${file}.sh
      ${CMAKE_CURRENT_BINARY_DIR}/${file} ESCAPE_QUOTES @ONLY)
    IF(NOT ${file}_COMPONENT)
      SET(${file}_COMPONENT Server)
    ENDIF()
    INSTALL(FILES
      ${CMAKE_CURRENT_BINARY_DIR}/${file}
      DESTINATION ${INSTALL_BINDIR}
      COMPONENT ${${file}_COMPONENT}
    )
  ENDFOREACH()
ENDIF()

# Install libgcc as  mylibgcc.a
IF(CMAKE_COMPILER_IS_GNUCXX AND CMAKE_CXX_FLAGS MATCHES "-static")
  EXECUTE_PROCESS (
   COMMAND ${CMAKE_CXX_COMPILER} ${CMAKE_CXX_COMPILER_ARG1}
     ${CMAKE_CXX_FLAGS} --print-libgcc
     OUTPUT_VARIABLE  LIBGCC_LOCATION
     RESULT_VARIABLE RESULT
     OUTPUT_STRIP_TRAILING_WHITESPACE
     ERROR_QUIET
  )
  IF(${RESULT} EQUAL 0 AND EXISTS ${LIBGCC_LOCATION})
    INSTALL(FILES "${LIBGCC_LOCATION}" DESTINATION ${INSTALL_LIBDIR}
    COMPONENT Development)
  ENDIF()
ENDIF()
