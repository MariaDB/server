/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2011-2012 Kentoku SHIBA
  Copyright(C) 2011 Kouhei Sutou <kou@clear-code.com>

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
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include "mrn_sys.hpp"

bool mrn_hash_put(grn_ctx *ctx, grn_hash *hash, const char *key, grn_obj *value)
{
  int added;
  bool succeed;
  void *buf;
  grn_hash_add(ctx, hash, (const char *)key, strlen(key), &buf, &added);
  // duplicate check
  if (added == 0) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "hash put duplicated (key=%s)", key);
    succeed = false;
  } else {
    // store address of value
    memcpy(buf, &value, sizeof(grn_obj *));
    GRN_LOG(ctx, GRN_LOG_DEBUG, "hash put (key=%s)", key);
    succeed = true;
  }
  return succeed;
}

bool mrn_hash_get(grn_ctx *ctx, grn_hash *hash, const char *key, grn_obj **value)
{
  bool found;
  grn_id id;
  void *buf;
  id = grn_hash_get(ctx, hash, (const char *)key, strlen(key), &buf);
  // key not found
  if (id == GRN_ID_NIL) {
    GRN_LOG(ctx, GRN_LOG_DEBUG, "hash get not found (key=%s)", key);
    found = false;
  } else {
    // restore address of value
    memcpy(value, buf, sizeof(grn_obj *));
    found = true;
  }
  return found;
}

bool mrn_hash_remove(grn_ctx *ctx, grn_hash *hash, const char *key)
{
  bool succeed;
  grn_rc rc;
  grn_id id;
  id = grn_hash_get(ctx, hash, (const char*) key, strlen(key), NULL);
  if (id == GRN_ID_NIL) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "hash remove not found (key=%s)", key);
    succeed = false;
  } else {
    rc = grn_hash_delete_by_id(ctx, hash, id, NULL);
    if (rc != GRN_SUCCESS) {
      GRN_LOG(ctx, GRN_LOG_ERROR, "hash remove error (key=%s)", key);
      succeed = false;
    } else {
      GRN_LOG(ctx, GRN_LOG_DEBUG, "hash remove (key=%s)", key);
      succeed = true;
    }
  }
  return succeed;
}
