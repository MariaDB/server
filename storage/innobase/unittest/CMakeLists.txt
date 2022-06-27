# Copyright (c) 2021, MariaDB Corporation.
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

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/unittest/mytap
                    ${CMAKE_SOURCE_DIR}/storage/innobase/include
                    ${CMAKE_SOURCE_DIR}/tpool)
ADD_EXECUTABLE(innodb_fts-t innodb_fts-t.cc)
TARGET_LINK_LIBRARIES(innodb_fts-t mysys mytap)
ADD_DEPENDENCIES(innodb_fts-t GenError)
MY_ADD_TEST(innodb_fts)
# See explanation in innobase/CmakeLists.txt
IF(CMAKE_SYSTEM_PROCESSOR MATCHES "ppc64|powerpc64|s390x")
  ADD_COMPILE_FLAGS(
      ../sync/srw_lock.cc
      COMPILE_FLAGS "-mhtm"
      )
ENDIF()
ADD_EXECUTABLE(innodb_sync-t innodb_sync-t.cc ../sync/srw_lock.cc)
TARGET_LINK_LIBRARIES(innodb_sync-t mysys mytap)
ADD_DEPENDENCIES(innodb_sync-t GenError)
MY_ADD_TEST(innodb_sync)
