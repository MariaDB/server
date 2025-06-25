/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA

*****************************************************************************/

/********************************************************************//**
@file btr/btr0sea.cc
The index tree adaptive search

Created 2/17/1996 Heikki Tuuri
*************************************************************************/

#include "btr0sea.h"
#ifdef BTR_CUR_HASH_ADAPT
#include "buf0buf.h"
#include "buf0lru.h"
#include "page0page.h"
#include "page0cur.h"
#include "btr0cur.h"
#include "btr0pcur.h"
#include "btr0btr.h"
#include "srv0mon.h"
#include "log.h"

#ifdef UNIV_SEARCH_PERF_STAT
/** Number of successful adaptive hash index lookups */
ulint btr_search_n_succ;
/** Number of failed adaptive hash index lookups */
ulint btr_search_n_hash_fail;
#endif /* UNIV_SEARCH_PERF_STAT */

#ifdef UNIV_PFS_RWLOCK
mysql_pfs_key_t btr_search_latch_key;
#endif /* UNIV_PFS_RWLOCK */

/** The adaptive hash index */
btr_sea btr_search;

struct ahi_node {
  /** CRC-32C of the rec prefix */
  uint32_t fold;
  /** pointer to next record in the hash bucket chain, or nullptr  */
  ahi_node *next;
  /** B-tree index leaf page record */
  const rec_t *rec;
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  /** block containing rec, or nullptr */
  buf_block_t *block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
};

inline void btr_sea::partition::init() noexcept
{
  latch.SRW_LOCK_INIT(btr_search_latch_key);
  blocks_mutex.init();
  UT_LIST_INIT(blocks, &buf_page_t::list);
}

inline void btr_sea::partition::clear() noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
  ut_ad(blocks_mutex.is_locked());
#endif
  if (buf_block_t *b= spare)
  {
    spare= nullptr;
    MEM_MAKE_ADDRESSABLE(b->page.frame, srv_page_size);
    buf_pool.free_block(b);
  }
  table.free();

  while (buf_page_t *b= UT_LIST_GET_FIRST(blocks))
  {
    UT_LIST_REMOVE(blocks, b);
    ut_ad(b->free_offset);
    b->hash= nullptr;
    MEM_MAKE_ADDRESSABLE(b->frame, srv_page_size);
    buf_pool.free_block(reinterpret_cast<buf_block_t*>(b));
  }
}

inline void btr_sea::partition::free() noexcept
{
  if (table.array)
  {
    ut_d(latch.wr_lock(SRW_LOCK_CALL));
    ut_d(blocks_mutex.wr_lock());
    clear();
    ut_d(blocks_mutex.wr_unlock());
    ut_d(latch.wr_unlock());
  }
  latch.destroy();
  blocks_mutex.destroy();
}

inline void btr_sea::partition::alloc(ulint hash_size) noexcept
{
  table.create(hash_size);
}

void btr_sea::create() noexcept
{
  for (partition &part : parts)
    part.init();
  if (enabled)
    enable();
}

void btr_sea::alloc(ulint hash_size) noexcept
{
  hash_size/= n_parts;
  for (ulong i= 0; i < n_parts; ++i)
    parts[i].alloc(hash_size);
}

inline void btr_sea::clear() noexcept
{
  for (ulong i= 0; i < n_parts; ++i)
    parts[i].clear();
}

void btr_sea::free() noexcept
{
  for (partition &part : parts)
    part.free();
}

/** If the number of records on the page divided by this parameter
would have been successfully accessed using a hash index, the index
is then built on the page, assuming the global limit has been reached */
static constexpr unsigned BTR_SEARCH_PAGE_BUILD_LIMIT= 16;

/** The global limit for consecutive potentially successful hash searches,
before hash index building is started */
static constexpr uint8_t BTR_SEARCH_BUILD_LIMIT= 100;

/** Determine the number of accessed key fields.
@param n_bytes_fields  number of complete fields | incomplete_bytes << 16
@return number of complete or incomplete fields */
inline size_t btr_search_get_n_fields(ulint n_bytes_fields) noexcept
{
  return uint16_t(n_bytes_fields) + (n_bytes_fields >= 1U << 16);
}

/** Determine the number of accessed key fields.
@param cursor    b-tree cursor
@return number of complete or incomplete fields */
inline size_t btr_search_get_n_fields(const btr_cur_t *cursor) noexcept
{
  return btr_search_get_n_fields(cursor->n_bytes_fields);
}

/** Compute a hash value of a record in a page.
@tparam comp           whether ROW_FORMAT=REDUNDANT is not being used
@param rec             index record
@param index           index tree
@param n_bytes_fields  bytes << 16 | number of complete fields
@return CRC-32C of the record prefix */
template<bool comp>
static uint32_t rec_fold(const rec_t *rec, const dict_index_t &index,
                         uint32_t n_bytes_fields) noexcept
{
  ut_ad(page_rec_is_leaf(rec));
  ut_ad(page_rec_is_user_rec(rec));
  ut_ad(!rec_is_metadata(rec, index));
  ut_ad(index.n_uniq <= index.n_core_fields);
  size_t n_f= btr_search_get_n_fields(n_bytes_fields);
  ut_ad(n_f > 0);
  ut_ad(n_f <= index.n_core_fields);
  ut_ad(comp == index.table->not_redundant());

  size_t n;

  if (comp)
  {
    const byte *nulls= rec - REC_N_NEW_EXTRA_BYTES;
    const byte *lens;
    if (rec_get_status(rec) == REC_STATUS_INSTANT)
    {
      ulint n_fields= index.n_core_fields + rec_get_n_add_field(nulls) + 1;
      ut_ad(n_fields <= index.n_fields);
      const ulint n_nullable= index.get_n_nullable(n_fields);
      ut_ad(n_nullable <= index.n_nullable);
      lens= --nulls - UT_BITS_IN_BYTES(n_nullable);
    }
    else
      lens= --nulls - index.n_core_null_bytes;
    byte null_mask= 1;
    n= 0;

    const dict_field_t *field= index.fields;
    size_t len;
    do
    {
      const dict_col_t *col= field->col;
      if (col->is_nullable())
      {
        const int is_null{*nulls & null_mask};
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic push
# if __GNUC__ < 12 || defined WITH_UBSAN
#  pragma GCC diagnostic ignored "-Wconversion"
# endif
#endif
        null_mask<<= 1;
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic pop
#endif
        if (UNIV_UNLIKELY(!null_mask))
          null_mask= 1, nulls--;
        if (is_null)
        {
          len= 0;
          continue;
        }
      }

      len= field->fixed_len;

      if (!len)
      {
        len= *lens--;
        if (UNIV_UNLIKELY(len & 0x80) && DATA_BIG_COL(col))
        {
          len<<= 8;
          len|= *lens--;
          ut_ad(!(len & 0x4000));
          len&= 0x3fff;
        }
      }

      n+= len;
    }
    while (field++, --n_f);

    if (size_t n_bytes= n_bytes_fields >> 16)
      n+= std::min(n_bytes, len) - len;
  }
  else
  {
    const size_t n_bytes= n_bytes_fields >> 16;
    ut_ad(n_f <= rec_get_n_fields_old(rec));
    if (rec_get_1byte_offs_flag(rec))
    {
      n= rec_1_get_field_end_info(rec, n_f - 1) & ~REC_1BYTE_SQL_NULL_MASK;
      if (!n_bytes);
      else if (!uint16_t(n_bytes_fields))
        n= std::min(n_bytes, n);
      else
      {
        size_t len= n - (rec_1_get_field_end_info(rec, n_f - 2) &
                         ~REC_1BYTE_SQL_NULL_MASK);
        n+= std::min(n_bytes, n - len) - len;
      }
    }
    else
    {
      n= rec_2_get_field_end_info(rec, n_f - 1) & ~REC_2BYTE_SQL_NULL_MASK;
      ut_ad(n < REC_2BYTE_EXTERN_MASK); /* keys never are BLOBs */
      if (!n_bytes);
      else if (!uint16_t(n_bytes_fields))
        n= std::min(n_bytes, n);
      else
      {
        size_t len= n - (rec_2_get_field_end_info(rec, n_f - 2) &
                         ~REC_2BYTE_SQL_NULL_MASK);
        n+= std::min(n_bytes, n - len) - len;
      }
    }
  }

  return my_crc32c(uint32_t(ut_fold_ull(index.id)), rec, n);
}

static uint32_t rec_fold(const rec_t *rec, const dict_index_t &index,
                         uint32_t n_bytes_fields, ulint comp) noexcept
{
  return comp
    ? rec_fold<true>(rec, index, n_bytes_fields)
    : rec_fold<false>(rec, index, n_bytes_fields);
}

void btr_sea::partition::prepare_insert() noexcept
{
  /* spare may be consumed by insert() or clear() */
  if (!spare)
  {
    buf_block_t *block= buf_block_alloc();
    blocks_mutex.wr_lock();
    if (!spare && btr_search.enabled)
    {
      MEM_NOACCESS(block->page.frame, srv_page_size);
      spare= block;
      block= nullptr;
    }
    blocks_mutex.wr_unlock();
    if (block)
      buf_pool.free_block(block);
  }
}

/** Clear the AHI reference count on all tables.
@param tables list of tables */
static void
btr_search_disable(const UT_LIST_BASE_NODE_T(dict_table_t)& tables) noexcept
{
  for (dict_table_t *table= UT_LIST_GET_FIRST(dict_sys.table_LRU); table;
       table = UT_LIST_GET_NEXT(table_LRU, table))
    for (dict_index_t *index= dict_table_get_first_index(table); index;
         index= dict_table_get_next_index(index))
      index->search_info.ref_count= 0;
}

/** Lazily free detached metadata when removing the last reference. */
ATTRIBUTE_COLD void btr_search_lazy_free(dict_index_t *index) noexcept
{
  ut_ad(index->freed());
  dict_table_t *table= index->table;
  table->autoinc_mutex.wr_lock();

  /* Perform the skipped steps of dict_index_remove_from_cache_low(). */
  UT_LIST_REMOVE(table->freed_indexes, index);
  index->lock.free();
  dict_mem_index_free(index);

  if (!UT_LIST_GET_LEN(table->freed_indexes) &&
      !UT_LIST_GET_LEN(table->indexes))
  {
    ut_ad(!table->id);
    table->autoinc_mutex.wr_unlock();
    table->autoinc_mutex.destroy();
    dict_mem_table_free(table);
    return;
  }

  table->autoinc_mutex.wr_unlock();
}

ATTRIBUTE_COLD bool btr_sea::disable() noexcept
{
  dict_sys.freeze(SRW_LOCK_CALL);

  for (ulong i= 0; i < n_parts; i++)
  {
    parts[i].latch.wr_lock(SRW_LOCK_CALL);
    parts[i].blocks_mutex.wr_lock();
  }

  const bool was_enabled{enabled};

  if (was_enabled)
  {
    enabled= false;
    btr_search_disable(dict_sys.table_LRU);
    btr_search_disable(dict_sys.table_non_LRU);
    dict_sys.unfreeze();

    /* Set all block->index= nullptr. */
    buf_pool.clear_hash_index();
    clear();
  }
  else
    dict_sys.unfreeze();

  for (ulong i= 0; i < n_parts; i++)
  {
    parts[i].latch.wr_unlock();
    parts[i].blocks_mutex.wr_unlock();
  }

  return was_enabled;
}

/** Enable the adaptive hash search system.
@param resize whether buf_pool_t::resize() is the caller */
ATTRIBUTE_COLD void btr_sea::enable(bool resize) noexcept
{
  if (!resize)
  {
    mysql_mutex_lock(&buf_pool.mutex);
    const auto is_shrinking = buf_pool.is_shrinking();
    mysql_mutex_unlock(&buf_pool.mutex);
    if (is_shrinking)
      return;
  }

  for (ulong i= 0; i < n_parts; i++)
  {
    parts[i].latch.wr_lock(SRW_LOCK_CALL);
    parts[i].blocks_mutex.wr_lock();
  }

  if (!parts[0].table.array)
  {
    enabled= true;
    alloc(buf_pool.curr_pool_size() / sizeof(void *) / 64);
  }

  ut_ad(enabled);

  for (ulong i= 0; i < n_parts; i++)
  {
    parts[i].blocks_mutex.wr_unlock();
    parts[i].latch.wr_unlock();
  }
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
# define ha_insert_for_fold(p,f,b,d) (p).insert(f,d,b)
#else
# define ha_insert_for_fold(p,f,b,d) (p).insert(f,d)
#endif

ATTRIBUTE_NOINLINE
/** Update a hash node reference when it has been unsuccessfully used in a
search which could have succeeded with the used hash parameters. This can
happen because when building a hash index for a page, we do not check
what happens at page boundaries, and therefore there can be misleading
hash nodes. Also, collisions in the fold value can lead to misleading
references. This function lazily fixes these imperfections in the hash
index.
@param cursor              B-tree cursor
@param block               cursor block
@param left_bytes_fields   AHI paramaters */
static void btr_search_update_hash_ref(const btr_cur_t &cursor,
                                       buf_block_t *block,
                                       uint32_t left_bytes_fields) noexcept
{
  ut_ad(block == cursor.page_cur.block);
#ifdef UNIV_SEARCH_PERF_STAT
  btr_search_n_hash_fail++;
#endif /* UNIV_SEARCH_PERF_STAT */

  dict_index_t *const index= cursor.index();
  ut_ad(block->page.id().space() == index->table->space_id);
  btr_sea::partition &part= btr_search.get_part(index->id);
  part.prepare_insert();
  part.latch.wr_lock(SRW_LOCK_CALL);

  if (ut_d(const dict_index_t *block_index=) block->index)
  {
    ut_ad(block_index == index);
    ut_ad(btr_search.enabled);
    ut_ad(index->search_info.ahi_enabled);
    uint32_t bytes_fields{block->ahi_left_bytes_fields};
    if (bytes_fields != left_bytes_fields)
      goto skip;
    if (UNIV_UNLIKELY(index->search_info.left_bytes_fields !=
                      left_bytes_fields))
      goto skip;
    bytes_fields&= ~buf_block_t::LEFT_SIDE;
    const rec_t *rec= cursor.page_cur.rec;
    uint32_t fold;
    if (page_is_comp(block->page.frame))
    {
      switch (rec - block->page.frame) {
      case PAGE_NEW_INFIMUM:
      case PAGE_NEW_SUPREMUM:
        goto skip;
      default:
        fold= rec_fold<true>(rec, *index, bytes_fields);
      }
    }
    else
    {
      switch (rec - block->page.frame) {
      case PAGE_OLD_INFIMUM:
      case PAGE_OLD_SUPREMUM:
        goto skip;
      default:
        fold= rec_fold<false>(rec, *index, bytes_fields);
      }
    }

    ha_insert_for_fold(part, fold, block, rec);
    MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
  }
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  else
    ut_a(!block->n_pointers);
# endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

skip:
  part.latch.wr_unlock();
}

/** Updates the search info of an index about hash successes.
@param cursor   freshly positioned cursor
@return AHI parameters
@retval 0 if the adaptive hash index should not be rebuilt */
static uint32_t btr_search_info_update_hash(const btr_cur_t &cursor) noexcept
{
  ut_ad(cursor.flag == BTR_CUR_HASH_FAIL ||
        cursor.flag == BTR_CUR_HASH_ABORT ||
        cursor.flag == BTR_CUR_BINARY);

  dict_index_t *const index= cursor.index();
  buf_block_t *const block= cursor.page_cur.block;
  ut_ad(block->page.lock.have_any());
  ut_d(const uint32_t state= block->page.state());
  ut_ad(state >= buf_page_t::UNFIXED);
  ut_ad(!block->page.is_read_fixed(state));

  switch (uintptr_t(btr_cur_get_rec(&cursor) - block->page.frame)) {
  case PAGE_OLD_INFIMUM:
  case PAGE_OLD_SUPREMUM:
  case PAGE_NEW_INFIMUM:
  case PAGE_NEW_SUPREMUM:
    /* The adaptive hash index only includes user records. */
    return 0;
  }

  const dict_index_t *const block_index= block->index;
  uint16_t n_hash_helps{block->n_hash_helps};
  const uint16_t n_uniq=
    uint16_t(index->n_uniq ? index->n_uniq : index->n_fields);
  dict_index_t::ahi &info= index->search_info;
  uint32_t left_bytes_fields{info.left_bytes_fields};
  uint8_t n_hash_potential= info.n_hash_potential;
  uint32_t ret;

  if (!n_hash_potential)
  {
    info.left_bytes_fields= left_bytes_fields= buf_block_t::LEFT_SIDE | 1;
    info.hash_analysis_reset();
  increment_potential:
    if (n_hash_potential < BTR_SEARCH_BUILD_LIMIT)
      info.n_hash_potential= ++n_hash_potential;
    if (n_hash_helps)
      goto got_help;
    goto no_help;
  }
  else if (uint16_t(left_bytes_fields) >= n_uniq && cursor.up_match >= n_uniq)
    /* The search would have succeeded using the recommended prefix */
    goto increment_potential;
  else
  {
    const bool left_side{!!(left_bytes_fields & buf_block_t::LEFT_SIDE)};
    const int info_cmp=
      int(uint16_t((left_bytes_fields & ~buf_block_t::LEFT_SIDE) >> 16) |
          int{uint16_t(left_bytes_fields)} << 16);
    const int low_cmp = int(cursor.low_match << 16 | cursor.low_bytes);
    const int up_cmp = int(cursor.up_match << 16 | cursor.up_bytes);

    if (left_side == (info_cmp > low_cmp) && left_side == (info_cmp <= up_cmp))
      goto increment_potential;

    const int cmp= up_cmp - low_cmp;
    static_assert(buf_block_t::LEFT_SIDE == 1U << 31, "");
    left_bytes_fields= (cmp >= 0) << 31;

    if (left_bytes_fields)
    {
      if (cursor.up_match >= n_uniq)
        left_bytes_fields|= n_uniq;
      else if (cursor.low_match < cursor.up_match)
        left_bytes_fields|= uint32_t(cursor.low_match + 1);
      else
      {
        left_bytes_fields|= cursor.low_match;
        left_bytes_fields|= uint32_t(cursor.low_bytes + 1) << 16;
      }
    }
    else
    {
      if (cursor.low_match >= n_uniq)
        left_bytes_fields|= n_uniq;
      else if (cursor.low_match > cursor.up_match)
        left_bytes_fields|= uint32_t(cursor.up_match + 1);
      else
      {
        left_bytes_fields|= cursor.up_match;
        left_bytes_fields|= uint32_t(cursor.up_bytes + 1) << 16;
      }
    }
    /* We have to set a new recommendation; skip the hash analysis for a
    while to avoid unnecessary CPU time usage when there is no chance
    for success */
    info.hash_analysis_reset();
    info.left_bytes_fields= left_bytes_fields;
    info.n_hash_potential= cmp != 0;
    if (cmp == 0)
      goto no_help;
  }

  ut_ad(block->page.lock.have_x() || block->page.lock.have_s());
  ut_ad(btr_cur_get_page(&cursor) == page_align(btr_cur_get_rec(&cursor)));
  ut_ad(page_is_leaf(btr_cur_get_page(&cursor)));

  if (!n_hash_helps)
  {
  no_help:
    info.last_hash_succ= false;
    block->n_hash_helps= 1;
    ret= 0;
  }
  else
  {
  got_help:
    const uint32_t ahi_left_bytes_fields= block->ahi_left_bytes_fields;

    ret= left_bytes_fields;
    info.last_hash_succ=
      block_index && ahi_left_bytes_fields == left_bytes_fields;

    if (n_hash_potential >= BTR_SEARCH_BUILD_LIMIT)
    {
      const auto n_recs= page_get_n_recs(block->page.frame);
      if (n_hash_helps / 2 > n_recs)
        goto func_exit;
      if (n_hash_helps >= n_recs / BTR_SEARCH_PAGE_BUILD_LIMIT &&
          (!block_index || left_bytes_fields != ahi_left_bytes_fields))
        goto func_exit;
    }

    if (++n_hash_helps)
      block->n_hash_helps= n_hash_helps;
    ret= 0;
  }

func_exit:
  if (!block_index);
  else if (UNIV_UNLIKELY(block_index != index))
  {
    ut_ad(block_index->id == index->id);
    btr_search_drop_page_hash_index(block, nullptr);
  }
  else if (cursor.flag == BTR_CUR_HASH_FAIL)
    btr_search_update_hash_ref(cursor, block, left_bytes_fields);

  return ret;
}

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
/** Maximum number of records in a page */
constexpr ulint MAX_N_POINTERS = UNIV_PAGE_SIZE_MAX / REC_N_NEW_EXTRA_BYTES;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
void btr_sea::partition::insert(uint32 fold, const rec_t *rec,
                                buf_block_t *block) noexcept
#else
void btr_sea::partition::insert(uint32_t fold, const rec_t *rec) noexcept
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
#endif
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(block->page.frame == page_align(rec));
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
  ut_ad(btr_search.enabled);

  ahi_node **prev= table.cell_get(fold)->
    search(&ahi_node::next, [fold](const ahi_node *node)
    { return !node || node->fold == fold; });
  ahi_node *node= *prev;

  if (node)
  {
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
    buf_block_t *prev_block= node->block;
    if (prev_block != block)
    {
      ut_a(prev_block->page.frame == page_align(node->rec));
      ut_a(prev_block->n_pointers-- < MAX_N_POINTERS);
      ut_a(block->n_pointers++ < MAX_N_POINTERS);
      node->block= block;
    }
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
    node->rec= rec;
    return;
  }

  /* We have to allocate a new chain node */

  {
    blocks_mutex.wr_lock();
    buf_page_t *last= UT_LIST_GET_LAST(blocks);
    if (last && last->free_offset < srv_page_size - sizeof *node)
    {
      node= reinterpret_cast<ahi_node*>(last->frame + last->free_offset);
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic push
# if __GNUC__ < 12 || defined WITH_UBSAN
#  pragma GCC diagnostic ignored "-Wconversion"
# endif
#endif
      last->free_offset+= sizeof *node;
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic pop
#endif
      MEM_MAKE_ADDRESSABLE(node, sizeof *node);
    }
    else
    {
      last= &spare.load()->page;
      if (!last)
      {
        blocks_mutex.wr_unlock();
        return;
      }
      spare= nullptr;
      ut_ad(last->state() == buf_page_t::MEMORY);
      ut_ad(!reinterpret_cast<buf_block_t*>(last)->index);
      ut_ad(!reinterpret_cast<buf_block_t*>(last)->n_pointers);
      UT_LIST_ADD_LAST(blocks, last);
      last->free_offset= sizeof *node;
      node= reinterpret_cast<ahi_node*>(last->frame);
      MEM_UNDEFINED(last->frame, srv_page_size);
      MEM_MAKE_ADDRESSABLE(node, sizeof *node);
      MEM_NOACCESS(node + 1, srv_page_size - sizeof *node);
    }
    blocks_mutex.wr_unlock();
  }

#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(block->n_pointers++ < MAX_N_POINTERS);
  node->block= block;
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
  node->rec= rec;

  node->fold= fold;
  node->next= nullptr;

  *prev= node;
}

buf_block_t *btr_sea::partition::cleanup_after_erase(ahi_node *erase) noexcept
{
  ut_ad(btr_search.enabled);
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
#endif
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(erase->block->page.frame == page_align(erase->rec));
  ut_a(erase->block->n_pointers-- < MAX_N_POINTERS);
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */

  blocks_mutex.wr_lock();

  buf_page_t *last= UT_LIST_GET_LAST(blocks);
  const ahi_node *const top= reinterpret_cast<ahi_node*>
    (last->frame + last->free_offset - sizeof *top);

  if (erase != top)
  {
    /* Shrink the allocation by replacing the erased element with the top. */
    *erase= *top;
    ahi_node **prev= table.cell_get(top->fold)->
      search(&ahi_node::next, [top](const ahi_node *n) { return n == top; });
    *prev= erase;
  }

  buf_block_t *freed= nullptr;

#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic push
# if __GNUC__ < 12 || defined WITH_UBSAN
#  pragma GCC diagnostic ignored "-Wconversion"
# endif
#endif
  /* We may be able to shrink or free the last block */
  if (!(last->free_offset-= uint16_t(sizeof *erase)))
#if defined __GNUC__ && !defined __clang__
# pragma GCC diagnostic pop
#endif
  {
    if (spare)
    {
      freed= reinterpret_cast<buf_block_t*>(last);
      MEM_MAKE_ADDRESSABLE(last->frame, srv_page_size);
    }
    else
      spare= reinterpret_cast<buf_block_t*>(last);
    UT_LIST_REMOVE(blocks, last);
  }
  else
    MEM_NOACCESS(last->frame + last->free_offset, sizeof *erase);

  blocks_mutex.wr_unlock();
  return freed;
}

__attribute__((nonnull))
/** Delete all pointers to a page.
@param part      hash table partition
@param fold      CRC-32C value
@param page      page of a record to be deleted */
static void ha_remove_all_nodes_to_page(btr_sea::partition &part,
                                        uint32_t fold, const page_t *page)
  noexcept
{
  hash_cell_t *cell= part.table.cell_get(fold);
  const uintptr_t page_size{srv_page_size};

rewind:
  ahi_node **prev=
    cell->search(&ahi_node::next, [page,page_size](const ahi_node *node)
    { return !node || (uintptr_t(node->rec) ^ uintptr_t(page)) < page_size; });

  if (ahi_node *node= *prev)
  {
    *prev= node->next;
    node->next= nullptr;
    if (buf_block_t *block= part.cleanup_after_erase(node))
      buf_pool.free_block(block);
    /* The deletion may compact the heap of nodes and move other nodes! */
    goto rewind;
  }

  /* Check that all nodes really got deleted */
  ut_ad(!cell->find(&ahi_node::next, [page](const ahi_node* node)
  { return page_align(node->rec) == page; }));
}

inline bool btr_sea::partition::erase(uint32_t fold, const rec_t *rec) noexcept
{
#ifndef SUX_LOCK_GENERIC
  ut_ad(latch.is_write_locked());
#endif
  ut_ad(btr_search.enabled);

  ahi_node **prev= table.cell_get(fold)->
    search(&ahi_node::next, [rec](const ahi_node *node)
    { return !node || node->rec == rec; });

  if (ahi_node *node= *prev)
  {
    *prev= node->next;
    node->next= nullptr;
    buf_block_t *block= cleanup_after_erase(node);
    latch.wr_unlock();
    if (block)
      buf_pool.free_block(block);
    return true;
  }

  latch.wr_unlock();
  return false;
}

__attribute__((nonnull))
/** Looks for an element when we know the pointer to the data and
updates the pointer to data if found.
@param table     hash table
@param fold      folded value of the searched data
@param data      pointer to the data
@param new_data  new pointer to the data
@return whether the element was found */
static bool ha_search_and_update_if_found(hash_table_t *table, uint32_t fold,
                                          const rec_t *data,
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
                                          /** block containing new_data */
                                          buf_block_t *new_block,
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
                                          const rec_t *new_data) noexcept
{
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(new_block->page.frame == page_align(new_data));
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
  ut_ad(btr_search.enabled);

  if (ahi_node *node= table->cell_get(fold)->
      find(&ahi_node::next, [data](const ahi_node *node)
      { return node->rec == data; }))
  {
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
    if (node->block != new_block)
    {
      ut_a(node->block->n_pointers-- < MAX_N_POINTERS);
      ut_a(new_block->n_pointers++ < MAX_N_POINTERS);
      node->block= new_block;
    }
#endif /* UNIV_AHI_DEBUG || UNIV_DEBUG */
    node->rec= new_data;
    return true;
  }

  return false;
}

#if !defined UNIV_AHI_DEBUG && !defined UNIV_DEBUG
# define ha_search_and_update_if_found(table,fold,data,new_block,new_data) \
  ha_search_and_update_if_found(table,fold,data,new_data)
#endif

/** Fold a prefix given as the number of fields of a tuple.
@param tuple    index record
@param cursor   B-tree cursor
@return CRC-32C of the record prefix */
static uint32_t dtuple_fold(const dtuple_t *tuple, const btr_cur_t *cursor)
{
  ut_ad(tuple);
  ut_ad(tuple->magic_n == DATA_TUPLE_MAGIC_N);
  ut_ad(dtuple_check_typed(tuple));

  const bool comp= cursor->index()->table->not_redundant();
  uint32_t fold= uint32_t(ut_fold_ull(cursor->index()->id));
  const size_t n_fields= uint16_t(cursor->n_bytes_fields);


  for (unsigned i= 0; i < n_fields; i++)
  {
    const dfield_t *field= dtuple_get_nth_field(tuple, i);
    const void *data= dfield_get_data(field);
    size_t len= dfield_get_len(field);
    if (len == UNIV_SQL_NULL)
    {
      if (UNIV_UNLIKELY(!comp))
      {
        len= dtype_get_sql_null_size(dfield_get_type(field), 0);
        data= field_ref_zero;
      }
      else
        continue;
    }
    fold= my_crc32c(fold, data, len);
  }

  if (size_t n_bytes= cursor->n_bytes_fields >> 16)
  {
    const dfield_t *field= dtuple_get_nth_field(tuple, n_fields);
    const void *data= dfield_get_data(field);
    size_t len= dfield_get_len(field);
    if (len == UNIV_SQL_NULL)
    {
      if (UNIV_UNLIKELY(!comp))
      {
        len= dtype_get_sql_null_size(dfield_get_type(field), 0);
        data= field_ref_zero;
      }
      else
        return fold;
    }
    fold= my_crc32c(fold, data, std::min(n_bytes, len));
  }

  return fold;
}

/** Tries to guess the right search position based on the hash search info
of the index. Note that if mode is PAGE_CUR_LE, which is used in inserts,
and the function returns TRUE, then cursor->up_match and cursor->low_match
both have sensible values.
@param[in,out]	index		index
@param[in]	tuple		logical record
@param[in]	ge		false=PAGE_CUR_LE, true=PAGE_CUR_GE
@param[in]	latch_mode	BTR_SEARCH_LEAF, ...
@param[out]	cursor		tree cursor
@param[in]	mtr		mini-transaction
@return whether the search succeeded */
TRANSACTIONAL_TARGET
bool
btr_search_guess_on_hash(
	dict_index_t*	index,
	const dtuple_t*	tuple,
	bool		ge,
	btr_latch_mode	latch_mode,
	btr_cur_t*	cursor,
	mtr_t*		mtr) noexcept
{
  ut_ad(mtr->is_active());
  ut_ad(index->is_btree());
  ut_ad(latch_mode == BTR_SEARCH_LEAF || latch_mode == BTR_MODIFY_LEAF);
  ut_ad(cursor->flag == BTR_CUR_BINARY);

  if ((tuple->info_bits & REC_INFO_MIN_REC_FLAG))
    return false;

  if (!index->search_info.last_hash_succ ||
      !index->search_info.n_hash_potential)
  {
  ahi_unusable:
    if (!index->table->is_temporary() && btr_search.enabled)
      cursor->flag= BTR_CUR_HASH_ABORT;
    return false;
  }

  ut_ad(!index->table->is_temporary());

  static_assert(ulint{BTR_SEARCH_LEAF} == ulint{RW_S_LATCH}, "");
  static_assert(ulint{BTR_MODIFY_LEAF} == ulint{RW_X_LATCH}, "");

  cursor->n_bytes_fields= index->search_info.left_bytes_fields &
    ~buf_block_t::LEFT_SIDE;

  if (dtuple_get_n_fields(tuple) < btr_search_get_n_fields(cursor))
    goto ahi_unusable;

  const index_id_t index_id= index->id;

#ifdef UNIV_SEARCH_PERF_STAT
  index->search_info.n_hash_succ++;
#endif
  const uint32_t fold= dtuple_fold(tuple, cursor);
  cursor->fold= fold;
  btr_sea::partition &part= btr_search.get_part(*index);

  part.latch.rd_lock(SRW_LOCK_CALL);

  if (!btr_search.enabled)
  {
  ahi_release_and_fail:
    part.latch.rd_unlock();
  fail:
#ifdef UNIV_SEARCH_PERF_STAT
    ++index->search_info.n_hash_fail;
    if (index->search_info.n_hash_succ > 0)
      --index->search_info.n_hash_succ;
#endif /* UNIV_SEARCH_PERF_STAT */
    index->search_info.last_hash_succ= false;
    return false;
  }

  const ahi_node *node= part.table.cell_get(fold)->
    find(&ahi_node::next, [fold](const ahi_node* node)
    { return node->fold == fold; });

  if (!node)
  {
    cursor->flag= BTR_CUR_HASH_FAIL;
    goto ahi_release_and_fail;
  }

  const rec_t *rec= node->rec;
  buf_block_t* block = buf_pool.block_from(rec);
  ut_ad(block->page.frame == page_align(rec));
#if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
  ut_a(block == node->block);
#endif
  {
    buf_pool_t::hash_chain &chain=
      buf_pool.page_hash.cell_get(block->page.id().fold());
    /* We must hold the cell latch while attempting to acquire
    block->page.lock, because buf_LRU_block_remove_hashed() assumes
    that block->page.can_relocate() will not cease to hold. */
    transactional_shared_lock_guard<page_hash_latch> g
      {buf_pool.page_hash.lock_get(chain)};
    if (latch_mode == BTR_SEARCH_LEAF
        ? !block->page.lock.s_lock_try()
        : !block->page.lock.x_lock_try())
      goto ahi_release_and_fail;
  }

  const uint32_t state{block->page.state()};

  if (UNIV_UNLIKELY(state < buf_page_t::UNFIXED))
  {
    ut_ad(state == buf_page_t::REMOVE_HASH);
  block_and_ahi_release_and_fail:
    if (latch_mode == BTR_SEARCH_LEAF)
      block->page.lock.s_unlock();
    else
      block->page.lock.x_unlock();
    cursor->flag= BTR_CUR_HASH_FAIL;
    goto ahi_release_and_fail;
  }

  ut_ad(!block->page.is_read_fixed(state));
  ut_ad(!block->page.is_write_fixed(state) || latch_mode == BTR_SEARCH_LEAF);

  const dict_index_t *block_index= block->index;
  if (index != block_index && index_id == block_index->id)
  {
    ut_a(block_index->freed());
    cursor->flag= BTR_CUR_HASH_FAIL;
    goto block_and_ahi_release_and_fail;
  }

  /* We successfully validated the state of the block and that it
  actually belongs to our index. Now it is safe to release part.latch.
  Because we are holding block->page.lock, the page cannot be
  modified or evicted (buf_page_t::can_relocate() will not hold) while
  we validate the guessed rec. */
  part.latch.rd_unlock();

  block->page.fix();
  buf_page_make_young_if_needed(&block->page);
  static_assert(ulint{MTR_MEMO_PAGE_S_FIX} == ulint{BTR_SEARCH_LEAF}, "");
  static_assert(ulint{MTR_MEMO_PAGE_X_FIX} == ulint{BTR_MODIFY_LEAF}, "");

  ++buf_pool.stat.n_page_gets;

  mtr->memo_push(block, mtr_memo_type_t(latch_mode));

  ut_ad(page_rec_is_user_rec(rec));
  ut_ad(page_is_leaf(block->page.frame));

  btr_cur_position(index, const_cast<rec_t*>(rec), block, cursor);
  const ulint comp{page_is_comp(block->page.frame)};
  if (UNIV_LIKELY(comp != 0))
    switch (rec_get_status(rec)) {
    case REC_STATUS_INSTANT:
    case REC_STATUS_ORDINARY:
      break;
    default:
    mismatch:
      mtr->release_last_page();
      cursor->flag= BTR_CUR_HASH_FAIL;
      goto fail;
    }

  /* Check the validity of the guess within the page */
  if (index_id != btr_page_get_index_id(block->page.frame) ||
      cursor->check_mismatch(*tuple, ge, comp))
    goto mismatch;

  uint8_t n_hash_potential= index->search_info.n_hash_potential;

  if (n_hash_potential++ < BTR_SEARCH_BUILD_LIMIT)
    index->search_info.n_hash_potential= n_hash_potential;

  index->search_info.last_hash_succ= true;
  cursor->flag= BTR_CUR_HASH;

#ifdef UNIV_SEARCH_PERF_STAT
  btr_search_n_succ++;
#endif
  return true;
}

/** The maximum number of rec_fold() values to allocate in stack.
The actual maximum number of records per page is 8189, limited by
the 13-bit heap number field in the record header. */
static constexpr size_t REC_FOLD_IN_STACK= 128;

/** Drop any adaptive hash index entries that point to an index page.
@param block        latched block containing index page, or a buffer-unfixed
                    index page or a block in state BUF_BLOCK_REMOVE_HASH
@param not_garbage  drop only if the index is set and NOT this
@param folds        work area for REC_FOLD_IN_STACK rec_fold() values */
static void btr_search_drop_page_hash_index(buf_block_t *block,
                                            const dict_index_t *not_garbage,
                                            uint32_t *folds) noexcept
{
retry:
  dict_index_t *index= block->index;
  if (!index || index == not_garbage)
    return;

  ut_d(const auto state= block->page.state());
  ut_ad(state == buf_page_t::REMOVE_HASH || state >= buf_page_t::UNFIXED);
  ut_ad(state == buf_page_t::REMOVE_HASH ||
        !(~buf_page_t::LRU_MASK & state) || block->page.lock.have_any());
  ut_ad(state < buf_page_t::READ_FIX || state >= buf_page_t::WRITE_FIX);
  ut_ad(page_is_leaf(block->page.frame));

  /* We must not dereference block->index here, because it could be freed
  if (!index->table->get_ref_count() && !dict_sys.frozen()).
  Determine the ahi_slot based on the block contents. */
  const index_id_t index_id= btr_page_get_index_id(block->page.frame);
  btr_sea::partition &part= btr_search.get_part(index_id);

  part.latch.rd_lock(SRW_LOCK_CALL);
  index= block->index;

  if (!index)
  {
  unlock_and_return:
    part.latch.rd_unlock();
    return;
  }

  ut_ad(btr_search.enabled);

  bool holding_x= index->freed();

  if (holding_x)
  {
    part.latch.rd_unlock();
    part.latch.wr_lock(SRW_LOCK_CALL);
    if (index != block->index)
    {
      part.latch.wr_unlock();
      goto retry;
    }
  }
  else if (not_garbage != nullptr)
  {
    ut_ad(!index || index == not_garbage ||
          not_garbage == reinterpret_cast<dict_index_t*>(-1));
    goto unlock_and_return;
  }

  assert_block_ahi_valid(block);

  ut_ad(!index->table->is_temporary());

  ut_ad(block->page.id().space() == index->table->space_id);
  ut_a(index_id == index->id);

  const uint32_t left_bytes_fields= block->ahi_left_bytes_fields;

  /* NOTE: block->ahi_left_bytes_fields may change after we release part.latch,
  as we might only hold block->page.lock.s_lock()! */

  if (!holding_x)
    part.latch.rd_unlock();

  const uint32_t n_bytes_fields= left_bytes_fields & ~buf_block_t::LEFT_SIDE;
  ut_ad(n_bytes_fields);

  const page_t *const page= block->page.frame;
  size_t n_folds= 0;
  const rec_t *rec;

  if (page_is_comp(page))
  {
    rec= page_rec_next_get<true>(page, page + PAGE_NEW_INFIMUM);

    if (rec && rec_is_metadata(rec, true))
    {
      ut_ad(index->is_instant());
      rec= page_rec_next_get<true>(page, rec);
    }

    while (rec && rec != page + PAGE_NEW_SUPREMUM)
    {
    next_not_redundant:
      folds[n_folds]= rec_fold<true>(rec, *index, n_bytes_fields);
      rec= page_rec_next_get<true>(page, rec);
      if (!n_folds)
        n_folds++;
      else if (folds[n_folds] == folds[n_folds - 1]);
      else if (++n_folds == REC_FOLD_IN_STACK)
        break;
    }
  }
  else
  {
    rec= page_rec_next_get<false>(page, page + PAGE_OLD_INFIMUM);

    if (rec && rec_is_metadata(rec, false))
    {
      ut_ad(index->is_instant());
      rec= page_rec_next_get<false>(page, rec);
    }

    while (rec && rec != page + PAGE_OLD_SUPREMUM)
    {
    next_redundant:
      folds[n_folds]= rec_fold<false>(rec, *index, n_bytes_fields);
      rec= page_rec_next_get<false>(page, rec);
      if (!n_folds)
        n_folds++;
      else if (folds[n_folds] == folds[n_folds - 1]);
      else if (++n_folds == REC_FOLD_IN_STACK)
        break;
    }
  }

  if (!holding_x)
  {
    part.latch.wr_lock(SRW_LOCK_CALL);
    if (UNIV_UNLIKELY(!block->index))
      /* Someone else has meanwhile dropped the hash index */
      goto cleanup;
    ut_a(block->index == index);
  }

  if (block->ahi_left_bytes_fields != left_bytes_fields)
  {
    /* Someone else has meanwhile built a new hash index on the page,
    with different parameters */
    part.latch.wr_unlock();
    goto retry;
  }

  MONITOR_INC_VALUE(MONITOR_ADAPTIVE_HASH_ROW_REMOVED, n_folds);

  while (n_folds)
    ha_remove_all_nodes_to_page(part, folds[--n_folds], page);

  if (!rec);
  else if (page_is_comp(page))
  {
    if (rec != page + PAGE_NEW_SUPREMUM)
    {
      holding_x= true;
      goto next_not_redundant;
    }
  }
  else if (rec != page + PAGE_OLD_SUPREMUM)
  {
    holding_x= true;
    goto next_redundant;
  }

  switch (index->search_info.ref_count--) {
  case 0:
    ut_error;
  case 1:
    if (index->freed())
      btr_search_lazy_free(index);
  }

  ut_ad(!block->n_pointers);
  block->index= nullptr;

  MONITOR_INC(MONITOR_ADAPTIVE_HASH_PAGE_REMOVED);

cleanup:
  assert_block_ahi_valid(block);
  part.latch.wr_unlock();
}

void btr_search_drop_page_hash_index(buf_block_t *block,
                                     const dict_index_t *not_garbage) noexcept
{
  uint32_t folds[REC_FOLD_IN_STACK];
  btr_search_drop_page_hash_index(block, not_garbage, folds);
}

void btr_search_drop_page_hash_when_freed(const page_id_t page_id) noexcept
{
  mtr_t mtr;
  mtr.start();
  /* If the caller has a latch on the page, then the caller must be an
  x-latch page and it must have already dropped the hash index for the
  page. Because of the x-latch that we are possibly holding, we must
  (recursively) x-latch it, even though we are only reading. */
  if (buf_block_t *block= buf_page_get_gen(page_id, 0, RW_X_LATCH, nullptr,
                                           BUF_PEEK_IF_IN_POOL, &mtr))
  {
    /* In all our callers, the table handle should be open, or we
    should be in the process of dropping the table (preventing eviction). */
    ut_d(if (dict_index_t *i= block->index))
      ut_ad(i->table->get_ref_count() || dict_sys.locked());
    btr_search_drop_page_hash_index(block, nullptr);
  }

  mtr.commit();
}

/** Build a hash index on a page with the given parameters. If the page already
has a hash index with different parameters, the old hash index is removed.
If index is non-NULL, this function checks if n_fields and n_bytes are
sensible, and does not build a hash index if not.
@param index               B-tree index
@param block               latched B-tree leaf page
@param part                the adaptive search partition
@param left_bytes_fields   hash parameters */
static void btr_search_build_page_hash_index(dict_index_t *index,
                                             buf_block_t *block,
                                             btr_sea::partition &part,
                                             const uint32_t left_bytes_fields)
  noexcept
{
  ut_ad(!index->table->is_temporary());

  if (!btr_search.enabled || !index->search_info.ahi_enabled)
    return;

  ut_ad(block->page.id().space() == index->table->space_id);
  ut_ad(page_is_leaf(block->page.frame));
  ut_ad(block->page.lock.have_any());
  ut_ad(block->page.id().page_no() >= 3);
  ut_ad(&part == &btr_search.get_part(*index));

  part.latch.rd_lock(SRW_LOCK_CALL);

  const dict_index_t *const block_index= block->index;
  const bool rebuild= block_index &&
    (block_index != index ||
     block->ahi_left_bytes_fields != left_bytes_fields);
  const bool enabled= btr_search.enabled;

  part.latch.rd_unlock();

  if (!enabled)
    return;

  struct{uint32_t fold;uint32_t offset;} fr[REC_FOLD_IN_STACK / 2];

  if (rebuild)
    btr_search_drop_page_hash_index(block, nullptr, &fr[0].fold);

  const uint32_t n_bytes_fields{left_bytes_fields & ~buf_block_t::LEFT_SIDE};

  /* Check that the values for hash index build are sensible */
  ut_ad(n_bytes_fields);
  ut_ad(btr_search_get_n_fields(n_bytes_fields) <=
        (index->n_uniq ? index->n_uniq : index->n_fields));

  const page_t *const page= block->page.frame;
  size_t n_cached= 0;
  const rec_t *rec;

  if (page_is_comp(page))
  {
    rec= page_rec_next_get<true>(page, page + PAGE_NEW_INFIMUM);

    if (rec && rec_is_metadata(rec, true))
    {
      ut_ad(index->is_instant());
      rec= page_rec_next_get<true>(page, rec);
    }

    while (rec && rec != page + PAGE_NEW_SUPREMUM)
    {
    next_not_redundant:
      const uint32_t offset= uint32_t(uintptr_t(rec));
      fr[n_cached]= {rec_fold<true>(rec, *index, n_bytes_fields), offset};
      rec= page_rec_next_get<true>(page, rec);
      if (!n_cached)
        n_cached= 1;
      else if (fr[n_cached - 1].fold == fr[n_cached].fold)
      {
        if (!(left_bytes_fields & buf_block_t::LEFT_SIDE))
          fr[n_cached - 1].offset= offset;
      }
      else if (++n_cached == array_elements(fr))
        break;
    }
  }
  else
  {
    rec= page_rec_next_get<false>(page, page + PAGE_OLD_INFIMUM);

    if (rec && rec_is_metadata(rec, false))
    {
      ut_ad(index->is_instant());
      rec= page_rec_next_get<false>(page, rec);
    }

    while (rec && rec != page + PAGE_OLD_SUPREMUM)
    {
    next_redundant:
      const uint32_t offset= uint32_t(uintptr_t(rec));
      fr[n_cached]= {rec_fold<false>(rec, *index, n_bytes_fields), offset};
      rec= page_rec_next_get<false>(page, rec);
      if (!n_cached)
        n_cached= 1;
      else if (fr[n_cached - 1].fold == fr[n_cached].fold)
      {
        if (!(left_bytes_fields & buf_block_t::LEFT_SIDE))
          fr[n_cached - 1].offset= offset;
      }
      else if (++n_cached == array_elements(fr))
        break;
    }
  }

  part.prepare_insert();
  part.latch.wr_lock(SRW_LOCK_CALL);

  if (!block->index)
  {
    if (!btr_search.enabled)
      goto exit_func;
    ut_ad(!block->n_pointers);
    index->search_info.ref_count++;
  }
  else if (block->ahi_left_bytes_fields != left_bytes_fields)
    goto exit_func;

  block->n_hash_helps= 0;
  block->index= index;
  block->ahi_left_bytes_fields= left_bytes_fields;

  MONITOR_INC_VALUE(MONITOR_ADAPTIVE_HASH_ROW_ADDED, n_cached);

  while (n_cached)
  {
#if SIZEOF_SIZE_T <= 4
    const auto &f= fr[--n_cached];
    const rec_t *rec= reinterpret_cast<const rec_t*>(f.offset);
#else
    const auto f= fr[--n_cached];
    const rec_t *rec= page + (uint32_t(uintptr_t(page)) ^ f.offset);
#endif
    ha_insert_for_fold(part, f.fold, block, rec);
  }

  if (!rec);
  else if (page_is_comp(page))
  {
    if (rec != page + PAGE_NEW_SUPREMUM)
    {
      part.latch.wr_unlock();
      goto next_not_redundant;
    }
  }
  else if (rec != page + PAGE_OLD_SUPREMUM)
  {
    part.latch.wr_unlock();
    goto next_redundant;
  }

  MONITOR_INC(MONITOR_ADAPTIVE_HASH_PAGE_ADDED);
exit_func:
  assert_block_ahi_valid(block);
  part.latch.wr_unlock();
}

void btr_cur_t::search_info_update() const noexcept
{
  if (uint32_t left_bytes_fields= btr_search_info_update_hash(*this))
    btr_search_build_page_hash_index(index(), page_cur.block,
                                     btr_search.get_part(*index()),
                                     left_bytes_fields);
}

void btr_search_move_or_delete_hash_entries(buf_block_t *new_block,
                                            buf_block_t *block) noexcept
{
  ut_ad(block->page.lock.have_x());
  ut_ad(new_block->page.lock.have_x());

  dict_index_t *index= block->index;

  if (!btr_search.enabled || (index && !index->search_info.ahi_enabled))
    return;

  dict_index_t *new_block_index= new_block->index;

  assert_block_ahi_valid(block);
  assert_block_ahi_valid(new_block);

  if (new_block_index)
  {
    ut_ad(!index || index == new_block_index);
drop_exit:
    if (new_block_index->search_info.ahi_enabled)
      btr_search_drop_page_hash_index(block, nullptr);
    return;
  }

  if (!index)
    return;

  btr_sea::partition &part= btr_search.get_part(*index);
  part.latch.rd_lock(SRW_LOCK_CALL);

  if (index->freed())
  {
    part.latch.rd_unlock();
    goto drop_exit;
  }

  if (ut_d(dict_index_t *block_index=) block->index)
  {
    ut_ad(block_index == index);
    const uint32_t left_bytes_fields{block->ahi_left_bytes_fields};
    ut_ad(left_bytes_fields & ~buf_block_t::LEFT_SIDE);
    part.latch.rd_unlock();
    btr_search_build_page_hash_index(index, new_block, part,
                                     left_bytes_fields);
    return;
  }

  part.latch.rd_unlock();
}

void btr_search_update_hash_on_delete(btr_cur_t *cursor) noexcept
{
  ut_ad(page_is_leaf(btr_cur_get_page(cursor)));
  if (!btr_search.enabled)
    return;
  buf_block_t *block= btr_cur_get_block(cursor);

  ut_ad(block->page.lock.have_x());

  assert_block_ahi_valid(block);
  dict_index_t *index= block->index;
  if (!index || !index->search_info.ahi_enabled)
    return;
  ut_ad(!cursor->index()->table->is_temporary());

  if (UNIV_UNLIKELY(index != cursor->index()))
  {
    btr_search_drop_page_hash_index(block, nullptr);
    return;
  }

  ut_ad(block->page.id().space() == index->table->space_id);
  const uint32_t n_bytes_fields=
    block->ahi_left_bytes_fields & ~buf_block_t::LEFT_SIDE;
  ut_ad(n_bytes_fields);

  const rec_t *rec= btr_cur_get_rec(cursor);
  uint32_t fold= rec_fold(rec, *index, n_bytes_fields,
                          page_is_comp(btr_cur_get_page(cursor)));
  btr_sea::partition &part= btr_search.get_part(*index);
  part.latch.wr_lock(SRW_LOCK_CALL);
  assert_block_ahi_valid(block);

  if (ut_d(dict_index_t *block_index=) block->index)
  {
    ut_ad(btr_search.enabled);
    ut_ad(block_index == index);
    if (part.erase(fold, rec))
    {
      MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_REMOVED);
    }
    else
    {
      MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_REMOVE_NOT_FOUND);
    }
  }
  else
    part.latch.wr_unlock();
}

void btr_search_update_hash_on_insert(btr_cur_t *cursor, bool reorg) noexcept
{
  ut_ad(!cursor->index()->table->is_temporary());
  ut_ad(page_is_leaf(btr_cur_get_page(cursor)));

  buf_block_t *block= btr_cur_get_block(cursor);

  ut_ad(block->page.lock.have_x());
  assert_block_ahi_valid(block);

  dict_index_t *index= block->index;

  if (!index)
    return;

  ut_ad(block->page.id().space() == index->table->space_id);
  const rec_t *rec= btr_cur_get_rec(cursor);

  if (UNIV_UNLIKELY(index != cursor->index()))
  {
    ut_ad(index->id == cursor->index()->id);
  drop:
    btr_search_drop_page_hash_index(block, nullptr);
    return;
  }

  btr_sea::partition &part= btr_search.get_part(*index);
  bool locked= false;
  const uint32_t left_bytes_fields{block->ahi_left_bytes_fields};
  const uint32_t n_bytes_fields{left_bytes_fields & ~buf_block_t::LEFT_SIDE};
  const page_t *const page= block->page.frame;
  const rec_t *ins_rec;
  const rec_t *next_rec;
  uint32_t ins_fold, next_fold= 0, fold;
  bool next_is_supremum, rec_valid;

  if (!reorg && cursor->flag == BTR_CUR_HASH &&
      left_bytes_fields == cursor->n_bytes_fields)
  {
    part.latch.wr_lock(SRW_LOCK_CALL);
    if (!block->index)
      goto unlock_exit;
    ut_ad(btr_search.enabled);
    locked= true;
    if (page_is_comp(page))
    {
      ins_rec= page_rec_next_get<true>(page, rec);
    update_on_insert:
      /* The adaptive hash index is allowed to be a subset of what is
      actually present in the index page. It could happen that there are
      several INSERT with the same rec_fold() value, especially if the
      record prefix identified by n_bytes_fields is being duplicated
      in each INSERT. Therefore, we may fail to find the old rec
      (and fail to update the AHI to point to to our ins_rec). */
      if (ins_rec &&
          ha_search_and_update_if_found(&part.table,
                                        cursor->fold, rec, block, ins_rec))
      {
        MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_UPDATED);
      }

      assert_block_ahi_valid(block);
      goto unlock_exit;
    }
    else
    {
      ins_rec= page_rec_next_get<false>(page, rec);
      goto update_on_insert;
    }
  }

  if (page_is_comp(page))
  {
    ins_rec= page_rec_next_get<true>(page, rec);
    if (UNIV_UNLIKELY(!ins_rec)) goto drop;
    next_rec= page_rec_next_get<true>(page, ins_rec);
    if (UNIV_UNLIKELY(!next_rec)) goto drop;
    ins_fold= rec_fold<true>(ins_rec, *index, n_bytes_fields);
    next_is_supremum= next_rec == page + PAGE_NEW_SUPREMUM;
    if (!next_is_supremum)
      next_fold= rec_fold<true>(next_rec, *index, n_bytes_fields);
    rec_valid= rec != page + PAGE_NEW_INFIMUM && !rec_is_metadata(rec, true);
    if (rec_valid)
      fold= rec_fold<true>(rec, *index, n_bytes_fields);
  }
  else
  {
    ins_rec= page_rec_next_get<false>(page, rec);
    if (UNIV_UNLIKELY(!ins_rec)) goto drop;
    next_rec= page_rec_next_get<false>(page, ins_rec);
    if (UNIV_UNLIKELY(!next_rec)) goto drop;
    ins_fold= rec_fold<false>(ins_rec, *index, n_bytes_fields);
    next_is_supremum= next_rec == page + PAGE_OLD_SUPREMUM;
    if (!next_is_supremum)
      next_fold= rec_fold<false>(next_rec, *index, n_bytes_fields);
    rec_valid= rec != page + PAGE_OLD_INFIMUM && !rec_is_metadata(rec, false);
    if (rec_valid)
      fold= rec_fold<false>(rec, *index, n_bytes_fields);
  }

  part.prepare_insert();

  if (!rec_valid)
  {
    if (left_bytes_fields & buf_block_t::LEFT_SIDE)
    {
      locked= true;
      part.latch.wr_lock(SRW_LOCK_CALL);
      if (!block->index)
        goto unlock_exit;
      ha_insert_for_fold(part, ins_fold, block, ins_rec);
      MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
    }
  }
  else if (fold != ins_fold)
  {
    if (!locked)
    {
      locked= true;
      part.latch.wr_lock(SRW_LOCK_CALL);
      if (!block->index)
        goto unlock_exit;
    }
    if (left_bytes_fields & buf_block_t::LEFT_SIDE)
      fold= ins_fold, rec= ins_rec;
    ha_insert_for_fold(part, fold, block, rec);
    MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
  }

  if (next_is_supremum)
  {
    if (!(left_bytes_fields & ~buf_block_t::LEFT_SIDE))
    {
      if (!locked)
      {
        locked= true;
        part.latch.wr_lock(SRW_LOCK_CALL);
        if (!block->index)
          goto unlock_exit;
      }
      ha_insert_for_fold(part, ins_fold, block, ins_rec);
      MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
    }
  }
  else if (ins_fold != next_fold)
  {
    if (!locked)
    {
      locked= true;
      part.latch.wr_lock(SRW_LOCK_CALL);
      if (!block->index)
        goto unlock_exit;
    }
    if (!(left_bytes_fields & ~buf_block_t::LEFT_SIDE))
      next_fold= ins_fold, next_rec= ins_rec;
    ha_insert_for_fold(part, next_fold, block, next_rec);
    MONITOR_INC(MONITOR_ADAPTIVE_HASH_ROW_ADDED);
  }

  ut_ad(!locked || index == block->index);

  if (locked)
  unlock_exit:
    part.latch.wr_unlock();
}

# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
__attribute__((nonnull))
/** @return whether a range of the cells is valid */
static bool ha_validate(const hash_table_t *table,
                        ulint start_index, ulint end_index)
{
  ut_a(start_index <= end_index);
  ut_a(end_index < table->n_cells);

  bool ok= true;

  for (ulint i= start_index; i <= end_index; i++)
  {
    for (auto node= static_cast<const ahi_node*>(table->array[i].node); node;
         node= node->next)
    {
      if (table->calc_hash(node->fold) != i) {
        sql_print_error("InnoDB: Hash table node fold value " UINT32PF
                        " does not match the cell number %zu",
                        node->fold, i);
        ok= false;
      }
    }
  }

  return ok;
}

/** Lock all search latches in exclusive mode. */
static void btr_search_x_lock_all() noexcept
{
  for (ulong i= 0; i < btr_search.n_parts; i++)
    btr_search.parts[i].latch.wr_lock(SRW_LOCK_CALL);
}

/** Unlock all search latches from exclusive mode. */
static void btr_search_x_unlock_all() noexcept
{
  for (ulong i= 0; i < btr_search.n_parts; i++)
    btr_search.parts[i].latch.wr_unlock();
}

/** Validates the search system for given hash table.
@param thd            connection, for checking if CHECK TABLE has been killed
@param hash_table_id  hash table to validate
@return true if ok */
static bool btr_search_hash_table_validate(THD *thd, ulint hash_table_id)
  noexcept
{
	ahi_node*	node;
	bool		ok		= true;
	ulint		i;
	ulint		cell_count;

	btr_search_x_lock_all();
	if (!btr_search.enabled || (thd && thd_kill_level(thd))) {
func_exit:
		btr_search_x_unlock_all();
		return ok;
	}

	/* How many cells to check before temporarily releasing
	search latches. */
	ulint		chunk_size = 10000;

	mysql_mutex_lock(&buf_pool.mutex);

	btr_sea::partition& part = btr_search.parts[hash_table_id];

	cell_count = part.table.n_cells;

	for (i = 0; i < cell_count; i++) {
		/* We release search latches every once in a while to
		give other queries a chance to run. */
		if ((i != 0) && ((i % chunk_size) == 0)) {

			mysql_mutex_unlock(&buf_pool.mutex);
			btr_search_x_unlock_all();

			std::this_thread::yield();

			btr_search_x_lock_all();

			if (!btr_search.enabled
			    || (thd && thd_kill_level(thd))) {
				goto func_exit;
			}

			mysql_mutex_lock(&buf_pool.mutex);

			ulint curr_cell_count = part.table.n_cells;

			if (cell_count != curr_cell_count) {

				cell_count = curr_cell_count;

				if (i >= cell_count) {
					break;
				}
			}
		}

		node = static_cast<ahi_node*>(part.table.array[i].node);

		for (; node != NULL; node = node->next) {
			const buf_block_t*	block
				= buf_pool.block_from(node->rec);
			index_id_t		page_index_id;

			if (UNIV_LIKELY(block->page.in_file())) {
				/* The space and offset are only valid
				for file blocks.  It is possible that
				the block is being freed
				(BUF_BLOCK_REMOVE_HASH, see the
				assertion and the comment below) */
				const page_id_t id(block->page.id());
				if (const buf_page_t* hash_page
				    = buf_pool.page_hash.get(
					    id, buf_pool.page_hash.cell_get(
						    id.fold()))) {
					ut_ad(hash_page == &block->page);
					goto state_ok;
				}
			}

			/* When a block is being freed,
			buf_LRU_search_and_free_block() first removes
			the block from buf_pool.page_hash by calling
			buf_LRU_block_remove_hashed_page(). Then it
			invokes btr_search_drop_page_hash_index(). */
			ut_a(block->page.state() == buf_page_t::REMOVE_HASH);
state_ok:
			const dict_index_t* index = block->index;
			ut_ad(block->page.id().space() == index->table->space_id);

			const page_t* page = block->page.frame;

			page_index_id = btr_page_get_index_id(page);

			const uint32_t fold = rec_fold(
				node->rec, *block->index,
				block->ahi_left_bytes_fields
				& ~buf_block_t::LEFT_SIDE,
				page_is_comp(page));

			if (node->fold != fold) {
				ok = FALSE;

				ib::error() << "Error in an adaptive hash"
					<< " index pointer to page "
					<< block->page.id()
					<< ", ptr mem address "
					<< reinterpret_cast<const void*>(
						node->rec)
					<< ", index id " << page_index_id
					<< ", node fold " << node->fold
					<< ", rec fold " << fold;
				ut_ad(0);
			}
		}
	}

	for (i = 0; i < cell_count; i += chunk_size) {
		/* We release search latches every once in a while to
		give other queries a chance to run. */
		if (i != 0) {
			mysql_mutex_unlock(&buf_pool.mutex);
			btr_search_x_unlock_all();

			std::this_thread::yield();

			btr_search_x_lock_all();

			if (!btr_search.enabled
			    || (thd && thd_kill_level(thd))) {
				goto func_exit;
			}

			mysql_mutex_lock(&buf_pool.mutex);

			ulint curr_cell_count = part.table.n_cells;

			if (cell_count != curr_cell_count) {

				cell_count = curr_cell_count;

				if (i >= cell_count) {
					break;
				}
			}
		}

		ulint end_index = ut_min(i + chunk_size - 1, cell_count - 1);

		if (!ha_validate(&part.table, i, end_index)) {
			ok = false;
		}
	}

	mysql_mutex_unlock(&buf_pool.mutex);
	goto func_exit;
}

/** Validates the search system.
@param thd   connection, for checking if CHECK TABLE has been killed
@return true if ok */
bool btr_search_validate(THD *thd) noexcept
{
  for (ulint i= 0; i < btr_search.n_parts; ++i)
    if (!btr_search_hash_table_validate(thd, i))
      return false;
  return true;
}
# endif /* defined UNIV_AHI_DEBUG || defined UNIV_DEBUG */

# ifdef UNIV_DEBUG
bool btr_search_check_marked_free_index(const buf_block_t *block) noexcept
{
  const index_id_t index_id= btr_page_get_index_id(block->page.frame);
  btr_sea::partition &part= btr_search.get_part(index_id);
  bool is_freed= false;
  part.latch.rd_lock(SRW_LOCK_CALL);
  if (dict_index_t *index= block->index)
    is_freed= index->freed();
  part.latch.rd_unlock();
  return is_freed;
}
# endif /* UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
