include(CheckCSourceCompiles)
include(CheckCXXSourceCompiles)
# We need some extra FAIL_REGEX patterns
# Note that CHECK_C_SOURCE_COMPILES is a misnomer, it will also link.
SET(fail_patterns
    FAIL_REGEX "argument unused during compilation"
    FAIL_REGEX "unsupported .*option"
    FAIL_REGEX "unknown .*option"
    FAIL_REGEX "unrecognized .*option"
    FAIL_REGEX "ignoring unknown option"
    FAIL_REGEX "warning:.*ignored"
    FAIL_REGEX "[Ww]arning: [Oo]ption"
    )

MACRO (MY_CHECK_C_COMPILER_FLAG flag result)
  SET(SAVE_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${flag}")
  CHECK_C_SOURCE_COMPILES("int main(void) { return 0; }" ${result}
    ${fail_patterns})
  SET(CMAKE_REQUIRED_FLAGS "${SAVE_CMAKE_REQUIRED_FLAGS}")
ENDMACRO()

MACRO (MY_CHECK_CXX_COMPILER_FLAG flag result)
  SET(SAVE_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${flag}")
  CHECK_CXX_SOURCE_COMPILES("int main(void) { return 0; }" ${result}
    ${fail_patterns})
  SET(CMAKE_REQUIRED_FLAGS "${SAVE_CMAKE_REQUIRED_FLAGS}")
ENDMACRO()

FUNCTION(MY_CHECK_AND_SET_COMPILER_FLAG flag)
  # At the moment this is gcc-only.
  # Let's avoid expensive compiler tests on Windows:
  IF(WIN32)
    RETURN()
  ENDIF()
  MY_CHECK_C_COMPILER_FLAG(${flag} HAVE_C_${flag})
  MY_CHECK_CXX_COMPILER_FLAG(${flag} HAVE_CXX_${flag})
  IF (HAVE_C_${flag} AND HAVE_CXX_${flag})
    IF(ARGN)
      FOREACH(type ${ARGN})
        SET(CMAKE_C_FLAGS_${type} "${CMAKE_C_FLAGS_${type}} ${flag}" PARENT_SCOPE)
        SET(CMAKE_CXX_FLAGS_${type} "${CMAKE_CXX_FLAGS_${type}} ${flag}" PARENT_SCOPE)
      ENDFOREACH()
    ELSE()
      SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${flag}" PARENT_SCOPE)
      SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${flag}" PARENT_SCOPE)
    ENDIF()
  ENDIF()
ENDFUNCTION()

