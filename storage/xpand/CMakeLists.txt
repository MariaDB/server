#*****************************************************************************
# Copyright (c) 2019, MariaDB Corporation.
#****************************************************************************/

IF(MSVC)
  # Temporarily disable "conversion from size_t .."
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4267")
  ENDIF()
ENDIF()

SET(XPAND_PLUGIN_STATIC  "xpand")
SET(XPAND_PLUGIN_DYNAMIC "ha_xpand")
SET(XPAND_SOURCES ha_xpand.cc xpand_connection.cc ha_xpand_pushdown.cc)
MYSQL_ADD_PLUGIN(xpand ${XPAND_SOURCES} STORAGE_ENGINE)

IF(MSVC)
  IF (CMAKE_BUILD_TYPE STREQUAL "Debug")
    ADD_CUSTOM_COMMAND(TARGET xpand
                       POST_BUILD
                       COMMAND if not exist ..\\..\\sql\\lib mkdir ..\\..\\sql\\lib\\plugin
                       COMMAND copy Debug\\ha_xpand.dll ..\\..\\sql\\lib\\plugin\\ha_xpand.dll)
  ENDIF()
ENDIF()
