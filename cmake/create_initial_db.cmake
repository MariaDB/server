# Copyright (c) 2009, 2012, Oracle and/or its affiliates. All rights reserved.
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

# This script creates initial database for packaging on Windows
# Force Visual Studio to output to stdout

# This script can be run with "make initial_database" or
# "cmake --build .--target initial_database"

IF(ENV{VS_UNICODE_OUTPUT})
 SET ($ENV{VS_UNICODE_OUTPUT})
ENDIF()
IF(CMAKE_CFG_INTDIR AND CONFIG)
  #Resolve build configuration variables
  STRING(REPLACE "${CMAKE_CFG_INTDIR}" ${CONFIG} MYSQLD_EXECUTABLE 
    "${MYSQLD_EXECUTABLE}")
ENDIF()

# Create bootstrapper SQL script
FILE(WRITE bootstrap.sql "use mysql;\n" )
FOREACH(FILENAME mysql_system_tables.sql mysql_system_tables_data.sql mysql_performance_tables.sql)
   FILE(STRINGS ${TOP_SRCDIR}/scripts/${FILENAME} CONTENTS)
   FOREACH(STR ${CONTENTS})
    IF(NOT STR MATCHES "@current_hostname")
      FILE(APPEND bootstrap.sql "${STR}\n")
    ENDIF()
  ENDFOREACH()
ENDFOREACH()

FOREACH(FILENAME ${TOP_SRCDIR}/scripts/fill_help_tables.sql ${TOP_SRCDIR}/scripts/mysql_sys_schema.sql)
  FILE(READ ${FILENAME} CONTENTS)
  FILE(APPEND bootstrap.sql "${CONTENTS}")
ENDFOREACH()

MAKE_DIRECTORY(mysql)

SET(BOOTSTRAP_COMMAND 
  ${MYSQLD_EXECUTABLE} 
  --no-defaults 
  --console
  --bootstrap
  --datadir=.
)

GET_FILENAME_COMPONENT(CWD . ABSOLUTE)
EXECUTE_PROCESS(
  COMMAND "${CMAKE_COMMAND}" -E echo Executing ${BOOTSTRAP_COMMAND}
)
EXECUTE_PROCESS (
  COMMAND "${CMAKE_COMMAND}" -E
  echo input file bootstrap.sql, current directory ${CWD}
)
EXECUTE_PROCESS (  
  COMMAND ${BOOTSTRAP_COMMAND}
  INPUT_FILE bootstrap.sql
  OUTPUT_VARIABLE OUT
  ERROR_VARIABLE ERR
  RESULT_VARIABLE RESULT
 )

IF(NOT RESULT EQUAL 0)
  MESSAGE(FATAL_ERROR "Could not create initial database \n ${OUT} \n ${ERR}")
ENDIF()
