/* Copyright (c) 2009, 2013, Oracle and/or its affiliates. All rights reserved.
 
 This program is free software; you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation; version 2 of the License.
 
 This program is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.
 
 You should have received a copy of the GNU General Public License
 along with this program; if not, write to the Free Software
 Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#ifndef MY_CONFIG_H
#define MY_CONFIG_H
#cmakedefine DOT_FRM_VERSION @DOT_FRM_VERSION@
/* Headers we may want to use. */
#cmakedefine STDC_HEADERS 1
#cmakedefine _GNU_SOURCE 1
#cmakedefine HAVE_ALLOCA_H 1
#cmakedefine HAVE_ARPA_INET_H 1
#cmakedefine HAVE_ASM_TERMBITS_H 1
#cmakedefine HAVE_CRYPT_H 1
#cmakedefine HAVE_CURSES_H 1
#cmakedefine HAVE_BFD_H 1
#cmakedefine HAVE_NDIR_H 1
#cmakedefine HAVE_DIRENT_H 1
#cmakedefine HAVE_DLFCN_H 1
#cmakedefine HAVE_EXECINFO_H 1
#cmakedefine HAVE_FCNTL_H 1
#cmakedefine HAVE_FENV_H 1
#cmakedefine HAVE_FLOAT_H 1
#cmakedefine HAVE_FNMATCH_H 1
#cmakedefine HAVE_FPU_CONTROL_H 1
#cmakedefine HAVE_GETMNTENT 1
#cmakedefine HAVE_GETMNTENT_IN_SYS_MNTAB 1
#cmakedefine HAVE_GETMNTINFO 1
#cmakedefine HAVE_GETMNTINFO64 1
#cmakedefine HAVE_GETMNTINFO_TAKES_statvfs 1
#cmakedefine HAVE_GRP_H 1
#cmakedefine HAVE_IA64INTRIN_H 1
#cmakedefine HAVE_IEEEFP_H 1
#cmakedefine HAVE_INTTYPES_H 1
#cmakedefine HAVE_IMMINTRIN_H 1
#cmakedefine HAVE_KQUEUE 1
#cmakedefine HAVE_LIMITS_H 1
#cmakedefine HAVE_LINK_H 1
#cmakedefine HAVE_LINUX_UNISTD_H 1
#cmakedefine HAVE_LINUX_MMAN_H 1
#cmakedefine HAVE_LOCALE_H 1
#cmakedefine HAVE_MALLOC_H 1
#cmakedefine HAVE_MEMORY_H 1
#cmakedefine HAVE_NETINET_IN_H 1
#cmakedefine HAVE_PATHS_H 1
#cmakedefine HAVE_POLL_H 1
#cmakedefine HAVE_PWD_H 1
#cmakedefine HAVE_SCHED_H 1
#cmakedefine HAVE_SELECT_H 1
#cmakedefine HAVE_SOLARIS_LARGE_PAGES 1
#cmakedefine HAVE_STDDEF_H 1
#cmakedefine HAVE_STDLIB_H 1
#cmakedefine HAVE_STDARG_H 1
#cmakedefine HAVE_STRINGS_H 1
#cmakedefine HAVE_STRING_H 1
#cmakedefine HAVE_STDINT_H 1
#cmakedefine HAVE_SYNCH_H 1
#cmakedefine HAVE_SYSENT_H 1
#cmakedefine HAVE_SYS_DIR_H 1
#cmakedefine HAVE_SYS_FILE_H 1
#cmakedefine HAVE_SYS_FPU_H 1
#cmakedefine HAVE_SYS_IOCTL_H 1
#cmakedefine HAVE_SYS_MALLOC_H 1
#cmakedefine HAVE_SYS_MMAN_H 1
#cmakedefine HAVE_SYS_MNTENT_H 1
#cmakedefine HAVE_SYS_NDIR_H 1
#cmakedefine HAVE_SYS_PTE_H 1
#cmakedefine HAVE_SYS_PTEM_H 1
#cmakedefine HAVE_SYS_PRCTL_H 1
#cmakedefine HAVE_SYS_RESOURCE_H 1
#cmakedefine HAVE_SYS_SELECT_H 1
#cmakedefine HAVE_SYS_SOCKET_H 1
#cmakedefine HAVE_SYS_SOCKIO_H 1
#cmakedefine HAVE_SYS_UTSNAME_H 1
#cmakedefine HAVE_SYS_STAT_H 1
#cmakedefine HAVE_SYS_STREAM_H 1
#cmakedefine HAVE_SYS_SYSCALL_H 1
#cmakedefine HAVE_SYS_TIMEB_H 1
#cmakedefine HAVE_SYS_TIMES_H 1
#cmakedefine HAVE_SYS_TIME_H 1
#cmakedefine HAVE_SYS_TYPES_H 1
#cmakedefine HAVE_SYS_UN_H 1
#cmakedefine HAVE_SYS_VADVISE_H 1
#cmakedefine HAVE_SYS_STATVFS_H 1
#cmakedefine HAVE_UCONTEXT_H 1
#cmakedefine HAVE_TERM_H 1
#cmakedefine HAVE_TERMBITS_H 1
#cmakedefine HAVE_TERMIOS_H 1
#cmakedefine HAVE_TERMIO_H 1
#cmakedefine HAVE_TERMCAP_H 1
#cmakedefine HAVE_TIME_H 1
#cmakedefine HAVE_UNISTD_H 1
#cmakedefine HAVE_UTIME_H 1
#cmakedefine HAVE_VARARGS_H 1
#cmakedefine HAVE_SYS_UTIME_H 1
#cmakedefine HAVE_SYS_WAIT_H 1
#cmakedefine HAVE_SYS_PARAM_H 1

/* Libraries */
#cmakedefine HAVE_LIBWRAP 1
#cmakedefine HAVE_SYSTEMD 1
#cmakedefine HAVE_SYSTEMD_SD_LISTEN_FDS_WITH_NAMES 1

/* Does "struct timespec" have a "sec" and "nsec" field? */
#cmakedefine HAVE_TIMESPEC_TS_SEC 1

/* Readline */
#cmakedefine HAVE_HIST_ENTRY 1
#cmakedefine USE_LIBEDIT_INTERFACE 1
#cmakedefine USE_NEW_READLINE_INTERFACE 1

#cmakedefine FIONREAD_IN_SYS_IOCTL 1
#cmakedefine GWINSZ_IN_SYS_IOCTL 1
#cmakedefine TIOCSTAT_IN_SYS_IOCTL 1
#cmakedefine FIONREAD_IN_SYS_FILIO 1

/* Functions we may want to use. */
#cmakedefine HAVE_ACCEPT4 1
#cmakedefine HAVE_ACCESS 1
#cmakedefine HAVE_ALLOCA 1
#cmakedefine HAVE_BFILL 1
#cmakedefine HAVE_INDEX 1
#cmakedefine HAVE_CLOCK_GETTIME 1
#cmakedefine HAVE_CRYPT 1
#cmakedefine HAVE_CUSERID 1
#cmakedefine HAVE_DLADDR 1
#cmakedefine HAVE_DLERROR 1
#cmakedefine HAVE_DLOPEN 1
#cmakedefine HAVE_FCHMOD 1
#cmakedefine HAVE_FCNTL 1
#cmakedefine HAVE_FDATASYNC 1
#cmakedefine HAVE_DECL_FDATASYNC 1
#cmakedefine HAVE_FEDISABLEEXCEPT 1
#cmakedefine HAVE_FESETROUND 1
#cmakedefine HAVE_FP_EXCEPT 1
#cmakedefine HAVE_FSEEKO 1
#cmakedefine HAVE_FSYNC 1
#cmakedefine HAVE_FTIME 1
#cmakedefine HAVE_GETIFADDRS 1
#cmakedefine HAVE_GETCWD 1
#cmakedefine HAVE_GETHOSTBYADDR_R 1
#cmakedefine HAVE_GETHRTIME 1
#cmakedefine HAVE_GETPAGESIZES 1
#cmakedefine HAVE_GETPASS 1
#cmakedefine HAVE_GETPASSPHRASE 1
#cmakedefine HAVE_GETPWNAM 1
#cmakedefine HAVE_GETPWUID 1
#cmakedefine HAVE_GETRLIMIT 1
#cmakedefine HAVE_GETRUSAGE 1
#cmakedefine HAVE_GETTIMEOFDAY 1
#cmakedefine HAVE_GETWD 1
#cmakedefine HAVE_GMTIME_R 1
#cmakedefine gmtime_r @gmtime_r@
#cmakedefine HAVE_IN_ADDR_T 1
#cmakedefine HAVE_INITGROUPS 1
#cmakedefine HAVE_LDIV 1
#cmakedefine HAVE_LRAND48 1
#cmakedefine HAVE_LOCALTIME_R 1
#cmakedefine HAVE_LSTAT 1
/* #cmakedefine HAVE_MLOCK 1 see Bug#54662 */
#cmakedefine HAVE_NL_LANGINFO 1
#cmakedefine HAVE_MADVISE 1
#cmakedefine HAVE_DECL_MADVISE 1
#cmakedefine HAVE_DECL_MHA_MAPSIZE_VA 1
#cmakedefine HAVE_MALLINFO 1
#cmakedefine HAVE_MALLINFO2 1
#cmakedefine HAVE_MALLOC_ZONE 1
#cmakedefine HAVE_MEMCPY 1
#cmakedefine HAVE_MEMMOVE 1
#cmakedefine HAVE_MKSTEMP 1
#cmakedefine HAVE_MKOSTEMP 1
#cmakedefine HAVE_MLOCKALL 1
#cmakedefine HAVE_MMAP 1
#cmakedefine HAVE_MMAP64 1
#cmakedefine HAVE_MPROTECT 1
#cmakedefine HAVE_PERROR 1
#cmakedefine HAVE_POLL 1
#cmakedefine HAVE_POSIX_FALLOCATE 1
#cmakedefine HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE 1
#cmakedefine HAVE_PREAD 1
#cmakedefine HAVE_READ_REAL_TIME 1
#cmakedefine HAVE_PTHREAD_ATTR_CREATE 1
#cmakedefine HAVE_PTHREAD_ATTR_GETGUARDSIZE 1
#cmakedefine HAVE_PTHREAD_ATTR_GETSTACKSIZE 1
#cmakedefine HAVE_PTHREAD_ATTR_SETSCOPE 1
#cmakedefine HAVE_PTHREAD_ATTR_SETSTACKSIZE 1
#cmakedefine HAVE_PTHREAD_GETATTR_NP 1
#cmakedefine HAVE_PTHREAD_CONDATTR_CREATE 1
#cmakedefine HAVE_PTHREAD_GETAFFINITY_NP 1
#cmakedefine HAVE_PTHREAD_KEY_DELETE 1
#cmakedefine HAVE_PTHREAD_KILL 1
#cmakedefine HAVE_PTHREAD_RWLOCK_RDLOCK 1
#cmakedefine HAVE_PTHREAD_SIGMASK 1
#cmakedefine HAVE_PTHREAD_YIELD_NP 1
#cmakedefine HAVE_PTHREAD_YIELD_ZERO_ARG 1
#cmakedefine PTHREAD_ONCE_INITIALIZER @PTHREAD_ONCE_INITIALIZER@
#cmakedefine HAVE_PUTENV 1
#cmakedefine HAVE_READDIR_R 1
#cmakedefine HAVE_READLINK 1
#cmakedefine HAVE_REALPATH 1
#cmakedefine HAVE_RENAME 1
#cmakedefine HAVE_RWLOCK_INIT 1
#cmakedefine HAVE_SCHED_YIELD 1
#cmakedefine HAVE_SELECT 1
#cmakedefine HAVE_SETENV 1
#cmakedefine HAVE_SETLOCALE 1
#cmakedefine HAVE_SETMNTENT 1
#cmakedefine HAVE_SETUPTERM 1
#cmakedefine HAVE_SIGSET 1
#cmakedefine HAVE_SIGACTION 1
#cmakedefine HAVE_SIGTHREADMASK 1
#cmakedefine HAVE_SIGWAIT 1
#cmakedefine HAVE_SIGWAITINFO 1
#cmakedefine HAVE_SLEEP 1
#cmakedefine HAVE_STPCPY 1
#cmakedefine HAVE_STRERROR 1
#cmakedefine HAVE_STRCOLL 1
#cmakedefine HAVE_STRNLEN 1
#cmakedefine HAVE_STRPBRK 1
#cmakedefine HAVE_STRTOK_R 1
#cmakedefine HAVE_STRTOLL 1
#cmakedefine HAVE_STRTOUL 1
#cmakedefine HAVE_STRTOULL 1
#cmakedefine HAVE_TELL 1
#cmakedefine HAVE_THR_YIELD 1
#cmakedefine HAVE_TIME 1
#cmakedefine HAVE_TIMES 1
#cmakedefine HAVE_VIDATTR 1
#define HAVE_VIO_READ_BUFF 1
#cmakedefine HAVE_VASPRINTF 1
#cmakedefine HAVE_VSNPRINTF 1
#cmakedefine HAVE_FTRUNCATE 1
#cmakedefine HAVE_TZNAME 1
/* Symbols we may use */
#cmakedefine HAVE_SYS_ERRLIST 1
/* used by stacktrace functions */
#cmakedefine HAVE_BACKTRACE 1
#cmakedefine HAVE_BACKTRACE_SYMBOLS 1
#cmakedefine HAVE_BACKTRACE_SYMBOLS_FD 1
#cmakedefine HAVE_PRINTSTACK 1
#cmakedefine HAVE_IPV6 1
#cmakedefine ss_family @ss_family@
#cmakedefine HAVE_SOCKADDR_IN_SIN_LEN 1
#cmakedefine HAVE_SOCKADDR_IN6_SIN6_LEN 1
#cmakedefine STRUCT_TIMESPEC_HAS_TV_SEC 1
#cmakedefine STRUCT_TIMESPEC_HAS_TV_NSEC 1
#cmakedefine STRUCT_TM_HAS_TM_GMTOFF 1

/* this means that valgrind headers and macros are available */
#cmakedefine HAVE_VALGRIND_MEMCHECK_H 1

/* this means WITH_VALGRIND - we change some code paths for valgrind */
#cmakedefine HAVE_valgrind 1

/* Types we may use */
#ifdef __APPLE__
  /*
    Special handling required for OSX to support universal binaries that 
    mix 32 and 64 bit architectures.
  */
  #if(__LP64__)
    #define SIZEOF_LONG 8
  #else
    #define SIZEOF_LONG 4
  #endif
  #define SIZEOF_VOIDP   SIZEOF_LONG
  #define SIZEOF_CHARP   SIZEOF_LONG
  #define SIZEOF_SIZE_T  SIZEOF_LONG
#else
/* No indentation, to fetch the lines from verification scripts */
#cmakedefine SIZEOF_LONG   @SIZEOF_LONG@
#cmakedefine SIZEOF_VOIDP  @SIZEOF_VOIDP@
#cmakedefine SIZEOF_CHARP  @SIZEOF_CHARP@
#cmakedefine SIZEOF_SIZE_T @SIZEOF_CHARP@
#endif

#define HAVE_LONG 1
#define HAVE_CHARP 1
#cmakedefine SIZEOF_INT @SIZEOF_INT@
#define HAVE_INT 1
#cmakedefine SIZEOF_LONG_LONG @SIZEOF_LONG_LONG@
#cmakedefine HAVE_LONG_LONG 1
#cmakedefine SIZEOF_OFF_T @SIZEOF_OFF_T@
#cmakedefine HAVE_OFF_T 1
#cmakedefine SIZEOF_UCHAR @SIZEOF_UCHAR@
#cmakedefine HAVE_UCHAR 1
#cmakedefine SIZEOF_UINT @SIZEOF_UINT@
#cmakedefine HAVE_UINT 1
#cmakedefine SIZEOF_ULONG @SIZEOF_ULONG@
#cmakedefine HAVE_ULONG 1
#cmakedefine SIZEOF_INT8 @SIZEOF_INT8@
#cmakedefine HAVE_INT8 1
#cmakedefine SIZEOF_UINT8 @SIZEOF_UINT8@
#cmakedefine HAVE_UINT8 1
#cmakedefine SIZEOF_INT16 @SIZEOF_INT16@
#cmakedefine HAVE_INT16 1
#cmakedefine SIZEOF_UINT16 @SIZEOF_UINT16@
#cmakedefine HAVE_UINT16 1
#cmakedefine SIZEOF_INT32 @SIZEOF_INT32@
#cmakedefine HAVE_INT32 1
#cmakedefine SIZEOF_UINT32 @SIZEOF_UINT32@
#cmakedefine HAVE_UINT32 1
#cmakedefine SIZEOF_INT64 @SIZEOF_INT64@
#cmakedefine HAVE_INT64 1
#cmakedefine SIZEOF_UINT64 @SIZEOF_UINT64@
#cmakedefine HAVE_UINT64 1

#cmakedefine SOCKET_SIZE_TYPE @SOCKET_SIZE_TYPE@

#cmakedefine HAVE_MBSTATE_T 1

#cmakedefine MAX_INDEXES @MAX_INDEXES@

#cmakedefine QSORT_TYPE_IS_VOID 1
#cmakedefine RETQSORTTYPE @RETQSORTTYPE@

#cmakedefine RETSIGTYPE @RETSIGTYPE@
#cmakedefine VOID_SIGHANDLER 1
#cmakedefine HAVE_SIGHANDLER_T 1
#define STRUCT_RLIMIT struct rlimit

#ifdef __APPLE__
  #if __BIG_ENDIAN
    #define WORDS_BIGENDIAN 1
  #endif
#else
#cmakedefine WORDS_BIGENDIAN 1 
#endif

/* Define to `__inline__' or `__inline' if that's what the C compiler calls
   it, or to nothing if 'inline' is not supported under any name.  */
#cmakedefine C_HAS_inline 1
#if !(C_HAS_inline)
#ifndef __cplusplus
# define inline @C_INLINE@
#endif
#endif


#cmakedefine TARGET_OS_LINUX 1

#cmakedefine HAVE_WCTYPE_H 1
#cmakedefine HAVE_WCHAR_H 1
#cmakedefine HAVE_LANGINFO_H 1
#cmakedefine HAVE_MBRLEN 1
#cmakedefine HAVE_MBSRTOWCS 1
#cmakedefine HAVE_MBRTOWC 1
#cmakedefine HAVE_WCWIDTH 1
#cmakedefine HAVE_ISWLOWER 1
#cmakedefine HAVE_ISWUPPER 1
#cmakedefine HAVE_TOWLOWER 1
#cmakedefine HAVE_TOWUPPER 1
#cmakedefine HAVE_ISWCTYPE 1
#cmakedefine HAVE_WCHAR_T 1


#cmakedefine HAVE_STRCASECMP 1
#cmakedefine HAVE_TCGETATTR 1

#cmakedefine HAVE_WEAK_SYMBOL 1
#cmakedefine HAVE_ABI_CXA_DEMANGLE 1
#cmakedefine HAVE_ATTRIBUTE_CLEANUP 1

#cmakedefine HAVE_POSIX_SIGNALS 1
#cmakedefine HAVE_BSD_SIGNALS 1

#cmakedefine HAVE_SVR3_SIGNALS 1
#cmakedefine HAVE_V7_SIGNALS 1
#cmakedefine HAVE_ERR_remove_thread_state 1
#cmakedefine HAVE_X509_check_host 1

#cmakedefine HAVE_SOLARIS_STYLE_GETHOST 1

#cmakedefine HAVE_GCC_C11_ATOMICS 1
#cmakedefine HAVE_SOLARIS_ATOMIC 1
#cmakedefine NO_FCNTL_NONBLOCK 1

#cmakedefine _LARGE_FILES 1
#cmakedefine _LARGEFILE_SOURCE 1
#cmakedefine _LARGEFILE64_SOURCE 1

#cmakedefine TIME_WITH_SYS_TIME 1

#cmakedefine STACK_DIRECTION @STACK_DIRECTION@

#define SYSTEM_TYPE "@SYSTEM_TYPE@"
#define MACHINE_TYPE "@CMAKE_SYSTEM_PROCESSOR@"
#define DEFAULT_MACHINE "@DEFAULT_MACHINE@"
#cmakedefine HAVE_DTRACE 1

#cmakedefine SIGNAL_WITH_VIO_CLOSE 1

/* Windows stuff, mostly functions, that have Posix analogs but named differently */
#ifdef _WIN32
#define S_IROTH _S_IREAD
#define S_IFIFO _S_IFIFO
#define SIGQUIT SIGTERM
#define SIGPIPE SIGINT
#define sigset_t int
#define mode_t int
#define popen _popen
#define pclose _pclose
#define ssize_t SSIZE_T
#define strcasecmp _stricmp
#define strncasecmp _strnicmp
#define strtok_r strtok_s
#define tzname _tzname
#define P_tmpdir "C:\\TEMP"
#define setenv(a,b,c) _putenv_s(a,b)

#define HAVE_SETENV
#define NOMINMAX 1
#define PSAPI_VERSION 2     /* for GetProcessMemoryInfo() */
#endif /* _WIN32 */

/*
  MySQL features
*/
#define LOCAL_INFILE_MODE_OFF  0
#define LOCAL_INFILE_MODE_ON   1
#define LOCAL_INFILE_MODE_AUTO 2
#define ENABLED_LOCAL_INFILE LOCAL_INFILE_MODE_@ENABLED_LOCAL_INFILE@

#cmakedefine ENABLED_PROFILING 1
#cmakedefine EXTRA_DEBUG 1
#cmakedefine USE_SYMDIR 1

/* Character sets and collations */
#cmakedefine MYSQL_DEFAULT_CHARSET_NAME "@MYSQL_DEFAULT_CHARSET_NAME@"
#cmakedefine MYSQL_DEFAULT_COLLATION_NAME "@MYSQL_DEFAULT_COLLATION_NAME@"

#cmakedefine USE_MB
#cmakedefine USE_MB_IDENT

/* This should mean case insensitive file system */
#cmakedefine FN_NO_CASE_SENSE 1

/* Whether an anonymous private mapping is unaccessible after
madvise(MADV_DONTNEED) or madvise(MADV_FREE) or similar has been invoked;
this is the case with Microsoft Windows VirtualFree(MEM_DECOMMIT) */
#cmakedefine HAVE_UNACCESSIBLE_AFTER_MEM_DECOMMIT 1

#cmakedefine HAVE_CHARSET_armscii8 1
#cmakedefine HAVE_CHARSET_ascii 1
#cmakedefine HAVE_CHARSET_big5 1
#cmakedefine HAVE_CHARSET_cp1250 1
#cmakedefine HAVE_CHARSET_cp1251 1
#cmakedefine HAVE_CHARSET_cp1256 1
#cmakedefine HAVE_CHARSET_cp1257 1
#cmakedefine HAVE_CHARSET_cp850 1
#cmakedefine HAVE_CHARSET_cp852 1 
#cmakedefine HAVE_CHARSET_cp866 1
#cmakedefine HAVE_CHARSET_cp932 1
#cmakedefine HAVE_CHARSET_dec8 1
#cmakedefine HAVE_CHARSET_eucjpms 1
#cmakedefine HAVE_CHARSET_euckr 1
#cmakedefine HAVE_CHARSET_gb2312 1
#cmakedefine HAVE_CHARSET_gbk 1
#cmakedefine HAVE_CHARSET_geostd8 1
#cmakedefine HAVE_CHARSET_greek 1
#cmakedefine HAVE_CHARSET_hebrew 1
#cmakedefine HAVE_CHARSET_hp8 1
#cmakedefine HAVE_CHARSET_keybcs2 1
#cmakedefine HAVE_CHARSET_koi8r 1
#cmakedefine HAVE_CHARSET_koi8u 1
#cmakedefine HAVE_CHARSET_latin1 1
#cmakedefine HAVE_CHARSET_latin2 1
#cmakedefine HAVE_CHARSET_latin5 1
#cmakedefine HAVE_CHARSET_latin7 1
#cmakedefine HAVE_CHARSET_macce 1
#cmakedefine HAVE_CHARSET_macroman 1
#cmakedefine HAVE_CHARSET_sjis 1
#cmakedefine HAVE_CHARSET_swe7 1
#cmakedefine HAVE_CHARSET_tis620 1
#cmakedefine HAVE_CHARSET_ucs2 1
#cmakedefine HAVE_CHARSET_ujis 1
#cmakedefine HAVE_CHARSET_utf8mb4 1
#cmakedefine HAVE_CHARSET_utf8mb3 1
#cmakedefine HAVE_CHARSET_utf16 1
#cmakedefine HAVE_CHARSET_utf32 1
#cmakedefine HAVE_UCA_COLLATIONS 1
#cmakedefine HAVE_COMPRESS 1
#cmakedefine HAVE_EncryptAes128Ctr 1
#cmakedefine HAVE_EncryptAes128Gcm 1
#cmakedefine HAVE_des 1
#cmakedefine HAVE_hkdf 1

/*
  Important storage engines (those that really need define 
  WITH_<ENGINE>_STORAGE_ENGINE for the whole server)
*/
#cmakedefine WITH_INNOBASE_STORAGE_ENGINE 1
#cmakedefine WITH_PARTITION_STORAGE_ENGINE 1
#cmakedefine WITH_PERFSCHEMA_STORAGE_ENGINE 1
#cmakedefine WITH_ARIA_STORAGE_ENGINE 1
#cmakedefine USE_ARIA_FOR_TMP_TABLES 1

#cmakedefine DEFAULT_MYSQL_HOME "@DEFAULT_MYSQL_HOME@"
#cmakedefine SHAREDIR "@SHAREDIR@"
#cmakedefine DEFAULT_BASEDIR "@DEFAULT_BASEDIR@"
#cmakedefine MYSQL_DATADIR "@MYSQL_DATADIR@"
#cmakedefine DEFAULT_CHARSET_HOME "@DEFAULT_CHARSET_HOME@"
#cmakedefine PLUGINDIR "@PLUGINDIR@"
#cmakedefine DEFAULT_SYSCONFDIR "@DEFAULT_SYSCONFDIR@"
#cmakedefine DEFAULT_TMPDIR @DEFAULT_TMPDIR@

#cmakedefine SO_EXT "@CMAKE_SHARED_MODULE_SUFFIX@"

#define MYSQL_VERSION_MAJOR @MAJOR_VERSION@
#define MYSQL_VERSION_MINOR @MINOR_VERSION@
#define MYSQL_VERSION_PATCH @PATCH_VERSION@
#define MYSQL_VERSION_EXTRA "@EXTRA_VERSION@"

#define PACKAGE "mysql"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME "MySQL Server"
#define PACKAGE_STRING "MySQL Server @VERSION@"
#define PACKAGE_TARNAME "mysql"
#define PACKAGE_VERSION "@VERSION@"
#define VERSION "@VERSION@"
#define PROTOCOL_VERSION 10
#define PCRE2_CODE_UNIT_WIDTH 8

#define MALLOC_LIBRARY "@MALLOC_LIBRARY@"

/* time_t related defines */

#cmakedefine SIZEOF_TIME_T @SIZEOF_TIME_T@
#cmakedefine TIME_T_UNSIGNED @TIME_T_UNSIGNED@

#ifndef EMBEDDED_LIBRARY
#cmakedefine WSREP_INTERFACE_VERSION "@WSREP_INTERFACE_VERSION@"
#cmakedefine WITH_WSREP 1
#cmakedefine WSREP_PROC_INFO 1
#endif

#if !defined(__STDC_FORMAT_MACROS)
#define __STDC_FORMAT_MACROS
#endif  // !defined(__STDC_FORMAT_MACROS)

#endif

#cmakedefine HAVE_VFORK 1
