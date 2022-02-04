/*
   Copyright (c) 2015, MariaDB Foundation
   Copyright (c) 2015, 2020, MariaDB Corporation.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include "ctype-ascii.h"

#ifndef MY_FUNCTION_NAME
#error MY_FUNCTION_NAME is not defined
#endif

/*
  Define strnncoll() and strnncollsp() by default,
  unless "#define DEFINE_STRNNCOLL 0" is specified.
*/
#ifndef DEFINE_STRNNCOLL
#define DEFINE_STRNNCOLL 1
#endif


/*
  The weight for automatically padded spaces when comparing strings with
  the PAD SPACE property.
  Should normally be equal to the weight of a regular space.
*/
#ifndef WEIGHT_PAD_SPACE
#define WEIGHT_PAD_SPACE  (' ')
#endif


/*
  For binary collations:
  - on 32bit platforms perform only 4 byte optimization
  - on 64bit platforms perform both 4 byte and 8 byte optimization
*/
#if defined(STRCOLL_MB7_BIN)
#define MY_STRCOLL_MB7_4BYTES(a,b) my_strcoll_mb7_bin_4bytes((a),(b))
#if SIZEOF_VOIDP == 8
#define STRCOLL_MB7_8BYTES
#define MY_STRCOLL_MB7_8BYTES(a,b) my_strcoll_mb7_bin_8bytes((a),(b))
#endif /* Architecture test */
#endif /* STRCOLL_MB7_BIN */


/*
  For case insensitive collations with trivial mapping from [a-z] to [A-Z]
  perform optimization only on 64 bit platforms.
  There is no sense to perform my_ascii_to_upper_magic_uint64() based
  optimization on 32bit platforms. The idea of this optimization
  is that it handles 8bytes at a time, using 64bit CPU registers.
  Enabling this optimization on 32bit platform may only slow things down.
*/
#if defined(STRCOLL_MB7_TOUPPER)
#if SIZEOF_VOIDP == 8
#define MY_STRCOLL_MB7_4BYTES(a,b) my_strcoll_ascii_toupper_4bytes((a),(b))
#define MY_STRCOLL_MB7_8BYTES(a,b) my_strcoll_ascii_toupper_8bytes((a),(b))
#endif /* Architecture test */
#endif /* STRCOLL_MB7_TOUPPER */


/*
  A helper macro to shift two pointers forward, to the given amount.
*/
#define MY_STRING_SHIFT_PTR_PTR(a,b,len) do { a+= len; b+= len; } while(0)


/*
  Weight of an illegal byte, must follow these rules:
  1. Must be greater than weight of any normal character in the collation.
  2. Two different bad bytes must have different weights and must be
     compared in their binary order.

  Depends on mbmaxlen of the character set, as well as how the collation
  sorts various single-byte and multi-byte character blocks.

  The macro below is the default definition, it is suitable for mbmaxlen=2
  character sets that sort all multi-byte characters after all single-byte
  characters: big5, euckr, gb2312, gbk.

  All mbmaxlen>2 character sets must provide their own definitions.
  All collations that have a more complex order (than just MB1 followed by MB2)
  must also provide their own definitions (see definitions for
  cp932_japanese_ci and sjis_japanese_ci as examples of a more complex order).
*/
#ifndef WEIGHT_ILSEQ
#define WEIGHT_ILSEQ(x)   (0xFF00 + (x))
#endif


#if DEFINE_STRNNCOLL

/**
  Scan a valid character, or a bad byte, or an auto-padded space
  from a string and calculate the weight of the scanned sequence.

  @param [OUT] weight - the weight is returned here
  @param str          - the string
  @param end          - the end of the string
  @return             - the number of bytes scanned

  The including source file must define the following macros:
  IS_MB1_CHAR(b0)          - for character sets that have MB1 characters
  IS_MB1_MB2HEAD_GAP(b0)   - optional, for better performance
  IS_MB2_CHAR(b0,b1)       - for character sets that have MB2 characters
  IS_MB3_CHAR(b0,b1,b2)    - for character sets that have MB3 characters
  IS_MB4_CHAR(b0,b1,b2,b3) - for character sets with have MB4 characters
  WEIGHT_PAD_SPACE
  WEIGHT_MB1(b0)           - for character sets that have MB1 characters
  WEIGHT_MB2(b0,b1)        - for character sets that have MB2 characters
  WEIGHT_MB3(b0,b1,b2)     - for character sets that have MB3 characters
  WEIGHT_MB4(b0,b1,b2,b3)  - for character sets that have MB4 characters
  WEIGHT_ILSEQ(x)
*/
static inline uint
MY_FUNCTION_NAME(scan_weight)(int *weight, const uchar *str, const uchar *end)
{
  if (str >= end)
  {
    *weight= WEIGHT_PAD_SPACE;
    return 0;
  }

#ifdef IS_MB1_CHAR
  if (IS_MB1_CHAR(*str))
  {
    *weight= WEIGHT_MB1(*str);           /* A valid single byte character*/
    return 1;
  }
#endif

#ifdef IS_MB1_MBHEAD_UNUSED_GAP
  /*
    Quickly filter out unused bytes that are neither MB1 nor MBHEAD.
    E.g. [0x80..0xC1] in utf8mb(3|4). This allows using simplified conditions
    in IS_MB2_CHAR(), IS_MB3_CHAR(), etc.
  */
  if (IS_MB1_MBHEAD_UNUSED_GAP(*str))
    goto bad;
#endif

#ifdef IS_MB2_CHAR
  if (str + 2 > end)                     /* The string ended unexpectedly */
    goto bad;                            /* Treat as a bad byte */

  if (IS_MB2_CHAR(str[0], str[1]))
  {
    *weight= WEIGHT_MB2(str[0], str[1]);
    return 2;                            /* A valid two-byte character */
  }
#endif

#ifdef IS_MB3_CHAR
  if (str + 3 > end)                     /* Incomplete three-byte character */
    goto bad;

  if (IS_MB3_CHAR(str[0], str[1], str[2]))
  {
    *weight= WEIGHT_MB3(str[0], str[1], str[2]);
    return 3;                            /* A valid three-byte character */
  }
#endif

#ifdef IS_MB4_CHAR
  if (str + 4 > end)                     /* Incomplete four-byte character */
    goto bad;

  if (IS_MB4_CHAR(str[0], str[1], str[2], str[3]))
  {
    *weight= WEIGHT_MB4(str[0], str[1], str[2], str[3]);
    return 4;                            /* A valid four-byte character */
  }

#endif

bad:
  *weight= WEIGHT_ILSEQ(str[0]);         /* Bad byte */
  return 1;
}


/**
  Compare two strings according to the collation,
  without handling the PAD SPACE property.

  Note, strnncoll() is usually used to compare identifiers.
  Perhaps we should eventually (in 10.2?) create a new collation 
  my_charset_utf8mb3_general_ci_no_pad and have only one comparison function
  in MY_COLLATION_HANDLER.

  @param cs          - the character set and collation
  @param a           - the left string
  @param a_length    - the length of the left string
  @param b           - the right string
  @param b_length    - the length of the right string
  @param b_is_prefix - if the caller wants to check if "b" is a prefix of "a"
  @return            - the comparison result
*/
static int
MY_FUNCTION_NAME(strnncoll)(CHARSET_INFO *cs __attribute__((unused)),
                            const uchar *a, size_t a_length, 
                            const uchar *b, size_t b_length,
                            my_bool b_is_prefix)
{
  const uchar *a_end= a + a_length;
  const uchar *b_end= b + b_length;
  for ( ; ; )
  {
    int a_weight, b_weight, res;
    uint a_wlen= MY_FUNCTION_NAME(scan_weight)(&a_weight, a, a_end);
    uint b_wlen;

#ifdef MY_STRCOLL_MB7_4BYTES
    if (a_wlen == 1 && my_strcoll_ascii_4bytes_found(a, a_end, b, b_end))
    {
      int res;
#ifdef MY_STRCOLL_MB7_8BYTES
      /*TODO: a a loop here >='a' <='z' here, for automatic vectorization*/
      if (my_strcoll_ascii_4bytes_found(a + 4, a_end, b + 4, b_end))
      {
        if ((res= MY_STRCOLL_MB7_8BYTES(a, b)))
          return res;
        MY_STRING_SHIFT_PTR_PTR(a, b, 8);
        continue;
      }
#endif
      if ((res= MY_STRCOLL_MB7_4BYTES(a, b)))
        return res;
      MY_STRING_SHIFT_PTR_PTR(a, b, 4);
      continue;
    }
#endif /* MY_STRCOLL_MB7_4BYTES */

    b_wlen= MY_FUNCTION_NAME(scan_weight)(&b_weight, b, b_end);

    /*
      a_wlen  b_wlen Comment
      ------  ------ -------
      0       0      Strings ended simultaneously, "a" and "b" are equal.
      0       >0     "a" is a prefix of "b", so "a" is smaller.
      >0      0      "b" is a prefix of "a", check b_is_prefix.
      >0      >0     Two weights were scanned, check weight difference.
    */
    if (!a_wlen)
      return b_wlen ? -b_weight : 0;

    if (!b_wlen)
      return b_is_prefix ? 0 : a_weight;

    if ((res= (a_weight - b_weight)))
      return res;
    /*
      None of the strings has ended yet.
    */
    DBUG_ASSERT(a < a_end);
    DBUG_ASSERT(b < b_end);
    a+= a_wlen;
    b+= b_wlen;
  }
  DBUG_ASSERT(0);
  return 0;
}


#ifdef DEFINE_STRNNCOLLSP_NOPAD

/**
  Compare two strings according to the collation, with NO PAD handling.

  @param cs          - the character set and collation
  @param a           - the left string
  @param a_length    - the length of the left string
  @param b           - the right string
  @param b_length    - the length of the right string
  @return            - the comparison result
*/
static int
MY_FUNCTION_NAME(strnncollsp)(CHARSET_INFO *cs __attribute__((unused)),
                                    const uchar *a, size_t a_length,
                                    const uchar *b, size_t b_length)
{
  return MY_FUNCTION_NAME(strnncoll)(cs, a, a_length, b, b_length, FALSE);
}
#else
/**
  Compare two strings according to the collation, with PAD SPACE handling.

  @param cs          - the character set and collation
  @param a           - the left string
  @param a_length    - the length of the left string
  @param b           - the right string
  @param b_length    - the length of the right string
  @return            - the comparison result
*/
static int
MY_FUNCTION_NAME(strnncollsp)(CHARSET_INFO *cs __attribute__((unused)),
                              const uchar *a, size_t a_length, 
                              const uchar *b, size_t b_length)
{
  const uchar *a_end= a + a_length;
  const uchar *b_end= b + b_length;
  for ( ; ; )
  {
    int a_weight, b_weight, res;
    uint a_wlen= MY_FUNCTION_NAME(scan_weight)(&a_weight, a, a_end);
    uint b_wlen;

#ifdef MY_STRCOLL_MB7_4BYTES
    if (a_wlen == 1 && my_strcoll_ascii_4bytes_found(a, a_end, b, b_end))
    {
      int res;
#ifdef MY_STRCOLL_MB7_8BYTES
      if (my_strcoll_ascii_4bytes_found(a + 4, a_end, b + 4, b_end))
      {
        if ((res= MY_STRCOLL_MB7_8BYTES(a, b)))
          return res;
        MY_STRING_SHIFT_PTR_PTR(a, b, 8);
        continue;
      }
#endif
      if ((res= MY_STRCOLL_MB7_4BYTES(a, b)))
        return res;
      MY_STRING_SHIFT_PTR_PTR(a, b, 4);
      continue;
    }
#endif /* MY_STRCOLL_MB7_4BYTES */

    b_wlen= MY_FUNCTION_NAME(scan_weight)(&b_weight, b, b_end);

    if ((res= (a_weight - b_weight)))
    {
      /*
        Got two different weights. Each weight can be generated by either of:
        - a real character
        - a bad byte sequence or an incomplete byte sequence
        - an auto-generated trailing space (PAD SPACE)
        It does not matter how exactly each weight was generated.
        Just return the weight difference.
      */
      return res;
    }
    if (!a_wlen && !b_wlen)
    {
      /*
        Got two auto-generated trailing spaces, i.e.
        both strings have now ended, so they are equal.
      */
      DBUG_ASSERT(a == a_end);
      DBUG_ASSERT(b == b_end);
      return 0;
    }
    /*
      At least one of the strings has not ended yet, continue comparison.
    */
    DBUG_ASSERT(a < a_end || b < b_end);
    a+= a_wlen;
    b+= b_wlen;
  }
  DBUG_ASSERT(0);
  return 0;
}
#endif /* DEFINE_STRNNCOLLSP_NOPAD */


/**
  Compare two strings according to the collation,
  with trailing space padding or trimming, according to "nchars".

  @param cs          - the character set and collation
  @param a           - the left string
  @param a_length    - the length of the left string
  @param b           - the right string
  @param b_length    - the length of the right string
  @param nchars      - compare this amount of characters only
  @return            - the comparison result
*/
static int
MY_FUNCTION_NAME(strnncollsp_nchars)(CHARSET_INFO *cs __attribute__((unused)),
                                     const uchar *a, size_t a_length,
                                     const uchar *b, size_t b_length,
                                     size_t nchars)
{
  const uchar *a_end= a + a_length;
  const uchar *b_end= b + b_length;
  for ( ; nchars ; nchars--)
  {
    int a_weight, b_weight, res;
    uint a_wlen= MY_FUNCTION_NAME(scan_weight)(&a_weight, a, a_end);
    uint b_wlen= MY_FUNCTION_NAME(scan_weight)(&b_weight, b, b_end);

    if ((res= (a_weight - b_weight)))
    {
      /* Got two different weights. See comments in strnncollsp above. */
      return res;
    }
    if (!a_wlen && !b_wlen)
    {
      /* Got two auto-generated trailing spaces. */
      DBUG_ASSERT(a == a_end);
      DBUG_ASSERT(b == b_end);
      return 0;
    }
    /*
      At least one of the strings has not ended yet, continue comparison.
    */
    DBUG_ASSERT(a < a_end || b < b_end);
    a+= a_wlen;
    b+= b_wlen;
  }
  return 0;
}


#endif /* DEFINE_STRNNCOLL */


#ifdef DEFINE_STRNXFRM
#ifndef WEIGHT_MB2_FRM
#define WEIGHT_MB2_FRM(x,y)  WEIGHT_MB2(x,y)
#endif

static size_t
MY_FUNCTION_NAME(strnxfrm)(CHARSET_INFO *cs,
                           uchar *dst, size_t dstlen, uint nweights,
                           const uchar *src, size_t srclen, uint flags)
{
  uchar *d0= dst;
  uchar *de= dst + dstlen;
  const uchar *se= src + srclen;
  const uchar *sort_order= cs->sort_order;

  for (; dst < de && src < se && nweights; nweights--)
  {
    if (my_ci_charlen(cs, src, se) > 1)
    {
      /*
        Note, it is safe not to check (src < se)
        in the code below, because my_ci_charlen() would
        not return 2 if src was too short
      */
      uint16 e= WEIGHT_MB2_FRM(src[0], src[1]);
      *dst++= (uchar) (e >> 8);
      if (dst < de)
        *dst++= (uchar) (e & 0xFF);
      src+= 2;
    }
    else
      *dst++= sort_order ? sort_order[*src++] : *src++;
  }
#ifdef DEFINE_STRNNCOLLSP_NOPAD
  return my_strxfrm_pad_desc_and_reverse_nopad(cs, d0, dst, de,
					       nweights, flags, 0);
#else
  return my_strxfrm_pad_desc_and_reverse(cs, d0, dst, de, nweights, flags, 0);
#endif
}
#endif /* DEFINE_STRNXFRM */


#if defined(DEFINE_STRNXFRM_UNICODE) || defined(DEFINE_STRNXFRM_UNICODE_NOPAD)

/*
  Store sorting weights using 2 bytes per character.

  This function is shared between
  - utf8mb3_general_ci, utf8mb3_bin, ucs2_general_ci, ucs2_bin
    which support BMP only (U+0000..U+FFFF).
  - utf8mb4_general_ci, utf16_general_ci, utf32_general_ci,
    which map all supplementary characters to weight 0xFFFD.
*/

#ifndef MY_MB_WC
#error MY_MB_WC must be defined for DEFINE_STRNXFRM_UNICODE
#endif

#ifndef OPTIMIZE_ASCII
#error OPTIMIZE_ASCII must be defined for DEFINE_STRNXFRM_UNICODE
#endif

#ifndef UNICASE_MAXCHAR
#error UNICASE_MAXCHAR must be defined for DEFINE_STRNXFRM_UNICODE
#endif

#ifndef UNICASE_PAGE0
#error UNICASE_PAGE0 must be defined for DEFINE_STRNXFRM_UNICODE
#endif

#ifndef UNICASE_PAGES
#error UNICASE_PAGES must be defined for DEFINE_STRNXFRM_UNICODE
#endif


static size_t
MY_FUNCTION_NAME(strnxfrm_internal)(CHARSET_INFO *cs __attribute__((unused)),
                                    uchar *dst, uchar *de,
                                    uint *nweights,
                                    const uchar *src, const uchar *se)
{
  my_wc_t UNINIT_VAR(wc);
  uchar *dst0= dst;

  DBUG_ASSERT(src || !se);
  DBUG_ASSERT((cs->state & MY_CS_LOWER_SORT) == 0);
  DBUG_ASSERT(0x7F <= UNICASE_MAXCHAR);

  for (; dst < de && *nweights; (*nweights)--)
  {
    int res;
#if OPTIMIZE_ASCII
    if (src >= se)
      break;
    if (src[0] <= 0x7F)
    {
      wc= UNICASE_PAGE0[*src++].sort;
      PUT_WC_BE2_HAVE_1BYTE(dst, de, wc);
      continue;
    }
#endif
    if ((res= MY_MB_WC(cs, &wc, src, se)) <= 0)
      break;
    src+= res;
    if (wc <= UNICASE_MAXCHAR)
    {
      MY_UNICASE_CHARACTER *page;
      if ((page= UNICASE_PAGES[wc >> 8]))
        wc= page[wc & 0xFF].sort;
    }
    else
      wc= MY_CS_REPLACEMENT_CHARACTER;
    PUT_WC_BE2_HAVE_1BYTE(dst, de, wc);
  }
  return dst - dst0;
}


static size_t
MY_FUNCTION_NAME(strnxfrm)(CHARSET_INFO *cs,
                           uchar *dst, size_t dstlen, uint nweights,
                           const uchar *src, size_t srclen, uint flags)
{
  uchar *dst0= dst;
  uchar *de= dst + dstlen;
  dst+= MY_FUNCTION_NAME(strnxfrm_internal)(cs, dst, de, &nweights,
                                            src, src + srclen);
  DBUG_ASSERT(dst <= de); /* Safety */

  if (dst < de && nweights && (flags & MY_STRXFRM_PAD_WITH_SPACE))
    dst+= my_strxfrm_pad_nweights_unicode(dst, de, nweights);

  my_strxfrm_desc_and_reverse(dst0, dst, flags, 0);

  if ((flags & MY_STRXFRM_PAD_TO_MAXLEN) && dst < de)
    dst+= my_strxfrm_pad_unicode(dst, de);
  return dst - dst0;
}


#ifdef DEFINE_STRNXFRM_UNICODE_NOPAD
static size_t
MY_FUNCTION_NAME(strnxfrm_nopad)(CHARSET_INFO *cs,
                                 uchar *dst, size_t dstlen,
                                 uint nweights,
                                 const uchar *src, size_t srclen, uint flags)
{
  uchar *dst0= dst;
  uchar *de= dst + dstlen;
  dst+= MY_FUNCTION_NAME(strnxfrm_internal)(cs, dst, de, &nweights,
                                            src, src + srclen);
  DBUG_ASSERT(dst <= de); /* Safety */

  if (dst < de && nweights && (flags & MY_STRXFRM_PAD_WITH_SPACE))
  {
    size_t len= de - dst;
    set_if_smaller(len, nweights * 2);
    memset(dst, 0x00, len);
    dst+= len;
  }

  my_strxfrm_desc_and_reverse(dst0, dst, flags, 0);

  if ((flags & MY_STRXFRM_PAD_TO_MAXLEN) && dst < de)
  {
    memset(dst, 0x00, de - dst);
    dst= de;
  }
  return dst - dst0;
}
#endif

#endif /* DEFINE_STRNXFRM_UNICODE || DEFINE_STRNXFRM_UNICODE_NOPAD */



#ifdef DEFINE_STRNXFRM_UNICODE_BIN2

/*
  Store sorting weights using 2 bytes per character.

  These functions are shared between
  - utf8mb3_general_ci, utf8mb3_bin, ucs2_general_ci, ucs2_bin
    which support BMP only (U+0000..U+FFFF).
  - utf8mb4_general_ci, utf16_general_ci, utf32_general_ci,
    which map all supplementary characters to weight 0xFFFD.
*/

#ifndef MY_MB_WC
#error MY_MB_WC must be defined for DEFINE_STRNXFRM_UNICODE_BIN2
#endif

#ifndef OPTIMIZE_ASCII
#error OPTIMIZE_ASCII must be defined for DEFINE_STRNXFRM_UNICODE_BIN2
#endif


static size_t
MY_FUNCTION_NAME(strnxfrm_internal)(CHARSET_INFO *cs __attribute__((unused)),
                                    uchar *dst, uchar *de,
                                    uint *nweights,
                                    const uchar *src,
                                    const uchar *se)
{
  my_wc_t UNINIT_VAR(wc);
  uchar *dst0= dst;

  DBUG_ASSERT(src || !se);

  for (; dst < de && *nweights; (*nweights)--)
  {
    int res;
#if OPTIMIZE_ASCII
    if (src >= se)
      break;
    if (src[0] <= 0x7F)
    {
      wc= *src++;
      PUT_WC_BE2_HAVE_1BYTE(dst, de, wc);
      continue;
    }
#endif
    if ((res= MY_MB_WC(cs, &wc, src, se)) <= 0)
      break;
    src+= res;
    if (wc > 0xFFFF)
      wc= MY_CS_REPLACEMENT_CHARACTER;
    PUT_WC_BE2_HAVE_1BYTE(dst, de, wc);
  }
  return dst - dst0;
}


static size_t
MY_FUNCTION_NAME(strnxfrm)(CHARSET_INFO *cs,
                           uchar *dst, size_t dstlen, uint nweights,
                           const uchar *src, size_t srclen, uint flags)
{
  uchar *dst0= dst;
  uchar *de= dst + dstlen;
  dst+= MY_FUNCTION_NAME(strnxfrm_internal)(cs, dst, de, &nweights,
                                            src, src + srclen);
  DBUG_ASSERT(dst <= de); /* Safety */

  if (dst < de && nweights && (flags & MY_STRXFRM_PAD_WITH_SPACE))
    dst+= my_strxfrm_pad_nweights_unicode(dst, de, nweights);

  my_strxfrm_desc_and_reverse(dst0, dst, flags, 0);

  if ((flags & MY_STRXFRM_PAD_TO_MAXLEN) && dst < de)
    dst+= my_strxfrm_pad_unicode(dst, de);
  return dst - dst0;
}


static size_t
MY_FUNCTION_NAME(strnxfrm_nopad)(CHARSET_INFO *cs,
                                 uchar *dst, size_t dstlen, uint nweights,
                                 const uchar *src, size_t srclen, uint flags)
{
  uchar *dst0= dst;
  uchar *de= dst + dstlen;
  dst+= MY_FUNCTION_NAME(strnxfrm_internal)(cs, dst, de, &nweights,
                                            src, src + srclen);
  DBUG_ASSERT(dst <= de); /* Safety */

  if (dst < de && nweights && (flags & MY_STRXFRM_PAD_WITH_SPACE))
  {
    size_t len= de - dst;
    set_if_smaller(len, nweights * 2);
    memset(dst, 0x00, len);
    dst+= len;
  }

  my_strxfrm_desc_and_reverse(dst0, dst, flags, 0);

  if ((flags & MY_STRXFRM_PAD_TO_MAXLEN) && dst < de)
  {
    memset(dst, 0x00, de - dst);
    dst= de;
  }
  return dst - dst0;
}

#endif /* DEFINE_STRNXFRM_UNICODE_BIN2 */


/*
  We usually include this file at least two times from the same source file,
  for the _ci and the _bin collations. Prepare for the second inclusion.
*/
#undef MY_FUNCTION_NAME
#undef MY_MB_WC
#undef OPTIMIZE_ASCII
#undef UNICASE_MAXCHAR
#undef UNICASE_PAGE0
#undef UNICASE_PAGES
#undef WEIGHT_ILSEQ
#undef WEIGHT_MB1
#undef WEIGHT_MB2
#undef WEIGHT_MB3
#undef WEIGHT_MB4
#undef WEIGHT_PAD_SPACE
#undef WEIGHT_MB2_FRM
#undef DEFINE_STRNXFRM
#undef DEFINE_STRNXFRM_UNICODE
#undef DEFINE_STRNXFRM_UNICODE_NOPAD
#undef DEFINE_STRNXFRM_UNICODE_BIN2
#undef DEFINE_STRNNCOLL
#undef DEFINE_STRNNCOLLSP_NOPAD

#undef STRCOLL_MB7_TOUPPER
#undef STRCOLL_MB7_BIN
#undef MY_STRCOLL_MB7_4BYTES
#undef MY_STRCOLL_MB7_8BYTES
