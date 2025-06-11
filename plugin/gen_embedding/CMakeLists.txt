# Copyright (c) 2019, MariaDB corporation. All rights reserved.
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
# Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
FIND_PACKAGE(CURL)
IF(NOT CURL_FOUND)
  # Can't build plugin
  MESSAGE_ONCE(WARNING "Generate OpenAI Embeddings plugin requires curl development package")
  RETURN()
ENDIF()

SET_PACKAGE_PROPERTIES(CURL PROPERTIES TYPE REQUIRED)
INCLUDE_DIRECTORIES(${CURL_INCLUDE_DIRS})

MYSQL_ADD_PLUGIN(
    gen_embedding plugin.cc 
    LINK_LIBRARIES ${CURL_LIBRARIES}
    RECOMPILE_FOR_EMBEDDED
    MODULE_ONLY
)
