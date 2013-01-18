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

if( WITH_WARNINGS )
add_definitions(-Wall -Wfatal-errors -Wextra)
message(STATUS "GCC: All warnings enabled")
else()
add_definitions(--no-warnings)
message(STATUS "GCC: All warnings disabled")
endif()

add_definitions( -DUNIX -DLINUX -DUBUNTU -DMARIADB -DFORCE_INIT_OF_VARS -DMYSQL_DYNAMIC_PLUGIN )
add_definitions( -DHUGE_SUPPORT -DXML_SUPPORT -DMYSQL_SUPPORT -DZIP_SUPPORT -DODBC_SUPPORT )

SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive -fexceptions -fPIC ")
INCLUDE_DIRECTORIES(/usr/include/libxml2)
INCLUDE_DIRECTORIES(/usr/include/i386-linux-gnu)
INCLUDE_DIRECTORIES()
get_property(inc_dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
message("inc_dirs = ${inc_dirs}")

SET(CONNECT_PLUGIN_STATIC  "connect")
SET(CONNECT_PLUGIN_DYNAMIC "connect")

SET(CONNECT_SOURCES  
ha_connect.cc connect.cc user_connect.cc mycat.cc
fmdlex.c inihandl.c osutil.c plugutil.c rcmsg.c
csort.cpp maputil.cpp plgdbutl.cpp 
colblk.cpp reldef.cpp tabcol.cpp table.cpp 
filamap.cpp filamdbf.cpp filamfix.cpp filamtxt.cpp filamvct.cpp
tabdos.cpp tabfix.cpp tabfmt.cpp tabmul.cpp tabsys.cpp tabvct.cpp   
valblk.cpp value.cpp xindex.cpp xobject.cpp
tabodbc.cpp odbconn.cpp
filamzip.cpp tabtbl.cpp myconn.cpp myutil.cpp tabmysql.cpp
tabxml.cpp plgxml.cpp libdoc.cpp)


SET(XML_LIBRARY "-L/usr/lib/i386-linux-gnu -lxml2")
#SET(MYSQL_LIBRARY "-L/usr/local/mysql/lib -lmysqlclient")
#SET(MYSQL_LIBRARY "-L/ ../../libmysql/libmysqlclient.a")

# For static linking
SET(MYSQL_LIBRARY mysqlclient)

# For dynaric linking
#SET(MYSQL_LIBRARY libmysql)

SET(ODBC_LIBRARY "-L/usr/lib -lodbc")

MYSQL_ADD_PLUGIN(connect  ${CONNECT_SOURCES}  
  STORAGE_ENGINE MODULE_ONLY 
  MODULE_OUTPUT_NAME "ha_connect"
  LINK_LIBRARIES ${ZLIB_LIBRARY} ${XML_LIBRARY} ${MYSQL_LIBRARY} ${ODBC_LIBRARY})
