# Copyright 2010, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA

SET(WIXCA_SOURCES CustomAction.cpp CustomAction.def)

INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/sql ${CMAKE_CURRENT_SOURCE_DIR} ${WIX_INCLUDE_DIR})

# Custom action should not depend on C runtime, since we do not know if CRT is installed.
FORCE_STATIC_CRT()
ADD_VERSION_INFO(wixca SHARED WIXCA_SOURCES)
ADD_LIBRARY(wixca SHARED EXCLUDE_FROM_ALL ${WIXCA_SOURCES} ${CMAKE_SOURCE_DIR}/sql/winservice.c)
TARGET_LINK_LIBRARIES(wixca ${WIX_WCAUTIL_LIBRARY} ${WIX_DUTIL_LIBRARY} msi version)
