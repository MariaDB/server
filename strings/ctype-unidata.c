/* Copyright (c) 2000, 2013, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; version 2
   of the License.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
   MA 02110-1335  USA */

#include "strings_def.h"
#include <m_ctype.h>
#include "ctype-mb.h"

#ifndef EILSEQ
#define EILSEQ ENOENT
#endif


#include "ctype-unidata.h"
#include "ctype-unicode300-general_ci.h"
#include "ctype-unicode300-general_mysql500_ci.h"
#include "ctype-unicode300-casefold.h"
#include "ctype-unicode300-casefold-tr.h"
#include "ctype-unicode520-casefold.h"
#include "ctype-unicode1400-casefold.h"
#include "ctype-unicode1400-casefold-tr.h"



MY_CASEFOLD_INFO my_casefold_default=
{
  0xFFFF,
  my_u300_casefold_index,
  weight_general_ci_index
};


/*
  Turkish lower/upper mapping:
  1. LOWER(0x0049 LATIN CAPITAL LETTER I) ->
           0x0131 LATIN SMALL   LETTER DOTLESS I
  2. UPPER(0x0069 LATIN SMALL   LETTER I) ->
           0x0130 LATIN CAPITAL LETTER I WITH DOT ABOVE
*/

MY_CASEFOLD_INFO my_casefold_turkish=
{
  0xFFFF,
  my_u300tr_casefold_index,
  weight_general_ci_index
};


/*
  general_mysql500_ci is very similar to general_ci, but maps sorting order
  for U+00DF to 0x00DF instead of 0x0053.
*/
MY_CASEFOLD_INFO my_casefold_mysql500=
{
  0xFFFF,
  my_u300_casefold_index,
  weight_general_mysql500_ci_index
};



MY_CASEFOLD_INFO my_casefold_unicode520=
{
  0x10FFFF,
  my_u520_casefold_index,
  NULL
};


MY_CASEFOLD_INFO my_casefold_unicode1400=
{
  0x10FFFF,
  my_u1400_casefold_index,
  NULL
};


MY_CASEFOLD_INFO my_casefold_unicode1400tr=
{
  0x10FFFF,
  my_u1400tr_casefold_index,
  NULL
};
