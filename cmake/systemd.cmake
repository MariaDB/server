# Copyright (c) 2015, Daniel Black. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA

MACRO(CHECK_SYSTEMD)
  IF(UNIX)
    INCLUDE(FindPkgConfig)
    # http://www.cmake.org/cmake/help/v3.0/module/FindPkgConfig.html
    SET(WITH_SYSTEMD "auto" CACHE STRING "Enable systemd scripts and notification support")
    IF(WITH_SYSTEMD STREQUAL "yes" OR WITH_SYSTEMD STREQUAL "auto")
      IF(PKG_CONFIG_FOUND)
        IF(WITH_SYSTEMD STREQUAL "yes")
          pkg_search_module(LIBSYSTEMD REQUIRED libsystemd libsystemd-daemon)
        ELSE()
          pkg_search_module(LIBSYSTEMD libsystemd libsystemd-daemon)
        ENDIF()
        IF(HAVE_DLOPEN)
          SET(LIBSYSTEMD ${LIBSYSTEMD_LDFLAGS} ${LIBSYSTEMD_LIBRARIES})
        ELSE()
          SET(LIBSYSTEMD ${LIBSYSTEMD_STATIC_LDFLAGS} ${LIBSYSTEMD_STATIC_LIBRARIES})
        ENDIF()
      ELSE()
        SET(LIBSYSTEMD systemd)
      ENDIF()
      SET(CMAKE_REQUIRED_LIBRARIES ${LIBSYSTEMD})
      CHECK_LIBRARY_EXISTS(systemd sd_listen_fds "" HAVE_SYSTEMD_SD_LISTEN_FDS)
      CHECK_INCLUDE_FILES(systemd/sd-daemon.h HAVE_SYSTEMD_SD_DAEMON_H)
      CHECK_FUNCTION_EXISTS(sd_notify HAVE_SYSTEMD_SD_NOTIFY)
      CHECK_FUNCTION_EXISTS(sd_notifyf HAVE_SYSTEMD_SD_NOTIFYF)
      SET(CMAKE_REQUIRED_LIBRARIES)
      IF(HAVE_SYSTEMD_SD_DAEMON_H AND HAVE_SYSTEMD_SD_LISTEN_FDS
         AND HAVE_SYSTEMD_SD_NOTIFY AND HAVE_SYSTEMD_SD_NOTIFYF)
        SET(HAVE_SYSTEMD TRUE)
        SET(SYSTEMD_SCRIPTS mariadb-service-convert galera_new_cluster galera_recovery)
        IF(DEB)
          SET(SYSTEMD_EXECSTARTPRE "ExecStartPre=/usr/bin/install -m 755 -o mysql -g root -d /var/run/mysqld")
          SET(SYSTEMD_EXECSTARTPOST "ExecStartPost=/etc/mysql/debian-start")
        ENDIF()
        MESSAGE_ONCE(systemd "Systemd features enabled")
      ELSE()
        UNSET(LIBSYSTEMD)
        UNSET(HAVE_SYSTEMD)
        UNSET(HAVE_SYSTEMD_SD_DAEMON_H)
        UNSET(HAVE_SYSTEMD_SD_LISTEN_FDS)
        UNSET(HAVE_SYSTEMD_SD_NOTIFY)
        UNSET(HAVE_SYSTEMD_SD_NOTIFYF)
        MESSAGE_ONCE(systemd "Systemd features not enabled")
        IF(WITH_SYSTEMD STREQUAL "yes")
          MESSAGE(FATAL_ERROR "Requested WITH_SYSTEMD=yes however no dependencies installed/found")
        ENDIF()
      ENDIF()
    ELSEIF(NOT WITH_SYSTEMD STREQUAL "no")
      MESSAGE(FATAL_ERROR "Invalid value for WITH_SYSTEMD. Must be 'yes', 'no', or 'auto'.")
    ENDIF()
    ADD_FEATURE_INFO(SYSTEMD LIBSYSTEMD "Systemd scripts and notification support")
  ENDIF()
ENDMACRO()
