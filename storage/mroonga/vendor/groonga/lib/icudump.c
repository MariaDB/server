/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2010 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/
#include <stdio.h>
#include <getopt.h>
#include <unistd.h>
#include <string.h>
#include <unicode/utf.h>
#include <unicode/uchar.h>
#include <unicode/unorm.h>
#include <unicode/ustring.h>

#define MAX_UNICODE 0x110000
#define BUF_SIZE 0x100

static int
ucs2utf(unsigned int i, unsigned char *buf)
{
  unsigned char *p = buf;
  if (i < 0x80) {
    *p++ = i;
  } else {
    if (i < 0x800) {
      *p++ = (i >> 6) | 0xc0;
    } else {
      if (i < 0x00010000) {
        *p++ = (i >> 12) | 0xe0;
      } else {
        if (i < 0x00200000) {
          *p++ = (i >> 18) | 0xf0;
        } else {
          if (i < 0x04000000) {
            *p++ = (i >> 24) | 0xf8;
          } else if (i < 0x80000000) {
            *p++ = (i >> 30) | 0xfc;
            *p++ = ((i >> 24) & 0x3f) | 0x80;
          }
          *p++ = ((i >> 18) & 0x3f) | 0x80;
        }
        *p++ = ((i >> 12) & 0x3f) | 0x80;
      }
      *p++ = ((i >> 6) & 0x3f) | 0x80;
    }
    *p++ = (0x3f & i) | 0x80;
  }
  *p = '\0';
  return (p - buf);
}

void
blockcode(void)
{
  UChar32 ch;
  unsigned char *p, src[7];
  UBlockCode code, lc = -1;
  for (ch = 1; ch < MAX_UNICODE; ch++) {
    if (!U_IS_UNICODE_CHAR(ch)) { continue; }
    code = ublock_getCode(ch);
    if (code != lc) {
      ucs2utf(ch, src);
      for (p = src; *p; p++) {
        printf("%x:", *p);
      }
      printf("\t%04x\t%d\n", ch, code);
    }
    lc = code;
  }
}

int
normalize(const char *str, char *res, UNormalizationMode mode)
{
  UErrorCode rc;
  int32_t ulen, nlen;
  UChar ubuf[BUF_SIZE], nbuf[BUF_SIZE];
  rc = U_ZERO_ERROR;
  u_strFromUTF8(ubuf, BUF_SIZE, &ulen, str, -1, &rc);
  if (rc != U_ZERO_ERROR /*&& rc != U_STRING_NOT_TERMINATED_WARNING*/) {
    return -1;
  }
  rc = U_ZERO_ERROR;
  nlen = unorm_normalize(ubuf, ulen, mode, 0, nbuf, BUF_SIZE, &rc);
  if (rc != U_ZERO_ERROR /*&& rc != U_STRING_NOT_TERMINATED_WARNING*/) {
    return -1;
  }
  rc = U_ZERO_ERROR;
  u_strToUTF8(res, BUF_SIZE, NULL, nbuf, nlen, &rc);
  if (rc != U_ZERO_ERROR /*&& rc != U_BUFFER_OVERFLOW_ERROR*/) {
    return -1;
  }
  return 0;
}

void
dump(UNormalizationMode mode)
{
  UChar32 ch;
  char str[7], norm[BUF_SIZE];
  for (ch = 1; ch < MAX_UNICODE; ch++) {
    if (!U_IS_UNICODE_CHAR(ch)) { continue; }
    ucs2utf(ch, (unsigned char *)str);
    if (normalize(str, norm, mode)) {
      printf("ch=%04x error occure\n", ch);
      continue;
    }
    if (strcmp(norm, str)) {
      printf("%04x\t%s\t%s\n", ch, str, norm);
    }
  }
}

void
ccdump(void)
{
  UChar32 ch;
  char str[7], nfd[BUF_SIZE], nfc[BUF_SIZE];
  for (ch = 1; ch < MAX_UNICODE; ch++) {
    if (!U_IS_UNICODE_CHAR(ch)) { continue; }
    ucs2utf(ch, (unsigned char *)str);
    if (normalize(str, nfd, UNORM_NFD)) {
      printf("ch=%04x error occure\n", ch);
      continue;
    }
    if (normalize(str, nfc, UNORM_NFC)) {
      printf("ch=%04x error occure\n", ch);
      continue;
    }
    if (strcmp(nfd, nfc)) {
      printf("%04x\t%s\t%s\n", ch, nfd, nfc);
    }
  }
}

enum {
  ctype_null = 0,
  ctype_alpha,
  ctype_digit,
  ctype_symbol,
  ctype_hiragana,
  ctype_katakana,
  ctype_kanji,
  ctype_others
};

static const char *ctypes[] = {
  "GRN_CHAR_NULL",
  "GRN_CHAR_ALPHA",
  "GRN_CHAR_DIGIT",
  "GRN_CHAR_SYMBOL",
  "GRN_CHAR_HIRAGANA",
  "GRN_CHAR_KATAKANA",
  "GRN_CHAR_KANJI",
  "GRN_CHAR_OTHERS"
};

void
gcdump(void)
{
  UChar32 ch;
  unsigned char *p, src[7];
  int ctype, lc = -1;
  for (ch = 1; ch < MAX_UNICODE; ch++) {
    UCharCategory cat;
    UBlockCode code;
    if (!U_IS_UNICODE_CHAR(ch)) { continue; }
    code = ublock_getCode(ch);
    switch (code) {
    case UBLOCK_CJK_RADICALS_SUPPLEMENT: /* cjk radicals */
    case UBLOCK_KANGXI_RADICALS: /* kanji radicals */
    case UBLOCK_BOPOMOFO: /* bopomofo letter */
    case UBLOCK_HANGUL_COMPATIBILITY_JAMO: /* hangul letter */
    case UBLOCK_KANBUN: /* kaeri ten used in kanbun ex. re-ten */
    case UBLOCK_BOPOMOFO_EXTENDED: /* bopomofo extended letter */
    case UBLOCK_CJK_UNIFIED_IDEOGRAPHS_EXTENSION_A: /* cjk letter */
    case UBLOCK_CJK_UNIFIED_IDEOGRAPHS: /* cjk letter */
    case UBLOCK_YI_SYLLABLES: /* Yi syllables */
    case UBLOCK_YI_RADICALS: /* Yi radicals */
    case UBLOCK_HANGUL_SYLLABLES: /* hangul syllables */
    case UBLOCK_CJK_COMPATIBILITY_IDEOGRAPHS: /* cjk letter */
    case UBLOCK_CJK_UNIFIED_IDEOGRAPHS_EXTENSION_B: /* cjk letter */
    case UBLOCK_CJK_COMPATIBILITY_IDEOGRAPHS_SUPPLEMENT: /* cjk letter */
    case UBLOCK_CJK_STROKES: /* kakijun*/
      ctype = ctype_kanji;
      break;
    case UBLOCK_CJK_SYMBOLS_AND_PUNCTUATION: /* symbols ex. JIS mark */
    case UBLOCK_ENCLOSED_CJK_LETTERS_AND_MONTHS: /* ex. (kabu) */
    case UBLOCK_CJK_COMPATIBILITY: /* symbols ex. ton doll */
    case UBLOCK_CJK_COMPATIBILITY_FORMS: /* symbols ex. tategaki kagi-kakko */
      ctype = ctype_symbol;
      break;
    case UBLOCK_HIRAGANA:
      ctype = ctype_hiragana;
      break;
    case UBLOCK_KATAKANA:
    case UBLOCK_KATAKANA_PHONETIC_EXTENSIONS:
      ctype = ctype_katakana;
      break;
    default:
      cat = u_charType(ch);
      switch (cat) {
      case U_UPPERCASE_LETTER:
      case U_LOWERCASE_LETTER:
      case U_TITLECASE_LETTER:
      case U_MODIFIER_LETTER:
      case U_OTHER_LETTER:
        ctype = ctype_alpha;
        break;
      case U_DECIMAL_DIGIT_NUMBER:
      case U_LETTER_NUMBER:
      case U_OTHER_NUMBER:
        ctype = ctype_digit;
        break;
      case U_DASH_PUNCTUATION:
      case U_START_PUNCTUATION:
      case U_END_PUNCTUATION:
      case U_CONNECTOR_PUNCTUATION:
      case U_OTHER_PUNCTUATION:
      case U_MATH_SYMBOL:
      case U_CURRENCY_SYMBOL:
      case U_MODIFIER_SYMBOL:
      case U_OTHER_SYMBOL:
        ctype = ctype_symbol;
        break;
      default:
        ctype = ctype_others;
        break;
      }
      break;
    }
    if (ctype != lc) {
      ucs2utf(ch, src);
      for (p = src; *p; p++) {
        printf("%x:", *p);
      }
      printf("\t%04x\t%s\n", ch, ctypes[ctype]);
    }
    lc = ctype;
  }
}

struct option options[] = {
  {"bc", 0, NULL, 'b'},
  {"nfd", 0, NULL, 'd'},
  {"nfkd", 0, NULL, 'D'},
  {"nfc", 0, NULL, 'c'},
  {"nfkc", 0, NULL, 'C'},
  {"cc", 0, NULL, 'o'},
  {"gc", 0, NULL, 'g'},
  {"version", 0, NULL, 'v'},
};

int
main(int argc, char **argv)
{
  switch (getopt_long(argc, argv, "bdDcCogv", options, NULL)) {
  case 'b' :
    blockcode();
    break;
  case 'd' :
    dump(UNORM_NFD);
    break;
  case 'D' :
    dump(UNORM_NFKD);
    break;
  case 'c' :
    dump(UNORM_NFC);
    break;
  case 'C' :
    dump(UNORM_NFKC);
    break;
  case 'o' :
    ccdump();
    break;
  case 'g' :
    gcdump();
    break;
  case 'v' :
    printf("%s\n", U_UNICODE_VERSION);
    break;
  default :
    fputs("usage: icudump --[bc|nfd|nfkd|nfc|nfkc|cc|gc|version]\n", stderr);
    break;
  }
  return 0;
}
