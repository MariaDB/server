
SET(WITH_LIBFMT "auto" CACHE STRING
   "Which libfmt to use (possible values are 'bundled', 'system', or 'auto')")

MACRO(BUNDLE_LIBFMT)
  SET(dir "${CMAKE_BINARY_DIR}/extra/libfmt")
  SET(LIBFMT_INCLUDE_DIR "${dir}/src/libfmt/include")

  IF(CMAKE_VERSION VERSION_GREATER "3.0")
    SET(fmt_byproducts BUILD_BYPRODUCTS ${LIBFMT_INCLUDE_DIR}/fmt/format-inl.h)
  ENDIF()

  INCLUDE (ExternalProject)
  ExternalProject_Add(
    libfmt
    PREFIX   "${dir}"
    URL      "https://github.com/fmtlib/fmt/archive/refs/tags/8.0.1.zip"
    URL_MD5  e77873199e897ca9f780479ad68e25b1
    INSTALL_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    ${fmt_byproducts}
  )
ENDMACRO()

MACRO (CHECK_LIBFMT)
  IF(WITH_LIBFMT STREQUAL "system" OR WITH_LIBFMT STREQUAL "auto")
    FIND_PACKAGE(fmt)
    IF(fmt_FOUND)
      set(HAVE_SYSTEM_LIBFMT ${fmt_FOUND})
    ENDIF()

    FIND_LIBRARY(FMT_LIB_FOUND fmt)
    IF(FMT_LIB_FOUND)
      set(FMT_LIBRARIES fmt CACHE STRING "LibFormat libraries" FORCE)
      ADD_DEFINITIONS(-DLINK_SYSTEM_LIBFMT)
    ELSE(FMT_LIB_FOUND)
      set(FMT_HEADER_ONLY "#define FMT_HEADER_ONLY 1")
      set(FMT_LIBRARIES "" CACHE STRING "LibFormat libraries" FORCE)
    ENDIF(FMT_LIB_FOUND)

    INCLUDE (CheckCXXSourceCompiles)
    CHECK_CXX_SOURCE_COMPILES(
    "#define FMT_STATIC_THOUSANDS_SEPARATOR ','
     ${FMT_HEADER_ONLY}
     #include <fmt/format-inl.h>
     #include <iostream>
     #include <string>
     #if FMT_VERSION < 70000
     using namespace ::fmt::internal;
     #else
     using namespace ::fmt::detail;
     #endif
     int main() {
       fmt::format_args::format_arg arg=
         fmt::detail::make_arg<fmt::format_context>(42);
         std::cout << fmt::vformat(\"The answer is {}.\",
                                   fmt::format_args(&arg, 1));
       return 0;
     }" HAVE_SYSTEM_LIBFMT)
     IF (HAVE_SYSTEM_LIBFMT)
       ADD_DEFINITIONS(-DHAVE_SYSTEM_LIBFMT)
     ENDIF()
  ENDIF()
  IF(NOT HAVE_SYSTEM_LIBFMT OR WITH_LIBFMT STREQUAL "bundled")
    IF (WITH_LIBFMT STREQUAL "system")
      MESSAGE(FATAL_ERROR "system libfmt library is not found or unusable")
    ENDIF()
    BUNDLE_LIBFMT()
  ELSE()
    FIND_FILE(Libfmt_core_h fmt/core.h) # for build_depends.cmake
  ENDIF()
ENDMACRO()

MARK_AS_ADVANCED(LIBFMT_INCLUDE_DIR)
