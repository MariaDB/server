/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2012 Brazil

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

#include <string.h>

#include "grn_normalizer.h"
#include "grn_string.h"
#include "grn_nfkc.h"
#include <groonga/normalizer.h>
#include <groonga/tokenizer.h>

grn_rc
grn_normalizer_register(grn_ctx *ctx,
                        const char *name_ptr,
                        int name_length,
                        grn_proc_func *init,
                        grn_proc_func *next,
                        grn_proc_func *fin)
{
  grn_expr_var vars[] = {
    { NULL, 0 }
  };
  GRN_PTR_INIT(&vars[0].value, 0, GRN_ID_NIL);

  if (name_length < 0) {
    name_length = strlen(name_ptr);
  }

  {
    grn_obj * const normalizer = grn_proc_create(ctx,
                                                 name_ptr, name_length,
                                                 GRN_PROC_NORMALIZER,
                                                 init, next, fin,
                                                 sizeof(*vars) / sizeof(vars),
                                                 vars);
    if (!normalizer) {
      GRN_PLUGIN_ERROR(ctx, GRN_NORMALIZER_ERROR,
                       "[normalizer] failed to register normalizer: <%.*s>",
                       name_length, name_ptr);
      return ctx->rc;
    }
  }
  return GRN_SUCCESS;
}

grn_rc
grn_normalizer_init(void)
{
  return GRN_SUCCESS;
}

grn_rc
grn_normalizer_fin(void)
{
  return GRN_SUCCESS;
}

static unsigned char symbol[] = {
  ',', '.', 0, ':', ';', '?', '!', 0, 0, 0, '`', 0, '^', '~', '_', 0, 0, 0,
  0, 0, 0, 0, 0, 0, 0, '-', '-', '/', '\\', 0, 0, '|', 0, 0, 0, '\'', 0,
  '"', '(', ')', 0, 0, '[', ']', '{', '}', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  '+', '-', 0, 0, 0, '=', 0, '<', '>', 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
  '$', 0, 0, '%', '#', '&', '*', '@', 0, 0, 0, 0, 0, 0, 0, 0
};

inline static grn_obj *
eucjp_normalize(grn_ctx *ctx, grn_string *nstr)
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
  size_t size = nstr->original_length_in_bytes, length = 0;
  int removeblankp = nstr->flags & GRN_STRING_REMOVE_BLANK;
  if (!(nstr->normalized = GRN_MALLOC(size * 2 + 1))) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[string][eucjp] failed to allocate normalized text space");
    return NULL;
  }
  d0 = (unsigned char *) nstr->normalized;
  if (nstr->flags & GRN_STRING_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * 2 * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->normalized);
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][eucjp] failed to allocate checks space");
      return NULL;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STRING_WITH_TYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->normalized);
      nstr->checks = NULL;
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][eucjp] failed to allocate character types space");
      return NULL;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->original + size;
  for (s = s_ = (unsigned char *) nstr->original, d = d_ = d0; s < e; s++) {
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
                if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
                continue;
              } else {
                *d = ' ';
                ctype = GRN_CHAR_BLANK|GRN_CHAR_SYMBOL;
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
        if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
        continue;
      case 2 :
        if (c == 0x20) {
          if (removeblankp) {
            if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
            continue;
          } else {
            *d = ' ';
            ctype = GRN_CHAR_BLANK|GRN_CHAR_SYMBOL;
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
  nstr->n_characters = length;
  nstr->normalized_length_in_bytes = (size_t)(d - (unsigned char *)nstr->normalized);
  return NULL;
}

inline static grn_obj *
sjis_normalize(grn_ctx *ctx, grn_string *nstr)
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
  size_t size = nstr->original_length_in_bytes, length = 0;
  int removeblankp = nstr->flags & GRN_STRING_REMOVE_BLANK;
  if (!(nstr->normalized = GRN_MALLOC(size * 2 + 1))) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[string][sjis] failed to allocate normalized text space");
    return NULL;
  }
  d0 = (unsigned char *) nstr->normalized;
  if (nstr->flags & GRN_STRING_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * 2 * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->normalized);
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][sjis] failed to allocate checks space");
      return NULL;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STRING_WITH_TYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->normalized);
      nstr->checks = NULL;
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][sjis] failed to allocate character types space");
      return NULL;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->original + size;
  for (s = s_ = (unsigned char *) nstr->original, d = d_ = d0; s < e; s++) {
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
                  if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
                  continue;
                } else {
                  *d = ' ';
                  ctype = GRN_CHAR_BLANK|GRN_CHAR_SYMBOL;
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
        if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
        continue;
      case 2 :
        if (c == 0x20) {
          if (removeblankp) {
            if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
            continue;
          } else {
            *d = ' ';
            ctype = GRN_CHAR_BLANK|GRN_CHAR_SYMBOL;
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
  nstr->n_characters = length;
  nstr->normalized_length_in_bytes = (size_t)(d - (unsigned char *)nstr->normalized);
  return NULL;
}

#ifdef GRN_WITH_NFKC
static inline int
grn_str_charlen_utf8(grn_ctx *ctx, const unsigned char *str, const unsigned char *end)
{
  /* MEMO: This function allows non-null-terminated string as str. */
  /*       But requires the end of string. */
  const unsigned char *p = str;
  if (end <= p || !*p) { return 0; }
  if (*p & 0x80) {
    int b, w;
    int size;
    int i;
    for (b = 0x40, w = 0; b && (*p & b); b >>= 1, w++);
    if (!w) {
      GRN_LOG(ctx, GRN_LOG_WARNING,
              "invalid utf8 string: the first bit is 0x80: <%.*s>: <%.*s>",
              (int)(end - p), p,
              (int)(end - str), str);
      return 0;
    }
    size = w + 1;
    for (i = 1; i < size; i++) {
      if (++p >= end) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "invalid utf8 string: too short: "
                "%d byte is required but %d byte is given: <%.*s>",
                size, i,
                (int)(end - str), str);
        return 0;
      }
      if (!*p) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "invalid utf8 string: NULL character is found: <%.*s>",
                (int)(end - str), str);
        return 0;
      }
      if ((*p & 0xc0) != 0x80) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "invalid utf8 string: 0x80 is not allowed: <%.*s>: <%.*s>",
                (int)(end - p), p,
                (int)(end - str), str);
        return 0;
      }
    }
    return size;
  } else {
    return 1;
  }
  return 0;
}

inline static grn_obj *
utf8_normalize(grn_ctx *ctx, grn_string *nstr)
{
  int16_t *ch;
  const unsigned char *s, *s_, *s__ = NULL, *p, *p2, *pe, *e;
  unsigned char *d, *d_, *de;
  uint_least8_t *cp;
  size_t length = 0, ls, lp, size = nstr->original_length_in_bytes, ds = size * 3;
  int removeblankp = nstr->flags & GRN_STRING_REMOVE_BLANK;
  grn_bool remove_tokenized_delimiter_p =
    nstr->flags & GRN_STRING_REMOVE_TOKENIZED_DELIMITER;
  if (!(nstr->normalized = GRN_MALLOC(ds + 1))) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[string][utf8] failed to allocate normalized text space");
    return NULL;
  }
  if (nstr->flags & GRN_STRING_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(ds * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->normalized);
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][utf8] failed to allocate checks space");
      return NULL;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STRING_WITH_TYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(ds + 1))) {
      if (nstr->checks) { GRN_FREE(nstr->checks); nstr->checks = NULL; }
      GRN_FREE(nstr->normalized); nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][utf8] failed to allocate character types space");
      return NULL;
    }
  }
  cp = nstr->ctypes;
  d = (unsigned char *)nstr->normalized;
  de = d + ds;
  d_ = NULL;
  e = (unsigned char *)nstr->original + size;
  for (s = s_ = (unsigned char *)nstr->original; ; s += ls) {
    if (!(ls = grn_str_charlen_utf8(ctx, s, e))) {
      break;
    }
    if (remove_tokenized_delimiter_p &&
        grn_tokenizer_is_tokenized_delimiter(ctx, (const char *)s, ls,
                                             GRN_ENC_UTF8)) {
      continue;
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
        if (ch[0] >= 0) {
          s_ = s__;
        }
      }
      d = d_;
      length--;
    }
    for (; ; p += lp) {
      if (!(lp = grn_str_charlen_utf8(ctx, p, pe))) {
        break;
      }
      if ((*p == ' ' && removeblankp) || *p < 0x20  /* skip unprintable ascii */ ) {
        if (cp > nstr->ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
      } else {
        if (de <= d + lp) {
          unsigned char *normalized;
          ds += (ds >> 1) + lp;
          if (!(normalized = GRN_REALLOC(nstr->normalized, ds + 1))) {
            if (nstr->ctypes) { GRN_FREE(nstr->ctypes); nstr->ctypes = NULL; }
            if (nstr->checks) { GRN_FREE(nstr->checks); nstr->checks = NULL; }
            GRN_FREE(nstr->normalized); nstr->normalized = NULL;
            ERR(GRN_NO_MEMORY_AVAILABLE,
                "[string][utf8] failed to expand normalized text space");
            return NULL;
          }
          de = normalized + ds;
          d = normalized + (d - (unsigned char *)nstr->normalized);
          nstr->normalized = (char *)normalized;
          if (ch) {
            int16_t *checks;
            if (!(checks = GRN_REALLOC(nstr->checks, ds * sizeof(int16_t) + 1))) {
              if (nstr->ctypes) { GRN_FREE(nstr->ctypes); nstr->ctypes = NULL; }
              GRN_FREE(nstr->checks); nstr->checks = NULL;
              GRN_FREE(nstr->normalized); nstr->normalized = NULL;
              ERR(GRN_NO_MEMORY_AVAILABLE,
                  "[string][utf8] failed to expand checks space");
              return NULL;
            }
            ch = checks + (ch - nstr->checks);
            nstr->checks = checks;
          }
          if (cp) {
            uint_least8_t *ctypes;
            if (!(ctypes = GRN_REALLOC(nstr->ctypes, ds + 1))) {
              GRN_FREE(nstr->ctypes); nstr->ctypes = NULL;
              if (nstr->checks) { GRN_FREE(nstr->checks); nstr->checks = NULL; }
              GRN_FREE(nstr->normalized); nstr->normalized = NULL;
              ERR(GRN_NO_MEMORY_AVAILABLE,
                  "[string][utf8] failed to expand character types space");
              return NULL;
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
  nstr->n_characters = length;
  nstr->normalized_length_in_bytes = (size_t)(d - (unsigned char *)nstr->normalized);
  return NULL;
}
#endif /* GRN_WITH_NFKC */

inline static grn_obj *
ascii_normalize(grn_ctx *ctx, grn_string *nstr)
{
  int16_t *ch;
  const unsigned char *s, *s_, *e;
  unsigned char *d, *d0, *d_;
  uint_least8_t *cp, *ctypes, ctype;
  size_t size = nstr->original_length_in_bytes, length = 0;
  int removeblankp = nstr->flags & GRN_STRING_REMOVE_BLANK;
  if (!(nstr->normalized = GRN_MALLOC(size + 1))) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[string][ascii] failed to allocate normalized text space");
    return NULL;
  }
  d0 = (unsigned char *) nstr->normalized;
  if (nstr->flags & GRN_STRING_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->normalized);
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][ascii] failed to allocate checks space");
      return NULL;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STRING_WITH_TYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->normalized);
      nstr->checks = NULL;
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][ascii] failed to allocate character types space");
      return NULL;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->original + size;
  for (s = s_ = (unsigned char *) nstr->original, d = d_ = d0; s < e; s++) {
    unsigned char c = *s;
    switch (c >> 4) {
    case 0 :
    case 1 :
      /* skip unprintable ascii */
      if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
      continue;
    case 2 :
      if (c == 0x20) {
        if (removeblankp) {
          if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
          continue;
        } else {
          *d = ' ';
          ctype = GRN_CHAR_BLANK|GRN_CHAR_SYMBOL;
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
  nstr->n_characters = length;
  nstr->normalized_length_in_bytes = (size_t)(d - (unsigned char *)nstr->normalized);
  return NULL;
}

/* use cp1252 as latin1 */
inline static grn_obj *
latin1_normalize(grn_ctx *ctx, grn_string *nstr)
{
  int16_t *ch;
  const unsigned char *s, *s_, *e;
  unsigned char *d, *d0, *d_;
  uint_least8_t *cp, *ctypes, ctype;
  size_t size = nstr->original_length_in_bytes, length = 0;
  int removeblankp = nstr->flags & GRN_STRING_REMOVE_BLANK;
  if (!(nstr->normalized = GRN_MALLOC(size + 1))) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[string][latin1] failed to allocate normalized text space");
    return NULL;
  }
  d0 = (unsigned char *) nstr->normalized;
  if (nstr->flags & GRN_STRING_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->normalized);
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][latin1] failed to allocate checks space");
      return NULL;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STRING_WITH_TYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->normalized);
      nstr->checks = NULL;
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[normalizer][latin1] failed to allocate character types space");
      return NULL;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->original + size;
  for (s = s_ = (unsigned char *) nstr->original, d = d_ = d0; s < e; s++) {
    unsigned char c = *s;
    switch (c >> 4) {
    case 0 :
    case 1 :
      /* skip unprintable ascii */
      if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
      continue;
    case 2 :
      if (c == 0x20) {
        if (removeblankp) {
          if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
          continue;
        } else {
          *d = ' ';
          ctype = GRN_CHAR_BLANK|GRN_CHAR_SYMBOL;
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
  nstr->n_characters = length;
  nstr->normalized_length_in_bytes = (size_t)(d - (unsigned char *)nstr->normalized);
  return NULL;
}

inline static grn_obj *
koi8r_normalize(grn_ctx *ctx, grn_string *nstr)
{
  int16_t *ch;
  const unsigned char *s, *s_, *e;
  unsigned char *d, *d0, *d_;
  uint_least8_t *cp, *ctypes, ctype;
  size_t size = nstr->original_length_in_bytes, length = 0;
  int removeblankp = nstr->flags & GRN_STRING_REMOVE_BLANK;
  if (!(nstr->normalized = GRN_MALLOC(size + 1))) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[string][koi8r] failed to allocate normalized text space");
    return NULL;
  }
  d0 = (unsigned char *) nstr->normalized;
  if (nstr->flags & GRN_STRING_WITH_CHECKS) {
    if (!(nstr->checks = GRN_MALLOC(size * sizeof(int16_t) + 1))) {
      GRN_FREE(nstr->normalized);
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][koi8r] failed to allocate checks space");
      return NULL;
    }
  }
  ch = nstr->checks;
  if (nstr->flags & GRN_STRING_WITH_TYPES) {
    if (!(nstr->ctypes = GRN_MALLOC(size + 1))) {
      GRN_FREE(nstr->checks);
      GRN_FREE(nstr->normalized);
      nstr->checks = NULL;
      nstr->normalized = NULL;
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[string][koi8r] failed to allocate character types space");
      return NULL;
    }
  }
  cp = ctypes = nstr->ctypes;
  e = (unsigned char *)nstr->original + size;
  for (s = s_ = (unsigned char *) nstr->original, d = d_ = d0; s < e; s++) {
    unsigned char c = *s;
    switch (c >> 4) {
    case 0 :
    case 1 :
      /* skip unprintable ascii */
      if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
      continue;
    case 2 :
      if (c == 0x20) {
        if (removeblankp) {
          if (cp > ctypes) { *(cp - 1) |= GRN_CHAR_BLANK; }
          continue;
        } else {
          *d = ' ';
          ctype = GRN_CHAR_BLANK|GRN_CHAR_SYMBOL;
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
  nstr->n_characters = length;
  nstr->normalized_length_in_bytes = (size_t)(d - (unsigned char *)nstr->normalized);
  return NULL;
}

static grn_obj *
auto_next(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_string *string = (grn_string *)(args[0]);
  switch (string->encoding) {
  case GRN_ENC_EUC_JP :
    eucjp_normalize(ctx, string);
    break;
  case GRN_ENC_UTF8 :
#ifdef GRN_WITH_NFKC
    utf8_normalize(ctx, string);
#else /* GRN_WITH_NFKC */
    ascii_normalize(ctx, string);
#endif /* GRN_WITH_NFKC */
    break;
  case GRN_ENC_SJIS :
    sjis_normalize(ctx, string);
    break;
  case GRN_ENC_LATIN1 :
    latin1_normalize(ctx, string);
    break;
  case GRN_ENC_KOI8R :
    koi8r_normalize(ctx, string);
    break;
  default :
    ascii_normalize(ctx, string);
    break;
  }
  return NULL;
}

#ifdef GRN_WITH_NFKC
static grn_obj *
nfkc51_next(grn_ctx *ctx, int nargs, grn_obj **args, grn_user_data *user_data)
{
  grn_string *string = (grn_string *)(args[0]);
  utf8_normalize(ctx, string);
  return NULL;
}
#endif /* GRN_WITH_NFKC */

grn_rc
grn_normalizer_normalize(grn_ctx *ctx, grn_obj *normalizer, grn_obj *string)
{
  grn_rc rc;
  int nargs = 0;

  grn_ctx_push(ctx, string);
  nargs++;
  rc = grn_proc_call(ctx, normalizer, nargs, NULL);
  grn_ctx_pop(ctx);

  return rc;
}

grn_rc
grn_db_init_builtin_normalizers(grn_ctx *ctx)
{
  const char *normalizer_nfkc51_name = "NormalizerNFKC51";

  grn_normalizer_register(ctx, GRN_NORMALIZER_AUTO_NAME, -1,
                          NULL, auto_next, NULL);

#ifdef GRN_WITH_NFKC
  grn_normalizer_register(ctx, normalizer_nfkc51_name, -1,
                          NULL, nfkc51_next, NULL);
#else /* GRN_WITH_NFKC */
  grn_normalizer_register(ctx, normalizer_nfkc51_name, -1,
                          NULL, NULL, NULL);
#endif /* GRN_WITH_NFKC */
/*
  grn_normalizer_register(ctx, "NormalizerUCA", -1,
                          NULL, uca_next, NULL);
*/

  return GRN_SUCCESS;
}
