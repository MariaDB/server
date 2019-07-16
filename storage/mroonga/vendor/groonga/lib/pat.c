/* -*- c-basic-offset: 2 -*- */
/*
  Copyright(C) 2009-2017 Brazil

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
#include <string.h>
#include <limits.h>
#include "grn_pat.h"
#include "grn_output.h"
#include "grn_util.h"
#include "grn_normalizer.h"

#define GRN_PAT_DELETED (GRN_ID_MAX + 1)

#define GRN_PAT_SEGMENT_SIZE 0x400000
#define W_OF_KEY_IN_A_SEGMENT 22
#define W_OF_PAT_IN_A_SEGMENT 18
#define W_OF_SIS_IN_A_SEGMENT 19
#define KEY_MASK_IN_A_SEGMENT 0x3fffff
#define PAT_MASK_IN_A_SEGMENT 0x3ffff
#define SIS_MASK_IN_A_SEGMENT 0x7ffff
#define SEG_NOT_ASSIGNED 0xffff
#define GRN_PAT_MAX_SEGMENT 0x1000
#define GRN_PAT_MDELINFOS (GRN_PAT_NDELINFOS - 1)

#define GRN_PAT_BIN_KEY 0x70000

typedef struct {
  grn_id lr[2];
  /*
    lr[0]: the left node.
    lr[1]: the right node.

    The left node has 0 at the nth bit at the nth byte.
    The right node has 1 at the nth bit at the nth byte.
    'check' value indicate 'at the nth bit at the nth byte'.

    The both available nodes has larger check value rather
    than the current node.

    The first node (PAT_AT(pat, GRN_ID_NIL, node)) has only
    the right node and the node is the start point.
   */
  uint32_t key;
  /*
    PAT_IMD(node) == 0: key bytes offset in memory map.
    PAT_IMD(node) == 1: the key bytes.
   */
  uint16_t check;
  /*
    nth byte: 12, nth bit: 3, terminated: 1

    nth byte is different in key bytes: (check >> 4): max == 4095
    the left most byte is the 0th byte and the right most byte is the 11th byte.

    nth bit is different in nth byte: ((check >> 1) & 0b111)
    the left most bit is the 0th bit and the right most bit is the 7th bit.

    terminated: (check & 0b1)
    terminated == 1: key is terminated.
   */
  uint16_t bits;
  /* length: 13, immediate: 1, deleting: 1 */
} pat_node;

#define PAT_DELETING  (1<<1)
#define PAT_IMMEDIATE (1<<2)

#define PAT_DEL(x) ((x)->bits & PAT_DELETING)
#define PAT_IMD(x) ((x)->bits & PAT_IMMEDIATE)
#define PAT_LEN(x) (((x)->bits >> 3) + 1)
#define PAT_CHK(x) ((x)->check)
#define PAT_DEL_ON(x) ((x)->bits |= PAT_DELETING)
#define PAT_IMD_ON(x) ((x)->bits |= PAT_IMMEDIATE)
#define PAT_DEL_OFF(x) ((x)->bits &= ~PAT_DELETING)
#define PAT_IMD_OFF(x) ((x)->bits &= ~PAT_IMMEDIATE)
#define PAT_LEN_SET(x,v) ((x)->bits = ((x)->bits & ((1<<3) - 1))|(((v) - 1) << 3))
#define PAT_CHK_SET(x,v) ((x)->check = (v))

typedef struct {
  grn_id children;
  grn_id sibling;
} sis_node;

enum {
  segment_key = 0,
  segment_pat = 1,
  segment_sis = 2
};

void grn_p_pat_node(grn_ctx *ctx, grn_pat *pat, pat_node *node);

/* error utilities */
inline static int
grn_pat_name(grn_ctx *ctx, grn_pat *pat, char *buffer, int buffer_size)
{
  int name_size;

  if (DB_OBJ(pat)->id == GRN_ID_NIL) {
    grn_strcpy(buffer, buffer_size, "(anonymous)");
    name_size = strlen(buffer);
  } else {
    name_size = grn_obj_name(ctx, (grn_obj *)pat, buffer, buffer_size);
  }

  return name_size;
}

/* bit operation */

#define nth_bit(key,n,l) ((((key)[(n)>>4]) >> (7 - (((n)>>1) & 7))) & 1)

/* segment operation */

/* patricia array operation */

#define PAT_AT(pat,id,n) do {\
  int flags = 0;\
  GRN_IO_ARRAY_AT(pat->io, segment_pat, id, &flags, n);\
} while (0)

inline static pat_node *
pat_get(grn_ctx *ctx, grn_pat *pat, grn_id id)
{
  pat_node *res;
  int flags = GRN_TABLE_ADD;
  if (id > GRN_ID_MAX) { return NULL; }
  GRN_IO_ARRAY_AT(pat->io, segment_pat, id, &flags, res);
  return res;
}

/* sis operation */

inline static sis_node *
sis_at(grn_ctx *ctx, grn_pat *pat, grn_id id)
{
  sis_node *res;
  int flags = 0;
  if (id > GRN_ID_MAX) { return NULL; }
  GRN_IO_ARRAY_AT(pat->io, segment_sis, id, &flags, res);
  return res;
}

inline static sis_node *
sis_get(grn_ctx *ctx, grn_pat *pat, grn_id id)
{
  sis_node *res;
  int flags = GRN_TABLE_ADD;
  if (id > GRN_ID_MAX) { return NULL; }
  GRN_IO_ARRAY_AT(pat->io, segment_sis, id, &flags, res);
  return res;
}

#define MAX_LEVEL 16

static void
sis_collect(grn_ctx *ctx, grn_pat *pat, grn_hash *h, grn_id id, uint32_t level)
{
  uint32_t *offset;
  sis_node *sl = sis_at(ctx, pat, id);
  if (sl) {
    grn_id sid = sl->children;
    while (sid && sid != id) {
      if (grn_hash_add(ctx, h, &sid, sizeof(grn_id), (void **) &offset, NULL)) {
        *offset = level;
        if (level < MAX_LEVEL) { sis_collect(ctx, pat, h, sid, level + 1); }
        if (!(sl = sis_at(ctx, pat, sid))) { break; }
        sid = sl->sibling;
      } else {
        /* todo : must be handled */
      }
    }
  }
}

/* key operation */

#define KEY_AT(pat,pos,ptr,addp) do {\
  int flags = addp;\
  GRN_IO_ARRAY_AT(pat->io, segment_key, pos, &flags, ptr);\
} while (0)

inline static uint32_t
key_put(grn_ctx *ctx, grn_pat *pat, const uint8_t *key, uint32_t len)
{
  uint32_t res, ts;
//  if (len >= GRN_PAT_SEGMENT_SIZE) { return 0; /* error */ }
  res = pat->header->curr_key;
  if (res < GRN_PAT_MAX_TOTAL_KEY_SIZE &&
      len > GRN_PAT_MAX_TOTAL_KEY_SIZE - res) {
    char name[GRN_TABLE_MAX_KEY_SIZE];
    int name_size;
    name_size = grn_pat_name(ctx, pat, name, GRN_TABLE_MAX_KEY_SIZE);
    ERR(GRN_NOT_ENOUGH_SPACE,
        "[pat][key][put] total key size is over: <%.*s>: "
        "max=%u: current=%u: new key size=%u",
        name_size, name,
        GRN_PAT_MAX_TOTAL_KEY_SIZE,
        res,
        len);
    return 0;
  }

  ts = (res + len) >> W_OF_KEY_IN_A_SEGMENT;
  if (res >> W_OF_KEY_IN_A_SEGMENT != ts) {
    res = pat->header->curr_key = ts << W_OF_KEY_IN_A_SEGMENT;
  }
  {
    uint8_t *dest;
    KEY_AT(pat, res, dest, GRN_TABLE_ADD);
    if (!dest) {
      char name[GRN_TABLE_MAX_KEY_SIZE];
      int name_size;
      name_size = grn_pat_name(ctx, pat, name, GRN_TABLE_MAX_KEY_SIZE);
      ERR(GRN_NO_MEMORY_AVAILABLE,
          "[pat][key][put] failed to allocate memory for new key: <%.*s>: "
          "new offset:%u key size:%u",
          name_size, name,
          res,
          len);
      return 0;
    }
    grn_memcpy(dest, key, len);
  }
  pat->header->curr_key += len;
  return res;
}

inline static uint8_t *
pat_node_get_key(grn_ctx *ctx, grn_pat *pat, pat_node *n)
{
  if (PAT_IMD(n)) {
    return (uint8_t *) &n->key;
  } else {
    uint8_t *res;
    KEY_AT(pat, n->key, res, 0);
    return res;
  }
}

inline static grn_rc
pat_node_set_key(grn_ctx *ctx, grn_pat *pat, pat_node *n, const uint8_t *key, uint32_t len)
{
  grn_rc rc;
  if (!key || !len) { return GRN_INVALID_ARGUMENT; }
  PAT_LEN_SET(n, len);
  if (len <= sizeof(uint32_t)) {
    PAT_IMD_ON(n);
    grn_memcpy(&n->key, key, len);
    rc = GRN_SUCCESS;
  } else {
    PAT_IMD_OFF(n);
    n->key = key_put(ctx, pat, key, len);
    rc = ctx->rc;
  }
  return rc;
}

/* delinfo operation */

enum {
  /* The delinfo is currently not used. */
  DL_EMPTY = 0,
  /*
   * stat->d refers to a deleting node (in a tree).
   * The deletion requires an additional operation.
   */
  DL_PHASE1,
  /*
   * stat->d refers to a deleted node (not in a tree).
   * The node is pending for safety.
   */
  DL_PHASE2
};

inline static grn_pat_delinfo *
delinfo_search(grn_pat *pat, grn_id id)
{
  int i;
  grn_pat_delinfo *di;
  for (i = (pat->header->curr_del2) & GRN_PAT_MDELINFOS;
       i != pat->header->curr_del;
       i = (i + 1) & GRN_PAT_MDELINFOS) {
    di = &pat->header->delinfos[i];
    if (di->stat != DL_PHASE1) { continue; }
    if (di->ld == id) { return di; }
    if (di->d == id) { return di; }
  }
  return NULL;
}

inline static grn_rc
delinfo_turn_2(grn_ctx *ctx, grn_pat *pat, grn_pat_delinfo *di)
{
  grn_id d, *p = NULL;
  pat_node *ln, *dn;
  // grn_log("delinfo_turn_2> di->d=%d di->ld=%d stat=%d", di->d, di->ld, di->stat);
  if (di->stat != DL_PHASE1) {
    return GRN_SUCCESS;
  }
  PAT_AT(pat, di->ld, ln);
  if (!ln) {
    return GRN_INVALID_ARGUMENT;
  }
  d = di->d;
  if (!d) {
    return GRN_INVALID_ARGUMENT;
  }
  PAT_AT(pat, d, dn);
  if (!dn) {
    return GRN_INVALID_ARGUMENT;
  }
  PAT_DEL_OFF(ln);
  PAT_DEL_OFF(dn);
  {
    grn_id *p0;
    pat_node *rn;
    int c0 = -1, c;
    uint32_t len = PAT_LEN(dn) * 16;
    const uint8_t *key = pat_node_get_key(ctx, pat, dn);
    if (!key) {
      return GRN_INVALID_ARGUMENT;
    }
    PAT_AT(pat, 0, rn);
    p0 = &rn->lr[1];
    for (;;) {
      grn_id r = *p0;
      if (!r) {
        break;
      }
      if (r == d) {
        p = p0;
        break;
      }
      PAT_AT(pat, r, rn);
      if (!rn) {
        return GRN_FILE_CORRUPT;
      }
      c = PAT_CHK(rn);
      if (c <= c0 || len <= c) {
        break;
      }
      if (c & 1) {
        p0 = (c + 1 < len) ? &rn->lr[1] : &rn->lr[0];
      } else {
        p0 = &rn->lr[nth_bit((uint8_t *)key, c, len)];
      }
      c0 = c;
    }
  }
  if (p) {
    PAT_CHK_SET(ln, PAT_CHK(dn));
    ln->lr[1] = dn->lr[1];
    ln->lr[0] = dn->lr[0];
    *p = di->ld;
  } else {
    /* debug */
    int j;
    grn_id dd;
    grn_pat_delinfo *ddi;
    GRN_LOG(ctx, GRN_LOG_DEBUG, "failed to find d=%d", d);
    for (j = (pat->header->curr_del2 + 1) & GRN_PAT_MDELINFOS;
         j != pat->header->curr_del;
         j = (j + 1) & GRN_PAT_MDELINFOS) {
      ddi = &pat->header->delinfos[j];
      if (ddi->stat != DL_PHASE1) { continue; }
      PAT_AT(pat, ddi->ld, ln);
      if (!ln) { continue; }
      if (!(dd = ddi->d)) { continue; }
      if (d == ddi->ld) {
        GRN_LOG(ctx, GRN_LOG_DEBUG, "found!!!, d(%d) become ld of (%d)", d, dd);
      }
    }
    /* debug */
  }
  di->stat = DL_PHASE2;
  di->d = d;
  // grn_log("delinfo_turn_2< di->d=%d di->ld=%d", di->d, di->ld);
  return GRN_SUCCESS;
}

inline static grn_rc
delinfo_turn_3(grn_ctx *ctx, grn_pat *pat, grn_pat_delinfo *di)
{
  pat_node *dn;
  uint32_t size;
  if (di->stat != DL_PHASE2) { return GRN_SUCCESS; }
  PAT_AT(pat, di->d, dn);
  if (!dn) { return GRN_INVALID_ARGUMENT; }
  if (di->shared) {
    PAT_IMD_ON(dn);
    size = 0;
  } else {
    if (PAT_IMD(dn)) {
      size = 0;
    } else {
      size = PAT_LEN(dn);
    }
  }
  di->stat = DL_EMPTY;
  //  dn->lr[1] = GRN_PAT_DELETED;
  dn->lr[0] = pat->header->garbages[size];
  pat->header->garbages[size] = di->d;
  return GRN_SUCCESS;
}

inline static grn_pat_delinfo *
delinfo_new(grn_ctx *ctx, grn_pat *pat)
{
  grn_pat_delinfo *res = &pat->header->delinfos[pat->header->curr_del];
  uint32_t n = (pat->header->curr_del + 1) & GRN_PAT_MDELINFOS;
  int gap = ((n + GRN_PAT_NDELINFOS - pat->header->curr_del2) & GRN_PAT_MDELINFOS)
            - (GRN_PAT_NDELINFOS / 2);
  while (gap-- > 0) {
    if (delinfo_turn_2(ctx, pat, &pat->header->delinfos[pat->header->curr_del2])) {
      GRN_LOG(ctx, GRN_LOG_CRIT, "d2 failed: %d", pat->header->delinfos[pat->header->curr_del2].ld);
    }
    pat->header->curr_del2 = (pat->header->curr_del2 + 1) & GRN_PAT_MDELINFOS;
  }
  if (n == pat->header->curr_del3) {
    if (delinfo_turn_3(ctx, pat, &pat->header->delinfos[pat->header->curr_del3])) {
      GRN_LOG(ctx, GRN_LOG_CRIT, "d3 failed: %d", pat->header->delinfos[pat->header->curr_del3].ld);
    }
    pat->header->curr_del3 = (pat->header->curr_del3 + 1) & GRN_PAT_MDELINFOS;
  }
  pat->header->curr_del = n;
  return res;
}

/* pat operation */

inline static grn_pat *
_grn_pat_create(grn_ctx *ctx, grn_pat *pat,
                const char *path, uint32_t key_size,
                uint32_t value_size, uint32_t flags) {
  grn_io *io;
  pat_node *node0;
  struct grn_pat_header *header;
  uint32_t entry_size, w_of_element;
  grn_encoding encoding = ctx->encoding;
  if (flags & GRN_OBJ_KEY_WITH_SIS) {
    entry_size = sizeof(sis_node) + value_size;
  } else {
    entry_size = value_size;
  }
  for (w_of_element = 0; (1 << w_of_element) < entry_size; w_of_element++) {
    /* nop */
  }
  {
    grn_io_array_spec array_spec[3];
    array_spec[segment_key].w_of_element = 0;
    array_spec[segment_key].max_n_segments = 0x400;
    array_spec[segment_pat].w_of_element = 4;
    array_spec[segment_pat].max_n_segments = 1 << (30 - (22 - 4));
    array_spec[segment_sis].w_of_element = w_of_element;
    array_spec[segment_sis].max_n_segments = 1 << (30 - (22 - w_of_element));
    io = grn_io_create_with_array(ctx, path, sizeof(struct grn_pat_header),
                                  GRN_PAT_SEGMENT_SIZE, grn_io_auto, 3, array_spec);
  }
  if (!io) { return NULL; }
  if (encoding == GRN_ENC_DEFAULT) { encoding = grn_gctx.encoding; }
  header = grn_io_header(io);
  grn_io_set_type(io, GRN_TABLE_PAT_KEY);
  header->flags = flags;
  header->encoding = encoding;
  header->key_size = key_size;
  header->value_size = value_size;
  header->n_entries = 0;
  header->curr_rec = 0;
  header->curr_key = 0;
  header->curr_del = 0;
  header->curr_del2 = 0;
  header->curr_del3 = 0;
  header->n_garbages = 0;
  header->tokenizer = GRN_ID_NIL;
  if (header->flags & GRN_OBJ_KEY_NORMALIZE) {
    header->flags &= ~GRN_OBJ_KEY_NORMALIZE;
    pat->normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
    header->normalizer = grn_obj_id(ctx, pat->normalizer);
  } else {
    pat->normalizer = NULL;
    header->normalizer = GRN_ID_NIL;
  }
  header->truncated = GRN_FALSE;
  GRN_PTR_INIT(&(pat->token_filters), GRN_OBJ_VECTOR, GRN_ID_NIL);
  pat->io = io;
  pat->header = header;
  pat->key_size = key_size;
  pat->value_size = value_size;
  pat->tokenizer = NULL;
  pat->encoding = encoding;
  pat->obj.header.flags = header->flags;
  if (!(node0 = pat_get(ctx, pat, 0))) {
    grn_io_close(ctx, io);
    return NULL;
  }
  node0->lr[1] = 0;
  node0->lr[0] = 0;
  node0->key = 0;
  return pat;
}

grn_pat *
grn_pat_create(grn_ctx *ctx, const char *path, uint32_t key_size,
               uint32_t value_size, uint32_t flags)
{
  grn_pat *pat;
  if (!(pat = GRN_CALLOC(sizeof(grn_pat)))) {
    return NULL;
  }
  GRN_DB_OBJ_SET_TYPE(pat, GRN_TABLE_PAT_KEY);
  if (!_grn_pat_create(ctx, pat, path, key_size, value_size, flags)) {
    GRN_FREE(pat);
    return NULL;
  }
  pat->cache = NULL;
  pat->cache_size = 0;
  pat->is_dirty = GRN_FALSE;
  CRITICAL_SECTION_INIT(pat->lock);
  return pat;
}

/*
 grn_pat_cache_enable() and grn_pat_cache_disable() are not thread-safe.
 So far, they can be used only from single threaded programs.
 */

grn_rc
grn_pat_cache_enable(grn_ctx *ctx, grn_pat *pat, uint32_t cache_size)
{
  if (pat->cache || pat->cache_size) {
    ERR(GRN_INVALID_ARGUMENT, "cache is already enabled");
    return ctx->rc;
  }
  if (cache_size & (cache_size - 1)) {
    ERR(GRN_INVALID_ARGUMENT, "cache_size(%u) must be a power of two", cache_size);
    return ctx->rc;
  }
  if (!(pat->cache = GRN_CALLOC(cache_size * sizeof(grn_id)))) {
    return ctx->rc;
  }
  pat->cache_size = cache_size;
  return GRN_SUCCESS;
}

void
grn_pat_cache_disable(grn_ctx *ctx, grn_pat *pat)
{
  if (pat->cache) {
    GRN_FREE(pat->cache);
    pat->cache_size = 0;
    pat->cache = NULL;
  }
}

grn_pat *
grn_pat_open(grn_ctx *ctx, const char *path)
{
  grn_io *io;
  grn_pat *pat;
  pat_node *node0;
  struct grn_pat_header *header;
  uint32_t io_type;
  io = grn_io_open(ctx, path, grn_io_auto);
  if (!io) { return NULL; }
  header = grn_io_header(io);
  io_type = grn_io_get_type(io);
  if (io_type != GRN_TABLE_PAT_KEY) {
    ERR(GRN_INVALID_FORMAT, "[table][pat] file type must be %#04x: <%#04x>",
        GRN_TABLE_PAT_KEY, io_type);
    grn_io_close(ctx, io);
    return NULL;
  }
  if (!(pat = GRN_MALLOC(sizeof(grn_pat)))) {
    grn_io_close(ctx, io);
    return NULL;
  }
  GRN_DB_OBJ_SET_TYPE(pat, GRN_TABLE_PAT_KEY);
  pat->io = io;
  pat->header = header;
  pat->key_size = header->key_size;
  pat->value_size = header->value_size;
  pat->encoding = header->encoding;
  pat->tokenizer = grn_ctx_at(ctx, header->tokenizer);
  if (header->flags & GRN_OBJ_KEY_NORMALIZE) {
    header->flags &= ~GRN_OBJ_KEY_NORMALIZE;
    pat->normalizer = grn_ctx_get(ctx, GRN_NORMALIZER_AUTO_NAME, -1);
    header->normalizer = grn_obj_id(ctx, pat->normalizer);
  } else {
    pat->normalizer = grn_ctx_at(ctx, header->normalizer);
  }
  GRN_PTR_INIT(&(pat->token_filters), GRN_OBJ_VECTOR, GRN_ID_NIL);
  pat->obj.header.flags = header->flags;
  PAT_AT(pat, 0, node0);
  if (!node0) {
    grn_io_close(ctx, io);
    GRN_FREE(pat);
    return NULL;
  }
  pat->cache = NULL;
  pat->cache_size = 0;
  pat->is_dirty = GRN_FALSE;
  CRITICAL_SECTION_INIT(pat->lock);
  return pat;
}

/*
 * grn_pat_error_if_truncated() logs an error and returns its error code if
 * a pat is truncated by another process.
 * Otherwise, this function returns GRN_SUCCESS.
 * Note that `ctx` and `pat` must be valid.
 *
 * FIXME: A pat should be reopened if possible.
 */
static grn_rc
grn_pat_error_if_truncated(grn_ctx *ctx, grn_pat *pat)
{
  if (pat->header->truncated) {
    ERR(GRN_FILE_CORRUPT,
        "pat is truncated, please unmap or reopen the database");
    return GRN_FILE_CORRUPT;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_pat_close(grn_ctx *ctx, grn_pat *pat)
{
  grn_rc rc;

  CRITICAL_SECTION_FIN(pat->lock);

  if (pat->is_dirty) {
    uint32_t n_dirty_opens;
    GRN_ATOMIC_ADD_EX(&(pat->header->n_dirty_opens), -1, n_dirty_opens);
  }

  if ((rc = grn_io_close(ctx, pat->io))) {
    ERR(rc, "grn_io_close failed");
  } else {
    grn_pvector_fin(ctx, &pat->token_filters);
    if (pat->cache) { grn_pat_cache_disable(ctx, pat); }
    GRN_FREE(pat);
  }

  return rc;
}

grn_rc
grn_pat_remove(grn_ctx *ctx, const char *path)
{
  if (!path) {
    ERR(GRN_INVALID_ARGUMENT, "path is null");
    return GRN_INVALID_ARGUMENT;
  }
  return grn_io_remove(ctx, path);
}

grn_rc
grn_pat_truncate(grn_ctx *ctx, grn_pat *pat)
{
  grn_rc rc;
  const char *io_path;
  char *path;
  uint32_t key_size, value_size, flags;

  rc = grn_pat_error_if_truncated(ctx, pat);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if ((io_path = grn_io_path(pat->io)) && *io_path != '\0') {
    if (!(path = GRN_STRDUP(io_path))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_path);
      return GRN_NO_MEMORY_AVAILABLE;
    }
  } else {
    path = NULL;
  }
  key_size = pat->key_size;
  value_size = pat->value_size;
  flags = pat->obj.header.flags;
  if (path) {
    pat->header->truncated = GRN_TRUE;
  }
  if ((rc = grn_io_close(ctx, pat->io))) { goto exit; }
  grn_pvector_fin(ctx, &pat->token_filters);
  pat->io = NULL;
  if (path && (rc = grn_io_remove(ctx, path))) { goto exit; }
  if (!_grn_pat_create(ctx, pat, path, key_size, value_size, flags)) {
    rc = GRN_UNKNOWN_ERROR;
  }
  if (pat->cache && pat->cache_size) {
    memset(pat->cache, 0, pat->cache_size * sizeof(grn_id));
  }
exit:
  if (path) { GRN_FREE(path); }
  return rc;
}

inline static grn_id
_grn_pat_add(grn_ctx *ctx, grn_pat *pat, const uint8_t *key, uint32_t size, uint32_t *new, uint32_t *lkey)
{
  grn_id r, r0, *p0, *p1 = NULL;
  pat_node *rn, *rn0;
  int c, c0 = -1, c1 = -1, len;
  uint32_t cache_id = 0;

  *new = 0;
  if (pat->cache) {
    const uint8_t *p = key;
    uint32_t length = size;
    for (cache_id = 0; length--; p++) { cache_id = (cache_id * 37) + *p; }
    cache_id &= (pat->cache_size - 1);
    if (pat->cache[cache_id]) {
      PAT_AT(pat, pat->cache[cache_id], rn);
      if (rn) {
        const uint8_t *k = pat_node_get_key(ctx, pat, rn);
        if (k && size == PAT_LEN(rn) && !memcmp(k, key, size)) {
          return pat->cache[cache_id];
        }
      }
    }
  }

  len = (int)size * 16;
  PAT_AT(pat, 0, rn0);
  p0 = &rn0->lr[1];
  if (*p0) {
    uint32_t size2;
    int xor, mask;
    const uint8_t *s, *d;
    for (;;) {
      if (!(r0 = *p0)) {
        if (!(s = pat_node_get_key(ctx, pat, rn0))) { return GRN_ID_NIL; }
        size2 = PAT_LEN(rn0);
        break;
      }
      PAT_AT(pat, r0, rn0);
      if (!rn0) { return GRN_ID_NIL; }
      if (c0 < rn0->check && rn0->check < len) {
        c1 = c0; c0 = rn0->check;
        p1 = p0;
        if (c0 & 1) {
          p0 = (c0 + 1 < len) ? &rn0->lr[1] : &rn0->lr[0];
        } else {
          p0 = &rn0->lr[nth_bit(key, c0, len)];
        }
      } else {
        if (!(s = pat_node_get_key(ctx, pat, rn0))) { return GRN_ID_NIL; }
        size2 = PAT_LEN(rn0);
        if (size == size2 && !memcmp(s, key, size)) {
          if (pat->cache) { pat->cache[cache_id] = r0; }
          return r0;
        }
        break;
      }
    }
    {
      uint32_t min = size > size2 ? size2 : size;
      for (c = 0, d = key; min && *s == *d; c += 16, s++, d++, min--);
      if (min) {
        for (xor = *s ^ *d, mask = 0x80; !(xor & mask); mask >>= 1, c += 2);
      } else {
        c--;
      }
    }
    if (c == c0 && !*p0) {
      if (c < len - 2) { c += 2; }
    } else {
      if (c < c0) {
        if (c > c1) {
          p0 = p1;
        } else {
          PAT_AT(pat, 0, rn0);
          p0 = &rn0->lr[1];
          while ((r0 = *p0)) {
            PAT_AT(pat, r0, rn0);
            if (!rn0) { return GRN_ID_NIL; }
            c0 = PAT_CHK(rn0);
            if (c < c0) { break; }
            if (c0 & 1) {
              p0 = (c0 + 1 < len) ? &rn0->lr[1] : &rn0->lr[0];
            } else {
              p0 = &rn0->lr[nth_bit(key, c0, len)];
            }
          }
        }
      }
    }
    if (c >= len) { return GRN_ID_NIL; }
  } else {
    c = len - 2;
  }
  {
    uint32_t size2 = size > sizeof(uint32_t) ? size : 0;
    if (*lkey && size2) {
      if (pat->header->garbages[0]) {
        r = pat->header->garbages[0];
        PAT_AT(pat, r, rn);
        if (!rn) { return GRN_ID_NIL; }
        pat->header->n_entries++;
        pat->header->n_garbages--;
        pat->header->garbages[0] = rn->lr[0];
      } else {
        r = pat->header->curr_rec + 1;
        rn = pat_get(ctx, pat, r);
        if (!rn) { return GRN_ID_NIL; }
        pat->header->curr_rec = r;
        pat->header->n_entries++;
      }
      PAT_IMD_OFF(rn);
      PAT_LEN_SET(rn, size);
      rn->key = *lkey;
    } else {
      if (pat->header->garbages[size2]) {
        uint8_t *keybuf;
        r = pat->header->garbages[size2];
        PAT_AT(pat, r, rn);
        if (!rn) { return GRN_ID_NIL; }
        if (!(keybuf = pat_node_get_key(ctx, pat, rn))) { return GRN_ID_NIL; }
        pat->header->n_entries++;
        pat->header->n_garbages--;
        pat->header->garbages[size2] = rn->lr[0];
        PAT_LEN_SET(rn, size);
        grn_memcpy(keybuf, key, size);
      } else {
        r = pat->header->curr_rec + 1;
        rn = pat_get(ctx, pat, r);
        if (!rn) { return GRN_ID_NIL; }
        if (pat_node_set_key(ctx, pat, rn, key, size)) { return GRN_ID_NIL; }
        pat->header->curr_rec = r;
        pat->header->n_entries++;
      }
      *lkey = rn->key;
    }
  }
  PAT_CHK_SET(rn, c);
  PAT_DEL_OFF(rn);
  if ((c & 1) ? (c + 1 < len) : nth_bit(key, c, len)) {
    rn->lr[1] = r;
    rn->lr[0] = *p0;
  } else {
    rn->lr[1] = *p0;
    rn->lr[0] = r;
  }
  // smp_wmb();
  *p0 = r;
  *new = 1;
  if (pat->cache) { pat->cache[cache_id] = r; }
  return r;
}

inline static grn_bool
chop(grn_ctx *ctx, grn_pat *pat, const char **key, const char *end, uint32_t *lkey)
{
  size_t len = grn_charlen(ctx, *key, end);
  if (len) {
    *lkey += len;
    *key += len;
    return (end - *key) > 0;
  } else {
    return GRN_FALSE;
  }
}

#define MAX_FIXED_KEY_SIZE (sizeof(int64_t))

#define KEY_NEEDS_CONVERT(pat,size) \
  (!((pat)->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE) && (size) <= MAX_FIXED_KEY_SIZE)

#define KEY_ENC(pat,keybuf,key,size) do {\
  switch ((pat)->obj.header.flags & GRN_OBJ_KEY_MASK) {\
  case GRN_OBJ_KEY_UINT :\
    if (((pat)->obj.header.domain != GRN_DB_TOKYO_GEO_POINT) &&\
        ((pat)->obj.header.domain != GRN_DB_WGS84_GEO_POINT)) {\
      grn_hton((keybuf), (key), (size));\
      break;\
    }\
  case GRN_OBJ_KEY_GEO_POINT :\
    grn_gton((keybuf), (key), (size));\
    break;\
  case GRN_OBJ_KEY_INT :\
    grn_hton((keybuf), (key), (size));\
    *((uint8_t *)(keybuf)) ^= 0x80;\
    break;\
  case GRN_OBJ_KEY_FLOAT :\
    if ((size) == sizeof(int64_t)) {\
      int64_t v = *(int64_t *)(key);\
      v ^= ((v >> 63)|(1LL << 63));\
      grn_hton((keybuf), &v, (size));\
    }\
    break;\
  }\
} while (0)

#define KEY_DEC(pat,keybuf,key,size) do {\
  switch ((pat)->obj.header.flags & GRN_OBJ_KEY_MASK) {\
  case GRN_OBJ_KEY_UINT :\
    if (((pat)->obj.header.domain != GRN_DB_TOKYO_GEO_POINT) &&\
        ((pat)->obj.header.domain != GRN_DB_WGS84_GEO_POINT)) {\
      grn_ntoh((keybuf), (key), (size));\
      break;\
    }\
  case GRN_OBJ_KEY_GEO_POINT :\
    grn_ntog((keybuf), (key), (size));\
    break;\
  case GRN_OBJ_KEY_INT :\
    grn_ntohi((keybuf), (key), (size));\
    break;\
  case GRN_OBJ_KEY_FLOAT :\
    if ((size) == sizeof(int64_t)) {\
      int64_t v;\
      grn_hton(&v, (key), (size));\
      *((int64_t *)(keybuf)) = v ^ (((v^(1LL<<63))>> 63)|(1LL<<63));  \
    }\
    break;\
  }\
} while (0)

#define KEY_ENCODE(pat,keybuf,key,size) do {\
  if (KEY_NEEDS_CONVERT(pat,size)) {\
    KEY_ENC((pat), (keybuf), (key), (size));\
    (key) = (keybuf);\
  }\
} while (0)

grn_id
grn_pat_add(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size,
            void **value, int *added)
{
  uint32_t new, lkey = 0;
  grn_id r0;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  if (!key || !key_size) { return GRN_ID_NIL; }
  if (key_size > GRN_TABLE_MAX_KEY_SIZE) {
    ERR(GRN_INVALID_ARGUMENT, "too long key: (%u)", key_size);
    return GRN_ID_NIL;
  }
  KEY_ENCODE(pat, keybuf, key, key_size);
  r0 = _grn_pat_add(ctx, pat, (uint8_t *)key, key_size, &new, &lkey);
  if (r0 == GRN_ID_NIL) { return GRN_ID_NIL; }
  if (added) { *added = new; }
  if (r0 && (pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) &&
      (*((uint8_t *)key) & 0x80)) { // todo: refine!!
    sis_node *sl, *sr;
    grn_id l = r0, r;
    if (new && (sl = sis_get(ctx, pat, l))) {
      const char *sis = key, *end = sis + key_size;
      sl->children = l;
      sl->sibling = 0;
      while (chop(ctx, pat, &sis, end, &lkey)) {
        if (!(*sis & 0x80)) { break; }
        if (!(r = _grn_pat_add(ctx, pat, (uint8_t *)sis, end - sis, &new, &lkey))) {
          break;
        }
        if (!(sr = sis_get(ctx, pat, r))) { break; }
        if (new) {
          sl->sibling = r;
          sr->children = l;
          sr->sibling = 0;
        } else {
          sl->sibling = sr->children;
          sr->children = l;
          break;
        }
        l = r;
        sl = sr;
      }
    }
  }
  if (r0 && value) {
    byte *v = (byte *)sis_get(ctx, pat, r0);
    if (pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) {
      *value = v + sizeof(sis_node);
    } else {
      *value = v;
    }
  }
  return r0;
}

inline static grn_id
_grn_pat_get(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size, void **value)
{
  grn_id r;
  pat_node *rn;
  int c0 = -1, c;
  uint32_t len = key_size * 16;
  PAT_AT(pat, 0, rn);
  for (r = rn->lr[1]; r;) {
    PAT_AT(pat, r, rn);
    if (!rn) { break; /* corrupt? */ }
    c = PAT_CHK(rn);
    if (len <= c) { break; }
    if (c <= c0) {
      const uint8_t *k = pat_node_get_key(ctx, pat, rn);
      if (k && key_size == PAT_LEN(rn) && !memcmp(k, key, key_size)) {
        if (value) {
          byte *v = (byte *)sis_get(ctx, pat, r);
          if (pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) {
            *value = v + sizeof(sis_node);
          } else {
            *value = v;
          }
        }
        return r;
      }
      break;
    }
    if (c & 1) {
      r = (c + 1 < len) ? rn->lr[1] : rn->lr[0];
    } else {
      r = rn->lr[nth_bit((uint8_t *)key, c, len)];
    }
    c0 = c;
  }
  return GRN_ID_NIL;
}

grn_id
grn_pat_get(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size, void **value)
{
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  KEY_ENCODE(pat, keybuf, key, key_size);
  return _grn_pat_get(ctx, pat, key, key_size, value);
}

grn_id
grn_pat_nextid(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size)
{
  grn_id r = GRN_ID_NIL;
  if (pat && key) {
    if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
      return GRN_ID_NIL;
    }
    if (!(r = pat->header->garbages[key_size > sizeof(uint32_t) ? key_size : 0])) {
      r = pat->header->curr_rec + 1;
    }
  }
  return r;
}

static void
get_tc(grn_ctx *ctx, grn_pat *pat, grn_hash *h, pat_node *rn)
{
  grn_id id;
  pat_node *node;
  id = rn->lr[1];
  if (id) {
    PAT_AT(pat, id, node);
    if (node) {
      if (PAT_CHK(node) > PAT_CHK(rn)) {
        get_tc(ctx, pat, h, node);
      } else {
        grn_hash_add(ctx, h, &id, sizeof(grn_id), NULL, NULL);
      }
    }
  }
  id = rn->lr[0];
  if (id) {
    PAT_AT(pat, id, node);
    if (node) {
      if (PAT_CHK(node) > PAT_CHK(rn)) {
        get_tc(ctx, pat, h, node);
      } else {
        grn_hash_add(ctx, h, &id, sizeof(grn_id), NULL, NULL);
      }
    }
  }
}

grn_rc
grn_pat_prefix_search(grn_ctx *ctx, grn_pat *pat,
                      const void *key, uint32_t key_size, grn_hash *h)
{
  int c0 = -1, c;
  const uint8_t *k;
  uint32_t len = key_size * 16;
  grn_id r;
  pat_node *rn;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  grn_rc rc = grn_pat_error_if_truncated(ctx, pat);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  KEY_ENCODE(pat, keybuf, key, key_size);
  PAT_AT(pat, 0, rn);
  r = rn->lr[1];
  while (r) {
    PAT_AT(pat, r, rn);
    if (!rn) { return GRN_FILE_CORRUPT; }
    c = PAT_CHK(rn);
    if (c0 < c && c < len - 1) {
      if (c & 1) {
        r = (c + 1 < len) ? rn->lr[1] : rn->lr[0];
      } else {
        r = rn->lr[nth_bit((uint8_t *)key, c, len)];
      }
      c0 = c;
      continue;
    }
    if (!(k = pat_node_get_key(ctx, pat, rn))) { break; }
    if (PAT_LEN(rn) < key_size) { break; }
    if (!memcmp(k, key, key_size)) {
      if (c >= len - 1) {
        get_tc(ctx, pat, h, rn);
      } else {
        grn_hash_add(ctx, h, &r, sizeof(grn_id), NULL, NULL);
      }
      return GRN_SUCCESS;
    }
    break;
  }
  return GRN_END_OF_DATA;
}

grn_hash *
grn_pat_prefix_search2(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size)
{
  grn_hash *h;
  if (!pat || !key) { return NULL; }
  if ((h = grn_hash_create(ctx, NULL, sizeof(grn_id), 0, 0))) {
    if (grn_pat_prefix_search(ctx, pat, key, key_size, h)) {
      grn_hash_close(ctx, h);
      h = NULL;
    }
  }
  return h;
}

grn_rc
grn_pat_suffix_search(grn_ctx *ctx, grn_pat *pat,
                      const void *key, uint32_t key_size, grn_hash *h)
{
  grn_id r;
  if ((r = grn_pat_get(ctx, pat, key, key_size, NULL))) {
    uint32_t *offset;
    if (grn_hash_add(ctx, h, &r, sizeof(grn_id), (void **) &offset, NULL)) {
      *offset = 0;
      if (pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) { sis_collect(ctx, pat, h, r, 1); }
      return GRN_SUCCESS;
    }
  }
  return GRN_END_OF_DATA;
}

grn_hash *
grn_pat_suffix_search2(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size)
{
  grn_hash *h;
  if (!pat || !key) { return NULL; }
  if ((h = grn_hash_create(ctx, NULL, sizeof(grn_id), sizeof(uint32_t), 0))) {
    if (grn_pat_suffix_search(ctx, pat, key, key_size, h)) {
      grn_hash_close(ctx, h);
      h = NULL;
    }
  }
  return h;
}

grn_id
grn_pat_lcp_search(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size)
{
  pat_node *rn;
  grn_id r, r2 = GRN_ID_NIL;
  uint32_t len = key_size * 16;
  int c0 = -1, c;
  if (!pat || !key) {
    return GRN_ID_NIL;
  }
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  if (!(pat->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE)) { return GRN_ID_NIL; }
  PAT_AT(pat, 0, rn);
  for (r = rn->lr[1]; r;) {
    PAT_AT(pat, r, rn);
    if (!rn) { break; /* corrupt? */ }
    c = PAT_CHK(rn);
    if (c <= c0) {
      if (PAT_LEN(rn) <= key_size) {
        uint8_t *p = pat_node_get_key(ctx, pat, rn);
        if (!p) { break; }
        if (!memcmp(p, key, PAT_LEN(rn))) { return r; }
      }
      break;
    }
    if (len <= c) { break; }
    if (c & 1) {
      uint8_t *p;
      pat_node *rn0;
      grn_id r0 = rn->lr[0];
      PAT_AT(pat, r0, rn0);
      if (!rn0) { break; /* corrupt? */ }
      p = pat_node_get_key(ctx, pat, rn0);
      if (!p) { break; }
      if (PAT_LEN(rn0) <= key_size && !memcmp(p, key, PAT_LEN(rn0))) { r2 = r0; }
      r = (c + 1 < len) ? rn->lr[1] : rn->lr[0];
    } else {
      r = rn->lr[nth_bit((uint8_t *)key, c, len)];
    }
    c0 = c;
  }
  return r2;
}

static grn_id
common_prefix_pat_node_get(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size)
{
  int c0 = -1, c;
  const uint8_t *k;
  uint32_t len = key_size * 16;
  grn_id r;
  pat_node *rn;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];

  KEY_ENCODE(pat, keybuf, key, key_size);
  PAT_AT(pat, 0, rn);
  r = rn->lr[1];
  while (r) {
    PAT_AT(pat, r, rn);
    if (!rn) { return GRN_ID_NIL; }
    c = PAT_CHK(rn);
    if (c0 < c && c < len - 1) {
      if (c & 1) {
        r = (c + 1 < len) ? rn->lr[1] : rn->lr[0];
      } else {
        r = rn->lr[nth_bit((uint8_t *)key, c, len)];
      }
      c0 = c;
      continue;
    }
    if (!(k = pat_node_get_key(ctx, pat, rn))) { break; }
    if (PAT_LEN(rn) < key_size) { break; }
    if (!memcmp(k, key, key_size)) {
      return r;
    }
    break;
  }
  return GRN_ID_NIL;
}

typedef struct {
  grn_id id;
  uint16_t distance;
} fuzzy_heap_node;

typedef struct {
  int n_entries;
  int limit;
  fuzzy_heap_node *nodes;
} fuzzy_heap;

static inline fuzzy_heap *
fuzzy_heap_open(grn_ctx *ctx, int max)
{
  fuzzy_heap *h = GRN_MALLOC(sizeof(fuzzy_heap));
  if (!h) { return NULL; }
  h->nodes = GRN_MALLOC(sizeof(fuzzy_heap_node) * max);
  if (!h->nodes) {
    GRN_FREE(h);
    return NULL;
  }
  h->n_entries = 0;
  h->limit = max;
  return h;
}

static inline grn_bool
fuzzy_heap_push(grn_ctx *ctx, fuzzy_heap *h, grn_id id, uint16_t distance)
{
  int n, n2;
  fuzzy_heap_node node = {id, distance};
  fuzzy_heap_node node2;
  if (h->n_entries >= h->limit) {
    int max = h->limit * 2;
    fuzzy_heap_node *nodes = GRN_REALLOC(h->nodes, sizeof(fuzzy_heap) * max);
    if (!h) {
      return GRN_FALSE;
    }
    h->limit = max;
    h->nodes = nodes;
  }
  h->nodes[h->n_entries] = node;
  n = h->n_entries++;
  while (n) {
    n2 = (n - 1) >> 1;
    if (h->nodes[n2].distance <= h->nodes[n].distance) { break; }
    node2 = h->nodes[n];
    h->nodes[n] = h->nodes[n2];
    h->nodes[n2] = node2;
    n = n2;
  }
  return GRN_TRUE;
}

static inline void
fuzzy_heap_close(grn_ctx *ctx, fuzzy_heap *h)
{
  GRN_FREE(h->nodes);
  GRN_FREE(h);
}

#define DIST(ox,oy) (dists[((lx + 1) * (oy)) + (ox)])

inline static uint16_t
calc_edit_distance_by_offset(grn_ctx *ctx,
                             const char *sx, const char *ex,
                             const char *sy, const char *ey,
                             uint16_t *dists, uint32_t lx,
                             uint32_t offset, uint32_t max_distance,
                             grn_bool *can_transition, int flags)
{
  uint32_t cx, cy, x, y;
  const char *px, *py;

  /* Skip already calculated rows */
  for (py = sy, y = 1; py < ey && (cy = grn_charlen(ctx, py, ey)); py += cy, y++) {
    if (py - sy >= offset) {
      break;
    }
  }
  for (; py < ey && (cy = grn_charlen(ctx, py, ey)); py += cy, y++) {
    /* children nodes will be no longer smaller than max distance
     * with only insertion costs.
     * This is end of row on allocated memory. */
    if (y > lx + max_distance) {
      *can_transition = GRN_FALSE;
      return max_distance + 1;
    }

    for (px = sx, x = 1; px < ex && (cx = grn_charlen(ctx, px, ex)); px += cx, x++) {
      if (cx == cy && !memcmp(px, py, cx)) {
        DIST(x, y) = DIST(x - 1, y - 1);
      } else {
        uint32_t a, b, c;
        a = DIST(x - 1, y) + 1;
        b = DIST(x, y - 1) + 1;
        c = DIST(x - 1, y - 1) + 1;
        DIST(x, y) = ((a < b) ? ((a < c) ? a : c) : ((b < c) ? b : c));
        if (flags & GRN_TABLE_FUZZY_SEARCH_WITH_TRANSPOSITION &&
            x > 1 && y > 1 &&
            cx == cy &&
            memcmp(px, py - cy, cx) == 0 &&
            memcmp(px - cx, py, cx) == 0) {
          uint32_t t = DIST(x - 2, y - 2) + 1;
          DIST(x, y) = ((DIST(x, y) < t) ? DIST(x, y) : t);
        }
      }
    }
  }
  if (lx) {
    /* If there is no cell which is smaller than equal to max distance on end of row,
     * children nodes will be no longer smaller than max distance */
    *can_transition = GRN_FALSE;
    for (x = 1; x <= lx; x++) {
      if (DIST(x, y - 1) <= max_distance) {
        *can_transition = GRN_TRUE;
        break;
      }
    }
  }
  return DIST(lx, y - 1);
}

typedef struct {
  const char *key;
  int key_length;
  grn_bool can_transition;
} fuzzy_node;

inline static void
_grn_pat_fuzzy_search(grn_ctx *ctx, grn_pat *pat, grn_id id,
                      const char *key, uint32_t key_size,
                      uint16_t *dists, uint32_t lx,
                      int last_check, fuzzy_node *last_node,
                      uint32_t max_distance, int flags, fuzzy_heap *heap)
{
  pat_node *node = NULL;
  int check, len;
  const char *k;
  uint32_t offset = 0;

  PAT_AT(pat, id, node);
  if (!node) {
    return;
  }
  check = PAT_CHK(node);
  len = PAT_LEN(node);
  k = pat_node_get_key(ctx, pat, node);

  if (check > last_check) {
    if (len >= last_node->key_length &&
        !memcmp(k, last_node->key, last_node->key_length)) {
      if (last_node->can_transition == GRN_FALSE) {
        return;
      }
    }
    _grn_pat_fuzzy_search(ctx, pat, node->lr[0],
                          key, key_size, dists, lx,
                          check, last_node,
                          max_distance, flags, heap);

    _grn_pat_fuzzy_search(ctx, pat, node->lr[1],
                          key, key_size, dists, lx,
                          check, last_node,
                          max_distance, flags, heap);
  } else {
    if (id) {
      /* Set already calculated common prefix length */
      if (len >= last_node->key_length &&
          !memcmp(k, last_node->key, last_node->key_length)) {
        if (last_node->can_transition == GRN_FALSE) {
          return;
        }
        offset = last_node->key_length;
      } else {
        if (last_node->can_transition == GRN_FALSE) {
          last_node->can_transition = GRN_TRUE;
        }
        if (last_node->key_length) {
          const char *kp = k;
          const char *ke = k + len;
          const char *p = last_node->key;
          const char *e = last_node->key + last_node->key_length;
          int lp;
          for (;p < e && kp < ke && (lp = grn_charlen(ctx, p, e));
               p += lp, kp += lp) {
            if (p + lp <= e && kp + lp <= ke && memcmp(p, kp, lp)) {
              break;
            }
          }
          offset = kp - k;
        }
      }
      if (len - offset) {
        uint16_t distance;
        distance =
          calc_edit_distance_by_offset(ctx,
                                       key, key + key_size,
                                       k, k + len,
                                       dists, lx,
                                       offset, max_distance,
                                       &(last_node->can_transition), flags);
        if (distance <= max_distance) {
          fuzzy_heap_push(ctx, heap, id, distance);
        }
      }
      last_node->key = k;
      last_node->key_length = len;
    }
  }
  return;
}

#define HEAP_SIZE 256

grn_rc
grn_pat_fuzzy_search(grn_ctx *ctx, grn_pat *pat,
                     const void *key, uint32_t key_size,
                     grn_fuzzy_search_optarg *args, grn_hash *h)
{
  pat_node *node;
  grn_id id;
  uint16_t *dists;
  uint32_t lx, len, x, y, i;
  const char *s = key;
  const char *e = (const char *)key + key_size;
  fuzzy_node last_node;
  fuzzy_heap *heap;
  uint32_t max_distance = 1;
  uint32_t max_expansion = 0;
  uint32_t prefix_match_size = 0;
  int flags = 0;
  grn_rc rc = grn_pat_error_if_truncated(ctx, pat);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (args) {
    max_distance = args->max_distance;
    max_expansion = args->max_expansion;
    prefix_match_size = args->prefix_match_size;
    flags = args->flags;
  }
  if (key_size > GRN_TABLE_MAX_KEY_SIZE ||
      max_distance > GRN_TABLE_MAX_KEY_SIZE ||
      prefix_match_size > key_size) {
    return GRN_INVALID_ARGUMENT;
  }

  heap = fuzzy_heap_open(ctx, HEAP_SIZE);
  if (!heap) {
    return GRN_NO_MEMORY_AVAILABLE;
  }

  PAT_AT(pat, GRN_ID_NIL, node);
  id = node->lr[1];

  if (prefix_match_size) {
    grn_id tid;
    tid = common_prefix_pat_node_get(ctx, pat, key, prefix_match_size);
    if (tid != GRN_ID_NIL) {
      id = tid;
    } else {
      return GRN_END_OF_DATA;
    }
  }
  for (lx = 0; s < e && (len = grn_charlen(ctx, s, e)); s += len) {
    lx++;
  }
  dists = GRN_MALLOC((lx + 1) * (lx + max_distance + 1) * sizeof(uint16_t));
  if (!dists) {
    return GRN_NO_MEMORY_AVAILABLE;
  }

  for (x = 0; x <= lx; x++) { DIST(x, 0) = x; }
  for (y = 0; y <= lx + max_distance ; y++) { DIST(0, y) = y; }

  last_node.key = NULL;
  last_node.key_length = 0;
  last_node.can_transition = GRN_TRUE;
  _grn_pat_fuzzy_search(ctx, pat, id,
                        key, key_size, dists, lx,
                        -1, &last_node, max_distance, flags, heap);
  GRN_FREE(dists);
  for (i = 0; i < heap->n_entries; i++) {
    if (max_expansion > 0 && i >= max_expansion) {
      break;
    }
    if (DB_OBJ(h)->header.flags & GRN_OBJ_WITH_SUBREC) {
      grn_rset_recinfo *ri;
      if (grn_hash_add(ctx, h, &(heap->nodes[i].id), sizeof(grn_id), (void **)&ri, NULL)) {
        ri->score = max_distance - heap->nodes[i].distance + 1;
      }
    } else {
      grn_hash_add(ctx, h, &(heap->nodes[i].id), sizeof(grn_id), NULL, NULL);
    }
  }
  fuzzy_heap_close(ctx, heap);
  if (grn_hash_size(ctx, h)) {
    return GRN_SUCCESS;
  } else {
    return GRN_END_OF_DATA;
  }
}

inline static grn_rc
_grn_pat_del(grn_ctx *ctx, grn_pat *pat, const char *key, uint32_t key_size, int shared,
             grn_table_delete_optarg *optarg)
{
  grn_pat_delinfo *di;
  pat_node *rn, *rn0 = NULL, *rno = NULL;
  int c = -1, c0 = -1, ch;
  uint32_t len = key_size * 16;
  grn_id r, otherside, *proot, *p, *p0 = NULL;

  /* delinfo_new() must be called before searching for rn. */
  di = delinfo_new(ctx, pat);
  di->shared = shared;

  /*
   * Search a patricia tree for a given key.
   * If the key exists, get its output node.
   *
   * rn, rn0: the output node and its previous node.
   * rno: the other side of rn (the other destination of rn0).
   * c, c0: checks of rn0 and its previous node.
   * p, p0: pointers to transitions (IDs) that refer to rn and rn0.
   */
  PAT_AT(pat, 0, rn);
  proot = p = &rn->lr[1];
  for (;;) {
    r = *p;
    if (!r) {
      return GRN_INVALID_ARGUMENT;
    }
    PAT_AT(pat, r, rn);
    if (!rn) {
      return GRN_FILE_CORRUPT;
    }
    ch = PAT_CHK(rn);
    if (len <= ch) {
      return GRN_INVALID_ARGUMENT;
    }
    if (c >= ch) {
      /* Output node found. */
      const uint8_t *k = pat_node_get_key(ctx, pat, rn);
      if (!k) {
        return GRN_INVALID_ARGUMENT;
      }
      if (key_size != PAT_LEN(rn) || memcmp(k, key, key_size)) {
        return GRN_INVALID_ARGUMENT;
      }
      /* Given key found. */
      break;
    }
    c0 = c;
    p0 = p;
    c = ch;
    if (c & 1) {
      p = (c + 1 < len) ? &rn->lr[1] : &rn->lr[0];
    } else {
      p = &rn->lr[nth_bit((uint8_t *)key, c, len)];
    }
    rn0 = rn;
  }
  if (optarg && optarg->func &&
      !optarg->func(ctx, (grn_obj *)pat, r, optarg->func_arg)) {
    return GRN_SUCCESS;
  }
  if (rn0->lr[0] == rn0->lr[1]) {
    GRN_LOG(ctx, GRN_LOG_DEBUG, "*p0 (%d), rn0->lr[0] == rn0->lr[1] (%d)",
            *p0, rn0->lr[0]);
    return GRN_FILE_CORRUPT;
  }
  otherside = (rn0->lr[1] == r) ? rn0->lr[0] : rn0->lr[1];
  if (otherside) {
    PAT_AT(pat, otherside, rno);
    if (!rno) {
      return GRN_FILE_CORRUPT;
    }
  }

  if (rn == rn0) {
    /* The last transition (p) is a self-loop. */
    di->stat = DL_PHASE2;
    di->d = r;
    if (otherside) {
      if (c0 < PAT_CHK(rno) && PAT_CHK(rno) <= c) {
        /* To keep rno as an output node, its check is set to zero. */
        if (!delinfo_search(pat, otherside)) {
          GRN_LOG(ctx, GRN_LOG_DEBUG, "no delinfo found %d", otherside);
        }
        PAT_CHK_SET(rno, 0);
      }
      if (proot == p0 && !rno->check) {
        /*
         * Update rno->lr because the first node, rno becomes the new first
         * node, is not an output node even if its check is zero.
         */
        const uint8_t *k = pat_node_get_key(ctx, pat, rno);
        int direction = k ? (*k >> 7) : 1;
        rno->lr[direction] = otherside;
        rno->lr[!direction] = 0;
      }
    }
    *p0 = otherside;
  } else if ((!rn->lr[0] && rn->lr[1] == r) ||
             (!rn->lr[1] && rn->lr[0] == r)) {
    /* The output node has only a disabled self-loop. */
    di->stat = DL_PHASE2;
    di->d = r;
    *p = 0;
  } else {
    /* The last transition (p) is not a self-loop. */
    grn_pat_delinfo *ldi = NULL, *ddi = NULL;
    if (PAT_DEL(rn)) {
      ldi = delinfo_search(pat, r);
    }
    if (PAT_DEL(rn0)) {
      ddi = delinfo_search(pat, *p0);
    }
    if (ldi) {
      PAT_DEL_OFF(rn);
      di->stat = DL_PHASE2;
      if (ddi) {
        PAT_DEL_OFF(rn0);
        ddi->stat = DL_PHASE2;
        if (ddi == ldi) {
          if (r != ddi->ld) {
            GRN_LOG(ctx, GRN_LOG_ERROR, "r(%d) != ddi->ld(%d)", r, ddi->ld);
          }
          di->d = r;
        } else {
          ldi->ld = ddi->ld;
          di->d = r;
        }
      } else {
        PAT_DEL_ON(rn0);
        ldi->ld = *p0;
        di->d = r;
      }
    } else {
      PAT_DEL_ON(rn);
      if (ddi) {
        if (ddi->d != *p0) {
          GRN_LOG(ctx, GRN_LOG_ERROR, "ddi->d(%d) != *p0(%d)", ddi->d, *p0);
        }
        PAT_DEL_OFF(rn0);
        ddi->stat = DL_PHASE2;
        di->stat = DL_PHASE1;
        di->ld = ddi->ld;
        di->d = r;
        /*
        PAT_DEL_OFF(rn0);
        ddi->d = r;
        di->stat = DL_PHASE2;
        di->d = *p0;
        */
      } else {
        PAT_DEL_ON(rn0);
        di->stat = DL_PHASE1;
        di->ld = *p0;
        di->d = r;
        // grn_log("pat_del d=%d ld=%d stat=%d", r, *p0, DL_PHASE1);
      }
    }
    if (*p0 == otherside) {
      /* The previous node (*p0) has a self-loop (rn0 == rno). */
      PAT_CHK_SET(rno, 0);
      if (proot == p0) {
        /*
         * Update rno->lr because the first node, rno becomes the new first
         * node, is not an output node even if its check is zero.
         */
        const uint8_t *k = pat_node_get_key(ctx, pat, rno);
        int direction = k ? (*k >> 7) : 1;
        rno->lr[direction] = otherside;
        rno->lr[!direction] = 0;
      }
    } else {
      if (otherside) {
        if (c0 < PAT_CHK(rno) && PAT_CHK(rno) <= c) {
          /* To keep rno as an output node, its check is set to zero. */
          if (!delinfo_search(pat, otherside)) {
            GRN_LOG(ctx, GRN_LOG_ERROR, "no delinfo found %d", otherside);
          }
          PAT_CHK_SET(rno, 0);
        }
        if (proot == p0 && !rno->check) {
          /*
           * Update rno->lr because the first node, rno becomes the new first
           * node, is not an output node even if its check is zero.
           */
          const uint8_t *k = pat_node_get_key(ctx, pat, rno);
          int direction = k ? (*k >> 7) : 1;
          rno->lr[direction] = otherside;
          rno->lr[!direction] = 0;
        }
      }
      *p0 = otherside;
    }
  }
  pat->header->n_entries--;
  pat->header->n_garbages++;
  return GRN_SUCCESS;
}

static grn_rc
_grn_pat_delete(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size,
                grn_table_delete_optarg *optarg)
{
  if (pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) {
    grn_id id = grn_pat_get(ctx, pat, key, key_size, NULL);
    if (id && grn_pat_delete_with_sis(ctx, pat, id, optarg)) {
      return GRN_SUCCESS;
    }
    return GRN_INVALID_ARGUMENT;
  }
  return _grn_pat_del(ctx, pat, key, key_size, 0, optarg);
}

grn_rc
grn_pat_delete(grn_ctx *ctx, grn_pat *pat, const void *key, uint32_t key_size,
               grn_table_delete_optarg *optarg)
{
  grn_rc rc;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  if (!pat || !key || !key_size) { return GRN_INVALID_ARGUMENT; }
  rc = grn_pat_error_if_truncated(ctx, pat);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  KEY_ENCODE(pat, keybuf, key, key_size);
  return _grn_pat_delete(ctx, pat, key, key_size, optarg);
}

uint32_t
grn_pat_size(grn_ctx *ctx, grn_pat *pat)
{
  if (!pat) { return GRN_INVALID_ARGUMENT; }
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return 0;
  }
  return pat->header->n_entries;
}

const char *
_grn_pat_key(grn_ctx *ctx, grn_pat *pat, grn_id id, uint32_t *key_size)
{
  pat_node *node;
  uint8_t *key;
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    *key_size = 0;
    return NULL;
  }
  PAT_AT(pat, id, node);
  if (!node) {
    *key_size = 0;
    return NULL;
  }
  key = pat_node_get_key(ctx, pat, node);
  if (key) {
    *key_size = PAT_LEN(node);
  } else {
    *key_size = 0;
  }
  return (const char *)key;
}

grn_rc
grn_pat_delete_by_id(grn_ctx *ctx, grn_pat *pat, grn_id id,
                     grn_table_delete_optarg *optarg)
{
  grn_rc rc;
  if (!pat || !id) { return GRN_INVALID_ARGUMENT; }
  rc = grn_pat_error_if_truncated(ctx, pat);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  {
    uint32_t key_size;
    const char *key = _grn_pat_key(ctx, pat, id, &key_size);
    return _grn_pat_delete(ctx, pat, key, key_size, optarg);
  }
}

int
grn_pat_get_key(grn_ctx *ctx, grn_pat *pat, grn_id id, void *keybuf, int bufsize)
{
  int len;
  uint8_t *key;
  pat_node *node;
  if (!pat) { return 0; }
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return 0;
  }
  if (!id) { return 0; }
  PAT_AT(pat, id, node);
  if (!node) { return 0; }
  if (!(key = pat_node_get_key(ctx, pat, node))) { return 0; }
  len = PAT_LEN(node);
  if (keybuf && bufsize >= len) {
    if (KEY_NEEDS_CONVERT(pat, len)) {
      KEY_DEC(pat, keybuf, key, len);
    } else {
      grn_memcpy(keybuf, key, len);
    }
  }
  return len;
}

int
grn_pat_get_key2(grn_ctx *ctx, grn_pat *pat, grn_id id, grn_obj *bulk)
{
  uint32_t len;
  uint8_t *key;
  pat_node *node;
  if (!pat) { return GRN_INVALID_ARGUMENT; }
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return 0;
  }
  if (!id) { return 0; }
  PAT_AT(pat, id, node);
  if (!node) { return 0; }
  if (!(key = pat_node_get_key(ctx, pat, node))) { return 0; }
  len = PAT_LEN(node);
  if (KEY_NEEDS_CONVERT(pat, len)) {
    if (bulk->header.impl_flags & GRN_OBJ_REFER) {
      GRN_TEXT_INIT(bulk, 0);
    }
    if (!grn_bulk_reserve(ctx, bulk, len)) {
      char *curr = GRN_BULK_CURR(bulk);
      KEY_DEC(pat, curr, key, len);
      grn_bulk_truncate(ctx, bulk, GRN_BULK_VSIZE(bulk) + len);
    }
  } else {
    if (bulk->header.impl_flags & GRN_OBJ_REFER) {
      bulk->u.b.head = (char *)key;
      bulk->u.b.curr = (char *)key + len;
    } else {
      grn_bulk_write(ctx, bulk, (char *)key, len);
    }
  }
  return len;
}

int
grn_pat_get_value(grn_ctx *ctx, grn_pat *pat, grn_id id, void *valuebuf)
{
  int value_size;
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return 0;
  }
  value_size = (int)pat->value_size;
  if (value_size) {
    byte *v = (byte *)sis_at(ctx, pat, id);
    if (v) {
      if (valuebuf) {
        if (pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) {
          grn_memcpy(valuebuf, v + sizeof(sis_node), value_size);
        } else {
          grn_memcpy(valuebuf, v, value_size);
        }
      }
      return value_size;
    }
  }
  return 0;
}

const char *
grn_pat_get_value_(grn_ctx *ctx, grn_pat *pat, grn_id id, uint32_t *size)
{
  const char *value = NULL;
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return NULL;
  }
  if ((*size = pat->value_size)) {
    if ((value = (const char *)sis_at(ctx, pat, id))
        && (pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS)) {
      value += sizeof(sis_node);
    }
  }
  return value;
}

grn_rc
grn_pat_set_value(grn_ctx *ctx, grn_pat *pat, grn_id id,
                  const void *value, int flags)
{
  grn_rc rc = grn_pat_error_if_truncated(ctx, pat);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (value) {
    uint32_t value_size = pat->value_size;
    if (value_size) {
      byte *v = (byte *)sis_get(ctx, pat, id);
      if (v) {
        if (pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) { v += sizeof(sis_node); }
        switch ((flags & GRN_OBJ_SET_MASK)) {
        case GRN_OBJ_SET :
          grn_memcpy(v, value, value_size);
          return GRN_SUCCESS;
        case GRN_OBJ_INCR :
          switch (value_size) {
          case sizeof(int32_t) :
            *((int32_t *)v) += *((int32_t *)value);
            return GRN_SUCCESS;
          case sizeof(int64_t) :
            *((int64_t *)v) += *((int64_t *)value);
            return GRN_SUCCESS;
          default :
            return GRN_INVALID_ARGUMENT;
          }
          break;
        case GRN_OBJ_DECR :
          switch (value_size) {
          case sizeof(int32_t) :
            *((int32_t *)v) -= *((int32_t *)value);
            return GRN_SUCCESS;
          case sizeof(int64_t) :
            *((int64_t *)v) -= *((int64_t *)value);
            return GRN_SUCCESS;
          default :
            return GRN_INVALID_ARGUMENT;
          }
          break;
        default :
          // todo : support other types.
          return GRN_INVALID_ARGUMENT;
        }
      } else {
        return GRN_NO_MEMORY_AVAILABLE;
      }
    }
  }
  return GRN_INVALID_ARGUMENT;
}

grn_rc
grn_pat_info(grn_ctx *ctx, grn_pat *pat, int *key_size, unsigned int *flags,
             grn_encoding *encoding, unsigned int *n_entries, unsigned int *file_size)
{
  grn_rc rc;
  ERRCLR(NULL);
  if (!pat) { return GRN_INVALID_ARGUMENT; }
  rc = grn_pat_error_if_truncated(ctx, pat);
  if (rc != GRN_SUCCESS) {
    return rc;
  }
  if (key_size) { *key_size = pat->key_size; }
  if (flags) { *flags = pat->obj.header.flags; }
  if (encoding) { *encoding = pat->encoding; }
  if (n_entries) { *n_entries = pat->header->n_entries; }
  if (file_size) {
    uint64_t tmp = 0;
    if ((rc = grn_io_size(ctx, pat->io, &tmp))) {
      return rc;
    }
    *file_size = (unsigned int) tmp; /* FIXME: inappropriate cast */
  }
  return GRN_SUCCESS;
}

int
grn_pat_delete_with_sis(grn_ctx *ctx, grn_pat *pat, grn_id id,
                        grn_table_delete_optarg *optarg)
{
  int level = 0, shared;
  const char *key = NULL, *_key;
  sis_node *sp, *ss = NULL, *si;
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return 0;
  }
  si = sis_at(ctx, pat, id);
  while (id) {
    pat_node *rn;
    uint32_t key_size;
    if ((si && si->children && si->children != id) ||
        (optarg && optarg->func &&
         !optarg->func(ctx, (grn_obj *)pat, id, optarg->func_arg))) {
      break;
    }
    PAT_AT(pat, id, rn);
    if (!(_key = (char *)pat_node_get_key(ctx, pat, rn))) { return 0; }
    if (_key == key) {
      shared = 1;
    } else {
      key = _key;
      shared = 0;
    }
    key_size = PAT_LEN(rn);
    if (key && key_size) { _grn_pat_del(ctx, pat, key, key_size, shared, NULL); }
    if (si) {
      grn_id *p, sid;
      uint32_t lkey = 0;
      if ((*key & 0x80) && chop(ctx, pat, &key, key + key_size, &lkey)) {
        if ((sid = grn_pat_get(ctx, pat, key, key_size - lkey, NULL)) &&
            (ss = sis_at(ctx, pat, sid))) {
          for (p = &ss->children; *p && *p != sid; p = &sp->sibling) {
            if (*p == id) {
              *p = si->sibling;
              break;
            }
            if (!(sp = sis_at(ctx, pat, *p))) { break; }
          }
        }
      } else {
        sid = GRN_ID_NIL;
      }
      si->sibling = 0;
      si->children = 0;
      id = sid;
      si = ss;
    } else {
      id = GRN_ID_NIL;
    }
    level++;
  }
  if (level) {
    uint32_t lkey = 0;
    while (id && key) {
      uint32_t key_size;
      if (_grn_pat_key(ctx, pat, id, &key_size) != key) { break; }
      {
        pat_node *rn;
        PAT_AT(pat, id, rn);
        if (!rn) { break; }
        if (lkey) {
          rn->key = lkey;
        } else {
          pat_node_set_key(ctx, pat, rn, (uint8_t *)key, key_size);
          lkey = rn->key;
        }
      }
      {
        const char *end = key + key_size;
        if (!((*key & 0x80) && chop(ctx, pat, &key, end, &lkey))) { break; }
        id = grn_pat_get(ctx, pat, key, end - key, NULL);
      }
    }
  }
  return level;
}

grn_id
grn_pat_next(grn_ctx *ctx, grn_pat *pat, grn_id id)
{
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  while (++id <= pat->header->curr_rec) {
    uint32_t key_size;
    const char *key = _grn_pat_key(ctx, pat, id, &key_size);
    if (id == grn_pat_get(ctx, pat, key, key_size, NULL)) {
      return id;
    }
  }
  return GRN_ID_NIL;
}

grn_id
grn_pat_at(grn_ctx *ctx, grn_pat *pat, grn_id id)
{
  uint32_t key_size;
  const char *key = _grn_pat_key(ctx, pat, id, &key_size);
  if (key && (id == _grn_pat_get(ctx, pat, key, key_size, NULL))) { return id; }
  return GRN_ID_NIL;
}

grn_id
grn_pat_curr_id(grn_ctx *ctx, grn_pat *pat)
{
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return GRN_ID_NIL;
  }
  return pat->header->curr_rec;
}

int
grn_pat_scan(grn_ctx *ctx, grn_pat *pat, const char *str, unsigned int str_len,
             grn_pat_scan_hit *sh, unsigned int sh_size, const char **rest)
{
  int n = 0;
  grn_id tid;
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return 0;
  }
  if (pat->normalizer) {
    int flags =
      GRN_STRING_REMOVE_BLANK |
      GRN_STRING_WITH_TYPES |
      GRN_STRING_WITH_CHECKS;
    grn_obj *nstr = grn_string_open(ctx, str, str_len,
                                    pat->normalizer, flags);
    if (nstr) {
      const short *cp = grn_string_get_checks(ctx, nstr);
      const unsigned char *tp = grn_string_get_types(ctx, nstr);
      unsigned int offset = 0, offset0 = 0;
      unsigned int normalized_length_in_bytes;
      const char *sp, *se;
      grn_string_get_normalized(ctx, nstr, &sp, &normalized_length_in_bytes,
                                NULL);
      se = sp + normalized_length_in_bytes;
      while (n < sh_size) {
        if ((tid = grn_pat_lcp_search(ctx, pat, sp, se - sp))) {
          const char *key;
          uint32_t len;
          int first_key_char_len;
          key = _grn_pat_key(ctx, pat, tid, &len);
          sh[n].id = tid;
          sh[n].offset = (*cp > 0) ? offset : offset0;
          first_key_char_len = grn_charlen(ctx, key, key + len);
          if (sh[n].offset > 0 &&
              GRN_CHAR_IS_BLANK(tp[-1]) &&
              ((first_key_char_len == 1 && key[0] != ' ') ||
               first_key_char_len > 1)){
            /* Remove leading spaces. */
            const char *original_str = str + sh[n].offset;
            while (grn_charlen(ctx, original_str, str + str_len) == 1 &&
                   original_str[0] == ' ') {
              original_str++;
              sh[n].offset++;
            }
          }
          {
            grn_bool blank_in_alnum = GRN_FALSE;
            const unsigned char *start_tp = tp;
            const unsigned char *blank_in_alnum_check_tp;
            while (len--) {
              if (*cp > 0) { offset0 = offset; offset += *cp; tp++; }
              sp++; cp++;
            }
            sh[n].length = offset - sh[n].offset;
            for (blank_in_alnum_check_tp = start_tp + 1;
                 blank_in_alnum_check_tp < tp;
                 blank_in_alnum_check_tp++) {
#define GRN_CHAR_IS_ALNUM(char_type)                         \
              (GRN_CHAR_TYPE(char_type) == GRN_CHAR_ALPHA || \
               GRN_CHAR_TYPE(char_type) == GRN_CHAR_DIGIT)
              if (GRN_CHAR_IS_BLANK(blank_in_alnum_check_tp[0]) &&
                  GRN_CHAR_IS_ALNUM(blank_in_alnum_check_tp[-1]) &&
                  (blank_in_alnum_check_tp + 1) < tp &&
                  GRN_CHAR_IS_ALNUM(blank_in_alnum_check_tp[1])) {
                blank_in_alnum = GRN_TRUE;
              }
#undef GRN_CHAR_IS_ALNUM
            }
            if (!blank_in_alnum) {
              n++;
            }
          }
        } else {
          if (*cp > 0) { offset0 = offset; offset += *cp; tp++; }
          do {
            sp++; cp++;
          } while (sp < se && !*cp);
        }
        if (se <= sp) { offset = str_len; break; }
      }
      if (rest) {
        grn_string_get_original(ctx, nstr, rest, NULL);
        *rest += offset;
      }
      grn_obj_close(ctx, nstr);
    } else {
      n = -1;
      if (rest) { *rest = str; }
    }
  } else {
    uint32_t len;
    const char *sp, *se = str + str_len;
    for (sp = str; sp < se && n < sh_size; sp += len) {
      if ((tid = grn_pat_lcp_search(ctx, pat, sp, se - sp))) {
        _grn_pat_key(ctx, pat, tid, &len);
        sh[n].id = tid;
        sh[n].offset = sp - str;
        sh[n].length = len;
        n++;
      } else {
        len = grn_charlen(ctx, sp, se);
      }
      if (!len) { break; }
    }
    if (rest) { *rest = sp; }
  }
  return n;
}

#define INITIAL_SIZE 512

inline static void
push(grn_pat_cursor *c, grn_id id, uint16_t check)
{
  grn_ctx *ctx = c->ctx;
  grn_pat_cursor_entry *se;
  if (c->size <= c->sp) {
    if (c->ss) {
      uint32_t size = c->size * 4;
      grn_pat_cursor_entry *ss = GRN_REALLOC(c->ss, size);
      if (!ss) { return; /* give up */ }
      c->ss = ss;
      c->size = size;
    } else {
      if (!(c->ss = GRN_MALLOC(sizeof(grn_pat_cursor_entry) * INITIAL_SIZE))) {
        return; /* give up */
      }
      c->size = INITIAL_SIZE;
    }
  }
  se = &c->ss[c->sp++];
  se->id = id;
  se->check = check;
}

inline static grn_pat_cursor_entry *
pop(grn_pat_cursor *c)
{
  return c->sp ? &c->ss[--c->sp] : NULL;
}

static grn_id
grn_pat_cursor_next_by_id(grn_ctx *ctx, grn_pat_cursor *c)
{
  grn_pat *pat = c->pat;
  int dir = (c->obj.header.flags & GRN_CURSOR_DESCENDING) ? -1 : 1;
  while (c->curr_rec != c->tail) {
    c->curr_rec += dir;
    if (pat->header->n_garbages) {
      uint32_t key_size;
      const void *key = _grn_pat_key(ctx, pat, c->curr_rec, &key_size);
      if (_grn_pat_get(ctx, pat, key, key_size, NULL) != c->curr_rec) {
        continue;
      }
    }
    c->rest--;
    return c->curr_rec;
  }
  return GRN_ID_NIL;
}

grn_id
grn_pat_cursor_next(grn_ctx *ctx, grn_pat_cursor *c)
{
  pat_node *node;
  grn_pat_cursor_entry *se;
  if (!c->rest) { return GRN_ID_NIL; }
  if ((c->obj.header.flags & GRN_CURSOR_BY_ID)) {
    return grn_pat_cursor_next_by_id(ctx, c);
  }
  while ((se = pop(c))) {
    grn_id id = se->id;
    int check = se->check, ch;
    while (id) {
      PAT_AT(c->pat, id, node);
      if (!node) {
        break;
      }
      ch = PAT_CHK(node);
      if (ch > check) {
        if (c->obj.header.flags & GRN_CURSOR_DESCENDING) {
          push(c, node->lr[0], ch);
          id = node->lr[1];
        } else {
          push(c, node->lr[1], ch);
          id = node->lr[0];
        }
        check = ch;
        continue;
      } else {
        if (id == c->tail) {
          c->sp = 0;
        } else {
          if (!c->curr_rec && c->tail) {
            uint32_t lmin, lmax;
            pat_node *nmin, *nmax;
            const uint8_t *kmin, *kmax;
            if (c->obj.header.flags & GRN_CURSOR_DESCENDING) {
              PAT_AT(c->pat, c->tail, nmin);
              PAT_AT(c->pat, id, nmax);
            } else {
              PAT_AT(c->pat, id, nmin);
              PAT_AT(c->pat, c->tail, nmax);
            }
            lmin = PAT_LEN(nmin);
            lmax = PAT_LEN(nmax);
            kmin = pat_node_get_key(ctx, c->pat, nmin);
            kmax = pat_node_get_key(ctx, c->pat, nmax);
            if ((lmin < lmax) ?
                (memcmp(kmin, kmax, lmin) > 0) :
                (memcmp(kmin, kmax, lmax) >= 0)) {
              c->sp = 0;
              break;
            }
          }
        }
        c->curr_rec = id;
        c->rest--;
        return id;
      }
    }
  }
  return GRN_ID_NIL;
}

void
grn_pat_cursor_close(grn_ctx *ctx, grn_pat_cursor *c)
{
  GRN_ASSERT(c->ctx == ctx);
  if (c->ss) { GRN_FREE(c->ss); }
  GRN_FREE(c);
}

inline static int
bitcmp(const void *s1, const void *s2, int offset, int length)
{
  int r, rest = length + (offset & 7) - 8, bl = offset >> 3, mask = 0xff >> (offset & 7);
  unsigned char *a = (unsigned char *)s1 + bl, *b = (unsigned char *)s2 + bl;
  if (rest <= 0) {
    mask &= 0xff << -rest;
    return (*a & mask) - (*b & mask);
  }
  if ((r = (*a & mask) - (*b & mask))) { return r; }
  a++; b++;
  if ((bl = rest >> 3)) {
    if ((r = memcmp(a, b, bl))) { return r; }
    a += bl; b += bl;
  }
  mask = 0xff << (8 - (rest & 7));
  return (*a & mask) - (*b & mask);
}

inline static grn_rc
set_cursor_prefix(grn_ctx *ctx, grn_pat *pat, grn_pat_cursor *c,
                  const void *key, uint32_t key_size, int flags)
{
  int c0 = -1, ch;
  const uint8_t *k;
  uint32_t len, byte_len;
  grn_id id;
  pat_node *node;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  if (flags & GRN_CURSOR_SIZE_BY_BIT) {
    len = key_size * 2;
    byte_len = key_size >> 3;
  } else {
    len = key_size * 16;
    byte_len = key_size;
  }
  KEY_ENCODE(pat, keybuf, key, byte_len);
  PAT_AT(pat, 0, node);
  id = node->lr[1];
  while (id) {
    PAT_AT(pat, id, node);
    if (!node) { return GRN_FILE_CORRUPT; }
    ch = PAT_CHK(node);
    if (c0 < ch && ch < len - 1) {
      if (ch & 1) {
        id = (ch + 1 < len) ? node->lr[1] : node->lr[0];
      } else {
        id = node->lr[nth_bit((uint8_t *)key, ch, len)];
      }
      c0 = ch;
      continue;
    }
    if (!(k = pat_node_get_key(ctx, pat, node))) { break; }
    if (PAT_LEN(node) < byte_len) { break; }
    if ((flags & GRN_CURSOR_SIZE_BY_BIT)
        ? !bitcmp(k, key, 0, key_size)
        : !memcmp(k, key, key_size)) {
      if (c0 < ch) {
        if (flags & GRN_CURSOR_DESCENDING) {
          if ((ch > len - 1) || !(flags & GRN_CURSOR_GT)) {
            push(c, node->lr[0], ch);
          }
          push(c, node->lr[1], ch);
        } else {
          push(c, node->lr[1], ch);
          if ((ch > len - 1) || !(flags & GRN_CURSOR_GT)) {
            push(c, node->lr[0], ch);
          }
        }
      } else {
        if (PAT_LEN(node) * 16 > len || !(flags & GRN_CURSOR_GT)) {
          push(c, id, ch);
        }
      }
    }
    break;
  }
  return GRN_SUCCESS;
}

inline static grn_rc
set_cursor_near(grn_ctx *ctx, grn_pat *pat, grn_pat_cursor *c,
                uint32_t min_size, const void *key, int flags)
{
  grn_id id;
  pat_node *node;
  const uint8_t *k;
  int r, check = -1, ch;
  uint32_t min = min_size * 16;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  KEY_ENCODE(pat, keybuf, key, pat->key_size);
  PAT_AT(pat, 0, node);
  for (id = node->lr[1]; id;) {
    PAT_AT(pat, id, node);
    if (!node) { return GRN_FILE_CORRUPT; }
    ch = PAT_CHK(node);
    if (ch <= check) {
      if (check >= min) { push(c, id, check); }
      break;
    }
    if ((check += 2) < ch) {
      if (!(k = pat_node_get_key(ctx, pat, node))) { return GRN_FILE_CORRUPT; }
      if ((r = bitcmp(key, k, check >> 1, (ch - check) >> 1))) {
        if (ch >= min) {
          push(c, node->lr[1], ch);
          push(c, node->lr[0], ch);
        }
        break;
      }
    }
    check = ch;
    if (nth_bit((uint8_t *)key, check, pat->key_size)) {
      if (check >= min) { push(c, node->lr[0], check); }
      id = node->lr[1];
    } else {
      if (check >= min) { push(c, node->lr[1], check); }
      id = node->lr[0];
    }
  }
  return GRN_SUCCESS;
}

inline static grn_rc
set_cursor_common_prefix(grn_ctx *ctx, grn_pat *pat, grn_pat_cursor *c,
                         uint32_t min_size, const void *key, uint32_t key_size, int flags)
{
  grn_id id;
  pat_node *node;
  const uint8_t *k;
  int check = -1, ch;
  uint32_t len = key_size * 16;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  KEY_ENCODE(pat, keybuf, key, key_size);
  PAT_AT(pat, 0, node);
  for (id = node->lr[1]; id;) {
    PAT_AT(pat, id, node);
    if (!node) { return GRN_FILE_CORRUPT; }
    ch = PAT_CHK(node);
    if (ch <= check) {
      if (!(k = pat_node_get_key(ctx, pat, node))) { return GRN_FILE_CORRUPT; }
      {
        uint32_t l = PAT_LEN(node);
        if (min_size <= l && l <= key_size) {
          if (!memcmp(key, k, l)) { push(c, id, check); }
        }
      }
      break;
    }
    check = ch;
    if (len <= check) { break; }
    if (check & 1) {
      grn_id id0 = node->lr[0];
      pat_node *node0;
      PAT_AT(pat, id0, node0);
      if (!node0) { return GRN_FILE_CORRUPT; }
      if (!(k = pat_node_get_key(ctx, pat, node0))) { return GRN_FILE_CORRUPT; }
      {
        uint32_t l = PAT_LEN(node0);
        if (memcmp(key, k, l)) { break; }
        if (min_size <= l) {
          push(c, id0, check);
        }
      }
      id = node->lr[1];
    } else {
      id = node->lr[nth_bit((uint8_t *)key, check, len)];
    }
  }
  return GRN_SUCCESS;
}

inline static grn_rc
set_cursor_ascend(grn_ctx *ctx, grn_pat *pat, grn_pat_cursor *c,
                  const void *key, uint32_t key_size, int flags)
{
  grn_id id;
  pat_node *node;
  const uint8_t *k;
  int r, check = -1, ch, c2;
  uint32_t len = key_size * 16;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  KEY_ENCODE(pat, keybuf, key, key_size);
  PAT_AT(pat, 0, node);
  for (id = node->lr[1]; id;) {
    PAT_AT(pat, id, node);
    if (!node) { return GRN_FILE_CORRUPT; }
    ch = PAT_CHK(node);
    if (ch <= check) {
      if (!(k = pat_node_get_key(ctx, pat, node))) { return GRN_FILE_CORRUPT; }
      {
        uint32_t l = PAT_LEN(node);
        if (l == key_size) {
          if (flags & GRN_CURSOR_GT) {
            if (memcmp(key, k, l) < 0) { push(c, id, check); }
          } else {
            if (memcmp(key, k, l) <= 0) { push(c, id, check); }
          }
        } else if (l < key_size) {
          if (memcmp(key, k, l) < 0) { push(c, id, check); }
        } else {
          if (memcmp(key, k, key_size) <= 0) { push(c, id, check); }
        }
      }
      break;
    }
    c2 = len < ch ? len : ch;
    if ((check += 2) < c2) {
      if (!(k = pat_node_get_key(ctx, pat, node))) { return GRN_FILE_CORRUPT; }
      if ((r = bitcmp(key, k, check >> 1, ((c2 + 1) >> 1) - (check >> 1)))) {
        if (r < 0) {
          push(c, node->lr[1], ch);
          push(c, node->lr[0], ch);
        }
        break;
      }
    }
    check = ch;
    if (len <= check) {
      push(c, node->lr[1], ch);
      push(c, node->lr[0], ch);
      break;
    }
    if (check & 1) {
      if (check + 1 < len) {
        id = node->lr[1];
      } else {
        push(c, node->lr[1], check);
        id = node->lr[0];
      }
    } else {
      if (nth_bit((uint8_t *)key, check, len)) {
        id = node->lr[1];
      } else {
        push(c, node->lr[1], check);
        id = node->lr[0];
      }
    }
  }
  return GRN_SUCCESS;
}

inline static grn_rc
set_cursor_descend(grn_ctx *ctx, grn_pat *pat, grn_pat_cursor *c,
                   const void *key, uint32_t key_size, int flags)
{
  grn_id id;
  pat_node *node;
  const uint8_t *k;
  int r, check = -1, ch, c2;
  uint32_t len = key_size * 16;
  uint8_t keybuf[MAX_FIXED_KEY_SIZE];
  KEY_ENCODE(pat, keybuf, key, key_size);
  PAT_AT(pat, 0, node);
  for (id = node->lr[1]; id;) {
    PAT_AT(pat, id, node);
    if (!node) { return GRN_FILE_CORRUPT; }
    ch = PAT_CHK(node);
    if (ch <= check) {
      if (!(k = pat_node_get_key(ctx, pat, node))) { return GRN_FILE_CORRUPT; }
      {
        uint32_t l = PAT_LEN(node);
        if (l <= key_size) {
          if ((flags & GRN_CURSOR_LT) && l == key_size) {
            if (memcmp(key, k, l) > 0) { push(c, id, check); }
          } else {
            if (memcmp(key, k, l) >= 0) { push(c, id, check); }
          }
        } else {
          if (memcmp(key, k, key_size) > 0) { push(c, id, check); }
        }
      }
      break;
    }
    c2 = len < ch ? len : ch;
    if ((check += 2) < c2) {
      if (!(k = pat_node_get_key(ctx, pat, node))) { return GRN_FILE_CORRUPT; }
      if ((r = bitcmp(key, k, check >> 1, ((c2 + 1) >> 1) - (check >> 1)))) {
        if (r >= 0) {
          push(c, node->lr[0], ch);
          push(c, node->lr[1], ch);
        }
        break;
      }
    }
    check = ch;
    if (len <= check) { break; }
    if (check & 1) {
      if (check + 1 < len) {
        push(c, node->lr[0], check);
        id = node->lr[1];
      } else {
        id = node->lr[0];
      }
    } else {
      if (nth_bit((uint8_t *)key, check, len)) {
        push(c, node->lr[0], check);
        id = node->lr[1];
      } else {
        id = node->lr[0];
      }
    }
  }
  return GRN_SUCCESS;
}

static grn_pat_cursor *
grn_pat_cursor_open_by_id(grn_ctx *ctx, grn_pat *pat,
                          const void *min, uint32_t min_size,
                          const void *max, uint32_t max_size,
                          int offset, int limit, int flags)
{
  int dir;
  grn_pat_cursor *c;
  if (!pat || !ctx) { return NULL; }
  if (!(c = GRN_MALLOCN(grn_pat_cursor, 1))) { return NULL; }
  GRN_DB_OBJ_SET_TYPE(c, GRN_CURSOR_TABLE_PAT_KEY);
  c->pat = pat;
  c->ctx = ctx;
  c->obj.header.flags = flags;
  c->obj.header.domain = GRN_ID_NIL;
  c->size = 0;
  c->sp = 0;
  c->ss = NULL;
  c->tail = 0;
  if (flags & GRN_CURSOR_DESCENDING) {
    dir = -1;
    if (max) {
      if (!(c->curr_rec = grn_pat_get(ctx, pat, max, max_size, NULL))) {
        c->tail = GRN_ID_NIL;
        goto exit;
      }
      if (!(flags & GRN_CURSOR_LT)) { c->curr_rec++; }
    } else {
      c->curr_rec = pat->header->curr_rec + 1;
    }
    if (min) {
      if (!(c->tail = grn_pat_get(ctx, pat, min, min_size, NULL))) {
        c->curr_rec = GRN_ID_NIL;
        goto exit;
      }
      if ((flags & GRN_CURSOR_GT)) { c->tail++; }
    } else {
      c->tail = GRN_ID_NIL + 1;
    }
    if (c->curr_rec < c->tail) { c->tail = c->curr_rec; }
  } else {
    dir = 1;
    if (min) {
      if (!(c->curr_rec = grn_pat_get(ctx, pat, min, min_size, NULL))) {
        c->tail = GRN_ID_NIL;
        goto exit;
      }
      if (!(flags & GRN_CURSOR_GT)) { c->curr_rec--; }
    } else {
      c->curr_rec = GRN_ID_NIL;
    }
    if (max) {
      if (!(c->tail = grn_pat_get(ctx, pat, max, max_size, NULL))) {
        c->curr_rec = GRN_ID_NIL;
        goto exit;
      }
      if ((flags & GRN_CURSOR_LT)) { c->tail--; }
    } else {
      c->tail = pat->header->curr_rec;
    }
    if (c->tail < c->curr_rec) { c->tail = c->curr_rec; }
  }
  if (pat->header->n_garbages) {
    while (offset && c->curr_rec != c->tail) {
      uint32_t key_size;
      const void *key;
      c->curr_rec += dir;
      key = _grn_pat_key(ctx, pat, c->curr_rec, &key_size);
      if (_grn_pat_get(ctx, pat, key, key_size, NULL) == c->curr_rec) {
        offset--;
      }
    }
  } else {
    if ((dir * (c->tail - c->curr_rec)) < offset) {
      c->curr_rec = c->tail;
    } else {
      c->curr_rec += dir * offset;
    }
  }
  c->rest = (limit < 0) ? GRN_ID_MAX : limit;
exit :
  return c;
}

static grn_rc set_cursor_rk(grn_ctx *ctx, grn_pat *pat, grn_pat_cursor *c,
                            const void *key, uint32_t key_size, int flags);

grn_pat_cursor *
grn_pat_cursor_open(grn_ctx *ctx, grn_pat *pat,
                    const void *min, uint32_t min_size,
                    const void *max, uint32_t max_size,
                    int offset, int limit, int flags)
{
  grn_id id;
  pat_node *node;
  grn_pat_cursor *c;
  if (!pat || !ctx) { return NULL; }
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return NULL;
  }
  if ((flags & GRN_CURSOR_BY_ID)) {
    return grn_pat_cursor_open_by_id(ctx, pat, min, min_size, max, max_size,
                                     offset, limit, flags);
  }
  if (!(c = GRN_MALLOCN(grn_pat_cursor, 1))) { return NULL; }
  GRN_DB_OBJ_SET_TYPE(c, GRN_CURSOR_TABLE_PAT_KEY);
  c->pat = pat;
  c->ctx = ctx;
  c->size = 0;
  c->sp = 0;
  c->ss = NULL;
  c->tail = 0;
  c->rest = GRN_ID_MAX;
  c->curr_rec = GRN_ID_NIL;
  c->obj.header.domain = GRN_ID_NIL;
  if (flags & GRN_CURSOR_PREFIX) {
    if (max && max_size) {
      if ((pat->obj.header.flags & GRN_OBJ_KEY_VAR_SIZE)) {
        set_cursor_common_prefix(ctx, pat, c, min_size, max, max_size, flags);
      } else {
        set_cursor_near(ctx, pat, c, min_size, max, flags);
      }
      goto exit;
    } else {
      if (min && min_size) {
        if (flags & GRN_CURSOR_RK) {
          set_cursor_rk(ctx, pat, c, min, min_size, flags);
        } else {
          set_cursor_prefix(ctx, pat, c, min, min_size, flags);
        }
        goto exit;
      }
    }
  }
  if (flags & GRN_CURSOR_DESCENDING) {
    if (min && min_size) {
      set_cursor_ascend(ctx, pat, c, min, min_size, flags);
      c->obj.header.flags = GRN_CURSOR_ASCENDING;
      c->tail = grn_pat_cursor_next(ctx, c);
      c->sp = 0;
      if (!c->tail) { goto exit; }
    }
    if (max && max_size) {
      set_cursor_descend(ctx, pat, c, max, max_size, flags);
    } else {
      PAT_AT(pat, 0, node);
      if (!node) {
        grn_pat_cursor_close(ctx, c);
        return NULL;
      }
      if ((id = node->lr[1])) {
        PAT_AT(pat, id, node);
        if (node) {
          int ch = PAT_CHK(node);
          push(c, node->lr[0], ch);
          push(c, node->lr[1], ch);
        }
      }
    }
  } else {
    if (max && max_size) {
      set_cursor_descend(ctx, pat, c, max, max_size, flags);
      c->obj.header.flags = GRN_CURSOR_DESCENDING;
      c->tail = grn_pat_cursor_next(ctx, c);
      c->sp = 0;
      if (!c->tail) { goto exit; }
    }
    if (min && min_size) {
      set_cursor_ascend(ctx, pat, c, min, min_size, flags);
    } else {
      PAT_AT(pat, 0, node);
      if (!node) {
        grn_pat_cursor_close(ctx, c);
        return NULL;
      }
      if ((id = node->lr[1])) {
        PAT_AT(pat, id, node);
        if (node) {
          int ch = PAT_CHK(node);
          push(c, node->lr[1], ch);
          push(c, node->lr[0], ch);
        }
      }
    }
  }
exit :
  c->obj.header.flags = flags;
  c->curr_rec = GRN_ID_NIL;
  while (offset--) { grn_pat_cursor_next(ctx, c); }
  c->rest = (limit < 0) ? GRN_ID_MAX : limit;
  return c;
}

int
grn_pat_cursor_get_key(grn_ctx *ctx, grn_pat_cursor *c, void **key)
{
  *key = c->curr_key;
  return grn_pat_get_key(ctx, c->pat, c->curr_rec, *key, GRN_TABLE_MAX_KEY_SIZE);
}

int
grn_pat_cursor_get_value(grn_ctx *ctx, grn_pat_cursor *c, void **value)
{
  int value_size = (int)c->pat->value_size;
  if (value_size) {
    byte *v = (byte *)sis_at(ctx, c->pat, c->curr_rec);
    if (v) {
      if (c->pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) {
        *value = v + sizeof(sis_node);
      } else {
        *value = v;
      }
    } else {
      *value = NULL;
    }
  }
  return value_size;
}

int
grn_pat_cursor_get_key_value(grn_ctx *ctx, grn_pat_cursor *c,
                             void **key, uint32_t *key_size, void **value)
{
  int value_size = (int)c->pat->value_size;
  if (key_size) {
    *key_size = (uint32_t) grn_pat_get_key(ctx, c->pat, c->curr_rec, c->curr_key,
                                           GRN_TABLE_MAX_KEY_SIZE);
    if (key) { *key = c->curr_key; }
  }
  if (value && value_size) {
    byte *v = (byte *)sis_at(ctx, c->pat, c->curr_rec);
    if (v) {
      if (c->pat->obj.header.flags & GRN_OBJ_KEY_WITH_SIS) {
        *value = v + sizeof(sis_node);
      } else {
        *value = v;
      }
    } else {
      *value = NULL;
    }
  }
  return value_size;
}

grn_rc
grn_pat_cursor_set_value(grn_ctx *ctx, grn_pat_cursor *c,
                         const void *value, int flags)
{
  return grn_pat_set_value(ctx, c->pat, c->curr_rec, value, flags);
}

grn_rc
grn_pat_cursor_delete(grn_ctx *ctx, grn_pat_cursor *c,
                      grn_table_delete_optarg *optarg)
{
  return grn_pat_delete_by_id(ctx, c->pat, c->curr_rec, optarg);
}

void
grn_pat_check(grn_ctx *ctx, grn_pat *pat)
{
  char buf[8];
  struct grn_pat_header *h = pat->header;
  if (grn_pat_error_if_truncated(ctx, pat) != GRN_SUCCESS) {
    return;
  }
  GRN_OUTPUT_ARRAY_OPEN("RESULT", 1);
  GRN_OUTPUT_MAP_OPEN("SUMMARY", 23);
  GRN_OUTPUT_CSTR("flags");
  grn_itoh(h->flags, buf, 8);
  GRN_OUTPUT_STR(buf, 8);
  GRN_OUTPUT_CSTR("key size");
  GRN_OUTPUT_INT64(h->key_size);
  GRN_OUTPUT_CSTR("value_size");
  GRN_OUTPUT_INT64(h->value_size);
  GRN_OUTPUT_CSTR("tokenizer");
  GRN_OUTPUT_INT64(h->tokenizer);
  GRN_OUTPUT_CSTR("normalizer");
  GRN_OUTPUT_INT64(h->normalizer);
  GRN_OUTPUT_CSTR("n_entries");
  GRN_OUTPUT_INT64(h->n_entries);
  GRN_OUTPUT_CSTR("curr_rec");
  GRN_OUTPUT_INT64(h->curr_rec);
  GRN_OUTPUT_CSTR("curr_key");
  GRN_OUTPUT_INT64(h->curr_key);
  GRN_OUTPUT_CSTR("curr_del");
  GRN_OUTPUT_INT64(h->curr_del);
  GRN_OUTPUT_CSTR("curr_del2");
  GRN_OUTPUT_INT64(h->curr_del2);
  GRN_OUTPUT_CSTR("curr_del3");
  GRN_OUTPUT_INT64(h->curr_del3);
  GRN_OUTPUT_CSTR("n_garbages");
  GRN_OUTPUT_INT64(h->n_garbages);
  GRN_OUTPUT_MAP_CLOSE();
  GRN_OUTPUT_ARRAY_CLOSE();
}

/* utilities */
void
grn_p_pat_node(grn_ctx *ctx, grn_pat *pat, pat_node *node)
{
  uint8_t *key = NULL;

  if (!node) {
    printf("#<pat_node:(null)>\n");
    return;
  }

  if (PAT_IMD(node)) {
    key = (uint8_t *)&(node->key);
  } else {
    KEY_AT(pat, node->key, key, 0);
  }

  printf("#<pat_node:%p "
         "left:%u "
         "right:%u "
         "deleting:%s "
         "immediate:%s "
         "length:%u "
         "nth-byte:%u "
         "nth-bit:%u "
         "terminated:%s "
         "key:<%.*s>"
         ">\n",
         node,
         node->lr[0],
         node->lr[1],
         PAT_DEL(node) ? "true" : "false",
         PAT_IMD(node) ? "true" : "false",
         PAT_LEN(node),
         PAT_CHK(node) >> 4,
         (PAT_CHK(node) >> 1) & 0x7,
         (PAT_CHK(node) & 0x1) ? "true" : "false",
         PAT_LEN(node),
         (char *)key);
}

static void
grn_pat_inspect_check(grn_ctx *ctx, grn_obj *buf, int check)
{
  GRN_TEXT_PUTS(ctx, buf, "{");
  grn_text_lltoa(ctx, buf, check >> 4);
  GRN_TEXT_PUTS(ctx, buf, ",");
  grn_text_lltoa(ctx, buf, (check >> 1) & 7);
  GRN_TEXT_PUTS(ctx, buf, ",");
  grn_text_lltoa(ctx, buf, check & 1);
  GRN_TEXT_PUTS(ctx, buf, "}");
}

static void
grn_pat_inspect_node(grn_ctx *ctx, grn_pat *pat, grn_id id, int check,
                     grn_obj *key_buf, int indent, const char *prefix,
                     grn_obj *buf)
{
  pat_node *node = NULL;
  int i, c;

  PAT_AT(pat, id, node);
  c = PAT_CHK(node);

  for (i = 0; i < indent; i++) {
    GRN_TEXT_PUTC(ctx, buf, ' ');
  }
  GRN_TEXT_PUTS(ctx, buf, prefix);
  grn_text_lltoa(ctx, buf, id);
  grn_pat_inspect_check(ctx, buf, c);

  if (c > check) {
    GRN_TEXT_PUTS(ctx, buf, "\n");
    grn_pat_inspect_node(ctx, pat, node->lr[0], c, key_buf,
                         indent + 2, "L:", buf);
    GRN_TEXT_PUTS(ctx, buf, "\n");
    grn_pat_inspect_node(ctx, pat, node->lr[1], c, key_buf,
                         indent + 2, "R:", buf);
  } else if (id) {
    int key_size;
    uint8_t *key;

    key_size = PAT_LEN(node);
    GRN_BULK_REWIND(key_buf);
    grn_bulk_space(ctx, key_buf, key_size);
    grn_pat_get_key(ctx, pat, id, GRN_BULK_HEAD(key_buf), key_size);
    GRN_TEXT_PUTS(ctx, buf, "(");
    grn_inspect(ctx, buf, key_buf);
    GRN_TEXT_PUTS(ctx, buf, ")");

    GRN_TEXT_PUTS(ctx, buf, "[");
    key = pat_node_get_key(ctx, pat, node);
    for (i = 0; i < key_size; i++) {
      int j;
      uint8_t byte = key[i];
      if (i != 0) {
        GRN_TEXT_PUTS(ctx, buf, " ");
      }
      for (j = 0; j < 8; j++) {
        grn_text_lltoa(ctx, buf, (byte >> (7 - j)) & 1);
      }
    }
    GRN_TEXT_PUTS(ctx, buf, "]");
  }
}

void
grn_pat_inspect_nodes(grn_ctx *ctx, grn_pat *pat, grn_obj *buf)
{
  pat_node *node;
  grn_obj key_buf;

  GRN_TEXT_PUTS(ctx, buf, "{");
  PAT_AT(pat, GRN_ID_NIL, node);
  if (node->lr[1]) {
    GRN_TEXT_PUTS(ctx, buf, "\n");
    GRN_OBJ_INIT(&key_buf, GRN_BULK, 0, pat->obj.header.domain);
    grn_pat_inspect_node(ctx, pat, node->lr[1], -1, &key_buf, 0, "", buf);
    GRN_OBJ_FIN(ctx, &key_buf);
    GRN_TEXT_PUTS(ctx, buf, "\n");
  }
  GRN_TEXT_PUTS(ctx, buf, "}");
}

static void
grn_pat_cursor_inspect_entries(grn_ctx *ctx, grn_pat_cursor *c, grn_obj *buf)
{
  int i;
  GRN_TEXT_PUTS(ctx, buf, "[");
  for (i = 0; i < c->sp; i++) {
    grn_pat_cursor_entry *e = c->ss + i;
    if (i != 0) {
      GRN_TEXT_PUTS(ctx, buf, ", ");
    }
    GRN_TEXT_PUTS(ctx, buf, "[");
    grn_text_lltoa(ctx, buf, e->id);
    GRN_TEXT_PUTS(ctx, buf, ",");
    grn_pat_inspect_check(ctx, buf, e->check);
    GRN_TEXT_PUTS(ctx, buf, "]");
  }
  GRN_TEXT_PUTS(ctx, buf, "]");
}

void
grn_pat_cursor_inspect(grn_ctx *ctx, grn_pat_cursor *c, grn_obj *buf)
{
  GRN_TEXT_PUTS(ctx, buf, "#<cursor:pat:");
  grn_inspect_name(ctx, buf, (grn_obj *)(c->pat));

  GRN_TEXT_PUTS(ctx, buf, " ");
  GRN_TEXT_PUTS(ctx, buf, "current:");
  grn_text_lltoa(ctx, buf, c->curr_rec);

  GRN_TEXT_PUTS(ctx, buf, " ");
  GRN_TEXT_PUTS(ctx, buf, "tail:");
  grn_text_lltoa(ctx, buf, c->tail);

  GRN_TEXT_PUTS(ctx, buf, " ");
  GRN_TEXT_PUTS(ctx, buf, "flags:");
  if (c->obj.header.flags & GRN_CURSOR_PREFIX) {
    GRN_TEXT_PUTS(ctx, buf, "prefix");
  } else {
    if (c->obj.header.flags & GRN_CURSOR_DESCENDING) {
      GRN_TEXT_PUTS(ctx, buf, "descending");
    } else {
      GRN_TEXT_PUTS(ctx, buf, "ascending");
    }
    GRN_TEXT_PUTS(ctx, buf, "|");
    if (c->obj.header.flags & GRN_CURSOR_GT) {
      GRN_TEXT_PUTS(ctx, buf, "greater-than");
    } else {
      GRN_TEXT_PUTS(ctx, buf, "greater");
    }
    GRN_TEXT_PUTS(ctx, buf, "|");
    if (c->obj.header.flags & GRN_CURSOR_LT) {
      GRN_TEXT_PUTS(ctx, buf, "less-than");
    } else {
      GRN_TEXT_PUTS(ctx, buf, "less");
    }
    if (c->obj.header.flags & GRN_CURSOR_BY_ID) {
      GRN_TEXT_PUTS(ctx, buf, "|by-id");
    }
    if (c->obj.header.flags & GRN_CURSOR_BY_KEY) {
      GRN_TEXT_PUTS(ctx, buf, "|by-key");
    }
  }

  GRN_TEXT_PUTS(ctx, buf, " ");
  GRN_TEXT_PUTS(ctx, buf, "rest:");
  grn_text_lltoa(ctx, buf, c->rest);

  GRN_TEXT_PUTS(ctx, buf, " ");
  GRN_TEXT_PUTS(ctx, buf, "entries:");
  grn_pat_cursor_inspect_entries(ctx, c, buf);

  GRN_TEXT_PUTS(ctx, buf, ">");
}

typedef struct {
  uint8_t code;
  uint8_t next;
  uint8_t emit;
  uint8_t attr;
} rk_tree_node;

static uint16_t rk_str_idx[] = {
  0x0003, 0x0006, 0x0009, 0x000c, 0x0012, 0x0015, 0x0018, 0x001e, 0x0024, 0x002a,
  0x0030, 0x0036, 0x003c, 0x0042, 0x0048, 0x004e, 0x0054, 0x005a, 0x0060, 0x0066,
  0x006c, 0x0072, 0x0078, 0x007e, 0x0084, 0x008a, 0x0090, 0x0096, 0x009c, 0x00a2,
  0x00a8, 0x00ae, 0x00b4, 0x00ba, 0x00c0, 0x00c3, 0x00c6, 0x00c9, 0x00cc, 0x00cf,
  0x00d2, 0x00d5, 0x00db, 0x00e1, 0x00e7, 0x00ea, 0x00f0, 0x00f6, 0x00fc, 0x00ff,
  0x0105, 0x0108, 0x010e, 0x0111, 0x0114, 0x0117, 0x011a, 0x011d, 0x0120, 0x0123,
  0x0129, 0x012f, 0x0135, 0x013b, 0x013e, 0x0144, 0x014a, 0x0150, 0x0156, 0x0159,
  0x015c, 0x015f, 0x0162, 0x0165, 0x0168, 0x016b, 0x016e, 0x0171, 0x0177, 0x017d,
  0x0183, 0x0189, 0x018c, 0x0192, 0x0198, 0x019e, 0x01a1, 0x01a4, 0x01aa, 0x01b0,
  0x01b6, 0x01bc, 0x01bf, 0x01c2, 0x01c8, 0x01ce, 0x01d1, 0x01d7, 0x01dd, 0x01e0,
  0x01e6, 0x01e9, 0x01ef, 0x01f2, 0x01f5, 0x01fb, 0x0201, 0x0207, 0x020d, 0x0213,
  0x0216, 0x0219, 0x021c, 0x021f, 0x0222, 0x0225, 0x0228, 0x022e, 0x0234, 0x023a,
  0x023d, 0x0243, 0x0249, 0x024f, 0x0252, 0x0258, 0x025e, 0x0264, 0x0267, 0x026d,
  0x0273, 0x0279, 0x027f, 0x0285, 0x0288, 0x028b, 0x028e, 0x0291, 0x0294, 0x0297,
  0x029a, 0x029d, 0x02a0, 0x02a3, 0x02a9, 0x02af, 0x02b5, 0x02b8, 0x02bb, 0x02be,
  0x02c1, 0x02c4, 0x02c7, 0x02ca, 0x02cd, 0x02d0, 0x02d3, 0x02d6, 0x02dc, 0x02e2,
  0x02e8, 0x02eb, 0x02ee, 0x02f1, 0x02f4, 0x02f7, 0x02fa, 0x02fd, 0x0300, 0x0303,
  0x0309, 0x030c, 0x0312, 0x0318, 0x031e, 0x0324, 0x0327, 0x032a, 0x032d
};
static char rk_str[] = {
  0xe3, 0x82, 0xa1, 0xe3, 0x82, 0xa2, 0xe3, 0x82, 0xa3, 0xe3, 0x82, 0xa4, 0xe3,
  0x82, 0xa4, 0xe3, 0x82, 0xa7, 0xe3, 0x82, 0xa5, 0xe3, 0x82, 0xa6, 0xe3, 0x82,
  0xa6, 0xe3, 0x82, 0xa2, 0xe3, 0x82, 0xa6, 0xe3, 0x82, 0xa3, 0xe3, 0x82, 0xa6,
  0xe3, 0x82, 0xa4, 0xe3, 0x82, 0xa6, 0xe3, 0x82, 0xa6, 0xe3, 0x82, 0xa6, 0xe3,
  0x82, 0xa7, 0xe3, 0x82, 0xa6, 0xe3, 0x82, 0xa8, 0xe3, 0x82, 0xa6, 0xe3, 0x82,
  0xaa, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xa0, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xa1,
  0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xa2, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xa3, 0xe3,
  0x82, 0xa6, 0xe3, 0x83, 0xa4, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xa5, 0xe3, 0x82,
  0xa6, 0xe3, 0x83, 0xa6, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xa7, 0xe3, 0x82, 0xa6,
  0xe3, 0x83, 0xa8, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xa9, 0xe3, 0x82, 0xa6, 0xe3,
  0x83, 0xaa, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xab, 0xe3, 0x82, 0xa6, 0xe3, 0x83,
  0xac, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xad, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xae,
  0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xaf, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xb0, 0xe3,
  0x82, 0xa6, 0xe3, 0x83, 0xb1, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xb2, 0xe3, 0x82,
  0xa6, 0xe3, 0x83, 0xb3, 0xe3, 0x82, 0xa6, 0xe3, 0x83, 0xbc, 0xe3, 0x82, 0xa7,
  0xe3, 0x82, 0xa8, 0xe3, 0x82, 0xa9, 0xe3, 0x82, 0xaa, 0xe3, 0x82, 0xab, 0xe3,
  0x82, 0xac, 0xe3, 0x82, 0xad, 0xe3, 0x82, 0xad, 0xe3, 0x83, 0xa3, 0xe3, 0x82,
  0xad, 0xe3, 0x83, 0xa5, 0xe3, 0x82, 0xad, 0xe3, 0x83, 0xa7, 0xe3, 0x82, 0xae,
  0xe3, 0x82, 0xae, 0xe3, 0x83, 0xa3, 0xe3, 0x82, 0xae, 0xe3, 0x83, 0xa5, 0xe3,
  0x82, 0xae, 0xe3, 0x83, 0xa7, 0xe3, 0x82, 0xaf, 0xe3, 0x82, 0xaf, 0xe3, 0x82,
  0xa1, 0xe3, 0x82, 0xb0, 0xe3, 0x82, 0xb0, 0xe3, 0x82, 0xa1, 0xe3, 0x82, 0xb1,
  0xe3, 0x82, 0xb2, 0xe3, 0x82, 0xb3, 0xe3, 0x82, 0xb4, 0xe3, 0x82, 0xb5, 0xe3,
  0x82, 0xb6, 0xe3, 0x82, 0xb7, 0xe3, 0x82, 0xb7, 0xe3, 0x82, 0xa7, 0xe3, 0x82,
  0xb7, 0xe3, 0x83, 0xa3, 0xe3, 0x82, 0xb7, 0xe3, 0x83, 0xa5, 0xe3, 0x82, 0xb7,
  0xe3, 0x83, 0xa7, 0xe3, 0x82, 0xb8, 0xe3, 0x82, 0xb8, 0xe3, 0x82, 0xa7, 0xe3,
  0x82, 0xb8, 0xe3, 0x83, 0xa3, 0xe3, 0x82, 0xb8, 0xe3, 0x83, 0xa5, 0xe3, 0x82,
  0xb8, 0xe3, 0x83, 0xa7, 0xe3, 0x82, 0xb9, 0xe3, 0x82, 0xba, 0xe3, 0x82, 0xbb,
  0xe3, 0x82, 0xbc, 0xe3, 0x82, 0xbd, 0xe3, 0x82, 0xbe, 0xe3, 0x82, 0xbf, 0xe3,
  0x83, 0x80, 0xe3, 0x83, 0x81, 0xe3, 0x83, 0x81, 0xe3, 0x82, 0xa7, 0xe3, 0x83,
  0x81, 0xe3, 0x83, 0xa3, 0xe3, 0x83, 0x81, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0x81,
  0xe3, 0x83, 0xa7, 0xe3, 0x83, 0x82, 0xe3, 0x83, 0x82, 0xe3, 0x83, 0xa3, 0xe3,
  0x83, 0x82, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0x82, 0xe3, 0x83, 0xa7, 0xe3, 0x83,
  0x83, 0xe3, 0x83, 0x84, 0xe3, 0x83, 0x84, 0xe3, 0x82, 0xa1, 0xe3, 0x83, 0x84,
  0xe3, 0x82, 0xa3, 0xe3, 0x83, 0x84, 0xe3, 0x82, 0xa7, 0xe3, 0x83, 0x84, 0xe3,
  0x82, 0xa9, 0xe3, 0x83, 0x85, 0xe3, 0x83, 0x86, 0xe3, 0x83, 0x86, 0xe3, 0x82,
  0xa3, 0xe3, 0x83, 0x86, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0x87, 0xe3, 0x83, 0x87,
  0xe3, 0x82, 0xa3, 0xe3, 0x83, 0x87, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0x88, 0xe3,
  0x83, 0x88, 0xe3, 0x82, 0xa5, 0xe3, 0x83, 0x89, 0xe3, 0x83, 0x89, 0xe3, 0x82,
  0xa5, 0xe3, 0x83, 0x8a, 0xe3, 0x83, 0x8b, 0xe3, 0x83, 0x8b, 0xe3, 0x82, 0xa3,
  0xe3, 0x83, 0x8b, 0xe3, 0x82, 0xa7, 0xe3, 0x83, 0x8b, 0xe3, 0x83, 0xa3, 0xe3,
  0x83, 0x8b, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0x8b, 0xe3, 0x83, 0xa7, 0xe3, 0x83,
  0x8c, 0xe3, 0x83, 0x8d, 0xe3, 0x83, 0x8e, 0xe3, 0x83, 0x8f, 0xe3, 0x83, 0x90,
  0xe3, 0x83, 0x91, 0xe3, 0x83, 0x92, 0xe3, 0x83, 0x92, 0xe3, 0x83, 0xa3, 0xe3,
  0x83, 0x92, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0x92, 0xe3, 0x83, 0xa7, 0xe3, 0x83,
  0x93, 0xe3, 0x83, 0x93, 0xe3, 0x83, 0xa3, 0xe3, 0x83, 0x93, 0xe3, 0x83, 0xa5,
  0xe3, 0x83, 0x93, 0xe3, 0x83, 0xa7, 0xe3, 0x83, 0x94, 0xe3, 0x83, 0x94, 0xe3,
  0x83, 0xa3, 0xe3, 0x83, 0x94, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0x94, 0xe3, 0x83,
  0xa7, 0xe3, 0x83, 0x95, 0xe3, 0x83, 0x95, 0xe3, 0x82, 0xa1, 0xe3, 0x83, 0x95,
  0xe3, 0x82, 0xa3, 0xe3, 0x83, 0x95, 0xe3, 0x82, 0xa7, 0xe3, 0x83, 0x95, 0xe3,
  0x82, 0xa9, 0xe3, 0x83, 0x95, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0x96, 0xe3, 0x83,
  0x97, 0xe3, 0x83, 0x98, 0xe3, 0x83, 0x99, 0xe3, 0x83, 0x9a, 0xe3, 0x83, 0x9b,
  0xe3, 0x83, 0x9c, 0xe3, 0x83, 0x9d, 0xe3, 0x83, 0x9e, 0xe3, 0x83, 0x9f, 0xe3,
  0x83, 0x9f, 0xe3, 0x83, 0xa3, 0xe3, 0x83, 0x9f, 0xe3, 0x83, 0xa5, 0xe3, 0x83,
  0x9f, 0xe3, 0x83, 0xa7, 0xe3, 0x83, 0xa0, 0xe3, 0x83, 0xa1, 0xe3, 0x83, 0xa2,
  0xe3, 0x83, 0xa3, 0xe3, 0x83, 0xa4, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0xa6, 0xe3,
  0x83, 0xa7, 0xe3, 0x83, 0xa8, 0xe3, 0x83, 0xa9, 0xe3, 0x83, 0xaa, 0xe3, 0x83,
  0xaa, 0xe3, 0x83, 0xa3, 0xe3, 0x83, 0xaa, 0xe3, 0x83, 0xa5, 0xe3, 0x83, 0xaa,
  0xe3, 0x83, 0xa7, 0xe3, 0x83, 0xab, 0xe3, 0x83, 0xac, 0xe3, 0x83, 0xad, 0xe3,
  0x83, 0xae, 0xe3, 0x83, 0xaf, 0xe3, 0x83, 0xb0, 0xe3, 0x83, 0xb1, 0xe3, 0x83,
  0xb2, 0xe3, 0x83, 0xb3, 0xe3, 0x83, 0xb3, 0xe3, 0x83, 0xbc, 0xe3, 0x83, 0xb4,
  0xe3, 0x83, 0xb4, 0xe3, 0x82, 0xa1, 0xe3, 0x83, 0xb4, 0xe3, 0x82, 0xa3, 0xe3,
  0x83, 0xb4, 0xe3, 0x82, 0xa7, 0xe3, 0x83, 0xb4, 0xe3, 0x82, 0xa9, 0xe3, 0x83,
  0xb5, 0xe3, 0x83, 0xb6, 0xe3, 0x83, 0xbc
};
static uint16_t rk_tree_idx[] = {
  0x001b, 0x0022, 0x0025, 0x0028, 0x002d, 0x0030, 0x0039, 0x003b, 0x003c, 0x003f,
  0x0046, 0x0047, 0x004f, 0x0050, 0x0053, 0x005a, 0x005d, 0x0064, 0x0067, 0x006f,
  0x0070, 0x0073, 0x007d, 0x007f, 0x0081, 0x0082, 0x0083, 0x0088, 0x008f, 0x0092,
  0x00af, 0x00b5, 0x00bc, 0x00bf, 0x00c6, 0x00c9, 0x00d1, 0x00d6, 0x00da, 0x00e4,
  0x00e6, 0x00eb, 0x00ec, 0x00f0, 0x00f6, 0x00fc, 0x00fe, 0x0108, 0x010a, 0x010c,
  0x010d, 0x010e, 0x0113, 0x0118, 0x011f, 0x0123, 0x0125, 0x0164, 0x0180, 0x0183,
  0x0199, 0x01ad
};
static rk_tree_node rk_tree[] = {
  {0x2d, 0x00, 0xb2, 0x01}, {0x61, 0x00, 0x01, 0x01}, {0x62, 0x01, 0xff, 0x01},
  {0x63, 0x03, 0xff, 0x01}, {0x64, 0x06, 0xff, 0x01}, {0x65, 0x00, 0x24, 0x01},
  {0x66, 0x0a, 0xff, 0x01}, {0x67, 0x0c, 0xff, 0x01}, {0x68, 0x0f, 0xff, 0x01},
  {0x69, 0x00, 0x03, 0x01}, {0x6a, 0x11, 0xff, 0x01}, {0x6b, 0x13, 0xff, 0x01},
  {0x6c, 0x16, 0xff, 0x01}, {0x6d, 0x1c, 0xff, 0x01}, {0x6e, 0x1e, 0xff, 0x01},
  {0x6f, 0x00, 0x26, 0x01}, {0x70, 0x20, 0xff, 0x01}, {0x72, 0x22, 0xff, 0x01},
  {0x73, 0x24, 0xff, 0x01}, {0x74, 0x27, 0xff, 0x01}, {0x75, 0x00, 0x06, 0x01},
  {0x76, 0x2c, 0xff, 0x01}, {0x77, 0x2d, 0xff, 0x01}, {0x78, 0x2f, 0xff, 0x01},
  {0x79, 0x35, 0xff, 0x01}, {0x7a, 0x36, 0xff, 0x01}, {0xe3, 0x38, 0xff, 0x01},
  {0x61, 0x00, 0x72, 0x01}, {0x62, 0x01, 0x56, 0x01}, {0x65, 0x00, 0x89, 0x01},
  {0x69, 0x00, 0x78, 0x01}, {0x6f, 0x00, 0x8c, 0x01}, {0x75, 0x00, 0x86, 0x01},
  {0x79, 0x02, 0xff, 0x00}, {0x61, 0x00, 0x79, 0x01}, {0x6f, 0x00, 0x7b, 0x01},
  {0x75, 0x00, 0x7a, 0x01}, {0x63, 0x03, 0x56, 0x01}, {0x68, 0x04, 0xff, 0x01},
  {0x79, 0x05, 0xff, 0x01}, {0x61, 0x00, 0x4f, 0x00}, {0x65, 0x00, 0x4e, 0x00},
  {0x69, 0x00, 0x4d, 0x01}, {0x6f, 0x00, 0x51, 0x00}, {0x75, 0x00, 0x50, 0x00},
  {0x61, 0x00, 0x4f, 0x01}, {0x6f, 0x00, 0x51, 0x01}, {0x75, 0x00, 0x50, 0x01},
  {0x61, 0x00, 0x4c, 0x01}, {0x64, 0x06, 0x56, 0x01}, {0x65, 0x00, 0x60, 0x01},
  {0x68, 0x07, 0xff, 0x00}, {0x69, 0x00, 0x61, 0x00}, {0x6f, 0x00, 0x65, 0x01},
  {0x75, 0x00, 0x5c, 0x01}, {0x77, 0x08, 0xff, 0x00}, {0x79, 0x09, 0xff, 0x01},
  {0x69, 0x00, 0x61, 0x01}, {0x75, 0x00, 0x62, 0x01}, {0x75, 0x00, 0x66, 0x01},
  {0x61, 0x00, 0x53, 0x01}, {0x6f, 0x00, 0x55, 0x01}, {0x75, 0x00, 0x54, 0x01},
  {0x61, 0x00, 0x81, 0x00}, {0x65, 0x00, 0x83, 0x00}, {0x66, 0x0a, 0x56, 0x01},
  {0x69, 0x00, 0x82, 0x00}, {0x6f, 0x00, 0x84, 0x00}, {0x75, 0x00, 0x80, 0x01},
  {0x79, 0x0b, 0xff, 0x00}, {0x75, 0x00, 0x85, 0x01}, {0x61, 0x00, 0x28, 0x01},
  {0x65, 0x00, 0x36, 0x01}, {0x67, 0x0c, 0x56, 0x01}, {0x69, 0x00, 0x2d, 0x01},
  {0x6f, 0x00, 0x38, 0x01}, {0x75, 0x00, 0x33, 0x01}, {0x77, 0x0d, 0xff, 0x00},
  {0x79, 0x0e, 0xff, 0x00}, {0x61, 0x00, 0x34, 0x01}, {0x61, 0x00, 0x2e, 0x01},
  {0x6f, 0x00, 0x30, 0x01}, {0x75, 0x00, 0x2f, 0x01}, {0x61, 0x00, 0x71, 0x01},
  {0x65, 0x00, 0x88, 0x01}, {0x68, 0x0f, 0x56, 0x01}, {0x69, 0x00, 0x74, 0x01},
  {0x6f, 0x00, 0x8b, 0x01}, {0x75, 0x00, 0x80, 0x01}, {0x79, 0x10, 0xff, 0x00},
  {0x61, 0x00, 0x75, 0x01}, {0x6f, 0x00, 0x77, 0x01}, {0x75, 0x00, 0x76, 0x01},
  {0x61, 0x00, 0x42, 0x00}, {0x65, 0x00, 0x41, 0x00}, {0x69, 0x00, 0x40, 0x01},
  {0x6a, 0x11, 0x56, 0x01}, {0x6f, 0x00, 0x44, 0x00}, {0x75, 0x00, 0x43, 0x00},
  {0x79, 0x12, 0xff, 0x00}, {0x61, 0x00, 0x42, 0x01}, {0x6f, 0x00, 0x44, 0x01},
  {0x75, 0x00, 0x43, 0x01}, {0x61, 0x00, 0x27, 0x01}, {0x65, 0x00, 0x35, 0x01},
  {0x69, 0x00, 0x29, 0x01}, {0x6b, 0x13, 0x56, 0x01}, {0x6f, 0x00, 0x37, 0x01},
  {0x75, 0x00, 0x31, 0x01}, {0x77, 0x14, 0xff, 0x00}, {0x79, 0x15, 0xff, 0x00},
  {0x61, 0x00, 0x32, 0x01}, {0x61, 0x00, 0x2a, 0x01}, {0x6f, 0x00, 0x2c, 0x01},
  {0x75, 0x00, 0x2b, 0x01}, {0x61, 0x00, 0x00, 0x01}, {0x65, 0x00, 0x23, 0x01},
  {0x69, 0x00, 0x02, 0x01}, {0x6b, 0x17, 0xff, 0x01}, {0x6c, 0x16, 0x56, 0x01},
  {0x6f, 0x00, 0x25, 0x01}, {0x74, 0x18, 0xff, 0x01}, {0x75, 0x00, 0x05, 0x01},
  {0x77, 0x1a, 0xff, 0x01}, {0x79, 0x1b, 0xff, 0x01}, {0x61, 0x00, 0xb0, 0x01},
  {0x65, 0x00, 0xb1, 0x01}, {0x73, 0x19, 0xff, 0x00}, {0x75, 0x00, 0x56, 0x01},
  {0x75, 0x00, 0x56, 0x01}, {0x61, 0x00, 0xa4, 0x01}, {0x61, 0x00, 0x96, 0x01},
  {0x65, 0x00, 0x23, 0x01}, {0x69, 0x00, 0x02, 0x01}, {0x6f, 0x00, 0x9a, 0x01},
  {0x75, 0x00, 0x98, 0x01}, {0x61, 0x00, 0x8e, 0x01}, {0x65, 0x00, 0x94, 0x01},
  {0x69, 0x00, 0x8f, 0x01}, {0x6d, 0x1c, 0x56, 0x01}, {0x6f, 0x00, 0x95, 0x01},
  {0x75, 0x00, 0x93, 0x01}, {0x79, 0x1d, 0xff, 0x00}, {0x61, 0x00, 0x90, 0x01},
  {0x6f, 0x00, 0x92, 0x01}, {0x75, 0x00, 0x91, 0x01}, {0x00, 0x00, 0xa9, 0x01},
  {0x27, 0x00, 0xa9, 0x00}, {0x2d, 0x00, 0xaa, 0x00}, {0x61, 0x00, 0x67, 0x01},
  {0x62, 0x01, 0xa9, 0x00}, {0x63, 0x03, 0xa9, 0x00}, {0x64, 0x06, 0xa9, 0x00},
  {0x65, 0x00, 0x6f, 0x01}, {0x66, 0x0a, 0xa9, 0x00}, {0x67, 0x0c, 0xa9, 0x00},
  {0x68, 0x0f, 0xa9, 0x00}, {0x69, 0x00, 0x68, 0x01}, {0x6a, 0x11, 0xa9, 0x00},
  {0x6b, 0x13, 0xa9, 0x00}, {0x6c, 0x16, 0xa9, 0x00}, {0x6d, 0x1c, 0xa9, 0x00},
  {0x6e, 0x00, 0xa9, 0x00}, {0x6f, 0x00, 0x70, 0x01}, {0x70, 0x20, 0xa9, 0x00},
  {0x72, 0x22, 0xa9, 0x00}, {0x73, 0x24, 0xa9, 0x00}, {0x74, 0x27, 0xa9, 0x00},
  {0x75, 0x00, 0x6e, 0x01}, {0x76, 0x2c, 0xa9, 0x00}, {0x77, 0x2d, 0xa9, 0x00},
  {0x78, 0x2f, 0xa9, 0x00}, {0x79, 0x1f, 0xff, 0x00}, {0x7a, 0x36, 0xa9, 0x00},
  {0xe3, 0x38, 0xa9, 0x00}, {0x00, 0x00, 0xa9, 0x01}, {0x61, 0x00, 0x6b, 0x01},
  {0x65, 0x00, 0x6a, 0x01}, {0x69, 0x00, 0x69, 0x01}, {0x6f, 0x00, 0x6d, 0x01},
  {0x75, 0x00, 0x6c, 0x01}, {0x61, 0x00, 0x73, 0x01}, {0x65, 0x00, 0x8a, 0x01},
  {0x69, 0x00, 0x7c, 0x01}, {0x6f, 0x00, 0x8d, 0x01}, {0x70, 0x20, 0x56, 0x01},
  {0x75, 0x00, 0x87, 0x01}, {0x79, 0x21, 0xff, 0x00}, {0x61, 0x00, 0x7d, 0x01},
  {0x6f, 0x00, 0x7f, 0x01}, {0x75, 0x00, 0x7e, 0x01}, {0x61, 0x00, 0x9c, 0x01},
  {0x65, 0x00, 0xa2, 0x01}, {0x69, 0x00, 0x9d, 0x01}, {0x6f, 0x00, 0xa3, 0x01},
  {0x72, 0x22, 0x56, 0x01}, {0x75, 0x00, 0xa1, 0x01}, {0x79, 0x23, 0xff, 0x00},
  {0x61, 0x00, 0x9e, 0x01}, {0x6f, 0x00, 0xa0, 0x01}, {0x75, 0x00, 0x9f, 0x01},
  {0x61, 0x00, 0x39, 0x01}, {0x65, 0x00, 0x47, 0x01}, {0x68, 0x25, 0xff, 0x00},
  {0x69, 0x00, 0x3b, 0x01}, {0x6f, 0x00, 0x49, 0x01}, {0x73, 0x24, 0x56, 0x01},
  {0x75, 0x00, 0x45, 0x01}, {0x79, 0x26, 0xff, 0x00}, {0x61, 0x00, 0x3d, 0x00},
  {0x65, 0x00, 0x3c, 0x00}, {0x69, 0x00, 0x3b, 0x01}, {0x6f, 0x00, 0x3f, 0x00},
  {0x75, 0x00, 0x3e, 0x00}, {0x61, 0x00, 0x3d, 0x01}, {0x65, 0x00, 0x3c, 0x01},
  {0x6f, 0x00, 0x3f, 0x01}, {0x75, 0x00, 0x3e, 0x01}, {0x61, 0x00, 0x4b, 0x01},
  {0x65, 0x00, 0x5d, 0x01}, {0x68, 0x28, 0xff, 0x00}, {0x69, 0x00, 0x4d, 0x01},
  {0x6f, 0x00, 0x63, 0x01}, {0x73, 0x29, 0xff, 0x00}, {0x74, 0x27, 0x56, 0x01},
  {0x75, 0x00, 0x57, 0x01}, {0x77, 0x2a, 0xff, 0x00}, {0x79, 0x2b, 0xff, 0x00},
  {0x69, 0x00, 0x5e, 0x01}, {0x75, 0x00, 0x5f, 0x01}, {0x61, 0x00, 0x58, 0x00},
  {0x65, 0x00, 0x5a, 0x00}, {0x69, 0x00, 0x59, 0x00}, {0x6f, 0x00, 0x5b, 0x00},
  {0x75, 0x00, 0x57, 0x01}, {0x75, 0x00, 0x64, 0x01}, {0x61, 0x00, 0x4f, 0x01},
  {0x65, 0x00, 0x4e, 0x01}, {0x6f, 0x00, 0x51, 0x01}, {0x75, 0x00, 0x50, 0x01},
  {0x61, 0x00, 0xac, 0x00}, {0x65, 0x00, 0xae, 0x00}, {0x69, 0x00, 0xad, 0x00},
  {0x6f, 0x00, 0xaf, 0x00}, {0x75, 0x00, 0xab, 0x01}, {0x76, 0x2c, 0x56, 0x01},
  {0x61, 0x00, 0xa5, 0x01}, {0x65, 0x00, 0x0b, 0x01}, {0x69, 0x00, 0x08, 0x01},
  {0x6f, 0x00, 0xa8, 0x01}, {0x77, 0x2d, 0x56, 0x01}, {0x79, 0x2e, 0xff, 0x01},
  {0x65, 0x00, 0xa7, 0x01}, {0x69, 0x00, 0xa6, 0x01}, {0x61, 0x00, 0x00, 0x01},
  {0x65, 0x00, 0x23, 0x01}, {0x69, 0x00, 0x02, 0x01}, {0x6b, 0x30, 0xff, 0x01},
  {0x6f, 0x00, 0x25, 0x01}, {0x74, 0x31, 0xff, 0x01}, {0x75, 0x00, 0x05, 0x01},
  {0x77, 0x33, 0xff, 0x01}, {0x78, 0x2f, 0x56, 0x01}, {0x79, 0x34, 0xff, 0x01},
  {0x61, 0x00, 0xb0, 0x01}, {0x65, 0x00, 0xb1, 0x01}, {0x73, 0x32, 0xff, 0x00},
  {0x75, 0x00, 0x56, 0x01}, {0x75, 0x00, 0x56, 0x01}, {0x61, 0x00, 0xa4, 0x01},
  {0x61, 0x00, 0x96, 0x01}, {0x65, 0x00, 0x23, 0x01}, {0x69, 0x00, 0x02, 0x01},
  {0x6f, 0x00, 0x9a, 0x01}, {0x75, 0x00, 0x98, 0x01}, {0x61, 0x00, 0x97, 0x01},
  {0x65, 0x00, 0x04, 0x01}, {0x6f, 0x00, 0x9b, 0x01}, {0x75, 0x00, 0x99, 0x01},
  {0x79, 0x35, 0x56, 0x01}, {0x61, 0x00, 0x3a, 0x01}, {0x65, 0x00, 0x48, 0x01},
  {0x69, 0x00, 0x40, 0x01}, {0x6f, 0x00, 0x4a, 0x01}, {0x75, 0x00, 0x46, 0x01},
  {0x79, 0x37, 0xff, 0x00}, {0x7a, 0x36, 0x56, 0x01}, {0x61, 0x00, 0x42, 0x01},
  {0x65, 0x00, 0x41, 0x01}, {0x6f, 0x00, 0x44, 0x01}, {0x75, 0x00, 0x43, 0x01},
  {0x81, 0x39, 0xff, 0x01}, {0x82, 0x3d, 0xff, 0x01}, {0x81, 0x00, 0x00, 0x01},
  {0x82, 0x00, 0x01, 0x01}, {0x83, 0x00, 0x02, 0x01}, {0x84, 0x00, 0x03, 0x01},
  {0x85, 0x00, 0x05, 0x01}, {0x86, 0x3a, 0xff, 0x01}, {0x87, 0x00, 0x23, 0x01},
  {0x88, 0x00, 0x24, 0x01}, {0x89, 0x00, 0x25, 0x01}, {0x8a, 0x00, 0x26, 0x01},
  {0x8b, 0x00, 0x27, 0x01}, {0x8c, 0x00, 0x28, 0x01}, {0x8d, 0x00, 0x29, 0x01},
  {0x8e, 0x00, 0x2d, 0x01}, {0x8f, 0x00, 0x31, 0x01}, {0x90, 0x00, 0x33, 0x01},
  {0x91, 0x00, 0x35, 0x01}, {0x92, 0x00, 0x36, 0x01}, {0x93, 0x00, 0x37, 0x01},
  {0x94, 0x00, 0x38, 0x01}, {0x95, 0x00, 0x39, 0x01}, {0x96, 0x00, 0x3a, 0x01},
  {0x97, 0x00, 0x3b, 0x01}, {0x98, 0x00, 0x40, 0x01}, {0x99, 0x00, 0x45, 0x01},
  {0x9a, 0x00, 0x46, 0x01}, {0x9b, 0x00, 0x47, 0x01}, {0x9c, 0x00, 0x48, 0x01},
  {0x9d, 0x00, 0x49, 0x01}, {0x9e, 0x00, 0x4a, 0x01}, {0x9f, 0x00, 0x4b, 0x01},
  {0xa0, 0x00, 0x4c, 0x01}, {0xa1, 0x00, 0x4d, 0x01}, {0xa2, 0x00, 0x52, 0x01},
  {0xa3, 0x00, 0x56, 0x01}, {0xa4, 0x00, 0x57, 0x01}, {0xa5, 0x00, 0x5c, 0x01},
  {0xa6, 0x00, 0x5d, 0x01}, {0xa7, 0x00, 0x60, 0x01}, {0xa8, 0x00, 0x63, 0x01},
  {0xa9, 0x00, 0x65, 0x01}, {0xaa, 0x00, 0x67, 0x01}, {0xab, 0x00, 0x68, 0x01},
  {0xac, 0x00, 0x6e, 0x01}, {0xad, 0x00, 0x6f, 0x01}, {0xae, 0x00, 0x70, 0x01},
  {0xaf, 0x00, 0x71, 0x01}, {0xb0, 0x00, 0x72, 0x01}, {0xb1, 0x00, 0x73, 0x01},
  {0xb2, 0x00, 0x74, 0x01}, {0xb3, 0x00, 0x78, 0x01}, {0xb4, 0x00, 0x7c, 0x01},
  {0xb5, 0x00, 0x80, 0x01}, {0xb6, 0x00, 0x86, 0x01}, {0xb7, 0x00, 0x87, 0x01},
  {0xb8, 0x00, 0x88, 0x01}, {0xb9, 0x00, 0x89, 0x01}, {0xba, 0x00, 0x8a, 0x01},
  {0xbb, 0x00, 0x8b, 0x01}, {0xbc, 0x00, 0x8c, 0x01}, {0xbd, 0x00, 0x8d, 0x01},
  {0xbe, 0x00, 0x8e, 0x01}, {0xbf, 0x00, 0x8f, 0x01}, {0x00, 0x00, 0x06, 0x00},
  {0x2d, 0x00, 0x22, 0x00}, {0x61, 0x00, 0x07, 0x00}, {0x62, 0x01, 0x06, 0x00},
  {0x63, 0x03, 0x06, 0x00}, {0x64, 0x06, 0x06, 0x00}, {0x65, 0x00, 0x0c, 0x00},
  {0x66, 0x0a, 0x06, 0x00}, {0x67, 0x0c, 0x06, 0x00}, {0x68, 0x0f, 0x06, 0x00},
  {0x69, 0x00, 0x09, 0x00}, {0x6a, 0x11, 0x06, 0x00}, {0x6b, 0x13, 0x06, 0x00},
  {0x6c, 0x16, 0x06, 0x00}, {0x6d, 0x1c, 0x06, 0x00}, {0x6e, 0x1e, 0x06, 0x00},
  {0x6f, 0x00, 0x0d, 0x00}, {0x70, 0x20, 0x06, 0x00}, {0x72, 0x22, 0x06, 0x00},
  {0x73, 0x24, 0x06, 0x00}, {0x74, 0x27, 0x06, 0x00}, {0x75, 0x00, 0x0a, 0x00},
  {0x76, 0x2c, 0x06, 0x00}, {0x77, 0x2d, 0x06, 0x00}, {0x78, 0x2f, 0x06, 0x00},
  {0x79, 0x35, 0x06, 0x00}, {0x7a, 0x36, 0x06, 0x00}, {0xe3, 0x3b, 0xff, 0x01},
  {0x00, 0x00, 0x06, 0x00}, {0x81, 0x39, 0x06, 0x00}, {0x82, 0x3c, 0xff, 0x01},
  {0x00, 0x00, 0x06, 0x01}, {0x80, 0x00, 0x0e, 0x00}, {0x81, 0x00, 0x0f, 0x00},
  {0x82, 0x00, 0x10, 0x00}, {0x83, 0x00, 0x11, 0x00}, {0x84, 0x00, 0x12, 0x00},
  {0x85, 0x00, 0x13, 0x00}, {0x86, 0x00, 0x14, 0x00}, {0x87, 0x00, 0x15, 0x00},
  {0x88, 0x00, 0x16, 0x00}, {0x89, 0x00, 0x17, 0x00}, {0x8a, 0x00, 0x18, 0x00},
  {0x8b, 0x00, 0x19, 0x00}, {0x8c, 0x00, 0x1a, 0x00}, {0x8d, 0x00, 0x1b, 0x00},
  {0x8e, 0x00, 0x1c, 0x00}, {0x8f, 0x00, 0x1d, 0x00}, {0x90, 0x00, 0x1e, 0x00},
  {0x91, 0x00, 0x1f, 0x00}, {0x92, 0x00, 0x20, 0x00}, {0x93, 0x00, 0x21, 0x00},
  {0x9b, 0x00, 0xab, 0x01}, {0x80, 0x00, 0x93, 0x01}, {0x81, 0x00, 0x94, 0x01},
  {0x82, 0x00, 0x95, 0x01}, {0x83, 0x00, 0x96, 0x01}, {0x84, 0x00, 0x97, 0x01},
  {0x85, 0x00, 0x98, 0x01}, {0x86, 0x00, 0x99, 0x01}, {0x87, 0x00, 0x9a, 0x01},
  {0x88, 0x00, 0x9b, 0x01}, {0x89, 0x00, 0x9c, 0x01}, {0x8a, 0x00, 0x9d, 0x01},
  {0x8b, 0x00, 0xa1, 0x01}, {0x8c, 0x00, 0xa2, 0x01}, {0x8d, 0x00, 0xa3, 0x01},
  {0x8e, 0x00, 0xa4, 0x01}, {0x8f, 0x00, 0xa5, 0x01}, {0x90, 0x00, 0xa6, 0x01},
  {0x91, 0x00, 0xa7, 0x01}, {0x92, 0x00, 0xa8, 0x01}, {0x93, 0x00, 0xa9, 0x01}
};

static rk_tree_node *
rk_lookup(uint8_t state, uint8_t code)
{
  if (state < sizeof(rk_tree_idx)/sizeof(uint16_t)) {
    uint16_t ns = state ? rk_tree_idx[state - 1] : 0;
    uint16_t ne = rk_tree_idx[state];
    while (ns < ne) {
      uint16_t m = (ns + ne)>>1;
      rk_tree_node *rn = &rk_tree[m];
      if (rn->code == code) { return rn; }
      if (rn->code < code) {
        ns = m + 1;
      } else {
        ne = m;
      }
    }
  }
  return NULL;
}

static uint32_t
rk_emit(rk_tree_node *rn, char **str)
{
  if (rn && rn->emit != 0xff) {
    uint16_t pos = rn->emit ? rk_str_idx[rn->emit - 1] :  0;
    *str = &rk_str[pos];
    return (uint32_t)(rk_str_idx[rn->emit] - pos);
  } else {
    *str = NULL;
    return 0;
  }
}

#define RK_OUTPUT(e,l) do {\
  if (oc < oe) {\
    uint32_t l_ = (oc + (l) < oe) ? (l) : (oe - oc);\
    grn_memcpy(oc, (e), l_);\
    oc += l_;\
    ic_ = ic;\
  }\
} while (0)

static uint32_t
rk_conv(const char *str, uint32_t str_len, uint8_t *buf, uint32_t buf_size, uint8_t *statep)
{
  uint32_t l;
  uint8_t state = 0;
  rk_tree_node *rn;
  char *e;
  uint8_t *oc = buf, *oe = oc + buf_size;
  const uint8_t *ic = (uint8_t *)str, *ic_ = ic, *ie = ic + str_len;
  while (ic < ie) {
    if ((rn = rk_lookup(state, *ic))) {
      ic++;
      if ((l = rk_emit(rn, &e))) { RK_OUTPUT(e, l); }
      state = rn->next;
    } else {
      if (!state) { ic++; }
      if (ic_ < ic) { RK_OUTPUT(ic_, ic - ic_); }
      state = 0;
    }
  }
#ifdef FLUSH_UNRESOLVED_INPUT
  if ((rn = rk_lookup(state, 0))) {
    if ((l = rk_emit(rn, &e))) { RK_OUTPUT(e, l); }
    state = rn->next;
  } else {
    if (ic_ < ic) { RK_OUTPUT(ic_, ic - ic_); }
  }
#endif /* FLUSH_UNRESOLVED_INPUT */
  *statep = state;
  return oc - buf;
}

static grn_id
sub_search(grn_ctx *ctx, grn_pat *pat, grn_id id,
           int *c0, uint8_t *key, uint32_t key_len)
{
  pat_node *pn;
  uint32_t len = key_len * 16;
  if (!key_len) { return id; }
  PAT_AT(pat, id, pn);
  while (pn) {
    int ch;
    ch = PAT_CHK(pn);
    if (*c0 < ch && ch < len - 1) {
      if (ch & 1) {
        id = (ch + 1 < len) ? pn->lr[1] : pn->lr[0];
      } else {
        id = pn->lr[nth_bit(key, ch, len)];
      }
      *c0 = ch;
      PAT_AT(pat, id, pn);
    } else {
      const uint8_t *k = pat_node_get_key(ctx, pat, pn);
      return (k && key_len <= PAT_LEN(pn) && !memcmp(k, key, key_len)) ? id : GRN_ID_NIL;
    }
  }
  return GRN_ID_NIL;
}

static void
search_push(grn_ctx *ctx, grn_pat *pat, grn_pat_cursor *c,
            uint8_t *key, uint32_t key_len, uint8_t state, grn_id id, int c0, int flags)
{
  if (state) {
    int step;
    uint16_t ns, ne;
    if (flags & GRN_CURSOR_DESCENDING) {
      ns = rk_tree_idx[state - 1];
      ne = rk_tree_idx[state];
      step = 1;
    } else {
      ns = rk_tree_idx[state] - 1;
      ne = rk_tree_idx[state - 1] - 1;
      step = -1;
    }
    for (; ns != ne; ns += step) {
      rk_tree_node *rn = &rk_tree[ns];
      if (rn->attr) {
        char *e;
        uint32_t l = rk_emit(rn, &e);
        if (l) {
          if (l + key_len <= GRN_TABLE_MAX_KEY_SIZE) {
            int ch = c0;
            grn_id i;
            grn_memcpy(key + key_len, e, l);
            if ((i = sub_search(ctx, pat, id, &ch, key, key_len + l))) {
              search_push(ctx, pat, c, key, key_len + l, rn->next, i, ch, flags);
            }
          }
        } else {
          search_push(ctx, pat, c, key, key_len, rn->next, id, c0, flags);
        }
      }
    }
  } else {
    pat_node *pn;
    PAT_AT(pat, id, pn);
    if (pn) {
      int ch = PAT_CHK(pn);
      uint32_t len = key_len * 16;
      if (c0 < ch) {
        if (flags & GRN_CURSOR_DESCENDING) {
          if ((ch > len - 1) || !(flags & GRN_CURSOR_GT)) { push(c, pn->lr[0], ch); }
          push(c, pn->lr[1], ch);
        } else {
          push(c, pn->lr[1], ch);
          if ((ch > len - 1) || !(flags & GRN_CURSOR_GT)) { push(c, pn->lr[0], ch); }
        }
      } else {
        if (PAT_LEN(pn) * 16 > len || !(flags & GRN_CURSOR_GT)) { push(c, id, ch); }
      }
    }
  }
}

static grn_rc
set_cursor_rk(grn_ctx *ctx, grn_pat *pat, grn_pat_cursor *c,
                  const void *key, uint32_t key_len, int flags)
{
  grn_id id;
  uint8_t state;
  pat_node *pn;
  int c0 = -1;
  uint32_t len, byte_len;
  uint8_t keybuf[GRN_TABLE_MAX_KEY_SIZE];
  if (flags & GRN_CURSOR_SIZE_BY_BIT) { return GRN_OPERATION_NOT_SUPPORTED; }
  byte_len = rk_conv(key, key_len, keybuf, GRN_TABLE_MAX_KEY_SIZE, &state);
  len = byte_len * 16;
  PAT_AT(pat, 0, pn);
  id = pn->lr[1];
  if ((id = sub_search(ctx, pat, id, &c0, keybuf, byte_len))) {
    search_push(ctx, pat, c, keybuf, byte_len, state, id, c0, flags);
  }
  return ctx->rc;
}

uint32_t
grn_pat_total_key_size(grn_ctx *ctx, grn_pat *pat)
{
  return pat->header->curr_key;
}

grn_bool
grn_pat_is_key_encoded(grn_ctx *ctx, grn_pat *pat)
{
  grn_obj *domain;
  uint32_t key_size;

  domain = grn_ctx_at(ctx, pat->obj.header.domain);
  if (grn_obj_is_type(ctx, domain)) {
    key_size = grn_type_size(ctx, domain);
  } else {
    key_size = sizeof(grn_id);
  }

  return KEY_NEEDS_CONVERT(pat, key_size);
}

grn_rc
grn_pat_dirty(grn_ctx *ctx, grn_pat *pat)
{
  grn_rc rc = GRN_SUCCESS;

  CRITICAL_SECTION_ENTER(pat->lock);
  if (!pat->is_dirty) {
    uint32_t n_dirty_opens;
    pat->is_dirty = GRN_TRUE;
    GRN_ATOMIC_ADD_EX(&(pat->header->n_dirty_opens), 1, n_dirty_opens);
    rc = grn_io_flush(ctx, pat->io);
  }
  CRITICAL_SECTION_LEAVE(pat->lock);

  return rc;
}

grn_bool
grn_pat_is_dirty(grn_ctx *ctx, grn_pat *pat)
{
  return pat->header->n_dirty_opens > 0;
}

grn_rc
grn_pat_clean(grn_ctx *ctx, grn_pat *pat)
{
  grn_rc rc = GRN_SUCCESS;

  CRITICAL_SECTION_ENTER(pat->lock);
  if (pat->is_dirty) {
    uint32_t n_dirty_opens;
    pat->is_dirty = GRN_FALSE;
    GRN_ATOMIC_ADD_EX(&(pat->header->n_dirty_opens), -1, n_dirty_opens);
    rc = grn_io_flush(ctx, pat->io);
  }
  CRITICAL_SECTION_LEAVE(pat->lock);

  return rc;
}

grn_rc
grn_pat_clear_dirty(grn_ctx *ctx, grn_pat *pat)
{
  grn_rc rc = GRN_SUCCESS;

  CRITICAL_SECTION_ENTER(pat->lock);
  pat->is_dirty = GRN_FALSE;
  pat->header->n_dirty_opens = 0;
  rc = grn_io_flush(ctx, pat->io);
  CRITICAL_SECTION_LEAVE(pat->lock);

  return rc;
}
