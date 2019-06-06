#*****************************************************************************
# Copyright (c) 2019, MariaDB Corporation.
#****************************************************************************/

IF(MSVC)
  # Temporarily disable "conversion from size_t .."
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4267")
  ENDIF()
ENDIF()

SET(CLUSTRIXDB_PLUGIN_STATIC  "clustrixdb")
SET(CLUSTRIXDB_PLUGIN_DYNAMIC "ha_clustrixdb")
SET(CLUSTRIXDB_SOURCES ha_clustrixdb.cc clustrix_connection.cc)
MYSQL_ADD_PLUGIN(clustrixdb ${CLUSTRIXDB_SOURCES} STORAGE_ENGINE)

IF(MSVC)
  IF (CMAKE_BUILD_TYPE STREQUAL "Debug")
    ADD_CUSTOM_COMMAND(TARGET clustrixdb
                       POST_BUILD
                       COMMAND if not exist ..\\..\\sql\\lib mkdir ..\\..\\sql\\lib\\plugin
                       COMMAND copy Debug\\ha_clustrixdb.dll ..\\..\\sql\\lib\\plugin\\ha_clustrixdb.dll)
  ENDIF()
ENDIF()
