INCLUDE (CheckCXXSourceCompiles)
INCLUDE (ExternalProject)

SET(WITH_LIBFMT "auto" CACHE STRING
   "Which libfmt to use (possible values are 'bundled', 'system', or 'auto')")

MACRO(BUNDLE_LIBFMT)
  SET(dir "${CMAKE_BINARY_DIR}/extra/libfmt")
  SET(LIBFMT_INCLUDE_DIR "${dir}/src/libfmt/include")
  ADD_LIBRARY(fmt STATIC IMPORTED GLOBAL)
  SET(file ${dir}/src/libfmt-build/${CMAKE_CFG_INTDIR}/${CMAKE_STATIC_LIBRARY_PREFIX}fmt${CMAKE_STATIC_LIBRARY_SUFFIX})
  SET_TARGET_PROPERTIES(fmt PROPERTIES IMPORTED_LOCATION ${file})

  ExternalProject_Add(
    libfmt
    PREFIX   "${dir}"
    URL      "https://github.com/fmtlib/fmt/archive/refs/tags/8.0.1.zip"
    URL_MD5  e77873199e897ca9f780479ad68e25b1
    INSTALL_COMMAND ""
    CMAKE_ARGS
      "-DCMAKE_POSITION_INDEPENDENT_CODE=ON"
      "-DBUILD_SHARED_LIBS=OFF"
      "-DFMT_DEBUG_POSTFIX="
      "-DFMT_DOC=OFF"
      "-DFMT_TEST=OFF"
      "-DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
      "-DCMAKE_CXX_FLAGS=${CMAKE_CXX_FLAGS} ${PIC_FLAG}"
      "-DCMAKE_CXX_FLAGS_DEBUG=${CMAKE_CXX_FLAGS_DEBUG}"
      "-DCMAKE_CXX_FLAGS_RELWITHDEBINFO=${CMAKE_CXX_FLAGS_RELWITHDEBINFO}"
      "-DCMAKE_CXX_FLAGS_RELEASE=${CMAKE_CXX_FLAGS_RELEASE}"
      "-DCMAKE_CXX_FLAGS_MINSIZEREL=${CMAKE_CXX_FLAGS_MINSIZEREL}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
    BUILD_BYPRODUCTS ${file}
  )
  SET_TARGET_PROPERTIES(fmt PROPERTIES EXCLUDE_FROM_ALL TRUE)
ENDMACRO()

MACRO (CHECK_LIBFMT)
  IF(WITH_LIBFMT STREQUAL "system" OR WITH_LIBFMT STREQUAL "auto")
    SET(CMAKE_REQUIRED_LIBRARIES fmt)
    CHECK_CXX_SOURCE_COMPILES(
    "#include <fmt/core.h>
     #include <iostream>
     int main() {
       std::cout << fmt::format(\"The answer is {}.\", 42);
     }" HAVE_SYSTEM_LIBFMT)
    SET(CMAKE_REQUIRED_LIBRARIES)
  ENDIF()
  IF(NOT HAVE_SYSTEM_LIBFMT OR WITH_LIBFMT STREQUAL "bundled")
    IF (WITH_LIBFMT STREQUAL "system")
      MESSAGE(FATAL_ERROR "system libfmt library is not found")
    ENDIF()
    BUNDLE_LIBFMT()
  ELSE()
    FIND_FILE(Libfmt_core_h fmt/core.h) # for build_depends.cmake
  ENDIF()
ENDMACRO()

MARK_AS_ADVANCED(LIBFMT_INCLUDE_DIR)
