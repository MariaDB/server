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

#ifndef MRN_SMART_GRN_OBJ_HPP_
#define MRN_SMART_GRN_OBJ_HPP_

#include <groonga.h>

namespace mrn {
  class SmartGrnObj {
    grn_ctx *ctx_;
    grn_obj *obj_;
  public:
    SmartGrnObj(grn_ctx *ctx, grn_obj *obj);
    SmartGrnObj(grn_ctx *ctx, const char *name, int name_size=-1);
    SmartGrnObj(grn_ctx *ctx, grn_id id);
    ~SmartGrnObj();

    grn_obj *get();
    grn_obj *release();
  };
}

#endif // MRN_SMART_GRN_OBJ_HPP_
