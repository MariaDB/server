/*
   Copyright (c) 2001, 2013, Oracle and/or its affiliates.
   Copyright (c) 2009, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA */

/* This is the include file that should be included 'first' in every C file. */

#ifndef MY_GLOBAL_INCLUDED
#define MY_GLOBAL_INCLUDED

/*
  MDEV-25602 Deprecate __WIN__ symbol.
*/
#if defined (_MSC_VER) && !defined(__clang__)
#pragma deprecated("__WIN__")
#elif defined (__GNUC__)
#pragma GCC poison __WIN__
#endif

#if defined(_MSC_VER) && !defined(__clang__)
/*
  Following functions have bugs, when used with UTF-8 active codepage.
  #include <winservice.h> will use the non-buggy wrappers
*/
#pragma deprecated("CreateServiceA", "OpenServiceA", "ChangeServiceConfigA")
#endif

/*
  InnoDB depends on some MySQL internals which other plugins should not
  need.  This is because of InnoDB's foreign key support, "safe" binlog
  truncation, and other similar legacy features.

  We define accessors for these internals unconditionally, but do not
  expose them in mysql/plugin.h.  They are declared in ha_innodb.h for
  InnoDB's use.
*/
#define INNODB_COMPATIBILITY_HOOKS

#ifdef __CYGWIN__
/* We use a Unix API, so pretend it's not Windows */
#undef WIN
#undef WIN32
#undef _WIN
#undef _WIN32
#undef _WIN64
#undef _WIN32
#undef __WIN32__
#define HAVE_ERRNO_AS_DEFINE
#define _POSIX_MONOTONIC_CLOCK
#define _POSIX_THREAD_CPUTIME
#endif /* __CYGWIN__ */

#if defined(__OpenBSD__) && (OpenBSD >= 200411)
#define HAVE_ERRNO_AS_DEFINE
#endif

#if defined(i386) && !defined(__i386__)
#define __i386__
#endif

/* Macros to make switching between C and C++ mode easier */
#ifdef __cplusplus
#define C_MODE_START    extern "C" {
#define C_MODE_END	}
#else
#define C_MODE_START
#define C_MODE_END
#endif

#ifdef __cplusplus
#define CPP_UNNAMED_NS_START  namespace {
#define CPP_UNNAMED_NS_END    }
#endif

#include <my_config.h>

#ifdef WITH_PERFSCHEMA_STORAGE_ENGINE
#define HAVE_PSI_INTERFACE
#endif /* WITH_PERFSCHEMA_STORAGE_ENGINE */

/* Make it easier to add conditional code in _expressions_ */
#ifdef _WIN32
#define IF_WIN(A,B) A
#else
#define IF_WIN(A,B) B
#endif

#ifdef EMBEDDED_LIBRARY
#define IF_EMBEDDED(A,B) A
#else
#define IF_EMBEDDED(A,B) B
#endif

#ifdef WITH_PARTITION_STORAGE_ENGINE
#define IF_PARTITIONING(A,B) A
#else
#define IF_PARTITIONING(A,B) B
#endif

#if defined (_WIN32)
/*
 off_t is 32 bit long. We do not use C runtime functions
 with off_t but native Win32 file IO APIs, that work with
 64 bit offsets.
*/
#undef SIZEOF_OFF_T
#define SIZEOF_OFF_T 8

/*
 Prevent inclusion of  Windows GDI headers - they define symbol
 ERROR that conflicts with mysql headers.
*/
#ifndef NOGDI
#define NOGDI
#endif

/* Include common headers.*/
#include <winsock2.h>
#include <ws2tcpip.h> /* SOCKET */
#include <io.h>       /* access(), chmod() */
#include <process.h>  /* getpid() */

#define sleep(a) Sleep((a)*1000)

/* Define missing access() modes. */
#define F_OK 0
#define W_OK 2
#define R_OK 4                        /* Test for read permission.  */

/* Define missing file locking constants. */
#define F_RDLCK 1
#define F_WRLCK 2
#define F_UNLCK 3
#define F_TO_EOF 0x3FFFFFFF

#endif /* _WIN32*/

/*
  The macros below are used to allow build of Universal/fat binaries of
  MySQL and MySQL applications under darwin. 
*/
#if defined(__APPLE__) && defined(__MACH__)
#  undef SIZEOF_CHARP 
#  undef SIZEOF_INT 
#  undef SIZEOF_LONG 
#  undef SIZEOF_LONG_LONG 
#  undef SIZEOF_OFF_T 
#  undef WORDS_BIGENDIAN
#  define SIZEOF_INT 4
#  define SIZEOF_LONG_LONG 8
#  define SIZEOF_OFF_T 8
#  if defined(__i386__) || defined(__ppc__)
#    define SIZEOF_CHARP 4
#    define SIZEOF_LONG 4
#  elif defined(__x86_64__) || defined(__ppc64__) || defined(__aarch64__) || defined(__arm64__)
#    define SIZEOF_CHARP 8
#    define SIZEOF_LONG 8
#  else
#    error Building FAT binary for an unknown architecture.
#  endif
#  if defined(__ppc__) || defined(__ppc64__)
#    define WORDS_BIGENDIAN
#  endif
#endif /* defined(__APPLE__) && defined(__MACH__) */


/*
  The macros below are borrowed from include/linux/compiler.h in the
  Linux kernel. Use them to indicate the likelihood of the truthfulness
  of a condition. This serves two purposes - newer versions of gcc will be
  able to optimize for branch predication, which could yield siginficant
  performance gains in frequently executed sections of the code, and the
  other reason to use them is for documentation
*/

#if !defined(__GNUC__) || (__GNUC__ == 2 && __GNUC_MINOR__ < 96)
#define __builtin_expect(x, expected_value) (x)
#endif

/* Fix problem with S_ISLNK() on Linux */
#if defined(TARGET_OS_LINUX) || defined(__GLIBC__)
#undef  _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

/*
  Temporary solution to solve bug#7156. Include "sys/types.h" before
  the thread headers, else the function madvise() will not be defined
*/
#if defined(HAVE_SYS_TYPES_H) && ( defined(sun) || defined(__sun) )
#include <sys/types.h>
#endif

#define __EXTENSIONS__ 1	/* We want some extension */
#ifndef __STDC_EXT__
#define __STDC_EXT__ 1          /* To get large file support on hpux */
#endif

/*
  Solaris 9 include file <sys/feature_tests.h> refers to X/Open document

    System Interfaces and Headers, Issue 5

  saying we should define _XOPEN_SOURCE=500 to get POSIX.1c prototypes,
  but apparently other systems (namely FreeBSD) don't agree.

  On a newer Solaris 10, the above file recognizes also _XOPEN_SOURCE=600.
  Furthermore, it tests that if a program requires older standard
  (_XOPEN_SOURCE<600 or _POSIX_C_SOURCE<200112L) it cannot be
  run on a new compiler (that defines _STDC_C99) and issues an #error.
  It's also an #error if a program requires new standard (_XOPEN_SOURCE=600
  or _POSIX_C_SOURCE=200112L) and a compiler does not define _STDC_C99.

  To add more to this mess, Sun Studio C compiler defines _STDC_C99 while
  C++ compiler does not!

  So, in a desperate attempt to get correct prototypes for both
  C and C++ code, we define either _XOPEN_SOURCE=600 or _XOPEN_SOURCE=500
  depending on the compiler's announced C standard support.

  Cleaner solutions are welcome.
*/
#ifdef __sun
#if __STDC_VERSION__ - 0 >= 199901L
#define _XOPEN_SOURCE 600
#else
#define _XOPEN_SOURCE 500
#endif
#endif


#ifdef _AIX
/*
  AIX includes inttypes.h from sys/types.h
  Explicitly request format macros before the first inclusion of inttypes.h
*/
#if !defined(__STDC_FORMAT_MACROS)
#define __STDC_FORMAT_MACROS
#endif  // !defined(__STDC_FORMAT_MACROS)
#endif


#if !defined(_WIN32)
#ifndef _POSIX_PTHREAD_SEMANTICS
#define _POSIX_PTHREAD_SEMANTICS /* We want posix threads */
#endif

#if !defined(SCO)
#define _REENTRANT	1	/* Some thread libraries require this */
#endif
#if !defined(_THREAD_SAFE) && !defined(_AIX)
#define _THREAD_SAFE            /* Required for OSF1 */
#endif
#if defined(HPUX10) || defined(HPUX11)
C_MODE_START			/* HPUX needs this, signal.h bug */
#include <pthread.h>
C_MODE_END
#else
#include <pthread.h>		/* AIX must have this included first */
#endif
#if !defined(SCO) && !defined(_REENTRANT)
#define _REENTRANT	1	/* Threads requires reentrant code */
#endif
#endif /* !defined(_WIN32) */

/* gcc/egcs issues */

#if defined(__GNUC) && defined(__EXCEPTIONS)
#error "Please add -fno-exceptions to CXXFLAGS and reconfigure/recompile"
#endif

#if defined(_lint) && !defined(lint)
#define lint
#endif

#ifndef stdin
#include <stdio.h>
#endif
#include <stdarg.h>
#ifdef HAVE_STDLIB_H
#include <stdlib.h>
#endif
#ifdef HAVE_STDDEF_H
#include <stddef.h>
#endif

#include <math.h>
#ifdef HAVE_LIMITS_H
#include <limits.h>
#endif
#ifdef HAVE_FLOAT_H
#include <float.h>
#endif
#ifdef HAVE_FENV_H
#include <fenv.h> /* For fesetround() */
#endif

#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif

#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#ifdef HAVE_SYS_STAT_H
#include <sys/stat.h>
#endif
#if TIME_WITH_SYS_TIME
# include <sys/time.h>
# include <time.h>
#else
# if HAVE_SYS_TIME_H
#  include <sys/time.h>
# else
#  include <time.h>
# endif
#endif /* TIME_WITH_SYS_TIME */
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
#if defined(__cplusplus) && defined(NO_CPLUSPLUS_ALLOCA)
#undef HAVE_ALLOCA
#undef HAVE_ALLOCA_H
#endif
#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif

#include <errno.h>				/* Recommended by debian */
/* We need the following to go around a problem with openssl on solaris */
#if defined(HAVE_CRYPT_H)
#include <crypt.h>
#endif

/* Add checking if we are using likely/unlikely wrong */
#ifdef CHECK_UNLIKELY
C_MODE_START
extern void init_my_likely(), end_my_likely(FILE *);
extern int my_likely_ok(const char *file_name, uint line);
extern int my_likely_fail(const char *file_name, uint line);
C_MODE_END

#define likely(A) ((A) ? (my_likely_ok(__FILE__, __LINE__),1) : (my_likely_fail(__FILE__, __LINE__), 0))
#define unlikely(A) ((A) ? (my_likely_fail(__FILE__, __LINE__),1) : (my_likely_ok(__FILE__, __LINE__), 0))
/*
  These macros should be used when the check fails often when running benchmarks but
  we know for sure that the check is correct in a production environment
*/
#define checked_likely(A) (A)
#define checked_unlikely(A) (A)
#else
/**
  The semantics of builtin_expect() are that
  1) its two arguments are long
  2) it's likely that they are ==
  Those of our likely(x) are that x can be bool/int/longlong/pointer.
*/

#define likely(x)	__builtin_expect(((x) != 0),1)
#define unlikely(x)	__builtin_expect(((x) != 0),0)
#define checked_likely(x) likely(x)
#define checked_unlikely(x) unlikely(x)
#endif /* CHECK_UNLIKELY */

/*
  A lot of our programs uses asserts, so better to always include it
  This also fixes a problem when people uses DBUG_ASSERT without including
  assert.h
*/
#include <assert.h>

/* an assert that works at compile-time. only for constant expression */
#ifdef _some_old_compiler_that_does_not_understand_the_construct_below_
#define compile_time_assert(X)  do { } while(0)
#else
#define compile_time_assert(X)                                  \
  do                                                            \
  {                                                             \
    typedef char compile_time_assert[(X) ? 1 : -1] __attribute__((unused)); \
  } while(0)
#endif

/* Go around some bugs in different OS and compilers */
#if defined (HPUX11) && defined(_LARGEFILE_SOURCE)
#ifndef _LARGEFILE64_SOURCE
#define _LARGEFILE64_SOURCE
#endif
#endif

#if defined(_HPUX_SOURCE) && defined(HAVE_SYS_STREAM_H)
#include <sys/stream.h>		/* HPUX 10.20 defines ulong here. UGLY !!! */
#define HAVE_ULONG
#endif
#if defined(HPUX10) && defined(_LARGEFILE64_SOURCE)
/* Fix bug in setrlimit */
#undef setrlimit
#define setrlimit cma_setrlimit64
#endif
/* Declare madvise where it is not declared for C++, like Solaris */
#if HAVE_MADVISE && !HAVE_DECL_MADVISE && defined(__cplusplus)
extern "C" int madvise(void *addr, size_t len, int behav);
#endif
#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif
/** FreeBSD equivalent */
#if defined(MADV_CORE) && !defined(MADV_DODUMP)
#define MADV_DODUMP MADV_CORE
#define MADV_DONTDUMP MADV_NOCORE
#define DODUMP_STR "MADV_CORE"
#define DONTDUMP_STR "MADV_NOCORE"
#else
#define DODUMP_STR "MADV_DODUMP"
#define DONTDUMP_STR "MADV_DONTDUMP"
#endif


#define QUOTE_ARG(x)		#x	/* Quote argument (before cpp) */
#define STRINGIFY_ARG(x) QUOTE_ARG(x)	/* Quote argument, after cpp */

/* Paranoid settings. Define I_AM_PARANOID if you are paranoid */
#ifdef I_AM_PARANOID
#define DONT_ALLOW_USER_CHANGE 1
#define DONT_USE_MYSQL_PWD 1
#endif

/* Does the system remember a signal handler after a signal ? */
#if !defined(HAVE_BSD_SIGNALS) && !defined(HAVE_SIGACTION)
#define SIGNAL_HANDLER_RESET_ON_DELIVERY
#endif

/* don't assume that STDERR_FILENO is 2, mysqld can freopen */
#undef STDERR_FILENO

#ifndef SO_EXT
#ifdef _WIN32
#define SO_EXT ".dll"
#else
#define SO_EXT ".so"
#endif
#endif

/*
   Suppress uninitialized variable warning without generating code.
*/
#if defined(__GNUC__)
/* GCC specific self-initialization which inhibits the warning. */
#define UNINIT_VAR(x) x= x
#elif defined(_lint) || defined(FORCE_INIT_OF_VARS)
#define UNINIT_VAR(x) x= 0
#else
#define UNINIT_VAR(x) x
#endif

/* This is only to be used when resetting variables in a class constructor */
#if defined(_lint) || defined(FORCE_INIT_OF_VARS)
#define LINT_INIT(x) x= 0
#else
#define LINT_INIT(x)
#endif

#if !defined(HAVE_UINT)
#undef HAVE_UINT
#define HAVE_UINT
typedef unsigned int uint;
typedef unsigned short ushort;
#endif

#define swap_variables(t, a, b) do { t dummy; dummy= a; a= b; b= dummy; } while(0)
#define MY_TEST(a) ((a) ? 1 : 0)
#define set_if_bigger(a,b)  do { if ((a) < (b)) (a)=(b); } while(0)
#define set_if_smaller(a,b) do { if ((a) > (b)) (a)=(b); } while(0)
#define set_bits(type, bit_count) (sizeof(type)*8 <= (bit_count) ? ~(type) 0 : ((((type) 1) << (bit_count)) - (type) 1))
#define test_all_bits(a,b) (((a) & (b)) == (b))
#define array_elements(A) ((uint) (sizeof(A)/sizeof(A[0])))

/* Define some general constants */
#ifndef TRUE
#define TRUE		(1)	/* Logical true */
#define FALSE		(0)	/* Logical false */
#endif

#include <my_compiler.h>

/*
  Wen using the embedded library, users might run into link problems,
  duplicate declaration of __cxa_pure_virtual, solved by declaring it a
  weak symbol.
*/
#if defined(USE_MYSYS_NEW) && ! defined(DONT_DECLARE_CXA_PURE_VIRTUAL)
C_MODE_START
int __cxa_pure_virtual () __attribute__ ((weak));
C_MODE_END
#endif

/* The DBUG_ON flag always takes precedence over default DBUG_OFF */
#if defined(DBUG_ON) && defined(DBUG_OFF)
#undef DBUG_OFF
#endif

/* We might be forced to turn debug off, if not turned off already */
#if (defined(FORCE_DBUG_OFF) || defined(_lint)) && !defined(DBUG_OFF)
#  define DBUG_OFF
#  ifdef DBUG_ON
#    undef DBUG_ON
#  endif
#endif

#ifdef DBUG_OFF
#undef EXTRA_DEBUG
#endif

/* Some types that is different between systems */

typedef int	File;		/* File descriptor */
#ifdef _WIN32
typedef SOCKET my_socket;
#else
typedef int	my_socket;	/* File descriptor for sockets */
#define INVALID_SOCKET -1
#endif
/* Type for functions that handles signals */
#define sig_handler RETSIGTYPE
#if defined(__GNUC__) && !defined(_lint)
typedef char	pchar;		/* Mixed prototypes can take char */
typedef char	puchar;		/* Mixed prototypes can take char */
typedef char	pbool;		/* Mixed prototypes can take char */
typedef short	pshort;		/* Mixed prototypes can take short int */
typedef float	pfloat;		/* Mixed prototypes can take float */
#else
typedef int	pchar;		/* Mixed prototypes can't take char */
typedef uint	puchar;		/* Mixed prototypes can't take char */
typedef int	pbool;		/* Mixed prototypes can't take char */
typedef int	pshort;		/* Mixed prototypes can't take short int */
typedef double	pfloat;		/* Mixed prototypes can't take float */
#endif
C_MODE_START
typedef int	(*qsort_cmp)(const void *,const void *);
typedef int	(*qsort_cmp2)(void*, const void *,const void *);
C_MODE_END
#define qsort_t RETQSORTTYPE	/* Broken GCC can't handle typedef !!!! */
#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
typedef SOCKET_SIZE_TYPE size_socket;

#ifndef SOCKOPT_OPTLEN_TYPE
#define SOCKOPT_OPTLEN_TYPE size_socket
#endif

/* file create flags */

#ifndef O_SHARE			/* Probably not windows */
#define O_SHARE		0	/* Flag to my_open for shared files */
#ifndef O_BINARY
#define O_BINARY	0	/* Flag to my_open for binary files */
#endif
#ifndef FILE_BINARY
#define FILE_BINARY	O_BINARY /* Flag to my_fopen for binary streams */
#endif
#ifdef HAVE_FCNTL
#define HAVE_FCNTL_LOCK
#define F_TO_EOF	0L	/* Param to lockf() to lock rest of file */
#endif
#endif /* O_SHARE */

#ifndef O_SEQUENTIAL
#define O_SEQUENTIAL	0
#endif
#ifndef O_SHORT_LIVED
#define O_SHORT_LIVED	0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW      0
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC       0
#endif
#ifdef __GLIBC__
#define STR_O_CLOEXEC "e"
#else
#define STR_O_CLOEXEC ""
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC    0
#else
#define HAVE_SOCK_CLOEXEC
#endif

/* additional file share flags for win32 */
#ifdef _WIN32
#define _SH_DENYRWD     0x110    /* deny read/write mode & delete */
#define _SH_DENYWRD     0x120    /* deny write mode & delete      */
#define _SH_DENYRDD     0x130    /* deny read mode & delete       */
#define _SH_DENYDEL     0x140    /* deny delete only              */
#endif /* _WIN32 */


/* General constants */
#define FN_LEN		256	/* Max file name len */
#define FN_HEADLEN	253	/* Max length of filepart of file name */
#define FN_EXTLEN	20	/* Max length of extension (part of FN_LEN) */
#define FN_REFLEN	512	/* Max length of full path-name */
#define FN_EXTCHAR	'.'
#define FN_HOMELIB	'~'	/* ~/ is used as abbrev for home dir */
#define FN_CURLIB	'.'	/* ./ is used as abbrev for current dir */
#define FN_PARENTDIR	".."	/* Parent directory; Must be a string */

#ifdef _WIN32
#define FN_LIBCHAR	'\\'
#define FN_LIBCHAR2	'/'
#define FN_DIRSEP       "/\\"               /* Valid directory separators */
#define FN_EXEEXT   ".exe"
#define FN_SOEXT    ".dll"
#define FN_ROOTDIR	"\\"
#define FN_DEVCHAR	':'
#define FN_NETWORK_DRIVES	/* Uses \\ to indicate network drives */
#define FN_NO_CASE_SENCE	/* Files are not case-sensitive */
#else
#define FN_LIBCHAR	'/'
#define FN_LIBCHAR2	'/'
#define FN_DIRSEP       "/"     /* Valid directory separators */
#define FN_EXEEXT   ""
#define FN_SOEXT    ".so"
#define FN_ROOTDIR	"/"
#endif

/* 
  MY_FILE_MIN is  Windows speciality and is used to quickly detect
  the mismatch of CRT and mysys file IO usage on Windows at runtime.
  CRT file descriptors can be in the range 0-2047, whereas descriptors returned
  by my_open() will start with 2048. If a file descriptor with value less then
  MY_FILE_MIN is passed to mysys IO function, chances are it stemms from
  open()/fileno() and not my_open()/my_fileno.

  For Posix,  mysys functions are light wrappers around libc, and MY_FILE_MIN
  is logically 0.
*/

#ifdef _WIN32
#define MY_FILE_MIN  2048
#else
#define MY_FILE_MIN  0
#endif

/* 
  MY_NFILE is the default size of my_file_info array.

  It is larger on Windows, because it all file handles are stored in my_file_info
  Default size is 16384 and this should be enough for most cases.If it is not 
  enough, --max-open-files with larger value can be used.

  For Posix , my_file_info array is only used to store filenames for
  error reporting and its size is not a limitation for number of open files.
*/ 
#ifdef _WIN32
#define MY_NFILE (16384 + MY_FILE_MIN)
#else
#define MY_NFILE 64
#endif

#ifndef OS_FILE_LIMIT
#define OS_FILE_LIMIT	UINT_MAX
#endif

/*
  Io buffer size; Must be a power of 2 and a multiple of 512. May be
  smaller what the disk page size. This influences the speed of the
  isam btree library. eg to big to slow.
*/
#define IO_SIZE			4096U
/*
  How much overhead does malloc have. The code often allocates
  something like 1024-MALLOC_OVERHEAD bytes
*/
#define MALLOC_OVERHEAD 8

	/* get memory in huncs */
#define ONCE_ALLOC_INIT		(uint) 4096
	/* Typical record cache */
#define RECORD_CACHE_SIZE	(uint) (128*1024)
	/* Typical key cache */
#define KEY_CACHE_SIZE		(uint) (128L*1024L*1024L)
	/* Default size of a key cache block  */
#define KEY_CACHE_BLOCK_SIZE	(uint) 1024

	/* Some things that this system doesn't have */

#ifdef _WIN32
#define NO_DIR_LIBRARY		/* Not standard dir-library */
#endif

/* Some defines of functions for portability */

#undef remove		/* Crashes MySQL on SCO 5.0.0 */
#ifndef _WIN32
#define closesocket(A)	close(A)
#endif

#if defined(_MSC_VER)
#if !defined(_WIN64)
inline double my_ulonglong2double(unsigned long long value)
{
  long long nr=(long long) value;
  if (nr >= 0)
    return (double) nr;
  return (18446744073709551616.0 + (double) nr);
}
#define ulonglong2double my_ulonglong2double
#define my_off_t2double  my_ulonglong2double
#endif /* _WIN64 */
inline unsigned long long my_double2ulonglong(double d)
{
  double t= d - (double) 0x8000000000000000ULL;

  if (t >= 0)
    return  ((unsigned long long) t) + 0x8000000000000000ULL;
  return (unsigned long long) d;
}
#define double2ulonglong my_double2ulonglong
#endif

#ifndef ulonglong2double
#define ulonglong2double(A) ((double) (ulonglong) (A))
#define my_off_t2double(A)  ((double) (my_off_t) (A))
#endif
#ifndef double2ulonglong
#define double2ulonglong(A) ((ulonglong) (double) (A))
#endif

#ifndef offsetof
#define offsetof(TYPE, MEMBER) ((size_t) &((TYPE *)0)->MEMBER)
#endif
#define ulong_to_double(X) ((double) (ulong) (X))

#ifndef STACK_DIRECTION
#error "please add -DSTACK_DIRECTION=1 or -1 to your CPPFLAGS"
#endif

#if !defined(HAVE_STRTOK_R)
#define strtok_r(A,B,C) strtok((A),(B))
#endif

#if SIZEOF_LONG_LONG >= 8
#define HAVE_LONG_LONG 1
#else
#error WHAT? sizeof(long long) < 8 ???
#endif

/*
  Some pre-ANSI-C99 systems like AIX 5.1 and Linux/GCC 2.95 define
  ULONGLONG_MAX, LONGLONG_MIN, LONGLONG_MAX; we use them if they're defined.
*/

#if defined(HAVE_LONG_LONG) && !defined(LONGLONG_MIN)
#define LONGLONG_MIN	((long long) 0x8000000000000000LL)
#define LONGLONG_MAX	((long long) 0x7FFFFFFFFFFFFFFFLL)
#endif
/* Max length needed for a buffer to hold a longlong or ulonglong + end \0 */
#define LONGLONG_BUFFER_SIZE 21

#if defined(HAVE_LONG_LONG) && !defined(ULONGLONG_MAX)
/* First check for ANSI C99 definition: */
#ifdef ULLONG_MAX
#define ULONGLONG_MAX  ULLONG_MAX
#else
#define ULONGLONG_MAX ((unsigned long long)(~0ULL))
#endif
#endif /* defined (HAVE_LONG_LONG) && !defined(ULONGLONG_MAX)*/

#define INT_MIN64       (~0x7FFFFFFFFFFFFFFFLL)
#define INT_MAX64       0x7FFFFFFFFFFFFFFFLL
#define INT_MIN32       (~0x7FFFFFFFL)
#define INT_MAX32       0x7FFFFFFFL
#define UINT_MAX32      0xFFFFFFFFL
#define INT_MIN24       (~0x007FFFFF)
#define INT_MAX24       0x007FFFFF
#define UINT_MAX24      0x00FFFFFF
#define INT_MIN16       (~0x7FFF)
#define INT_MAX16       0x7FFF
#define UINT_MAX16      0xFFFF
#define INT_MIN8        (~0x7F)
#define INT_MAX8        0x7F
#define UINT_MAX8       0xFF

/* From limits.h instead */
#ifndef DBL_MIN
#define DBL_MIN		4.94065645841246544e-324
#define FLT_MIN		((float)1.40129846432481707e-45)
#endif
#ifndef DBL_MAX
#define DBL_MAX		1.79769313486231470e+308
#define FLT_MAX		((float)3.40282346638528860e+38)
#endif
#ifndef SIZE_T_MAX
#define SIZE_T_MAX      (~((size_t) 0))
#endif

/* Define missing math constants. */
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif
#ifndef M_LN2
#define M_LN2 0.69314718055994530942
#endif

/*
  Max size that must be added to a so that we know Size to make
  addressable obj.
*/
#if SIZEOF_CHARP == 4
typedef long		my_ptrdiff_t;
#else
typedef long long	my_ptrdiff_t;
#endif

#define MY_ALIGN(A,L)	   (((A) + (L) - 1) & ~((L) - 1))
#define MY_ALIGN_DOWN(A,L) ((A) & ~((L) - 1))
#define ALIGN_SIZE(A)	MY_ALIGN((A),sizeof(double))
#define ALIGN_MAX_UNIT  (sizeof(double))
/* Size to make addressable obj. */
#define ALIGN_PTR(A, t) ((t*) MY_ALIGN((A), sizeof(double)))
#define ADD_TO_PTR(ptr,size,type) (type) ((uchar*) (ptr)+size)
#define PTR_BYTE_DIFF(A,B) (my_ptrdiff_t) ((uchar*) (A) - (uchar*) (B))
#define PREV_BITS(type,A)	((type) (((type) 1 << (A)) -1))

/*
  Custom version of standard offsetof() macro which can be used to get
  offsets of members in class for non-POD types (according to the current
  version of C++ standard offsetof() macro can't be used in such cases and
  attempt to do so causes warnings to be emitted, OTOH in many cases it is
  still OK to assume that all instances of the class has the same offsets
  for the same members).

  This is temporary solution which should be removed once File_parser class
  and related routines are refactored.
*/

#define my_offsetof(TYPE, MEMBER) PTR_BYTE_DIFF(&((TYPE *)0x10)->MEMBER, 0x10)

#define NullS		(char *) 0

#ifdef STDCALL
#undef STDCALL
#endif

#ifdef _WIN32
#define STDCALL __stdcall
#else
#define STDCALL
#endif

/* Typdefs for easyier portability */

#ifndef HAVE_UCHAR
typedef unsigned char	uchar;	/* Short for unsigned char */
#endif

#ifndef HAVE_INT8
typedef signed char int8;       /* Signed integer >= 8  bits */
#endif
#ifndef HAVE_UINT8
typedef unsigned char uint8;    /* Unsigned integer >= 8  bits */
#endif
#ifndef HAVE_INT16
typedef short int16;
#endif
#ifndef HAVE_UINT16
typedef unsigned short uint16;
#endif
#if SIZEOF_INT == 4
#ifndef HAVE_INT32
typedef int int32;
#endif
#ifndef HAVE_UINT32
typedef unsigned int uint32;
#endif
#elif SIZEOF_LONG == 4
#ifndef HAVE_INT32
typedef long int32;
#endif
#ifndef HAVE_UINT32
typedef unsigned long uint32;
#endif
#else
#error Neither int or long is of 4 bytes width
#endif

#if !defined(HAVE_ULONG) && !defined(__USE_MISC)
typedef unsigned long	ulong;		  /* Short for unsigned long */
#endif
#ifndef longlong_defined
/* 
  Using [unsigned] long long is preferable as [u]longlong because we use 
  [unsigned] long long unconditionally in many places, 
  for example in constants with [U]LL suffix.
*/
#if defined(HAVE_LONG_LONG) && SIZEOF_LONG_LONG == 8
typedef unsigned long long int ulonglong; /* ulong or unsigned long long */
typedef long long int	longlong;
#else
typedef unsigned long	ulonglong;	  /* ulong or unsigned long long */
typedef long		longlong;
#endif
#endif
#ifndef HAVE_INT64
typedef longlong int64;
#endif
#ifndef HAVE_UINT64
typedef ulonglong uint64;
#endif

#if defined(NO_CLIENT_LONG_LONG)
typedef unsigned long my_ulonglong;
#elif defined (_WIN32)
typedef unsigned __int64 my_ulonglong;
#else
typedef unsigned long long my_ulonglong;
#endif

#if SIZEOF_CHARP == SIZEOF_INT
typedef unsigned int intptr;
#elif SIZEOF_CHARP == SIZEOF_LONG
typedef unsigned long intptr;
#elif SIZEOF_CHARP == SIZEOF_LONG_LONG
typedef unsigned long long intptr;
#else
#error sizeof(void *) is neither sizeof(int) nor sizeof(long) nor sizeof(long long)
#endif

#define MY_ERRPTR ((void*)(intptr)1)

#if defined(_WIN32)
typedef unsigned long long my_off_t;
typedef unsigned long long os_off_t;
#else
typedef off_t os_off_t;
#if SIZEOF_OFF_T > 4
typedef ulonglong my_off_t;
#else
typedef unsigned long my_off_t;
#endif
#endif /*_WIN32*/
#define MY_FILEPOS_ERROR	(~(my_off_t) 0)

/*
  TODO Convert these to use Bitmap class.
 */
typedef ulonglong table_map;          /* Used for table bits in join */

/* often used type names - opaque declarations */
typedef const struct charset_info_st CHARSET_INFO;
typedef struct st_mysql_lex_string LEX_STRING;

#if defined(_WIN32)
#define socket_errno	WSAGetLastError()
#define SOCKET_EINTR	WSAEINTR
#define SOCKET_ETIMEDOUT WSAETIMEDOUT
#define SOCKET_EWOULDBLOCK WSAEWOULDBLOCK
#define SOCKET_EADDRINUSE WSAEADDRINUSE
#define SOCKET_ECONNRESET WSAECONNRESET
#define SOCKET_ENFILE	ENFILE
#define SOCKET_EMFILE	EMFILE
#else /* Unix */
#define socket_errno	errno
#define closesocket(A)	close(A)
#define SOCKET_EINTR	EINTR
#define SOCKET_EAGAIN	EAGAIN
#define SOCKET_EWOULDBLOCK EWOULDBLOCK
#define SOCKET_EADDRINUSE EADDRINUSE
#define SOCKET_ETIMEDOUT ETIMEDOUT
#define SOCKET_ECONNRESET ECONNRESET
#define SOCKET_ENFILE	ENFILE
#define SOCKET_EMFILE	EMFILE
#endif

#include <mysql/plugin.h>  /* my_bool */

typedef ulong		myf;	/* Type of MyFlags in my_funcs */

#define MYF(v)		(myf) (v)

/*
  Defines to make it possible to prioritize register assignments. No
  longer that important with modern compilers.
*/
#ifndef USING_X
#define reg1 register
#define reg2 register
#define reg3 register
#define reg4 register
#define reg5 register
#define reg6 register
#define reg7 register
#define reg8 register
#define reg9 register
#define reg10 register
#define reg11 register
#define reg12 register
#define reg13 register
#define reg14 register
#define reg15 register
#define reg16 register
#endif

/*
  MYSQL_PLUGIN_IMPORT macro is used to export mysqld data
  (i.e variables) for usage in storage engine loadable plugins.
  Outside of Windows, it is dummy.
*/
#ifndef MYSQL_PLUGIN_IMPORT
#if (defined(_WIN32) && defined(MYSQL_DYNAMIC_PLUGIN))
#define MYSQL_PLUGIN_IMPORT __declspec(dllimport)
#else
#define MYSQL_PLUGIN_IMPORT
#endif
#endif

#include <my_dbug.h>

/* Some helper macros */
#define YESNO(X) ((X) ? "yes" : "no")

#define MY_HOW_OFTEN_TO_ALARM	2	/* How often we want info on screen */
#define MY_HOW_OFTEN_TO_WRITE	10000	/* How often we want info on screen */

#include <my_byteorder.h>

#ifdef HAVE_CHARSET_utf8mb4
#define MYSQL_UNIVERSAL_CLIENT_CHARSET "utf8mb4"
#elif defined(HAVE_CHARSET_utf8mb3)
#define MYSQL_UNIVERSAL_CLIENT_CHARSET "utf8mb3"
#else
#define MYSQL_UNIVERSAL_CLIENT_CHARSET MYSQL_DEFAULT_CHARSET_NAME
#endif

#if defined(EMBEDDED_LIBRARY) && !defined(HAVE_EMBEDDED_PRIVILEGE_CONTROL)
#define NO_EMBEDDED_ACCESS_CHECKS
#endif

#ifdef _WIN32
#define dlsym(lib, name) (void*)GetProcAddress((HMODULE)lib, name)
#define dlopen(libname, unused) LoadLibraryEx(libname, NULL, 0)
#define RTLD_DEFAULT GetModuleHandle(NULL)
#define dlclose(lib) FreeLibrary((HMODULE)lib)
static inline char *dlerror(void)
{
  static char win_errormsg[2048];
  FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM
      | FORMAT_MESSAGE_IGNORE_INSERTS
      | FORMAT_MESSAGE_MAX_WIDTH_MASK,
    0, GetLastError(), 0, win_errormsg, 2048, NULL);
  return win_errormsg;
}
#define HAVE_DLOPEN 1
#define HAVE_DLERROR 1
#endif

#ifdef HAVE_DLFCN_H
#include <dlfcn.h>
#endif

#ifdef HAVE_DLOPEN
#ifndef HAVE_DLERROR
#define dlerror() ""
#endif
#ifndef HAVE_DLADDR
#define dladdr(A, B) 0
/* Dummy definition in case we're missing dladdr() */
typedef struct { const char *dli_fname, dli_fbase; } Dl_info;
#endif
#else
#define dlerror() "No support for dynamic loading (static build?)"
#define dlopen(A,B) 0
#define dlsym(A,B) 0
#define dlclose(A) 0
#define dladdr(A, B) 0
/* Dummy definition in case we're missing dladdr() */
typedef struct { const char *dli_fname, dli_fbase; } Dl_info;
#endif

/*
 *  Include standard definitions of operator new and delete.
 */
#ifdef __cplusplus
#include <new>
#endif

/* Length of decimal number represented by INT32. */
#define MY_INT32_NUM_DECIMAL_DIGITS 11

/* Length of decimal number represented by INT64. */
#define MY_INT64_NUM_DECIMAL_DIGITS 21

#ifdef __cplusplus
#include <limits> /* should be included before min/max macros */
#endif

/* Define some useful general macros (should be done after all headers). */
#define MY_MAX(a, b)	((a) > (b) ? (a) : (b))
#define MY_MIN(a, b)	((a) < (b) ? (a) : (b))

#define CMP_NUM(a,b)    (((a) < (b)) ? -1 : ((a) == (b)) ? 0 : 1)

/*
  Only Linux is known to need an explicit sync of the directory to make sure a
  file creation/deletion/renaming in(from,to) this directory durable.
*/
#ifdef TARGET_OS_LINUX
#define NEED_EXPLICIT_SYNC_DIR 1
#else
/*
  On linux default rwlock scheduling policy is good enough for
  waiting_threads.c, on other systems use our special implementation
  (which is slower).

  QQ perhaps this should be tested in configure ? how ?
*/
#define WT_RWLOCKS_USE_MUTEXES 1
#endif

#if !defined(__cplusplus) && !defined(bool)
#define bool In_C_you_should_use_my_bool_instead()
#endif

/* Provide __func__ macro definition for platforms that miss it. */
#if !defined (__func__)
#if defined(__STDC_VERSION__) && __STDC_VERSION__ < 199901L
#  if __GNUC__ >= 2
#    define __func__ __FUNCTION__
#  else
#    define __func__ "<unknown>"
#  endif
#elif defined(_MSC_VER)
#  if _MSC_VER < 1300
#    define __func__ "<unknown>"
#  else
#    define __func__ __FUNCTION__
#  endif
#elif defined(__BORLANDC__)
#  define __func__ __FUNC__
#else
#  define __func__ "<unknown>"
#endif
#endif /* !defined(__func__) */

/* Defines that are unique to the embedded version of MySQL */

#ifdef EMBEDDED_LIBRARY

/* Things we don't need in the embedded version of MySQL */
/* TODO HF add #undef HAVE_VIO if we don't want client in embedded library */

#else
#define HAVE_REPLICATION
#define HAVE_EXTERNAL_CLIENT
#endif /* EMBEDDED_LIBRARY */

/*
  Provide defaults for the CPU cache line size, if it has not been detected by
  CMake using getconf
*/
#if !defined(CPU_LEVEL1_DCACHE_LINESIZE) || CPU_LEVEL1_DCACHE_LINESIZE == 0
  #if defined(CPU_LEVEL1_DCACHE_LINESIZE) && CPU_LEVEL1_DCACHE_LINESIZE == 0
    #undef CPU_LEVEL1_DCACHE_LINESIZE
  #endif

  #if defined(__s390__)
    #define CPU_LEVEL1_DCACHE_LINESIZE 256
  #elif defined(__powerpc__) || defined(__aarch64__)
    #define CPU_LEVEL1_DCACHE_LINESIZE 128
  #else
    #define CPU_LEVEL1_DCACHE_LINESIZE 64
  #endif
#endif

#define FLOATING_POINT_DECIMALS 31

/* Keep client compatible with earlier versions */
#ifdef MYSQL_SERVER
#define NOT_FIXED_DEC           DECIMAL_NOT_SPECIFIED
#else
#define NOT_FIXED_DEC           FLOATING_POINT_DECIMALS
#endif
#endif /* my_global_h */
