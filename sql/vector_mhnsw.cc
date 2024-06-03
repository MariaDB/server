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


class FVectorRef: public Sql_alloc
{
public:
  // Shallow ref copy. Used for other ref lookups in HashSet
  FVectorRef(const void *ref, size_t ref_len): ref{(uchar*)ref}, ref_len{ref_len} {}

  static uchar *get_key(const FVectorRef *elem, size_t *key_len, my_bool)
  {
    *key_len= elem->ref_len;
    return elem->ref;
  }

  static void free_vector(void *elem)
  {
    delete (FVectorRef *)elem;
  }

  size_t get_ref_len() const { return ref_len; }
  uchar* get_ref() const { return ref; }

protected:
  FVectorRef() = default;
  uchar *ref;
  size_t ref_len;
};

class FVector: public FVectorRef
{
private:
  float *vec;
  size_t vec_len;
public:
  FVector(): vec(nullptr), vec_len(0) {}

  bool init(MEM_ROOT *root, const uchar *ref_, size_t ref_len_, const void *vec_, size_t bytes)
  {
    ref= (uchar*)alloc_root(root, ref_len_ + bytes);
    if (!ref)
      return true;

    vec= reinterpret_cast<float *>(ref + ref_len_);

    memcpy(ref, ref_, ref_len_);
    memcpy(vec, vec_, bytes);

    ref_len= ref_len_;
    vec_len= bytes / sizeof(float);

    return false;
  }

  size_t size_of() const { return vec_len * sizeof(float); }

  float distance_to(const FVector &other) const
  {
    DBUG_ASSERT(other.vec_len == vec_len);
    return euclidean_vec_distance(vec, other.vec, vec_len);
  }
};

class MHNSW_Context
{
  public:
  MEM_ROOT root;
  TABLE *table;
  Field *vec_field;
  Hash_set<FVectorRef> vector_cache{PSI_INSTRUMENT_MEM, FVectorRef::get_key};
  Hash_set<FVectorRef> vector_ref_cache{PSI_INSTRUMENT_MEM, FVectorRef::get_key};

  MHNSW_Context(TABLE *table, Field *vec_field)
    : table(table), vec_field(vec_field)
  {
    init_alloc_root(PSI_INSTRUMENT_MEM, &root, 8192, 0, MYF(MY_THREAD_SPECIFIC));
  }

  ~MHNSW_Context()
  {
    free_root(&root, MYF(0));
  }

  FVectorRef *get_fvector_ref(const uchar *ref, size_t ref_len)
  {
    FVectorRef tmp(ref, ref_len);
    FVectorRef *v= vector_ref_cache.find(&tmp);
    if (v)
      return v;

    uchar *buf= (uchar*)memdup_root(&root, ref, ref_len);
    if ((v= new (&root) FVectorRef(buf, ref_len)))
      vector_ref_cache.insert(v);
    return v;
  }

  FVector *get_fvector_from_source(const FVectorRef &ref)
  {
    FVectorRef *v= vector_cache.find(&ref);
    if (v)
      return (FVector *)v;

    if (table->file->ha_rnd_pos(table->record[0], ref.get_ref()))
      return nullptr; // XXX the error code is lost

    String buf, *vec= vec_field->val_str(&buf);

    FVector *new_vector= new (&root) FVector;
    new_vector->init(&root, ref.get_ref(), ref.get_ref_len(), vec->ptr(), vec->length());

    vector_cache.insert(new_vector);

    return new_vector;
  }
};

static int cmp_vec(const FVector *target, const FVector *a, const FVector *b)
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
                         const FVectorRef &source_node,
                         List<FVectorRef> *neighbors)
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

  const uchar *neigh_arr_bytes= reinterpret_cast<const uchar *>(str->ptr());
  uint number_of_neighbors= HNSW_MAX_M_read(neigh_arr_bytes);
  if (number_of_neighbors * ref_length + HNSW_MAX_M_WIDTH != str->length())
    return HA_ERR_CRASHED; // should not happen, corrupted HNSW index

  const uchar *pos= neigh_arr_bytes + HNSW_MAX_M_WIDTH;
  for (uint i= 0; i < number_of_neighbors; i++)
  {
    FVectorRef *v= ctx->get_fvector_ref(pos, ref_length);
    if (!v)
      return HA_ERR_OUT_OF_MEM;
    neighbors->push_back(v, &ctx->root);
    pos+= ref_length;
  }

  return 0;
}


static int select_neighbors(MHNSW_Context *ctx,
                            size_t layer_number, const FVector &target,
                            const List<FVectorRef> &candidates,
                            size_t max_neighbor_connections,
                            List<FVectorRef> *neighbors)
{
  /*
    TODO: If the input neighbors list is already sorted in search_layer, then
    no need to do additional queue build steps here.
   */

  Hash_set<FVectorRef> visited(PSI_INSTRUMENT_MEM, FVectorRef::get_key);

  Queue<FVector, const FVector> pq; // working queue
  Queue<FVector, const FVector> pq_discard; // queue for discarded candidates
  Queue<FVector, const FVector> best; // neighbors to return

  // TODO(cvicentiu) this 1000 here is a hardcoded value for max queue size.
  // This should not be fixed.
  if (pq.init(10000, 0, cmp_vec, &target) ||
      pq_discard.init(10000, 0, cmp_vec, &target) ||
      best.init(max_neighbor_connections, true, cmp_vec, &target))
    return HA_ERR_OUT_OF_MEM;

  for (const FVectorRef &candidate : candidates)
  {
    FVector *v= ctx->get_fvector_from_source(candidate);
    if (!v)
      return HA_ERR_OUT_OF_MEM;
    visited.insert(&candidate);
    pq.push(v);
  }

  if (EXTEND_CANDIDATES)
  {
    for (const FVectorRef &candidate : candidates)
    {
      List<FVectorRef> candidate_neighbors;
      if (int err= get_neighbors(ctx, layer_number, candidate,
                                 &candidate_neighbors))
        return err;
      for (const FVectorRef &extra_candidate : candidate_neighbors)
      {
        if (visited.find(&extra_candidate))
          continue;
        visited.insert(&extra_candidate);
        FVector *v= ctx->get_fvector_from_source(extra_candidate);
        if (!v)
          return HA_ERR_OUT_OF_MEM;
        pq.push(v);
      }
    }
  }

  DBUG_ASSERT(pq.elements());
  best.push(pq.pop());

  float best_top= best.top()->distance_to(target);
  while (pq.elements() && best.elements() < max_neighbor_connections)
  {
    const FVector *vec= pq.pop();
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
                               const FVectorRef &ref)
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

static void dbug_print_vec_neigh(uint layer, const List<FVectorRef> &neighbors)
{
#ifndef DBUG_OFF
  DBUG_PRINT("VECTOR", ("NEIGH: NUM: %d", neighbors.elements));
  for (const FVectorRef& ref : neighbors)
  {
    dbug_print_vec_ref("NEIGH: ", layer, ref);
  }
#endif
}

static void dbug_print_hash_vec(Hash_set<FVectorRef> &h)
{
#ifndef DBUG_OFF
  for (FVectorRef &ptr : h)
  {
    DBUG_PRINT("VECTOR", ("HASH elem: %p", &ptr));
    dbug_print_vec_ref("VISITED: ", 0, ptr);
  }
#endif
}


static int write_neighbors(MHNSW_Context *ctx, size_t layer_number,
                            const FVectorRef &source_node,
                            const List<FVectorRef> &new_neighbors)
{
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

  uchar *key= static_cast<uchar*>(alloca(graph->key_info->key_length));
  key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);

  // XXX try to write first?
  int err= graph->file->ha_index_read_map(graph->record[1], key, HA_WHOLE_KEY,
                                          HA_READ_KEY_EXACT);

  // no record
  if (err == HA_ERR_KEY_NOT_FOUND)
  {
    dbug_print_vec_ref("INSERT ", layer_number, source_node);
    err= graph->file->ha_write_row(graph->record[0]);
  }
  else if (!err)
  {
    dbug_print_vec_ref("UPDATE ", layer_number, source_node);
    dbug_print_vec_neigh(layer_number, new_neighbors);

    err= graph->file->ha_update_row(graph->record[1], graph->record[0]);
  }
  my_safe_afree(neighbor_array_bytes, total_size);
  return err;
}


static int update_second_degree_neighbors(MHNSW_Context *ctx,
                                          size_t layer_number,
                                          uint max_neighbors,
                                          const FVectorRef &source_node,
                                          const List<FVectorRef> &neighbors)
{
  //dbug_print_vec_ref("Updating second degree neighbors", layer_number, source_node);
  //dbug_print_vec_neigh(layer_number, neighbors);
  for (const FVectorRef &neigh: neighbors) // XXX why this loop?
  {
    List<FVectorRef> new_neighbors;
    if (int err= get_neighbors(ctx, layer_number, neigh, &new_neighbors))
      return err;
    new_neighbors.push_back(&source_node, &ctx->root);
    if (int err= write_neighbors(ctx, layer_number, neigh, new_neighbors))
      return err;
  }

  for (const FVectorRef &neigh: neighbors)
  {
    List<FVectorRef> new_neighbors;
    if (int err= get_neighbors(ctx, layer_number, neigh, &new_neighbors))
      return err;

    if (new_neighbors.elements > max_neighbors)
    {
      // shrink the neighbors
      List<FVectorRef> selected;
      FVector *v= ctx->get_fvector_from_source(neigh);
      if (!v)
        return HA_ERR_OUT_OF_MEM;
      if (int err= select_neighbors(ctx, layer_number, *v,
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
                            const FVectorRef &source_node,
                            const List<FVectorRef> &neighbors)
{
  // 1. update node's neighbors
  if (int err= write_neighbors(ctx, layer_number, source_node, neighbors))
    return err;
  // 2. update node's neighbors' neighbors (shrink before update)
  return update_second_degree_neighbors(ctx, layer_number,
                                        max_neighbors, source_node, neighbors);
}


static int search_layer(MHNSW_Context *ctx, const FVector &target,
                        const List<FVectorRef> &start_nodes,
                        uint max_candidates_return, size_t layer,
                        List<FVectorRef> *result)
{
  DBUG_ASSERT(start_nodes.elements > 0);
  DBUG_ASSERT(result->elements == 0);

  Queue<FVector, const FVector> candidates;
  Queue<FVector, const FVector> best;
  Hash_set<FVectorRef> visited(PSI_INSTRUMENT_MEM, FVectorRef::get_key);

  candidates.init(10000, false, cmp_vec, &target);
  best.init(max_candidates_return, true, cmp_vec, &target);

  for (const FVectorRef &node : start_nodes)
  {
    FVector *v= ctx->get_fvector_from_source(node);
    candidates.push(v);
    if (best.elements() < max_candidates_return)
      best.push(v);
    else if (v->distance_to(target) > best.top()->distance_to(target))
      best.replace_top(v);
    visited.insert(v);
    dbug_print_vec_ref("INSERTING node in visited: ", layer, node);
  }

  float furthest_best= best.top()->distance_to(target);
  while (candidates.elements())
  {
    const FVector &cur_vec= *candidates.pop();
    float cur_distance= cur_vec.distance_to(target);
    if (cur_distance > furthest_best && best.elements() == max_candidates_return)
    {
      break; // All possible candidates are worse than what we have.
             // Can't get better.
    }

    List<FVectorRef> neighbors;
    get_neighbors(ctx, layer, cur_vec, &neighbors);

    for (const FVectorRef &neigh: neighbors)
    {
      dbug_print_hash_vec(visited);
      if (visited.find(&neigh))
        continue;

      FVector *clone= ctx->get_fvector_from_source(neigh);
      visited.insert(clone);
      if (best.elements() < max_candidates_return)
      {
        candidates.push(clone);
        best.push(clone);
        furthest_best= best.top()->distance_to(target);
      }
      else if (clone->distance_to(target) < furthest_best)
      {
        best.replace_top(clone);
        candidates.push(clone);
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

  if (int err= graph->file->ha_index_last(graph->record[0]))
  {
    if (err != HA_ERR_END_OF_FILE)
      return err;

    // First insert!
    h->position(table->record[0]);
    return write_neighbors(&ctx, 0, {h->ref, h->ref_length}, {});
  }

  longlong max_layer= graph->field[0]->val_int();

  h->position(table->record[0]);

  List<FVectorRef> candidates;
  List<FVectorRef> start_nodes;
  String ref_str, *ref_ptr;

  ref_ptr= graph->field[1]->val_str(&ref_str);
  FVectorRef start_node_ref{ref_ptr->ptr(), ref_ptr->length()};

  // TODO(cvicentiu) use a random start node in last layer.
  // XXX or may be *all* nodes in the last layer? there should be few
  if (start_nodes.push_back(&start_node_ref, &ctx.root))
    return HA_ERR_OUT_OF_MEM;

  FVector *v= ctx.get_fvector_from_source(start_node_ref);
  if (!v)
    return HA_ERR_OUT_OF_MEM;

  if (v->size_of() != res->length())
    return bad_value_on_insert(vec_field);

  FVector target;
  target.init(&ctx.root, h->ref, h->ref_length, res->ptr(), res->length());

  double new_num= my_rnd(&thd->rand);
  double log= -std::log(new_num) * NORMALIZATION_FACTOR;
  longlong new_node_layer= static_cast<longlong>(std::floor(log));

  for (longlong cur_layer= max_layer; cur_layer > new_node_layer; cur_layer--)
  {
    if (int err= search_layer(&ctx, target, start_nodes,
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
    List<FVectorRef> neighbors;
    if (int err= search_layer(&ctx, target, start_nodes,
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

  List<FVectorRef> candidates; // XXX List? not Queue by distance?
  List<FVectorRef> start_nodes;
  String ref_str, *ref_ptr;

  ref_ptr= graph->field[1]->val_str(&ref_str);
  FVectorRef start_node_ref{ref_ptr->ptr(), ref_ptr->length()};

  // TODO(cvicentiu) use a random start node in last layer.
  // XXX or may be *all* nodes in the last layer? there should be few
  if (start_nodes.push_back(&start_node_ref, &ctx.root))
    return HA_ERR_OUT_OF_MEM;

  FVector *v= ctx.get_fvector_from_source(start_node_ref);
  if (!v)
    return HA_ERR_OUT_OF_MEM;

  /*
    if the query vector is NULL or invalid, VEC_DISTANCE will return
    NULL, so the result is basically unsorted, we can return rows
    in any order. For simplicity let's sort by the start_node.
  */
  if (!res || v->size_of() != res->length())
    res= vec_field->val_str(&buf);

  FVector target;
  if (target.init(&ctx.root, h->ref, h->ref_length, res->ptr(), res->length()))
    return HA_ERR_OUT_OF_MEM;

  ulonglong ef_search= std::max<ulonglong>( //XXX why not always limit?
    thd->variables.hnsw_ef_search, limit);

  for (size_t cur_layer= max_layer; cur_layer > 0; cur_layer--)
  {
    //XXX in the paper ef_search=1 here
    if (int err= search_layer(&ctx, target, start_nodes, ef_search,
                              cur_layer, &candidates))
      return err;
    start_nodes.empty();
    start_nodes.push_back(candidates.head(), &ctx.root); // XXX so ef_search=1 ???
    candidates.empty();
  }

  if (int err= search_layer(&ctx, target, start_nodes, ef_search, 0,
                            &candidates))
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
