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
#include "hash.h"
#include "item.h"
#include "item_vectorfunc.h"
#include "key.h"
#include "my_base.h"
#include "mysql/psi/psi_base.h"
#include "sql_queue.h"

#define HNSW_MAX_M 10000

const LEX_CSTRING mhnsw_hlindex_table={STRING_WITH_LEN("\
  CREATE TABLE i (                                      \
    layer int not null,                                 \
    src varbinary(255) not null,                        \
    neighbors varbinary(10000) not null,                 \
    index (layer, src))                                 \
")};


class FVectorRef
{
public:
  // Shallow ref copy. Used for other ref lookups in HashSet
  FVectorRef(uchar *ref, size_t ref_len): ref{ref}, ref_len{ref_len} {}
  virtual ~FVectorRef() {}

  static const uchar *get_key(const FVectorRef *elem, size_t *key_len, my_bool)
  {
    *key_len= elem->ref_len;
    return elem->ref;
  }

  static void free_vector(void *elem)
  {
    delete (FVectorRef *)elem;
  }

  size_t get_ref_len() const { return ref_len; }
  const uchar* get_ref() const { return ref; }

protected:
  FVectorRef() = default;
  uchar *ref;
  size_t ref_len;
};

class FVector;

Hash_set<FVectorRef> all_vector_set(
  PSI_INSTRUMENT_MEM, &my_charset_bin,
  1000, 0, 0,
  (my_hash_get_key)FVectorRef::get_key,
  NULL,
  HASH_UNIQUE);

Hash_set<FVectorRef> all_vector_ref_set(
  PSI_INSTRUMENT_MEM, &my_charset_bin,
  1000, 0, 0,
  (my_hash_get_key)FVectorRef::get_key,
  NULL,
  HASH_UNIQUE);


class FVector: public FVectorRef
{
private:
  float *vec;
  size_t vec_len;
public:
  FVector(): vec(nullptr), vec_len(0) {}
  ~FVector() { my_free(this->ref); }

  bool init(const uchar *ref, size_t ref_len,
            const float *vec, size_t vec_len)
  {
    this->ref= (uchar *)my_malloc(PSI_NOT_INSTRUMENTED,
                                  ref_len + vec_len * sizeof(float),
                                  MYF(0));
    if (!this->ref)
      return true;

    this->vec= reinterpret_cast<float *>(this->ref + ref_len);

    memcpy(this->ref, ref, ref_len);
    memcpy(this->vec, vec, vec_len * sizeof(float));

    this->ref_len= ref_len;
    this->vec_len= vec_len;
    return false;
  }

  size_t get_vec_len() const { return vec_len; }
  const float* get_vec() const { return vec; }

  double distance_to(const FVector &other) const
  {
    DBUG_ASSERT(other.vec_len == vec_len);
    return euclidean_vec_distance(vec, other.vec, vec_len);
  }

  static FVectorRef *get_fvector_ref(const uchar *ref, size_t ref_len)
  {
    FVectorRef tmp{(uchar*)ref, ref_len};
    FVectorRef *v= all_vector_ref_set.find(&tmp);
    if (v)
      return v;

    // TODO(cvicentiu) memory management.
    uchar *buf= (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, ref_len, MYF(0));
    memcpy(buf, ref, ref_len);
    v= new FVectorRef{buf, ref_len};
    all_vector_ref_set.insert(v);
    return v;
  }

  static FVector *get_fvector_from_source(TABLE *source,
                                          Field *vect_field,
                                          const FVectorRef &ref)
  {

    FVectorRef *v= all_vector_set.find(&ref);
    if (v)
      return (FVector *)v;

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

    all_vector_set.insert(new_vector);

    return new_vector;
  }
};




static int cmp_vec(const FVector *reference, const FVector *a, const FVector *b)
{
  double a_dist= reference->distance_to(*a);
  double b_dist= reference->distance_to(*b);

  if (a_dist < b_dist)
    return -1;
  if (a_dist > b_dist)
    return 1;
  return 0;
}

const bool KEEP_PRUNED_CONNECTIONS=true;
const bool EXTEND_CANDIDATES=true;

static bool get_neighbours(TABLE *graph,
                           size_t layer_number,
                           const FVectorRef &source_node,
                           List<FVectorRef> *neighbours);

static bool select_neighbours(TABLE *source, TABLE *graph,
                              Field *vect_field,
                              size_t layer_number,
                              const FVector &target,
                              const List<FVectorRef> &candidates,
                              size_t max_neighbour_connections,
                              List<FVectorRef> *neighbours)
{
  /*
    TODO: If the input neighbours list is already sorted in search_layer, then
    no need to do additional queue build steps here.
   */

  Hash_set<FVectorRef> visited(PSI_INSTRUMENT_MEM, &my_charset_bin,
                               1000, 0, 0,
                               (my_hash_get_key)FVectorRef::get_key,
                               NULL,
                               HASH_UNIQUE);

  Queue<FVector, const FVector> pq;
  Queue<FVector, const FVector> pq_discard;
  Queue<FVector, const FVector> best;

  // TODO(cvicentiu) this 1000 here is a hardcoded value for max queue size.
  // This should not be fixed.
  pq.init(10000, 0, cmp_vec, &target);
  pq_discard.init(10000, 0, cmp_vec, &target);
  best.init(max_neighbour_connections, true, cmp_vec, &target);

  // TODO(cvicentiu) error checking.
  for (const FVectorRef &candidate : candidates)
  {
    pq.push(FVector::get_fvector_from_source(source, vect_field, candidate));
    visited.insert(&candidate);
  }


  if (EXTEND_CANDIDATES)
  {
    for (const FVectorRef &candidate : candidates)
    {
      List<FVectorRef> candidate_neighbours;
      get_neighbours(graph, layer_number, candidate, &candidate_neighbours);
      for (const FVectorRef &extra_candidate : candidate_neighbours)
      {
        if (visited.find(&extra_candidate))
          continue;
        visited.insert(&extra_candidate);
        pq.push(FVector::get_fvector_from_source(source,
                                                 vect_field,
                                                 extra_candidate));
      }
    }
  }

  DBUG_ASSERT(pq.elements());
  best.push(pq.pop());

  double best_top = best.top()->distance_to(target);
  while (pq.elements() && best.elements() < max_neighbour_connections)
  {
    const FVector *vec= pq.pop();
    double cur_dist = vec->distance_to(target);
    // TODO(cvicentiu) best distance can be cached.
    if (cur_dist < best_top) {

      best.push(vec);
      best_top = cur_dist;
    }
    else
      pq_discard.push(vec);
  }

  if (KEEP_PRUNED_CONNECTIONS)
  {
    while (pq_discard.elements() &&
           best.elements() < max_neighbour_connections)
    {
      best.push(pq_discard.pop());
    }
  }

  DBUG_ASSERT(best.elements() <= max_neighbour_connections);
  while (best.elements()) {
    neighbours->push_front(best.pop());
  }

  return false;
}

//static bool select_neighbours(TABLE *source, TABLE *graph,
//                                   Field *vect_field,
//                                   size_t layer_number,
//                                   const FVector &target,
//                                   const List<FVectorRef> &candidates,
//                                   size_t max_neighbour_connections,
//                                   List<FVectorRef> *neighbours)
//{
//  /*
//    TODO: If the input neighbours list is already sorted in search_layer, then
//    no need to do additional queue build steps here.
//   */
//
//  Queue<FVector, const FVector> pq;
//  pq.init(candidates.elements, 0, 0, cmp_vec, &target);
//
//  // TODO(cvicentiu) error checking.
//  for (const FVectorRef &candidate : candidates)
//    pq.push(FVector::get_fvector_from_source(source, vect_field, candidate));
//
//  for (size_t i = 0; i < max_neighbour_connections; i++)
//  {
//    if (!pq.elements())
//      break;
//    neighbours->push_back(pq.pop());
//  }
//
//  return false;
//}


static void dbug_print_vec_ref(const char *prefix,
                               uint layer,
                               const FVectorRef &ref)
{
#ifndef DBUG_OFF
  // TODO(cvicentiu) disable this in release build.
  char *ref_str= (char *)alloca(ref.get_ref_len() * 2 + 1);
  DBUG_ASSERT(ref_str);
  char *ptr= ref_str;
  for (size_t i = 0; i < ref.get_ref_len(); ptr += 2, i++)
  {
    snprintf(ptr, 3, "%02x", ref.get_ref()[i]);
  }
  DBUG_PRINT("VECTOR", ("%s %u %s", prefix, layer, ref_str));
#endif
}

static void dbug_print_vec_neigh(uint layer,
                                 const List<FVectorRef> &neighbors)
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
  Hash_set<FVectorRef>::Iterator it(h);
  FVectorRef *ptr;
  while ((ptr = it++))
  {
    DBUG_PRINT("VECTOR", ("HASH elem: %p", ptr));
    dbug_print_vec_ref("VISITED: ", 0, *ptr);
  }
#endif
}


static bool write_neighbours(TABLE *graph,
                             size_t layer_number,
                             const FVectorRef &source_node,
                             const List<FVectorRef> &new_neighbours)
{
  DBUG_ASSERT(new_neighbours.elements <= HNSW_MAX_M);


  size_t total_size= sizeof(uint16_t) +
    new_neighbours.elements * source_node.get_ref_len();

  // Allocate memory for the struct and the flexible array member
  char *neighbor_array_bytes= static_cast<char *>(alloca(total_size));

  DBUG_ASSERT(new_neighbours.elements <= INT16_MAX);
  *(uint16_t *) neighbor_array_bytes= new_neighbours.elements;
  char *pos= neighbor_array_bytes + sizeof(uint16_t);
  for (const auto &node: new_neighbours)
  {
    DBUG_ASSERT(node.get_ref_len() == source_node.get_ref_len());
    memcpy(pos, node.get_ref(), node.get_ref_len());
    pos+= node.get_ref_len();
  }

  graph->field[0]->store(layer_number);
  graph->field[1]->store_binary(
    reinterpret_cast<const char *>(source_node.get_ref()),
    source_node.get_ref_len());
  graph->field[2]->set_null();


  uchar *key= (uchar*)alloca(graph->key_info->key_length);
  key_copy(key, graph->record[0], graph->key_info, graph->key_info->key_length);

  int err= graph->file->ha_index_read_map(graph->record[1], key,
                                          HA_WHOLE_KEY,
                                          HA_READ_KEY_EXACT);

  // no record
  if (err == HA_ERR_KEY_NOT_FOUND)
  {
    dbug_print_vec_ref("INSERT ", layer_number, source_node);
    graph->field[2]->store_binary(neighbor_array_bytes, total_size);
    graph->file->ha_write_row(graph->record[0]);
    return false;
  }
  dbug_print_vec_ref("UPDATE ", layer_number, source_node);
  dbug_print_vec_neigh(layer_number, new_neighbours);

  graph->field[2]->store_binary(neighbor_array_bytes, total_size);
  graph->file->ha_update_row(graph->record[1], graph->record[0]);
  return false;
}


static bool get_neighbours(TABLE *graph,
                           size_t layer_number,
                           const FVectorRef &source_node,
                           List<FVectorRef> *neighbours)
{
  // TODO(cvicentiu) This allocation need not happen in this function.
  uchar *key= (uchar*)alloca(graph->key_info->key_length);

  graph->field[0]->store(layer_number);
  graph->field[1]->store_binary(
    reinterpret_cast<const char *>(source_node.get_ref()),
    source_node.get_ref_len());
  graph->field[2]->set_null();
  key_copy(key, graph->record[0],
           graph->key_info, graph->key_info->key_length);
  if ((graph->file->ha_index_read_map(graph->record[0], key,
                                      HA_WHOLE_KEY,
                                      HA_READ_KEY_EXACT)))
    return true;

  //TODO This does two memcpys, one should use str's buffer.
  String strbuf;
  String *str= graph->field[2]->val_str(&strbuf);

  // All ref should have same length
  uint ref_length= source_node.get_ref_len();

  const uchar *neigh_arr_bytes= reinterpret_cast<const uchar *>(str->ptr());
  uint16_t number_of_neighbours=
    *reinterpret_cast<const uint16_t*>(neigh_arr_bytes);
  if (number_of_neighbours != (str->length() - sizeof(uint16_t)) / ref_length)
  {
    /*
      neighbours number does not match the data length,
      should not happen, possible corrupted HNSW index
    */
    DBUG_ASSERT(0); // TODO(cvicentiu) remove this after testing.
    return true;
  }

  const uchar *pos = neigh_arr_bytes + sizeof(uint16_t);
  for (uint16_t i= 0; i < number_of_neighbours; i++)
  {
    neighbours->push_back(FVector::get_fvector_ref(pos, ref_length));
    pos+= ref_length;
  }

  return false;
}


static bool update_second_degree_neighbors(TABLE *source,
                                           Field *vec_field,
                                           TABLE *graph,
                                           size_t layer_number,
                                           uint max_neighbours,
                                           const FVectorRef &source_node,
                                           const List<FVectorRef> &neighbours)
{
  //dbug_print_vec_ref("Updating second degree neighbours", layer_number, source_node);
  //dbug_print_vec_neigh(layer_number, neighbours);
  for (const FVectorRef &neigh: neighbours)
  {
    List<FVectorRef> new_neighbours;
    get_neighbours(graph, layer_number, neigh, &new_neighbours);
    new_neighbours.push_back(&source_node);
    write_neighbours(graph, layer_number, neigh, new_neighbours);
  }

  for (const FVectorRef &neigh: neighbours)
  {
    List<FVectorRef> new_neighbours;
    get_neighbours(graph, layer_number, neigh, &new_neighbours);
    // TODO(cvicentiu) get_fvector_from_source results must not need to be freed.
    FVector *neigh_vec = FVector::get_fvector_from_source(source, vec_field, neigh);

    if (new_neighbours.elements > max_neighbours)
    {
      // shrink the neighbours
      List<FVectorRef> selected;
      select_neighbours(source, graph, vec_field, layer_number,
                        *neigh_vec, new_neighbours,
                        max_neighbours, &selected);
      write_neighbours(graph, layer_number, neigh, selected);
    }

    // release memory
    new_neighbours.empty();
  }

  return false;
}


static bool update_neighbours(TABLE *source,
                              TABLE *graph,
                              Field *vec_field,
                              size_t layer_number,
                              uint max_neighbours,
                              const FVectorRef &source_node,
                              const List<FVectorRef> &neighbours)
{
  // 1. update node's neighbours
  write_neighbours(graph, layer_number, source_node, neighbours);
  // 2. update node's neighbours' neighbours (shrink before update)
  update_second_degree_neighbors(source, vec_field, graph, layer_number,
                                 max_neighbours, source_node, neighbours);
  return false;
}


static bool search_layer(TABLE *source,
                         TABLE *graph,
                         Field *vec_field,
                         const FVector &target,
                         const List<FVectorRef> &start_nodes,
                         uint max_candidates_return,
                         size_t layer,
                         List<FVectorRef> *result)
{
  DBUG_ASSERT(start_nodes.elements > 0);
  // Result list must be empty, otherwise there's a risk of memory leak
  DBUG_ASSERT(result->elements == 0);

  Queue<FVector, const FVector> candidates;
  Queue<FVector, const FVector> best;
  //TODO(cvicentiu) Fix this hash method.
  Hash_set<FVectorRef> visited(PSI_INSTRUMENT_MEM, &my_charset_bin,
                               1000, 0, 0,
                               (my_hash_get_key)FVectorRef::get_key,
                               NULL,
                               HASH_UNIQUE);

  candidates.init(10000, false, cmp_vec, &target);
  best.init(max_candidates_return, true, cmp_vec, &target);

  for (const FVectorRef &node : start_nodes)
  {
    FVector *v= FVector::get_fvector_from_source(source, vec_field, node);
    candidates.push(v);
    if (best.elements() < max_candidates_return)
      best.push(v);
    else if (target.distance_to(*v) > target.distance_to(*best.top())) {
      best.replace_top(v);
    }
    visited.insert(v);
    dbug_print_vec_ref("INSERTING node in visited: ", layer, node);
  }

  double furthest_best = target.distance_to(*best.top());
  while (candidates.elements())
  {
    const FVector &cur_vec= *candidates.pop();
    double cur_distance = target.distance_to(cur_vec);
    if (cur_distance > furthest_best && best.elements() == max_candidates_return)
    {
      break; // All possible candidates are worse than what we have.
             // Can't get better.
    }

    List<FVectorRef> neighbours;
    get_neighbours(graph, layer, cur_vec, &neighbours);

    for (const FVectorRef &neigh: neighbours)
    {
      dbug_print_hash_vec(visited);
      if (visited.find(&neigh))
        continue;

      FVector *clone = FVector::get_fvector_from_source(source, vec_field, neigh);
      // TODO(cvicentiu) mem ownershipw...
      visited.insert(clone);
      if (best.elements() < max_candidates_return)
      {
        candidates.push(clone);
        best.push(clone);
        furthest_best = target.distance_to(*best.top());
      }
      else if (target.distance_to(*clone) < furthest_best)
      {
        best.replace_top(clone);
        candidates.push(clone);
        furthest_best = target.distance_to(*best.top());
      }
    }
    neighbours.empty();
  }
  DBUG_PRINT("VECTOR", ("SEARCH_LAYER_END %d best", best.elements()));

  while (best.elements())
  {
    // TODO(cvicentiu) FVector memory leak.
    // TODO(cvicentiu) this is n*log(n), we need a queue iterator.
    result->push_front(best.pop());
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
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_VECTOR);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(vec_field->binary());
  DBUG_ASSERT(vec_field->cmp_type() == STRING_RESULT);
  DBUG_ASSERT(res); // ER_INDEX_CANNOT_HAVE_NULL
  DBUG_ASSERT(h->ref_length <= graph->field[1]->field_length);
  DBUG_ASSERT(h->ref_length <= graph->field[2]->field_length);

  if (res->length() == 0 || res->length() % 4)
    return 1;

  const double NORMALIZATION_FACTOR = 1 / std::log(1.0 *
                                                   table->in_use->variables.hnsw_max_connection_per_layer);

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
    write_neighbours(graph, 0, {h->ref, h->ref_length}, {});

    h->ha_rnd_end();
    graph->file->ha_index_end();
    return 0; // TODO (error during store_link)
  }
  else
    max_layer= graph->field[0]->val_int();

  FVector target;
  h->position(table->record[0]);
  // TODO (cvicentiu) Error checking.
  target.init(h->ref, h->ref_length,
              reinterpret_cast<const float *>(res->ptr()),
              res->length() / sizeof(float));


  std::uniform_real_distribution<> dis(0.0, 1.0);
  double new_num= dis(gen);
  double log= -std::log(new_num) * NORMALIZATION_FACTOR;
  longlong new_node_layer= std::floor(log);

  List<FVectorRef> start_nodes;

  String ref_str, *ref_ptr;
  ref_ptr= graph->field[1]->val_str(&ref_str);

  FVectorRef start_node_ref{(uchar *)ref_ptr->ptr(), ref_ptr->length()};
  //FVector *start_node= start_node_ref.get_fvector_from_source(table, vec_field);

  // TODO(cvicentiu) error checking. Also make sure we use a random start node
  // in last layer.
  start_nodes.push_back(&start_node_ref);
  // TODO start_nodes needs to have one element in it.
  for (longlong cur_layer= max_layer; cur_layer > new_node_layer; cur_layer--)
  {
    List<FVectorRef> candidates;
    search_layer(table, graph, vec_field, target, start_nodes,
                 table->in_use->variables.hnsw_ef_constructor, cur_layer,
                 &candidates);
    start_nodes.empty();
    start_nodes.push_back(candidates.head());
    //candidates.delete_elements();
    //TODO(cvicentiu) memory leak
  }

  for (longlong cur_layer= std::min(max_layer, new_node_layer);
       cur_layer >= 0; cur_layer--)
  {
    List<FVectorRef> candidates;
    List<FVectorRef> neighbours;
    search_layer(table, graph, vec_field, target, start_nodes,
                 table->in_use->variables.hnsw_ef_constructor,
                 cur_layer, &candidates);
    // release vectors
    start_nodes.empty();

    uint max_neighbours= (cur_layer == 0) ?
     table->in_use->variables.hnsw_max_connection_per_layer * 2
     : table->in_use->variables.hnsw_max_connection_per_layer;

    select_neighbours(table, graph, vec_field, cur_layer,
                      target, candidates,
                      max_neighbours, &neighbours);
    update_neighbours(table, graph, vec_field, cur_layer, max_neighbours,
                      target, neighbours);
    start_nodes= candidates;
  }
  start_nodes.empty();

  for (longlong cur_layer= max_layer + 1; cur_layer <= new_node_layer;
       cur_layer++)
  {
    write_neighbours(graph, cur_layer, target, {});
  }


  h->ha_rnd_end();
  graph->file->ha_index_end();
  dbug_tmp_restore_column_map(&table->read_set, old_map);

  return err == HA_ERR_END_OF_FILE ? 0 : err;
}


int mhnsw_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit)
{
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  // TODO(cvicentiu) onlye one hlindex now.
  Field *vec_field= keyinfo->key_part->field;
  Item_func_vec_distance *fun= (Item_func_vec_distance *)dist;
  String buf, *res= fun->arguments()[1]->val_str(&buf);
  handler *h= table->file;

  //TODO(scope_exit)
  int err;
  if ((err= h->ha_rnd_init(0)))
    return err;

  if ((err= graph->file->ha_index_init(0, 1)))
    return err;

  h->position(table->record[0]);
  FVector target;
  target.init(h->ref,
              h->ref_length,
              reinterpret_cast<const float *>(res->ptr()),
              res->length() / sizeof(float));

  List<FVectorRef> candidates;
  List<FVectorRef> start_nodes;

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
  FVectorRef start_node_ref{(uchar *)ref_ptr->ptr(), ref_ptr->length()};
  // TODO(cvicentiu) error checking. Also make sure we use a random start node
  // in last layer.
  start_nodes.push_back(&start_node_ref);

  ulonglong ef_search= MY_MAX(
    table->in_use->variables.hnsw_ef_search, limit);

  for (size_t cur_layer= max_layer; cur_layer > 0; cur_layer--)
  {
    search_layer(table, graph, vec_field, target, start_nodes, ef_search,
                 cur_layer, &candidates);
    start_nodes.empty();
    //start_nodes.delete_elements();
    start_nodes.push_back(candidates.head());
    //candidates.delete_elements();
    candidates.empty();
    //TODO(cvicentiu) memleak.
  }

  search_layer(table, graph, vec_field, target, start_nodes,
               ef_search, 0, &candidates);

  // 8. return results
  FVectorRef **context= (FVectorRef**)table->in_use->alloc(
    sizeof(FVectorRef*) * (limit + 1));
  graph->context= context;

  FVectorRef **ptr= context;
  while (limit--)
    *ptr++= candidates.pop();
  *ptr= nullptr;

  err= mhnsw_next(table);
  graph->file->ha_index_end();

  // TODO release vectors after query

  dbug_tmp_restore_column_map(&table->read_set, old_map);
  return err;
}

int mhnsw_next(TABLE *table)
{
  FVectorRef ***context= (FVectorRef ***)&table->hlindex->context;
  FVectorRef *cur_vec= **context;
  if (cur_vec)
  {
    int err= table->file->ha_rnd_pos(table->record[0],
                                     (uchar *)(cur_vec)->get_ref());
    // release vectors
    // delete cur_vec;

    (*context)++;
    return err;
  }
  return HA_ERR_END_OF_FILE;
}
