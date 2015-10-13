/*
   Copyright (c) 2001, 2011, Oracle and/or its affiliates.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

#ifndef _my_stacktrace_h_
#define _my_stacktrace_h_

#include <my_global.h>

#ifdef TARGET_OS_LINUX
#if defined (__x86_64__) || defined (__i386__) || \
    (defined(__alpha__) && defined(__GNUC__))
#define HAVE_STACKTRACE 1
#endif
#elif defined(__WIN__) || defined(HAVE_PRINTSTACK)
#define HAVE_STACKTRACE 1
#endif

#if HAVE_BACKTRACE && (HAVE_BACKTRACE_SYMBOLS || HAVE_BACKTRACE_SYMBOLS_FD)
#undef HAVE_STACKTRACE
#define HAVE_STACKTRACE 1
#endif

#define HAVE_WRITE_CORE

#if HAVE_BACKTRACE && HAVE_BACKTRACE_SYMBOLS && HAVE_ABI_CXA_DEMANGLE && \
    HAVE_WEAK_SYMBOL
#define BACKTRACE_DEMANGLE 1
#endif

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

C_MODE_START

#if defined(HAVE_STACKTRACE) || defined(HAVE_BACKTRACE)
void my_init_stacktrace();
void my_print_stacktrace(uchar* stack_bottom, ulong thread_stack);
int my_safe_print_str(const char* val, int max_len);
void my_write_core(int sig);
#if BACKTRACE_DEMANGLE
char *my_demangle(const char *mangled_name, int *status);
#endif /* BACKTRACE_DEMANGLE */
#ifdef __WIN__
void my_set_exception_pointers(EXCEPTION_POINTERS *ep);
#endif /* __WIN__ */
#else
#define my_init_stacktrace() do { } while(0)
#endif /* ! (defined(HAVE_STACKTRACE) || defined(HAVE_BACKTRACE)) */

#ifndef _WIN32
#define MY_ADDR_RESOLVE_FORK
#endif

#if defined(HAVE_BFD_H) || defined(MY_ADDR_RESOLVE_FORK)
#define HAVE_MY_ADDR_RESOLVE 1
#endif

typedef struct {
  const char *file;
  const char *func;
  uint line;
} my_addr_loc;

#ifdef HAVE_MY_ADDR_RESOLVE
int my_addr_resolve(void *ptr, my_addr_loc *loc);
const char *my_addr_resolve_init();
#else
#define my_addr_resolve_init()  (0)
#define my_addr_resolve(A,B)    (1)
#endif

#ifdef HAVE_WRITE_CORE
void my_write_core(int sig);
#endif

/**
  A (very) limited version of snprintf, which writes the result to STDERR.
  @sa my_safe_snprintf
  Implemented with simplicity, and async-signal-safety in mind.
  @note Has an internal buffer capacity of 512 bytes,
  which should suffice for our signal handling routines.
*/
size_t my_safe_printf_stderr(const char* fmt, ...)
  ATTRIBUTE_FORMAT(printf, 1, 2);

/**
  Writes up to count bytes from buffer to STDERR.
  Implemented with simplicity, and async-signal-safety in mind.
  @param   buf   Buffer containing data to be written.
  @param   count Number of bytes to write.
  @returns Number of bytes written.
*/
size_t my_write_stderr(const void *buf, size_t count);

/*
 Core dump control options to exclude certain buffers from core dump files

 There are two motivations for excluding things from core dumps:

 * resource utilization: stuff like the InnoDB pool buffer is rarely needed
   for post mortem debugging, but on machines with large amounts of memory
   just the time and file system space it takes to write a core dump can
   become substantial. Large core dump sizes can also become an obstacle
   when providing a core dump to a 3rd party for analysis

 * security: certain buffers, especially the InnoDB pool buffer or the
   Aria page cache, are likely to contain sensitive user data. Excluding
   these from a core dump can improve data security and especially can be
   a requirement for being able to pass production server core dumps to
   3rd parties for analysis
*/

#ifdef HAVE_MADV_DONTDUMP

/* Core dump exclusion constants */
#define CORE_NODUMP_NONE                0
#define CORE_NODUMP_MAX                 1 << 1
#define CORE_NODUMP_INNODB_POOL_BUFFER  1 << 2
#define CORE_NODUMP_MYISAM_KEY_BUFFER   1 << 3

#endif /* HAVE_MADV_DONTDUMP */

C_MODE_END

#endif /* _my_stacktrace_h_ */
