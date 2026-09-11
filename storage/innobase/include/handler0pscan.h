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

/** @file include/handler0pscan.h
Handler-side state and logic of an InnoDB parallel scan.

A scan has two sides, and a handler is only ever on one of them, so there is a
class per side: Parallel_scan_coordinator on the handler the plan's driving
table was opened on, Parallel_scan_worker on the private handler each worker
thread opens from the shared TABLE_SHARE. Both are extensions of ha_innobase -
they hold a back-pointer to their handler and read through its prebuilt, TABLE
and index cursor - and both are allocated only once a parallel scan actually
starts, so a handler that never takes part in one carries two null pointers.

Included from ha_innodb.h; relies on handler.h (Dynamic_array,
KEY_MULTI_RANGE, key_range) already being visible there. */

#ifndef handler0pscan_h
#define handler0pscan_h

#include "row0pcoord.h"

class ha_innobase;

/** Parallel scan parameters recorded by Parallel_scan_coordinator::init()
and then copied to each Parallel_scan_worker.

The ranges are allocated in the coordinator's range heap, which its end()
frees only after every worker has been joined. */
struct Pscan_params
{
	/** The scanned key intervals, indexed the same way as the
	coordinator's scan ids, or NULL for a full scan. Used to re-arm the
	lower bound whenever a worker moves to a chunk of another interval. */
	const KEY_MULTI_RANGE *m_ranges{};
	uint m_n_ranges{};

	/** table->key_info[] number of the index being scanned in parallel.
	MAX_KEY means the clustered index. */
	uint m_keynr{MAX_KEY};
};

/** The coordinator side of a parallel scan: the extension of the ha_innobase
handler used by the master thread of a parallel scan.

It is responsible for:
- partitioning the requested scan of an index or a table into a set of chunks;
- dispatching the queue of chunks;
- handing chunks from the queue to the workers.

The class supports:
- full table/index scans (a table in InnoDB is stored as the clustered index),
- scans on key intervals (ranges), both single- and multiple-range,
- scans on the primary key (clustered index),
- scans on regular secondary indexes (with some exceptions like
                                      R-Tree indexes or descending).

The class is an extension of ha_innobase: it holds a back-pointer to
the parent ha_innobase and uses parent's methods and data members.
*/
class Parallel_scan_coordinator
{
public:
	Parallel_scan_coordinator(ha_innobase *owner) : m_owner(owner) {}

	/** The chunk bookkeeping and the range heap must not outlive the
	handler they were built for, whatever teardown path got us here.
	end() is idempotent. */
	~Parallel_scan_coordinator() { end(); }

	/** Partition the requested index for n_threads workers.
	@param n_threads  number of workers the SQL layer will start
	@param keynr      table->key_info[] number, or MAX_KEY for the
	                  clustered index
	@param ranges     key intervals to scan, empty for a full scan
	@return           0, or HA_ERR_*. HA_ERR_UNSUPPORTED signals the requested
	                  scan cannot be handled in parallel */
	int init(size_t n_threads, uint keynr,
		 const Dynamic_array<KEY_MULTI_RANGE> &ranges);

	/** Release the chunks and the range heap. Safe to call twice, and
	safe to call on a coordinator that never got as far as init(). */
	int end();

	Parallel_worker_ctx *get_worker_context(size_t worker_idx)
	{
		return m_coordinator.get_worker_ctx(worker_idx);
	}

	void get_chunk_stats(ulonglong *chunks_created,
			     ulonglong *chunks_resplit) const
	{
		m_coordinator.get_chunk_stats(chunks_created, chunks_resplit);
	}

	/** What a worker has to adopt before it can read a chunk. */
	const Pscan_params &params() const { return m_params; }

private:
	/** Resolve the index a parallel scan was asked for and check that the
	partitioner can handle it.
	@param keynr  table->key_info[] number, or MAX_KEY for the clustered
	              index
	@return the index, or NULL if it does not exist or is not supported -
	        the caller declines the scan and the serial path runs instead */
	dict_index_t *resolve_index(uint keynr);

	/** Convert one MySQL-format key endpoint into an InnoDB tuple.
	@param kr       endpoint, or NULL for an unbounded one
	@param index    index the key belongs to
	@param heap     heap to allocate the tuple and its data from
	@return the tuple, or NULL if kr was NULL */
	dtuple_t *convert_key(const key_range *kr, const dict_index_t *index,
			      mem_heap_t *heap);

	/** The handler this instance belongs to. */
	ha_innobase *const m_owner;

	/** What init() decided; handed to every worker. */
	Pscan_params m_params;

	/** The partitioner proper: cuts the index into chunks and serves them
	to the workers. Distinct from this class, which is only its
	handler-side half. */
	Parallel_coordinator m_coordinator;

	/** Holds the key tuples handed to m_coordinator for a range scan, and
	the copy of the intervals m_params points into. */
	mem_heap_t *m_range_heap{};
};


/** The worker side of a parallel scan: the extension of the ha_innobase
handler used by a worker thread of a parallel scan.

It is responsible for:
- taking chunks from the coordinator's queue;
- reading the rows of a chunk, one at a time;
- keeping the read within the chunk boundaries;
- restoring the handler state when the scan ends.

The class supports:
- chunks of a full table/index scan and chunks of key intervals,
- inclusive and exclusive interval bounds,
- moving between chunks that belong to different intervals.
*/
class Parallel_scan_worker
{
public:
	Parallel_scan_worker(ha_innobase *owner) : m_owner(owner) {}

	~Parallel_scan_worker() { end(); }

	/** Take the first chunk and get ready to read it.
	@param wctx    this worker's context, from
	               Parallel_scan_coordinator::get_worker_context()
	@param params  the coordinator's scan parameters. m_keynr says which
	               index the chunk boundaries were computed on, so it
	               decides which index this handler must open; without it
	               the worker would search the wrong tree - MAX_KEY, the
	               clustered index - with the right boundaries.
	@return 0, HA_ERR_END_OF_FILE if no chunk was left, or an error */
	int init(Parallel_worker_ctx *wctx, const Pscan_params &params);

	int get_next_row(Parallel_worker_ctx *wctx);

	/** Close the index cursor, unclamp the scan and drop the borrowed
	parameters. Safe to call twice. */
	int end();

private:
	/** Lower bound of interval 'scan_id', or NULL if it is unbounded or
	this is a full table scan. The upper bound needs no handler-side state:
	it is enforced by the chunk clamp inside row_search_mvcc(). */
	const key_range *get_start_key(size_t scan_id) const;

	/** Prepare to read the chunk wctx has just picked up. */
	void begin_chunk(Parallel_coordinator::Worker_ctx *wctx);

	/** Whether the row in table->record[0] falls short of the lower bound
	of the interval wctx is currently reading. Clears wctx->m_check_start
	once a row clears the bound. */
	bool before_range_start(Parallel_coordinator::Worker_ctx *wctx);

	/** The bound to open 'chunk' on so that an exclusive lower bound costs
	no wasted reads, or NULL to open on the chunk's own first record. */
	const dtuple_t *exclusive_start(
		const Parallel_coordinator::Exec_ctx &chunk) const;

	/** The handler this instance belongs to. */
	ha_innobase *const m_owner;

	/** Borrowed from the coordinator by init(); never freed here. */
	Pscan_params m_params;

	/** The handler's own search tuple, displaced for as long as this scan
	lasts by the chunk boundary keys get_next_row() searches on. */
	dtuple_t *m_saved_search_tuple{};
};

#endif /* handler0pscan_h */
