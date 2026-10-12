/****************************************************************************

Copyright (c) 2026, MariaDB plc

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

*****************************************************************************/

#include "btr0blink_alloc.h"

#include "btr0btr.h"
#include "dict0mem.h"
#include "mtr0mtr.h"

#include <deque>
#include <mutex>
#include <new>

struct blink_page_pool_t
{
  blink_page_pool_t(ulint leaf_target_arg, ulint internal_target_arg)
    : leaf_target(leaf_target_arg), internal_target(internal_target_arg)
  {}

  std::mutex mutex;
  std::mutex refill_mutex;
  std::deque<uint32_t> leaf_pages;
  std::deque<uint32_t> internal_pages;
  const ulint leaf_target;
  const ulint internal_target;
};

bool blink_page_pool_register(dict_index_t *index, ulint leaf_target,
                              ulint internal_target) noexcept
{
  ut_ad(index);
  if (index->blink_page_pool)
    return true;

  index->blink_page_pool= new (std::nothrow)
    blink_page_pool_t(leaf_target, internal_target);
  return index->blink_page_pool != nullptr;
}

void blink_page_pool_unregister(dict_index_t *index) noexcept
{
  ut_ad(index);
  delete index->blink_page_pool;
  index->blink_page_pool= nullptr;
}

static dberr_t blink_page_pool_refill_kind(dict_index_t *index,
                                           blink_page_pool_t *pool,
                                           blink_page_kind kind,
                                           trx_t *trx) noexcept
{
  std::deque<uint32_t> *pages;
  ulint target;
  {
    std::lock_guard<std::mutex> guard(pool->mutex);
    pages= kind == blink_page_kind::LEAF
      ? &pool->leaf_pages : &pool->internal_pages;
    target= kind == blink_page_kind::LEAF
      ? pool->leaf_target : pool->internal_target;
  }

  while (true)
  {
    uint32_t hint;
    {
      std::lock_guard<std::mutex> guard(pool->mutex);
      if (pages->size() >= target)
        return DB_SUCCESS;
      hint= pages->empty() ? index->page : pages->back();
    }

    mtr_t init_mtr{trx};
    mtr_t alloc_mtr{trx};
    init_mtr.start();
    alloc_mtr.start();
    index->set_modified(init_mtr);
    index->set_modified(alloc_mtr);

    uint32_t n_reserved= 0;
    dberr_t err= fsp_reserve_free_extents(&n_reserved, index->table->space, 1,
                                          FSP_NORMAL, &alloc_mtr);
    buf_block_t *block= nullptr;
    if (err == DB_SUCCESS)
      block= btr_page_alloc(index, hint + 1, FSP_NO_DIR,
                            kind == blink_page_kind::LEAF ? 0 : 1,
                            &alloc_mtr, &init_mtr, &err);
    index->table->space->release_free_extents(n_reserved);
    alloc_mtr.commit();

    if (!block)
    {
      init_mtr.commit();
      return err;
    }

    btr_page_create(block, nullptr, index,
                    kind == blink_page_kind::LEAF ? 0 : 1, &init_mtr);
    const uint32_t page_no= block->page.id().page_no();
    init_mtr.commit();

    std::lock_guard<std::mutex> guard(pool->mutex);
    pages->push_back(page_no);
  }
}

dberr_t blink_page_pool_refill(dict_index_t *index, blink_page_kind kind,
                               trx_t *trx) noexcept
{
  ut_ad(index);
  blink_page_pool_t *pool= index->blink_page_pool;
  if (!pool)
    return DB_SUCCESS;

  /* Serialize refillers without blocking split consumers on FSP allocation. */
  std::lock_guard<std::mutex> refill_guard(pool->refill_mutex);
  return blink_page_pool_refill_kind(index, pool, kind, trx);
}

dberr_t blink_page_pool_refill(dict_index_t *index, trx_t *trx) noexcept
{
  dberr_t err= blink_page_pool_refill(index, blink_page_kind::LEAF, trx);
  return err == DB_SUCCESS
    ? blink_page_pool_refill(index, blink_page_kind::INTERNAL, trx) : err;
}

bool blink_page_pool_try_pop(dict_index_t *index, blink_page_kind kind,
                             uint32_t *page_no) noexcept
{
  ut_ad(index);
  ut_ad(page_no);
  blink_page_pool_t *pool= index->blink_page_pool;
  if (!pool)
    return false;

  std::lock_guard<std::mutex> guard(pool->mutex);
  std::deque<uint32_t> &pages= kind == blink_page_kind::LEAF
    ? pool->leaf_pages : pool->internal_pages;
  if (pages.empty())
    return false;

  *page_no= pages.front();
  pages.pop_front();
  return true;
}
