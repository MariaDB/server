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
@file btr/btr0ra.cc
Logical read-ahead of B-tree leaf pages: collecting the page numbers of the
leaves from their PAGE_LEVEL=1 parent page

The algorithm is described in handler/ha_innodb.cc (see "Logical read-ahead"). */

#include "btr0ra.h"
#include "btr0btr.h"
#include "buf0buf.h"
#include "dict0dict.h"
#include "page0page.h"
#include "rem0rec.h"

const rec_t *btr_ra_batch::next_node_ptr(const rec_t *rec,
                                         bool reverse) noexcept
{
  if (reverse)
  {
    rec= page_rec_get_prev_const(rec);
    return !rec || page_rec_is_infimum(rec) ? nullptr : rec;
  }
  rec= page_rec_get_next_const(rec);
  return !rec || page_rec_is_supremum(rec) ? nullptr : rec;
}

template<bool reverse>
btr_ra_result btr_ra_batch::collect(const dict_index_t *index,
                                    const rec_t *rec) noexcept
{
  ut_ad(!full());
  ut_ad(btr_page_get_level(page_align(rec)) == 1);

  for (;;)
  {
    const uint32_t child= index->get_child(rec);
    pages[n++]= child;
    l1_child= child;
    if (child == stop_page)
      return BTR_RA_STOP;
    if (full())
      return BTR_RA_FULL;
    if (!(rec= next_node_ptr(rec, reverse)))
      return BTR_RA_EDGE;
  }
}

template btr_ra_result
btr_ra_batch::collect<true>(const dict_index_t *, const rec_t *) noexcept;
template btr_ra_result
btr_ra_batch::collect<false>(const dict_index_t *, const rec_t *) noexcept;

template<bool reverse>
const rec_t *btr_ra_batch::resume_rec(const buf_block_t *block,
                                      const dict_index_t *index,
                                      uint32_t resume_child) noexcept
{
  const page_t *page= block->page.frame;
  ut_ad(btr_page_get_level(page) == 1);
  const rec_t *rec= reverse
    ? page_rec_get_prev_const(page_get_supremum_rec(page))
    : page_rec_get_next_const(page_get_infimum_rec(page));

  if (!resume_child)
    return rec;

  /* Locate the last read child page from the records */
  while (rec &&
         !(reverse ? page_rec_is_infimum(rec) : page_rec_is_supremum(rec)))
  {
    const uint32_t child= index->get_child(rec);
    rec= reverse
      ? page_rec_get_prev_const(rec) : page_rec_get_next_const(rec);
    if (child == resume_child)
      return rec;
  }
  return nullptr;
}

template const rec_t *
btr_ra_batch::resume_rec<true>(const buf_block_t *, const dict_index_t *,
                               uint32_t) noexcept;
template const rec_t *
btr_ra_batch::resume_rec<false>(const buf_block_t *, const dict_index_t *,
                                uint32_t) noexcept;
