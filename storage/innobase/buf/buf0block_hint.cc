/*****************************************************************************

Copyright (c) 2020, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2020, 2021, MariaDB Corporation.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is also distributed with certain software (including but not
limited to OpenSSL) that is licensed under separate terms, as designated in a
particular file or component or in included license documentation. The authors
of MySQL hereby grant you an additional permission to link the program and
your derivative works with the separately licensed software that they have
included with MySQL.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

#include "buf0block_hint.h"
namespace buf {

TRANSACTIONAL_TARGET
void Block_hint::buffer_fix_block_if_still_valid()
{
  /* To check if m_block belongs to the current buf_pool, we must
  prevent freeing memory while we check, and until we buffer-fix the
  block. For this purpose it is enough to latch any of the many
  latches taken by buf_pool_t::resize().

  Similar to buf_page_optimistic_get(), we must validate
  m_block->page.id() after acquiring the hash_lock, because the object
  may have been freed and not actually attached to buf_pool.page_hash
  at the moment. (The block could have been reused to store a
  different page, and that slice of buf_pool.page_hash could be protected
  by another hash_lock that we are not holding.)

  Finally, we must ensure that the block is not being freed. */
  if (m_block)
  {
    auto &cell= buf_pool.page_hash.cell_get(m_page_id.fold());
    transactional_shared_lock_guard<page_hash_latch> g
      {buf_pool.page_hash.lock_get(cell)};
    if (buf_pool.is_uncompressed(m_block) && m_page_id == m_block->page.id() &&
        m_block->page.frame && m_block->page.in_file())
      m_block->page.fix();
    else
      clear();
  }
}
}  // namespace buf
