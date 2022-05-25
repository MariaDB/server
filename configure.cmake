# Copyright (c) 2009, 2013, Oracle and/or its affiliates. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation; version 2 of the License.
# 
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software
# Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA
#

INCLUDE (CheckCSourceCompiles)
INCLUDE (CheckCXXSourceCompiles)
INCLUDE (CheckStructHasMember)
INCLUDE (CheckLibraryExists)
INCLUDE (CheckFunctionExists)
INCLUDE (CheckCCompilerFlag)
INCLUDE (CheckCSourceRuns)
INCLUDE (CheckSymbolExists)


# WITH_PIC options.Not of much use, PIC is taken care of on platforms
# where it makes sense anyway.
IF(UNIX)
  IF(APPLE)  
    # OSX  executable are always PIC
    SET(WITH_PIC ON)
  ELSE()
    OPTION(WITH_PIC "Generate PIC objects" OFF)
    IF(WITH_PIC)
      SET(CMAKE_C_FLAGS 
        "${CMAKE_C_FLAGS} ${CMAKE_SHARED_LIBRARY_C_FLAGS}")
      SET(CMAKE_CXX_FLAGS 
        "${CMAKE_CXX_FLAGS} ${CMAKE_SHARED_LIBRARY_CXX_FLAGS}")
    ENDIF()
  ENDIF()
ENDIF()



# System type affects version_compile_os variable 
IF(NOT SYSTEM_TYPE)
  IF(PLATFORM)
    SET(SYSTEM_TYPE ${PLATFORM})
  ELSE()
    SET(SYSTEM_TYPE ${CMAKE_SYSTEM_NAME})
  ENDIF()
ENDIF()

IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND (NOT MSVC))
  IF (CMAKE_EXE_LINKER_FLAGS MATCHES " -static " 
     OR CMAKE_EXE_LINKER_FLAGS MATCHES " -static$")
     SET(HAVE_DLOPEN FALSE CACHE "Disable dlopen due to -static flag" FORCE)
     SET(WITHOUT_DYNAMIC_PLUGINS TRUE)
  ENDIF()
ENDIF()

# workaround for old gcc on x86, gcc atomic ops only work under -march=i686
IF(CMAKE_SYSTEM_PROCESSOR STREQUAL "i686" AND CMAKE_COMPILER_IS_GNUCC AND
   CMAKE_C_COMPILER_VERSION VERSION_LESS "4.4.0")
  SET(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -march=i686")
  SET(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -march=i686")
  # query_response_time.cc causes "error: unable to find a register to spill"
  SET(PLUGIN_QUERY_RESPONSE_TIME NO CACHE BOOL "Disabled, gcc is too old")
ENDIF()

# use runtime atomic-support detection in aarch64
IF(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64")
  MY_CHECK_AND_SET_COMPILER_FLAG("-moutline-atomics")
ENDIF()

IF(WITHOUT_DYNAMIC_PLUGINS)
  MESSAGE("Dynamic plugins are disabled.")
ENDIF(WITHOUT_DYNAMIC_PLUGINS)

# Large files, common flag
SET(_LARGEFILE_SOURCE  1)

# If finds the size of a type, set SIZEOF_<type> and HAVE_<type>
FUNCTION(MY_CHECK_TYPE_SIZE type defbase)
  CHECK_TYPE_SIZE("${type}" SIZEOF_${defbase})
  IF(SIZEOF_${defbase})
    SET(HAVE_${defbase} 1 PARENT_SCOPE)
  ENDIF()
ENDFUNCTION()

# Same for structs, setting HAVE_STRUCT_<name> instead
FUNCTION(MY_CHECK_STRUCT_SIZE type defbase)
  CHECK_TYPE_SIZE("struct ${type}" SIZEOF_${defbase})
  IF(SIZEOF_${defbase})
    SET(HAVE_STRUCT_${defbase} 1 PARENT_SCOPE)
  ENDIF()
ENDFUNCTION()

# Searches function in libraries
# if function is found, sets output parameter result to the name of the library
# if function is found in libc, result will be empty 
FUNCTION(MY_SEARCH_LIBS func libs result)
  IF(${${result}})
    # Library is already found or was predefined
    RETURN()
  ENDIF()
  CHECK_FUNCTION_EXISTS(${func} HAVE_${func}_IN_LIBC)
  IF(HAVE_${func}_IN_LIBC)
    SET(${result} "" PARENT_SCOPE)
    RETURN()
  ENDIF()
  FOREACH(lib  ${libs})
    CHECK_LIBRARY_EXISTS(${lib} ${func} "" HAVE_${func}_IN_${lib}) 
    IF(HAVE_${func}_IN_${lib})
      SET(${result} ${lib} PARENT_SCOPE)
      SET(HAVE_${result} 1 PARENT_SCOPE)
      RETURN()
    ENDIF()
  ENDFOREACH()
ENDFUNCTION()

# Find out which libraries to use.
IF(UNIX)
  MY_SEARCH_LIBS(floor m LIBM)
  IF(NOT LIBM)
    MY_SEARCH_LIBS(__infinity m LIBM)
  ENDIF()
  MY_SEARCH_LIBS(gethostbyname_r  "nsl_r;nsl" LIBNSL)
  MY_SEARCH_LIBS(bind "bind;socket" LIBBIND)
  MY_SEARCH_LIBS(crypt crypt LIBCRYPT)
  MY_SEARCH_LIBS(setsockopt socket LIBSOCKET)
  MY_SEARCH_LIBS(sched_yield rt LIBRT)
  IF(NOT LIBRT)
    MY_SEARCH_LIBS(clock_gettime rt LIBRT)
  ENDIF()
  set(THREADS_PREFER_PTHREAD_FLAG ON)
  FIND_PACKAGE(Threads)

  SET(CMAKE_REQUIRED_LIBRARIES 
    ${LIBM} ${LIBNSL} ${LIBBIND} ${LIBCRYPT} ${LIBSOCKET} ${CMAKE_DL_LIBS} ${CMAKE_THREAD_LIBS_INIT} ${LIBRT} ${LIBEXECINFO})
  # Need explicit pthread for gcc -fsanitize=address
  IF(CMAKE_USE_PTHREADS_INIT AND CMAKE_C_FLAGS MATCHES "-fsanitize=")
    SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} pthread)
  ENDIF()

  IF(CMAKE_REQUIRED_LIBRARIES)
    LIST(REMOVE_DUPLICATES CMAKE_REQUIRED_LIBRARIES)
  ENDIF()  
  LINK_LIBRARIES(${CMAKE_THREAD_LIBS_INIT})
  
  OPTION(WITH_LIBWRAP "Compile with tcp wrappers support" OFF)
  IF(WITH_LIBWRAP)
    SET(SAVE_CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES})
    SET(CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES} wrap)
    CHECK_C_SOURCE_COMPILES(
    "
    #include <sys/types.h>
    #include <tcpd.h>
    int allow_severity = 0;
    int deny_severity  = 0;
    int main()
    {
      hosts_access(0);
    }"
    HAVE_LIBWRAP)
    SET(CMAKE_REQUIRED_LIBRARIES ${SAVE_CMAKE_REQUIRED_LIBRARIES})
    IF(HAVE_LIBWRAP)
      SET(MYSYS_LIBWRAP_SOURCE  ${CMAKE_SOURCE_DIR}/mysys/my_libwrap.c)
      SET(LIBWRAP "wrap")
    ENDIF()
  ENDIF()
  ADD_FEATURE_INFO(LIBWRAP HAVE_LIBWRAP "Support for tcp wrappers")
ENDIF()

#
# Tests for header files
#
INCLUDE (CheckIncludeFiles)
INCLUDE (CheckIncludeFileCXX)

CHECK_INCLUDE_FILES ("stdlib.h;stdarg.h;string.h;float.h" STDC_HEADERS)
CHECK_INCLUDE_FILES (sys/types.h HAVE_SYS_TYPES_H)
CHECK_INCLUDE_FILES (alloca.h HAVE_ALLOCA_H)
CHECK_INCLUDE_FILES (arpa/inet.h HAVE_ARPA_INET_H)
CHECK_INCLUDE_FILES (crypt.h HAVE_CRYPT_H)
CHECK_INCLUDE_FILES (dirent.h HAVE_DIRENT_H)
CHECK_INCLUDE_FILES (dlfcn.h HAVE_DLFCN_H)
CHECK_INCLUDE_FILES (execinfo.h HAVE_EXECINFO_H)
CHECK_INCLUDE_FILES (fcntl.h HAVE_FCNTL_H)
CHECK_INCLUDE_FILES (fenv.h HAVE_FENV_H)
CHECK_INCLUDE_FILES (float.h HAVE_FLOAT_H)
CHECK_INCLUDE_FILES (fpu_control.h HAVE_FPU_CONTROL_H)
CHECK_INCLUDE_FILES (grp.h HAVE_GRP_H)
CHECK_INCLUDE_FILES (ieeefp.h HAVE_IEEEFP_H)
CHECK_INCLUDE_FILES (inttypes.h HAVE_INTTYPES_H)
CHECK_INCLUDE_FILES (langinfo.h HAVE_LANGINFO_H)
CHECK_INCLUDE_FILES (link.h HAVE_LINK_H)
CHECK_INCLUDE_FILES (linux/unistd.h HAVE_LINUX_UNISTD_H)
CHECK_INCLUDE_FILES (limits.h HAVE_LIMITS_H)
CHECK_INCLUDE_FILES (locale.h HAVE_LOCALE_H)
CHECK_INCLUDE_FILES (malloc.h HAVE_MALLOC_H)
CHECK_INCLUDE_FILES (memory.h HAVE_MEMORY_H)
CHECK_INCLUDE_FILES (ndir.h HAVE_NDIR_H)
CHECK_INCLUDE_FILES (netinet/in.h HAVE_NETINET_IN_H)
CHECK_INCLUDE_FILES (paths.h HAVE_PATHS_H)
CHECK_INCLUDE_FILES (poll.h HAVE_POLL_H)
CHECK_INCLUDE_FILES (sys/poll.h HAVE_SYS_POLL_H)
CHECK_INCLUDE_FILES (pwd.h HAVE_PWD_H)
CHECK_INCLUDE_FILES (sched.h HAVE_SCHED_H)
CHECK_INCLUDE_FILES (select.h HAVE_SELECT_H)
CHECK_INCLUDE_FILES ("sys/types.h;sys/dir.h" HAVE_SYS_DIR_H)
CHECK_INCLUDE_FILES ("sys/types.h;sys/event.h" HAVE_SYS_EVENT_H)
CHECK_INCLUDE_FILES (sys/ndir.h HAVE_SYS_NDIR_H)
CHECK_INCLUDE_FILES (sys/pte.h HAVE_SYS_PTE_H)
CHECK_INCLUDE_FILES (stddef.h HAVE_STDDEF_H)
CHECK_INCLUDE_FILES (stdint.h HAVE_STDINT_H)
CHECK_INCLUDE_FILES (stdlib.h HAVE_STDLIB_H)
CHECK_INCLUDE_FILES (strings.h HAVE_STRINGS_H)
CHECK_INCLUDE_FILES (string.h HAVE_STRING_H)
CHECK_INCLUDE_FILES (synch.h HAVE_SYNCH_H)
CHECK_INCLUDE_FILES (sysent.h HAVE_SYSENT_H)
CHECK_INCLUDE_FILES (sys/file.h HAVE_SYS_FILE_H)
CHECK_INCLUDE_FILES (sys/fpu.h HAVE_SYS_FPU_H)
CHECK_INCLUDE_FILES (sys/ioctl.h HAVE_SYS_IOCTL_H)
CHECK_INCLUDE_FILES ("sys/types.h;sys/malloc.h" HAVE_SYS_MALLOC_H)
CHECK_INCLUDE_FILES (sys/mman.h HAVE_SYS_MMAN_H)
CHECK_INCLUDE_FILES (linux/mman.h HAVE_LINUX_MMAN_H)
CHECK_INCLUDE_FILES (sys/prctl.h HAVE_SYS_PRCTL_H)
CHECK_INCLUDE_FILES (sys/resource.h HAVE_SYS_RESOURCE_H)
CHECK_INCLUDE_FILES (sys/select.h HAVE_SYS_SELECT_H)
CHECK_INCLUDE_FILES (sys/socket.h HAVE_SYS_SOCKET_H)
CHECK_INCLUDE_FILES (sys/stat.h HAVE_SYS_STAT_H)
CHECK_INCLUDE_FILES (sys/stream.h HAVE_SYS_STREAM_H)
CHECK_INCLUDE_FILES (sys/syscall.h HAVE_SYS_SYSCALL_H)
CHECK_INCLUDE_FILES (asm/termbits.h HAVE_ASM_TERMBITS_H)
CHECK_INCLUDE_FILES (termbits.h HAVE_TERMBITS_H)
CHECK_INCLUDE_FILES (termios.h HAVE_TERMIOS_H)
CHECK_INCLUDE_FILES (termio.h HAVE_TERMIO_H)
CHECK_INCLUDE_FILES (termcap.h HAVE_TERMCAP_H)
CHECK_INCLUDE_FILES (unistd.h HAVE_UNISTD_H)
CHECK_INCLUDE_FILES (utime.h HAVE_UTIME_H)
CHECK_INCLUDE_FILES (varargs.h HAVE_VARARGS_H)
CHECK_INCLUDE_FILES (sys/time.h HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILES (sys/utime.h HAVE_SYS_UTIME_H)
CHECK_INCLUDE_FILES (sys/wait.h HAVE_SYS_WAIT_H)
CHECK_INCLUDE_FILES (sys/param.h HAVE_SYS_PARAM_H)
CHECK_INCLUDE_FILES (sys/vadvise.h HAVE_SYS_VADVISE_H)
CHECK_INCLUDE_FILES (fnmatch.h HAVE_FNMATCH_H)
CHECK_INCLUDE_FILES (stdarg.h  HAVE_STDARG_H)
CHECK_INCLUDE_FILES ("stdlib.h;sys/un.h" HAVE_SYS_UN_H)
CHECK_INCLUDE_FILES (wchar.h HAVE_WCHAR_H)
CHECK_INCLUDE_FILES (wctype.h HAVE_WCTYPE_H)
CHECK_INCLUDE_FILES (sys/sockio.h HAVE_SYS_SOCKIO_H)
CHECK_INCLUDE_FILES (sys/utsname.h HAVE_SYS_UTSNAME_H)
CHECK_INCLUDE_FILES (sys/statvfs.h HAVE_SYS_STATVFS_H)

SET(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS} -DPACKAGE=test) # bfd.h is picky
CHECK_INCLUDE_FILES (bfd.h BFD_H_EXISTS)
IF(BFD_H_EXISTS)
  IF(NOT_FOR_DISTRIBUTION)
    SET(NON_DISTRIBUTABLE_WARNING "GPLv3" CACHE INTERNAL "")
    SET(HAVE_BFD_H 1)
  ENDIF()
ENDIF()

IF(HAVE_SYS_STREAM_H)
  # Needs sys/stream.h on Solaris
  CHECK_INCLUDE_FILES ("sys/stream.h;sys/ptem.h" HAVE_SYS_PTEM_H)
ELSE()
  CHECK_INCLUDE_FILES (sys/ptem.h HAVE_SYS_PTEM_H)
ENDIF()

# Figure out threading library
#
FIND_PACKAGE (Threads)

FUNCTION(MY_CHECK_PTHREAD_ONCE_INIT)
  MY_CHECK_C_COMPILER_FLAG("-Werror")
  IF(NOT have_C__Werror)
    RETURN()
  ENDIF()
  SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
  CHECK_C_SOURCE_COMPILES("
    #include <pthread.h>
    void foo(void) {}
    int main()
    {
      pthread_once_t once_control = PTHREAD_ONCE_INIT;
      pthread_once(&once_control, foo);
      return 0;
    }"
    HAVE_PTHREAD_ONCE_INIT
  )
  # http://bugs.opensolaris.org/bugdatabase/printableBug.do?bug_id=6611808
  IF(NOT HAVE_PTHREAD_ONCE_INIT)
    CHECK_C_SOURCE_COMPILES("
      #include <pthread.h>
      void foo(void) {}
      int main()
      {
        pthread_once_t once_control = { PTHREAD_ONCE_INIT };
        pthread_once(&once_control, foo);
        return 0;
      }"
      HAVE_ARRAY_PTHREAD_ONCE_INIT
    )
  ENDIF()
  IF(HAVE_PTHREAD_ONCE_INIT)
    SET(PTHREAD_ONCE_INITIALIZER "PTHREAD_ONCE_INIT" PARENT_SCOPE)
  ENDIF()
  IF(HAVE_ARRAY_PTHREAD_ONCE_INIT)
    SET(PTHREAD_ONCE_INITIALIZER "{ PTHREAD_ONCE_INIT }" PARENT_SCOPE)
  ENDIF()
ENDFUNCTION()

IF(CMAKE_USE_PTHREADS_INIT)
  MY_CHECK_PTHREAD_ONCE_INIT()
ENDIF()

#
# Tests for functions
#
CHECK_FUNCTION_EXISTS (accept4 HAVE_ACCEPT4)
CHECK_FUNCTION_EXISTS (access HAVE_ACCESS)
CHECK_FUNCTION_EXISTS (alarm HAVE_ALARM)
SET(HAVE_ALLOCA 1)
CHECK_FUNCTION_EXISTS (backtrace HAVE_BACKTRACE)
CHECK_FUNCTION_EXISTS (backtrace_symbols HAVE_BACKTRACE_SYMBOLS)
CHECK_FUNCTION_EXISTS (backtrace_symbols_fd HAVE_BACKTRACE_SYMBOLS_FD)
CHECK_FUNCTION_EXISTS (printstack HAVE_PRINTSTACK)
CHECK_FUNCTION_EXISTS (bfill HAVE_BFILL)
CHECK_FUNCTION_EXISTS (index HAVE_INDEX)
CHECK_FUNCTION_EXISTS (clock_gettime HAVE_CLOCK_GETTIME)
CHECK_FUNCTION_EXISTS (cuserid HAVE_CUSERID)
CHECK_FUNCTION_EXISTS (ftruncate HAVE_FTRUNCATE)
CHECK_FUNCTION_EXISTS (compress HAVE_COMPRESS)
CHECK_FUNCTION_EXISTS (crypt HAVE_CRYPT)
CHECK_FUNCTION_EXISTS (dladdr HAVE_DLADDR)
CHECK_FUNCTION_EXISTS (dlerror HAVE_DLERROR)
CHECK_FUNCTION_EXISTS (dlopen HAVE_DLOPEN)
CHECK_FUNCTION_EXISTS (fchmod HAVE_FCHMOD)
CHECK_FUNCTION_EXISTS (fcntl HAVE_FCNTL)
CHECK_FUNCTION_EXISTS (fdatasync HAVE_FDATASYNC)
CHECK_SYMBOL_EXISTS(fdatasync "unistd.h" HAVE_DECL_FDATASYNC)
CHECK_FUNCTION_EXISTS (fesetround HAVE_FESETROUND)
CHECK_FUNCTION_EXISTS (fedisableexcept HAVE_FEDISABLEEXCEPT)
CHECK_FUNCTION_EXISTS (fseeko HAVE_FSEEKO)
CHECK_FUNCTION_EXISTS (fsync HAVE_FSYNC)
CHECK_FUNCTION_EXISTS (getcwd HAVE_GETCWD)
CHECK_FUNCTION_EXISTS (gethostbyaddr_r HAVE_GETHOSTBYADDR_R)
CHECK_FUNCTION_EXISTS (gethrtime HAVE_GETHRTIME)
CHECK_FUNCTION_EXISTS (getpass HAVE_GETPASS)
CHECK_FUNCTION_EXISTS (getpassphrase HAVE_GETPASSPHRASE)
CHECK_FUNCTION_EXISTS (getpwnam HAVE_GETPWNAM)
CHECK_FUNCTION_EXISTS (getpwuid HAVE_GETPWUID)
CHECK_FUNCTION_EXISTS (getrlimit HAVE_GETRLIMIT)
CHECK_FUNCTION_EXISTS (getifaddrs HAVE_GETIFADDRS)
CHECK_FUNCTION_EXISTS (getrusage HAVE_GETRUSAGE)
CHECK_FUNCTION_EXISTS (getwd HAVE_GETWD)
CHECK_FUNCTION_EXISTS (gmtime_r HAVE_GMTIME_R)
CHECK_FUNCTION_EXISTS (initgroups HAVE_INITGROUPS)
CHECK_FUNCTION_EXISTS (ldiv HAVE_LDIV)
CHECK_FUNCTION_EXISTS (localtime_r HAVE_LOCALTIME_R)
CHECK_FUNCTION_EXISTS (lstat HAVE_LSTAT)
CHECK_FUNCTION_EXISTS (madvise HAVE_MADVISE)
CHECK_FUNCTION_EXISTS (mallinfo HAVE_MALLINFO)
CHECK_FUNCTION_EXISTS (mallinfo2 HAVE_MALLINFO2)
CHECK_FUNCTION_EXISTS (malloc_zone_statistics HAVE_MALLOC_ZONE)
CHECK_FUNCTION_EXISTS (memcpy HAVE_MEMCPY)
CHECK_FUNCTION_EXISTS (memmove HAVE_MEMMOVE)
CHECK_FUNCTION_EXISTS (mkstemp HAVE_MKSTEMP)
CHECK_FUNCTION_EXISTS (mkostemp HAVE_MKOSTEMP)
CHECK_FUNCTION_EXISTS (mlock HAVE_MLOCK)
CHECK_FUNCTION_EXISTS (mlockall HAVE_MLOCKALL)
CHECK_FUNCTION_EXISTS (mmap HAVE_MMAP)
CHECK_FUNCTION_EXISTS (mmap64 HAVE_MMAP64)
CHECK_FUNCTION_EXISTS (mprotect HAVE_MPROTECT)
CHECK_FUNCTION_EXISTS (perror HAVE_PERROR)
CHECK_FUNCTION_EXISTS (poll HAVE_POLL)
CHECK_FUNCTION_EXISTS (posix_fallocate HAVE_POSIX_FALLOCATE)
CHECK_FUNCTION_EXISTS (pread HAVE_PREAD)
CHECK_FUNCTION_EXISTS (pthread_attr_create HAVE_PTHREAD_ATTR_CREATE)
CHECK_FUNCTION_EXISTS (pthread_attr_getstacksize HAVE_PTHREAD_ATTR_GETSTACKSIZE)
CHECK_FUNCTION_EXISTS (pthread_attr_setscope HAVE_PTHREAD_ATTR_SETSCOPE)
CHECK_FUNCTION_EXISTS (pthread_attr_getguardsize HAVE_PTHREAD_ATTR_GETGUARDSIZE)
CHECK_FUNCTION_EXISTS (pthread_attr_setstacksize HAVE_PTHREAD_ATTR_SETSTACKSIZE)
CHECK_FUNCTION_EXISTS (pthread_condattr_create HAVE_PTHREAD_CONDATTR_CREATE)
CHECK_FUNCTION_EXISTS (pthread_getaffinity_np HAVE_PTHREAD_GETAFFINITY_NP)
CHECK_FUNCTION_EXISTS (pthread_key_delete HAVE_PTHREAD_KEY_DELETE)
CHECK_FUNCTION_EXISTS (pthread_rwlock_rdlock HAVE_PTHREAD_RWLOCK_RDLOCK)
CHECK_FUNCTION_EXISTS (pthread_sigmask HAVE_PTHREAD_SIGMASK)
CHECK_FUNCTION_EXISTS (pthread_yield_np HAVE_PTHREAD_YIELD_NP)
CHECK_FUNCTION_EXISTS (putenv HAVE_PUTENV)
CHECK_FUNCTION_EXISTS (readlink HAVE_READLINK)
CHECK_FUNCTION_EXISTS (realpath HAVE_REALPATH)
CHECK_FUNCTION_EXISTS (rename HAVE_RENAME)
CHECK_FUNCTION_EXISTS (rwlock_init HAVE_RWLOCK_INIT)
CHECK_FUNCTION_EXISTS (sched_yield HAVE_SCHED_YIELD)
CHECK_FUNCTION_EXISTS (setenv HAVE_SETENV)
CHECK_FUNCTION_EXISTS (setlocale HAVE_SETLOCALE)
CHECK_FUNCTION_EXISTS (sigaction HAVE_SIGACTION)
CHECK_FUNCTION_EXISTS (sigthreadmask HAVE_SIGTHREADMASK)
CHECK_FUNCTION_EXISTS (sigwait HAVE_SIGWAIT)
CHECK_FUNCTION_EXISTS (sigwaitinfo HAVE_SIGWAITINFO)
CHECK_FUNCTION_EXISTS (sigset HAVE_SIGSET)
CHECK_FUNCTION_EXISTS (sleep HAVE_SLEEP)
CHECK_FUNCTION_EXISTS (snprintf HAVE_SNPRINTF)
CHECK_FUNCTION_EXISTS (stpcpy HAVE_STPCPY)
CHECK_FUNCTION_EXISTS (strcoll HAVE_STRCOLL)
CHECK_FUNCTION_EXISTS (strerror HAVE_STRERROR)
CHECK_FUNCTION_EXISTS (strnlen HAVE_STRNLEN)
CHECK_FUNCTION_EXISTS (strpbrk HAVE_STRPBRK)
CHECK_FUNCTION_EXISTS (strtok_r HAVE_STRTOK_R)
CHECK_FUNCTION_EXISTS (strtoll HAVE_STRTOLL)
CHECK_FUNCTION_EXISTS (strtoul HAVE_STRTOUL)
CHECK_FUNCTION_EXISTS (strtoull HAVE_STRTOULL)
CHECK_FUNCTION_EXISTS (strcasecmp HAVE_STRCASECMP)
CHECK_FUNCTION_EXISTS (tell HAVE_TELL)
CHECK_FUNCTION_EXISTS (thr_setconcurrency HAVE_THR_SETCONCURRENCY)
CHECK_FUNCTION_EXISTS (thr_yield HAVE_THR_YIELD)
CHECK_FUNCTION_EXISTS (vasprintf HAVE_VASPRINTF)
CHECK_FUNCTION_EXISTS (vsnprintf HAVE_VSNPRINTF)
CHECK_FUNCTION_EXISTS (memalign HAVE_MEMALIGN)
CHECK_FUNCTION_EXISTS (nl_langinfo HAVE_NL_LANGINFO)

IF(HAVE_SYS_EVENT_H)
CHECK_FUNCTION_EXISTS (kqueue HAVE_KQUEUE)
ENDIF()

# readdir_r might exist, but be marked deprecated
SET(CMAKE_REQUIRED_FLAGS -Werror)
CHECK_CXX_SOURCE_COMPILES(
"#include  <dirent.h>
int main() {
  readdir_r(0,0,0);
  return 0;
  }" HAVE_READDIR_R)
SET(CMAKE_REQUIRED_FLAGS)

#--------------------------------------------------------------------
# Support for WL#2373 (Use cycle counter for timing)
#--------------------------------------------------------------------

CHECK_INCLUDE_FILES(time.h HAVE_TIME_H)
CHECK_INCLUDE_FILES(sys/time.h HAVE_SYS_TIME_H)
CHECK_INCLUDE_FILES(sys/times.h HAVE_SYS_TIMES_H)

CHECK_INCLUDE_FILES(ia64intrin.h HAVE_IA64INTRIN_H)

CHECK_FUNCTION_EXISTS(times HAVE_TIMES)
CHECK_FUNCTION_EXISTS(gettimeofday HAVE_GETTIMEOFDAY)
CHECK_FUNCTION_EXISTS(read_real_time HAVE_READ_REAL_TIME)
# This should work on AIX.

CHECK_FUNCTION_EXISTS(ftime HAVE_FTIME)
# This is still a normal call for milliseconds.

CHECK_FUNCTION_EXISTS(time HAVE_TIME)
# We can use time() on Macintosh if there is no ftime().


#
# Tests for symbols
#

#CHECK_SYMBOL_EXISTS(sys_errlist "stdio.h" HAVE_SYS_ERRLIST)
CHECK_SYMBOL_EXISTS(madvise "sys/mman.h" HAVE_DECL_MADVISE)
CHECK_SYMBOL_EXISTS(getpagesizes "sys/mman.h" HAVE_GETPAGESIZES)
CHECK_SYMBOL_EXISTS(tzname "time.h" HAVE_TZNAME)
CHECK_SYMBOL_EXISTS(lrand48 "stdlib.h" HAVE_LRAND48)
CHECK_SYMBOL_EXISTS(getpagesize "unistd.h" HAVE_GETPAGESIZE)
CHECK_SYMBOL_EXISTS(TIOCGWINSZ "sys/ioctl.h" GWINSZ_IN_SYS_IOCTL)
CHECK_SYMBOL_EXISTS(FIONREAD "sys/ioctl.h" FIONREAD_IN_SYS_IOCTL)
CHECK_SYMBOL_EXISTS(TIOCSTAT "sys/ioctl.h" TIOCSTAT_IN_SYS_IOCTL)
CHECK_SYMBOL_EXISTS(FIONREAD "sys/filio.h" FIONREAD_IN_SYS_FILIO)
CHECK_SYMBOL_EXISTS(gettimeofday "sys/time.h" HAVE_GETTIMEOFDAY)

#
# Test for endianness
#
INCLUDE(TestBigEndian)
IF(APPLE)
  # Cannot run endian test on universal PPC/Intel binaries 
  # would return inconsistent result.
  # config.h.cmake includes a special #ifdef for Darwin
ELSE()
  TEST_BIG_ENDIAN(WORDS_BIGENDIAN)
ENDIF()

#
# Tests for type sizes (and presence)
#
INCLUDE (CheckTypeSize)
set(CMAKE_REQUIRED_DEFINITIONS ${CMAKE_REQUIRED_DEFINITIONS}
        -D_LARGEFILE_SOURCE -D_LARGE_FILES -D_FILE_OFFSET_BITS=64
        -D__STDC_LIMIT_MACROS -D__STDC_CONSTANT_MACROS -D__STDC_FORMAT_MACROS)
SET(CMAKE_EXTRA_INCLUDE_FILES signal.h)
MY_CHECK_TYPE_SIZE(sigset_t SIGSET_T)
IF(NOT SIZEOF_SIGSET_T)
 SET(sigset_t int)
ENDIF()
MY_CHECK_TYPE_SIZE(mode_t MODE_T)
IF(NOT SIZEOF_MODE_T)
 SET(mode_t int)
ENDIF()
MY_CHECK_TYPE_SIZE(sighandler_t SIGHANDLER_T)

IF(HAVE_NETINET_IN_H)
  SET(CMAKE_EXTRA_INCLUDE_FILES netinet/in.h)
  MY_CHECK_TYPE_SIZE(in_addr_t IN_ADDR_T)
ENDIF(HAVE_NETINET_IN_H)

IF(HAVE_STDINT_H)
  SET(CMAKE_EXTRA_INCLUDE_FILES stdint.h)
ENDIF(HAVE_STDINT_H)

SET(HAVE_VOIDP 1)
SET(HAVE_CHARP 1)
SET(HAVE_LONG 1)

IF(NOT APPLE)
MY_CHECK_TYPE_SIZE("void *" VOIDP)
MY_CHECK_TYPE_SIZE("char *" CHARP)
MY_CHECK_TYPE_SIZE(long LONG)
MY_CHECK_TYPE_SIZE(size_t SIZE_T)
ENDIF()

MY_CHECK_TYPE_SIZE(short SHORT)
MY_CHECK_TYPE_SIZE(int INT)
MY_CHECK_TYPE_SIZE("long long" LONG_LONG)
SET(CMAKE_EXTRA_INCLUDE_FILES stdio.h sys/types.h)
MY_CHECK_TYPE_SIZE(off_t OFF_T)
MY_CHECK_TYPE_SIZE(uchar UCHAR)
MY_CHECK_TYPE_SIZE(uint UINT)
MY_CHECK_TYPE_SIZE(ulong ULONG)
MY_CHECK_TYPE_SIZE(int8 INT8)
MY_CHECK_TYPE_SIZE(uint8 UINT8)
MY_CHECK_TYPE_SIZE(int16 INT16)
MY_CHECK_TYPE_SIZE(uint16 UINT16)
MY_CHECK_TYPE_SIZE(int32 INT32)
MY_CHECK_TYPE_SIZE(uint32 UINT32)
MY_CHECK_TYPE_SIZE(int64 INT64)
MY_CHECK_TYPE_SIZE(uint64 UINT64)
MY_CHECK_TYPE_SIZE(time_t TIME_T)
SET (CMAKE_EXTRA_INCLUDE_FILES sys/types.h)
SET(CMAKE_EXTRA_INCLUDE_FILES)
IF(HAVE_SYS_SOCKET_H)
  SET(CMAKE_EXTRA_INCLUDE_FILES sys/socket.h)
ENDIF(HAVE_SYS_SOCKET_H)
SET(CMAKE_EXTRA_INCLUDE_FILES)

IF(HAVE_IEEEFP_H)
  SET(CMAKE_EXTRA_INCLUDE_FILES ieeefp.h)
  MY_CHECK_TYPE_SIZE(fp_except FP_EXCEPT)
ENDIF()


#
# Code tests
#

# check whether time_t is unsigned
CHECK_C_SOURCE_COMPILES("
#include <time.h>
int main()
{
  int array[(((time_t)-1) > 0) ? 1 : -1];
  return 0;
}"
TIME_T_UNSIGNED)

CHECK_C_SOURCE_COMPILES("
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#endif
int main()
{
  select(0,0,0,0,0);
  return 0;
}"
HAVE_SELECT)

#
# Check if timespec has ts_sec and ts_nsec fields
#

CHECK_C_SOURCE_COMPILES("
#include <pthread.h>

int main(int ac, char **av)
{
  struct timespec abstime;
  abstime.ts_sec = time(NULL)+1;
  abstime.ts_nsec = 0;
}
" HAVE_TIMESPEC_TS_SEC)


#
# Check return type of qsort()
#
CHECK_C_SOURCE_COMPILES("
#include <stdlib.h>
#ifdef __cplusplus
extern \"C\"
#endif
void qsort(void *base, size_t nel, size_t width,
  int (*compar) (const void *, const void *));
int main(int ac, char **av) {}
" QSORT_TYPE_IS_VOID)
IF(QSORT_TYPE_IS_VOID)
  SET(RETQSORTTYPE "void")
ELSE(QSORT_TYPE_IS_VOID)
  SET(RETQSORTTYPE "int")
ENDIF(QSORT_TYPE_IS_VOID)

IF(WIN32)
SET(SOCKET_SIZE_TYPE int)
ELSE()
CHECK_CXX_SOURCE_COMPILES("
#include <sys/socket.h>
int main(int argc, char **argv)
{
  getsockname(0,0,(socklen_t *) 0);
  return 0; 
}"
HAVE_SOCKET_SIZE_T_AS_socklen_t)

IF(HAVE_SOCKET_SIZE_T_AS_socklen_t)
  SET(SOCKET_SIZE_TYPE socklen_t)
ELSE()
  CHECK_CXX_SOURCE_COMPILES("
  #include <sys/socket.h>
  int main(int argc, char **argv)
  {
    getsockname(0,0,(int *) 0);
    return 0; 
  }"
  HAVE_SOCKET_SIZE_T_AS_int)
  IF(HAVE_SOCKET_SIZE_T_AS_int)
    SET(SOCKET_SIZE_TYPE int)
  ELSE()
    CHECK_CXX_SOURCE_COMPILES("
    #include <sys/socket.h>
    int main(int argc, char **argv)
    {
      getsockname(0,0,(size_t *) 0);
      return 0; 
    }"
    HAVE_SOCKET_SIZE_T_AS_size_t)
    IF(HAVE_SOCKET_SIZE_T_AS_size_t)
      SET(SOCKET_SIZE_TYPE size_t)
    ELSE()
      SET(SOCKET_SIZE_TYPE int)
    ENDIF()
  ENDIF()
ENDIF()
ENDIF()

CHECK_CXX_SOURCE_COMPILES("
#include <pthread.h>
int main()
{
  pthread_yield();
  return 0;
}
" HAVE_PTHREAD_YIELD_ZERO_ARG)

IF(NOT STACK_DIRECTION)
  IF(CMAKE_CROSSCOMPILING AND NOT DEFINED CMAKE_CROSSCOMPILING_EMULATOR)
    MESSAGE(FATAL_ERROR 
    "STACK_DIRECTION is not defined.  Please specify -DSTACK_DIRECTION=1 "
    "or -DSTACK_DIRECTION=-1 when calling cmake.")
  ELSE()
    TRY_RUN(STACKDIR_RUN_RESULT STACKDIR_COMPILE_RESULT    
     ${CMAKE_BINARY_DIR} 
     ${CMAKE_SOURCE_DIR}/cmake/stack_direction.c
     )
     # Test program returns 0 (down) or 1 (up).
     # Convert to -1 or 1
     IF(STACKDIR_RUN_RESULT EQUAL 0)
       SET(STACK_DIRECTION -1 CACHE INTERNAL "Stack grows direction")
     ELSE()
       SET(STACK_DIRECTION 1 CACHE INTERNAL "Stack grows direction")
     ENDIF()
     MESSAGE(STATUS "Checking stack direction : ${STACK_DIRECTION}")
   ENDIF()
ENDIF()

#
# Check return type of signal handlers
#
CHECK_C_SOURCE_COMPILES("
#include <signal.h>
#ifdef signal
# undef signal
#endif
#ifdef __cplusplus
extern \"C\" void (*signal (int, void (*)(int)))(int);
#else
void (*signal ()) ();
#endif
int main(int ac, char **av) {}
" SIGNAL_RETURN_TYPE_IS_VOID)
IF(SIGNAL_RETURN_TYPE_IS_VOID)
  SET(RETSIGTYPE void)
  SET(VOID_SIGHANDLER 1)
ELSE(SIGNAL_RETURN_TYPE_IS_VOID)
  SET(RETSIGTYPE int)
ENDIF(SIGNAL_RETURN_TYPE_IS_VOID)


CHECK_INCLUDE_FILES("time.h;sys/time.h" TIME_WITH_SYS_TIME)
CHECK_SYMBOL_EXISTS(O_NONBLOCK "unistd.h;fcntl.h" HAVE_FCNTL_NONBLOCK)
IF(NOT HAVE_FCNTL_NONBLOCK)
 SET(NO_FCNTL_NONBLOCK 1)
ENDIF()

#
# Test for how the C compiler does inline, if at all
#
# SunPro is weird, apparently it only supports inline at -xO3 or -xO4.
# And if CMAKE_C_FLAGS has -xO4 but CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE} has -xO2
# then CHECK_C_SOURCE_COMPILES will succeed but the built will fail.
# We must test all flags here.
# XXX actually, we can do this for all compilers, not only SunPro
IF (CMAKE_CXX_COMPILER_ID MATCHES "SunPro" AND
    CMAKE_GENERATOR MATCHES "Makefiles")
  STRING(TOUPPER "CMAKE_C_FLAGS_${CMAKE_BUILD_TYPE}" flags)
  SET(CMAKE_REQUIRED_FLAGS "${${flags}}")
ENDIF()
CHECK_C_SOURCE_COMPILES("
extern int bar(int x);
static inline int foo(){return bar(1);}
int main(int argc, char *argv[]){return 0;}"
                            C_HAS_inline)
IF(NOT C_HAS_inline)
  CHECK_C_SOURCE_COMPILES("
  extern int bar(int x);
  static __inline int foo(){return bar(1);}
  int main(int argc, char *argv[]){return 0;}"
                            C_HAS___inline)
  IF(C_HAS___inline)
    SET(C_INLINE __inline)
  ElSE()
    SET(C_INLINE)
    MESSAGE(WARNING "C compiler does not support funcion inlining")
    IF(NOT NOINLINE)
      MESSAGE(FATAL_ERROR "Use -DNOINLINE=TRUE to allow compilation without inlining")
    ENDIF()
  ENDIF()
ENDIF()

CHECK_SYMBOL_EXISTS(tcgetattr "termios.h" HAVE_TCGETATTR 1)

#
# Check type of signal routines (posix, 4.2bsd, 4.1bsd or v7)
#
CHECK_C_SOURCE_COMPILES("
  #include <signal.h>
  int main(int ac, char **av)
  {
    sigset_t ss;
    struct sigaction sa;
    sigemptyset(&ss); sigsuspend(&ss);
    sigaction(SIGINT, &sa, (struct sigaction *) 0);
    sigprocmask(SIG_BLOCK, &ss, (sigset_t *) 0);
  }"
  HAVE_POSIX_SIGNALS)

IF(NOT HAVE_POSIX_SIGNALS)
 CHECK_C_SOURCE_COMPILES("
  #include <signal.h>
  int main(int ac, char **av)
  {
    int mask = sigmask(SIGINT);
    sigsetmask(mask); sigblock(mask); sigpause(mask);
  }"
  HAVE_BSD_SIGNALS)
ENDIF(NOT HAVE_POSIX_SIGNALS)

# Assume regular sprintf
SET(SPRINTFS_RETURNS_INT 1)

IF(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
CHECK_CXX_SOURCE_COMPILES("
 #include <cxxabi.h>
 int main(int argc, char **argv) 
  {
    char *foo= 0; int bar= 0;
    foo= abi::__cxa_demangle(foo, foo, 0, &bar);
    return 0;
  }"
  HAVE_ABI_CXA_DEMANGLE)
ENDIF()

CHECK_C_SOURCE_COMPILES("
    int main()
    {
      extern void __attribute__((weak)) foo(void);
      return 0;
    }"
    HAVE_WEAK_SYMBOL
)

CHECK_C_SOURCE_COMPILES("
    void foo(int *x) { }
    int main() {
      int a __attribute__((cleanup(foo)));
      return 0;
    }"
    HAVE_ATTRIBUTE_CLEANUP
)

CHECK_CXX_SOURCE_COMPILES("
    #include <new>
    int main()
    {
     char *c = new char;
     return 0;
    }"
    HAVE_CXX_NEW
)

CHECK_CXX_SOURCE_COMPILES("
    #undef inline
    #if !defined(SCO) && !defined(__osf__) && !defined(_REENTRANT)
    #define _REENTRANT
    #endif
    #include <pthread.h>
    #include <sys/types.h>
    #include <sys/socket.h>
    #include <netinet/in.h>
    #include <arpa/inet.h>
    #include <netdb.h>
    int main()
    {

       struct hostent *foo =
       gethostbyaddr_r((const char *) 0,
          0, 0, (struct hostent *) 0, (char *) NULL,  0, (int *)0);
       return 0;
    }
  "
  HAVE_SOLARIS_STYLE_GETHOST)

SET(NO_ALARM 1 CACHE BOOL  "No need to use alarm to implement timeout")

# As a consequence of ALARMs no longer being used, thread
# notification for KILL must close the socket to wake up
# other threads.
SET(SIGNAL_WITH_VIO_CLOSE 1)

MARK_AS_ADVANCED(NO_ALARM)


CHECK_CXX_SOURCE_COMPILES("
int main()
{
  char x=1;
  short y=1;
  int z=1;
  long w = 1;
  long long s = 1;
  x = __atomic_add_fetch(&x, 1, __ATOMIC_SEQ_CST);
  y = __atomic_add_fetch(&y, 1, __ATOMIC_SEQ_CST);
  z = __atomic_add_fetch(&z, 1, __ATOMIC_SEQ_CST);
  w = __atomic_add_fetch(&w, 1, __ATOMIC_SEQ_CST);
  return (int)__atomic_load_n(&s, __ATOMIC_SEQ_CST);
}"
HAVE_GCC_C11_ATOMICS_WITHOUT_LIBATOMIC)
IF (HAVE_GCC_C11_ATOMICS_WITHOUT_LIBATOMIC)
  SET(HAVE_GCC_C11_ATOMICS True)
ELSE()
  SET(OLD_CMAKE_REQUIRED_LIBRARIES ${CMAKE_REQUIRED_LIBRARIES})
  LIST(APPEND CMAKE_REQUIRED_LIBRARIES "atomic")
  CHECK_CXX_SOURCE_COMPILES("
  int main()
  {
    char x=1;
    short y=1;
    int z=1;
    long w = 1;
    long long s = 1;
    x = __atomic_add_fetch(&x, 1, __ATOMIC_SEQ_CST);
    y = __atomic_add_fetch(&y, 1, __ATOMIC_SEQ_CST);
    z = __atomic_add_fetch(&z, 1, __ATOMIC_SEQ_CST);
    w = __atomic_add_fetch(&w, 1, __ATOMIC_SEQ_CST);
    return (int)__atomic_load_n(&s, __ATOMIC_SEQ_CST);
  }"
  HAVE_GCC_C11_ATOMICS_WITH_LIBATOMIC)
  IF(HAVE_GCC_C11_ATOMICS_WITH_LIBATOMIC)
    SET(HAVE_GCC_C11_ATOMICS True)
  ENDIF()
  SET(CMAKE_REQUIRED_LIBRARIES ${OLD_CMAKE_REQUIRED_LIBRARIES})
ENDIF()

IF(WITH_VALGRIND)
  SET(HAVE_valgrind 1)
ENDIF()

CHECK_INCLUDE_FILES("valgrind/memcheck.h;valgrind/valgrind.h" 
  HAVE_VALGRIND_MEMCHECK_H)

#--------------------------------------------------------------------
# Check for IPv6 support
#--------------------------------------------------------------------
CHECK_INCLUDE_FILE(netinet/in6.h HAVE_NETINET_IN6_H)

IF(UNIX)
  SET(CMAKE_EXTRA_INCLUDE_FILES sys/types.h netinet/in.h sys/socket.h)
  IF(HAVE_NETINET_IN6_H)
    SET(CMAKE_EXTRA_INCLUDE_FILES ${CMAKE_EXTRA_INCLUDE_FILES} netinet/in6.h)
  ENDIF()
ELSEIF(WIN32)
  SET(CMAKE_EXTRA_INCLUDE_FILES ${CMAKE_EXTRA_INCLUDE_FILES} winsock2.h ws2ipdef.h)
ENDIF()

MY_CHECK_STRUCT_SIZE("sockaddr_in6" SOCKADDR_IN6)
MY_CHECK_STRUCT_SIZE("in6_addr" IN6_ADDR)

IF(HAVE_STRUCT_SOCKADDR_IN6 OR HAVE_STRUCT_IN6_ADDR)
  SET(HAVE_IPV6 TRUE CACHE INTERNAL "")
ENDIF()


# Check for sockaddr_storage.ss_family
# It is called differently under OS400 and older AIX

CHECK_STRUCT_HAS_MEMBER("struct sockaddr_storage"
 ss_family "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_STORAGE_SS_FAMILY)
IF(NOT HAVE_SOCKADDR_STORAGE_SS_FAMILY)
  CHECK_STRUCT_HAS_MEMBER("struct sockaddr_storage"
  __ss_family "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_STORAGE___SS_FAMILY)
  IF(HAVE_SOCKADDR_STORAGE___SS_FAMILY)
    SET(ss_family __ss_family)
  ENDIF()
ENDIF()

#
# Check if struct sockaddr_in::sin_len is available.
#

CHECK_STRUCT_HAS_MEMBER("struct sockaddr_in" sin_len
  "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_IN_SIN_LEN)

#
# Check if struct sockaddr_in6::sin6_len is available.
#

CHECK_STRUCT_HAS_MEMBER("struct sockaddr_in6" sin6_len
  "${CMAKE_EXTRA_INCLUDE_FILES}" HAVE_SOCKADDR_IN6_SIN6_LEN)

SET(CMAKE_EXTRA_INCLUDE_FILES) 

CHECK_STRUCT_HAS_MEMBER("struct dirent" d_ino "dirent.h"  STRUCT_DIRENT_HAS_D_INO)
CHECK_STRUCT_HAS_MEMBER("struct dirent" d_namlen "dirent.h"  STRUCT_DIRENT_HAS_D_NAMLEN)
SET(SPRINTF_RETURNS_INT 1)

CHECK_STRUCT_HAS_MEMBER("struct timespec" tv_sec "time.h" STRUCT_TIMESPEC_HAS_TV_SEC)
CHECK_STRUCT_HAS_MEMBER("struct timespec" tv_nsec "time.h" STRUCT_TIMESPEC_HAS_TV_NSEC)

IF(NOT MSVC)
  CHECK_C_SOURCE_COMPILES(
  "
  #define _GNU_SOURCE
  #include <fcntl.h>
  #include <linux/falloc.h>
  int main()
  {
    /* Ignore the return value for now. Check if the flags exist.
    The return value is checked  at runtime. */
    fallocate(0, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 0);

    return(0);
  }"
  HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE
  )
ENDIF()

MY_CHECK_C_COMPILER_FLAG("-Werror")
IF(have_C__Werror)
  SET(SAVE_CMAKE_REQUIRED_FLAGS ${CMAKE_REQUIRED_FLAGS})
  SET(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS} -Werror")
  CHECK_C_SOURCE_COMPILES("
    #include <unistd.h>
    int main()
    {
      pid_t pid=vfork();
      return (int)pid;
    }"
    HAVE_VFORK
  )
  SET(CMAKE_REQUIRED_FLAGS ${SAVE_CMAKE_REQUIRED_FLAGS})
ENDIF()
