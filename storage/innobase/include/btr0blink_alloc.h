/****************************************************************************

Copyright (c) 2026, MariaDB plc

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

*****************************************************************************/

#ifndef btr0blink_alloc_h
#define btr0blink_alloc_h

#include "db0err.h"
#include "fil0fil.h"
#include "univ.i"

struct dict_index_t;
struct trx_t;

/** Opaque per-index pool of pages reserved for B-link splits. */
struct blink_page_pool_t;

/** The two B-tree segments from which pages can be prefetched. */
enum class blink_page_kind : uint8_t
{
  LEAF,
  INTERNAL
};

/** Default benchmark pool sizes. */
constexpr ulint BLINK_LEAF_POOL_PAGES= 4096;
constexpr ulint BLINK_INTERNAL_POOL_PAGES= 256;

/** Attach an empty page pool to a B-link index. This does not allocate file
pages and is therefore safe while the dictionary latch is held.
@return whether a pool is attached */
bool blink_page_pool_register(dict_index_t *index,
                              ulint leaf_target= BLINK_LEAF_POOL_PAGES,
                              ulint internal_target= BLINK_INTERNAL_POOL_PAGES)
  noexcept;

/** Detach and destroy an index page pool. The index must no longer be in use. */
void blink_page_pool_unregister(dict_index_t *index) noexcept;

/** Fill one queue to its configured target using btr_page_alloc().
Allocation and page latches are committed in private mini-transactions.

This is deliberately a synchronous benchmark hook. It must be called before
acquiring an X latch on any leaf page of index. Reserved pages that have not
been popped are not reclaimed by unregister().
@return DB_SUCCESS or the first allocation error */
dberr_t blink_page_pool_refill(dict_index_t *index, blink_page_kind kind,
                               trx_t *trx= nullptr) noexcept;

/** Fill both the leaf and internal queues to their configured targets. */
dberr_t blink_page_pool_refill(dict_index_t *index, trx_t *trx= nullptr)
  noexcept;

/** Remove one reserved page from a queue without doing file-space allocation.
The returned page is allocated but not initialized as a B-tree page. The split
mini-transaction must X-fix it and call btr_page_create().
@return whether a page was returned */
bool blink_page_pool_try_pop(dict_index_t *index, blink_page_kind kind,
                             uint32_t *page_no) noexcept;

#endif /* btr0blink_alloc_h */
