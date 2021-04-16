INCLUDE (CheckCSourceRuns)
INCLUDE (ExternalProject)

SET(WITH_PCRE "auto" CACHE STRING
   "Which pcre to use (possible values are 'bundled', 'system', or 'auto')")

MACRO(BUNDLE_PCRE2)
  SET(dir "${CMAKE_BINARY_DIR}/extra/pcre2")
  SET(PCRE_INCLUDES ${dir}/src/pcre2-build ${dir}/src/pcre2/src)
  SET(byproducts)
  FOREACH(lib pcre2-posix pcre2-8)
    ADD_LIBRARY(${lib} STATIC IMPORTED GLOBAL)
    ADD_DEPENDENCIES(${lib} pcre2)
    IF(WIN32)
      # Debug libary name.
      # Same condition as in pcre2 CMakeLists.txt that adds "d"
      GET_PROPERTY(MULTICONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
      IF(MULTICONFIG)
        SET(intdir "${CMAKE_CFG_INTDIR}/")
      ELSE()
        SET(intdir)
      ENDIF()

      SET(file ${dir}/src/pcre2-build/${intdir}${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${CMAKE_STATIC_LIBRARY_SUFFIX})
      SET(file_d ${dir}/src/pcre2-build/${intdir}${CMAKE_STATIC_LIBRARY_PREFIX}${lib}d${CMAKE_STATIC_LIBRARY_SUFFIX})
      SET_TARGET_PROPERTIES(${lib} PROPERTIES IMPORTED_LOCATION_DEBUG ${file_d})
    ELSE()
      SET(file ${dir}/src/pcre2-build/${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${CMAKE_STATIC_LIBRARY_SUFFIX})
      SET(file_d)
    ENDIF()
    SET(byproducts ${byproducts} BUILD_BYPRODUCTS ${file} ${file_d})
    SET_TARGET_PROPERTIES(${lib} PROPERTIES IMPORTED_LOCATION ${file})
  ENDFOREACH()
  FOREACH(v "" "_DEBUG" "_RELWITHDEBINFO" "_RELEASE" "_MINSIZEREL")
    STRING(REPLACE "/WX" "" pcre2_flags${v} "${CMAKE_C_FLAGS${v}}")
    IF(MSVC)
      # Suppress a warning
      STRING(APPEND pcre2_flags${v} " /wd4244 " )
      # Disable asan support
      STRING(REPLACE "-fsanitize=address" "" pcre2_flags${v} "${CMAKE_C_FLAGS${v}}")
    ENDIF()
  ENDFOREACH()
  ExternalProject_Add(
    pcre2
    PREFIX   "${dir}"
    URL      "http://ftp.pcre.org/pub/pcre/pcre2-10.36.zip"
    URL_MD5  ba9e743af42aac5642f7504b12af4116
    INSTALL_COMMAND ""
    CMAKE_ARGS
      "-DPCRE2_BUILD_TESTS=OFF"
      "-DPCRE2_BUILD_PCRE2GREP=OFF"
      "-DBUILD_SHARED_LIBS=OFF"
      "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
      "-DCMAKE_C_FLAGS=${pcre2_flags} ${PIC_FLAG}"
      "-DCMAKE_C_FLAGS_DEBUG=${pcre2_flags_DEBUG}"
      "-DCMAKE_C_FLAGS_RELWITHDEBINFO=${pcre2_flags_RELWITHDEBINFO}"
      "-DCMAKE_C_FLAGS_RELEASE=${pcre2_flags_RELEASE}"
      "-DCMAKE_C_FLAGS_MINSIZEREL=${pcre2_flags_MINSIZEREL}"
      "-DCMAKE_C_COMPILER=${CMAKE_C_COMPILER}"
      ${stdlibs}
      ${byproducts}
  )
SET_TARGET_PROPERTIES(pcre2 PROPERTIES EXCLUDE_FROM_ALL TRUE)
ENDMACRO()

MACRO (CHECK_PCRE)
  IF(WITH_PCRE STREQUAL "system" OR WITH_PCRE STREQUAL "auto")
    CHECK_LIBRARY_EXISTS(pcre2-8 pcre2_match_8 "" HAVE_PCRE2)
  ENDIF()
  IF(NOT HAVE_PCRE2 OR WITH_PCRE STREQUAL "bundled")
    IF (WITH_PCRE STREQUAL "system")
      MESSAGE(FATAL_ERROR "system pcre2-8 library is not found or unusable")
    ENDIF()
    BUNDLE_PCRE2()
  ELSE()
    CHECK_LIBRARY_EXISTS(pcre2-posix PCRE2regcomp "" NEEDS_PCRE2_DEBIAN_HACK)
    IF(NEEDS_PCRE2_DEBIAN_HACK)
      SET(PCRE2_DEBIAN_HACK "-Dregcomp=PCRE2regcomp -Dregexec=PCRE2regexec -Dregerror=PCRE2regerror -Dregfree=PCRE2regfree")
    ENDIF()
  ENDIF()
ENDMACRO()

