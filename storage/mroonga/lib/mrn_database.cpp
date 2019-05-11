/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2015 Kouhei Sutou <kou@clear-code.com>

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

#include <mrn_mysql.h>

#include "mrn_database.hpp"
#include "mrn_operations.hpp"

// for debug
#define MRN_CLASS_NAME "mrn::Database"

namespace mrn {
  Database::Database(grn_ctx *ctx, grn_obj *db)
    : ctx_(ctx),
      db_(db),
      broken_table_names_(NULL),
      is_broken_(false) {
    Operations operations(ctx_);
    broken_table_names_ = operations.collect_processing_table_names();
    is_broken_ = operations.is_locked();
  }

  Database::~Database(void) {
    close();
  }

  void Database::close() {
    MRN_DBUG_ENTER_METHOD();
    if (db_) {
      grn_hash_close(ctx_, broken_table_names_);
      broken_table_names_ = NULL;
      grn_obj_close(ctx_, db_);
      db_ = NULL;
    }
    DBUG_VOID_RETURN;
  }

  grn_rc Database::remove() {
    MRN_DBUG_ENTER_METHOD();
    grn_rc rc = GRN_SUCCESS;
    if (db_) {
      grn_hash_close(ctx_, broken_table_names_);
      broken_table_names_ = NULL;
      rc = grn_obj_remove(ctx_, db_);
      if (rc == GRN_SUCCESS) {
        db_ = NULL;
      }
    }
    DBUG_RETURN(rc);
  }

  grn_obj *Database::get() {
    MRN_DBUG_ENTER_METHOD();
    DBUG_RETURN(db_);
  }

  bool Database::is_broken() {
    MRN_DBUG_ENTER_METHOD();
    DBUG_RETURN(is_broken_);
  }

  bool Database::is_broken_table(const char *name, size_t name_size) {
    MRN_DBUG_ENTER_METHOD();
    grn_id id = grn_hash_get(ctx_, broken_table_names_, name, name_size, NULL);
    DBUG_RETURN(id != GRN_ID_NIL);
  }

  void Database::mark_table_repaired(const char *name, size_t name_size) {
    MRN_DBUG_ENTER_METHOD();
    grn_hash_delete(ctx_, broken_table_names_, name, name_size, NULL);
    DBUG_VOID_RETURN;
  }
}
