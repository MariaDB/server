/*****************************************************************************

Copyright (c) 2020, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2020, MariaDB Corporation.
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
#pragma once
#include "buf0buf.h"

namespace buf {
class Block_hint {
public:
  /** Stores the pointer to the block, which is currently buffer-fixed.
  @param  block   a pointer to a buffer-fixed block to be stored */
  inline void store(buf_block_t *block)
  {
    ut_ad(block->page.buf_fix_count());
    m_block= block;
    m_page_id= block->page.id();
  }

  /** Clears currently stored pointer. */
  inline void clear() { m_block= nullptr; }

  /** Invoke f on m_block(which may be null)
  @param  f   The function to be executed. It will be passed the pointer.
              If you wish to use the block pointer subsequently,
	      you need to ensure you buffer-fix it before returning from f.
  @return the return value of f
  */
  template <typename F>
  bool run_with_hint(const F &f)
  {
    buffer_fix_block_if_still_valid();
    /* m_block could be changed during f() call, so we use local
    variable to remember which block we need to unfix */
    buf_block_t *block= m_block;
    bool res= f(block);
    if (block)
      block->page.unfix();
    return res;
  }

  buf_block_t *block() const { return m_block; }

 private:
  /** The block pointer stored by store(). */
  buf_block_t *m_block= nullptr;
  /** If m_block is non-null, the m_block->page.id at time it was stored. */
  page_id_t m_page_id{0, 0};

  /** A helper function which checks if m_block is not a dangling pointer and
  still points to block with page with m_page_id and if so, buffer-fixes it,
  otherwise clear()s it */
  void buffer_fix_block_if_still_valid();
};
}  // namespace buf
