/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2016 Brazil

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
#include "grn.h"
#include <limits.h>
#include <stdarg.h>
#include <string.h>
#include "grn_db.h"
#include "grn_str.h"
#include "grn_nfkc.h"

#ifndef _ISOC99_SOURCE
#define _ISOC99_SOURCE
#endif /* _ISOC99_SOURCE */
#include <math.h>

#if defined(HAVE__GMTIME64_S) && defined(__GNUC__)
# ifdef _WIN64
#  define gmtime_s(tm, time) _gmtime64_s(tm, time)
# else /* _WIN64 */
#  define gmtime_s(tm, time) _gmtime32_s(tm, time)
# endif /* _WIN64 */
#endif /* defined(HAVE__GMTIME64_S) && defined(__GNUC__) */

inline static int
grn_str_charlen_utf8(grn_ctx *ctx, const unsigned char *str, const unsigned char *end)
{
  /* MEMO: This function allows non-null-terminated string as str. */
  /*       But requires the end of string. */
  if (end <= str || !*str) {
    return 0;
  }
  if (*str & 0x80) {
    int i;
    int len;
    GRN_BIT_SCAN_REV(~(((uint) *str) << 24), len);
    len = 31 - len;
    if ((unsigned int)(len - 2) >= 3) {  /* (len == 1 || len >= 5) */
      GRN_LOG(ctx, GRN_LOG_WARNING,
              "grn_str_charlen_utf8(): first byte is invalid");
      return 0;
    }
    if (str + len > end) {
      GRN_LOG(ctx, GRN_LOG_WARNING,
              "grn_str_charlen_utf8(): incomplete character");
      return 0;
    }
    for (i = 1; i < len; ++i) {
      if ((str[i] & 0xc0) != 0x80) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "grn_str_charlen_utf8(): <%d>th byte is invalid",
                i + 1);
        return 0;
      }
    }
    return len;
  } else {
    return 1;
  }
}

unsigned int
grn_str_charlen(grn_ctx *ctx, const char *str, grn_encoding encoding)
{
  /* MEMO: This function requires null-terminated string as str.*/
  unsigned char *p = (unsigned char *) str;
  if (!*p) { return 0; }
  switch (encoding) {
  case GRN_ENC_EUC_JP :
    if (*p & 0x80) {
      if (*(p + 1)) {
        return 2;
      } else {
        /* This is invalid character */
        GRN_LOG(ctx, GRN_LOG_WARNING, "invalid euc-jp string end on grn_str_charlen");
        return 0;
      }
    }
    return 1;
  case GRN_ENC_UTF8 :
    if (*p & 0x80) {
      int b, w;
      size_t size;
      for (b = 0x40, w = 0; b && (*p & b); b >>= 1, w++);
      if (!w) {
        GRN_LOG(ctx, GRN_LOG_WARNING, "invalid utf8 string(1) on grn_str_charlen");
        return 0;
      }
      for (size = 1; w--; size++) {
        if (!*++p || (*p & 0xc0) != 0x80) {
          GRN_LOG(ctx, GRN_LOG_WARNING, "invalid utf8 string(2) on grn_str_charlen");
          return 0;
        }
      }
      return size;
    } else {
      return 1;
    }
  case GRN_ENC_SJIS :
    if (*p & 0x80) {
      /* we regard 0xa0 as JIS X 0201 KANA. adjusted to other tools. */
      if (0xa0 <= *p && *p <= 0xdf) {
        /* hankaku-kana */
        return 1;
      } else if (!(*(p + 1))) {
        /* This is invalid character */
        GRN_LOG(ctx, GRN_LOG_WARNING, "invalid sjis string end on grn_str_charlen");
        return 0;
      } else {
        return 2;
      }
    } else {
      return 1;
    }
  default :
    return 1;
  }
  return 0;
}

int
grn_charlen_(grn_ctx *ctx, const char *str, const char *end, grn_encoding encoding)
{
  /* MEMO: This function allows non-null-terminated string as str. */
  /*       But requires the end of string. */
  unsigned char *p = (unsigned char *) str;
  if (p >= (unsigned char *)end) { return 0; }
  switch (encoding) {
  case GRN_ENC_EUC_JP :
    if (*p & 0x80) {
      if ((p + 1) < (unsigned char *)end) {
        return 2;
      } else {
        /* This is invalid character */
        GRN_LOG(ctx, GRN_LOG_WARNING, "invalid euc-jp string end on grn_charlen");
        return 0;
      }
    }
    return 1;
  case GRN_ENC_UTF8 :
    return grn_str_charlen_utf8(ctx, p, (unsigned char *)end);
  case GRN_ENC_SJIS :
    if (*p & 0x80) {
      /* we regard 0xa0 as JIS X 0201 KANA. adjusted to other tools. */
      if (0xa0 <= *p && *p <= 0xdf) {
        /* hankaku-kana */
        return 1;
      } else if (++p >= (unsigned char *)end) {
        /* This is invalid character */
        GRN_LOG(ctx, GRN_LOG_WARNING, "invalid sjis string end on grn_charlen");
        return 0;
      } else {
        return 2;
      }
    } else {
      return 1;
    }
  default :
    return 1;
  }
  return 0;
}

int
grn_charlen(grn_ctx *ctx, const char *str, const char *end)
{
  return grn_charlen_(ctx, str, end, ctx->encoding);
}

static unsigned char symbol[] = {
  ',', '.', 0, ':', ';', '?', '!', 0, 0, 0, '`', 0, '^', '~', '_', 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, '-', '-', '/', '\\', 0, 0, '|', 0, 0, 0, '\'', 0,
  '"', '(', ')', 0, 0, '[', ']', '{', '}', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  '+', '-', 0, 0, 0, '=', 0, '<', '>', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  '$', 0, 0, '%', '#', '&', '*', '@', 0, 0, 0, 0, 0, 0, 0, 0
};

inline static grn_rc
normalize_euc(grn_ctx *ctx, grn_str *nstr)
{
  static uint16_t hankana[] = {
    0xa1a1, 0xa1a3, 0xa1d6, 0xa1d7, 0xa1a2, 0xa1a6, 0xa5f2, 0xa5a1, 0xa5a3,
    0xa5a5, 0xa5a7, 0xa5a9, 0xa5e3, 0xa5e5, 0xa5e7, 0xa5c3, 0xa1bc, 0xa5a2,
    0xa5a4, 0xa5a6, 0xa5a8, 0xa5aa, 0xa5ab, 0xa5ad, 0xa5af, 0xa5b1, 0xa5b3,
    0xa5b5, 0xa5b7, 0xa5b9, 0xa5bb, 0xa5bd, 0xa5bf, 0xa5c1, 0xa5c4, 0xa5c6,
    0xa5c8, 0xa5ca, 0xa5cb, 0xa5cc, 0xa5cd, 0xa5ce, 0xa5cf, 0xa5d2, 0xa5d5,
    0xa5d8, 0xa5db, 0xa5de, 0xa5df, 0xa5e0, 0xa5e1, 0xa5e2, 0xa5e4, 0xa5e6,
    0xa5e8, 0xa5e9, 0xa5ea, 0xa5eb, 0xa5ec, 0xa5ed, 0xa5ef, 0xa5f3, 0xa1ab,
    0xa1eb
  };
  static unsigned char dakuten[] = {
    0xf4, 0, 0, 0, 0, 0xac, 0, 0xae, 0, 0xb0, 0, 0xb2, 0, 0xb4, 0, 0xb6, 0,
    0xb8, 0, 0xba, 0, 0xbc, 0, 0xbe, 0, 0xc0, 0, 0xc2, 0, 0, 0xc5, 0, 0xc7,
    0, 0xc9, 0, 0, 0, 0, 0, 0, 0xd0, 0, 0, 0xd3, 0, 0, 0xd6, 0, 0, 0xd9, 0,
    0, 0xdc
  };
  static unsigned char handaku[] = {
    0xd1, 0, 0, 0xd4, 0, 0, 0xd7, 0, 0, 0xda, 0, 0, 0xdd
  };
  int16_t *ch;
  const unsigned char *s, *s_, *e;
  unsigned char *d, *d0, *d_, b;
  uint_least8_t *cp, *ctypes, ctype;
  size_t size = nstr->orig_blen, length = 0;
  int removeblankp = nstr->flags & GRN_STR_REMOVEBLANK;
  if (!(nstr->norm = GRN_MALLOC(size * 2 + 1))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  d0 = (unsigned char *) nstr->norm;
  if (nstr->flags & GRN_STR_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * 2 * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->norm);
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STR_WITH_CTYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->norm);
      nstr->checks = NULL;
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->orig + size;
  for (s = s_ = (unsigned char *) nstr->orig, d = d_ = d0; s < e; s++) {
    if ((*s & 0x80)) {
      if (((s + 1) < e) && (*(s + 1) & 0x80)) {
        unsigned char c1 = *s++, c2 = *s, c3 = 0;
        switch (c1 >> 4) {
        case 0x08 :
          if (c1 == 0x8e && 0xa0 <= c2 && c2 <= 0xdf) {
            uint16_t c = hankana[c2 - 0xa0];
            switch (c) {
            case 0xa1ab :
              if (d > d0 + 1 && d[-2] == 0xa5
                  && 0xa6 <= d[-1] && d[-1] <= 0xdb && (b = dakuten[d[-1] - 0xa6])) {
                *(d - 1) = b;
                if (ch) { ch[-1] += 2; s_ += 2; }
                continue;
              } else {
                *d++ = c >> 8; *d = c & 0xff;
              }
              break;
            case 0xa1eb :
              if (d > d0 + 1 && d[-2] == 0xa5
                  && 0xcf <= d[-1] && d[-1] <= 0xdb && (b = handaku[d[-1] - 0xcf])) {
                *(d - 1) = b;
                if (ch) { ch[-1] += 2; s_ += 2; }
                continue;
              } else {
                *d++ = c >> 8; *d = c & 0xff;
              }
              break;
            default :
              *d++ = c >> 8; *d = c & 0xff;
              break;
            }
            ctype = GRN_CHAR_KATAKANA;
          } else {
            *d++ = c1; *d = c2;
            ctype = GRN_CHAR_OTHERS;
          }
          break;
        case 0x09 :
          *d++ = c1; *d = c2;
          ctype = GRN_CHAR_OTHERS;
          break;
        case 0x0a :
          switch (c1 & 0x0f) {
          case 1 :
            switch (c2) {
            case 0xbc :
              *d++ = c1; *d = c2;
              ctype = GRN_CHAR_KATAKANA;
              break;
            case 0xb9 :
              *d++ = c1; *d = c2;
              ctype = GRN_CHAR_KANJI;
              break;
            case 0xa1 :
              if (removeblankp) {
                if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
                continue;
              } else {
                *d = ' ';
                ctype = GRN_STR_BLANK|GRN_CHAR_SYMBOL;
              }
              break;
            default :
              if (c2 >= 0xa4 && (c3 = symbol[c2 - 0xa4])) {
                *d = c3;
                ctype = GRN_CHAR_SYMBOL;
              } else {
                *d++ = c1; *d = c2;
                ctype = GRN_CHAR_OTHERS;
              }
              break;
            }
            break;
          case 2 :
            *d++ = c1; *d = c2;
            ctype = GRN_CHAR_SYMBOL;
            break;
          case 3 :
            c3 = c2 - 0x80;
            if ('a' <= c3 && c3 <= 'z') {
              ctype = GRN_CHAR_ALPHA;
              *d = c3;
            } else if ('A' <= c3 && c3 <= 'Z') {
              ctype = GRN_CHAR_ALPHA;
              *d = c3 + 0x20;
            } else if ('0' <= c3 && c3 <= '9') {
              ctype = GRN_CHAR_DIGIT;
              *d = c3;
            } else {
              ctype = GRN_CHAR_OTHERS;
              *d++ = c1; *d = c2;
            }
            break;
          case 4 :
            *d++ = c1; *d = c2;
            ctype = GRN_CHAR_HIRAGANA;
            break;
          case 5 :
            *d++ = c1; *d = c2;
            ctype = GRN_CHAR_KATAKANA;
            break;
          case 6 :
          case 7 :
          case 8 :
            *d++ = c1; *d = c2;
            ctype = GRN_CHAR_SYMBOL;
            break;
          default :
            *d++ = c1; *d = c2;
            ctype = GRN_CHAR_OTHERS;
            break;
          }
          break;
        default :
          *d++ = c1; *d = c2;
          ctype = GRN_CHAR_KANJI;
          break;
        }
      } else {
        /* skip invalid character */
        continue;
      }
    } else {
      unsigned char c = *s;
      switch (c >> 4) {
      case 0 :
      case 1 :
        /* skip unprintable ascii */
        if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
        continue;
      case 2 :
        if (c == 0x20) {
          if (removeblankp) {
            if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
            continue;
          } else {
            *d = ' ';
            ctype = GRN_STR_BLANK|GRN_CHAR_SYMBOL;
          }
        } else {
          *d = c;
          ctype = GRN_CHAR_SYMBOL;
        }
        break;
      case 3 :
        *d = c;
        ctype = (c <= 0x39) ? GRN_CHAR_DIGIT : GRN_CHAR_SYMBOL;
        break;
      case 4 :
        *d = ('A' <= c) ? c + 0x20 : c;
        ctype = (c == 0x40) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
        break;
      case 5 :
        *d = (c <= 'Z') ? c + 0x20 : c;
        ctype = (c <= 0x5a) ? GRN_CHAR_ALPHA : GRN_CHAR_SYMBOL;
        break;
      case 6 :
        *d = c;
        ctype = (c == 0x60) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
        break;
      case 7 :
        *d = c;
        ctype = (c <= 0x7a) ? GRN_CHAR_ALPHA : (c == 0x7f ? GRN_CHAR_OTHERS : GRN_CHAR_SYMBOL);
        break;
      default :
        *d = c;
        ctype = GRN_CHAR_OTHERS;
        break;
      }
    }
    d++;
    length++;
    if (cp) { *cp++ = ctype; }
    if (ch) {
      *ch++ = (int16_t)(s + 1 - s_);
      s_ = s + 1;
      while (++d_ < d) { *ch++ = 0; }
    }
  }
  if (cp) { *cp = GRN_CHAR_NULL; }
  *d = '\0';
  nstr->length = length;
  nstr->norm_blen = (size_t)(d - (unsigned char *)nstr->norm);
  return GRN_SUCCESS;
}

#ifdef GRN_WITH_NFKC
inline static grn_rc
normalize_utf8(grn_ctx *ctx, grn_str *nstr)
{
  int16_t *ch;
  const unsigned char *s, *s_, *s__ = NULL, *p, *p2, *pe, *e;
  unsigned char *d, *d_, *de;
  uint_least8_t *cp;
  size_t length = 0, ls, lp, size = nstr->orig_blen, ds = size * 3;
  int removeblankp = nstr->flags & GRN_STR_REMOVEBLANK;
  if (!(nstr->norm = GRN_MALLOC(ds + 1))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  if (nstr->flags & GRN_STR_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(ds * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->norm); nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STR_WITH_CTYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(ds + 1))) {
      if (nstr->checks) { GRN_FREE(nstr->checks); nstr->checks = NULL; }
      GRN_FREE(nstr->norm); nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  cp = nstr->ctypes;
  d = (unsigned char *)nstr->norm;
  de = d + ds;
  d_ = NULL;
  e = (unsigned char *)nstr->orig + size;
  for (s = s_ = (unsigned char *)nstr->orig; ; s += ls) {
    if (!(ls = grn_str_charlen_utf8(ctx, s, e))) {
      break;
    }
    if ((p = (unsigned char *)grn_nfkc_decompose(s))) {
      pe = p + strlen((char *)p);
    } else {
      p = s;
      pe = p + ls;
    }
    if (d_ && (p2 = (unsigned char *)grn_nfkc_compose(d_, p))) {
      p = p2;
      pe = p + strlen((char *)p);
      if (cp) { cp--; }
      if (ch) {
        ch -= (d - d_);
        s_ = s__;
      }
      d = d_;
      length--;
    }
    for (; ; p += lp) {
      if (!(lp = grn_str_charlen_utf8(ctx, p, pe))) {
        break;
      }
      if ((*p == ' ' && removeblankp) || *p < 0x20  /* skip unprintable ascii */ ) {
        if (cp > nstr->ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
      } else {
        if (de <= d + lp) {
          unsigned char *norm;
          ds += (ds >> 1) + lp;
          if (!(norm = GRN_REALLOC(nstr->norm, ds + 1))) {
            if (nstr->ctypes) { GRN_FREE(nstr->ctypes); nstr->ctypes = NULL; }
            if (nstr->checks) { GRN_FREE(nstr->checks); nstr->checks = NULL; }
            GRN_FREE(nstr->norm); nstr->norm = NULL;
            return GRN_NO_MEMORY_AVAILABLE;
          }
          de = norm + ds;
          d = norm + (d - (unsigned char *)nstr->norm);
          nstr->norm = (char *)norm;
          if (ch) {
            int16_t *checks;
            if (!(checks = GRN_REALLOC(nstr->checks, ds * sizeof(int16_t)+ 1))) {
              if (nstr->ctypes) { GRN_FREE(nstr->ctypes); nstr->ctypes = NULL; }
              GRN_FREE(nstr->checks); nstr->checks = NULL;
              GRN_FREE(nstr->norm); nstr->norm = NULL;
              return GRN_NO_MEMORY_AVAILABLE;
            }
            ch = checks + (ch - nstr->checks);
            nstr->checks = checks;
          }
          if (cp) {
            uint_least8_t *ctypes;
            if (!(ctypes = GRN_REALLOC(nstr->ctypes, ds + 1))) {
              GRN_FREE(nstr->ctypes); nstr->ctypes = NULL;
              if (nstr->checks) { GRN_FREE(nstr->checks); nstr->checks = NULL; }
              GRN_FREE(nstr->norm); nstr->norm = NULL;
              return GRN_NO_MEMORY_AVAILABLE;
            }
            cp = ctypes + (cp - nstr->ctypes);
            nstr->ctypes = ctypes;
          }
        }
        grn_memcpy(d, p, lp);
        d_ = d;
        d += lp;
        length++;
        if (cp) { *cp++ = grn_nfkc_char_type(p); }
        if (ch) {
          size_t i;
          if (s_ == s + ls) {
            *ch++ = -1;
          } else {
            *ch++ = (int16_t)(s + ls - s_);
            s__ = s_;
            s_ = s + ls;
          }
          for (i = lp; i > 1; i--) { *ch++ = 0; }
        }
      }
    }
  }
  if (cp) { *cp = GRN_CHAR_NULL; }
  *d = '\0';
  nstr->length = length;
  nstr->norm_blen = (size_t)(d - (unsigned char *)nstr->norm);
  return GRN_SUCCESS;
}
#endif /* GRN_WITH_NFKC */

inline static grn_rc
normalize_sjis(grn_ctx *ctx, grn_str *nstr)
{
  static uint16_t hankana[] = {
    0x8140, 0x8142, 0x8175, 0x8176, 0x8141, 0x8145, 0x8392, 0x8340, 0x8342,
    0x8344, 0x8346, 0x8348, 0x8383, 0x8385, 0x8387, 0x8362, 0x815b, 0x8341,
    0x8343, 0x8345, 0x8347, 0x8349, 0x834a, 0x834c, 0x834e, 0x8350, 0x8352,
    0x8354, 0x8356, 0x8358, 0x835a, 0x835c, 0x835e, 0x8360, 0x8363, 0x8365,
    0x8367, 0x8369, 0x836a, 0x836b, 0x836c, 0x836d, 0x836e, 0x8371, 0x8374,
    0x8377, 0x837a, 0x837d, 0x837e, 0x8380, 0x8381, 0x8382, 0x8384, 0x8386,
    0x8388, 0x8389, 0x838a, 0x838b, 0x838c, 0x838d, 0x838f, 0x8393, 0x814a,
    0x814b
  };
  static unsigned char dakuten[] = {
    0x94, 0, 0, 0, 0, 0x4b, 0, 0x4d, 0, 0x4f, 0, 0x51, 0, 0x53, 0, 0x55, 0,
    0x57, 0, 0x59, 0, 0x5b, 0, 0x5d, 0, 0x5f, 0, 0x61, 0, 0, 0x64, 0, 0x66,
    0, 0x68, 0, 0, 0, 0, 0, 0, 0x6f, 0, 0, 0x72, 0, 0, 0x75, 0, 0, 0x78, 0,
    0, 0x7b
  };
  static unsigned char handaku[] = {
    0x70, 0, 0, 0x73, 0, 0, 0x76, 0, 0, 0x79, 0, 0, 0x7c
  };
  int16_t *ch;
  const unsigned char *s, *s_;
  unsigned char *d, *d0, *d_, b, *e;
  uint_least8_t *cp, *ctypes, ctype;
  size_t size = nstr->orig_blen, length = 0;
  int removeblankp = nstr->flags & GRN_STR_REMOVEBLANK;
  if (!(nstr->norm = GRN_MALLOC(size * 2 + 1))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  d0 = (unsigned char *) nstr->norm;
  if (nstr->flags & GRN_STR_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * 2 * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->norm);
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STR_WITH_CTYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->norm);
      nstr->checks = NULL;
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->orig + size;
  for (s = s_ = (unsigned char *) nstr->orig, d = d_ = d0; s < e; s++) {
    if ((*s & 0x80)) {
      if (0xa0 <= *s && *s <= 0xdf) {
        uint16_t c = hankana[*s - 0xa0];
        switch (c) {
        case 0x814a :
          if (d > d0 + 1 && d[-2] == 0x83
              && 0x45 <= d[-1] && d[-1] <= 0x7a && (b = dakuten[d[-1] - 0x45])) {
            *(d - 1) = b;
            if (ch) { ch[-1]++; s_++; }
            continue;
          } else {
            *d++ = c >> 8; *d = c & 0xff;
          }
          break;
        case 0x814b :
          if (d > d0 + 1 && d[-2] == 0x83
              && 0x6e <= d[-1] && d[-1] <= 0x7a && (b = handaku[d[-1] - 0x6e])) {
            *(d - 1) = b;
            if (ch) { ch[-1]++; s_++; }
            continue;
          } else {
            *d++ = c >> 8; *d = c & 0xff;
          }
          break;
        default :
          *d++ = c >> 8; *d = c & 0xff;
          break;
        }
        ctype = GRN_CHAR_KATAKANA;
      } else {
        if ((s + 1) < e && 0x40 <= *(s + 1) && *(s + 1) <= 0xfc) {
          unsigned char c1 = *s++, c2 = *s, c3 = 0;
          if (0x81 <= c1 && c1 <= 0x87) {
            switch (c1 & 0x0f) {
            case 1 :
              switch (c2) {
              case 0x5b :
                *d++ = c1; *d = c2;
                ctype = GRN_CHAR_KATAKANA;
                break;
              case 0x58 :
                *d++ = c1; *d = c2;
                ctype = GRN_CHAR_KANJI;
                break;
              case 0x40 :
                if (removeblankp) {
                  if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
                  continue;
                } else {
                  *d = ' ';
                  ctype = GRN_STR_BLANK|GRN_CHAR_SYMBOL;
                }
                break;
              default :
                if (0x43 <= c2 && c2 <= 0x7e && (c3 = symbol[c2 - 0x43])) {
                  *d = c3;
                  ctype = GRN_CHAR_SYMBOL;
                } else if (0x7f <= c2 && c2 <= 0x97 && (c3 = symbol[c2 - 0x44])) {
                  *d = c3;
                  ctype = GRN_CHAR_SYMBOL;
                } else {
                  *d++ = c1; *d = c2;
                  ctype = GRN_CHAR_OTHERS;
                }
                break;
              }
              break;
            case 2 :
              c3 = c2 - 0x1f;
              if (0x4f <= c2 && c2 <= 0x58) {
                ctype = GRN_CHAR_DIGIT;
                *d = c2 - 0x1f;
              } else if (0x60 <= c2 && c2 <= 0x79) {
                ctype = GRN_CHAR_ALPHA;
                *d = c2 + 0x01;
              } else if (0x81 <= c2 && c2 <= 0x9a) {
                ctype = GRN_CHAR_ALPHA;
                *d = c2 - 0x20;
              } else if (0x9f <= c2 && c2 <= 0xf1) {
                *d++ = c1; *d = c2;
                ctype = GRN_CHAR_HIRAGANA;
              } else {
                *d++ = c1; *d = c2;
                ctype = GRN_CHAR_OTHERS;
              }
              break;
            case 3 :
              if (0x40 <= c2 && c2 <= 0x96) {
                *d++ = c1; *d = c2;
                ctype = GRN_CHAR_KATAKANA;
              } else {
                *d++ = c1; *d = c2;
                ctype = GRN_CHAR_SYMBOL;
              }
              break;
            case 4 :
            case 7 :
              *d++ = c1; *d = c2;
              ctype = GRN_CHAR_SYMBOL;
              break;
            default :
              *d++ = c1; *d = c2;
              ctype = GRN_CHAR_OTHERS;
              break;
            }
          } else {
            *d++ = c1; *d = c2;
            ctype = GRN_CHAR_KANJI;
          }
        } else {
          /* skip invalid character */
          continue;
        }
      }
    } else {
      unsigned char c = *s;
      switch (c >> 4) {
      case 0 :
      case 1 :
        /* skip unprintable ascii */
        if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
        continue;
      case 2 :
        if (c == 0x20) {
          if (removeblankp) {
            if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
            continue;
          } else {
            *d = ' ';
            ctype = GRN_STR_BLANK|GRN_CHAR_SYMBOL;
          }
        } else {
          *d = c;
          ctype = GRN_CHAR_SYMBOL;
        }
        break;
      case 3 :
        *d = c;
        ctype = (c <= 0x39) ? GRN_CHAR_DIGIT : GRN_CHAR_SYMBOL;
        break;
      case 4 :
        *d = ('A' <= c) ? c + 0x20 : c;
        ctype = (c == 0x40) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
        break;
      case 5 :
        *d = (c <= 'Z') ? c + 0x20 : c;
        ctype = (c <= 0x5a) ? GRN_CHAR_ALPHA : GRN_CHAR_SYMBOL;
        break;
      case 6 :
        *d = c;
        ctype = (c == 0x60) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
        break;
      case 7 :
        *d = c;
        ctype = (c <= 0x7a) ? GRN_CHAR_ALPHA : (c == 0x7f ? GRN_CHAR_OTHERS : GRN_CHAR_SYMBOL);
        break;
      default :
        *d = c;
        ctype = GRN_CHAR_OTHERS;
        break;
      }
    }
    d++;
    length++;
    if (cp) { *cp++ = ctype; }
    if (ch) {
      *ch++ = (int16_t)(s + 1 - s_);
      s_ = s + 1;
      while (++d_ < d) { *ch++ = 0; }
    }
  }
  if (cp) { *cp = GRN_CHAR_NULL; }
  *d = '\0';
  nstr->length = length;
  nstr->norm_blen = (size_t)(d - (unsigned char *)nstr->norm);
  return GRN_SUCCESS;
}

inline static grn_rc
normalize_none(grn_ctx *ctx, grn_str *nstr)
{
  int16_t *ch;
  const unsigned char *s, *s_, *e;
  unsigned char *d, *d0, *d_;
  uint_least8_t *cp, *ctypes, ctype;
  size_t size = nstr->orig_blen, length = 0;
  int removeblankp = nstr->flags & GRN_STR_REMOVEBLANK;
  if (!(nstr->norm = GRN_MALLOC(size + 1))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  d0 = (unsigned char *) nstr->norm;
  if (nstr->flags & GRN_STR_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->norm);
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STR_WITH_CTYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->norm);
      nstr->checks = NULL;
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->orig + size;
  for (s = s_ = (unsigned char *) nstr->orig, d = d_ = d0; s < e; s++) {
    unsigned char c = *s;
    switch (c >> 4) {
    case 0 :
    case 1 :
      /* skip unprintable ascii */
      if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
      continue;
    case 2 :
      if (c == 0x20) {
        if (removeblankp) {
          if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
          continue;
        } else {
          *d = ' ';
          ctype = GRN_STR_BLANK|GRN_CHAR_SYMBOL;
        }
      } else {
        *d = c;
        ctype = GRN_CHAR_SYMBOL;
      }
      break;
    case 3 :
      *d = c;
      ctype = (c <= 0x39) ? GRN_CHAR_DIGIT : GRN_CHAR_SYMBOL;
      break;
    case 4 :
      *d = ('A' <= c) ? c + 0x20 : c;
      ctype = (c == 0x40) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
      break;
    case 5 :
      *d = (c <= 'Z') ? c + 0x20 : c;
      ctype = (c <= 0x5a) ? GRN_CHAR_ALPHA : GRN_CHAR_SYMBOL;
      break;
    case 6 :
      *d = c;
      ctype = (c == 0x60) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
      break;
    case 7 :
      *d = c;
      ctype = (c <= 0x7a) ? GRN_CHAR_ALPHA : (c == 0x7f ? GRN_CHAR_OTHERS : GRN_CHAR_SYMBOL);
      break;
    default :
      *d = c;
      ctype = GRN_CHAR_OTHERS;
      break;
    }
    d++;
    length++;
    if (cp) { *cp++ = ctype; }
    if (ch) {
      *ch++ = (int16_t)(s + 1 - s_);
      s_ = s + 1;
      while (++d_ < d) { *ch++ = 0; }
    }
  }
  if (cp) { *cp = GRN_CHAR_NULL; }
  *d = '\0';
  nstr->length = length;
  nstr->norm_blen = (size_t)(d - (unsigned char *)nstr->norm);
  return GRN_SUCCESS;
}

/* use cp1252 as latin1 */
inline static grn_rc
normalize_latin1(grn_ctx *ctx, grn_str *nstr)
{
  int16_t *ch;
  const unsigned char *s, *s_, *e;
  unsigned char *d, *d0, *d_;
  uint_least8_t *cp, *ctypes, ctype;
  size_t size = nstr->orig_blen, length = 0;
  int removeblankp = nstr->flags & GRN_STR_REMOVEBLANK;
  if (!(nstr->norm = GRN_MALLOC(size + 1))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  d0 = (unsigned char *) nstr->norm;
  if (nstr->flags & GRN_STR_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->norm);
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STR_WITH_CTYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->norm);
      nstr->checks = NULL;
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->orig + size;
  for (s = s_ = (unsigned char *) nstr->orig, d = d_ = d0; s < e; s++) {
    unsigned char c = *s;
    switch (c >> 4) {
    case 0 :
    case 1 :
      /* skip unprintable ascii */
      if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
      continue;
    case 2 :
      if (c == 0x20) {
        if (removeblankp) {
          if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
          continue;
        } else {
          *d = ' ';
          ctype = GRN_STR_BLANK|GRN_CHAR_SYMBOL;
        }
      } else {
        *d = c;
        ctype = GRN_CHAR_SYMBOL;
      }
      break;
    case 3 :
      *d = c;
      ctype = (c <= 0x39) ? GRN_CHAR_DIGIT : GRN_CHAR_SYMBOL;
      break;
    case 4 :
      *d = ('A' <= c) ? c + 0x20 : c;
      ctype = (c == 0x40) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
      break;
    case 5 :
      *d = (c <= 'Z') ? c + 0x20 : c;
      ctype = (c <= 0x5a) ? GRN_CHAR_ALPHA : GRN_CHAR_SYMBOL;
      break;
    case 6 :
      *d = c;
      ctype = (c == 0x60) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
      break;
    case 7 :
      *d = c;
      ctype = (c <= 0x7a) ? GRN_CHAR_ALPHA : (c == 0x7f ? GRN_CHAR_OTHERS : GRN_CHAR_SYMBOL);
      break;
    case 8 :
      if (c == 0x8a || c == 0x8c || c == 0x8e) {
        *d = c + 0x10;
        ctype = GRN_CHAR_ALPHA;
      } else {
        *d = c;
        ctype = GRN_CHAR_SYMBOL;
      }
      break;
    case 9 :
      if (c == 0x9a || c == 0x9c || c == 0x9e || c == 0x9f) {
        *d = (c == 0x9f) ? c + 0x60 : c;
        ctype = GRN_CHAR_ALPHA;
      } else {
        *d = c;
        ctype = GRN_CHAR_SYMBOL;
      }
      break;
    case 0x0c :
      *d = c + 0x20;
      ctype = GRN_CHAR_ALPHA;
      break;
    case 0x0d :
      *d = (c == 0xd7 || c == 0xdf) ? c : c + 0x20;
      ctype = (c == 0xd7) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
      break;
    case 0x0e :
      *d = c;
      ctype = GRN_CHAR_ALPHA;
      break;
    case 0x0f :
      *d = c;
      ctype = (c == 0xf7) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
      break;
    default :
      *d = c;
      ctype = GRN_CHAR_OTHERS;
      break;
    }
    d++;
    length++;
    if (cp) { *cp++ = ctype; }
    if (ch) {
      *ch++ = (int16_t)(s + 1 - s_);
      s_ = s + 1;
      while (++d_ < d) { *ch++ = 0; }
    }
  }
  if (cp) { *cp = GRN_CHAR_NULL; }
  *d = '\0';
  nstr->length = length;
  nstr->norm_blen = (size_t)(d - (unsigned char *)nstr->norm);
  return GRN_SUCCESS;
}

inline static grn_rc
normalize_koi8r(grn_ctx *ctx, grn_str *nstr)
{
  int16_t *ch;
  const unsigned char *s, *s_, *e;
  unsigned char *d, *d0, *d_;
  uint_least8_t *cp, *ctypes, ctype;
  size_t size = strlen(nstr->orig), length = 0;
  int removeblankp = nstr->flags & GRN_STR_REMOVEBLANK;
  if (!(nstr->norm = GRN_MALLOC(size + 1))) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  d0 = (unsigned char *) nstr->norm;
  if (nstr->flags & GRN_STR_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->norm);
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STR_WITH_CTYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->norm);
      nstr->checks = NULL;
      nstr->norm = NULL;
      return GRN_NO_MEMORY_AVAILABLE;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->orig + size;
  for (s = s_ = (unsigned char *) nstr->orig, d = d_ = d0; s < e; s++) {
    unsigned char c = *s;
    switch (c >> 4) {
    case 0 :
    case 1 :
      /* skip unprintable ascii */
      if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
      continue;
    case 2 :
      if (c == 0x20) {
        if (removeblankp) {
          if (cp > ctypes) { *(cp - 1) |= GRN_STR_BLANK; }
          continue;
        } else {
          *d = ' ';
          ctype = GRN_STR_BLANK|GRN_CHAR_SYMBOL;
        }
      } else {
        *d = c;
        ctype = GRN_CHAR_SYMBOL;
      }
      break;
    case 3 :
      *d = c;
      ctype = (c <= 0x39) ? GRN_CHAR_DIGIT : GRN_CHAR_SYMBOL;
      break;
    case 4 :
      *d = ('A' <= c) ? c + 0x20 : c;
      ctype = (c == 0x40) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
      break;
    case 5 :
      *d = (c <= 'Z') ? c + 0x20 : c;
      ctype = (c <= 0x5a) ? GRN_CHAR_ALPHA : GRN_CHAR_SYMBOL;
      break;
    case 6 :
      *d = c;
      ctype = (c == 0x60) ? GRN_CHAR_SYMBOL : GRN_CHAR_ALPHA;
      break;
    case 7 :
      *d = c;
      ctype = (c <= 0x7a) ? GRN_CHAR_ALPHA : (c == 0x7f ? GRN_CHAR_OTHERS : GRN_CHAR_SYMBOL);
      break;
    case 0x0a :
      *d = c;
      ctype = (c == 0xa3) ? GRN_CHAR_ALPHA : GRN_CHAR_OTHERS;
      break;
    case 0x0b :
      if (c == 0xb3) {
        *d = c - 0x10;
        ctype = GRN_CHAR_ALPHA;
      } else {
        *d = c;
        ctype = GRN_CHAR_OTHERS;
      }
      break;
    case 0x0c :
    case 0x0d :
      *d = c;
      ctype = GRN_CHAR_ALPHA;
      break;
    case 0x0e :
    case 0x0f :
      *d = c - 0x20;
      ctype = GRN_CHAR_ALPHA;
      break;
    default :
      *d = c;
      ctype = GRN_CHAR_OTHERS;
      break;
    }
    d++;
    length++;
    if (cp) { *cp++ = ctype; }
    if (ch) {
      *ch++ = (int16_t)(s + 1 - s_);
      s_ = s + 1;
      while (++d_ < d) { *ch++ = 0; }
    }
  }
  if (cp) { *cp = GRN_CHAR_NULL; }
  *d = '\0';
  nstr->length = length;
  nstr->norm_blen = (size_t)(d - (unsigned char *)nstr->norm);
  return GRN_SUCCESS;
}

static grn_str *
grn_fakenstr_open(grn_ctx *ctx, const char *str, size_t str_len, grn_encoding encoding, int flags)
{
  /* TODO: support GRN_STR_REMOVEBLANK flag and ctypes */
  grn_str *nstr;
  if (!(nstr = GRN_MALLOC(sizeof(grn_str)))) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "memory allocation on grn_fakenstr_open failed !");
    return NULL;
  }
  if (!(nstr->norm = GRN_MALLOC(str_len + 1))) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "memory allocation for keyword on grn_snip_add_cond failed !");
    GRN_FREE(nstr);
    return NULL;
  }
  nstr->orig = str;
  nstr->orig_blen = str_len;
  grn_memcpy(nstr->norm, str, str_len);
  nstr->norm[str_len] = '\0';
  nstr->norm_blen = str_len;
  nstr->ctypes = NULL;
  nstr->flags = flags;

  if (flags & GRN_STR_WITH_CHECKS) {
    int16_t f = 0;
    unsigned char c;
    size_t i;
    if (!(nstr->checks = (int16_t *) GRN_MALLOC(sizeof(int16_t) * str_len))) {
      GRN_FREE(nstr->norm);
      GRN_FREE(nstr);
      return NULL;
    }
    switch (encoding) {
    case GRN_ENC_EUC_JP:
      for (i = 0; i < str_len; i++) {
        if (!f) {
          c = (unsigned char) str[i];
          f = ((c >= 0xa1U && c <= 0xfeU) || c == 0x8eU ? 2 : (c == 0x8fU ? 3 : 1)
            );
          nstr->checks[i] = f;
        } else {
          nstr->checks[i] = 0;
        }
        f--;
      }
      break;
    case GRN_ENC_SJIS:
      for (i = 0; i < str_len; i++) {
        if (!f) {
          c = (unsigned char) str[i];
          f = (c >= 0x81U && ((c <= 0x9fU) || (c >= 0xe0U && c <= 0xfcU)) ? 2 : 1);
          nstr->checks[i] = f;
        } else {
          nstr->checks[i] = 0;
        }
        f--;
      }
      break;
    case GRN_ENC_UTF8:
      for (i = 0; i < str_len; i++) {
        if (!f) {
          c = (unsigned char) str[i];
          f = (c & 0x80U ? (c & 0x20U ? (c & 0x10U ? 4 : 3)
                           : 2)
               : 1);
          nstr->checks[i] = f;
        } else {
          nstr->checks[i] = 0;
        }
        f--;
      }
      break;
    default:
      for (i = 0; i < str_len; i++) {
        nstr->checks[i] = 1;
      }
      break;
    }
  } else {
    nstr->checks = NULL;
  }
  return nstr;
}

grn_str *
grn_str_open_(grn_ctx *ctx, const char *str, unsigned int str_len, int flags, grn_encoding encoding)
{
  grn_rc rc;
  grn_str *nstr;
  if (!str || !str_len) { return NULL; }

  if (!(flags & GRN_STR_NORMALIZE)) {
    return grn_fakenstr_open(ctx, str, str_len, encoding, flags);
  }

  if (!(nstr = GRN_MALLOC(sizeof(grn_str)))) {
    GRN_LOG(ctx, GRN_LOG_ALERT, "memory allocation on grn_str_open failed !");
    return NULL;
  }
  nstr->orig = str;
  nstr->orig_blen = str_len;
  nstr->norm = NULL;
  nstr->norm_blen = 0;
  nstr->checks = NULL;
  nstr->ctypes = NULL;
  nstr->encoding = encoding;
  nstr->flags = flags;
  switch (encoding) {
  case GRN_ENC_EUC_JP :
    rc = normalize_euc(ctx, nstr);
    break;
  case GRN_ENC_UTF8 :
#ifdef GRN_WITH_NFKC
    rc = normalize_utf8(ctx, nstr);
#else /* GRN_WITH_NFKC */
    rc = normalize_none(ctx, nstr);
#endif /* GRN_WITH_NFKC */
    break;
  case GRN_ENC_SJIS :
    rc = normalize_sjis(ctx, nstr);
    break;
  case GRN_ENC_LATIN1 :
    rc = normalize_latin1(ctx, nstr);
    break;
  case GRN_ENC_KOI8R :
    rc = normalize_koi8r(ctx, nstr);
    break;
  default :
    rc = normalize_none(ctx, nstr);
    break;
  }
  if (rc) {
    grn_str_close(ctx, nstr);
    return NULL;
  }
  return nstr;
}

grn_str *
grn_str_open(grn_ctx *ctx, const char *str, unsigned int str_len, int flags)
{
  return grn_str_open_(ctx, str, str_len, flags, ctx->encoding);
}

grn_rc
grn_str_close(grn_ctx *ctx, grn_str *nstr)
{
  if (nstr) {
    if (nstr->norm) { GRN_FREE(nstr->norm); }
    if (nstr->ctypes) { GRN_FREE(nstr->ctypes); }
    if (nstr->checks) { GRN_FREE(nstr->checks); }
    GRN_FREE(nstr);
    return GRN_SUCCESS;
  } else {
    return GRN_INVALID_ARGUMENT;
  }
}

static const char *grn_enc_string[] = {
  "default",
  "none",
  "euc_jp",
  "utf8",
  "sjis",
  "latin1",
  "koi8r"
};

const char *
grn_encoding_to_string(grn_encoding enc)
{
  if (enc < (sizeof(grn_enc_string) / sizeof(char *))) {
    return grn_enc_string[enc];
  } else {
    return "unknown";
  }
}

grn_encoding
grn_encoding_parse(const char *str)
{
  grn_encoding e = GRN_ENC_UTF8;
  int i = sizeof(grn_enc_string) / sizeof(grn_enc_string[0]);
  while (i--) {
    if (!strcmp(str, grn_enc_string[i])) {
      e = (grn_encoding)i;
    }
  }
  return e;
}

size_t
grn_str_len(grn_ctx *ctx, const char *str, grn_encoding encoding, const char **last)
{
  size_t len, tlen;
  const char *p = NULL;
  for (len = 0; ; len++) {
    p = str;
    if (!(tlen = grn_str_charlen(ctx, str, encoding))) {
      break;
    }
    str += tlen;
  }
  if (last) { *last = p; }
  return len;
}

int
grn_isspace(const char *str, grn_encoding encoding)
{
  const unsigned char *s = (const unsigned char *) str;
  if (!s) { return 0; }
  switch (s[0]) {
  case ' ' :
  case '\f' :
  case '\n' :
  case '\r' :
  case '\t' :
  case '\v' :
    return 1;
  case 0x81 :
    if (encoding == GRN_ENC_SJIS && s[1] == 0x40) { return 2; }
    break;
  case 0xA1 :
    if (encoding == GRN_ENC_EUC_JP && s[1] == 0xA1) { return 2; }
    break;
  case 0xE3 :
    if (encoding == GRN_ENC_UTF8 && s[1] == 0x80 && s[2] == 0x80) { return 3; }
    break;
  default :
    break;
  }
  return 0;
}

int8_t
grn_atoi8(const char *nptr, const char *end, const char **rest)
{
  const char *p = nptr;
  int8_t v = 0, t, n = 0, o = 0;
  if (p < end && *p == '-') {
    p++;
    n = 1;
    o = 1;
  }
  while (p < end && *p >= '0' && *p <= '9') {
    t = v * 10 - (*p - '0');
    if (t > v || (!n && t == INT8_MIN)) { v = 0; break; }
    v = t;
    o = 0;
    p++;
  }
  if (rest) { *rest = o ? nptr : p; }
  return n ? v : -v;
}

uint8_t
grn_atoui8(const char *nptr, const char *end, const char **rest)
{
  uint8_t v = 0, t;
  while (nptr < end && *nptr >= '0' && *nptr <= '9') {
    t = v * 10 + (*nptr - '0');
    if (t < v) { v = 0; break; }
    v = t;
    nptr++;
  }
  if (rest) { *rest = nptr; }
  return v;
}

int16_t
grn_atoi16(const char *nptr, const char *end, const char **rest)
{
  const char *p = nptr;
  int16_t v = 0, t, n = 0, o = 0;
  if (p < end && *p == '-') {
    p++;
    n = 1;
    o = 1;
  }
  while (p < end && *p >= '0' && *p <= '9') {
    t = v * 10 - (*p - '0');
    if (t > v || (!n && t == INT16_MIN)) { v = 0; break; }
    v = t;
    o = 0;
    p++;
  }
  if (rest) { *rest = o ? nptr : p; }
  return n ? v : -v;
}

uint16_t
grn_atoui16(const char *nptr, const char *end, const char **rest)
{
  uint16_t v = 0, t;
  while (nptr < end && *nptr >= '0' && *nptr <= '9') {
    t = v * 10 + (*nptr - '0');
    if (t < v) { v = 0; break; }
    v = t;
    nptr++;
  }
  if (rest) { *rest = nptr; }
  return v;
}

int
grn_atoi(const char *nptr, const char *end, const char **rest)
{
  const char *p = nptr;
  int v = 0, t, n = 0, o = 0;
  if (p < end && *p == '-') {
    p++;
    n = 1;
    o = 1;
  }
  while (p < end && *p >= '0' && *p <= '9') {
    t = v * 10 - (*p - '0');
    if (t > v || (!n && t == INT32_MIN)) { v = 0; break; }
    v = t;
    o = 0;
    p++;
  }
  if (rest) { *rest = o ? nptr : p; }
  return n ? v : -v;
}

unsigned int
grn_atoui(const char *nptr, const char *end, const char **rest)
{
  unsigned int v = 0, t;
  while (nptr < end && *nptr >= '0' && *nptr <= '9') {
    t = v * 10 + (*nptr - '0');
    if (t < v) { v = 0; break; }
    v = t;
    nptr++;
  }
  if (rest) { *rest = nptr; }
  return v;
}

int64_t
grn_atoll(const char *nptr, const char *end, const char **rest)
{
  const char *p = nptr;
  int o = 0;
  int64_t v = 0;
  if (p < end && *p == '-') {
    p++;
    o = 1;
    while (p < end && *p >= '0' && *p <= '9') {
      int64_t t = v * 10 - (*p - '0');
      if (t > v) { v = 0; break; }
      v = t;
      o = 0;
      p++;
    }
  } else {
    while (p < end && *p >= '0' && *p <= '9') {
      int64_t t = v * 10 + (*p - '0');
      if (t < v) { v = 0; break; }
      v = t;
      p++;
    }
  }
  if (rest) { *rest = o ? nptr : p; }
  return v;
}

uint64_t
grn_atoull(const char *nptr, const char *end, const char **rest)
{
  uint64_t v = 0, t;
  while (nptr < end && *nptr >= '0' && *nptr <= '9') {
    t = v * 10 + (*nptr - '0');
    if (t < v) { v = 0; break; }
    v = t;
    nptr++;
  }
  if (rest) { *rest = nptr; }
  return v;
}

unsigned int
grn_htoui(const char *nptr, const char *end, const char **rest)
{
  unsigned int v = 0, t;
  while (nptr < end) {
    switch (*nptr) {
    case '0' :
    case '1' :
    case '2' :
    case '3' :
    case '4' :
    case '5' :
    case '6' :
    case '7' :
    case '8' :
    case '9' :
      t = v * 16 + (*nptr++ - '0');
      break;
    case 'a' :
    case 'b' :
    case 'c' :
    case 'd' :
    case 'e' :
    case 'f' :
      t = v * 16 + (*nptr++ - 'a') + 10;
      break;
    case 'A' :
    case 'B' :
    case 'C' :
    case 'D' :
    case 'E' :
    case 'F' :
      t = v * 16 + (*nptr++ - 'A') + 10;
      break;
    default :
      v = 0; goto exit;
    }
    if (t < v) { v = 0; goto exit; }
    v = t;
  }
exit :
  if (rest) { *rest = nptr; }
  return v;
}

void
grn_itoh(unsigned int i, char *p, unsigned int len)
{
  static const char *hex = "0123456789ABCDEF";
  p += len - 1;
  while (len--) {
    *p-- = hex[i & 0xf];
    i >>= 4;
  }
}

grn_rc
grn_itoa(int i, char *p, char *end, char **rest)
{
  char *q;
  if (p >= end) { return GRN_INVALID_ARGUMENT; }
  q = p;
  if (i < 0) {
    *p++ = '-';
    q = p;
    if (i == INT_MIN) {
      if (p >= end) { return GRN_INVALID_ARGUMENT; }
      *p++ = (-(i % 10)) + '0';
      i /= 10;
    }
    i = -i;
  }
  do {
    if (p >= end) { return GRN_INVALID_ARGUMENT; }
    *p++ = i % 10 + '0';
  } while ((i /= 10) > 0);
  if (rest) { *rest = p; }
  for (p--; q < p; q++, p--) {
    char t = *q;
    *q = *p;
    *p = t;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_itoa_padded(int i, char *p, char *end, char ch)
{
  char *q;
  if (p >= end) { return GRN_INVALID_ARGUMENT; }
  if (i < 0) {
    *p++ = '-';
    if (i == INT_MIN) {
      if (p >= end) { return GRN_INVALID_ARGUMENT; }
      *p++ = (-(i % 10)) + '0';
      i /= 10;
    }
    i = -i;
  }
  q = end - 1;
  do {
    if (q < p) { return GRN_INVALID_ARGUMENT; }
    *q-- = i % 10 + '0';
  } while ((i /= 10) > 0);
  while (q >= p) {
    *q-- = ch;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_lltoa(int64_t i, char *p, char *end, char **rest)
{
  char *q;
  if (p >= end) { return GRN_INVALID_ARGUMENT; }
  q = p;
  if (i < 0) {
    *p++ = '-';
    q = p;
    if (i == INT64_MIN) {
      *p++ = (-(i % 10)) + '0';
      i /= 10;
    }
    i = -i;
  }
  do {
    if (p >= end) { return GRN_INVALID_ARGUMENT; }
    *p++ = i % 10 + '0';
  } while ((i /= 10) > 0);
  if (rest) { *rest = p; }
  for (p--; q < p; q++, p--) {
    char t = *q;
    *q = *p;
    *p = t;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ulltoa(uint64_t i, char *p, char *end, char **rest)
{
  char *q;
  if (p >= end) { return GRN_INVALID_ARGUMENT; }
  q = p;
  do {
    if (p >= end) { return GRN_INVALID_ARGUMENT; }
    *p++ = i % 10 + '0';
  } while ((i /= 10) > 0);
  if (rest) { *rest = p; }
  for (p--; q < p; q++, p--) {
    char t = *q;
    *q = *p;
    *p = t;
  }
  return GRN_SUCCESS;
}

#define I2B(i) \
 ("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"[(i) & 0x3f])

#define B2I(b) \
 (((b) < '+' || 'z' < (b)) ? 0xff : "\x3e\xff\xff\xff\x3f\x34\x35\x36\x37\x38\x39\x3a\x3b\x3c\x3d\xff\xff\xff\xff\xff\xff\xff\x00\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b\x0c\x0d\x0e\x0f\x10\x11\x12\x13\x14\x15\x16\x17\x18\x19\xff\xff\xff\xff\xff\xff\x1a\x1b\x1c\x1d\x1e\x1f\x20\x21\x22\x23\x24\x25\x26\x27\x28\x29\x2a\x2b\x2c\x2d\x2e\x2f\x30\x31\x32\x33"[(b) - '+'])

#define MASK 0x34d34d34

char *
grn_itob(grn_id id, char *p)
{
  id ^= MASK;
  *p++ = I2B(id >> 24);
  *p++ = I2B(id >> 18);
  *p++ = I2B(id >> 12);
  *p++ = I2B(id >> 6);
  *p++ = I2B(id);
  return p;
}

grn_id
grn_btoi(char *b)
{
  uint8_t i;
  grn_id id = 0;
  int len = 5;
  while (len--) {
    char c = *b++;
    if ((i = B2I(c)) == 0xff) { return 0; }
    id = (id << 6) + i;
  }
  return id ^ MASK;
}

#define I2B32H(i) ("0123456789ABCDEFGHIJKLMNOPQRSTUV"[(i) & 0x1f])

char *
grn_lltob32h(int64_t i, char *p)
{
  uint64_t u = (uint64_t)i + 0x8000000000000000ULL;
  *p++ = I2B32H(u >> 60);
  *p++ = I2B32H(u >> 55);
  *p++ = I2B32H(u >> 50);
  *p++ = I2B32H(u >> 45);
  *p++ = I2B32H(u >> 40);
  *p++ = I2B32H(u >> 35);
  *p++ = I2B32H(u >> 30);
  *p++ = I2B32H(u >> 25);
  *p++ = I2B32H(u >> 20);
  *p++ = I2B32H(u >> 15);
  *p++ = I2B32H(u >> 10);
  *p++ = I2B32H(u >> 5);
  *p++ = I2B32H(u);
  return p;
}

char *
grn_ulltob32h(uint64_t i, char *p)
{
  char lb = (i >> 59) & 0x10;
  i += 0x8000000000000000ULL;
  *p++ = lb + I2B32H(i >> 60);
  *p++ = I2B32H(i >> 55);
  *p++ = I2B32H(i >> 50);
  *p++ = I2B32H(i >> 45);
  *p++ = I2B32H(i >> 40);
  *p++ = I2B32H(i >> 35);
  *p++ = I2B32H(i >> 30);
  *p++ = I2B32H(i >> 25);
  *p++ = I2B32H(i >> 20);
  *p++ = I2B32H(i >> 15);
  *p++ = I2B32H(i >> 10);
  *p++ = I2B32H(i >> 5);
  *p++ = I2B32H(i);
  return p;
}

grn_rc
grn_aton(grn_ctx *ctx, const char *p, const char *end, const char **rest,
         grn_obj *res)
{
  if (*p == '+') {
    p++;
  }

  switch (*p) {
  case '-' :
  case '0' : case '1' : case '2' : case '3' : case '4' :
  case '5' : case '6' : case '7' : case '8' : case '9' :
    {
      int64_t int64;
      char rest_char;
      int64 = grn_atoll(p, end, rest);
      rest_char = **rest;
      if (end == *rest) {
        if ((int64_t)INT32_MIN <= int64 && int64 <= (int64_t)INT32_MAX) {
          grn_obj_reinit(ctx, res, GRN_DB_INT32, 0);
          GRN_INT32_SET(ctx, res, int64);
        } else if ((int64_t)INT32_MAX < int64 && int64 <= (int64_t)UINT32_MAX) {
          grn_obj_reinit(ctx, res, GRN_DB_UINT32, 0);
          GRN_UINT32_SET(ctx, res, int64);
        } else {
          grn_obj_reinit(ctx, res, GRN_DB_INT64, 0);
          GRN_INT64_SET(ctx, res, int64);
        }
      } else {
        if (*p != '-' && rest_char >= '0' && rest_char <= '9') {
          uint64_t uint64 = grn_atoull(p, end, rest);
          if (end == *rest) {
            grn_obj_reinit(ctx, res, GRN_DB_UINT64, 0);
            GRN_UINT64_SET(ctx, res, uint64);
          }
        }
        if (end != *rest) {
          if (rest_char == '.' || rest_char == 'e' || rest_char == 'E' ||
              (rest_char >= '0' && rest_char <= '9')) {
            char *rest_float;
            double d;
            errno = 0;
            d = strtod(p, &rest_float);
            if (!errno && rest_float == end) {
              grn_obj_reinit(ctx, res, GRN_DB_FLOAT, 0);
              GRN_FLOAT_SET(ctx, res, d);
              *rest = rest_float;
            } else {
              return GRN_INVALID_ARGUMENT;
            }
          }
        }
      }
    }
    break;
  default :
    return GRN_INVALID_ARGUMENT;
  }

  return GRN_SUCCESS;
}

int
grn_str_tok(const char *str, size_t str_len, char delim, const char **tokbuf, int buf_size, const char **rest)
{
  const char **tok = tokbuf, **tok_end = tokbuf + buf_size;
  if (buf_size > 0) {
    const char *str_end = str + str_len;
    for (;;str++) {
      if (str == str_end) {
        *tok++ = str;
        break;
      }
      if (delim == *str) {
        // *str = '\0';
        *tok++ = str;
        if (tok == tok_end) { break; }
      }
    }
  }
  if (rest) { *rest = str; }
  return tok - tokbuf;
}

inline static int
op_getopt_flag(int *flags, const grn_str_getopt_opt *o,
               int argc, char * const argv[], int i, const char *optvalue)
{
  switch (o->op) {
    case GETOPT_OP_NONE:
      break;
    case GETOPT_OP_ON:
      *flags |= o->flag;
      break;
    case GETOPT_OP_OFF:
      *flags &= ~o->flag;
      break;
    case GETOPT_OP_UPDATE:
      *flags = o->flag;
      break;
    default:
      return i;
  }
  if (o->arg) {
    if (optvalue) {
      *o->arg = (char *)optvalue;
    } else if (++i < argc) {
      *o->arg = argv[i];
    } else {
      return -1;
    }
  }
  return i;
}

int
grn_str_getopt(int argc, char * const argv[], const grn_str_getopt_opt *opts,
               int *flags)
{
  int i;
  for (i = 1; i < argc; i++) {
    const char * v = argv[i];
    if (*v == '-') {
      const grn_str_getopt_opt *o;
      int found;
      if (*++v == '-') {
        const char *eq;
        size_t len;
        found = 0;
        v++;
        for (eq = v; *eq != '\0' && *eq != '='; eq++) {}
        len = eq - v;
        for (o = opts; o->opt != '\0' || o->longopt != NULL; o++) {
          if (o->longopt && strlen(o->longopt) == len &&
              !memcmp(v, o->longopt, len)) {
            i = op_getopt_flag(flags, o, argc, argv, i,
                               (*eq == '\0' ? NULL : eq + 1));
            if (i < 0) {
              fprintf(stderr, "%s: option '--%s' needs argument.\n", argv[0], o->longopt);
              return -1;
            }
            found = 1;
            break;
          }
        }
        if (!found) { goto exit; }
      } else {
        const char *p;
        for (p = v; *p; p++) {
          found = 0;
          for (o = opts; o->opt != '\0' || o->longopt != NULL; o++) {
            if (o->opt && *p == o->opt) {
              i = op_getopt_flag(flags, o, argc, argv, i, NULL);
              if (i < 0) {
                fprintf(stderr, "%s: option '-%c' needs argument.\n", argv[0], *p);
                return -1;
              }
              found = 1;
              break;
            }
          }
          if (!found) { goto exit; }
        }
      }
    } else {
      break;
    }
  }
  return i;
exit:
  fprintf(stderr, "%s: cannot recognize option '%s'.\n", argv[0], argv[i]);
  return -1;
}

#define UNIT_SIZE (1 << 12)
#define UNIT_MASK (UNIT_SIZE - 1)

int grn_bulk_margin_size = 0;

grn_rc
grn_bulk_resize(grn_ctx *ctx, grn_obj *buf, unsigned int newsize)
{
  char *head;
  unsigned int rounded_newsize;
  newsize += grn_bulk_margin_size + 1;
  if (GRN_BULK_OUTP(buf)) {
    rounded_newsize = (newsize + (UNIT_MASK)) & ~UNIT_MASK;
    if (rounded_newsize < newsize) { return GRN_NOT_ENOUGH_SPACE; }
    newsize = rounded_newsize;
    head = buf->u.b.head - (buf->u.b.head ? grn_bulk_margin_size : 0);
    if (!(head = GRN_REALLOC(head, newsize))) { return GRN_NO_MEMORY_AVAILABLE; }
    buf->u.b.curr = head + grn_bulk_margin_size + GRN_BULK_VSIZE(buf);
    buf->u.b.head = head + grn_bulk_margin_size;
    buf->u.b.tail = head + newsize;
  } else {
    if (newsize > GRN_BULK_BUFSIZE) {
      rounded_newsize = (newsize + (UNIT_MASK)) & ~UNIT_MASK;
      if (rounded_newsize < newsize) { return GRN_NOT_ENOUGH_SPACE; }
      newsize = rounded_newsize;
      if (!(head = GRN_MALLOC(newsize))) { return GRN_NO_MEMORY_AVAILABLE; }
      grn_memcpy(head, GRN_BULK_HEAD(buf), GRN_BULK_VSIZE(buf));
      buf->u.b.curr = head + grn_bulk_margin_size + GRN_BULK_VSIZE(buf);
      buf->u.b.head = head + grn_bulk_margin_size;
      buf->u.b.tail = head + newsize;
      buf->header.impl_flags |= GRN_OBJ_OUTPLACE;
    }
  }
  return GRN_SUCCESS;
}

grn_rc
grn_bulk_reinit(grn_ctx *ctx, grn_obj *buf, unsigned int size)
{
  GRN_BULK_REWIND(buf);
  return grn_bulk_resize(ctx, buf, size);
}

grn_rc
grn_bulk_write(grn_ctx *ctx, grn_obj *buf, const char *str, unsigned int len)
{
  grn_rc rc = GRN_SUCCESS;
  char *curr;
  if (GRN_BULK_REST(buf) < len) {
    if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_VSIZE(buf) + len))) { return rc; }
  }
  curr = GRN_BULK_CURR(buf);
  if (str)
    grn_memcpy(curr, str, len);
  GRN_BULK_INCR_LEN(buf, len);
  return rc;
}

grn_rc
grn_bulk_write_from(grn_ctx *ctx, grn_obj *bulk,
                    const char *str, unsigned int from, unsigned int len)
{
  grn_rc rc = grn_bulk_truncate(ctx, bulk, from);
  if (!rc) { rc = grn_bulk_write(ctx, bulk, str, len); }
  return rc;
}

grn_rc
grn_bulk_reserve(grn_ctx *ctx, grn_obj *buf, unsigned int len)
{
  grn_rc rc = GRN_SUCCESS;
  if (GRN_BULK_REST(buf) < len) {
    if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_VSIZE(buf) + len))) { return rc; }
  }
  return rc;
}

grn_rc
grn_bulk_space(grn_ctx *ctx, grn_obj *buf, unsigned int len)
{
  grn_rc rc = grn_bulk_reserve(ctx, buf, len);
  if (!rc) {
    GRN_BULK_INCR_LEN(buf, len);
  }
  return rc;
}

static grn_rc
grn_bulk_space_clear(grn_ctx *ctx, grn_obj *buf, unsigned int len)
{
  grn_rc rc = grn_bulk_reserve(ctx, buf, len);
  if (!rc) {
    memset(GRN_BULK_CURR(buf), 0, len);
    GRN_BULK_INCR_LEN(buf, len);
  }
  return rc;
}

grn_rc
grn_bulk_truncate(grn_ctx *ctx, grn_obj *bulk, unsigned int len)
{
  if (GRN_BULK_OUTP(bulk)) {
    if ((bulk->u.b.tail - bulk->u.b.head) < len) {
      return grn_bulk_space_clear(ctx, bulk, len);
    } else {
      bulk->u.b.curr = bulk->u.b.head + len;
    }
  } else {
    if (GRN_BULK_BUFSIZE < len) {
      return grn_bulk_space_clear(ctx, bulk, len);
    } else {
      bulk->header.flags &= ~GRN_BULK_BUFSIZE_MAX;
      bulk->header.flags += len;
    }
  }
  return GRN_SUCCESS;
}

grn_rc
grn_text_itoa(grn_ctx *ctx, grn_obj *buf, int i)
{
  grn_rc rc = GRN_SUCCESS;
  for (;;) {
    char *curr = GRN_BULK_CURR(buf);
    char *tail = GRN_BULK_TAIL(buf);
    if (grn_itoa(i, curr, tail, &curr)) {
      if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_WSIZE(buf) + UNIT_SIZE))) { return rc; }
    } else {
      GRN_BULK_SET_CURR(buf, curr);
      break;
    }
  }
  return rc;
}

grn_rc
grn_text_itoa_padded(grn_ctx *ctx, grn_obj *buf, int i, char ch, unsigned int len)
{
  grn_rc rc = GRN_SUCCESS;
  char *curr;
  if ((rc = grn_bulk_reserve(ctx, buf, len))) { return rc; }
  curr = GRN_BULK_CURR(buf);
  if (!grn_itoa_padded(i, curr, curr + len, ch)) {
    GRN_BULK_SET_CURR(buf, curr + len);
  }
  return rc;
}

grn_rc
grn_text_lltoa(grn_ctx *ctx, grn_obj *buf, long long int i)
{
  grn_rc rc = GRN_SUCCESS;
  for (;;) {
    char *curr = GRN_BULK_CURR(buf);
    char *tail = GRN_BULK_TAIL(buf);
    if (grn_lltoa(i, curr, tail, &curr)) {
      if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_WSIZE(buf) + UNIT_SIZE))) { return rc; }
    } else {
      GRN_BULK_SET_CURR(buf, curr);
      break;
    }
  }
  return rc;
}

grn_rc
grn_text_ulltoa(grn_ctx *ctx, grn_obj *buf, unsigned long long int i)
{
  grn_rc rc = GRN_SUCCESS;
  for (;;) {
    char *curr = GRN_BULK_CURR(buf);
    char *tail = GRN_BULK_TAIL(buf);
    if (grn_ulltoa(i, curr, tail, &curr)) {
      if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_WSIZE(buf) + UNIT_SIZE))) { return rc; }
    } else {
      GRN_BULK_SET_CURR(buf, curr);
      break;
    }
  }
  return rc;
}

inline static void
ftoa_(grn_ctx *ctx, grn_obj *buf, double d)
{
  char *start;
  size_t before_size;
  size_t len;
#define DIGIT_NUMBER 16
#define FIRST_BUFFER_SIZE (DIGIT_NUMBER + 4)
  before_size = GRN_BULK_VSIZE(buf);
  grn_bulk_reserve(ctx, buf, FIRST_BUFFER_SIZE);
  grn_text_printf(ctx, buf, "%#.*g", DIGIT_NUMBER, d);
  len = GRN_BULK_VSIZE(buf) - before_size;
  start = GRN_BULK_CURR(buf) - len;
#undef FIRST_BUFFER_SIZE
#undef DIGIT_NUMBER
  if (start[len - 1] == '.') {
    GRN_TEXT_PUTC(ctx, buf, '0');
  } else {
    char *p, *q;
    start[len] = '\0';
    if ((p = strchr(start, 'e'))) {
      for (q = p; *(q - 2) != '.' && *(q - 1) == '0'; q--) { len--; }
      grn_memmove(q, p, start + len - q);
    } else {
      for (q = start + len; *(q - 2) != '.' && *(q - 1) == '0'; q--) { len--; }
    }
    grn_bulk_truncate(ctx, buf, before_size + len);
  }
}

grn_rc
grn_text_ftoa(grn_ctx *ctx, grn_obj *buf, double d)
{
  grn_rc rc = GRN_SUCCESS;
  if (GRN_BULK_REST(buf) < 32) {
    if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_VSIZE(buf) + 32))) { return rc; }
  }
#ifdef HAVE_FPCLASSIFY
  switch (fpclassify(d)) {
  case FP_NAN :
    GRN_TEXT_PUTS(ctx, buf, "#<nan>");
    break;
  case FP_INFINITE :
    GRN_TEXT_PUTS(ctx, buf, d > 0 ? "#i1/0" : "#i-1/0");
    break;
  default :
    ftoa_(ctx, buf, d);
    break;
  }
#else /* HAVE_FPCLASSIFY */
  if (d == d) {
    if (d != 0 && ((d / 2.0) == d)) {
      GRN_TEXT_PUTS(ctx, buf, d > 0 ? "#i1/0" : "#i-1/0");
    } else {
      ftoa_(ctx, buf, d);
    }
  } else {
    GRN_TEXT_PUTS(ctx, buf, "#<nan>");
  }
#endif /* HAVE_FPCLASSIFY */
  return rc;
}

grn_rc
grn_text_itoh(grn_ctx *ctx, grn_obj *buf, int i, unsigned int len)
{
  grn_rc rc = GRN_SUCCESS;
  if (GRN_BULK_REST(buf) < len) {
    if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_VSIZE(buf) + len))) { return rc; }
  }
  grn_itoh(i, GRN_BULK_CURR(buf), len);
  GRN_BULK_INCR_LEN(buf, len);
  return rc;
}

grn_rc
grn_text_itob(grn_ctx *ctx, grn_obj *buf, grn_id id)
{
  size_t len = 5;
  grn_rc rc = GRN_SUCCESS;
  if (GRN_BULK_REST(buf) < len) {
    if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_VSIZE(buf) + len))) { return rc; }
  }
  grn_itob(id, GRN_BULK_CURR(buf));
  GRN_BULK_INCR_LEN(buf, len);
  return rc;
}

grn_rc
grn_text_lltob32h(grn_ctx *ctx, grn_obj *buf, long long int i)
{
  size_t len = 13;
  grn_rc rc = GRN_SUCCESS;
  if (GRN_BULK_REST(buf) < len) {
    if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_VSIZE(buf) + len))) { return rc; }
  }
  grn_lltob32h(i, GRN_BULK_CURR(buf));
  GRN_BULK_INCR_LEN(buf, len);
  return rc;
}

grn_rc
grn_text_esc(grn_ctx *ctx, grn_obj *buf, const char *s, unsigned int len)
{
  const char *e;
  unsigned int l;
  grn_rc rc = GRN_SUCCESS;

  GRN_TEXT_PUTC(ctx, buf, '"');
  for (e = s + len; s < e; s += l) {
    if (!(l = grn_charlen(ctx, s, e))) { break; }
    if (l == 1) {
      switch (*s) {
      case '"' :
        grn_bulk_write(ctx, buf, "\\\"", 2);
        break;
      case '\\' :
        grn_bulk_write(ctx, buf, "\\\\", 2);
        break;
      case '\b' :
        grn_bulk_write(ctx, buf, "\\b", 2);
        break;
      case '\f' :
        grn_bulk_write(ctx, buf, "\\f", 2);
        break;
      case '\n' :
        grn_bulk_write(ctx, buf, "\\n", 2);
        break;
      case '\r' :
        grn_bulk_write(ctx, buf, "\\r", 2);
        break;
      case '\t' :
        grn_bulk_write(ctx, buf, "\\t", 2);
        break;
      case '\x00': case '\x01': case '\x02': case '\x03': case '\x04': case '\x05':
      case '\x06': case '\x07': case '\x0b': case '\x0e': case '\x0f': case '\x10':
      case '\x11': case '\x12': case '\x13': case '\x14': case '\x15': case '\x16':
      case '\x17': case '\x18': case '\x19': case '\x1a': case '\x1b': case '\x1c':
      case '\x1d': case '\x1e': case '\x1f': case '\x7f':
        if (!(rc = grn_bulk_write(ctx, buf, "\\u", 2))) {
          if ((rc = grn_text_itoh(ctx, buf, *s, 4))) {
            GRN_BULK_INCR_LEN(buf, -2);
            return rc;
          }
        } else {
          return rc;
        }
        break;
      default :
        GRN_TEXT_PUTC(ctx, buf, *s);
      }
    } else if (l == 3) {
      if (*s == '\xe2' && *(s + 1) == '\x80') {
        switch (*(s + 2)) {
        case '\xa8': /* \u2028 */
          grn_bulk_write(ctx, buf, "\\u2028", 6);
          break;
        case '\xa9': /* \u2029 */
          grn_bulk_write(ctx, buf, "\\u2029", 6);
          break;
        default:
          grn_bulk_write(ctx, buf, s, l);
        }
      } else {
        grn_bulk_write(ctx, buf, s, l);
      }
    } else {
      grn_bulk_write(ctx, buf, s, l);
    }
  }
  GRN_TEXT_PUTC(ctx, buf, '"');
  return rc;
}

grn_rc
grn_text_escape_xml(grn_ctx *ctx, grn_obj *buf, const char *s, unsigned int len)
{
  const char *e;
  unsigned int l;
  grn_rc rc = GRN_SUCCESS;

  for (e = s + len; s < e; s += l) {
    if (!(l = grn_charlen(ctx, s, e))) { break; }
    if (l == 1) {
      switch (*s) {
      case '"' :
        grn_bulk_write(ctx, buf, "&quot;", 6);
        break;
      case '<' :
        grn_bulk_write(ctx, buf, "&lt;", 4);
        break;
      case '>' :
        grn_bulk_write(ctx, buf, "&gt;", 4);
        break;
      case '&' :
        grn_bulk_write(ctx, buf, "&amp;", 5);
        break;
      default :
        GRN_TEXT_PUTC(ctx, buf, *s);
      }
    } else {
      grn_bulk_write(ctx, buf, s, l);
    }
  }
  return rc;
}

#define TOK_ESC                        (0x80)

const char *
grn_text_unesc_tok(grn_ctx *ctx, grn_obj *buf, const char *s, const char *e, char *tok_type)
{
  const char *p;
  unsigned int len;
  uint8_t stat = GRN_TOK_VOID;
  for (p = s; p < e; p += len) {
    if (!(len = grn_charlen(ctx, p, e))) {
      p = e;
      stat &= ~TOK_ESC;
      goto exit;
    }
    switch (stat) {
    case GRN_TOK_VOID :
      if (*p == ' ') { continue; }
      switch (*p) {
      case '"' :
        stat = GRN_TOK_STRING;
        break;
      case '\'' :
        stat = GRN_TOK_QUOTE;
        break;
      case ')' :
      case '(' :
        GRN_TEXT_PUT(ctx, buf, p, len);
        p += len;
        stat = GRN_TOK_SYMBOL;
        goto exit;
      case '\\' :
        stat = GRN_TOK_SYMBOL|TOK_ESC;
        break;
      default :
        stat = GRN_TOK_SYMBOL;
        GRN_TEXT_PUT(ctx, buf, p, len);
        break;
      }
      break;
    case GRN_TOK_SYMBOL :
      if (*p == ' ') { goto exit; }
      switch (*p) {
      case '\'' :
      case '"' :
      case ')' :
      case '(' :
        goto exit;
      case '\\' :
        stat |= TOK_ESC;
        break;
      default :
        GRN_TEXT_PUT(ctx, buf, p, len);
        break;
      }
      break;
    case GRN_TOK_STRING :
      switch (*p) {
      case '"' :
        p += len;
        goto exit;
      case '\\' :
        stat |= TOK_ESC;
        break;
      default :
        GRN_TEXT_PUT(ctx, buf, p, len);
        break;
      }
      break;
    case GRN_TOK_QUOTE :
      switch (*p) {
      case '\'' :
        p += len;
        goto exit;
      case '\\' :
        stat |= TOK_ESC;
        break;
      default :
        GRN_TEXT_PUT(ctx, buf, p, len);
        break;
      }
      break;
    case GRN_TOK_SYMBOL|TOK_ESC :
    case GRN_TOK_STRING|TOK_ESC :
    case GRN_TOK_QUOTE|TOK_ESC :
      switch (*p) {
      case 'b' :
        GRN_TEXT_PUTC(ctx, buf, '\b');
        break;
      case 'f' :
        GRN_TEXT_PUTC(ctx, buf, '\f');
        break;
      case 'n' :
        GRN_TEXT_PUTC(ctx, buf, '\n');
        break;
      case 'r' :
        GRN_TEXT_PUTC(ctx, buf, '\r');
        break;
      case 't' :
        GRN_TEXT_PUTC(ctx, buf, '\t');
        break;
      default :
        GRN_TEXT_PUT(ctx, buf, p, len);
        break;
      }
      stat &= ~TOK_ESC;
      break;
    }
  }
exit :
  *tok_type = stat;
  return p;
}

grn_rc
grn_text_benc(grn_ctx *ctx, grn_obj *buf, unsigned int v)
{
  grn_rc rc = GRN_SUCCESS;
  uint8_t *p;
  if (GRN_BULK_REST(buf) < 5) {
    if ((rc = grn_bulk_resize(ctx, buf, GRN_BULK_VSIZE(buf) + 5))) { return rc; }
  }
  p = (uint8_t *)GRN_BULK_CURR(buf);
  GRN_B_ENC(v, p);
  GRN_BULK_SET_CURR(buf, (char *)p);
  return rc;
}

/* 0x00 - 0x7f */
static const int_least8_t urlenc_tbl[] = {
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 0, 1, 1, 1, 1, 1, 1, 0, 0, 0, 1, 1, 0, 0, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1
};

grn_rc
grn_text_urlenc(grn_ctx *ctx, grn_obj *buf, const char *s, unsigned int len)
{
  const char *e, c = '%';
  for (e = s + len; s < e; s++) {
    if ((signed char)*s < 0 || urlenc_tbl[(int)*s]) {
      if (!grn_bulk_write(ctx, buf, &c, 1)) {
        if (grn_text_itoh(ctx, buf, *s, 2)) {
          GRN_BULK_INCR_LEN(buf, -1);
        }
      }
    } else {
      GRN_TEXT_PUTC(ctx, buf, *s);
    }
  }
  return GRN_SUCCESS;
}

static const char *weekdays[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
static const char *months[12] = {
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

grn_rc
grn_text_time2rfc1123(grn_ctx *ctx, grn_obj *bulk, int sec)
{
  time_t tsec;
  struct tm *t;
#ifdef HAVE__GMTIME64_S
  struct tm tm;
  tsec = (time_t)sec;
  t = (gmtime_s(&tm, &tsec) == 0) ? &tm : NULL;
#else /* HAVE__GMTIME64_S */
# ifdef HAVE_GMTIME_R
  struct tm tm;
  tsec = (time_t)sec;
  t = gmtime_r(&tsec, &tm);
# else /* HAVE_GMTIME_R */
  tsec = (time_t)sec;
  t = gmtime(&tsec);
# endif /* HAVE_GMTIME_R */
#endif /* HAVE__GMTIME64_S */
  if (t) {
    GRN_TEXT_SET(ctx, bulk, weekdays[t->tm_wday], 3);
    GRN_TEXT_PUTS(ctx, bulk, ", ");
    grn_text_itoa_padded(ctx, bulk, t->tm_mday, '0', 2);
    GRN_TEXT_PUTS(ctx, bulk, " ");
    GRN_TEXT_PUT(ctx, bulk, months[t->tm_mon], 3);
    GRN_TEXT_PUTS(ctx, bulk, " ");
    grn_text_itoa(ctx, bulk, t->tm_year + 1900);
    GRN_TEXT_PUTS(ctx, bulk, " ");
    grn_text_itoa_padded(ctx, bulk, t->tm_hour, '0', 2);
    GRN_TEXT_PUTS(ctx, bulk, ":");
    grn_text_itoa_padded(ctx, bulk, t->tm_min, '0', 2);
    GRN_TEXT_PUTS(ctx, bulk, ":");
    grn_text_itoa_padded(ctx, bulk, t->tm_sec, '0', 2);
    GRN_TEXT_PUTS(ctx, bulk, " GMT");
  } else {
    GRN_TEXT_SETS(ctx, bulk, "Mon, 16 Mar 1980 20:40:00 GMT");
  }
  return GRN_SUCCESS;
}

grn_rc
grn_text_printf(grn_ctx *ctx, grn_obj *bulk, const char *format, ...)
{
  va_list args;

  va_start(args, format);
  grn_text_vprintf(ctx, bulk, format, args);
  va_end(args);

  return GRN_SUCCESS;
}

grn_rc
grn_text_vprintf(grn_ctx *ctx, grn_obj *bulk, const char *format, va_list args)
{
  grn_bool is_written = GRN_FALSE;
  int written_size;

  {
    int rest_size;
    va_list copied_args;

    rest_size = GRN_BULK_REST(bulk);
    va_copy(copied_args, args);
    written_size = vsnprintf(GRN_BULK_CURR(bulk), rest_size,
                             format, copied_args);
    va_end(copied_args);

    if (0 <= written_size && written_size < rest_size) {
      is_written = GRN_TRUE;
    }
  }

  if (!is_written) {
#ifdef WIN32
# define N_NEW_SIZES 3
    int i;
    int new_sizes[N_NEW_SIZES];

    new_sizes[0] = GRN_BULK_REST(bulk) + strlen(format) * 2;
    new_sizes[1] = new_sizes[0] + 4096;
    new_sizes[2] = new_sizes[0] + 65536;

    for (i = 0; i < N_NEW_SIZES; i++) {
      grn_rc rc;
      int new_size = new_sizes[i];
      va_list copied_args;

      rc = grn_bulk_reserve(ctx, bulk, GRN_BULK_VSIZE(bulk) + new_size);
      if (rc) {
        return rc;
      }
      va_copy(copied_args, args);
      written_size = vsnprintf(GRN_BULK_CURR(bulk), new_size,
                               format, copied_args);
      va_end(copied_args);
      if (written_size != -1) {
        break;
      }
    }
# undef N_NEW_SIZES
#else /* WIN32 */
    grn_rc rc;
    int required_size = written_size + 1; /* "+ 1" for terminate '\0'. */

    rc = grn_bulk_reserve(ctx, bulk, GRN_BULK_VSIZE(bulk) + required_size);
    if (rc) {
      return rc;
    }
    written_size = vsnprintf(GRN_BULK_CURR(bulk), required_size,
                             format, args);
#endif /* WIN32 */
  }

  if (written_size < 0) {
    return GRN_INVALID_ARGUMENT;
  }

  GRN_BULK_INCR_LEN(bulk, written_size);
  return GRN_SUCCESS;
}

grn_rc
grn_bulk_fin(grn_ctx *ctx, grn_obj *buf)
{
  if (!(buf->header.impl_flags & GRN_OBJ_REFER)) {
    if (GRN_BULK_OUTP(buf) && buf->u.b.head) {
      GRN_REALLOC(buf->u.b.head - grn_bulk_margin_size, 0);
    }
  }
  buf->header.flags = 0;
  buf->header.impl_flags &= ~GRN_OBJ_DO_SHALLOW_COPY;
  buf->u.b.head = NULL;
  buf->u.b.curr = NULL;
  buf->u.b.tail = NULL;
  return GRN_SUCCESS;
}

grn_rc
grn_substring(grn_ctx *ctx, char **str, char **str_end, int start, int end, grn_encoding encoding)
{
  int i;
  size_t l;
  char *s = *str, *e = *str_end;
  for (i = 0; s < e; i++, s += l) {
    if (i == start) { *str = s; }
    if (!(l = grn_charlen(ctx, s, e))) {
      return GRN_INVALID_ARGUMENT;
    }
    if (i == end) {
      *str_end = s;
      break;
    }
  }
  return GRN_SUCCESS;
}

static void
grn_text_atoj(grn_ctx *ctx, grn_obj *bulk, grn_obj *obj, grn_id id)
{
  uint32_t vs;
  grn_obj buf;
  if (obj->header.type == GRN_ACCESSOR) {
    grn_accessor *a = (grn_accessor *)obj;
    GRN_TEXT_INIT(&buf, 0);
    for (;;) {
      GRN_BULK_REWIND(&buf);
      switch (a->action) {
      case GRN_ACCESSOR_GET_ID :
        GRN_UINT32_PUT(ctx, &buf, id);
        buf.header.domain = GRN_DB_UINT32;
        break;
      case GRN_ACCESSOR_GET_KEY :
        grn_table_get_key2(ctx, a->obj, id, &buf);
        buf.header.domain = DB_OBJ(a->obj)->header.domain;
        break;
      case GRN_ACCESSOR_GET_VALUE :
        grn_obj_get_value(ctx, a->obj, id, &buf);
        buf.header.domain = GRN_DB_INT32; /* fix me */
        break;
      case GRN_ACCESSOR_GET_SCORE :
        {
          grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
          int32_t int32_score = ri->score;
          GRN_INT32_PUT(ctx, &buf, int32_score);
        }
        buf.header.domain = GRN_DB_INT32;
        break;
      case GRN_ACCESSOR_GET_NSUBRECS :
        {
          grn_rset_recinfo *ri = (grn_rset_recinfo *)grn_obj_get_value_(ctx, a->obj, id, &vs);
          GRN_INT32_PUT(ctx, &buf, ri->n_subrecs);
        }
        buf.header.domain = GRN_DB_INT32;
        break;
      case GRN_ACCESSOR_GET_COLUMN_VALUE :
        if ((a->obj->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) == GRN_OBJ_COLUMN_VECTOR) {
          if (a->next) {
            grn_id *idp;
            grn_obj_get_value(ctx, a->obj, id, &buf);
            idp = (grn_id *)GRN_BULK_HEAD(&buf);
            GRN_TEXT_PUTC(ctx, bulk, '[');
            for (vs = GRN_BULK_VSIZE(&buf) / sizeof(grn_id); vs--; idp++) {
              grn_text_atoj(ctx, bulk, (grn_obj *)a->next, *idp);
              if (vs) { GRN_TEXT_PUTC(ctx, bulk, ','); }
            }
            GRN_TEXT_PUTC(ctx, bulk, ']');
          } else {
            grn_text_atoj(ctx, bulk, a->obj, id);
          }
          goto exit;
        } else {
          grn_obj_get_value(ctx, a->obj, id, &buf);
        }
        break;
      case GRN_ACCESSOR_GET_DB_OBJ :
        /* todo */
        break;
      case GRN_ACCESSOR_LOOKUP :
        /* todo */
        break;
      case GRN_ACCESSOR_FUNCALL :
        /* todo */
        break;
      }
      if (a->next) {
        a = a->next;
        id = *((grn_id *)GRN_BULK_HEAD(&buf));
      } else {
        break;
      }
    }
  } else {
    switch (obj->header.type) {
    case GRN_COLUMN_FIX_SIZE :
      GRN_VALUE_FIX_SIZE_INIT(&buf, 0, DB_OBJ(obj)->range);
      break;
    case GRN_COLUMN_VAR_SIZE :
      if ((obj->header.flags & GRN_OBJ_COLUMN_TYPE_MASK) == GRN_OBJ_COLUMN_VECTOR) {
        grn_obj *range = grn_ctx_at(ctx, DB_OBJ(obj)->range);
        if (range->header.flags & GRN_OBJ_KEY_VAR_SIZE) {
          GRN_VALUE_VAR_SIZE_INIT(&buf, GRN_OBJ_VECTOR, DB_OBJ(obj)->range);
        } else {
          GRN_VALUE_FIX_SIZE_INIT(&buf, GRN_OBJ_VECTOR, DB_OBJ(obj)->range);
        }
      } else {
        GRN_VALUE_VAR_SIZE_INIT(&buf, 0, DB_OBJ(obj)->range);
      }
      break;
    case GRN_COLUMN_INDEX :
      GRN_UINT32_INIT(&buf, 0);
      break;
    default:
      GRN_TEXT_INIT(&buf, 0);
      break;
    }
    grn_obj_get_value(ctx, obj, id, &buf);
  }
  grn_text_otoj(ctx, bulk, &buf, NULL);
exit :
  grn_obj_close(ctx, &buf);
}

grn_rc
grn_text_otoj(grn_ctx *ctx, grn_obj *bulk, grn_obj *obj, grn_obj_format *format)
{
  grn_obj buf;
  GRN_TEXT_INIT(&buf, 0);
  switch (obj->header.type) {
  case GRN_BULK :
    switch (obj->header.domain) {
    case GRN_DB_VOID :
    case GRN_DB_SHORT_TEXT :
    case GRN_DB_TEXT :
    case GRN_DB_LONG_TEXT :
      grn_text_esc(ctx, bulk, GRN_BULK_HEAD(obj), GRN_BULK_VSIZE(obj));
      break;
    case GRN_DB_BOOL :
      if (*((unsigned char *)GRN_BULK_HEAD(obj))) {
        GRN_TEXT_PUTS(ctx, bulk, "true");
      } else {
        GRN_TEXT_PUTS(ctx, bulk, "false");
      }
      break;
    case GRN_DB_INT8 :
      grn_text_itoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_INT8_VALUE(obj) : 0);
      break;
    case GRN_DB_UINT8 :
      grn_text_lltoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_UINT8_VALUE(obj) : 0);
      break;
    case GRN_DB_INT16 :
      grn_text_itoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_INT16_VALUE(obj) : 0);
      break;
    case GRN_DB_UINT16 :
      grn_text_lltoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_UINT16_VALUE(obj) : 0);
      break;
    case GRN_DB_INT32 :
      grn_text_itoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_INT32_VALUE(obj) : 0);
      break;
    case GRN_DB_UINT32 :
      grn_text_lltoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_UINT32_VALUE(obj) : 0);
      break;
    case GRN_DB_INT64 :
      grn_text_lltoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_INT64_VALUE(obj) : 0);
      break;
    case GRN_DB_UINT64 :
      grn_text_ulltoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_UINT64_VALUE(obj) : 0);
      break;
    case GRN_DB_FLOAT :
      grn_text_ftoa(ctx, bulk, GRN_BULK_VSIZE(obj) ? GRN_FLOAT_VALUE(obj) : 0);
      break;
    case GRN_DB_TIME :
      {
        double dv = *((int64_t *)GRN_BULK_HEAD(obj));
        dv /= 1000000.0;
        grn_text_ftoa(ctx, bulk, dv);
      }
      break;
    case GRN_DB_TOKYO_GEO_POINT :
    case GRN_DB_WGS84_GEO_POINT :
      if (GRN_BULK_VSIZE(obj) == sizeof(grn_geo_point)) {
        grn_geo_point *gp = (grn_geo_point *)GRN_BULK_HEAD(obj);
        GRN_TEXT_PUTC(ctx, bulk, '"');
        grn_text_itoa(ctx, bulk, gp->latitude);
        GRN_TEXT_PUTC(ctx, bulk, 'x');
        grn_text_itoa(ctx, bulk, gp->longitude);
        GRN_TEXT_PUTC(ctx, bulk, '"');
      } else {
        GRN_TEXT_PUTS(ctx, bulk, "\"\"");
      }
      break;
    default :
      if (format) {
        int j;
        int ncolumns = GRN_BULK_VSIZE(&format->columns)/sizeof(grn_obj *);
        grn_id id = GRN_RECORD_VALUE(obj);
        grn_obj **columns = (grn_obj **)GRN_BULK_HEAD(&format->columns);
        if (format->flags & GRN_OBJ_FORMAT_WITH_COLUMN_NAMES) {
          GRN_TEXT_PUTS(ctx, bulk, "[");
          for (j = 0; j < ncolumns; j++) {
            grn_id range_id;
            if (j) { GRN_TEXT_PUTC(ctx, bulk, ','); }
            GRN_TEXT_PUTS(ctx, bulk, "[");
            GRN_BULK_REWIND(&buf);
            grn_column_name_(ctx, columns[j], &buf);
            grn_text_otoj(ctx, bulk, &buf, NULL);
            GRN_TEXT_PUTC(ctx, bulk, ',');
            /* column range */
            range_id = grn_obj_get_range(ctx, columns[j]);
            if (range_id == GRN_ID_NIL) {
              GRN_TEXT_PUTS(ctx, bulk, "null");
            } else {
              int name_len;
              grn_obj *range_obj;
              char name_buf[GRN_TABLE_MAX_KEY_SIZE];

              range_obj = grn_ctx_at(ctx, range_id);
              name_len = grn_obj_name(ctx, range_obj, name_buf,
                                      GRN_TABLE_MAX_KEY_SIZE);
              GRN_BULK_REWIND(&buf);
              GRN_TEXT_PUT(ctx, &buf, name_buf, name_len);
              grn_text_otoj(ctx, bulk, &buf, NULL);
            }
            GRN_TEXT_PUTS(ctx, bulk, "]");
          }
          GRN_TEXT_PUTS(ctx, bulk, "],");
        }
        GRN_TEXT_PUTC(ctx, bulk, '[');
        for (j = 0; j < ncolumns; j++) {
          if (j) { GRN_TEXT_PUTC(ctx, bulk, ','); }
          grn_text_atoj(ctx, bulk, columns[j], id);
        }
        GRN_TEXT_PUTC(ctx, bulk, ']');
      } else {
        if (GRN_BULK_VSIZE(obj) == 0) {
          GRN_TEXT_PUTS(ctx, bulk, "null");
        } else {
          grn_obj *table = grn_ctx_at(ctx, obj->header.domain);
          grn_id id = GRN_RECORD_VALUE(obj);
          if (table && table->header.type != GRN_TABLE_NO_KEY) {
            /* todo : temporal patch. grn_table_at() is kinda costful... */
            if (grn_table_at(ctx, table, id)) {
              grn_obj *accessor = grn_obj_column(ctx, table,
                                                 GRN_COLUMN_NAME_KEY,
                                                 GRN_COLUMN_NAME_KEY_LEN);
              if (accessor) {
                grn_obj_get_value(ctx, accessor, id, &buf);
                grn_obj_unlink(ctx, accessor);
              }
            }
            grn_text_otoj(ctx, bulk, &buf, format);
          } else {
            grn_text_lltoa(ctx, bulk, id);
          }
        }
      }
    }
    break;
  case GRN_UVECTOR :
    if (format) {
      if (format->flags & GRN_OBJ_FORMAT_WITH_WEIGHT) {
        int i, n;
        grn_obj *domain;

        n = grn_uvector_size(ctx, obj);
        domain = grn_ctx_at(ctx, obj->header.domain);
        GRN_TEXT_PUTS(ctx, bulk, "{");
        for (i = 0; i < n; i++) {
          grn_id id;
          unsigned int weight;

          if (i > 0) {
            GRN_TEXT_PUTC(ctx, bulk, ',');
          }
          id = grn_uvector_get_element(ctx, obj, i, &weight);
          if (domain) {
            if (domain->header.type == GRN_TABLE_NO_KEY) {
              GRN_TEXT_PUTC(ctx, bulk, '"');
              grn_text_ulltoa(ctx, bulk, id);
              GRN_TEXT_PUTC(ctx, bulk, '"');
            } else {
              GRN_BULK_REWIND(&buf);
              grn_table_get_key2(ctx, domain, id, &buf);
              grn_text_otoj(ctx, bulk, &buf, NULL);
            }
          } else {
            GRN_TEXT_PUTC(ctx, bulk, '"');
            grn_text_ulltoa(ctx, bulk, id);
            GRN_TEXT_PUTC(ctx, bulk, '"');
          }
          GRN_TEXT_PUTC(ctx, bulk, ':');
          grn_text_ulltoa(ctx, bulk, weight);
        }
        GRN_TEXT_PUTS(ctx, bulk, "}");
      } else {
        /* TODO: Does we still need this code? If we don't need this, we should
           remove this. */
        int i, j;
        grn_id *v = (grn_id *)GRN_BULK_HEAD(obj), *ve = (grn_id *)GRN_BULK_CURR(obj);
        int ncolumns = GRN_BULK_VSIZE(&format->columns) / sizeof(grn_obj *);
        grn_obj **columns = (grn_obj **)GRN_BULK_HEAD(&format->columns);
        GRN_TEXT_PUTS(ctx, bulk, "[[");
        grn_text_itoa(ctx, bulk, ve - v);
        GRN_TEXT_PUTC(ctx, bulk, ']');
        if (v < ve) {
          if (format->flags & GRN_OBJ_FORMAT_WITH_COLUMN_NAMES) {
            GRN_TEXT_PUTS(ctx, bulk, ",[");
            for (j = 0; j < ncolumns; j++) {
              grn_id range_id;
              if (j) { GRN_TEXT_PUTC(ctx, bulk, ','); }
              GRN_TEXT_PUTS(ctx, bulk, "[");
              GRN_BULK_REWIND(&buf);
              grn_column_name_(ctx, columns[j], &buf);
              grn_text_otoj(ctx, bulk, &buf, NULL);
              GRN_TEXT_PUTC(ctx, bulk, ',');
              /* column range */
              range_id = grn_obj_get_range(ctx, columns[j]);
              if (range_id == GRN_ID_NIL) {
                GRN_TEXT_PUTS(ctx, bulk, "null");
              } else {
                int name_len;
                grn_obj *range_obj;
                char name_buf[GRN_TABLE_MAX_KEY_SIZE];

                range_obj = grn_ctx_at(ctx, range_id);
                name_len = grn_obj_name(ctx, range_obj, name_buf,
                                        GRN_TABLE_MAX_KEY_SIZE);
                GRN_BULK_REWIND(&buf);
                GRN_TEXT_PUT(ctx, &buf, name_buf, name_len);
                grn_text_otoj(ctx, bulk, &buf, NULL);
              }
              GRN_TEXT_PUTS(ctx, bulk, "]");
            }
            GRN_TEXT_PUTC(ctx, bulk, ']');
          }
          for (i = 0;; i++) {
            GRN_TEXT_PUTS(ctx, bulk, ",[");
            for (j = 0; j < ncolumns; j++) {
              if (j) { GRN_TEXT_PUTC(ctx, bulk, ','); }
              GRN_BULK_REWIND(&buf);
              grn_obj_get_value(ctx, columns[j], *v, &buf);
              grn_text_otoj(ctx, bulk, &buf, NULL);
            }
            GRN_TEXT_PUTC(ctx, bulk, ']');
            v++;
            if (v < ve) {
              GRN_TEXT_PUTC(ctx, bulk, ',');
            } else {
              break;
            }
          }
        }
        GRN_TEXT_PUTC(ctx, bulk, ']');
      }
    } else {
      grn_obj *range = grn_ctx_at(ctx, obj->header.domain);
      if (range && range->header.type == GRN_TYPE) {
        grn_id value_size = ((struct _grn_type *)range)->obj.range;
        char *v = (char *)GRN_BULK_HEAD(obj),
             *ve = (char *)GRN_BULK_CURR(obj);
        GRN_TEXT_PUTC(ctx, bulk, '[');
        if (v < ve) {
          for (;;) {
            grn_obj value;
            GRN_OBJ_INIT(&value, GRN_BULK, 0, obj->header.domain);
            grn_bulk_write_from(ctx, &value, v, 0, value_size);
            grn_text_otoj(ctx, bulk, &value, NULL);

            v += value_size;
            if (v < ve) {
              GRN_TEXT_PUTC(ctx, bulk, ',');
            } else {
              break;
            }
          }
        }
        GRN_TEXT_PUTC(ctx, bulk, ']');
      } else {
        grn_id *v = (grn_id *)GRN_BULK_HEAD(obj),
               *ve = (grn_id *)GRN_BULK_CURR(obj);
        GRN_TEXT_PUTC(ctx, bulk, '[');
        if (v < ve) {
          for (;;) {
            if (range->header.type != GRN_TABLE_NO_KEY) {
              grn_obj key;
              GRN_OBJ_INIT(&key, GRN_BULK, 0, range->header.domain);
              grn_table_get_key2(ctx, range, *v, &key);
              grn_text_otoj(ctx, bulk, &key, NULL);
              GRN_OBJ_FIN(ctx, &key);
            } else {
              grn_text_lltoa(ctx, bulk, *v);
            }
            v++;
            if (v < ve) {
              GRN_TEXT_PUTC(ctx, bulk, ',');
            } else {
              break;
            }
          }
        }
        GRN_TEXT_PUTC(ctx, bulk, ']');
      }
    }
    break;
  case GRN_VECTOR :
    if (obj->header.domain == GRN_DB_VOID) {
      ERR(GRN_INVALID_ARGUMENT, "invalid obj->header.domain");
    } else {
      unsigned int i, n;
      grn_obj value;
      grn_obj weight;
      grn_bool with_weight;

      GRN_VOID_INIT(&value);
      GRN_UINT32_INIT(&weight, 0);
      with_weight = (format && format->flags & GRN_OBJ_FORMAT_WITH_WEIGHT);
      n = grn_vector_size(ctx, obj);
      if (with_weight) {
        GRN_TEXT_PUTC(ctx, bulk, '{');
      } else {
        GRN_TEXT_PUTC(ctx, bulk, '[');
      }
      for (i = 0; i < n; i++) {
        const char *_value;
        unsigned int _weight, length;
        grn_id domain;
        if (i) { GRN_TEXT_PUTC(ctx, bulk, ','); }

        length = grn_vector_get_element(ctx, obj, i,
                                        &_value, &_weight, &domain);
        if (domain != GRN_DB_VOID) {
          grn_obj_reinit(ctx, &value, domain, 0);
        } else {
          grn_obj_reinit(ctx, &value, obj->header.domain, 0);
        }
        grn_bulk_write(ctx, &value, _value, length);
        grn_text_otoj(ctx, bulk, &value, NULL);
        if (with_weight) {
          GRN_TEXT_PUTC(ctx, bulk, ':');
          GRN_UINT32_SET(ctx, &weight, _weight);
          grn_text_otoj(ctx, bulk, &weight, NULL);
        }
      }
      if (with_weight) {
        GRN_TEXT_PUTC(ctx, bulk, '}');
      } else {
        GRN_TEXT_PUTC(ctx, bulk, ']');
      }
      GRN_OBJ_FIN(ctx, &value);
      GRN_OBJ_FIN(ctx, &weight);
    }
    break;
  case GRN_PVECTOR :
    if (format) {
      ERR(GRN_FUNCTION_NOT_IMPLEMENTED,
          "cannot print GRN_PVECTOR using grn_obj_format");
    } else {
      unsigned int i, n;
      GRN_TEXT_PUTC(ctx, bulk, '[');
      n = GRN_BULK_VSIZE(obj) / sizeof(grn_obj *);
      for (i = 0; i < n; i++) {
        grn_obj *value;

        if (i) { GRN_TEXT_PUTC(ctx, bulk, ','); }
        value = GRN_PTR_VALUE_AT(obj, i);
        grn_text_otoj(ctx, bulk, value, NULL);
      }
      GRN_TEXT_PUTC(ctx, bulk, ']');
    }
    break;
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_NO_KEY :
    if (format) {
      int i, j;
      int ncolumns = GRN_BULK_VSIZE(&format->columns)/sizeof(grn_obj *);
      grn_obj **columns = (grn_obj **)GRN_BULK_HEAD(&format->columns);
      grn_table_cursor *tc = grn_table_cursor_open(ctx, obj, NULL, 0, NULL, 0,
                                                   format->offset, format->limit,
                                                   GRN_CURSOR_ASCENDING);
      if (!tc) { ERRCLR(ctx); }
      GRN_TEXT_PUTS(ctx, bulk, "[[");
      grn_text_itoa(ctx, bulk, format->nhits);
      GRN_TEXT_PUTC(ctx, bulk, ']');
      if (format->flags & GRN_OBJ_FORMAT_WITH_COLUMN_NAMES) {
        GRN_TEXT_PUTS(ctx, bulk, ",[");
        for (j = 0; j < ncolumns; j++) {
          grn_id range_id;
          if (j) { GRN_TEXT_PUTC(ctx, bulk, ','); }
          GRN_TEXT_PUTS(ctx, bulk, "[");
          GRN_BULK_REWIND(&buf);
          grn_column_name_(ctx, columns[j], &buf);
          grn_text_otoj(ctx, bulk, &buf, NULL);
          GRN_TEXT_PUTC(ctx, bulk, ',');
          /* column range */
          range_id = grn_obj_get_range(ctx, columns[j]);
          if (range_id == GRN_ID_NIL) {
            GRN_TEXT_PUTS(ctx, bulk, "null");
          } else {
            int name_len;
            grn_obj *range_obj;
            char name_buf[GRN_TABLE_MAX_KEY_SIZE];

            range_obj = grn_ctx_at(ctx, range_id);
            name_len = grn_obj_name(ctx, range_obj, name_buf,
                                    GRN_TABLE_MAX_KEY_SIZE);
            GRN_BULK_REWIND(&buf);
            GRN_TEXT_PUT(ctx, &buf, name_buf, name_len);
            grn_text_otoj(ctx, bulk, &buf, NULL);
          }
          GRN_TEXT_PUTS(ctx, bulk, "]");
        }
        GRN_TEXT_PUTC(ctx, bulk, ']');
      }
      if (tc) {
        grn_id id;
        for (i = 0; (id = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL; i++) {
          GRN_TEXT_PUTS(ctx, bulk, ",[");
          for (j = 0; j < ncolumns; j++) {
            if (j) { GRN_TEXT_PUTC(ctx, bulk, ','); }
            grn_text_atoj(ctx, bulk, columns[j], id);
          }
          GRN_TEXT_PUTC(ctx, bulk, ']');
        }
        grn_table_cursor_close(ctx, tc);
      }
      GRN_TEXT_PUTC(ctx, bulk, ']');
    } else {
      int i;
      grn_id id;
      grn_obj *column = grn_obj_column(ctx, obj,
                                       GRN_COLUMN_NAME_KEY,
                                       GRN_COLUMN_NAME_KEY_LEN);
      grn_table_cursor *tc = grn_table_cursor_open(ctx, obj, NULL, 0, NULL, 0,
                                                   0, -1, GRN_CURSOR_ASCENDING);
      GRN_TEXT_PUTC(ctx, bulk, '[');
      if (tc) {
        for (i = 0; (id = grn_table_cursor_next(ctx, tc)) != GRN_ID_NIL; i++) {
          if (i) { GRN_TEXT_PUTC(ctx, bulk, ','); }
          GRN_BULK_REWIND(&buf);
          grn_obj_get_value(ctx, column, id, &buf);
          grn_text_esc(ctx, bulk, GRN_BULK_HEAD(&buf), GRN_BULK_VSIZE(&buf));
        }
        grn_table_cursor_close(ctx, tc);
      }
      GRN_TEXT_PUTC(ctx, bulk, ']');
      grn_obj_unlink(ctx, column);
    }
    break;
  }
  grn_obj_close(ctx, &buf);
  return GRN_SUCCESS;
}

const char *
grn_text_urldec(grn_ctx *ctx, grn_obj *buf, const char *p, const char *e, char d)
{
  while (p < e) {
    if (*p == d) {
      p++; break;
    } else if (*p == '%' && p + 3 <= e) {
      const char *r;
      unsigned int c = grn_htoui(p + 1, p + 3, &r);
      if (p + 3 == r) {
        GRN_TEXT_PUTC(ctx, buf, c);
        p += 3;
      } else {
        GRN_LOG(ctx, GRN_LOG_NOTICE, "invalid %% sequence (%c%c)", p[1], p[2]);
        GRN_TEXT_PUTC(ctx, buf, '%');
        p += 1;
      }
    } else {
      GRN_TEXT_PUTC(ctx, buf, *p);
      p++;
    }
  }
  return p;
}

const char *
grn_text_cgidec(grn_ctx *ctx, grn_obj *buf, const char *p, const char *e,
                const char *delimiters)
{
  while (p < e) {
    grn_bool found_delimiter = GRN_FALSE;
    const char *delimiter;
    for (delimiter = delimiters; *delimiter; delimiter++) {
      if (*p == *delimiter) {
        found_delimiter = GRN_TRUE;
        break;
      }
    }
    if (found_delimiter) {
      p++;
      break;
    }

    if (*p == '+') {
      GRN_TEXT_PUTC(ctx, buf, ' ');
      p++;
    } else if (*p == '%' && p + 3 <= e) {
      const char *r;
      unsigned int c = grn_htoui(p + 1, p + 3, &r);
      if (p + 3 == r) {
        GRN_TEXT_PUTC(ctx, buf, c);
        p += 3;
      } else {
        GRN_LOG(ctx, GRN_LOG_NOTICE, "invalid %% sequence (%c%c)", p[1], p[2]);
        GRN_TEXT_PUTC(ctx, buf, '%');
        p += 1;
      }
    } else {
      GRN_TEXT_PUTC(ctx, buf, *p);
      p++;
    }
  }
  return p;
}

void
grn_str_url_path_normalize(grn_ctx *ctx, const char *path, size_t path_len,
                           char *buf, size_t buf_len)
{
  char *b = buf, *be = buf + buf_len - 1;
  const char *p = path, *pe = path + path_len, *pc;

  if (buf_len < 2) { return; }

  while (p < pe) {
    for (pc = p; pc < pe && *pc != '/'; pc++) {}
    if (*p == '.') {
      if (pc == p + 2 && *(p + 1) == '.') {
        /* '..' */
        if (b - buf >= 2) {
          for (b -= 2; *b != '/' && b >= buf; b--) {}
        }
        if (*b == '/') {
          b++;
          ERR(GRN_INVALID_ARGUMENT, "parent path doesn't exist.");
        }
        p = pc + 1;
        continue;
      } else if (pc == p + 1) {
        /* '.' */
        p = pc + 1;
        continue;
      }
    }
    if (be - b >= pc - p) {
      grn_memcpy(b, p, (pc - p));
      b += pc - p;
      p = pc;
      if (p < pe && *pc == '/' && be > b) {
        *b++ = '/';
        p++;
      }
    }
  }
  *b = '\0';
}

grn_bool
grn_bulk_is_zero(grn_ctx *ctx, grn_obj *obj)
{
  const char *v = GRN_BULK_HEAD(obj);
  unsigned int s = GRN_BULK_VSIZE(obj);
  for (; s; s--, v++) {
    if (*v) { return GRN_FALSE; }
  }
  return GRN_TRUE;
}

