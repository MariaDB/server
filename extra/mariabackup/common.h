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
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************/

#ifndef XB_COMMON_H
#define XB_COMMON_H

#include <my_global.h>
#include <mysql_version.h>
#include <fcntl.h>
#include <stdarg.h>

#define xb_a(expr)							\
	do {								\
		if (!(expr)) {						\
			msg("Assertion \"%s\" failed at %s:%lu\n",	\
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

static inline int msg(const char *fmt, ...) ATTRIBUTE_FORMAT(printf, 1, 2);
static inline int msg(const char *fmt, ...)
{
	int	result;
	va_list	args;

	va_start(args, fmt);
	result = vfprintf(stderr, fmt, args);
	va_end(args);

	return result;
}

static inline int msg_ts(const char *fmt, ...) ATTRIBUTE_FORMAT(printf, 1, 2);
static inline int msg_ts(const char *fmt, ...)
{
	int	result;
	time_t 	t = time(NULL);
	char	date[100];
	char	*line;
	va_list	args;

	strftime(date, sizeof(date), "%y%m%d %H:%M:%S", localtime(&t));

	va_start(args, fmt);
	result = vasprintf(&line, fmt, args);
	va_end(args);

	if (result != -1) {
		result = fprintf(stderr, "%s %s", date, line);
		free(line);
	}

	return result;
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
static inline ulong
get_bit_shift(ulong value)
{
    ulong shift;

    if (value == 0)
	return 0;

    for (shift = 0; !(value & 1UL); shift++) {
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

#endif
