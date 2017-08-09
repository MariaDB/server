INCLUDE (CheckLibraryExists)

SET(WITH_JEMALLOC auto CACHE STRING
  "Build with jemalloc. Possible values are 'yes', 'no', 'static', 'auto'")

MACRO (CHECK_JEMALLOC)
  # compatibility with old WITH_JEMALLOC values
  IF(WITH_JEMALLOC STREQUAL "bundled")
    MESSAGE(FATAL_ERROR "MariaDB no longer bundles jemalloc")
  ENDIF()
  IF(WITH_JEMALLOC STREQUAL "system")
    SET(WITH_JEMALLOC "yes")
  ENDIF()

  IF(WITH_JEMALLOC STREQUAL "yes" OR WITH_JEMALLOC STREQUAL "auto" OR
      WITH_JEMALLOC STREQUAL "static")

    IF(WITH_JEMALLOC STREQUAL "static")
      SET(libname jemalloc_pic)
      SET(CMAKE_REQUIRED_LIBRARIES pthread dl m)
      SET(what bundled)
    ELSE()
      SET(libname jemalloc c)
      SET(what system)
    ENDIF()

    FOREACH(lib ${libname})
      CHECK_LIBRARY_EXISTS(${lib} malloc_stats_print "" HAVE_JEMALLOC_IN_${lib})
      IF (HAVE_JEMALLOC_IN_${lib})
        SET(LIBJEMALLOC ${lib})
        SET(MALLOC_LIBRARY "${what} jemalloc")
        BREAK()
      ENDIF()
    ENDFOREACH()
    SET(CMAKE_REQUIRED_LIBRARIES)

    IF (NOT LIBJEMALLOC AND NOT WITH_JEMALLOC STREQUAL "auto")
      MESSAGE(FATAL_ERROR "jemalloc is not found")
    ENDIF()
    ADD_FEATURE_INFO(JEMALLOC LIBJEMALLOC "Use the JeMalloc memory allocator")
  ENDIF()
ENDMACRO()
