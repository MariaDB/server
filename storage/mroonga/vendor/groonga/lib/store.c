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
#include "grn_str.h"
#include "grn_store.h"
#include "grn_ctx_impl.h"
#include "grn_output.h"
#include <string.h>

/* rectangular arrays */

#define GRN_RA_W_SEGMENT    22
#define GRN_RA_SEGMENT_SIZE (1 << GRN_RA_W_SEGMENT)

static grn_ra *
_grn_ra_create(grn_ctx *ctx, grn_ra *ra, const char *path, unsigned int element_size)
{
  grn_io *io;
  int max_segments, n_elm, w_elm;
  struct grn_ra_header *header;
  unsigned int actual_size;
  if (element_size > GRN_RA_SEGMENT_SIZE) {
    GRN_LOG(ctx, GRN_LOG_ERROR, "element_size too large (%d)", element_size);
    return NULL;
  }
  for (actual_size = 1; actual_size < element_size; actual_size *= 2) ;
  max_segments = ((GRN_ID_MAX + 1) / GRN_RA_SEGMENT_SIZE) * actual_size;
  io = grn_io_create(ctx, path, sizeof(struct grn_ra_header),
                     GRN_RA_SEGMENT_SIZE, max_segments, grn_io_auto,
                     GRN_IO_EXPIRE_SEGMENT);
  if (!io) { return NULL; }
  header = grn_io_header(io);
  grn_io_set_type(io, GRN_COLUMN_FIX_SIZE);
  header->element_size = actual_size;
  n_elm = GRN_RA_SEGMENT_SIZE / header->element_size;
  for (w_elm = GRN_RA_W_SEGMENT; (1 << w_elm) > n_elm; w_elm--);
  ra->io = io;
  ra->header = header;
  ra->element_mask =  n_elm - 1;
  ra->element_width = w_elm;
  return ra;
}

grn_ra *
grn_ra_create(grn_ctx *ctx, const char *path, unsigned int element_size)
{
  grn_ra *ra = (grn_ra *)GRN_CALLOC(sizeof(grn_ra));
  if (!ra) {
    return NULL;
  }
  GRN_DB_OBJ_SET_TYPE(ra, GRN_COLUMN_FIX_SIZE);
  if (!_grn_ra_create(ctx, ra, path, element_size)) {
    GRN_FREE(ra);
    return NULL;
  }
  return ra;
}

grn_ra *
grn_ra_open(grn_ctx *ctx, const char *path)
{
  grn_io *io;
  int n_elm, w_elm;
  grn_ra *ra = NULL;
  struct grn_ra_header *header;
  uint32_t io_type;
  io = grn_io_open(ctx, path, grn_io_auto);
  if (!io) { return NULL; }
  header = grn_io_header(io);
  io_type = grn_io_get_type(io);
  if (io_type != GRN_COLUMN_FIX_SIZE) {
    ERR(GRN_INVALID_FORMAT,
        "[column][fix-size] file type must be %#04x: <%#04x>",
        GRN_COLUMN_FIX_SIZE, io_type);
    grn_io_close(ctx, io);
    return NULL;
  }
  ra = GRN_MALLOCN(grn_ra, 1);
  if (!ra) {
    grn_io_close(ctx, io);
    return NULL;
  }
  n_elm = GRN_RA_SEGMENT_SIZE / header->element_size;
  for (w_elm = GRN_RA_W_SEGMENT; (1 << w_elm) > n_elm; w_elm--);
  GRN_DB_OBJ_SET_TYPE(ra, GRN_COLUMN_FIX_SIZE);
  ra->io = io;
  ra->header = header;
  ra->element_mask =  n_elm - 1;
  ra->element_width = w_elm;
  return ra;
}

grn_rc
grn_ra_info(grn_ctx *ctx, grn_ra *ra, unsigned int *element_size)
{
  if (!ra) { return GRN_INVALID_ARGUMENT; }
  if (element_size) { *element_size = ra->header->element_size; }
  return GRN_SUCCESS;
}

grn_rc
grn_ra_close(grn_ctx *ctx, grn_ra *ra)
{
  grn_rc rc;
  if (!ra) { return GRN_INVALID_ARGUMENT; }
  rc = grn_io_close(ctx, ra->io);
  GRN_FREE(ra);
  return rc;
}

grn_rc
grn_ra_remove(grn_ctx *ctx, const char *path)
{
  if (!path) { return GRN_INVALID_ARGUMENT; }
  return grn_io_remove(ctx, path);
}

grn_rc
grn_ra_truncate(grn_ctx *ctx, grn_ra *ra)
{
  grn_rc rc;
  const char *io_path;
  char *path;
  unsigned int element_size;
  if ((io_path = grn_io_path(ra->io)) && *io_path != '\0') {
    if (!(path = GRN_STRDUP(io_path))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_path);
      return GRN_NO_MEMORY_AVAILABLE;
    }
  } else {
    path = NULL;
  }
  element_size = ra->header->element_size;
  if ((rc = grn_io_close(ctx, ra->io))) { goto exit; }
  ra->io = NULL;
  if (path && (rc = grn_io_remove(ctx, path))) { goto exit; }
  if (!_grn_ra_create(ctx, ra, path, element_size)) {
    rc = GRN_UNKNOWN_ERROR;
  }
exit:
  if (path) { GRN_FREE(path); }
  return rc;
}

void *
grn_ra_ref(grn_ctx *ctx, grn_ra *ra, grn_id id)
{
  void *p = NULL;
  uint16_t seg;
  if (id > GRN_ID_MAX) { return NULL; }
  seg = id >> ra->element_width;
  GRN_IO_SEG_REF(ra->io, seg, p);
  if (!p) { return NULL; }
  return (void *)(((byte *)p) + ((id & ra->element_mask) * ra->header->element_size));
}

grn_rc
grn_ra_unref(grn_ctx *ctx, grn_ra *ra, grn_id id)
{
  uint16_t seg;
  if (id > GRN_ID_MAX) { return GRN_INVALID_ARGUMENT; }
  seg = id >> ra->element_width;
  GRN_IO_SEG_UNREF(ra->io, seg);
  return GRN_SUCCESS;
}

void *
grn_ra_ref_cache(grn_ctx *ctx, grn_ra *ra, grn_id id, grn_ra_cache *cache)
{
  void *p = NULL;
  uint16_t seg;
  if (id > GRN_ID_MAX) { return NULL; }
  seg = id >> ra->element_width;
  if (seg == cache->seg) {
    p = cache->p;
  } else {
    if (cache->seg != -1) { GRN_IO_SEG_UNREF(ra->io, cache->seg); }
    GRN_IO_SEG_REF(ra->io, seg, p);
    cache->seg = seg;
    cache->p = p;
  }
  if (!p) { return NULL; }
  return (void *)(((byte *)p) + ((id & ra->element_mask) * ra->header->element_size));
}

grn_rc
grn_ra_cache_fin(grn_ctx *ctx, grn_ra *ra, grn_id id)
{
  uint16_t seg;
  if (id > GRN_ID_MAX) { return GRN_INVALID_ARGUMENT; }
  seg = id >> ra->element_width;
  GRN_IO_SEG_UNREF(ra->io, seg);
  return GRN_SUCCESS;
}

/**** jagged arrays ****/

#define GRN_JA_W_SEGREGATE_THRESH_V1   7
#define GRN_JA_W_SEGREGATE_THRESH_V2   16
#define GRN_JA_W_CAPACITY              38
#define GRN_JA_W_SEGMENT               22

#define JA_ESEG_VOID                   (0xffffffffU)
#define JA_SEGMENT_SIZE                (1U << GRN_JA_W_SEGMENT)
#define JA_W_EINFO                     3
#define JA_W_SEGMENTS_MAX              (GRN_JA_W_CAPACITY - GRN_JA_W_SEGMENT)
#define JA_W_EINFO_IN_A_SEGMENT        (GRN_JA_W_SEGMENT - JA_W_EINFO)
#define JA_N_EINFO_IN_A_SEGMENT        (1U << JA_W_EINFO_IN_A_SEGMENT)
#define JA_M_EINFO_IN_A_SEGMENT        (JA_N_EINFO_IN_A_SEGMENT - 1)
#define JA_N_GARBAGES_IN_A_SEGMENT     ((1U << (GRN_JA_W_SEGMENT - 3)) - 2)
#define JA_N_ELEMENT_VARIATION_V1      (GRN_JA_W_SEGREGATE_THRESH_V1 - JA_W_EINFO + 1)
#define JA_N_ELEMENT_VARIATION_V2      (GRN_JA_W_SEGREGATE_THRESH_V2 - JA_W_EINFO + 1)
#define JA_N_DSEGMENTS                 (1U << JA_W_SEGMENTS_MAX)
#define JA_N_ESEGMENTS                 (1U << (GRN_ID_WIDTH - JA_W_EINFO_IN_A_SEGMENT))

typedef struct _grn_ja_einfo grn_ja_einfo;

struct _grn_ja_einfo {
  union {
    struct {
      uint16_t seg;
      uint16_t pos;
      uint16_t size;
      uint8_t c1;
      uint8_t c2;
    } n;
    struct {
      uint32_t size;
      uint16_t seg;
      uint8_t c1;
      uint8_t c2;
    } h;
    uint8_t c[8];
  } u;
};

#define ETINY (0x80)
#define EHUGE (0x40)
#define ETINY_P(e) ((e)->u.c[7] & ETINY)
#define ETINY_ENC(e,_size) ((e)->u.c[7] = (_size) + ETINY)
#define ETINY_DEC(e,_size) ((_size) = (e)->u.c[7] & ~(ETINY|EHUGE))
#define EHUGE_P(e) ((e)->u.c[7] & EHUGE)
#define EHUGE_ENC(e,_seg,_size) do {\
  (e)->u.h.c1 = 0;\
  (e)->u.h.c2 = EHUGE;\
  (e)->u.h.seg = (_seg);\
  (e)->u.h.size = (_size);\
} while (0)
#define EHUGE_DEC(e,_seg,_size) do {\
  (_seg) = (e)->u.h.seg;\
  (_size) = (e)->u.h.size;\
} while (0)
#define EINFO_ENC(e,_seg,_pos,_size) do {\
  (e)->u.n.c1 = (_pos) >> 16;\
  (e)->u.n.c2 = ((_size) >> 16);\
  (e)->u.n.seg = (_seg);\
  (e)->u.n.pos = (_pos);\
  (e)->u.n.size = (_size);\
} while (0)
#define EINFO_DEC(e,_seg,_pos,_size) do {\
  (_seg) = (e)->u.n.seg;\
  (_pos) = ((e)->u.n.c1 << 16) + (e)->u.n.pos;\
  (_size) = ((e)->u.n.c2 << 16) + (e)->u.n.size;\
} while (0)

typedef struct {
  uint32_t seg;
  uint32_t pos;
} ja_pos;

typedef struct {
  uint32_t head;
  uint32_t tail;
  uint32_t nrecs;
  uint32_t next;
  ja_pos recs[JA_N_GARBAGES_IN_A_SEGMENT];
} grn_ja_ginfo;

struct grn_ja_header_v1 {
  uint32_t flags;
  uint32_t curr_seg;
  uint32_t curr_pos;
  uint32_t max_element_size;
  ja_pos free_elements[JA_N_ELEMENT_VARIATION_V1];
  uint32_t garbages[JA_N_ELEMENT_VARIATION_V1];
  uint32_t ngarbages[JA_N_ELEMENT_VARIATION_V1];
  uint32_t dsegs[JA_N_DSEGMENTS];
  uint32_t esegs[JA_N_ESEGMENTS];
};

struct grn_ja_header_v2 {
  uint32_t flags;
  uint32_t curr_seg;
  uint32_t curr_pos;
  uint32_t max_element_size;
  ja_pos free_elements[JA_N_ELEMENT_VARIATION_V2];
  uint32_t garbages[JA_N_ELEMENT_VARIATION_V2];
  uint32_t ngarbages[JA_N_ELEMENT_VARIATION_V2];
  uint32_t dsegs[JA_N_DSEGMENTS];
  uint32_t esegs[JA_N_ESEGMENTS];
  uint8_t segregate_threshold;
  uint8_t n_element_variation;
};

struct grn_ja_header {
  uint32_t flags;
  uint32_t *curr_seg;
  uint32_t *curr_pos;
  uint32_t max_element_size;
  ja_pos *free_elements;
  uint32_t *garbages;
  uint32_t *ngarbages;
  uint32_t *dsegs;
  uint32_t *esegs;
  uint8_t segregate_threshold;
  uint8_t n_element_variation;
};

#define SEG_SEQ        (0x10000000U)
#define SEG_HUGE       (0x20000000U)
#define SEG_EINFO      (0x30000000U)
#define SEG_GINFO      (0x40000000U)
#define SEG_MASK       (0xf0000000U)

#define SEGMENTS_AT(ja,seg) ((ja)->header->dsegs[seg])
#define SEGMENTS_SEGRE_ON(ja,seg,width) (SEGMENTS_AT(ja,seg) = width)
#define SEGMENTS_SEQ_ON(ja,seg) (SEGMENTS_AT(ja,seg) = SEG_SEQ)
#define SEGMENTS_HUGE_ON(ja,seg) (SEGMENTS_AT(ja,seg) = SEG_HUGE)
#define SEGMENTS_EINFO_ON(ja,seg,lseg) (SEGMENTS_AT(ja,seg) = SEG_EINFO|(lseg))
#define SEGMENTS_GINFO_ON(ja,seg,width) (SEGMENTS_AT(ja,seg) = SEG_GINFO|(width))
#define SEGMENTS_OFF(ja,seg) (SEGMENTS_AT(ja,seg) = 0)

static grn_ja *
_grn_ja_create(grn_ctx *ctx, grn_ja *ja, const char *path,
               unsigned int max_element_size, uint32_t flags)
{
  int i;
  grn_io *io;
  struct grn_ja_header *header;
  struct grn_ja_header_v2 *header_v2;
  io = grn_io_create(ctx, path, sizeof(struct grn_ja_header_v2),
                     JA_SEGMENT_SIZE, JA_N_DSEGMENTS, grn_io_auto,
                     GRN_IO_EXPIRE_SEGMENT);
  if (!io) { return NULL; }
  grn_io_set_type(io, GRN_COLUMN_VAR_SIZE);

  header_v2 = grn_io_header(io);
  header_v2->flags = flags;
  header_v2->curr_seg = 0;
  header_v2->curr_pos = JA_SEGMENT_SIZE;
  header_v2->max_element_size = max_element_size;
  for (i = 0; i < JA_N_ESEGMENTS; i++) { header_v2->esegs[i] = JA_ESEG_VOID; }
  header_v2->segregate_threshold = GRN_JA_W_SEGREGATE_THRESH_V2;
  header_v2->n_element_variation = JA_N_ELEMENT_VARIATION_V2;

  header = GRN_MALLOCN(struct grn_ja_header, 1);
  if (!header) {
    grn_io_close(ctx, io);
    return NULL;
  }
  header->flags               = header_v2->flags;
  header->curr_seg            = &(header_v2->curr_seg);
  header->curr_pos            = &(header_v2->curr_pos);
  header->max_element_size    = header_v2->max_element_size;
  header->free_elements       = header_v2->free_elements;
  header->garbages            = header_v2->garbages;
  header->ngarbages           = header_v2->ngarbages;
  header->dsegs               = header_v2->dsegs;
  header->esegs               = header_v2->esegs;
  header->segregate_threshold = header_v2->segregate_threshold;
  header->n_element_variation = header_v2->n_element_variation;

  ja->io = io;
  ja->header = header;
  SEGMENTS_EINFO_ON(ja, 0, 0);
  header->esegs[0] = 0;
  return ja;
}

grn_ja *
grn_ja_create(grn_ctx *ctx, const char *path, unsigned int max_element_size, uint32_t flags)
{
  grn_ja *ja = NULL;
  ja = (grn_ja *)GRN_CALLOC(sizeof(grn_ja));
  if (!ja) {
    return NULL;
  }
  GRN_DB_OBJ_SET_TYPE(ja, GRN_COLUMN_VAR_SIZE);
  if (!_grn_ja_create(ctx, ja, path, max_element_size, flags)) {
    GRN_FREE(ja);
    return NULL;
  }
  return ja;
}

grn_ja *
grn_ja_open(grn_ctx *ctx, const char *path)
{
  grn_io *io;
  grn_ja *ja = NULL;
  struct grn_ja_header *header;
  struct grn_ja_header_v2 *header_v2;
  uint32_t io_type;
  io = grn_io_open(ctx, path, grn_io_auto);
  if (!io) { return NULL; }
  header_v2 = grn_io_header(io);
  io_type = grn_io_get_type(io);
  if (io_type != GRN_COLUMN_VAR_SIZE) {
    ERR(GRN_INVALID_FORMAT,
        "[column][var-size] file type must be %#04x: <%#04x>",
        GRN_COLUMN_VAR_SIZE, io_type);
    grn_io_close(ctx, io);
    return NULL;
  }
  if (header_v2->segregate_threshold == 0) {
    header_v2->segregate_threshold = GRN_JA_W_SEGREGATE_THRESH_V1;
  }
  if (header_v2->n_element_variation == 0) {
    header_v2->n_element_variation = JA_N_ELEMENT_VARIATION_V1;
  }
  ja = GRN_MALLOCN(grn_ja, 1);
  if (!ja) {
    grn_io_close(ctx, io);
    return NULL;
  }
  GRN_DB_OBJ_SET_TYPE(ja, GRN_COLUMN_VAR_SIZE);
  header = GRN_MALLOCN(struct grn_ja_header, 1);
  if (!header) {
    grn_io_close(ctx, io);
    GRN_FREE(ja);
    return NULL;
  }

  header->flags               = header_v2->flags;
  header->curr_seg            = &(header_v2->curr_seg);
  header->curr_pos            = &(header_v2->curr_pos);
  header->max_element_size    = header_v2->max_element_size;
  header->segregate_threshold = header_v2->segregate_threshold;
  header->n_element_variation = header_v2->n_element_variation;
  if (header->segregate_threshold == GRN_JA_W_SEGREGATE_THRESH_V1) {
    struct grn_ja_header_v1 *header_v1 = (struct grn_ja_header_v1 *)header_v2;
    header->free_elements = header_v1->free_elements;
    header->garbages      = header_v1->garbages;
    header->ngarbages     = header_v1->ngarbages;
    header->dsegs         = header_v1->dsegs;
    header->esegs         = header_v1->esegs;
  } else {
    header->free_elements = header_v2->free_elements;
    header->garbages      = header_v2->garbages;
    header->ngarbages     = header_v2->ngarbages;
    header->dsegs         = header_v2->dsegs;
    header->esegs         = header_v2->esegs;
  }

  ja->io = io;
  ja->header = header;

  return ja;
}

grn_rc
grn_ja_info(grn_ctx *ctx, grn_ja *ja, unsigned int *max_element_size)
{
  if (!ja) { return GRN_INVALID_ARGUMENT; }
  if (max_element_size) { *max_element_size = ja->header->max_element_size; }
  return GRN_SUCCESS;
}

grn_column_flags
grn_ja_get_flags(grn_ctx *ctx, grn_ja *ja)
{
  if (!ja) {
    return 0;
  }

  return ja->header->flags;
}

grn_rc
grn_ja_close(grn_ctx *ctx, grn_ja *ja)
{
  grn_rc rc;
  if (!ja) { return GRN_INVALID_ARGUMENT; }
  rc = grn_io_close(ctx, ja->io);
  GRN_FREE(ja->header);
  GRN_FREE(ja);
  return rc;
}

grn_rc
grn_ja_remove(grn_ctx *ctx, const char *path)
{
  if (!path) { return GRN_INVALID_ARGUMENT; }
  return grn_io_remove(ctx, path);
}

grn_rc
grn_ja_truncate(grn_ctx *ctx, grn_ja *ja)
{
  grn_rc rc;
  const char *io_path;
  char *path;
  unsigned int max_element_size;
  uint32_t flags;
  if ((io_path = grn_io_path(ja->io)) && *io_path != '\0') {
    if (!(path = GRN_STRDUP(io_path))) {
      ERR(GRN_NO_MEMORY_AVAILABLE, "cannot duplicate path: <%s>", io_path);
      return GRN_NO_MEMORY_AVAILABLE;
    }
  } else {
    path = NULL;
  }
  max_element_size = ja->header->max_element_size;
  flags = ja->header->flags;
  if ((rc = grn_io_close(ctx, ja->io))) { goto exit; }
  ja->io = NULL;
  if (path && (rc = grn_io_remove(ctx, path))) { goto exit; }
  GRN_FREE(ja->header);
  if (!_grn_ja_create(ctx, ja, path, max_element_size, flags)) {
    rc = GRN_UNKNOWN_ERROR;
  }
exit:
  if (path) { GRN_FREE(path); }
  return rc;
}

static void *
grn_ja_ref_raw(grn_ctx *ctx, grn_ja *ja, grn_id id, grn_io_win *iw, uint32_t *value_len)
{
  uint32_t pseg = ja->header->esegs[id >> JA_W_EINFO_IN_A_SEGMENT];
  iw->size = 0;
  iw->addr = NULL;
  iw->pseg = pseg;
  iw->uncompressed_value = NULL;
  if (pseg != JA_ESEG_VOID) {
    grn_ja_einfo *einfo = NULL;
    GRN_IO_SEG_REF(ja->io, pseg, einfo);
    if (einfo) {
      grn_ja_einfo *ei = &einfo[id & JA_M_EINFO_IN_A_SEGMENT];
      if (ETINY_P(ei)) {
        iw->tiny_p = 1;
        ETINY_DEC(ei, iw->size);
        iw->io = ja->io;
        iw->ctx = ctx;
        iw->addr = (void *)ei;
      } else {
        uint32_t jag, vpos, vsize;
        iw->tiny_p = 0;
        if (EHUGE_P(ei)) {
          EHUGE_DEC(ei, jag, vsize);
          vpos = 0;
        } else {
          EINFO_DEC(ei, jag, vpos, vsize);
        }
        grn_io_win_map(ja->io, ctx, iw, jag, vpos, vsize, grn_io_rdonly);
      }
      if (!iw->addr) { GRN_IO_SEG_UNREF(ja->io, pseg); }
    }
  }
  *value_len = iw->size;
  return iw->addr;
}

grn_rc
grn_ja_unref(grn_ctx *ctx, grn_io_win *iw)
{
  if (iw->uncompressed_value) {
    GRN_FREE(iw->uncompressed_value);
    iw->uncompressed_value = NULL;
  }
  if (!iw->addr) { return GRN_INVALID_ARGUMENT; }
  GRN_IO_SEG_UNREF(iw->io, iw->pseg);
  if (!iw->tiny_p) { grn_io_win_unmap(iw); }
  return GRN_SUCCESS;
}

#define DELETED 0x80000000

static grn_rc
grn_ja_free(grn_ctx *ctx, grn_ja *ja, grn_ja_einfo *einfo)
{
  grn_ja_ginfo *ginfo = NULL;
  uint32_t seg, pos, element_size, aligned_size, m, *gseg;
  if (ETINY_P(einfo)) { return GRN_SUCCESS; }
  if (EHUGE_P(einfo)) {
    uint32_t n;
    EHUGE_DEC(einfo, seg, element_size);
    n = ((element_size + JA_SEGMENT_SIZE - 1) >> GRN_JA_W_SEGMENT);
    for (; n--; seg++) { SEGMENTS_OFF(ja, seg); }
    return GRN_SUCCESS;
  }
  EINFO_DEC(einfo, seg, pos, element_size);
  if (!element_size) { return GRN_SUCCESS; }
  {
    int es = element_size - 1;
    GRN_BIT_SCAN_REV(es, m);
    m++;
  }
  if (m > ja->header->segregate_threshold) {
    byte *addr = NULL;
    GRN_IO_SEG_REF(ja->io, seg, addr);
    if (!addr) { return GRN_NO_MEMORY_AVAILABLE; }
    aligned_size = (element_size + sizeof(grn_id) - 1) & ~(sizeof(grn_id) - 1);
    *(uint32_t *)(addr + pos - sizeof(grn_id)) = DELETED|aligned_size;
    if (SEGMENTS_AT(ja, seg) < (aligned_size + sizeof(grn_id)) + SEG_SEQ) {
      GRN_LOG(ctx, GRN_WARN, "inconsistent ja entry detected (%d > %d)",
              element_size, SEGMENTS_AT(ja, seg) - SEG_SEQ);
    }
    SEGMENTS_AT(ja, seg) -= (aligned_size + sizeof(grn_id));
    if (SEGMENTS_AT(ja, seg) == SEG_SEQ) {
      /* reuse the segment */
      SEGMENTS_OFF(ja, seg);
      if (seg == *(ja->header->curr_seg)) {
        *(ja->header->curr_pos) = JA_SEGMENT_SIZE;
      }
    }
    GRN_IO_SEG_UNREF(ja->io, seg);
  } else {
    uint32_t lseg = 0, lseg_;
    gseg = &ja->header->garbages[m - JA_W_EINFO];
    while ((lseg_ = *gseg)) {
      if (lseg) { GRN_IO_SEG_UNREF(ja->io, lseg); }
      GRN_IO_SEG_REF(ja->io, lseg_, ginfo);
      if (!ginfo) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      lseg = lseg_;
      if (ginfo->nrecs < JA_N_GARBAGES_IN_A_SEGMENT) { break; }
      gseg = &ginfo->next;
    }
    if (!lseg_) {
      uint32_t i = 0;
      while (SEGMENTS_AT(ja, i)) {
        if (++i >= JA_N_DSEGMENTS) {
          if (lseg) { GRN_IO_SEG_UNREF(ja->io, lseg); }
          return GRN_NO_MEMORY_AVAILABLE;
        }
      }
      SEGMENTS_GINFO_ON(ja, i, m - JA_W_EINFO);
      *gseg = i;
      lseg_ = *gseg;
      if (lseg) { GRN_IO_SEG_UNREF(ja->io, lseg); }
      GRN_IO_SEG_REF(ja->io, lseg_, ginfo);
      lseg = lseg_;
      if (!ginfo) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      ginfo->head = 0;
      ginfo->tail = 0;
      ginfo->nrecs = 0;
      ginfo->next = 0;
    }
    ginfo->recs[ginfo->head].seg = seg;
    ginfo->recs[ginfo->head].pos = pos;
    if (++ginfo->head == JA_N_GARBAGES_IN_A_SEGMENT) { ginfo->head = 0; }
    ginfo->nrecs++;
    ja->header->ngarbages[m - JA_W_EINFO]++;
    if (lseg) { GRN_IO_SEG_UNREF(ja->io, lseg); }
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ja_replace(grn_ctx *ctx, grn_ja *ja, grn_id id,
               grn_ja_einfo *ei, uint64_t *cas)
{
  grn_rc rc = GRN_SUCCESS;
  uint32_t lseg, *pseg, pos;
  grn_ja_einfo *einfo = NULL, eback;
  lseg = id >> JA_W_EINFO_IN_A_SEGMENT;
  pos = id & JA_M_EINFO_IN_A_SEGMENT;
  pseg = &ja->header->esegs[lseg];
  if (grn_io_lock(ctx, ja->io, grn_lock_timeout)) {
    return ctx->rc;
  }
  if (*pseg == JA_ESEG_VOID) {
    int i = 0;
    while (SEGMENTS_AT(ja, i)) {
      if (++i >= JA_N_DSEGMENTS) {
        ERR(GRN_NOT_ENOUGH_SPACE, "grn_ja file (%s) is full", ja->io->path);
        rc = GRN_NOT_ENOUGH_SPACE;
        goto exit;
      }
    }
    SEGMENTS_EINFO_ON(ja, i, lseg);
    GRN_IO_SEG_REF(ja->io, i, einfo);
    if (einfo) {
      *pseg = i;
      memset(einfo, 0, JA_SEGMENT_SIZE);
    }
  } else {
    GRN_IO_SEG_REF(ja->io, *pseg, einfo);
  }
  if (!einfo) {
    rc = GRN_NO_MEMORY_AVAILABLE;
    goto exit;
  }
  eback = einfo[pos];
  if (cas && *cas != *((uint64_t *)&eback)) {
    ERR(GRN_CAS_ERROR, "cas failed (%d)", id);
    GRN_IO_SEG_UNREF(ja->io, *pseg);
    rc = GRN_CAS_ERROR;
    goto exit;
  }
  // smb_wmb();
  {
    uint64_t *location = (uint64_t *)(einfo + pos);
    uint64_t value = *((uint64_t *)ei);
    GRN_SET_64BIT(location, value);
  }
  GRN_IO_SEG_UNREF(ja->io, *pseg);
  grn_ja_free(ctx, ja, &eback);
exit :
  grn_io_unlock(ja->io);
  return rc;
}

#define JA_N_GARBAGES_TH 10

// todo : grn_io_win_map cause verbose copy when nseg > 1, it should be copied directly.
static grn_rc
grn_ja_alloc(grn_ctx *ctx, grn_ja *ja, grn_id id,
             uint32_t element_size, grn_ja_einfo *einfo, grn_io_win *iw)
{
  byte *addr = NULL;
  iw->io = ja->io;
  iw->ctx = ctx;
  iw->cached = 1;
  if (element_size < 8) {
    ETINY_ENC(einfo, element_size);
    iw->tiny_p = 1;
    iw->addr = (void *)einfo;
    return GRN_SUCCESS;
  }
  iw->tiny_p = 0;
  if (grn_io_lock(ctx, ja->io, grn_lock_timeout)) { return ctx->rc; }
  if (element_size + sizeof(grn_id) > JA_SEGMENT_SIZE) {
    int i, j, n = (element_size + JA_SEGMENT_SIZE - 1) >> GRN_JA_W_SEGMENT;
    for (i = 0, j = -1; i < JA_N_DSEGMENTS; i++) {
      if (SEGMENTS_AT(ja, i)) {
        j = i;
      } else {
        if (i == j + n) {
          j++;
          addr = grn_io_win_map(ja->io, ctx, iw, j, 0, element_size, grn_io_wronly);
          if (!addr) {
            grn_io_unlock(ja->io);
            return GRN_NO_MEMORY_AVAILABLE;
          }
          EHUGE_ENC(einfo, j, element_size);
          for (; j <= i; j++) { SEGMENTS_HUGE_ON(ja, j); }
          grn_io_unlock(ja->io);
          return GRN_SUCCESS;
        }
      }
    }
    GRN_LOG(ctx, GRN_LOG_CRIT, "ja full. requested element_size=%d.", element_size);
    grn_io_unlock(ja->io);
    return GRN_NO_MEMORY_AVAILABLE;
  } else {
    ja_pos *vp;
    int m, aligned_size, es = element_size - 1;
    GRN_BIT_SCAN_REV(es, m);
    m++;
    if (m > ja->header->segregate_threshold) {
      uint32_t seg = *(ja->header->curr_seg);
      uint32_t pos = *(ja->header->curr_pos);
      if (pos + element_size + sizeof(grn_id) > JA_SEGMENT_SIZE) {
        seg = 0;
        while (SEGMENTS_AT(ja, seg)) {
          if (++seg >= JA_N_DSEGMENTS) {
            grn_io_unlock(ja->io);
            GRN_LOG(ctx, GRN_LOG_CRIT, "ja full. seg=%d.", seg);
            return GRN_NOT_ENOUGH_SPACE;
          }
        }
        SEGMENTS_SEQ_ON(ja, seg);
        *(ja->header->curr_seg) = seg;
        pos = 0;
      }
      GRN_IO_SEG_REF(ja->io, seg, addr);
      if (!addr) {
        grn_io_unlock(ja->io);
        return GRN_NO_MEMORY_AVAILABLE;
      }
      *(grn_id *)(addr + pos) = id;
      aligned_size = (element_size + sizeof(grn_id) - 1) & ~(sizeof(grn_id) - 1);
      if (pos + aligned_size < JA_SEGMENT_SIZE) {
        *(grn_id *)(addr + pos + aligned_size) = GRN_ID_NIL;
      }
      SEGMENTS_AT(ja, seg) += aligned_size + sizeof(grn_id);
      pos += sizeof(grn_id);
      EINFO_ENC(einfo, seg, pos, element_size);
      iw->segment = seg;
      iw->addr = addr + pos;
      *(ja->header->curr_pos) = pos + aligned_size;
      grn_io_unlock(ja->io);
      return GRN_SUCCESS;
    } else {
      uint32_t lseg = 0, lseg_;
      aligned_size = 1 << m;
      if (ja->header->ngarbages[m - JA_W_EINFO] > JA_N_GARBAGES_TH) {
        grn_ja_ginfo *ginfo = NULL;
        uint32_t seg, pos, *gseg;
        gseg = &ja->header->garbages[m - JA_W_EINFO];
        while ((lseg_ = *gseg)) {
          GRN_IO_SEG_REF(ja->io, lseg_, ginfo);
          if (!ginfo) {
            if (lseg) { GRN_IO_SEG_UNREF(ja->io, lseg); }
            grn_io_unlock(ja->io);
            return GRN_NO_MEMORY_AVAILABLE;
          }
          if (ginfo->next || ginfo->nrecs > JA_N_GARBAGES_TH) {
            seg = ginfo->recs[ginfo->tail].seg;
            pos = ginfo->recs[ginfo->tail].pos;
            GRN_IO_SEG_REF(ja->io, seg, addr);
            if (!addr) {
              if (lseg) { GRN_IO_SEG_UNREF(ja->io, lseg); }
              GRN_IO_SEG_UNREF(ja->io, lseg_);
              grn_io_unlock(ja->io);
              return GRN_NO_MEMORY_AVAILABLE;
            }
            EINFO_ENC(einfo, seg, pos, element_size);
            iw->segment = seg;
            iw->addr = addr + pos;
            if (++ginfo->tail == JA_N_GARBAGES_IN_A_SEGMENT) { ginfo->tail = 0; }
            ginfo->nrecs--;
            ja->header->ngarbages[m - JA_W_EINFO]--;
            if (!ginfo->nrecs) {
              SEGMENTS_OFF(ja, *gseg);
              *gseg = ginfo->next;
            }
            if (lseg) { GRN_IO_SEG_UNREF(ja->io, lseg); }
            GRN_IO_SEG_UNREF(ja->io, lseg_);
            grn_io_unlock(ja->io);
            return GRN_SUCCESS;
          }
          if (lseg) { GRN_IO_SEG_UNREF(ja->io, lseg); }
          if (!ginfo->next) {
            GRN_IO_SEG_UNREF(ja->io, lseg_);
            break;
          }
          lseg = lseg_;
          gseg = &ginfo->next;
        }
      }
      vp = &ja->header->free_elements[m - JA_W_EINFO];
      if (!vp->seg) {
        int i = 0;
        while (SEGMENTS_AT(ja, i)) {
          if (++i >= JA_N_DSEGMENTS) {
            grn_io_unlock(ja->io);
            return GRN_NO_MEMORY_AVAILABLE;
          }
        }
        SEGMENTS_SEGRE_ON(ja, i, m);
        vp->seg = i;
        vp->pos = 0;
      }
    }
    EINFO_ENC(einfo, vp->seg, vp->pos, element_size);
    GRN_IO_SEG_REF(ja->io, vp->seg, addr);
    if (!addr) {
      grn_io_unlock(ja->io);
      return GRN_NO_MEMORY_AVAILABLE;
    }
    iw->segment = vp->seg;
    iw->addr = addr + vp->pos;
    if ((vp->pos += aligned_size) == JA_SEGMENT_SIZE) {
      vp->seg = 0;
      vp->pos = 0;
    }
    iw->uncompressed_value = NULL;
    grn_io_unlock(ja->io);
    return GRN_SUCCESS;
  }
}

static grn_rc
set_value(grn_ctx *ctx, grn_ja *ja, grn_id id, void *value, uint32_t value_len,
          grn_ja_einfo *einfo)
{
  grn_rc rc = GRN_SUCCESS;
  grn_io_win iw;
  if ((ja->header->flags & GRN_OBJ_RING_BUFFER) &&
      value_len >= ja->header->max_element_size) {
    if ((rc = grn_ja_alloc(ctx, ja, id, value_len + sizeof(uint32_t), einfo, &iw))) {
      return rc;
    }
    grn_memcpy(iw.addr, value, value_len);
    memset((byte *)iw.addr + value_len, 0, sizeof(uint32_t));
    grn_io_win_unmap(&iw);
  } else {
    if ((rc = grn_ja_alloc(ctx, ja, id, value_len, einfo, &iw))) { return rc; }
    grn_memcpy(iw.addr, value, value_len);
    grn_io_win_unmap(&iw);
  }
  return rc;
}

static grn_rc
grn_ja_put_raw(grn_ctx *ctx, grn_ja *ja, grn_id id,
               void *value, uint32_t value_len, int flags, uint64_t *cas)
{
  int rc;
  int64_t buf;
  grn_io_win iw;
  grn_ja_einfo einfo;

  if ((flags & GRN_OBJ_SET_MASK) == GRN_OBJ_SET &&
      value_len > 0) {
    grn_io_win jw;
    uint32_t old_len;
    void *old_value;
    grn_bool same_value = GRN_FALSE;

    old_value = grn_ja_ref(ctx, ja, id, &jw, &old_len);
    if (value_len == old_len && memcmp(value, old_value, value_len) == 0) {
      same_value = GRN_TRUE;
    }
    grn_ja_unref(ctx, &jw);
    if (same_value) {
      return GRN_SUCCESS;
    }
  }

  switch (flags & GRN_OBJ_SET_MASK) {
  case GRN_OBJ_APPEND :
    if (value_len) {
      grn_io_win jw;
      uint32_t old_len;
      void *oldvalue = grn_ja_ref(ctx, ja, id, &jw, &old_len);
      if (oldvalue) {
        if ((ja->header->flags & GRN_OBJ_RING_BUFFER) &&
            old_len + value_len >= ja->header->max_element_size) {
          if (old_len >= ja->header->max_element_size) {
            byte *b = oldvalue;
            uint32_t el = old_len - sizeof(uint32_t);
            uint32_t pos = *((uint32_t *)(b + el));
            GRN_ASSERT(pos < el);
            if (el <= pos + value_len) {
              uint32_t rest = el - pos;
              grn_memcpy(b + pos, value, rest);
              grn_memcpy(b, (byte *)value + rest, value_len - rest);
              *((uint32_t *)(b + el)) = value_len - rest;
            } else {
              grn_memcpy(b + pos, value, value_len);
              *((uint32_t *)(b + el)) = pos + value_len;
            }
            return GRN_SUCCESS;
          } else {
            if ((rc = grn_ja_alloc(ctx, ja, id,
                                   value_len + old_len + sizeof(uint32_t),
                                   &einfo, &iw))) {
              grn_ja_unref(ctx, &jw);
              return rc;
            }
            grn_memcpy(iw.addr, oldvalue, old_len);
            grn_memcpy((byte *)iw.addr + old_len, value, value_len);
            memset((byte *)iw.addr + old_len + value_len, 0, sizeof(uint32_t));
            grn_io_win_unmap(&iw);
          }
        } else {
          if ((rc = grn_ja_alloc(ctx, ja, id, value_len + old_len, &einfo, &iw))) {
            grn_ja_unref(ctx, &jw);
            return rc;
          }
          grn_memcpy(iw.addr, oldvalue, old_len);
          grn_memcpy((byte *)iw.addr + old_len, value, value_len);
          grn_io_win_unmap(&iw);
        }
        grn_ja_unref(ctx, &jw);
      } else {
        set_value(ctx, ja, id, value, value_len, &einfo);
      }
    }
    break;
  case GRN_OBJ_PREPEND :
    if (value_len) {
      grn_io_win jw;
      uint32_t old_len;
      void *oldvalue = grn_ja_ref(ctx, ja, id, &jw, &old_len);
      if (oldvalue) {
        if ((ja->header->flags & GRN_OBJ_RING_BUFFER) &&
            old_len + value_len >= ja->header->max_element_size) {
          if (old_len >= ja->header->max_element_size) {
            byte *b = oldvalue;
            uint32_t el = old_len - sizeof(uint32_t);
            uint32_t pos = *((uint32_t *)(b + el));
            GRN_ASSERT(pos < el);
            if (pos < value_len) {
              uint32_t rest = value_len - pos;
              grn_memcpy(b, (byte *)value + rest, pos);
              grn_memcpy(b + el - rest, value, rest);
              *((uint32_t *)(b + el)) = el - rest;
            } else {
              grn_memcpy(b + pos - value_len, value, value_len);
              *((uint32_t *)(b + el)) = pos - value_len;
            }
            return GRN_SUCCESS;
          } else {
            if ((rc = grn_ja_alloc(ctx, ja, id,
                                   value_len + old_len + sizeof(uint32_t),
                                   &einfo, &iw))) {
              grn_ja_unref(ctx, &jw);
              return rc;
            }
            grn_memcpy(iw.addr, value, value_len);
            grn_memcpy((byte *)iw.addr + value_len, oldvalue, old_len);
            memset((byte *)iw.addr + value_len + old_len, 0, sizeof(uint32_t));
            grn_io_win_unmap(&iw);
          }
        } else {
          if ((rc = grn_ja_alloc(ctx, ja, id, value_len + old_len, &einfo, &iw))) {
            grn_ja_unref(ctx, &jw);
            return rc;
          }
          grn_memcpy(iw.addr, value, value_len);
          grn_memcpy((byte *)iw.addr + value_len, oldvalue, old_len);
          grn_io_win_unmap(&iw);
        }
        grn_ja_unref(ctx, &jw);
      } else {
        set_value(ctx, ja, id, value, value_len, &einfo);
      }
    }
    break;
  case GRN_OBJ_DECR :
    if (value_len == sizeof(int64_t)) {
      int64_t *v = (int64_t *)&buf;
      *v = -(*(int64_t *)value);
      value = v;
    } else if (value_len == sizeof(int32_t)) {
      int32_t *v = (int32_t *)&buf;
      *v = -(*(int32_t *)value);
      value = v;
    } else {
      return GRN_INVALID_ARGUMENT;
    }
    /* fallthru */
  case GRN_OBJ_INCR :
    {
      grn_io_win jw;
      uint32_t old_len;
      void *oldvalue = grn_ja_ref(ctx, ja, id, &jw, &old_len);
      if (oldvalue && old_len) {
        grn_rc rc = GRN_INVALID_ARGUMENT;
        if (old_len == sizeof(int64_t) && value_len == sizeof(int64_t)) {
          (*(int64_t *)oldvalue) += (*(int64_t *)value);
          rc = GRN_SUCCESS;
        } else if (old_len == sizeof(int32_t) && value_len == sizeof(int32_t)) {
          (*(int32_t *)oldvalue) += (*(int32_t *)value);
          rc = GRN_SUCCESS;
        }
        grn_ja_unref(ctx, &jw);
        return rc;
      }
    }
    /* fallthru */
  case GRN_OBJ_SET :
    if (value_len) {
      set_value(ctx, ja, id, value, value_len, &einfo);
    } else {
      memset(&einfo, 0, sizeof(grn_ja_einfo));
    }
    break;
  default :
    ERR(GRN_INVALID_ARGUMENT, "grn_ja_put_raw called with illegal flags value");
    return GRN_INVALID_ARGUMENT;
  }
  if ((rc = grn_ja_replace(ctx, ja, id, &einfo, cas))) {
    if (!grn_io_lock(ctx, ja->io, grn_lock_timeout)) {
      grn_ja_free(ctx, ja, &einfo);
      grn_io_unlock(ja->io);
    }
  }
  return rc;
}

grn_rc
grn_ja_putv(grn_ctx *ctx, grn_ja *ja, grn_id id, grn_obj *vector, int flags)
{
  grn_obj header, footer;
  grn_rc rc = GRN_SUCCESS;
  grn_section *vp;
  int i, f = 0, n = grn_vector_size(ctx, vector);
  GRN_TEXT_INIT(&header, 0);
  GRN_TEXT_INIT(&footer, 0);
  grn_text_benc(ctx, &header, n);
  for (i = 0, vp = vector->u.v.sections; i < n; i++, vp++) {
    grn_text_benc(ctx, &header, vp->length);
    if (vp->weight || vp->domain) { f = 1; }
  }
  if (f) {
    for (i = 0, vp = vector->u.v.sections; i < n; i++, vp++) {
      grn_text_benc(ctx, &footer, vp->weight);
      grn_text_benc(ctx, &footer, vp->domain);
    }
  }
  {
    grn_io_win iw;
    grn_ja_einfo einfo;
    grn_obj *body = vector->u.v.body;
    size_t sizeh = GRN_BULK_VSIZE(&header);
    size_t sizev = body ? GRN_BULK_VSIZE(body) : 0;
    size_t sizef = GRN_BULK_VSIZE(&footer);
    if ((rc = grn_ja_alloc(ctx, ja, id, sizeh + sizev + sizef, &einfo, &iw))) { goto exit; }
    grn_memcpy(iw.addr, GRN_BULK_HEAD(&header), sizeh);
    if (body) {
      grn_memcpy((char *)iw.addr + sizeh, GRN_BULK_HEAD(body), sizev);
    }
    if (f) {
      grn_memcpy((char *)iw.addr + sizeh + sizev, GRN_BULK_HEAD(&footer), sizef);
    }
    grn_io_win_unmap(&iw);
    rc = grn_ja_replace(ctx, ja, id, &einfo, NULL);
  }
exit :
  GRN_OBJ_FIN(ctx, &footer);
  GRN_OBJ_FIN(ctx, &header);
  return rc;
}

uint32_t
grn_ja_size(grn_ctx *ctx, grn_ja *ja, grn_id id)
{
  grn_ja_einfo *einfo = NULL, *ei;
  uint32_t lseg, *pseg, pos, size;
  lseg = id >> JA_W_EINFO_IN_A_SEGMENT;
  pos = id & JA_M_EINFO_IN_A_SEGMENT;
  pseg = &ja->header->esegs[lseg];
  if (*pseg == JA_ESEG_VOID) {
    ctx->rc = GRN_INVALID_ARGUMENT;
    return 0;
  }
  GRN_IO_SEG_REF(ja->io, *pseg, einfo);
  if (!einfo) {
    ctx->rc = GRN_NO_MEMORY_AVAILABLE;
    return 0;
  }
  ei = &einfo[pos];
  if (ETINY_P(ei)) {
    ETINY_DEC(ei, size);
  } else {
    if (EHUGE_P(ei)) {
      size = ei->u.h.size;
    } else {
      size = (ei->u.n.c2 << 16) + ei->u.n.size;
    }
  }
  GRN_IO_SEG_UNREF(ja->io, *pseg);
  return size;
}

grn_rc
grn_ja_element_info(grn_ctx *ctx, grn_ja *ja, grn_id id,
                    uint64_t *cas, uint32_t *pos, uint32_t *size)
{
  uint32_t pseg = ja->header->esegs[id >> JA_W_EINFO_IN_A_SEGMENT];
  if (pseg == JA_ESEG_VOID) {
    return GRN_INVALID_ARGUMENT;
  } else {
    grn_ja_einfo *einfo = NULL;
    GRN_IO_SEG_REF(ja->io, pseg, einfo);
    if (einfo) {
      grn_ja_einfo *ei;
      *cas = *((uint64_t *)&einfo[id & JA_M_EINFO_IN_A_SEGMENT]);
      ei = (grn_ja_einfo *)cas;
      if (ETINY_P(ei)) {
        ETINY_DEC(ei, *size);
        *pos = 0;
      } else {
        uint32_t jag;
        if (EHUGE_P(ei)) {
          EHUGE_DEC(ei, jag, *size);
          *pos = 0;
        } else {
          EINFO_DEC(ei, jag, *pos, *size);
        }
      }
      GRN_IO_SEG_UNREF(ja->io, pseg);
    } else {
      return GRN_INVALID_ARGUMENT;
    }
  }
  return GRN_SUCCESS;
}

#define COMPRESSED_VALUE_META_FLAG(meta) ((meta) & 0xf000000000000000)
#define COMPRESSED_VALUE_META_FLAG_RAW             0x1000000000000000
#define COMPRESSED_VALUE_META_UNCOMPRESSED_LEN(meta) \
                                         ((meta) & 0x0fffffffffffffff)

#define COMPRESS_THRESHOLD_BYTE 256
#define COMPRESS_PACKED_VALUE_SIZE_MAX 257
        /* COMPRESS_THRESHOLD_BYTE - 1 + sizeof(uint64_t) = 257 */

#if defined(GRN_WITH_ZLIB) || defined(GRN_WITH_LZ4) || defined(GRN_WITH_ZSTD)
#  define GRN_WITH_COMPRESSED
#endif

#ifdef GRN_WITH_COMPRESSED
static void *
grn_ja_ref_packed(grn_ctx *ctx,
                  grn_io_win *iw,
                  uint32_t *value_len,
                  void *raw_value,
                  uint32_t raw_value_len,
                  void **compressed_value,
                  uint32_t *compressed_value_len,
                  uint32_t *uncompressed_value_len)
{
  uint64_t compressed_value_meta;

  compressed_value_meta = *((uint64_t *)raw_value);
  *compressed_value = (void *)(((uint64_t *)raw_value) + 1);
  *compressed_value_len = raw_value_len - sizeof(uint64_t);

  *uncompressed_value_len =
    COMPRESSED_VALUE_META_UNCOMPRESSED_LEN(compressed_value_meta);
  switch (COMPRESSED_VALUE_META_FLAG(compressed_value_meta)) {
  case COMPRESSED_VALUE_META_FLAG_RAW :
    iw->uncompressed_value = NULL;
    *value_len = *uncompressed_value_len;
    return *compressed_value;
  default :
    return NULL;
  }
}

static grn_rc
grn_ja_put_packed(grn_ctx *ctx,
                  grn_ja *ja,
                  grn_id id,
                  void *value,
                  uint32_t value_len,
                  int flags,
                  uint64_t *cas)
{
  char *packed_value[COMPRESS_PACKED_VALUE_SIZE_MAX];
  uint32_t packed_value_len;
  uint64_t packed_value_meta;

  packed_value_len = value_len + sizeof(uint64_t);
  packed_value_meta = value_len | COMPRESSED_VALUE_META_FLAG_RAW;
  *((uint64_t *)packed_value) = packed_value_meta;
  memcpy(((uint64_t *)packed_value) + 1,
         value,
         value_len);
  return grn_ja_put_raw(ctx,
                        ja,
                        id,
                        packed_value,
                        packed_value_len,
                        flags,
                        cas);
}

static void
grn_ja_compress_error(grn_ctx *ctx,
                      grn_ja *ja,
                      grn_id id,
                      grn_rc rc,
                      const char *message,
                      const char *detail)
{
  char name[GRN_TABLE_MAX_KEY_SIZE];
  int name_len;

  if (ja->obj.id == GRN_ID_NIL) {
    name[0] = '\0';
    name_len = 0;
  } else {
    name_len = grn_obj_name(ctx, (grn_obj *)ja, name, GRN_TABLE_MAX_KEY_SIZE);
  }
  ERR(GRN_ZSTD_ERROR,
      "[ja]%s: %s%.*s%s<%u>%s%s%s",
      message,
      name_len == 0 ? "" : "<",
      name_len,
      name,
      name_len == 0 ? "" : ">: ",
      id,
      detail ? " :<" : "",
      detail ? detail : "",
      detail ? ">" : "");
}
#endif /* GRN_WITH_COMPRESSED */

#ifdef GRN_WITH_ZLIB
#include <zlib.h>

static const char *
grn_zrc_to_string(int zrc)
{
  switch (zrc) {
  case Z_OK :
    return "OK";
  case Z_STREAM_END :
    return "Stream is end";
  case Z_NEED_DICT :
    return "Need dictionary";
  case Z_ERRNO :
    return "See errno";
  case Z_STREAM_ERROR :
    return "Stream error";
  case Z_DATA_ERROR :
    return "Data error";
  case Z_MEM_ERROR :
    return "Memory error";
  case Z_BUF_ERROR :
    return "Buffer error";
  case Z_VERSION_ERROR :
    return "Version error";
  default :
    return "Unknown";
  }
}

static void *
grn_ja_ref_zlib(grn_ctx *ctx, grn_ja *ja, grn_id id, grn_io_win *iw, uint32_t *value_len)
{
  z_stream zstream;
  void *raw_value;
  uint32_t raw_value_len;
  void *zvalue;
  uint32_t zvalue_len;
  void *unpacked_value;
  uint32_t uncompressed_value_len;
  int zrc;

  if (!(raw_value = grn_ja_ref_raw(ctx, ja, id, iw, &raw_value_len))) {
    iw->uncompressed_value = NULL;
    *value_len = 0;
    return NULL;
  }

  unpacked_value = grn_ja_ref_packed(ctx,
                                     iw, value_len,
                                     raw_value, raw_value_len,
                                     &zvalue, &zvalue_len,
                                     &uncompressed_value_len);
  if (unpacked_value) {
    return unpacked_value;
  }

  zstream.next_in = (Bytef *)zvalue;
  zstream.avail_in = zvalue_len;
  zstream.zalloc = Z_NULL;
  zstream.zfree = Z_NULL;
  zrc = inflateInit2(&zstream, 15 /* windowBits */);
  if (zrc != Z_OK) {
    iw->uncompressed_value = NULL;
    *value_len = 0;
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZLIB_ERROR,
                          "[zlib] failed to decompress: initialize",
                          grn_zrc_to_string(zrc));
    return NULL;
  }
  if (!(iw->uncompressed_value = GRN_MALLOC(uncompressed_value_len))) {
    inflateEnd(&zstream);
    iw->uncompressed_value = NULL;
    *value_len = 0;
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZLIB_ERROR,
                          "[zlib] failed to decompress: allocate buffer",
                          NULL);
    return NULL;
  }
  zstream.next_out = (Bytef *)iw->uncompressed_value;
  zstream.avail_out = uncompressed_value_len;
  zrc = inflate(&zstream, Z_FINISH);
  if (zrc != Z_STREAM_END) {
    inflateEnd(&zstream);
    GRN_FREE(iw->uncompressed_value);
    iw->uncompressed_value = NULL;
    *value_len = 0;
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZLIB_ERROR,
                          "[zlib] failed to decompress: finish",
                          grn_zrc_to_string(zrc));
    return NULL;
  }
  *value_len = zstream.total_out;
  zrc = inflateEnd(&zstream);
  if (zrc != Z_OK) {
    GRN_FREE(iw->uncompressed_value);
    iw->uncompressed_value = NULL;
    *value_len = 0;
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZLIB_ERROR,
                          "[zlib] failed to decompress: end",
                          grn_zrc_to_string(zrc));
    return NULL;
  }
  return iw->uncompressed_value;
}
#endif /* GRN_WITH_ZLIB */

#ifdef GRN_WITH_LZ4
#include <lz4.h>

# if (LZ4_VERSION_MAJOR == 1 && LZ4_VERSION_MINOR < 6)
#  define LZ4_compress_default(source, dest, source_size, max_dest_size) \
  LZ4_compress((source), (dest), (source_size))
# endif

static void *
grn_ja_ref_lz4(grn_ctx *ctx, grn_ja *ja, grn_id id, grn_io_win *iw, uint32_t *value_len)
{
  void *raw_value;
  uint32_t raw_value_len;
  void *lz4_value;
  uint32_t lz4_value_len;
  void *unpacked_value;
  uint32_t uncompressed_value_len;

  if (!(raw_value = grn_ja_ref_raw(ctx, ja, id, iw, &raw_value_len))) {
    iw->uncompressed_value = NULL;
    *value_len = 0;
    return NULL;
  }

  unpacked_value = grn_ja_ref_packed(ctx,
                                     iw, value_len,
                                     raw_value, raw_value_len,
                                     &lz4_value, &lz4_value_len,
                                     &uncompressed_value_len);
  if (unpacked_value) {
    return unpacked_value;
  }

  if (!(iw->uncompressed_value = GRN_MALLOC(uncompressed_value_len))) {
    iw->uncompressed_value = NULL;
    *value_len = 0;
    return NULL;
  }
  if (LZ4_decompress_safe((const char *)(lz4_value),
                          (char *)(iw->uncompressed_value),
                          lz4_value_len,
                          uncompressed_value_len) < 0) {
    GRN_FREE(iw->uncompressed_value);
    iw->uncompressed_value = NULL;
    *value_len = 0;
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_LZ4_ERROR,
                          "[lz4] failed to decompress",
                          NULL);
    return NULL;
  }
  *value_len = uncompressed_value_len;
  return iw->uncompressed_value;
}
#endif /* GRN_WITH_LZ4 */

#ifdef GRN_WITH_ZSTD
#include <zstd.h>

static void *
grn_ja_ref_zstd(grn_ctx *ctx,
                grn_ja *ja,
                grn_id id,
                grn_io_win *iw,
                uint32_t *value_len)
{
  void *raw_value;
  uint32_t raw_value_len;
  void *zstd_value;
  uint32_t zstd_value_len;
  void *unpacked_value;
  uint32_t uncompressed_value_len;
  size_t written_len;

  if (!(raw_value = grn_ja_ref_raw(ctx, ja, id, iw, &raw_value_len))) {
    iw->uncompressed_value = NULL;
    *value_len = 0;
    return NULL;
  }

  unpacked_value = grn_ja_ref_packed(ctx,
                                     iw, value_len,
                                     raw_value, raw_value_len,
                                     &zstd_value, &zstd_value_len,
                                     &uncompressed_value_len);
  if (unpacked_value) {
    return unpacked_value;
  }

  if (!(iw->uncompressed_value = GRN_MALLOC(uncompressed_value_len))) {
    iw->uncompressed_value = NULL;
    *value_len = 0;
    return NULL;
  }

  written_len = ZSTD_decompress((char *)(iw->uncompressed_value),
                                uncompressed_value_len,
                                zstd_value,
                                zstd_value_len);
  if (ZSTD_isError(written_len)) {
    GRN_FREE(iw->uncompressed_value);
    iw->uncompressed_value = NULL;
    *value_len = 0;
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZSTD_ERROR,
                          "[zstd] failed to decompress",
                          ZSTD_getErrorName(written_len));
    return NULL;
  }
  *value_len = uncompressed_value_len;
  return iw->uncompressed_value;
}
#endif /* GRN_WITH_ZSTD */

void *
grn_ja_ref(grn_ctx *ctx, grn_ja *ja, grn_id id, grn_io_win *iw, uint32_t *value_len)
{
  switch (ja->header->flags & GRN_OBJ_COMPRESS_MASK) {
#ifdef GRN_WITH_ZLIB
  case GRN_OBJ_COMPRESS_ZLIB :
    return grn_ja_ref_zlib(ctx, ja, id, iw, value_len);
#endif /* GRN_WITH_ZLIB */
#ifdef GRN_WITH_LZ4
  case GRN_OBJ_COMPRESS_LZ4 :
    return grn_ja_ref_lz4(ctx, ja, id, iw, value_len);
#endif /* GRN_WITH_LZ4 */
#ifdef GRN_WITH_ZSTD
  case GRN_OBJ_COMPRESS_ZSTD :
    return grn_ja_ref_zstd(ctx, ja, id, iw, value_len);
#endif /* GRN_WITH_ZSTD */
  default :
    return grn_ja_ref_raw(ctx, ja, id, iw, value_len);
  }
}

grn_obj *
grn_ja_get_value(grn_ctx *ctx, grn_ja *ja, grn_id id, grn_obj *value)
{
  void *v;
  uint32_t len;
  grn_io_win iw;
  if (!value) {
    if (!(value = grn_obj_open(ctx, GRN_BULK, 0, 0))) {
      ERR(GRN_INVALID_ARGUMENT, "grn_obj_get_value failed");
      goto exit;
    }
  }
  if ((v = grn_ja_ref(ctx, ja, id, &iw, &len))) {
    if ((ja->header->flags & GRN_OBJ_RING_BUFFER) &&
        len > ja->header->max_element_size) {
      byte *b = v;
      uint32_t el = len - sizeof(uint32_t);
      uint32_t pos = *((uint32_t *)(b + el));
      GRN_ASSERT(pos < el);
      grn_bulk_write(ctx, value, (char *)(b + pos), el - pos);
      grn_bulk_write(ctx, value, (char *)(b), pos);
    } else {
      grn_bulk_write(ctx, value, v, len);
    }
    grn_ja_unref(ctx, &iw);
  }
exit :
  return value;
}

#ifdef GRN_WITH_ZLIB
inline static grn_rc
grn_ja_put_zlib(grn_ctx *ctx, grn_ja *ja, grn_id id,
                void *value, uint32_t value_len, int flags, uint64_t *cas)
{
  grn_rc rc;
  z_stream zstream;
  void *zvalue;
  int zvalue_len;
  int zrc;

  if (value_len == 0) {
    return grn_ja_put_raw(ctx, ja, id, value, value_len, flags, cas);
  }

  if (value_len < COMPRESS_THRESHOLD_BYTE) {
    return grn_ja_put_packed(ctx, ja, id, value, value_len, flags, cas);
  }

  zstream.next_in = value;
  zstream.avail_in = value_len;
  zstream.zalloc = Z_NULL;
  zstream.zfree = Z_NULL;
  zrc = deflateInit2(&zstream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                     15 /* windowBits */,
                     8 /* memLevel */,
                     Z_DEFAULT_STRATEGY);
  if (zrc != Z_OK) {
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZLIB_ERROR,
                          "[zlib] failed to compress: initialize",
                          grn_zrc_to_string(zrc));
    return ctx->rc;
  }
  zvalue_len = deflateBound(&zstream, value_len);
  if (!(zvalue = GRN_MALLOC(zvalue_len + sizeof(uint64_t)))) {
    deflateEnd(&zstream);
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZLIB_ERROR,
                          "[zlib] failed to allocate compress buffer",
                          NULL);
    return ctx->rc;
  }
  zstream.next_out = (Bytef *)(((uint64_t *)zvalue) + 1);
  zstream.avail_out = zvalue_len;
  zrc = deflate(&zstream, Z_FINISH);
  if (zrc != Z_STREAM_END) {
    deflateEnd(&zstream);
    GRN_FREE(zvalue);
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZLIB_ERROR,
                          "[zlib] failed to compress: finish",
                          grn_zrc_to_string(zrc));
    return ctx->rc;
  }
  zvalue_len = zstream.total_out;
  zrc = deflateEnd(&zstream);
  if (zrc != Z_OK) {
    GRN_FREE(zvalue);
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZLIB_ERROR,
                          "[zlib] failed to compress: end",
                          grn_zrc_to_string(zrc));
    return ctx->rc;
  }
  *(uint64_t *)zvalue = value_len;
  rc = grn_ja_put_raw(ctx, ja, id, zvalue, zvalue_len + sizeof(uint64_t), flags, cas);
  GRN_FREE(zvalue);
  return rc;
}
#endif /* GRN_WITH_ZLIB */

#ifdef GRN_WITH_LZ4
inline static grn_rc
grn_ja_put_lz4(grn_ctx *ctx, grn_ja *ja, grn_id id,
               void *value, uint32_t value_len, int flags, uint64_t *cas)
{
  grn_rc rc;
  void *packed_value;
  int packed_value_len_max;
  int packed_value_len_real;
  char *lz4_value;
  int lz4_value_len_max;
  int lz4_value_len_real;

  if (value_len == 0) {
    return grn_ja_put_raw(ctx, ja, id, value, value_len, flags, cas);
  }

  if (value_len < COMPRESS_THRESHOLD_BYTE) {
    return grn_ja_put_packed(ctx, ja, id, value, value_len, flags, cas);
  }

  if (value_len > (uint32_t)LZ4_MAX_INPUT_SIZE) {
    uint64_t packed_value_meta;

    packed_value_len_real = value_len + sizeof(uint64_t);
    packed_value = GRN_MALLOC(packed_value_len_real);
    if (!packed_value) {
      grn_ja_compress_error(ctx,
                            ja,
                            id,
                            GRN_LZ4_ERROR,
                            "[lz4] failed to allocate packed buffer",
                            NULL);
      return ctx->rc;
    }
    packed_value_meta = value_len | COMPRESSED_VALUE_META_FLAG_RAW;
    *((uint64_t *)packed_value) = packed_value_meta;
    memcpy(((uint64_t *)packed_value) + 1,
           value,
           value_len);
    rc = grn_ja_put_raw(ctx,
                        ja,
                        id,
                        packed_value,
                        packed_value_len_real,
                        flags,
                        cas);
    GRN_FREE(packed_value);
    return rc;
  }

  lz4_value_len_max = LZ4_compressBound(value_len);
  packed_value_len_max = lz4_value_len_max + sizeof(uint64_t);
  if (!(packed_value = GRN_MALLOC(packed_value_len_max))) {
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_LZ4_ERROR,
                          "[lz4] failed to allocate compress buffer",
                          NULL);
    return ctx->rc;
  }
  lz4_value = (char *)((uint64_t *)packed_value + 1);
  lz4_value_len_real = LZ4_compress_default((const char *)value,
                                            lz4_value,
                                            value_len,
                                            lz4_value_len_max);
  if (lz4_value_len_real <= 0) {
    GRN_FREE(packed_value);
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_LZ4_ERROR,
                          "[lz4] failed to compress",
                          NULL);
    return ctx->rc;
  }
  *(uint64_t *)packed_value = value_len;
  packed_value_len_real = lz4_value_len_real + sizeof(uint64_t);
  rc = grn_ja_put_raw(ctx,
                      ja,
                      id,
                      packed_value,
                      packed_value_len_real,
                      flags,
                      cas);
  GRN_FREE(packed_value);
  return rc;
}
#endif /* GRN_WITH_LZ4 */

#ifdef GRN_WITH_ZSTD
inline static grn_rc
grn_ja_put_zstd(grn_ctx *ctx,
                grn_ja *ja,
                grn_id id,
                void *value,
                uint32_t value_len,
                int flags,
                uint64_t *cas)
{
  grn_rc rc;
  void *packed_value;
  int packed_value_len_max;
  int packed_value_len_real;
  void *zstd_value;
  int zstd_value_len_max;
  int zstd_value_len_real;
  int zstd_compression_level = 3;

  if (value_len == 0) {
    return grn_ja_put_raw(ctx, ja, id, value, value_len, flags, cas);
  }

  if (value_len < COMPRESS_THRESHOLD_BYTE) {
    return grn_ja_put_packed(ctx, ja, id, value, value_len, flags, cas);
  }

  zstd_value_len_max = ZSTD_compressBound(value_len);
  packed_value_len_max = zstd_value_len_max + sizeof(uint64_t);
  if (!(packed_value = GRN_MALLOC(packed_value_len_max))) {
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZSTD_ERROR,
                          "[zstd] failed to allocate compress buffer",
                          NULL);
    return ctx->rc;
  }
  zstd_value = ((uint64_t *)packed_value) + 1;
  zstd_value_len_real = ZSTD_compress(zstd_value, zstd_value_len_max,
                                      value, value_len,
                                      zstd_compression_level);
  if (ZSTD_isError(zstd_value_len_real)) {
    grn_ja_compress_error(ctx,
                          ja,
                          id,
                          GRN_ZSTD_ERROR,
                          "[zstd] failed to compress",
                          ZSTD_getErrorName(zstd_value_len_real));
    return ctx->rc;
  }
  *(uint64_t *)packed_value = value_len;
  packed_value_len_real = zstd_value_len_real + sizeof(uint64_t);
  rc = grn_ja_put_raw(ctx,
                      ja,
                      id,
                      packed_value,
                      packed_value_len_real,
                      flags,
                      cas);
  GRN_FREE(packed_value);
  return rc;
}
#endif /* GRN_WITH_ZSTD */

grn_rc
grn_ja_put(grn_ctx *ctx, grn_ja *ja, grn_id id, void *value, uint32_t value_len,
           int flags, uint64_t *cas)
{
  switch (ja->header->flags & GRN_OBJ_COMPRESS_MASK) {
#ifdef GRN_WITH_ZLIB
  case GRN_OBJ_COMPRESS_ZLIB :
    return grn_ja_put_zlib(ctx, ja, id, value, value_len, flags, cas);
#endif /* GRN_WITH_ZLIB */
#ifdef GRN_WITH_LZ4
  case GRN_OBJ_COMPRESS_LZ4 :
    return grn_ja_put_lz4(ctx, ja, id, value, value_len, flags, cas);
#endif /* GRN_WITH_LZ4 */
#ifdef GRN_WITH_ZSTD
  case GRN_OBJ_COMPRESS_ZSTD :
    return grn_ja_put_zstd(ctx, ja, id, value, value_len, flags, cas);
#endif /* GRN_WITH_ZSTD */
  default :
    return grn_ja_put_raw(ctx, ja, id, value, value_len, flags, cas);
  }
}

static grn_rc
grn_ja_defrag_seg(grn_ctx *ctx, grn_ja *ja, uint32_t seg)
{
  byte *v = NULL, *ve;
  uint32_t element_size, cum = 0, *seginfo = &SEGMENTS_AT(ja,seg), sum;
  sum = (*seginfo & ~SEG_MASK);
  GRN_IO_SEG_REF(ja->io, seg, v);
  if (!v) { return GRN_NO_MEMORY_AVAILABLE; }
  ve = v + JA_SEGMENT_SIZE;
  while (v < ve && cum < sum) {
    grn_id id = *((grn_id *)v);
    if (!id) { break; }
    if (id & DELETED) {
      element_size = (id & ~DELETED);
    } else {
      uint64_t cas;
      uint32_t pos;
      if (grn_ja_element_info(ctx, ja, id, &cas, &pos, &element_size)) { break; }
      if (v + sizeof(uint32_t) != ve - JA_SEGMENT_SIZE + pos) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "dseges[%d] = pos unmatch (%d != %" GRN_FMT_LLD ")",
                seg, pos, (long long int)(v + sizeof(uint32_t) + JA_SEGMENT_SIZE - ve));
        break;
      }
      if (grn_ja_put(ctx, ja, id, v + sizeof(uint32_t), element_size, GRN_OBJ_SET, &cas)) {
        GRN_LOG(ctx, GRN_LOG_WARNING,
                "dseges[%d] = put failed (%d)", seg, id);
        break;
      }
      element_size = (element_size + sizeof(grn_id) - 1) & ~(sizeof(grn_id) - 1);
      cum += sizeof(uint32_t) + element_size;
    }
    v += sizeof(uint32_t) + element_size;
  }
  if (*seginfo) {
    GRN_LOG(ctx, GRN_LOG_WARNING, "dseges[%d] = %d after defrag", seg, (*seginfo & ~SEG_MASK));
  }
  GRN_IO_SEG_UNREF(ja->io, seg);
  return GRN_SUCCESS;
}

int
grn_ja_defrag(grn_ctx *ctx, grn_ja *ja, int threshold)
{
  int nsegs = 0;
  uint32_t seg, ts = 1U << (GRN_JA_W_SEGMENT - threshold);
  for (seg = 0; seg < JA_N_DSEGMENTS; seg++) {
    if (seg == *(ja->header->curr_seg)) { continue; }
    if (((SEGMENTS_AT(ja, seg) & SEG_MASK) == SEG_SEQ) &&
        ((SEGMENTS_AT(ja, seg) & ~SEG_MASK) < ts)) {
      if (!grn_ja_defrag_seg(ctx, ja, seg)) { nsegs++; }
    }
  }
  return nsegs;
}

void
grn_ja_check(grn_ctx *ctx, grn_ja *ja)
{
  char buf[8];
  uint32_t seg;
  struct grn_ja_header *h = ja->header;
  GRN_OUTPUT_ARRAY_OPEN("RESULT", 8);
  GRN_OUTPUT_MAP_OPEN("SUMMARY", 8);
  GRN_OUTPUT_CSTR("flags");
  grn_itoh(h->flags, buf, 8);
  GRN_OUTPUT_STR(buf, 8);
  GRN_OUTPUT_CSTR("curr seg");
  GRN_OUTPUT_INT64(*(h->curr_seg));
  GRN_OUTPUT_CSTR("curr pos");
  GRN_OUTPUT_INT64(*(h->curr_pos));
  GRN_OUTPUT_CSTR("max_element_size");
  GRN_OUTPUT_INT64(h->max_element_size);
  GRN_OUTPUT_CSTR("segregate_threshold");
  GRN_OUTPUT_INT64(h->segregate_threshold);
  GRN_OUTPUT_CSTR("n_element_variation");
  GRN_OUTPUT_INT64(h->n_element_variation);
  GRN_OUTPUT_MAP_CLOSE();
  GRN_OUTPUT_ARRAY_OPEN("DETAIL", -1);
  for (seg = 0; seg < JA_N_DSEGMENTS; seg++) {
    int dseg = SEGMENTS_AT(ja, seg);
    if (dseg) {
      GRN_OUTPUT_MAP_OPEN("SEG", -1);
      GRN_OUTPUT_CSTR("seg id");
      GRN_OUTPUT_INT64(seg);
      GRN_OUTPUT_CSTR("seg type");
      GRN_OUTPUT_INT64((dseg & SEG_MASK)>>28);
      GRN_OUTPUT_CSTR("seg value");
      GRN_OUTPUT_INT64(dseg & ~SEG_MASK);
      if ((dseg & SEG_MASK) == SEG_SEQ) {
        byte *v = NULL, *ve;
        uint32_t element_size, cum = 0, sum = dseg & ~SEG_MASK;
        uint32_t n_del_elements = 0, n_elements = 0, s_del_elements = 0, s_elements = 0;
        GRN_IO_SEG_REF(ja->io, seg, v);
        if (v) {
          /*
          GRN_OUTPUT_CSTR("seg seq");
          GRN_OUTPUT_ARRAY_OPEN("SEQ", -1);
          */
          ve = v + JA_SEGMENT_SIZE;
          while (v < ve && cum < sum) {
            grn_id id = *((grn_id *)v);
            /*
            GRN_OUTPUT_MAP_OPEN("ENTRY", -1);
            GRN_OUTPUT_CSTR("id");
            GRN_OUTPUT_INT64(id);
            */
            if (!id) { break; }
            if (id & DELETED) {
              element_size = (id & ~DELETED);
              n_del_elements++;
              s_del_elements += element_size;
            } else {
              element_size = grn_ja_size(ctx, ja, id);
              element_size = (element_size + sizeof(grn_id) - 1) & ~(sizeof(grn_id) - 1);
              cum += sizeof(uint32_t) + element_size;
              n_elements++;
              s_elements += sizeof(uint32_t) + element_size;
            }
            v += sizeof(uint32_t) + element_size;
            /*
            GRN_OUTPUT_CSTR("size");
            GRN_OUTPUT_INT64(element_size);
            GRN_OUTPUT_CSTR("cum");
            GRN_OUTPUT_INT64(cum);
            GRN_OUTPUT_MAP_CLOSE();
            */
          }
          GRN_IO_SEG_UNREF(ja->io, seg);
          /*
          GRN_OUTPUT_ARRAY_CLOSE();
          */
          GRN_OUTPUT_CSTR("n_elements");
          GRN_OUTPUT_INT64(n_elements);
          GRN_OUTPUT_CSTR("s_elements");
          GRN_OUTPUT_INT64(s_elements);
          GRN_OUTPUT_CSTR("n_del_elements");
          GRN_OUTPUT_INT64(n_del_elements);
          GRN_OUTPUT_CSTR("s_del_elements");
          GRN_OUTPUT_INT64(s_del_elements);
          if (cum != sum) {
            GRN_OUTPUT_CSTR("cum gap");
            GRN_OUTPUT_INT64(cum - sum);
          }
        }
      }
      GRN_OUTPUT_MAP_CLOSE();
    }
  }
  GRN_OUTPUT_ARRAY_CLOSE();
  GRN_OUTPUT_ARRAY_CLOSE();
}

/* grn_ja_reader */

grn_rc
grn_ja_reader_init(grn_ctx *ctx, grn_ja_reader *reader, grn_ja *ja)
{
  reader->ja = ja;
  reader->einfo_seg_id = JA_ESEG_VOID;
  reader->ref_avail = GRN_FALSE;
  reader->ref_seg_id = JA_ESEG_VOID;
  reader->ref_seg_ids = NULL;
  reader->nref_seg_ids = 0;
  reader->ref_seg_ids_size = 0;
  reader->body_seg_id = JA_ESEG_VOID;
  reader->body_seg_addr = NULL;
  reader->packed_buf = NULL;
  reader->packed_buf_size = 0;
#ifdef GRN_WITH_ZLIB
  if (reader->ja->header->flags & GRN_OBJ_COMPRESS_ZLIB) {
    z_stream *new_stream = GRN_MALLOCN(z_stream, 1);
    if (!new_stream) {
      return GRN_NO_MEMORY_AVAILABLE;
    }
    new_stream->zalloc = NULL;
    new_stream->zfree = NULL;
    new_stream->opaque = NULL;
    if (inflateInit2(new_stream, 15) != Z_OK) {
      GRN_FREE(new_stream);
      return GRN_ZLIB_ERROR;
    }
    reader->stream = new_stream;
  }
#endif /* GRN_WITH_ZLIB */
  return GRN_SUCCESS;
}

grn_rc
grn_ja_reader_fin(grn_ctx *ctx, grn_ja_reader *reader)
{
  grn_rc rc = GRN_SUCCESS;
  if (reader->einfo_seg_id != JA_ESEG_VOID) {
    GRN_IO_SEG_UNREF(reader->ja->io, reader->einfo_seg_id);
  }
  if (reader->ref_seg_ids) {
    grn_ja_reader_unref(ctx, reader);
    GRN_FREE(reader->ref_seg_ids);
  }
  if (reader->body_seg_addr) {
    GRN_IO_SEG_UNREF(reader->ja->io, reader->body_seg_id);
  }
  if (reader->packed_buf) {
    GRN_FREE(reader->packed_buf);
  }
#ifdef GRN_WITH_ZLIB
  if (reader->ja->header->flags & GRN_OBJ_COMPRESS_ZLIB) {
    if (reader->stream) {
      if (inflateEnd((z_stream *)reader->stream) != Z_OK) {
        rc = GRN_UNKNOWN_ERROR;
      }
      GRN_FREE(reader->stream);
    }
  }
#endif /* GRN_WITH_ZLIB */
  return rc;
}

grn_rc
grn_ja_reader_open(grn_ctx *ctx, grn_ja *ja, grn_ja_reader **reader)
{
  grn_rc rc;
  grn_ja_reader *new_reader = GRN_MALLOCN(grn_ja_reader, 1);
  if (!new_reader) {
    return GRN_NO_MEMORY_AVAILABLE;
  }
  rc = grn_ja_reader_init(ctx, new_reader, ja);
  if (rc != GRN_SUCCESS) {
    GRN_FREE(new_reader);
    return rc;
  }
  *reader = new_reader;
  return GRN_SUCCESS;
}

grn_rc
grn_ja_reader_close(grn_ctx *ctx, grn_ja_reader *reader)
{
  grn_rc rc = grn_ja_reader_fin(ctx, reader);
  GRN_FREE(reader);
  return rc;
}

#ifdef GRN_WITH_COMPRESSED
/* grn_ja_reader_seek_compressed() prepares to access a compressed value. */
static grn_rc
grn_ja_reader_seek_compressed(grn_ctx *ctx, grn_ja_reader *reader, grn_id id)
{
  grn_ja_einfo *einfo;
  void *seg_addr;
  uint32_t seg_id = reader->ja->header->esegs[id >> JA_W_EINFO_IN_A_SEGMENT];
  if (seg_id == JA_ESEG_VOID) {
    return GRN_INVALID_ARGUMENT;
  }
  if (seg_id != reader->einfo_seg_id) {
    GRN_IO_SEG_REF(reader->ja->io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    if (reader->einfo_seg_id != JA_ESEG_VOID) {
      GRN_IO_SEG_UNREF(reader->ja->io, reader->einfo_seg_id);
    }
    reader->einfo_seg_id = seg_id;
    reader->einfo_seg_addr = seg_addr;
  }
  einfo = (grn_ja_einfo *)reader->einfo_seg_addr;
  einfo += id & JA_M_EINFO_IN_A_SEGMENT;
  reader->einfo = einfo;
  /* ETINY_P(einfo) is always false because the original size needs 8 bytes. */
  if (EHUGE_P(einfo)) {
    EHUGE_DEC(einfo, seg_id, reader->packed_size);
    reader->body_seg_offset = 0;
  } else {
    EINFO_DEC(einfo, seg_id, reader->body_seg_offset, reader->packed_size);
  }
  if (seg_id != reader->body_seg_id) {
    GRN_IO_SEG_REF(reader->ja->io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    if (reader->body_seg_addr) {
      GRN_IO_SEG_UNREF(reader->ja->io, reader->body_seg_id);
    }
    reader->body_seg_id = seg_id;
    reader->body_seg_addr = seg_addr;
  }
  seg_addr = (char *)reader->body_seg_addr + reader->body_seg_offset;
  reader->value_size = (uint32_t)*(uint64_t *)seg_addr;
  return GRN_SUCCESS;
}
#endif /* GRN_WITH_COMPRESSED */

/* grn_ja_reader_seek_raw() prepares to access a value. */
static grn_rc
grn_ja_reader_seek_raw(grn_ctx *ctx, grn_ja_reader *reader, grn_id id)
{
  grn_ja_einfo *einfo;
  void *seg_addr;
  uint32_t seg_id = reader->ja->header->esegs[id >> JA_W_EINFO_IN_A_SEGMENT];
  if (seg_id == JA_ESEG_VOID) {
    return GRN_INVALID_ARGUMENT;
  }
  if (seg_id != reader->einfo_seg_id) {
    GRN_IO_SEG_REF(reader->ja->io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    if (reader->einfo_seg_id != JA_ESEG_VOID) {
      GRN_IO_SEG_UNREF(reader->ja->io, reader->einfo_seg_id);
    }
    reader->einfo_seg_id = seg_id;
    reader->einfo_seg_addr = seg_addr;
  }
  einfo = (grn_ja_einfo *)reader->einfo_seg_addr;
  einfo += id & JA_M_EINFO_IN_A_SEGMENT;
  reader->einfo = einfo;
  if (ETINY_P(einfo)) {
    ETINY_DEC(einfo, reader->value_size);
    reader->ref_avail = GRN_FALSE;
  } else {
    if (EHUGE_P(einfo)) {
      EHUGE_DEC(einfo, seg_id, reader->value_size);
      reader->ref_avail = GRN_FALSE;
    } else {
      EINFO_DEC(einfo, seg_id, reader->body_seg_offset, reader->value_size);
      reader->ref_avail = GRN_TRUE;
    }
    if (reader->body_seg_addr) {
      if (seg_id != reader->body_seg_id) {
        GRN_IO_SEG_UNREF(reader->ja->io, reader->body_seg_id);
        reader->body_seg_addr = NULL;
      }
    }
    reader->body_seg_id = seg_id;
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ja_reader_seek(grn_ctx *ctx, grn_ja_reader *reader, grn_id id)
{
  switch (reader->ja->header->flags & GRN_OBJ_COMPRESS_MASK) {
#ifdef GRN_WITH_ZLIB
  case GRN_OBJ_COMPRESS_ZLIB :
    return grn_ja_reader_seek_compressed(ctx, reader, id);
#endif /* GRN_WITH_ZLIB */
#ifdef GRN_WITH_LZ4
  case GRN_OBJ_COMPRESS_LZ4 :
    return grn_ja_reader_seek_compressed(ctx, reader, id);
#endif /* GRN_WITH_LZ4 */
#ifdef GRN_WITH_ZSTD
  case GRN_OBJ_COMPRESS_ZSTD :
    return grn_ja_reader_seek_compressed(ctx, reader, id);
#endif /* GRN_WITH_ZSTD */
  default :
    return grn_ja_reader_seek_raw(ctx, reader, id);
  }
}

grn_rc
grn_ja_reader_ref(grn_ctx *ctx, grn_ja_reader *reader, void **addr)
{
  if (!reader->ref_avail) {
    return GRN_INVALID_ARGUMENT;
  }
  if (reader->body_seg_id != reader->ref_seg_id) {
    void *seg_addr;
    if (reader->nref_seg_ids == reader->ref_seg_ids_size) {
      size_t n_bytes;
      uint32_t new_size, *new_seg_ids;
      if (reader->ref_seg_ids_size == 0) {
        new_size = GRN_JA_READER_INITIAL_REF_SEG_IDS_SIZE;
      } else {
        new_size = reader->ref_seg_ids_size * 2;
      }
      n_bytes = sizeof(uint32_t) * new_size;
      new_seg_ids = (uint32_t *)GRN_REALLOC(reader->ref_seg_ids, n_bytes);
      if (!new_seg_ids) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      reader->ref_seg_ids = new_seg_ids;
      reader->ref_seg_ids_size = new_size;
    }
    GRN_IO_SEG_REF(reader->ja->io, reader->body_seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    reader->ref_seg_id = reader->body_seg_id;
    reader->ref_seg_addr = seg_addr;
    reader->ref_seg_ids[reader->nref_seg_ids++] = reader->body_seg_id;
  }
  *addr = (char *)reader->ref_seg_addr + reader->body_seg_offset;
  return GRN_SUCCESS;
}

grn_rc
grn_ja_reader_unref(grn_ctx *ctx, grn_ja_reader *reader)
{
  uint32_t i;
  for (i = 0; i < reader->nref_seg_ids; i++) {
    GRN_IO_SEG_UNREF(reader->ja->io, reader->ref_seg_ids[i]);
  }
  reader->ref_seg_id = JA_ESEG_VOID;
  reader->nref_seg_ids = 0;
  return GRN_FUNCTION_NOT_IMPLEMENTED;
}

#ifdef GRN_WITH_ZLIB
/* grn_ja_reader_read_zlib() reads a value compressed with zlib. */
static grn_rc
grn_ja_reader_read_zlib(grn_ctx *ctx, grn_ja_reader *reader, void *buf)
{
  uLong dest_size = reader->value_size;
  z_stream *stream = (z_stream *)reader->stream;
  grn_ja_einfo *einfo = (grn_ja_einfo *)reader->einfo;
  if (EHUGE_P(einfo)) {
    /* TODO: Use z_stream to avoid copy. */
    grn_io *io = reader->ja->io;
    void *seg_addr;
    char *packed_ptr;
    uint32_t size, seg_id;
    if (reader->packed_size > reader->packed_buf_size) {
      void *new_buf = GRN_REALLOC(reader->packed_buf, reader->packed_size);
      if (!new_buf) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      reader->packed_buf = new_buf;
      reader->packed_buf_size = reader->packed_size;
    }
    packed_ptr = (char *)reader->packed_buf;
    grn_memcpy(packed_ptr, (char *)reader->body_seg_addr + sizeof(uint64_t),
               io->header->segment_size - sizeof(uint64_t));
    packed_ptr += io->header->segment_size - sizeof(uint64_t);
    size = reader->packed_size - (io->header->segment_size - sizeof(uint64_t));
    seg_id = reader->body_seg_id + 1;
    while (size > io->header->segment_size) {
      GRN_IO_SEG_REF(io, seg_id, seg_addr);
      if (!seg_addr) {
        return GRN_UNKNOWN_ERROR;
      }
      grn_memcpy(packed_ptr, seg_addr, io->header->segment_size);
      GRN_IO_SEG_UNREF(io, seg_id);
      seg_id++;
      size -= io->header->segment_size;
      packed_ptr += io->header->segment_size;
    }
    GRN_IO_SEG_REF(io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    grn_memcpy(packed_ptr, seg_addr, size);
    GRN_IO_SEG_UNREF(io, seg_id);
    seg_id++;
    if (uncompress((Bytef *)buf, &dest_size, (Bytef *)reader->packed_buf,
                   reader->packed_size - sizeof(uint64_t)) != Z_OK) {
      return GRN_ZLIB_ERROR;
    }
    if (dest_size != reader->value_size) {
      return GRN_ZLIB_ERROR;
    }
  } else {
    char *packed_addr = (char *)reader->body_seg_addr;
    packed_addr += reader->body_seg_offset + sizeof(uint64_t);
    if (inflateReset(stream) != Z_OK) {
      return GRN_ZLIB_ERROR;
    }
    stream->next_in = (Bytef *)packed_addr;
    stream->avail_in = reader->packed_size - sizeof(uint64_t);
    stream->next_out = (Bytef *)buf;
    stream->avail_out = dest_size;
    if ((inflate(stream, Z_FINISH) != Z_STREAM_END) || stream->avail_out) {
      return GRN_ZLIB_ERROR;
    }
  }
  return GRN_SUCCESS;
}
#endif /* GRN_WITH_ZLIB */

#ifdef GRN_WITH_LZ4
/* grn_ja_reader_read_lz4() reads a value compressed with LZ4. */
static grn_rc
grn_ja_reader_read_lz4(grn_ctx *ctx, grn_ja_reader *reader, void *buf)
{
  int src_size, dest_size;
  grn_ja_einfo *einfo = (grn_ja_einfo *)reader->einfo;
  if (EHUGE_P(einfo)) {
    grn_io *io = reader->ja->io;
    void *seg_addr;
    char *packed_ptr;
    uint32_t size, seg_id;
    if (reader->packed_size > reader->packed_buf_size) {
      void *new_buf = GRN_REALLOC(reader->packed_buf, reader->packed_size);
      if (!new_buf) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      reader->packed_buf = new_buf;
      reader->packed_buf_size = reader->packed_size;
    }
    packed_ptr = (char *)reader->packed_buf;
    grn_memcpy(packed_ptr, (char *)reader->body_seg_addr + sizeof(uint64_t),
               io->header->segment_size - sizeof(uint64_t));
    packed_ptr += io->header->segment_size - sizeof(uint64_t);
    size = reader->packed_size - (io->header->segment_size - sizeof(uint64_t));
    seg_id = reader->body_seg_id + 1;
    while (size > io->header->segment_size) {
      GRN_IO_SEG_REF(io, seg_id, seg_addr);
      if (!seg_addr) {
        return GRN_UNKNOWN_ERROR;
      }
      grn_memcpy(packed_ptr, seg_addr, io->header->segment_size);
      GRN_IO_SEG_UNREF(io, seg_id);
      seg_id++;
      size -= io->header->segment_size;
      packed_ptr += io->header->segment_size;
    }
    GRN_IO_SEG_REF(io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    grn_memcpy(packed_ptr, seg_addr, size);
    GRN_IO_SEG_UNREF(io, seg_id);
    seg_id++;
    src_size = (int)(reader->packed_size - sizeof(uint64_t));
    dest_size = LZ4_decompress_safe(reader->packed_buf, buf, src_size,
                                    (int)reader->value_size);
  } else {
    char *packed_addr = (char *)reader->body_seg_addr;
    packed_addr += reader->body_seg_offset + sizeof(uint64_t);
    src_size = (int)(reader->packed_size - sizeof(uint64_t));
    dest_size = LZ4_decompress_safe(packed_addr, buf, src_size,
                                    (int)reader->value_size);
  }
  if ((uint32_t)dest_size != reader->value_size) {
    return GRN_LZ4_ERROR;
  }
  return GRN_SUCCESS;
}
#endif /* GRN_WITH_LZ4 */

#ifdef GRN_WITH_ZSTD
/* grn_ja_reader_read_zstd() reads a value compressed with Zstandard. */
static grn_rc
grn_ja_reader_read_zstd(grn_ctx *ctx, grn_ja_reader *reader, void *buf)
{
  int src_size, dest_size;
  grn_ja_einfo *einfo = (grn_ja_einfo *)reader->einfo;
  if (EHUGE_P(einfo)) {
    grn_io *io = reader->ja->io;
    void *seg_addr;
    char *packed_ptr;
    uint32_t size, seg_id;
    if (reader->packed_size > reader->packed_buf_size) {
      void *new_buf = GRN_REALLOC(reader->packed_buf, reader->packed_size);
      if (!new_buf) {
        return GRN_NO_MEMORY_AVAILABLE;
      }
      reader->packed_buf = new_buf;
      reader->packed_buf_size = reader->packed_size;
    }
    packed_ptr = (char *)reader->packed_buf;
    grn_memcpy(packed_ptr, (char *)reader->body_seg_addr + sizeof(uint64_t),
               io->header->segment_size - sizeof(uint64_t));
    packed_ptr += io->header->segment_size - sizeof(uint64_t);
    size = reader->packed_size - (io->header->segment_size - sizeof(uint64_t));
    seg_id = reader->body_seg_id + 1;
    while (size > io->header->segment_size) {
      GRN_IO_SEG_REF(io, seg_id, seg_addr);
      if (!seg_addr) {
        return GRN_UNKNOWN_ERROR;
      }
      grn_memcpy(packed_ptr, seg_addr, io->header->segment_size);
      GRN_IO_SEG_UNREF(io, seg_id);
      seg_id++;
      size -= io->header->segment_size;
      packed_ptr += io->header->segment_size;
    }
    GRN_IO_SEG_REF(io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    grn_memcpy(packed_ptr, seg_addr, size);
    GRN_IO_SEG_UNREF(io, seg_id);
    seg_id++;
    src_size = (int)(reader->packed_size - sizeof(uint64_t));
    dest_size = ZSTD_decompress(reader->packed_buf, reader->value_size,
                                buf, src_size);
  } else {
    char *packed_addr = (char *)reader->body_seg_addr;
    packed_addr += reader->body_seg_offset + sizeof(uint64_t);
    src_size = (int)(reader->packed_size - sizeof(uint64_t));
    dest_size = ZSTD_decompress(packed_addr, reader->value_size,
                                buf, src_size);
  }
  if ((uint32_t)dest_size != reader->value_size) {
    return GRN_ZSTD_ERROR;
  }
  return GRN_SUCCESS;
}
#endif /* GRN_WITH_ZSTD */

/* grn_ja_reader_read_raw() reads a value. */
static grn_rc
grn_ja_reader_read_raw(grn_ctx *ctx, grn_ja_reader *reader, void *buf)
{
  grn_io *io = reader->ja->io;
  grn_ja_einfo *einfo = (grn_ja_einfo *)reader->einfo;
  if (ETINY_P(einfo)) {
    grn_memcpy(buf, einfo, reader->value_size);
  } else if (EHUGE_P(einfo)) {
    char *buf_ptr = (char *)buf;
    void *seg_addr;
    uint32_t seg_id = reader->body_seg_id;
    uint32_t size = reader->value_size;
    while (size > io->header->segment_size) {
      GRN_IO_SEG_REF(io, seg_id, seg_addr);
      if (!seg_addr) {
        return GRN_UNKNOWN_ERROR;
      }
      grn_memcpy(buf_ptr, seg_addr, io->header->segment_size);
      GRN_IO_SEG_UNREF(io, seg_id);
      seg_id++;
      size -= io->header->segment_size;
      buf_ptr += io->header->segment_size;
    }
    GRN_IO_SEG_REF(io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    grn_memcpy(buf_ptr, seg_addr, size);
    GRN_IO_SEG_UNREF(io, seg_id);
    seg_id++;
  } else {
    if (!reader->body_seg_addr) {
      GRN_IO_SEG_REF(io, reader->body_seg_id, reader->body_seg_addr);
      if (!reader->body_seg_addr) {
        return GRN_UNKNOWN_ERROR;
      }
    }
    grn_memcpy(buf, (char *)reader->body_seg_addr + reader->body_seg_offset,
               reader->value_size);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ja_reader_read(grn_ctx *ctx, grn_ja_reader *reader, void *buf)
{
  switch (reader->ja->header->flags & GRN_OBJ_COMPRESS_MASK) {
#ifdef GRN_WITH_ZLIB
  case GRN_OBJ_COMPRESS_ZLIB :
    return grn_ja_reader_read_zlib(ctx, reader, buf);
#endif /* GRN_WITH_ZLIB */
#ifdef GRN_WITH_LZ4
  case GRN_OBJ_COMPRESS_LZ4 :
    return grn_ja_reader_read_lz4(ctx, reader, buf);
#endif /* GRN_WITH_LZ4 */
#ifdef GRN_WITH_ZSTD
  case GRN_OBJ_COMPRESS_ZSTD :
    return grn_ja_reader_read_zstd(ctx, reader, buf);
#endif /* GRN_WITH_ZSTD */
  default :
    return grn_ja_reader_read_raw(ctx, reader, buf);
  }
}

#ifdef GRN_WITH_ZLIB
/* grn_ja_reader_pread_zlib() reads a part of a value compressed with zlib. */
static grn_rc
grn_ja_reader_pread_zlib(grn_ctx *ctx, grn_ja_reader *reader,
                         size_t offset, size_t size, void *buf)
{
  /* TODO: To be supported? */
  return GRN_FUNCTION_NOT_IMPLEMENTED;
}
#endif /* GRN_WITH_ZLIB */

#ifdef GRN_WITH_LZ4
/* grn_ja_reader_pread_lz4() reads a part of a value compressed with LZ4. */
static grn_rc
grn_ja_reader_pread_lz4(grn_ctx *ctx, grn_ja_reader *reader,
                        size_t offset, size_t size, void *buf)
{
  /* TODO: To be supported? */
  return GRN_FUNCTION_NOT_IMPLEMENTED;
}
#endif /* GRN_WITH_LZ4 */

#ifdef GRN_WITH_ZSTD
/* grn_ja_reader_pread_zstd() reads a part of a value compressed with ZSTD. */
static grn_rc
grn_ja_reader_pread_zstd(grn_ctx *ctx, grn_ja_reader *reader,
                         size_t offset, size_t size, void *buf)
{
  /* TODO: To be supported? */
  return GRN_FUNCTION_NOT_IMPLEMENTED;
}
#endif /* GRN_WITH_ZSTD */

/* grn_ja_reader_pread_raw() reads a part of a value. */
static grn_rc
grn_ja_reader_pread_raw(grn_ctx *ctx, grn_ja_reader *reader,
                        size_t offset, size_t size, void *buf)
{
  grn_io *io = reader->ja->io;
  grn_ja_einfo *einfo = (grn_ja_einfo *)reader->einfo;
  if ((offset >= reader->value_size) || !size) {
    return GRN_SUCCESS;
  }
  if (size > (reader->value_size - offset)) {
    size = reader->value_size - offset;
  }
  if (ETINY_P(einfo)) {
    grn_memcpy(buf, (char *)einfo + offset, size);
  } else if (EHUGE_P(einfo)) {
    char *buf_ptr = (char *)buf;
    void *seg_addr;
    uint32_t seg_id = reader->body_seg_id;
    if (offset >= io->header->segment_size) {
      seg_id += offset / io->header->segment_size;
      offset %= io->header->segment_size;
    }
    GRN_IO_SEG_REF(io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    grn_memcpy(buf_ptr, (char *)seg_addr + offset,
               io->header->segment_size - offset);
    GRN_IO_SEG_UNREF(io, seg_id);
    seg_id++;
    size -= io->header->segment_size - offset;
    buf_ptr += io->header->segment_size - offset;
    while (size > io->header->segment_size) {
      GRN_IO_SEG_REF(io, seg_id, seg_addr);
      if (!seg_addr) {
        return GRN_UNKNOWN_ERROR;
      }
      grn_memcpy(buf_ptr, (char *)seg_addr, io->header->segment_size);
      GRN_IO_SEG_UNREF(io, seg_id);
      seg_id++;
      size -= io->header->segment_size;
      buf_ptr += io->header->segment_size;
    }
    GRN_IO_SEG_REF(io, seg_id, seg_addr);
    if (!seg_addr) {
      return GRN_UNKNOWN_ERROR;
    }
    grn_memcpy(buf_ptr, seg_addr, size);
    GRN_IO_SEG_UNREF(io, seg_id);
  } else {
    if (!reader->body_seg_addr) {
      GRN_IO_SEG_REF(io, reader->body_seg_id, reader->body_seg_addr);
      if (!reader->body_seg_addr) {
        return GRN_UNKNOWN_ERROR;
      }
    }
    offset += reader->body_seg_offset;
    grn_memcpy(buf, (char *)reader->body_seg_addr + offset, size);
  }
  return GRN_SUCCESS;
}

grn_rc
grn_ja_reader_pread(grn_ctx *ctx, grn_ja_reader *reader,
                    size_t offset, size_t size, void *buf)
{
  switch (reader->ja->header->flags & GRN_OBJ_COMPRESS_MASK) {
#ifdef GRN_WITH_ZLIB
  case GRN_OBJ_COMPRESS_ZLIB :
    return grn_ja_reader_pread_zlib(ctx, reader, offset, size, buf);
#endif /* GRN_WITH_ZLIB */
#ifdef GRN_WITH_LZ4
  case GRN_OBJ_COMPRESS_LZ4 :
    return grn_ja_reader_pread_lz4(ctx, reader, offset, size, buf);
#endif /* GRN_WITH_LZ4 */
#ifdef GRN_WITH_ZSTD
  case GRN_OBJ_COMPRESS_ZSTD :
    return grn_ja_reader_pread_zstd(ctx, reader, offset, size, buf);
#endif /* GRN_WITH_ZSTD */
  default :
    return grn_ja_reader_pread_raw(ctx, reader, offset, size, buf);
  }
}

/**** vgram ****/

/*

static int len_sum = 0;
static int img_sum = 0;
static int simple_sum = 0;
static int skip_sum = 0;

grn_vgram *
grn_vgram_create(const char *path)
{
  grn_vgram *s;
  if (!(s = GRN_MALLOCN(grn_vgram, 1))) { return NULL; }
  s->vgram = grn_sym_create(path, sizeof(grn_id) * 2, 0, GRN_ENC_NONE);
  if (!s->vgram) {
    GRN_FREE(s);
    return NULL;
  }
  return s;
}

grn_vgram *
grn_vgram_open(const char *path)
{
  grn_vgram *s;
  if (!(s = GRN_MALLOCN(grn_vgram, 1))) { return NULL; }
  s->vgram = grn_sym_open(path);
  if (!s->vgram) {
    GRN_FREE(s);
    return NULL;
  }
  return s;
}

grn_vgram_buf *
grn_vgram_buf_open(size_t len)
{
  grn_vgram_buf *b;
  if (!(b = GRN_MALLOCN(grn_vgram_buf, 1))) { return NULL; }
  b->len = len;
  b->tvs = b->tvp = GRN_MALLOCN(grn_id, len);
  if (!b->tvp) { GRN_FREE(b); return NULL; }
  b->tve = b->tvs + len;
  b->vps = b->vpp = GRN_MALLOCN(grn_vgram_vnode, len * 2);
  if (!b->vpp) { GRN_FREE(b->tvp); GRN_FREE(b); return NULL; }
  b->vpe = b->vps + len;
  return b;
}

grn_rc
grn_vgram_buf_add(grn_vgram_buf *b, grn_id tid)
{
  uint8_t dummybuf[8], *dummyp;
  if (b->tvp < b->tve) { *b->tvp++ = tid; }
  dummyp = dummybuf;
  GRN_B_ENC(tid, dummyp);
  simple_sum += dummyp - dummybuf;
  return GRN_SUCCESS;
}

typedef struct {
  grn_id vid;
  grn_id tid;
} vgram_key;

grn_rc
grn_vgram_update(grn_vgram *vgram, grn_id rid, grn_vgram_buf *b, grn_hash *terms)
{
  grn_inv_updspec **u;
  if (b && b->tvs < b->tvp) {
    grn_id *t0, *tn;
    for (t0 = b->tvs; t0 < b->tvp - 1; t0++) {
      grn_vgram_vnode *v, **vp;
      if (grn_set_at(terms, t0, (void **) &u)) {
        vp = &(*u)->vnodes;
        for (tn = t0 + 1; tn < b->tvp; tn++) {
          for (v = *vp; v && v->tid != *tn; v = v->cdr) ;
          if (!v) {
            if (b->vpp < b->vpe) {
              v = b->vpp++;
            } else {
              // todo;
              break;
            }
            v->car = NULL;
            v->cdr = *vp;
            *vp = v;
            v->tid = *tn;
            v->vid = 0;
            v->freq = 0;
            v->len = tn - t0;
          }
          v->freq++;
          if (v->vid) {
            vp = &v->car;
          } else {
            break;
          }
        }
      }
    }
    {
      grn_set *th = grn_set_open(sizeof(grn_id), sizeof(int), 0);
      if (!th) { return GRN_NO_MEMORY_AVAILABLE; }
      if (t0 == b->tvp) { GRN_LOG(ctx, GRN_LOG_DEBUG, "t0 == tvp"); }
      for (t0 = b->tvs; t0 < b->tvp; t0++) {
        grn_id vid, vid0 = *t0, vid1 = 0;
        grn_vgram_vnode *v, *v2 = NULL, **vp;
        if (grn_set_at(terms, t0, (void **) &u)) {
          vp = &(*u)->vnodes;
          for (tn = t0 + 1; tn < b->tvp; tn++) {
            for (v = *vp; v; v = v->cdr) {
              if (!v->vid && (v->freq < 2 || v->freq * v->len < 4)) {
                *vp = v->cdr;
                v->freq = 0;
              }
              if (v->tid == *tn) { break; }
              vp = &v->cdr;
            }
            if (v) {
              if (v->freq) {
                v2 = v;
                vid1 = vid0;
                vid0 = v->vid;
              }
              if (v->vid) {
                vp = &v->car;
                continue;
              }
            }
            break;
          }
        }
        if (v2) {
          if (!v2->vid) {
            vgram_key key;
            key.vid = vid1;
            key.tid = v2->tid;
            if (!(v2->vid = grn_sym_get(vgram->vgram, (char *)&key))) {
              grn_set_close(th);
              return GRN_NO_MEMORY_AVAILABLE;
            }
          }
          vid = *t0 = v2->vid * 2 + 1;
          memset(t0 + 1, 0, sizeof(grn_id) * v2->len);
          t0 += v2->len;
        } else {
          vid = *t0 *= 2;
        }
        {
          int *tf;
          if (!grn_set_get(th, &vid, (void **) &tf)) {
            grn_set_close(th);
            return GRN_NO_MEMORY_AVAILABLE;
          }
          (*tf)++;
        }
      }
      if (!th->n_entries) { GRN_LOG(ctx, GRN_LOG_DEBUG, "th->n_entries == 0"); }
      {
        int j = 0;
        int skip = 0;
        grn_set_eh *ehs, *ehp, *ehe;
        grn_set_sort_optarg arg;
        uint8_t *ps = GRN_MALLOC(b->len * 2), *pp, *pe;
        if (!ps) {
          grn_set_close(th);
          return GRN_NO_MEMORY_AVAILABLE;
        }
        pp = ps;
        pe = ps + b->len * 2;
        arg.mode = grn_sort_descending;
        arg.compar = NULL;
        arg.compar_arg = (void *)(intptr_t)sizeof(grn_id);
        ehs = grn_set_sort(th, 0, &arg);
        if (!ehs) {
          GRN_FREE(ps);
          grn_set_close(th);
          return GRN_NO_MEMORY_AVAILABLE;
        }
        GRN_B_ENC(th->n_entries, pp);
        for (ehp = ehs, ehe = ehs + th->n_entries; ehp < ehe; ehp++, j++) {
          int *id = (int *)GRN_SET_INTVAL(*ehp);
          GRN_B_ENC(*GRN_SET_INTKEY(*ehp), pp);
          *id = j;
        }
        for (t0 = b->tvs; t0 < b->tvp; t0++) {
          if (*t0) {
            int *id;
            if (!grn_set_at(th, t0, (void **) &id)) {
              GRN_LOG(ctx, GRN_LOG_ERROR, "lookup error (%d)", *t0);
            }
            GRN_B_ENC(*id, pp);
          } else {
            skip++;
          }
        }
        len_sum += b->len;
        img_sum += pp - ps;
        skip_sum += skip;
        GRN_FREE(ehs);
        GRN_FREE(ps);
      }
      grn_set_close(th);
    }
  }
  return GRN_SUCCESS;
}

grn_rc
grn_vgram_buf_close(grn_vgram_buf *b)
{
  if (!b) { return GRN_INVALID_ARGUMENT; }
  if (b->tvs) { GRN_FREE(b->tvs); }
  if (b->vps) { GRN_FREE(b->vps); }
  GRN_FREE(b);
  return GRN_SUCCESS;
}

grn_rc
grn_vgram_close(grn_vgram *vgram)
{
  if (!vgram) { return GRN_INVALID_ARGUMENT; }
  GRN_LOG(ctx, GRN_LOG_DEBUG, "len=%d img=%d skip=%d simple=%d", len_sum, img_sum, skip_sum, simple_sum);
  grn_sym_close(vgram->vgram);
  GRN_FREE(vgram);
  return GRN_SUCCESS;
}
*/
