/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Brazil

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

#include "../grn_ctx_impl.h"

#ifdef GRN_WITH_MRUBY
# include <mruby.h>
# include <mruby/string.h>
# include <mruby/class.h>
# include <mruby/data.h>

# include "../grn_mrb.h"
# include "mrb_object.h"
# include "mrb_operator.h"
# include "mrb_converter.h"

void
grn_mrb_object_flags_init(grn_ctx *ctx)
{
  grn_mrb_data *data = &(ctx->impl->mrb);
  mrb_state *mrb = data->state;
  struct RClass *module = data->module;
  struct RClass *flags_module;

  flags_module = mrb_define_module_under(mrb, module, "ObjectFlags");

#define MRB_DEFINE_FLAG(name)                           \
  mrb_define_const(mrb, flags_module, #name,            \
                   mrb_fixnum_value(GRN_OBJ_ ## name))

  MRB_DEFINE_FLAG(TABLE_TYPE_MASK);
  MRB_DEFINE_FLAG(TABLE_HASH_KEY);
  MRB_DEFINE_FLAG(TABLE_PAT_KEY);
  MRB_DEFINE_FLAG(TABLE_DAT_KEY);
  MRB_DEFINE_FLAG(TABLE_NO_KEY);

  MRB_DEFINE_FLAG(KEY_MASK);
  MRB_DEFINE_FLAG(KEY_UINT);
  MRB_DEFINE_FLAG(KEY_INT);
  MRB_DEFINE_FLAG(KEY_FLOAT);
  MRB_DEFINE_FLAG(KEY_GEO_POINT);

  MRB_DEFINE_FLAG(KEY_WITH_SIS);
  MRB_DEFINE_FLAG(KEY_NORMALIZE);

  MRB_DEFINE_FLAG(COLUMN_TYPE_MASK);
  MRB_DEFINE_FLAG(COLUMN_SCALAR);
  MRB_DEFINE_FLAG(COLUMN_VECTOR);
  MRB_DEFINE_FLAG(COLUMN_INDEX);

  MRB_DEFINE_FLAG(COMPRESS_MASK);
  MRB_DEFINE_FLAG(COMPRESS_NONE);
  MRB_DEFINE_FLAG(COMPRESS_ZLIB);
  MRB_DEFINE_FLAG(COMPRESS_LZ4);
  MRB_DEFINE_FLAG(COMPRESS_ZSTD);

  MRB_DEFINE_FLAG(WITH_SECTION);
  MRB_DEFINE_FLAG(WITH_WEIGHT);
  MRB_DEFINE_FLAG(WITH_POSITION);
  MRB_DEFINE_FLAG(RING_BUFFER);

  MRB_DEFINE_FLAG(UNIT_MASK);
  MRB_DEFINE_FLAG(UNIT_DOCUMENT_NONE);
  MRB_DEFINE_FLAG(UNIT_DOCUMENT_SECTION);
  MRB_DEFINE_FLAG(UNIT_DOCUMENT_POSITION);
  MRB_DEFINE_FLAG(UNIT_SECTION_NONE);
  MRB_DEFINE_FLAG(UNIT_SECTION_POSITION);
  MRB_DEFINE_FLAG(UNIT_POSITION_NONE);
  MRB_DEFINE_FLAG(UNIT_USERDEF_DOCUMENT);
  MRB_DEFINE_FLAG(UNIT_USERDEF_SECTION);
  MRB_DEFINE_FLAG(UNIT_USERDEF_POSITION);

  MRB_DEFINE_FLAG(NO_SUBREC);
  MRB_DEFINE_FLAG(WITH_SUBREC);

  MRB_DEFINE_FLAG(KEY_VAR_SIZE);

  MRB_DEFINE_FLAG(TEMPORARY);
  MRB_DEFINE_FLAG(PERSISTENT);
}
#endif /* GRN_WITH_MRUBY */
