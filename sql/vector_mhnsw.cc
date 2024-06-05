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

#include "field.h"
#include "hash.h"
#include "item.h"
#include "item_vectorfunc.h"
#include "key.h"
#include "my_base.h"
#include "mysql/psi/psi_base.h"
#include "sql_queue.h"
#include <scope.h>

#define HNSW_MAX_M 10000 // practically the number of neighbors should be ~100
#define HNSW_MAX_M_WIDTH 2
#define HNSW_MAX_M_store int2store
#define HNSW_MAX_M_read  uint2korr

const LEX_CSTRING mhnsw_hlindex_table={STRING_WITH_LEN("\
  CREATE TABLE i (                                      \
    layer int not null,                                 \
    src varbinary(255) not null,                        \
    neighbors blob not null,                            \
    index (layer, src))                                 \
")};


class MHNSW_Context;

class FVector: public Sql_alloc
{
public:
  MHNSW_Context *ctx;
  FVector(MHNSW_Context *ctx_, const void *vec_);
  float *vec;
protected:
  FVector(MHNSW_Context *ctx_) : ctx(ctx_), vec(nullptr) {}
};

class FVectorNode: public FVector
{
private:
  uchar *ref;
public:
  FVectorNode(MHNSW_Context *ctx_, const void *ref_);
  FVectorNode(MHNSW_Context *ctx_, const void *ref_, const void *vec_);
  float distance_to(const FVector &other) const;
  int instantiate_vector();
  size_t get_ref_len() const;
  uchar *get_ref() const { return ref; }
  bool is_new() const;

  static uchar *get_key(const FVectorNode *elem, size_t *key_len, my_bool);
};

class MHNSW_Context
{
  public:
  MEM_ROOT root;
  TABLE *table;
  Field *vec_field;
  size_t vec_len= 0;
  FVector *target= 0;

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

  FVectorNode *get_node(const void *ref_);
};

FVector::FVector(MHNSW_Context *ctx_, const void *vec_) : ctx(ctx_)
{
  vec= (float*)memdup_root(&ctx->root, vec_, ctx->vec_len * sizeof(float));
}

FVectorNode::FVectorNode(MHNSW_Context *ctx_, const void *ref_)
  : FVector(ctx_)
{
  ref= (uchar*)memdup_root(&ctx->root, ref_, get_ref_len());
}

FVectorNode::FVectorNode(MHNSW_Context *ctx_, const void *ref_, const void *vec_)
  : FVector(ctx_, vec_)
{
  ref= (uchar*)memdup_root(&ctx->root, ref_, get_ref_len());
}

float FVectorNode::distance_to(const FVector &other) const
{
  if (!vec)
    const_cast<FVectorNode*>(this)->instantiate_vector();
  return euclidean_vec_distance(vec, other.vec, ctx->vec_len);
}

int FVectorNode::instantiate_vector()
{
  DBUG_ASSERT(vec == nullptr);
  if (int err= ctx->table->file->ha_rnd_pos(ctx->table->record[0], ref))
    return err;
  String buf, *v= ctx->vec_field->val_str(&buf);
  ctx->vec_len= v->length() / sizeof(float);
  vec= (float*)memdup_root(&ctx->root, v->ptr(), v->length());
  return 0;
}

size_t FVectorNode::get_ref_len() const
{
  return ctx->table->file->ref_length;
}

bool FVectorNode::is_new() const
{
  return this == ctx->target;
}

uchar *FVectorNode::get_key(const FVectorNode *elem, size_t *key_len, my_bool)
{
  *key_len= elem->get_ref_len();
  return elem->ref;
}

FVectorNode *MHNSW_Context::get_node(const void *ref)
{
  FVectorNode *node= node_cache.find(ref, table->file->ref_length);
  if (!node)
  {
    node= new (&root) FVectorNode(this, ref);
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

const bool KEEP_PRUNED_CONNECTIONS=true; // XXX why?
const bool EXTEND_CANDIDATES=true; // XXX or false?

static int get_neighbors(MHNSW_Context *ctx, size_t layer_number,
                         const FVectorNode &source_node,
                         List<FVectorNode> *neighbors)
{
  TABLE *graph= ctx->table->hlindex;
  uchar *key= static_cast<uchar*>(alloca(graph->key_info->key_length));

  graph->field[0]->store(layer_number, false);
  graph->field[1]->store_binary(source_node.get_ref(), source_node.get_ref_len());
  key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);
  if (int err= graph->file->ha_index_read_map(graph->record[0], key,
                                              HA_WHOLE_KEY, HA_READ_KEY_EXACT))
    return err;

  String strbuf, *str= graph->field[2]->val_str(&strbuf);

  // mhnsw_insert() guarantees that all ref have the same length
  uint ref_length= source_node.get_ref_len();

  const char *neigh_arr_bytes= str->ptr();
  uint number_of_neighbors= HNSW_MAX_M_read(neigh_arr_bytes);
  if (number_of_neighbors * ref_length + HNSW_MAX_M_WIDTH != str->length())
    return HA_ERR_CRASHED; // should not happen, corrupted HNSW index

  const char *pos= neigh_arr_bytes + HNSW_MAX_M_WIDTH;
  for (uint i= 0; i < number_of_neighbors; i++)
  {
    FVectorNode *neigh= ctx->get_node(pos);
    neighbors->push_back(neigh, &ctx->root);
    pos+= ref_length;
  }

  return 0;
}


static int select_neighbors(MHNSW_Context *ctx,
                            size_t layer_number, const FVector &target,
                            const List<FVectorNode> &candidates,
                            size_t max_neighbor_connections,
                            List<FVectorNode> *neighbors)
{
  /*
    TODO: If the input neighbors list is already sorted in search_layer, then
    no need to do additional queue build steps here.
   */

  Hash_set<FVectorNode> visited(PSI_INSTRUMENT_MEM, FVectorNode::get_key);

  Queue<FVectorNode, const FVector> pq; // working queue
  Queue<FVectorNode, const FVector> pq_discard; // queue for discarded candidates
  Queue<FVectorNode, const FVector> best; // neighbors to return

  // TODO(cvicentiu) this 1000 here is a hardcoded value for max queue size.
  // This should not be fixed.
  if (pq.init(10000, 0, cmp_vec, &target) ||
      pq_discard.init(10000, 0, cmp_vec, &target) ||
      best.init(max_neighbor_connections, true, cmp_vec, &target))
    return HA_ERR_OUT_OF_MEM;

  for (const FVectorNode &candidate : candidates)
  {
    visited.insert(&candidate);
    pq.push(&candidate);
  }

  if (EXTEND_CANDIDATES)
  {
    for (const FVectorNode &candidate : candidates)
    {
      List<FVectorNode> candidate_neighbors;
      if (int err= get_neighbors(ctx, layer_number, candidate,
                                 &candidate_neighbors))
        return err;
      for (const FVectorNode &extra_candidate : candidate_neighbors)
      {
        if (visited.find(&extra_candidate))
          continue;
        visited.insert(&extra_candidate);
        pq.push(&extra_candidate);
      }
    }
  }

  DBUG_ASSERT(pq.elements());
  best.push(pq.pop());

  float best_top= best.top()->distance_to(target);
  while (pq.elements() && best.elements() < max_neighbor_connections)
  {
    const FVectorNode *vec= pq.pop();
    const float cur_dist= vec->distance_to(target);
    if (cur_dist < best_top)
    {
      DBUG_ASSERT(0); // impossible. XXX redo the loop
      best.push(vec);
      best_top= cur_dist;
    }
    else
      pq_discard.push(vec);
  }

  if (KEEP_PRUNED_CONNECTIONS)
  {
    while (pq_discard.elements() &&
           best.elements() < max_neighbor_connections)
    {
      best.push(pq_discard.pop());
    }
  }

  DBUG_ASSERT(best.elements() <= max_neighbor_connections);
  while (best.elements()) // XXX why not to return best directly?
    neighbors->push_front(best.pop(), &ctx->root);

  return 0;
}


static void dbug_print_vec_ref(const char *prefix, uint layer,
                               const FVectorNode &ref)
{
#ifndef DBUG_OFF
  // TODO(cvicentiu) disable this in release build.
  char *ref_str= static_cast<char *>(alloca(ref.get_ref_len() * 2 + 1));
  DBUG_ASSERT(ref_str);
  char *ptr= ref_str;
  for (size_t i= 0; i < ref.get_ref_len(); ptr += 2, i++)
  {
    snprintf(ptr, 3, "%02x", ref.get_ref()[i]);
  }
  DBUG_PRINT("VECTOR", ("%s %u %s", prefix, layer, ref_str));
#endif
}

static void dbug_print_vec_neigh(uint layer, const List<FVectorNode> &neighbors)
{
#ifndef DBUG_OFF
  DBUG_PRINT("VECTOR", ("NEIGH: NUM: %d", neighbors.elements));
  for (const FVectorNode& ref : neighbors)
  {
    dbug_print_vec_ref("NEIGH: ", layer, ref);
  }
#endif
}

static void dbug_print_hash_vec(Hash_set<FVectorNode> &h)
{
#ifndef DBUG_OFF
  for (FVectorNode &ptr : h)
  {
    DBUG_PRINT("VECTOR", ("HASH elem: %p", &ptr));
    dbug_print_vec_ref("VISITED: ", 0, ptr);
  }
#endif
}


static int write_neighbors(MHNSW_Context *ctx, size_t layer_number,
                            const FVectorNode &source_node,
                            const List<FVectorNode> &new_neighbors)
{
  int err;
  TABLE *graph= ctx->table->hlindex;
  DBUG_ASSERT(new_neighbors.elements <= HNSW_MAX_M);

  size_t total_size= HNSW_MAX_M_WIDTH + new_neighbors.elements * source_node.get_ref_len();

  // Allocate memory for the struct and the flexible array member
  char *neighbor_array_bytes= static_cast<char *>(my_safe_alloca(total_size));

  // XXX why bother storing it?
  HNSW_MAX_M_store(neighbor_array_bytes, new_neighbors.elements);
  char *pos= neighbor_array_bytes + HNSW_MAX_M_WIDTH;
  for (const auto &node: new_neighbors)
  {
    DBUG_ASSERT(node.get_ref_len() == source_node.get_ref_len());
    memcpy(pos, node.get_ref(), node.get_ref_len());
    pos+= node.get_ref_len();
  }

  graph->field[0]->store(layer_number, false);
  graph->field[1]->store_binary(source_node.get_ref(), source_node.get_ref_len());
  graph->field[2]->store_binary(neighbor_array_bytes, total_size);

  if (source_node.is_new())
  {
    dbug_print_vec_ref("INSERT ", layer_number, source_node);
    err= graph->file->ha_write_row(graph->record[0]);
  }
  else
  {
    dbug_print_vec_ref("UPDATE ", layer_number, source_node);
    dbug_print_vec_neigh(layer_number, new_neighbors);

    uchar *key= static_cast<uchar*>(alloca(graph->key_info->key_length));
    key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);

    err= graph->file->ha_index_read_map(graph->record[1], key,
                                            HA_WHOLE_KEY, HA_READ_KEY_EXACT);
    if (!err)
      err= graph->file->ha_update_row(graph->record[1], graph->record[0]);

  }
  my_safe_afree(neighbor_array_bytes, total_size);
  return err;
}


static int update_second_degree_neighbors(MHNSW_Context *ctx,
                                          size_t layer_number,
                                          uint max_neighbors,
                                          const FVectorNode &source_node,
                                          const List<FVectorNode> &neighbors)
{
  //dbug_print_vec_ref("Updating second degree neighbors", layer_number, source_node);
  //dbug_print_vec_neigh(layer_number, neighbors);
  for (const FVectorNode &neigh: neighbors) // XXX why this loop?
  {
    List<FVectorNode> new_neighbors;
    if (int err= get_neighbors(ctx, layer_number, neigh, &new_neighbors))
      return err;
    new_neighbors.push_back(&source_node, &ctx->root);
    if (int err= write_neighbors(ctx, layer_number, neigh, new_neighbors))
      return err;
  }

  for (const FVectorNode &neigh: neighbors)
  {
    List<FVectorNode> new_neighbors;
    if (int err= get_neighbors(ctx, layer_number, neigh, &new_neighbors))
      return err;

    if (new_neighbors.elements > max_neighbors)
    {
      // shrink the neighbors
      List<FVectorNode> selected;
      if (int err= select_neighbors(ctx, layer_number, neigh,
                                    new_neighbors, max_neighbors, &selected))
        return err;
      if (int err= write_neighbors(ctx, layer_number, neigh, selected))
        return err;
    }
  }

  return 0;
}


static int update_neighbors(MHNSW_Context *ctx,
                            size_t layer_number, uint max_neighbors,
                            const FVectorNode &source_node,
                            const List<FVectorNode> &neighbors)
{
  // 1. update node's neighbors
  if (int err= write_neighbors(ctx, layer_number, source_node, neighbors))
    return err;
  // 2. update node's neighbors' neighbors (shrink before update)
  return update_second_degree_neighbors(ctx, layer_number,
                                        max_neighbors, source_node, neighbors);
}


static int search_layer(MHNSW_Context *ctx,
                        const List<FVectorNode> &start_nodes,
                        uint max_candidates_return, size_t layer,
                        List<FVectorNode> *result)
{
  DBUG_ASSERT(start_nodes.elements > 0);
  DBUG_ASSERT(result->elements == 0);

  Queue<FVectorNode, const FVector> candidates;
  Queue<FVectorNode, const FVector> best;
  Hash_set<FVectorNode> visited(PSI_INSTRUMENT_MEM, FVectorNode::get_key);
  const FVector &target= *ctx->target;

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
    dbug_print_vec_ref("INSERTING node in visited: ", layer, node);
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

    List<FVectorNode> neighbors;
    get_neighbors(ctx, layer, cur_vec, &neighbors);

    for (const FVectorNode &neigh: neighbors)
    {
      dbug_print_hash_vec(visited);
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
    neighbors.empty();
  }
  DBUG_PRINT("VECTOR", ("SEARCH_LAYER_END %d best", best.elements()));

  while (best.elements())
  {
    // TODO(cvicentiu) this is n*log(n), we need a queue iterator.
    result->push_front(best.pop(), &ctx->root);
  }

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
  handler *h= table->file->lookup_handler;
  MHNSW_Context ctx(table, vec_field);

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_VECTOR);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(vec_field->binary());
  DBUG_ASSERT(vec_field->cmp_type() == STRING_RESULT);
  DBUG_ASSERT(res); // ER_INDEX_CANNOT_HAVE_NULL
  DBUG_ASSERT(h->ref_length <= graph->field[1]->field_length);

  // XXX returning an error here will rollback the insert in InnoDB
  // but in MyISAM the row will stay inserted, making the index out of sync:
  // invalid vector values are present in the table but cannot be found
  // via an index. The easiest way to fix it is with a VECTOR(N) type
  if (res->length() == 0 || res->length() % 4)
    return bad_value_on_insert(vec_field);

  const double NORMALIZATION_FACTOR= 1 / std::log(thd->variables.hnsw_max_connection_per_layer);

  if (int err= h->ha_rnd_init(1))
    return err;

  SCOPE_EXIT([h](){ h->ha_rnd_end(); });

  if (int err= graph->file->ha_index_init(0, 1))
    return err;

  SCOPE_EXIT([graph](){ graph->file->ha_index_end(); });

  h->position(table->record[0]);

  if (int err= graph->file->ha_index_last(graph->record[0]))
  {
    if (err != HA_ERR_END_OF_FILE)
      return err;

    // First insert!
    FVectorNode target(&ctx, h->ref);
    ctx.target= &target;
    return write_neighbors(&ctx, 0, target, {});
  }

  longlong max_layer= graph->field[0]->val_int();

  List<FVectorNode> candidates;
  List<FVectorNode> start_nodes;
  String ref_str, *ref_ptr;

  ref_ptr= graph->field[1]->val_str(&ref_str);
  FVectorNode start_node(&ctx, ref_ptr->ptr());

  // TODO(cvicentiu) use a random start node in last layer.
  // XXX or may be *all* nodes in the last layer? there should be few
  if (start_nodes.push_back(&start_node, &ctx.root))
    return HA_ERR_OUT_OF_MEM;

  if (int err= start_node.instantiate_vector())
    return err;

  if (ctx.vec_len * sizeof(float) != res->length())
    return bad_value_on_insert(vec_field);

  FVectorNode target(&ctx, h->ref, res->ptr());
  ctx.target= &target;

  double new_num= my_rnd(&thd->rand);
  double log= -std::log(new_num) * NORMALIZATION_FACTOR;
  longlong new_node_layer= static_cast<longlong>(std::floor(log));

  for (longlong cur_layer= max_layer; cur_layer > new_node_layer; cur_layer--)
  {
    if (int err= search_layer(&ctx, start_nodes,
                              thd->variables.hnsw_ef_constructor, cur_layer,
                              &candidates))
      return err;
    start_nodes.empty();
    start_nodes.push_back(candidates.head(), &ctx.root); // XXX ef=1
    candidates.empty();
  }

  for (longlong cur_layer= std::min(max_layer, new_node_layer);
       cur_layer >= 0; cur_layer--)
  {
    List<FVectorNode> neighbors;
    if (int err= search_layer(&ctx, start_nodes,
                              thd->variables.hnsw_ef_constructor, cur_layer,
                              &candidates))
      return err;

    uint max_neighbors= (cur_layer == 0) ? // heuristics from the paper
     thd->variables.hnsw_max_connection_per_layer * 2
     : thd->variables.hnsw_max_connection_per_layer;

    if (int err= select_neighbors(&ctx, cur_layer, target, candidates,
                                  max_neighbors, &neighbors))
      return err;
    if (int err= update_neighbors(&ctx, cur_layer, max_neighbors, target,
                                  neighbors))
      return err;
    start_nodes= candidates;
  }
  start_nodes.empty();

  // XXX what is that?
  for (longlong cur_layer= max_layer + 1; cur_layer <= new_node_layer;
       cur_layer++)
  {
    if (int err= write_neighbors(&ctx, cur_layer, target, {}))
      return err;
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

  SCOPE_EXIT([graph](){ graph->file->ha_index_end(); });

  if (int err= graph->file->ha_index_last(graph->record[0]))
    return err;

  longlong max_layer= graph->field[0]->val_int();

  List<FVectorNode> candidates; // XXX List? not Queue by distance?
  List<FVectorNode> start_nodes;
  String ref_str, *ref_ptr= graph->field[1]->val_str(&ref_str);

  FVectorNode start_node(&ctx, ref_ptr->ptr());

  // TODO(cvicentiu) use a random start node in last layer.
  // XXX or may be *all* nodes in the last layer? there should be few
  if (start_nodes.push_back(&start_node, &ctx.root))
    return HA_ERR_OUT_OF_MEM;

  if (int err= start_node.instantiate_vector())
    return err;

  /*
    if the query vector is NULL or invalid, VEC_DISTANCE will return
    NULL, so the result is basically unsorted, we can return rows
    in any order. For simplicity let's sort by the start_node.
  */
  if (!res || ctx.vec_len * sizeof(float) != res->length())
    res= vec_field->val_str(&buf);

  FVector target(&ctx, res->ptr());
  ctx.target= &target;

  ulonglong ef_search= std::max<ulonglong>( //XXX why not always limit?
    thd->variables.hnsw_ef_search, limit);

  for (size_t cur_layer= max_layer; cur_layer > 0; cur_layer--)
  {
    //XXX in the paper ef_search=1 here
    if (int err= search_layer(&ctx, start_nodes, ef_search, cur_layer,
                              &candidates))
      return err;
    start_nodes.empty();
    start_nodes.push_back(candidates.head(), &ctx.root); // XXX so ef_search=1 ???
    candidates.empty();
  }

  if (int err= search_layer(&ctx, start_nodes, ef_search, 0, &candidates))
    return err;

  size_t context_size=limit * h->ref_length + sizeof(ulonglong);
  char *context= thd->alloc(context_size);
  graph->context= context;

  *(ulonglong*)context= limit;
  context+= context_size;

  while (limit--)
  {
    context-= h->ref_length;
    memcpy(context, candidates.pop()->get_ref(), h->ref_length);
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
