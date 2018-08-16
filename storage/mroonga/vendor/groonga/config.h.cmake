/* config.h.cmake.  Generated from CMakeLists.txt by cmake.  */

/* general constants */
#define CONFIGURE_OPTIONS    "${CONFIGURE_OPTIONS}"

#define HOST_CPU             "${CMAKE_SYSTEM_PROCESSOR}"
#define HOST_OS              "${CMAKE_SYSTEM_NAME}"

#define VERSION              "${VERSION}"
#define PACKAGE              "${PROJECT_NAME}"
#define PACKAGE_NAME         "${PROJECT_NAME}"
#define PACKAGE_LABEL        "${GRN_PROJECT_LABEL}"
#define PACKAGE_STRING       "${PROJECT_NAME} ${VERSION}"
#define PACKAGE_TARNAME      "${PROJECT_NAME}"
#define PACKAGE_URL          "${PACKAGE_URL}"
#define PACKAGE_VERSION      "${VERSION}"

/* Groonga related constants */
#define GRN_CONFIG_PATH      "${GRN_CONFIG_PATH}"
#define GRN_LOG_PATH         "${GRN_LOG_PATH}"
#define GRN_VERSION          "${GRN_VERSION}"

#define GRN_DEFAULT_DB_KEY   "${GRN_DEFAULT_DB_KEY}"
#define GRN_DEFAULT_ENCODING "${GRN_DEFAULT_ENCODING}"
#define GRN_DEFAULT_MATCH_ESCALATION_THRESHOLD \
  ${GRN_DEFAULT_MATCH_ESCALATION_THRESHOLD}
#define GRN_DEFAULT_RELATIVE_DOCUMENT_ROOT \
  "${GRN_DEFAULT_RELATIVE_DOCUMENT_ROOT}"
#define GRN_DEFAULT_DOCUMENT_ROOT \
  "${GRN_DEFAULT_DOCUMENT_ROOT}"

#define GRN_STACK_SIZE       ${GRN_STACK_SIZE}

#define GRN_LOCK_TIMEOUT     ${GRN_LOCK_TIMEOUT}
#define GRN_LOCK_WAIT_TIME_NANOSECOND \
  ${GRN_LOCK_WAIT_TIME_NANOSECOND}

#define GRN_RELATIVE_PLUGINS_DIR \
  "${GRN_RELATIVE_PLUGINS_DIR}"
#define GRN_PLUGINS_DIR      "${GRN_PLUGINS_DIR}"
#define GRN_PLUGIN_SUFFIX    "${GRN_PLUGIN_SUFFIX}"

#define GRN_QUERY_EXPANDER_TSV_RELATIVE_SYNONYMS_FILE "${GRN_QUERY_EXPANDER_TSV_RELATIVE_SYNONYMS_FILE}"
#define GRN_QUERY_EXPANDER_TSV_SYNONYMS_FILE          "${GRN_QUERY_EXPANDER_TSV_SYNONYMS_FILE}"

#define GRN_RELATIVE_RUBY_SCRIPTS_DIR \
  "${GRN_RELATIVE_RUBY_SCRIPTS_DIR}"
#define GRN_RUBY_SCRIPTS_DIR "${GRN_RUBY_SCRIPTS_DIR}"

#define GRN_DLL_FILENAME     L"${GRN_DLL_FILENAME}"

/* build switches */
#cmakedefine USE_MEMORY_DEBUG
#cmakedefine USE_MAP_HUGETLB
#cmakedefine USE_AIO
#cmakedefine USE_DYNAMIC_MALLOC_CHANGE
#cmakedefine USE_EPOLL
#cmakedefine USE_EXACT_ALLOC_COUNT
#cmakedefine USE_FAIL_MALLOC
#cmakedefine USE_FUTEX
#cmakedefine USE_KQUEUE
#cmakedefine USE_MSG_MORE
#cmakedefine USE_MSG_NOSIGNAL
#cmakedefine USE_POLL
#cmakedefine USE_QUERY_ABORT
#cmakedefine USE_SELECT

/* compiler specific build options */
#cmakedefine _FILE_OFFSET_BITS @_FILE_OFFSET_BITS@
#ifndef _GNU_SOURCE
 #cmakedefine _GNU_SOURCE
#endif
#cmakedefine _ISOC99_SOURCE
#cmakedefine _LARGE_FILES
#cmakedefine _NETBSD_SOURCE
#cmakedefine _XOPEN_SOURCE
#cmakedefine _XPG4_2
#cmakedefine __EXTENSIONS__

/* build environment */
#cmakedefine WORDS_BIGENDIAN

/* packages */
#cmakedefine GRN_WITH_BENCHMARK
#cmakedefine GRN_WITH_CUTTER
#cmakedefine GRN_WITH_KYTEA
#cmakedefine GRN_WITH_LZ4
#cmakedefine GRN_WITH_ZSTD
#cmakedefine GRN_WITH_MECAB
#cmakedefine GRN_WITH_MESSAGE_PACK
#cmakedefine GRN_WITH_MRUBY
#cmakedefine GRN_WITH_NFKC
#cmakedefine GRN_WITH_ONIGMO
#cmakedefine GRN_WITH_ZEROMQ
#cmakedefine GRN_WITH_ZLIB

/* headers */
#cmakedefine HAVE_DIRENT_H
#cmakedefine HAVE_DLFCN_H
#cmakedefine HAVE_ERRNO_H
#cmakedefine HAVE_EXECINFO_H
#cmakedefine HAVE_INTTYPES_H
#cmakedefine HAVE_LINUX_FUTEX_H
#cmakedefine HAVE_MEMORY_H
#cmakedefine HAVE_NETDB_H
#cmakedefine HAVE_PTHREAD_H
#cmakedefine HAVE_SIGNAL_H
#cmakedefine HAVE_SYS_MMAN_H
#cmakedefine HAVE_SYS_PARAM_H
#cmakedefine HAVE_SYS_POLL_H
#cmakedefine HAVE_SYS_RESOURCE_H
#cmakedefine HAVE_SYS_SELECT_H
#cmakedefine HAVE_SYS_SOCKET_H
#cmakedefine HAVE_SYS_STAT_H
#cmakedefine HAVE_SYS_SYSCALL_H
#cmakedefine HAVE_SYS_SYSCTL_H
#cmakedefine HAVE_SYS_TIME_H
#cmakedefine HAVE_SYS_WAIT_H
#cmakedefine HAVE_TIME_H
#cmakedefine HAVE_UCONTEXT_H
#cmakedefine HAVE_UNISTD_H

/* libraries */
#cmakedefine HAVE_LIBEDIT
#cmakedefine HAVE_LIBEVENT
#cmakedefine HAVE_LIBM
#cmakedefine HAVE_LIBRT

/* structs */
#cmakedefine HAVE_MECAB_DICTIONARY_INFO_T

/* functions */
#cmakedefine HAVE__GMTIME64_S
#cmakedefine HAVE__LOCALTIME64_S
#cmakedefine HAVE__STRTOUI64
#cmakedefine HAVE_BACKTRACE
#cmakedefine HAVE_CLOCK
#cmakedefine HAVE_CLOCK_GETTIME
#cmakedefine HAVE_FPCLASSIFY
#cmakedefine HAVE_GMTIME_R
#cmakedefine HAVE_LOCALTIME_R
#cmakedefine HAVE_MKSTEMP
#cmakedefine HAVE_STRCASECMP
#cmakedefine HAVE_STRNCASECMP
#cmakedefine HAVE_STRTOULL
#cmakedefine HAVE_PTHREAD_MUTEXATTR_SETPSHARED
#cmakedefine HAVE_PTHREAD_CONDATTR_SETPSHARED
