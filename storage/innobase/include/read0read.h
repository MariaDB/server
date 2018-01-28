/*****************************************************************************

Copyright (c) 1997, 2013, Oracle and/or its affiliates. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin Street, Suite 500, Boston, MA 02110-1335 USA

*****************************************************************************/

/**************************************************//**
@file include/read0read.h
Cursor read

Created 2/16/1997 Heikki Tuuri
*******************************************************/

#ifndef read0read_h
#define read0read_h

#include "univ.i"

#include "read0types.h"

/** The MVCC read view manager */
class MVCC
{
  /** Active views. */
  UT_LIST_BASE_NODE_T(ReadView) m_views;


  /** Validates a read view list. */
  bool validate() const;
public:
  MVCC() { UT_LIST_INIT(m_views, &ReadView::m_view_list); }
  ~MVCC() { ut_ad(UT_LIST_GET_LEN(m_views) == 0); }


  /**
    Allocate and create a view.
    @param trx transaction creating the view
  */
  void view_open(trx_t *trx);


  /**
    Close a view created by the above function.
    @param view view allocated by view_open.
  */
  void view_close(ReadView &view);


  /**
    Clones the oldest view and stores it in view. No need to
    call view_close(). The caller owns the view that is passed in.
    This function is called by Purge to create it view.

    @param view Preallocated view, owned by the caller
  */
  void clone_oldest_view(ReadView *view);


  /** @return the number of active views */
  size_t size() const;
};

#endif /* read0read_h */
