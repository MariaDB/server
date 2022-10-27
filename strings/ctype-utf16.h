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

#ifndef _CTYPE_UTF16_H
#define _CTYPE_UTF16_H

/*
  D800..DB7F - Non-provate surrogate high (896 pages)
  DB80..DBFF - Private surrogate high     (128 pages)
  DC00..DFFF - Surrogate low              (1024 codes in a page)
*/
#define MY_UTF16_SURROGATE_HIGH_FIRST 0xD800
#define MY_UTF16_SURROGATE_HIGH_LAST  0xDBFF
#define MY_UTF16_SURROGATE_LOW_FIRST  0xDC00
#define MY_UTF16_SURROGATE_LOW_LAST   0xDFFF

#define MY_UTF16_HIGH_HEAD(x)      ((((uchar) (x)) & 0xFC) == 0xD8)
#define MY_UTF16_LOW_HEAD(x)       ((((uchar) (x)) & 0xFC) == 0xDC)
/* Test if a byte is a leading byte of a high or low surrogate head: */
#define MY_UTF16_SURROGATE_HEAD(x) ((((uchar) (x)) & 0xF8) == 0xD8)
/* Test if a Unicode code point is a high or low surrogate head */
#define MY_UTF16_SURROGATE(x)      (((x) & 0xF800) == 0xD800)

#define MY_UTF16_WC2(a, b)         ((a << 8) + b)

/*
  a= 110110??  (<< 18)
  b= ????????  (<< 10)
  c= 110111??  (<<  8)
  d= ????????  (<<  0)
*/
#define MY_UTF16_WC4(a, b, c, d) (((a & 3) << 18) + (b << 10) + \
                                  ((c & 3) << 8) + d + 0x10000)

static inline int
my_mb_wc_utf16_quick(my_wc_t *pwc, const uchar *s, const uchar *e)
{
  if (s + 2 > e)
    return MY_CS_TOOSMALL2;

  /*
    High bytes: 0xD[89AB] = B'110110??'
    Low bytes:  0xD[CDEF] = B'110111??'
    Surrogate mask:  0xFC = B'11111100'
  */

  if (MY_UTF16_HIGH_HEAD(*s)) /* Surrogate head */
  {
    if (s + 4 > e)
      return MY_CS_TOOSMALL4;

    if (!MY_UTF16_LOW_HEAD(s[2]))  /* Broken surrigate pair */
      return MY_CS_ILSEQ;

    *pwc= MY_UTF16_WC4(s[0], s[1], s[2], s[3]);
    return 4;
  }

  if (MY_UTF16_LOW_HEAD(*s)) /* Low surrogate part without high part */
    return MY_CS_ILSEQ;

  *pwc= MY_UTF16_WC2(s[0], s[1]);
  return 2;
}

#endif /* _CTYPE_UTF16_H */
