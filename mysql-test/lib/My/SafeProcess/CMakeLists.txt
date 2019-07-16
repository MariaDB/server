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


IF (WIN32)
  ADD_EXECUTABLE(my_safe_process safe_process_win.cc)
  ADD_EXECUTABLE(my_safe_kill safe_kill_win.cc)
  TARGET_LINK_LIBRARIES(my_safe_kill dbghelp psapi)
ELSE()
  ADD_EXECUTABLE(my_safe_process safe_process.cc)
ENDIF()

IF(WITH_WSREP)
  ADD_EXECUTABLE(wsrep_check_version wsrep_check_version.c)
  TARGET_LINK_LIBRARIES(wsrep_check_version ${LIBDL})
ENDIF()

IF(NOT INSTALL_MYSQLTESTDIR)
  RETURN()
ENDIF()

SET(INSTALL_ARGS 
  DESTINATION "${INSTALL_MYSQLTESTDIR}/lib/My/SafeProcess" 
  COMPONENT Test
)

INSTALL(TARGETS my_safe_process ${INSTALL_ARGS})
IF(WITH_WSREP)
  INSTALL(TARGETS wsrep_check_version ${INSTALL_ARGS})
ENDIF()
IF (WIN32)
  INSTALL(TARGETS my_safe_kill ${INSTALL_ARGS})
ENDIF()

INSTALL(FILES Base.pm ${INSTALL_ARGS})
