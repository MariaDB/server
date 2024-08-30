INCLUDE (CheckCXXSourceRuns)
INCLUDE (ExternalProject)

SET(WITH_LIBFMT "auto" CACHE STRING
   "Which libfmt to use (possible values are 'bundled', 'system', or 'auto')")

MACRO(BUNDLE_LIBFMT)
  SET(dir "${CMAKE_BINARY_DIR}/extra/libfmt")
  SET(LIBFMT_INCLUDE_DIR "${dir}/src/libfmt/include")

  IF(CMAKE_VERSION VERSION_GREATER "3.0")
    SET(fmt_byproducts BUILD_BYPRODUCTS ${LIBFMT_INCLUDE_DIR}/fmt/format-inl.h)
  ENDIF()

  ExternalProject_Add(
    libfmt
    PREFIX   "${dir}"
    URL      "https://github.com/fmtlib/fmt/releases/download/11.0.1/fmt-11.0.1.zip"
    URL_MD5  5f3915e2eff60e7f70c558120592100d
    INSTALL_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    ${fmt_byproducts}
  )
ENDMACRO()

MACRO (CHECK_LIBFMT)
  IF(WITH_LIBFMT STREQUAL "system" OR WITH_LIBFMT STREQUAL "auto")
    SET(CMAKE_REQUIRED_INCLUDES ${LIBFMT_INCLUDE_DIR})
    CHECK_CXX_SOURCE_RUNS(
    "#define FMT_STATIC_THOUSANDS_SEPARATOR ','
     #define FMT_HEADER_ONLY 1
     #include <fmt/format-inl.h>
     int main() {
       int answer= 4321;
       fmt::format_args::format_arg arg=
         fmt::detail::make_arg<fmt::format_context>(answer);
       return fmt::vformat(\"{:L}\", fmt::format_args(&arg, 1)).compare(\"4,321\");
     }" HAVE_SYSTEM_LIBFMT)
    SET(CMAKE_REQUIRED_INCLUDES)
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
