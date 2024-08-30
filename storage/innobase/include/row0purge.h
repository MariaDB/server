/*****************************************************************************

Copyright (c) 1997, 2016, Oracle and/or its affiliates. All Rights Reserved.
Copyright (c) 2017, 2023, MariaDB Corporation.

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

/**************************************************//**
@file include/row0purge.h
Purge obsolete records

Created 3/14/1997 Heikki Tuuri
*******************************************************/

#pragma once

#include "que0types.h"
#include "btr0types.h"
#include "btr0pcur.h"
#include "trx0types.h"
#include "row0types.h"
#include "row0mysql.h"
#include "mysqld.h"
#include <queue>
#include <unordered_map>

class MDL_ticket;

/***************************************************************
Does the purge operation.
@return query thread to run next */
que_thr_t*
row_purge_step(
/*===========*/
	que_thr_t*	thr)	/*!< in: query thread */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Purge worker context */
struct purge_node_t
{
  /** node type: QUE_NODE_PURGE */
  que_common_t common;

  /** DB_TRX_ID of the undo log record */
  trx_id_t trx_id;
  /** DB_ROLL_PTR pointing to undo log record */
  roll_ptr_t roll_ptr;

  /** undo number of the record */
  undo_no_t undo_no;

  /** record type: TRX_UNDO_INSERT_REC, ... */
  byte rec_type;
  /** compiler analysis info of an update */
  byte cmpl_info;
  /** whether the clustered index record determined by ref was found
  in the clustered index of the table, and we were able to position
  pcur on it */
  bool found_clust;
#ifdef UNIV_DEBUG
  /** whether the operation is in progress */
  bool in_progress= false;
#endif
  /** table where purge is done */
  dict_table_t *table= nullptr;
  /** update vector for a clustered index record */
  upd_t *update;
  /** row reference to the next row to handle, or nullptr */
  const dtuple_t *ref;
  /** nullptr, or a deep copy of the indexed fields of the row to handle */
  dtuple_t *row;
  /** nullptr, or the next index of table whose record should be handled */
  dict_index_t *index;
  /** memory heap used as auxiliary storage; must be emptied between rows */
  mem_heap_t *heap;
  /** persistent cursor to the clustered index record */
  btr_pcur_t pcur;

  /** Undo recs to purge */
  std::queue<trx_purge_rec_t> undo_recs;

  /** map of table identifiers to table handles and meta-data locks */
  std::unordered_map<table_id_t, std::pair<dict_table_t*,MDL_ticket*>> tables;

  /** Constructor */
  explicit purge_node_t(que_thr_t *parent) :
    common(QUE_NODE_PURGE, parent), heap(mem_heap_create(256)),
    tables(TRX_PURGE_TABLE_BUCKETS) {}

#ifdef UNIV_DEBUG
  /** Validate the persistent cursor. The purge node has two references
  to the clustered index record: ref and pcur, which must match
  each other if found_clust.
  @return whether pcur is consistent with ref */
  bool validate_pcur();
#endif

  /** Start processing an undo log record. */
  inline void start();

  /** Reset the state at end
  @return the query graph parent */
  inline que_node_t *end(THD *);
};
