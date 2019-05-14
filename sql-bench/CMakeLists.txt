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

# Install sql-bench files

IF(NOT INSTALL_SQLBENCHDIR)
 RETURN()
ENDIF()

IF(INSTALL_SQLBENCHDIR STREQUAL ".")
  SET(prefix)
ELSE()
 SET(prefix ${INSTALL_SQLBENCHDIR}/)
ENDIF()

INSTALL(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Data/ATIS
  DESTINATION  ${prefix}sql-bench/Data COMPONENT SqlBench)

INSTALL(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/Data/Wisconsin
  DESTINATION  ${prefix}sql-bench/Data COMPONENT SqlBench)

INSTALL(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/limits
  DESTINATION  ${prefix}sql-bench COMPONENT SqlBench)

SET(all_files README bench-count-distinct.sh bench-init.pl.sh
  compare-results.sh copy-db.sh crash-me.sh example.bat
  graph-compare-results.sh innotest1.sh innotest1a.sh innotest1b.sh
  innotest2.sh innotest2a.sh innotest2b.sh myisam.cnf pwd.bat
  run-all-tests.sh server-cfg.sh test-ATIS.sh test-alter-table.sh
  test-big-tables.sh test-connect.sh test-create.sh test-insert.sh
  test-select.sh test-table-elimination.sh test-transactions.sh
  test-wisconsin.sh uname.bat
  )

FOREACH(file ${all_files})
  GET_FILENAME_COMPONENT(ext ${file} EXT)
  GET_FILENAME_COMPONENT(name ${file} NAME)
  SET(target ${name})
  IF(ext MATCHES ".sh$")
    # Those are perl files actually
    STRING(REPLACE ".sh" "" target ${target} )
    IF(WIN32)
      IF(NOT ext MATCHES ".pl")
        SET(target "${target}.pl")
      ENDIF()
    ENDIF()
  ENDIF()
  SET(target "${CMAKE_CURRENT_BINARY_DIR}/${target}")
  CONFIGURE_FILE(${file} ${target} COPYONLY)
  IF (UNIX AND NOT ${name} MATCHES "README")
    EXECUTE_PROCESS(COMMAND chmod +x ${target})
  ENDIF()
  IF (ext MATCHES ".bat")
    IF(WIN32)
      INSTALL(PROGRAMS ${target}
        DESTINATION ${prefix}sql-bench COMPONENT SqlBench)
    ENDIF()
  ELSEIF(name MATCHES "README")
    INSTALL(FILES ${target}
      DESTINATION ${prefix}sql-bench COMPONENT SqlBench)
  ELSE()
    INSTALL(PROGRAMS ${target}
      DESTINATION ${prefix}sql-bench COMPONENT SqlBench)
  ENDIF()
ENDFOREACH()
