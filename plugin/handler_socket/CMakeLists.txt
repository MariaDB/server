
IF(WIN32 OR WITHOUT_SERVER)
  # Handlersocket does not compile on Windows, compiles but does 
  # not start on FreeBSD.
  # It is a server plugin and disable it explicitly here.
  RETURN()
ENDIF()

MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-deprecated-declarations")

INCLUDE_DIRECTORIES(libhsclient)

# Handlersocket client library. We do not distribute it, 
# it is just compiled in.
SET(LIBHSCLIENT_SOURCES
  libhsclient/config.cpp 
  libhsclient/escape.cpp 
  libhsclient/fatal.cpp 
  libhsclient/hstcpcli.cpp 
  libhsclient/socket.cpp 
  libhsclient/string_util.cpp
)
ADD_CONVENIENCE_LIBRARY(hsclient  ${LIBHSCLIENT_SOURCES})
# Solaris needs to link some network libraries
TARGET_LINK_LIBRARIES(hsclient ${LIBSOCKET} ${LIBNLS} ${LIBBIND})

# handlersocket daemon plugin itself.
SET(HANDLERSOCKET_SOURCES
  handlersocket/database.cpp
  handlersocket/handlersocket.cpp
  handlersocket/hstcpsvr_worker.cpp 
  handlersocket/hstcpsvr.cpp
)
MYSQL_ADD_PLUGIN(handlersocket 
  ${HANDLERSOCKET_SOURCES} 
  MODULE_ONLY  COMPONENT Server
  LINK_LIBRARIES  hsclient RECOMPILE_FOR_EMBEDDED
)

