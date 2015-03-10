/* -*- c-basic-offset: 2 -*- */
/* Copyright(C) 2009-2012 Brazil

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
#ifndef GRN_II_H
#define GRN_II_H

/* "ii" is for inverted index */

#include "grn.h"
#include "grn_hash.h"
#include "grn_io.h"
#include "grn_store.h"

#ifdef  __cplusplus
extern "C" {
#endif

struct _grn_ii {
  grn_db_obj obj;
  grn_io *seg;
  grn_io *chunk;
  grn_obj *lexicon;
  grn_obj_flags lflags;
  grn_encoding encoding;
  uint32_t n_elements;
  struct grn_ii_header *header;
};

#define GRN_II_BGQSIZE 16
#define GRN_II_MAX_LSEG           0x10000
#define GRN_II_W_TOTAL_CHUNK      40
#define GRN_II_W_CHUNK            22
#define GRN_II_W_LEAST_CHUNK      (GRN_II_W_TOTAL_CHUNK - 32)
#define GRN_II_MAX_CHUNK          (1 << (GRN_II_W_TOTAL_CHUNK - GRN_II_W_CHUNK))
#define GRN_II_N_CHUNK_VARIATION  (GRN_II_W_CHUNK - GRN_II_W_LEAST_CHUNK)

struct grn_ii_header {
  uint64_t total_chunk_size;
  uint64_t bmax;
  uint32_t flags;
  uint32_t amax;
  uint32_t smax;
  uint32_t param1;
  uint32_t param2;
  uint32_t pnext;
  uint32_t bgqhead;
  uint32_t bgqtail;
  uint32_t bgqbody[GRN_II_BGQSIZE];
  uint32_t reserved[288];
  uint32_t ainfo[GRN_II_MAX_LSEG];
  uint32_t binfo[GRN_II_MAX_LSEG];
  uint32_t free_chunks[GRN_II_N_CHUNK_VARIATION + 1];
  uint32_t garbages[GRN_II_N_CHUNK_VARIATION + 1];
  uint32_t ngarbages[GRN_II_N_CHUNK_VARIATION + 1];
  uint8_t chunks[GRN_II_MAX_CHUNK >> 3];
};

struct _grn_ii_pos {
  struct _grn_ii_pos *next;
  uint32_t pos;
};

struct _grn_ii_updspec {
  uint32_t rid;
  uint32_t sid;
  int32_t weight;
  int32_t tf;                 /* number of postings successfully stored to index */
  int32_t atf;                /* actual number of postings */
  int32_t offset;
  struct _grn_ii_pos *pos;
  struct _grn_ii_pos *tail;
  /* grn_vgram_vnode *vnodes; */
};

typedef struct _grn_ii_updspec grn_ii_updspec;

GRN_API grn_ii *grn_ii_create(grn_ctx *ctx, const char *path, grn_obj *lexicon,
                              uint32_t flags);
GRN_API grn_ii *grn_ii_open(grn_ctx *ctx, const char *path, grn_obj *lexicon);
GRN_API grn_rc grn_ii_close(grn_ctx *ctx, grn_ii *ii);
GRN_API grn_rc grn_ii_remove(grn_ctx *ctx, const char *path);
grn_rc grn_ii_info(grn_ctx *ctx, grn_ii *ii, uint64_t *seg_size, uint64_t *chunk_size);
grn_rc grn_ii_update_one(grn_ctx *ctx, grn_ii *ii, uint32_t key, grn_ii_updspec *u,
                         grn_hash *h);
grn_rc grn_ii_delete_one(grn_ctx *ctx, grn_ii *ii, uint32_t key, grn_ii_updspec *u,
                         grn_hash *h);
grn_ii_updspec *grn_ii_updspec_open(grn_ctx *ctx, uint32_t rid, uint32_t sid);
grn_rc grn_ii_updspec_close(grn_ctx *ctx, grn_ii_updspec *u);
grn_rc grn_ii_updspec_add(grn_ctx *ctx, grn_ii_updspec *u, int pos, int32_t weight);
int grn_ii_updspec_cmp(grn_ii_updspec *a, grn_ii_updspec *b);

void grn_ii_expire(grn_ctx *ctx, grn_ii *ii);

typedef struct {
  grn_id rid;
  uint32_t sid;
  uint32_t pos;
  uint32_t tf;
  uint32_t weight;
  uint32_t rest;
} grn_ii_posting;

typedef struct _grn_ii_cursor grn_ii_cursor;

GRN_API grn_rc grn_ii_posting_add(grn_ctx *ctx, grn_ii_posting *pos,
                                  grn_hash *s, grn_operator op);

GRN_API grn_ii_cursor *grn_ii_cursor_open(grn_ctx *ctx, grn_ii *ii, grn_id tid,
                                          grn_id min, grn_id max, int nelements, int flags);
grn_ii_cursor *grn_ii_cursor_openv1(grn_ii *ii, uint32_t key);
grn_rc grn_ii_cursor_openv2(grn_ii_cursor **cursors, int ncursors);
GRN_API grn_ii_posting *grn_ii_cursor_next(grn_ctx *ctx, grn_ii_cursor *c);
grn_ii_posting *grn_ii_cursor_next_pos(grn_ctx *ctx, grn_ii_cursor *c);
GRN_API grn_rc grn_ii_cursor_close(grn_ctx *ctx, grn_ii_cursor *c);

uint32_t grn_ii_max_section(grn_ii *ii);

const char *grn_ii_path(grn_ii *ii);
grn_obj *grn_ii_lexicon(grn_ii *ii);

/*
grn_rc grn_ii_upd(grn_ctx *ctx, grn_ii *ii, grn_id rid, grn_vgram *vgram,
                   const char *oldvalue, unsigned int oldvalue_len,
                   const char *newvalue, unsigned int newvalue_len);
grn_rc grn_ii_update(grn_ctx *ctx, grn_ii *ii, grn_id rid, grn_vgram *vgram,
                      unsigned int section,
                      grn_values *oldvalues, grn_values *newvalues);
*/

typedef struct _grn_select_optarg grn_select_optarg;

struct _grn_select_optarg {
  grn_operator mode;
  int similarity_threshold;
  int max_interval;
  int *weight_vector;
  int vector_size;
  int (*func)(grn_ctx *, grn_hash *, const void *, int, void *);
  void *func_arg;
  int max_size;
  grn_obj *scorer;
};

GRN_API grn_rc grn_ii_column_update(grn_ctx *ctx, grn_ii *ii, grn_id id,
                                    unsigned int section, grn_obj *oldvalue,
                                    grn_obj *newvalue, grn_obj *posting);
grn_rc grn_ii_term_extract(grn_ctx *ctx, grn_ii *ii, const char *string,
                            unsigned int string_len, grn_hash *s,
                            grn_operator op, grn_select_optarg *optarg);
grn_rc grn_ii_similar_search(grn_ctx *ctx, grn_ii *ii, const char *string, unsigned int string_len,
                              grn_hash *s, grn_operator op, grn_select_optarg *optarg);
GRN_API grn_rc grn_ii_select(grn_ctx *ctx, grn_ii *ii, const char *string, unsigned int string_len,
                             grn_hash *s, grn_operator op, grn_select_optarg *optarg);
grn_rc grn_ii_sel(grn_ctx *ctx, grn_ii *ii, const char *string, unsigned int string_len,
                  grn_hash *s, grn_operator op, grn_search_optarg *optarg);

void grn_ii_resolve_sel_and(grn_ctx *ctx, grn_hash *s, grn_operator op);

grn_rc grn_ii_at(grn_ctx *ctx, grn_ii *ii, grn_id id, grn_hash *s, grn_operator op);

void grn_ii_inspect_values(grn_ctx *ctx, grn_ii *ii, grn_obj *buf);
void grn_ii_cursor_inspect(grn_ctx *ctx, grn_ii_cursor *c, grn_obj *buf);

grn_rc grn_ii_build(grn_ctx *ctx, grn_ii *ii, uint64_t sparsity);

#ifdef __cplusplus
}
#endif

#endif /* GRN_II_H */
