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

#if defined(IS_MB3_CHAR) && !defined(IS_MB2_CHAR)
#error IS_MB3_CHAR is defined, while IS_MB2_CHAR is not!
#endif

#if defined(IS_MB4_CHAR) && !defined(IS_MB3_CHAR)
#error IS_MB4_CHAR is defined, while IS_MB3_CHAR is not!
#endif


#ifdef DEFINE_ASIAN_ROUTINES
#define DEFINE_WELL_FORMED_CHAR_LENGTH
#define DEFINE_CHARLEN
#define DEFINE_NATIVE_TO_MB_VARLEN
#endif


#ifdef DEFINE_CHARLEN
/**
  Returns length of the left-most character of a string.
  @param cs - charset with mbminlen==1 and mbmaxlen<=4
  @param b  - the beginning of the string
  @param e  - the end of the string

  @return   MY_CS_ILSEQ         if a bad byte sequence was found
  @return   MY_CS_TOOSMALL(N)   if the string ended unexpectedly
  @return   >0                  if a valid character was found
*/
static int
MY_FUNCTION_NAME(charlen)(CHARSET_INFO *cs __attribute__((unused)),
                          const uchar *b, const uchar *e)
{
  DBUG_ASSERT(cs->mbminlen == 1);
  DBUG_ASSERT(cs->mbmaxlen <= 4);

  if (b >= e)
    return MY_CS_TOOSMALL;
  if ((uchar) b[0] < 128)
    return 1; /* Single byte ASCII character */

#ifdef IS_8BIT_CHAR
  if (IS_8BIT_CHAR(b[0]))
  {      
    /* Single byte non-ASCII character, e.g. half width kana in sjis */
    return 1;
  }
#endif

  if (b + 2 > e)
    return MY_CS_TOOSMALLN(2);
  if (IS_MB2_CHAR(b[0], b[1]))
    return 2; /* Double byte character */

#ifdef IS_MB3_CHAR
  if (b + 3 > e)
  {
#ifdef IS_MB_PREFIX2
    if (!IS_MB_PREFIX2(b[0], b[1]))
      return MY_CS_ILSEQ;
#endif
    return MY_CS_TOOSMALLN(3);
  }
  if (IS_MB3_CHAR(b[0], b[1], b[2]))
    return 3; /* Three-byte character */
#endif

#ifdef IS_MB4_CHAR
  if (b + 4 > e)
    return MY_CS_TOOSMALLN(4);
  if (IS_MB4_CHAR(b[0], b[1], b[2], b[3]))
    return 4; /* Four-byte character */
#endif

  /* Wrong byte sequence */
  return MY_CS_ILSEQ;
}
#endif /* DEFINE_CHARLEN */


#ifdef DEFINE_WELL_FORMED_CHAR_LENGTH
/**
  Returns well formed length of a string 
  measured in characters (rather than in bytes).
  Version for character sets that define IS_MB?_CHAR(), e.g. big5.
*/
static size_t
MY_FUNCTION_NAME(well_formed_char_length)(CHARSET_INFO *cs __attribute__((unused)),
                                          const char *b, const char *e,
                                          size_t nchars,
                                          MY_STRCOPY_STATUS *status)
{
  size_t nchars0= nchars;
  for ( ; b < e && nchars ; nchars--)
  {
    if ((uchar) b[0] < 128)
    {
      b++; /* Single byte ASCII character */
      continue;
    }

    if (b + 2 <= e && IS_MB2_CHAR(b[0], b[1]))
    {
      b+= 2; /* Double byte character */
      continue;
    }

#ifdef IS_MB3_CHAR
    if (b + 3 <= e && IS_MB3_CHAR(b[0], b[1], b[2]))
    {
      b+= 3; /* Three-byte character */
      continue;
    }
#endif

#ifdef IS_MB4_CHAR
    if (b + 4 <= e && IS_MB4_CHAR(b[0], b[1], b[2], b[3]))
    {
      b+= 4; /* Four-byte character */
      continue;
    }
#endif

#ifdef IS_8BIT_CHAR
    if (IS_8BIT_CHAR(b[0]))
    {      
      b++; /* Single byte non-ASCII character, e.g. half width kana in sjis */
      continue;
    }
#endif

    /* Wrong byte sequence */
    status->m_source_end_pos= status->m_well_formed_error_pos= b;
    return nchars0 - nchars;
  }
  status->m_source_end_pos= b;
  status->m_well_formed_error_pos= NULL;
  return nchars0 - nchars;
}
#endif /* DEFINE_WELL_FORMED_CHAR_LENGTH */


#ifdef DEFINE_WELL_FORMED_CHAR_LENGTH_USING_CHARLEN
#ifndef CHARLEN
#error CHARLEN is not defined
#endif
/**
  Returns well formed length of a string 
  measured in characters (rather than in bytes).
  Version for character sets that define CHARLEN(), e.g. utf8mb3.
  CHARLEN(cs,b,e) must use the same return code convension that mb_wc() does:
  - a positive number in the range [1-mbmaxlen] if a valid
    single-byte or multi-byte character was found
  - MY_CS_ILSEQ (0) on a bad byte sequence
  - MY_CS_TOOSMALLxx if the incoming sequence is incomplete
*/
static size_t
MY_FUNCTION_NAME(well_formed_char_length)(CHARSET_INFO *cs __attribute__((unused)),
                                          const char *b, const char *e,
                                          size_t nchars,
                                          MY_STRCOPY_STATUS *status)
{
  size_t nchars0= nchars;
  int chlen;
  for ( ; nchars ; nchars--, b+= chlen)
  {
    if ((chlen= CHARLEN(cs, (uchar*) b, (uchar*) e)) <= 0)
    {
      status->m_well_formed_error_pos= b < e ? b : NULL;
      status->m_source_end_pos= b;
      return nchars0 - nchars;
    }
  }
  status->m_well_formed_error_pos= NULL;
  status->m_source_end_pos= b;
  return nchars0 - nchars;
}
#endif /* DEFINE_WELL_FORMED_CHAR_LENGTH_USING_CHARLEN */


#ifdef DEFINE_NATIVE_TO_MB_VARLEN
/*
  Write a native 2-byte character.
  If the full character does not fit, only the first byte is written.
*/
static inline int
my_native_to_mb_fixed2(my_wc_t wc, uchar *s, uchar *e)
{
  /* The caller must insure there is a space for at least one byte */
  DBUG_ASSERT(s < e);
  s[0]= (uchar) (wc >> 8);
  if (s + 2 > e)
    return MY_CS_TOOSMALL2;
  s[1]= wc & 0xFF;
  return 2;
}


/*
  Write a native 3-byte character.
  If the full character does not fit, only the leading bytes are written.
*/
static inline int
my_native_to_mb_fixed3(my_wc_t wc, uchar *s, uchar *e)
{
  /* The caller must insure there is a space for at least one byte */
  DBUG_ASSERT(s < e);
  s[0]= (uchar) (wc >> 16);
  if (s + 2 > e)
    return MY_CS_TOOSMALL2;
  s[1]= (wc >> 8) & 0xFF;
  if (s + 3 > e)
    return MY_CS_TOOSMALL3;
  s[2]= wc & 0xFF;
  return 3;
}


/*
  Write a native 1-byte or 2-byte or 3-byte character.
*/

static int
MY_FUNCTION_NAME(native_to_mb)(CHARSET_INFO *cs __attribute__((unused)),
                               my_wc_t wc, uchar *s, uchar *e)
{
  if (s >= e)
    return MY_CS_TOOSMALL;
  if ((int) wc <= 0xFF)
  {
    s[0]= (uchar) wc;
    return 1;
  }
#ifdef IS_MB3_HEAD
  if (wc > 0xFFFF)
    return my_native_to_mb_fixed3(wc, s, e);
#endif
  return my_native_to_mb_fixed2(wc, s, e);
}
#endif /* DEFINE_NATIVE_TO_MB_VARLEN */


#undef MY_FUNCTION_NAME
