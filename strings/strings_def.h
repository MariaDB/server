#ifndef STRINGS_DEF_INCLUDED
#define STRINGS_DEF_INCLUDED
/* Copyright (C) 2011 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

/* This file is to be include first in all files in the string directory */

#undef DBUG_ASSERT_AS_PRINTF
#include <my_global.h>		/* Define standard vars */
#include "m_string.h"		/* Exernal definitions of string functions */
#include "m_ctype.h"

/*
  We can't use the original DBUG_ASSERT() (which includes _db_flush())
  in the strings library as libdbug is compiled after the the strings
  library and we don't want to have strings depending on libdbug which
  depends on mysys and strings.
*/

#if !defined(DBUG_OFF)
#undef DBUG_ASSERT
#define DBUG_ASSERT(A) assert(A)
#endif

#define MY_NOPAD_ID(x)  ((x)+0x400)

/* SPACE_INT is a word that contains only spaces */
#if SIZEOF_INT == 4
#define SPACE_INT 0x20202020
#elif SIZEOF_INT == 8
#define SPACE_INT 0x2020202020202020
#else
#error define the appropriate constant for a word full of spaces
#endif

/**
  Skip trailing space.

  On most systems reading memory in larger chunks (ideally equal to the size of
  the chinks that the machine physically reads from memory) causes fewer memory
  access loops and hence increased performance.
  This is why the 'int' type is used : it's closest to that (according to how
  it's defined in C).
  So when we determine the amount of whitespace at the end of a string we do
  the following :
    1. We divide the string into 3 zones :
      a) from the start of the string (__start) to the first multiple
        of sizeof(int)  (__start_words)
      b) from the end of the string (__end) to the last multiple of sizeof(int)
        (__end_words)
      c) a zone that is aligned to sizeof(int) and can be safely accessed
        through an int *
    2. We start comparing backwards from (c) char-by-char. If all we find is
       space then we continue
    3. If there are elements in zone (b) we compare them as unsigned ints to a
       int mask (SPACE_INT) consisting of all spaces
    4. Finally we compare the remaining part (a) of the string char by char.
       This covers for the last non-space unsigned int from 3. (if any)

   This algorithm works well for relatively larger strings, but it will slow
   the things down for smaller strings (because of the additional calculations
   and checks compared to the naive method). Thus the barrier of length 20
   is added.

   @param     ptr   pointer to the input string
   @param     len   the length of the string
   @return          the last non-space character
*/

static inline const uchar *skip_trailing_space(const uchar *ptr,size_t len)
{
  const uchar *end= ptr + len;
  DBUG_ASSERT(ptr); /* Avoid UBSAN nullptr-with-offset */
  if (len > 20)
  {
    const uchar *end_words= (const uchar *)(intptr)
      (((ulonglong)(intptr)end) / SIZEOF_INT * SIZEOF_INT);
    const uchar *start_words= (const uchar *)(intptr)
       ((((ulonglong)(intptr)ptr) + SIZEOF_INT - 1) / SIZEOF_INT * SIZEOF_INT);

    DBUG_ASSERT(((ulonglong)(intptr)ptr) >= SIZEOF_INT);
    if (end_words > ptr)
    {
      while (end > end_words && end[-1] == 0x20)
        end--;
      if (end[-1] == 0x20 && start_words < end_words)
        while (end > start_words && ((unsigned *)end)[-1] == SPACE_INT)
          end -= SIZEOF_INT;
    }
  }
  while (end > ptr && end[-1] == 0x20)
    end--;
  return (end);
}


static inline my_strnxfrm_ret_t
my_strnxfrm_ret_construct(size_t output_length,
                          size_t source_length_used,
                          uint warnings)
{
  my_strnxfrm_ret_t rc= {output_length,
                         source_length_used,
                         warnings};
  return rc;
}


static inline my_strnxfrm_pad_ret_t
my_strnxfrm_pad_ret_construct(size_t output_length,
                              uint warnings)
{
  my_strnxfrm_pad_ret_t rc= {output_length,
                             warnings};
  return rc;
}


static inline void
my_strnxfrm_ret_join_pad(my_strnxfrm_ret_t *rc,
                         const my_strnxfrm_pad_ret_t *rcpad)
{
  rc->m_result_length+= rcpad->m_result_length;
  rc->m_warnings|= rcpad->m_warnings;
}


int my_strnncollsp_nchars_generic(CHARSET_INFO *cs,
                                  const uchar *str1, size_t len1,
                                  const uchar *str2, size_t len2,
                                  size_t nchars,
                                  uint flags);

int my_strnncollsp_nchars_generic_8bit(CHARSET_INFO *cs,
                                       const uchar *str1, size_t len1,
                                       const uchar *str2, size_t len2,
                                       size_t nchars,
                                       uint flags);


size_t  my_strnxfrmlen_simple(CHARSET_INFO *, size_t);

my_strnxfrm_ret_t
my_strnxfrm_simple(CHARSET_INFO *,
                   uchar *dst, size_t dstlen, uint nweights,
                   const uchar *src, size_t srclen, uint flags);


my_strnxfrm_ret_t
my_strnxfrm_mb(CHARSET_INFO *,
               uchar *dst, size_t dstlen, uint nweights,
               const uchar *src, size_t srclen, uint flags);

my_strnxfrm_ret_t
my_strnxfrm_mb_nopad(CHARSET_INFO *,
                    uchar *dst, size_t dstlen, uint nweights,
                    const uchar *src, size_t srclen, uint flags);

size_t  my_strnxfrmlen_unicode(CHARSET_INFO *, size_t);

size_t  my_strnxfrmlen_unicode_full_bin(CHARSET_INFO *, size_t);

my_strnxfrm_ret_t
my_strnxfrm_unicode_full_bin(CHARSET_INFO *,
                             uchar *dst, size_t dstlen, uint nweights,
                             const uchar *src, size_t srclen, uint flags);

my_strnxfrm_ret_t
my_strnxfrm_unicode_full_nopad_bin(CHARSET_INFO *,
                                   uchar *dst, size_t dstlen, uint nweights,
                                   const uchar *src, size_t srclen,
                                   uint flags);

my_strnxfrm_ret_t
my_strxfrm_pad_desc_and_reverse(CHARSET_INFO *cs,
                                uchar *str, uchar *frmend, uchar *strend,
                                uint nweights, uint flags, uint level);

my_strnxfrm_ret_t
my_strxfrm_pad_desc_and_reverse_nopad(CHARSET_INFO *cs,
                                      uchar *str, uchar *frmend,
                                      uchar *strend, uint nweights,
                                      uint flags, uint level);

uint my_8bit_charset_flags_from_data(CHARSET_INFO *cs);
uint my_8bit_collation_flags_from_data(CHARSET_INFO *cs);


/* Macros for hashing characters */

static inline void MY_HASH_ADD(my_hasher_st *hasher, uchar value)
{
  DBUG_ASSERT((hasher->m_hash_byte == NULL) == (hasher->m_hash_str == NULL));
  if (hasher->m_hash_byte)
    hasher->m_hash_byte(hasher, value);
  else
    MY_HASH_ADD_MARIADB(hasher->m_nr1, hasher->m_nr2, value);
}

static inline void MY_HASH_ADD_STR(my_hasher_st *hasher, const uchar* key,
                                   size_t len)
{
  const uchar *end= key + len;
  DBUG_ASSERT((hasher->m_hash_byte == NULL) == (hasher->m_hash_str == NULL));
  if (hasher->m_hash_str)
    hasher->m_hash_str(hasher, key, len);
  else
  {
    for (; key < end; key++)
      MY_HASH_ADD_MARIADB(hasher->m_nr1, hasher->m_nr2, (uint) *key);
  }
}

#define MY_HASH_ADD_16(A, value)                                        \
  do { MY_HASH_ADD(A, ((uchar)(value & 0xFF))) ; MY_HASH_ADD(A, ((uchar)(value >>8))); } while(0)

#define my_wc_t ulong

int my_wc_to_printable_ex(CHARSET_INFO *cs, my_wc_t wc,
                          uchar *s, uchar *e,
                          uint bs, uint bslen, uint diglen);

int my_wc_to_printable_generic(CHARSET_INFO *cs, my_wc_t wc,
                               uchar *s, uchar *e);

int my_wc_to_printable_8bit(CHARSET_INFO *cs, my_wc_t wc,
                            uchar *s, uchar *e);

void my_ci_set_strength(struct charset_info_st *cs, uint strength);
void my_ci_set_level_flags(struct charset_info_st *cs, uint flags);

uint my_casefold_multiply_1(CHARSET_INFO *cs);
uint my_casefold_multiply_2(CHARSET_INFO *cs);

my_bool my_ci_eq_collation_generic(CHARSET_INFO *self, CHARSET_INFO *other);

struct charset_info_st *my_ci_alloc(MY_CHARSET_LOADER *loader,
                                    const LEX_CSTRING name,
                                    LEX_CSTRING *out_name,
                                    const LEX_CSTRING comment,
                                    LEX_CSTRING *out_comment);

/* Some common character set names */
extern const char charset_name_latin2[];
#define charset_name_latin2_length 6
extern const char charset_name_utf8mb3[];
#define charset_name_utf8mb3_length 7
extern const char charset_name_utf16[];
#define charset_name_utf16_length 5
extern const char charset_name_utf32[];
#define charset_name_utf32_length 5
extern const char charset_name_ucs2[];
#define charset_name_ucs2_length 4
extern const char charset_name_utf8mb4[];
#define charset_name_utf8mb4_length 7

#endif /*STRINGS_DEF_INCLUDED */
