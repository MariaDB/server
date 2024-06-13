/*
   Copyright (c) 2024, MariaDB plc

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA
*/

#include <my_global.h>
#include "vector_mhnsw.h"
#include "item_vectorfunc.h"
#include <scope.h>

// Algorithm parameters
static constexpr float alpha = 1.1f;
static constexpr uint ef_construction= 10;

// SIMD definitions
#define SIMD_word   (256/8)
#define SIMD_floats (SIMD_word/sizeof(float))
// how many extra bytes we need to alloc to be able to convert
// sizeof(double) aligned memory to SIMD_word aligned
#define SIMD_margin (SIMD_word - sizeof(double))

enum Graph_table_fields {
  FIELD_LAYER, FIELD_TREF, FIELD_VEC, FIELD_NEIGHBORS
};
enum Graph_table_indices {
  IDX_LAYER
};

class MHNSW_Context;

class FVector: public Sql_alloc
{
public:
  MHNSW_Context *ctx;
  FVector(MHNSW_Context *ctx_, const void *vec_);
  float *vec;
protected:
  FVector(MHNSW_Context *ctx_) : ctx(ctx_), vec(nullptr) {}
  void make_vec(const void *vec_);
};

class FVectorNode: public FVector
{
private:
  uchar *tref, *gref;
  size_t max_layer;

  static uchar *gref_max;

  int alloc_neighborhood(uint8_t layer);
public:
  List<FVectorNode> *neighbors= nullptr;

  FVectorNode(MHNSW_Context *ctx_, const void *gref_);
  FVectorNode(MHNSW_Context *ctx_, const void *tref_, uint8_t layer,
              const void *vec_);
  float distance_to(const FVector &other) const;
  int load();
  int load_from_record();
  int save();
  size_t get_tref_len() const;
  uchar *get_tref() const { return tref; }
  size_t get_gref_len() const;
  uchar *get_gref() const { return gref; }

  static uchar *get_key(const FVectorNode *elem, size_t *key_len, my_bool);
};

// this assumes that 1) rows from graph table are never deleted,
// 2) and thus a ref for a new row is larger than refs of existing rows,
// thus we can treat the not-yet-inserted row as having max possible ref.
// oh, yes, and 3) 8 bytes ought to be enough for everyone
uchar *FVectorNode::gref_max=(uchar*)"\xff\xff\xff\xff\xff\xff\xff\xff";

class MHNSW_Context
{
  public:
  MEM_ROOT root;
  TABLE *table;
  Field *vec_field;
  size_t vec_len= 0;
  size_t byte_len= 0;
  uint err= 0;

  Hash_set<FVectorNode> node_cache{PSI_INSTRUMENT_MEM, FVectorNode::get_key};

  MHNSW_Context(TABLE *table, Field *vec_field)
    : table(table), vec_field(vec_field)
  {
    init_alloc_root(PSI_INSTRUMENT_MEM, &root, 8192, 0, MYF(MY_THREAD_SPECIFIC));
  }

  ~MHNSW_Context()
  {
    free_root(&root, MYF(0));
  }

  FVectorNode *get_node(const void *gref);
  void set_lengths(size_t len)
  {
    byte_len= len;
    vec_len= MY_ALIGN(byte_len/sizeof(float), SIMD_floats);
  }
};

FVector::FVector(MHNSW_Context *ctx_, const void *vec_) : ctx(ctx_)
{
  make_vec(vec_);
}

void FVector::make_vec(const void *vec_)
{
  DBUG_ASSERT(ctx->vec_len);
  vec= (float*)alloc_root(&ctx->root,
                          ctx->vec_len * sizeof(float) + SIMD_margin);
  if (int off= ((intptr)vec) % SIMD_word)
    vec += (SIMD_word - off) / sizeof(float);
  memcpy(vec, vec_, ctx->byte_len);
  for (size_t i=ctx->byte_len/sizeof(float); i < ctx->vec_len; i++)
    vec[i]=0;
}

FVectorNode::FVectorNode(MHNSW_Context *ctx_, const void *gref_)
  : FVector(ctx_), tref(nullptr)
{
  gref= (uchar*)memdup_root(&ctx->root, gref_, get_gref_len());
}

FVectorNode::FVectorNode(MHNSW_Context *ctx_, const void *tref_, uint8_t layer,
                         const void *vec_)
  : FVector(ctx_, vec_), gref(gref_max)
{
  tref= (uchar*)memdup_root(&ctx->root, tref_, get_tref_len());
  alloc_neighborhood(layer);
}

float FVectorNode::distance_to(const FVector &other) const
{
  const_cast<FVectorNode*>(this)->load();
#if __GNUC__ > 7
  typedef float v8f __attribute__((vector_size(SIMD_word)));
  v8f *p1= (v8f*)vec;
  v8f *p2= (v8f*)other.vec;
  v8f d= {0,0,0,0,0,0,0,0};
  for (size_t i= 0; i < ctx->vec_len/SIMD_floats; p1++, p2++, i++)
  {
    v8f dist= *p1 - *p2;
    d+= dist * dist;
  }
  return d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7];
#else
  return euclidean_vec_distance(vec, other.vec, ctx->vec_len);
#endif
}

int FVectorNode::alloc_neighborhood(uint8_t layer)
{
  DBUG_ASSERT(!neighbors);
  max_layer= layer;
  neighbors= new (&ctx->root) List<FVectorNode>[layer+1];
  return 0;
}

int FVectorNode::load()
{
  DBUG_ASSERT(gref);
  if (tref)
    return 0;

  TABLE *graph= ctx->table->hlindex;
  if ((ctx->err= graph->file->ha_rnd_pos(graph->record[0], gref)))
    return ctx->err;
  return load_from_record();
}

int FVectorNode::load_from_record()
{
  TABLE *graph= ctx->table->hlindex;
  String buf, *v= graph->field[FIELD_TREF]->val_str(&buf);
  if (unlikely(!v || v->length() != get_tref_len()))
    return ctx->err= HA_ERR_CRASHED;
  tref= (uchar*)memdup_root(&ctx->root, v->ptr(), v->length());

  v= graph->field[FIELD_VEC]->val_str(&buf);
  if (unlikely(!v))
    return ctx->err= HA_ERR_CRASHED;

  DBUG_ASSERT(ctx->byte_len);
  if (v->length() != ctx->byte_len)
    return ctx->err= HA_ERR_CRASHED;
  make_vec(v->ptr());

  longlong layer= graph->field[FIELD_LAYER]->val_int();
  if (layer > 100) // 10e30 nodes at M=2, more at larger M's
    return ctx->err= HA_ERR_CRASHED;

  if (alloc_neighborhood(static_cast<uint8_t>(layer)))
    return ctx->err;

  v= graph->field[FIELD_NEIGHBORS]->val_str(&buf);
  if (unlikely(!v))
    return ctx->err= HA_ERR_CRASHED;

  // <N> <gref> <gref> ... <N> ...etc...
  uchar *ptr= (uchar*)v->ptr(), *end= ptr + v->length();
  for (size_t i=0; i <= max_layer; i++)
  {
    if (unlikely(ptr >= end))
      return ctx->err= HA_ERR_CRASHED;
    size_t grefs= *ptr++;
    if (unlikely(ptr + grefs * get_gref_len() > end))
      return ctx->err= HA_ERR_CRASHED;
    for (; grefs--; ptr+= get_gref_len())
      neighbors[i].push_back(ctx->get_node(ptr), &ctx->root);
  }
  return 0;
}

size_t FVectorNode::get_tref_len() const
{
  return ctx->table->file->ref_length;
}

size_t FVectorNode::get_gref_len() const
{
  return ctx->table->hlindex->file->ref_length;
}

uchar *FVectorNode::get_key(const FVectorNode *elem, size_t *key_len, my_bool)
{
  *key_len= elem->get_gref_len();
  return elem->gref;
}

FVectorNode *MHNSW_Context::get_node(const void *gref)
{
  FVectorNode *node= node_cache.find(gref, table->hlindex->file->ref_length);
  if (!node)
  {
    node= new (&root) FVectorNode(this, gref);
    node_cache.insert(node);
  }
  return node;
}

static int cmp_vec(const FVector *target, const FVectorNode *a, const FVectorNode *b)
{
  float a_dist= a->distance_to(*target);
  float b_dist= b->distance_to(*target);

  if (a_dist < b_dist)
    return -1;
  if (a_dist > b_dist)
    return 1;
  return 0;
}

static int select_neighbors(MHNSW_Context *ctx, size_t layer,
                            FVectorNode &target,
                            const List<FVectorNode> &candidates_unsafe,
                            size_t max_neighbor_connections)
{
  Hash_set<FVectorNode> visited(PSI_INSTRUMENT_MEM, FVectorNode::get_key);
  Queue<FVectorNode, const FVector> pq; // working queue
  Queue<FVectorNode, const FVector> pq_discard; // queue for discarded candidates
  /*
    make a copy of candidates in case it's target.neighbors[layer].
    because we're going to modify the latter below
  */
  List<FVectorNode> candidates= candidates_unsafe;
  List<FVectorNode> &neighbors= target.neighbors[layer];

  neighbors.empty();

  if (pq.init(10000, 0, cmp_vec, &target) ||
      pq_discard.init(10000, 0, cmp_vec, &target))
    return ctx->err= HA_ERR_OUT_OF_MEM;

  for (const FVectorNode &candidate : candidates)
  {
    visited.insert(&candidate);
    pq.push(&candidate);
  }

  DBUG_ASSERT(pq.elements());
  neighbors.push_back(pq.pop(), &ctx->root);

  while (pq.elements() && neighbors.elements < max_neighbor_connections)
  {
    const FVectorNode *vec= pq.pop();
    const float target_dist= vec->distance_to(target);
    const float target_dista= target_dist / alpha;
    bool discard= false;
    for (const FVectorNode &neigh : neighbors)
    {
      if ((discard= vec->distance_to(neigh) < target_dista))
        break;
    }
    if (!discard)
      neighbors.push_back(vec, &ctx->root);
    else if (pq_discard.elements() + neighbors.elements < max_neighbor_connections)
      pq_discard.push(vec);
  }

  while (pq_discard.elements() && neighbors.elements < max_neighbor_connections)
  {
    const FVectorNode *vec= pq_discard.pop();
    neighbors.push_back(vec, &ctx->root);
  }

  return 0;
}


int FVectorNode::save()
{
  TABLE *graph= ctx->table->hlindex;

  DBUG_ASSERT(tref);
  DBUG_ASSERT(vec);
  DBUG_ASSERT(neighbors);

  graph->field[FIELD_LAYER]->store(max_layer, false);
  graph->field[FIELD_TREF]->set_notnull();
  graph->field[FIELD_TREF]->store_binary(tref, get_tref_len());
  graph->field[FIELD_VEC]->store_binary((uchar*)vec, ctx->byte_len);

  size_t total_size= 0;
  for (size_t i=0; i <= max_layer; i++)
    total_size+= 1 + get_gref_len() * neighbors[i].elements;

  uchar *neighbor_blob= static_cast<uchar *>(my_safe_alloca(total_size));
  uchar *ptr= neighbor_blob;
  for (size_t i= 0; i <= max_layer; i++)
  {
    *ptr++= (uchar)(neighbors[i].elements);
    for (const auto &neigh: neighbors[i])
    {
      memcpy(ptr, neigh.get_gref(), get_gref_len());
      ptr+= neigh.get_gref_len();
    }
  }
  graph->field[FIELD_NEIGHBORS]->store_binary(neighbor_blob, total_size);

  if (gref != gref_max)
  {
    ctx->err= graph->file->ha_rnd_pos(graph->record[1], gref);
    if (!ctx->err)
    {
      ctx->err= graph->file->ha_update_row(graph->record[1], graph->record[0]);
      if (ctx->err == HA_ERR_RECORD_IS_THE_SAME)
        ctx->err= 0;
    }
  }
  else
  {
    ctx->err= graph->file->ha_write_row(graph->record[0]);
    graph->file->position(graph->record[0]);
    gref= (uchar*)memdup_root(&ctx->root, graph->file->ref, get_gref_len());
  }

  my_safe_afree(neighbor_blob, total_size);
  return ctx->err;
}


static int update_second_degree_neighbors(MHNSW_Context *ctx, size_t layer,
                                          uint max_neighbors,
                                          const FVectorNode &node)
{
  for (FVectorNode &neigh: node.neighbors[layer])
  {
    List<FVectorNode> &neighneighbors= neigh.neighbors[layer];
    neighneighbors.push_back(&node, &ctx->root);
    if (neighneighbors.elements > max_neighbors)
    {
      if (select_neighbors(ctx, layer, neigh, neighneighbors, max_neighbors))
        return ctx->err;
    }
    if (neigh.save())
      return ctx->err;
  }
  return 0;
}


static int search_layer(MHNSW_Context *ctx, const FVector &target,
                        const List<FVectorNode> &start_nodes,
                        uint max_candidates_return, size_t layer,
                        List<FVectorNode> *result)
{
  DBUG_ASSERT(start_nodes.elements > 0);
  DBUG_ASSERT(result->elements == 0);

  Queue<FVectorNode, const FVector> candidates;
  Queue<FVectorNode, const FVector> best;
  Hash_set<FVectorNode> visited(PSI_INSTRUMENT_MEM, FVectorNode::get_key);

  candidates.init(10000, false, cmp_vec, &target);
  best.init(max_candidates_return, true, cmp_vec, &target);

  for (const FVectorNode &node : start_nodes)
  {
    candidates.push(&node);
    if (best.elements() < max_candidates_return)
      best.push(&node);
    else if (node.distance_to(target) > best.top()->distance_to(target))
      best.replace_top(&node);
    visited.insert(&node);
  }

  float furthest_best= best.top()->distance_to(target);
  while (candidates.elements())
  {
    const FVectorNode &cur_vec= *candidates.pop();
    float cur_distance= cur_vec.distance_to(target);
    if (cur_distance > furthest_best && best.elements() == max_candidates_return)
    {
      break; // All possible candidates are worse than what we have.
             // Can't get better.
    }

    for (const FVectorNode &neigh: cur_vec.neighbors[layer])
    {
      if (visited.find(&neigh))
        continue;

      visited.insert(&neigh);
      if (best.elements() < max_candidates_return)
      {
        candidates.push(&neigh);
        best.push(&neigh);
        furthest_best= best.top()->distance_to(target);
      }
      else if (neigh.distance_to(target) < furthest_best)
      {
        best.replace_top(&neigh);
        candidates.push(&neigh);
        furthest_best= best.top()->distance_to(target);
      }
    }
  }

  while (best.elements())
    result->push_front(best.pop(), &ctx->root);

  return 0;
}


static int bad_value_on_insert(Field *f)
{
  my_error(ER_TRUNCATED_WRONG_VALUE_FOR_FIELD, MYF(0), "vector", "...",
           f->table->s->db.str, f->table->s->table_name.str, f->field_name.str,
           f->table->in_use->get_stmt_da()->current_row_for_warning());
  return HA_ERR_GENERIC;
}


int mhnsw_insert(TABLE *table, KEY *keyinfo)
{
  THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  Field *vec_field= keyinfo->key_part->field;
  String buf, *res= vec_field->val_str(&buf);
  MHNSW_Context ctx(table, vec_field);

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_VECTOR);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(vec_field->binary());
  DBUG_ASSERT(vec_field->cmp_type() == STRING_RESULT);
  DBUG_ASSERT(res); // ER_INDEX_CANNOT_HAVE_NULL
  DBUG_ASSERT(table->file->ref_length <= graph->field[FIELD_TREF]->field_length);

  // XXX returning an error here will rollback the insert in InnoDB
  // but in MyISAM the row will stay inserted, making the index out of sync:
  // invalid vector values are present in the table but cannot be found
  // via an index. The easiest way to fix it is with a VECTOR(N) type
  if (res->length() == 0 || res->length() % 4)
    return bad_value_on_insert(vec_field);

  const double NORMALIZATION_FACTOR= 1 / std::log(thd->variables.mhnsw_max_edges_per_node);

  table->file->position(table->record[0]);

  if (int err= graph->file->ha_index_init(IDX_LAYER, 1))
    return err;

  ctx.err= graph->file->ha_index_last(graph->record[0]);
  graph->file->ha_index_end();

  if (ctx.err)
  {
    if (ctx.err != HA_ERR_END_OF_FILE)
      return ctx.err;
    ctx.err= 0;

    // First insert!
    ctx.set_lengths(res->length());
    FVectorNode target(&ctx, table->file->ref, 0, res->ptr());
    return target.save();
  }

  longlong max_layer= graph->field[FIELD_LAYER]->val_int();

  List<FVectorNode> candidates;
  List<FVectorNode> start_nodes;

  graph->file->position(graph->record[0]);
  FVectorNode *start_node= ctx.get_node(graph->file->ref);

  if (start_nodes.push_back(start_node, &ctx.root))
    return HA_ERR_OUT_OF_MEM;

  ctx.set_lengths(graph->field[FIELD_VEC]->value_length());
  if (int err= start_node->load_from_record())
    return err;

  if (ctx.byte_len != res->length())
    return bad_value_on_insert(vec_field);

  if (int err= graph->file->ha_rnd_init(0))
    return err;

  SCOPE_EXIT([graph](){ graph->file->ha_rnd_end(); });

  double new_num= my_rnd(&thd->rand);
  double log= -std::log(new_num) * NORMALIZATION_FACTOR;
  longlong new_node_layer= std::min<longlong>(std::floor(log), max_layer + 1);
  longlong cur_layer;

  FVectorNode target(&ctx, table->file->ref, new_node_layer, res->ptr());

  for (cur_layer= max_layer; cur_layer > new_node_layer; cur_layer--)
  {
    if (search_layer(&ctx, target, start_nodes, 1, cur_layer, &candidates))
      return ctx.err;
    start_nodes= candidates;
    candidates.empty();
  }

  for (; cur_layer >= 0; cur_layer--)
  {
    uint max_neighbors= (cur_layer == 0)   // heuristics from the paper
     ? thd->variables.mhnsw_max_edges_per_node * 2
     : thd->variables.mhnsw_max_edges_per_node;
    if (search_layer(&ctx, target, start_nodes, ef_construction, cur_layer,
                     &candidates))
      return ctx.err;

    if (select_neighbors(&ctx, cur_layer, target, candidates, max_neighbors))
      return ctx.err;
    start_nodes= candidates;
    candidates.empty();
  }

  if (target.save())
    return ctx.err;

  for (longlong cur_layer= new_node_layer; cur_layer >= 0; cur_layer--)
  {
    uint max_neighbors= (cur_layer == 0)   // heuristics from the paper
     ? thd->variables.mhnsw_max_edges_per_node * 2
     : thd->variables.mhnsw_max_edges_per_node;
    // XXX do only one ha_update_row() per node
    if (update_second_degree_neighbors(&ctx, cur_layer, max_neighbors, target))
      return ctx.err;
  }

  dbug_tmp_restore_column_map(&table->read_set, old_map);

  return 0;
}


int mhnsw_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit)
{
  THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  Field *vec_field= keyinfo->key_part->field;
  Item_func_vec_distance *fun= (Item_func_vec_distance *)dist;
  String buf, *res= fun->get_const_arg()->val_str(&buf);
  handler *h= table->file;
  MHNSW_Context ctx(table, vec_field);

  if (int err= h->ha_rnd_init(0))
    return err;

  if (int err= graph->file->ha_index_init(0, 1))
    return err;
  ctx.err= graph->file->ha_index_last(graph->record[0]);
  graph->file->ha_index_end();

  if (ctx.err)
    return ctx.err;

  longlong max_layer= graph->field[FIELD_LAYER]->val_int();

  List<FVectorNode> candidates;
  List<FVectorNode> start_nodes;

  graph->file->position(graph->record[0]);
  FVectorNode *start_node= ctx.get_node(graph->file->ref);

  // one could put all max_layer nodes in start_nodes
  // but it has no effect of the recall or speed
  if (start_nodes.push_back(start_node, &ctx.root))
    return HA_ERR_OUT_OF_MEM;

  ctx.set_lengths(graph->field[FIELD_VEC]->value_length());
  if (int err= start_node->load_from_record())
    return err;

  /*
    if the query vector is NULL or invalid, VEC_DISTANCE will return
    NULL, so the result is basically unsorted, we can return rows
    in any order. For simplicity let's sort by the start_node.
  */
  if (!res || ctx.byte_len != res->length())
    (res= &buf)->set((char*)start_node->vec, ctx.byte_len, &my_charset_bin);

  if (int err= graph->file->ha_rnd_init(0))
    return err;

  SCOPE_EXIT([graph](){ graph->file->ha_rnd_end(); });

  FVector target(&ctx, res->ptr());

  uint ef_search= thd->variables.mhnsw_min_limit;

  for (size_t cur_layer= max_layer; cur_layer > 0; cur_layer--)
  {
    if (search_layer(&ctx, target, start_nodes, 1, cur_layer, &candidates))
      return ctx.err;
    start_nodes= candidates;
    candidates.empty();
  }

  if (search_layer(&ctx, target, start_nodes, ef_search, 0, &candidates))
    return ctx.err;

  size_t context_size=limit * h->ref_length + sizeof(ulonglong);
  char *context= thd->alloc(context_size);
  graph->context= context;

  *(ulonglong*)context= limit;
  context+= context_size;

  while (limit--)
  {
    context-= h->ref_length;
    memcpy(context, candidates.pop()->get_tref(), h->ref_length);
  }
  DBUG_ASSERT(context - sizeof(ulonglong) == graph->context);

  return mhnsw_next(table);
}

int mhnsw_next(TABLE *table)
{
  uchar *ref= (uchar*)(table->hlindex->context);
  if (ulonglong *limit= (ulonglong*)ref)
  {
    ref+= sizeof(ulonglong) + (--*limit) * table->file->ref_length;
    return table->file->ha_rnd_pos(table->record[0], ref);
  }
  return HA_ERR_END_OF_FILE;
}

const LEX_CSTRING mhnsw_hlindex_table_def(THD *thd, uint ref_length)
{
  const char templ[]="CREATE TABLE i (                   "
                     "  layer tinyint not null,          "
                     "  tref varbinary(%u),              "
                     "  vec blob not null,               "
                     "  neighbors blob not null,         "
                     "  key (layer))                     ";
  size_t len= sizeof(templ) + 32;
  char *s= thd->alloc(len);
  len= my_snprintf(s, len, templ, ref_length);
  return {s, len};
}
