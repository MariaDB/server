INCLUDE (CheckCXXSourceCompiles)
INCLUDE (ExternalProject)

SET(WITH_LIBFMT "auto" CACHE STRING
   "Which libfmt to use (possible values are 'bundled', 'system', or 'auto')")

MACRO(BUNDLE_LIBFMT)
  SET(dir "${CMAKE_BINARY_DIR}/extra/libfmt")
  SET(LIBFMT_INCLUDE_DIR "${dir}/src/libfmt/include")

  ExternalProject_Add(
    libfmt
    PREFIX   "${dir}"
    URL      "https://github.com/fmtlib/fmt/archive/refs/tags/8.0.1.zip"
    URL_MD5  e77873199e897ca9f780479ad68e25b1
    INSTALL_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    BUILD_BYPRODUCTS ${LIBFMT_INCLUDE_DIR}/fmt/format-inl.h
  )
ENDMACRO()

MACRO (CHECK_LIBFMT)
  IF(WITH_LIBFMT STREQUAL "system" OR WITH_LIBFMT STREQUAL "auto")
    CHECK_CXX_SOURCE_COMPILES(
    "#define FMT_STATIC_THOUSANDS_SEPARATOR ','
     #define FMT_HEADER_ONLY 1
     #include <fmt/format-inl.h>
     #include <iostream>
     int main() {
       std::cout << fmt::format(\"The answer is {}.\", 42);
     }" HAVE_SYSTEM_LIBFMT)
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
