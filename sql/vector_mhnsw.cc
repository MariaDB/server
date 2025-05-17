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
#include "create_options.h"
#include "table_cache.h"
#include "vector_mhnsw.h"
#include <scope.h>
#include <my_atomic_wrapper.h>
#include "bloom_filters.h"

// distance can be a little bit < 0 because of fast math
static constexpr float NEAREST = -1.0f;

// Algorithm parameters
static constexpr float alpha = 1.1f;
static constexpr uint ef_construction= 10;
static constexpr uint max_ef= 10000;

static ulonglong mhnsw_max_cache_size;
static MYSQL_SYSVAR_ULONGLONG(max_cache_size, mhnsw_max_cache_size,
       PLUGIN_VAR_RQCMDARG, "Upper limit for one MHNSW vector index cache",
       nullptr, nullptr, 16*1024*1024, 1024*1024, SIZE_T_MAX, 1);
static MYSQL_THDVAR_UINT(ef_search, PLUGIN_VAR_RQCMDARG,
       "Larger values mean slower SELECTs but more accurate results. "
       "Defines the minimal number of result candidates to look for in the "
       "vector index for ORDER BY ... LIMIT N queries. The search will never "
       "search for less rows than that, even if LIMIT is smaller",
       nullptr, nullptr, 20, 1, max_ef, 1);
static MYSQL_THDVAR_UINT(default_m, PLUGIN_VAR_RQCMDARG,
       "Larger values mean slower SELECTs and INSERTs, larger index size "
       "and higher memory consumption but more accurate results",
       nullptr, nullptr, 6, 3, 200, 1);

enum metric_type : uint { EUCLIDEAN, COSINE };
static const char *distance_names[]= { "euclidean", "cosine", nullptr };
static TYPELIB distances= CREATE_TYPELIB_FOR(distance_names);
static MYSQL_THDVAR_ENUM(default_distance, PLUGIN_VAR_RQCMDARG,
       "Distance function to build the vector index for",
       nullptr, nullptr, EUCLIDEAN, &distances);

struct ha_index_option_struct
{
  ulonglong M; // option struct does not support uint
  metric_type metric;
};

enum Graph_table_fields {
  FIELD_LAYER, FIELD_TREF, FIELD_VEC, FIELD_NEIGHBORS
};
enum Graph_table_indices {
  IDX_TREF, IDX_LAYER
};

class MHNSW_Share;
class FVectorNode;

/*
  One vector, an array of coordinates in ctx->vec_len dimensions
*/
#pragma pack(push, 1)
struct FVector
{
  static constexpr size_t data_header= sizeof(float);
  static constexpr size_t alloc_header= data_header + sizeof(float);

  float abs2, scale;
  int16_t dims[4];

  uchar *data() const { return (uchar*)(&scale); }

  static size_t data_size(size_t n)
  { return data_header + n*2; }

  static size_t data_to_value_size(size_t data_size)
  { return (data_size - data_header)*2; }

  static const FVector *create(metric_type metric, void *mem, const void *src, size_t src_len)
  {
    float scale=0, *v= (float *)src;
    size_t vec_len= src_len / sizeof(float);
    for (size_t i= 0; i < vec_len; i++)
      if (std::abs(scale) < std::abs(get_float(v + i)))
        scale= get_float(v + i);

    FVector *vec= align_ptr(mem);
    vec->scale= scale ? scale/32767 : 1;
    for (size_t i= 0; i < vec_len; i++)
      vec->dims[i] = static_cast<int16_t>(std::round(get_float(v + i) / vec->scale));
    vec->postprocess(vec_len);
    if (metric == COSINE)
    {
      if (vec->abs2 > 0.0f)
        vec->scale/= std::sqrt(2*vec->abs2);
      vec->abs2= 0.5f;
    }
    return vec;
  }

  void postprocess(size_t vec_len)
  {
    fix_tail(vec_len);
    abs2= scale * scale * dot_product(dims, dims, vec_len) / 2;
  }

#ifdef AVX2_IMPLEMENTATION
  /************* AVX2 *****************************************************/
  static constexpr size_t AVX2_bytes= 256/8;
  static constexpr size_t AVX2_dims= AVX2_bytes/sizeof(int16_t);

  AVX2_IMPLEMENTATION
  static float dot_product(const int16_t *v1, const int16_t *v2, size_t len)
  {
    typedef float v8f __attribute__((vector_size(AVX2_bytes)));
    union { v8f v; __m256 i; } tmp;
    __m256i *p1= (__m256i*)v1;
    __m256i *p2= (__m256i*)v2;
    v8f d= {0};
    for (size_t i= 0; i < (len + AVX2_dims-1)/AVX2_dims; p1++, p2++, i++)
    {
      tmp.i= _mm256_cvtepi32_ps(_mm256_madd_epi16(*p1, *p2));
      d+= tmp.v;
    }
    return d[0] + d[1] + d[2] + d[3] + d[4] + d[5] + d[6] + d[7];
  }

  AVX2_IMPLEMENTATION
  static size_t alloc_size(size_t n)
  { return alloc_header + MY_ALIGN(n*2, AVX2_bytes) + AVX2_bytes - 1; }

  AVX2_IMPLEMENTATION
  static FVector *align_ptr(void *ptr)
  { return (FVector*)(MY_ALIGN(((intptr)ptr) + alloc_header, AVX2_bytes)
                      - alloc_header); }

  AVX2_IMPLEMENTATION
  void fix_tail(size_t vec_len)
  {
    bzero(dims + vec_len, (MY_ALIGN(vec_len, AVX2_dims) - vec_len)*2);
  }
#endif

#ifdef AVX512_IMPLEMENTATION
  /************* AVX512 ****************************************************/
  static constexpr size_t AVX512_bytes= 512/8;
  static constexpr size_t AVX512_dims= AVX512_bytes/sizeof(int16_t);

  AVX512_IMPLEMENTATION
  static float dot_product(const int16_t *v1, const int16_t *v2, size_t len)
  {
    __m512i *p1= (__m512i*)v1;
    __m512i *p2= (__m512i*)v2;
    __m512 d= _mm512_setzero_ps();
    for (size_t i= 0; i < (len + AVX512_dims-1)/AVX512_dims; p1++, p2++, i++)
      d= _mm512_add_ps(d, _mm512_cvtepi32_ps(_mm512_madd_epi16(*p1, *p2)));
    return _mm512_reduce_add_ps(d);
  }

  AVX512_IMPLEMENTATION
  static size_t alloc_size(size_t n)
  { return alloc_header + MY_ALIGN(n*2, AVX512_bytes) + AVX512_bytes - 1; }

  AVX512_IMPLEMENTATION
  static FVector *align_ptr(void *ptr)
  { return (FVector*)(MY_ALIGN(((intptr)ptr) + alloc_header, AVX512_bytes)
                      - alloc_header); }

  AVX512_IMPLEMENTATION
  void fix_tail(size_t vec_len)
  {
    bzero(dims + vec_len, (MY_ALIGN(vec_len, AVX512_dims) - vec_len)*2);
  }
#endif


/*
  ARM NEON implementation. A microbenchmark shows 1.7x dot_product() performance
  improvement compared to regular -O2/-O3 builds and 2.4x compared to builds
  with auto-vectorization disabled.

  There seem to be no performance difference between vmull+vmull_high and
  vmull+vmlal2_high implementations.
*/

#ifdef NEON_IMPLEMENTATION
  static constexpr size_t NEON_bytes= 128 / 8;
  static constexpr size_t NEON_dims= NEON_bytes / sizeof(int16_t);

  static float dot_product(const int16_t *v1, const int16_t *v2, size_t len)
  {
    int64_t d= 0;
    for (size_t i= 0; i < (len + NEON_dims - 1) / NEON_dims; i++)
    {
      int16x8_t p1= vld1q_s16(v1);
      int16x8_t p2= vld1q_s16(v2);
      d+= vaddlvq_s32(vmull_s16(vget_low_s16(p1), vget_low_s16(p2))) +
          vaddlvq_s32(vmull_high_s16(p1, p2));
      v1+= NEON_dims;
      v2+= NEON_dims;
    }
    return static_cast<float>(d);
  }

  static size_t alloc_size(size_t n)
  { return alloc_header + MY_ALIGN(n * 2, NEON_bytes) + NEON_bytes - 1; }

  static FVector *align_ptr(void *ptr)
  { return (FVector*) (MY_ALIGN(((intptr) ptr) + alloc_header, NEON_bytes)
                       - alloc_header); }

  void fix_tail(size_t vec_len)
  {
    bzero(dims + vec_len, (MY_ALIGN(vec_len, NEON_dims) - vec_len) * 2);
  }
#endif

#ifdef POWER_IMPLEMENTATION
  /************* POWERPC *****************************************************/
  static constexpr size_t POWER_bytes= 128 / 8; // Assume 128-bit vector width
  static constexpr size_t POWER_dims= POWER_bytes / sizeof(int16_t);

  static float dot_product(const int16_t *v1, const int16_t *v2, size_t len)
  {
    // Using vector long long for int64_t accumulation
    vector long long ll_sum= {0, 0};
    // Round up to process full vector, including padding
    size_t base= ((len + POWER_dims - 1) / POWER_dims) * POWER_dims;

    for (size_t i= 0; i < base; i+= POWER_dims)
    {
      vector short x= vec_ld(0, &v1[i]);
      vector short y= vec_ld(0, &v2[i]);

      // Vectorized multiplication using vec_mule() and vec_mulo()
      vector int product_hi= vec_mule(x, y);
      vector int product_lo= vec_mulo(x, y);

      // Extend vector int to vector long long for accumulation
      vector long long llhi1= vec_unpackh(product_hi);
      vector long long llhi2= vec_unpackl(product_hi);
      vector long long lllo1= vec_unpackh(product_lo);
      vector long long lllo2= vec_unpackl(product_lo);

      ll_sum+= llhi1 + llhi2 + lllo1 + lllo2;
    }

    return static_cast<float>(static_cast<int64_t>(ll_sum[0]) +
                              static_cast<int64_t>(ll_sum[1]));
  }

  static size_t alloc_size(size_t n)
  {
    return alloc_header + MY_ALIGN(n * 2, POWER_bytes) + POWER_bytes - 1;
  }

  static FVector *align_ptr(void *ptr)
  {
    return (FVector*)(MY_ALIGN(((intptr)ptr) + alloc_header, POWER_bytes)
                    - alloc_header);
  }

  void fix_tail(size_t vec_len)
  {
    bzero(dims + vec_len, (MY_ALIGN(vec_len, POWER_dims) - vec_len) * 2);
  }
#undef DEFAULT_IMPLEMENTATION
#endif

  /************* no-SIMD default ******************************************/
#ifdef DEFAULT_IMPLEMENTATION
  DEFAULT_IMPLEMENTATION
  static float dot_product(const int16_t *v1, const int16_t *v2, size_t len)
  {
    int64_t d= 0;
    for (size_t i= 0; i < len; i++)
      d+= int32_t(v1[i]) * int32_t(v2[i]);
    return static_cast<float>(d);
  }

  DEFAULT_IMPLEMENTATION
  static size_t alloc_size(size_t n) { return alloc_header + n*2; }

  DEFAULT_IMPLEMENTATION
  static FVector *align_ptr(void *ptr) { return (FVector*)ptr; }

  DEFAULT_IMPLEMENTATION
  void fix_tail(size_t) {  }
#endif

  float distance_to(const FVector *other, size_t vec_len) const
  {
    return abs2 + other->abs2 - scale * other->scale *
           dot_product(dims, other->dims, vec_len);
  }
};
#pragma pack(pop)

/*
  An array of pointers to graph nodes

  It's mainly used to store all neighbors of a given node on a given layer.

  An array is fixed size, 2*M for the zero layer, M for other layers
  see MHNSW_Share::max_neighbors().

  Number of neighbors is zero-padded to multiples of 8 (for SIMD Bloom filter).

  Also used as a simply array of nodes in search_layer, the array size
  then is defined by ef or efConstruction.
*/
struct Neighborhood: public Sql_alloc
{
  FVectorNode **links;
  size_t num;
  FVectorNode **init(FVectorNode **ptr, size_t n)
  {
    num= 0;
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
  is constrained by mhnsw_max_cache_size, so every byte matters here
*/
#pragma pack(push, 1)
class FVectorNode
{
private:
  MHNSW_Share *ctx;

  const FVector *make_vec(const void *v);
  int alloc_neighborhood(uint8_t layer);
public:
  const FVector *vec= nullptr;
  Neighborhood *neighbors= nullptr;
  uint8_t max_layer;
  bool stored:1, deleted:1;

  FVectorNode(MHNSW_Share *ctx_, const void *gref_);
  FVectorNode(MHNSW_Share *ctx_, const void *tref_, uint8_t layer,
              const void *vec_);
  float distance_to(const FVector *other) const;
  int load(TABLE *graph);
  int load_from_record(TABLE *graph);
  int save(TABLE *graph);
  size_t tref_len() const;
  size_t gref_len() const;
  uchar *gref() const;
  uchar *tref() const;
  void push_neighbor(size_t layer, FVectorNode *v);

  static const uchar *get_key(const void *elem, size_t *key_len, my_bool);
};
#pragma pack(pop)

/*
  Shared algorithm context. The graph.

  Stored in TABLE_SHARE and on TABLE_SHARE::mem_root.
  Stores the complete graph in MHNSW_Share::root,
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
class MHNSW_Share : public Sql_alloc
{
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
  std::atomic<uint> refcnt{0};
  MEM_ROOT root;
  Hash_set<FVectorNode> node_cache{PSI_INSTRUMENT_MEM, FVectorNode::get_key};

public:
  ulonglong version= 0;                 // protected by commit_lock
  mysql_rwlock_t commit_lock;
  size_t vec_len= 0;
  size_t byte_len= 0;
  Atomic_relaxed<double> ef_power{0.6}; // for the bloom filter size heuristic
  Atomic_relaxed<float>  diameter{0};   // for the generosity heuristic
  FVectorNode *start= 0;
  const uint tref_len;
  const uint gref_len;
  const uint M;
  metric_type metric;

  MHNSW_Share(TABLE *t)
    : tref_len(t->file->ref_length),
      gref_len(t->hlindex->file->ref_length),
      M(static_cast<uint>(t->s->key_info[t->s->keys].option_struct->M)),
      metric(t->s->key_info[t->s->keys].option_struct->metric)
  {
    mysql_rwlock_init(PSI_INSTRUMENT_ME, &commit_lock);
    mysql_mutex_init(PSI_INSTRUMENT_ME, &cache_lock, MY_MUTEX_INIT_FAST);
    for (uint i=0; i < array_elements(node_lock); i++)
      mysql_mutex_init(PSI_INSTRUMENT_ME, node_lock + i, MY_MUTEX_INIT_SLOW);
    init_alloc_root(PSI_INSTRUMENT_MEM, &root, 1024*1024, 0, MYF(0));
  }

  virtual ~MHNSW_Share()
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

  uint max_neighbors(size_t layer) const
  {
    return (layer ? 1 : 2) * M; // heuristic from the paper
  }

  void set_lengths(size_t len)
  {
    byte_len= len;
    vec_len= len / sizeof(float);
  }

  static int acquire(MHNSW_Share **ctx, TABLE *table, bool for_update);
  static MHNSW_Share *get_from_share(TABLE_SHARE *share, TABLE *table);

  virtual void reset(TABLE_SHARE *share)
  {
    share->lock_share();
    if (static_cast<MHNSW_Share*>(share->hlindex->hlindex_data) == this)
    {
      share->hlindex->hlindex_data= nullptr;
      --refcnt;
    }
    share->unlock_share();
  }

  void release(TABLE *table)
  {
    return release(table->file->has_transactions(), table->s);
  }

  virtual void release(bool can_commit, TABLE_SHARE *share)
  {
    if (can_commit)
      mysql_rwlock_unlock(&commit_lock);
    if (root_size(&root) > mhnsw_max_cache_size)
      reset(share);
    if (--refcnt == 0)
      this->~MHNSW_Share(); // XXX reuse
  }

  virtual MHNSW_Share *dup(bool can_commit)
  {
    refcnt++;
    if (can_commit)
      mysql_rwlock_rdlock(&commit_lock);
    return this;
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
class MHNSW_Trx : public MHNSW_Share
{
public:
  MDL_ticket *table_id;
  bool list_of_nodes_is_lost= false;
  MHNSW_Trx *next= nullptr;

  MHNSW_Trx(TABLE *table) : MHNSW_Share(table), table_id(table->mdl_ticket) {}
  void reset(TABLE_SHARE *) override
  {
    node_cache.clear();
    free_root(&root, MYF(0));
    start= 0;
    list_of_nodes_is_lost= true;
  }
  void release(bool, TABLE_SHARE *) override
  {
    if (--refcnt == 0 && root_size(&root) > mhnsw_max_cache_size)
      reset(nullptr);
  }

  virtual MHNSW_Share *dup(bool) override
  {
    refcnt++;
    return this;
  }

  static MHNSW_Trx *get_from_thd(TABLE *table, bool for_update);

  // it's okay in a transaction-local cache, there's no concurrent access
  Hash_set<FVectorNode> &get_cache() { return node_cache; }

  static transaction_participant tp;
  static int do_commit(THD *thd, bool);
  static int do_savepoint_rollback(THD *thd, void *);
  static int do_rollback(THD *thd, bool);
  static int do_prepare(THD *thd, bool);
};

struct transaction_participant MHNSW_Trx::tp=
{
  0, 0, 0,
  nullptr,                        /* close_connection */
  [](THD *, void *){ return 0; }, /* savepoint_set */
  MHNSW_Trx::do_savepoint_rollback,
  [](THD *thd){ return true; },   /*savepoint_rollback_can_release_mdl*/
  nullptr,                        /*savepoint_release*/
  MHNSW_Trx::do_commit, MHNSW_Trx::do_rollback,
  MHNSW_Trx::do_prepare,          /* prepare */
  nullptr,                        /* recover */
  nullptr, nullptr,               /* commit/rollback_by_xid */
  nullptr, nullptr,               /* recover_rollback_by_xid/recovery_done */
  nullptr, nullptr, nullptr,      /* snapshot, commit/prepare_ordered */
  nullptr, nullptr                /* checkpoint, versioned */
};

int MHNSW_Trx::do_savepoint_rollback(THD *thd, void *)
{
  for (auto trx= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &tp));
       trx; trx= trx->next)
    trx->reset(nullptr);
  return 0;
}

int MHNSW_Trx::do_rollback(THD *thd, bool)
{
  MHNSW_Trx *trx_next;
  for (auto trx= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &tp));
       trx; trx= trx_next)
  {
    trx_next= trx->next;
    trx->~MHNSW_Trx();
  }
  thd_set_ha_data(current_thd, &tp, nullptr);
  return 0;
}

int MHNSW_Trx::do_commit(THD *thd, bool)
{
  MHNSW_Trx *trx_next;
  for (auto trx= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &tp));
       trx; trx= trx_next)
  {
    trx_next= trx->next;
    if (trx->table_id)
    {
      const MDL_key *key= trx->table_id->get_key();
      LEX_CSTRING db=  {key->db_name(), key->db_name_length()},
                  tbl= {key->name(), key->name_length()};
      TABLE_LIST tl;
      tl.init_one_table(&db, &tbl, nullptr, TL_IGNORE);
      TABLE_SHARE *share= tdc_acquire_share(thd, &tl, GTS_TABLE, nullptr);
      if (share)
      {
        auto ctx= share->hlindex ? MHNSW_Share::get_from_share(share, nullptr)
                                 : nullptr;
        if (ctx)
        {
          mysql_rwlock_wrlock(&ctx->commit_lock);
          ctx->version++;
          if (trx->list_of_nodes_is_lost)
            ctx->reset(share);
          else
          {
            // consider copying nodes from trx to shared cache when it makes
            // sense. for ann_benchmarks it does not.
            // also, consider flushing only changed nodes (a flag in the node)
            for (FVectorNode &from : trx->get_cache())
              if (FVectorNode *node= ctx->find_node(from.gref()))
                node->vec= nullptr;
            ctx->start= nullptr;
          }
          ctx->release(true, share);
        }
        tdc_release_share(share);
      }
    }
    trx->~MHNSW_Trx();
  }
  thd_set_ha_data(current_thd, &tp, nullptr);
  return 0;
}

int MHNSW_Trx::do_prepare(THD *thd, bool)
{
  /* Explicit XA is not supported yet */
  return thd->transaction->xid_state.is_explicit_XA()
         ? HA_ERR_UNSUPPORTED : 0;
}

MHNSW_Trx *MHNSW_Trx::get_from_thd(TABLE *table, bool for_update)
{
  if (!table->file->has_transactions())
      return NULL;

  THD *thd= table->in_use;
  auto trx= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &tp));
  if (!for_update && !trx)
    return NULL;

  while (trx && trx->table_id != table->mdl_ticket) trx= trx->next;
  if (!trx)
  {
    trx= new (&thd->transaction->mem_root) MHNSW_Trx(table);
    trx->next= static_cast<MHNSW_Trx*>(thd_get_ha_data(thd, &tp));
    thd_set_ha_data(thd, &tp, trx);
    if (!trx->next)
    {
      bool all= thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
      trans_register_ha(thd, all, &tp, 0);
    }
  }
  trx->refcnt++;
  return trx;
}

MHNSW_Share *MHNSW_Share::get_from_share(TABLE_SHARE *share, TABLE *table)
{
  share->lock_share();
  auto ctx= static_cast<MHNSW_Share*>(share->hlindex->hlindex_data);
  if (!ctx && table)
  {
    ctx= new (&share->hlindex->mem_root) MHNSW_Share(table);
    if (!ctx) return nullptr;
    share->hlindex->hlindex_data= ctx;
    ctx->refcnt++;
  }
  if (ctx)
    ctx->refcnt++;
  share->unlock_share();
  return ctx;
}

int MHNSW_Share::acquire(MHNSW_Share **ctx, TABLE *table, bool for_update)
{
  TABLE *graph= table->hlindex;

  if (!(*ctx= MHNSW_Trx::get_from_thd(table, for_update)))
  {
    *ctx= MHNSW_Share::get_from_share(table->s, table);
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
  (*ctx)->set_lengths(FVector::data_to_value_size(graph->field[FIELD_VEC]->value_length()));

  auto node= (*ctx)->get_node(graph->file->ref);
  if ((err= node->load_from_record(graph)))
    return err;

  (*ctx)->start= node; // set the shared start only when node is fully loaded
  return 0;
}

/* copy the vector, preprocessed as needed */
const FVector *FVectorNode::make_vec(const void *v)
{
  return FVector::create(ctx->metric, tref() + tref_len(), v, ctx->byte_len);
}

FVectorNode::FVectorNode(MHNSW_Share *ctx_, const void *gref_)
  : ctx(ctx_), stored(true), deleted(false)
{
  memcpy(gref(), gref_, gref_len());
}

FVectorNode::FVectorNode(MHNSW_Share *ctx_, const void *tref_, uint8_t layer,
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

int FVectorNode::alloc_neighborhood(uint8_t layer)
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
  memcpy(vec_ptr->data(), v->ptr(), v->length());
  vec_ptr->postprocess(ctx->vec_len);

  longlong layer= graph->field[FIELD_LAYER]->val_int();
  if (layer > 100) // 10e30 nodes at M=2, more at larger M's
    return my_errno= HA_ERR_CRASHED;

  if (int err= alloc_neighborhood(static_cast<uint8_t>(layer)))
    return err;

  v= graph->field[FIELD_NEIGHBORS]->val_str(&buf);
  if (unlikely(!v))
    return my_errno= HA_ERR_CRASHED;

  // <N> <gref> <gref> ... <N> ...etc...
  uchar *ptr= (uchar*)v->ptr(), *end= ptr + v->length();
  for (size_t i=0; i <= max_layer; i++)
  {
    if (unlikely(ptr >= end))
      return my_errno= HA_ERR_CRASHED;
    size_t grefs= *ptr++;
    if (unlikely(ptr + grefs * gref_len() > end))
      return my_errno= HA_ERR_CRASHED;
    neighbors[i].num= grefs;
    for (size_t j=0; j < grefs; j++, ptr+= gref_len())
      neighbors[i].links[j]= ctx->get_node(ptr);
  }
  vec= vec_ptr; // must be done at the very end
  return 0;
}

void FVectorNode::push_neighbor(size_t layer, FVectorNode *other)
{
  DBUG_ASSERT(neighbors[layer].num < ctx->max_neighbors(layer));
  neighbors[layer].links[neighbors[layer].num++]= other;
}

size_t FVectorNode::tref_len() const { return ctx->tref_len; }
size_t FVectorNode::gref_len() const { return ctx->gref_len; }
uchar *FVectorNode::gref() const { return (uchar*)(this+1); }
uchar *FVectorNode::tref() const { return gref() + gref_len(); }

const uchar *FVectorNode::get_key(const void *elem, size_t *key_len, my_bool)
{
  *key_len= static_cast<const FVectorNode*>(elem)->gref_len();
  return static_cast<const FVectorNode*>(elem)->gref();
}

/* one visited node during the search. caches the distance to target */
struct Visited : public Sql_alloc
{
  FVectorNode *node;
  const float distance_to_target;
  Visited(FVectorNode *n, float d) : node(n), distance_to_target(d) {}
  static int cmp(void *, const void* a_, const void *b_)
  {
    const Visited *a= static_cast<const Visited*>(a_);
    const Visited *b= static_cast<const Visited*>(b_);
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
    root(root), target(target), map(size, 0.01f) {}
  Visited *create(FVectorNode *node)
  {
    auto *v= new (root) Visited(node, node->distance_to(target));
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
static int select_neighbors(MHNSW_Share *ctx, TABLE *graph, size_t layer,
                            FVectorNode &target, const Neighborhood &candidates,
                            FVectorNode *extra_candidate,
                            size_t max_neighbor_connections)
{
  Queue<Visited> pq; // working queue

  if (pq.init(max_ef, false, Visited::cmp))
    return my_errno= HA_ERR_OUT_OF_MEM;

  MEM_ROOT * const root= graph->in_use->mem_root;
  auto discarded= (Visited**)my_safe_alloca(sizeof(Visited**)*max_neighbor_connections);
  size_t discarded_num= 0;
  Neighborhood &neighbors= target.neighbors[layer];

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
  neighbors.num= 0;

  while (pq.elements() && neighbors.num < max_neighbor_connections)
  {
    Visited *vec= pq.pop();
    FVectorNode * const node= vec->node;
    const float target_dista= std::max(32*FLT_EPSILON, vec->distance_to_target / alpha);
    bool discard= false;
    for (size_t i=0; i < neighbors.num; i++)
      if ((discard= node->distance_to(neighbors.links[i]->vec) <= target_dista))
        break;
    if (!discard)
      target.push_neighbor(layer, node);
    else if (discarded_num + neighbors.num < max_neighbor_connections)
      discarded[discarded_num++]= vec;
  }

  for (size_t i=0; i < discarded_num && neighbors.num < max_neighbor_connections; i++)
    target.push_neighbor(layer, discarded[i]->node);

  my_safe_afree(discarded, sizeof(Visited**)*max_neighbor_connections);
  return 0;
}


int FVectorNode::save(TABLE *graph)
{
  DBUG_ASSERT(vec);
  DBUG_ASSERT(neighbors);

  restore_record(graph, s->default_values);
  graph->field[FIELD_LAYER]->store(max_layer, false);
  if (deleted)
    graph->field[FIELD_TREF]->set_null();
  else
  {
    graph->field[FIELD_TREF]->set_notnull();
    graph->field[FIELD_TREF]->store_binary(tref(), tref_len());
  }
  graph->field[FIELD_VEC]->store_binary(vec->data(), FVector::data_size(ctx->vec_len));

  size_t total_size= 0;
  for (size_t i=0; i <= max_layer; i++)
    total_size+= 1 + gref_len() * neighbors[i].num;

  uchar *neighbor_blob= static_cast<uchar *>(my_safe_alloca(total_size));
  uchar *ptr= neighbor_blob;
  for (size_t i= 0; i <= max_layer; i++)
  {
    *ptr++= (uchar)(neighbors[i].num);
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

static int update_second_degree_neighbors(MHNSW_Share *ctx, TABLE *graph,
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
      neigh->push_neighbor(layer, node);
    else
      if (int err= select_neighbors(ctx, graph, layer, *neigh, neighneighbors,
                                    node, max_neighbors))
        return err;
    if (int err= neigh->save(graph))
      return err;
  }
  return 0;
}


static inline float generous_furthest(const Queue<Visited> &q, float maxd, float g)
{
  float d0=maxd*g/2;
  float d= q.top()->distance_to_target;
  float k= 5;
  float x= (d-d0)/d0;
  float sigmoid= k*x/std::sqrt(1+(k*k-1)*x*x); // or any other sigmoid
  return d*(1 + (g - 1)/2 * (1 - sigmoid));
}

/*
  @param[in/out] inout    in: start nodes, out: result nodes
*/
static int search_layer(MHNSW_Share *ctx, TABLE *graph, const FVector *target,
                        float threshold, uint result_size,
                        size_t layer, Neighborhood *inout, bool construction)
{
  DBUG_ASSERT(inout->num > 0);

  MEM_ROOT * const root= graph->in_use->mem_root;
  Queue<Visited> candidates, best;
  bool skip_deleted;
  uint ef= result_size;
  float generosity= 1.1f + ctx->M/500.0f;

  if (construction)
  {
    skip_deleted= false;
    if (ef > 1)
      ef= std::max(ef_construction, ef);
  }
  else
  {
    skip_deleted= layer == 0;
    if (ef > 1 || layer == 0)
      ef= std::max(THDVAR(graph->in_use, ef_search), ef);
  }

  // WARNING! heuristic here
  const double est_heuristic= 8 * std::sqrt(ctx->max_neighbors(layer));
  const uint est_size= static_cast<uint>(est_heuristic * std::pow(ef, ctx->ef_power));
  VisitedSet visited(root, target, est_size);

  candidates.init(max_ef, false, Visited::cmp);
  best.init(ef, true, Visited::cmp);

  DBUG_ASSERT(inout->num <= result_size);
  float max_distance= ctx->diameter;
  for (size_t i=0; i < inout->num; i++)
  {
    Visited *v= visited.create(inout->links[i]);
    max_distance= std::max(max_distance, v->distance_to_target);
    candidates.push(v);
    if ((skip_deleted && v->node->deleted) || threshold > NEAREST)
      continue;
    best.push(v);
  }

  float furthest_best= best.is_empty() ? FLT_MAX
                       : generous_furthest(best, max_distance, generosity);
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
        Visited *v= visited.create(links[i]);
        if (v->distance_to_target <= threshold)
          continue;
        if (!best.is_full())
        {
          max_distance= std::max(max_distance, v->distance_to_target);
          candidates.safe_push(v);
          if (skip_deleted && v->node->deleted)
            continue;
          best.push(v);
          furthest_best= generous_furthest(best, max_distance, generosity);
        }
        else if (v->distance_to_target < furthest_best)
        {
          candidates.safe_push(v);
          if (skip_deleted && v->node->deleted)
            continue;
          if (v->distance_to_target < best.top()->distance_to_target)
          {
            best.replace_top(v);
            furthest_best= generous_furthest(best, max_distance, generosity);
          }
        }
      }
    }
  }
  set_if_bigger(ctx->diameter, max_distance); // not atomic, but it's ok
  if (ef > 1 && visited.count*2 > est_size)
  {
    double ef_power= std::log(visited.count*2/est_heuristic) / std::log(ef);
    set_if_bigger(ctx->ef_power, ef_power); // not atomic, but it's ok
  }

  while (best.elements() > result_size)
    best.pop();

  inout->num= best.elements();
  for (FVectorNode **links= inout->links + inout->num; best.elements();)
    *--links= best.pop()->node;

  return 0;
}


int mhnsw_insert(TABLE *table, KEY *keyinfo)
{
  THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  MY_BITMAP *old_map= dbug_tmp_use_all_columns(table, &table->read_set);
  Field *vec_field= keyinfo->key_part->field;
  String buf, *res= vec_field->val_str(&buf);
  MHNSW_Share *ctx;

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_VECTOR);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);
  DBUG_ASSERT(vec_field->binary());
  DBUG_ASSERT(vec_field->cmp_type() == STRING_RESULT);
  DBUG_ASSERT(res); // ER_INDEX_CANNOT_HAVE_NULL
  DBUG_ASSERT(table->file->ref_length <= graph->field[FIELD_TREF]->field_length);
  DBUG_ASSERT(res->length() > 0 && res->length() % 4 == 0);

  table->file->position(table->record[0]);

  int err= MHNSW_Share::acquire(&ctx, table, true);
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
    return my_errno= HA_ERR_CRASHED;

  MEM_ROOT_SAVEPOINT memroot_sv;
  root_make_savepoint(thd->mem_root, &memroot_sv);
  SCOPE_EXIT([memroot_sv](){ root_free_to_savepoint(&memroot_sv); });

  const size_t max_found= ctx->max_neighbors(0);
  Neighborhood candidates;
  candidates.init(thd->alloc<FVectorNode*>(max_found + 7), max_found);
  candidates.links[candidates.num++]= ctx->start;

  const double NORMALIZATION_FACTOR= 1 / std::log(ctx->M);
  double log= -std::log(my_rnd(&thd->rand)) * NORMALIZATION_FACTOR;
  const uint8_t max_layer= candidates.links[0]->max_layer;
  uint8_t target_layer= std::min<uint8_t>(static_cast<uint8_t>(std::floor(log)), max_layer + 1);
  int cur_layer;

  FVectorNode *target= new (ctx->alloc_node())
                 FVectorNode(ctx, table->file->ref, target_layer, res->ptr());

  if (int err= graph->file->ha_rnd_init(0))
    return err;
  SCOPE_EXIT([graph](){ graph->file->ha_rnd_end(); });

  for (cur_layer= max_layer; cur_layer > target_layer; cur_layer--)
  {
    if (int err= search_layer(ctx, graph, target->vec, NEAREST,
                              1, cur_layer, &candidates, false))
      return err;
  }

  for (; cur_layer >= 0; cur_layer--)
  {
    uint max_neighbors= ctx->max_neighbors(cur_layer);
    if (int err= search_layer(ctx, graph, target->vec, NEAREST,
                              max_neighbors, cur_layer, &candidates, true))
      return err;

    if (int err= select_neighbors(ctx, graph, cur_layer, *target, candidates,
                                  0, max_neighbors))
      return err;
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


struct Search_context: public Sql_alloc
{
  Neighborhood found;
  MHNSW_Share *ctx;
  const FVector *target;
  ulonglong ctx_version;
  size_t pos= 0;
  float threshold= NEAREST/2;
  Search_context(Neighborhood *n, MHNSW_Share *s, const FVector *v)
    : found(*n), ctx(s->dup(false)), target(v), ctx_version(ctx->version) {}
};


int mhnsw_read_first(TABLE *table, KEY *keyinfo, Item *dist, ulonglong limit)
{
  THD *thd= table->in_use;
  TABLE *graph= table->hlindex;
  auto *fun= static_cast<Item_func_vec_distance*>(dist->real_item());
  DBUG_ASSERT(fun);

  limit= std::min<ulonglong>(limit, max_ef);

  String buf, *res= fun->get_const_arg()->val_str(&buf);
  MHNSW_Share *ctx;

  if (int err= table->file->ha_rnd_init(0))
    return err;

  int err= MHNSW_Share::acquire(&ctx, table, false);
  SCOPE_EXIT([ctx, table](){ ctx->release(table); });
  if (err)
    return err;

  Neighborhood candidates;
  candidates.init(thd->alloc<FVectorNode*>(limit + 7), limit);

  // one could put all max_layer nodes in candidates
  // but it has no effect on the recall or speed
  candidates.links[candidates.num++]= ctx->start;

  /*
    if the query vector is NULL or invalid, VEC_DISTANCE will return
    NULL, so the result is basically unsorted, we can return rows
    in any order. Let's use some hardcoded value here
  */
  if (!res || ctx->byte_len != res->length())
  {
    res= &buf;
    buf.alloc(ctx->byte_len);
    buf.length(ctx->byte_len);
    for (size_t i=0; i < ctx->vec_len; i++)
      ((float*)buf.ptr())[i]= i == 0;
  }

  const longlong max_layer= candidates.links[0]->max_layer;
  auto target= FVector::create(ctx->metric, thd->alloc(FVector::alloc_size(ctx->vec_len)),
                               res->ptr(), res->length());

  if (int err= graph->file->ha_rnd_init(0))
    return err;

  for (size_t cur_layer= max_layer; cur_layer > 0; cur_layer--)
  {
    if (int err= search_layer(ctx, graph, target, NEAREST,
                              1, cur_layer, &candidates, false))
    {
      graph->file->ha_rnd_end();
      return err;
    }
  }

  if (int err= search_layer(ctx, graph, target, NEAREST,
                            static_cast<uint>(limit), 0, &candidates, false))
  {
    graph->file->ha_rnd_end();
    return err;
  }

  auto result= new (thd->mem_root) Search_context(&candidates, ctx, target);
  graph->context= result;

  return mhnsw_read_next(table);
}

int mhnsw_read_next(TABLE *table)
{
  auto result= static_cast<Search_context*>(table->hlindex->context);
  if (result->pos < result->found.num)
  {
    uchar *ref= result->found.links[result->pos++]->tref();
    return table->file->ha_rnd_pos(table->record[0], ref);
  }
  if (!result->found.num)
    return my_errno= HA_ERR_END_OF_FILE;

  TABLE *graph= table->hlindex;
  MHNSW_Share *ctx= result->ctx->dup(table->file->has_transactions());
  SCOPE_EXIT([&ctx, table](){ ctx->release(table); });

  if (ctx->version != result->ctx_version)
  {
    // oops, shared ctx was modified, need to switch to MHNSW_Trx
    MHNSW_Share *trx;
    graph->file->ha_rnd_end();
    int err= MHNSW_Share::acquire(&trx, table, true);
    SCOPE_EXIT([&trx, table](){ trx->release(table); });
    if (int err2= graph->file->ha_rnd_init(0))
      err= err ?  err :  err2;
    if (err)
      return err;
    for (size_t i=0; i < result->found.num; i++)
    {
      FVectorNode *node= trx->get_node(result->found.links[i]->gref());
      if (!node)
        return my_errno= HA_ERR_OUT_OF_MEM;
      if ((err= node->load(graph)))
        return err;
      result->found.links[i]= node;
    }
    ctx->release(false, table->s);      // release shared ctx
    result->ctx= trx;                   // replace it with trx
    result->ctx_version= trx->version;
    std::swap(trx, ctx);        // free shared ctx in this scope, keep trx
  }

  float new_threshold= result->found.links[result->found.num-1]->distance_to(result->target);

  if (int err= search_layer(ctx, graph, result->target, result->threshold,
                   static_cast<uint>(result->pos), 0, &result->found, false))
    return err;
  result->pos= 0;
  result->threshold= new_threshold + FLT_EPSILON;
  return mhnsw_read_next(table);
}

int mhnsw_read_end(TABLE *table)
{
  auto result= static_cast<Search_context*>(table->hlindex->context);
  result->ctx->release(false, table->s);
  table->hlindex->context= 0;
  table->hlindex->file->ha_rnd_end();
  return 0;
}

void mhnsw_free(TABLE_SHARE *share)
{
  TABLE_SHARE *graph_share= share->hlindex;
  if (!graph_share->hlindex_data)
    return;

  static_cast<MHNSW_Share*>(graph_share->hlindex_data)->~MHNSW_Share();
  graph_share->hlindex_data= 0;
}

int mhnsw_invalidate(TABLE *table, const uchar *rec, KEY *keyinfo)
{
  TABLE *graph= table->hlindex;
  handler *h= table->file;
  MHNSW_Share *ctx;

  int err= MHNSW_Share::acquire(&ctx, table, true);
  SCOPE_EXIT([ctx, table](){ ctx->release(table); });
  if (err)
    return err;

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_VECTOR);
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

  graph->file->position(graph->record[0]);
  FVectorNode *node= ctx->get_node(graph->file->ref);
  node->deleted= true;

  return 0;
}

int mhnsw_delete_all(TABLE *table, KEY *keyinfo, bool truncate)
{
  TABLE *graph= table->hlindex;

  /* metadata are checked on open */
  DBUG_ASSERT(graph);
  DBUG_ASSERT(keyinfo->algorithm == HA_KEY_ALG_VECTOR);
  DBUG_ASSERT(keyinfo->usable_key_parts == 1);

  if (int err= truncate ? graph->file->truncate()
                        : graph->file->delete_all_rows())
   return err;

  MHNSW_Share *ctx;
  if (!MHNSW_Share::acquire(&ctx, table, true))
  {
    ctx->reset(table->s);
    ctx->release(table);
  }

  return 0;
}

const LEX_CSTRING mhnsw_hlindex_table_def(THD *thd, uint ref_length)
{
  constexpr int max_ref_length= 256; // arbitrary limit < max key length
  if (ref_length > max_ref_length)
  {
    my_printf_error(ER_TOO_LONG_KEY, "Primary key was too long for vector "
                    "indexes, max length is %d bytes", MYF(0), max_ref_length);
    return { nullptr, 0 };
  }
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

Item_func_vec_distance::distance_kind mhnsw_uses_distance(const TABLE *table, KEY *keyinfo)
{
  if (keyinfo->option_struct->metric == EUCLIDEAN)
    return Item_func_vec_distance::EUCLIDEAN;
  return Item_func_vec_distance::COSINE;
}

/*
  Declare the plugin and index options
*/

ha_create_table_option mhnsw_index_options[]=
{
  HA_IOPTION_SYSVAR("m", M, default_m),
  HA_IOPTION_SYSVAR("distance", metric, default_distance),
  HA_IOPTION_END
};

st_plugin_int *mhnsw_plugin;

static int mhnsw_init(void *p)
{
  mhnsw_plugin= (st_plugin_int *)p;
  mhnsw_plugin->data= &MHNSW_Trx::tp;
  if (setup_transaction_participant(mhnsw_plugin))
    return 1;

  return resolve_sysvar_table_options(mhnsw_index_options);
}

static int mhnsw_deinit(void *)
{
  free_sysvar_table_options(mhnsw_index_options);
  return 0;
}

static struct st_mysql_storage_engine mhnsw_daemon=
{ MYSQL_DAEMON_INTERFACE_VERSION };

static struct st_mysql_sys_var *mhnsw_sys_vars[]=
{
  MYSQL_SYSVAR(max_cache_size),
  MYSQL_SYSVAR(default_m),
  MYSQL_SYSVAR(default_distance),
  MYSQL_SYSVAR(ef_search),
  NULL
};

maria_declare_plugin(mhnsw)
{
  MYSQL_DAEMON_PLUGIN,
  &mhnsw_daemon, "mhnsw", "MariaDB plc",
  "A plugin for mhnsw vector index algorithm",
  PLUGIN_LICENSE_GPL, mhnsw_init, mhnsw_deinit, 0x0100, NULL,
  mhnsw_sys_vars, "1.0", MariaDB_PLUGIN_MATURITY_STABLE
}
maria_declare_plugin_end;
