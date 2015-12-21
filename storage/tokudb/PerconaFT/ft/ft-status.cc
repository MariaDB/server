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

#include "ft/ft.h"
#include "ft/ft-status.h"

#include <toku_race_tools.h>

LE_STATUS_S le_status;
void LE_STATUS_S::init() {
    if (m_initialized) return;
#define LE_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "le: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    LE_STATUS_INIT(LE_MAX_COMMITTED_XR,     LEAF_ENTRY_MAX_COMMITTED_XR,    UINT64,     "max committed xr");
    LE_STATUS_INIT(LE_MAX_PROVISIONAL_XR,   LEAF_ENTRY_MAX_PROVISIONAL_XR,  UINT64,     "max provisional xr");
    LE_STATUS_INIT(LE_EXPANDED,             LEAF_ENTRY_EXPANDED,            UINT64,     "expanded");
    LE_STATUS_INIT(LE_MAX_MEMSIZE,          LEAF_ENTRY_MAX_MEMSIZE,         UINT64,     "max memsize");
    LE_STATUS_INIT(LE_APPLY_GC_BYTES_IN,    LEAF_ENTRY_APPLY_GC_BYTES_IN,   PARCOUNT,   "size of leafentries before garbage collection (during message application)");
    LE_STATUS_INIT(LE_APPLY_GC_BYTES_OUT,   LEAF_ENTRY_APPLY_GC_BYTES_OUT,  PARCOUNT,   "size of leafentries after garbage collection (during message application)");
    LE_STATUS_INIT(LE_NORMAL_GC_BYTES_IN,   LEAF_ENTRY_NORMAL_GC_BYTES_IN,  PARCOUNT,   "size of leafentries before garbage collection (outside message application)");
    LE_STATUS_INIT(LE_NORMAL_GC_BYTES_OUT,  LEAF_ENTRY_NORMAL_GC_BYTES_OUT, PARCOUNT,   "size of leafentries after garbage collection (outside message application)");
    m_initialized = true;
#undef LE_STATUS_INIT
}
void LE_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < LE_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}



CHECKPOINT_STATUS_S cp_status;
void CHECKPOINT_STATUS_S::init(void) {
    if (m_initialized) return;
#define CP_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "checkpoint: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    CP_STATUS_INIT(CP_PERIOD,                               CHECKPOINT_PERIOD,              UINT64,     "period");
    CP_STATUS_INIT(CP_FOOTPRINT,                            CHECKPOINT_FOOTPRINT,           UINT64,     "footprint");
    CP_STATUS_INIT(CP_TIME_LAST_CHECKPOINT_BEGIN,           CHECKPOINT_LAST_BEGAN,          UNIXTIME,   "last checkpoint began");
    CP_STATUS_INIT(CP_TIME_LAST_CHECKPOINT_BEGIN_COMPLETE,  CHECKPOINT_LAST_COMPLETE_BEGAN, UNIXTIME,   "last complete checkpoint began");
    CP_STATUS_INIT(CP_TIME_LAST_CHECKPOINT_END,             CHECKPOINT_LAST_COMPLETE_ENDED, UNIXTIME,   "last complete checkpoint ended");
    CP_STATUS_INIT(CP_TIME_CHECKPOINT_DURATION,             CHECKPOINT_DURATION,            UINT64,     "time spent during checkpoint (begin and end phases)");
    CP_STATUS_INIT(CP_TIME_CHECKPOINT_DURATION_LAST,        CHECKPOINT_DURATION_LAST,       UINT64,     "time spent during last checkpoint (begin and end phases)");
    CP_STATUS_INIT(CP_LAST_LSN,                             CHECKPOINT_LAST_LSN,            UINT64,     "last complete checkpoint LSN");
    CP_STATUS_INIT(CP_CHECKPOINT_COUNT,                     CHECKPOINT_TAKEN,               UINT64,     "checkpoints taken ");
    CP_STATUS_INIT(CP_CHECKPOINT_COUNT_FAIL,                CHECKPOINT_FAILED,              UINT64,     "checkpoints failed");
    CP_STATUS_INIT(CP_WAITERS_NOW,                          CHECKPOINT_WAITERS_NOW,         UINT64,     "waiters now");
    CP_STATUS_INIT(CP_WAITERS_MAX,                          CHECKPOINT_WAITERS_MAX,         UINT64,     "waiters max");
    CP_STATUS_INIT(CP_CLIENT_WAIT_ON_MO,                    CHECKPOINT_CLIENT_WAIT_ON_MO,   UINT64,     "non-checkpoint client wait on mo lock");
    CP_STATUS_INIT(CP_CLIENT_WAIT_ON_CS,                    CHECKPOINT_CLIENT_WAIT_ON_CS,   UINT64,     "non-checkpoint client wait on cs lock");
    CP_STATUS_INIT(CP_BEGIN_TIME,                           CHECKPOINT_BEGIN_TIME,          UINT64,     "checkpoint begin time");
    CP_STATUS_INIT(CP_LONG_BEGIN_COUNT,                     CHECKPOINT_LONG_BEGIN_COUNT,    UINT64,     "long checkpoint begin count");
    CP_STATUS_INIT(CP_LONG_BEGIN_TIME,                      CHECKPOINT_LONG_BEGIN_TIME,     UINT64,     "long checkpoint begin time");
    CP_STATUS_INIT(CP_END_TIME,                             CHECKPOINT_END_TIME,            UINT64,     "checkpoint end time");
    CP_STATUS_INIT(CP_LONG_END_COUNT,                       CHECKPOINT_LONG_END_COUNT,      UINT64,     "long checkpoint end count");
    CP_STATUS_INIT(CP_LONG_END_TIME,                        CHECKPOINT_LONG_END_TIME,       UINT64,     "long checkpoint end time");

    m_initialized = true;
#undef CP_STATUS_INIT
}
void CHECKPOINT_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < CP_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}

CACHETABLE_STATUS_S ct_status;
void CACHETABLE_STATUS_S::init() {
    if (m_initialized) return;
#define CT_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "cachetable: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    CT_STATUS_INIT(CT_MISS,                     CACHETABLE_MISS,                        UINT64, "miss");
    CT_STATUS_INIT(CT_MISSTIME,                 CACHETABLE_MISS_TIME,                   UINT64, "miss time");
    CT_STATUS_INIT(CT_PREFETCHES,               CACHETABLE_PREFETCHES,                  UINT64, "prefetches");
    CT_STATUS_INIT(CT_SIZE_CURRENT,             CACHETABLE_SIZE_CURRENT,                UINT64, "size current");
    CT_STATUS_INIT(CT_SIZE_LIMIT,               CACHETABLE_SIZE_LIMIT,                  UINT64, "size limit"); 
    CT_STATUS_INIT(CT_SIZE_WRITING,             CACHETABLE_SIZE_WRITING,                UINT64, "size writing");
    CT_STATUS_INIT(CT_SIZE_NONLEAF,             CACHETABLE_SIZE_NONLEAF,                UINT64, "size nonleaf");
    CT_STATUS_INIT(CT_SIZE_LEAF,                CACHETABLE_SIZE_LEAF,                   UINT64, "size leaf");
    CT_STATUS_INIT(CT_SIZE_ROLLBACK,            CACHETABLE_SIZE_ROLLBACK,               UINT64, "size rollback");
    CT_STATUS_INIT(CT_SIZE_CACHEPRESSURE,       CACHETABLE_SIZE_CACHEPRESSURE,          UINT64, "size cachepressure");
    CT_STATUS_INIT(CT_SIZE_CLONED,              CACHETABLE_SIZE_CLONED,                 UINT64, "size currently cloned data for checkpoint");
    CT_STATUS_INIT(CT_EVICTIONS,                CACHETABLE_EVICTIONS,                   UINT64, "evictions");
    CT_STATUS_INIT(CT_CLEANER_EXECUTIONS,       CACHETABLE_CLEANER_EXECUTIONS,          UINT64, "cleaner executions");
    CT_STATUS_INIT(CT_CLEANER_PERIOD,           CACHETABLE_CLEANER_PERIOD,              UINT64, "cleaner period");
    CT_STATUS_INIT(CT_CLEANER_ITERATIONS,       CACHETABLE_CLEANER_ITERATIONS,          UINT64, "cleaner iterations");
    CT_STATUS_INIT(CT_WAIT_PRESSURE_COUNT,      CACHETABLE_WAIT_PRESSURE_COUNT,         UINT64, "number of waits on cache pressure");
    CT_STATUS_INIT(CT_WAIT_PRESSURE_TIME,       CACHETABLE_WAIT_PRESSURE_TIME,          UINT64, "time waiting on cache pressure");
    CT_STATUS_INIT(CT_LONG_WAIT_PRESSURE_COUNT, CACHETABLE_LONG_WAIT_PRESSURE_COUNT,    UINT64, "number of long waits on cache pressure");
    CT_STATUS_INIT(CT_LONG_WAIT_PRESSURE_TIME,  CACHETABLE_LONG_WAIT_PRESSURE_TIME,     UINT64, "long time waiting on cache pressure");
    
    CT_STATUS_INIT(CT_POOL_CLIENT_NUM_THREADS,                  CACHETABLE_POOL_CLIENT_NUM_THREADS,                 UINT64, "number of threads in pool");
    CT_STATUS_INIT(CT_POOL_CLIENT_NUM_THREADS_ACTIVE,           CACHETABLE_POOL_CLIENT_NUM_THREADS_ACTIVE,          UINT64, "number of currently active threads in pool");
    CT_STATUS_INIT(CT_POOL_CLIENT_QUEUE_SIZE,                   CACHETABLE_POOL_CLIENT_QUEUE_SIZE,                  UINT64, "number of currently queued work items");
    CT_STATUS_INIT(CT_POOL_CLIENT_MAX_QUEUE_SIZE,               CACHETABLE_POOL_CLIENT_MAX_QUEUE_SIZE,              UINT64, "largest number of queued work items");
    CT_STATUS_INIT(CT_POOL_CLIENT_TOTAL_ITEMS_PROCESSED,        CACHETABLE_POOL_CLIENT_TOTAL_ITEMS_PROCESSED,       UINT64, "total number of work items processed");
    CT_STATUS_INIT(CT_POOL_CLIENT_TOTAL_EXECUTION_TIME,         CACHETABLE_POOL_CLIENT_TOTAL_EXECUTION_TIME,        UINT64, "total execution time of processing work items");
    CT_STATUS_INIT(CT_POOL_CACHETABLE_NUM_THREADS,              CACHETABLE_POOL_CACHETABLE_NUM_THREADS,             UINT64, "number of threads in pool");
    CT_STATUS_INIT(CT_POOL_CACHETABLE_NUM_THREADS_ACTIVE,       CACHETABLE_POOL_CACHETABLE_NUM_THREADS_ACTIVE,      UINT64, "number of currently active threads in pool");
    CT_STATUS_INIT(CT_POOL_CACHETABLE_QUEUE_SIZE,               CACHETABLE_POOL_CACHETABLE_QUEUE_SIZE,              UINT64, "number of currently queued work items");
    CT_STATUS_INIT(CT_POOL_CACHETABLE_MAX_QUEUE_SIZE,           CACHETABLE_POOL_CACHETABLE_MAX_QUEUE_SIZE,          UINT64, "largest number of queued work items");
    CT_STATUS_INIT(CT_POOL_CACHETABLE_TOTAL_ITEMS_PROCESSED,    CACHETABLE_POOL_CACHETABLE_TOTAL_ITEMS_PROCESSED,   UINT64, "total number of work items processed");
    CT_STATUS_INIT(CT_POOL_CACHETABLE_TOTAL_EXECUTION_TIME,     CACHETABLE_POOL_CACHETABLE_TOTAL_EXECUTION_TIME,    UINT64, "total execution time of processing work items");
    CT_STATUS_INIT(CT_POOL_CHECKPOINT_NUM_THREADS,              CACHETABLE_POOL_CHECKPOINT_NUM_THREADS,             UINT64, "number of threads in pool");
    CT_STATUS_INIT(CT_POOL_CHECKPOINT_NUM_THREADS_ACTIVE,       CACHETABLE_POOL_CHECKPOINT_NUM_THREADS_ACTIVE,      UINT64, "number of currently active threads in pool");
    CT_STATUS_INIT(CT_POOL_CHECKPOINT_QUEUE_SIZE,               CACHETABLE_POOL_CHECKPOINT_QUEUE_SIZE,              UINT64, "number of currently queued work items");
    CT_STATUS_INIT(CT_POOL_CHECKPOINT_MAX_QUEUE_SIZE,           CACHETABLE_POOL_CHECKPOINT_MAX_QUEUE_SIZE,          UINT64, "largest number of queued work items");
    CT_STATUS_INIT(CT_POOL_CHECKPOINT_TOTAL_ITEMS_PROCESSED,    CACHETABLE_POOL_CHECKPOINT_TOTAL_ITEMS_PROCESSED,   UINT64, "total number of work items processed");
    CT_STATUS_INIT(CT_POOL_CHECKPOINT_TOTAL_EXECUTION_TIME,     CACHETABLE_POOL_CHECKPOINT_TOTAL_EXECUTION_TIME,    UINT64, "total execution time of processing work items");

    m_initialized = true;
#undef CT_STATUS_INIT
}
void CACHETABLE_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < CT_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}



LTM_STATUS_S ltm_status;
void LTM_STATUS_S::init() {
    if (m_initialized) return;
#define LTM_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "locktree: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    LTM_STATUS_INIT(LTM_SIZE_CURRENT,               LOCKTREE_MEMORY_SIZE,                   UINT64, "memory size");
    LTM_STATUS_INIT(LTM_SIZE_LIMIT,                 LOCKTREE_MEMORY_SIZE_LIMIT,             UINT64, "memory size limit");
    LTM_STATUS_INIT(LTM_ESCALATION_COUNT,           LOCKTREE_ESCALATION_NUM,                UINT64, "number of times lock escalation ran");
    LTM_STATUS_INIT(LTM_ESCALATION_TIME,            LOCKTREE_ESCALATION_SECONDS,            TOKUTIME, "time spent running escalation (seconds)");
    LTM_STATUS_INIT(LTM_ESCALATION_LATEST_RESULT,   LOCKTREE_LATEST_POST_ESCALATION_MEMORY_SIZE, UINT64, "latest post-escalation memory size");
    LTM_STATUS_INIT(LTM_NUM_LOCKTREES,              LOCKTREE_OPEN_CURRENT,                  UINT64, "number of locktrees open now");
    LTM_STATUS_INIT(LTM_LOCK_REQUESTS_PENDING,      LOCKTREE_PENDING_LOCK_REQUESTS,         UINT64, "number of pending lock requests");
    LTM_STATUS_INIT(LTM_STO_NUM_ELIGIBLE,           LOCKTREE_STO_ELIGIBLE_NUM,              UINT64, "number of locktrees eligible for the STO");
    LTM_STATUS_INIT(LTM_STO_END_EARLY_COUNT,        LOCKTREE_STO_ENDED_NUM,                 UINT64, "number of times a locktree ended the STO early");
    LTM_STATUS_INIT(LTM_STO_END_EARLY_TIME,         LOCKTREE_STO_ENDED_SECONDS,             TOKUTIME, "time spent ending the STO early (seconds)");
    LTM_STATUS_INIT(LTM_WAIT_COUNT,                 LOCKTREE_WAIT_COUNT,                    UINT64, "number of wait locks");
    LTM_STATUS_INIT(LTM_WAIT_TIME,                  LOCKTREE_WAIT_TIME,                     UINT64, "time waiting for locks");
    LTM_STATUS_INIT(LTM_LONG_WAIT_COUNT,            LOCKTREE_LONG_WAIT_COUNT,               UINT64, "number of long wait locks");
    LTM_STATUS_INIT(LTM_LONG_WAIT_TIME,             LOCKTREE_LONG_WAIT_TIME,                UINT64, "long time waiting for locks");
    LTM_STATUS_INIT(LTM_TIMEOUT_COUNT,              LOCKTREE_TIMEOUT_COUNT,                 UINT64, "number of lock timeouts");
    LTM_STATUS_INIT(LTM_WAIT_ESCALATION_COUNT,      LOCKTREE_WAIT_ESCALATION_COUNT,         UINT64, "number of waits on lock escalation");
    LTM_STATUS_INIT(LTM_WAIT_ESCALATION_TIME,       LOCKTREE_WAIT_ESCALATION_TIME,          UINT64, "time waiting on lock escalation");
    LTM_STATUS_INIT(LTM_LONG_WAIT_ESCALATION_COUNT, LOCKTREE_LONG_WAIT_ESCALATION_COUNT,    UINT64, "number of long waits on lock escalation");
    LTM_STATUS_INIT(LTM_LONG_WAIT_ESCALATION_TIME,  LOCKTREE_LONG_WAIT_ESCALATION_TIME,     UINT64, "long time waiting on lock escalation");

    m_initialized = true;
#undef LTM_STATUS_INIT
}
void LTM_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < LTM_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}



FT_STATUS_S ft_status;
void FT_STATUS_S::init() {
    if (m_initialized) return;
#define FT_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "ft: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    FT_STATUS_INIT(FT_UPDATES,                                DICTIONARY_UPDATES,                   PARCOUNT, "dictionary updates");
    FT_STATUS_INIT(FT_UPDATES_BROADCAST,                      DICTIONARY_BROADCAST_UPDATES,         PARCOUNT, "dictionary broadcast updates");
    FT_STATUS_INIT(FT_DESCRIPTOR_SET,                         DESCRIPTOR_SET,                       PARCOUNT, "descriptor set");
    FT_STATUS_INIT(FT_MSN_DISCARDS,                           MESSAGES_IGNORED_BY_LEAF_DUE_TO_MSN,  PARCOUNT, "messages ignored by leaf due to msn");
    FT_STATUS_INIT(FT_TOTAL_RETRIES,                          TOTAL_SEARCH_RETRIES,                 PARCOUNT, "total search retries due to TRY_AGAIN");
    FT_STATUS_INIT(FT_SEARCH_TRIES_GT_HEIGHT,                 SEARCH_TRIES_GT_HEIGHT,               PARCOUNT, "searches requiring more tries than the height of the tree");
    FT_STATUS_INIT(FT_SEARCH_TRIES_GT_HEIGHTPLUS3,            SEARCH_TRIES_GT_HEIGHTPLUS3,          PARCOUNT, "searches requiring more tries than the height of the tree plus three");
    FT_STATUS_INIT(FT_CREATE_LEAF,                            LEAF_NODES_CREATED,                   PARCOUNT, "leaf nodes created");
    FT_STATUS_INIT(FT_CREATE_NONLEAF,                         NONLEAF_NODES_CREATED,                PARCOUNT, "nonleaf nodes created");
    FT_STATUS_INIT(FT_DESTROY_LEAF,                           LEAF_NODES_DESTROYED,                 PARCOUNT, "leaf nodes destroyed");
    FT_STATUS_INIT(FT_DESTROY_NONLEAF,                        NONLEAF_NODES_DESTROYED,              PARCOUNT, "nonleaf nodes destroyed");
    FT_STATUS_INIT(FT_MSG_BYTES_IN,                           MESSAGES_INJECTED_AT_ROOT_BYTES,      PARCOUNT, "bytes of messages injected at root (all trees)");
    FT_STATUS_INIT(FT_MSG_BYTES_OUT,                          MESSAGES_FLUSHED_FROM_H1_TO_LEAVES_BYTES, PARCOUNT, "bytes of messages flushed from h1 nodes to leaves");
    FT_STATUS_INIT(FT_MSG_BYTES_CURR,                         MESSAGES_IN_TREES_ESTIMATE_BYTES,     PARCOUNT, "bytes of messages currently in trees (estimate)");
    FT_STATUS_INIT(FT_MSG_NUM,                                MESSAGES_INJECTED_AT_ROOT,            PARCOUNT, "messages injected at root");
    FT_STATUS_INIT(FT_MSG_NUM_BROADCAST,                      BROADCASE_MESSAGES_INJECTED_AT_ROOT,  PARCOUNT, "broadcast messages injected at root");

    FT_STATUS_INIT(FT_NUM_BASEMENTS_DECOMPRESSED_NORMAL,      BASEMENTS_DECOMPRESSED_TARGET_QUERY,  PARCOUNT, "basements decompressed as a target of a query");
    FT_STATUS_INIT(FT_NUM_BASEMENTS_DECOMPRESSED_AGGRESSIVE,  BASEMENTS_DECOMPRESSED_PRELOCKED_RANGE, PARCOUNT, "basements decompressed for prelocked range");
    FT_STATUS_INIT(FT_NUM_BASEMENTS_DECOMPRESSED_PREFETCH,    BASEMENTS_DECOMPRESSED_PREFETCH,      PARCOUNT, "basements decompressed for prefetch");
    FT_STATUS_INIT(FT_NUM_BASEMENTS_DECOMPRESSED_WRITE,       BASEMENTS_DECOMPRESSED_FOR_WRITE,     PARCOUNT, "basements decompressed for write");
    FT_STATUS_INIT(FT_NUM_MSG_BUFFER_DECOMPRESSED_NORMAL,     BUFFERS_DECOMPRESSED_TARGET_QUERY,    PARCOUNT, "buffers decompressed as a target of a query");
    FT_STATUS_INIT(FT_NUM_MSG_BUFFER_DECOMPRESSED_AGGRESSIVE, BUFFERS_DECOMPRESSED_PRELOCKED_RANGE, PARCOUNT, "buffers decompressed for prelocked range");
    FT_STATUS_INIT(FT_NUM_MSG_BUFFER_DECOMPRESSED_PREFETCH,   BUFFERS_DECOMPRESSED_PREFETCH,        PARCOUNT, "buffers decompressed for prefetch");
    FT_STATUS_INIT(FT_NUM_MSG_BUFFER_DECOMPRESSED_WRITE,      BUFFERS_DECOMPRESSED_FOR_WRITE,       PARCOUNT, "buffers decompressed for write");

    // Eviction statistics:
    FT_STATUS_INIT(FT_FULL_EVICTIONS_LEAF,                    LEAF_NODE_FULL_EVICTIONS,             PARCOUNT, "leaf node full evictions");
    FT_STATUS_INIT(FT_FULL_EVICTIONS_LEAF_BYTES,              LEAF_NODE_FULL_EVICTIONS_BYTES,       PARCOUNT, "leaf node full evictions (bytes)");
    FT_STATUS_INIT(FT_FULL_EVICTIONS_NONLEAF,                 NONLEAF_NODE_FULL_EVICTIONS,          PARCOUNT, "nonleaf node full evictions");
    FT_STATUS_INIT(FT_FULL_EVICTIONS_NONLEAF_BYTES,           NONLEAF_NODE_FULL_EVICTIONS_BYTES,    PARCOUNT, "nonleaf node full evictions (bytes)");
    FT_STATUS_INIT(FT_PARTIAL_EVICTIONS_LEAF,                 LEAF_NODE_PARTIAL_EVICTIONS,          PARCOUNT, "leaf node partial evictions");
    FT_STATUS_INIT(FT_PARTIAL_EVICTIONS_LEAF_BYTES,           LEAF_NODE_PARTIAL_EVICTIONS_BYTES,    PARCOUNT, "leaf node partial evictions (bytes)");
    FT_STATUS_INIT(FT_PARTIAL_EVICTIONS_NONLEAF,              NONLEAF_NODE_PARTIAL_EVICTIONS,       PARCOUNT, "nonleaf node partial evictions");
    FT_STATUS_INIT(FT_PARTIAL_EVICTIONS_NONLEAF_BYTES,        NONLEAF_NODE_PARTIAL_EVICTIONS_BYTES, PARCOUNT, "nonleaf node partial evictions (bytes)");

    // Disk read statistics: 
    //
    // Pivots: For queries, prefetching, or writing.
    FT_STATUS_INIT(FT_NUM_PIVOTS_FETCHED_QUERY,               PIVOTS_FETCHED_FOR_QUERY,             PARCOUNT, "pivots fetched for query");
    FT_STATUS_INIT(FT_BYTES_PIVOTS_FETCHED_QUERY,             PIVOTS_FETCHED_FOR_QUERY_BYTES,       PARCOUNT, "pivots fetched for query (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_PIVOTS_FETCHED_QUERY,          PIVOTS_FETCHED_FOR_QUERY_SECONDS,     TOKUTIME, "pivots fetched for query (seconds)");
    FT_STATUS_INIT(FT_NUM_PIVOTS_FETCHED_PREFETCH,            PIVOTS_FETCHED_FOR_PREFETCH,          PARCOUNT, "pivots fetched for prefetch");
    FT_STATUS_INIT(FT_BYTES_PIVOTS_FETCHED_PREFETCH,          PIVOTS_FETCHED_FOR_PREFETCH_BYTES,    PARCOUNT, "pivots fetched for prefetch (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_PIVOTS_FETCHED_PREFETCH,       PIVOTS_FETCHED_FOR_PREFETCH_SECONDS,  TOKUTIME, "pivots fetched for prefetch (seconds)");
    FT_STATUS_INIT(FT_NUM_PIVOTS_FETCHED_WRITE,               PIVOTS_FETCHED_FOR_WRITE,             PARCOUNT, "pivots fetched for write");
    FT_STATUS_INIT(FT_BYTES_PIVOTS_FETCHED_WRITE,             PIVOTS_FETCHED_FOR_WRITE_BYTES,       PARCOUNT, "pivots fetched for write (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_PIVOTS_FETCHED_WRITE,          PIVOTS_FETCHED_FOR_WRITE_SECONDS,     TOKUTIME, "pivots fetched for write (seconds)");
    // Basements: For queries, aggressive fetching in prelocked range, prefetching, or writing.
    FT_STATUS_INIT(FT_NUM_BASEMENTS_FETCHED_NORMAL,           BASEMENTS_FETCHED_TARGET_QUERY,       PARCOUNT, "basements fetched as a target of a query");
    FT_STATUS_INIT(FT_BYTES_BASEMENTS_FETCHED_NORMAL,         BASEMENTS_FETCHED_TARGET_QUERY_BYTES, PARCOUNT, "basements fetched as a target of a query (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_BASEMENTS_FETCHED_NORMAL,      BASEMENTS_FETCHED_TARGET_QUERY_SECONDS, TOKUTIME, "basements fetched as a target of a query (seconds)");
    FT_STATUS_INIT(FT_NUM_BASEMENTS_FETCHED_AGGRESSIVE,       BASEMENTS_FETCHED_PRELOCKED_RANGE,    PARCOUNT, "basements fetched for prelocked range");
    FT_STATUS_INIT(FT_BYTES_BASEMENTS_FETCHED_AGGRESSIVE,     BASEMENTS_FETCHED_PRELOCKED_RANGE_BYTES, PARCOUNT, "basements fetched for prelocked range (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_BASEMENTS_FETCHED_AGGRESSIVE,  BASEMENTS_FETCHED_PRELOCKED_RANGE_SECONDS, TOKUTIME, "basements fetched for prelocked range (seconds)");
    FT_STATUS_INIT(FT_NUM_BASEMENTS_FETCHED_PREFETCH,         BASEMENTS_FETCHED_PREFETCH,           PARCOUNT, "basements fetched for prefetch");
    FT_STATUS_INIT(FT_BYTES_BASEMENTS_FETCHED_PREFETCH,       BASEMENTS_FETCHED_PREFETCH_BYTES,     PARCOUNT, "basements fetched for prefetch (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_BASEMENTS_FETCHED_PREFETCH,    BASEMENTS_FETCHED_PREFETCH_SECONDS,   TOKUTIME, "basements fetched for prefetch (seconds)");
    FT_STATUS_INIT(FT_NUM_BASEMENTS_FETCHED_WRITE,            BASEMENTS_FETCHED_FOR_WRITE,          PARCOUNT, "basements fetched for write");
    FT_STATUS_INIT(FT_BYTES_BASEMENTS_FETCHED_WRITE,          BASEMENTS_FETCHED_FOR_WRITE_BYTES,    PARCOUNT, "basements fetched for write (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_BASEMENTS_FETCHED_WRITE,       BASEMENTS_FETCHED_FOR_WRITE_SECONDS,  TOKUTIME, "basements fetched for write (seconds)");
    // Buffers: For queries, aggressive fetching in prelocked range, prefetching, or writing.
    FT_STATUS_INIT(FT_NUM_MSG_BUFFER_FETCHED_NORMAL,          BUFFERS_FETCHED_TARGET_QUERY,         PARCOUNT, "buffers fetched as a target of a query");
    FT_STATUS_INIT(FT_BYTES_MSG_BUFFER_FETCHED_NORMAL,        BUFFERS_FETCHED_TARGET_QUERY_BYTES,   PARCOUNT, "buffers fetched as a target of a query (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_MSG_BUFFER_FETCHED_NORMAL,     BUFFERS_FETCHED_TARGET_QUERY_SECONDS, TOKUTIME, "buffers fetched as a target of a query (seconds)");
    FT_STATUS_INIT(FT_NUM_MSG_BUFFER_FETCHED_AGGRESSIVE,      BUFFERS_FETCHED_PRELOCKED_RANGE,      PARCOUNT, "buffers fetched for prelocked range");
    FT_STATUS_INIT(FT_BYTES_MSG_BUFFER_FETCHED_AGGRESSIVE,    BUFFERS_FETCHED_PRELOCKED_RANGE_BYTES, PARCOUNT, "buffers fetched for prelocked range (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_MSG_BUFFER_FETCHED_AGGRESSIVE, BUFFERS_FETCHED_PRELOCKED_RANGE_SECONDS, TOKUTIME, "buffers fetched for prelocked range (seconds)");
    FT_STATUS_INIT(FT_NUM_MSG_BUFFER_FETCHED_PREFETCH,        BUFFERS_FETCHED_PREFETCH,             PARCOUNT, "buffers fetched for prefetch");
    FT_STATUS_INIT(FT_BYTES_MSG_BUFFER_FETCHED_PREFETCH,      BUFFERS_FETCHED_PREFETCH_BYTES,       PARCOUNT, "buffers fetched for prefetch (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_MSG_BUFFER_FETCHED_PREFETCH,   BUFFERS_FETCHED_PREFETCH_SECONDS,     TOKUTIME, "buffers fetched for prefetch (seconds)");
    FT_STATUS_INIT(FT_NUM_MSG_BUFFER_FETCHED_WRITE,           BUFFERS_FETCHED_FOR_WRITE,            PARCOUNT, "buffers fetched for write");
    FT_STATUS_INIT(FT_BYTES_MSG_BUFFER_FETCHED_WRITE,         BUFFERS_FETCHED_FOR_WRITE_BYTES,      PARCOUNT, "buffers fetched for write (bytes)");
    FT_STATUS_INIT(FT_TOKUTIME_MSG_BUFFER_FETCHED_WRITE,      BUFFERS_FETCHED_FOR_WRITE_SECONDS,    TOKUTIME, "buffers fetched for write (seconds)");

    // Disk write statistics.
    //
    // Leaf/Nonleaf: Not for checkpoint
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF,                        LEAF_NODES_FLUSHED_NOT_CHECKPOINT,    PARCOUNT, "leaf nodes flushed to disk (not for checkpoint)");
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF_BYTES,                  LEAF_NODES_FLUSHED_NOT_CHECKPOINT_BYTES, PARCOUNT, "leaf nodes flushed to disk (not for checkpoint) (bytes)");
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF_UNCOMPRESSED_BYTES,     LEAF_NODES_FLUSHED_NOT_CHECKPOINT_UNCOMPRESSED_BYTES, PARCOUNT, "leaf nodes flushed to disk (not for checkpoint) (uncompressed bytes)");
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF_TOKUTIME,               LEAF_NODES_FLUSHED_NOT_CHECKPOINT_SECONDS, TOKUTIME, "leaf nodes flushed to disk (not for checkpoint) (seconds)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF,                     NONLEAF_NODES_FLUSHED_TO_DISK_NOT_CHECKPOINT, PARCOUNT, "nonleaf nodes flushed to disk (not for checkpoint)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF_BYTES,               NONLEAF_NODES_FLUSHED_TO_DISK_NOT_CHECKPOINT_BYTES, PARCOUNT, "nonleaf nodes flushed to disk (not for checkpoint) (bytes)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF_UNCOMPRESSED_BYTES,  NONLEAF_NODES_FLUSHED_TO_DISK_NOT_CHECKPOINT_UNCOMPRESSED_BYTES, PARCOUNT, "nonleaf nodes flushed to disk (not for checkpoint) (uncompressed bytes)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF_TOKUTIME,            NONLEAF_NODES_FLUSHED_TO_DISK_NOT_CHECKPOINT_SECONDS, TOKUTIME, "nonleaf nodes flushed to disk (not for checkpoint) (seconds)");
    // Leaf/Nonleaf: For checkpoint
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF_FOR_CHECKPOINT,         LEAF_NODES_FLUSHED_CHECKPOINT,        PARCOUNT, "leaf nodes flushed to disk (for checkpoint)");
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF_BYTES_FOR_CHECKPOINT,   LEAF_NODES_FLUSHED_CHECKPOINT_BYTES,  PARCOUNT, "leaf nodes flushed to disk (for checkpoint) (bytes)");
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF_UNCOMPRESSED_BYTES_FOR_CHECKPOINT, LEAF_NODES_FLUSHED_CHECKPOINT_UNCOMPRESSED_BYTES, PARCOUNT, "leaf nodes flushed to disk (for checkpoint) (uncompressed bytes)");
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF_TOKUTIME_FOR_CHECKPOINT, LEAF_NODES_FLUSHED_CHECKPOINT_SECONDS, TOKUTIME, "leaf nodes flushed to disk (for checkpoint) (seconds)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF_FOR_CHECKPOINT,      NONLEAF_NODES_FLUSHED_TO_DISK_CHECKPOINT, PARCOUNT, "nonleaf nodes flushed to disk (for checkpoint)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF_BYTES_FOR_CHECKPOINT, NONLEAF_NODES_FLUSHED_TO_DISK_CHECKPOINT_BYTES, PARCOUNT, "nonleaf nodes flushed to disk (for checkpoint) (bytes)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF_UNCOMPRESSED_BYTES_FOR_CHECKPOINT, NONLEAF_NODES_FLUSHED_TO_DISK_CHECKPOINT_UNCOMPRESSED_BYTES, PARCOUNT, "nonleaf nodes flushed to disk (for checkpoint) (uncompressed bytes)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF_TOKUTIME_FOR_CHECKPOINT, NONLEAF_NODES_FLUSHED_TO_DISK_CHECKPOINT_SECONDS, TOKUTIME, "nonleaf nodes flushed to disk (for checkpoint) (seconds)");
    FT_STATUS_INIT(FT_DISK_FLUSH_LEAF_COMPRESSION_RATIO,      LEAF_NODE_COMPRESSION_RATIO,          DOUBLE, "uncompressed / compressed bytes written (leaf)");
    FT_STATUS_INIT(FT_DISK_FLUSH_NONLEAF_COMPRESSION_RATIO,   NONLEAF_NODE_COMPRESSION_RATIO,       DOUBLE, "uncompressed / compressed bytes written (nonleaf)");
    FT_STATUS_INIT(FT_DISK_FLUSH_OVERALL_COMPRESSION_RATIO,   OVERALL_NODE_COMPRESSION_RATIO,       DOUBLE, "uncompressed / compressed bytes written (overall)");

    // CPU time statistics for [de]serialization and [de]compression.
    FT_STATUS_INIT(FT_LEAF_COMPRESS_TOKUTIME,                 LEAF_COMPRESSION_TO_MEMORY_SECONDS,   TOKUTIME, "leaf compression to memory (seconds)");
    FT_STATUS_INIT(FT_LEAF_SERIALIZE_TOKUTIME,                LEAF_SERIALIZATION_TO_MEMORY_SECONDS, TOKUTIME, "leaf serialization to memory (seconds)");
    FT_STATUS_INIT(FT_LEAF_DECOMPRESS_TOKUTIME,               LEAF_DECOMPRESSION_TO_MEMORY_SECONDS, TOKUTIME, "leaf decompression to memory (seconds)");
    FT_STATUS_INIT(FT_LEAF_DESERIALIZE_TOKUTIME,              LEAF_DESERIALIZATION_TO_MEMORY_SECONDS, TOKUTIME, "leaf deserialization to memory (seconds)");
    FT_STATUS_INIT(FT_NONLEAF_COMPRESS_TOKUTIME,              NONLEAF_COMPRESSION_TO_MEMORY_SECONDS, TOKUTIME, "nonleaf compression to memory (seconds)");
    FT_STATUS_INIT(FT_NONLEAF_SERIALIZE_TOKUTIME,             NONLEAF_SERIALIZATION_TO_MEMORY_SECONDS, TOKUTIME, "nonleaf serialization to memory (seconds)");
    FT_STATUS_INIT(FT_NONLEAF_DECOMPRESS_TOKUTIME,            NONLEAF_DECOMPRESSION_TO_MEMORY_SECONDS, TOKUTIME, "nonleaf decompression to memory (seconds)");
    FT_STATUS_INIT(FT_NONLEAF_DESERIALIZE_TOKUTIME,           NONLEAF_DESERIALIZATION_TO_MEMORY_SECONDS, TOKUTIME, "nonleaf deserialization to memory (seconds)");

    // Promotion statistics.
    FT_STATUS_INIT(FT_PRO_NUM_ROOT_SPLIT,                     PROMOTION_ROOTS_SPLIT,                PARCOUNT, "promotion: roots split");
    FT_STATUS_INIT(FT_PRO_NUM_ROOT_H0_INJECT,                 PROMOTION_LEAF_ROOTS_INJECTED_INTO,   PARCOUNT, "promotion: leaf roots injected into");
    FT_STATUS_INIT(FT_PRO_NUM_ROOT_H1_INJECT,                 PROMOTION_H1_ROOTS_INJECTED_INTO,     PARCOUNT, "promotion: h1 roots injected into");
    FT_STATUS_INIT(FT_PRO_NUM_INJECT_DEPTH_0,                 PROMOTION_INJECTIONS_AT_DEPTH_0,      PARCOUNT, "promotion: injections at depth 0");
    FT_STATUS_INIT(FT_PRO_NUM_INJECT_DEPTH_1,                 PROMOTION_INJECTIONS_AT_DEPTH_1,      PARCOUNT, "promotion: injections at depth 1");
    FT_STATUS_INIT(FT_PRO_NUM_INJECT_DEPTH_2,                 PROMOTION_INJECTIONS_AT_DEPTH_2,      PARCOUNT, "promotion: injections at depth 2");
    FT_STATUS_INIT(FT_PRO_NUM_INJECT_DEPTH_3,                 PROMOTION_INJECTIONS_AT_DEPTH_3,      PARCOUNT, "promotion: injections at depth 3");
    FT_STATUS_INIT(FT_PRO_NUM_INJECT_DEPTH_GT3,               PROMOTION_INJECTIONS_LOWER_THAN_DEPTH_3, PARCOUNT, "promotion: injections lower than depth 3");
    FT_STATUS_INIT(FT_PRO_NUM_STOP_NONEMPTY_BUF,              PROMOTION_STOPPED_NONEMPTY_BUFFER,    PARCOUNT, "promotion: stopped because of a nonempty buffer");
    FT_STATUS_INIT(FT_PRO_NUM_STOP_H1,                        PROMOTION_STOPPED_AT_HEIGHT_1,        PARCOUNT, "promotion: stopped at height 1");
    FT_STATUS_INIT(FT_PRO_NUM_STOP_LOCK_CHILD,                PROMOTION_STOPPED_CHILD_LOCKED_OR_NOT_IN_MEMORY, PARCOUNT, "promotion: stopped because the child was locked or not at all in memory");
    FT_STATUS_INIT(FT_PRO_NUM_STOP_CHILD_INMEM,               PROMOTION_STOPPED_CHILD_NOT_FULLY_IN_MEMORY, PARCOUNT, "promotion: stopped because the child was not fully in memory");
    FT_STATUS_INIT(FT_PRO_NUM_DIDNT_WANT_PROMOTE,             PROMOTION_STOPPED_AFTER_LOCKING_CHILD, PARCOUNT, "promotion: stopped anyway, after locking the child");
    FT_STATUS_INIT(FT_BASEMENT_DESERIALIZE_FIXED_KEYSIZE,     BASEMENT_DESERIALIZATION_FIXED_KEY,   PARCOUNT, "basement nodes deserialized with fixed-keysize");
    FT_STATUS_INIT(FT_BASEMENT_DESERIALIZE_VARIABLE_KEYSIZE,  BASEMENT_DESERIALIZATION_VARIABLE_KEY, PARCOUNT, "basement nodes deserialized with variable-keysize");
    FT_STATUS_INIT(FT_PRO_RIGHTMOST_LEAF_SHORTCUT_SUCCESS,    PRO_RIGHTMOST_LEAF_SHORTCUT_SUCCESS,  PARCOUNT, "promotion: succeeded in using the rightmost leaf shortcut");
    FT_STATUS_INIT(FT_PRO_RIGHTMOST_LEAF_SHORTCUT_FAIL_POS,   PRO_RIGHTMOST_LEAF_SHORTCUT_FAIL_POS, PARCOUNT, "promotion: tried the rightmost leaf shorcut but failed (out-of-bounds)");
    FT_STATUS_INIT(FT_PRO_RIGHTMOST_LEAF_SHORTCUT_FAIL_REACTIVE,RIGHTMOST_LEAF_SHORTCUT_FAIL_REACTIVE, PARCOUNT, "promotion: tried the rightmost leaf shorcut but failed (child reactive)");

    FT_STATUS_INIT(FT_CURSOR_SKIP_DELETED_LEAF_ENTRY,         CURSOR_SKIP_DELETED_LEAF_ENTRY,       PARCOUNT, "cursor skipped deleted leaf entries");

    m_initialized = true;
#undef FT_STATUS_INIT
}
void FT_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < FT_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}



FT_FLUSHER_STATUS_S fl_status;
void FT_FLUSHER_STATUS_S::init() {
    if (m_initialized) return;
#define FL_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "ft flusher: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_TOTAL_NODES,                 FLUSHER_CLEANER_TOTAL_NODES,                 UINT64, "total nodes potentially flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_H1_NODES,                    FLUSHER_CLEANER_H1_NODES,                    UINT64, "height-one nodes flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_HGT1_NODES,                  FLUSHER_CLEANER_HGT1_NODES,                  UINT64, "height-greater-than-one nodes flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_EMPTY_NODES,                 FLUSHER_CLEANER_EMPTY_NODES,                 UINT64, "nodes cleaned which had empty buffers");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_NODES_DIRTIED,               FLUSHER_CLEANER_NODES_DIRTIED,               UINT64, "nodes dirtied by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_MAX_BUFFER_SIZE,             FLUSHER_CLEANER_MAX_BUFFER_SIZE,             UINT64, "max bytes in a buffer flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_MIN_BUFFER_SIZE,             FLUSHER_CLEANER_MIN_BUFFER_SIZE,             UINT64, "min bytes in a buffer flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_TOTAL_BUFFER_SIZE,           FLUSHER_CLEANER_TOTAL_BUFFER_SIZE,           UINT64, "total bytes in buffers flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_MAX_BUFFER_WORKDONE,         FLUSHER_CLEANER_MAX_BUFFER_WORKDONE,         UINT64, "max workdone in a buffer flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_MIN_BUFFER_WORKDONE,         FLUSHER_CLEANER_MIN_BUFFER_WORKDONE,         UINT64, "min workdone in a buffer flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_TOTAL_BUFFER_WORKDONE,       FLUSHER_CLEANER_TOTAL_BUFFER_WORKDONE,       UINT64, "total workdone in buffers flushed by cleaner thread");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_NUM_LEAF_MERGES_STARTED,     FLUSHER_CLEANER_NUM_LEAF_MERGES_STARTED,     UINT64, "times cleaner thread tries to merge a leaf");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_NUM_LEAF_MERGES_RUNNING,     FLUSHER_CLEANER_NUM_LEAF_MERGES_RUNNING,     UINT64, "cleaner thread leaf merges in progress");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_NUM_LEAF_MERGES_COMPLETED,   FLUSHER_CLEANER_NUM_LEAF_MERGES_COMPLETED,   UINT64, "cleaner thread leaf merges successful");
    FL_STATUS_INIT(FT_FLUSHER_CLEANER_NUM_DIRTIED_FOR_LEAF_MERGE,  FLUSHER_CLEANER_NUM_DIRTIED_FOR_LEAF_MERGE,  UINT64, "nodes dirtied by cleaner thread leaf merges");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_TOTAL,                         FLUSHER_FLUSH_TOTAL,                         UINT64, "total number of flushes done by flusher threads or cleaner threads");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_IN_MEMORY,                     FLUSHER_FLUSH_IN_MEMORY,                     UINT64, "number of in memory flushes");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_NEEDED_IO,                     FLUSHER_FLUSH_NEEDED_IO,                     UINT64, "number of flushes that read something off disk");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_CASCADES,                      FLUSHER_FLUSH_CASCADES,                      UINT64, "number of flushes that triggered another flush in child");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_CASCADES_1,                    FLUSHER_FLUSH_CASCADES_1,                    UINT64, "number of flushes that triggered 1 cascading flush");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_CASCADES_2,                    FLUSHER_FLUSH_CASCADES_2,                    UINT64, "number of flushes that triggered 2 cascading flushes");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_CASCADES_3,                    FLUSHER_FLUSH_CASCADES_3,                    UINT64, "number of flushes that triggered 3 cascading flushes");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_CASCADES_4,                    FLUSHER_FLUSH_CASCADES_4,                    UINT64, "number of flushes that triggered 4 cascading flushes");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_CASCADES_5,                    FLUSHER_FLUSH_CASCADES_5,                    UINT64, "number of flushes that triggered 5 cascading flushes");
    FL_STATUS_INIT(FT_FLUSHER_FLUSH_CASCADES_GT_5,                 FLUSHER_FLUSH_CASCADES_GT_5,                 UINT64, "number of flushes that triggered over 5 cascading flushes");
    FL_STATUS_INIT(FT_FLUSHER_SPLIT_LEAF,                          FLUSHER_SPLIT_LEAF,                          UINT64, "leaf node splits");
    FL_STATUS_INIT(FT_FLUSHER_SPLIT_NONLEAF,                       FLUSHER_SPLIT_NONLEAF,                       UINT64, "nonleaf node splits");
    FL_STATUS_INIT(FT_FLUSHER_MERGE_LEAF,                          FLUSHER_MERGE_LEAF,                          UINT64, "leaf node merges");
    FL_STATUS_INIT(FT_FLUSHER_MERGE_NONLEAF,                       FLUSHER_MERGE_NONLEAF,                       UINT64, "nonleaf node merges");
    FL_STATUS_INIT(FT_FLUSHER_BALANCE_LEAF,                        FLUSHER_BALANCE_LEAF,                        UINT64, "leaf node balances");

    FL_STATUS_VAL(FT_FLUSHER_CLEANER_MIN_BUFFER_SIZE) = UINT64_MAX;
    FL_STATUS_VAL(FT_FLUSHER_CLEANER_MIN_BUFFER_WORKDONE) = UINT64_MAX;

    m_initialized = true;
#undef FL_STATUS_INIT
}
void FT_FLUSHER_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < FT_FLUSHER_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}



FT_HOT_STATUS_S hot_status;
void FT_HOT_STATUS_S::init() {
    if (m_initialized) return;
#define HOT_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "hot: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    HOT_STATUS_INIT(FT_HOT_NUM_STARTED,           HOT_NUM_STARTED,           UINT64, "operations ever started");
    HOT_STATUS_INIT(FT_HOT_NUM_COMPLETED,         HOT_NUM_COMPLETED,         UINT64, "operations successfully completed");
    HOT_STATUS_INIT(FT_HOT_NUM_ABORTED,           HOT_NUM_ABORTED,           UINT64, "operations aborted");
    HOT_STATUS_INIT(FT_HOT_MAX_ROOT_FLUSH_COUNT,  HOT_MAX_ROOT_FLUSH_COUNT,  UINT64, "max number of flushes from root ever required to optimize a tree");

    m_initialized = true;
#undef HOT_STATUS_INIT
}
void FT_HOT_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < FT_HOT_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}



TXN_STATUS_S txn_status;
void TXN_STATUS_S::init() {
    if (m_initialized) return;
#define TXN_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "txn: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    TXN_STATUS_INIT(TXN_BEGIN,          TXN_BEGIN,              PARCOUNT, "begin");
    TXN_STATUS_INIT(TXN_READ_BEGIN,     TXN_BEGIN_READ_ONLY,    PARCOUNT, "begin read only");
    TXN_STATUS_INIT(TXN_COMMIT,         TXN_COMMITS,            PARCOUNT, "successful commits");
    TXN_STATUS_INIT(TXN_ABORT,          TXN_ABORTS,             PARCOUNT, "aborts");
    m_initialized = true;
#undef TXN_STATUS_INIT
}
void TXN_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < TXN_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}


LOGGER_STATUS_S log_status;
void LOGGER_STATUS_S::init() {
    if (m_initialized) return;
#define LOG_STATUS_INIT(k,c,t,l) TOKUFT_STATUS_INIT((*this), k, c, t, "logger: " l, TOKU_ENGINE_STATUS|TOKU_GLOBAL_STATUS)
    LOG_STATUS_INIT(LOGGER_NEXT_LSN,                    LOGGER_NEXT_LSN,        UINT64,  "next LSN");
    LOG_STATUS_INIT(LOGGER_NUM_WRITES,                  LOGGER_WRITES,          UINT64, "writes");
    LOG_STATUS_INIT(LOGGER_BYTES_WRITTEN,               LOGGER_WRITES_BYTES,    UINT64, "writes (bytes)");
    LOG_STATUS_INIT(LOGGER_UNCOMPRESSED_BYTES_WRITTEN,  LOGGER_WRITES_UNCOMPRESSED_BYTES, UINT64, "writes (uncompressed bytes)");
    LOG_STATUS_INIT(LOGGER_TOKUTIME_WRITES,             LOGGER_WRITES_SECONDS,  TOKUTIME, "writes (seconds)");
    LOG_STATUS_INIT(LOGGER_WAIT_BUF_LONG,               LOGGER_WAIT_LONG,       UINT64, "number of long logger write operations");
    m_initialized = true;
#undef LOG_STATUS_INIT
}
void LOGGER_STATUS_S::destroy() {
    if (!m_initialized) return;
    for (int i = 0; i < LOGGER_STATUS_NUM_ROWS; ++i) {
        if (status[i].type == PARCOUNT) {
            destroy_partitioned_counter(status[i].value.parcount);
        }
    }
}

void toku_status_init(void) {
    le_status.init();
    cp_status.init();
    ltm_status.init();
    ft_status.init();
    fl_status.init();
    hot_status.init();
    txn_status.init();
    log_status.init();
}
void toku_status_destroy(void) {
    log_status.destroy();
    txn_status.destroy();
    hot_status.destroy();
    fl_status.destroy();
    ft_status.destroy();
    ltm_status.destroy();
    cp_status.destroy();
    le_status.destroy();
}
