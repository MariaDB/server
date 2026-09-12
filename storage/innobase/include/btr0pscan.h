/*****************************************************************************

Copyright (c) 2018, 2025, Oracle and/or its affiliates.
Copyright (c) 2026, MariaDB

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License, version 2.0, as published by the
Free Software Foundation.

This program is designed to work with certain software (including
but not limited to OpenSSL) that is licensed under separate terms,
as designated in a particular file or component or in included license
documentation.  The authors of MySQL hereby grant you an additional
permission to link the program and your derivative works with the
separately licensed software that they have either included with
the program or referenced in the documentation.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License, version 2.0,
for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc.,
51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA

*****************************************************************************/

/** @file include/btr0pscan.h
Parallel scan partitioner interface.

Based on MySQL commit dbfc59ffaf80 created 2018-01-27 by Sunny Bains. */

#ifndef btr0pscan_h
#define btr0pscan_h

#include <functional>
#include <vector>
#include <list>

#include "row0sel.h"
#include "btr0cur.h"
#include "db0err.h"
#include "fil0fil.h"
#include "rem0types.h"

/** The core idea is to find the path down the B+Tree to where the scan
starts. Follow the links at the appropriate btree level from there to the
right, splitting the scan on each of these sub-tree root nodes, and stop at
the first node pointer that is past the end of the scan.

If the user has set the maximum number of threads to use at say 4 threads
and there are 5 sub-trees at the selected level then we will split the 5th
sub-tree dynamically when it is ready for scan.

We want to allow several ranges to be divided at once. To achieve this
the scan context (Scan_ctx) is split from the unit of work (Chunk).
The Scan_ctx holds the index, the transaction and the range the caller
asked for; a Chunk is one piece of that range, described by its two
boundary keys. Chunks go on a queue, and a worker takes the next one
whenever it needs work.

To start a scan we need to instantiate a Parallel_scan_partitioner. A
partitioner can contain several Scan_ctx instances and a Scan_ctx is divided
into several Chunk instances. It's the Chunk instances that are handed
to the workers.

Each range to divide has to be added via add_scan(), which makes one Scan_ctx
of it. A multiple-range scan therefore holds several Scan_ctx instances, all
of them on the index the caller asked for.

To solve the imbalance problem we dynamically split the sub-trees as and
when required. e.g., If you have 5 sub-trees to scan and 4 threads then
it will tag the 5th sub-tree as m_to_be_resplit during phase I (add_scan()),
the worker that takes that Chunk off the queue will then dynamically split
the 5th sub-tree and add the newly created sub-trees to the Chunk run queue
in the Parallel_scan_partitioner, and pull again. As the other threads complete
their sub-tree scans they will pick up more Chunk instances from the
Parallel_scan_partitioner run queue and start scanning the sub-partitions as
normal.

Note: The Chunk instances form a chain: the start point of one is the end
point of the Chunk scanning values less than its start point. A Chunk
will scan from [Start, End) rows - except the last one of a range scan, which
takes End in too when the caller's own upper bound was inclusive. We use
std::shared_ptr to manage the reference counting, this allows us to dispose of
the Chunk instances without worrying about dangling pointers.

*/

// Forward declarations
struct trx_t;
struct mtr_t;
struct buf_block_t;
struct dict_table_t;

/** Page number */
typedef uint32_t page_no_t;

class Parallel_scan_partitioner
{
 public:
  // Forward declaration.
  class Chunk;
  class Scan_ctx;

  /** Specifies the range from where to start the scan and where to end it. */
  struct Scan_range
  {
    /** Default constructor. */
    Scan_range() : m_start(), m_end() {}

    /** Copy constructor.
    @param[in] scan_range       Instance to copy from. */
    Scan_range(const Scan_range &scan_range) = default;

    /** Constructor.
    @param[in] start            Start key
    @param[in] end              End key.
    @param[in] end_inclusive    Whether records equal to 'end' belong to the
                                scan. */
    Scan_range(const dtuple_t *start, const dtuple_t *end,
               bool end_inclusive= false)
        : m_start(start), m_end(end), m_end_inclusive(end_inclusive) {}

    /** Start of the scan, can be nullptr for -infinity. */
    const dtuple_t *m_start{};

    /** End of the scan, can be null for +infinity. */
    const dtuple_t *m_end{};

    /** Whether m_end itself is part of the scan.

    Chunks tile as [start, end): a chunk's start is inclusive and its end is
    exclusive - the same key is one chunk's end and the next chunk's start.
    A chunk end is therefore never inclusive, and this flag only describes the
    caller's own upper bound, so it can only be true for a range scan. A full
    table scan passes m_end == NULL. */
    bool m_end_inclusive{};

    /** Convert the instance to a string representation. */
    [[nodiscard]] std::string to_string() const;
  };

  /** Scan (Scan_ctx) configuration. */
  struct Config {
    /** Constructor.
    @param[in] scan_range     Range to scan.
    @param[in] index          Cluster index to scan.
    @param[in] read_level     Btree level from which records need to be
                              read. */
    Config(const Scan_range &scan_range, dict_index_t *index,
           uint16_t read_level = 0)
        : m_scan_range(scan_range),
          m_index(index),
          m_zip_size(index->table->space->zip_size()),
          m_read_level(read_level) {}

    /** Copy constructor.
    @param[in] config           Instance to copy from. */
    Config(const Config &config) = default;

    /** Range to scan. */
    const Scan_range m_scan_range;

    /** (Cluster) Index in table to scan. */
    dict_index_t *m_index{};

    /** Tablespace page size. */
    const ulint m_zip_size;

    /** Btree level from which records need to be read. */
    uint16_t m_read_level{0};
  };

  /** Constructor */
  Parallel_scan_partitioner() = default;

  /** Take the next chunk to scan off the queue, re-splitting chunks flagged
  for it until a scannable one turns up.
  @param[out] chunk  the chunk to scan, or nullptr when the scan is over.
                     Only meaningful when DB_SUCCESS is returned.
  @return DB_SUCCESS or the error that ended the scan. */
  [[nodiscard]] dberr_t get_next_chunk(std::shared_ptr<Chunk> *chunk);

  /** Initialization.
    @param[in]  n_workers Number of worker threads expected to be used
    @return 0 - SUCCESS, !=0 - error code
  */
  int initialize(size_t n_workers);

  /** Destroy after finished processing */
  void cleanup();

  /** Destructor. */
  ~Parallel_scan_partitioner()
  {
    cleanup();
  }

  /** Add scan context.
  @param[in,out]  trx         Covering transaction.
  @param[in]      config      Scan condfiguration.
  @return error. */
  [[nodiscard]] dberr_t add_scan(trx_t *trx, const Config &config);

  /** Get the error stored in the global error state.
  @return global error state. */
  [[nodiscard]] dberr_t get_error_state() const { return m_err; }

  /** @return the configured max threads size. */
  [[nodiscard]] size_t num_workers() const { return m_n_workers; }

  /** @return true if in error state. */
  [[nodiscard]] bool is_error_set() const {
    return m_err.load(std::memory_order_relaxed) != DB_SUCCESS;
  }

  /** Set the error state.
  @param[in] err                Error state to set to. */
  void set_error_state(dberr_t err) {
    m_err.store(err, std::memory_order_relaxed);
  }

  // Disable copying.
  Parallel_scan_partitioner(const Parallel_scan_partitioner &) = delete;
  Parallel_scan_partitioner(const Parallel_scan_partitioner &&) = delete;
  Parallel_scan_partitioner &operator=(Parallel_scan_partitioner &&) = delete;
  Parallel_scan_partitioner &operator=(const Parallel_scan_partitioner &) =
      delete;

 private:
  /** Add a chunk to the run queue.
  @param[in] chunk              Chunk to add to the queue. */
  void enqueue(std::shared_ptr<Chunk> chunk);

public:
  /** How the scan was divided; see m_chunks_created. Read after the workers
  have finished, so no locking. */
  void get_chunk_stats(ulonglong *created, ulonglong *resplit) const
  {
    *created= m_chunks_created;
    *resplit= m_chunks_resplit;
  }

private:

  /** Take the next chunk off the queue.
  @return the chunk, or nullptr if the queue is empty. */
  [[nodiscard]] std::shared_ptr<Chunk> dequeue();

 private:
  using Chunks =
      std::list<std::shared_ptr<Chunk>,
                ut_allocator<std::shared_ptr<Chunk>>>;

  using Scan_ctxs =
      std::list<std::shared_ptr<Scan_ctx>,
                ut_allocator<std::shared_ptr<Scan_ctx>>>;

  /** Number of worker threads expected to use. */
  size_t m_n_workers{};

  /** Indicates the status of the partitioner */
  bool m_is_initialized{false};

  /** Mutex protecting m_chunk_queue and m_n_resplitting. */
  mutable mysql_mutex_t m_mutex;

  /** Signalled when a chunk is added to m_chunk_queue and when a splitter
  retires. Paired with m_mutex. */
  mysql_cond_t m_cond;

  /** Chunks waiting to be scanned. */
  Chunks m_chunk_queue{};

  /** How many chunks this scan was divided into, counting the finer ones a
  re-split produced, and how many chunks were re-split rather than scanned.
  Reported by ANALYZE FORMAT=JSON. Protected by m_mutex. */
  size_t m_chunks_created{};
  size_t m_chunks_resplit{};

  /** Chunks taken off m_chunk_queue that are being re-partitioned and
  have not enqueued their sub-chunks yet. Protected by m_mutex. */
  size_t m_n_resplitting{};

  /** Scan contexts. */
  Scan_ctxs m_scan_ctxs{};

  /** Counter for allocating scan context IDs. */
  size_t m_scan_ctx_id{};

  /** Error during parallel read. */
  std::atomic<dberr_t> m_err{DB_SUCCESS};
};

/** Parallel scan partitioner context. */
class Parallel_scan_partitioner::Scan_ctx {
 public:
  /** Constructor.
  @param[in]  partitioner     Partitioner that owns this context.
  @param[in]  id              ID of this scan context.
  @param[in]  trx             Transaction covering the scan.
  @param[in]  config          Range scan config.
  @param[in]  f               Callback function. */
  Scan_ctx(Parallel_scan_partitioner *partitioner, size_t id, trx_t *trx,
           const Parallel_scan_partitioner::Config &config);

  /** Destructor. */
  ~Scan_ctx() = default;

  /** One cut point between chunks: a copy of the index record the cut falls
  on, taken so it outlives the page latch. An empty instance (m_tuple NULL)
  means -infinity as a start and +infinity as an end. */
  struct Boundary {
    /** Destructor. */
    ~Boundary();

    /** Heap used to allocate m_rec and m_tuple. */
    mem_heap_t *m_heap{};

    /** m_rec column offsets. */
    rec_offs *m_offsets{};

    /** The boundary record itself, raw data of the row. */
    const rec_t *m_rec{};

    /** Tuple representation inside m_rec, for two Boundary instances in a
    range m_tuple will be [first->m_tuple, second->m_tuple). */
    const dtuple_t *m_tuple{};
  };

  /** mtr_t savepoint. */
  using Savepoint = std::pair<ulint, buf_block_t *>;

  /** For releasing the S latches after processing the blocks. */
  using Savepoints = std::vector<Savepoint, ut_allocator<Savepoint>>;

  /** The two boundaries of one chunk: [first, second). */
  using Bounds =
      std::pair<std::shared_ptr<Boundary>, std::shared_ptr<Boundary>>;

  /** The chunk boundaries one partitioning produced, in key order. */
  using Bounds_list = std::vector<Bounds, ut_allocator<Bounds>>;

  /** @return the scan context ID. */
  [[nodiscard]] size_t id() const { return m_id; }

  /** Set the error state.
  @param[in] err                Error state to set to. */
  void set_error_state(dberr_t err) {
    m_err.store(err, std::memory_order_relaxed);
  }

  /** @return true if in error state. */
  [[nodiscard]] bool is_error_set() const {
    return m_err.load(std::memory_order_relaxed) != DB_SUCCESS;
  }

  /** @return the error that ended this scan, DB_SUCCESS if none. */
  [[nodiscard]] dberr_t get_error_state() const {
    return m_err.load(std::memory_order_relaxed);
  }

  /** Fetch a block from the buffer pool and acquire an S latch on it.
  @param[in]      page_id       Page ID.
  @param[in,out]  mtr           Mini-transaction covering the fetch.
  @param[in]      line          Line from where called.
  @return the block fetched from the buffer pool. */
  [[nodiscard]] buf_block_t *block_get_s_latched(const page_id_t &page_id,
                                                 mtr_t *mtr, size_t line) const;

  /** Partition the B+Tree for parallel read.
  @param[in] scan_range Range for partitioning.
  @param[in,out]  bounds_list   Chunk boundaries produced by the walk.
  @param[in] split_level  Sub-range required level (0 == root).
  @return the partition scan bounds_list. */
  dberr_t partition(const Scan_range &scan_range, Bounds_list &bounds_list,
                    size_t split_level);

  /** Find the page number of the node that contains the search key. If the
  key is null then we assume -infinity.
  @param[in]      block         Page to look in.
  @param[in]      key           Key of the first record in the range.
  @param[out]     page_no       The left child page number. FIL_NULL on
                                error.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t search(buf_block_t *block, const dtuple_t *key,
                               page_no_t *page_no) const;

  /** Traverse from given sub-tree page number to start of the scan range
  from the given page number.
  @param[in]      page_no       Page number of sub-tree.
  @param[in,out]  mtr           Mini-transaction.
  @param[in]      key           Key of the first record in the range.
  @param[in,out]  savepoints    Blocks S latched and accessed.
  @param[out]     cursor        The leaf node page cursor. Untouched on
                                error.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t start_range(page_no_t page_no, mtr_t *mtr,
                                    const dtuple_t *key,
                                    Savepoints &savepoints,
                                    page_cur_t *cursor) const;

  /** Add a range boundary at the cursor's record: close the range that is
  currently open, if any, and open a new one starting at that record. The
  newly opened range has no end until the next call, or until partition()
  stamps the scan's upper bound onto it.
  @param[in,out]  bounds_list   Boundaries collected so far; one is appended.
  @param[in,out]  leaf_page_cursor Leaf page cursor on the boundary record. */
  void add_boundary(Bounds_list &bounds_list,
                    page_cur_t &leaf_page_cursor) const;

  /** Find the subtrees to scan in a block.
  @param[in]      scan_range    Partition based on this scan range.
  @param[in]      page_no       Page to partition at if at required level.
  @param[in]      depth         Sub-range current level.
  @param[in]      split_level   Sub-range starting level (0 == root).
  @param[in,out]  bounds_list   Chunk boundaries produced by the walk.
  @param[in,out]  mtr           Mini-transaction */
  dberr_t create_bounds(const Scan_range &scan_range, page_no_t page_no,
                        size_t depth, const size_t split_level,
                        Bounds_list &bounds_list, mtr_t *mtr);

  /** Build a dtuple_t from rec_t.
  @param[in]      rec           Build the dtuple from this record.
  @param[in,out]  boundary      Build in this boundary. */
  void copy_row(const rec_t *rec, Boundary *boundary) const;

  /** Snapshot the record the cursor is on as a range boundary. The record is
  copied, so the result outlives the block latch.
  @param[in]      page_cursor   Leaf page cursor, on the boundary record
  @return the boundary; m_tuple is NULL if the page held no user record. */
  [[nodiscard]] std::shared_ptr<Boundary> snapshot_boundary(
      const page_cur_t &page_cursor) const;

  /** Create a chunk for one pair of boundaries and add it to
  the Parallel_scan_partitioner's run queue.
  @param[in] bounds             Boundaries of the chunk to create.
  @param[in] resplit            true if the sub-tree should be split further.
  @param[in] end_inclusive      true if records equal to the range's end
                                belong to it. Only ever true for the chunk
                                that ends at the caller's own upper bound.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t create_chunk(const Bounds &bounds, bool resplit,
                                     bool end_inclusive= false);

  /** Create the chunks, one per pair of boundaries.
  @param[in]  bounds_list   Boundaries the tree walk produced.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t create_chunks(const Bounds_list &bounds_list);

  /** @return the maximum number of worker thread configured. */
  [[nodiscard]] size_t num_workers() const {
     return m_partitioner->num_workers();
  }

  /** S lock the index. */
  void index_s_lock();

  /** S unlock the index. */
  void index_s_unlock();

  /** @return true if at least one thread owns the S latch on the index. */
  bool index_s_own() const {
    return m_s_locks.load(std::memory_order_acquire) > 0;
  }

 private:
  using Config = Parallel_scan_partitioner::Config;

  /** Context ID. */
  size_t m_id{std::numeric_limits<size_t>::max()};

  /** Parallel scan configuration. */
  Config m_config;

  /** Covering transaction. */
  trx_t *m_trx{};

  /** Depth of the Btree. */
  size_t m_depth{};

  /** The partitioner that owns this context. */
  Parallel_scan_partitioner *m_partitioner{};

  /** Error during parallel read. */
  mutable std::atomic<dberr_t> m_err{DB_SUCCESS};

  /** Number of threads that have S locked the index. */
  std::atomic_size_t m_s_locks{};

  friend class Parallel_scan_partitioner;

  Scan_ctx(Scan_ctx &&) = delete;
  Scan_ctx(const Scan_ctx &) = delete;
  Scan_ctx &operator=(Scan_ctx &&) = delete;
  Scan_ctx &operator=(const Scan_ctx &) = delete;
};

/** One chunk of a scan: the key interval a worker reads in one go. */
class Parallel_scan_partitioner::Chunk {
 public:
  /** Constructor.
  @param[in]    scan_ctx        Scan this chunk is a piece of.
  @param[in]    bounds          Boundaries the chunk covers. */
  Chunk(Scan_ctx *scan_ctx, const Scan_ctx::Bounds &bounds)
      : m_bounds(bounds), m_scan_ctx(scan_ctx) {}

  /** Destructor. */
  ~Chunk() = default;

 public:
  /** The scan ID of the scan context this belongs to. */
  [[nodiscard]] size_t scan_id() const { return m_scan_ctx->id(); }

  /** @return the covering transaction. */
  [[nodiscard]] const trx_t *trx() const { return m_scan_ctx->m_trx; }

  /** @return the index being scanned. */
  [[nodiscard]] const dict_index_t *index() const {
    return m_scan_ctx->m_config.m_index;
  }

  /** @return the scan's own lower bound, nullptr for -infinity.

  This is the caller's bound, not this chunk's start: the two coincide only
  for the chunk the bound falls in. */
  [[nodiscard]] const dtuple_t *scan_start() const {
    return m_scan_ctx->m_config.m_scan_range.m_start;
  }

  /** The boundaries of this chunk. */
  Scan_ctx::Bounds m_bounds{};

  /** Whether a record equal to m_bounds.second belongs to this chunk.

  Chunks tile as [start, end), so this is false for every chunk except the
  one ending at the caller's own inclusive upper bound. It reaches the row
  clamp through row_prebuilt_t::set_pscan_end_tuple(). */
  bool m_end_inclusive{};

  /** The scan this chunk is a piece of. */
  Scan_ctx *m_scan_ctx{};

private:
  /** Split the chunk into sub-ranges and add them to the run queue.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t split();

  /** @return true if in error state. */
  [[nodiscard]] bool is_error_set() const {
    return m_scan_ctx->m_partitioner->is_error_set() ||
           m_scan_ctx->is_error_set();
  }

  /** @return the error that ended the scan this chunk belongs to, whether it
  was recorded on the scan or on the partitioner. DB_SUCCESS if none. */
  [[nodiscard]] dberr_t get_error_state() const {
    const dberr_t err = m_scan_ctx->get_error_state();
    return err != DB_SUCCESS ? err
                             : m_scan_ctx->m_partitioner->get_error_state();
  }

 private:
  /** If true then re-split this chunk into smaller ones. */
  bool m_to_be_resplit{};

  friend class Parallel_scan_partitioner;
};

#endif /* !btr0pscan_h */
