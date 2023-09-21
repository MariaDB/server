/*
   Copyright (c) 2000, 2012, Oracle and/or its affiliates.
   Copyright (c) 2019, 2021, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

/* There may be problems included in all of these. Try to test in
   configure which ones are needed? */

/*  This is needed for the definitions of strchr... on solaris */

#ifndef _m_string_h
#define _m_string_h

#include "my_decimal_limits.h"

#ifndef __USE_GNU
#define __USE_GNU				/* We want to use stpcpy */
#endif
#if defined(HAVE_STRINGS_H)
#include <strings.h>
#endif
#if defined(HAVE_STRING_H)
#include <string.h>
#endif

/*  This is needed for the definitions of memcpy... on solaris */
#if defined(HAVE_MEMORY_H) && !defined(__cplusplus)
#include <memory.h>
#endif

#if !defined(HAVE_MEMCPY) && !defined(HAVE_MEMMOVE)
# define memcpy(d, s, n)	bcopy ((s), (d), (n))
# define memset(A,C,B)		bfill((A),(B),(C))
# define memmove(d, s, n)	bmove ((d), (s), (n))
#elif defined(HAVE_MEMMOVE)
# define bmove(d, s, n)		memmove((d), (s), (n))
#endif

/* Unixware 7 */
#if !defined(HAVE_BFILL)
# define bfill(A,B,C)           memset((A),(C),(B))
#endif

# define bmove_align(A,B,C)     memcpy((A),(B),(C))

# define bcmp(A,B,C)		memcmp((A),(B),(C))

#if !defined(bzero)
# define bzero(A,B)             memset((A),0,(B))
#endif

#if defined(__cplusplus)
extern "C" {
#endif

#ifdef DBUG_OFF
#if defined(HAVE_STPCPY) && defined(__GNUC__) && !defined(__INTEL_COMPILER)
#define strmov(A,B) __builtin_stpcpy((A),(B))
#elif defined(HAVE_STPCPY)
#define strmov(A,B) stpcpy((A),(B))
#endif
#endif

/* Declared in int2str() */
extern const char _dig_vec_upper[];
extern const char _dig_vec_lower[];

extern char *strmov_overlapp(char *dest, const char *src);

#if defined(_lint) || defined(FORCE_INIT_OF_VARS)
#define LINT_INIT_STRUCT(var) bzero(&var, sizeof(var)) /* No uninitialize-warning */
#else
#define LINT_INIT_STRUCT(var)
#endif

/* Prototypes for string functions */

extern	void bmove_upp(uchar *dst,const uchar *src,size_t len);
extern	void bchange(uchar *dst,size_t old_len,const uchar *src,
		     size_t new_len,size_t tot_len);
extern	void strappend(char *s,size_t len,pchar fill);
extern	char *strend(const char *s);
extern  char *strcend(const char *, pchar);
extern	char *strfill(char * s,size_t len,pchar fill);
extern	char *strmake(char *dst,const char *src,size_t length);

#if !defined(__GNUC__) || (__GNUC__ < 4)
#define strmake_buf(D,S)        strmake(D, S, sizeof(D) - 1)
#else
#define strmake_buf(D,S) ({                             \
  __typeof__ (D) __x __attribute__((unused)) = { 2 };   \
  strmake(D, S, sizeof(D) - 1);                         \
  })
#endif

#ifndef strmov
extern	char *strmov(char *dst,const char *src);
#endif
extern	char *strnmov(char *dst, const char *src, size_t n);
extern	char *strcont(const char *src, const char *set);
extern	char *strxmov(char *dst, const char *src, ...);
extern	char *strxnmov(char *dst, size_t len, const char *src, ...);

/* Prototypes of normal stringfunctions (with may ours) */
#ifndef HAVE_STRNLEN
extern size_t strnlen(const char *s, size_t n);
#endif

extern int is_prefix(const char *, const char *);

/* Conversion routines */
typedef enum {
  MY_GCVT_ARG_FLOAT,
  MY_GCVT_ARG_DOUBLE
} my_gcvt_arg_type;

double my_strtod(const char *str, char **end, int *error);
double my_atof(const char *nptr);
size_t my_fcvt(double x, int precision, char *to, my_bool *error);
size_t my_gcvt(double x, my_gcvt_arg_type type, int width, char *to,
               my_bool *error);

/*
  The longest string my_fcvt can return is 311 + "precision" bytes.
  Here we assume that we never cal my_fcvt() with
  precision >= DECIMAL_NOT_SPECIFIED
  (+ 1 byte for the terminating '\0').
*/
#define FLOATING_POINT_BUFFER (311 + DECIMAL_NOT_SPECIFIED)

/*
  We want to use the 'e' format in some cases even if we have enough space
  for the 'f' one just to mimic sprintf("%.15g") behavior for large integers,
  and to improve it for numbers < 10^(-4).
  That is, for |x| < 1 we require |x| >= 10^(-15), and for |x| > 1 we require
  it to be integer and be <= 10^DBL_DIG for the 'f' format to be used.
  We don't lose precision, but make cases like "1e200" or "0.00001" look nicer.
*/
#define MAX_DECPT_FOR_F_FORMAT DBL_DIG

/*
  The maximum possible field width for my_gcvt() conversion.
  (DBL_DIG + 2) significant digits + sign + "." + ("e-NNN" or
  MAX_DECPT_FOR_F_FORMAT zeros for cases when |x|<1 and the 'f' format is used).
*/
#define MY_GCVT_MAX_FIELD_WIDTH (DBL_DIG + 4 + MY_MAX(5, MAX_DECPT_FOR_F_FORMAT)) \

extern char *llstr(longlong value,char *buff);
extern char *ullstr(longlong value,char *buff);
#ifndef HAVE_STRTOUL
extern long strtol(const char *str, char **ptr, int base);
extern ulong strtoul(const char *str, char **ptr, int base);
#endif

extern char *int2str(long val, char *dst, int radix, int upcase);
extern char *int10_to_str(long val,char *dst,int radix);
extern char *str2int(const char *src,int radix,long lower,long upper,
			 long *val);
longlong my_strtoll10(const char *nptr, char **endptr, int *error);
#if SIZEOF_LONG == SIZEOF_LONG_LONG
#define ll2str(A,B,C,D) int2str((A),(B),(C),(D))
#define longlong10_to_str(A,B,C) int10_to_str((A),(B),(C))
#undef strtoll
#define strtoll(A,B,C) strtol((A),(B),(C))
#define strtoull(A,B,C) strtoul((A),(B),(C))
#ifndef HAVE_STRTOULL
#define HAVE_STRTOULL
#endif
#ifndef HAVE_STRTOLL
#define HAVE_STRTOLL
#endif
#else
#ifdef HAVE_LONG_LONG
extern char *ll2str(longlong val,char *dst,int radix, int upcase);
extern char *longlong10_to_str(longlong val,char *dst,int radix);
#if (!defined(HAVE_STRTOULL) || defined(NO_STRTOLL_PROTO))
extern longlong strtoll(const char *str, char **ptr, int base);
extern ulonglong strtoull(const char *str, char **ptr, int base);
#endif
#endif
#endif
#define longlong2str(A,B,C) ll2str((A),(B),(C),1)

#if defined(__cplusplus)
}
#endif

#include <mysql/plugin.h>

#ifdef __cplusplus
#include <type_traits>
template<typename T> inline constexpr const char *_swl_check(T s)
{
  static_assert(std::is_same<T, const char (&)[sizeof(T)]>::value
             || std::is_same<T, const char [sizeof(T)]>::value,
             "Wrong argument for STRING_WITH_LEN()");
  return s;
}
#define STRING_WITH_LEN(X) _swl_check<decltype(X)>(X), ((size_t) (sizeof(X) - 1))
#else
#define STRING_WITH_LEN(X) (X ""), ((size_t) (sizeof(X) - 1))
#endif

#define USTRING_WITH_LEN(X) (uchar*) STRING_WITH_LEN(X)
#define C_STRING_WITH_LEN(X) (char *) STRING_WITH_LEN(X)
#define LEX_STRING_WITH_LEN(X) (X).str, (X).length

typedef struct st_mysql_const_lex_string LEX_CSTRING;

/* A variant with const and unsigned */
struct st_mysql_const_unsigned_lex_string
{
  const uchar *str;
  size_t length;
};
typedef struct st_mysql_const_unsigned_lex_string LEX_CUSTRING;

static inline void lex_string_set(LEX_CSTRING *lex_str, const char *c_str)
{
  lex_str->str= c_str;
  lex_str->length= strlen(c_str);
}
static inline void lex_string_set3(LEX_CSTRING *lex_str, const char *c_str,
                                   size_t len)
{
  lex_str->str= c_str;
  lex_str->length= len;
}

/*
  Copies src into dst and ensures dst is a NULL terminated C string.

  Returns 1 if the src string was truncated due to too small size of dst.
  Returns 0 if src completely fit within dst. Pads the remaining dst with '\0'

  Note: dst_size must be > 0
*/
static inline int safe_strcpy(char *dst, size_t dst_size, const char *src)
{
  DBUG_ASSERT(dst_size > 0);

  /* 1) IF there is a 0 byte in the first dst_size bytes of src, strncpy will
   *    0-terminate dst, and pad dst with additional 0 bytes out to dst_size.
   *
   * 2) IF there is no 0 byte in the first dst_size bytes of src, strncpy will
   *    copy dst_size bytes, and the final byte won't be 0.
   *
   * In GCC 8+, the `-Wstringop-truncation` warning will object to strncpy()
   * being used in this way, so we need to disable this warning for this
   * single statement.
   */

#if defined(__GNUC__) && __GNUC__ >= 8
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wstringop-truncation"
#endif
  strncpy(dst, src, dst_size);
#if defined(__GNUC__) && __GNUC__ >= 8
#pragma GCC diagnostic pop
#endif

  if (dst[dst_size-1])
  {
    /* Only possible in case (2), meaning src was truncated. */
    dst[dst_size-1]= 0;
    return 1;
  }
  return 0;
}

/*
  Appends src to dst and ensures dst is a NULL terminated C string.

  Returns 1 if the src string was truncated due to too small size of dst.
  Returns 0 if src completely fit within the remaining dst space. Pads the
            remaining dst with '\0'.

  Note: dst_size must be > 0
*/
static inline int safe_strcat(char *dst, size_t dst_size, const char *src)
{
  size_t init_len= strlen(dst);
  if (init_len >= dst_size - 1)
    return 1;
  return safe_strcpy(dst + init_len, dst_size - init_len, src);
}

#ifdef __cplusplus
static inline char *safe_str(char *str)
{ return str ? str : const_cast<char*>(""); }
#endif

static inline const char *safe_str(const char *str)
{ return str ? str : ""; }

static inline size_t safe_strlen(const char *str)
{ return str ? strlen(str) : 0; }

#endif
