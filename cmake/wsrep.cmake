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
SET(WSREP_PATCH_VERSION "10")

# MariaDB addition: Revision number of the last revision merged from
# codership branch visible in @@version_comment.
# Branch : codership-mysql/5.6
SET(WSREP_PATCH_REVNO "4123")  # Should be updated on every merge.

# MariaDB addition: Revision number of the last revision merged from
# Branch : lp:maria/maria-10.0-galera
SET(WSREP_PATCH_REVNO2 "3867")  # Should be updated on every merge.

# MariaDB: Obtain patch revision number:
# Update WSREP_PATCH_REVNO if WSREP_REV environment variable is set.
IF (DEFINED ENV{WSREP_REV})
  SET(WSREP_PATCH_REVNO $ENV{WSREP_REV})
ENDIF()

SET(WSREP_INTERFACE_VERSION 25)

SET(WSREP_VERSION
    "${WSREP_INTERFACE_VERSION}.${WSREP_PATCH_VERSION}.r${WSREP_PATCH_REVNO}")

SET(WSREP_PROC_INFO ${WITH_WSREP})

IF(WITH_WSREP)
  SET(COMPILATION_COMMENT "${COMPILATION_COMMENT}, wsrep_${WSREP_VERSION}")
ENDIF()

