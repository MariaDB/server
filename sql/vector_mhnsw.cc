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
#include <random>

#include <my_global.h>
#include "vector_mhnsw.h"

#include "field.h"
#include "item.h"
#include "item_vectorfunc.h"
#include "key.h"
#include "my_base.h"
#include "mysql/psi/psi_base.h"
#include "scope.h"
#include "sql_queue.h"

const LEX_CSTRING mhnsw_hlindex_table={STRING_WITH_LEN("\
  CREATE TABLE i (                                      \
    layer int not null,                                 \
    src varbinary(255) not null,                        \
    dst varbinary(255) not null,                        \
    index (layer, src, dst))                            \
")};


class FVectorRef
{
public:
  // Shallow ref copy. Used for other ref lookups in HashSet
  FVectorRef(uchar *ref, size_t ref_len): ref{ref}, ref_len{ref_len} {}

  static uchar *get_key(const FVectorRef *elem, size_t *key_len, my_bool)
  {
    *key_len= elem->ref_len;
    return elem->ref;
  }

  size_t get_ref_len() const { return ref_len; }
  const uchar* get_ref() const { return ref; }

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
  bool init(const uchar *ref, size_t ref_length,
            const float *vec, size_t vec_length)
  {
    this->ref= (uchar *)my_malloc(PSI_NOT_INSTRUMENTED,
                                  ref_length + vec_length * sizeof(float),
                                  MYF(0));
    if (!this->ref)
      return true;
    this->vec= (float *)(this->ref + ref_length);
    ref_len= ref_length;
    vec_len= vec_length;
    memcpy(this->ref, ref, ref_len);
    memcpy(this->vec, vec, vec_len * sizeof(float));
    return false;
  }

  double distance_to(const FVector &other) const
  {
    return euclidean_vec_distance(vec, other.vec, vec_len);
  }
};


static int cmp_vec(const FVector *reference, FVector *a, FVector *b)
{
  double a_dist= reference->distance_to(*a);
  double b_dist= reference->distance_to(*b);

  if (a_dist < b_dist)
    return -1;
  if (a_dist > b_dist)
    return 1;
  return 0;
}


class Neighbours_iterator
{
public:
  Neighbours_iterator(const TABLE *graph, const FVector &src, size_t layer)
    : inited{false}, ptr{ref, src.get_ref_len()}, src{src}, graph{graph}, layer{layer}
  {
    // TODO(cvicentiu) this should not be part of the graph.
    key= (uchar*)my_malloc(PSI_NOT_INSTRUMENTED,
                           graph->key_info->key_length,
                           MYF(0));
  }

  // Returns shallow FVectorRefs.
  FVectorRef *next()
  {
    if (!inited)
    {
      graph->field[0]->store(layer, true);
      graph->field[1]->store_binary(
          reinterpret_cast<const char *>(src.get_ref()),
          src.get_ref_len());
      graph->field[2]->set_null();
      key_copy(key, graph->record[0],
               graph->key_info, graph->key_info->key_length);
      if ((graph->file->ha_index_read_map(graph->record[0], key,
                                          (1 | 2),
                                          HA_READ_KEY_EXACT)))
        return nullptr;
      inited= true;
    }
    else
      if (graph->file->ha_index_next_same(graph->record[0], key,
                                          graph->key_info->key_length))
        return nullptr;

    //TODO This does two memcpys, one should use str's buffer.
    String *str= graph->field[2]->val_str(&strbuf);
    memcpy(ref, str->ptr(), ptr.get_ref_len());

    return &ptr;
  }

private:
  String strbuf;
  bool inited;
  uchar ref[1000];
  FVectorRef ptr;
  uchar *key;
  const FVector &src;
  const TABLE *graph;
  size_t layer;
};


static FVector *get_fvector_from_source(TABLE *source,
                                        Field *vect_field,
                                        const FVectorRef &ref)
{
  FVector *new_vector= new FVector;
  if (!new_vector)
    return nullptr;

  source->file->ha_rnd_pos(source->record[0],
                           const_cast<uchar *>(ref.get_ref()));

  String buf, *vec;
  vec= vect_field->val_str(&buf);

  // TODO(cvicentiu) error checking
  new_vector->init(ref.get_ref(), ref.get_ref_len(),
                   reinterpret_cast<const float *>(vec->ptr()),
                   vec->length() / sizeof(float));

  return new_vector;
}


static bool search_layer(TABLE *source,
                         Field *vec_field,
                         TABLE *graph,
                         const FVector &target,
                         const List<FVector> &start_nodes,
                         uint max_candidates_return,
                         size_t layer,
                         List<FVector> *result)
{
  DBUG_ASSERT(start_nodes.elements > 0);
  result->empty();

  Queue<FVector, const FVector> candidates;
  Queue<FVector, const FVector> best;
  Hash_set<FVectorRef> visited(PSI_INSTRUMENT_MEM, FVectorRef::get_key);

  candidates.init(1000, 0, false, cmp_vec, &target);
  best.init(1000, 0, true, cmp_vec, &target);

  for (const FVector &node : start_nodes)
  {
    candidates.push(&node);
    best.push(&node);
    visited.insert(&node);
  }

  while (candidates.elements())
  {
    const FVector &cur_vec= *candidates.pop();
    const FVector &furthest_best= *best.top();
    // TODO(cvicentiu) what if we haven't yet reached max_candidates_return?
    // Should we just quit now, maybe continue searching for candidates till
    // we at least get max_candidates_return.
    if (target.distance_to(cur_vec) > target.distance_to(furthest_best))
      break; // All possible candidates are worse than what we have.
             // Can't get better.

    Neighbours_iterator neigh_iter{graph, cur_vec, layer};
    FVectorRef *neigh;
    while ((neigh= neigh_iter.next()))
    {
      if (visited.find(neigh))
        continue;
      FVector *new_vector= get_fvector_from_source(source, vec_field, *neigh);
      if (!new_vector)
        return true;

      visited.insert(new_vector);
      const FVector &furthest_best= *best.top();
      if (best.elements() < max_candidates_return)
      {
        candidates.push(new_vector);
        best.push(new_vector);
      }
      else if (target.distance_to(*new_vector) < target.distance_to(furthest_best))
      {
        best.replace_top(new_vector);
        candidates.push(new_vector);
      }
    }
  }

  while (best.elements())
  {
    // TODO(cvicentiu) this is n*log(n), we need a queue iterator.
    result->push_back(best.pop());
  }
  return false;
}

static bool select_neighbours(TABLE *source, TABLE *graph,
                              const FVector &target,
                              const List<FVector> &candidates,
                              size_t max_neighbour_connections,
                              List<FVector> *neighbours)
{
  Queue<FVector, const FVector> pq;
  pq.init(candidates.elements, 0, 0, cmp_vec, &target);

  // TODO(cvicentiu) error checking.
  for (const auto &candidate : candidates)
    pq.push(&candidate);

  for (size_t i = 0; i < max_neighbour_connections; i++)
  {
    if (!pq.elements())
      break;
    neighbours->push_back(pq.pop());
  }

  return false;
}

static int connection_exists(TABLE *graph,
                             uchar *key,
                             size_t layer_number,
                             const FVectorRef &src,
                             const FVectorRef &dst)
{
  graph->field[0]->store(layer_number);
  graph->field[1]->store_binary(
    reinterpret_cast<const char *>(src.get_ref()), src.get_ref_len());
  graph->field[2]->store_binary(
    reinterpret_cast<const char *>(dst.get_ref()), dst.get_ref_len());

  key_copy(key, graph->record[0],
           graph->key_info, graph->key_info->key_length);

  int err;
  if ((err= graph->file->ha_index_read_map(graph->record[0], key,
                                           HA_WHOLE_KEY,
                                           HA_READ_KEY_EXACT)))
  {
    if (err != HA_ERR_KEY_NOT_FOUND)
      return -1;
    return 0;
  }

  /*
  if ((err= graph->file->ha_index_next_same(graph->record[0], key,
                                            graph->key_info->key_length)))
    return err == HA_ERR_END_OF_FILE ? 0 : -1;
  */
  return 1;
}

static inline bool store_link(TABLE *graph,
                              size_t layer_number,
                              const FVectorRef &src,
                              const FVectorRef &dst)
{
  //TODO(cvicentiu) this causes 2 uneeded stores because field 0 and 1 almost
  //never change.
  //Or we could iterate and store them all src->dest first and then all dest->src
  graph->field[0]->store(layer_number);
  graph->field[1]->store_binary(
    reinterpret_cast<const char *>(src.get_ref()), src.get_ref_len());
  graph->field[2]->store_binary(
    reinterpret_cast<const char *>(dst.get_ref()), dst.get_ref_len());
  return graph->file->ha_write_row(graph->record[0]);
}


static bool write_connection_to_graph(TABLE *graph,
                                      uchar *key,
                                      size_t layer_number,
                                      const FVectorRef &src,
                                      const FVectorRef &dst)
{
  // TODO(cvicentiu) if store_link fails on the second write, we can end up
  // with a corrupt index. Handle this and write undo logic.
  int con_exists= connection_exists(graph, key, layer_number, src, dst);
  if (con_exists)
    return con_exists < 0;

  return store_link(graph, layer_number, src, dst) ||
         store_link(graph, layer_number, dst, src);
}

static bool insert_node_connections_on_layer(TABLE *graph,
                                             size_t layer_number,
                                             const FVector &source_node,
                                             const List<FVector> &dest_nodes)
{
  uchar *key= (uchar*)alloca(graph->key_info->key_length);

  if (dest_nodes.elements == 0)
  {
    return store_link(graph, layer_number, source_node, source_node);
  }

  for (const auto &node: dest_nodes)
  {
    if (write_connection_to_graph(graph, key, layer_number, source_node, node))
      return true;
  }
  return false;
}

static bool get_neighbours(TABLE *source,
                           Field *vec_field,
                           TABLE *graph,
                           size_t layer_number,
                           const FVector &source_node,
                           List<FVector> *neighbours)
{
  int err;
  //TODO(cvicentiu) move this key buffer outside to prevent allocations.
  uchar *key= (uchar*)alloca(graph->key_info->key_length);
  graph->field[1]->store_binary(
    reinterpret_cast<const char *>(source_node.get_ref()),
    source_node.get_ref_len());
  graph->field[2]->set_null();
  graph->field[0]->store(layer_number);

  key_copy(key, graph->record[0],
           graph->key_info, graph->key_info->key_length);

  if ((err= graph->file->ha_index_read_map(graph->record[0], key,
                                           (1 | 2),
                                           HA_READ_KEY_EXACT)))
  {
    if (err != HA_ERR_KEY_NOT_FOUND)
      return true;
    return false;
  }

  do
  {
    String ref_str, *ref_ptr;
    ref_ptr= graph->field[2]->val_str(&ref_str);
    // self-to-self is not needed. TODO(cvicentiu) this link should be
    // removed when we have other links from src.
    if (!memcmp(ref_ptr->ptr(),
                source_node.get_ref(), source_node.get_ref_len()))
      continue;

    FVector *dst_node= get_fvector_from_source(source, vec_field,
                                               {(uchar *)ref_ptr->ptr(),
                                               ref_ptr->length()});

    neighbours->push_back(dst_node);
  } while (!graph->file->ha_index_next_same(graph->record[0], key,
                                            graph->key_info->key_length));

  return false;
}


static bool update_neighbours(TABLE *graph,
                              size_t layer_number,
                              const FVector &source_node,
                              const List<FVector> &old_neighbours,
                              const List<FVector> &new_neighbours)
{
  uchar *key= (uchar*)alloca(graph->key_info->key_length);
  for (const auto &node: old_neighbours)
  {
    int c_e= connection_exists(graph, key, layer_number, source_node, node);
    if (c_e < 0)
      return true;
    DBUG_ASSERT(c_e);
    if (c_e)
      graph->file->ha_delete_row(graph->record[0]);

    c_e= connection_exists(graph, key, layer_number, node, source_node);
    if (c_e < 0)
      return true;
    DBUG_ASSERT(c_e);
    if (c_e)
      graph->file->ha_delete_row(graph->record[0]);
  }

  //TODO(cvicentiu) error checking...
  for (const auto &node: new_neighbours)
  {
    int c_e= connection_exists(graph, key, layer_number, source_node, node);
    if (c_e < 0)
      return true;
    if (!c_e)
      store_link(graph, layer_number, source_node, node);

    c_e= connection_exists(graph, key, layer_number, node, source_node);
    if (c_e < 0)
      return true;
    if (!c_e)
      store_link(graph, layer_number, source_node, node);
  }

  return false;
}

std::mt19937 gen(42); // TODO(cvicentiu) seeded with 42 for now, this should
                      // use a rnd service

int mhnsw_insert(TABLE *table, KEY *keyinfo)
{
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  Field *vec_field= keyinfo->key_part->field;
  String buf, *res= vec_field->val_str(&buf);
  handler *h= table->file;
  int err= 0;

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_MHNSW);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(vec_field->binary());
  DBUG_ASSERT(vec_field->cmp_type() == STRING_RESULT);
  DBUG_ASSERT(res); // ER_INDEX_CANNOT_HAVE_NULL
  DBUG_ASSERT(h->ref_length <= graph->field[1]->field_length);
  DBUG_ASSERT(h->ref_length <= graph->field[2]->field_length);

  if (res->length() == 0 || res->length() % 4)
    return 1;

  const uint EF_CONSTRUCTION = 10; // max candidate list size to connect to.
  const uint MAX_INSERT_NEIGHBOUR_CONNECTIONS = 10;
  const uint MAX_NEIGHBORS_PER_LAYER = 10;
  const double NORMALIZATION_FACTOR = 1.2;

  if ((err= h->ha_rnd_init(1)))
    return err;

  if ((err= graph->file->ha_index_init(0, 1)))
    return err;

  longlong max_layer;
  if ((err= graph->file->ha_index_last(graph->record[0])))
  {
    if (err != HA_ERR_END_OF_FILE)
    {
      graph->file->ha_index_end();
      return err;
    }
    // First insert!
    h->position(table->record[0]);
    FVectorRef ref{h->ref, h->ref_length};
    store_link(graph, 0, ref, ref);

    h->ha_rnd_end();
    graph->file->ha_index_end();
    return 0; // TODO (error during store_link)
  }
  else
    max_layer= graph->field[0]->val_int();

  FVector target;
  h->position(table->record[0]);
  target.init(h->ref, h->ref_length,
              reinterpret_cast<const float *>(res->ptr()),
              res->length() / sizeof(float));


  std::uniform_real_distribution<> dis(0.0, 1.0);
  double new_num= dis(gen);
  double log= -std::log(new_num) * NORMALIZATION_FACTOR;
  longlong new_node_layer= std::floor(log);

  List<FVector> candidates;
  List<FVector> start_nodes;

  String ref_str, *ref_ptr;
  ref_ptr= graph->field[1]->val_str(&ref_str);

  FVector *start_node= get_fvector_from_source(table, vec_field,
                                               {(uchar *)ref_ptr->ptr(),
                                               ref_ptr->length()});
  // TODO(cvicentiu) error checking. Also make sure we use a random start node
  // in last layer.
  start_nodes.push_back(start_node);
  // TODO start_nodes needs to have one element in it.
  for (longlong cur_layer= max_layer; cur_layer > new_node_layer; cur_layer--)
  {
    search_layer(table, vec_field, graph, target, start_nodes, 1, cur_layer,
                 &candidates);
    start_nodes.empty();
    start_nodes.push_back(candidates.head());

    // Candidates = SEARCH_LAYER(q, eP, eF=1, cur_layer)
    // eP = closest_from_candidates;
  }

  for (longlong cur_layer= std::min(max_layer, new_node_layer);
       cur_layer >= 0; cur_layer--)
  {
    List<FVector> neighbours;
    search_layer(table, vec_field, graph, target, start_nodes, EF_CONSTRUCTION,
                 cur_layer, &candidates);
    select_neighbours(table, graph, target, candidates,
                      MAX_INSERT_NEIGHBOUR_CONNECTIONS, &neighbours);
    insert_node_connections_on_layer(graph, cur_layer, target, neighbours);

    for (const auto& neigh : neighbours)
    {
      List<FVector> second_degree_neigh;
      get_neighbours(table, vec_field, graph, cur_layer, neigh,
                     &second_degree_neigh);
      if (second_degree_neigh.elements > MAX_NEIGHBORS_PER_LAYER)
      {
        List<FVector> remaining_second_degree_neigh;
        select_neighbours(table, graph, neigh, second_degree_neigh,
                          MAX_NEIGHBORS_PER_LAYER,
                          &remaining_second_degree_neigh);
        update_neighbours(graph, cur_layer, neigh,
                          second_degree_neigh,
                          remaining_second_degree_neigh);
      }
    }
    start_nodes= std::move(candidates);

    // Candidates = SEARCH_LAYER(q, eP, eF=EF_CONSTRUCTION, cur_layer)
    // neighbours = SELECT_NEIGHBOURS(q, Candidates, MAX_INSERT_NEIGHBORS_CONNECTIONS)
    // insert neighbours into graph on layer cur_layer

    // For each neighbour n
    // old_n = neighbours(n)
    // if old_n.size() > MAX_NEIGHBORS_PER_LAYER
    //  new_n = SELECT_NEIGHBOURS(n, old_n, MAX_NEIGHBORS_PER_LAYER, cur_layer)
    //  neighbours(n) = new_n
    // eP = Candidates
  }

  for (longlong cur_layer= max_layer + 1;
       cur_layer <= new_node_layer; cur_layer++)
  {
    insert_node_connections_on_layer(graph, cur_layer, target, {});
  }

  h->ha_rnd_end();
  graph->file->ha_index_end();
  dbug_tmp_restore_column_map(&table->read_set, old_map);
  return err == HA_ERR_END_OF_FILE ? 0 : err;
}



struct Node
{
  float distance;
  uchar ref[1000];
};

/*
static int cmp_node_distances(void *, Node *a, Node *b)
{
  if (a->distance < b->distance)
    return -1;
  if (a->distance > b->distance)
    return 1;
  return 0;
}
*/

int mhnsw_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit)
{
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  // TODO(cvicentiu) onlye one hlindex now.
  Field *vec_field= keyinfo->key_part->field;
  Item_func_vec_distance *fun= (Item_func_vec_distance *)dist;
  String buf, *res= fun->arguments()[1]->val_str(&buf);
  FVector target;
  handler *h= table->file;

  //TODO(scope_exit)
  int err;
  if ((err= h->ha_rnd_init(0)))
    return err;

  if ((err= graph->file->ha_index_init(0, 1)))
    return err;

  h->position(table->record[0]);
  target.init(h->ref, h->ref_length,
              reinterpret_cast<const float *>(res->ptr()),
              res->length() / sizeof(float));

  List<FVector> candidates;
  List<FVector> start_nodes;

  longlong max_layer;
  if ((err= graph->file->ha_index_last(graph->record[0])))
  {
    if (err != HA_ERR_END_OF_FILE)
    {
      graph->file->ha_index_end();
      return err;
    }
    h->ha_rnd_end();
    graph->file->ha_index_end();
    return 0; // TODO (error during store_link)
  }
  else
    max_layer= graph->field[0]->val_int();

  String ref_str, *ref_ptr;
  ref_ptr= graph->field[1]->val_str(&ref_str);
  FVector *start_node= get_fvector_from_source(table, vec_field,
                                               {(uchar *)ref_ptr->ptr(),
                                               ref_ptr->length()});
  // TODO(cvicentiu) error checking. Also make sure we use a random start node
  // in last layer.
  start_nodes.push_back(start_node);


  for (size_t cur_layer= max_layer; cur_layer > 0; cur_layer--)
  {
    search_layer(table, vec_field, graph, target, start_nodes, 1,
                 cur_layer, &candidates);
    start_nodes.empty();
    start_nodes.push_back(candidates.head());
  }
  search_layer(table, vec_field, graph, target, start_nodes, limit,
               0, &candidates);

  // 8. return results
  FVectorRef **context= (FVectorRef**)table->in_use->alloc(
    sizeof(FVectorRef**)*candidates.elements + 1);
  graph->context= context;

  FVectorRef **ptr= context + candidates.elements;
  *ptr= 0;
  while (candidates.elements)
    *--ptr= candidates.pop();

  err= mhnsw_next(table);
  graph->file->ha_index_end();

  dbug_tmp_restore_column_map(&table->read_set, old_map);
  return err;
}

int mhnsw_next(TABLE *table)
{
  FVectorRef ***context= (FVectorRef***)&table->hlindex->context;
  if (**context)
    return table->file->ha_rnd_pos(table->record[0],
                                   (uchar *)(*(*context)++)->get_ref());
  return HA_ERR_END_OF_FILE;
}


/*
int mhnsw_first(TABLE *table, Item *dist, ulonglong limit)
{
  TABLE *graph= table->hlindex;
  Queue<Node> todo, result;
  Node *cur;
  String *str, strbuf;
  const size_t ref_length= table->file->ref_length;
  const size_t element_size= ref_length + sizeof(float);
  Hash_set<Node> visited(PSI_INSTRUMENT_MEM, &my_charset_bin, limit,
                         sizeof(float), ref_length, 0, 0, HASH_UNIQUE);
  int err= 0;

  DBUG_ASSERT(graph);

  if (todo.init(1000, 0, 0, cmp_node_distances)) // XXX + autoextent
    return HA_ERR_OUT_OF_MEM;

  if (result.init(limit, 0, 1, cmp_node_distances))
    return HA_ERR_OUT_OF_MEM;

  if ((err= graph->file->ha_index_init(0, 1)))
    return err;

  SCOPE_EXIT([graph](){ graph->file->ha_index_end(); });

  // 1. read a start row
  if ((err= graph->file->ha_index_last(graph->record[0])))
    return err;

  if (!(str= graph->field[1]->val_str(&strbuf)))
    return HA_ERR_CRASHED;
  int layer= graph->field[0]->val_int();

  DBUG_ASSERT(str->length() == ref_length);

  cur= (Node*)table->in_use->alloc(element_size);
  memcpy(cur->ref, str->ptr(), ref_length);

  if ((err= table->file->ha_rnd_init(0)))
    return err;

  if ((err= table->file->ha_rnd_pos(table->record[0], cur->ref)))
    return HA_ERR_CRASHED;

  // 2. add it to the todo
  cur->distance= dist->val_real();
  if (dist->is_null())
    return HA_ERR_END_OF_FILE;
  todo.push(cur);
  visited.insert(cur);

  uchar *key= (uchar*)alloca(graph->key_info->key_length);
  while (todo.elements())
  {
    // 3. pick the top node from the todo
    cur= todo.pop();

    // 4. add it to the result
    if (result.is_full())
    {
      // 5. if not added, greedy search done
      if (cur->distance > result.top()->distance)
        break;
      result.replace_top(cur);
    }
    else
      result.push(cur);

    float threshold= result.is_full() ? result.top()->distance : FLT_MAX;

    graph->field[0]->store(0); // TODO: Layer unaware.
    graph->field[1]->store_binary((const char *)cur->ref,
                                  table->file->ref_length);
    graph->field[2]->set_null();
    // 6. add all its [yet unvisited] neighbours to the todo heap
    key_copy(key, graph->record[0],
             graph->key_info, graph->key_info->key_length);
    if ((err= graph->file->ha_index_read_map(graph->record[0], key,
                                             (1 | 2),
                                             HA_READ_KEY_EXACT)))
      return HA_ERR_CRASHED;

    do {
      if (!(str= graph->field[2]->val_str(&strbuf)))
        return HA_ERR_CRASHED;

      if (visited.find(str->ptr(), ref_length))
        continue;

      if ((err= table->file->ha_rnd_pos(table->record[0], (uchar*)str->ptr())))
        return HA_ERR_CRASHED;

      float distance= dist->val_real();
      if (distance > threshold)
        continue;

      cur= (Node*)table->in_use->alloc(element_size);
      cur->distance= distance;
      memcpy(cur->ref, str->ptr(), ref_length);
      todo.push(cur);
      visited.insert(cur);
    } while (!graph->file->ha_index_next_same(graph->record[0], key,
                                              graph->key_info->key_length));
    // 7. goto 3
  }

  // 8. return results
  Node **context= (Node**)table->in_use->alloc(sizeof(Node**)*result.elements()+1);
  graph->context= context;

  Node **ptr= context+result.elements();
  *ptr= 0;
  while (result.elements())
    *--ptr= result.pop();

  return mhnsw_next(table);
}
*/

/*
int mhnsw_next(TABLE *table)
{
  Node ***context= (Node***)&table->hlindex->context;
  if (**context)
    return table->file->ha_rnd_pos(table->record[0], (*(*context)++)->ref);
  return HA_ERR_END_OF_FILE;
}
*/
