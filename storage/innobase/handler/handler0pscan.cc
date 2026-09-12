/*****************************************************************************

Copyright (c) 2026, MariaDB

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

/** @file handler/handler0pscan.cc
Handler-side logic of an InnoDB parallel scan: Parallel_scan_coordinator,
Parallel_scan_worker, and the ha_innobase parallel_*() overrides that own
them. */

#include "univ.i"
#include <sql_class.h>
#include <key.h>

#include "ha_prototypes.h"
#include "ha_innodb.h"
#include "handler0pscan.h"

#include "dict0dict.h"
#include "row0merge.h"
#include "row0mysql.h"
#include "row0sel.h"
#include "trx0trx.h"
#include "ut0new.h"

namespace {
/* Expose the handler's stats counters to the reads done inside this scope.
A copy of the identical file-local helper in ha_innodb.cc: it is deliberately
not shared through a header - two small definitions beat widening the
interface between the two files. Keep them in step. */
struct mariadb_set_stats
{
  trx_t *const trx;
  mariadb_set_stats(trx_t *trx, ha_handler_stats *stats) : trx(trx)
  { trx->active_handler_stats= stats && stats->active ? stats : nullptr; }
  ~mariadb_set_stats() { trx->active_handler_stats= nullptr; }
};
}

int Parallel_scan_coordinator::init(size_t n_threads, uint keynr,
					    const Dynamic_array<KEY_MULTI_RANGE> &ranges)
{
	row_prebuilt_t*	prebuilt = m_owner->m_prebuilt;

	/* Reset any state left by a prior execution (correlateds subquery
	re-execution, stored procedure loop, etc.) before initializing fresh. */
	(void) end();

	/* Parallel scan performs consistent (non-locking) reads only. If this
	statement requires row locks, decline so the caller falls back to a
	serial scan that acquires them. */
	if (prebuilt->select_lock_type != LOCK_NONE)
		return HA_ERR_UNSUPPORTED;

	/* ROW_TYPE_REDUNDANT uses the old (pre-COMPACT) record header layout;
	the partitioner uses rec_get_node_ptr_flag() which assumes the new-style
	status byte. Decline so REDUNDANT tables take the serial path.*/
	if (dict_tf_get_rec_format(prebuilt->table->flags) ==
													REC_FORMAT_REDUNDANT) {
		return HA_ERR_UNSUPPORTED;
	}

	dict_index_t *index = resolve_index(keynr);

	if (index == NULL) {
		return HA_ERR_UNSUPPORTED;
	}

	/* Refuse the scan if the tablespace is discarded or unreadable. */
	if (!prebuilt->table->space) {
		ib_senderrf(m_owner->m_user_thd, IB_LOG_LEVEL_ERROR,
			    ER_TABLESPACE_DISCARDED,
			    m_owner->table->s->table_name.str);
		return HA_ERR_TABLESPACE_MISSING;
	}
	if (!prebuilt->table->is_readable()) {
		ib_senderrf(m_owner->m_user_thd, IB_LOG_LEVEL_ERROR,
			    ER_TABLESPACE_MISSING,
			    m_owner->table->s->table_name.str);
		return HA_ERR_TABLESPACE_MISSING;
	}

	/* Open the transaction's ReadView now, before any worker runs.
	  A serial rnd_next() opens it lazily inside row_search_mvcc(). For
	  parallel scan, an empty partition (no rows) skips row_search_mvcc()
	  entirely, leaving the transaction with no snapshot — a later SELECT
	  in the same RR transaction would then see uncommitted (to us) writes
	  from other transactions. Open the ReadView here to match serial
	  semantics.

	  This view is also the one the workers adopt (see
	  innobase_clone_consistent_snapshot): they read in their own
	  transactions, so the snapshot the chunk boundaries below are computed
	  from has to exist before any of them starts.

	  open() is a no-op if already open. */
	if (prebuilt->select_lock_type == LOCK_NONE) {
		trx_start_if_not_started(prebuilt->trx, false);
		prebuilt->trx->read_view.open(prebuilt->trx);
	}

	m_partitioner.initialize(n_threads);

	/* One context per worker, for the SQL layer to collect and hand back
	to the worker it belongs to. Each names this coordinator, which is how
	the worker finds the scan it is joining: its own handler knows nothing
	of the scan, and the context is the only thing it is given. */
	m_worker_ctxs.reserve(n_threads);
	for (size_t i = 0; i < n_threads; i++) {
		Pscan_worker_ctx *wctx = UT_NEW_NOKEY(Pscan_worker_ctx());
		if (wctx == nullptr)
			return HA_ERR_OUT_OF_MEM;
		wctx->m_coord = this;
		m_worker_ctxs.push_back(wctx);
	}

	dberr_t err = DB_SUCCESS;
	m_params.m_keynr = keynr;

	if (ranges.size() == 0) {
		/* Already cleared by the end() above;
		repeated here so both branches state what they leave the
		interval bookkeeping in. */
		m_params.m_ranges = nullptr;
		m_params.m_n_ranges = 0;

		const Parallel_scan_partitioner::Scan_range FULL_SCAN;
		err = m_partitioner.add_scan(
			prebuilt->trx,
			Parallel_scan_partitioner::Config(FULL_SCAN, index));
	} else {
		const size_t n_ranges = ranges.size();

		const ulint INITIAL_HEAP_SIZE = 1000;
		m_range_heap = mem_heap_create(INITIAL_HEAP_SIZE);

		KEY_MULTI_RANGE* saved = static_cast<KEY_MULTI_RANGE*>(
				mem_heap_alloc(m_range_heap,
							   n_ranges * sizeof(KEY_MULTI_RANGE)));
		memcpy(saved, ranges.front(),
		       n_ranges * sizeof(KEY_MULTI_RANGE));
		m_params.m_ranges = saved;
		m_params.m_n_ranges = (uint) n_ranges;

		for (size_t i = 0; i < n_ranges && err == DB_SUCCESS; i++) {
			/* keypart_map == 0 means the endpoint is unbounded;
			this is the MRR convention, see handler::multi_range_read_next().*/
			const key_range* min_key =
				ranges.at(i).start_key.keypart_map
				? &ranges.at(i).start_key : nullptr;
			const key_range* max_key =
				ranges.at(i).end_key.keypart_map
				? &ranges.at(i).end_key : nullptr;

			dtuple_t* start = convert_key(
				min_key, index, m_range_heap);
			dtuple_t* end = convert_key(
				max_key, index, m_range_heap);

			/* HA_READ_AFTER_KEY means "up to and including this key",
			see handler::set_end_range(). */
			const bool end_inclusive =
				max_key != nullptr && max_key->flag == HA_READ_AFTER_KEY;

			Parallel_scan_partitioner::Scan_range scan_range(
				start, end, end_inclusive);
			err = m_partitioner.add_scan(
				prebuilt->trx,
				Parallel_scan_partitioner::Config(
					scan_range, index));
		}
	}

	if (err != DB_SUCCESS) {
		return convert_error_code_to_mysql(err, prebuilt->table->flags,
						   m_owner->m_user_thd);
	}

	return 0;
}

dict_index_t* Parallel_scan_coordinator::resolve_index(uint keynr)
{
	/* keynr indexes table->key_info[].
	MAX_KEY asks for the clustered index, generated or not. */
	dict_index_t *index = m_owner->innobase_get_index(keynr);

	if (index == NULL)
		return NULL;

	if (index->is_corrupted()
	    || !row_merge_is_index_usable(m_owner->m_prebuilt->trx, index)) {
		/* Being built by online DDL, or flagged corrupt. Unlike the
		clustered index of an open table, a secondary index can be in
		this state on its own. */
		return NULL;
	}

	/* Index kinds the partitioner cannot reason about:
	  - spatial: non-leaf records hold MBRs;
	  - FTS: not a regular B+Tree of table rows;
	  - virtual columns: records hold computed values which have to be
	    validated against the clustered row before use. */
	if (index->is_spatial() || (index->type & DICT_FTS)
	    || index->has_virtual()) {
		return NULL;
	}

	/* The partitioner and the end-of-chunk check (cmp_dtuple_rec(...) <= 0)
	assume ascending physical key order. A descending key column reverses
	B+Tree traversal, breaking range boundaries, so decline. */
	for (unsigned i = dict_index_get_n_unique_in_tree(index); i--; ) {
		if (index->fields[i].descending) {
			return NULL;
		}
	}

	return index;
}

const key_range* Parallel_scan_worker::get_start_key(size_t scan_id) const
{
	/* Scan ids and m_params.m_ranges positions are the same sequence:
	Parallel_scan_coordinator::init() calls add_scan() once per interval,
	in order, on a coordinator whose scan id counter starts at 0. */
	ut_ad(m_params.m_n_ranges == 0 || scan_id < m_params.m_n_ranges);

	if (scan_id >= m_params.m_n_ranges) {
		return NULL;			/* full table scan */
	}

	/* keypart_map == 0 means the endpoint is unbounded; this is the MRR
	convention, see handler::multi_range_read_next(). */
	const KEY_MULTI_RANGE& r = m_params.m_ranges[scan_id];
	return r.start_key.keypart_map ? &r.start_key : NULL;
}

void Parallel_scan_worker::begin_chunk(Pscan_worker_ctx *wctx)
{
	wctx->m_first_call = true;

	const key_range* start_key =
		get_start_key(wctx->m_exec_ctx->scan_id());
	wctx->m_check_start = start_key != NULL;

	if (start_key) {
		/* before_range_start() compares table->record[0] against
		the key through range_key_part. Intervals are only ever handed
		to us naming a real key, so m_params.m_keynr is not MAX_KEY here -
		unlike a full scan of a table whose clustered index was
		auto-generated, where there is no MySQL key to point at. */
		ut_ad(m_params.m_keynr < MAX_KEY);
		m_owner->range_key_part =
			m_owner->table->key_info[m_params.m_keynr].key_part;
	}
}

bool Parallel_scan_worker::before_range_start(
	Pscan_worker_ctx *wctx)
{
	/* A chunk is entered with PAGE_CUR_GE, which cannot express an
	exclusive lower bound ("a > 5"). Skip such rows rather than hand them
	up: get_next_row() must only return rows inside the
	interval. */
	const key_range* start_key =
		get_start_key(wctx->m_exec_ctx->scan_id());
	const int cmp = key_cmp(m_owner->range_key_part, start_key->key,
				start_key->length);

	if (cmp < 0
	    || (cmp == 0 && start_key->flag == HA_READ_AFTER_KEY)) {
		return true;
	}

	/* rows are ordered, so the flag can be safely reset after
	the first row is checked */
	wctx->m_check_start = false;
	return false;
}

const dtuple_t *Parallel_scan_worker::exclusive_start(
	const Parallel_scan_partitioner::Exec_ctx &exec_ctx) const
{
	/* A chunk is opened at its own first record, and that record belongs to
	the chunk, so the open has to be PAGE_CUR_GE. That cannot express an
	exclusive lower bound: for "a > 5" the partitioner's first chunk begins
	at the first record with a = 5, and before_range_start() then
	throws away every record sharing that value - the bound's whole
	multiplicity, which on a low-cardinality column is most of a chunk.
	A serial scan never reads them: convert_search_mode_to_innobase() turns
	HA_READ_AFTER_KEY into PAGE_CUR_G and the descent lands past them.

	So do the same here, for as long as the chunk still holds the bound. */
	const key_range *start_key = get_start_key(exec_ctx.scan_id());
	if (start_key == nullptr || start_key->flag != HA_READ_AFTER_KEY) {
		return nullptr;
	}

	// Start bound as requested by the caller, not the actual chunk start:
	const dtuple_t *start_bound = exec_ctx.scan_start();
	if (!start_bound)
		return nullptr;

	const dtuple_t *chunk_start = exec_ctx.m_range.first->m_tuple;
	const ulint n_cmp = dtuple_get_n_fields_cmp(start_bound);

	ut_ad(n_cmp > 0);
	ut_ad(n_cmp <= dtuple_get_n_fields(chunk_start));

	for (ulint i = 0; i < n_cmp; i++) {
		const int cmp = cmp_dfield_dfield(
			dtuple_get_nth_field(start_bound, i),
			dtuple_get_nth_field(chunk_start, i));

		if (cmp != 0) {
			/* cmp < 0: the chunk begins above the bound already. */
			return cmp < 0 ? nullptr : start_bound;
		}
	}

	/* Equal on every named field: this chunk begins inside the start bound's
	key value, so every record from here up to the first one above the bound
	is out of range and has to be skipped. Opening on the bound is what
	skips them. */
	return start_bound;
}

dtuple_t* Parallel_scan_coordinator::convert_key(const key_range *kr,
					     const dict_index_t *index,
					     mem_heap_t *heap)
{
	if (kr == nullptr) {
		return nullptr;			/* -/+ infinity */
	}

	row_prebuilt_t*	prebuilt = m_owner->m_prebuilt;

	/* Each endpoint gets its own scratch buffer: the resulting tuple's
	fields point into it, so the two srch_key_val buffers on prebuilt
	(which records_in_range() can reuse freely) would alias across
	intervals here. */
	byte* buf = static_cast<byte*>(
		mem_heap_alloc(heap, prebuilt->srch_key_val_len));

	ut_ad(m_params.m_keynr < MAX_KEY);
	const uint n_key_fields =
		m_owner->table->key_info[m_params.m_keynr].ext_key_parts;

	dtuple_t* tuple = dtuple_create(heap, n_key_fields);
	dict_index_copy_types(tuple, index, n_key_fields);

	row_sel_convert_mysql_key_to_innobase(tuple, buf,
					      prebuilt->srch_key_val_len,
					      const_cast<dict_index_t*>(index),
					      kr->key, kr->length);
	ut_ad(dtuple_get_n_fields(tuple) > 0);
	ut_ad(dtuple_get_n_fields(tuple) <= n_key_fields);
	return tuple;
}

int Parallel_scan_worker::init(Parallel_worker_ctx *wctx)
{
	/* This handler may still carry state from a previous execution of the
	same plan (correlated subquery, stored procedure loop, ...).
	Reset it before starting over */
	(void) end();

	auto worker_ctx= static_cast<Pscan_worker_ctx*>(wctx);
	DBUG_ASSERT(worker_ctx);
	ut_ad(worker_ctx->m_coord);
	m_coord = worker_ctx->m_coord;

	/* The parameters were recorded on the coordinator's handler; this one
	has never seen them. Take them before anything below reads
	m_params.m_keynr: it decides which index is opened, and the chunk
	boundaries this worker is about to be handed were computed on that
	index. The ranges are borrowed, not copied - they live in the
	coordinator's range heap, which it frees only after every worker has
	been joined. */
	m_params = m_coord->params();

	auto exec_ctx = m_coord->get_next_chunk();
	if (exec_ctx == nullptr)
          return HA_ERR_END_OF_FILE; // No more data

	/* Save the prebuilt-owned search_tuple: get_next_row() points
	  m_prebuilt->search_tuple at a chunk boundary key owned by the
	  coordinator, and end() has to put this one back before the
	  coordinator releases the chunks. */
	m_saved_search_tuple= m_owner->m_prebuilt->search_tuple;

	/* MAX_KEY means the clustered index, which may be auto-generated and so
	have no table->key_info[] entry for ha_index_init() to name. Any other
	value names a real key, clustered or not. */
	if (int err= m_params.m_keynr == MAX_KEY
		     ? m_owner->ha_rnd_init(/*scan*/ true)
		     : m_owner->ha_index_init(m_params.m_keynr, /*sorted*/ false))
		return err; // preserve HA_ERR_* (e.g. HA_ERR_TABLE_DEF_CHANGED)

	worker_ctx->m_exec_ctx= exec_ctx;
	ut_ad(exec_ctx->m_range.first->m_tuple != nullptr);
	begin_chunk(worker_ctx);

	return 0;
}


int Parallel_scan_worker::get_next_row(Parallel_worker_ctx *wctx)
{
	auto worker_ctx = static_cast<Pscan_worker_ctx*>(wctx);
	row_prebuilt_t*	prebuilt = m_owner->m_prebuilt;

	/* Loop: when a chunk is exhausted we pull the next chunk */
	for (;;) {
		const auto& chunk = *worker_ctx->m_exec_ctx;
		dberr_t err;
		{
			mariadb_set_stats temp(prebuilt->trx,
					       m_owner->handler_stats);

			if (worker_ctx->m_first_call) {
				worker_ctx->m_first_call = false;
				/* Clamp the scan to this chunk inside the engine, so
				the prefetch cache stops exactly at the boundary and
				never reads into the next chunk. NULL == +infinity. */
				prebuilt->pscan_chunk_clamp.reset_to(
					chunk.m_range.second->m_tuple,
					chunk.m_end_inclusive);

				const dtuple_t *open_tuple =
					chunk.m_range.first->m_tuple;
				page_cur_mode_t open_mode = PAGE_CUR_GE;

				if (open_tuple == NULL) {
					/* -infinity:
					empty (0-field) tuple + PAGE_CUR_G = first user record. */
					open_tuple = dtuple_create(prebuilt->heap, 0);
					open_mode = PAGE_CUR_G;
				} else if (const dtuple_t *start_bound =
					   exclusive_start(chunk)) {
					/* Skip the start bound's key value in the descent rather
					than reading it row by row. */
					open_tuple = start_bound;
					open_mode = PAGE_CUR_G;
				}

				// Position at the first record to read, AND load it.
				prebuilt->search_tuple =
					const_cast<dtuple_t *>(open_tuple);
				err = row_search_mvcc(m_owner->table->record[0],
						      open_mode,
						      prebuilt, 0, 0 /*opening*/);
			}
			else {
				// Continuation: advance from stored position.
				err= row_search_mvcc(m_owner->table->record[0],
						     PAGE_CUR_UNSUPP,
						     prebuilt, 0, ROW_SEL_NEXT);
			}
		} // <-- mariadb_set_stats destructor

		if (err == DB_SUCCESS) {
			/* Only rows inside the interval may be handed up. The
			lower bound needs checking because a chunk is entered
			with PAGE_CUR_GE. The upper bound is enforced by the chunk
			clamp inside row_search_mvcc(). */
			if (worker_ctx->m_check_start
			    && before_range_start(worker_ctx)) {
				continue;	/* next row in this chunk */
			}
			return 0;
		}

		/* DB_RECORD_NOT_FOUND is returned both at the chunk boundary
		(our clamp above) and at end of index; DB_END_OF_INDEX likewise.
		In all cases the current sub-range is exhausted: pull the next
		chunk and continue, or stop if there is none. */
		if (err != DB_RECORD_NOT_FOUND && err != DB_END_OF_INDEX)
			return convert_error_code_to_mysql(err, prebuilt->table->flags,
											   m_owner->m_user_thd);

		auto exec_ctx = m_coord->get_next_chunk();
		if (exec_ctx == nullptr)
			return HA_ERR_END_OF_FILE; // No more data

		worker_ctx->m_exec_ctx = exec_ctx;
		ut_ad(exec_ctx->m_range.first->m_tuple != nullptr);
		begin_chunk(worker_ctx);
		// loop: re-enter the search for the new chunk
	}
}

int Parallel_scan_worker::end()
{
	m_owner->ha_index_or_rnd_end();
	if (m_saved_search_tuple)
	{
		m_owner->m_prebuilt->search_tuple = m_saved_search_tuple;
		m_saved_search_tuple = nullptr;
	}
	m_owner->m_prebuilt->pscan_chunk_clamp.reset_to(nullptr, false);

	/* Drop what init() borrowed, so nothing points into the coordinator's
	range heap once this scan is over. */
	m_params = Pscan_params();
	m_coord = nullptr;
	return 0;
}

int Parallel_scan_coordinator::end()
{
	for (Pscan_worker_ctx *wctx : m_worker_ctxs)
		UT_DELETE(wctx);
	m_worker_ctxs.clear();

	m_partitioner.cleanup();
	if (m_range_heap) {
		mem_heap_free(m_range_heap);
		m_range_heap = nullptr;
	}
	m_params = Pscan_params();
	return 0;
}


/*****************************************************************************
The handler API for parallel scans. ha_innobase carries one pointer per side
of a scan and allocates the object the first time the SQL layer asks it to
play that side, so a handler that never takes part in one costs two pointers.
*****************************************************************************/

int ha_innobase::parallel_init_coordinator(
	size_t n_threads, uint keynr,
	const Dynamic_array<KEY_MULTI_RANGE> &ranges)
{
	/* A handler is the driving table's or a worker's, never both. */
	ut_ad(!m_pscan_worker);

	if (!m_pscan_coord) {
		m_pscan_coord = UT_NEW_NOKEY(Parallel_scan_coordinator(this));
		if (!m_pscan_coord)
			return HA_ERR_OUT_OF_MEM;
	}

	const int err = m_pscan_coord->init(n_threads, keynr, ranges);

	if (err) {
		/* Declined (HA_ERR_UNSUPPORTED) or failed: the SQL layer runs
		the serial path and will not call parallel_end_coordinator(),
		so release it here. */
		UT_DELETE(m_pscan_coord);
		m_pscan_coord = nullptr;
	}

	return err;
}

int ha_innobase::parallel_end_coordinator()
{
	if (m_pscan_coord) {
		m_pscan_coord->end();
		UT_DELETE(m_pscan_coord);
		m_pscan_coord = nullptr;
	}
	return 0;
}

Parallel_worker_ctx *ha_innobase::parallel_get_worker_context(
	size_t worker_idx)
{
	/* Only ever asked of a handler parallel_init_coordinator() succeeded
	on, and only before the scan is torn down. */
	ut_ad(m_pscan_coord);
	return m_pscan_coord->get_worker_context(worker_idx);
}

void ha_innobase::parallel_get_chunk_stats(ulonglong *chunks_created,
					   ulonglong *chunks_resplit) const
{
	if (m_pscan_coord)
		m_pscan_coord->get_chunk_stats(chunks_created, chunks_resplit);
	else
		*chunks_created = *chunks_resplit = 0;
}

int ha_innobase::parallel_init_worker(Parallel_worker_ctx *wctx)
{
	/* A handler plays one side of a scan or the other, never both: the
	SQL layer opens a private TABLE, and so a private handler, per worker,
	and this is one of those. */
	ut_ad(!m_pscan_coord);

	if (!m_pscan_worker) {
		m_pscan_worker = UT_NEW_NOKEY(Parallel_scan_worker(this));
		if (!m_pscan_worker)
			return HA_ERR_OUT_OF_MEM;
	}

	return m_pscan_worker->init(wctx);
}

int ha_innobase::parallel_get_next_row(Parallel_worker_ctx *wctx)
{
	ut_ad(m_pscan_worker);
	return m_pscan_worker->get_next_row(wctx);
}

int ha_innobase::parallel_end_worker()
{
	if (m_pscan_worker) {
		m_pscan_worker->end();
		UT_DELETE(m_pscan_worker);
		m_pscan_worker = nullptr;
	}
	return 0;
}

void ha_innobase::parallel_scan_free()
{
	/* Last resort, for a handler torn down without the SQL layer having
	ended the scan. The worker is dropped without end(): that restores
	state in m_prebuilt, which close() has already freed by now, and the
	worker owns no memory of its own. The coordinator does own memory, and
	its destructor frees it without touching the handler. */
	UT_DELETE(m_pscan_worker);
	m_pscan_worker = nullptr;
	UT_DELETE(m_pscan_coord);
	m_pscan_coord = nullptr;
}
