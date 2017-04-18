/******************************************************
Copyright (c) 2011-2013 Percona LLC and/or its affiliates.

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

/* This file is required to abstract away regex(3) calls so that
my_regex is used on Windows and native calls are used on POSIX platforms. */

#ifndef XB_REGEX_H
#define XB_REGEX_H

#ifdef _WIN32

#include <my_regex.h>

typedef my_regex_t xb_regex_t;
typedef my_regmatch_t xb_regmatch_t;

#define xb_regex_init() my_regex_init(&my_charset_latin1)

#define xb_regexec(preg,string,nmatch,pmatch,eflags) \
	my_regexec(preg, string, nmatch, pmatch, eflags)

#define xb_regerror(errcode,preg,errbuf,errbuf_size) \
	my_regerror(errcode, preg, errbuf, errbuf_size)

#define xb_regcomp(preg,regex,cflags) \
	my_regcomp(preg, regex, cflags, &my_charset_latin1)

#define xb_regfree(preg) my_regfree(preg)

#define xb_regex_end() my_regex_end()

#else /* ! _WIN32 */

#include <regex.h>

typedef regex_t xb_regex_t;
typedef regmatch_t xb_regmatch_t;

#define xb_regex_init() do { } while(0)

#define xb_regexec(preg,string,nmatch,pmatch,eflags)	\
	regexec(preg, string, nmatch, pmatch, eflags)

#define xb_regerror(errcode,preg,errbuf,errbuf_size)	\
	regerror(errcode, preg, errbuf, errbuf_size)

#define xb_regcomp(preg,regex,cflags)				\
	regcomp(preg, regex, cflags)

#define xb_regfree(preg) regfree(preg)

#define xb_regex_end() do { } while (0)

#endif /* _WIN32 */

#endif /* XB_REGEX_H */
