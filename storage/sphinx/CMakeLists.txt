INCLUDE(CheckCCompilerFlag)

ADD_DEFINITIONS(-DMYSQL_SERVER)

MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-write-strings")

IF(MSVC)
  # Temporarily disable "conversion from size_t .." warnings
  IF(CMAKE_SIZEOF_VOID_P EQUAL 8)
    SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4267")
  ENDIF()
  # Disable warning about deprecated functions, inet_aton
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /wd4996")
  STRING(REPLACE "/permissive-" "" CMAKE_CXX_FLAGS  "${CMAKE_CXX_FLAGS}" )
ENDIF()

IF(MSVC)
  LINK_LIBRARIES(ws2_32)
ENDIF(MSVC)

SET(SPHINX_SOURCES ha_sphinx.cc snippets_udf.cc)
MYSQL_ADD_PLUGIN(sphinx ${SPHINX_SOURCES} STORAGE_ENGINE)
