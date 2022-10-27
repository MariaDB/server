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

#ifndef _CTYPE_UCS2_H
#define _CTYPE_UCS2_H


static inline int
my_mb_wc_ucs2_quick(my_wc_t * pwc, const uchar *s, const uchar *e)
{
  if (s+2 > e) /* Need 2 characters */
    return MY_CS_TOOSMALL2;
  *pwc= ((uchar)s[0]) * 256  + ((uchar)s[1]);
  return 2;
}


#endif /* _CTYPE_UCS2_H */
