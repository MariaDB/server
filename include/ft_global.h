/* Copyright (c) 2000-2005, 2007 MySQL AB, 2009 Sun Microsystems, Inc.
   Use is subject to license terms.

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

/* Written by Sergei A. Golubchik, who has a shared copyright to this code */

/* some definitions for full-text indices */

/* #include "myisam.h" */

#ifndef _ft_global_h
#define _ft_global_h

#include <my_compare.h>

#define HA_FT_MAXBYTELEN 254
#define HA_FT_MAXCHARLEN (HA_FT_MAXBYTELEN/3)

#define DEFAULT_FTB_SYNTAX "+ -><()~*:\"\"&|"

#ifdef __cplusplus
extern "C" {
#endif

extern const char *ft_stopword_file;

extern ulong ft_min_word_len;
extern ulong ft_max_word_len;
extern ulong ft_query_expansion_limit;
extern const char *ft_boolean_syntax;

int ft_init_stopwords(void);

#define FT_NL     0
#define FT_BOOL   1
#define FT_SORTED 2
#define FT_EXPAND 4   /* query expansion */

my_bool ft_boolean_check_syntax_string(const uchar *, size_t length,
                                       CHARSET_INFO *cs);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* _ft_global_h */
