# Copyright (c) 2006, 2010, Oracle and/or its affiliates. All rights reserved.
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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA

SET(CONNECT_PLUGIN_STATIC  "connect")
SET(CONNECT_PLUGIN_DYNAMIC "connect")

SET(CONNECT_SOURCES
ha_connect.cc connect.cc user_connect.cc mycat.cc
fmdlex.c osutil.c plugutil.c rcmsg.c
csort.cpp maputil.cpp plgdbutl.cpp
colblk.cpp reldef.cpp tabcol.cpp table.cpp
filamap.cpp filamdbf.cpp filamfix.cpp filamtxt.cpp filamvct.cpp
tabdos.cpp tabfix.cpp tabfmt.cpp tabmul.cpp tabsys.cpp tabvct.cpp
valblk.cpp value.cpp xindex.cpp xobject.cpp
filamzip.cpp tabtbl.cpp myconn.cpp myutil.cpp tabmysql.cpp)


#
# Definitions that are shared for all OSes
#
add_definitions( -DMYSQL_DYNAMIC_PLUGIN -DMARIADB -DFORCE_INIT_OF_VARS )
add_definitions( -DHUGE_SUPPORT -DZIP_SUPPORT )


#
# OS specific C flags, definitions and source files.
#
IF(UNIX)
  if(WITH_WARNINGS)
    add_definitions(-Wall -Wfatal-errors -Wextra)
    message(STATUS "CONNECT: GCC: All warnings enabled")
  else()
    add_definitions(--no-warnings)
    message(STATUS "CONNECT: GCC: All warnings disabled")
  endif(WITH_WARNINGS)

  add_definitions( -DUNIX -DLINUX -DUBUNTU )

  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive -fexceptions -fPIC ")
  get_property(inc_dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} inihandl.c)
  SET(IPHLPAPI_LIBRARY "")
ELSE(!UNIX)
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} tabwmi.cpp tabmac.cpp macutil.cpp)
  # Add exception handling to the CONNECT project)
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
  SET(IPHLPAPI_LIBRARY iphlpapi.lib)
ENDIF(UNIX)


#
# XML
#

IF(UNIX)
  FIND_PACKAGE(LibXml2)
  IF (LIBXML2_FOUND)
    INCLUDE_DIRECTORIES(${LIBXML2_INCLUDE_DIR})
    SET(XML_LIBRARY  ${LIBXML2_LIBRARIES})
    SET(CONNECT_SOURCES ${CONNECT_SOURCES} tabxml.cpp plgxml.cpp libdoc.cpp)
    add_definitions(-DXML_SUPPORT)
  ENDIF(LIBXML2_FOUND)
ELSE(!UNIX)
  # Windows
  # This to be localized
  INCLUDE_DIRECTORIES("D:/Libxml/include")
  SET(XML_LIBRARY "D:/libxml/lib/libxml2.lib")
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} tabxml.cpp plgxml.cpp libdoc.cpp)
  add_definitions(-DXML_SUPPORT)
  # Domdoc support
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} domdoc.cpp)
  add_definitions(-DDOMDOC_SUPPORT)
ENDIF(UNIX)


#
# MySQL
#

# For static linking
SET(MYSQL_LIBRARY mysqlclient)
# For dynamic linking
#SET(MYSQL_LIBRARY libmysql)
add_definitions(-DMYSQL_SUPPORT)
IF(!UNIX)
  #
  # TODO: remove this
  # change to use "#include "../../include/mysql.h" in the sources.
  INCLUDE_DIRECTORIES("../../include/mysql")
ENDIF(!UNIX)


#
# ODBC
#

if(UNIX)
  # Note, we currently detect unixODBC only on Linux.
  # TODO: detect iODBC as well. Simply adding "iodbc" into NAMES in
  # find_library does not work on machines with both unixODBC and iODBC
  # installed, because it finds headers from unixODBC while libraries
  # from iODBC. We could search for 'isql.h' instead of 'sql.h' so
  # the library 'libodbc' gets compiled with 'isql.h' and
  # the library 'libiodbc' gets compiled with 'sql'h.
  # This will also need changes in the sources (e.g. #include <isql.h>).

  find_path(ODBC_INCLUDE_DIR sql.h
  /usr/include
  /usr/include/odbc
  /usr/local/include
  /usr/local/include/odbc
  /usr/local/odbc/include
  #"C:/Program Files/ODBC/include"
  #"C:/Program Files/Microsoft SDKs/Windows/v7.0A/include"
  #"C:/Program Files/Microsoft SDKs/Windows/v6.0a/include"
  #"C:/Program Files (x86)/Microsoft SDKs/Windows/v7.0A/include"
  DOC "Specify the directory containing sql.h."
  )

  find_library(ODBC_LIBRARY
  NAMES odbc odbcinst odbc32
  PATHS
  /usr/lib
  /usr/lib/odbc
  /usr/local/lib
  /usr/local/lib/odbc
  /usr/local/odbc/lib
  #"C:/Program Files/ODBC/lib"
  #"C:/ODBC/lib/debug"
  #"C:/Program Files/Microsoft SDKs/Windows/v7.0A/Lib"
  #"C:/Program Files/Microsoft SDKs/Windows/v6.0A/Lib"
  #"C:/Program Files (x86)/Microsoft SDKs/Windows/v7.0A/Lib"
  DOC "Specify the ODBC driver manager library here."
  )

  IF(ODBC_INCLUDE_DIR AND ODBC_LIBRARY)
    INCLUDE_DIRECTORIES(${ODBC_INCLUDE_DIR})
    add_definitions(-DODBC_SUPPORT)
    SET(CONNECT_SOURCES ${CONNECT_SOURCES} tabodbc.cpp odbconn.cpp)
  ELSE()
    SET(ODBC_LIBRARY "")
  ENDIF()
ELSE(!UNIX)
  add_definitions(-DODBC_SUPPORT)
  SET(ODBC_LIBRARY "odbc32.lib odbccp32.lib")
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} tabodbc.cpp odbconn.cpp)
ENDIF(UNIX)


#
# iconv
#
IF(UNIX)
  SET(ICONV_LIBRARY "")
ELSE()
  #Windows
  # TODO: this is to be localized
  SET(ICONV_LIBRARY "D:/libxml/lib/iconv.lib")
ENDIF(UNIX)


#
# Plugin definition
#

MYSQL_ADD_PLUGIN(connect ${CONNECT_SOURCES}
  STORAGE_ENGINE MODULE_ONLY
  MODULE_OUTPUT_NAME "ha_connect"
  LINK_LIBRARIES ${ZLIB_LIBRARY} ${XML_LIBRARY} ${ICONV_LIBRARY}
                 ${MYSQL_LIBRARY} ${ODBC_LIBRARY} ${IPHLPAPI_LIBRARY})
