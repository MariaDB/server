/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2011-2017 Brazil

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
#include "grn.h"
#include <sys/types.h>
#include <sys/stat.h>
#include <cstring>
#include <new>
#include "grn_str.h"
#include "grn_io.h"
#include "grn_dat.h"
#include "grn_util.h"
#include "grn_normalizer.h"

#include "dat/trie.hpp"
#include "dat/cursor-factory.hpp"

namespace {

const uint32_t FILE_ID_LENGTH = 3;

class CriticalSection {
 public:
  CriticalSection() : lock_(NULL) {}
  explicit CriticalSection(grn_critical_section *lock) : lock_(lock) {
    CRITICAL_SECTION_ENTER(*lock_);
  }
  ~CriticalSection() {
    leave();
  }

  void enter(grn_critical_section *lock) {
    leave();
    lock_ = lock;
  }
  void leave() {
    if (lock_ != NULL) {
      CRITICAL_SECTION_LEAVE(*lock_);
      lock_ = NULL;
    }
  }

 private:
  grn_critical_section *lock_;

  // Disallows copy and assignment.
  CriticalSection(const CriticalSection &);
  CriticalSection &operator=(const CriticalSection &);
};

/*
  grn_dat_remove_file() removes a file specified by `path' and then returns
  true on success, false on failure. Note that grn_dat_remove_file() does not
  change `ctx->rc'.
 */
bool
grn_dat_remove_file(grn_ctx *ctx, const char *path)
{
  struct stat stat;

  if (::stat(path, &stat) == -1) {
    return false;
  }

  if (grn_unlink(path) == -1) {
    const char *system_message = grn_strerror(errno);
    GRN_LOG(ctx, GRN_LOG_WARNING,
            "[dat][remove-file] failed to remove path: %s: <%s>",
            system_message, path);
    return false;
  }

  GRN_LOG(ctx, GRN_LOG_INFO,
          "[dat][remove-file] removed: <%s>", path);
  return true;
}

grn_rc
grn_dat_translate_error_code(grn::dat::ErrorCode error_code) {
  switch (error_code) {
    case grn::dat::PARAM_ERROR: {
      return GRN_INVALID_ARGUMENT;
    }
    case grn::dat::IO_ERROR: {
      return GRN_INPUT_OUTPUT_ERROR;
    }
    case grn::dat::FORMAT_ERROR: {
      return GRN_INVALID_FORMAT;
    }
    case grn::dat::MEMORY_ERROR: {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    case grn::dat::SIZE_ERROR:
    case grn::dat::UNEXPECTED_ERROR: {
      return GRN_UNKNOWN_ERROR;
    }
    case grn::dat::STATUS_ERROR: {
      return GRN_FILE_CORRUPT;
    }
    default: {
      return GRN_UNKNOWN_ERROR;
    }
  }
}

void
grn_dat_init(grn_ctx *, grn_dat *dat)
{
  GRN_DB_OBJ_SET_TYPE(dat, GRN_TABLE_DAT_KEY);
  dat->io = NULL;
  dat->header = NULL;
  dat->file_id = 0;
  dat->encoding = GRN_ENC_DEFAULT;
  dat->trie = NULL;
  dat->old_trie = NULL;
  dat->tokenizer = NULL;
  dat->normalizer = NULL;
  GRN_PTR_INIT(&(dat->token_filters), GRN_OBJ_VECTOR, GRN_ID_NIL);
  CRITICAL_SECTION_INIT(dat->lock);
  dat->is_dirty = GRN_FALSE;
}

void
grn_dat_fin(grn_ctx *ctx, grn_dat *dat)
{
  CRITICAL_SECTION_FIN(dat->lock);
  delete static_cast<grn::dat::Trie *>(dat->old_trie);
  delete static_cast<grn::dat::Trie *>(dat->trie);
  dat->old_trie = NULL;
  dat->trie = NULL;
  if (dat->io) {
    if (dat->is_dirty) {
      uint32_t n_dirty_opens;
      GRN_ATOMIC_ADD_EX(&(dat->header->n_dirty_opens), -1, n_dirty_opens);
    }
    grn_io_close(ctx, dat->io);
    dat->io = NULL;
  }
  GRN_OBJ_FIN(ctx, &(dat->token_filters));
}

/*
  grn_dat_generate_trie_path() generates the path from `base_path' and
  `file_id'. The generated path is stored in `trie_path'.
 */
void
grn_dat_generate_trie_path(const char *base_path, char *trie_path, uint32_t file_id)
{
  if (!base_path || !base_path[0]) {
    trie_path[0] = '\0';
    return;
  }
  const size_t len = std::strlen(base_path);
  grn_memcpy(trie_path, base_path, len);
  trie_path[len] = '.';
  grn_itoh(file_id % (1U << (4 * FILE_ID_LENGTH)),
           trie_path + len + 1, FILE_ID_LENGTH);
  trie_path[len + 1 + FILE_ID_LENGTH] = '\0';
}

bool
grn_dat_open_trie_if_needed(grn_ctx *ctx, grn_dat *dat)
{
  if (!dat) {
    ERR(GRN_INVALID_ARGUMENT, "dat is null");
    return false;
  }

  const uint32_t file_id = dat->header->file_id;
  if (!file_id || (dat->trie && (file_id <= dat->file_id))) {
    /*
      There is no need to open file when no trie file is available or the
      current trie file is the latest one.
     */
    return true;
  }

  CriticalSection critical_section(&dat->lock);

  if (dat->trie && (file_id <= dat->file_id)) {
    /*
      There is no need to open file if the latest file has been opened by
      another thread.
     */
    return true;
  }

  char trie_path[PATH_MAX];
  grn_dat_generate_trie_path(grn_io_path(dat->io), trie_path, file_id);
  grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  grn::dat::Trie * const old_trie = static_cast<grn::dat::Trie *>(dat->old_trie);
  grn::dat::Trie * const new_trie = new (std::nothrow) grn::dat::Trie;
  if (!new_trie) {
    MERR("new grn::dat::Trie failed");
    return false;
  }

  if (trie_path[0] == '\0') {
    try {
      new_trie->create(trie_path);
    } catch (const grn::dat::Exception &ex) {
      ERR(grn_dat_translate_error_code(ex.code()),
          "grn::dat::Trie::create failed: %s",
          ex.what());
      delete new_trie;
      return false;
    }
  } else {
    try {
      new_trie->open(trie_path);
    } catch (const grn::dat::Exception &ex) {
      ERR(grn_dat_translate_error_code(ex.code()),
          "grn::dat::Trie::open failed: %s",
          ex.what());
      delete new_trie;
      return false;
    }
  }

  dat->old_trie = trie;
  dat->trie = new_trie;
  dat->file_id = file_id;

  critical_section.leave();

  delete old_trie;
  if (file_id >= 3) {
    grn_dat_generate_trie_path(grn_io_path(dat->io), trie_path, file_id - 2);
    grn_dat_remove_file(ctx, trie_path);
  }
  return true;
}

bool grn_dat_rebuild_trie(grn_ctx *ctx, grn_dat *dat) {
  const grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  grn::dat::Trie * const new_trie = new (std::nothrow) grn::dat::Trie;
  if (!new_trie) {
    MERR("new grn::dat::Trie failed");
    return false;
  }

  const uint32_t file_id = dat->header->file_id;
  char trie_path[PATH_MAX];
  grn_dat_generate_trie_path(grn_io_path(dat->io), trie_path, file_id + 1);

  for (uint64_t file_size = trie->file_size() * 2;; file_size *= 2) {
    try {
      new_trie->create(*trie, trie_path, file_size);
    } catch (const grn::dat::SizeError &) {
      continue;
    } catch (const grn::dat::Exception &ex) {
      ERR(grn_dat_translate_error_code(ex.code()),
          "grn::dat::Trie::open failed: %s",
          ex.what());
      delete new_trie;
      return false;
    }
    break;
  }

  grn::dat::Trie * const old_trie = static_cast<grn::dat::Trie *>(dat->old_trie);
  dat->old_trie = dat->trie;
  dat->trie = new_trie;
  dat->header->file_id = dat->file_id = file_id + 1;

  delete old_trie;
  if (file_id >= 2) {
    char trie_path[PATH_MAX];
    grn_dat_generate_trie_path(grn_io_path(dat->io), trie_path, file_id - 1);
    grn_dat_remove_file(ctx, trie_path);
  }
  return true;
}

void grn_dat_cursor_init(grn_ctx *, grn_dat_cursor *cursor) {
  GRN_DB_OBJ_SET_TYPE(cursor, GRN_CURSOR_TABLE_DAT_KEY);
  cursor->dat = NULL;
  cursor->cursor = NULL;
  cursor->key = &grn::dat::Key::invalid_key();
  cursor->curr_rec = GRN_ID_NIL;
}

void grn_dat_cursor_fin(grn_ctx *, grn_dat_cursor *cursor) {
  delete static_cast<grn::dat::Cursor *>(cursor->cursor);
  cursor->dat = NULL;
  cursor->cursor = NULL;
  cursor->key = &grn::dat::Key::invalid_key();
  cursor->curr_rec = GRN_ID_NIL;
}

}  // namespace

extern "C" {

grn_dat *
grn_dat_create(grn_ctx *ctx, const char *path, uint32_t,
               uint32_t, uint32_t flags)
{
  if (path) {
    if (path[0] == '\0') {
      path = NULL;
    } else if (std::strlen(path) >= (PATH_MAX - (FILE_ID_LENGTH + 1))) {
      ERR(GRN_FILENAME_TOO_LONG, "too long path");
      return NULL;
    }
  }

  grn_dat * const dat = static_cast<grn_dat *>(GRN_CALLOC(sizeof(grn_dat)));
  if (!dat) {
    return NULL;
  }
  grn_dat_init(ctx, dat);

  dat->io = grn_io_create(ctx, path, sizeof(struct grn_dat_header),
                          4096, 0, grn_io_auto, GRN_IO_EXPIRE_SEGMENT);
  if (!dat->io) {
    GRN_FREE(dat);
    return NULL;
  }
  grn_io_set_type(dat->io, GRN_TABLE_DAT_KEY);

  dat->header = static_cast<struct grn_dat_header *>(grn_io_header(dat->io));
  if (!dat->header) {
    grn_io_close(ctx, dat->io);
    grn_dat_remove_file(ctx, path);
    GRN_FREE(dat);
    return NULL;
  }
  const grn_encoding encoding = (ctx->encoding != GRN_ENC_DEFAULT) ?
      ctx->encoding : grn_gctx.encoding;
  dat->header->flags = flags;
  dat->header->encoding = encoding;
  dat->header->tokenizer = GRN_ID_NIL;
  dat->header->file_id = 0;
  if (dat->header->flags & GRN_OBJ_KEY_NORMALIZE) {
    dat->header->flags &= ~GRN_OBJ_KEY_NORMALIZE;
    dat->normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
    dat->header->normalizer = grn_obj_id(ctx, dat->normalizer);
  } else {
    dat->normalizer = NULL;
    dat->header->normalizer = GRN_ID_NIL;
  }
  dat->encoding = encoding;
  dat->tokenizer = NULL;
  GRN_PTR_INIT(&(dat->token_filters), GRN_OBJ_VECTOR, GRN_ID_NIL);

  dat->obj.header.flags = dat->header->flags;

  return dat;
}

grn_dat *
grn_dat_open(grn_ctx *ctx, const char *path)
{
  if (path && (std::strlen(path) >= (PATH_MAX - (FILE_ID_LENGTH + 1)))) {
    ERR(GRN_FILENAME_TOO_LONG, "too long path");
    return NULL;
  }

  grn_dat * const dat = static_cast<grn_dat *>(GRN_MALLOC(sizeof(grn_dat)));
  if (!dat) {
    return NULL;
  }

  grn_dat_init(ctx, dat);
  dat->io = grn_io_open(ctx, path, grn_io_auto);
  if (!dat->io) {
    GRN_FREE(dat);
    return NULL;
  }

  dat->header = (struct grn_dat_header *)grn_io_header(dat->io);
  if (!dat->header) {
    grn_io_close(ctx, dat->io);
    GRN_FREE(dat);
    return NULL;
  }
  dat->file_id = dat->header->file_id;
  dat->encoding = dat->header->encoding;
  dat->tokenizer = grn_ctx_at(ctx, dat->header->tokenizer);
  if (dat->header->flags & GRN_OBJ_KEY_NORMALIZE) {
    dat->header->flags &= ~GRN_OBJ_KEY_NORMALIZE;
    dat->normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
    dat->header->normalizer = grn_obj_id(ctx, dat->normalizer);
  } else {
    dat->normalizer = grn_ctx_at(ctx, dat->header->normalizer);
  }
  GRN_PTR_INIT(&(dat->token_filters), GRN_OBJ_VECTOR, GRN_ID_NIL);
  dat->obj.header.flags = dat->header->flags;
  return dat;
}

grn_rc
grn_dat_close(grn_ctx *ctx, grn_dat *dat)
{
  if (dat) {
    grn_dat_fin(ctx, dat);
    GRN_FREE(dat);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_dat_remove(grn_ctx *ctx, const char *path)
{
  if (!path) {
    ERR(GRN_INVALID_ARGUMENT, "path is null");
    return GRN_INVALID_ARGUMENT;
  }

  grn_dat * const dat = grn_dat_open(ctx, path);
  if (!dat) {
    return ctx->rc;
  }
  const uint32_t file_id = dat->header->file_id;
  grn_dat_close(ctx, dat);

  /*
    grn_dat_remove() tries to remove (file_id + 1)th trie file because
    grn::dat::Trie::create() might leave an incomplete file on failure.
   */
  char trie_path[PATH_MAX];
  grn_dat_generate_trie_path(path, trie_path, file_id + 1);
  grn_dat_remove_file(ctx, trie_path);
  for (uint32_t i = file_id; i > 0; --i) {
    grn_dat_generate_trie_path(path, trie_path, i);
    if (!grn_dat_remove_file(ctx, trie_path)) {
      break;
    }
  }

  /*
    grn_io_remove() reports an error when it fails to remove `path'.
   */
  return grn_io_remove(ctx, path);
}

grn_id
grn_dat_get(grn_ctx *ctx, grn_dat *dat, const void *key,
            unsigned int key_size, void **)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return GRN_ID_NIL;
  }
  const grn::dat::Trie * const trie = static_cast<const grn::dat::Trie *>(dat->trie);
  if (!trie) {
    return GRN_ID_NIL;
  }
  grn::dat::UInt32 key_pos;
  try {
    if (trie->search(key, key_size, &key_pos)) {
      return trie->get_key(key_pos).id();
    }
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::Trie::search failed: %s",
        ex.what());
  }
  return GRN_ID_NIL;
}

grn_id
grn_dat_add(grn_ctx *ctx, grn_dat *dat, const void *key,
            unsigned int key_size, void **, int *added)
{
  if (!key_size) {
    return GRN_ID_NIL;
  } else if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return GRN_ID_NIL;
  }

  if (!dat->trie) {
    char trie_path[PATH_MAX];
    grn_dat_generate_trie_path(grn_io_path(dat->io), trie_path, 1);
    grn::dat::Trie * const new_trie = new (std::nothrow) grn::dat::Trie;
    if (!new_trie) {
      MERR("new grn::dat::Trie failed");
      return GRN_ID_NIL;
    }
    try {
      new_trie->create(trie_path);
    } catch (const grn::dat::Exception &ex) {
      ERR(grn_dat_translate_error_code(ex.code()),
          "grn::dat::Trie::create failed: %s",
          ex.what());
      delete new_trie;
      return GRN_ID_NIL;
    }
    dat->trie = new_trie;
    dat->file_id = dat->header->file_id = 1;
  }

  grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  try {
    grn::dat::UInt32 key_pos;
    const bool res = trie->insert(key, key_size, &key_pos);
    if (added) {
      *added = res ? 1 : 0;
    }
    return trie->get_key(key_pos).id();
  } catch (const grn::dat::SizeError &) {
    if (!grn_dat_rebuild_trie(ctx, dat)) {
      return GRN_ID_NIL;
    }
    grn::dat::Trie * const new_trie = static_cast<grn::dat::Trie *>(dat->trie);
    grn::dat::UInt32 key_pos;
    const bool res = new_trie->insert(key, key_size, &key_pos);
    if (added) {
      *added = res ? 1 : 0;
    }
    return new_trie->get_key(key_pos).id();
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::Trie::insert failed: %s",
        ex.what());
    return GRN_ID_NIL;
  }
}

int
grn_dat_get_key(grn_ctx *ctx, grn_dat *dat, grn_id id, void *keybuf, int bufsize)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return 0;
  }
  const grn::dat::Trie * const trie = static_cast<const grn::dat::Trie *>(dat->trie);
  if (!trie) {
    return 0;
  }
  const grn::dat::Key &key = trie->ith_key(id);
  if (!key.is_valid()) {
    return 0;
  }
  if (keybuf && (bufsize >= (int)key.length())) {
    grn_memcpy(keybuf, key.ptr(), key.length());
  }
  return (int)key.length();
}

int
grn_dat_get_key2(grn_ctx *ctx, grn_dat *dat, grn_id id, grn_obj *bulk)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return 0;
  }
  const grn::dat::Trie * const trie = static_cast<const grn::dat::Trie *>(dat->trie);
  if (!trie) {
    return 0;
  }
  const grn::dat::Key &key = trie->ith_key(id);
  if (!key.is_valid()) {
    return 0;
  }
  if (bulk->header.impl_flags & GRN_OBJ_REFER) {
    bulk->u.b.head = static_cast<char *>(const_cast<void *>(key.ptr()));
    bulk->u.b.curr = bulk->u.b.head + key.length();
  } else {
    grn_bulk_write(ctx, bulk, static_cast<const char *>(key.ptr()), key.length());
  }
  return (int)key.length();
}

grn_rc
grn_dat_delete_by_id(grn_ctx *ctx, grn_dat *dat, grn_id id,
                     grn_table_delete_optarg *optarg)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  } else if (!dat->trie || (id == GRN_ID_NIL)) {
    return GRN_INVALID_ARGUMENT;
  }

  if (optarg && optarg->func) {
    const grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
    if (!trie->ith_entry(id).is_valid()) {
      return GRN_INVALID_ARGUMENT;
    } else if (!optarg->func(ctx, reinterpret_cast<grn_obj *>(dat), id, optarg->func_arg)) {
      return GRN_SUCCESS;
    }
  }

  try {
    grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
    if (!trie->remove(id)) {
      return GRN_INVALID_ARGUMENT;
    }
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::Trie::remove failed: %s",
        ex.what());
    return ctx->rc;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_dat_delete(grn_ctx *ctx, grn_dat *dat, const void *key, unsigned int key_size,
               grn_table_delete_optarg *optarg)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  } else if (!dat->trie || !key || !key_size) {
    return GRN_INVALID_ARGUMENT;
  }

  if (optarg && optarg->func) {
    try {
      const grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
      grn::dat::UInt32 key_pos;
      if (!trie->search(key, key_size, &key_pos)) {
        return GRN_INVALID_ARGUMENT;
      } else if (!optarg->func(ctx, reinterpret_cast<grn_obj *>(dat),
                               trie->get_key(key_pos).id(), optarg->func_arg)) {
        return GRN_SUCCESS;
      }
    } catch (const grn::dat::Exception &ex) {
      ERR(grn_dat_translate_error_code(ex.code()),
          "grn::dat::Trie::search failed: %s",
          ex.what());
      return ctx->rc;
    }
  }

  try {
    grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
    if (!trie->remove(key, key_size)) {
      return GRN_INVALID_ARGUMENT;
    }
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::Trie::remove failed: %s",
        ex.what());
    return ctx->rc;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_dat_update_by_id(grn_ctx *ctx, grn_dat *dat, grn_id src_key_id,
                     const void *dest_key, unsigned int dest_key_size)
{
  if (!dest_key_size) {
    return GRN_INVALID_ARGUMENT;
  } else if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  } else if (!dat->trie) {
    return GRN_INVALID_ARGUMENT;
  }
  try {
    try {
      grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
      if (!trie->update(src_key_id, dest_key, dest_key_size)) {
        return GRN_INVALID_ARGUMENT;
      }
    } catch (const grn::dat::SizeError &) {
      if (!grn_dat_rebuild_trie(ctx, dat)) {
        return ctx->rc;
      }
      grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
      if (!trie->update(src_key_id, dest_key, dest_key_size)) {
        return GRN_INVALID_ARGUMENT;
      }
    }
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::Trie::update failed: %s",
        ex.what());
    return ctx->rc;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_dat_update(grn_ctx *ctx, grn_dat *dat,
               const void *src_key, unsigned int src_key_size,
               const void *dest_key, unsigned int dest_key_size)
{
  if (!dest_key_size) {
    return GRN_INVALID_ARGUMENT;
  } else if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  } else if (!dat->trie) {
    return GRN_INVALID_ARGUMENT;
  }
  try {
    try {
      grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
      if (!trie->update(src_key, src_key_size, dest_key, dest_key_size)) {
        return GRN_INVALID_ARGUMENT;
      }
    } catch (const grn::dat::SizeError &) {
      if (!grn_dat_rebuild_trie(ctx, dat)) {
        return ctx->rc;
      }
      grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
      if (!trie->update(src_key, src_key_size, dest_key, dest_key_size)) {
        return GRN_INVALID_ARGUMENT;
      }
    }
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::Trie::update failed: %s",
        ex.what());
    return ctx->rc;
  }
  return GRN_SUCCESS;
}

int
grn_dat_scan(grn_ctx *ctx, grn_dat *dat, const char *str,
             unsigned int str_size, grn_dat_scan_hit *scan_hits,
             unsigned int max_num_scan_hits, const char **str_rest)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat) || !str ||
      !(dat->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) || !scan_hits) {
    if (str_rest) {
      *str_rest = str;
    }
    return -1;
  }

  grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (!trie) {
    if (str_rest) {
      *str_rest = str + str_size;
    }
    return 0;
  }

  if (!max_num_scan_hits || !str_size) {
    if (str_rest) {
      *str_rest = str;
    }
    return 0;
  }

  unsigned int num_scan_hits = 0;
  try {
    if (dat->normalizer) {
      int flags = GRN_STRING_WITH_CHECKS;
      grn_obj * const normalized_string = grn_string_open(ctx, str, str_size,
                                                          dat->normalizer,
                                                          flags);
      if (!normalized_string) {
        if (str_rest) {
          *str_rest = str;
        }
        return -1;
      }
      grn_string_get_normalized(ctx, normalized_string, &str, &str_size, NULL);
      const short *checks = grn_string_get_checks(ctx, normalized_string);
      unsigned int offset = 0;
      while (str_size) {
        if (*checks) {
          grn::dat::UInt32 key_pos;
          if (trie->lcp_search(str, str_size, &key_pos)) {
            const grn::dat::Key &key = trie->get_key(key_pos);
            const grn::dat::UInt32 key_length = key.length();
            if ((key_length == str_size) || (checks[key_length])) {
              unsigned int length = 0;
              for (grn::dat::UInt32 i = 0; i < key_length; ++i) {
                if (checks[i] > 0) {
                  length += checks[i];
                }
              }
              scan_hits[num_scan_hits].id = key.id();
              scan_hits[num_scan_hits].offset = offset;
              scan_hits[num_scan_hits].length = length;
              offset += length;
              str += key_length;
              str_size -= key_length;
              checks += key_length;
              if (++num_scan_hits >= max_num_scan_hits) {
                break;
              }
              continue;
            }
          }
          if (*checks > 0) {
            offset += *checks;
          }
        }
        ++str;
        --str_size;
        ++checks;
      }
      if (str_rest) {
        grn_string_get_original(ctx, normalized_string, str_rest, NULL);
        *str_rest += offset;
      }
      grn_obj_close(ctx, normalized_string);
    } else {
      const char * const begin = str;
      while (str_size) {
        grn::dat::UInt32 key_pos;
        if (trie->lcp_search(str, str_size, &key_pos)) {
          const grn::dat::Key &key = trie->get_key(key_pos);
          scan_hits[num_scan_hits].id = key.id();
          scan_hits[num_scan_hits].offset = str - begin;
          scan_hits[num_scan_hits].length = key.length();
          str += key.length();
          str_size -= key.length();
          if (++num_scan_hits >= max_num_scan_hits) {
            break;
          }
        } else {
          const int char_length = grn_charlen(ctx, str, str + str_size);
          if (char_length) {
            str += char_length;
            str_size -= char_length;
          } else {
            ++str;
            --str_size;
          }
        }
      }
      if (str_rest) {
        *str_rest = str;
      }
    }
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::lcp_search failed: %s",
        ex.what());
    if (str_rest) {
      *str_rest = str;
    }
    return -1;
  }
  return static_cast<int>(num_scan_hits);
}

grn_id
grn_dat_lcp_search(grn_ctx *ctx, grn_dat *dat,
                   const void *key, unsigned int key_size)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat) || !key ||
      !(dat->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE)) {
    return GRN_ID_NIL;
  }

  grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (!trie) {
    return GRN_ID_NIL;
  }

  try {
    grn::dat::UInt32 key_pos;
    if (!trie->lcp_search(key, key_size, &key_pos)) {
      return GRN_ID_NIL;
    }
    return trie->get_key(key_pos).id();
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::PrefixCursor::open failed: %s",
        ex.what());
    return GRN_ID_NIL;
  }
}

unsigned int
grn_dat_size(grn_ctx *ctx, grn_dat *dat)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return 0;
  }
  const grn::dat::Trie * const trie = static_cast<const grn::dat::Trie *>(dat->trie);
  if (trie) {
    return trie->num_keys();
  }
  return 0;
}

grn_dat_cursor *
grn_dat_cursor_open(grn_ctx *ctx, grn_dat *dat,
                    const void *min, unsigned int min_size,
                    const void *max, unsigned int max_size,
                    int offset, int limit, int flags)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return NULL;
  }

  grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (!trie) {
    grn_dat_cursor * const dc =
        static_cast<grn_dat_cursor *>(GRN_MALLOC(sizeof(grn_dat_cursor)));
    if (dc) {
      grn_dat_cursor_init(ctx, dc);
    }
    return dc;
  }

  grn_dat_cursor * const dc =
      static_cast<grn_dat_cursor *>(GRN_MALLOC(sizeof(grn_dat_cursor)));
  if (!dc) {
    return NULL;
  }
  grn_dat_cursor_init(ctx, dc);

  try {
    if ((flags & GRN_CURSOR_BY_ID) != 0) {
      dc->cursor = grn::dat::CursorFactory::open(*trie,
          min, min_size, max, max_size, offset, limit,
          grn::dat::ID_RANGE_CURSOR |
          ((flags & GRN_CURSOR_DESCENDING) ? grn::dat::DESCENDING_CURSOR : 0) |
          ((flags & GRN_CURSOR_GT) ? grn::dat::EXCEPT_LOWER_BOUND : 0) |
          ((flags & GRN_CURSOR_LT) ? grn::dat::EXCEPT_UPPER_BOUND : 0));
    } else if ((flags & GRN_CURSOR_PREFIX) != 0) {
      if (max && max_size) {
        if ((dat->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) != 0) {
          dc->cursor = grn::dat::CursorFactory::open(*trie,
              NULL, min_size, max, max_size, offset, limit,
              grn::dat::PREFIX_CURSOR | grn::dat::DESCENDING_CURSOR);
        } else {
          // TODO: near
        }
      } else if (min && min_size) {
        if ((flags & GRN_CURSOR_RK) != 0) {
          // TODO: rk search
        } else {
          dc->cursor = grn::dat::CursorFactory::open(*trie,
              min, min_size, NULL, 0, offset, limit,
              grn::dat::PREDICTIVE_CURSOR |
              ((flags & GRN_CURSOR_DESCENDING) ? grn::dat::DESCENDING_CURSOR : 0) |
              ((flags & GRN_CURSOR_GT) ? grn::dat::EXCEPT_EXACT_MATCH : 0));
        }
      }
    } else {
      dc->cursor = grn::dat::CursorFactory::open(*trie,
          min, min_size, max, max_size, offset, limit,
          grn::dat::KEY_RANGE_CURSOR |
          ((flags & GRN_CURSOR_DESCENDING) ? grn::dat::DESCENDING_CURSOR : 0) |
          ((flags & GRN_CURSOR_GT) ? grn::dat::EXCEPT_LOWER_BOUND : 0) |
          ((flags & GRN_CURSOR_LT) ? grn::dat::EXCEPT_UPPER_BOUND : 0));
    }
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::CursorFactory::open failed: %s",
        ex.what());
    GRN_FREE(dc);
    return NULL;
  }
  if (!dc->cursor) {
    ERR(GRN_INVALID_ARGUMENT, "unsupported query");
    GRN_FREE(dc);
    return NULL;
  }
  dc->dat = dat;
  return dc;
}

grn_id
grn_dat_cursor_next(grn_ctx *ctx, grn_dat_cursor *c)
{
  if (!c || !c->cursor) {
    return GRN_ID_NIL;
  }
  try {
    grn::dat::Cursor * const cursor = static_cast<grn::dat::Cursor *>(c->cursor);
    const grn::dat::Key &key = cursor->next();
    c->key = &key;
    c->curr_rec = key.is_valid() ? key.id() : GRN_ID_NIL;
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::Cursor::next failed: %s",
        ex.what());
    return GRN_ID_NIL;
  }
  return c->curr_rec;
}

void
grn_dat_cursor_close(grn_ctx *ctx, grn_dat_cursor *c)
{
  if (c) {
    grn_dat_cursor_fin(ctx, c);
    GRN_FREE(c);
  }
}

int
grn_dat_cursor_get_key(grn_ctx *ctx, grn_dat_cursor *c, const void **key)
{
  if (c) {
    const grn::dat::Key &key_ref = *static_cast<const grn::dat::Key *>(c->key);
    if (key_ref.is_valid()) {
      *key = key_ref.ptr();
      return (int)key_ref.length();
    }
  }
  return 0;
}

grn_rc
grn_dat_cursor_delete(grn_ctx *ctx, grn_dat_cursor *c,
                      grn_table_delete_optarg *optarg)
{
  if (!c || !c->cursor) {
    return GRN_INVALID_ARGUMENT;
  } else if (!grn_dat_open_trie_if_needed(ctx, c->dat)) {
    return ctx->rc;
  }
  grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(c->dat->trie);
  if (!trie) {
    return GRN_INVALID_ARGUMENT;
  }
  try {
    if (trie->remove(c->curr_rec)) {
      return GRN_SUCCESS;
    }
  } catch (const grn::dat::Exception &ex) {
    ERR(grn_dat_translate_error_code(ex.code()),
        "grn::dat::Trie::remove failed: %s",
        ex.what());
    return GRN_INVALID_ARGUMENT;
  }
  return GRN_INVALID_ARGUMENT;
}

grn_id
grn_dat_curr_id(grn_ctx *ctx, grn_dat *dat)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return GRN_ID_NIL;
  }
  const grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (trie) {
    return trie->max_key_id();
  }
  return GRN_ID_NIL;
}

grn_rc
grn_dat_truncate(grn_ctx *ctx, grn_dat *dat)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  }
  const grn::dat::Trie * const trie = static_cast<const grn::dat::Trie *>(dat->trie);
  if (!trie || !trie->max_key_id()) {
    return GRN_SUCCESS;
  }

  char trie_path[PATH_MAX];
  grn_dat_generate_trie_path(grn_io_path(dat->io), trie_path, dat->header->file_id + 1);
  try {
    grn::dat::Trie().create(trie_path);
  } catch (const grn::dat::Exception &ex) {
    const grn_rc error_code = grn_dat_translate_error_code(ex.code());
    ERR(error_code, "grn::dat::Trie::create failed: %s", ex.what());
    return error_code;
  }
  ++dat->header->file_id;
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  }
  return GRN_SUCCESS;
}

const char *
_grn_dat_key(grn_ctx *ctx, grn_dat *dat, grn_id id, uint32_t *key_size)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    *key_size = 0;
    return NULL;
  }
  const grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (!trie) {
    *key_size = 0;
    return NULL;
  }
  const grn::dat::Key &key = trie->ith_key(id);
  if (!key.is_valid()) {
    *key_size = 0;
    return NULL;
  }
  *key_size = key.length();
  return static_cast<const char *>(key.ptr());
}

grn_id
grn_dat_next(grn_ctx *ctx, grn_dat *dat, grn_id id)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return GRN_ID_NIL;
  }
  const grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (!trie) {
    return GRN_ID_NIL;
  }
  while (id < trie->max_key_id()) {
    if (trie->ith_key(++id).is_valid()) {
      return id;
    }
  }
  return GRN_ID_NIL;
}

grn_id
grn_dat_at(grn_ctx *ctx, grn_dat *dat, grn_id id)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return GRN_ID_NIL;
  }
  const grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (!trie) {
    return GRN_ID_NIL;
  }
  const grn::dat::Key &key = trie->ith_key(id);
  if (!key.is_valid()) {
    return GRN_ID_NIL;
  }
  return id;
}

grn_rc
grn_dat_clear_status_flags(grn_ctx *ctx, grn_dat *dat)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  }
  grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (!trie) {
    return GRN_INVALID_ARGUMENT;
  }
  trie->clear_status_flags();
  return GRN_SUCCESS;
}

grn_rc
grn_dat_repair(grn_ctx *ctx, grn_dat *dat)
{
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  }
  const grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
  if (!trie) {
    return GRN_INVALID_ARGUMENT;
  }

  char trie_path[PATH_MAX];
  grn_dat_generate_trie_path(grn_io_path(dat->io), trie_path, dat->header->file_id + 1);
  try {
    grn::dat::Trie().repair(*trie, trie_path);
  } catch (const grn::dat::Exception &ex) {
    const grn_rc error_code = grn_dat_translate_error_code(ex.code());
    ERR(error_code, "grn::dat::Trie::create failed: %s", ex.what());
    return error_code;
  }
  ++dat->header->file_id;
  if (!grn_dat_open_trie_if_needed(ctx, dat)) {
    return ctx->rc;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_dat_flush(grn_ctx *ctx, grn_dat *dat)
{
  if (!dat->io) {
    return GRN_SUCCESS;
  }

  grn_rc rc = grn_io_flush(ctx, dat->io);
  if (rc != GRN_SUCCESS) {
    return rc;
  }

  if (dat->trie) {
    grn::dat::Trie * const trie = static_cast<grn::dat::Trie *>(dat->trie);
    try {
      trie->flush();
    } catch (const grn::dat::Exception &ex) {
      const grn_rc error_code = grn_dat_translate_error_code(ex.code());
      if (error_code == GRN_INPUT_OUTPUT_ERROR) {
        SERR("grn::dat::Trie::flush failed: %s", ex.what());
      } else {
        ERR(error_code, "grn::dat::Trie::flush failed: %s", ex.what());
      }
      return error_code;
    }
  }

  return GRN_SUCCESS;
}

grn_rc
grn_dat_dirty(grn_ctx *ctx, grn_dat *dat)
{
  if (!dat->io) {
    return GRN_SUCCESS;
  }

  grn_rc rc = GRN_SUCCESS;

  {
    CriticalSection critical_section(&dat->lock);
    if (!dat->is_dirty) {
      uint32_t n_dirty_opens;
      dat->is_dirty = GRN_TRUE;
      GRN_ATOMIC_ADD_EX(&(dat->header->n_dirty_opens), 1, n_dirty_opens);
      rc = grn_io_flush(ctx, dat->io);
    }
  }

  return rc;
}

grn_bool
grn_dat_is_dirty(grn_ctx *ctx, grn_dat *dat)
{
  if (!dat->header) {
    return GRN_FALSE;
  }

  return dat->header->n_dirty_opens > 0;
}

grn_rc
grn_dat_clean(grn_ctx *ctx, grn_dat *dat)
{
  grn_rc rc = GRN_SUCCESS;

  if (!dat->io) {
    return rc;
  }

  {
    CriticalSection critical_section(&dat->lock);
    if (dat->is_dirty) {
      uint32_t n_dirty_opens;
      dat->is_dirty = GRN_FALSE;
      GRN_ATOMIC_ADD_EX(&(dat->header->n_dirty_opens), -1, n_dirty_opens);
      rc = grn_io_flush(ctx, dat->io);
    }
  }

  return rc;
}

grn_rc
grn_dat_clear_dirty(grn_ctx *ctx, grn_dat *dat)
{
  grn_rc rc = GRN_SUCCESS;

  if (!dat->io) {
    return rc;
  }

  {
    CriticalSection critical_section(&dat->lock);
    dat->is_dirty = GRN_FALSE;
    dat->header->n_dirty_opens = 0;
    rc = grn_io_flush(ctx, dat->io);
  }

  return rc;
}

grn_bool
grn_dat_is_corrupt(grn_ctx *ctx, grn_dat *dat)
{
  if (!dat->io) {
    return GRN_FALSE;
  }

  {
    CriticalSection critical_section(&dat->lock);

    if (grn_io_is_corrupt(ctx, dat->io)) {
      return GRN_TRUE;
    }

    if (dat->header->file_id == 0) {
      return GRN_FALSE;
    }

    char trie_path[PATH_MAX];
    grn_dat_generate_trie_path(grn_io_path(dat->io),
                               trie_path,
                               dat->header->file_id);
    struct stat stat;
    if (::stat(trie_path, &stat) != 0) {
      SERR("[dat][corrupt] used path doesn't exist: <%s>",
           trie_path);
      return GRN_TRUE;
    }
  }

  return GRN_FALSE;
}

size_t
grn_dat_get_disk_usage(grn_ctx *ctx, grn_dat *dat)
{
  if (!dat->io) {
    return 0;
  }

  {
    CriticalSection critical_section(&dat->lock);
    size_t usage;

    usage = grn_io_get_disk_usage(ctx, dat->io);

    if (dat->header->file_id == 0) {
      return usage;
    }

    char trie_path[PATH_MAX];
    grn_dat_generate_trie_path(grn_io_path(dat->io),
                               trie_path,
                               dat->header->file_id);
    struct stat stat;
    if (::stat(trie_path, &stat) == 0) {
      usage += stat.st_size;
    }

    return usage;
  }
}

}  // extern "C"
