/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2013-2014 Brazil

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License version 2.1 as published by the Free Software Foundation.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifndef GRN_MRB_H
#define GRN_MRB_H

#include "grn.h"
#include "grn_ctx.h"

#ifdef GRN_WITH_MRUBY
# include <mruby.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#ifdef GRN_WITH_MRUBY
GRN_API mrb_value grn_mrb_eval(grn_ctx *ctx, const char *script, int script_length);
GRN_API mrb_value grn_mrb_load(grn_ctx *ctx, const char *path);
GRN_API grn_rc grn_mrb_to_grn(grn_ctx *ctx, mrb_value mrb_object, grn_obj *grn_object);
GRN_API const char *grn_mrb_get_system_ruby_scripts_dir(grn_ctx *ctx);
#endif

#ifdef __cplusplus
}
#endif

#endif /* GRN_MRB_H */
