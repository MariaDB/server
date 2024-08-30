IF(HAVE_WVLA)
  SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -Wno-vla")
  SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} -Wno-vla")
ENDIF()

IF(MSVC)
  # Temporarily disable "conversion from size_t .."
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4267")
  ENDIF()
ENDIF()

SET(SPIDER_SOURCES
  spd_param.cc spd_sys_table.cc spd_trx.cc spd_db_conn.cc spd_conn.cc
  spd_table.cc spd_direct_sql.cc spd_udf.cc spd_ping_table.cc
  spd_copy_tables.cc spd_i_s.cc spd_malloc.cc ha_spider.cc spd_udf.def
  spd_db_mysql.cc spd_group_by_handler.cc spd_db_include.cc
)

IF(PLUGIN_PARTITION MATCHES "^NO$")
  MESSAGE(STATUS "Spider is skipped because partitioning is disabled")
ELSE()
  IF(DEB)
    SET(extra_options COMPONENT spider-engine)
  ELSE()
    SET(extra_options CONFIG spider.cnf)
  ENDIF()

  MYSQL_ADD_PLUGIN(spider ${SPIDER_SOURCES} ${extra_options}
    STORAGE_ENGINE MODULE_ONLY)
  IF(NOT TARGET spider)
    RETURN()
  ENDIF()
ENDIF()

IF(MSVC AND (TARGET spider))
  IF (CMAKE_BUILD_TYPE STREQUAL "Debug")
    ADD_CUSTOM_COMMAND(TARGET spider
                       POST_BUILD
                       COMMAND if not exist ..\\..\\sql\\lib mkdir ..\\..\\sql\\lib\\plugin
                       COMMAND copy Debug\\ha_spider.dll ..\\..\\sql\\lib\\plugin\\ha_spider.dll)
  ENDIF()
ENDIF()
