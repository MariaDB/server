/*****************************************************************************

Copyright (c) 1996, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2022, MariaDB Corporation.

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
@file include/btr0sea.h
The index tree adaptive search

Created 2/17/1996 Heikki Tuuri
*************************************************************************/

#pragma once

#include "dict0dict.h"
#ifdef BTR_CUR_HASH_ADAPT
# include "buf0buf.h"

# ifdef UNIV_PFS_RWLOCK
extern mysql_pfs_key_t btr_search_latch_key;
# endif /* UNIV_PFS_RWLOCK */

# define btr_search_sys_create() btr_search.create()
# define btr_search_sys_free() btr_search.free()

ATTRIBUTE_COLD void btr_search_lazy_free(dict_index_t *index) noexcept;

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
bool
btr_search_guess_on_hash(
	dict_index_t*	index,
	const dtuple_t*	tuple,
	bool		ge,
	btr_latch_mode	latch_mode,
	btr_cur_t*	cursor,
	mtr_t*		mtr) noexcept;

/** Move or delete hash entries for moved records, usually in a page split.
If new_block is already hashed, then any hash index for block is dropped.
If new_block is not hashed, and block is hashed, then a new hash index is
built to new_block with the same parameters as block.
@param new_block   destination page
@param block       source page (subject to deletion later) */
void btr_search_move_or_delete_hash_entries(buf_block_t *new_block,
                                            buf_block_t *block) noexcept;

/** Drop any adaptive hash index entries that point to an index page.
@param block        latched block containing index page, or a buffer-unfixed
                    index page or a block in state BUF_BLOCK_REMOVE_HASH
@param not_garbage  drop only if the index is set and NOT this */
void btr_search_drop_page_hash_index(buf_block_t *block,
                                     const dict_index_t *not_garbage) noexcept;

/** Drop possible adaptive hash index entries when a page is evicted
from the buffer pool or freed in a file, or the index is being dropped.
@param mtr       mini-transaction
@param page_id   page identifier of the being-dropped page */
void btr_search_drop_page_hash_when_freed(mtr_t *mtr, const page_id_t page_id)
  noexcept;

/** Update the page hash index after a single record is inserted on a page.
@param cursor cursor which was positioned before the inserted record
@param reorg  whether the page was reorganized */
void btr_search_update_hash_on_insert(btr_cur_t *cursor, bool reorg) noexcept;

/** Updates the page hash index before a single record is deleted from a page.
@param cursor   cursor positioned on the to-be-deleted record */
void btr_search_update_hash_on_delete(btr_cur_t *cursor) noexcept;

/** Validates the search system.
@param thd   connection, for checking if CHECK TABLE has been killed
@return true if ok */
bool btr_search_validate(THD *thd) noexcept;

# ifdef UNIV_DEBUG
/** @return if the index is marked as freed */
bool btr_search_check_marked_free_index(const buf_block_t *block) noexcept;
# endif /* UNIV_DEBUG */

struct ahi_node;

/** The hash index system */
struct btr_sea
{
  /** the actual value of innodb_adaptive_hash_index, protected by
  all partition::latch. Note that if buf_block_t::index is not nullptr
  while a thread is holding a partition::latch, then also this must hold. */
  Atomic_relaxed<bool> enabled;

private:
  /** Disable the adaptive hash search system and empty the index.
  @return whether the adaptive hash index was enabled */
  ATTRIBUTE_COLD bool disable_and_lock() noexcept;

  /** Unlock the adaptive hash search system. */
  ATTRIBUTE_COLD void unlock() noexcept;
public:
  /** Disable the adaptive hash search system and empty the index.
  @return whether the adaptive hash index was enabled */
  ATTRIBUTE_COLD bool disable() noexcept;

  /** Enable the adaptive hash search system.
  @param resize whether buf_pool_t::resize() is the caller */
  ATTRIBUTE_COLD void enable(bool resize= false) noexcept;

  /** Hash cell chain in hash_table */
  struct hash_chain
  {
    /** pointer to the first block */
    ahi_node *first;

    /** Find an element.
    @param u   unary predicate
    @return the first matching element
    @retval nullptr if not found */
    template<typename UnaryPred>
    inline ahi_node *find(UnaryPred u) const noexcept;

    /** Search for a pointer to an element.
    @param u   unary predicate
    @return pointer to the first matching element,
    or to the last element in the chain */
    template<typename UnaryPred>
    inline ahi_node **search(UnaryPred u) noexcept;
  };

  /** Hash table with singly-linked overflow lists.
  Based on @see buf_pool_t::page_hash_table */
  struct hash_table
  {
    static_assert(CPU_LEVEL1_DCACHE_LINESIZE >= 64, "less than 64 bytes");
    static_assert(!(CPU_LEVEL1_DCACHE_LINESIZE & 63),
      "not a multiple of 64 bytes");

    /** Number of array[] elements per page_hash_latch.
    Must be one less than a power of 2. */
#if 0
    static constexpr size_t ELEMENTS_PER_LATCH= 64 / sizeof(void*) - 1;

    /** Extra padding. FIXME: Is this ever useful to be nonzero?
    Long time ago, some testing on an ARMv8 implementation seemed
    to suggest so, but this has not been validated recently. */
    static constexpr size_t EMPTY_SLOTS_PER_LATCH=
      ((CPU_LEVEL1_DCACHE_LINESIZE / 64) - 1) * (64 / sizeof(void*));
#else
    static constexpr size_t ELEMENTS_PER_LATCH=
      CPU_LEVEL1_DCACHE_LINESIZE / sizeof(void*) - 1;
    static constexpr size_t EMPTY_SLOTS_PER_LATCH= 0;
#endif

    /** number of payload elements in array[] */
    Atomic_relaxed<size_t> n_cells;
    /** the hash table, with pad(n_cells) elements, aligned to L1 cache size */
    hash_chain *array;

    /** Create the hash table.
    @param n  the lower bound of n_cells
    @return whether the creation succeeded */
    inline bool create(ulint n) noexcept;

    /** Free the hash table. */
    void free() noexcept { aligned_free(array); array= nullptr; }

    /** @return the index of an array element */
    ulint calc_hash(ulint fold) const noexcept
    { return calc_hash(fold, n_cells); }
    /** @return raw array index converted to padded index */
    static ulint pad(ulint h) noexcept
    {
      ulint latches= h / ELEMENTS_PER_LATCH;
      ulint empty_slots= latches * EMPTY_SLOTS_PER_LATCH;
      return 1 + latches + empty_slots + h;
    }
  private:
    /** @return the index of an array element */
    static ulint calc_hash(ulint fold, ulint n_cells) noexcept
    {
      return pad(fold % n_cells);
    }
  public:
    /** @return the latch covering a hash table chain */
    static page_hash_latch &lock_get(hash_chain &chain) noexcept
    {
      static_assert(!((ELEMENTS_PER_LATCH + 1) & ELEMENTS_PER_LATCH),
                    "must be one less than a power of 2");
      const size_t addr= reinterpret_cast<size_t>(&chain);
      ut_ad(addr & (ELEMENTS_PER_LATCH * sizeof chain));
      return *reinterpret_cast<page_hash_latch*>
        (addr & ~(ELEMENTS_PER_LATCH * sizeof chain));
    }

    /** Get a hash table slot. */
    hash_chain &cell_get(ulint fold) const
    { return array[calc_hash(fold, n_cells)]; }
  };

  /** Partition of the hash table */
  struct partition
  {
    /** latch protecting table: either an exclusive latch, or
    a shared latch combined with lock_get() */
    alignas(CPU_LEVEL1_DCACHE_LINESIZE)
    IF_DBUG(srw_lock_debug,srw_spin_lock) latch;
    /** map of CRC-32C of rec prefix to rec_t* in buf_page_t::frame */
    hash_table table;
    /** protects blocks; acquired while holding latch
    and possibly table.lock_get() */
    srw_mutex blocks_mutex;
    /** allocated blocks */
    UT_LIST_BASE_NODE_T(buf_page_t) blocks;
    /** a cached block to extend blocks */
    Atomic_relaxed<buf_block_t*> spare;

    inline void init() noexcept;

    /** @return whether the allocation succeeded */
    inline bool alloc(size_t hash_size) noexcept;

    inline void clear() noexcept;

    inline void free() noexcept;

    /** @return the number of allocated buffer pool blocks */
    TPOOL_SUPPRESS_TSAN size_t get_blocks() const noexcept
    { return UT_LIST_GET_LEN(blocks) + !!spare; }

    /** Ensure that there is a spare block for a future insert() */
    void prepare_insert() noexcept;

    /** Undo prepare_insert() in case !btr_search.enabled */
    void rollback_insert() noexcept;

  private:
    /** Start cleanup_after_erase()
    @return the last allocated element */
    inline ahi_node *cleanup_after_erase_start() noexcept;
    /** Finish cleanup_after_erase().
    We reduce the allocated size in UT_LIST_GET_LAST(blocks)->free_offset.
    If that size reaches 0, the last block will be removed from blocks,
    and a block may have to be freed by our caller.
    @return buffer block to be freed
    @retval nullptr if no buffer block was freed */
    buf_block_t *cleanup_after_erase_finish() noexcept;
  public:
    __attribute__((nonnull))
    /** Clean up after erasing an AHI node, while the caller is
    holding an exclusive latch. Unless "erase" is the last allocated
    element, we will swap it with the last allocated element.
    Finally, we return via cleanup_after_erase_finish().
    @param erase node being erased
    @return buffer block to be freed
    @retval nullptr if no buffer block was freed */
    buf_block_t *cleanup_after_erase(ahi_node *erase) noexcept;

    __attribute__((nonnull))
    /** Clean up after erasing an AHI node. This is similar to
    cleanup_after_erase(ahi_node*), except that the operation may fail.
    @param erase   node being erased
    @param l       the latch held together with shared latch
    @return buffer block to be freed
    @retval nullptr if no buffer block was freed
    @retval -1     if we fail to shrink the allocation and erasing
                   needs to be retried while holding an exclusive latch */
    buf_block_t *cleanup_after_erase(ahi_node *erase, page_hash_latch *l)
      noexcept;

    __attribute__((nonnull))
# if defined UNIV_AHI_DEBUG || defined UNIV_DEBUG
    /** Insert or replace an entry into the hash table.
    @param fold  CRC-32C of rec prefix
    @param rec   B-tree leaf page record
    @param block the buffer block that contains rec */
    void insert(uint32_t fold, const rec_t *rec, buf_block_t *block) noexcept;
# else
    /** Insert or replace an entry into the hash table.
    @param fold  CRC-32C of rec prefix
    @param rec   B-tree leaf page record */
    void insert(uint32_t fold, const rec_t *rec) noexcept;
# endif

    /** erase() return value */
    enum erase_status{
      /** must retry with exclusive latch */
      ERASE_RETRY= -1,
      /** the pointer to the record was erased */
      ERASED= 0,
      /** nothing was erased */
      NOT_ERASED= 1
    };

    /** Delete a pointer to a record if it exists, and release the latch.
    @tparam ex   true=holding exclusive latch, false=shared latch
    @param cell  hash table cell that may contain the CRC-32C of rec prefix
    @param rec   B-tree leaf page record
    @return status */
    template<bool ex>
    erase_status erase(hash_chain &cell, const rec_t *rec) noexcept;
  };

  /** number of hash table entries, to be multiplied by n_parts */
  uint n_cells;
  /** innodb_adaptive_hash_index_parts */
  ulong n_parts;
  /** Partitions of the adaptive hash index */
  partition parts[512];

  /** Get an adaptive hash index partition */
  partition &get_part(index_id_t id) noexcept { return parts[id % n_parts]; }

  /** Get an adaptive hash index partition */
  partition &get_part(const dict_index_t &index) noexcept
  { return get_part(index.id); }

  /** Create and initialize at startup */
  void create() noexcept;

  /** @return whether the allocation succeeded */
  bool alloc(size_t hash_size) noexcept;

  /** Change the number of cells */
  void resize(uint n_cells) noexcept;

  /** Clear when disabling the adaptive hash index */
  inline void clear() noexcept;

  /** Free at shutdown */
  void free() noexcept;
};

/** The adaptive hash index */
extern btr_sea btr_search;

# ifdef UNIV_SEARCH_PERF_STAT
/** Number of successful adaptive hash index lookups */
extern ulint btr_search_n_succ;
/** Number of failed adaptive hash index lookups */
extern ulint btr_search_n_hash_fail;
# endif /* UNIV_SEARCH_PERF_STAT */
#else /* BTR_CUR_HASH_ADAPT */
# define btr_search_sys_create()
# define btr_search_sys_free()
# define btr_search_drop_page_hash_index(block, not_garbage)
# define btr_search_move_or_delete_hash_entries(new_block, block)
# define btr_search_update_hash_on_insert(cursor, ahi_latch)
# define btr_search_update_hash_on_delete(cursor)
# ifdef UNIV_DEBUG
#  define btr_search_check_marked_free_index(block)
# endif /* UNIV_DEBUG */
#endif /* BTR_CUR_HASH_ADAPT */
