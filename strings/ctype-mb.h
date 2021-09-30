#ifndef CTYPE_MB_INCLUDED
#define CTYPE_MB_INCLUDED
/* Copyright (C) 2021 MariaDB Corporation

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* This file is to be include first in all files in the string directory */

#undef DBUG_ASSERT_AS_PRINTF
#include <my_global.h>		/* Define standard vars */
#include "m_string.h"		/* Exernal definitions of string functions */


size_t
my_min_str_mb_simple(CHARSET_INFO *cs,
                     uchar *dst, size_t dst_size, size_t nchars);

size_t
my_min_str_mb_simple_nopad(CHARSET_INFO *cs,
                           uchar *dst, size_t dst_size, size_t nchars);

size_t
my_max_str_mb_simple(CHARSET_INFO *cs,
                     uchar *dst, size_t dst_size, size_t nchars);

#endif /*CTYPE_MB_INCLUDED */
