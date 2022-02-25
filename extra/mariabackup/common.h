/******************************************************
Copyright (c) 2011-2013 Percona LLC and/or its affiliates.

Common declarations for XtraBackup.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA

*******************************************************/

#ifndef XB_COMMON_H
#define XB_COMMON_H

#include <my_global.h>
#include <mysql_version.h>
#include <fcntl.h>
#include <stdarg.h>
#include <my_sys.h>


/** Determine if (i) is a user tablespace id or not. */
# define fil_is_user_tablespace_id(i) (i != 0 \
				       && !srv_is_undo_tablespace(i))

#ifdef _MSC_VER
#define stat _stati64
#define PATH_MAX MAX_PATH
#endif

#ifndef HAVE_VASPRINTF
static inline int vasprintf(char **strp, const char *fmt, va_list args)
{
  int len;
#ifdef _MSC_VER
  len = _vscprintf(fmt, args);
#else
  len = vsnprintf(NULL, 0, fmt, args);
#endif
  if (len < 0)
  {
    return -1;
  }
  *strp = (char *)malloc(len + 1);
  if (!*strp)
  {
    return -1;
  }
  vsprintf(*strp, fmt, args);
  return len;
}

static inline int asprintf(char **strp, const char *fmt,...)
{
  va_list	args;
  va_start(args, fmt);
  int len = vasprintf(strp, fmt, args);
  va_end(args);
  return len;
}
#endif

#define xb_a(expr)							\
	do {								\
		if (!(expr)) {						\
			fprintf(stderr,"Assertion \"%s\" failed at %s:%lu\n",	\
			    #expr, __FILE__, (ulong) __LINE__);		\
			abort();					\
		}							\
	} while (0);

#ifdef XB_DEBUG
#define xb_ad(expr) xb_a(expr)
#else
#define xb_ad(expr)
#endif

#define XB_DELTA_INFO_SUFFIX ".meta"

static inline int msg1(uint thread_num, const char *prefix, const char *fmt, va_list args)
{
  int result;
  time_t t = time(NULL);
  char date[100];
  char *line;
  strftime(date, sizeof(date), "%Y-%m-%d %H:%M:%S", localtime(&t));
  result = vasprintf(&line, fmt, args);
  if (result != -1) {
    if (fmt && fmt[strlen(fmt)] != '\n')
      result = fprintf(stderr, "[%02u] %s%s %s\n", thread_num, prefix, date, line);
    else
      result = fprintf(stderr, "[%02u] %s%s %s", thread_num, prefix, date, line);
    free(line);
  }
  return result;
}

static inline  ATTRIBUTE_FORMAT(printf, 2, 3) int msg(unsigned int thread_num, const char *fmt, ...)
{
  int result;
  va_list args;
  va_start(args, fmt);
  result = msg1(thread_num,"", fmt, args);
  va_end(args);
  return result;
}

static inline ATTRIBUTE_FORMAT(printf, 1, 2) int msg(const char *fmt, ...)
{
  int result;
  va_list args;
  va_start(args, fmt);
  result = msg1(0, "", fmt, args);
  va_end(args);
  return result;
}

static inline ATTRIBUTE_FORMAT(printf, 1,2) ATTRIBUTE_NORETURN void die(const char *fmt, ...)
{
  va_list args;
  va_start(args, fmt);
  msg1(0, "FATAL ERROR: ", fmt, args);
  va_end(args);
  fflush(stderr);
  _exit(EXIT_FAILURE);
}


/* Use POSIX_FADV_NORMAL when available */

#ifdef POSIX_FADV_NORMAL
# define USE_POSIX_FADVISE
#else
# define POSIX_FADV_NORMAL
# define POSIX_FADV_SEQUENTIAL
# define POSIX_FADV_DONTNEED
# define posix_fadvise(a,b,c,d) do {} while(0)
#endif

/***********************************************************************
Computes bit shift for a given value. If the argument is not a power
of 2, returns 0.*/
static inline unsigned get_bit_shift(size_t value)
{
    unsigned shift;

    if (value == 0)
	return 0;

    for (shift = 0; !(value & 1); shift++) {
	value >>= 1;
    }
    return (value >> 1) ? 0 : shift;
}

/****************************************************************************
Read 'len' bytes from 'fd'. It is identical to my_read(..., MYF(MY_FULL_IO)),
i.e. tries to combine partial reads into a single block of size 'len', except
that it bails out on EOF or error, and returns the number of successfully read
bytes instead. */
static inline size_t
xb_read_full(File fd, uchar *buf, size_t len)
{
	size_t tlen = 0;
	size_t tbytes;

	while (tlen < len) {
		tbytes = my_read(fd, buf, len - tlen, MYF(MY_WME));
		if (tbytes == 0 || tbytes == MY_FILE_ERROR) {
			break;
		}

		buf += tbytes;
		tlen += tbytes;
	}

	return tlen;
}

#ifdef _WIN32
#define IS_TRAILING_SLASH(name, length) \
	((length) > 1 && \
		(name[(length) - 1] == '/' || \
		 name[(length) - 1] == '\\'))
#else
#define IS_TRAILING_SLASH(name, length) \
	((length) > 1 && name[(length) - 1] == FN_LIBCHAR)
#endif

#endif
