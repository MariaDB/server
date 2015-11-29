# Copyright (c) 2011, 2014, Oracle and/or its affiliates. All rights reserved.
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

IF(PLUGIN_DAEMON_MEMCACHED STREQUAL "NO")
  RETURN()
ENDIF()

IF(NOT UNIX)
  RETURN()
ENDIF()

SET(DAEMON_MEMCACHED_WITH_LIBEVENT "dynamic"
    CACHE STRING "How to link libevent with daemon_memcached plugin. Possible values are 'static', 'dynamic' and default is 'dynamic'")
OPTION(DAEMON_MEMCACHED_ENABLE_SASL "Enable SASL on daemon_memcached plugin" OFF)
OPTION(DAEMON_MEMCACHED_ENABLE_SASL_PWDB "Enable SASL PWDB on daemon_memcached plugin" OFF)

IF(DAEMON_MEMCACHED_WITH_LIBEVENT STREQUAL "dynamic")
  CHECK_INCLUDE_FILES(event.h HAVE_EVENT_H)
  CHECK_LIBRARY_EXISTS(event event_init "" HAVE_EVENT_LIB)
  IF(HAVE_EVENT_H AND HAVE_EVENT_LIB)
    SET(DAEMON_MEMCACHED_LIBRARIES ${DAEMON_MEMCACHED_LIBRARIES} event)
  ELSE()
    MESSAGE(WARNING "Libevent not found. Plugin daemon_memcached will not be compiled")
    RETURN()
  ENDIF()
ELSEIF(DAEMON_MEMCACHED_WITH_LIBEVENT STREQUAL "static")
  CHECK_INCLUDE_FILES(event.h HAVE_EVENT_H)
  CHECK_LIBRARY_EXISTS(event.a event_init "" HAVE_EVENT_LIB)
  IF(HAVE_EVENT_H AND HAVE_EVENT_LIB)
    SET(DAEMON_MEMCACHED_LIBRARIES ${DAEMON_MEMCACHED_LIBRARIES} event.a)
  ELSE()
    MESSAGE(WARNING "Libevent not found. Plugin daemon_memcached will not be compiled")
    RETURN()
  ENDIF()
ENDIF()

ADD_SUBDIRECTORY(daemon_memcached)
ADD_SUBDIRECTORY(daemon_memcached_engine_ib)

INSTALL(FILES "${CMAKE_SOURCE_DIR}/plugin/daemon_memcached/daemon_memcached/scripts/daemon_memcached_config.sql"
        DESTINATION ${INSTALL_MYSQLSHAREDIR} COMPONENT Server
        PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ WORLD_READ)
