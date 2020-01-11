/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010-2016 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "grn.h"
#include "grn_nfkc.h"
#include <groonga/nfkc.h>

#ifdef GRN_WITH_NFKC

grn_char_type
grn_nfkc_char_type(const unsigned char *utf8)
{
  return grn_nfkc50_char_type(utf8);
}

const char *
grn_nfkc_decompose(const unsigned char *utf8)
{
  return grn_nfkc50_decompose(utf8);
}

const char *
grn_nfkc_compose(const unsigned char *prefix_utf8,
                 const unsigned char *suffix_utf8)
{
  return grn_nfkc50_compose(prefix_utf8, suffix_utf8);
}

#endif /* GRN_WITH_NFKC */

