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
#include "../grn_db.h"

#ifdef GRN_WITH_MRUBY
#include <mruby.h>
#include <mruby/hash.h>

#include "mrb_options.h"

mrb_value
grn_mrb_options_get_static(mrb_state *mrb,
                           mrb_value mrb_options,
                           const char *key,
                           size_t key_size)
{
  mrb_sym mrb_key;

  mrb_key = mrb_intern_static(mrb, key, key_size);
  return mrb_hash_get(mrb, mrb_options, mrb_symbol_value(mrb_key));
}
#endif
