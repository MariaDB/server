/*****************************************************************************

Copyright (c) 1997, 2012, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2010-2012, Percona Inc. All Rights Reserved.

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

#ifndef read0i_s_h
#define read0i_s_h

#include <trx0types.h>

struct i_s_xtradb_read_view_struct {
	undo_no_t	undo_no;/*!< 0 or if type is
				VIEW_HIGH_GRANULARITY
				transaction undo_no when this high-granularity
				consistent read view was created */
	trx_id_t	low_limit_no;
				/*!< The view does not need to see the undo
				logs for transactions whose transaction number
				is strictly smaller (<) than this value: they
				can be removed in purge if not needed by other
				views */
	trx_id_t	low_limit_id;
				/*!< The read should not see any transaction
				with trx id >= this value. In other words,
				this is the "high water mark". */
	trx_id_t	up_limit_id;
				/*!< The read should see all trx ids which
				are strictly smaller (<) than this value.
				In other words,
				this is the "low water mark". */
};

typedef struct i_s_xtradb_read_view_struct i_s_xtradb_read_view_t;

UNIV_INTERN
i_s_xtradb_read_view_t*
read_fill_i_s_xtradb_read_view(i_s_xtradb_read_view_t *rv);


#endif /* read0i_s_h */
