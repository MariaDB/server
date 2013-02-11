# use the first path that has Thrift.h included, if found

FIND_PATH(Thrift_INCLUDE_DIRS Thrift.h PATHS
$ENV{THRIFT_INCLUDE}      # environment variable to be used optionally
${Thrift_INCLUDE_DIR}     # this may be set
/usr/local/include/thrift # list of additional directories to look from
/opt/local/include/thrift
/usr/include/thrift
/opt/include/thrift
)

# Verify that thrift linking library is found
FIND_LIBRARY(Thrift_LIBS NAMES thrift PATHS ${Thrift_LIB_PATHS} ${Thrift_LIB})
IF(EXISTS ${Thrift_LIBS})
  GET_FILENAME_COMPONENT(LINK_DIR ${Thrift_LIBS} PATH ABSOLUTE)
ELSE()
  RETURN()
ENDIF()

INCLUDE_DIRECTORIES(AFTER ${Thrift_INCLUDE_DIRS}) 
SET(CMAKE_REQUIRED_INCLUDES ${Thrift_INCLUDE_DIRS})

STRING(REPLACE "-fno-exceptions" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
STRING(REPLACE "-fno-implicit-templates" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})

CHECK_CXX_SOURCE_COMPILES(
"
#include <Thrift.h>
#include <boost/shared_ptr.hpp>
int main() {
  boost::shared_ptr<char> p(new char(10));
  return 0;
}
" CASSANDRASE_OK)

IF(CASSANDRASE_OK)
  SET(cassandra_sources 
      ha_cassandra.cc 
      ha_cassandra.h 
      cassandra_se.h
      cassandra_se.cc
      gen-cpp/Cassandra.cpp
      gen-cpp/cassandra_types.h 
      gen-cpp/cassandra_types.cpp
      gen-cpp/cassandra_constants.h 
      gen-cpp/cassandra_constants.cpp
      gen-cpp/Cassandra.h)

  LINK_DIRECTORIES(${LINK_DIR})

  MYSQL_ADD_PLUGIN(cassandra ${cassandra_sources} STORAGE_ENGINE MODULE_ONLY LINK_LIBRARIES thrift COMPONENT CassandraSE)
  IF (INSTALL_SYSCONFDIR)
    INSTALL(FILES cassandra.cnf DESTINATION ${INSTALL_SYSCONFDIR}/my.cnf.d
            COMPONENT CassandraSE)
  ENDIF(INSTALL_SYSCONFDIR)

  IF(RPM)
    SET(CPACK_COMPONENT_CASSANDRASELIBRARIES_GROUP "CassandraSE" PARENT_SCOPE)
    SET(CPACK_COMPONENTS_ALL ${CPACK_COMPONENTS_ALL} CassandraSE PARENT_SCOPE)
    SET(CPACK_RPM_CassandraSE_PACKAGE_REQUIRES "MariaDB-server" PARENT_SCOPE)

    # workarounds for cmake issues #13248 and #12864:
    SET(CPACK_RPM_CassandraSE_USER_FILELIST ${ignored} "%config(noreplace) /etc/my.cnf.d/*" PARENT_SCOPE)
    SET(CPACK_RPM_CassandraSE_PACKAGE_PROVIDES "cmake_bug_13248" PARENT_SCOPE)
    SET(CPACK_RPM_CassandraSE_PACKAGE_OBSOLETES "cmake_bug_13248" PARENT_SCOPE)
  ENDIF(RPM)

ENDIF(CASSANDRASE_OK)
