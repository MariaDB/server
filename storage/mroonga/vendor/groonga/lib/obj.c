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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/
#include "grn.h"
#include "grn_db.h"
#include <groonga/obj.h>

grn_bool
grn_obj_is_builtin(grn_ctx *ctx, grn_obj *obj)
{
  grn_id id;

  if (!obj) { return GRN_FALSE; }

  id = grn_obj_id(ctx, obj);
  if (id == GRN_ID_NIL) {
    return GRN_FALSE;
  } else {
    return id < GRN_N_RESERVED_TYPES;
  }
}

grn_bool
grn_obj_is_table(grn_ctx *ctx, grn_obj *obj)
{
  grn_bool is_table = GRN_FALSE;

  if (!obj) {
    return GRN_FALSE;
  }

  switch (obj->header.type) {
  case GRN_TABLE_NO_KEY :
  case GRN_TABLE_HASH_KEY :
  case GRN_TABLE_PAT_KEY :
  case GRN_TABLE_DAT_KEY :
    is_table = GRN_TRUE;
  default :
    break;
  }

  return is_table;
}

grn_bool
grn_obj_is_proc(grn_ctx *ctx, grn_obj *obj)
{
  if (!obj) {
    return GRN_FALSE;
  }

  return obj->header.type == GRN_PROC;
}

grn_bool
grn_obj_is_function_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->type == GRN_PROC_FUNCTION;
}

grn_bool
grn_obj_is_selector_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_function_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->selector != NULL;
}

grn_bool
grn_obj_is_scorer_proc(grn_ctx *ctx, grn_obj *obj)
{
  grn_proc *proc;

  if (!grn_obj_is_proc(ctx, obj)) {
    return GRN_FALSE;
  }

  proc = (grn_proc *)obj;
  return proc->type == GRN_PROC_SCORER;
}
