/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2010 Tetsuro IKEDA
  Copyright(C) 2011-2012 Kouhei Sutou <kou@clear-code.com>

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
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
*/

#include <string.h>
#include <cppcutter.h>

#include <mrn_sys.hpp>

static grn_ctx *ctx;
static grn_obj *db;
static grn_hash *hash;
static grn_obj buffer;

namespace test_mrn_sys
{
  void cut_startup()
  {
    ctx = (grn_ctx *)malloc(sizeof(grn_ctx));
    grn_init();
    grn_ctx_init(ctx, 0);
    db = grn_db_create(ctx, NULL, NULL);
    grn_ctx_use(ctx, db);
  }

  void cut_shutdown()
  {
    grn_obj_unlink(ctx, db);
    grn_ctx_fin(ctx);
    grn_fin();
    free(ctx);
  }

  void cut_setup()
  {
    hash = grn_hash_create(ctx, NULL, 1024, sizeof(grn_obj *),
                           GRN_OBJ_KEY_VAR_SIZE);
    GRN_TEXT_INIT(&buffer, 0);
  }

  void cut_teardown()
  {
    grn_hash_close(ctx, hash);
    grn_obj_unlink(ctx, &buffer);
  }

  void test_mrn_hash_put()
  {
    const char *key = "mroonga";

    cut_assert_true(mrn_hash_put(ctx, hash, key, &buffer));
    cut_assert_false(mrn_hash_put(ctx, hash, key, &buffer));
  }

  void test_mrn_hash_get()
  {
    const char *key = "mroonga";
    const char *value = "A storage engine based on groonga.";
    grn_obj *result;

    GRN_TEXT_SETS(ctx, &buffer, value);
    GRN_TEXT_PUT(ctx, &buffer, "\0", 1);

    mrn_hash_put(ctx, hash, key, &buffer);
    cut_assert_true(mrn_hash_get(ctx, hash, key, &result));
    cppcut_assert_equal(value, GRN_TEXT_VALUE(&buffer));
  }

  void test_mrn_hash_remove()
  {
    const char *key = "mroonga";

    mrn_hash_put(ctx, hash, key, &buffer);

    cut_assert_false(mrn_hash_remove(ctx, hash, "nonexistent"));
    cut_assert_true(mrn_hash_remove(ctx, hash, key));
    cut_assert_false(mrn_hash_remove(ctx, hash, key));
  }
}
