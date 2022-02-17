# Copyright (c) 2011, Codership Oy <info@codership.com>.
# Copyright (c) 2013, Monty Program Ab.
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

#
# Galera library does not compile with windows
#
IF (NOT WITHOUT_SERVER)
IF(UNIX)
  SET(with_wsrep_default ON)
ELSE()
  SET(with_wsrep_default OFF)
ENDIF()

OPTION(WITH_WSREP "WSREP replication API (to use, e.g. Galera Replication library)" ${with_wsrep_default})
OPTION(WITH_WSREP_ALL
  "Build all components of WSREP (unit tests, sample programs)"
  OFF)

IF(WITH_WSREP)
  # Set the patch version
  SET(WSREP_PATCH_VERSION "22")

  IF(NOT EXISTS "${CMAKE_SOURCE_DIR}/wsrep-lib/wsrep-API/v26/wsrep_api.h")
    MESSAGE(FATAL_ERROR "No MariaDB wsrep-API code! Run
    ${GIT_EXECUTABLE} submodule update --init --recursive
Then restart the build.
")
  ENDIF()
  # Obtain wsrep API version
  FILE(STRINGS "${CMAKE_SOURCE_DIR}/wsrep-lib/wsrep-API/v26/wsrep_api.h" WSREP_API_VERSION
       LIMIT_COUNT 1 REGEX "WSREP_INTERFACE_VERSION")
  STRING(REGEX MATCH "([0-9]+)" WSREP_API_VERSION "${WSREP_API_VERSION}")

  SET(WSREP_VERSION "${WSREP_API_VERSION}.${WSREP_PATCH_VERSION}"
      CACHE INTERNAL "WSREP version")

  SET(WSREP_PROC_INFO ${WITH_WSREP})

  SET(WSREP_PATCH_VERSION "wsrep_${WSREP_VERSION}")
  if (NOT WITH_WSREP_ALL)
    SET(WSREP_LIB_WITH_UNIT_TESTS OFF CACHE BOOL
      "Disable unit tests for wsrep-lib")
    SET(WSREP_LIB_WITH_DBSIM OFF CACHE BOOL
      "Disable building dbsim for wsrep-lib")
  endif()
  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/wsrep-lib/include)
  INCLUDE_DIRECTORIES(${CMAKE_SOURCE_DIR}/wsrep-lib/wsrep-API/v26)

  SET(old_BUILD_SHARED_LIBS ${BUILD_SHARED_LIBS})
  SET(BUILD_SHARED_LIBS OFF)
  ADD_SUBDIRECTORY(wsrep-lib)
  SET(BUILD_SHARED_LIBS ${old_BUILD_SHARED_LIBS})
ENDIF()
IF (NOT WIN32)
  ADD_FEATURE_INFO(WSREP WITH_WSREP "WSREP replication API (to use, e.g. Galera Replication library)")
ENDIF()
ENDIF(NOT WITHOUT_SERVER)
