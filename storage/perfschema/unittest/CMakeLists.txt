# Copyright (c) 2009, 2019, Oracle and/or its affiliates. All rights reserved.
#
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License, version 2.0,
# as published by the Free Software Foundation.
#
# This program is also distributed with certain software (including
# but not limited to OpenSSL) that is licensed under separate terms,
# as designated in a particular file or component or in included license
# documentation.  The authors of MySQL hereby grant you an additional
# permission to link the program and your derivative works with the
# separately licensed software that they have included with MySQL.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License, version 2.0, for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/include/mysql
                    ${PCRE_INCLUDES}
                    ${CMAKE_SOURCE_DIR}/sql
                    ${SSL_INCLUDE_DIRS}
                    ${CMAKE_SOURCE_DIR}/unittest/mytap
                    ${CMAKE_SOURCE_DIR}/storage/perfschema)

ADD_DEFINITIONS(-DMYSQL_SERVER ${SSL_DEFINES})

ADD_CONVENIENCE_LIBRARY(pfs_server_stubs pfs_server_stubs.cc)

ADD_DEPENDENCIES(pfs_server_stubs GenError)

MY_ADD_TESTS(pfs_instr_class pfs_instr_class-oom pfs_instr pfs_instr-oom
             pfs_account-oom pfs_host-oom pfs_timer pfs_user-oom pfs pfs_misc
  EXT "cc" LINK_LIBRARIES perfschema mysys pfs_server_stubs)
