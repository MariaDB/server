/* Copyright (c) 2000, 2012, Oracle and/or its affiliates. All rights reserved.

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mysys_priv.h"

/*
  Include all compiled character sets into the client
  If a client don't want to use all of them, he can define his own
  init_compiled_charsets() that only adds those that he wants
*/

#ifdef HAVE_UCA_COLLATIONS

#ifdef HAVE_CHARSET_ucs2
extern struct charset_info_st my_charset_ucs2_german2_uca_ci;
extern struct charset_info_st my_charset_ucs2_icelandic_uca_ci;
extern struct charset_info_st my_charset_ucs2_latvian_uca_ci;
extern struct charset_info_st my_charset_ucs2_romanian_uca_ci;
extern struct charset_info_st my_charset_ucs2_slovenian_uca_ci;
extern struct charset_info_st my_charset_ucs2_polish_uca_ci;
extern struct charset_info_st my_charset_ucs2_estonian_uca_ci;
extern struct charset_info_st my_charset_ucs2_spanish_uca_ci;
extern struct charset_info_st my_charset_ucs2_swedish_uca_ci;
extern struct charset_info_st my_charset_ucs2_turkish_uca_ci;
extern struct charset_info_st my_charset_ucs2_czech_uca_ci;
extern struct charset_info_st my_charset_ucs2_danish_uca_ci;
extern struct charset_info_st my_charset_ucs2_lithuanian_uca_ci;
extern struct charset_info_st my_charset_ucs2_slovak_uca_ci;
extern struct charset_info_st my_charset_ucs2_spanish2_uca_ci;
extern struct charset_info_st my_charset_ucs2_roman_uca_ci;
extern struct charset_info_st my_charset_ucs2_persian_uca_ci;
extern struct charset_info_st my_charset_ucs2_esperanto_uca_ci;
extern struct charset_info_st my_charset_ucs2_hungarian_uca_ci;
extern struct charset_info_st my_charset_ucs2_croatian_mysql561_uca_ci;
extern struct charset_info_st my_charset_ucs2_sinhala_uca_ci;
extern struct charset_info_st my_charset_ucs2_unicode_520_ci;
extern struct charset_info_st my_charset_ucs2_vietnamese_ci;
extern struct charset_info_st my_charset_ucs2_croatian_uca_ci;
extern struct charset_info_st my_charset_ucs2_myanmar_uca_ci;
extern struct charset_info_st my_charset_ucs2_thai_520_w2;
extern struct charset_info_st my_charset_ucs2_unicode_520_nopad_ci;
#endif


#ifdef HAVE_CHARSET_utf32
extern struct charset_info_st my_charset_utf32_german2_uca_ci;
extern struct charset_info_st my_charset_utf32_icelandic_uca_ci;
extern struct charset_info_st my_charset_utf32_latvian_uca_ci;
extern struct charset_info_st my_charset_utf32_romanian_uca_ci;
extern struct charset_info_st my_charset_utf32_slovenian_uca_ci;
extern struct charset_info_st my_charset_utf32_polish_uca_ci;
extern struct charset_info_st my_charset_utf32_estonian_uca_ci;
extern struct charset_info_st my_charset_utf32_spanish_uca_ci;
extern struct charset_info_st my_charset_utf32_swedish_uca_ci;
extern struct charset_info_st my_charset_utf32_turkish_uca_ci;
extern struct charset_info_st my_charset_utf32_czech_uca_ci;
extern struct charset_info_st my_charset_utf32_danish_uca_ci;
extern struct charset_info_st my_charset_utf32_lithuanian_uca_ci;
extern struct charset_info_st my_charset_utf32_slovak_uca_ci;
extern struct charset_info_st my_charset_utf32_spanish2_uca_ci;
extern struct charset_info_st my_charset_utf32_roman_uca_ci;
extern struct charset_info_st my_charset_utf32_persian_uca_ci;
extern struct charset_info_st my_charset_utf32_esperanto_uca_ci;
extern struct charset_info_st my_charset_utf32_hungarian_uca_ci;
extern struct charset_info_st my_charset_utf32_croatian_mysql561_uca_ci;
extern struct charset_info_st my_charset_utf32_sinhala_uca_ci;
extern struct charset_info_st my_charset_utf32_unicode_520_ci;
extern struct charset_info_st my_charset_utf32_vietnamese_ci;
extern struct charset_info_st my_charset_utf32_croatian_uca_ci;
extern struct charset_info_st my_charset_utf32_myanmar_uca_ci;
extern struct charset_info_st my_charset_utf32_thai_520_w2;
extern struct charset_info_st my_charset_utf32_unicode_520_nopad_ci;
#endif /* HAVE_CHARSET_utf32 */


#ifdef HAVE_CHARSET_utf16
extern struct charset_info_st my_charset_utf16_german2_uca_ci;
extern struct charset_info_st my_charset_utf16_icelandic_uca_ci;
extern struct charset_info_st my_charset_utf16_latvian_uca_ci;
extern struct charset_info_st my_charset_utf16_romanian_uca_ci;
extern struct charset_info_st my_charset_utf16_slovenian_uca_ci;
extern struct charset_info_st my_charset_utf16_polish_uca_ci;
extern struct charset_info_st my_charset_utf16_estonian_uca_ci;
extern struct charset_info_st my_charset_utf16_spanish_uca_ci;
extern struct charset_info_st my_charset_utf16_swedish_uca_ci;
extern struct charset_info_st my_charset_utf16_turkish_uca_ci;
extern struct charset_info_st my_charset_utf16_czech_uca_ci;
extern struct charset_info_st my_charset_utf16_danish_uca_ci;
extern struct charset_info_st my_charset_utf16_lithuanian_uca_ci;
extern struct charset_info_st my_charset_utf16_slovak_uca_ci;
extern struct charset_info_st my_charset_utf16_spanish2_uca_ci;
extern struct charset_info_st my_charset_utf16_roman_uca_ci;
extern struct charset_info_st my_charset_utf16_persian_uca_ci;
extern struct charset_info_st my_charset_utf16_esperanto_uca_ci;
extern struct charset_info_st my_charset_utf16_hungarian_uca_ci;
extern struct charset_info_st my_charset_utf16_croatian_mysql561_uca_ci;
extern struct charset_info_st my_charset_utf16_sinhala_uca_ci;
extern struct charset_info_st my_charset_utf16_unicode_520_ci;
extern struct charset_info_st my_charset_utf16_vietnamese_ci;
extern struct charset_info_st my_charset_utf16_croatian_uca_ci;
extern struct charset_info_st my_charset_utf16_myanmar_uca_ci;
extern struct charset_info_st my_charset_utf16_thai_520_w2;
extern struct charset_info_st my_charset_utf16_unicode_520_nopad_ci;
#endif  /* HAVE_CHARSET_utf16 */


#ifdef HAVE_CHARSET_utf8mb3
extern struct charset_info_st my_charset_utf8mb3_german2_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_icelandic_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_latvian_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_romanian_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_slovenian_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_polish_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_estonian_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_spanish_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_swedish_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_turkish_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_czech_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_danish_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_lithuanian_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_slovak_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_spanish2_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_roman_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_persian_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_esperanto_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_hungarian_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_croatian_mysql561_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_sinhala_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_unicode_520_ci;
extern struct charset_info_st my_charset_utf8mb3_vietnamese_ci;
extern struct charset_info_st my_charset_utf8mb3_croatian_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_myanmar_uca_ci;
extern struct charset_info_st my_charset_utf8mb3_thai_520_w2;
#ifdef HAVE_UTF8_GENERAL_CS
extern struct charset_info_st my_charset_utf8mb3_general_cs;
#endif
extern struct charset_info_st my_charset_utf8mb3_unicode_520_nopad_ci;
#endif

#ifdef HAVE_CHARSET_utf8mb4
extern struct charset_info_st my_charset_utf8mb4_german2_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_icelandic_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_latvian_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_romanian_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_slovenian_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_polish_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_estonian_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_spanish_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_swedish_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_turkish_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_czech_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_danish_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_lithuanian_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_slovak_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_spanish2_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_roman_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_persian_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_esperanto_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_hungarian_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_croatian_mysql561_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_sinhala_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_unicode_520_ci;
extern struct charset_info_st my_charset_utf8mb4_vietnamese_ci;
extern struct charset_info_st my_charset_utf8mb4_croatian_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_myanmar_uca_ci;
extern struct charset_info_st my_charset_utf8mb4_thai_520_w2;
extern struct charset_info_st my_charset_utf8mb4_unicode_520_nopad_ci;
#endif /* HAVE_CHARSET_utf8mb4 */

#endif /* HAVE_UCA_COLLATIONS */

my_bool init_compiled_charsets(myf flags __attribute__((unused)))
{
  CHARSET_INFO *cs;

  add_compiled_collation(&my_charset_bin);
  add_compiled_collation(&my_charset_filename);
  
  add_compiled_collation(&my_charset_latin1);
  add_compiled_collation(&my_charset_latin1_nopad);
  add_compiled_collation(&my_charset_latin1_bin);
  add_compiled_collation(&my_charset_latin1_nopad_bin);
  add_compiled_collation(&my_charset_latin1_german2_ci);

#ifdef HAVE_CHARSET_big5
  add_compiled_collation(&my_charset_big5_chinese_ci);
  add_compiled_collation(&my_charset_big5_bin);
  add_compiled_collation(&my_charset_big5_chinese_nopad_ci);
  add_compiled_collation(&my_charset_big5_nopad_bin);
#endif

#ifdef HAVE_CHARSET_cp1250
  add_compiled_collation(&my_charset_cp1250_czech_ci);
#endif

#ifdef HAVE_CHARSET_cp932
  add_compiled_collation(&my_charset_cp932_japanese_ci);
  add_compiled_collation(&my_charset_cp932_bin);
  add_compiled_collation(&my_charset_cp932_japanese_nopad_ci);
  add_compiled_collation(&my_charset_cp932_nopad_bin);
#endif

#ifdef HAVE_CHARSET_latin2
  add_compiled_collation(&my_charset_latin2_czech_ci);
#endif

#ifdef HAVE_CHARSET_eucjpms
  add_compiled_collation(&my_charset_eucjpms_japanese_ci);
  add_compiled_collation(&my_charset_eucjpms_bin);
  add_compiled_collation(&my_charset_eucjpms_japanese_nopad_ci);
  add_compiled_collation(&my_charset_eucjpms_nopad_bin);
#endif

#ifdef HAVE_CHARSET_euckr
  add_compiled_collation(&my_charset_euckr_korean_ci);
  add_compiled_collation(&my_charset_euckr_bin);
  add_compiled_collation(&my_charset_euckr_korean_nopad_ci);
  add_compiled_collation(&my_charset_euckr_nopad_bin);
#endif

#ifdef HAVE_CHARSET_gb2312
  add_compiled_collation(&my_charset_gb2312_chinese_ci);
  add_compiled_collation(&my_charset_gb2312_bin);
  add_compiled_collation(&my_charset_gb2312_chinese_nopad_ci);
  add_compiled_collation(&my_charset_gb2312_nopad_bin);
#endif

#ifdef HAVE_CHARSET_gbk
  add_compiled_collation(&my_charset_gbk_chinese_ci);
  add_compiled_collation(&my_charset_gbk_bin);
  add_compiled_collation(&my_charset_gbk_chinese_nopad_ci);
  add_compiled_collation(&my_charset_gbk_nopad_bin);
#endif

#ifdef HAVE_CHARSET_sjis
  add_compiled_collation(&my_charset_sjis_japanese_ci);
  add_compiled_collation(&my_charset_sjis_bin);
  add_compiled_collation(&my_charset_sjis_japanese_nopad_ci);
  add_compiled_collation(&my_charset_sjis_nopad_bin);
#endif

#ifdef HAVE_CHARSET_tis620
  add_compiled_collation(&my_charset_tis620_thai_ci);
  add_compiled_collation(&my_charset_tis620_bin);
  add_compiled_collation(&my_charset_tis620_thai_nopad_ci);
  add_compiled_collation(&my_charset_tis620_nopad_bin);
#endif

#ifdef HAVE_CHARSET_ucs2
  add_compiled_collation(&my_charset_ucs2_general_ci);
  add_compiled_collation(&my_charset_ucs2_bin);
  add_compiled_collation(&my_charset_ucs2_general_nopad_ci);
  add_compiled_collation(&my_charset_ucs2_nopad_bin);
  add_compiled_collation(&my_charset_ucs2_general_mysql500_ci);
#ifdef HAVE_UCA_COLLATIONS
  add_compiled_collation(&my_charset_ucs2_unicode_ci);
  add_compiled_collation(&my_charset_ucs2_german2_uca_ci);
  add_compiled_collation(&my_charset_ucs2_icelandic_uca_ci);
  add_compiled_collation(&my_charset_ucs2_latvian_uca_ci);
  add_compiled_collation(&my_charset_ucs2_romanian_uca_ci);
  add_compiled_collation(&my_charset_ucs2_slovenian_uca_ci);
  add_compiled_collation(&my_charset_ucs2_polish_uca_ci);
  add_compiled_collation(&my_charset_ucs2_estonian_uca_ci);
  add_compiled_collation(&my_charset_ucs2_spanish_uca_ci);
  add_compiled_collation(&my_charset_ucs2_swedish_uca_ci);
  add_compiled_collation(&my_charset_ucs2_turkish_uca_ci);
  add_compiled_collation(&my_charset_ucs2_czech_uca_ci);
  add_compiled_collation(&my_charset_ucs2_danish_uca_ci);
  add_compiled_collation(&my_charset_ucs2_lithuanian_uca_ci);
  add_compiled_collation(&my_charset_ucs2_slovak_uca_ci);
  add_compiled_collation(&my_charset_ucs2_spanish2_uca_ci);
  add_compiled_collation(&my_charset_ucs2_roman_uca_ci);
  add_compiled_collation(&my_charset_ucs2_persian_uca_ci);
  add_compiled_collation(&my_charset_ucs2_esperanto_uca_ci);
  add_compiled_collation(&my_charset_ucs2_hungarian_uca_ci);
  add_compiled_collation(&my_charset_ucs2_croatian_mysql561_uca_ci);
  add_compiled_collation(&my_charset_ucs2_sinhala_uca_ci);
  add_compiled_collation(&my_charset_ucs2_unicode_520_ci);
  add_compiled_collation(&my_charset_ucs2_vietnamese_ci);
  add_compiled_collation(&my_charset_ucs2_croatian_uca_ci);
  add_compiled_collation(&my_charset_ucs2_myanmar_uca_ci);
  add_compiled_collation(&my_charset_ucs2_thai_520_w2);
  add_compiled_collation(&my_charset_ucs2_unicode_nopad_ci);
  add_compiled_collation(&my_charset_ucs2_unicode_520_nopad_ci);
#endif
#endif

#ifdef HAVE_CHARSET_ujis
  add_compiled_collation(&my_charset_ujis_japanese_ci);
  add_compiled_collation(&my_charset_ujis_bin);
  add_compiled_collation(&my_charset_ujis_japanese_nopad_ci);
  add_compiled_collation(&my_charset_ujis_nopad_bin);
#endif

#ifdef HAVE_CHARSET_utf8mb3
  add_compiled_collation(&my_charset_utf8mb3_general_ci);
  add_compiled_collation(&my_charset_utf8mb3_general_nopad_ci);
  add_compiled_collation(&my_charset_utf8mb3_bin);
  add_compiled_collation(&my_charset_utf8mb3_nopad_bin);
  add_compiled_collation(&my_charset_utf8mb3_general_mysql500_ci);
#ifdef HAVE_UTF8_GENERAL_CS
  add_compiled_collation(&my_charset_utf8mb3_general_cs);
#endif
#ifdef HAVE_UCA_COLLATIONS
  add_compiled_collation(&my_charset_utf8mb3_unicode_ci);
  add_compiled_collation(&my_charset_utf8mb3_german2_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_icelandic_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_latvian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_romanian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_slovenian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_polish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_estonian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_spanish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_swedish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_turkish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_czech_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_danish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_lithuanian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_slovak_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_spanish2_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_roman_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_persian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_esperanto_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_hungarian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_croatian_mysql561_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_sinhala_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_unicode_520_ci);
  add_compiled_collation(&my_charset_utf8mb3_vietnamese_ci);
  add_compiled_collation(&my_charset_utf8mb3_croatian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_myanmar_uca_ci);
  add_compiled_collation(&my_charset_utf8mb3_thai_520_w2);
  add_compiled_collation(&my_charset_utf8mb3_unicode_nopad_ci);
  add_compiled_collation(&my_charset_utf8mb3_unicode_520_nopad_ci);
#endif
#endif /* HAVE_CHARSET_utf8mb3 */


#ifdef HAVE_CHARSET_utf8mb4
  add_compiled_collation(&my_charset_utf8mb4_general_ci);
  add_compiled_collation(&my_charset_utf8mb4_bin);
  add_compiled_collation(&my_charset_utf8mb4_general_nopad_ci);
  add_compiled_collation(&my_charset_utf8mb4_nopad_bin);
#ifdef HAVE_UCA_COLLATIONS
  add_compiled_collation(&my_charset_utf8mb4_unicode_ci);
  add_compiled_collation(&my_charset_utf8mb4_german2_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_icelandic_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_latvian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_romanian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_slovenian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_polish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_estonian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_spanish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_swedish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_turkish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_czech_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_danish_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_lithuanian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_slovak_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_spanish2_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_roman_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_persian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_esperanto_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_hungarian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_croatian_mysql561_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_sinhala_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_unicode_520_ci);
  add_compiled_collation(&my_charset_utf8mb4_vietnamese_ci);
  add_compiled_collation(&my_charset_utf8mb4_croatian_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_myanmar_uca_ci);
  add_compiled_collation(&my_charset_utf8mb4_thai_520_w2);
  add_compiled_collation(&my_charset_utf8mb4_unicode_nopad_ci);
  add_compiled_collation(&my_charset_utf8mb4_unicode_520_nopad_ci);
#endif /* HAVE_UCA_COLLATIONS  */
#endif /* HAVE_CHARSET_utf8mb4 */


#ifdef HAVE_CHARSET_utf16
  add_compiled_collation(&my_charset_utf16_general_ci);
  add_compiled_collation(&my_charset_utf16_bin);
  add_compiled_collation(&my_charset_utf16le_general_ci);
  add_compiled_collation(&my_charset_utf16le_bin);
  add_compiled_collation(&my_charset_utf16_general_nopad_ci);
  add_compiled_collation(&my_charset_utf16_nopad_bin);
  add_compiled_collation(&my_charset_utf16le_general_nopad_ci);
  add_compiled_collation(&my_charset_utf16le_nopad_bin);
#ifdef HAVE_UCA_COLLATIONS
  add_compiled_collation(&my_charset_utf16_unicode_ci);
  add_compiled_collation(&my_charset_utf16_german2_uca_ci);
  add_compiled_collation(&my_charset_utf16_icelandic_uca_ci);
  add_compiled_collation(&my_charset_utf16_latvian_uca_ci);
  add_compiled_collation(&my_charset_utf16_romanian_uca_ci);
  add_compiled_collation(&my_charset_utf16_slovenian_uca_ci);
  add_compiled_collation(&my_charset_utf16_polish_uca_ci);
  add_compiled_collation(&my_charset_utf16_estonian_uca_ci);
  add_compiled_collation(&my_charset_utf16_spanish_uca_ci);
  add_compiled_collation(&my_charset_utf16_swedish_uca_ci);
  add_compiled_collation(&my_charset_utf16_turkish_uca_ci);
  add_compiled_collation(&my_charset_utf16_czech_uca_ci);
  add_compiled_collation(&my_charset_utf16_danish_uca_ci);
  add_compiled_collation(&my_charset_utf16_lithuanian_uca_ci);
  add_compiled_collation(&my_charset_utf16_slovak_uca_ci);
  add_compiled_collation(&my_charset_utf16_spanish2_uca_ci);
  add_compiled_collation(&my_charset_utf16_roman_uca_ci);
  add_compiled_collation(&my_charset_utf16_persian_uca_ci);
  add_compiled_collation(&my_charset_utf16_esperanto_uca_ci);
  add_compiled_collation(&my_charset_utf16_hungarian_uca_ci);
  add_compiled_collation(&my_charset_utf16_croatian_mysql561_uca_ci);
  add_compiled_collation(&my_charset_utf16_sinhala_uca_ci);
  add_compiled_collation(&my_charset_utf16_unicode_520_ci);
  add_compiled_collation(&my_charset_utf16_vietnamese_ci);
  add_compiled_collation(&my_charset_utf16_croatian_uca_ci);
  add_compiled_collation(&my_charset_utf16_myanmar_uca_ci);
  add_compiled_collation(&my_charset_utf16_thai_520_w2);
  add_compiled_collation(&my_charset_utf16_unicode_nopad_ci);
  add_compiled_collation(&my_charset_utf16_unicode_520_nopad_ci);
#endif /* HAVE_UCA_COLLATIONS */
#endif /* HAVE_CHARSET_utf16 */


#ifdef HAVE_CHARSET_utf32
  add_compiled_collation(&my_charset_utf32_general_ci);
  add_compiled_collation(&my_charset_utf32_bin);
  add_compiled_collation(&my_charset_utf32_general_nopad_ci);
  add_compiled_collation(&my_charset_utf32_nopad_bin);
#ifdef HAVE_UCA_COLLATIONS
  add_compiled_collation(&my_charset_utf32_unicode_ci);
  add_compiled_collation(&my_charset_utf32_german2_uca_ci);
  add_compiled_collation(&my_charset_utf32_icelandic_uca_ci);
  add_compiled_collation(&my_charset_utf32_latvian_uca_ci);
  add_compiled_collation(&my_charset_utf32_romanian_uca_ci);
  add_compiled_collation(&my_charset_utf32_slovenian_uca_ci);
  add_compiled_collation(&my_charset_utf32_polish_uca_ci);
  add_compiled_collation(&my_charset_utf32_estonian_uca_ci);
  add_compiled_collation(&my_charset_utf32_spanish_uca_ci);
  add_compiled_collation(&my_charset_utf32_swedish_uca_ci);
  add_compiled_collation(&my_charset_utf32_turkish_uca_ci);
  add_compiled_collation(&my_charset_utf32_czech_uca_ci);
  add_compiled_collation(&my_charset_utf32_danish_uca_ci);
  add_compiled_collation(&my_charset_utf32_lithuanian_uca_ci);
  add_compiled_collation(&my_charset_utf32_slovak_uca_ci);
  add_compiled_collation(&my_charset_utf32_spanish2_uca_ci);
  add_compiled_collation(&my_charset_utf32_roman_uca_ci);
  add_compiled_collation(&my_charset_utf32_persian_uca_ci);
  add_compiled_collation(&my_charset_utf32_esperanto_uca_ci);
  add_compiled_collation(&my_charset_utf32_hungarian_uca_ci);
  add_compiled_collation(&my_charset_utf32_croatian_mysql561_uca_ci);
  add_compiled_collation(&my_charset_utf32_sinhala_uca_ci);
  add_compiled_collation(&my_charset_utf32_unicode_520_ci);
  add_compiled_collation(&my_charset_utf32_vietnamese_ci);
  add_compiled_collation(&my_charset_utf32_croatian_uca_ci);
  add_compiled_collation(&my_charset_utf32_myanmar_uca_ci);
  add_compiled_collation(&my_charset_utf32_thai_520_w2);
  add_compiled_collation(&my_charset_utf32_unicode_nopad_ci);
  add_compiled_collation(&my_charset_utf32_unicode_520_nopad_ci);
#endif /* HAVE_UCA_COLLATIONS */
#endif /* HAVE_CHARSET_utf32 */

  /* Copy compiled charsets */
  for (cs=compiled_charsets; cs->col_name.str; cs++)
    add_compiled_extra_collation((struct charset_info_st *) cs);
  
  return FALSE;
}
