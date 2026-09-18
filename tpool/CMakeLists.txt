
SET(COMMON_SOURCES
  tpool_structs.h
  tpool.h
  tpool_generic.cc
  task_group.cc
  task.cc
  wait_notification.cc
)

IF(WITH_EMBEDDED_SERVER)
  LIST(APPEND tpool_list tpool tpool_embedded)
  LIST(APPEND tpool_common_list tpool_common tpool_common_embedded)
ELSE()
  SET(tpool_list tpool)
  SET(tpool_common_list tpool_common)
ENDIF()

IF(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  ADD_LIBRARY(tpool OBJECT ${COMMON_SOURCES} aio_simulated.cc)
  IF(WITH_EMBEDDED_SERVER)
    ADD_CONVENIENCE_LIBRARY(tpool_embedded ${COMMON_SOURCES} aio_simulated.cc)
  ENDIF()
  FOREACH(t ${tpool_list})
    IF(WIN32)
      TARGET_SOURCES(${t} PRIVATE tpool_win.cc aio_win.cc)
    ENDIF()
    TARGET_INCLUDE_DIRECTORIES(${t} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
  ENDFOREACH()
  ADD_LIBRARY(tpool_min ALIAS tpool)
ELSE() # Linux
  ADD_LIBRARY(tpool STATIC aio_linux.cc aio_simulated.cc)
  ADD_LIBRARY(tpool_common OBJECT ${COMMON_SOURCES})
  IF(WITH_EMBEDDED_SERVER)
    ADD_CONVENIENCE_LIBRARY(tpool_embedded aio_linux.cc aio_simulated.cc)
    ADD_CONVENIENCE_LIBRARY(tpool_common_embedded OBJECT ${COMMON_SOURCES})
  ENDIF()
  FOREACH(t ${tpool_common_list})
    # Ensure that aiocb used as opaque in common code, or ABI will be broken
    TARGET_COMPILE_DEFINITIONS(${t} PRIVATE -DTPOOL_OPAQUE_AIOCB)
    TARGET_INCLUDE_DIRECTORIES(${t} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${PROJECT_SOURCE_DIR}/include)
  ENDFOREACH()

  TARGET_LINK_LIBRARIES(tpool PUBLIC tpool_common)
  IF(WITH_EMBEDDED_SERVER)
    TARGET_LINK_LIBRARIES(tpool_embedded PUBLIC tpool_common_embedded)
  ENDIF()

  OPTION(WITH_URING "Require that io_uring be used" OFF)
  OPTION(WITH_LIBAIO "Require that libaio is used" OFF)
  IF(WITH_URING)
    SET(URING_REQUIRED REQUIRED)
  ENDIF()
  IF(WITH_LIBAIO)
    SET(LIBAIO_REQUIRED REQUIRED)
  ENDIF()
  FIND_PACKAGE(URING QUIET ${URING_REQUIRED})
  IF(URING_FOUND)
    SET(URING_FOUND ${URING_FOUND} PARENT_SCOPE)
    FOREACH(t ${tpool_list})
      TARGET_COMPILE_DEFINITIONS(${t} PUBLIC "-DHAVE_URING")
      TARGET_LINK_LIBRARIES(${t} PRIVATE ${URING_LIBRARIES})
      TARGET_INCLUDE_DIRECTORIES(${t} PUBLIC ${URING_INCLUDE_DIRS})
      TARGET_SOURCES(${t} PRIVATE aio_liburing.cc)
      ADD_DEPENDENCIES(${t} GenError)
    ENDFOREACH()
    SET(CMAKE_REQUIRED_INCLUDES_SAVE ${CMAKE_REQUIRED_INCLUDES})
    SET(CMAKE_REQUIRED_LIBRARIES_SAVE ${CMAKE_REQUIRED_LIBRARIES})
    SET(CMAKE_REQUIRED_INCLUDES ${URING_INCLUDE_DIRS})
    SET(CMAKE_REQUIRED_LIBRARIES ${URING_LIBRARIES})
    CHECK_SYMBOL_EXISTS(io_uring_mlock_size "liburing.h" HAVE_IO_URING_MLOCK_SIZE)
    SET(CMAKE_REQUIRED_INCLUDES ${CMAKE_REQUIRED_INCLUDES_SAVE})
    SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES_SAVE})
    IF(HAVE_IO_URING_MLOCK_SIZE)
      SET_SOURCE_FILES_PROPERTIES(aio_liburing.cc PROPERTIES COMPILE_FLAGS "-DHAVE_IO_URING_MLOCK_SIZE")
    ENDIF()
  ENDIF()

  FIND_PACKAGE(LIBAIO QUIET ${LIBAIO_REQUIRED})
  IF(LIBAIO_FOUND)
    FOREACH(t ${tpool_list})
      TARGET_COMPILE_DEFINITIONS(${t} PUBLIC  "-DHAVE_LIBAIO")
      TARGET_INCLUDE_DIRECTORIES(${t} PUBLIC ${LIBAIO_INCLUDE_DIRS})
      TARGET_LINK_LIBRARIES(${t} PRIVATE ${LIBAIO_LIBRARIES})
      TARGET_SOURCES(${t} PRIVATE aio_libaio.cc)
    ENDFOREACH()
  ENDIF()

  IF(LIBAIO_FOUND OR URING_FOUND)
    # Create a "light" version of the library that does not link
    # to additional io libs, and is not compiled with
    # -DHAVE_LIBAIO or -DHAVE_URING flags.
    ADD_LIBRARY(tpool_min STATIC aio_linux.cc)
    TARGET_LINK_LIBRARIES(tpool_min PUBLIC tpool_common)
  ELSE()
    ADD_LIBRARY(tpool_min ALIAS tpool)
  ENDIF()
ENDIF()

