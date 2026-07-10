#ifndef CTYPE_UCA_0520_H
#define CTYPE_UCA_0520_H
/* Copyright (c) 2025, MariaDB Corporation

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Library General Public
   License as published by the Free Software Foundation; version 2
   of the License.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Library General Public License for more details.

   You should have received a copy of the GNU Library General Public
   License along with this library; if not, write to the Free
   Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
   MA 02110-1335  USA */


extern struct charset_info_st my_charset_utf8mb3_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_utf8mb3_unicode_520_ci;
extern struct charset_info_st my_charset_utf8mb4_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_utf8mb4_unicode_520_ci;
extern struct charset_info_st my_charset_ucs2_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_ucs2_unicode_520_ci;
extern struct charset_info_st my_charset_utf16_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_utf16_unicode_520_ci;
extern struct charset_info_st my_charset_utf32_unicode_520_nopad_ci;
extern struct charset_info_st my_charset_utf32_unicode_520_ci;
extern struct charset_info_st my_charset_utf8mb4_turkish_uca_ci;


/*
  Get a UCA-5.2.0 CHARSET_INFO using its character set ID and PAD flags.
  Used to initialize UCA-14.0.0 collations.
*/
static inline
CHARSET_INFO *my_uca0520_builtin_collation_by_id(my_cs_encoding_t cs_id,
                                                 uint nopad_flags)
{
  switch (cs_id) {
  case MY_CS_ENCODING_UTF8MB3:
    return nopad_flags ? &my_charset_utf8mb3_unicode_520_nopad_ci :
                         &my_charset_utf8mb3_unicode_520_ci;
  case MY_CS_ENCODING_UTF8MB4:
    return nopad_flags ? &my_charset_utf8mb4_unicode_520_nopad_ci :
                         &my_charset_utf8mb4_unicode_520_ci;
#ifdef HAVE_CHARSET_ucs2
  case MY_CS_ENCODING_UCS2:
    return nopad_flags ? &my_charset_ucs2_unicode_520_nopad_ci :
                         &my_charset_ucs2_unicode_520_ci;
#endif
#ifdef HAVE_CHARSET_utf16
  case MY_CS_ENCODING_UTF16:
    return nopad_flags ? &my_charset_utf16_unicode_520_nopad_ci :
                         &my_charset_utf16_unicode_520_ci;
#endif
#ifdef HAVE_CHARSET_utf32
  case MY_CS_ENCODING_UTF32:
    return nopad_flags ? &my_charset_utf32_unicode_520_nopad_ci :
                         &my_charset_utf32_unicode_520_ci;
#endif
  }
  return NULL;
}


#endif /* CTYPE_UCA_0520_H */
