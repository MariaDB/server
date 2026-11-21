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



/********** Tests for neutral UCA collations ***/

/* Shared data for any strength */
static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_aX_cX[]=
{
  {CSTR("aaaa"), CSTR("aaab"),           -1},
  {CSTR("aaaa"), CSTR("AAAB"),           -1},
  {CSTR("aaaa"), CSTR("AALA"),           -1},
  {CSTR("aaaa"), CSTR("AAAL"),           -1},
  {CSTR("aaaCh"),CSTR("AAAh"),           -1},
  {CSTR("aaacH"),CSTR("AAAh"),           -1},
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_ai_ci[]=
{
  {CSTR("aaaa"), CSTR("AAA" UTF8_ARING),  0},
  {CSTR("aaaa"), CSTR("AAAA"),            0},
  {CSTR("aala"), CSTR("AALA"),            0},
  {CSTR("aaal"), CSTR("AAAL"),            0},
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_as_ci[]=
{
  {CSTR("aaaa"), CSTR("AAA" UTF8_ARING), -1},
  {CSTR("aaaa"), CSTR("AAAA"),            0},
  {CSTR("aala"), CSTR("AALA"),            0},
  {CSTR("aaal"), CSTR("AAAL"),            0},
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_as_cs[]=
{
  {CSTR("aaaa"), CSTR("AAA" UTF8_ARING), -1},
  {CSTR("aaaa"), CSTR("AAAA"),           -1},
  {CSTR("aala"), CSTR("AALA"),           -1},
  {CSTR("aaal"), CSTR("AAAL"),           -1},
  {NULL, 0, NULL, 0, 0}
};



/********** Tests for Czech UCA collations ****/

/* Shared data for any strength */
static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_czech_aX_cX[]=
{
  {CSTR("aaaa"), CSTR("aaab"),           -1},
  {CSTR("aaaa"), CSTR("AAAB"),           -1},
  {CSTR("aaaa"), CSTR("AALA"),           -1},
  {CSTR("aaaa"), CSTR("AAAL"),           -1},
  {CSTR("aaaCh"),CSTR("AAAH"),            1},
  {CSTR("aaacH"),CSTR("AAAH"),           -1},
  {NULL, 0, NULL, 0, 0}
};


/********** Tests for Danish UCA collation ***/

/* Shared data for any strength */
static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_danish_aX_cX[]=
{
  {CSTR("aaaa"), CSTR("aaab"),            1},
  {CSTR("aaaa"), CSTR(UTF8_ARING "ab"),   1},
  {CSTR("aaaa"), CSTR("AAAB"),            1},
  {CSTR("aaaa"), CSTR("AALA"),            1},
  {CSTR("aaaa"), CSTR("AAAL"),            1},
  {CSTR("aaaCh"),CSTR("AAAH"),           -1},
  {CSTR("aaacH"),CSTR("AAAH"),           -1},
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_danish_ai_ci[]=
{
  {CSTR("aaaa"), CSTR("AAAA"),            0},
  {CSTR("aaAA"), CSTR("AA" UTF8_ARING),   0},
  {CSTR("aaaa"), CSTR("AA" UTF8_ARING),   0},
  {CSTR("aaaA"), CSTR("AA" UTF8_ARING),  -1},
  {CSTR("aaal"), CSTR("AAAL"),            0},
  {CSTR("aaal"), CSTR(UTF8_ARING "AL"),   0},
  {CSTR("Aaal"), CSTR(UTF8_ARING "AL"),   0},
  {CSTR("AAal"), CSTR(UTF8_ARING "AL"),   0},
  {CSTR("aAal"), CSTR(UTF8_ARING "AL"),  -1},
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_danish_as_ci[]=
{
  {CSTR("aaaa"), CSTR("AA" UTF8_ARING),   1},
  {CSTR("aaaa"), CSTR("AAA" UTF8_ARING),  1},
  {CSTR("aaaa"), CSTR("AAAA"),            0},
  {CSTR("aala"), CSTR("AALA"),            0},
  {CSTR("aala"), CSTR(UTF8_ARING "LA"),   1},
  {CSTR("aaal"), CSTR("AAAL"),            0},
  {NULL, 0, NULL, 0, 0}
};


static STRNNCOLL_PARAM strcoll_utf8mb4_uca1400_danish_as_cs[]=
{
  {CSTR("aaaa"), CSTR("AAAA"),           -1},
  {CSTR("aaaa"), CSTR("aaAa"),           -1},
  {CSTR("aaAa"), CSTR("aaAA"),           -1},
  {CSTR("aala"), CSTR("AALA"),           -1},
  {CSTR("aaal"), CSTR("AAAL"),           -1},
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
    snprintf(dst, dstend - dst, "%02X", (unsigned char) src[0]);
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
strcollsp_by_name(const char *collation, const STRNNCOLL_PARAM *param)
{
  CHARSET_INFO *cs= get_charset_by_name(collation, MYF(0));

  if (!cs)
  {
    diag("get_charset_by_name() failed");
    return 1;
  }
  return strcollsp(cs, param);
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

  failed+= strcollsp_by_name("utf8mb4_uca1400_ai_ci",         strcoll_utf8mb4_uca1400_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_ai_ci",         strcoll_utf8mb4_uca1400_ai_ci);

  failed+= strcollsp_by_name("utf8mb4_uca1400_as_ci",         strcoll_utf8mb4_uca1400_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_as_ci",         strcoll_utf8mb4_uca1400_as_ci);

  failed+= strcollsp_by_name("utf8mb4_uca1400_as_cs",         strcoll_utf8mb4_uca1400_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_as_cs",         strcoll_utf8mb4_uca1400_as_cs);

  failed+= strcollsp_by_name("utf8mb4_uca1400_czech_ai_ci",   strcoll_utf8mb4_uca1400_czech_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_czech_ai_ci",   strcoll_utf8mb4_uca1400_ai_ci);

  failed+= strcollsp_by_name("utf8mb4_uca1400_czech_as_ci",   strcoll_utf8mb4_uca1400_czech_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_czech_as_ci",   strcoll_utf8mb4_uca1400_as_ci);

  failed+= strcollsp_by_name("utf8mb4_uca1400_czech_as_cs",   strcoll_utf8mb4_uca1400_czech_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_czech_as_cs",   strcoll_utf8mb4_uca1400_as_cs);

  failed+= strcollsp_by_name("utf8mb4_uca1400_danish_ai_ci",  strcoll_utf8mb4_uca1400_danish_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_danish_ai_ci",  strcoll_utf8mb4_uca1400_danish_ai_ci);

  failed+= strcollsp_by_name("utf8mb4_uca1400_danish_as_ci",  strcoll_utf8mb4_uca1400_danish_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_danish_as_ci",  strcoll_utf8mb4_uca1400_danish_as_ci);

  failed+= strcollsp_by_name("utf8mb4_uca1400_danish_as_cs",  strcoll_utf8mb4_uca1400_danish_aX_cX);
  failed+= strcollsp_by_name("utf8mb4_uca1400_danish_as_cs",  strcoll_utf8mb4_uca1400_danish_as_cs);

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


/*
  my_mb_wc_filename() decodes a 5-byte '@HHHH' escape. Two truncated inputs
  must not read past the intended byte ('0','0' has no entry in the 3-byte
  table, so both reach the hex branch):
   - "@000" with end= buf + 4 hits the s + 5 > e bound and returns
     MY_CS_TOOSMALL5 without consuming buf[4].
   - "@00\0" is NUL-terminated at s[3]. 'e' may be fake (see the note above
     my_mb_wc_filename), so with an over-long end the s[3] guard must keep
     it from reading s[4] past the terminator and return MY_CS_ILSEQ.
*/
static int test_mb_wc_filename_truncated()
{
  CHARSET_INFO *cs= &my_charset_filename;
  my_wc_t wc= 0;
  int failed= 0;
  uchar tight[5]= {'@', '0', '0', '0', '1'};
  uchar *terminated= (uchar*) malloc(4);

  if (my_ci_mb_wc(cs, &wc, tight, tight + 4) != MY_CS_TOOSMALL5)
    failed++;

  terminated[0]= '@'; terminated[1]= '0'; terminated[2]= '0';
  terminated[3]= '\0';
  if (my_ci_mb_wc(cs, &wc, terminated, terminated + 5) != MY_CS_ILSEQ)
    failed++;
  free(terminated);

  return failed;
}


/*
  my_string_repertoire() must not return a repertoire which is a subset
  of ASCII (e.g. MY_REPERTOIRE_NONE, which is compatible with everything)
  for a string with a bad byte sequence, an unassigned character, or
  an incomplete character: the characters of such strings are unknown.
*/
typedef struct
{
  CHARSET_INFO *cs;
  const char *str;
  size_t length;
  my_repertoire_t expected_at_least; /* Bits that must be set */
  my_bool ascii_only;                /* Is the result a subset of ASCII? */
} STRING_REPERTOIRE_PARAM;


static int test_string_repertoire()
{
#define LOWER MY_REPERTOIRE_ASCII_LETTERS_LOWER
#define EXT   MY_REPERTOIRE_EXTENDED
  static const STRING_REPERTOIRE_PARAM param[]=
  {
    /* Valid strings */
    {&my_charset_utf16_general_ci, "\0a\0b",     4, LOWER, TRUE},
    {&my_charset_utf16_general_ci, "\0\xE9",     2, EXT,   FALSE},
    {&my_charset_ucs2_general_ci,  "\0a",         2, LOWER, TRUE},
    {&my_charset_utf32_general_ci, "\0\0\0a",   4, LOWER, TRUE},
    /* A lone high surrogate */
    {&my_charset_utf16_general_ci, "\xD8\0",     2, EXT,   FALSE},
    /* ASCII characters followed by a lone high surrogate */
    {&my_charset_utf16_general_ci, "\0a\xD8\0", 4, EXT,   FALSE},
    /* An incomplete character at the end */
    {&my_charset_utf16_general_ci, "\0a\0",      3, EXT,   FALSE},
    /* A code point beyond U+10FFFF */
    {&my_charset_utf32_general_ci, "\0\x11\0\0", 4, EXT, FALSE}
  };
#undef LOWER
#undef EXT
  size_t i;
  int failed= 0;
  for (i= 0; i < array_elements(param); i++)
  {
    const STRING_REPERTOIRE_PARAM *p= &param[i];
    my_repertoire_t rep= my_string_repertoire(p->cs, p->str, p->length);
    if ((rep & p->expected_at_least) != p->expected_at_least ||
        my_repertoire_is_subset_of(rep, MY_REPERTOIRE_ASCII) != p->ascii_only)
    {
      diag("my_string_repertoire() failed for %s, test #%d: %d",
           p->cs->coll_name.str, (int) i, (int) rep);
      failed++;
    }
  }
  return failed;
}


/*
  The repertoire of an ASCII based character, by the definitions of
  MY_REPERTOIRE_XXX.
*/
static my_repertoire_t ascii_based_char_repertoire(uint ch)
{
  if (ch > 0x7F)
    return MY_REPERTOIRE_EXTENDED;
  if (ch >= '0' && ch <= '9')
    return MY_REPERTOIRE_ASCII_DIGITS;
  if (ch >= 'A' && ch <= 'Z')
    return MY_REPERTOIRE_ASCII_LETTERS_UPPER;
  if (ch >= 'a' && ch <= 'z')
    return MY_REPERTOIRE_ASCII_LETTERS_LOWER;
  if (ch == '_')
    return MY_REPERTOIRE_ASCII_UNDERSCORE;
  if (ch == '.')
    return MY_REPERTOIRE_ASCII_DOT;
  if (ch == '-')
    return MY_REPERTOIRE_ASCII_MINUS;
  return MY_REPERTOIRE_ASCII_NOT_IDENT;
}


/*
  The union of the repertoires of the bytes of an ASCII based string,
  except the byte at the position skip.
*/
static my_repertoire_t
ascii_based_string_repertoire(const char *str, size_t length, size_t skip)
{
  my_repertoire_t res= MY_REPERTOIRE_NONE;
  size_t i;
  for (i= 0; i < length; i++)
  {
    if (i != skip)
      res|= ascii_based_char_repertoire((uchar) str[i]);
  }
  return res;
}


/* Report only the first failures: a broken scan fails millions of checks */
#define REPERTOIRE_FAILURES_TO_REPORT 20
static int repertoire_failures= 0;

/* Lower case letters, and a character of every ASCII repertoire */
static const char *fills[]= {"a", "Aa0_.-;"};


static int
test_string_repertoire_one(CHARSET_INFO *cs, const char *str, size_t length,
                           size_t align, size_t fill, size_t pos, uint byte,
                           my_repertoire_t expected)
{
  MY_STRING_METADATA metadata;
  my_repertoire_t rep= my_string_repertoire(cs, str, length);
  my_string_metadata_get(&metadata, cs, str, length);
  if (rep == expected && metadata.repertoire == expected &&
      metadata.char_length == length)
    return 0;
  if (repertoire_failures++ < REPERTOIRE_FAILURES_TO_REPORT)
    diag("%s: fill=%u align=%u length=%u pos=%u byte=0x%02X expected=%u "
         "my_string_repertoire()=%u my_string_metadata_get()=%u,%u",
         cs->coll_name.str, (uint) fill, (uint) align, (uint) length,
         (uint) pos, byte, (uint) expected, (uint) rep,
         (uint) metadata.repertoire, (uint) metadata.char_length);
  return 1;
}


/*
  my_string_repertoire() and my_string_metadata_get() on 8-bit ASCII based
  character sets: the repertoire of a string is the union of the
  repertoires of its bytes. The scan looks bytes up 8 at a time, and
  once it has found every ASCII repertoire it tests a machine word at a
  time for a byte above 0x7F. The rest is read byte by byte. So put every
  byte value at every position of strings that are shorter than, equal to
  and longer than a few groups of 8 bytes, at every starting alignment,
  among lower case letters, and among characters of every ASCII
  repertoire, which the scan finds within the first or the second group
  of 8 bytes. The bytes around the string are above 0x7F, so a scan that
  reads outside the string finds MY_REPERTOIRE_EXTENDED in an ASCII
  string.
  Then check every ASCII character in utf8mb4, for which
  my_string_repertoire() reads the bytes as above, and
  my_string_metadata_get() decodes the characters.
*/
static int
test_string_repertoire_8bit()
{
  static CHARSET_INFO *charsets[]= {&my_charset_bin, &my_charset_latin1};
  /* 64 bytes for the string and its alignment, then 8 guard bytes */
  char buf[64 + 8];
  int failed= 0;
  size_t c, f, align, length, pos, i;
  uint byte;

  for (c= 0; c < array_elements(charsets); c++)
  {
    for (f= 0; f < array_elements(fills); f++)
    {
      size_t fill_length= strlen(fills[f]);
      for (align= 0; align < 8; align++)
      {
        char *str= buf + align;
        for (length= 0; length + align <= 64; length++)
        {
          memset(buf, 0x80, sizeof(buf));
          for (i= 0; i < length; i++)
            str[i]= fills[f][i % fill_length];
          failed+= test_string_repertoire_one(charsets[c], str, length,
                     align, f, length, 0,
                     ascii_based_string_repertoire(str, length, length));
          for (pos= 0; pos < length; pos++)
          {
            my_repertoire_t others=
              ascii_based_string_repertoire(str, length, pos);
            for (byte= 0; byte < 256; byte++)
            {
              str[pos]= (char) byte;
              failed+= test_string_repertoire_one(charsets[c], str, length,
                         align, f, pos, byte,
                         others | ascii_based_char_repertoire(byte));
            }
            str[pos]= fills[f][pos % fill_length];
          }
        }
      }
    }
  }

  for (byte= 0; byte < 0x80; byte++)
  {
    char ch= (char) byte;
    failed+= test_string_repertoire_one(&my_charset_utf8mb4_general_ci,
                                        &ch, 1, 0, 0, 0, byte,
                                        ascii_based_char_repertoire(byte));
  }
  if (failed > REPERTOIRE_FAILURES_TO_REPORT)
    diag("%d checks failed, the first %d are reported",
         failed, REPERTOIRE_FAILURES_TO_REPORT);
  return failed;
}


/*
  The metadata of a string by its definition: decode it character by
  character. A bad byte sequence is a character of one byte, an
  unassigned character is one character, and an incomplete character at
  the end is not counted. All of them are MY_REPERTOIRE_EXTENDED.
*/
static void
string_metadata_char_by_char(MY_STRING_METADATA *metadata,
                             CHARSET_INFO *cs, const char *str, size_t length)
{
  const uchar *s= (const uchar *) str, *end= s + length;
  uint repertoire= MY_REPERTOIRE_NONE;
  size_t char_length= 0;
  while (s < end)
  {
    my_wc_t wc;
    int mblen= my_ci_mb_wc(cs, &wc, s, end);
    if (mblen > 0)
    {
      repertoire|= ascii_based_char_repertoire((uint) wc);
      s+= mblen;
    }
    else
    {
      repertoire|= MY_REPERTOIRE_EXTENDED;
      if (mblen == MY_CS_ILSEQ)
        s++;
      else if (mblen > MY_CS_TOOSMALL)
        s+= -mblen;
      else
        break;
    }
    char_length++;
  }
  metadata->repertoire= (my_repertoire_t) repertoire;
  metadata->char_length= char_length;
}


typedef struct
{
  uchar str[MY_CS_MBMAXLEN];
  size_t length;
} STRING_METADATA_PROBE;

#define STRING_METADATA_MAX_LENGTH 32
/*
  Every byte value, then for every length up to MY_CS_MBMAXLEN a
  character and the incomplete characters at its start, then an
  unassigned character
*/
#define STRING_METADATA_MAX_PROBES (256 + MY_CS_MBMAXLEN * MY_CS_MBMAXLEN)


static int
test_string_metadata_mb_one(CHARSET_INFO *cs, const char *str, size_t length,
                            size_t fill, size_t pos,
                            const STRING_METADATA_PROBE *probe)
{
  MY_STRING_METADATA metadata, expected;
  my_string_metadata_get(&metadata, cs, str, length);
  string_metadata_char_by_char(&expected, cs, str, length);
  if (metadata.repertoire == expected.repertoire &&
      metadata.char_length == expected.char_length)
    return 0;
  if (repertoire_failures++ < REPERTOIRE_FAILURES_TO_REPORT)
    diag("%s: fill=%u length=%u pos=%u probe=0x%02X,%u bytes "
         "expected=%u,%u my_string_metadata_get()=%u,%u",
         cs->coll_name.str, (uint) fill, (uint) length, (uint) pos,
         probe ? (uint) probe->str[0] : 0, probe ? (uint) probe->length : 0,
         (uint) expected.repertoire, (uint) expected.char_length,
         (uint) metadata.repertoire, (uint) metadata.char_length);
  return 1;
}


/*
  my_string_metadata_get() on the character sets other than the 8-bit
  ASCII based ones, against decoding the string character by character.
  In an ASCII based character set a byte below 0x80 at the start of a
  character is an ASCII character of its own, so after an ASCII
  character the scan takes the bytes below 0x80 that follow it 8 at a
  time, without decoding them. The other character sets, such as ucs2
  or sjis, must still decode every character.
  Put a probe at every position of strings that are shorter than, equal
  to and longer than a few groups of 8 bytes, among lower case letters,
  and among characters of every ASCII repertoire. The probes are every
  byte value, the first character of every length above 1 that the
  character set has, the bytes at the start of each such character,
  which are an incomplete character at the end of the string, and two
  bytes above 0x7F that make an unassigned character, if any do.
  A byte above 0x7F can also make a character with the bytes after it,
  e.g. 0x81 with 'a' in gbk, so the next ASCII characters do not start
  where they do in the string without it. The bytes after the string
  are ASCII, so a scan that reads past its end finds more characters.
*/
static int
test_string_metadata_mb()
{
  char buf[STRING_METADATA_MAX_LENGTH + 8];
  STRING_METADATA_PROBE probes[STRING_METADATA_MAX_PROBES];
  int failed= 0;
  uint id;

  repertoire_failures= 0;
  /*
    Initialize only the primary collations, rather than the hundreds of
    collations of all ids. The first lookup fills all_charsets[].
  */
  (void) get_charset_name(1);
  for (id= 1; id < MY_ALL_CHARSETS_SIZE; id++)
  {
    CHARSET_INFO *cs= all_charsets[id];
    size_t probe_count, f, length, pos, p, i;
    uint lengths= 0, all_lengths; /* Bit N: a character of N bytes */
    uint b;
    my_wc_t wc;
    if (!cs || !(cs->state & MY_CS_PRIMARY) ||
        !(cs= get_charset(id, MYF(0))) ||
        (cs->mbmaxlen == 1 && !(cs->state & MY_CS_NONASCII)))
      continue;

    for (probe_count= 0; probe_count < 256; probe_count++)
    {
      probes[probe_count].str[0]= (uchar) probe_count;
      probes[probe_count].length= 1;
    }
    all_lengths= ((2U << cs->mbmaxlen) - 1) & ~3U;
    for (wc= 0x80; wc <= 0x10FFFF && lengths != all_lengths; wc++)
    {
      size_t first= probe_count;
      int rc= my_ci_wc_mb(cs, wc, probes[first].str,
                          probes[first].str + MY_CS_MBMAXLEN);
      if (rc < 2 || (lengths & (1U << rc)))
        continue;
      lengths|= 1U << rc;
      probes[probe_count++].length= (size_t) rc;
      for (i= 2; i < (size_t) rc; i++)
      {
        probes[probe_count]= probes[first];
        probes[probe_count++].length= i;
      }
    }
    for (b= 0; b < 0x80 * 0x80; b++)
    {
      uchar *str= probes[probe_count].str;
      int rc;
      str[0]= (uchar) (0x80 | b >> 7);
      str[1]= (uchar) (0x80 | (b & 0x7F));
      rc= my_ci_mb_wc(cs, &wc, str, str + 2);
      if (rc < 0 && rc > MY_CS_TOOSMALL)
      {
        probes[probe_count++].length= 2;
        break;
      }
    }

    for (f= 0; f < array_elements(fills); f++)
    {
      size_t fill_length= strlen(fills[f]);
      for (length= 0; length <= STRING_METADATA_MAX_LENGTH; length++)
      {
        memset(buf, '~', sizeof(buf));
        for (i= 0; i < length; i++)
          buf[i]= fills[f][i % fill_length];
        failed+= test_string_metadata_mb_one(cs, buf, length, f, length,
                                             NULL);
        for (p= 0; p < probe_count; p++)
        {
          for (pos= 0; pos + probes[p].length <= length; pos++)
          {
            memcpy(buf + pos, probes[p].str, probes[p].length);
            failed+= test_string_metadata_mb_one(cs, buf, length, f, pos,
                                                 &probes[p]);
            for (i= pos; i < pos + probes[p].length; i++)
              buf[i]= fills[f][i % fill_length];
          }
        }
      }
    }
  }
  if (failed > REPERTOIRE_FAILURES_TO_REPORT)
    diag("%d checks failed, the first %d are reported",
         failed, REPERTOIRE_FAILURES_TO_REPORT);
  return failed;
}


int main(int ac, char **av)
{
  size_t i, failed= 0;

  MY_INIT(av[0]);

  plan(8);
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

  diag("Testing my_charset_filename mb_wc end-pointer bounds");
  ok(test_mb_wc_filename_truncated() == 0,
     "filename decoder does not read past the end pointer");

  diag("Testing my_string_repertoire() for strings with bad characters");
  ok(test_string_repertoire() == 0,
     "my_string_repertoire() does not return ASCII for bad strings");

  diag("MDEV-42316 A binary string parameter is read byte by byte on every "
       "execution to compute its repertoire, and since MDEV-41246 this read "
       "takes about 1.2 ms per MB of ASCII data");
  diag("Testing my_string_repertoire() on 8-bit character sets");
  ok(test_string_repertoire_8bit() == 0,
     "8-bit repertoire scan finds every byte value at any position");

  diag("MDEV-42316 A binary string parameter is read byte by byte on every "
       "execution to compute its repertoire, and since MDEV-41246 this read "
       "takes about 1.2 ms per MB of ASCII data");
  diag("Testing my_string_metadata_get() on multibyte character sets");
  ok(test_string_metadata_mb() == 0,
     "multibyte metadata equals decoding character by character");

  my_end(0);

  return exit_status();
}
