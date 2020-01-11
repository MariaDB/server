/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2017 Brazil

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
#include "grn_ctx.h"

#ifdef GRN_WITH_MRUBY
# include <mruby.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

void grn_mrb_init_from_env(void);

#ifdef GRN_WITH_MRUBY
GRN_API mrb_value grn_mrb_load(grn_ctx *ctx, const char *path);
GRN_API const char *grn_mrb_get_system_ruby_scripts_dir(grn_ctx *ctx);
grn_bool grn_mrb_is_order_by_estimated_size_enabled(void);
#endif

#ifdef __cplusplus
}
#endif
