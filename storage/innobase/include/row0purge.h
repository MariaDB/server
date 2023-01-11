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

#ifndef row0purge_h
#define row0purge_h

#include "que0types.h"
#include "btr0types.h"
#include "btr0pcur.h"
#include "trx0types.h"
#include "row0types.h"
#include "row0mysql.h"
#include "mysqld.h"
#include <queue>

class MDL_ticket;

/***************************************************************
Does the purge operation.
@return query thread to run next */
que_thr_t*
row_purge_step(
/*===========*/
	que_thr_t*	thr)	/*!< in: query thread */
	MY_ATTRIBUTE((nonnull, warn_unused_result));

/** Info required to purge a record */
struct trx_purge_rec_t
{
  /** Record to purge */
  trx_undo_rec_t *undo_rec;
  /** File pointer to undo record */
  roll_ptr_t roll_ptr;
};

/* Purge node structure */

struct purge_node_t{
	que_common_t	common;	/*!< node type: QUE_NODE_PURGE */
	/*----------------------*/
	/* Local storage for this graph node */
	roll_ptr_t	roll_ptr;/* roll pointer to undo log record */

	undo_no_t	undo_no;/*!< undo number of the record */

	ulint		rec_type;/*!< undo log record type: TRX_UNDO_INSERT_REC,
				... */
private:
	/** latest unavailable table ID (do not bother looking up again) */
	table_id_t	unavailable_table_id;
	/** the latest modification of the table definition identified by
	unavailable_table_id, or TRX_ID_MAX */
	trx_id_t	def_trx_id;
public:
	dict_table_t*	table;	/*!< table where purge is done */

	ulint		cmpl_info;/* compiler analysis info of an update */

	upd_t*		update;	/*!< update vector for a clustered index
				record */
	const dtuple_t*	ref;	/*!< NULL, or row reference to the next row to
				handle */
	dtuple_t*	row;	/*!< NULL, or a copy (also fields copied to
				heap) of the indexed fields of the row to
				handle */
	dict_index_t*	index;	/*!< NULL, or the next index whose record should
				be handled */
	mem_heap_t*	heap;	/*!< memory heap used as auxiliary storage for
				row; this must be emptied after a successful
				purge of a row */
	ibool		found_clust;/*!< whether the clustered index record
				determined by ref was found in the clustered
				index, and we were able to position pcur on
				it */
	btr_pcur_t	pcur;	/*!< persistent cursor used in searching the
				clustered index record */
#ifdef UNIV_DEBUG
	/** whether the operation is in progress */
	bool		in_progress;
#endif
	trx_id_t	trx_id;	/*!< trx id for this purging record */

	/** meta-data lock for the table name */
	MDL_ticket*		mdl_ticket;

	/** table id of the previous undo log record */
	table_id_t		last_table_id;

	/** purge thread */
	THD*			purge_thd;

	/** metadata lock holds for this number of undo log recs */
	int			mdl_hold_recs;

	/** Undo recs to purge */
	std::queue<trx_purge_rec_t>	undo_recs;

	/** Constructor */
	explicit purge_node_t(que_thr_t* parent) :
		common(QUE_NODE_PURGE, parent),
		unavailable_table_id(0),
		table(NULL),
		heap(mem_heap_create(256)),
#ifdef UNIV_DEBUG
		in_progress(false),
#endif
		mdl_ticket(NULL),
		last_table_id(0),
		purge_thd(NULL),
		mdl_hold_recs(0)
	{
	}

#ifdef UNIV_DEBUG
	/***********************************************************//**
	Validate the persisent cursor. The purge node has two references
	to the clustered index record - one via the ref member, and the
	other via the persistent cursor.  These two references must match
	each other if the found_clust flag is set.
	@return true if the persistent cursor is consistent with
	the ref member.*/
	bool validate_pcur();
#endif

	/** Determine if a table should be skipped in purge.
	@param[in]	table_id	table identifier
	@return	whether to skip the table lookup and processing */
	bool is_skipped(table_id_t id) const
	{
		return id == unavailable_table_id && trx_id <= def_trx_id;
	}

	/** Remember that a table should be skipped in purge.
	@param[in]	id	table identifier
	@param[in]	limit	last transaction for which to skip */
	void skip(table_id_t id, trx_id_t limit)
	{
		DBUG_ASSERT(limit >= trx_id);
		unavailable_table_id = id;
		def_trx_id = limit;
	}

  /** Start processing an undo log record. */
  inline void start();


  /** Close the existing table and release the MDL for it. */
  void close_table()
  {
    last_table_id= 0;
    if (!table)
    {
      ut_ad(!mdl_ticket);
      return;
    }

    innobase_reset_background_thd(purge_thd);
    dict_table_close(table, false, purge_thd, mdl_ticket);
    table= nullptr;
    mdl_ticket= nullptr;
  }


  /** Retail mdl for the table id.
  @param[in]	table_id	table id to be processed
  @return true if retain mdl */
  bool retain_mdl(table_id_t table_id)
  {
    ut_ad(table_id);
    if (last_table_id == table_id && mdl_hold_recs < 100)
    {
      ut_ad(table);
      mdl_hold_recs++;
      return true;
    }

    mdl_hold_recs= 0;
    close_table();
    return false;
  }


  /** Reset the state at end
  @return the query graph parent */
  inline que_node_t *end();
};

#endif
