/*****************************************************************************

Copyright (c) 2026, MariaDB Corporation.

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

/**
@file include/btr0ra.h
Logical read-ahead of B-tree leaf pages: collecting the page numbers of the
leaves from their PAGE_LEVEL=1 parent page

The algorithm is described in handler/ha_innodb.cc (see "Logical read-ahead").
*/

#pragma once

#include "btr0types.h"
#include "span.h"

/** Return value of btr_ra_collect() */
enum btr_ra_result
{
  /** Already read full capacity */
  BTR_RA_FULL,
  /** Current level page was completed; sibling may continue it */
  BTR_RA_EDGE,
  /** Reached stop_page */
  BTR_RA_STOP
};

/** Collector of B-tree leaf child page numbers gathered
during a root-to-leaf descent (at PAGE_LEVEL=1), used to drive
logical read-ahead. */
struct btr_ra_batch
{
  /** Buffer receiving child page numbers; its size is the capacity */
  const st_::span<uint32_t> pages;
  /** number of entries filled in pages */
  uint32_t n= 0;
  /** PAGE_LEVEL=1 page the child numbers were collected from, or
  0 if none (page 0 is never an index page). Used it with l1_child to
  resume the read ahead operation */
  uint32_t l1_page= 0;
  /** child page number of the last node pointer collected from
  l1_page, or 0 if none */
  uint32_t l1_child= 0;
  /** stop right after collecting the child with this page number
  (the last leaf of the scan); 0 (never a child page) = no such page */
  uint32_t stop_page= 0;

  explicit btr_ra_batch(st_::span<uint32_t> buf) : pages(buf) {}

  /** @return whether the buffer is full */
  bool full() const noexcept { return n == pages.size(); }
  /** @return the page numbers collected so far */
  st_::span<const uint32_t> collected() const noexcept
  { return st_::span<const uint32_t>(pages.data(), n); }
  /** @return whether a later harvest can continue where this one ended:
  a level-1 page was recorded, and the scan's last leaf was not reached */
  bool resumable() const noexcept
  { return l1_page && !(stop_page && l1_child == stop_page); }

  /** Read node-pointer child page numbers from a single PAGE_LEVEL=1
  page, walking from rec in scan order. Appends to pages and updates
  l1_child, so a later call can resume from where this one stopped by
  re-locating that child.
  @tparam reverse  whether the scan goes in reverse key order
  @param index        index the record belongs to
  @param rec          first node pointer to harvest
  @retval BTR_RA_FULL when n reached pages.size()
  @retval BTR_RA_EDGE when the level-1 page was completely scanned
  @retval BTR_RA_STOP when a child reached stop_page */
  template<bool reverse>
  btr_ra_result collect(const dict_index_t *index,
                        const rec_t *rec) noexcept;

  /** Find the record on a PAGE_LEVEL=1 page from which read-ahead
  should resume: the node pointer following the one whose child page
  number is 'resume_child'. The page is walked only through its
  genuine record chain, so a concurrent reorganization
  cannot cause an invalid read; at worst the child is not found.
  @tparam reverse   whether the scan goes in reverse key order
  @param block         PAGE_LEVEL=1 page
  @param index         index the record belongs to
  @param resume_child  child page number last read, or 0 to
                       resume from the page edge
  @return the record to resume harvesting from
  @retval nullptr if 'resume_child' is no longer present on this page */
  template<bool reverse>
  static const rec_t *resume_rec(const buf_block_t *block,
                                 const dict_index_t *index,
                                 uint32_t resume_child) noexcept;

private:
  /** @return the node pointer after rec in the scan direction;
  nullptr if rec was the last one of the page */
  static const rec_t *next_node_ptr(const rec_t *rec,
                                    bool reverse) noexcept;
};
