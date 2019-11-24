/*
   Copyright (c) 2015, MariaDB Foundation

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


#ifndef MY_FUNCTION_NAME
#error MY_FUNCTION_NAME is not defined
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
    E.g. [0x80..0xC1] in utf8. This allows using simplified conditions
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

  Note, cs->coll->strnncoll() is usually used to compare identifiers.
  Perhaps we should eventually (in 10.2?) create a new collation 
  my_charset_utf8_general_ci_no_pad and have only one comparison function
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
    uint b_wlen= MY_FUNCTION_NAME(scan_weight)(&b_weight, b, b_end);
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
    uint b_wlen= MY_FUNCTION_NAME(scan_weight)(&b_weight, b, b_end);
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
#endif


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
    if (my_charlen(cs, (const char *) src, (const char *) se) > 1)
    {
      /*
        Note, it is safe not to check (src < se)
        in the code below, because my_charlen() would
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


/*
  We usually include this file at least two times from the same source file,
  for the _ci and the _bin collations. Prepare for the second inclusion.
*/
#undef MY_FUNCTION_NAME
#undef WEIGHT_ILSEQ
#undef WEIGHT_MB1
#undef WEIGHT_MB2
#undef WEIGHT_MB3
#undef WEIGHT_MB4
#undef WEIGHT_PAD_SPACE
#undef WEIGHT_MB2_FRM
#undef DEFINE_STRNXFRM
#undef DEFINE_STRNNCOLLSP_NOPAD
