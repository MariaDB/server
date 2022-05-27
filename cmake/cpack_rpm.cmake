IF(RPM)

MESSAGE(STATUS "CPackRPM building with RPM configuration: ${RPM}")

SET(CPACK_GENERATOR "RPM")
SET(CPACK_RPM_PACKAGE_DEBUG 1)
SET(CPACK_PACKAGING_INSTALL_PREFIX ${CMAKE_INSTALL_PREFIX})
CMAKE_MINIMUM_REQUIRED(VERSION 2.8.7)

SET(CPACK_RPM_COMPONENT_INSTALL ON)

SET(CPACK_COMPONENT_SERVER_GROUP "server")
SET(CPACK_COMPONENT_MANPAGESSERVER_GROUP "server")
SET(CPACK_COMPONENT_INIFILES_GROUP "server")
SET(CPACK_COMPONENT_SERVER_SCRIPTS_GROUP "server")
SET(CPACK_COMPONENT_SUPPORTFILES_GROUP "server")
SET(CPACK_COMPONENT_DEVELOPMENT_GROUP "devel")
SET(CPACK_COMPONENT_MANPAGESDEVELOPMENT_GROUP "devel")
SET(CPACK_COMPONENT_TEST_GROUP "test")
SET(CPACK_COMPONENT_MANPAGESTEST_GROUP "test")
SET(CPACK_COMPONENT_CLIENT_GROUP "client")
SET(CPACK_COMPONENT_MANPAGESCLIENT_GROUP "client")
SET(CPACK_COMPONENT_README_GROUP "server")
SET(CPACK_COMPONENT_SHAREDLIBRARIES_GROUP "shared")
SET(CPACK_COMPONENT_COMMON_GROUP "common")
SET(CPACK_COMPONENT_CLIENTPLUGINS_GROUP "common")
SET(CPACK_COMPONENT_COMPAT_GROUP "compat")
SET(CPACK_COMPONENT_BACKUP_GROUP "backup")

SET(CPACK_COMPONENTS_ALL Server ManPagesServer IniFiles Server_Scripts
                         SupportFiles Development ManPagesDevelopment
                         ManPagesTest Readme ManPagesClient Test
                         Common Client SharedLibraries ClientPlugins
                         backup
)

SET(CPACK_RPM_PACKAGE_NAME ${CPACK_PACKAGE_NAME})
SET(CPACK_RPM_PACKAGE_VERSION ${CPACK_PACKAGE_VERSION})
IF(CMAKE_VERSION VERSION_LESS "3.6.0")
  SET(CPACK_PACKAGE_FILE_NAME "${CPACK_RPM_PACKAGE_NAME}-${SERVER_VERSION}-${RPM}-${CMAKE_SYSTEM_PROCESSOR}")
ELSE()
  SET(CPACK_RPM_FILE_NAME "RPM-DEFAULT")
  OPTION(CPACK_RPM_DEBUGINFO_PACKAGE "" ON)
  MARK_AS_ADVANCED(CPACK_RPM_DEBUGINFO_PACKAGE)
  SET(CPACK_RPM_BUILD_SOURCE_DIRS_PREFIX "/usr/src/debug/${CPACK_RPM_PACKAGE_NAME}-${CPACK_RPM_PACKAGE_VERSION}")
ENDIF()

SET(CPACK_RPM_PACKAGE_RELEASE "1%{?dist}")
SET(CPACK_RPM_PACKAGE_LICENSE "GPLv2")
SET(CPACK_RPM_PACKAGE_RELOCATABLE FALSE)
SET(CPACK_PACKAGE_RELOCATABLE FALSE)
SET(CPACK_RPM_PACKAGE_GROUP "Applications/Databases")
SET(CPACK_RPM_PACKAGE_URL ${CPACK_PACKAGE_URL})

SET(CPACK_RPM_shared_PACKAGE_VENDOR "MariaDB Corporation Ab")
SET(CPACK_RPM_shared_PACKAGE_LICENSE "LGPLv2.1")

# Set default description for packages
SET(CPACK_RPM_PACKAGE_DESCRIPTION "MariaDB: a very fast and robust SQL database server

It is GPL v2 licensed, which means you can use the it free of charge under the
conditions of the GNU General Public License Version 2 (http://www.gnu.org/licenses/).

MariaDB documentation can be found at https://mariadb.com/kb
MariaDB bug reports should be submitted through https://jira.mariadb.org")

# mariabackup
SET(CPACK_RPM_backup_PACKAGE_SUMMARY "Backup tool for MariaDB server")
SET(CPACK_RPM_backup_PACKAGE_DESCRIPTION "Mariabackup is an open source tool provided by MariaDB
for performing physical online backups of InnoDB, Aria and MyISAM tables.
For InnoDB, “hot online” backups are possible.
It was originally forked from Percona XtraBackup 2.3.8.")

# Packages with default description
SET(CPACK_RPM_client_PACKAGE_SUMMARY "MariaDB database client binaries")
SET(CPACK_RPM_client_PACKAGE_DESCRIPTION "${CPACK_RPM_PACKAGE_DESCRIPTION}")
SET(CPACK_RPM_common_PACKAGE_SUMMARY "MariaDB database common files (e.g. /etc/mysql/conf.d/mariadb.cnf)")
SET(CPACK_RPM_common_PACKAGE_DESCRIPTION "${CPACK_RPM_PACKAGE_DESCRIPTION}")
SET(CPACK_RPM_compat_PACKAGE_SUMMARY "MariaDB database client library MySQL compat package")
SET(CPACK_RPM_compat_PACKAGE_DESCRIPTION "${CPACK_RPM_PACKAGE_DESCRIPTION}")
SET(CPACK_RPM_devel_PACKAGE_SUMMARY "MariaDB database development files")
SET(CPACK_RPM_devel_PACKAGE_DESCRIPTION "${CPACK_RPM_PACKAGE_DESCRIPTION}")
SET(CPACK_RPM_server_PACKAGE_SUMMARY "MariaDB database server binaries")
SET(CPACK_RPM_server_PACKAGE_DESCRIPTION "${CPACK_RPM_PACKAGE_DESCRIPTION}")
SET(CPACK_RPM_test_PACKAGE_SUMMARY "MariaDB database regression test suite")
SET(CPACK_RPM_test_PACKAGE_DESCRIPTION "${CPACK_RPM_PACKAGE_DESCRIPTION}")

# libmariadb3
SET(CPACK_RPM_shared_PACKAGE_SUMMARY "LGPL MariaDB database client library")
SET(CPACK_RPM_shared_PACKAGE_DESCRIPTION "This is LGPL MariaDB client library that can be used to connect to MySQL
or MariaDB.

This code is based on the LGPL libmysql client library from MySQL 3.23
and PHP's mysqlnd extension.

This product includes PHP software, freely available from
http://www.php.net/software/")

SET(CPACK_RPM_SPEC_MORE_DEFINE "
%define mysql_vendor ${CPACK_PACKAGE_VENDOR}
%define mysqlversion ${MYSQL_NO_DASH_VERSION}
%define mysqlbasedir ${CMAKE_INSTALL_PREFIX}
%define mysqldatadir ${INSTALL_MYSQLDATADIR}
%define mysqld_user  mysql
%define mysqld_group mysql
%define _bindir     ${INSTALL_BINDIRABS}
%define _sbindir    ${INSTALL_SBINDIRABS}
%define _sysconfdir ${INSTALL_SYSCONFDIR}
%define restart_flag_dir %{_localstatedir}/lib/rpm-state/mariadb
%define restart_flag %{restart_flag_dir}/need-restart

%{?filter_setup:
%filter_provides_in \\\\.\\\\(test\\\\|result\\\\|h\\\\|cc\\\\|c\\\\|inc\\\\|opt\\\\|ic\\\\|cnf\\\\|rdiff\\\\|cpp\\\\)$
%filter_requires_in \\\\.\\\\(test\\\\|result\\\\|h\\\\|cc\\\\|c\\\\|inc\\\\|opt\\\\|ic\\\\|cnf\\\\|rdiff\\\\|cpp\\\\)$
%filter_from_provides /perl(\\\\(mtr\\\\|My::\\\\)/d
%filter_from_requires /\\\\(perl(\\\\(.*mtr\\\\|My::\\\\|.*HandlerSocket\\\\|Mysql\\\\)\\\\)/d
%filter_setup
}
")

# this creative hack is described here: http://www.cmake.org/pipermail/cmake/2012-January/048416.html
# both /etc and /etc/init.d should be ignored as of 2.8.7
# only /etc/init.d as of 2.8.8
# and eventually this hack should go away completely
SET(CPACK_RPM_SPEC_MORE_DEFINE "${CPACK_RPM_SPEC_MORE_DEFINE}
%define ignore \#
")

SET(CPACK_RPM_PACKAGE_REQUIRES "MariaDB-common")

SET(ignored
  "%ignore /etc"
  "%ignore /etc/init.d"
  "%ignore /etc/logrotate.d"
  "%ignore /etc/security"
  "%ignore /etc/systemd"
  "%ignore /etc/systemd/system"
  "%ignore /lib"
  "%ignore /lib/security"
  "%ignore /lib64"
  "%ignore /lib64/security"
  "%ignore ${CMAKE_INSTALL_PREFIX}"
  "%ignore ${CMAKE_INSTALL_PREFIX}/bin"
  "%ignore ${CMAKE_INSTALL_PREFIX}/include"
  "%ignore ${CMAKE_INSTALL_PREFIX}/lib"
  "%ignore ${CMAKE_INSTALL_PREFIX}/lib/systemd"
  "%ignore ${CMAKE_INSTALL_PREFIX}/lib/systemd/system"
  "%ignore ${CMAKE_INSTALL_PREFIX}/lib/tmpfiles.d"
  "%ignore ${CMAKE_INSTALL_PREFIX}/lib/sysusers.d"
  "%ignore ${CMAKE_INSTALL_PREFIX}/lib64"
  "%ignore ${CMAKE_INSTALL_PREFIX}/lib64/pkgconfig"
  "%ignore ${CMAKE_INSTALL_PREFIX}/sbin"
  "%ignore ${CMAKE_INSTALL_PREFIX}/share"
  "%ignore ${CMAKE_INSTALL_PREFIX}/share/aclocal"
  "%ignore ${CMAKE_INSTALL_PREFIX}/share/doc"
  "%ignore ${CMAKE_INSTALL_PREFIX}/share/man"
  "%ignore ${CMAKE_INSTALL_PREFIX}/share/man/man1"
  "%ignore ${CMAKE_INSTALL_PREFIX}/share/man/man8"
  "%ignore ${CMAKE_INSTALL_PREFIX}/share/pkgconfig"
  )

SET(CPACK_RPM_server_USER_FILELIST
    ${ignored}
    "%config(noreplace) ${INSTALL_SYSCONF2DIR}/*"
    "%config(noreplace) ${INSTALL_SYSCONFDIR}/logrotate.d/mysql"
    )
SET(CPACK_RPM_common_USER_FILELIST ${ignored} "%config(noreplace) ${INSTALL_SYSCONFDIR}/my.cnf")
SET(CPACK_RPM_shared_USER_FILELIST ${ignored} "%config(noreplace) ${INSTALL_SYSCONF2DIR}/*")
SET(CPACK_RPM_client_USER_FILELIST ${ignored} "%config(noreplace) ${INSTALL_SYSCONF2DIR}/*")
SET(CPACK_RPM_compat_USER_FILELIST ${ignored})
SET(CPACK_RPM_devel_USER_FILELIST ${ignored})
SET(CPACK_RPM_test_USER_FILELIST ${ignored})
SET(CPACK_RPM_backup_USER_FILELIST ${ignored})

# "set/append array" - append a set of strings, separated by a space
MACRO(SETA var)
  FOREACH(v ${ARGN})
    SET(${var} "${${var}} ${v}")
  ENDFOREACH()
ENDMACRO(SETA)

SETA(CPACK_RPM_client_PACKAGE_OBSOLETES
  "mysql-client"
  "MySQL-client"
  "mytop <= 1.7")
SETA(CPACK_RPM_client_PACKAGE_PROVIDES
  "MySQL-client"
  "mysql-client"
  "mytop")
SETA(CPACK_RPM_client_PACKAGE_CONFLICTS
  "MariaDB-server < 10.6.0")

SETA(CPACK_RPM_common_PACKAGE_CONFLICTS
  "MariaDB-server < 10.6.1")

SETA(CPACK_RPM_devel_PACKAGE_OBSOLETES
  "MySQL-devel")
SETA(CPACK_RPM_devel_PACKAGE_PROVIDES
  "MySQL-devel")
SETA(CPACK_RPM_devel_PACKAGE_REQUIRES
  "MariaDB-shared >= 10.2.42")

SETA(CPACK_RPM_server_PACKAGE_OBSOLETES
  "MariaDB"
  "MySQL"
  "mysql-server"
  "MySQL-server"
  "MariaDB-Galera-server")
SETA(CPACK_RPM_server_PACKAGE_PROVIDES
  "MariaDB"
  "MySQL"
  "MySQL-server"
  "msqlormysql"
  "mysql-server")

SETA(CPACK_RPM_test_PACKAGE_OBSOLETES
  "MySQL-test")
SETA(CPACK_RPM_test_PACKAGE_PROVIDES
  "MySQL-test")

SETA(CPACK_RPM_server_PACKAGE_REQUIRES
  "MariaDB-common >= 10.6.1"
  "MariaDB-client >= 10.6.1")

IF(WITH_WSREP)
  SETA(CPACK_RPM_server_PACKAGE_REQUIRES
    "galera-4" "rsync" "lsof" "grep" "gawk" "iproute"
    "coreutils" "findutils" "tar")
  SETA(CPACK_RPM_server_PACKAGE_RECOMMENDS "pv")
ENDIF()

SET(CPACK_RPM_server_PRE_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/server-prein.sh)
SET(CPACK_RPM_server_PRE_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/server-preun.sh)
SET(CPACK_RPM_server_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/server-postin.sh)
SET(CPACK_RPM_server_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/server-postun.sh)
SET(CPACK_RPM_server_POST_TRANS_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/server-posttrans.sh)
SET(CPACK_RPM_shared_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/shared-post.sh)
SET(CPACK_RPM_shared_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/shared-post.sh)
SET(CPACK_RPM_compat_POST_INSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/shared-post.sh)
SET(CPACK_RPM_compat_POST_UNINSTALL_SCRIPT_FILE ${CMAKE_SOURCE_DIR}/support-files/rpm/shared-post.sh)

MACRO(ALTERNATIVE_NAME real alt)
  IF(${ARGC} GREATER 2)
    SET(ver ${ARGV2})
  ELSE()
    SET(ver "${epoch}%{version}-%{release}")
  ENDIF()

  SET(p "CPACK_RPM_${real}_PACKAGE_PROVIDES")
  SET(${p} "${${p}} ${alt} = ${ver} ${alt}%{?_isa} = ${ver} config(${alt}) = ${ver}")
  SET(o "CPACK_RPM_${real}_PACKAGE_OBSOLETES")
  SET(${o} "${${o}} ${alt}")
ENDMACRO(ALTERNATIVE_NAME)

ALTERNATIVE_NAME("devel"  "mysql-devel")
ALTERNATIVE_NAME("server" "mysql-server")
ALTERNATIVE_NAME("test"   "mysql-test")

# Argh! Different distributions call packages differently, to be a drop-in
# replacement we have to fake distribution-specific dependencies
# NOTE, use ALTERNATIVE_NAME when a package has a different name
# in some distribution, it's not for adding new PROVIDES

IF(RPM MATCHES "(rhel|centos)6")
  ALTERNATIVE_NAME("client" "mysql")
ELSEIF(RPM MATCHES "fedora" OR RPM MATCHES "(rhel|centos)7")
  SET(epoch 1:)
  ALTERNATIVE_NAME("client" "mariadb")
  ALTERNATIVE_NAME("client" "mysql")
  ALTERNATIVE_NAME("devel"  "mariadb-devel")
  ALTERNATIVE_NAME("server" "mariadb-server")
  ALTERNATIVE_NAME("server" "mysql-compat-server")
  ALTERNATIVE_NAME("test"   "mariadb-test")
ELSEIF(RPM MATCHES "(rhel|centos)8")
  SET(epoch 3:)
  ALTERNATIVE_NAME("backup" "mariadb-backup")
  ALTERNATIVE_NAME("client" "mariadb")
  ALTERNATIVE_NAME("common" "mariadb-common")
  ALTERNATIVE_NAME("common" "mariadb-errmsg")
  ALTERNATIVE_NAME("server" "mariadb-server")
  ALTERNATIVE_NAME("server" "mariadb-server-utils")
  ALTERNATIVE_NAME("shared" "mariadb-connector-c" ${MARIADB_CONNECTOR_C_VERSION}-1)
  ALTERNATIVE_NAME("shared" "mariadb-connector-c-config" ${MARIADB_CONNECTOR_C_VERSION}-1)
  ALTERNATIVE_NAME("devel" "mariadb-connector-c-devel" ${MARIADB_CONNECTOR_C_VERSION}-1)
  SETA(CPACK_RPM_client_PACKAGE_PROVIDES "mariadb-galera = 3:%{version}-%{release}")
  SETA(CPACK_RPM_common_PACKAGE_PROVIDES "mariadb-galera-common = 3:%{version}-%{release}")
  SETA(CPACK_RPM_common_PACKAGE_REQUIRES "MariaDB-shared")
ELSEIF(RPM MATCHES "sles")
  ALTERNATIVE_NAME("server" "mariadb")
  SETA(CPACK_RPM_server_PACKAGE_PROVIDES
    "mysql = %{version}-%{release}"
    "mariadb_${MAJOR_VERSION}${MINOR_VERSION} = %{version}-%{release}"
    "mariadb-${MAJOR_VERSION}${MINOR_VERSION} = %{version}-%{release}"
    "mariadb-server = %{version}-%{release}"
  )
ENDIF()

# MDEV-24629, we need it outside of ELSIFs
IF(RPM MATCHES "fedora3[234]")
  ALTERNATIVE_NAME("common" "mariadb-connector-c-config" ${MARIADB_CONNECTOR_C_VERSION}-1)
ENDIF()

SET(PYTHON_SHEBANG "/usr/bin/python3" CACHE STRING "python shebang")

# If we want to build build MariaDB-shared-compat,
# extract compat libraries from MariaDB-shared-5.3 rpm
FILE(GLOB compat53 RELATIVE ${CMAKE_SOURCE_DIR}
    "${CMAKE_SOURCE_DIR}/../MariaDB-shared-5.3.*.rpm")
FILE(GLOB compat101 RELATIVE ${CMAKE_SOURCE_DIR}
    "${CMAKE_SOURCE_DIR}/../MariaDB-shared-10.1.*.rpm")
IF(compat53 AND compat101)
  FOREACH(compat_rpm "${compat53}" "${compat101}")
    MESSAGE(STATUS "Using ${compat_rpm} to build MariaDB-compat")
    INSTALL(CODE "EXECUTE_PROCESS(
                   COMMAND rpm2cpio ${CMAKE_SOURCE_DIR}/${compat_rpm}
                   COMMAND cpio --extract --make-directories */libmysqlclient*.so.* -
                   WORKING_DIRECTORY \$ENV{DESTDIR})
                  EXECUTE_PROCESS(
                   COMMAND chmod -R a+rX .
                   WORKING_DIRECTORY \$ENV{DESTDIR})"
                   COMPONENT Compat)
  ENDFOREACH()

  EXECUTE_PROCESS(
    COMMAND rpm -q --provides -p "${CMAKE_SOURCE_DIR}/${compat101}"
    ERROR_QUIET
    OUTPUT_VARIABLE compat_provides)
  EXECUTE_PROCESS(
    COMMAND rpm -q --obsoletes -p "${CMAKE_SOURCE_DIR}/${compat101}"
    ERROR_QUIET
    OUTPUT_VARIABLE compat_obsoletes)

  STRING(REPLACE "\n" " " compat_provides "${compat_provides}")
  STRING(REPLACE "\n" " " compat_obsoletes "${compat_obsoletes}")
  STRING(REGEX REPLACE "[^ ]+\\([^ ]+ *" "" compat_obsoletes "${compat_obsoletes}")
  SETA(CPACK_RPM_compat_PACKAGE_PROVIDES "${compat_provides}")
  SETA(CPACK_RPM_compat_PACKAGE_OBSOLETES "${compat_obsoletes}")

  SET(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} Compat)

  # RHEL6/CentOS6 install Postfix by default, and it requires
  # libmysqlclient.so.16 that pulls in mysql-libs-5.1.x
  # And the latter conflicts with our rpms.
  # Make sure that for these distribuions all our rpms require
  # MariaDB-compat, that will replace mysql-libs-5.1
  IF(RPM MATCHES "(rhel|centos)[67]")
    SET(CPACK_RPM_common_PACKAGE_REQUIRES "MariaDB-compat")
    SET(CPACK_RPM_compat_PACKAGE_CONFLICTS "mariadb-libs < 1:10.1.0")
  ENDIF()
ENDIF()

################
IF(CMAKE_VERSION VERSION_GREATER "3.9.99")

SET(CPACK_SOURCE_GENERATOR "RPM")
SETA(CPACK_RPM_SOURCE_PKG_BUILD_PARAMS "-DRPM=${RPM}")

MACRO(ADDIF var)
  IF(DEFINED ${var})
    SETA(CPACK_RPM_SOURCE_PKG_BUILD_PARAMS "-D${var}=${${var}}")
  ENDIF()
ENDMACRO()

ADDIF(MYSQL_MAINTAINER_MODE)
ADDIF(CMAKE_BUILD_TYPE)
ADDIF(BUILD_CONFIG)
ADDIF(WITH_SSL)
ADDIF(WITH_JEMALLOC)

ENDIF()
ENDIF(RPM)
