SET(WITH_PCRE "auto" CACHE STRING
   "Which pcre to use (possible values are 'bundled', 'system', or 'auto')")

MACRO (CHECK_PCRE)
  IF(WITH_PCRE STREQUAL "system" OR WITH_PCRE STREQUAL "auto")
    CHECK_LIBRARY_EXISTS(pcre pcre_stack_guard "" HAVE_PCRE)
  ENDIF()
  IF(NOT HAVE_PCRE OR WITH_PCRE STREQUAL "bundled")
    IF (WITH_PCRE STREQUAL "system")
      MESSAGE(FATAL_ERROR "system pcre is not found or unusable")
    ENDIF()
    SET(PCRE_INCLUDES ${CMAKE_BINARY_DIR}/pcre ${CMAKE_SOURCE_DIR}/pcre)
    ADD_SUBDIRECTORY(pcre)
  ENDIF()
ENDMACRO()

