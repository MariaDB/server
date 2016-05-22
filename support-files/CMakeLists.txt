# Copyright (c) 2006, 2014, Oracle and/or its affiliates.
# Copyright (c) 2012, 2014, SkySQL Ab.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

IF(WIN32)
  SET(localstatedir "C:\\\\mysql\\\\data\\\\")
  SET(ini_file_extension "ini")
ELSE()
  SET(localstatedir "${MYSQL_DATADIR}")
  SET(prefix "${CMAKE_INSTALL_PREFIX}")
  SET(libexecdir ${INSTALL_SBINDIRABS})
  SET(bindir ${INSTALL_BINDIRABS})
  SET(sbindir "${libexecdir}")
  SET(datadir "${MYSQL_DATADIR}")
  SET(CC ${CMAKE_C_COMPILER})
  SET(CXX ${CMAKE_CXX_COMPILER})
  SET(CFLAGS ${CMAKE_C_FLAGS})
  SET(CXXFLAGS ${CMAKE_CXX_FLAGS})
  SET(MYSQLD_USER "mysql")
  SET(ini_file_extension "cnf")
  SET(HOSTNAME "hostname")
ENDIF()

# XXX: shouldn't we just have variables for all this stuff and centralise
# XXX: their configuration in install_layout.cmake?
IF(WIN32)
  SET(inst_location ${INSTALL_DOCREADMEDIR})
ELSE()
  SET(inst_location ${INSTALL_SUPPORTFILESDIR})
ENDIF()

FOREACH(inifile my-huge my-innodb-heavy-4G my-large my-medium my-small wsrep)
  CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${inifile}.cnf.sh 
                 ${CMAKE_CURRENT_BINARY_DIR}/${inifile}.${ini_file_extension} @ONLY)
  INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/${inifile}.${ini_file_extension}
          DESTINATION ${inst_location} COMPONENT IniFiles)
ENDFOREACH()

IF(UNIX)
  SET(prefix ${CMAKE_INSTALL_PREFIX})
  FOREACH(script mysqld_multi.server mysql-log-rotate binary-configure wsrep_notify)
    CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${script}.sh 
                   ${CMAKE_CURRENT_BINARY_DIR}/${script} @ONLY )
    INSTALL(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/${script}
      DESTINATION ${inst_location} COMPONENT Server_Scripts)
  ENDFOREACH()
  IF(INSTALL_SUPPORTFILESDIR)
    INSTALL(FILES magic DESTINATION ${inst_location} COMPONENT SupportFiles)
    INSTALL(DIRECTORY policy DESTINATION ${inst_location} COMPONENT SupportFiles)
  ENDIF()

  CONFIGURE_FILE(mariadb.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mariadb.pc @ONLY)
  INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mariadb.pc DESTINATION ${INSTALL_SHAREDIR}/pkgconfig COMPONENT Development)

  INSTALL(FILES mysql.m4 DESTINATION ${INSTALL_SHAREDIR}/aclocal COMPONENT Development)
  
  SET(bindir ${INSTALL_BINDIRABS})
  SET(sbindir ${INSTALL_SBINDIRABS})
  SET(scriptdir ${INSTALL_SCRIPTDIRABS})
  SET(libexecdir ${INSTALL_SBINDIRABS})
  SET(pkgdatadir ${INSTALL_MYSQLSHAREDIRABS})
  CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/mysql.server.sh 
                 ${CMAKE_CURRENT_BINARY_DIR}/mysql.server @ONLY)
  INSTALL(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/mysql.server
    DESTINATION ${inst_location} COMPONENT SupportFiles)

  IF(HAVE_SYSTEMD)
    CONFIGURE_FILE(mariadb.service.in
                   ${CMAKE_CURRENT_BINARY_DIR}/mariadb.service @ONLY)
    INSTALL(FILES use_galera_new_cluster.conf
            ${CMAKE_CURRENT_BINARY_DIR}/mariadb.service
            DESTINATION ${inst_location}/systemd COMPONENT SupportFiles)

    # @ in directory name broken between CMake version 2.8.12.2 and 3.3
    # http://public.kitware.com/Bug/view.php?id=14782
    IF(NOT CMAKE_VERSION VERSION_LESS 3.3.0 OR NOT RPM)
      CONFIGURE_FILE(mariadb@.service.in
                     ${CMAKE_CURRENT_BINARY_DIR}/mariadb@.service @ONLY)
      INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mariadb@.service
              DESTINATION ${inst_location}/systemd COMPONENT SupportFiles)
    ENDIF()

    IF(INSTALL_SYSTEMD_UNITDIR)
      INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mariadb.service
              DESTINATION ${INSTALL_SYSTEMD_UNITDIR} COMPONENT Server)

      # http://public.kitware.com/Bug/view.php?id=14782
      IF(NOT CMAKE_VERSION VERSION_LESS 3.3.0 OR NOT RPM)
        INSTALL(FILES use_galera_new_cluster.conf
                DESTINATION
                "${INSTALL_SYSTEMD_UNITDIR}/mariadb@bootstrap.service.d"
                COMPONENT Server)
        INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mariadb@.service
                DESTINATION ${INSTALL_SYSTEMD_UNITDIR} COMPONENT Server)
      ENDIF()

    ENDIF()
  ENDIF()

  IF (INSTALL_SYSCONFDIR)
    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mysql-log-rotate DESTINATION ${INSTALL_SYSCONFDIR}/logrotate.d
            RENAME mysql COMPONENT SupportFiles)
    INSTALL(PROGRAMS ${CMAKE_CURRENT_BINARY_DIR}/mysql.server
            DESTINATION  ${INSTALL_SYSCONFDIR}/init.d
            RENAME mysql COMPONENT SupportFiles)

    INSTALL(FILES rpm/my.cnf DESTINATION ${INSTALL_SYSCONFDIR}
            COMPONENT Common)

    IF (INSTALL_SYSCONF2DIR)
      INSTALL(FILES rpm/client.cnf DESTINATION ${INSTALL_SYSCONF2DIR}
              COMPONENT SharedLibraries)
      INSTALL(FILES rpm/mysql-clients.cnf DESTINATION ${INSTALL_SYSCONF2DIR}
              COMPONENT Client)
      INSTALL(FILES rpm/server.cnf DESTINATION ${INSTALL_SYSCONF2DIR}
              COMPONENT IniFiles)
      INSTALL(FILES rpm/enable_encryption.preset DESTINATION ${INSTALL_SYSCONF2DIR}
              COMPONENT IniFiles)
    ENDIF()

    # This is for SuSE:
    INSTALL(CODE "EXECUTE_PROCESS(COMMAND ${CMAKE_COMMAND} -E create_symlink
                   ${INSTALL_SYSCONFDIR}/init.d/mysql
                   ${INSTALL_SBINDIR}/rcmysql
                   WORKING_DIRECTORY \$ENV{DESTDIR}${prefix})"
                   COMPONENT SupportFiles)
  ENDIF(INSTALL_SYSCONFDIR)
ENDIF()
