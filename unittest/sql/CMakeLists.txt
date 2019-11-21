# Copyright (c) 2006, 2013, Oracle and/or its affiliates. All rights reserved.
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

MY_ADD_TESTS(my_apc LINK_LIBRARIES mysys EXT cc)

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/sql
                    ${CMAKE_SOURCE_DIR}/include
                    ${CMAKE_SOURCE_DIR}/unittest/mytap
                    ${CMAKE_SOURCE_DIR}/extra/yassl/include)

IF(WIN32)
  ADD_EXECUTABLE(explain_filename-t explain_filename-t.cc
                                    ../../sql/nt_servc.cc)
ELSE()
  ADD_EXECUTABLE(explain_filename-t explain_filename-t.cc)
ENDIF()

TARGET_LINK_LIBRARIES(explain_filename-t sql mytap)
MY_ADD_TEST(explain_filename)

ADD_EXECUTABLE(mf_iocache-t mf_iocache-t.cc ../../sql/mf_iocache_encr.cc)
TARGET_LINK_LIBRARIES(mf_iocache-t mysys mytap mysys_ssl)
ADD_DEPENDENCIES(mf_iocache-t GenError)
MY_ADD_TEST(mf_iocache)
