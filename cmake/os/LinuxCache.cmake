# Avoid system checks on Linux by pre-caching results. Most of the system checks
# are consistent across modern Linux distributions, and running individual checks
# for each feature significantly increases configuration time, as CMake creates
# a separate compilation test for each check.

IF(CMAKE_SYSTEM_NAME MATCHES "Linux")
SET(HAVE_ALLOCA 1 CACHE INTERNAL "")
SET(HAVE_LDIV 1 CACHE INTERNAL "")
SET(HAVE_MEMCPY 1 CACHE INTERNAL "")
SET(HAVE_MEMMOVE 1 CACHE INTERNAL "")
SET(HAVE_PERROR 1 CACHE INTERNAL "")
SET(HAVE_SETLOCALE 1 CACHE INTERNAL "")
SET(HAVE_STRCOLL 1 CACHE INTERNAL "")
SET(HAVE_STRERROR 1 CACHE INTERNAL "")
SET(HAVE_STRPBRK 1 CACHE INTERNAL "")
SET(HAVE_STRTOLL 1 CACHE INTERNAL "")
SET(HAVE_STRTOUL 1 CACHE INTERNAL "")
SET(HAVE_STRTOULL 1 CACHE INTERNAL "")
SET(HAVE_VSNPRINTF 1 CACHE INTERNAL "")
ENDIF()