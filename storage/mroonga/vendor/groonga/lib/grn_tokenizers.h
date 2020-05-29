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

#include "grn_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

extern grn_obj *grn_tokenizer_uvector;

grn_rc grn_tokenizers_init(void);
grn_rc grn_tokenizers_fin(void);

grn_rc grn_db_init_mecab_tokenizer(grn_ctx *ctx);
void grn_db_fin_mecab_tokenizer(grn_ctx *ctx);
grn_rc grn_db_init_builtin_tokenizers(grn_ctx *ctx);

#ifdef __cplusplus
}
#endif
