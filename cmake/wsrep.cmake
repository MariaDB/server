# Copyright (c) 2011, Codership Oy <info@codership.com>.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA 

# We need to generate a proper spec file even without --with-wsrep flag,
# so WSREP_VERSION is produced regardless

# Set the patch version
SET(WSREP_PATCH_VERSION "6")

# Obtain patch revision number
SET(WSREP_PATCH_REVNO $ENV{WSREP_REV})
IF(NOT WSREP_PATCH_REVNO)
  EXECUTE_PROCESS(
    COMMAND bzr revno
    OUTPUT_VARIABLE WSREP_PATCH_REVNO
    RESULT_VARIABLE RESULT
  )
STRING(REGEX REPLACE "(\r?\n)+$" "" WSREP_PATCH_REVNO "${WSREP_PATCH_REVNO}")
#FILE(WRITE "wsrep_config" "Debug: WSREP_PATCH_REVNO result: ${RESULT}\n")
ENDIF()
IF(NOT WSREP_PATCH_REVNO)
  SET(WSREP_PATCH_REVNO "XXXX")
ENDIF()

# Obtain wsrep API version
EXECUTE_PROCESS(
  COMMAND sh -c "grep WSREP_INTERFACE_VERSION ${MySQL_SOURCE_DIR}/wsrep/wsrep_api.h | cut -d '\"' -f 2"
  OUTPUT_VARIABLE WSREP_API_VERSION
  RESULT_VARIABLE RESULT
)
#FILE(WRITE "wsrep_config" "Debug: WSREP_API_VERSION result: ${RESULT}\n")
STRING(REGEX REPLACE "(\r?\n)+$" "" WSREP_API_VERSION "${WSREP_API_VERSION}")

SET(WSREP_VERSION
    "${WSREP_API_VERSION}.${WSREP_PATCH_VERSION}.r${WSREP_PATCH_REVNO}"
)

OPTION(WITH_WSREP "WSREP replication API (to use, e.g. Galera Replication library)" OFF)
IF (WITH_WSREP)
  SET(WSREP_C_FLAGS   "-DWITH_WSREP -DWSREP_PROC_INFO -DMYSQL_MAX_VARIABLE_VALUE_LEN=2048")
  SET(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${WSREP_C_FLAGS}")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${WSREP_C_FLAGS}")
  SET(COMPILATION_COMMENT "${COMPILATION_COMMENT}, wsrep_${WSREP_VERSION}")
  SET(WITH_EMBEDDED_SERVER OFF)
ENDIF()

#
