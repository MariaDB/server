INCLUDE_DIRECTORIES(${CMAKE_CURRENT_SOURCE_DIR})
IF(WIN32)
  SET(EXTRA_SOURCES tpool_win.cc aio_win.cc)
ELSE()
  SET(EXTRA_SOURCES aio_linux.cc)
ENDIF()

IF(CMAKE_SYSTEM_NAME STREQUAL "Linux")
 CHECK_INCLUDE_FILES (libaio.h HAVE_LIBAIO_H)
 CHECK_LIBRARY_EXISTS(aio io_queue_init "" HAVE_LIBAIO)
 IF(HAVE_LIBAIO_H AND HAVE_LIBAIO)
    ADD_DEFINITIONS(-DLINUX_NATIVE_AIO=1)
    LINK_LIBRARIES(aio)
 ENDIF()
ENDIF()

ADD_LIBRARY(tpool STATIC
  aio_simulated.cc
  tpool_structs.h
  CMakeLists.txt
  tpool.h
  tpool_generic.cc
  task_group.cc
  task.cc
  wait_notification.cc
  ${EXTRA_SOURCES}
)

INCLUDE_DIRECTORIES(${PROJECT_SOURCE_DIR}/include)