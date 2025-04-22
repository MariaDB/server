/* Copyright (c) 2010, Oracle and/or its affiliates
   Copyright (c) 2009, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#include <tap.h>
#include <my_global.h>
#include <my_sys.h>


/*
  U+00DF LATIN SMALL LETTER SHARP S = _utf8 x'C39F' = _latin1 x'DF'
*/

#define UTF8_sz   "\xC3\x9F"
#define LATIN1_sz "\xDF"

/*
  U+00C5 LATIN CAPITAL LETTER A WITH RING ABOVE = _utf8 x'C385'
*/

#define UTF8_ARING "\xC3\x85"

/*
  U+00E4 LATIN SMALL LETTER A WITH DIAERESIS = _utf8 x'C3A4'
*/
#define UTF8_auml   "\xC3\xA4"
#define LATIN1_auml "\xE4"

#define UCS2_a  "\x00\x61"
#define UCS2_b  "\x00\x62"
#define UCS2_sp "\x00\x20"


/*
  Test that like_range() returns well-formed results.
*/
static int
test_like_range_for_charset(CHARSET_INFO *cs, const char *src, size_t src_len)
{
  char min_str[32], max_str[32];
  size_t min_len, max_len, min_well_formed_len, max_well_formed_len;
  int error= 0;

  my_ci_like_range(cs, src, src_len, '\\', '_', '%',
                   sizeof(min_str),  min_str, max_str, &min_len, &max_len);
  diag("min_len=%d\tmax_len=%d\t%s", (int) min_len, (int) max_len,
       cs->coll_name.str);
  min_well_formed_len= my_well_formed_length(cs,
                                             min_str, min_str + min_len,
                                             10000, &error);
  max_well_formed_len= my_well_formed_length(cs,
                                             max_str, max_str + max_len,
                                             10000, &error);
  if (min_len != min_well_formed_len)
    diag("Bad min_str: min_well_formed_len=%d min_str[%d]=0x%02X",
          (int) min_well_formed_len, (int) min_well_formed_len,
          (uchar) min_str[min_well_formed_len]);
  if (max_len != max_well_formed_len)
    diag("Bad max_str: max_well_formed_len=%d max_str[%d]=0x%02X",
          (int) max_well_formed_len, (int) max_well_formed_len,
          (uchar) max_str[max_well_formed_len]);
  return
    min_len == min_well_formed_len &&
    max_len == max_well_formed_len ? 0 : 1;
}


static CHARSET_INFO *charset_list[]=
{
#ifdef HAVE_CHARSET_big5
  &my_charset_big5_chinese_ci,
  &my_charset_big5_bin,
#endif
#ifdef HAVE_CHARSET_euckr
  &my_charset_euckr_korean_ci,
  &my_charset_euckr_bin,
#endif
#ifdef HAVE_CHARSET_gb2312
  &my_charset_gb2312_chinese_ci,
  &my_charset_gb2312_bin,
#endif
#ifdef HAVE_CHARSET_gbk
  &my_charset_gbk_chinese_ci,
  &my_charset_gbk_bin,
#endif
#ifdef HAVE_CHARSET_latin1
  &my_charset_latin1,
  &my_charset_latin1_bin,
#endif
#ifdef HAVE_CHARSET_sjis
  &my_charset_sjis_japanese_ci,
  &my_charset_sjis_bin,
#endif
#ifdef HAVE_CHARSET_tis620
  &my_charset_tis620_thai_ci,
  &my_charset_tis620_bin,
#endif
#ifdef HAVE_CHARSET_ujis
  &my_charset_ujis_japanese_ci,
  &my_charset_ujis_bin,
#endif
#ifdef HAVE_CHARSET_utf8mb3
  &my_charset_utf8mb3_general_ci,
#ifdef HAVE_UCA_COLLATIONS
  &my_charset_utf8mb3_unicode_ci,
#endif
  &my_charset_utf8mb3_bin,
#endif
};


typedef struct
{
  const char *a;
  size_t alen;
  const char *b;
  size_t blen;
  int res;
} STRNNCOLL_PARAM;


#define CSTR(x)  (x),(sizeof(x)-1)

/*
  Byte sequence types used in the tests:
    8BIT     - a 8 bit byte (>=00x80) which makes a single byte characters
    MB2      - two bytes that make a valid character
    H2       - a byte which is a valid MB2 head byte
    T2       - a byte which is a valid MB2 tail byte
    ILSEQ    - a byte which makes an illegal sequence
    H2+ILSEQ - a sequence that starts with a valid H2 byte,
               but not followed by a valid T2 byte.

  Charset H2               T2                      8BIT
  ------- ---------------- ---------------         --------
  big5    [A1..F9]         [40..7E,A1..FE]
  euckr   [81..FE]         [41..5A,61..7A,81..FE]
  gb2312  [A1..F7]         [A1..FE]
  gbk     [81..FE]         [40..7E,80..FE]

  cp932   [81..9F,E0..FC]  [40..7E,80..FC]         [A1..DF]
  sjis    [81..9F,E0..FC]  [40..7E,80..FC]         [A1..DF]


  Essential byte sequences in various character sets:

  Sequence  big5   cp932      euckr  gb2312    gbk   sjis
  --------  ----   -----      -----  ------    ---   ----
  80        ILSEQ  ILSEQ      ILSEQ  ILSEQ     ILSEQ ILSEQ
  81        ILSEQ  H2         H2     ILSEQ     H2    H2
  A1        H2     8BIT       H2     H2        H2    8BIT
  A1A1      MB2    8BIT+8BIT  MB2    MB2       MB2   8BIT+8BIT
  E0E0      MB2    MB2        MB2    MB2       MB2   MB2
  F9FE      MB2    H2+ILSEQ   MB2    ILSEQ+T2  MB2   H2+ILSEQ
*/


/*
  For character sets that have the following byte sequences:
    80   - ILSEQ
    81   - ILSEQ or H2
    F9   - ILSEQ or H2
    A1A1 - MB2 or 8BIT+8BIT
    E0E0 - MB2
*/
static STRNNCOLL_PARAM strcoll_mb2_common[]=
{
  /* Compare two good sequences */
  {CSTR(""),         CSTR(""),           0},
  {CSTR(""),         CSTR(" "),          0},
  {CSTR(""),         CSTR("A"),         -1},
  {CSTR(""),         CSTR("a"),         -1},
  {CSTR(""),         CSTR("\xA1\xA1"),  -1},
  {CSTR(""),         CSTR("\xE0\xE0"),  -1},

  {CSTR(" "),        CSTR(""),          0},
  {CSTR(" "),        CSTR(" "),         0},
  {CSTR(" "),        CSTR("A"),        -1},
  {CSTR(" "),        CSTR("a"),        -1},
  {CSTR(" "),        CSTR("\xA1\xA1"), -1},
  {CSTR(" "),        CSTR("\xE0\xE0"), -1},

  {CSTR("a"),        CSTR(""),          1},
  {CSTR("a"),        CSTR(" "),         1},
  {CSTR("a"),        CSTR("a"),         0},
  {CSTR("a"),        CSTR("\xA1\xA1"), -1},
  {CSTR("a"),        CSTR("\xE0\xE0"), -1},

  {CSTR("\xA1\xA1"), CSTR("\xA1\xA1"),  0},
  {CSTR("\xA1\xA1"), CSTR("\xE0\xE0"), -1},

  /* Compare a good character to an illegal or an incomplete sequence */
  {CSTR(""),         CSTR("\x80"),     -1},
  {CSTR(""),         CSTR("\x81"),     -1},
  {CSTR(""),         CSTR("\xF9"),     -1},

  {CSTR(" "),        CSTR("\x80"),     -1},
  {CSTR(" "),        CSTR("\x81"),     -1},
  {CSTR(" "),        CSTR("\xF9"),     -1},

  {CSTR("a"),        CSTR("\x80"),     -1},
  {CSTR("a"),        CSTR("\x81"),     -1},
  {CSTR("a"),        CSTR("\xF9"),     -1},

  {CSTR("\xA1\xA1"), CSTR("\x80"),     -1},
  {CSTR("\xA1\xA1"), CSTR("\x81"),     -1},
  {CSTR("\xA1\xA1"), CSTR("\xF9"),     -1},

  {CSTR("\xE0\xE0"), CSTR("\x80"),     -1},
  {CSTR("\xE0\xE0"), CSTR("\x81"),     -1},
  {CSTR("\xE0\xE0"), CSTR("\xF9"),     -1},

  /* Compare two bad/incomplete sequences */
  {CSTR("\x80"),     CSTR("\x80"),      0},
  {CSTR("\x80"),     CSTR("\x81"),     -1},
  {CSTR("\x80"),     CSTR("\xF9"),     -1},
  {CSTR("\x81"),     CSTR("\x81"),      0},
  {CSTR("\x81"),     CSTR("\xF9"),     -1},

  {NULL, 0, NULL, 0, 0}
};


/*
  For character sets that have good mb2 characters A1A1 and F9FE
*/
static STRNNCOLL_PARAM strcoll_mb2_A1A1_mb2_F9FE[]=
{
  /* Compare two good characters */
  {CSTR(""),         CSTR("\xF9\xFE"), -1},
  {CSTR(" "),        CSTR("\xF9\xFE"), -1},
  {CSTR("a")       , CSTR("\xF9\xFE"), -1},
  {CSTR("\xA1\xA1"), CSTR("\xF9\xFE"), -1},
  {CSTR("\xF9\xFE"), CSTR("\xF9\xFE"),  0},

  /* Compare a good character to an illegal or an incomplete sequence */
  {CSTR(""),         CSTR("\xA1"),     -1},
  {CSTR(""),         CSTR("\xF9"),     -1},
  {CSTR("a"),        CSTR("\xA1"),     -1},
  {CSTR("a"),        CSTR("\xF9"),     -1},

  {CSTR("\xA1\xA1"), CSTR("\xA1"),     -1},
  {CSTR("\xA1\xA1"), CSTR("\xF9"),     -1},

  {CSTR("\xF9\xFE"), CSTR("\x80"),     -1},
  {CSTR("\xF9\xFE"), CSTR("\x81"),     -1},
  {CSTR("\xF9\xFE"), CSTR("\xA1"),     -1},
  {CSTR("\xF9\xFE"), CSTR("\xF9"),     -1},

  /* Compare two bad/incomplete sequences */
  {CSTR("\x80"),     CSTR("\xA1"),     -1},
  {CSTR("\x80"),     CSTR("\xF9"),     -1},

  {NULL, 0, NULL, 0, 0}
};


/*
  For character sets that have:
    A1A1 - a good mb2 character
    F9FE - a bad sequence
*/
static STRNNCOLL_PARAM strcoll_mb2_A1A1_bad_F9FE[]=
{
  /* Compare a good character to an illegal or an incomplete sequence */
  {CSTR(""),         CSTR("\xF9\xFE"), -1},
  {CSTR(" "),        CSTR("\xF9\xFE"), -1},
  {CSTR("a")       , CSTR("\xF9\xFE"), -1},
  {CSTR("\xA1\xA1"), CSTR("\xF9\xFE"), -1},

  {CSTR(""),         CSTR("\xA1"),     -1},
  {CSTR(""),         CSTR("\xF9"),     -1},
  {CSTR("a"),        CSTR("\xA1"),     -1},
  {CSTR("a"),        CSTR("\xF9"),     -1},

  {CSTR("\xA1\xA1"), CSTR("\xA1"),     -1},
  {CSTR("\xA1\xA1"), CSTR("\xF9"),     -1},

  /* Compare two bad/incomplete sequences */
  {CSTR("\xF9\xFE"), CSTR("\x80"),     1},
  {CSTR("\xF9\xFE"), CSTR("\x81"),     1},
  {CSTR("\xF9\xFE"), CSTR("\xA1"),     1},
  {CSTR("\xF9\xFE"), CSTR("\xF9"),     1},
  {CSTR("\x80"),     CSTR("\xA1"),     -1},
  {CSTR("\x80"),     CSTR("\xF9"),     -1},
  {CSTR("\xF9\xFE"), CSTR("\xF9\xFE"),  0},

  {NULL, 0, NULL, 0, 0}
};


/*
  For character sets that have:
    80   - ILSEQ or H2
    81   - ILSEQ or H2
    A1   - 8BIT
    F9   - ILSEQ or H2
    F9FE - a bad sequence (ILSEQ+XX or H2+ILSEQ)
*/
static STRNNCOLL_PARAM strcoll_mb1_A1_bad_F9FE[]=
{
  /* Compare two good characters */
  {CSTR(""),         CSTR("\xA1"),     -1},
  {CSTR("\xA1\xA1"), CSTR("\xA1"),      1},

  /* Compare a good character to an illegal or an incomplete sequence */
  {CSTR(""),         CSTR("\xF9"),     -1},
  {CSTR(""),         CSTR("\xF9\xFE"), -1},
  {CSTR(" "),        CSTR("\xF9\xFE"), -1},
  {CSTR("a"),        CSTR("\xF9\xFE"), -1},
  {CSTR("a"),        CSTR("\xA1"),     -1},
  {CSTR("a"),        CSTR("\xF9"),     -1},

  {CSTR("\xA1\xA1"), CSTR("\xF9"),     -1},
  {CSTR("\xA1\xA1"), CSTR("\xF9\xFE"), -1},

  {CSTR("\xF9\xFE"), CSTR("\x80"),     1},
  {CSTR("\xF9\xFE"), CSTR("\x81"),     1},
  {CSTR("\xF9\xFE"), CSTR("\xA1"),     1},
  {CSTR("\xF9\xFE"), CSTR("\xF9"),     1},

  {CSTR("\x80"),     CSTR("\xA1"),      1},

  /* Compare two bad/incomplete sequences */
  {CSTR("\x80"),     CSTR("\xF9"),     -1},
  {CSTR("\xF9\xFE"), CSTR("\xF9\xFE"),  0},

  {NULL, 0, NULL, 0, 0}
};


/*
  For character sets (e.g. cp932 and sjis) that have:
    8181 - a valid MB2 character
    A1   - a valid 8BIT character
    E0E0 - a valid MB2 character
  and sort in this order:
    8181 < A1 < E0E0
*/
static STRNNCOLL_PARAM strcoll_8181_A1_E0E0[]=
{
  {CSTR("\x81\x81"), CSTR("\xA1"),     -1},
  {CSTR("\x81\x81"), CSTR("\xE0\xE0"), -1},
  {CSTR("\xA1"),     CSTR("\xE0\xE0"), -1},

  {NULL, 0, NULL, 0, 0}
};


/*
  A shared test for eucjpms and ujis.
*/
static STRNNCOLL_PARAM strcoll_ujis[]=
{
  {CSTR("\x8E\xA1"), CSTR("\x8E"),     -1},     /* Good MB2 vs incomplete MB2 */
  {CSTR("\x8E\xA1"), CSTR("\x8F\xA1"), -1},     /* Good MB2 vs incomplete MB3 */
  {CSTR("\x8E\xA1"), CSTR("\x8F\xA1\xA1"), -1}, /* Good MB2 vs good MB3 */
  {CSTR("\xA1\xA1"), CSTR("\x8F\xA1\xA1"),  1}, /* Good MB2 vs good MB3 */
  {CSTR("\x8E"),     CSTR("\x8F\xA1"), -1}, /* Incomplete MB2 vs incomplete MB3 */
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb3_common[]=
{
  {CSTR("\xC0"),     CSTR("\xC1"),        -1},    /* Unused byte vs unused byte */
  {CSTR("\xC0"),     CSTR("\xFF"),        -1},    /* Unused byte vs unused byte */
  {CSTR("\xC2\xA1"), CSTR("\xC0"),        -1},    /* MB2 vs unused byte */
  {CSTR("\xC2\xA1"), CSTR("\xC2"),        -1},    /* MB2 vs incomplete MB2 */
  {CSTR("\xC2\xA1"), CSTR("\xC2\xA2"),    -1},    /* MB2 vs MB2 */
  {CSTR("\xC2\xA1"), CSTR("\xE0\xA0\x7F"),-1},    /* MB2 vs broken MB3 */
  {CSTR("\xC2\xA1"), CSTR("\xE0\xA0\x80"),-1},    /* MB2 vs MB3 */
  {CSTR("\xC2\xA1"), CSTR("\xE0\xA0\xBF"),-1},    /* MB2 vs MB3 */
  {CSTR("\xC2\xA1"), CSTR("\xE0\xA0\xC0"),-1},    /* MB2 vs broken MB3 */
  {CSTR("\xC2\xA1"), CSTR("\xE0\xA0"),     -1},   /* MB2 vs incomplete MB3 */
  {CSTR("\xE0\xA0\x7E"), CSTR("\xE0\xA0\x7F"),-1},/* Broken MB3 vs broken MB3 */
  {CSTR("\xE0\xA0\x80"), CSTR("\xE0\xA0"),    -1},/* MB3 vs incomplete MB3 */
  {CSTR("\xE0\xA0\x80"), CSTR("\xE0\xA0\x7F"),-1},/* MB3 vs broken MB3 */
  {CSTR("\xE0\xA0\x80"), CSTR("\xE0\xA0\xBF"),-1},/* MB3 vs MB3 */
  {CSTR("\xE0\xA0\x80"), CSTR("\xE0\xA0\xC0"),-1},/* MB3 vs broken MB3 */
  {CSTR("\xE0\xA0\xC0"), CSTR("\xE0\xA0\xC1"),-1},/* Broken MB3 vs broken MB3 */
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb4_common[]=
{
  /* Minimum four-byte character: U+10000 == _utf8 0xF0908080 */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xC0"),        -1},  /* MB4 vs unused byte */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xC2"),        -1},  /* MB4 vs incomplete MB2 */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xE0\xA0\x7F"),-1},  /* MB4 vs broken MB3 */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xE0\xA0\xC0"),-1},  /* MB4 vs broken MB3 */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xE0\xA0"),     -1}, /* MB4 vs incomplete MB3 */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xF0\x90\x80"),-1},  /* MB4 vs incomplete MB4 */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xF0\x90\x80\x7F"),-1},/* MB4 vs broken MB4 */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xF0\x90\x80\xC0"),-1},/* MB4 vs broken MB4 */

  /* Maximum four-byte character: U+10FFFF == _utf8 0xF48FBFBF */
  {CSTR("\xF4\x8F\xBF\xBF"), CSTR("\xC0"),        -1},  /* MB4 vs unused byte */
  {CSTR("\xF4\x8F\xBF\xBF"), CSTR("\xC2"),        -1},  /* MB4 vs incomplete MB2 */
  {CSTR("\xF4\x8F\xBF\xBF"), CSTR("\xE0\xA0\x7F"),-1},  /* MB4 vs broken MB3 */
  {CSTR("\xF4\x8F\xBF\xBF"), CSTR("\xE0\xA0\xC0"),-1},  /* MB4 vs broken MB3 */
  {CSTR("\xF4\x8F\xBF\xBF"), CSTR("\xE0\xA0"),     -1}, /* MB4 vs incomplete MB3 */
  {CSTR("\xF4\x8F\xBF\xBF"), CSTR("\xF0\x90\x80"),-1},  /* MB4 vs incomplete MB4 */
  {CSTR("\xF4\x8F\xBF\xBF"), CSTR("\xF0\x90\x80\x7F"),-1},/* MB4 vs broken MB4 */
  {CSTR("\xF4\x8F\xBF\xBF"), CSTR("\xF0\x90\x80\xC0"),-1},/* MB4 vs broken MB4 */

  /* Broken MB4 vs incomplete/broken MB3 */
  {CSTR("\xF0\x90\x80\x7F"), CSTR("\xE0\xA0"),    1},  /* Broken MB4 vs incomplete MB3 */
  {CSTR("\xF0\x90\x80\x7F"), CSTR("\xE0\xA0\x7F"),1},  /* Broken MB4 vs broken MB3 */
  {CSTR("\xF0\x90\x80\x7F"), CSTR("\xE0\xA0\xC0"),1},  /* Broken MB4 vs broken MB3 */

  /*
    Broken MB4 vs incomplete MB4:
    The three leftmost bytes are compared binary, the fourth byte is compared
    to auto-padded space.
  */
  {CSTR("\xF0\x90\x80\x1F"), CSTR("\xF0\x90\x80"),-1}, /* Broken MB4 vs incomplete MB4 */
  {CSTR("\xF0\x90\x80\x7E"), CSTR("\xF0\x90\x80"),1},  /* Broken MB4 vs incomplete MB4 */

  /* Broken MB4 vs broken MB4 */
  {CSTR("\xF0\x90\x80\x7E"), CSTR("\xF0\x90\x80\x7F"),-1},/* Broken MB4 vs broken MB4 */
  {CSTR("\xF0\x90\x80\x7E"), CSTR("\xF0\x90\x80\xC0"),-1},/* Broken MB4 vs broken MB4 */

  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb4_general_ci[]=
{
  /* All non-BMP characters are equal in utf8mb4_general_ci */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xF0\x90\x80\x81"),0},/* Non-BMB MB4 vs non-BMP MB4 */
  {CSTR("\xF0\x90\x80\x80"), CSTR("\xF4\x8F\xBF\xBF"),0},/* Non-BMB MB4 vs non-BMP MB4 */
  {CSTR("\x00"),             CSTR("\xF0\x90\x80\x80"),-1},/* U+0000 vs non-BMP MB4 */
  {CSTR("\x00"),             CSTR("\xF0\x90\x80\x81"),-1},/* U+0000 vs non-BMP MB4 */
  {CSTR("\x00"),             CSTR("\xF4\x8F\xBF\xBF"),-1},/* U+0000 vs non-BMP MB4 */
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_ucs2_common[]=
{
  {CSTR("\xC0"),     CSTR("\xC1"),        -1},    /* Incomlete MB2 vs incomplete MB2 */
  {CSTR("\xC0"),     CSTR("\xFF"),        -1},    /* Incomlete MB2 vs incomplete MB2 */
  {CSTR("\xC2\xA1"), CSTR("\xC0"),        -1},    /* MB2 vs incomplete MB2 */
  {CSTR("\xC2\xA1"), CSTR("\xC2"),        -1},    /* MB2 vs incomplete MB2 */
  {CSTR("\xC2\xA0"), CSTR("\xC2\xA1"),    -1},    /* MB2 vs MB2 */
  {CSTR("\xC2\xA1"), CSTR("\xC2\xA2"),    -1},    /* MB2 vs MB2 */

  {CSTR("\xFF\xFF"),         CSTR("\x00"),-1},        /* MB2  vs incomplete */
  {CSTR("\xFF\xFF\xFF\xFF"), CSTR("\x00"),-1},        /* MB2+MB2 vs incomplete */
  {CSTR("\xFF\xFF\xFF\xFF"), CSTR("\x00\x00\x00"), 1},/* MB2+MB2 vs MB2+incomplete */

  {NULL, 0, NULL, 0, 0}
};


/* Tests that involve comparison to SPACE (explicit, or padded) */
static STRNNCOLL_PARAM strcoll_ucs2_space[]=
{
  {CSTR("\x00\x1F"), CSTR("\x00\x20"),    -1},    /* MB2 vs MB2 */
  {CSTR("\x00\x20"), CSTR("\x00\x21"),    -1},    /* MB2 vs MB2 */
  {CSTR("\x00\x1F"), CSTR(""),            -1},    /* MB2 vs empty */
  {CSTR("\x00\x20"), CSTR(""),             0},    /* MB2 vs empty */
  {CSTR("\x00\x21"), CSTR(""),             1},    /* MB2 vs empty */

  {NULL, 0, NULL, 0, 0}
};


/* Tests that involve comparison to SPACE (explicit, or padded) */
static STRNNCOLL_PARAM strcoll_utf16le_space[]=
{
  {CSTR("\x1F\x00"), CSTR("\x20\x00"),  -1},    /* MB2 vs MB2 */
  {CSTR("\x20\x00"), CSTR("\x21\x00"),  -1},    /* MB2 vs MB2 */
  {CSTR("\x1F\x00"), CSTR(""),          -1},    /* MB2 vs empty */
  {CSTR("\x20\x00"), CSTR(""),           0},    /* MB2 vs empty */
  {CSTR("\x21\x00"), CSTR(""),           1},    /* MB2 vs empty */

  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf16_common[]=
{
  /* Minimum four-byte character: U+10000 == _utf16 0xD800DC00 */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xC0"),            -1},/* MB4 vs incomplete MB2 */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xC2"),            -1},/* MB4 vs incomplete MB2 */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xD8\x00\xDB\x00"),-1},/* MB4 vs broken MB4 */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xD8\x00\xE0\x00"),-1},/* MB4 vs broken MB4 */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xDC\x00"),        -1},/* MB4 vs broken MB2 */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xD8\x00\xDC"),    -1},/* MB4 vs incomplete MB4 */

  /* Maximum four-byte character: U+10FFFF == _utf8 0xF48FBFBF */
  {CSTR("\xDB\xFF\xDF\xFF"), CSTR("\xC0"),            -1},/* MB4 vs incomplete MB2 */
  {CSTR("\xDB\xFF\xDF\xFF"), CSTR("\xC2"),            -1},/* MB4 vs incomplete MB2 */
  {CSTR("\xDB\xFF\xDF\xFF"), CSTR("\xD8\x00\xDB\x00"),-1},/* MB4 vs broken MB4 */
  {CSTR("\xDB\xFF\xDF\xFF"), CSTR("\xD8\x00\xE0\x00"),-1},/* MB4 vs broken MB4 */
  {CSTR("\xDB\xFF\xDF\xFF"), CSTR("\xDC\x00"),        -1},/* MB4 vs broken MB2 */
  {CSTR("\xDB\xFF\xDF\xFF"), CSTR("\xDC\xFF\xDF"),    -1},/* MB4 vs incomplete MB4 */

  /* Broken MB4 vs broken MB4 */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xD8\x00\xDB\x01"),-1},/* Broken MB4 vs broken MB4 */
  {CSTR("\xDB\xFF\xE0\xFE"), CSTR("\xDB\xFF\xE0\xFF"),-1},/* Broken MB4 vs broken MB4 */

  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf16_general_ci[]=
{
  /* All non-BMP characters are compared as equal */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xD8\x00\xDC\x01"), 0},/* Non-BMP MB4 vs non-BMP MB4 */
  {CSTR("\xD8\x00\xDC\x00"), CSTR("\xDB\xFF\xDF\xFF"), 0},/* Non-BMP MB4 vs non-BMP MB4 */
  {CSTR("\x00\x00"),         CSTR("\xD8\x00\xDC\x01"),-1},/* U+0000 vs non-BMP MB4 */
  {CSTR("\x00\x00"),         CSTR("\xDB\xFF\xDF\xFF"),-1},/* U+0000 vs non-BMP MB4 */
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf16le_common[]=
{
  /* Minimum four-byte character: U+10000 == _utf16 0xD800DC00 */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\xC0"),            -1},/* MB4 vs incomplete MB2 */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\xC2"),            -1},/* MB4 vs incomplete MB2 */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\x00\xD8\x00\xDB"),-1},/* MB4 vs broken MB4 */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\x00\xD8\x00\xD0"),-1},/* MB4 vs broken MB4 */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\x00\xDC"),        -1},/* MB4 vs broken MB2 */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\x00\xD8\x00"),    -1},/* MB4 vs incomplete MB4 */

  /* Maximum four-byte character: U+10FFFF == _utf8 0xF48FBFBF */
  {CSTR("\xFF\xDB\xFF\xDF"), CSTR("\xC0"),            -1},/* MB4 vs incomplete MB2 */
  {CSTR("\xFF\xDB\xFF\xDF"), CSTR("\xC2"),            -1},/* MB4 vs incomplete MB2 */
  {CSTR("\xFF\xDB\xFF\xDF"), CSTR("\x00\xD8\x00\xDB"),-1},/* MB4 vs broken MB4 */
  {CSTR("\xFF\xDB\xFF\xDF"), CSTR("\x00\xD8\x00\xE0"),-1},/* MB4 vs broken MB4 */
  {CSTR("\xFF\xDB\xFF\xDF"), CSTR("\x00\xDC"),        -1},/* MB4 vs broken MB2 */
  {CSTR("\xFF\xDB\xFF\xDF"), CSTR("\xFF\xDC\x00"),    -1},/* MB4 vs incomplete MB4 */

  /* Broken MB4 vs broken MB4 */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\x00\xD8\x01\xDB"),-1},/* Broken MB4 vs broken MB4 */
  {CSTR("\xFF\xDB\xFE\xE0"), CSTR("\xFF\xDB\xFF\xE0"),-1},/* Broken MB4 vs broken MB4 */

  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf16le_general_ci[]=
{
  /* All non-BMP characters are compared as equal */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\x00\xD8\x01\xDC"), 0},/* Non-BMP MB4 vs non-BMP MB4 */
  {CSTR("\x00\xD8\x00\xDC"), CSTR("\xFF\xDB\xFF\xDF"), 0},/* Non-BMP MB4 vs non-BMP MB4 */
  {CSTR("\x00\x00"), CSTR("\x00\xD8\x01\xDC"),        -1},/* U+0000 vs non-BMP MB4 */
  {CSTR("\x00\x00"), CSTR("\xFF\xDB\xFF\xDF"),        -1},/* U+0000 vs non-BMP MB4 */

  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf32_common[]=
{
  /* Minimum character: U+0000 == _utf32 0x00000000 */
  {CSTR("\x00\x00\x00\x00"), CSTR("\x00"),        -1},    /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x00\x00\x00"), CSTR("\xFF"),        -1},    /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x00\x00\x00"), CSTR("\x00\x00"),    -1},    /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x00\x00\x00"), CSTR("\x00\x00\x00"),-1},    /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x00\x00\x00"), CSTR("\x00\x20\x00\x00"),-1},/* MB4 vs broken MB4 */
  {CSTR("\x00\x00\x00\x00"), CSTR("\xFF\xFF\xFF\xFF"),-1},/* MB4 vs broken MB4 */

  /* Minimum non-BMP character: U+10000 == _utf32 0x00010000 */
  {CSTR("\x00\x01\x00\x00"), CSTR("\x00"),        -1},    /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x01\x00\x00"), CSTR("\xFF"),        -1},    /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x01\x00\x00"), CSTR("\x00\x00"),    -1},    /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x01\x00\x00"), CSTR("\x00\x00\x00"),-1},    /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x01\x00\x00"), CSTR("\x00\x20\x00\x00"),-1},/* MB4 vs broken MB4 */
  {CSTR("\x00\x01\x00\x00"), CSTR("\xFF\xFF\xFF\xFF"),-1},/* MB4 vs broken MB4 */

  /* Maximum character: U+10FFFF == _utf32 0x0010FFFF */
  {CSTR("\x00\x10\xFF\xFF"), CSTR("\x00"),         -1},   /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x10\xFF\xFF"), CSTR("\xFF"),         -1},   /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x10\xFF\xFF"), CSTR("\x00\x00"),     -1},   /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x10\xFF\xFF"), CSTR("\x00\x00\x00"), -1},   /* MB4 vs incomplete MB4 */
  {CSTR("\x00\x10\xFF\xFF"), CSTR("\x20\x00\x00\x00"),-1},/* MB4 vs broken MB3 */
  {CSTR("\x00\x10\xFF\xFF"), CSTR("\xFF\xFF\xFF\xFF"),-1},/* MB4 vs broken MB4 */


  /* Broken MB4 vs incomplete/broken MB3 */
  {CSTR("\x00\x20\x00\x00"), CSTR("\x00"),         1},    /* Broken MB4 vs incomplete MB4 */
  {CSTR("\x00\x20\x00\x00"), CSTR("\x00\x00"),     1},    /* Broken MB4 vs incomplete MB4 */
  {CSTR("\x00\x20\x00\x00"), CSTR("\x00\x00\x00"), 1},    /* Broken MB4 vs incomplete MB4 */
  {CSTR("\x00\x20\x00\x00"), CSTR("\x00\x20\x00\x01"),-1},/* Broken MB4 vs broken MB4 */

  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf32_general_ci[]=
{
  /* Two non-BMP characters are compared as equal */
  {CSTR("\x00\x01\x00\x00"), CSTR("\x00\x01\x00\x01"),  0}, /* non-BMP MB4 vs non-BMP MB4 */
  {CSTR("\x00\x00\x00\x00"), CSTR("\x00\x01\x00\x00"), -1}, /* U+0000 vs non-BMP MB4 */
  {CSTR("\x00\x00\x00\x00"), CSTR("\x00\x01\x00\x01"), -1}, /* U+0000 vs non-BMP MB4 */

  {NULL, 0, NULL, 0, 0}
};


static void
str2hex(char *dst, size_t dstlen, const char *src, size_t srclen)
{
  char *dstend= dst + dstlen;
  const char *srcend= src + srclen;
  for (*dst= '\0' ; dst + 3 < dstend && src < srcend; )
  {
    sprintf(dst, "%02X", (unsigned char) src[0]);
    dst+=2;
    src++;
  }
}


/*
  Check if the two comparison result are semantically equal:
  both are negative, both are positive, or both are zero.
*/
static int
eqres(int ares, int bres)
{
  return (ares < 0 && bres < 0) ||
         (ares > 0 && bres > 0) ||
         (ares == 0 && bres == 0);
}


static int
strcollsp(CHARSET_INFO *cs, const STRNNCOLL_PARAM *param)
{
  int failed= 0;
  const STRNNCOLL_PARAM *p;
  diag("%-20s %-10s %-10s %10s %10s", "Collation", "a", "b", "ExpectSign", "Actual");
  for (p= param; p->a; p++)
  {
    char ahex[64], bhex[64];
    int res= my_ci_strnncollsp(cs, (const uchar *) p->a, p->alen,
                                   (const uchar *) p->b, p->blen);
    str2hex(ahex, sizeof(ahex), p->a, p->alen);
    str2hex(bhex, sizeof(bhex), p->b, p->blen);
    diag("%-20s %-10s %-10s %10d %10d%s",
         cs->coll_name.str, ahex, bhex, p->res, res,
         eqres(res, p->res) ? "" : " FAILED");
    if (!eqres(res, p->res))
    {
      failed++;
    }
    else
    {
      /* Test in reverse order */
      res= my_ci_strnncollsp(cs, (const uchar *) p->b, p->blen,
                                 (const uchar *) p->a, p->alen);
      if (!eqres(res, -p->res))
      {
        diag("Comparison in reverse order failed. Expected %d, got %d",
             -p->res, res);
        failed++;
      }
    }
  }
  return failed;
}


static int
test_strcollsp()
{
  int failed= 0;
#ifdef HAVE_CHARSET_big5
  failed+= strcollsp(&my_charset_big5_chinese_ci, strcoll_mb2_common);
  failed+= strcollsp(&my_charset_big5_chinese_ci, strcoll_mb2_A1A1_mb2_F9FE);
  failed+= strcollsp(&my_charset_big5_bin,        strcoll_mb2_common);
  failed+= strcollsp(&my_charset_big5_bin,        strcoll_mb2_A1A1_mb2_F9FE);
#endif
#ifdef HAVE_CHARSET_cp932
  failed+= strcollsp(&my_charset_cp932_japanese_ci, strcoll_mb2_common);
  failed+= strcollsp(&my_charset_cp932_japanese_ci, strcoll_mb1_A1_bad_F9FE);
  failed+= strcollsp(&my_charset_cp932_bin,         strcoll_mb2_common);
  failed+= strcollsp(&my_charset_cp932_bin,         strcoll_mb1_A1_bad_F9FE);
  failed+= strcollsp(&my_charset_cp932_japanese_ci, strcoll_8181_A1_E0E0);
  failed+= strcollsp(&my_charset_cp932_bin,         strcoll_8181_A1_E0E0);
#endif
#ifdef HAVE_CHARSET_eucjpms
  failed+= strcollsp(&my_charset_eucjpms_japanese_ci, strcoll_mb2_common);
  failed+= strcollsp(&my_charset_eucjpms_bin,         strcoll_mb2_common);
  failed+= strcollsp(&my_charset_eucjpms_japanese_ci, strcoll_mb2_A1A1_mb2_F9FE);
  failed+= strcollsp(&my_charset_eucjpms_bin,         strcoll_mb2_A1A1_mb2_F9FE);
  failed+= strcollsp(&my_charset_eucjpms_japanese_ci, strcoll_ujis);
  failed+= strcollsp(&my_charset_eucjpms_bin,         strcoll_ujis);
#endif
#ifdef HAVE_CHARSET_euckr
  failed+= strcollsp(&my_charset_euckr_korean_ci, strcoll_mb2_common);
  failed+= strcollsp(&my_charset_euckr_korean_ci, strcoll_mb2_A1A1_mb2_F9FE);
  failed+= strcollsp(&my_charset_euckr_bin,       strcoll_mb2_common);
  failed+= strcollsp(&my_charset_euckr_bin,       strcoll_mb2_A1A1_mb2_F9FE);
#endif
#ifdef HAVE_CHARSET_gb2312
  failed+= strcollsp(&my_charset_gb2312_chinese_ci, strcoll_mb2_common);
  failed+= strcollsp(&my_charset_gb2312_chinese_ci, strcoll_mb2_A1A1_bad_F9FE);
  failed+= strcollsp(&my_charset_gb2312_bin,        strcoll_mb2_common);
  failed+= strcollsp(&my_charset_gb2312_bin,        strcoll_mb2_A1A1_bad_F9FE);
#endif
#ifdef HAVE_CHARSET_gbk
  failed+= strcollsp(&my_charset_gbk_chinese_ci, strcoll_mb2_common);
  failed+= strcollsp(&my_charset_gbk_chinese_ci, strcoll_mb2_A1A1_mb2_F9FE);
  failed+= strcollsp(&my_charset_gbk_bin,        strcoll_mb2_common);
  failed+= strcollsp(&my_charset_gbk_bin,        strcoll_mb2_A1A1_mb2_F9FE);
#endif
#ifdef HAVE_CHARSET_sjis
  failed+= strcollsp(&my_charset_sjis_japanese_ci, strcoll_mb2_common);
  failed+= strcollsp(&my_charset_sjis_bin,         strcoll_mb2_common);
  failed+= strcollsp(&my_charset_sjis_japanese_ci, strcoll_mb1_A1_bad_F9FE);
  failed+= strcollsp(&my_charset_sjis_bin,         strcoll_mb1_A1_bad_F9FE);
  failed+= strcollsp(&my_charset_sjis_japanese_ci, strcoll_8181_A1_E0E0);
  failed+= strcollsp(&my_charset_sjis_bin,         strcoll_8181_A1_E0E0);
#endif
#ifdef HAVE_CHARSET_ucs2
  failed+= strcollsp(&my_charset_ucs2_general_ci,  strcoll_ucs2_common);
  failed+= strcollsp(&my_charset_ucs2_general_ci,  strcoll_ucs2_space);
  failed+= strcollsp(&my_charset_ucs2_bin,         strcoll_ucs2_common);
  failed+= strcollsp(&my_charset_ucs2_bin,         strcoll_ucs2_space);
#endif
#ifdef HAVE_CHARSET_ujis
  failed+= strcollsp(&my_charset_ujis_japanese_ci, strcoll_mb2_common);
  failed+= strcollsp(&my_charset_ujis_bin,         strcoll_mb2_common);
  failed+= strcollsp(&my_charset_ujis_japanese_ci, strcoll_mb2_A1A1_mb2_F9FE);
  failed+= strcollsp(&my_charset_ujis_bin,         strcoll_mb2_A1A1_mb2_F9FE);
  failed+= strcollsp(&my_charset_ujis_japanese_ci, strcoll_ujis);
  failed+= strcollsp(&my_charset_ujis_bin,         strcoll_ujis);
#endif
#ifdef HAVE_CHARSET_utf16
  failed+= strcollsp(&my_charset_utf16_general_ci,  strcoll_ucs2_common);
  failed+= strcollsp(&my_charset_utf16_general_ci,  strcoll_ucs2_space);
  failed+= strcollsp(&my_charset_utf16_general_ci,  strcoll_utf16_common);
  failed+= strcollsp(&my_charset_utf16_general_ci,  strcoll_utf16_general_ci);
  failed+= strcollsp(&my_charset_utf16_bin,         strcoll_ucs2_common);
  failed+= strcollsp(&my_charset_utf16_bin,         strcoll_ucs2_space);
  failed+= strcollsp(&my_charset_utf16_bin,         strcoll_utf16_common);

  failed+= strcollsp(&my_charset_utf16le_general_ci,strcoll_ucs2_common);
  failed+= strcollsp(&my_charset_utf16le_general_ci,strcoll_utf16le_space);
  failed+= strcollsp(&my_charset_utf16le_general_ci,strcoll_utf16le_common);
  failed+= strcollsp(&my_charset_utf16le_general_ci,strcoll_utf16le_general_ci);
  failed+= strcollsp(&my_charset_utf16le_bin,       strcoll_ucs2_common);
  failed+= strcollsp(&my_charset_utf16le_bin,       strcoll_utf16le_space);
  failed+= strcollsp(&my_charset_utf16le_bin,       strcoll_utf16le_common);
#endif
#ifdef HAVE_CHARSET_utf32
  failed+= strcollsp(&my_charset_utf32_general_ci,  strcoll_utf32_common);
  failed+= strcollsp(&my_charset_utf32_general_ci,  strcoll_utf32_general_ci);
  failed+= strcollsp(&my_charset_utf32_bin,         strcoll_utf32_common);
#endif
#ifdef HAVE_CHARSET_utf8
  failed+= strcollsp(&my_charset_utf8mb3_general_ci,          strcoll_utf8mb3_common);
  failed+= strcollsp(&my_charset_utf8mb3_general_mysql500_ci, strcoll_utf8mb3_common);
  failed+= strcollsp(&my_charset_utf8mb3_bin,                 strcoll_utf8mb3_common);
#endif
#ifdef HAVE_CHARSET_utf8mb4
  failed+= strcollsp(&my_charset_utf8mb4_general_ci,          strcoll_utf8mb3_common);
  failed+= strcollsp(&my_charset_utf8mb4_bin,                 strcoll_utf8mb3_common);
  failed+= strcollsp(&my_charset_utf8mb4_general_ci,          strcoll_utf8mb4_common);
  failed+= strcollsp(&my_charset_utf8mb4_general_ci,          strcoll_utf8mb4_general_ci);
  failed+= strcollsp(&my_charset_utf8mb4_bin,                 strcoll_utf8mb4_common);
#endif
  return failed;
}


typedef struct
{
  size_t size;
  size_t nchars;
  LEX_CSTRING min;
  LEX_CSTRING max;
} MINMAX_PARAM;


static MINMAX_PARAM minmax_param_latin1_swedish_ci[]=
{
  {0, 0, {CSTR("")},             {CSTR("")}},
  {0, 1, {CSTR("")},             {CSTR("")}},
  {0, 2, {CSTR("")},             {CSTR("")}},
  {0, 3, {CSTR("")},             {CSTR("")}},
  {1, 0, {CSTR("")},             {CSTR("")}},
  {1, 1, {CSTR("\x00")},         {CSTR("\xFF")}},
  {1, 2, {CSTR("\x00")},         {CSTR("\xFF")}},
  {1, 3, {CSTR("\x00")},         {CSTR("\xFF")}},
  {2, 0, {CSTR("")},             {CSTR("")}},
  {2, 1, {CSTR("\x00")},         {CSTR("\xFF")}},
  {2, 2, {CSTR("\x00\x00")},     {CSTR("\xFF\xFF")}},
  {2, 3, {CSTR("\x00\x00")},     {CSTR("\xFF\xFF")}},
  {3, 0, {CSTR("")},             {CSTR("")}},
  {3, 1, {CSTR("\x00")},         {CSTR("\xFF")}},
  {3, 2, {CSTR("\x00\x00")},     {CSTR("\xFF\xFF")}},
  {3, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xFF\xFF\xFF")}}
};


static MINMAX_PARAM minmax_param_latin1_nopad_bin[]=
{
  {0, 0, {CSTR("")},             {CSTR("")}},
  {0, 1, {CSTR("")},             {CSTR("")}},
  {0, 2, {CSTR("")},             {CSTR("")}},
  {0, 3, {CSTR("")},             {CSTR("")}},
  {1, 0, {CSTR("")},             {CSTR("")}},
  {1, 1, {CSTR("")},             {CSTR("\xFF")}},
  {1, 2, {CSTR("")},             {CSTR("\xFF")}},
  {1, 3, {CSTR("")},             {CSTR("\xFF")}},
  {2, 0, {CSTR("")},             {CSTR("")}},
  {2, 1, {CSTR("")},             {CSTR("\xFF")}},
  {2, 2, {CSTR("")},             {CSTR("\xFF\xFF")}},
  {2, 3, {CSTR("")},             {CSTR("\xFF\xFF")}},
  {3, 0, {CSTR("")},             {CSTR("")}},
  {3, 1, {CSTR("")},             {CSTR("\xFF")}},
  {3, 2, {CSTR("")},             {CSTR("\xFF\xFF")}},
  {3, 3, {CSTR("")},             {CSTR("\xFF\xFF\xFF")}}
};


static MINMAX_PARAM minmax_param_utf8mb3_unicode_ci[]=
{
  {0, 0, {CSTR("")},             {CSTR("")}},
  {0, 1, {CSTR("")},             {CSTR("")}},
  {0, 2, {CSTR("")},             {CSTR("")}},
  {0, 3, {CSTR("")},             {CSTR("")}},
  {1, 0, {CSTR("")},             {CSTR("")}},
  {1, 1, {CSTR("\x09")},         {CSTR("")}},
  {1, 2, {CSTR("\x09")},         {CSTR("")}},
  {1, 3, {CSTR("\x09")},         {CSTR("")}},
  {2, 0, {CSTR("")},             {CSTR("")}},
  {2, 1, {CSTR("\x09")},         {CSTR("")}},
  {2, 2, {CSTR("\x09\x09")},     {CSTR("")}},
  {2, 3, {CSTR("\x09\x09")},     {CSTR("")}},
  {3, 0, {CSTR("")},             {CSTR("")}},
  {3, 1, {CSTR("\x09")},         {CSTR("\xEF\xBF\xBF")}},
  {3, 2, {CSTR("\x09\x09")},     {CSTR("\xEF\xBF\xBF")}},
  {3, 3, {CSTR("\x09\x09\x09")}, {CSTR("\xEF\xBF\xBF")}},
  {4, 0, {CSTR("")},             {CSTR("")}},
  {4, 1, {CSTR("\x09")},         {CSTR("\xEF\xBF\xBF")}},
  {4, 2, {CSTR("\x09\x09")},     {CSTR("\xEF\xBF\xBF")}},
  {4, 3, {CSTR("\x09\x09\x09")}, {CSTR("\xEF\xBF\xBF")}},
  {5, 0, {CSTR("")},             {CSTR("")}},
  {5, 1, {CSTR("\x09")},         {CSTR("\xEF\xBF\xBF")}},
  {5, 2, {CSTR("\x09\x09")},     {CSTR("\xEF\xBF\xBF")}},
  {5, 3, {CSTR("\x09\x09\x09")}, {CSTR("\xEF\xBF\xBF")}},
  {6, 0, {CSTR("")},             {CSTR("")}},
  {6, 1, {CSTR("\x09")},         {CSTR("\xEF\xBF\xBF")}},
  {6, 2, {CSTR("\x09\x09")},     {CSTR("\xEF\xBF\xBF\xEF\xBF\xBF")}},
  {6, 3, {CSTR("\x09\x09\x09")}, {CSTR("\xEF\xBF\xBF\xEF\xBF\xBF")}},
  {7, 0, {CSTR("")},             {CSTR("")}},
  {7, 1, {CSTR("\x09")},         {CSTR("\xEF\xBF\xBF")}},
  {7, 2, {CSTR("\x09\x09")},     {CSTR("\xEF\xBF\xBF\xEF\xBF\xBF")}},
  {7, 3, {CSTR("\x09\x09\x09")}, {CSTR("\xEF\xBF\xBF\xEF\xBF\xBF")}},
  {8, 0, {CSTR("")},             {CSTR("")}},
  {8, 1, {CSTR("\x09")},         {CSTR("\xEF\xBF\xBF")}},
  {8, 2, {CSTR("\x09\x09")},     {CSTR("\xEF\xBF\xBF\xEF\xBF\xBF")}},
  {8, 3, {CSTR("\x09\x09\x09")}, {CSTR("\xEF\xBF\xBF\xEF\xBF\xBF")}},
  {9, 0, {CSTR("")},             {CSTR("")}},
  {9, 1, {CSTR("\x09")},         {CSTR("\xEF\xBF\xBF")}},
  {9, 2, {CSTR("\x09\x09")},     {CSTR("\xEF\xBF\xBF\xEF\xBF\xBF")}},
  {9, 3, {CSTR("\x09\x09\x09")}, {CSTR("\xEF\xBF\xBF\xEF\xBF\xBF\xEF\xBF\xBF")}},
};


#ifdef HAVE_CHARSET_big5
static MINMAX_PARAM minmax_param_big5_chinese_ci[]=
{
  {0, 0, {CSTR("")},             {CSTR("")}},
  {0, 1, {CSTR("")},             {CSTR("")}},
  {0, 2, {CSTR("")},             {CSTR("")}},
  {0, 3, {CSTR("")},             {CSTR("")}},
  {1, 0, {CSTR("")},             {CSTR("")}},
  {1, 1, {CSTR("\x00")},         {CSTR("")}},
  {1, 2, {CSTR("\x00")},         {CSTR("")}},
  {1, 3, {CSTR("\x00")},         {CSTR("")}},
  {2, 0, {CSTR("")},             {CSTR("")}},
  {2, 1, {CSTR("\x00")},         {CSTR("\xF9\xD5")}},
  {2, 2, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5")}},
  {2, 3, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5")}},
  {3, 0, {CSTR("")},             {CSTR("")}},
  {3, 1, {CSTR("\x00")},         {CSTR("\xF9\xD5")}},
  {3, 2, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5")}},
  {3, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xF9\xD5")}},
  {4, 0, {CSTR("")},             {CSTR("")}},
  {4, 1, {CSTR("\x00")},         {CSTR("\xF9\xD5")}},
  {4, 2, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5\xF9\xD5")}},
  {4, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xF9\xD5\xF9\xD5")}},
  {5, 0, {CSTR("")},             {CSTR("")}},
  {5, 1, {CSTR("\x00")},         {CSTR("\xF9\xD5")}},
  {5, 2, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5\xF9\xD5")}},
  {5, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xF9\xD5\xF9\xD5")}},
  {6, 0, {CSTR("")},             {CSTR("")}},
  {6, 1, {CSTR("\x00")},         {CSTR("\xF9\xD5")}},
  {6, 2, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5\xF9\xD5")}},
  {6, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xF9\xD5\xF9\xD5\xF9\xD5")}},
  {7, 0, {CSTR("")},             {CSTR("")}},
  {7, 1, {CSTR("\x00")},         {CSTR("\xF9\xD5")}},
  {7, 2, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5\xF9\xD5")}},
  {7, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xF9\xD5\xF9\xD5\xF9\xD5")}},
  {8, 0, {CSTR("")},             {CSTR("")}},
  {8, 1, {CSTR("\x00")},         {CSTR("\xF9\xD5")}},
  {8, 2, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5\xF9\xD5")}},
  {8, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xF9\xD5\xF9\xD5\xF9\xD5")}},
  {9, 0, {CSTR("")},             {CSTR("")}},
  {9, 1, {CSTR("\x00")},         {CSTR("\xF9\xD5")}},
  {9, 2, {CSTR("\x00\x00")},     {CSTR("\xF9\xD5\xF9\xD5")}},
  {9, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xF9\xD5\xF9\xD5\xF9\xD5")}},
};
#endif

#ifdef HAVE_CHARSET_cp1250
static MINMAX_PARAM minmax_param_cp1250_czech_cs[]=
{
  {0, 0, {CSTR("")},             {CSTR("")}},
  {0, 1, {CSTR("")},             {CSTR("")}},
  {0, 2, {CSTR("")},             {CSTR("")}},
  {0, 3, {CSTR("")},             {CSTR("")}},
  {1, 0, {CSTR("")},             {CSTR("")}},
  {1, 1, {CSTR("\x00")},         {CSTR("\xFF")}},
  {1, 2, {CSTR("\x00")},         {CSTR("\xFF")}},
  {1, 3, {CSTR("\x00")},         {CSTR("\xFF")}},
  {2, 0, {CSTR("")},             {CSTR("")}},
  {2, 1, {CSTR("\x00")},         {CSTR("\xFF")}},
  {2, 2, {CSTR("\x00\x00")},     {CSTR("\xFF\xFF")}},
  {2, 3, {CSTR("\x00\x00")},     {CSTR("\xFF\xFF")}},
  {3, 0, {CSTR("")},             {CSTR("")}},
  {3, 1, {CSTR("\x00")},         {CSTR("\xFF")}},
  {3, 2, {CSTR("\x00\x00")},     {CSTR("\xFF\xFF")}},
  {3, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xFF\xFF\xFF")}}
};
#endif


#ifdef HAVE_CHARSET_latin2
static MINMAX_PARAM minmax_param_latin2_czech_cs[]=
{
  {0, 0, {CSTR("")},             {CSTR("")}},
  {0, 1, {CSTR("")},             {CSTR("")}},
  {0, 2, {CSTR("")},             {CSTR("")}},
  {0, 3, {CSTR("")},             {CSTR("")}},
  {1, 0, {CSTR("")},             {CSTR("")}},
  {1, 1, {CSTR("\x00")},         {CSTR("\xAE")}},
  {1, 2, {CSTR("\x00")},         {CSTR("\xAE")}},
  {1, 3, {CSTR("\x00")},         {CSTR("\xAE")}},
  {2, 0, {CSTR("")},             {CSTR("")}},
  {2, 1, {CSTR("\x00")},         {CSTR("\xAE")}},
  {2, 2, {CSTR("\x00\x00")},     {CSTR("\xAE\xAE")}},
  {2, 3, {CSTR("\x00\x00")},     {CSTR("\xAE\xAE")}},
  {3, 0, {CSTR("")},             {CSTR("")}},
  {3, 1, {CSTR("\x00")},         {CSTR("\xAE")}},
  {3, 2, {CSTR("\x00\x00")},     {CSTR("\xAE\xAE")}},
  {3, 3, {CSTR("\x00\x00\x00")}, {CSTR("\xAE\xAE\xAE")}}
};
#endif


static int test_minmax_str_one(CHARSET_INFO *cs,
                               const MINMAX_PARAM *params, size_t count)
{
  size_t i;
  int failed_total= 0;
  for (i= 0; i < count; i++)
  {
    int failed;
    char min[32], hmin[64];
    char max[32], hmax[64];
    const MINMAX_PARAM *prm= &params[i];
    size_t minlen= cs->coll->min_str(cs, (uchar *) min, prm->size,
                                                        prm->nchars);
    size_t maxlen= cs->coll->max_str(cs, (uchar *) max, prm->size,
                                                        prm->nchars);
    failed= minlen != prm->min.length || memcmp(min, prm->min.str, minlen) ||
            maxlen != prm->max.length || memcmp(max, prm->max.str, maxlen);

    str2hex(hmin, sizeof(hmin), min, minlen);
    str2hex(hmax, sizeof(hmax), max, maxlen);
    diag("%-32s %2d %2d   %-10s   %-10s%s",
         cs->coll_name.str, (int) prm->size, (int) prm->nchars, hmin, hmax,
         failed ? " FAILED" : "");
    if (failed)
    {
      str2hex(hmin, sizeof(hmin), prm->min.str, prm->min.length);
      str2hex(hmax, sizeof(hmax), prm->max.str, prm->max.length);
      diag("%-40s %-10s   %-10s EXPECTED", cs->coll_name.str, hmin, hmax);
    }
    failed_total+= failed;
  }
  return failed_total;
}


static int test_minmax_str()
{
  int failed= 0;
  failed+= test_minmax_str_one(&my_charset_latin1_nopad_bin,
                               minmax_param_latin1_nopad_bin,
                               array_elements(minmax_param_latin1_nopad_bin));
  failed+= test_minmax_str_one(&my_charset_latin1,
                               minmax_param_latin1_swedish_ci,
                               array_elements(minmax_param_latin1_swedish_ci));
  failed+= test_minmax_str_one(&my_charset_utf8mb3_unicode_ci,
                               minmax_param_utf8mb3_unicode_ci,
                               array_elements(minmax_param_utf8mb3_unicode_ci));
#ifdef HAVE_CHARSET_big5
  failed+= test_minmax_str_one(&my_charset_big5_chinese_ci,
                               minmax_param_big5_chinese_ci,
                               array_elements(minmax_param_big5_chinese_ci));
#endif
#ifdef HAVE_CHARSET_cp1250
  failed+= test_minmax_str_one(&my_charset_cp1250_czech_cs,
                               minmax_param_cp1250_czech_cs,
                               array_elements(minmax_param_cp1250_czech_cs));
#endif
#ifdef HAVE_CHARSET_latin2
  failed+= test_minmax_str_one(&my_charset_latin2_czech_cs,
                               minmax_param_latin2_czech_cs,
                               array_elements(minmax_param_latin2_czech_cs));
#endif
  return failed;
}

typedef struct
{
  LEX_CSTRING a;
  LEX_CSTRING b;
  size_t nchars;
  uint flags;
  int res;
} STRNNCOLLSP_CHAR_PARAM;

#undef TCHAR
#define TCHAR MY_STRNNCOLLSP_NCHARS_EMULATE_TRIMMED_TRAILING_SPACES

#define TVCHAR 0

/*
  Some lines in the below test data are marked as follows:

  IF  - An ignorable failure. The scanner finds an ignorable character
        followed by a normal character (or by a contraction),
        but the "nchars" limit allows only one character to be scanned.
        The whole sequence is ignored an is treated as end-of-line.
  CF - A contraction failure. The scanner finds a contraction consisting
        of two characters, but the "nchars" limit allows only one character
        to be scanned. The whole contraction is ignored and is treated
        as end-of-line.
*/


/*
  Tests for mbminlen1 character sets,
  for both PAD SPACE and NOPAD collations
*/
static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_mbminlen1_xpad_common[]=
{
  {{CSTR("a")},              {CSTR("a")},                       0,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a")},                       1,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a")},                       2,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a")},                       3,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a")},                     100,  TCHAR,  0},

  {{CSTR("a")},              {CSTR("ab")},                      0,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("ab")},                      1,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("ab")},                      2,  TCHAR, -1},
  {{CSTR("a")},              {CSTR("ab")},                      3,  TCHAR, -1},
  {{CSTR("a")},              {CSTR("ab")},                    100,  TCHAR, -1},

  {{CSTR("a")},              {CSTR("a ")},                      0,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a ")},                      1,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a ")},                      2,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a ")},                      3,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a ")},                    100,  TCHAR,  0},

  {{CSTR("a")},              {CSTR("a  ")},                     0,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a  ")},                     1,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a  ")},                     2,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a  ")},                     3,  TCHAR,  0},
  {{CSTR("a")},              {CSTR("a  ")},                   100,  TCHAR,  0},

  {{CSTR("ss")},             {CSTR("ss")},                      0,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("ss")},                      1,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("ss")},                      2,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("ss")},                      3,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("ss")},                    100,  TCHAR,  0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


/* Tests for utf8, for both PAD SPACE and NOPAD collations */
static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_utf8mbx_xpad_common[]=
{
  {{CSTR(UTF8_sz)},          {CSTR(UTF8_sz)},                   0,  TCHAR,  0},
  {{CSTR(UTF8_sz)},          {CSTR(UTF8_sz)},                   1,  TCHAR,  0},
  {{CSTR(UTF8_sz)},          {CSTR(UTF8_sz)},                   2,  TCHAR,  0},
  {{CSTR(UTF8_sz)},          {CSTR(UTF8_sz)},                   3,  TCHAR,  0},
  {{CSTR(UTF8_sz)},          {CSTR(UTF8_sz)},                 100,  TCHAR,  0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


/* Tests for latin1, for both PAD and NOPAD collations */
static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_latin1_xpad_common[]=
{
  {{CSTR(LATIN1_sz)},        {CSTR(LATIN1_sz)},                 0,  TCHAR,  0},
  {{CSTR(LATIN1_sz)},        {CSTR(LATIN1_sz)},                 1,  TCHAR,  0},
  {{CSTR(LATIN1_sz)},        {CSTR(LATIN1_sz)},                 2,  TCHAR,  0},
  {{CSTR(LATIN1_sz)},        {CSTR(LATIN1_sz)},                 3,  TCHAR,  0},
  {{CSTR(LATIN1_sz)},        {CSTR(LATIN1_sz)},               100,  TCHAR,  0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


/* Tests for utf8 collations that sort "A WITH DIAERESIS" equal to "A" */
static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_utf8mbx_xpad_a_eq_auml[]=
{
  {{CSTR(UTF8_auml "h")},    {CSTR("ah")},                      0,  TCHAR,  0},
  {{CSTR(UTF8_auml "h")},    {CSTR("ah")},                      1,  TCHAR,  0},
  {{CSTR(UTF8_auml "h")},    {CSTR("ah")},                      2,  TCHAR,  0},
  {{CSTR(UTF8_auml "h")},    {CSTR("ah")},                      3,  TCHAR,  0},
  {{CSTR(UTF8_auml "h")},    {CSTR("ah")},                    100,  TCHAR,  0},

  {{CSTR(UTF8_auml "h")},    {CSTR("ah ")},                     0,  TCHAR,  0},
  {{CSTR(UTF8_auml "h")},    {CSTR("ah ")},                     1,  TCHAR,  0},
  {{CSTR(UTF8_auml "h")},    {CSTR("ah ")},                     2,  TCHAR,  0},
  {{CSTR(UTF8_auml "h")},    {CSTR("ah ")},                     3,  TCHAR,  0},
  {{CSTR(UTF8_auml "h")},    {CSTR("ah ")},                   100,  TCHAR,  0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_utf8mb3_unicode_ci[]=
{
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},            0,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},            1,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")}/*IF*/,      2,  TCHAR,  1},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},            3,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},            4,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},          100,  TCHAR,  0},

  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   0,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   1,  TCHAR, -1},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   2,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   3,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   4,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                 100,  TCHAR,  0},

  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},                 0,  TCHAR,  0},
  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},                 1,  TCHAR,  0},
  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},                 2,  TCHAR,  0},
  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},                 3,  TCHAR,  0},
  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},               100,  TCHAR,  0},

  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 0,  TCHAR,  0},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 1,  TCHAR,  0},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 2,  TCHAR,  0},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 3,  TCHAR,  0},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 4,  TCHAR,  0},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},               100,  TCHAR,  0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_utf8mb3_unicode_nopad_ci[]=
{
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},            0,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},            1,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")}/*IF*/,      2,  TCHAR,  1},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},            3,  TCHAR,  1},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},            4,  TCHAR,  1},
  {{CSTR("ss")},             {CSTR("s" "\x00" "s")},          100,  TCHAR,  1},

  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   0,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   1,  TCHAR, -1},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   2,  TCHAR, -1},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   3,  TCHAR, -1},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   4,  TCHAR, -1},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                 100,  TCHAR, -1},

  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   0,  TVCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   1,  TVCHAR, -1},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   2,  TVCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   3,  TVCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   4,  TVCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                 100,  TVCHAR,  0},

  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},                 0,  TCHAR,  0},
  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},                 1,  TCHAR,  0},
  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},                 2,  TCHAR,  -1},
  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},                 3,  TCHAR,  0},
  {{CSTR("a" "\x01")},       {CSTR(UTF8_auml)},               100,  TCHAR,  0},

  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 0,  TCHAR,  0},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 1,  TCHAR,  0},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 2,  TCHAR,  -1},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 3,  TCHAR,  -1},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},                 4,  TCHAR,   0},
  {{CSTR("a" "\x01\x01")},   {CSTR(UTF8_auml)},               100,  TCHAR,   0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_utf8mb3_danish_ci[]=
{
  {{CSTR("aa")},             {CSTR("")},                        0,  TCHAR,  0},
  {{CSTR("aa")}/*CF*/,       {CSTR("")},                        1,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("")},                        2,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("")},                        3,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("")},                      100,  TCHAR,  1},

  {{CSTR("aa")},             {CSTR("a")},                       0,  TCHAR,  0},
  {{CSTR("aa")}/*CF*/,       {CSTR("a")},                       1,  TCHAR,  0},
  {{CSTR("aa")},             {CSTR("a")},                       2,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("a")},                       3,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("a")},                     100,  TCHAR,  1},

  {{CSTR("aa")},             {CSTR("aa")},                      0,  TCHAR,  0},
  {{CSTR("aa")}/*CF*/,       {CSTR("aa")}/*CF*/,                1,  TCHAR,  0},
  {{CSTR("aa")},             {CSTR("aa")},                      2,  TCHAR,  0},
  {{CSTR("aa")},             {CSTR("aa")},                      3,  TCHAR,  0},
  {{CSTR("aa")},             {CSTR("aa")},                    100,  TCHAR,  0},

  {{CSTR("aa")},             {CSTR("\x00" "a")},                0,  TCHAR,  0},
  {{CSTR("aa")}/*CF*/,       {CSTR("\x00" "a")}/*IF*/,          1,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("\x00" "a")},                2,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("\x00" "a")},                3,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("\x00" "a")},              100,  TCHAR,  1},

  {{CSTR("aa")},             {CSTR("\x00" "aa")},                0, TCHAR,  0},
  {{CSTR("aa")}/*CF*/,       {CSTR("\x00" "aa")}/*IF*/,          1, TCHAR,  1},
  {{CSTR("aa")},             {CSTR("\x00" "aa")}/*IF*/,          2, TCHAR,  1},
  {{CSTR("aa")},             {CSTR("\x00" "aa")},                3, TCHAR,  0},
  {{CSTR("aa")},             {CSTR("\x00" "aa")},              100, TCHAR,  0},

  {{CSTR("aa")},             {CSTR("a" "\x00" "a")},            0,  TCHAR,  0},
  {{CSTR("aa")}/*CF*/,       {CSTR("a" "\x00" "a")},            1,  TCHAR,  0},
  {{CSTR("aa")},             {CSTR("a" "\x00" "a")}/*IF*/,      2,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("a" "\x00" "a")},            3,  TCHAR,  1},
  {{CSTR("aa")},             {CSTR("a" "\x00" "a")},          100,  TCHAR,  1},

  {{CSTR("aa")},             {CSTR(UTF8_ARING)},                0,  TCHAR,  0},
  {{CSTR("aa")}/*CF*/,       {CSTR(UTF8_ARING)},                1,  TCHAR, -1},
  {{CSTR("aa")},             {CSTR(UTF8_ARING)},                2,  TCHAR,  0},
  {{CSTR("aa")},             {CSTR(UTF8_ARING)},                3,  TCHAR,  0},
  {{CSTR("aa")},             {CSTR(UTF8_ARING)},              100,  TCHAR,  0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_latin1_german2_ci[]=
{
  {{CSTR("ss")},             {CSTR(LATIN1_sz)},                 0,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(LATIN1_sz)},                 1,  TCHAR, -1},
  {{CSTR("ss")},             {CSTR(LATIN1_sz)},                 2,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(LATIN1_sz)},                 3,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(LATIN1_sz)},               100,  TCHAR,  0},

  {{CSTR("ae")},             {CSTR(LATIN1_auml)},               0,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(LATIN1_auml)},               1,  TCHAR, -1},
  {{CSTR("ae")},             {CSTR(LATIN1_auml)},               2,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(LATIN1_auml)},               3,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(LATIN1_auml)},             100,  TCHAR,  0},

  {{CSTR("ae")},             {CSTR(LATIN1_auml " ")},           0,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(LATIN1_auml " ")},           1,  TCHAR, -1},
  {{CSTR("ae")},             {CSTR(LATIN1_auml " ")},           2,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(LATIN1_auml " ")},           3,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(LATIN1_auml " ")},         100,  TCHAR,  0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_utf8mbx_german2_ci[]=
{
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   0,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   1,  TCHAR, -1},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   2,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                   3,  TCHAR,  0},
  {{CSTR("ss")},             {CSTR(UTF8_sz)},                 100,  TCHAR,  0},

  {{CSTR("ae")},             {CSTR(UTF8_auml)},                 0,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(UTF8_auml)},                 1,  TCHAR, -1},
  {{CSTR("ae")},             {CSTR(UTF8_auml)},                 2,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(UTF8_auml)},                 3,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(UTF8_auml)},               100,  TCHAR,  0},

  {{CSTR("ae")},             {CSTR(UTF8_auml " ")},             0,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(UTF8_auml " ")},             1,  TCHAR, -1},
  {{CSTR("ae")},             {CSTR(UTF8_auml " ")},             2,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(UTF8_auml " ")},             3,  TCHAR,  0},
  {{CSTR("ae")},             {CSTR(UTF8_auml " ")},           100,  TCHAR,  0},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_mbminlen1_xpad_czech[]=
{
  {{CSTR("c")},              {CSTR("ch")},                      0,  TCHAR,  0},
  {{CSTR("c")},              {CSTR("ch")},                      1,  TCHAR,  0},
  {{CSTR("c")},              {CSTR("ch")},                      2,  TCHAR, -1},

  {{CSTR("h")},              {CSTR("ch")},                      0,  TCHAR,  0},
  {{CSTR("h")},              {CSTR("ch")},                      1,  TCHAR,  1},
  {{CSTR("h")},              {CSTR("ch")},                      2,  TCHAR, -1},

  {{CSTR("i")},              {CSTR("ch")},                      0,  TCHAR,  0},
  {{CSTR("i")},              {CSTR("ch")},                      1,  TCHAR,  1},
  {{CSTR("i")},              {CSTR("ch")},                      2,  TCHAR,  1},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


static STRNNCOLLSP_CHAR_PARAM strnncollsp_char_mbminlen2_xpad_common[]=
{
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a)},                    0,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a)},                    1,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a)},                    2,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a)},                    3,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a)},                  100,  TCHAR,  0},

  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp)},            0,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp)},            1,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp)},            2,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp)},            3,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp)},          100,  TCHAR,  0},

  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp UCS2_sp)},    0,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp UCS2_sp)},    1,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp UCS2_sp)},    2,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp UCS2_sp)},    3,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_sp UCS2_sp)},  100,  TCHAR,  0},

  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_b)},             0,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_b)},             1,  TCHAR,  0},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_b)},             2,  TCHAR, -1},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_b)},             3,  TCHAR, -1},
  {{CSTR(UCS2_a)},           {CSTR(UCS2_a UCS2_b)},           100,  TCHAR, -1},

  {{NULL, 0},                {NULL, 0},                         0,    0,  0}
};


static int
strnncollsp_char_one(CHARSET_INFO *cs, const STRNNCOLLSP_CHAR_PARAM *p)
{
  int failed= 0;
  char ahex[64], bhex[64];
  int res= cs->coll->strnncollsp_nchars(cs,
                                        (uchar *) p->a.str, p->a.length,
                                        (uchar *) p->b.str, p->b.length,
                                        p->nchars, p->flags);
  str2hex(ahex, sizeof(ahex), p->a.str, p->a.length);
  str2hex(bhex, sizeof(bhex), p->b.str, p->b.length);
  diag("%-25s %-12s %-12s %3d %7d %7d%s",
       cs->coll_name.str, ahex, bhex, (int) p->nchars, p->res, res,
       eqres(res, p->res) ? "" : " FAILED");
  if (!eqres(res, p->res))
  {
    failed++;
  }
  else
  {
    /* Test in reverse order */
    res= cs->coll->strnncollsp_nchars(cs,
                                      (uchar *) p->b.str, p->b.length,
                                      (uchar *) p->a.str, p->a.length,
                                      p->nchars, p->flags);
    if (!eqres(res, -p->res))
    {
      diag("Comparison in reverse order failed. Expected %d, got %d",
           -p->res, res);
      failed++;
    }
  }
  return failed;
}


static int
strnncollsp_char(const char *collation, const STRNNCOLLSP_CHAR_PARAM *param)
{
  int failed= 0;
  const STRNNCOLLSP_CHAR_PARAM *p;
  CHARSET_INFO *cs= get_charset_by_name(collation, MYF(0));

  if (!cs)
  {
    diag("get_charset_by_name() failed");
    return 1;
  }

  diag("%-25s %-12s %-12s %-3s %7s %7s",
       "Collation", "a", "b", "Nch", "ExpSign", "Actual");

  for (p= param; p->a.str; p++)
  {
    failed+= strnncollsp_char_one(cs, p);
  }

  return failed;
}


static int
strnncollsp_char_mbminlen1(const char *collation,
                           const STRNNCOLLSP_CHAR_PARAM *specific)
{
  int failed= 0;
  failed+= strnncollsp_char(collation, strnncollsp_char_mbminlen1_xpad_common);
  if (specific)
    failed+= strnncollsp_char(collation, specific);
  return failed;
}


static int
strnncollsp_char_mbminlen2(const char *collation,
                           const STRNNCOLLSP_CHAR_PARAM *specific)
{
  int failed= 0;
  failed+= strnncollsp_char(collation, strnncollsp_char_mbminlen2_xpad_common);
  if (specific)
    failed+= strnncollsp_char(collation, specific);
  return failed;
}


static int
strnncollsp_char_latin1(const char *collation,
                        const STRNNCOLLSP_CHAR_PARAM *specific)
{
  int failed= 0;
  failed+= strnncollsp_char(collation, strnncollsp_char_mbminlen1_xpad_common);
  failed+= strnncollsp_char(collation, strnncollsp_char_latin1_xpad_common);
  if (specific)
    failed+= strnncollsp_char(collation, specific);
  return failed;
}


static int
strnncollsp_char_utf8mbx(const char *collation,
                         const STRNNCOLLSP_CHAR_PARAM *specific)
{
  int failed= 0;
  failed+= strnncollsp_char(collation, strnncollsp_char_mbminlen1_xpad_common);
  failed+= strnncollsp_char(collation, strnncollsp_char_utf8mbx_xpad_common);

  if (!strstr(collation, "_bin") &&
      !strstr(collation, "_german2") &&
      !strstr(collation, "_danish"))
    failed+= strnncollsp_char(collation,
                              strnncollsp_char_utf8mbx_xpad_a_eq_auml);
  if (specific)
    failed+= strnncollsp_char(collation, specific);
  return failed;
}


static int
test_strnncollsp_char()
{
  int failed= 0;
  failed+= strnncollsp_char_latin1("latin1_swedish_ci", NULL);
  failed+= strnncollsp_char_latin1("latin1_swedish_nopad_ci", NULL);
  failed+= strnncollsp_char_latin1("latin1_bin", NULL);
  failed+= strnncollsp_char_latin1("latin1_nopad_bin", NULL);
  failed+= strnncollsp_char_latin1("latin1_german2_ci",
                                   strnncollsp_char_latin1_german2_ci);

#ifdef HAVE_CHARSET_cp1250
  failed+= strnncollsp_char_mbminlen1("cp1250_czech_cs",
                                      strnncollsp_char_mbminlen1_xpad_czech);
#endif

#ifdef HAVE_CHARSET_latin2
  failed+= strnncollsp_char_mbminlen1("latin2_czech_cs",
                                      strnncollsp_char_mbminlen1_xpad_czech);
#endif

#ifdef HAVE_CHARSET_tis620
  failed+= strnncollsp_char_mbminlen1("tis620_thai_ci", NULL);
  failed+= strnncollsp_char_mbminlen1("tis620_thai_nopad_ci", NULL);
#endif

#ifdef HAVE_CHARSET_big5
  failed+= strnncollsp_char_mbminlen1("big5_chinese_ci", NULL);
  failed+= strnncollsp_char_mbminlen1("big5_chinese_nopad_ci", NULL);
  failed+= strnncollsp_char_mbminlen1("big5_bin", NULL);
  failed+= strnncollsp_char_mbminlen1("big5_nopad_bin", NULL);
#endif

  failed+= strnncollsp_char_utf8mbx("utf8mb3_general_ci", NULL);
  failed+= strnncollsp_char_utf8mbx("utf8mb3_general_nopad_ci", NULL);
  failed+= strnncollsp_char_utf8mbx("utf8mb3_bin", NULL);
  failed+= strnncollsp_char_utf8mbx("utf8mb3_nopad_bin", NULL);

  failed+= strnncollsp_char_utf8mbx("utf8mb3_unicode_ci",
                                    strnncollsp_char_utf8mb3_unicode_ci);
  failed+= strnncollsp_char_utf8mbx("utf8mb3_unicode_nopad_ci",
                                    strnncollsp_char_utf8mb3_unicode_nopad_ci);
  failed+= strnncollsp_char_utf8mbx("utf8mb3_danish_ci",
                                    strnncollsp_char_utf8mb3_danish_ci);
  failed+= strnncollsp_char_utf8mbx("utf8mb3_german2_ci",
                                    strnncollsp_char_utf8mbx_german2_ci);
  failed+= strnncollsp_char_utf8mbx("utf8mb3_czech_ci",
                                    strnncollsp_char_mbminlen1_xpad_czech);

#ifdef HAVE_CHARSET_ucs2
  failed+= strnncollsp_char_mbminlen2("ucs2_general_ci", NULL);
  failed+= strnncollsp_char_mbminlen2("ucs2_general_nopad_ci", NULL);
  failed+= strnncollsp_char_mbminlen2("ucs2_bin", NULL);
  failed+= strnncollsp_char_mbminlen2("ucs2_nopad_bin", NULL);
  failed+= strnncollsp_char_mbminlen2("ucs2_unicode_ci", NULL);
  failed+= strnncollsp_char_mbminlen2("ucs2_unicode_nopad_ci", NULL);
#endif

  return failed;
}

int test_longlong10_to_str() {
  int failed= 0;
  char *r= NULL;

  char buf1[23];
  longlong v1 = 123456789012345;
  char v1_ans[] = "123456789012345";
  unsigned int expected_len_1 = strlen(v1_ans);

  char buf2[23];
  longlong v2 = -123456789012345;
  char v2_ans[] = "-123456789012345";
  unsigned int expected_len_2 = strlen(v2_ans);

  char buf3[23];
  longlong v3 = 0;
  char v3_ans[] = "0";
  unsigned int expected_len_3 = strlen(v3_ans);

  char buf4[23];
  longlong v4 = -0;
  char v4_ans[] = "0";
  unsigned int expected_len_4 = strlen(v4_ans);

  char buf5[23];
  longlong v5 = LONGLONG_MAX;  // 9223372036854775807
  char v5_ans[] = "9223372036854775807";
  unsigned int expected_len_5 = strlen(v5_ans);

  char buf6[23];
  longlong v6 = LONGLONG_MIN;  // -9223372036854775808
  char v6_ans[] = "-9223372036854775808";
  unsigned int expected_len_6 = strlen(v6_ans);

  char buf7[23];
  longlong v7 = 1;
  char v7_ans[] = "1";
  unsigned int expected_len_7 = strlen(v7_ans);

  char buf8[23];
  longlong v8 = -1;
  char v8_ans[] = "-1";
  unsigned int expected_len_8 = strlen(v8_ans);

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Winteger-overflow"
  // Test overflow results
  char buf9[23];
  longlong v9 = (longlong)LONG_MAX + 1;
  char v9_ans[] = "9223372036854775808";
  unsigned int expected_len_9 = strlen(v9_ans);

  char buf9_signed[23];
  longlong v9_signed = (longlong)LONG_MAX + 1;
  char v9_ans_signed[] = "-9223372036854775808";
  unsigned int expected_len_9_signed = strlen(v9_ans_signed);

  char buf10[23];
  longlong v10 = (longlong)LONG_MIN - 1;
  char v10_ans[] = "9223372036854775807";
  unsigned int expected_len_10 = strlen(v10_ans);
#pragma GCC diagnostic pop

  // Test boundary values between long and longlong
  char buf11[23];
  longlong v11 = (longlong)INT_MAX + 1;
  char v11_ans[] = "2147483648";  // Assuming 32-bit int
  unsigned int expected_len_11 = strlen(v11_ans);

  char buf12[23];
  longlong v12 = (longlong)INT_MIN - 1;
  char v12_ans[] = "-2147483649";  // Assuming 32-bit int
  unsigned int expected_len_12 = strlen(v12_ans);

  r = longlong10_to_str(v1, buf1, 10);
  failed += (r - &buf1[0]) != expected_len_1;
  failed += strcmp(buf1, v1_ans) != 0;

  r = longlong10_to_str(v2, buf2, -10);
  failed += (r - &buf2[0]) != expected_len_2;
  failed += strcmp(buf2, v2_ans) != 0;

  r = longlong10_to_str(v3, buf3, 10);
  failed += (r - &buf3[0]) != expected_len_3;
  failed += strcmp(buf3, v3_ans) != 0;

  r = longlong10_to_str(v4, buf4, -10);
  failed += (r - &buf4[0]) != expected_len_4;
  failed += strcmp(buf4, v4_ans) != 0;

  r = longlong10_to_str(v5, buf5, 10);
  failed += (r - &buf5[0]) != expected_len_5;
  failed += strcmp(buf5, v5_ans) != 0;

  r = longlong10_to_str(v6, buf6, -10);
  failed += (r - &buf6[0]) != expected_len_6;
  failed += strcmp(buf6, v6_ans) != 0;

  r = longlong10_to_str(v7, buf7, 10);
  failed += (r - &buf7[0]) != expected_len_7;
  failed += strcmp(buf7, v7_ans) != 0;

  r = longlong10_to_str(v8, buf8, -10);
  failed += (r - &buf8[0]) != expected_len_8;
  failed += strcmp(buf8, v8_ans) != 0;

  r = longlong10_to_str(v9, buf9, 10);
  failed += (r - &buf9[0]) != expected_len_9;
  failed += strcmp(buf9, v9_ans) != 0;

  r = longlong10_to_str(v9_signed, buf9_signed, -10);
  failed += (r - &buf9_signed[0]) != expected_len_9_signed;
  failed += strcmp(buf9_signed, v9_ans_signed) != 0;

  r = longlong10_to_str(v10, buf10, -10);
  failed += (r - &buf10[0]) != expected_len_10;
  failed += strcmp(buf10, v10_ans) != 0;

  r = longlong10_to_str(v11, buf11, 10);
  failed += (r - &buf11[0]) != expected_len_11;
  failed += strcmp(buf11, v11_ans) != 0;

  r = longlong10_to_str(v12, buf12, -10);
  failed += (r - &buf12[0]) != expected_len_12;
  failed += strcmp(buf12, v12_ans) != 0;

  return failed;
}

int main(int ac, char **av)
{
  size_t i, failed= 0;

  MY_INIT(av[0]);

  plan(5);
  diag("Testing my_like_range_xxx() functions");

  for (i= 0; i < array_elements(charset_list); i++)
  {
    CHARSET_INFO *cs= charset_list[i];
    if (test_like_range_for_charset(cs, "abc%", 4))
    {
      ++failed;
      diag("Failed for %s", cs->coll_name.str);
    }
  }
  ok(failed == 0, "Testing my_like_range_xxx() functions");

  diag("my_ci_strnncollsp()");
  failed= test_strcollsp();
  ok(failed == 0, "Testing my_ci_strnncollsp()");

  diag("Testing min_str() and max_str()");
  failed= test_minmax_str();
  ok(failed == 0, "Testing min_str() and max_str() functions");

  diag("Testing cs->coll->strnncollsp_char()");
  failed= test_strnncollsp_char();
  ok(failed == 0, "Testing cs->coll->strnncollsp_char()");

  diag("Testing longlong10_to_str()");
  failed= test_longlong10_to_str();
  ok(failed == 0, "Testing longlong10_to_str()");

  my_end(0);

  return exit_status();
}
