/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2016 Brazil

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

#pragma once

#include "grn.h"
#include <groonga/nfkc.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
  GETOPT_OP_NONE = 0,
  GETOPT_OP_ON,
  GETOPT_OP_OFF,
  GETOPT_OP_UPDATE
} grn_str_getopt_op;

typedef struct {
  const char opt; /* ends opt == 0 && longopt == NULL */
  const char *longopt;
  const char **arg; /* if NULL, no arg are required */
  int flag;
  grn_str_getopt_op op;
} grn_str_getopt_opt;

GRN_API size_t grn_str_len(grn_ctx *ctx, const char *str, grn_encoding encoding, const char **last);

#define GRN_STR_BLANK      0x80
#define GRN_STR_ISBLANK(c) (c & 0x80)
#define GRN_STR_CTYPE(c)   (c & 0x7f)

GRN_API int grn_isspace(const char *s, grn_encoding encoding);
int8_t grn_atoi8(const char *nptr, const char *end, const char **rest);
uint8_t grn_atoui8(const char *nptr, const char *end, const char **rest);
int16_t grn_atoi16(const char *nptr, const char *end, const char **rest);
uint16_t grn_atoui16(const char *nptr, const char *end, const char **rest);
GRN_API int grn_atoi(const char *nptr, const char *end, const char **rest);
GRN_API unsigned int grn_atoui(const char *nptr, const char *end, const char **rest);
unsigned int grn_htoui(const char *nptr, const char *end, const char **rest);
GRN_API int64_t grn_atoll(const char *nptr, const char *end, const char **rest);
GRN_API uint64_t grn_atoull(const char *nptr, const char *end, const char **rest);
grn_rc grn_itoa(int i, char *p, char *end, char **rest);
grn_rc grn_lltoa(int64_t i, char *p, char *end, char **rest);
grn_rc grn_ulltoa(uint64_t i, char *p, char *end, char **rest);
GRN_API grn_rc grn_aton(grn_ctx *ctx, const char *p, const char *end, const char **rest, grn_obj *res);

GRN_API void grn_itoh(unsigned int i, char *p, unsigned int len);
int grn_str_tok(const char *str, size_t str_len, char delim, const char **tokbuf, int buf_size, const char **rest);
GRN_API int grn_str_getopt(int argc, char * const argv[], const grn_str_getopt_opt *opts, int *flags);

extern int grn_str_margin_size;

char *grn_itob(grn_id id, char *p);
grn_id grn_btoi(char *b);

grn_rc grn_substring(grn_ctx *ctx, char **str, char **str_end, int start, int end, grn_encoding encoding);

GRN_API int grn_charlen_(grn_ctx *ctx, const char *str, const char *end, grn_encoding encoding);
GRN_API grn_str *grn_str_open_(grn_ctx *ctx, const char *str, unsigned int str_len, int flags, grn_encoding encoding);

#define GRN_BULK_SET_CURR(buf,p) do {\
  if (GRN_BULK_OUTP(buf)) {\
    (buf)->u.b.curr = (char *)(p);\
  } else {\
    (buf)->header.flags = (char *)(p) - GRN_BULK_HEAD(buf);\
  }\
} while (0)

grn_rc grn_text_ulltoa(grn_ctx *ctx, grn_obj *buf, unsigned long long int i);

GRN_API const char *grn_text_cgidec(grn_ctx *ctx, grn_obj *buf,
                                    const char *p, const char *e,
                                    const char *delimiters);

#define GRN_TOK_VOID                   (0x00)
#define GRN_TOK_SYMBOL                 (0x01)
#define GRN_TOK_STRING                 (0x02)
#define GRN_TOK_QUOTE                  (0x03)

GRN_API const char *grn_text_unesc_tok(grn_ctx *ctx, grn_obj *buf,
                                       const char *p, const char *e,
                                       char *tok_type);

GRN_API void grn_str_url_path_normalize(grn_ctx *ctx,
                                        const char *path, size_t path_len,
                                        char *buf, size_t buf_len);

#define GRN_OBJ_FORMAT_XML_ELEMENT_MASK             (0x01<<1)
#define GRN_OBJ_FORMAT_XML_ELEMENT_RESULTSET        (0x00<<1)
#define GRN_OBJ_FORMAT_XML_ELEMENT_NAVIGATIONENTRY  (0x01<<1)

#include <stdio.h>
GRN_API grn_rc grn_text_fgets(grn_ctx *ctx, grn_obj *buf, FILE *fp);

grn_bool grn_bulk_is_zero(grn_ctx *ctx, grn_obj *obj);

#ifdef __cplusplus
}
#endif
