# Copyright (c) 2006, 2016, Oracle and/or its affiliates.
# Copyright (c) 2012, 2017, MariaDB
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
  SET(MYSQLD_GROUP "mysql")
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

IF (NOT WITHOUT_SERVER)
FOREACH(inifile my-huge my-innodb-heavy-4G my-large my-medium my-small)
  CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/${inifile}.cnf.sh
                 ${CMAKE_CURRENT_BINARY_DIR}/${inifile}.${ini_file_extension} @ONLY)
  INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/${inifile}.${ini_file_extension}
          DESTINATION ${inst_location} COMPONENT IniFiles)
ENDFOREACH()
ENDIF()

IF(WITH_WSREP)
  CONFIGURE_FILE(${CMAKE_CURRENT_SOURCE_DIR}/wsrep.cnf.sh
                 ${CMAKE_CURRENT_BINARY_DIR}/wsrep.${ini_file_extension} @ONLY)
  INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/wsrep.${ini_file_extension}
          DESTINATION ${inst_location} COMPONENT IniFiles)
ENDIF()

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
    FIND_PROGRAM(CHECKMODULE checkmodule)
    FIND_PROGRAM(SEMODULE_PACKAGE semodule_package)
    MARK_AS_ADVANCED(CHECKMODULE SEMODULE_PACKAGE)

    # Build pp files in policy/selinux
    IF(CHECKMODULE AND SEMODULE_PACKAGE)
      FOREACH(pol mariadb)
        SET(src ${CMAKE_CURRENT_SOURCE_DIR}/policy/selinux/${pol}.te)
        SET(tmp ${CMAKE_CURRENT_BINARY_DIR}/${pol}.mod)
        SET(out ${CMAKE_CURRENT_BINARY_DIR}/${pol}.pp)
        ADD_CUSTOM_COMMAND(OUTPUT ${out}
          COMMAND ${CHECKMODULE} -M -m ${src} -o ${tmp}
          COMMAND ${SEMODULE_PACKAGE} -m ${tmp} -o ${out}
          COMMAND ${CMAKE_COMMAND} -E remove ${tmp}
        DEPENDS ${src})
        ADD_CUSTOM_TARGET(${pol}-pp ALL DEPENDS ${out})
        INSTALL(FILES ${out} DESTINATION ${inst_location}/policy/selinux COMPONENT SupportFiles)
      ENDFOREACH()
      IF(RPM)
        EXECUTE_PROCESS(COMMAND rpm -q --qf "%{VERSION}" libsepol."${CMAKE_SYSTEM_PROCESSOR}"
                        OUTPUT_VARIABLE LIBSEPOL_VERSION RESULT_VARIABLE err)
        IF (NOT err)
          SET(CPACK_RPM_server_PACKAGE_REQUIRES
            "${CPACK_RPM_server_PACKAGE_REQUIRES} libsepol >= ${LIBSEPOL_VERSION}"
            PARENT_SCOPE)
        ENDIF()
      ENDIF()
    ENDIF()
  ENDIF()

  CONFIGURE_FILE(mariadb.pc.in ${CMAKE_CURRENT_BINARY_DIR}/mariadb.pc @ONLY)
  INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/mariadb.pc DESTINATION ${INSTALL_LIBDIR}/pkgconfig COMPONENT Development)

  INSTALL(FILES mysql.m4 DESTINATION ${INSTALL_SHAREDIR}/aclocal COMPONENT Development)
  
  SET(bindir ${INSTALL_BINDIRABS})
  SET(sbindir ${INSTALL_SBINDIRABS})
  SET(scriptdir ${INSTALL_SCRIPTDIRABS})
  SET(libexecdir ${INSTALL_SBINDIRABS})
  SET(pkgdatadir ${INSTALL_MYSQLSHAREDIRABS})
  SET(sysconfdir ${INSTALL_SYSCONFDIR})
  SET(sysconf2dir ${INSTALL_SYSCONF2DIR})
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

  # Allow installing sysusers and tmpusers without requiring SYSTEMD on the
  # system. This is useful for distributions running other init systems to
  # parse these files and create appropriate users & tmpfiles.
  IF((HAVE_SYSTEMD OR INSTALL_SYSTEMD_SYSUSERS) AND INSTALL_SYSTEMD_SYSUSERSDIR)
    CONFIGURE_FILE(sysusers.conf.in
            ${CMAKE_CURRENT_BINARY_DIR}/sysusers.conf @ONLY)
    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/sysusers.conf
            DESTINATION ${INSTALL_SYSTEMD_SYSUSERSDIR}
            RENAME mariadb.conf COMPONENT Server)
  ENDIF()

  IF((HAVE_SYSTEMD OR INSTALL_SYSTEMD_TMPUSERS) AND INSTALL_SYSTEMD_TMPFILESDIR)
    get_filename_component(MYSQL_UNIX_DIR ${MYSQL_UNIX_ADDR} DIRECTORY)
    CONFIGURE_FILE(tmpfiles.conf.in
            ${CMAKE_CURRENT_BINARY_DIR}/tmpfiles.conf @ONLY)
    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/tmpfiles.conf
            DESTINATION ${INSTALL_SYSTEMD_TMPFILESDIR}
            RENAME mariadb.conf COMPONENT Server)
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
