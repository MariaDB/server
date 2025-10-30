/*****************************************************************************

Copyright (c) 2001, 2014, Oracle and/or its affiliates. All Rights Reserved.

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

/******************************************************************//**
@file include/fts0opt.h
Full Text Search optimize thread

Created 2011-02-15 Jimmy Yang
***********************************************************************/
#ifndef INNODB_FTS0OPT_H
#define INNODB_FTS0OPT_H

/** The FTS optimize thread's work queue. */
extern ib_wqueue_t*	fts_optimize_wq;

/** Callback function to fetch the rows in an FTS INDEX record.
@param index    auxiliary table clustered index
@param rec      auxiliary table record
@param offsets  offset to the record
@param user_arg points to ib_vector_t
@retval false if result memory exceeds FTS result cache limit */
bool fts_optimize_index_fetch_node(dict_index_t *index, const rec_t *rec,
                                   const rec_offs *offsets, void *user_arg);
#endif
