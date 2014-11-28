INCLUDE (CheckLibraryExists)

SET(WITH_JEMALLOC auto CACHE STRING
  "Build with jemalloc. Possible values are 'yes', 'no', 'static', 'auto'")

MACRO(JEMALLOC_TRY_STATIC)
  SET(libname jemalloc_pic)
  SET(CMAKE_REQUIRED_LIBRARIES pthread dl m)
  SET(what bundled)
  CHECK_LIBRARY_EXISTS(${libname} malloc_stats_print "" HAVE_STATIC_JEMALLOC)
  SET(CMAKE_REQUIRED_LIBRARIES)
ENDMACRO()

MACRO(JEMALLOC_TRY_DYNAMIC)
  SET(libname jemalloc)
  SET(what system)
  CHECK_LIBRARY_EXISTS(${libname} malloc_stats_print "" HAVE_DYNAMIC_JEMALLOC)
ENDMACRO()

MACRO (CHECK_JEMALLOC)
  # compatibility with old WITH_JEMALLOC values
  IF(WITH_JEMALLOC STREQUAL "bundled")
    MESSAGE(FATAL_ERROR "MariaDB no longer bundles jemalloc")
  ENDIF()
  IF(WITH_JEMALLOC STREQUAL "system")
    SET(WITH_JEMALLOC "yes")
  ENDIF()

  IF (WITH_JEMALLOC STREQUAL "yes" OR WITH_JEMALLOC STREQUAL "auto")
    JEMALLOC_TRY_DYNAMIC()
  ENDIF()

  IF (WITH_JEMALLOC STREQUAL "static" OR WITH_JEMALLOC STREQUAL "auto"
      AND NOT HAVE_DYNAMIC_JEMALLOC)
    JEMALLOC_TRY_STATIC()
  ENDIF()

  IF (libname)
    IF (HAVE_DYNAMIC_JEMALLOC OR HAVE_STATIC_JEMALLOC)
      SET(LIBJEMALLOC ${libname})
      SET(MALLOC_LIBRARY "${what} jemalloc")
    ELSEIF (NOT WITH_JEMALLOC STREQUAL "auto")
      MESSAGE(FATAL_ERROR "${libname} is not found")
    ENDIF()
  ENDIF()
ENDMACRO()
