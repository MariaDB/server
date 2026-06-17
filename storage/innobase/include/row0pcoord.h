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

/** @file include/row0pcoord.h
Parallel coordinator interface.

Based on MySQL commit dbfc59ffaf80 created 2018-01-27 by Sunny Bains. */

#ifndef row0par_coord_h
#define row0par_coord_h

#include <functional>
#include <vector>
#include <list>

#include "row0sel.h"
#include "btr0cur.h"
#include "db0err.h"
#include "fil0fil.h"
#include "rem0types.h"
#include "parallel_worker_ctx.h"

/** The core idea is to find the left and right paths down the B+Tree.These
paths correspond to the scan start and scan end search. Follow the links
at the appropriate btree level from the left to right and split the scan
on each of these sub-tree root nodes.

If the user has set the maximum number of threads to use at say 4 threads
and there are 5 sub-trees at the selected level then we will split the 5th
sub-tree dynamically when it is ready for scan.

We want to allow multiple parallel range scans on different indexes at the
same time. To achieve this split out the scan  context (Scan_ctx) from the
execution context (Exec_ctx). The Scan_ctx has the index  and transaction
information and the Exec_ctx keeps track of the cursor for a specific thread
during the scan.

To start a scan we need to instantiate a Parallel_coordinator. A parallel
coordinator can contain several Scan_ctx instances and a Scan_ctx can contain
several Exec_ctx instances. Its' the Exec_ctx instances that are
eventually executed.

This design allows for a single Parallel_coordinator to scan multiple indexes
at once.  Each index range scan has to be added via its add_scan() method.
This functionality is required to handle parallel partition scans because
partitions are separate indexes. This can be used to scan completely
different indexes and tables by one instance of a Parallel_coordinator.

To solve the imbalance problem we dynamically split the sub-trees as and
when required. e.g., If you have 5 sub-trees to scan and 4 threads then
it will tag the 5th sub-tree as "to_be_split" during phase I (add_scan()),
the first thread that finishes scanning the first set of 4 partitions will
then dynamically split the 5th sub-tree and add the newly created sub-trees
to the execution context (Ctx) run queue in the Parallel_coordinator. As the
other threads complete their sub-tree scans they will pick up more execution
contexts (Ctx) from the Parallel_coordinator run queue and start scanning the
sub-partitions as normal.

Note: The Exec_ctx instances are in a virtual list. Each Exec_ctx instance
has arange to scan. The start point of this range instance is the end point
of the Exec_ctx instance scanning values less than its start point. An Exec_ctx
will scan from [Start, End) rows. We use std::shared_ptr to manage the
reference counting, this allows us to dispose of the Exec_ctx instances
without worrying about dangling pointers.

NOTE: Secondary index scans are not supported currently. */

// Forward declarations
struct trx_t;
struct mtr_t;
struct btr_pcur_t;
struct buf_block_t;
struct dict_table_t;

/** Page number */
typedef uint32_t page_no_t;

class Parallel_coordinator
{
 public:
  // Forward declaration.
  class Exec_ctx;
  class Scan_ctx;
  struct Worker_ctx;

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
    @param[in] end              End key. */
    Scan_range(const dtuple_t *start, const dtuple_t *end)
        : m_start(start), m_end(end) {}

    /** Start of the scan, can be nullptr for -infinity. */
    const dtuple_t *m_start{};

    /** End of the scan, can be null for +infinity. */
    const dtuple_t *m_end{};

    /** Convert the instance to a string representation. */
    [[nodiscard]] std::string to_string() const;
  };

  /** Scan (Scan_ctx) configuration. */
  struct Config {
    /** Constructor.
    @param[in] scan_range     Range to scan.
    @param[in] index          Cluster index to scan.
    @param[in] read_level     Btree level from which records need to be read.
    @param[in] partition_id   Partition id if the index to be scanned.
                              belongs to a partitioned table. */
    Config(const Scan_range &scan_range, dict_index_t *index,
           uint16_t read_level = 0,
           size_t partition_id = std::numeric_limits<size_t>::max())
        : m_scan_range(scan_range),
          m_index(index),
          m_is_compact(dict_table_is_comp(index->table)),
          m_zip_size(index->table->space->zip_size()),
          m_read_level(read_level),
          m_partition_id(partition_id) {}

    /** Copy constructor.
    @param[in] config           Instance to copy from. */
    Config(const Config &config) = default;

    /** Range to scan. */
    const Scan_range m_scan_range;

    /** (Cluster) Index in table to scan. */
    dict_index_t *m_index{};

    /** Row format of table. */
    const bool m_is_compact{};

    /** Tablespace page size. */
    const ulint m_zip_size;

    /** Btree level from which records need to be read. */
    uint16_t m_read_level{0};

    /** Partition id if the index to be scanned belongs to a partitioned table,
    else std::numeric_limits<size_t>::max(). */
    size_t m_partition_id{std::numeric_limits<size_t>::max()};
  };

  struct Worker_ctx : public Parallel_worker_ctx
  {
    Worker_ctx(size_t idx, Parallel_coordinator *pc)
      : m_worker_idx(idx), m_pcoordinator(pc) {}

    size_t m_worker_idx;
    Parallel_coordinator *m_pcoordinator;
    std::shared_ptr<Parallel_coordinator::Exec_ctx> m_exec_ctx{};
  };

  /** Constructor */
  Parallel_coordinator() = default;

  /** @return pre-allocated worker context for the given worker index. */
  Worker_ctx *get_worker_ctx(size_t worker_idx) const;

  std::shared_ptr<Parallel_coordinator::Exec_ctx>
  get_job_for_worker(Worker_ctx *wctx);

  /** Initialization.
    @param[in]  n_workers Number of worker threads expected to be used
    @return 0 - SUCCESS, !=0 - error code
  */
  int initialize(size_t n_workers);

  /** Destroy after finished processing */
  void cleanup();

  /** Destructor. */
  ~Parallel_coordinator()
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
  Parallel_coordinator(const Parallel_coordinator &) = delete;
  Parallel_coordinator(const Parallel_coordinator &&) = delete;
  Parallel_coordinator &operator=(Parallel_coordinator &&) = delete;
  Parallel_coordinator &operator=(const Parallel_coordinator &) = delete;

 private:
  /** Add an execution context to the run queue.
  @param[in] ctx                Execution context to add to the queue. */
  void enqueue(std::shared_ptr<Exec_ctx> ctx);

  /** Fetch the next job execute.
  @return job to execute or nullptr. */
  [[nodiscard]] std::shared_ptr<Exec_ctx> dequeue();

 private:
  using Exec_ctxs =
      std::list<std::shared_ptr<Exec_ctx>,
                ut_allocator<std::shared_ptr<Exec_ctx>>>;

  using Scan_ctxs =
      std::list<std::shared_ptr<Scan_ctx>,
                ut_allocator<std::shared_ptr<Scan_ctx>>>;

  /** Number of worker threads expected to use. */
  size_t m_n_workers{};

  /** Indicates the status of the coordinator */
  bool m_is_initialized{false};

  /** Mutex protecting m_ctxs. */
  mutable mysql_mutex_t m_mutex;

  /** Contexts that must be executed. */
  Exec_ctxs m_ctxs{};

  /** Scan contexts. */
  Scan_ctxs m_scan_ctxs{};

  /** Counter for allocating scan context IDs. */
  size_t m_scan_ctx_id{};

  /** Context ID. Monotonically increasing ID. */
  std::atomic_size_t m_ctx_id{};

  /** Error during parallel read. */
  std::atomic<dberr_t> m_err{DB_SUCCESS};

  /** Per-worker context exposed to the SQL layer via the handler API.
  Pre-allocated in initialize(N) and freed in cleanup(). */
  std::vector<Worker_ctx *, ut_allocator<Worker_ctx *>> m_worker_ctxs;
};

/** Parallel coordinator context. */
class Parallel_coordinator::Scan_ctx {
 public:
  /** Constructor.
  @param[in]  coordinator     Parallel coordinator that owns this context.
  @param[in]  id              ID of this scan context.
  @param[in]  trx             Transaction covering the scan.
  @param[in]  config          Range scan config.
  @param[in]  f               Callback function. */
  Scan_ctx(Parallel_coordinator *coordinator, size_t id, trx_t *trx,
           const Parallel_coordinator::Config &config);

  /** Destructor. */
  ~Scan_ctx() = default;

  /** Boundary of the range to scan. */
  struct Iter {
    /** Destructor. */
    ~Iter();

    /** Heap used to allocate m_rec, m_tuple and m_pcur. */
    mem_heap_t *m_heap{};

    /** m_rec column offsets. */
    rec_offs *m_offsets{};

    /** Start scanning from this key. Raw data of the row. */
    const rec_t *m_rec{};

    /** Tuple representation inside m_rec, for two Iter instances in a range
    m_tuple will be [first->m_tuple, second->m_tuple). */
    const dtuple_t *m_tuple{};

    /** Persistent cursor.*/
    btr_pcur_t *m_pcur{};
  };

  /** mtr_t savepoint. */
  using Savepoint = std::pair<ulint, buf_block_t *>;

  /** For releasing the S latches after processing the blocks. */
  using Savepoints = std::vector<Savepoint, ut_allocator<Savepoint>>;

  /** The first cursor should read up to the second cursor [f, s). */
  using Range = std::pair<std::shared_ptr<Iter>, std::shared_ptr<Iter>>;

  using Ranges = std::vector<Range, ut_allocator<Range>>;

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

  /** Fetch a block from the buffer pool and acquire an S latch on it.
  @param[in]      page_id       Page ID.
  @param[in,out]  mtr           Mini-transaction covering the fetch.
  @param[in]      line          Line from where called.
  @return the block fetched from the buffer pool. */
  [[nodiscard]] buf_block_t *block_get_s_latched(const page_id_t &page_id,
                                                 mtr_t *mtr, size_t line) const;

  /** Partition the B+Tree for parallel read.
  @param[in] scan_range Range for partitioning.
  @param[in,out]  ranges        Ranges to scan.
  @param[in] split_level  Sub-range required level (0 == root).
  @return the partition scan ranges. */
  dberr_t partition(const Scan_range &scan_range, Ranges &ranges,
                    size_t split_level);

  /** Find the page number of the node that contains the search key. If the
  key is null then we assume -infinity.
  @param[in]  block             Page to look in.
  @param[in] key                Key of the first record in the range.
  @param[in,out]  err           Error code.
  @return the left child page number. */
  [[nodiscard]] page_no_t search(buf_block_t *block,
                                 const dtuple_t *key,
                                 dberr_t *err) const;

  /** Traverse from given sub-tree page number to start of the scan range
  from the given page number.
  @param[in]      page_no       Page number of sub-tree.
  @param[in,out]  mtr           Mini-transaction.
  @param[in]      key           Key of the first record in the range.
  @param[in,out]  savepoints    Blocks S latched and accessed.
  @return the leaf node page cursor. */
  [[nodiscard]] page_cur_t start_range(page_no_t page_no, mtr_t *mtr,
                                       const dtuple_t *key,
                                       Savepoints &savepoints,
                                       dberr_t *err) const;

  /** Create and add the range to the scan ranges.
  @param[in,out]  ranges        Ranges to scan.
  @param[in,out]  leaf_page_cursor Leaf page cursor on which to create the
                                persistent cursor.
  @param[in,out]  mtr           Mini-transaction */
  void create_range(Ranges &ranges, page_cur_t &leaf_page_cursor,
                    mtr_t *mtr) const;

  /** Find the subtrees to scan in a block.
  @param[in]      scan_range    Partition based on this scan range.
  @param[in]      page_no       Page to partition at if at required level.
  @param[in]      depth         Sub-range current level.
  @param[in]      split_level   Sub-range starting level (0 == root).
  @param[in,out]  ranges        Ranges to scan.
  @param[in,out]  mtr           Mini-transaction */
  dberr_t create_ranges(const Scan_range &scan_range, page_no_t page_no,
                        size_t depth, const size_t split_level, Ranges &ranges,
                        mtr_t *mtr);

  /** Build a dtuple_t from rec_t.
  @param[in]      rec           Build the dtuple from this record.
  @param[in,out]  iter          Build in this iterator. */
  void copy_row(const rec_t *rec, Iter *iter) const;

  /** Create the persistent cursor that will be used to traverse the
  partition and position on the the start row.
  @param[in]      page_cursor   Current page cursor
  @param[in]      mtr           Mini-transaction covering the read.
  @return Start iterator. */
  [[nodiscard]] std::shared_ptr<Iter> create_persistent_cursor(
      const page_cur_t &page_cursor, mtr_t *mtr) const;

  /** Create an execution context for a range and add it to
  the Parallel_coordinator's run queue.
  @param[in] range              Range for which to create the context.
  @param[in] split              true if the sub-tree should be split further.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t create_context(const Range &range, bool split);

  /** Create the execution contexts based on the ranges.
  @param[in]  ranges            Ranges for which to create the contexts.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t create_contexts(const Ranges &ranges);

  /** @return the maximum number of worker thread configured. */
  [[nodiscard]] size_t num_workers() const {
     return m_coordinator->num_workers();
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
  using Config = Parallel_coordinator::Config;

  /** Context ID. */
  size_t m_id{std::numeric_limits<size_t>::max()};

  /** Parallel scan configuration. */
  Config m_config;

  /** Covering transaction. */
  trx_t *m_trx{};

  /** Depth of the Btree. */
  size_t m_depth{};

  /** The parallel coordinator. */
  Parallel_coordinator *m_coordinator{};

  /** Error during parallel read. */
  mutable std::atomic<dberr_t> m_err{DB_SUCCESS};

  /** Number of threads that have S locked the index. */
  std::atomic_size_t m_s_locks{};

  friend class Parallel_coordinator;

  Scan_ctx(Scan_ctx &&) = delete;
  Scan_ctx(const Scan_ctx &) = delete;
  Scan_ctx &operator=(Scan_ctx &&) = delete;
  Scan_ctx &operator=(const Scan_ctx &) = delete;
};

/** Parallel coordinator execution context. */
class Parallel_coordinator::Exec_ctx {
 public:
  /** Constructor.
  @param[in]    id              Thread ID.
  @param[in]    scan_ctx        Scan context.
  @param[in]    range           Range that the thread has to read. */
  Exec_ctx(size_t id, Scan_ctx *scan_ctx, const Scan_ctx::Range &range)
      : m_range(range), m_scan_ctx(scan_ctx), m_id(id) {}

  /** Destructor. */
  ~Exec_ctx() = default;

 public:
  /** @return the context ID. */
  [[nodiscard]] size_t id() const { return m_id; }

  /** The scan ID of the scan context this belongs to. */
  [[nodiscard]] size_t scan_id() const { return m_scan_ctx->id(); }

  /** @return the covering transaction. */
  [[nodiscard]] const trx_t *trx() const { return m_scan_ctx->m_trx; }

  /** @return the index being scanned. */
  [[nodiscard]] const dict_index_t *index() const {
    return m_scan_ctx->m_config.m_index;
  }

  /** @return the partition id of the index.
  @note this is std::numeric_limits<size_t>::max() if the index does not
  belong to a partition. */
  [[nodiscard]] size_t partition_id() const {
    return m_scan_ctx->m_config.m_partition_id;
  }

  /** Range to read in this context. */
  Scan_ctx::Range m_range{};

  /** Scanner context. */
  Scan_ctx *m_scan_ctx{};

private:
  /** Split the context into sub-ranges and add them to the execution queue.
  @return DB_SUCCESS or error code. */
  [[nodiscard]] dberr_t split();

  /** @return true if in error state. */
  [[nodiscard]] bool is_error_set() const {
    return m_scan_ctx->m_coordinator->is_error_set() ||
           m_scan_ctx->is_error_set();
  }

 private:
  /** Context ID. */
  size_t m_id{std::numeric_limits<size_t>::max()};

  /** If true then split the context at the block level. */
  bool m_split{};

  friend class Parallel_coordinator;
};

#endif /* !row0par_coord_h */
