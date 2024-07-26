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
#include "key.h"                                // key_copy()
#include "vector_mhnsw.h"
#include "item_vectorfunc.h"
#include <scope.h>
#include "bloom_filters.h"

ulonglong mhnsw_cache_size;

#define clo_nei_size 4
#define clo_nei_store float4store
#define clo_nei_read  float4get

// Algorithm parameters
static constexpr double alpha = 1.1;
static constexpr double generosity = 1.2;
static constexpr double stiffness = 0.002;
static constexpr uint ef_construction_max_factor= 16;
static constexpr uint clo_nei_threshold= 10000;

enum Graph_table_fields {
  FIELD_LAYER, FIELD_TREF, FIELD_VEC, FIELD_NEIGHBORS
};
enum Graph_table_indices {
  IDX_TREF, IDX_LAYER
};

class MHNSW_Context;
class FVectorNode;

/*
  One vector, an array of coordinates in ctx->vec_len dimensions
*/
#pragma pack(push, 1)
struct FVector
{
  float abs2, scale;
  int16_t dims[4];

  static constexpr size_t header= sizeof(float)*2;
  static constexpr size_t SIMD_bytes= 256/8;
  static constexpr size_t SIMD_dims= SIMD_bytes/sizeof(int16_t);

  static constexpr size_t alloc_size(size_t n)
  { return header + MY_ALIGN(n*2, SIMD_bytes) + SIMD_bytes - 1; }

  static constexpr size_t data_size(size_t n)
  { return header + n*2; }

  static FVector *align_ptr(void *ptr)
  { return (FVector*)(MY_ALIGN(((intptr)ptr) + header, SIMD_bytes) - header); }

  void fix_tail(size_t vec_len)
  { bzero(dims + vec_len, (MY_ALIGN(vec_len, SIMD_dims) - vec_len)*2); }

  static const FVector *create(void *mem, const void *src, size_t src_len)
  {
    FVector *vec= align_ptr(mem);
    float abs2= 0, scale=0, *v= (float *)src;
    size_t vec_len= src_len / sizeof(float);
    for (size_t i= 0; i < vec_len; i++)
    {
      abs2+= v[i]*v[i];
      if (std::abs(scale) < std::abs(v[i]))
        scale= v[i];
    }
    vec->abs2= abs2/2;
    vec->scale= scale ? scale/32767 : 1;
    for (size_t i= 0; i < vec_len; i++)
      vec->dims[i] = std::round(v[i] / vec->scale);
    vec->fix_tail(vec_len);
    return vec;
  }

#ifdef INTEL_SIMD_IMPLEMENTATION
  INTEL_SIMD_IMPLEMENTATION
  static float dot_product(const int16_t *v1, const int16_t *v2, size_t len)
  {
    typedef float v8f __attribute__((vector_size(SIMD_bytes)));
    union { v8f v; __m256 i; } tmp;
    __m256i *p1= (__m256i*)v1;
    __m256i *p2= (__m256i*)v2;
    v8f d= {0};
    for (size_t i= 0; i < len/SIMD_dims; p1++, p2++, i++)
    {
      tmp.i= _mm256_cvtepi32_ps(_mm256_madd_epi16(*p1, *p2));
      d+= tmp.v;
    }
    return d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7];
  }
#endif

  DEFAULT_IMPLEMENTATION
  static float dot_product(const int16_t *v1, const int16_t *v2, size_t len)
  {
    int64_t d= 0;
    for (size_t i= 0; i < len; i++)
      d+= int32_t(v1[i]) * int32_t(v2[i]);
    return d;
  }

  float distance_to(const FVector *other, size_t vec_len) const
  {
    return abs2 + other->abs2 - scale * other->scale *
           dot_product(dims, other->dims, MY_ALIGN(vec_len, SIMD_dims));
  }
};
#pragma pack(pop)

/*
  An array of pointers to graph nodes

  It's mainly used to store all neighbors of a given node on a given layer.
  Additionally it stores the distance to the closest neighbor.

  An array is fixed size, 2*M for the zero layer, M for other layers
  see MHNSW_Context::max_neighbors().

  Number of neighbors is zero-padded to multiples of 8 (for SIMD Bloom filter).

  Also used as a simply array of nodes in search_layer, the array size
  then is defined by ef or efConstruction.
*/
struct Neighborhood: public Sql_alloc
{
  FVectorNode **links;
  size_t num;
  float closest;
  void empty() { closest= FLT_MAX; num=0; }
  FVectorNode **init(FVectorNode **ptr, size_t n)
  {
    empty();
    links= ptr;
    n= MY_ALIGN(n, 8);
    bzero(ptr, n*sizeof(*ptr));
    return ptr + n;
  }
};


/*
  One node in a graph = one row in the graph table

  stores a vector itself, ref (= position) in the graph (= hlindex)
  table, a ref in the main table, and an array of Neighborhood's, one
  per layer.

  It's lazily initialized, may know only gref, everything else is
  loaded on demand.

  On the other hand, on INSERT the new node knows everything except
  gref - which only becomes known after ha_write_row.

  Allocated on memroot in two chunks. One is the same size for all nodes
  and stores FVectorNode object, gref, tref, and vector. The second
  stores neighbors, all Neighborhood's together, its size depends
  on the number of layers this node is on.

  There can be millions of nodes in the cache and the cache size
  is constrained by mhnsw_cache_size, so every byte matters here
*/
#pragma pack(push, 1)
class FVectorNode
{
private:
  MHNSW_Context *ctx;

  const FVector *make_vec(const void *v);
  int alloc_neighborhood(size_t layer);
public:
  const FVector *vec= nullptr;
  Neighborhood *neighbors= nullptr;
  uint8_t max_layer;
  bool stored:1, deleted:1;

  FVectorNode(MHNSW_Context *ctx_, const void *gref_);
  FVectorNode(MHNSW_Context *ctx_, const void *tref_, size_t layer,
              const void *vec_);
  float distance_to(const FVector *other) const;
  int load(TABLE *graph);
  int load_from_record(TABLE *graph);
  int save(TABLE *graph);
  size_t tref_len() const;
  size_t gref_len() const;
  uchar *gref() const;
  uchar *tref() const;
  void push_neighbor(size_t layer, float dist, FVectorNode *v);

  static uchar *get_key(const FVectorNode *elem, size_t *key_len, my_bool);
};
#pragma pack(pop)

/*
  Shared algorithm context. The graph.

  Stored in TABLE_SHARE and on TABLE_SHARE::mem_root.
  Stores the complete graph in MHNSW_Context::root,
  The mapping gref->FVectorNode is in the node_cache.
  Both root and node_cache are protected by a cache_lock, but it's
  needed when loading nodes and is not used when the whole graph is in memory.
  Graph can be traversed concurrently by different threads, as traversal
  changes neither nodes nor the ctx.
  Nodes can be loaded concurrently by different threads, this is protected
  by a partitioned node_lock.
  reference counter allows flushing the graph without interrupting
  concurrent searches.
  MyISAM automatically gets exclusive write access because of the TL_WRITE,
  but InnoDB has to use a dedicated ctx->commit_lock for that
*/
class MHNSW_Context : public Sql_alloc
{
  std::atomic<uint> refcnt;
  std::atomic<double> ef_power; // for the bloom filter size heuristic
  std::atomic<uint> ef_construction;
  mysql_mutex_t cache_lock;
  mysql_mutex_t node_lock[8];

  void cache_internal(FVectorNode *node)
  {
    DBUG_ASSERT(node->stored);
    node_cache.insert(node);
  }
  void *alloc_node_internal()
  {
    return alloc_root(&root, sizeof(FVectorNode) + gref_len + tref_len
                      + FVector::alloc_size(vec_len));
  }

protected:
  MEM_ROOT root;
  Hash_set<FVectorNode> node_cache{PSI_INSTRUMENT_MEM, FVectorNode::get_key};

public:
  mysql_rwlock_t commit_lock;
  size_t vec_len= 0;
  size_t byte_len= 0;
  FVectorNode *start= 0;
  const uint tref_len;
  const uint gref_len;
  const uint M;

  MHNSW_Context(TABLE *t)
    : tref_len(t->file->ref_length),
      gref_len(t->hlindex->file->ref_length),
      M(t->in_use->variables.mhnsw_max_edges_per_node)
  {
    mysql_rwlock_init(PSI_INSTRUMENT_ME, &commit_lock);
    mysql_mutex_init(PSI_INSTRUMENT_ME, &cache_lock, MY_MUTEX_INIT_FAST);
    for (uint i=0; i < array_elements(node_lock); i++)
      mysql_mutex_init(PSI_INSTRUMENT_ME, node_lock + i, MY_MUTEX_INIT_SLOW);
    init_alloc_root(PSI_INSTRUMENT_MEM, &root, 1024*1024, 0, MYF(0));
    set_ef_power(0.6);
    set_ef_construction(0);
    refcnt.store(0, std::memory_order_relaxed);
  }

  virtual ~MHNSW_Context()
  {
    free_root(&root, MYF(0));
    mysql_rwlock_destroy(&commit_lock);
    mysql_mutex_destroy(&cache_lock);
    for (size_t i=0; i < array_elements(node_lock); i++)
      mysql_mutex_destroy(node_lock + i);
  }

  uint lock_node(FVectorNode *ptr)
  {
    ulong nr1= 1, nr2= 4;
    my_hash_sort_bin(0, (uchar*)&ptr, sizeof(ptr), &nr1, &nr2);
    uint ticket= nr1 % array_elements(node_lock);
    mysql_mutex_lock(node_lock + ticket);
    return ticket;
  }

  void unlock_node(uint ticket)
  {
    mysql_mutex_unlock(node_lock + ticket);
  }

  double get_ef_power()
  {
    return ef_power.load(std::memory_order_relaxed);
  }

  void set_ef_power(double x)
  {
    if (x > get_ef_power()) // not atomic, but it doesn't matter
      ef_power.store(x, std::memory_order_relaxed);
  }

  uint get_ef_construction()
  {
    return ef_construction.load(std::memory_order_relaxed);
  }

  void set_ef_construction(uint x)
  {
    x= std::min(std::max(x, M), M*ef_construction_max_factor); // safety
    ef_construction.store(x, std::memory_order_relaxed);
  }

  uint max_neighbors(size_t layer) const
  {
    return (layer ? 1 : 2) * M; // heuristic from the paper
  }

  void set_lengths(size_t len)
  {
    byte_len= len;
    vec_len= len / sizeof(float);
  }

  static int acquire(MHNSW_Context **ctx, TABLE *table, bool for_update);
  static MHNSW_Context *get_from_share(TABLE_SHARE *share, TABLE *table);

  virtual void reset(TABLE_SHARE *share)
  {
    mysql_mutex_lock(&share->LOCK_share);
    if (static_cast<MHNSW_Context*>(share->hlindex->hlindex_data) == this)
    {
      share->hlindex->hlindex_data= nullptr;
      --refcnt;
    }
    mysql_mutex_unlock(&share->LOCK_share);
  }

  void release(TABLE *table)
  {
    return release(table->file->has_transactions(), table->s);
  }

  virtual void release(bool can_commit, TABLE_SHARE *share)
  {
    if (can_commit)
      mysql_rwlock_unlock(&commit_lock);
    if (root_size(&root) > mhnsw_cache_size)
      reset(share);
    if (--refcnt == 0)
      this->~MHNSW_Context(); // XXX reuse
  }

  FVectorNode *get_node(const void *gref)
  {
    mysql_mutex_lock(&cache_lock);
    FVectorNode *node= node_cache.find(gref, gref_len);
    if (!node)
    {
      node= new (alloc_node_internal()) FVectorNode(this, gref);
      cache_internal(node);
    }
    mysql_mutex_unlock(&cache_lock);
    return node;
  }

  /* used on INSERT, gref isn't known, so cannot cache the node yet */
  void *alloc_node()
  {
    mysql_mutex_lock(&cache_lock);
    auto p= alloc_node_internal();
    mysql_mutex_unlock(&cache_lock);
    return p;
  }

  /* explicitly cache the node after alloc_node() */
  void cache_node(FVectorNode *node)
  {
    mysql_mutex_lock(&cache_lock);
    cache_internal(node);
    mysql_mutex_unlock(&cache_lock);
  }

  /* find the node without creating, only used on merging trx->ctx */
  FVectorNode *find_node(const void *gref)
  {
    mysql_mutex_lock(&cache_lock);
    FVectorNode *node= node_cache.find(gref, gref_len);
    mysql_mutex_unlock(&cache_lock);
    return node;
  }

  void *alloc_neighborhood(size_t max_layer)
  {
    mysql_mutex_lock(&cache_lock);
    auto p= alloc_root(&root, sizeof(Neighborhood)*(max_layer+1) +
             sizeof(FVectorNode*)*(MY_ALIGN(M, 4)*2 + MY_ALIGN(M,8)*max_layer));
    mysql_mutex_unlock(&cache_lock);
    return p;
  }
};

/*
  This is a non-shared context that exists within one transaction.

  At the end of the transaction it's either discarded (on rollback)
  or merged into the shared ctx (on commit).

  trx's are stored in thd->ha_data[] in a single-linked list,
  one instance of trx per TABLE_SHARE and allocated on the
  thd->transaction->mem_root
*/
class MHNSW_Trx : public MHNSW_Context
{
public:
  TABLE_SHARE *table_share;
  bool list_of_nodes_is_lost= false;
  MHNSW_Trx *next= nullptr;

  MHNSW_Trx(TABLE *table) : MHNSW_Context(table), table_share(table->s) {}
  void reset(TABLE_SHARE *) override
  {
    node_cache.clear();
    free_root(&root, MYF(0));
    start= 0;
    list_of_nodes_is_lost= true;
  }
  void release(bool, TABLE_SHARE *) override
  {
    if (root_size(&root) > mhnsw_cache_size)
      reset(nullptr);
  }

  static MHNSW_Trx *get_from_thd(THD *thd, TABLE *table);

  // it's okay in a transaction-local cache, there's no concurrent access
  Hash_set<FVectorNode> &get_cache() { return node_cache; }

  /* fake handlerton to use thd->ha_data and to get notified of commits */
  static struct MHNSW_hton : public handlerton
  {
    MHNSW_hton()
    {
      db_type= DB_TYPE_HLINDEX_HELPER;
      flags = HTON_NOT_USER_SELECTABLE | HTON_HIDDEN;
      savepoint_offset= 0;
      savepoint_set= [](handlerton *, THD *, void *){ return 0; };
      savepoint_rollback_can_release_mdl= [](handlerton *, THD *){ return true; };
      savepoint_rollback= do_savepoint_rollback;
      commit= do_commit;
      rollback= do_rollback;
    }
    static int do_commit(handlerton *, THD *thd, bool);
    static int do_rollback(handlerton *, THD *thd, bool);
    static int do_savepoint_rollback(handlerton *, THD *thd, void *);
  } hton;
};

MHNSW_Trx::MHNSW_hton MHNSW_Trx::hton;

int MHNSW_Trx::MHNSW_hton::do_savepoint_rollback(handlerton *, THD *thd, void *)
{
  for (auto trx= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &hton));
       trx; trx= trx->next)
    trx->reset(nullptr);
  return 0;
}

int MHNSW_Trx::MHNSW_hton::do_rollback(handlerton *, THD *thd, bool)
{
  MHNSW_Trx *trx_next;
  for (auto trx= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &hton));
       trx; trx= trx_next)
  {
    trx_next= trx->next;
    trx->~MHNSW_Trx();
  }
  thd_set_ha_data(current_thd, &hton, nullptr);
  return 0;
}

int MHNSW_Trx::MHNSW_hton::do_commit(handlerton *, THD *thd, bool)
{
  MHNSW_Trx *trx_next;
  for (auto trx= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &hton));
       trx; trx= trx_next)
  {
    trx_next= trx->next;
    auto ctx= MHNSW_Context::get_from_share(trx->table_share, nullptr);
    if (ctx)
    {
      mysql_rwlock_wrlock(&ctx->commit_lock);
      if (trx->list_of_nodes_is_lost)
        ctx->reset(trx->table_share);
      else
      {
        // consider copying nodes from trx to shared cache when it makes sense
        // for ann_benchmarks it does not
        // also, consider flushing only changed nodes (a flag in the node)
        for (FVectorNode &from : trx->get_cache())
          if (FVectorNode *node= ctx->find_node(from.gref()))
            node->vec= nullptr;
        ctx->start= nullptr;
      }
      if (ctx->get_ef_construction() < trx->get_ef_construction())
        ctx->set_ef_construction(trx->get_ef_construction());
      ctx->release(true, trx->table_share);
    }
    trx->~MHNSW_Trx();
  }
  thd_set_ha_data(current_thd, &hton, nullptr);
  return 0;
}

MHNSW_Trx *MHNSW_Trx::get_from_thd(THD *thd, TABLE *table)
{
  auto trx= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &hton));
  while (trx && trx->table_share != table->s) trx= trx->next;
  if (!trx)
  {
    trx= new (&thd->transaction->mem_root) MHNSW_Trx(table);
    trx->next= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &hton));
    thd_set_ha_data(thd, &hton, trx);
    // XXX copy ef_construction from MHNSW_Context
    if (!trx->next)
    {
      bool all= thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
      trans_register_ha(thd, all, &hton, 0);
    }
  }
  return trx;
}

MHNSW_Context *MHNSW_Context::get_from_share(TABLE_SHARE *share, TABLE *table)
{
  mysql_mutex_lock(&share->LOCK_share);
  auto ctx= static_cast<MHNSW_Context*>(share->hlindex->hlindex_data);
  if (!ctx && table)
  {
    ctx= new (&share->hlindex->mem_root) MHNSW_Context(table);
    share->hlindex->hlindex_data= ctx;
    ctx->refcnt++;
  }
  if (ctx)
    ctx->refcnt++;
  mysql_mutex_unlock(&share->LOCK_share);
  return ctx;
}

int MHNSW_Context::acquire(MHNSW_Context **ctx, TABLE *table, bool for_update)
{
  TABLE *graph= table->hlindex;
  THD *thd= table->in_use;

  if (table->file->has_transactions() &&
       (for_update || thd_get_ha_data(thd, &MHNSW_Trx::hton)))
    *ctx= MHNSW_Trx::get_from_thd(thd, table);
  else
  {
    *ctx= MHNSW_Context::get_from_share(table->s, table);
    if (table->file->has_transactions())
      mysql_rwlock_rdlock(&(*ctx)->commit_lock);
  }

  if ((*ctx)->start)
    return 0;

  if (int err= graph->file->ha_index_init(IDX_LAYER, 1))
    return err;

  int err= graph->file->ha_index_last(graph->record[0]);
  graph->file->ha_index_end();
  if (err)
    return err;

  graph->file->position(graph->record[0]);
  (*ctx)->set_lengths((graph->field[FIELD_VEC]->value_length()-8)*2);
  (*ctx)->start= (*ctx)->get_node(graph->file->ref);
  return (*ctx)->start->load_from_record(graph);
}

/* copy the vector, preprocessed as needed */
const FVector *FVectorNode::make_vec(const void *v)
{
  return FVector::create(tref() + tref_len(), v, ctx->byte_len);
}

FVectorNode::FVectorNode(MHNSW_Context *ctx_, const void *gref_)
  : ctx(ctx_), stored(true), deleted(false)
{
  memcpy(gref(), gref_, gref_len());
}

FVectorNode::FVectorNode(MHNSW_Context *ctx_, const void *tref_, size_t layer,
                         const void *vec_)
  : ctx(ctx_), stored(false), deleted(false)
{
  DBUG_ASSERT(tref_);
  memset(gref(), 0xff, gref_len()); // important: larger than any real gref
  memcpy(tref(), tref_, tref_len());
  vec= make_vec(vec_);

  alloc_neighborhood(layer);
}

float FVectorNode::distance_to(const FVector *other) const
{
  return vec->distance_to(other, ctx->vec_len);
}

int FVectorNode::alloc_neighborhood(size_t layer)
{
  if (neighbors)
    return 0;
  max_layer= layer;
  neighbors= (Neighborhood*)ctx->alloc_neighborhood(layer);
  auto ptr= (FVectorNode**)(neighbors + (layer+1));
  for (size_t i= 0; i <= layer; i++)
    ptr= neighbors[i].init(ptr, ctx->max_neighbors(i));
  return 0;
}

int FVectorNode::load(TABLE *graph)
{
  if (likely(vec))
    return 0;

  DBUG_ASSERT(stored);
  // trx: consider loading nodes from shared, when it makes sense
  // for ann_benchmarks it does not
  if (int err= graph->file->ha_rnd_pos(graph->record[0], gref()))
    return err;
  return load_from_record(graph);
}

int FVectorNode::load_from_record(TABLE *graph)
{
  DBUG_ASSERT(ctx->byte_len);

  uint ticket= ctx->lock_node(this);
  SCOPE_EXIT([this, ticket](){ ctx->unlock_node(ticket); });

  if (vec)
    return 0;

  String buf, *v= graph->field[FIELD_TREF]->val_str(&buf);
  deleted= graph->field[FIELD_TREF]->is_null();
  if (!deleted)
  {
    if (unlikely(v->length() != tref_len()))
      return my_errno= HA_ERR_CRASHED;
    memcpy(tref(), v->ptr(), v->length());
  }

  v= graph->field[FIELD_VEC]->val_str(&buf);
  if (unlikely(!v))
    return my_errno= HA_ERR_CRASHED;

  if (v->length() != FVector::data_size(ctx->vec_len))
    return my_errno= HA_ERR_CRASHED;
  FVector *vec_ptr= FVector::align_ptr(tref() + tref_len());
  memcpy(vec_ptr, v->ptr(), v->length());
  vec_ptr->fix_tail(ctx->vec_len);

  size_t layer= graph->field[FIELD_LAYER]->val_int();
  if (layer > 100) // 10e30 nodes at M=2, more at larger M's
    return my_errno= HA_ERR_CRASHED;

  if (int err= alloc_neighborhood(layer))
    return err;

  v= graph->field[FIELD_NEIGHBORS]->val_str(&buf);
  if (unlikely(!v))
    return my_errno= HA_ERR_CRASHED;

  // <N> <closest distance> <gref> <gref> ... <N> <closest distance> ...etc...
  uchar *ptr= (uchar*)v->ptr(), *end= ptr + v->length();
  for (size_t i=0; i <= max_layer; i++)
  {
    if (unlikely(ptr >= end))
      return my_errno= HA_ERR_CRASHED;
    size_t grefs= *ptr++;
    if (unlikely(ptr + clo_nei_size + grefs * gref_len() > end))
      return my_errno= HA_ERR_CRASHED;
    clo_nei_read(neighbors[i].closest, ptr);
    ptr+= clo_nei_size;
    neighbors[i].num= grefs;
    for (size_t j=0; j < grefs; j++, ptr+= gref_len())
      neighbors[i].links[j]= ctx->get_node(ptr);
  }
  vec= vec_ptr; // must be done at the very end
  return 0;
}

/* note that "closest" relation is asymmetric! */
void FVectorNode::push_neighbor(size_t layer, float dist, FVectorNode *other)
{
  DBUG_ASSERT(neighbors[layer].num < ctx->max_neighbors(layer));
  neighbors[layer].links[neighbors[layer].num++]= other;
  if (memcmp(gref(), other->gref(), gref_len()) < 0 &&
      neighbors[layer].closest > dist)
    neighbors[layer].closest= dist;
}

size_t FVectorNode::tref_len() const { return ctx->tref_len; }
size_t FVectorNode::gref_len() const { return ctx->gref_len; }
uchar *FVectorNode::gref() const { return (uchar*)(this+1); }
uchar *FVectorNode::tref() const { return gref() + gref_len(); }

uchar *FVectorNode::get_key(const FVectorNode *elem, size_t *key_len, my_bool)
{
  *key_len= elem->gref_len();
  return elem->gref();
}

/* one visited node during the search. caches the distance to target */
struct Visited : public Sql_alloc
{
  FVectorNode *node;
  const float distance_to_target;
  bool expand;
  Visited(FVectorNode *n, float d, bool e= false)
    : node(n), distance_to_target(d), expand(e) {}
  static int cmp(void *, const Visited* a, const Visited *b)
  {
    return a->distance_to_target < b->distance_to_target ? -1 :
           a->distance_to_target > b->distance_to_target ?  1 : 0;
  }
};

/*
  a factory to create Visited and keep track of already seen nodes

  note that PatternedSimdBloomFilter works in blocks of 8 elements,
  so on insert they're accumulated in nodes[], on search the caller
  provides 8 addresses at once. we record 0x0 as "seen" so that
  the caller could pad the input with nullptr's
*/
class VisitedSet
{
  MEM_ROOT *root;
  const FVector *target;
  PatternedSimdBloomFilter<FVectorNode> map;
  const FVectorNode *nodes[8]= {0,0,0,0,0,0,0,0};
  size_t idx= 1; // to record 0 in the filter
  public:
  uint count= 0;
  VisitedSet(MEM_ROOT *root, const FVector *target, uint size) :
    root(root), target(target), map(size, 0.01) {}
  Visited *create(FVectorNode *node, bool e= false)
  {
    auto *v= new (root) Visited(node, node->distance_to(target), e);
    insert(node);
    count++;
    return v;
  }
  void insert(const FVectorNode *n)
  {
    nodes[idx++]= n;
    if (idx == 8) flush();
  }
  void flush() {
    if (idx) map.Insert(nodes);
    idx=0;
  }
  uint8_t seen(FVectorNode **nodes) { return map.Query(nodes); }
};


/*
  selects best neighbors from the list of candidates plus one extra candidate

  one extra candidate is specified separately to avoid appending it to
  the Neighborhood candidates, which might be already at its max size.
*/
static int select_neighbors(MHNSW_Context *ctx, TABLE *graph, size_t layer,
                            FVectorNode &target, const Neighborhood &candidates,
                            FVectorNode *extra_candidate,
                            size_t max_neighbor_connections)
{
  Queue<Visited> pq; // working queue

  if (pq.init(10000, false, Visited::cmp))
    return my_errno= HA_ERR_OUT_OF_MEM;

  MEM_ROOT * const root= graph->in_use->mem_root;
  auto discarded= (Visited**)my_safe_alloca(sizeof(Visited**)*max_neighbor_connections);
  size_t discarded_num= 0;
  Neighborhood &neighbors= target.neighbors[layer];
  const bool do_cn= max_neighbor_connections*ctx->vec_len > clo_nei_threshold;

  for (size_t i=0; i < candidates.num; i++)
  {
    FVectorNode *node= candidates.links[i];
    if (int err= node->load(graph))
      return err;
    pq.push(new (root) Visited(node, node->distance_to(target.vec)));
  }
  if (extra_candidate)
    pq.push(new (root) Visited(extra_candidate, extra_candidate->distance_to(target.vec)));

  DBUG_ASSERT(pq.elements());
  neighbors.empty();

  while (pq.elements() && neighbors.num < max_neighbor_connections)
  {
    Visited *vec= pq.pop();
    FVectorNode * const node= vec->node;
    const float target_dista= vec->distance_to_target / alpha;
    bool discard= false;
    if (do_cn)
      discard= node->neighbors[layer].closest < target_dista;
    else
    {
      for (size_t i=0; i < neighbors.num; i++)
        if ((discard= node->distance_to(neighbors.links[i]->vec) < target_dista))
          break;
    }
    if (!discard)
      target.push_neighbor(layer, vec->distance_to_target, node);
    else if (discarded_num + neighbors.num < max_neighbor_connections)
      discarded[discarded_num++]= vec;
  }

  for (size_t i=0; i < discarded_num && neighbors.num < max_neighbor_connections; i++)
    target.push_neighbor(layer, discarded[i]->distance_to_target, discarded[i]->node);

  my_safe_afree(discarded, sizeof(Visited**)*max_neighbor_connections);
  return 0;
}


int FVectorNode::save(TABLE *graph)
{
  DBUG_ASSERT(vec);
  DBUG_ASSERT(neighbors);

  graph->field[FIELD_LAYER]->store(max_layer, false);
  if (deleted)
    graph->field[FIELD_TREF]->set_null();
  else
  {
    graph->field[FIELD_TREF]->set_notnull();
    graph->field[FIELD_TREF]->store_binary(tref(), tref_len());
  }
  graph->field[FIELD_VEC]->store_binary((uchar*)vec, FVector::data_size(ctx->vec_len));

  size_t total_size= 0;
  for (size_t i=0; i <= max_layer; i++)
    total_size+= 1 + clo_nei_size + gref_len() * neighbors[i].num;

  uchar *neighbor_blob= static_cast<uchar *>(my_safe_alloca(total_size));
  uchar *ptr= neighbor_blob;
  for (size_t i= 0; i <= max_layer; i++)
  {
    *ptr++= (uchar)(neighbors[i].num);
    clo_nei_store(ptr, neighbors[i].closest);
    ptr+= clo_nei_size;
    for (size_t j= 0; j < neighbors[i].num; j++, ptr+= gref_len())
      memcpy(ptr, neighbors[i].links[j]->gref(), gref_len());
  }
  graph->field[FIELD_NEIGHBORS]->store_binary(neighbor_blob, total_size);

  int err;
  if (stored)
  {
    if (!(err= graph->file->ha_rnd_pos(graph->record[1], gref())))
    {
      err= graph->file->ha_update_row(graph->record[1], graph->record[0]);
      if (err == HA_ERR_RECORD_IS_THE_SAME)
        err= 0;
    }
  }
  else
  {
    err= graph->file->ha_write_row(graph->record[0]);
    graph->file->position(graph->record[0]);
    memcpy(gref(), graph->file->ref, gref_len());
    stored= true;
    ctx->cache_node(this);
  }
  my_safe_afree(neighbor_blob, total_size);
  return err;
}

static int update_second_degree_neighbors(MHNSW_Context *ctx, TABLE *graph,
                                          size_t layer, FVectorNode *node)
{
  const uint max_neighbors= ctx->max_neighbors(layer);
  // it seems that one could update nodes in the gref order
  // to avoid InnoDB deadlocks, but it produces no noticeable effect
  for (size_t i=0; i < node->neighbors[layer].num; i++)
  {
    FVectorNode *neigh= node->neighbors[layer].links[i];
    Neighborhood &neighneighbors= neigh->neighbors[layer];
    if (neighneighbors.num < max_neighbors)
      neigh->push_neighbor(layer, neigh->distance_to(node->vec), node);
    else
      if (int err= select_neighbors(ctx, graph, layer, *neigh, neighneighbors,
                                    node, max_neighbors))
        return err;
    if (int err= neigh->save(graph))
      return err;
  }
  return 0;
}

static int search_layer(MHNSW_Context *ctx, TABLE *graph, const FVector *target,
                        Neighborhood *start_nodes, uint result_size,
                        size_t layer, Neighborhood *result, bool construction)
{
  DBUG_ASSERT(start_nodes->num > 0);
  result->empty();

  MEM_ROOT * const root= graph->in_use->mem_root;
  Queue<Visited> candidates, best;
  bool skip_deleted;
  uint ef= result_size, expand_size= 0;

  if (construction)
  {
    skip_deleted= false;
    if (ef > 1)
    {
      uint efc= std::max(ctx->get_ef_construction(), ef);
      // round down efc/2 to 2^n-1
      expand_size=  (my_round_up_to_next_power((efc >> 1) + 2) - 1) >> 1;
      ef= efc + expand_size;
    }
  }
  else
  {
    skip_deleted= layer == 0;
    if (ef > 1 || layer == 0)
      ef= ef * graph->in_use->variables.mhnsw_limit_multiplier;
  }

  // WARNING! heuristic here
  const double est_heuristic= 8 * std::sqrt(ctx->max_neighbors(layer));
  const uint est_size= est_heuristic * std::pow(ef, ctx->get_ef_power());
  VisitedSet visited(root, target, est_size);

  candidates.init(10000, false, Visited::cmp);
  best.init(ef, true, Visited::cmp);

  DBUG_ASSERT(start_nodes->num <= result_size);
  for (size_t i=0; i < start_nodes->num; i++)
  {
    Visited *v= visited.create(start_nodes->links[i]);
    candidates.push(v);
    if (skip_deleted && v->node->deleted)
      continue;
    best.push(v);
  }

  float furthest_best= FLT_MAX;
  while (candidates.elements())
  {
    const Visited &cur= *candidates.pop();
    if (cur.distance_to_target > furthest_best && best.is_full())
      break; // All possible candidates are worse than what we have

    visited.flush();

    Neighborhood &neighbors= cur.node->neighbors[layer];
    FVectorNode **links= neighbors.links, **end= links + neighbors.num;
    for (; links < end; links+= 8)
    {
      uint8_t res= visited.seen(links);
      if (res == 0xff)
        continue;

      for (size_t i= 0; i < 8; i++)
      {
        if (res & (1 << i))
          continue;
        if (int err= links[i]->load(graph))
          return err;
        Visited *v= visited.create(links[i], cur.expand);
        if (!best.is_full())
        {
          candidates.push(v);
          if (skip_deleted && v->node->deleted)
            continue;
          best.push(v);
          furthest_best= best.top()->distance_to_target * generosity;
        }
        else if (v->distance_to_target < furthest_best)
        {
          candidates.safe_push(v);
          if (skip_deleted && v->node->deleted)
            continue;
          if ((generosity > 1 &&
               v->distance_to_target >= best.top()->distance_to_target)
              || best.replace_top(v) <= expand_size)
            v->expand= true;
          furthest_best= best.top()->distance_to_target * generosity;
        }
      }
    }
  }
  if (ef > 1 && visited.count*2 > est_size)
    ctx->set_ef_power(std::log(visited.count*2/est_heuristic) / std::log(ef));

  while (best.elements() > result_size)
    best.pop();

  uint expanded= 0;
  result->num= best.elements();
  for (FVectorNode **links= result->links + result->num; best.elements();)
  {
    expanded+= best.top()->expand;
    *--links= best.pop()->node;
  }

  if (expanded && expanded > stiffness*expand_size*result_size) // Hooke's law
    ctx->set_ef_construction(ef);
  else if (expand_size)
    ctx->set_ef_construction(ef - expand_size - 1); // decrease slowly

  return 0;
}


static int bad_value_on_insert(Field *f)
{
  my_error(ER_TRUNCATED_WRONG_VALUE_FOR_FIELD, MYF(0), "vector", "...",
           f->table->s->db.str, f->table->s->table_name.str, f->field_name.str,
           f->table->in_use->get_stmt_da()->current_row_for_warning());
  return my_errno= HA_ERR_GENERIC;
}


int mhnsw_insert(TABLE *table, KEY *keyinfo)
{
  THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  Field *vec_field= keyinfo->key_part->field;
  String buf, *res= vec_field->val_str(&buf);
  MHNSW_Context *ctx;

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_MHNSW);
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

  table->file->position(table->record[0]);

  int err= MHNSW_Context::acquire(&ctx, table, true);
  SCOPE_EXIT([ctx, table](){ ctx->release(table); });
  if (err)
  {
    if (err != HA_ERR_END_OF_FILE)
      return err;

    // First insert!
    ctx->set_lengths(res->length());
    FVectorNode *target= new (ctx->alloc_node())
                   FVectorNode(ctx, table->file->ref, 0, res->ptr());
    if (!((err= target->save(graph))))
      ctx->start= target;
    return err;
  }

  if (ctx->byte_len != res->length())
    return bad_value_on_insert(vec_field);

  const size_t max_found= ctx->max_neighbors(0);
  Neighborhood candidates, start_nodes;
  candidates.init(thd->alloc<FVectorNode*>(max_found + 7), max_found);
  start_nodes.init(thd->alloc<FVectorNode*>(max_found + 7), max_found);
  start_nodes.links[start_nodes.num++]= ctx->start;

  const double NORMALIZATION_FACTOR= 1 / std::log(ctx->M);
  double log= -std::log(my_rnd(&thd->rand)) * NORMALIZATION_FACTOR;
  const longlong max_layer= start_nodes.links[0]->max_layer;
  longlong target_layer= std::min<longlong>(std::floor(log), max_layer + 1);
  longlong cur_layer;

  FVectorNode *target= new (ctx->alloc_node())
                 FVectorNode(ctx, table->file->ref, target_layer, res->ptr());

  if (int err= graph->file->ha_rnd_init(0))
    return err;
  SCOPE_EXIT([graph](){ graph->file->ha_rnd_end(); });

  for (cur_layer= max_layer; cur_layer > target_layer; cur_layer--)
  {
    if (int err= search_layer(ctx, graph, target->vec, &start_nodes, 1,
                              cur_layer, &candidates, false))
      return err;
    std::swap(start_nodes, candidates);
  }

  for (; cur_layer >= 0; cur_layer--)
  {
    uint max_neighbors= ctx->max_neighbors(cur_layer);
    if (int err= search_layer(ctx, graph, target->vec, &start_nodes,
                              max_neighbors, cur_layer, &candidates, true))
      return err;

    if (int err= select_neighbors(ctx, graph, cur_layer, *target, candidates,
                                  0, max_neighbors))
      return err;
    std::swap(start_nodes, candidates);
  }

  if (int err= target->save(graph))
    return err;

  if (target_layer > max_layer)
    ctx->start= target;

  for (cur_layer= target_layer; cur_layer >= 0; cur_layer--)
  {
    if (int err= update_second_degree_neighbors(ctx, graph, cur_layer, target))
      return err;
  }

  dbug_tmp_restore_column_map(&table->read_set, old_map);

  return 0;
}


int mhnsw_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit)
{
  THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  Item_func_vec_distance *fun= (Item_func_vec_distance *)dist;
  String buf, *res= fun->get_const_arg()->val_str(&buf);
  MHNSW_Context *ctx;

  if (int err= table->file->ha_rnd_init(0))
    return err;

  if (int err= MHNSW_Context::acquire(&ctx, table, false))
    return err;
  SCOPE_EXIT([ctx, table](){ ctx->release(table); });

  Neighborhood candidates, start_nodes;
  candidates.init(thd->alloc<FVectorNode*>(limit + 7), limit);
  start_nodes.init(thd->alloc<FVectorNode*>(limit + 7), limit);

  // one could put all max_layer nodes in start_nodes
  // but it has no effect of the recall or speed
  start_nodes.links[start_nodes.num++]= ctx->start;

  /*
    if the query vector is NULL or invalid, VEC_DISTANCE will return
    NULL, so the result is basically unsorted, we can return rows
    in any order. Let's use some hardcoded value here
  */
  if (!res || ctx->byte_len != res->length())
  {
    res= &buf;
    buf.alloc(ctx->byte_len);
    for (size_t i=0; i < ctx->vec_len; i++)
      ((float*)buf.ptr())[i]= i == 0;
  }

  const longlong max_layer= start_nodes.links[0]->max_layer;
  auto target= FVector::create(thd->alloc(FVector::alloc_size(ctx->vec_len)),
                               res->ptr(), res->length());

  if (int err= graph->file->ha_rnd_init(0))
    return err;
  SCOPE_EXIT([graph](){ graph->file->ha_rnd_end(); });

  for (size_t cur_layer= max_layer; cur_layer > 0; cur_layer--)
  {
    if (int err= search_layer(ctx, graph, target, &start_nodes, 1, cur_layer,
                              &candidates, false))
      return err;
    std::swap(start_nodes, candidates);
  }

  if (int err= search_layer(ctx, graph, target, &start_nodes, limit, 0,
                            &candidates, false))
    return err;

  if (limit > candidates.num)
    limit= candidates.num;
  size_t context_size=limit * ctx->tref_len + sizeof(ulonglong);
  char *context= thd->alloc(context_size);
  graph->context= context;

  *(ulonglong*)context= limit;
  context+= context_size;

  for (size_t i=0; limit--; i++)
  {
    context-= ctx->tref_len;
    memcpy(context, candidates.links[i]->tref(), ctx->tref_len);
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
  return my_errno= HA_ERR_END_OF_FILE;
}

void mhnsw_free(TABLE_SHARE *share)
{
  TABLE_SHARE *graph_share= share->hlindex;
  if (!graph_share->hlindex_data)
    return;

  static_cast<MHNSW_Context*>(graph_share->hlindex_data)->~MHNSW_Context();
  graph_share->hlindex_data= 0;
}

int mhnsw_invalidate(TABLE *table, const uchar *rec, KEY *keyinfo)
{
  TABLE *graph= table->hlindex;
  handler *h= table->file;
  MHNSW_Context *ctx;
  bool use_ctx= !MHNSW_Context::acquire(&ctx, table, true);

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_MHNSW);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(h->ref_length <= graph->field[FIELD_TREF]->field_length);

  // target record:
  h->position(rec);
  graph->field[FIELD_TREF]->set_notnull();
  graph->field[FIELD_TREF]->store_binary(h->ref, h->ref_length);

  uchar *key= (uchar*)alloca(graph->key_info[IDX_TREF].key_length);
  key_copy(key, graph->record[0], &graph->key_info[IDX_TREF],
           graph->key_info[IDX_TREF].key_length);

  if (int err= graph->file->ha_index_read_idx_map(graph->record[1], IDX_TREF,
                                        key, HA_WHOLE_KEY, HA_READ_KEY_EXACT))
   return err;

  restore_record(graph, record[1]);
  graph->field[FIELD_TREF]->set_null();
  if (int err= graph->file->ha_update_row(graph->record[1], graph->record[0]))
    return err;

  if (use_ctx)
  {
    graph->file->position(graph->record[0]);
    FVectorNode *node= ctx->get_node(graph->file->ref);
    node->deleted= true;
    ctx->release(table);
  }

  return 0;
}

int mhnsw_delete_all(TABLE *table, KEY *keyinfo)
{
  TABLE *graph= table->hlindex;

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_MHNSW);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);

  if (int err= graph->file->ha_delete_all_rows())
   return err;

  MHNSW_Context *ctx;
  if (!MHNSW_Context::acquire(&ctx, table, true))
  {
    ctx->reset(table->s);
    ctx->release(table);
  }

  return 0;
}

const LEX_CSTRING mhnsw_hlindex_table_def(THD *thd, uint ref_length)
{
  const char templ[]="CREATE TABLE i (                   "
                     "  layer tinyint not null,          "
                     "  tref varbinary(%u),              "
                     "  vec blob not null,               "
                     "  neighbors blob not null,         "
                     "  unique (tref),                   "
                     "  key (layer))                     ";
  size_t len= sizeof(templ) + 32;
  char *s= thd->alloc(len);
  len= my_snprintf(s, len, templ, ref_length);
  return {s, len};
}
