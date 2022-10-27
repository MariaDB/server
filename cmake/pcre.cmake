INCLUDE (CheckCSourceRuns)

SET(WITH_PCRE "auto" CACHE STRING
   "Which pcre to use (possible values are 'bundled', 'system', or 'auto')")

MACRO (CHECK_PCRE)
  IF(WITH_PCRE STREQUAL "system" OR WITH_PCRE STREQUAL "auto")
    CHECK_LIBRARY_EXISTS(pcre pcre_stack_guard "" HAVE_PCRE_STACK_GUARD)
    IF(NOT CMAKE_CROSSCOMPILING)
      SET(CMAKE_REQUIRED_LIBRARIES "pcre")
      CHECK_C_SOURCE_RUNS("
        #include <pcre.h>
        int main() {
        return -pcre_exec(NULL, NULL, NULL, -999, -999, 0, NULL, 0) < 256;
        }"  PCRE_STACK_SIZE_OK)
      SET(CMAKE_REQUIRED_LIBRARIES)
    ENDIF()
  ENDIF()
  IF(NOT HAVE_PCRE_STACK_GUARD OR NOT PCRE_STACK_SIZE_OK OR
     WITH_PCRE STREQUAL "bundled")
    IF (WITH_PCRE STREQUAL "system")
      MESSAGE(FATAL_ERROR "system pcre is not found or unusable")
    ENDIF()
    SET(PCRE_INCLUDES ${CMAKE_BINARY_DIR}/pcre ${CMAKE_SOURCE_DIR}/pcre)
    ADD_SUBDIRECTORY(pcre)
  ENDIF()
ENDMACRO()

