INCLUDE (CheckCSourceRuns)

SET(WITH_PCRE "auto" CACHE STRING
   "Which pcre to use (possible values are 'bundled', 'system', or 'auto')")

MACRO (CHECK_PCRE)
  IF(WITH_PCRE STREQUAL "system" OR WITH_PCRE STREQUAL "auto")
    CHECK_LIBRARY_EXISTS(pcre2-8 pcre2_match_8 "" HAVE_PCRE2)
  ENDIF()
  IF(NOT HAVE_PCRE2 OR WITH_PCRE STREQUAL "bundled")
    IF (WITH_PCRE STREQUAL "system")
      MESSAGE(FATAL_ERROR "system pcre2-8 library is not found or unusable")
    ENDIF()
    SET(PCRE_INCLUDES ${CMAKE_BINARY_DIR}/pcre2 ${CMAKE_SOURCE_DIR}/pcre2
                     ${CMAKE_BINARY_DIR}/pcre2/src ${CMAKE_SOURCE_DIR}/pcre2/src)
    SET(PCRE2_BUILD_TESTS OFF CACHE BOOL "Disable tests.")
    SET(PCRE2_BUILD_PCRE2GREP OFF CACHE BOOL "Disable pcre2grep")
    ADD_SUBDIRECTORY(pcre2)
  ENDIF()
ENDMACRO()

