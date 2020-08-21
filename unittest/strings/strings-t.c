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
       cs->col_name.str);
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
         cs->col_name.str, ahex, bhex, p->res, res,
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


int main()
{
  size_t i, failed= 0;
  
  plan(2);
  diag("Testing my_like_range_xxx() functions");
  
  for (i= 0; i < array_elements(charset_list); i++)
  {
    CHARSET_INFO *cs= charset_list[i];
    if (test_like_range_for_charset(cs, "abc%", 4))
    {
      ++failed;
      diag("Failed for %s", cs->col_name.str);
    }
  }
  ok(failed == 0, "Testing my_like_range_xxx() functions");

  diag("my_ci_strnncollsp()");
  failed= test_strcollsp();
  ok(failed == 0, "Testing my_ci_strnncollsp()");

  return exit_status();
}
