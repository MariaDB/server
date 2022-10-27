/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015-2016 Brazil

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

#include "../grn_ctx.h"

#ifdef __cplusplus
extern "C" {
#endif

mrb_value grn_mrb_options_get_static(mrb_state *mrb,
                                     mrb_value mrb_options,
                                     const char *key,
                                     size_t key_size);
#define grn_mrb_options_get_lit(mrb, mrb_options, literal)               \
  grn_mrb_options_get_static(mrb, mrb_options,                           \
                             (literal), mrb_strlen_lit(literal))

#ifdef __cplusplus
}
#endif

