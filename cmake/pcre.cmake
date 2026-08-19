INCLUDE (ExternalProject)

SET(WITH_PCRE "auto" CACHE STRING
   "Which pcre to use (possible values are 'bundled', 'system', or 'auto')")

MACRO(BUNDLE_PCRE2)
  SET(WITH_PCRE "bundled" CACHE STRING
   "Which pcre to use (possible values are 'bundled', 'system', or 'auto')")

  SET(dir "${CMAKE_BINARY_DIR}/extra/pcre2")
  MESSAGE(STATUS "Will download and bundle pcre2")
  SET(byproducts)
  FOREACH(lib pcre2-posix pcre2-8)
    ADD_LIBRARY(${lib} STATIC IMPORTED GLOBAL)
    ADD_DEPENDENCIES(${lib} pcre2)

    GET_PROPERTY(MULTICONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
    IF(MULTICONFIG)
      SET(intdir "${CMAKE_CFG_INTDIR}/")
    ELSE()
      SET(intdir)
    ENDIF()

    # PCRE names static libraries differently depending on platform.
    # On Windows, but not elsewhere, it adds "-static" to the library name,
    # or "-staticd".
    IF(WIN32)
      SET(PCRE_STATIC "-static")
    ELSE()
      SET(PCRE_STATIC "")
    ENDIF()

    SET(file ${dir}/src/pcre2-build/${intdir}${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${PCRE_STATIC}${CMAKE_STATIC_LIBRARY_SUFFIX})

    IF(WIN32)
      # Debug libary name.
      # Same condition as in pcre2 CMakeLists.txt that adds "d"
      SET(file_d ${dir}/src/pcre2-build/${intdir}${CMAKE_STATIC_LIBRARY_PREFIX}${lib}${PCRE_STATIC}d${CMAKE_STATIC_LIBRARY_SUFFIX})
      SET_TARGET_PROPERTIES(${lib} PROPERTIES IMPORTED_LOCATION_DEBUG ${file_d})
    ELSE()
      SET(file_d)
    ENDIF()
    SET(byproducts ${byproducts} BUILD_BYPRODUCTS ${file} ${file_d})
    SET_TARGET_PROPERTIES(${lib} PROPERTIES IMPORTED_LOCATION ${file})
    MAKE_DIRECTORY(${dir}/src/pcre2-build/interface)
    TARGET_INCLUDE_DIRECTORIES(${lib} INTERFACE ${dir}/src/pcre2-build/interface)
  ENDFOREACH()

  # pcre2-posix calls into pcre2-8; as static libs the linker needs -8 after
  # -posix. Make it a dependency so consumers get the order right.
  TARGET_LINK_LIBRARIES(pcre2-posix INTERFACE pcre2-8)

  FOREACH(v "" "_DEBUG" "_RELWITHDEBINFO" "_RELEASE" "_MINSIZEREL")
    SET(pcre2_flags${v} "${CMAKE_C_FLAGS${v}}")
    IF(MSVC)
      STRING(REPLACE "/WX" "" pcre2_flags${v} "${pcre2_flags${v}}")
      # Suppress a warning
      STRING(APPEND pcre2_flags${v} " /wd4244 /wd4267 " )
    ENDIF()
  ENDFOREACH()

  IF(CMAKE_MSVC_RUNTIME_LIBRARY)
    SET(CMAKE_MSVC_RUNTIME_LIBRARY_ARG
      "-DCMAKE_MSVC_RUNTIME_LIBRARY=${CMAKE_MSVC_RUNTIME_LIBRARY}")
  ELSE()
    SET(CMAKE_MSVC_RUNTIME_LIBRARY_ARG)
  ENDIF()

  ExternalProject_Add(
    pcre2
    PREFIX   "${dir}"
    URL "https://github.com/PCRE2Project/pcre2/releases/download/pcre2-10.47/pcre2-10.47.zip"
    URL_MD5 7906aec38f872b74f1b925122dde9069
    INSTALL_COMMAND ""
    CMAKE_ARGS
      "-DCMAKE_WARN_DEPRECATED=FALSE"
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
      ${CMAKE_MSVC_RUNTIME_LIBRARY_ARG}
      ${stdlibs}
      ${byproducts}
  )
SET_TARGET_PROPERTIES(pcre2 PROPERTIES EXCLUDE_FROM_ALL TRUE)
ENDMACRO()

MACRO (CHECK_PCRE)
  IF (NOT TARGET pcre2 AND NOT PCRE_FOUND)
    IF(WITH_PCRE STREQUAL "system" OR WITH_PCRE STREQUAL "auto")
      FIND_PACKAGE(PkgConfig QUIET)
      PKG_CHECK_MODULES(PCRE libpcre2-8)
      # in case pkg-config or libpcre2-8.pc is not installed:
      CHECK_LIBRARY_EXISTS(pcre2-8 pcre2_match_8 "${PCRE_LIBRARY_DIRS}" HAVE_PCRE2_MATCH_8)
    ENDIF()
    IF(NOT HAVE_PCRE2_MATCH_8 OR WITH_PCRE STREQUAL "bundled")
      IF (WITH_PCRE STREQUAL "system")
        MESSAGE(FATAL_ERROR "system pcre2-8 library is not found or unusable")
      ENDIF()
      BUNDLE_PCRE2()
    ELSE()
      CHECK_LIBRARY_EXISTS(pcre2-posix PCRE2regcomp "${PCRE_LIBRARY_DIRS}" NEEDS_PCRE2_DEBIAN_HACK)
      IF(NEEDS_PCRE2_DEBIAN_HACK)
        SET(PCRE2_DEBIAN_HACK "-Dregcomp=PCRE2regcomp -Dregexec=PCRE2regexec -Dregerror=PCRE2regerror -Dregfree=PCRE2regfree")
      ENDIF()
    ENDIF()
  ENDIF()
  # MariaDB::pcre2-8 and MariaDB::pcre2-posix - the pcre2 libraries plus their
  # headers, pointing at the bundled imported targets or the system libraries.
  # For the bundled case the include dir comes from the pcre2 targets; for
  # system from PCRE_INCLUDE_DIRS.
  IF(NOT TARGET mariadb_pcre2_8)
    ADD_LIBRARY(mariadb_pcre2_8 INTERFACE)
    TARGET_LINK_LIBRARIES(mariadb_pcre2_8 INTERFACE pcre2-8)
    TARGET_INCLUDE_DIRECTORIES(mariadb_pcre2_8 INTERFACE ${PCRE_INCLUDE_DIRS})
    ADD_LIBRARY(MariaDB::pcre2-8 ALIAS mariadb_pcre2_8)

    ADD_LIBRARY(mariadb_pcre2_posix INTERFACE)
    TARGET_LINK_LIBRARIES(mariadb_pcre2_posix INTERFACE pcre2-posix)
    TARGET_INCLUDE_DIRECTORIES(mariadb_pcre2_posix INTERFACE ${PCRE_INCLUDE_DIRS})
    ADD_LIBRARY(MariaDB::pcre2-posix ALIAS mariadb_pcre2_posix)
  ENDIF()
ENDMACRO()

