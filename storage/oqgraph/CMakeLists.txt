set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

SET(CPACK_RPM_oqgraph-engine_PACKAGE_SUMMARY "OQGraph storage engine for MariaDB" PARENT_SCOPE)
SET(CPACK_RPM_oqgraph-engine_PACKAGE_DESCRIPTION "The Open Query GRAPH computation engine, or OQGRAPH as the engine itself is called,
allows you to handle hierarchies (tree structures) and complex graphs
(nodes having many connections in several directions).
It is intended to be used for retrieving hierarchical information, such as those used for graphs,
routes or social relationships, in plain SQL." PARENT_SCOPE)

MACRO(CHECK_OQGRAPH)
MESSAGE(STATUS "Configuring OQGraph")
FIND_PACKAGE(Boost 1.40.0)
SET_PACKAGE_PROPERTIES(Boost PROPERTIES
  PURPOSE "Required for the OQGraph storage engine"
  TYPE OPTIONAL
)
IF(NOT Boost_FOUND)
  MESSAGE(STATUS "Boost not found. OQGraph will not be compiled")
  SET(OQGRAPH_OK 0 CACHE INTERNAL "")
  RETURN()
ENDIF()
INCLUDE_DIRECTORIES(BEFORE ${Boost_INCLUDE_DIRS})

FIND_PACKAGE(Judy)
SET_PACKAGE_PROPERTIES(Judy PROPERTIES
  PURPOSE "Required for the OQGraph storage engine"
  TYPE OPTIONAL
)
IF(NOT Judy_FOUND)
  MESSAGE(STATUS "Judy not found. OQGraph will not be compiled")
  SET(OQGRAPH_OK 0 CACHE INTERNAL "")
  RETURN()
ENDIF()
INCLUDE_DIRECTORIES(${Judy_INCLUDE_DIR})
SET(OQGRAPH_OK 1)
ENDMACRO()

IF(PLUGIN_OQGRAPH STREQUAL "NO")
  RETURN()
ENDIF()

IF(NOT DEFINED OQGRAPH_OK)
  CHECK_OQGRAPH()
  IF (NOT OQGRAPH_OK)
    MESSAGE(STATUS "Requisites for OQGraph not met. OQGraph will not be compiled")
  ENDIF()
ENDIF()

IF(NOT OQGRAPH_OK)
  RETURN()
ENDIF()

ADD_DEFINITIONS(-DHAVE_OQGRAPH)
IF(MSVC)
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
  # Fix problem with judy not finding inttypes.h on Windows:
  ADD_DEFINITIONS(-DJU_WIN)
ELSE(MSVC)
  # Fix lp bug 1221555 with -fpermissive, so that errors in gcc 4.7 become warnings for the time being
  STRING(REPLACE "-fno-exceptions" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-deprecated -fno-strict-aliasing -fpermissive")
ENDIF(MSVC)

ADD_DEFINITIONS(-DBOOST_NO_RTTI=1 -DBOOST_NO_TYPEID=1 -DBOOST_DISABLE_ASSERTS=1)
MYSQL_ADD_PLUGIN(oqgraph ha_oqgraph.cc graphcore.cc graphcore-graph.cc
                 oqgraph_shim.cc oqgraph_thunk.cc oqgraph_judy.cc
                 STORAGE_ENGINE
                 MODULE_ONLY
                 RECOMPILE_FOR_EMBEDDED
                 COMPONENT oqgraph-engine
                 LINK_LIBRARIES ${Judy_LIBRARIES})
