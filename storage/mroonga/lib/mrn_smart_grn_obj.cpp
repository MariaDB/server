/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014 Kouhei Sutou <kou@clear-code.com>

  This library is free software; you can redistribute it and/or
  modify it under the terms of the GNU Lesser General Public
  License as published by the Free Software Foundation; either
  version 2.1 of the License, or (at your option) any later version.

  This library is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with this library; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1335  USA
*/

#include <string.h>

#include "mrn_smart_grn_obj.hpp"

namespace mrn {
  SmartGrnObj::SmartGrnObj(grn_ctx *ctx, grn_obj *obj)
    : ctx_(ctx),
      obj_(obj) {
  }

  SmartGrnObj::SmartGrnObj(grn_ctx *ctx, const char *name, int name_size)
    : ctx_(ctx),
      obj_(NULL) {
    if (name_size < 0) {
      name_size = strlen(name);
    }
    obj_ = grn_ctx_get(ctx_, name, name_size);
  }

  SmartGrnObj::SmartGrnObj(grn_ctx *ctx, grn_id id)
    : ctx_(ctx),
      obj_(grn_ctx_at(ctx_, id)) {
  }

  SmartGrnObj::~SmartGrnObj() {
    if (obj_) {
      grn_obj_unlink(ctx_, obj_);
    }
  }

  grn_obj *SmartGrnObj::get() {
    return obj_;
  }

  grn_obj *SmartGrnObj::release() {
    grn_obj *obj = obj_;
    obj_ = NULL;
    return obj;
  }
}
