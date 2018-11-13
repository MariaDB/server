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

#ifndef _CTYPE_UTF8_H
#define _CTYPE_UTF8_H

/* Detect special bytes and sequences */
#define IS_CONTINUATION_BYTE(c)   (((uchar) (c) ^ 0x80) < 0x40)

/*
  Check MB2 character assuming that b0 is alredy known to be >= 0xC2.
  Use this macro if the caller already checked b0 for:
  - an MB1 character
  - an unused gap between MB1 and MB2HEAD
*/
#define IS_UTF8MB2_STEP2(b0,b1)     (((uchar) (b0) < 0xE0) && \
                                     IS_CONTINUATION_BYTE((uchar) b1))

/*
  Check MB3 character assuming that b0 is already known to be
  in the valid MB3HEAD range [0xE0..0xEF].
*/
#define IS_UTF8MB3_STEP2(b0,b1,b2) (IS_CONTINUATION_BYTE(b1) && \
                                    IS_CONTINUATION_BYTE(b2) && \
                                    ((uchar) b0 >= 0xe1 || (uchar) b1 >= 0xa0))

/*
  Check MB3 character assuming that b0 is already known to be >= 0xE0,
  but is not checked for the high end 0xF0 yet.
  Use this macro if the caller already checked b0 for:
  - an MB1 character
  - an unused gap between MB1 and MB2HEAD
  - an MB2HEAD
*/
#define IS_UTF8MB3_STEP3(b0,b1,b2) (((uchar) (b0) < 0xF0) && \
                                    IS_UTF8MB3_STEP2(b0,b1,b2))

/*
  UTF-8 quick four-byte mask:
  11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
  Encoding allows to encode U+00010000..U+001FFFFF

  The maximum character defined in the Unicode standard is U+0010FFFF.
  Higher characters U+00110000..U+001FFFFF are not used.

  11110000.10010000.10xxxxxx.10xxxxxx == F0.90.80.80 == U+00010000 (min)
  11110100.10001111.10111111.10111111 == F4.8F.BF.BF == U+0010FFFF (max)

  Valid codes:
  [F0][90..BF][80..BF][80..BF]
  [F1][80..BF][80..BF][80..BF]
  [F2][80..BF][80..BF][80..BF]
  [F3][80..BF][80..BF][80..BF]
  [F4][80..8F][80..BF][80..BF]
*/

/*
  Check MB4 character assuming that b0 is already
  known to be in the range [0xF0..0xF4]
*/
#define IS_UTF8MB4_STEP2(b0,b1,b2,b3) (IS_CONTINUATION_BYTE(b1) && \
                                       IS_CONTINUATION_BYTE(b2) && \
                                       IS_CONTINUATION_BYTE(b3) && \
                                       (b0 >= 0xf1 || b1 >= 0x90) && \
                                       (b0 <= 0xf3 || b1 <= 0x8F))
#define IS_UTF8MB4_STEP3(b0,b1,b2,b3) (((uchar) (b0) < 0xF5) && \
                                       IS_UTF8MB4_STEP2(b0,b1,b2,b3))

/* Convert individual bytes to Unicode code points */
#define UTF8MB2_CODE(b0,b1)       (((my_wc_t) ((uchar) b0 & 0x1f) << 6)  |\
                                   ((my_wc_t) ((uchar) b1 ^ 0x80)))
#define UTF8MB3_CODE(b0,b1,b2)    (((my_wc_t) ((uchar) b0 & 0x0f) << 12) |\
                                   ((my_wc_t) ((uchar) b1 ^ 0x80) << 6)  |\
                                   ((my_wc_t) ((uchar) b2 ^ 0x80)))
#define UTF8MB4_CODE(b0,b1,b2,b3) (((my_wc_t) ((uchar) b0 & 0x07) << 18) |\
                                   ((my_wc_t) ((uchar) b1 ^ 0x80) << 12) |\
                                   ((my_wc_t) ((uchar) b2 ^ 0x80) << 6)  |\
                                    (my_wc_t) ((uchar) b3 ^ 0x80))

static inline int
my_mb_wc_utf8mb3_quick(my_wc_t * pwc, const uchar *s, const uchar *e)
{
  uchar c;

  if (s >= e)
    return MY_CS_TOOSMALL;

  c= s[0];
  if (c < 0x80)
  {
    *pwc = c;
    return 1;
  }
  else if (c < 0xc2)
    return MY_CS_ILSEQ;
  else if (c < 0xe0)
  {
    if (s+2 > e) /* We need 2 characters */
      return MY_CS_TOOSMALL2;

    if (!(IS_CONTINUATION_BYTE(s[1])))
      return MY_CS_ILSEQ;

    *pwc= UTF8MB2_CODE(c, s[1]);
    return 2;
  }
  else if (c < 0xf0)
  {
    if (s+3 > e) /* We need 3 characters */
      return MY_CS_TOOSMALL3;

    if (!IS_UTF8MB3_STEP2(c, s[1], s[2]))
      return MY_CS_ILSEQ;

    *pwc= UTF8MB3_CODE(c, s[1], s[2]);
    return 3;
  }
  return MY_CS_ILSEQ;
}


#ifdef HAVE_CHARSET_utf8mb4
static inline int
my_mb_wc_utf8mb4_quick(my_wc_t *pwc, const uchar *s, const uchar *e)
{
  uchar c;

  if (s >= e)
    return MY_CS_TOOSMALL;

  c= s[0];
  if (c < 0x80)
  {
    *pwc= c;
    return 1;
  }
  else if (c < 0xc2)
    return MY_CS_ILSEQ;
  else if (c < 0xe0)
  {
    if (s + 2 > e) /* We need 2 characters */
      return MY_CS_TOOSMALL2;

    if (!(IS_CONTINUATION_BYTE(s[1])))
      return MY_CS_ILSEQ;

    *pwc= UTF8MB2_CODE(c, s[1]);
    return 2;
  }
  else if (c < 0xf0)
  {
    if (s + 3 > e) /* We need 3 characters */
      return MY_CS_TOOSMALL3;

    if (!IS_UTF8MB3_STEP2(c, s[1], s[2]))
      return MY_CS_ILSEQ;

    *pwc= UTF8MB3_CODE(c, s[1], s[2]);
    return 3;
  }
  else if (c < 0xf5)
  {
    if (s + 4 > e) /* We need 4 characters */
      return MY_CS_TOOSMALL4;

    if (!IS_UTF8MB4_STEP2(c, s[1], s[2], s[3]))
      return MY_CS_ILSEQ;
    *pwc= UTF8MB4_CODE(c, s[1], s[2], s[3]);
    return 4;
  }
  return MY_CS_ILSEQ;
}
#endif /* HAVE_CHARSET_utf8mb4*/


#endif /* _CTYPE_UTF8_H */
