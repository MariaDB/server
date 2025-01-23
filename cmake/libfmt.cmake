INCLUDE (CheckCXXSourceRuns)
INCLUDE (ExternalProject)
FIND_PACKAGE(fmt QUIET)

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
    URL      "https://github.com/fmtlib/fmt/releases/download/11.0.2/fmt-11.0.2.zip"
    URL_MD5 c622dca45ec3fc95254c48370a9f7a1d
    INSTALL_COMMAND ""
    CONFIGURE_COMMAND ""
    BUILD_COMMAND ""
    ${fmt_byproducts}
  )
ENDMACRO()

MACRO(CHECK_LIBFMT)
  IF (WITH_LIBFMT STREQUAL "system" OR WITH_LIBFMT STREQUAL "auto")
    IF (fmt_FOUND)
      MESSAGE(STATUS "Found system libfmt: ${fmt_VERSION}")
      SET(HAVE_SYSTEM_LIBFMT 1)
    ELSE()
      MESSAGE(STATUS "Could NOT find system libfmt via config; trying compile test.")

      SET(CMAKE_REQUIRED_INCLUDES "${LIBFMT_INCLUDE_DIR}")
      CHECK_CXX_SOURCE_RUNS(
        "#define FMT_STATIC_THOUSANDS_SEPARATOR ','
         #define FMT_HEADER_ONLY 1
         #include <fmt/args.h>
         int main() {
           using ArgStore= fmt::dynamic_format_arg_store<fmt::format_context>;
           ArgStore arg_store;
           int answer= 4321;
           arg_store.push_back(answer);
           return fmt::vformat(\"{:L}\", arg_store).compare(\"4,321\");
         }"
        HAVE_SYSTEM_LIBFMT
      )
      SET(CMAKE_REQUIRED_INCLUDES)
    ENDIF()
  ENDIF()

  IF (NOT HAVE_SYSTEM_LIBFMT OR WITH_LIBFMT STREQUAL "bundled")
    IF (WITH_LIBFMT STREQUAL "system")
      MESSAGE(FATAL_ERROR "system libfmt library is not found or unusable")
    ENDIF()
    BUNDLE_LIBFMT()
  ELSE()
    FIND_FILE(Libfmt_core_h fmt/core.h) # for build_depends.cmake
  ENDIF()
ENDMACRO()

MARK_AS_ADVANCED(LIBFMT_INCLUDE_DIR)
