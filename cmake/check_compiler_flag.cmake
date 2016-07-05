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
    FAIL_REGEX "warning:.*is valid for.*but not for"
    FAIL_REGEX "warning:.*redefined"
    FAIL_REGEX "[Ww]arning: [Oo]ption"
    )

MACRO (MY_CHECK_C_COMPILER_FLAG flag)
  STRING(REGEX REPLACE "[-,= +]" "_" result "have_C_${flag}")
  SET(SAVE_CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS}")
  SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} ${flag}")
  CHECK_C_SOURCE_COMPILES("int main(void) { return 0; }" ${result}
    ${fail_patterns})
  SET(CMAKE_REQUIRED_FLAGS "${SAVE_CMAKE_REQUIRED_FLAGS}")
ENDMACRO()

MACRO (MY_CHECK_CXX_COMPILER_FLAG flag)
  STRING(REGEX REPLACE "[-,= +]" "_" result "have_CXX_${flag}")
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
  MY_CHECK_C_COMPILER_FLAG(${flag})
  MY_CHECK_CXX_COMPILER_FLAG(${flag})
  STRING(REGEX REPLACE "[-,= +]" "_" result "${flag}")
  FOREACH(lang C CXX)
    IF (have_${lang}_${result})
      IF(ARGN)
        FOREACH(type ${ARGN})
          SET(CMAKE_${lang}_FLAGS_${type} "${CMAKE_${lang}_FLAGS_${type}} ${flag}" PARENT_SCOPE)
        ENDFOREACH()
      ELSE()
        SET(CMAKE_${lang}_FLAGS "${CMAKE_${lang}_FLAGS} ${flag}" PARENT_SCOPE)
      ENDIF()
    ENDIF()
  ENDFOREACH()
ENDFUNCTION()

