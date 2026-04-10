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
#define DOT_FRM_VERSION 6
/* Headers we may want to use. */
#define STDC_HEADERS 1
/* #undef _GNU_SOURCE */
#define HAVE_ALLOCA_H 1
#define HAVE_ARPA_INET_H 1
/* #undef HAVE_ASM_TERMBITS_H */
/* #undef HAVE_CRYPT_H */
#define HAVE_CURSES_H 1
/* #undef HAVE_BFD_H */
/* #undef HAVE_NDIR_H */
#define HAVE_DIRENT_H 1
#define HAVE_DLFCN_H 1
#define HAVE_EXECINFO_H 1
#define HAVE_FCNTL_H 1
#define HAVE_FENV_H 1
#define HAVE_FLOAT_H 1
#define HAVE_FNMATCH_H 1
/* #undef HAVE_FPU_CONTROL_H */
/* #undef HAVE_GETMNTENT */
/* #undef HAVE_GETMNTENT_IN_SYS_MNTAB */
#define HAVE_GETMNTINFO 1
/* #undef HAVE_GETMNTINFO64 */
/* #undef HAVE_GETMNTINFO_TAKES_statvfs */
#define HAVE_GRP_H 1
/* #undef HAVE_IA64INTRIN_H */
/* #undef HAVE_IEEEFP_H */
#define HAVE_INTTYPES_H 1
/* #undef HAVE_IMMINTRIN_H */
#define HAVE_KQUEUE 1
#define HAVE_LIMITS_H 1
/* #undef HAVE_LINK_H */
/* #undef HAVE_LINUX_UNISTD_H */
/* #undef HAVE_LINUX_MMAN_H */
#define HAVE_LOCALE_H 1
/* #undef HAVE_MALLOC_H */
#define HAVE_MEMORY_H 1
#define HAVE_NETINET_IN_H 1
#define HAVE_PATHS_H 1
#define HAVE_POLL_H 1
#define HAVE_PWD_H 1
#define HAVE_SCHED_H 1
/* #undef HAVE_SELECT_H */
/* #undef HAVE_SOLARIS_LARGE_PAGES */
#define HAVE_STDDEF_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STDARG_H 1
#define HAVE_STRINGS_H 1
#define HAVE_STRING_H 1
#define HAVE_STDINT_H 1
/* #undef HAVE_SYNCH_H */
/* #undef HAVE_SYSENT_H */
#define HAVE_SYS_DIR_H 1
#define HAVE_SYS_FILE_H 1
/* #undef HAVE_SYS_FPU_H */
#define HAVE_SYS_IOCTL_H 1
#define HAVE_SYS_MALLOC_H 1
#define HAVE_SYS_MMAN_H 1
/* #undef HAVE_SYS_MNTENT_H */
/* #undef HAVE_SYS_NDIR_H */
/* #undef HAVE_SYS_PTE_H */
/* #undef HAVE_SYS_PTEM_H */
/* #undef HAVE_SYS_PRCTL_H */
#define HAVE_SYS_RESOURCE_H 1
#define HAVE_SYS_SELECT_H 1
#define HAVE_SYS_SOCKET_H 1
#define HAVE_SYS_SOCKIO_H 1
#define HAVE_SYS_UTSNAME_H 1
#define HAVE_SYS_STAT_H 1
/* #undef HAVE_SYS_STREAM_H */
#define HAVE_SYS_SYSCALL_H 1
/* #undef HAVE_SYS_TIMEB_H */
#define HAVE_SYS_TIMES_H 1
#define HAVE_SYS_TIME_H 1
#define HAVE_SYS_TYPES_H 1
#define HAVE_SYS_UN_H 1
#define HAVE_SYS_VADVISE_H 1
#define HAVE_SYS_STATVFS_H 1
#define HAVE_UCONTEXT_H 1
#define HAVE_TERM_H 1
/* #undef HAVE_TERMBITS_H */
#define HAVE_TERMIOS_H 1
/* #undef HAVE_TERMIO_H */
#define HAVE_TERMCAP_H 1
#define HAVE_TIME_H 1
#define HAVE_UNISTD_H 1
#define HAVE_UTIME_H 1
/* #undef HAVE_VARARGS_H */
/* #undef HAVE_SYS_UTIME_H */
#define HAVE_SYS_WAIT_H 1
#define HAVE_SYS_PARAM_H 1

/* Libraries */
/* #undef HAVE_LIBWRAP */
/* #undef HAVE_SYSTEMD */
/* #undef HAVE_SYSTEMD_SD_LISTEN_FDS_WITH_NAMES */

/* Does "struct timespec" have a "sec" and "nsec" field? */
/* #undef HAVE_TIMESPEC_TS_SEC */

/* Readline */
#define HAVE_HIST_ENTRY 1
#define USE_LIBEDIT_INTERFACE 1
/* #undef USE_NEW_READLINE_INTERFACE */

#define FIONREAD_IN_SYS_IOCTL 1
#define GWINSZ_IN_SYS_IOCTL 1
#define TIOCSTAT_IN_SYS_IOCTL 1
#define FIONREAD_IN_SYS_FILIO 1

/* Functions we may want to use. */
/* #undef HAVE_ACCEPT4 */
#define HAVE_ACCESS 1
#define HAVE_ALLOCA 1
/* #undef HAVE_BFILL */
#define HAVE_INDEX 1
#define HAVE_CLOCK_GETTIME 1
#define HAVE_CRYPT 1
/* #undef HAVE_CUSERID */
#define HAVE_DLADDR 1
#define HAVE_DLERROR 1
#define HAVE_DLOPEN 1
#define HAVE_FCHMOD 1
#define HAVE_FCNTL 1
#define HAVE_FDATASYNC 1
/* #undef HAVE_DECL_FDATASYNC */
/* #undef HAVE_FEDISABLEEXCEPT */
#define HAVE_FESETROUND 1
/* #undef HAVE_FP_EXCEPT */
#define HAVE_FSEEKO 1
#define HAVE_FSYNC 1
#define HAVE_FTIME 1
#define HAVE_GETIFADDRS 1
#define HAVE_GETCWD 1
/* #undef HAVE_GETHOSTBYADDR_R */
/* #undef HAVE_GETHRTIME */
/* #undef HAVE_GETPAGESIZES */
#define HAVE_GETPASS 1
/* #undef HAVE_GETPASSPHRASE */
#define HAVE_GETPWNAM 1
#define HAVE_GETPWUID 1
#define HAVE_GETRLIMIT 1
#define HAVE_GETRUSAGE 1
#define HAVE_GETTIMEOFDAY 1
#define HAVE_GETWD 1
#define HAVE_GMTIME_R 1
/* #undef gmtime_r */
#define HAVE_IN_ADDR_T 1
#define HAVE_INITGROUPS 1
#define HAVE_LDIV 1
#define HAVE_LRAND48 1
#define HAVE_LOCALTIME_R 1
#define HAVE_LSTAT 1
/* #define HAVE_MLOCK 1 see Bug#54662 */
#define HAVE_NL_LANGINFO 1
#define HAVE_MADVISE 1
#define HAVE_DECL_MADVISE 1
/* #undef HAVE_DECL_MHA_MAPSIZE_VA */
/* #undef HAVE_MALLINFO */
/* #undef HAVE_MALLINFO2 */
#define HAVE_MALLOC_ZONE 1
#define HAVE_MEMCPY 1
#define HAVE_MEMMOVE 1
#define HAVE_MKSTEMP 1
#define HAVE_MKOSTEMP 1
#define HAVE_MLOCKALL 1
#define HAVE_MMAP 1
/* #undef HAVE_MMAP64 */
#define HAVE_MPROTECT 1
#define HAVE_PERROR 1
#define HAVE_POLL 1
/* #undef HAVE_POSIX_FALLOCATE */
/* #undef HAVE_FALLOC_PUNCH_HOLE_AND_KEEP_SIZE */
#define HAVE_PREAD 1
/* #undef HAVE_READ_REAL_TIME */
/* #undef HAVE_PTHREAD_ATTR_CREATE */
#define HAVE_PTHREAD_ATTR_GETGUARDSIZE 1
#define HAVE_PTHREAD_ATTR_GETSTACKSIZE 1
#define HAVE_PTHREAD_ATTR_SETSCOPE 1
#define HAVE_PTHREAD_ATTR_SETSTACKSIZE 1
/* #undef HAVE_PTHREAD_GETATTR_NP */
/* #undef HAVE_PTHREAD_CONDATTR_CREATE */
/* #undef HAVE_PTHREAD_GETAFFINITY_NP */
/* #undef HAVE_PTHREAD_KEY_DELETE */
/* #undef HAVE_PTHREAD_KILL */
#define HAVE_PTHREAD_RWLOCK_RDLOCK 1
#define HAVE_PTHREAD_SIGMASK 1
#define HAVE_PTHREAD_YIELD_NP 1
/* #undef HAVE_PTHREAD_YIELD_ZERO_ARG */
#define PTHREAD_ONCE_INITIALIZER PTHREAD_ONCE_INIT
#define HAVE_PUTENV 1
#define HAVE_READDIR_R 1
#define HAVE_READLINK 1
#define HAVE_REALPATH 1
#define HAVE_RENAME 1
/* #undef HAVE_RWLOCK_INIT */
#define HAVE_SCHED_YIELD 1
/* #undef HAVE_SELECT */
#define HAVE_SETENV 1
#define HAVE_SETLOCALE 1
/* #undef HAVE_SETMNTENT */
#define HAVE_SETUPTERM 1
#define HAVE_SIGSET 1
#define HAVE_SIGACTION 1
/* #undef HAVE_SIGTHREADMASK */
#define HAVE_SIGWAIT 1
/* #undef HAVE_SIGWAITINFO */
#define HAVE_SLEEP 1
#define HAVE_STPCPY 1
#define HAVE_STRERROR 1
#define HAVE_STRCOLL 1
#define HAVE_STRNLEN 1
#define HAVE_STRPBRK 1
#define HAVE_STRTOK_R 1
#define HAVE_STRTOLL 1
#define HAVE_STRTOUL 1
#define HAVE_STRTOULL 1
/* #undef HAVE_TELL */
/* #undef HAVE_THR_YIELD */
#define HAVE_TIME 1
#define HAVE_TIMES 1
#define HAVE_VIDATTR 1
#define HAVE_VIO_READ_BUFF 1
#define HAVE_VASPRINTF 1
#define HAVE_VSNPRINTF 1
#define HAVE_FTRUNCATE 1
#define HAVE_TZNAME 1
/* Symbols we may use */
/* #undef HAVE_SYS_ERRLIST */
/* used by stacktrace functions */
#define HAVE_BACKTRACE 1
#define HAVE_BACKTRACE_SYMBOLS 1
#define HAVE_BACKTRACE_SYMBOLS_FD 1
/* #undef HAVE_PRINTSTACK */
#define HAVE_IPV6 1
/* #undef ss_family */
#define HAVE_SOCKADDR_IN_SIN_LEN 1
#define HAVE_SOCKADDR_IN6_SIN6_LEN 1
#define STRUCT_TIMESPEC_HAS_TV_SEC 1
#define STRUCT_TIMESPEC_HAS_TV_NSEC 1
#define STRUCT_TM_HAS_TM_GMTOFF 1

/* this means that valgrind headers and macros are available */
/* #undef HAVE_VALGRIND_MEMCHECK_H */

/* this means WITH_VALGRIND - we change some code paths for valgrind */
/* #undef HAVE_valgrind */

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
#define SIZEOF_LONG   8
#define SIZEOF_VOIDP  8
#define SIZEOF_CHARP  8
#define SIZEOF_SIZE_T 8
#endif

#define HAVE_LONG 1
#define HAVE_CHARP 1
#define SIZEOF_INT 4
#define HAVE_INT 1
#define SIZEOF_LONG_LONG 8
#define HAVE_LONG_LONG 1
#define SIZEOF_OFF_T 8
#define HAVE_OFF_T 1
/* #undef SIZEOF_UCHAR */
/* #undef HAVE_UCHAR */
#define SIZEOF_UINT 4
#define HAVE_UINT 1
/* #undef SIZEOF_ULONG */
/* #undef HAVE_ULONG */
/* #undef SIZEOF_INT8 */
/* #undef HAVE_INT8 */
/* #undef SIZEOF_UINT8 */
/* #undef HAVE_UINT8 */
/* #undef SIZEOF_INT16 */
/* #undef HAVE_INT16 */
/* #undef SIZEOF_UINT16 */
/* #undef HAVE_UINT16 */
/* #undef SIZEOF_INT32 */
/* #undef HAVE_INT32 */
/* #undef SIZEOF_UINT32 */
/* #undef HAVE_UINT32 */
/* #undef SIZEOF_INT64 */
/* #undef HAVE_INT64 */
/* #undef SIZEOF_UINT64 */
/* #undef HAVE_UINT64 */

#define SOCKET_SIZE_TYPE socklen_t

#define HAVE_MBSTATE_T 1

#define MAX_INDEXES 64

#define QSORT_TYPE_IS_VOID 1
#define RETQSORTTYPE void

#define RETSIGTYPE void
#define VOID_SIGHANDLER 1
/* #undef HAVE_SIGHANDLER_T */
#define STRUCT_RLIMIT struct rlimit

#ifdef __APPLE__
  #if __BIG_ENDIAN
    #define WORDS_BIGENDIAN 1
  #endif
#else
/* #undef WORDS_BIGENDIAN */
#endif

/* Define to `__inline__' or `__inline' if that's what the C compiler calls
   it, or to nothing if 'inline' is not supported under any name.  */
#define C_HAS_inline 1
#if !(C_HAS_inline)
#ifndef __cplusplus
# define inline 
#endif
#endif


/* #undef TARGET_OS_LINUX */

#define HAVE_WCTYPE_H 1
#define HAVE_WCHAR_H 1
#define HAVE_LANGINFO_H 1
#define HAVE_MBRLEN 1
#define HAVE_MBSRTOWCS 1
#define HAVE_MBRTOWC 1
#define HAVE_WCWIDTH 1
#define HAVE_ISWLOWER 1
#define HAVE_ISWUPPER 1
#define HAVE_TOWLOWER 1
#define HAVE_TOWUPPER 1
#define HAVE_ISWCTYPE 1
#define HAVE_WCHAR_T 1


#define HAVE_STRCASECMP 1
#define HAVE_TCGETATTR 1

#define HAVE_WEAK_SYMBOL 1
#define HAVE_ABI_CXA_DEMANGLE 1
#define HAVE_ATTRIBUTE_CLEANUP 1

#define HAVE_POSIX_SIGNALS 1
/* #undef HAVE_BSD_SIGNALS */

/* #undef HAVE_SVR3_SIGNALS */
/* #undef HAVE_V7_SIGNALS */
#define HAVE_ERR_remove_thread_state 1
/* #undef HAVE_X509_check_host */

/* #undef HAVE_SOLARIS_STYLE_GETHOST */

#define HAVE_GCC_C11_ATOMICS 1
/* #undef HAVE_SOLARIS_ATOMIC */
/* #undef NO_FCNTL_NONBLOCK */

/* #undef _LARGE_FILES */
#define _LARGEFILE_SOURCE 1
/* #undef _LARGEFILE64_SOURCE */

#define TIME_WITH_SYS_TIME 1

#define STACK_DIRECTION -1

#define SYSTEM_TYPE "osx10.20"
#define MACHINE_TYPE "arm64"
#define DEFAULT_MACHINE "arm64"
#define HAVE_DTRACE 1

#define SIGNAL_WITH_VIO_CLOSE 1

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
#define ENABLED_LOCAL_INFILE LOCAL_INFILE_MODE_AUTO

#define ENABLED_PROFILING 1
/* #undef EXTRA_DEBUG */
/* #undef USE_SYMDIR */

/* Character sets and collations */
#define MYSQL_DEFAULT_CHARSET_NAME "utf8mb4"
#define MYSQL_DEFAULT_COLLATION_NAME "utf8mb4_uca1400_ai_ci"

#define USE_MB
#define USE_MB_IDENT

/* This should mean case insensitive file system */
/* #undef FN_NO_CASE_SENSE */

#define HAVE_CHARSET_armscii8 1
#define HAVE_CHARSET_ascii 1
#define HAVE_CHARSET_big5 1
#define HAVE_CHARSET_cp1250 1
#define HAVE_CHARSET_cp1251 1
#define HAVE_CHARSET_cp1256 1
#define HAVE_CHARSET_cp1257 1
#define HAVE_CHARSET_cp850 1
#define HAVE_CHARSET_cp852 1 
#define HAVE_CHARSET_cp866 1
#define HAVE_CHARSET_cp932 1
#define HAVE_CHARSET_dec8 1
#define HAVE_CHARSET_eucjpms 1
#define HAVE_CHARSET_euckr 1
#define HAVE_CHARSET_gb2312 1
#define HAVE_CHARSET_gbk 1
#define HAVE_CHARSET_geostd8 1
#define HAVE_CHARSET_greek 1
#define HAVE_CHARSET_hebrew 1
#define HAVE_CHARSET_hp8 1
#define HAVE_CHARSET_keybcs2 1
#define HAVE_CHARSET_koi8r 1
#define HAVE_CHARSET_koi8u 1
#define HAVE_CHARSET_latin1 1
#define HAVE_CHARSET_latin2 1
#define HAVE_CHARSET_latin5 1
#define HAVE_CHARSET_latin7 1
#define HAVE_CHARSET_macce 1
#define HAVE_CHARSET_macroman 1
#define HAVE_CHARSET_sjis 1
#define HAVE_CHARSET_swe7 1
#define HAVE_CHARSET_tis620 1
#define HAVE_CHARSET_ucs2 1
#define HAVE_CHARSET_ujis 1
#define HAVE_CHARSET_utf8mb4 1
#define HAVE_CHARSET_utf8mb3 1
#define HAVE_CHARSET_utf16 1
#define HAVE_CHARSET_utf32 1
#define HAVE_UCA_COLLATIONS 1
#define HAVE_COMPRESS 1
#define HAVE_EncryptAes128Ctr 1
/* #undef HAVE_EncryptAes128Gcm */
#define HAVE_hkdf 1

/*
  Important storage engines (those that really need define 
  WITH_<ENGINE>_STORAGE_ENGINE for the whole server)
*/
#define WITH_INNOBASE_STORAGE_ENGINE 1
#define WITH_PARTITION_STORAGE_ENGINE 1
#define WITH_PERFSCHEMA_STORAGE_ENGINE 1
#define WITH_ARIA_STORAGE_ENGINE 1
#define USE_ARIA_FOR_TMP_TABLES 1

#define DEFAULT_MYSQL_HOME "/usr/local/mysql"
#define SHAREDIR "/usr/local/mysql/share"
#define DEFAULT_BASEDIR "/usr/local/mysql"
#define MYSQL_DATADIR "/usr/local/mysql/data"
#define DEFAULT_CHARSET_HOME "/usr/local/mysql"
#define PLUGINDIR "/usr/local/mysql/lib/plugin"
/* #undef DEFAULT_SYSCONFDIR */
#define DEFAULT_TMPDIR P_tmpdir

/* #undef SO_EXT */

#define MYSQL_VERSION_MAJOR 13
#define MYSQL_VERSION_MINOR 0
#define MYSQL_VERSION_PATCH 1
#define MYSQL_VERSION_EXTRA ""

#define PACKAGE "mysql"
#define PACKAGE_BUGREPORT ""
#define PACKAGE_NAME "MySQL Server"
#define PACKAGE_STRING "MySQL Server 13.0.1"
#define PACKAGE_TARNAME "mysql"
#define PACKAGE_VERSION "13.0.1"
#define VERSION "13.0.1"
#define PROTOCOL_VERSION 10
#define PCRE2_CODE_UNIT_WIDTH 8

#define MALLOC_LIBRARY "system"

/* time_t related defines */

#define SIZEOF_TIME_T 8
/* #undef TIME_T_UNSIGNED */

#ifndef EMBEDDED_LIBRARY
/* #undef WSREP_INTERFACE_VERSION */
/* #undef WITH_WSREP */
#endif

#if !defined(__STDC_FORMAT_MACROS)
#define __STDC_FORMAT_MACROS
#endif  // !defined(__STDC_FORMAT_MACROS)

#endif

/* #undef HAVE_VFORK */

#define IO_SIZE 4096
