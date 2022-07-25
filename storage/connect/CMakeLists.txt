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
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335 USA

IF(WITHOUT_DYNAMIC_PLUGINS OR WITH_NONE OR ("${PLUGIN_CONNECT}" STREQUAL "NO"))
  RETURN()
ENDIF()

SET(CPACK_RPM_connect-engine_PACKAGE_SUMMARY "Connect storage engine for MariaDB" PARENT_SCOPE)
SET(CPACK_RPM_connect-engine_PACKAGE_DESCRIPTION "Connect engine supports a number of file formats (dbf, xml, txt, bin, etc),
connections to ODBC tables and remote MySQL tables, as well as a number of
other interesting features." PARENT_SCOPE)

SET(CONNECT_PLUGIN_STATIC  "connect")
SET(CONNECT_PLUGIN_DYNAMIC "connect")

SET(CONNECT_SOURCES
ha_connect.cc connect.cc user_connect.cc mycat.cc
fmdlex.c osutil.c rcmsg.c rcmsg.h
array.cpp blkfil.cpp colblk.cpp csort.cpp
filamap.cpp filamdbf.cpp filamfix.cpp filamgz.cpp filamtxt.cpp filter.cpp
json.cpp jsonudf.cpp maputil.cpp myconn.cpp myutil.cpp plgdbutl.cpp plugutil.cpp
reldef.cpp tabcol.cpp tabdos.cpp tabext.cpp tabfix.cpp tabfmt.cpp tabjson.cpp
table.cpp tabmul.cpp tabmysql.cpp taboccur.cpp tabpivot.cpp tabsys.cpp tabtbl.cpp
tabutil.cpp tabvir.cpp tabxcl.cpp valblk.cpp value.cpp xindex.cpp xobject.cpp

array.h blkfil.h block.h catalog.h checklvl.h colblk.h connect.h csort.h
engmsg.h filamap.h filamdbf.h filamfix.h filamgz.h filamtxt.h
filter.h global.h ha_connect.h inihandl.h json.h jsonudf.h maputil.h msgid.h
mycat.h myconn.h myutil.h os.h osutil.h plgcnx.h plgdbsem.h preparse.h reldef.h
resource.h tabcol.h tabdos.h tabext.h tabfix.h tabfmt.h tabjson.h tabmul.h
tabmysql.h taboccur.h tabpivot.h tabsys.h tabtbl.h tabutil.h tabvir.h tabxcl.h
user_connect.h valblk.h value.h xindex.h xobject.h xtable.h)

#
# Definitions that are shared for all OSes
#
add_definitions( -DMARIADB -DFORCE_INIT_OF_VARS -Dconnect_EXPORTS)
add_definitions( -DHUGE_SUPPORT -DGZ_SUPPORT )

macro(DISABLE_WARNING W)
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-error=${W}")
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-${W}" DEBUG)
endmacro()

#
# OS specific C flags, definitions and source files.
#
IF(UNIX)
  MY_CHECK_AND_SET_COMPILER_FLAG("-Wall -Wmissing-declarations")
  if(NOT WITH_WARNINGS)
    DISABLE_WARNING("unused-function")
    DISABLE_WARNING("unused-variable")
    DISABLE_WARNING("unused-value")
    DISABLE_WARNING("parentheses")
    DISABLE_WARNING("strict-aliasing")
    DISABLE_WARNING("misleading-indentation")
    DISABLE_WARNING("format-truncation")
    DISABLE_WARNING("implicit-fallthrough")
    DISABLE_WARNING("type-limits")
  endif(NOT WITH_WARNINGS)

  add_definitions( -DUNIX -DLINUX -DUBUNTU )

  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fpermissive -fexceptions -fPIC ")
  get_property(inc_dirs DIRECTORY PROPERTY INCLUDE_DIRECTORIES)
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} inihandl.cpp)
  SET(IPHLPAPI_LIBRARY "")
ELSE(NOT UNIX)
  SET(CONNECT_SOURCES ${CONNECT_SOURCES}
  tabwmi.cpp tabwmi.h tabmac.cpp tabmac.h macutil.cpp macutil.h)
  # Add exception handling to the CONNECT project)
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
  SET(IPHLPAPI_LIBRARY iphlpapi.lib)
  IF(MSVC AND (CMAKE_CXX_COMPILER_ID MATCHES Clang))
    # Connect does not work with clang-cl
    RETURN()
  ENDIF()
ENDIF(UNIX)


#
# BSON: the new handling of JSON data included temporarily for testing
#

OPTION(CONNECT_WITH_BSON "Compile CONNECT storage engine with BSON support" ON)

IF(CONNECT_WITH_BSON)
  SET(CONNECT_SOURCES ${CONNECT_SOURCES}
 bson.cpp bsonudf.cpp tabbson.cpp bson.h bsonudf.h tabbson.h)
  add_definitions(-DBSON_SUPPORT)
ENDIF(CONNECT_WITH_BSON)


#
# VCT: the VEC format might be not supported in future versions
#

OPTION(CONNECT_WITH_VCT "Compile CONNECT storage engine with VCT support" ON)

IF(CONNECT_WITH_VCT)
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} filamvct.cpp tabvct.cpp filamvct.h tabvct.h)
  add_definitions(-DVCT_SUPPORT)
ENDIF(CONNECT_WITH_VCT)
ADD_FEATURE_INFO(CONNECT_VCT CONNECT_WITH_VCT "Support for VCT in the CONNECT storage engine")


#
# XML
#

OPTION(CONNECT_WITH_LIBXML2 "Compile CONNECT storage engine with LIBXML2 support" ON)

IF(CONNECT_WITH_LIBXML2)
  IF(WIN32)
    #
    # NOTE: when switching to static linking of libxml2
    # make sure to define LIBXML_STATIC.
    #
    # Adding some typical places to search in
    SET(PC_LIBXML_INCLUDE_DIRS
        C:/libxml2/include
        C:/libxml/include
        D:/libxml/include)
    SET(PC_LIBXML_LIBRARY_DIRS
        C:/libxml2/lib
        C:/libxml/lib
        D:/libxml/lib)
  ENDIF(WIN32)
  FIND_PACKAGE(LibXml2)
  SET_PACKAGE_PROPERTIES(LibXml2 PROPERTIES TYPE OPTIONAL)
  IF (LIBXML2_FOUND)
    INCLUDE_DIRECTORIES(${LIBXML2_INCLUDE_DIR})
    SET(XML_LIBRARY  ${LIBXML2_LIBRARIES})
    SET(CONNECT_SOURCES ${CONNECT_SOURCES} libdoc.cpp libdoc.h)
    add_definitions(-DLIBXML2_SUPPORT)
  ENDIF(LIBXML2_FOUND)
ENDIF(CONNECT_WITH_LIBXML2)
ADD_FEATURE_INFO(CONNECT_LIBXML2 CONNECT_WITH_LIBXML2
                 "Support for LIBXML2 in the CONNECT storage engine")


IF(WIN32)
  # /MP option of the Microsoft compiler does not work well with COM #import
  string(REPLACE "/MP" "" CMAKE_C_FLAGS "${CMAKE_C_FLAGS}")
  string(REPLACE "/MP" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

  OPTION(CONNECT_WITH_MSXML "Compile CONNECT storage engine with MSXML support" ON)
  IF(CONNECT_WITH_MSXML)
    add_definitions(-DMSX6 -DDOMDOC_SUPPORT)
    SET(MSXML_FOUND 1)
    SET(CONNECT_SOURCES ${CONNECT_SOURCES} domdoc.cpp domdoc.h)
  ENDIF(CONNECT_WITH_MSXML)
  ADD_FEATURE_INFO(CONNECT_MSXML CONNECT_WITH_MSXML
                   "Support for MSXML in the CONNECT storage engine")
ENDIF(WIN32)

IF(LIBXML2_FOUND OR MSXML_FOUND)
  SET(CONNECT_SOURCES ${CONNECT_SOURCES}
  tabxml.cpp tabxml.h plgxml.cpp plgxml.h)
ENDIF()

#
# MySQL is now included unconditionnally
#

IF(NOT UNIX)
  #
  # TODO: remove this
  # change to use "#include "../../include/mysql.h" in the sources.
  INCLUDE_DIRECTORIES("../../include/mysql")
ENDIF(NOT UNIX)


#
# ODBC
#

OPTION(CONNECT_WITH_ODBC "Compile CONNECT storage engine with ODBC support" ON)

IF(CONNECT_WITH_ODBC)
  if(UNIX)
    # Note, we currently detect unixODBC only on Linux.
    # TODO: detect iODBC as well. Simply adding "iodbc" into NAMES in
    # find_library does not work on machines with both unixODBC and iODBC
    # installed, because it finds headers from unixODBC while libraries
    # from iODBC. We could search for 'isql.h' instead of 'sql.h' so
    # the library 'libodbc' gets compiled with 'isql.h' and
    # the library 'libiodbc' gets compiled with 'sql'h.
    # This will also need changes in the sources (e.g. #include <isql.h>).

    find_file(ODBC_INCLUDES sql.h
    PATHS
    /usr/include
    /usr/include/odbc
    /usr/local/include
    /usr/local/include/odbc
    /usr/local/odbc/include
    #"C:/Program Files/ODBC/include"
    #"C:/Program Files/Microsoft SDKs/Windows/v7.0A/include"
    #"C:/Program Files/Microsoft SDKs/Windows/v6.0a/include"
    #"C:/Program Files (x86)/Microsoft SDKs/Windows/v7.0A/include"
    DOC "Specify the path to sql.h."
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

    mark_as_advanced(ODBC_LIBRARY ODBC_INCLUDES)

    IF(ODBC_INCLUDES AND ODBC_LIBRARY)
      get_filename_component(ODBC_INCLUDE_DIR "${ODBC_INCLUDES}" PATH)
      set(CMAKE_REQUIRED_LIBRARIES ${ODBC_LIBRARY})
      set(CMAKE_REQUIRED_INCLUDES ${ODBC_INCLUDE_DIR})
      CHECK_CXX_SOURCE_COMPILES(
"
#include <stdio.h>
#include <sql.h>
#include <sqlext.h>
typedef long BOOL;   /* this fails with iODBC */
int main() {
  SQLULEN rowofs= 0; /* this fails on older unixODBC */
  SQLExtendedFetch(NULL, 0, 0, &rowofs, NULL);
  return 0;
}
" ODBC_OK)
    ENDIF()

    IF(ODBC_OK)
      INCLUDE_DIRECTORIES(${ODBC_INCLUDE_DIR})
      add_definitions(-DODBC_SUPPORT)
      SET(CONNECT_SOURCES ${CONNECT_SOURCES} tabodbc.cpp odbconn.cpp)
    ELSE()
      SET(ODBC_LIBRARY "")
    ENDIF()
  ELSE(NOT UNIX)
    add_definitions(-DODBC_SUPPORT)
    SET(ODBC_LIBRARY odbc32.lib odbccp32.lib)
    SET(CONNECT_SOURCES ${CONNECT_SOURCES}
    tabodbc.cpp tabodbc.h odbccat.h odbconn.cpp odbconn.h)
  ENDIF(UNIX)
ENDIF(CONNECT_WITH_ODBC)
ADD_FEATURE_INFO(CONNECT_ODBC ODBC_LIBRARY "Support for ODBC in the CONNECT storage engine")

#
# JDBC with MongoDB Java Driver included but disabled if without MONGO
#
OPTION(CONNECT_WITH_MONGO "Compile CONNECT storage engine with MONGO support" ON)
OPTION(CONNECT_WITH_JDBC "Compile CONNECT storage engine with JDBC support" ON)

IF(CONNECT_WITH_JDBC)
  FIND_PACKAGE(Java 1.6)
  SET_PACKAGE_PROPERTIES(Java PROPERTIES TYPE OPTIONAL)
  FIND_PACKAGE(JNI)
  SET_PACKAGE_PROPERTIES(JNI PROPERTIES TYPE OPTIONAL)
  IF (JAVA_FOUND AND JNI_FOUND)
    INCLUDE(UseJava)
    INCLUDE_DIRECTORIES(${JAVA_INCLUDE_PATH})
    INCLUDE_DIRECTORIES(${JAVA_INCLUDE_PATH2})
    # SET(JDBC_LIBRARY ${JAVA_JVM_LIBRARY}) will be dynamically linked
    SET(CONNECT_SOURCES ${CONNECT_SOURCES}
    javaconn.cpp jdbconn.cpp tabjdbc.cpp
    jmgfam.cpp jmgoconn.cpp mongo.cpp tabjmg.cpp
    jdbccat.h javaconn.h jdbconn.h tabjdbc.h
    jmgfam.h jmgoconn.h mongo.h tabjmg.h
    JdbcInterface.java ApacheInterface.java MariadbInterface.java
    MysqlInterface.java OracleInterface.java PostgresqlInterface.java
    Mongo2Interface.java Mongo3Interface.java
    mysql-test/connect/std_data/JavaWrappers.jar)
    add_definitions(-DJAVA_SUPPORT)
    ADD_FEATURE_INFO(CONNECT_JDBC "ON" "Support for JDBC in the CONNECT storage engine")
    IF(CONNECT_WITH_MONGO)
      SET(CONNECT_SOURCES ${CONNECT_SOURCES}
        mysql-test/connect/std_data/Mongo2.jar
        mysql-test/connect/std_data/Mongo3.jar)
      add_definitions(-DMONGO_SUPPORT)
    ENDIF()
  ELSE()
    SET(JDBC_LIBRARY "")
    ADD_FEATURE_INFO(CONNECT_JDBC "OFF" "Support for JDBC in the CONNECT storage engine")
  ENDIF()
ELSE(CONNECT_WITH_JDBC)
  ADD_FEATURE_INFO(CONNECT_JDBC "OFF" "Support for JDBC in the CONNECT storage engine")
ENDIF(CONNECT_WITH_JDBC)

#
# ZIP
#

OPTION(CONNECT_WITH_ZIP "Compile CONNECT storage engine with ZIP support" ON)

IF(CONNECT_WITH_ZIP)
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} filamzip.cpp tabzip.cpp unzip.c ioapi.c zip.c
    filamzip.h tabzip.h ioapi.h unzip.h zip.h)
  add_definitions(-DZIP_SUPPORT -DNOCRYPT)
ENDIF(CONNECT_WITH_ZIP)
ADD_FEATURE_INFO(CONNECT_ZIP CONNECT_WITH_ZIP "Support for ZIP in the CONNECT storage engine")

#
# MONGO C Driver
#

IF(CONNECT_WITH_MONGO)
  IF(WIN32)
    # Adding some typical places to search in
    SET(PC_MONGO_INCLUDE_DIRS
        C:/mongo-c-driver/include
        D:/mongo-c-driver/include)
    SET(PC_MONGO_LIBRARY_DIRS
        C:/mongo-c-driver/lib
        D:/mongo-c-driver/lib)
  ENDIF(WIN32)
  FIND_PACKAGE(libmongoc-1.0 1.7 QUIET)
  SET_PACKAGE_PROPERTIES(libmongoc PROPERTIES TYPE OPTIONAL)
  IF (libmongoc-1.0_FOUND)
    INCLUDE_DIRECTORIES(${MONGOC_INCLUDE_DIRS})
    SET(MONGOC_LIBRARY  ${MONGOC_LIBRARIES})
    SET(CONNECT_SOURCES ${CONNECT_SOURCES}
    cmgoconn.cpp cmgfam.cpp tabcmg.cpp
    cmgoconn.h cmgfam.h tabcmg.h)
    add_definitions(-DCMGO_SUPPORT)
    ADD_FEATURE_INFO(CONNECT_MONGODB "ON" "Support for MongoDB in the CONNECT storage engine")
    IF (NOT JAVA_FOUND AND JNI_FOUND)
      SET(CONNECT_SOURCES ${CONNECT_SOURCES} mongo.cpp mongo.h)
      add_definitions(-DMONGO_SUPPORT)
    ENDIF (NOT JAVA_FOUND AND JNI_FOUND)
  ELSE(libmongoc-1.0_FOUND)
    ADD_FEATURE_INFO(CONNECT_MONGODB "OFF" "Support for MongoDB in the CONNECT storage engine")
  ENDIF(libmongoc-1.0_FOUND)
ELSE(CONNECT_WITH_MONGO)
  ADD_FEATURE_INFO(CONNECT_MONGODB "OFF" "Support for MongoDB in the CONNECT storage engine")
ENDIF(CONNECT_WITH_MONGO)

#
# REST
#

OPTION(CONNECT_WITH_REST "Compile CONNECT storage engine with REST support" ON)

IF(CONNECT_WITH_REST)
# MESSAGE(STATUS "=====> REST support is ON")
  SET(CONNECT_SOURCES ${CONNECT_SOURCES} tabrest.cpp tabrest.h)
  add_definitions(-DREST_SUPPORT)
# FIND_PACKAGE(cpprestsdk QUIET)
# IF (cpprestsdk_FOUND)
#	IF(UNIX)
##    INCLUDE_DIRECTORIES(${CPPRESTSDK_INCLUDE_DIR})
##    If needed edit next line to set the path to libcpprest.so
#     SET(REST_LIBRARY -lcpprest)
#     MESSAGE (STATUS ${REST_LIBRARY})
#	ELSE(NOT UNIX)
##    Next line sets debug compile mode matching cpprest_2_10d.dll
##    when it was binary installed (can be change later in Visual Studio)
##    Comment it out if not needed depending on your cpprestsdk installation.
#	  SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MDd")
#	ENDIF(UNIX)
##  IF(REST_LIBRARY)     why this? how about Windows
#   SET(CONNECT_SOURCES ${CONNECT_SOURCES} restget.cpp)
#   add_definitions(-DREST_SOURCE)
##  ENDIF()
##ELSE(NOT cpprestsdk_FOUND)
##  MESSAGE(STATUS "=====> cpprestsdk package not found")
# ENDIF (cpprestsdk_FOUND)
ENDIF(CONNECT_WITH_REST)
ADD_FEATURE_INFO(CONNECT_REST CONNECT_WITH_REST "Support for REST API in the CONNECT storage engine")

#
# XMAP
#

OPTION(CONNECT_WITH_XMAP "Compile CONNECT storage engine with index file mapping support" ON)

IF(CONNECT_WITH_XMAP)
  add_definitions(-DXMAP)
ENDIF(CONNECT_WITH_XMAP)
ADD_FEATURE_INFO(CONNECT_XMAP CONNECT_WITH_XMAP "Support for index file mapping in the CONNECT storage engine")

#
# Plugin definition
#

MYSQL_ADD_PLUGIN(connect ${CONNECT_SOURCES}
  STORAGE_ENGINE
  COMPONENT connect-engine
  RECOMPILE_FOR_EMBEDDED
  LINK_LIBRARIES ${ZLIB_LIBRARY} ${XML_LIBRARY} ${ICONV_LIBRARY}
    ${ODBC_LIBRARY} ${JDBC_LIBRARY} ${MONGOC_LIBRARY} ${IPHLPAPI_LIBRARY} ${REST_LIBRARY})

IF(NOT TARGET connect)
  RETURN()
ENDIF()

IF(WIN32)
  IF (libmongoc-1.0_FOUND)
    SET_TARGET_PROPERTIES(connect PROPERTIES LINK_FLAGS
      "/DELAYLOAD:libbson-1.0.dll /DELAYLOAD:libmongoc-1.0.dll")
  ENDIF(libmongoc-1.0_FOUND)
ENDIF(WIN32)

# Install some extra files that belong to connect engine
IF(WIN32)
  # install ha_connect.lib
  GET_TARGET_PROPERTY(CONNECT_LOCATION connect LOCATION)
  STRING(REPLACE "dll" "lib" CONNECT_LIB ${CONNECT_LOCATION})
  IF(CMAKE_CONFIGURATION_TYPES)
    STRING(REPLACE "${CMAKE_CFG_INTDIR}" "\${CMAKE_INSTALL_CONFIG_NAME}"
      CONNECT_LIB ${CONNECT_LIB})
  ENDIF()
  INSTALL(FILES ${CONNECT_LIB}
    DESTINATION ${INSTALL_PLUGINDIR} COMPONENT connect-engine)
ENDIF(WIN32)

IF(CONNECT_WITH_JDBC AND JAVA_FOUND AND JNI_FOUND)
  # TODO: Find how to compile and install the java wrapper classes
  # Find required libraries and include directories
  SET (JAVA_SOURCES JdbcInterface.java)
  add_jar(JdbcInterface ${JAVA_SOURCES})
  INSTALL(FILES
    ${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/connect/std_data/JavaWrappers.jar
    ${CMAKE_CURRENT_BINARY_DIR}/JdbcInterface.jar
    DESTINATION ${INSTALL_MYSQLSHAREDIR} COMPONENT connect-engine)
  IF(CONNECT_WITH_MONGO)
    INSTALL(FILES
      ${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/connect/std_data/Mongo2.jar
      ${CMAKE_CURRENT_SOURCE_DIR}/mysql-test/connect/std_data/Mongo3.jar
      DESTINATION ${INSTALL_MYSQLSHAREDIR} COMPONENT connect-engine)
  ENDIF()
ENDIF()
