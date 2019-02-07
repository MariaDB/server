/*
  Copyright (c) 2018 MariaDB Corporation

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301 USA
*/

#ifndef _CTYPE_UTF32_H
#define _CTYPE_UTF32_H

#define MY_UTF32_WC4(b0,b1,b2,b3)  ((((my_wc_t)b0) << 24) + (b1 << 16) + \
                                                (b2 << 8) + (b3))

static inline int
my_mb_wc_utf32_quick(my_wc_t *pwc, const uchar *s, const uchar *e)
{
  if (s + 4 > e)
    return MY_CS_TOOSMALL4;
  *pwc= MY_UTF32_WC4(s[0], s[1], s[2], s[3]);
  return *pwc > 0x10FFFF ? MY_CS_ILSEQ : 4;
}

#endif /* _CTYPE_UTF32_H */
