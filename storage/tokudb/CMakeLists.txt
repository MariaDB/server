SET(CPACK_RPM_tokudb-engine_PACKAGE_SUMMARY "TokuDB storage engine for MariaDB" PARENT_SCOPE)
SET(CPACK_RPM_tokudb-engine_PACKAGE_DESCRIPTION "The TokuDB storage engine is for use in high-performance and write-intensive
environments, offering increased compression and better performance based
on fractal indexes." PARENT_SCOPE)

SET(TOKUDB_VERSION 5.6.49-89.0)
# PerconaFT only supports x86-64 and cmake-2.8.9+
IF(WIN32)
  # tokudb never worked there
  RETURN()
ELSEIF(CMAKE_VERSION VERSION_LESS "2.8.9")
  MESSAGE(STATUS "CMake 2.8.9 or higher is required by TokuDB")
ELSEIF(NOT HAVE_DLOPEN)
  MESSAGE(STATUS "dlopen is required by TokuDB")
ELSEIF(PLUGIN_PERFSCHEMA MATCHES "^NO$")
  MESSAGE(STATUS "Performance Schema is required by TokuDB")
  RETURN()
ELSEIF(CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64" OR
       CMAKE_SYSTEM_PROCESSOR STREQUAL "amd64")
# tokudb requires F_NOCACHE or O_DIRECT, and designated initializers
CHECK_CXX_SOURCE_COMPILES(
"
#include <fcntl.h>
struct a {int b; int c; };
struct a d = { .b=1, .c=2 };
#if defined(O_DIRECT) || defined(F_NOCACHE)
int main() { return 0; }
#else
#error
#endif
" TOKUDB_OK)
ENDIF()

IF(NOT TOKUDB_OK)
  RETURN()
ENDIF()

SET(TOKUDB_SOURCES
    ha_tokudb.cc
    tokudb_background.cc
    tokudb_information_schema.cc
    tokudb_sysvars.cc
    tokudb_thread.cc
    tokudb_dir_cmd.cc)
MYSQL_ADD_PLUGIN(tokudb ${TOKUDB_SOURCES} STORAGE_ENGINE MODULE_ONLY
                 COMPONENT tokudb-engine CONFIG ${CMAKE_CURRENT_BINARY_DIR}/tokudb.cnf)

IF(NOT TARGET tokudb)
  RETURN()
ENDIF()

INCLUDE(jemalloc)
CHECK_JEMALLOC()

IF(NOT LIBJEMALLOC)
  MESSAGE(WARNING "TokuDB is enabled, but jemalloc is not. This configuration is not supported")
ELSEIF(LIBJEMALLOC STREQUAL jemalloc_pic)
  CHECK_CXX_SOURCE_COMPILES(
"
#include <jemalloc/jemalloc.h>
#if JEMALLOC_VERSION_MAJOR < 5
int main() { return 0; }
#else
#error
#endif
" JEMALLOC_OK)
  IF (NOT JEMALLOC_OK)
    MESSAGE(FATAL_ERROR "static jemalloc_pic.a can only be used up to jemalloc 4")
  ENDIF()
ELSEIF(LIBJEMALLOC STREQUAL jemalloc)
  FIND_LIBRARY(LIBJEMALLOC_SO jemalloc)
  IF(NOT LIBJEMALLOC_SO)
    MESSAGE(FATAL_ERROR "jemalloc is present, but cannot be found?")
  ENDIF()
  GET_FILENAME_COMPONENT(LIBJEMALLOC_PATH ${LIBJEMALLOC_SO} REALPATH CACHE)

  IF(RPM OR DEB)
    UNSET(LIBJEMALLOC)
    GET_DIRECTORY_PROPERTY(V DIRECTORY ${CMAKE_SOURCE_DIR} DEFINITION CPACK_RPM_tokudb-engine_PACKAGE_REQUIRES)
    SET(CPACK_RPM_tokudb-engine_PACKAGE_REQUIRES "${V} jemalloc" PARENT_SCOPE)
  ENDIF()

  IF(INSTALL_SYSCONFDIR)
    SET(systemd_env "Environment=\"LD_PRELOAD=${LIBJEMALLOC_PATH}\"")
    SET(cnf_malloc_lib "malloc-lib=${LIBJEMALLOC_PATH}")
    CONFIGURE_FILE(tokudb.conf.in tokudb.conf @ONLY)
    INSTALL(FILES ${CMAKE_CURRENT_BINARY_DIR}/tokudb.conf
            DESTINATION ${INSTALL_SYSCONFDIR}/systemd/system/mariadb.service.d/
            COMPONENT tokudb-engine)
  ENDIF()
ENDIF()

CONFIGURE_FILE(tokudb.cnf.in tokudb.cnf @ONLY)

MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-shadow")
MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-vla" DEBUG)
MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-implicit-fallthrough")
MY_CHECK_AND_SET_COMPILER_FLAG("-Wno-cpp" DEBUG)

############################################
MARK_AS_ADVANCED(BUILDNAME)
MARK_AS_ADVANCED(BUILD_TESTING)
MARK_AS_ADVANCED(CMAKE_TOKUDB_REVISION)
MARK_AS_ADVANCED(LIBTOKUDB)
MARK_AS_ADVANCED(LIBTOKUPORTABILITY)
MARK_AS_ADVANCED(PROFILING)
MARK_AS_ADVANCED(SNAPPY_SOURCE_DIR)
MARK_AS_ADVANCED(TOKUDB_DATA)
MARK_AS_ADVANCED(TOKU_DEBUG_PARANOID)
MARK_AS_ADVANCED(USE_VALGRIND)
MARK_AS_ADVANCED(XZ_SOURCE_DIR)
MARK_AS_ADVANCED(gcc_ar)
MARK_AS_ADVANCED(gcc_ranlib)
############################################

# pick language dialect
MY_CHECK_AND_SET_COMPILER_FLAG(-std=c++11)

SET(BUILD_TESTING OFF CACHE BOOL "")
SET(USE_VALGRIND OFF CACHE BOOL "")
SET(TOKU_DEBUG_PARANOID OFF CACHE BOOL "")

# Enable TokuDB's TOKUDB_DEBUG in debug builds
SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} -DTOKUDB_DEBUG")

IF(NOT DEFINED TOKUDB_VERSION)
    IF(DEFINED ENV{TOKUDB_VERSION})
        SET(TOKUDB_VERSION $ENV{TOKUDB_VERSION})
    ENDIF()
ENDIF()
IF(DEFINED TOKUDB_VERSION)
    ADD_DEFINITIONS("-DTOKUDB_VERSION=${TOKUDB_VERSION}")
    IF (${TOKUDB_VERSION} MATCHES "^tokudb-([0-9]+)\\.([0-9]+)\\.([0-9]+.*)")
        ADD_DEFINITIONS("-DTOKUDB_VERSION_MAJOR=${CMAKE_MATCH_1}")
        ADD_DEFINITIONS("-DTOKUDB_VERSION_MINOR=${CMAKE_MATCH_2}")
        ADD_DEFINITIONS("-DTOKUDB_VERSION_PATCH=${CMAKE_MATCH_3}")
    ENDIF()
ENDIF()

IF(DEFINED TOKUDB_NOPATCH_CONFIG)
    ADD_DEFINITIONS("-DTOKUDB_NOPATCH_CONFIG=${TOKUDB_NOPATCH_CONFIG}")
ENDIF()

MY_CHECK_AND_SET_COMPILER_FLAG(-Wno-missing-format-attribute)
MY_CHECK_AND_SET_COMPILER_FLAG(-Wno-missing-field-initializers)

IF (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/PerconaFT/")
    IF (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ft-index/")
        MESSAGE(FATAL_ERROR "Found both PerconaFT and ft-index sources.  Don't know which to use.")
    ENDIF ()
    SET(TOKU_FT_DIR_NAME "PerconaFT")
    
ELSEIF (EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ft-index/")
    MESSAGE(WARNING "Found ft-index sources, ft-index is deprecated and replaced with PerconaFT.")
    SET(TOKU_FT_DIR_NAME "ft-index")
ELSE ()
    MESSAGE(FATAL_ERROR "Could not find PerconaFT sources.")
ENDIF ()

IF (WITH_VALGRIND)
    SET(USE_VALGRIND "ON")
ENDIF ()

ADD_SUBDIRECTORY(${TOKU_FT_DIR_NAME})
INCLUDE_DIRECTORIES(${TOKU_FT_DIR_NAME})
INCLUDE_DIRECTORIES(${TOKU_FT_DIR_NAME}/portability)
INCLUDE_DIRECTORIES(${TOKU_FT_DIR_NAME}/util)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR}/${TOKU_FT_DIR_NAME})
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR}/${TOKU_FT_DIR_NAME}/buildheader)
INCLUDE_DIRECTORIES(${CMAKE_CURRENT_BINARY_DIR}/${TOKU_FT_DIR_NAME}/portability)

TARGET_LINK_LIBRARIES(tokudb tokufractaltree_static tokuportability_static
                      ${ZLIB_LIBRARY} ${LIBJEMALLOC} stdc++)

SET(CMAKE_MODULE_LINKER_FLAGS_RELEASE "${CMAKE_MODULE_LINKER_FLAGS_RELEASE} -flto -fuse-linker-plugin")
SET(CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO "${CMAKE_MODULE_LINKER_FLAGS_RELWITHDEBINFO} -flto -fuse-linker-plugin")

ADD_SUBDIRECTORY(man)
