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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA 

#
# Galera library does not compile with windows
#
IF(UNIX)
  SET(with_wsrep_default ON)
ELSE()
  SET(with_wsrep_default OFF)
ENDIF()

OPTION(WITH_WSREP "WSREP replication API (to use, e.g. Galera Replication library)" ${with_wsrep_default})

# Set the patch version
SET(WSREP_PATCH_VERSION "11")

# Obtain wsrep API version
FILE(STRINGS "${MySQL_SOURCE_DIR}/wsrep/wsrep_api.h" WSREP_API_VERSION
     LIMIT_COUNT 1 REGEX "WSREP_INTERFACE_VERSION")
STRING(REGEX MATCH "([0-9]+)" WSREP_API_VERSION "${WSREP_API_VERSION}")

SET(WSREP_VERSION "${WSREP_API_VERSION}.${WSREP_PATCH_VERSION}"
    CACHE INTERNAL "WSREP version")

SET(WSREP_PROC_INFO ${WITH_WSREP})

IF(WITH_WSREP)
  SET(WSREP_PATCH_VERSION "wsrep_${WSREP_VERSION}")
ENDIF()

