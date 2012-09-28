
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

#INCLUDE_DIRECTORIES(BEFORE ${Boost_INCLUDE_DIRS})
INCLUDE_DIRECTORIES(AFTER /usr/local/include/thrift) 
# 
STRING(REPLACE "-fno-exceptions" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
STRING(REPLACE "-fno-implicit-templates" "" CMAKE_CXX_FLAGS ${CMAKE_CXX_FLAGS})
#LINK_DIRECTORIES(/home/psergey/cassandra/thrift/lib)

MYSQL_ADD_PLUGIN(cassandra ${cassandra_sources} STORAGE_ENGINE LINK_LIBRARIES thrift)
# was: STORAGE_ENGINE MANDATORY 
