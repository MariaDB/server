/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2014 Brazil

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

#include "grn_ctx.h"
#include "grn_request_canceler.h"

typedef struct _grn_request_canceler grn_request_canceler;
struct _grn_request_canceler {
  grn_hash *entries;
  grn_mutex mutex;
};

typedef struct _grn_request_canceler_entry grn_request_canceler_entry;
struct _grn_request_canceler_entry {
  grn_ctx *ctx;
};

static grn_request_canceler *grn_the_request_canceler = NULL;

grn_bool
grn_request_canceler_init(void)
{
  grn_ctx *ctx = &grn_gctx;

  grn_the_request_canceler = GRN_MALLOC(sizeof(grn_request_canceler));
  if (!grn_the_request_canceler) {
    ERR(GRN_NO_MEMORY_AVAILABLE,
        "[request-canceler] failed to allocate the global request canceler");
    return GRN_FALSE;
  }

  grn_the_request_canceler->entries =
    grn_hash_create(ctx, NULL, GRN_TABLE_MAX_KEY_SIZE,
                    sizeof(grn_request_canceler_entry), GRN_OBJ_KEY_VAR_SIZE);
  if (!grn_the_request_canceler->entries) {
    return GRN_FALSE;
  }
  MUTEX_INIT(grn_the_request_canceler->mutex);

  return GRN_TRUE;
}

void
grn_request_canceler_register(grn_ctx *ctx,
                              const char *request_id, unsigned int size)
{
  MUTEX_LOCK(grn_the_request_canceler->mutex);
  {
    grn_hash *entries = grn_the_request_canceler->entries;
    grn_id id;
    void *value;
    id = grn_hash_add(&grn_gctx, entries, request_id, size, &value, NULL);
    if (id) {
      grn_request_canceler_entry *entry = value;
      entry->ctx = ctx;
    }
  }
  MUTEX_UNLOCK(grn_the_request_canceler->mutex);
}

void
grn_request_canceler_unregister(grn_ctx *ctx,
                                const char *request_id, unsigned int size)
{
  MUTEX_LOCK(grn_the_request_canceler->mutex);
  {
    grn_hash *entries = grn_the_request_canceler->entries;
    grn_hash_delete(&grn_gctx, entries, request_id, size, NULL);
  }
  MUTEX_UNLOCK(grn_the_request_canceler->mutex);

  if (ctx->rc == GRN_INTERRUPTED_FUNCTION_CALL) {
    ERRSET(ctx, GRN_LOG_NOTICE, ctx->rc,
           "[request-canceler] a request is canceled: <%.*s>",
           size, request_id);
  }
}

grn_bool
grn_request_canceler_cancel(const char *request_id, unsigned int size)
{
  grn_bool canceled = GRN_FALSE;
  MUTEX_LOCK(grn_the_request_canceler->mutex);
  {
    grn_hash *entries = grn_the_request_canceler->entries;
    void *value;
    if (grn_hash_get(&grn_gctx, entries, request_id, size, &value)) {
      grn_request_canceler_entry *entry = value;
      if (entry->ctx->rc == GRN_SUCCESS) {
        entry->ctx->rc = GRN_INTERRUPTED_FUNCTION_CALL;
        canceled = GRN_TRUE;
      }
    }
  }
  MUTEX_UNLOCK(grn_the_request_canceler->mutex);
  return canceled;
}

void
grn_request_canceler_fin(void)
{
  grn_ctx *ctx = &grn_gctx;

  grn_hash_close(ctx, grn_the_request_canceler->entries);
  MUTEX_FIN(grn_the_request_canceler->mutex);
  GRN_FREE(grn_the_request_canceler);
  grn_the_request_canceler = NULL;
}
