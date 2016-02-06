IF (WIN32)
 SET(USE_SSPI 1)
ENDIF()

IF(USE_SSPI)
  SET(GSSAPI_LIBS secur32)
  ADD_DEFINITIONS(-DPLUGIN_SSPI)
  SET(GSSAPI_CLIENT sspi_client.cc)
  SET(GSSAPI_SERVER sspi_server.cc)
  SET(GSSAPI_ERRMSG sspi_errmsg.cc)
ELSE()
 SET(CMAKE_MODULE_PATH ${CMAKE_MODULE_PATH} ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
 FIND_PACKAGE(GSSAPI)
 IF(GSSAPI_FOUND)
   INCLUDE_DIRECTORIES(${GSSAPI_INCS})
   ADD_DEFINITIONS(-DPLUGIN_GSSAPI)
   SET(GSSAPI_CLIENT gssapi_client.cc)
   SET(GSSAPI_SERVER gssapi_server.cc)
   SET(GSSAPI_ERRMSG gssapi_errmsg.cc)
 ELSE()
   # Can't build plugin
   RETURN()
 ENDIF()
ENDIF ()


MYSQL_ADD_PLUGIN(auth_gssapi server_plugin.cc ${GSSAPI_SERVER} ${GSSAPI_ERRMSG}
                 LINK_LIBRARIES ${GSSAPI_LIBS} 
                 COMPONENT gssapi-server
                 MODULE_ONLY)

MYSQL_ADD_PLUGIN(auth_gssapi_client client_plugin.cc ${GSSAPI_CLIENT} ${GSSAPI_ERRMSG}
                 LINK_LIBRARIES ${GSSAPI_LIBS}
                 COMPONENT gssapi-client
                 CLIENT
                 MODULE_ONLY)
