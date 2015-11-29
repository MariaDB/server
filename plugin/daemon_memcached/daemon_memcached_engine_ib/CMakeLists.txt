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

# Common defines and includes
INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/plugin/daemon_memcached/daemon_memcached_engine_ib
                    ${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/sql
                    ${CMAKE_SOURCE_DIR}/storage/innobase/include
                    ${CMAKE_SOURCE_DIR}/plugin/daemon_memcached/daemon_memcached_engine_ib/include
                    ${CMAKE_SOURCE_DIR}/plugin/daemon_memcached/daemon_memcached/include)

SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_SHARED_LIBRARY_C_FLAGS} -std=gnu99")

SET(CMAKE_CPP_FLAGS "${CMAKE_CPP_FLAGS}")

SET(DAEMON_MEMCACHED_ENGINE_IB_SOURCES
    src/innodb_config.c
    src/innodb_utility.c
    src/hash_item_util.c
    src/innodb_engine.c
    src/innodb_api.c
    src/handler_api.cc
    src/plugin_api.cc
    src/print_log.cc)

MYSQL_ADD_PLUGIN(daemon_memcached_engine_ib ${DAEMON_MEMCACHED_ENGINE_IB_SOURCES}
                 MODULE_ONLY MODULE_OUTPUT_NAME "daemon_memcached_engine_ib"
                 LINK_LIBRARIES daemon_memcached_utilities)
