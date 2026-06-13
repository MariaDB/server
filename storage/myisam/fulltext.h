/*
   Copyright (c) 2000, 2010, Oracle and/or its affiliates

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

#include "myisamdef.h"
#include "ft_global.h"

/* If HA_FT_MAXLEN is change to 127 or over, it must be tested properly as
   it may cause different representation on disk for full text indexes
*/
#define HA_FT_MAXLEN 126

int  _mi_ft_cmp(MI_INFO *, uint, const uchar *, const uchar *);
int  _mi_ft_add(MI_INFO *, uint, uchar *, const uchar *, my_off_t);
int  _mi_ft_del(MI_INFO *, uint, uchar *, const uchar *, my_off_t);

uint _mi_ft_convert_to_ft2(MI_INFO *, uint, uchar *);

extern const char *ft_precompiled_stopwords[];
extern struct st_mysql_ftparser ft_default_parser;
void ft_free_stopwords(void);

#define HA_FT_WTYPE  HA_KEYTYPE_FLOAT
#define HA_FT_WLEN   4
#define FT_SEGS      2

#define ft_sintXkorr(A)    mi_sint4korr(A)
#define ft_intXstore(T,A)  mi_int4store(T,A)

extern const HA_KEYSEG ft_keysegs[FT_SEGS];

typedef union {int32 i; float f;} FT_WEIGTH;
