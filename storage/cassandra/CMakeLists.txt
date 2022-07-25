
SET(CPACK_RPM_cassandra-engine_PACKAGE_SUMMARY "Cassandra storage engine for MariaDB" PARENT_SCOPE)
SET(CPACK_RPM_cassandra-engine_PACKAGE_DESCRIPTION "The Cassandra Storage Engine allows access to data in a Cassandra cluster from
MariaDB, combining the best of SQL and no-SQL worlds. Cassandra SE (storage
engine) makes Cassandra's column family appear as a table in MariaDB that you
can insert to, update, and select from. You can write joins against this table,
it is possible to join data that's stored in MariaDB with data that's stored in
Cassandra." PARENT_SCOPE)

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
SET(Thrift_LIB_PATHS ${Thrift_LIB_PATHS} /usr/local/lib /opt/local/lib /opt/lib)
FIND_LIBRARY(Thrift_LIBS NAMES thrift PATHS ${Thrift_LIB_PATHS} ${Thrift_LIB})

MARK_AS_ADVANCED(Thrift_LIBS Thrift_INCLUDE_DIRS)

IF(EXISTS ${Thrift_LIBS})
  GET_FILENAME_COMPONENT(LINK_DIR ${Thrift_LIBS} PATH ABSOLUTE)
  INCLUDE_DIRECTORIES(AFTER ${Thrift_INCLUDE_DIRS}/..)
  SET(CMAKE_REQUIRED_INCLUDES ${Thrift_INCLUDE_DIRS})

  STRING(REPLACE "-fno-exceptions" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  STRING(REPLACE "-fno-implicit-templates" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")

  SET(CMAKE_REQUIRED_INCLUDES "${Thrift_INCLUDE_DIRS}/..")
  CHECK_CXX_SOURCE_COMPILES(
"
#include <thrift/Thrift.h>
#include <boost/shared_ptr.hpp>
int main() {
  boost::shared_ptr<char> p(new char(10));
  return 0;
}
" CASSANDRASE_OK)
ENDIF()

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

IF(CASSANDRASE_OK)
  MYSQL_ADD_PLUGIN(cassandra ${cassandra_sources} STORAGE_ENGINE
    MODULE_ONLY LINK_LIBRARIES thrift COMPONENT cassandra-engine)
ENDIF(CASSANDRASE_OK)
