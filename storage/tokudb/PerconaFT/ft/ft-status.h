/* -*- mode: C++; c-basic-offset: 4; indent-tabs-mode: nil -*- */
// vim: ft=cpp:expandtab:ts=8:sw=4:softtabstop=4:
#ident "$Id$"
/*======
This file is part of PerconaFT.


Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved.

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License, version 2,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.

----------------------------------------

    PerconaFT is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License, version 3,
    as published by the Free Software Foundation.

    PerconaFT is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with PerconaFT.  If not, see <http://www.gnu.org/licenses/>.
======= */

#ident "Copyright (c) 2006, 2015, Percona and/or its affiliates. All rights reserved."

#pragma once

#include <db.h>

#include "portability/toku_config.h"
#include "portability/toku_list.h"
#include "portability/toku_race_tools.h"

#include "util/status.h"

//
// Leaf Entry statistics
//
class LE_STATUS_S {
public:
    enum {
        LE_MAX_COMMITTED_XR = 0,
        LE_MAX_PROVISIONAL_XR,
        LE_EXPANDED,
        LE_MAX_MEMSIZE,
        LE_APPLY_GC_BYTES_IN,
        LE_APPLY_GC_BYTES_OUT,
        LE_NORMAL_GC_BYTES_IN,
        LE_NORMAL_GC_BYTES_OUT,
        LE_STATUS_NUM_ROWS
    };

    void init();
    void destroy();

    TOKU_ENGINE_STATUS_ROW_S status[LE_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef LE_STATUS_S* LE_STATUS;
extern LE_STATUS_S le_status;

// executed too often to be worth making threadsafe
#define LE_STATUS_VAL(x) le_status.status[LE_STATUS_S::x].value.num
#define LE_STATUS_INC(x, d)                                                         \
    do {                                                                            \
        if (le_status.status[LE_STATUS_S::x].type == PARCOUNT) {                                 \
            increment_partitioned_counter(le_status.status[LE_STATUS_S::x].value.parcount, d);   \
        } else {                                                                    \
            toku_sync_fetch_and_add(&le_status.status[LE_STATUS_S::x].value.num, d);             \
        }                                                                           \
    } while (0)



//
// Checkpoint statistics
//
class CHECKPOINT_STATUS_S {
public:
    enum {
        CP_PERIOD,
        CP_FOOTPRINT,
        CP_TIME_LAST_CHECKPOINT_BEGIN,
        CP_TIME_LAST_CHECKPOINT_BEGIN_COMPLETE,
        CP_TIME_LAST_CHECKPOINT_END,
        CP_TIME_CHECKPOINT_DURATION,
        CP_TIME_CHECKPOINT_DURATION_LAST,
        CP_LAST_LSN,
        CP_CHECKPOINT_COUNT,
        CP_CHECKPOINT_COUNT_FAIL,
        CP_WAITERS_NOW,          // how many threads are currently waiting for the checkpoint_safe lock to perform a checkpoint
        CP_WAITERS_MAX,          // max threads ever simultaneously waiting for the checkpoint_safe lock to perform a checkpoint
        CP_CLIENT_WAIT_ON_MO,    // how many times a client thread waited to take the multi_operation lock, not for checkpoint
        CP_CLIENT_WAIT_ON_CS,    // how many times a client thread waited for the checkpoint_safe lock, not for checkpoint
        CP_BEGIN_TIME,
        CP_LONG_BEGIN_TIME,
        CP_LONG_BEGIN_COUNT,
        CP_END_TIME,
        CP_LONG_END_TIME,
        CP_LONG_END_COUNT,
        CP_STATUS_NUM_ROWS       // number of rows in this status array.  must be last.
    };

    void init();
    void destroy();

    TOKU_ENGINE_STATUS_ROW_S status[CP_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef CHECKPOINT_STATUS_S* CHECKPOINT_STATUS;
extern CHECKPOINT_STATUS_S cp_status;

#define CP_STATUS_VAL(x) cp_status.status[CHECKPOINT_STATUS_S::x].value.num



//
// Cachetable statistics
//
class CACHETABLE_STATUS_S {
public:
    enum {
        CT_MISS = 0,
        CT_MISSTIME,               // how many usec spent waiting for disk read because of cache miss
        CT_PREFETCHES,             // how many times has a block been prefetched into the cachetable?
        CT_SIZE_CURRENT,           // the sum of the sizes of the nodes represented in the cachetable
        CT_SIZE_LIMIT,             // the limit to the sum of the node sizes
        CT_SIZE_WRITING,           // the sum of the sizes of the nodes being written
        CT_SIZE_NONLEAF,           // number of bytes in cachetable belonging to nonleaf nodes
        CT_SIZE_LEAF,              // number of bytes in cachetable belonging to leaf nodes
        CT_SIZE_ROLLBACK,          // number of bytes in cachetable belonging to rollback nodes
        CT_SIZE_CACHEPRESSURE,     // number of bytes causing cache pressure (sum of buffers and workdone counters)
        CT_SIZE_CLONED,            // number of bytes of cloned data in the system
        CT_EVICTIONS,
        CT_CLEANER_EXECUTIONS,     // number of times the cleaner thread's loop has executed
        CT_CLEANER_PERIOD,
        CT_CLEANER_ITERATIONS,     // number of times the cleaner thread runs the cleaner per period
        CT_WAIT_PRESSURE_COUNT,
        CT_WAIT_PRESSURE_TIME,
        CT_LONG_WAIT_PRESSURE_COUNT,
        CT_LONG_WAIT_PRESSURE_TIME,

        CT_POOL_CLIENT_NUM_THREADS,
        CT_POOL_CLIENT_NUM_THREADS_ACTIVE,
        CT_POOL_CLIENT_QUEUE_SIZE,
        CT_POOL_CLIENT_MAX_QUEUE_SIZE,
        CT_POOL_CLIENT_TOTAL_ITEMS_PROCESSED,
        CT_POOL_CLIENT_TOTAL_EXECUTION_TIME,
        CT_POOL_CACHETABLE_NUM_THREADS,
        CT_POOL_CACHETABLE_NUM_THREADS_ACTIVE,
        CT_POOL_CACHETABLE_QUEUE_SIZE,
        CT_POOL_CACHETABLE_MAX_QUEUE_SIZE,
        CT_POOL_CACHETABLE_TOTAL_ITEMS_PROCESSED,
        CT_POOL_CACHETABLE_TOTAL_EXECUTION_TIME,
        CT_POOL_CHECKPOINT_NUM_THREADS,
        CT_POOL_CHECKPOINT_NUM_THREADS_ACTIVE,
        CT_POOL_CHECKPOINT_QUEUE_SIZE,
        CT_POOL_CHECKPOINT_MAX_QUEUE_SIZE,
        CT_POOL_CHECKPOINT_TOTAL_ITEMS_PROCESSED,
        CT_POOL_CHECKPOINT_TOTAL_EXECUTION_TIME,

        CT_STATUS_NUM_ROWS
    };

    void init();
    void destroy();
    
    TOKU_ENGINE_STATUS_ROW_S status[CT_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef CACHETABLE_STATUS_S* CACHETABLE_STATUS;
extern CACHETABLE_STATUS_S ct_status;

#define CT_STATUS_VAL(x) ct_status.status[CACHETABLE_STATUS_S::x].value.num



//
// Lock Tree Manager statistics
//
class LTM_STATUS_S {
public:
    enum {
        LTM_SIZE_CURRENT = 0,
        LTM_SIZE_LIMIT,
        LTM_ESCALATION_COUNT,
        LTM_ESCALATION_TIME,
        LTM_ESCALATION_LATEST_RESULT,
        LTM_NUM_LOCKTREES,
        LTM_LOCK_REQUESTS_PENDING,
        LTM_STO_NUM_ELIGIBLE,
        LTM_STO_END_EARLY_COUNT,
        LTM_STO_END_EARLY_TIME,
        LTM_WAIT_COUNT,
        LTM_WAIT_TIME,
        LTM_LONG_WAIT_COUNT,
        LTM_LONG_WAIT_TIME,
        LTM_TIMEOUT_COUNT,
        LTM_WAIT_ESCALATION_COUNT,
        LTM_WAIT_ESCALATION_TIME,
        LTM_LONG_WAIT_ESCALATION_COUNT,
        LTM_LONG_WAIT_ESCALATION_TIME,
        LTM_STATUS_NUM_ROWS // must be last
    };

    void init(void);
    void destroy(void);

    TOKU_ENGINE_STATUS_ROW_S status[LTM_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef  LTM_STATUS_S* LTM_STATUS;
extern LTM_STATUS_S ltm_status;

#define LTM_STATUS_VAL(x) ltm_status.status[LTM_STATUS_S::x].value.num


//
// Fractal Tree statistics
//
class FT_STATUS_S {
public:
    enum {
        FT_UPDATES = 0,
        FT_UPDATES_BROADCAST,
        FT_DESCRIPTOR_SET,
        FT_MSN_DISCARDS,                           // how many messages were ignored by leaf because of msn
        FT_TOTAL_RETRIES,                          // total number of search retries due to TRY_AGAIN
        FT_SEARCH_TRIES_GT_HEIGHT,                 // number of searches that required more tries than the height of the tree
        FT_SEARCH_TRIES_GT_HEIGHTPLUS3,            // number of searches that required more tries than the height of the tree plus three
        FT_DISK_FLUSH_LEAF,                        // number of leaf nodes flushed to disk,    not for checkpoint
        FT_DISK_FLUSH_LEAF_BYTES,                  // number of leaf nodes flushed to disk,    not for checkpoint
        FT_DISK_FLUSH_LEAF_UNCOMPRESSED_BYTES,     // number of leaf nodes flushed to disk,    not for checkpoint
        FT_DISK_FLUSH_LEAF_TOKUTIME,               // number of leaf nodes flushed to disk,    not for checkpoint
        FT_DISK_FLUSH_NONLEAF,                     // number of nonleaf nodes flushed to disk, not for checkpoint
        FT_DISK_FLUSH_NONLEAF_BYTES,               // number of nonleaf nodes flushed to disk, not for checkpoint
        FT_DISK_FLUSH_NONLEAF_UNCOMPRESSED_BYTES,  // number of nonleaf nodes flushed to disk, not for checkpoint
        FT_DISK_FLUSH_NONLEAF_TOKUTIME,            // number of nonleaf nodes flushed to disk, not for checkpoint
        FT_DISK_FLUSH_LEAF_FOR_CHECKPOINT,         // number of leaf nodes flushed to disk for checkpoint
        FT_DISK_FLUSH_LEAF_BYTES_FOR_CHECKPOINT,   // number of leaf nodes flushed to disk for checkpoint
        FT_DISK_FLUSH_LEAF_UNCOMPRESSED_BYTES_FOR_CHECKPOINT,// number of leaf nodes flushed to disk for checkpoint
        FT_DISK_FLUSH_LEAF_TOKUTIME_FOR_CHECKPOINT,// number of leaf nodes flushed to disk for checkpoint
        FT_DISK_FLUSH_NONLEAF_FOR_CHECKPOINT,      // number of nonleaf nodes flushed to disk for checkpoint
        FT_DISK_FLUSH_NONLEAF_BYTES_FOR_CHECKPOINT,// number of nonleaf nodes flushed to disk for checkpoint
        FT_DISK_FLUSH_NONLEAF_UNCOMPRESSED_BYTES_FOR_CHECKPOINT,// number of nonleaf nodes flushed to disk for checkpoint
        FT_DISK_FLUSH_NONLEAF_TOKUTIME_FOR_CHECKPOINT,// number of nonleaf nodes flushed to disk for checkpoint
        FT_DISK_FLUSH_LEAF_COMPRESSION_RATIO,      // effective compression ratio for leaf bytes flushed to disk
        FT_DISK_FLUSH_NONLEAF_COMPRESSION_RATIO,   // effective compression ratio for nonleaf bytes flushed to disk
        FT_DISK_FLUSH_OVERALL_COMPRESSION_RATIO,   // effective compression ratio for all bytes flushed to disk
        FT_PARTIAL_EVICTIONS_NONLEAF,              // number of nonleaf node partial evictions
        FT_PARTIAL_EVICTIONS_NONLEAF_BYTES,        // number of nonleaf node partial evictions
        FT_PARTIAL_EVICTIONS_LEAF,                 // number of leaf node partial evictions
        FT_PARTIAL_EVICTIONS_LEAF_BYTES,           // number of leaf node partial evictions
        FT_FULL_EVICTIONS_LEAF,                    // number of full cachetable evictions on leaf nodes
        FT_FULL_EVICTIONS_LEAF_BYTES,              // number of full cachetable evictions on leaf nodes (bytes)
        FT_FULL_EVICTIONS_NONLEAF,                 // number of full cachetable evictions on nonleaf nodes
        FT_FULL_EVICTIONS_NONLEAF_BYTES,           // number of full cachetable evictions on nonleaf nodes (bytes)
        FT_CREATE_LEAF,                            // number of leaf nodes created
        FT_CREATE_NONLEAF,                         // number of nonleaf nodes created
        FT_DESTROY_LEAF,                           // number of leaf nodes destroyed
        FT_DESTROY_NONLEAF,                        // number of nonleaf nodes destroyed
        FT_MSG_BYTES_IN,                           // how many bytes of messages injected at root (for all trees)
        FT_MSG_BYTES_OUT,                          // how many bytes of messages flushed from h1 nodes to leaves
        FT_MSG_BYTES_CURR,                         // how many bytes of messages currently in trees (estimate)
        FT_MSG_NUM,                                // how many messages injected at root
        FT_MSG_NUM_BROADCAST,                      // how many broadcast messages injected at root
        FT_NUM_BASEMENTS_DECOMPRESSED_NORMAL,      // how many basement nodes were decompressed because they were the target of a query
        FT_NUM_BASEMENTS_DECOMPRESSED_AGGRESSIVE,  // ... because they were between lc and rc
        FT_NUM_BASEMENTS_DECOMPRESSED_PREFETCH,
        FT_NUM_BASEMENTS_DECOMPRESSED_WRITE,
        FT_NUM_MSG_BUFFER_DECOMPRESSED_NORMAL,     // how many msg buffers were decompressed because they were the target of a query
        FT_NUM_MSG_BUFFER_DECOMPRESSED_AGGRESSIVE, // ... because they were between lc and rc
        FT_NUM_MSG_BUFFER_DECOMPRESSED_PREFETCH,
        FT_NUM_MSG_BUFFER_DECOMPRESSED_WRITE,
        FT_NUM_PIVOTS_FETCHED_QUERY,               // how many pivots were fetched for a query
        FT_BYTES_PIVOTS_FETCHED_QUERY,             // how many pivots were fetched for a query
        FT_TOKUTIME_PIVOTS_FETCHED_QUERY,          // how many pivots were fetched for a query
        FT_NUM_PIVOTS_FETCHED_PREFETCH,            // ... for a prefetch
        FT_BYTES_PIVOTS_FETCHED_PREFETCH,          // ... for a prefetch
        FT_TOKUTIME_PIVOTS_FETCHED_PREFETCH,       // ... for a prefetch
        FT_NUM_PIVOTS_FETCHED_WRITE,               // ... for a write
        FT_BYTES_PIVOTS_FETCHED_WRITE,             // ... for a write
        FT_TOKUTIME_PIVOTS_FETCHED_WRITE,          // ... for a write
        FT_NUM_BASEMENTS_FETCHED_NORMAL,           // how many basement nodes were fetched because they were the target of a query
        FT_BYTES_BASEMENTS_FETCHED_NORMAL,         // how many basement nodes were fetched because they were the target of a query
        FT_TOKUTIME_BASEMENTS_FETCHED_NORMAL,      // how many basement nodes were fetched because they were the target of a query
        FT_NUM_BASEMENTS_FETCHED_AGGRESSIVE,       // ... because they were between lc and rc
        FT_BYTES_BASEMENTS_FETCHED_AGGRESSIVE,     // ... because they were between lc and rc
        FT_TOKUTIME_BASEMENTS_FETCHED_AGGRESSIVE,  // ... because they were between lc and rc
        FT_NUM_BASEMENTS_FETCHED_PREFETCH,
        FT_BYTES_BASEMENTS_FETCHED_PREFETCH,
        FT_TOKUTIME_BASEMENTS_FETCHED_PREFETCH,
        FT_NUM_BASEMENTS_FETCHED_WRITE,
        FT_BYTES_BASEMENTS_FETCHED_WRITE,
        FT_TOKUTIME_BASEMENTS_FETCHED_WRITE,
        FT_NUM_MSG_BUFFER_FETCHED_NORMAL,          // how many msg buffers were fetched because they were the target of a query
        FT_BYTES_MSG_BUFFER_FETCHED_NORMAL,        // how many msg buffers were fetched because they were the target of a query
        FT_TOKUTIME_MSG_BUFFER_FETCHED_NORMAL,     // how many msg buffers were fetched because they were the target of a query
        FT_NUM_MSG_BUFFER_FETCHED_AGGRESSIVE,      // ... because they were between lc and rc
        FT_BYTES_MSG_BUFFER_FETCHED_AGGRESSIVE,    // ... because they were between lc and rc
        FT_TOKUTIME_MSG_BUFFER_FETCHED_AGGRESSIVE, // ... because they were between lc and rc
        FT_NUM_MSG_BUFFER_FETCHED_PREFETCH,
        FT_BYTES_MSG_BUFFER_FETCHED_PREFETCH,
        FT_TOKUTIME_MSG_BUFFER_FETCHED_PREFETCH,
        FT_NUM_MSG_BUFFER_FETCHED_WRITE,
        FT_BYTES_MSG_BUFFER_FETCHED_WRITE,
        FT_TOKUTIME_MSG_BUFFER_FETCHED_WRITE,
        FT_LEAF_COMPRESS_TOKUTIME, // seconds spent compressing leaf leaf nodes to memory
        FT_LEAF_SERIALIZE_TOKUTIME, // seconds spent serializing leaf node to memory
        FT_LEAF_DECOMPRESS_TOKUTIME, // seconds spent decompressing leaf nodes to memory
        FT_LEAF_DESERIALIZE_TOKUTIME, // seconds spent deserializing leaf nodes to memory
        FT_NONLEAF_COMPRESS_TOKUTIME, // seconds spent compressing nonleaf nodes to memory
        FT_NONLEAF_SERIALIZE_TOKUTIME, // seconds spent serializing nonleaf nodes to memory
        FT_NONLEAF_DECOMPRESS_TOKUTIME, // seconds spent decompressing nonleaf nodes to memory
        FT_NONLEAF_DESERIALIZE_TOKUTIME, // seconds spent deserializing nonleaf nodes to memory
        FT_PRO_NUM_ROOT_SPLIT,
        FT_PRO_NUM_ROOT_H0_INJECT,
        FT_PRO_NUM_ROOT_H1_INJECT,
        FT_PRO_NUM_INJECT_DEPTH_0,
        FT_PRO_NUM_INJECT_DEPTH_1,
        FT_PRO_NUM_INJECT_DEPTH_2,
        FT_PRO_NUM_INJECT_DEPTH_3,
        FT_PRO_NUM_INJECT_DEPTH_GT3,
        FT_PRO_NUM_STOP_NONEMPTY_BUF,
        FT_PRO_NUM_STOP_H1,
        FT_PRO_NUM_STOP_LOCK_CHILD,
        FT_PRO_NUM_STOP_CHILD_INMEM,
        FT_PRO_NUM_DIDNT_WANT_PROMOTE,
        FT_BASEMENT_DESERIALIZE_FIXED_KEYSIZE, // how many basement nodes were deserialized with a fixed keysize
        FT_BASEMENT_DESERIALIZE_VARIABLE_KEYSIZE, // how many basement nodes were deserialized with a variable keysize
        FT_PRO_RIGHTMOST_LEAF_SHORTCUT_SUCCESS,
        FT_PRO_RIGHTMOST_LEAF_SHORTCUT_FAIL_POS,
        FT_PRO_RIGHTMOST_LEAF_SHORTCUT_FAIL_REACTIVE,
        FT_CURSOR_SKIP_DELETED_LEAF_ENTRY, // how many deleted leaf entries were skipped by a cursor
        FT_STATUS_NUM_ROWS
    };

    void init(void);
    void destroy(void);

    TOKU_ENGINE_STATUS_ROW_S status[FT_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef FT_STATUS_S* FT_STATUS;
extern FT_STATUS_S ft_status;

#define FT_STATUS_VAL(x)                                                            \
    (ft_status.status[FT_STATUS_S::x].type == PARCOUNT ?                                         \
        read_partitioned_counter(ft_status.status[FT_STATUS_S::x].value.parcount) :              \
        ft_status.status[FT_STATUS_S::x].value.num)

#define FT_STATUS_INC(x, d)                                                         \
    do {                                                                            \
        if (ft_status.status[FT_STATUS_S::x].type == PARCOUNT) {                                 \
            increment_partitioned_counter(ft_status.status[FT_STATUS_S::x].value.parcount, d);   \
        } else {                                                                    \
            toku_sync_fetch_and_add(&ft_status.status[FT_STATUS_S::x].value.num, d);             \
        }                                                                           \
    } while (0)



//
// Flusher statistics
//
class FT_FLUSHER_STATUS_S {
public:
    enum {
        FT_FLUSHER_CLEANER_TOTAL_NODES = 0,     // total number of nodes whose buffers are potentially flushed by cleaner thread
        FT_FLUSHER_CLEANER_H1_NODES,            // number of nodes of height one whose message buffers are flushed by cleaner thread
        FT_FLUSHER_CLEANER_HGT1_NODES,          // number of nodes of height > 1 whose message buffers are flushed by cleaner thread
        FT_FLUSHER_CLEANER_EMPTY_NODES,         // number of nodes that are selected by cleaner, but whose buffers are empty
        FT_FLUSHER_CLEANER_NODES_DIRTIED,       // number of nodes that are made dirty by the cleaner thread
        FT_FLUSHER_CLEANER_MAX_BUFFER_SIZE,     // max number of bytes in message buffer flushed by cleaner thread
        FT_FLUSHER_CLEANER_MIN_BUFFER_SIZE,
        FT_FLUSHER_CLEANER_TOTAL_BUFFER_SIZE,
        FT_FLUSHER_CLEANER_MAX_BUFFER_WORKDONE, // max workdone value of any message buffer flushed by cleaner thread
        FT_FLUSHER_CLEANER_MIN_BUFFER_WORKDONE,
        FT_FLUSHER_CLEANER_TOTAL_BUFFER_WORKDONE,
        FT_FLUSHER_CLEANER_NUM_LEAF_MERGES_STARTED,     // number of times cleaner thread tries to merge a leaf
        FT_FLUSHER_CLEANER_NUM_LEAF_MERGES_RUNNING,     // number of cleaner thread leaf merges in progress
        FT_FLUSHER_CLEANER_NUM_LEAF_MERGES_COMPLETED,   // number of times cleaner thread successfully merges a leaf
        FT_FLUSHER_CLEANER_NUM_DIRTIED_FOR_LEAF_MERGE,  // nodes dirtied by the "flush from root" process to merge a leaf node
        FT_FLUSHER_FLUSH_TOTAL,                 // total number of flushes done by flusher threads or cleaner threads
        FT_FLUSHER_FLUSH_IN_MEMORY,             // number of in memory flushes
        FT_FLUSHER_FLUSH_NEEDED_IO,             // number of flushes that had to read a child (or part) off disk
        FT_FLUSHER_FLUSH_CASCADES,              // number of flushes that triggered another flush in the child
        FT_FLUSHER_FLUSH_CASCADES_1,            // number of flushes that triggered 1 cascading flush
        FT_FLUSHER_FLUSH_CASCADES_2,            // number of flushes that triggered 2 cascading flushes
        FT_FLUSHER_FLUSH_CASCADES_3,            // number of flushes that triggered 3 cascading flushes
        FT_FLUSHER_FLUSH_CASCADES_4,            // number of flushes that triggered 4 cascading flushes
        FT_FLUSHER_FLUSH_CASCADES_5,            // number of flushes that triggered 5 cascading flushes
        FT_FLUSHER_FLUSH_CASCADES_GT_5,         // number of flushes that triggered more than 5 cascading flushes
        FT_FLUSHER_SPLIT_LEAF,                  // number of leaf nodes split
        FT_FLUSHER_SPLIT_NONLEAF,               // number of nonleaf nodes split
        FT_FLUSHER_MERGE_LEAF,                  // number of times leaf nodes are merged
        FT_FLUSHER_MERGE_NONLEAF,               // number of times nonleaf nodes are merged
        FT_FLUSHER_BALANCE_LEAF,                // number of times a leaf node is balanced
        FT_FLUSHER_STATUS_NUM_ROWS
    };

    void init(void);
    void destroy(void);

    TOKU_ENGINE_STATUS_ROW_S status[FT_FLUSHER_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef FT_FLUSHER_STATUS_S* FT_FLUSHER_STATUS;
extern FT_FLUSHER_STATUS_S fl_status;

#define FL_STATUS_VAL(x) fl_status.status[FT_FLUSHER_STATUS_S::x].value.num



//
// Hot Flusher
//
class FT_HOT_STATUS_S {
public:
    enum {
        FT_HOT_NUM_STARTED = 0,      // number of HOT operations that have begun
        FT_HOT_NUM_COMPLETED,        // number of HOT operations that have successfully completed
        FT_HOT_NUM_ABORTED,          // number of HOT operations that have been aborted
        FT_HOT_MAX_ROOT_FLUSH_COUNT, // max number of flushes from root ever required to optimize a tree
        FT_HOT_STATUS_NUM_ROWS
    };

    void init(void);
    void destroy(void);

    TOKU_ENGINE_STATUS_ROW_S status[FT_HOT_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef FT_HOT_STATUS_S* FT_HOT_STATUS;
extern FT_HOT_STATUS_S hot_status;

#define HOT_STATUS_VAL(x) hot_status.status[FT_HOT_STATUS_S::x].value.num



//
// Transaction statistics
//
class TXN_STATUS_S {
public:
    enum {
        TXN_BEGIN,             // total number of transactions begun (does not include recovered txns)
        TXN_READ_BEGIN,        // total number of read only transactions begun (does not include recovered txns)
        TXN_COMMIT,            // successful commits
        TXN_ABORT,
        TXN_STATUS_NUM_ROWS
    };

    void init(void);
    void destroy(void);

    TOKU_ENGINE_STATUS_ROW_S status[TXN_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef TXN_STATUS_S* TXN_STATUS;
extern TXN_STATUS_S txn_status;

#define TXN_STATUS_INC(x, d) increment_partitioned_counter(txn_status.status[TXN_STATUS_S::x].value.parcount, d)



//
// Logger statistics
//
class LOGGER_STATUS_S {
public:
    enum {
        LOGGER_NEXT_LSN = 0,
        LOGGER_NUM_WRITES,
        LOGGER_BYTES_WRITTEN,
        LOGGER_UNCOMPRESSED_BYTES_WRITTEN,
        LOGGER_TOKUTIME_WRITES,
        LOGGER_WAIT_BUF_LONG,
        LOGGER_STATUS_NUM_ROWS
    };

    void init(void);
    void destroy(void);

    TOKU_ENGINE_STATUS_ROW_S status[LOGGER_STATUS_NUM_ROWS];

private:
    bool m_initialized;
};
typedef LOGGER_STATUS_S* LOGGER_STATUS;
extern LOGGER_STATUS_S log_status;

#define LOG_STATUS_VAL(x) log_status.status[LOGGER_STATUS_S::x].value.num

void toku_status_init(void);
void toku_status_destroy(void);
