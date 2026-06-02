/*
  Copyright (c) 2026 TidesDB Corp.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA
*/
#include "ha_tidesdb.h"

extern "C"
{
#define XXH_INLINE_ALL
#include <tidesdb/xxhash.h>
#ifdef TIDESDB_WITH_S3
    tidesdb_objstore_t *tidesdb_objstore_s3_create(const char *endpoint, const char *bucket,
                                                   const char *prefix, const char *access_key,
                                                   const char *secret_key, const char *region,
                                                   int use_ssl, int use_path_style);
#endif
}

#include <ft_global.h>
#include <mysql/plugin.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "key.h"
#include "sql_class.h"
#include "sql_priv.h"

/* MariaDB 12.3.1 (MDEV-37815) renamed TABLE_SHARE::option_struct to
   option_struct_table and introduced handler::option_struct as the preferred
   accessor.  We keep reading from TABLE_SHARE so the macro works from
   create(), inplace alter, and free functions that only have a TABLE*. */
#if MYSQL_VERSION_ID >= 120301
#define TDB_TABLE_OPTIONS(tbl) ((tbl)->s->option_struct_table)
#else
#define TDB_TABLE_OPTIONS(tbl) ((tbl)->s->option_struct)
#endif

/* Forward-declared for tdb_rc_to_ha(); defined with sysvars below */
static my_bool srv_print_all_conflicts = 0;
static my_bool srv_pessimistic_locking = 1;
static mysql_mutex_t last_conflict_mutex;
/* Buffer for the most recent conflict diagnostic surfaced under
   `Last conflict:` in SHOW ENGINE TIDESDB STATUS.  Sized comfortably above
   any expected single-line message; updates are bounded by snprintf with
   sizeof() so the constant only appears here. */
static constexpr size_t LAST_CONFLICT_INFO_LEN = 1024;
static char last_conflict_info[LAST_CONFLICT_INFO_LEN] = "";

/*
  Map TidesDB library error codes to MariaDB handler error codes.
  Transient errors (conflict, lock contention, memory pressure) are mapped
  to HA_ERR_LOCK_DEADLOCK so that MariaDB's deadlock-retry logic kicks in
  and applications can retry automatically instead of
  receiving the opaque HA_ERR_GENERIC / ER_GET_ERRNO 1030.
*/
static int tdb_rc_to_ha(int rc, const char *ctx)
{
    switch (rc)
    {
        case TDB_SUCCESS:
            return 0;

        /* Transient concurrency errors -- mapped to deadlock so MariaDB
           rolls back the transaction and the application can retry. */
        case TDB_ERR_CONFLICT:
            if (unlikely(srv_print_all_conflicts))
            {
                sql_print_information(
                    "[TIDESDB] %s: transaction aborted due to write-write "
                    "conflict (TDB_ERR_CONFLICT)",
                    ctx);
                mysql_mutex_lock(&last_conflict_mutex);
                snprintf(last_conflict_info, sizeof(last_conflict_info), "Last conflict: %s at %ld",
                         ctx, (long)time(NULL));
                mysql_mutex_unlock(&last_conflict_mutex);
            }
            return HA_ERR_LOCK_DEADLOCK;

        /* Lock wait timeout -- rolls back the current statement only
           (not the whole transaction), less disruptive than full deadlock. */
        case TDB_ERR_LOCKED:
            return HA_ERR_LOCK_WAIT_TIMEOUT;

        /* Back-pressure signal from the library (memtable / flush queue
           / L0 backlog at soft cap).  Callers that go through the
           tdb_txn_*_blocking wrappers absorb this transparently by
           waiting for capacity, so this fall-through path only fires
           when the configured wait timeout has been exhausted or no
           wrapper is in play -- in either case lock-wait-timeout is the
           accurate name (not deadlock; nothing is locked).

           TDB_ERR_BUSY is the same family.  The library now distinguishes
           a soft cap (TDB_ERR_MEMORY_LIMIT) from the case where it has
           stalled long enough that its internal no-progress budget was
           spent without freeing capacity.  Both are transient and the
           plugin treats them the same once the in-plugin backoff has
           given up. */
        case TDB_ERR_MEMORY_LIMIT:
        case TDB_ERR_BUSY:
            return HA_ERR_LOCK_WAIT_TIMEOUT;

        /* Hard out-of-memory.  Distinct from TDB_ERR_MEMORY_LIMIT above
           (a soft back-pressure signal); TDB_ERR_MEMORY means the
           allocator itself failed. */
        case TDB_ERR_MEMORY:
            sql_print_error("[TIDESDB] %s: TDB_ERR_MEMORY", ctx);
            return HA_ERR_OUT_OF_MEM;

        case TDB_ERR_NOT_FOUND:
            return HA_ERR_KEY_NOT_FOUND;

        case TDB_ERR_EXISTS:
            return HA_ERR_FOUND_DUPP_KEY;

        case TDB_ERR_READONLY:
            return HA_ERR_READ_ONLY_TRANSACTION;

        /* I/O and corruption errors -- table needs repair/recovery.
           matches InnoDB's mapping of DB_CORRUPTION to HA_ERR_CRASHED. */
        case TDB_ERR_IO:
            sql_print_error("[TIDESDB] %s: I/O error (TDB_ERR_IO)", ctx);
            return HA_ERR_CRASHED;

        case TDB_ERR_CORRUPTION:
            sql_print_error("[TIDESDB] %s: data corruption detected (TDB_ERR_CORRUPTION)", ctx);
            return HA_ERR_CRASHED;

        /* Row too large for the configured block/value size. */
        case TDB_ERR_TOO_LARGE:
            return HA_ERR_TO_BIG_ROW;

        /* Database handle invalid (closed or never opened). */
        case TDB_ERR_INVALID_DB:
            sql_print_error("[TIDESDB] %s: invalid database handle (TDB_ERR_INVALID_DB)", ctx);
            return HA_ERR_INTERNAL_ERROR;

        /* Invalid arguments -- programming error in the plugin. */
        case TDB_ERR_INVALID_ARGS:
            sql_print_error("[TIDESDB] %s: invalid arguments (TDB_ERR_INVALID_ARGS)", ctx);
            return HA_ERR_INTERNAL_ERROR;

        /* Unified-mode commit returns TDB_ERR_UNKNOWN when the active-memtable
           try_ref retry budget is exhausted under heavy rotation contention --
           same family as TDB_ERR_CONFLICT from the caller's perspective.
           Map to HA_ERR_LOCK_DEADLOCK so MariaDB triggers its deadlock
           retry path instead of surfacing an opaque ER_GET_ERRNO 1030. */
        case TDB_ERR_UNKNOWN:
            return HA_ERR_LOCK_DEADLOCK;

        default:
            sql_print_warning("[TIDESDB] %s: unexpected TidesDB error rc=%d", ctx, rc);
            return HA_ERR_GENERIC;
    }
}

/*
  Dispatch to tidesdb_txn_single_delete or tidesdb_txn_delete based on
  use_single_delete.  Secondary-index delete sites pass true because the
  single-delete contract (at most one put between single-deletes on the
  same key) holds by construction for (col_values, pk) / (term, pk) /
  (hilbert, pk) composites.  Primary-CF delete sites pass the cached
  value of the tidesdb_single_delete_primary session variable, which
  defaults off and is the caller's explicit promise that the session
  does no UPDATE on non-PK columns and no REPLACE INTO / IODKU overwrite
  path on no-secondary tables.
*/
static inline int tidesdb_txn_delete_cf(tidesdb_txn_t *txn, tidesdb_column_family_t *cf,
                                        const uint8_t *key, size_t key_size, bool use_single_delete)
{
    return use_single_delete ? tidesdb_txn_single_delete(txn, cf, key, key_size)
                             : tidesdb_txn_delete(txn, cf, key, key_size);
}

/* ******************** Library back-pressure wait ******************** */
/*
  TDB_ERR_MEMORY_LIMIT is the library's soft back-pressure signal -- the
  memtable / flush queue / L0 backlog is at its cap and the writer should
  pause until flush+compaction free capacity.  Surfacing that to the SQL
  layer as HA_ERR_LOCK_DEADLOCK -- as earlier revisions did -- breaks
  clients that treat 1213 as fatal and do not retry (bulk loaders, batch
  ETL, schema-build scripts), failing entire sessions after long writes
  even though nothing is locked and the engine just needs a moment to
  drain.

  The put/commit/delete wrappers below sleep with exponential backoff
  until the library accepts the operation again, the wait timeout
  expires, or the connection is killed.  After exhaustion the original
  TDB_ERR_MEMORY_LIMIT bubbles up through tdb_rc_to_ha and maps to
  HA_ERR_LOCK_WAIT_TIMEOUT, which is the accurate name (no lock is held).
*/
static constexpr uint TDB_BACKPRESSURE_BACKOFF_MIN_US = 100;   /* 0.1 ms initial */
static constexpr uint TDB_BACKPRESSURE_BACKOFF_MAX_US = 50000; /* 50 ms cap   */
static constexpr uint TDB_BACKPRESSURE_BACKOFF_MULTIPLIER = 2;
static constexpr ulong TDB_BACKPRESSURE_DEFAULT_TIMEOUT_MS = 60000;     /* 60 s default */
static constexpr ulong TDB_BACKPRESSURE_MAX_TIMEOUT_MS = 3600000;       /* 1 h max */
static constexpr ulong TDB_BACKPRESSURE_MIN_TIMEOUT_MS = 0;             /* 0 disables blocking */
static constexpr uint TDB_BACKPRESSURE_KILL_CHECK_INTERVAL_US = 100000; /* 100 ms */

/* Pessimistic row-lock wait bounds.  Default mirrors innodb_lock_wait_timeout
   (50 seconds).  0 means wait indefinitely, bounded only by KILL QUERY. */
static constexpr ulong TDB_LOCK_WAIT_DEFAULT_TIMEOUT_MS = 50000;
static constexpr ulong TDB_LOCK_WAIT_MIN_TIMEOUT_MS = 0;
static constexpr ulong TDB_LOCK_WAIT_MAX_TIMEOUT_MS = 3600000;
static constexpr ulonglong TDB_NS_PER_MS = 1000000ULL;
static constexpr ulonglong TDB_US_PER_S = 1000000ULL;

/* Stats -- bumped from the wrapper, read by tidesdb_refresh_status_vars. */
static std::atomic<long long> srv_stat_backpressure_waits{0};
static std::atomic<long long> srv_stat_backpressure_wait_us{0};
static std::atomic<long long> srv_stat_lock_waits{0};
static std::atomic<long long> srv_stat_lock_wait_us{0};
static std::atomic<long long> srv_stat_lock_deadlocks{0};
static std::atomic<long long> srv_stat_lock_timeouts{0};
static std::atomic<long long> srv_stat_lock_held{0};
static std::atomic<long long> srv_stat_lock_entries{0};
static std::atomic<long long> srv_stat_lock_entry_recycles{0};
static std::atomic<long long> srv_stat_lock_chain_max{0};

static ulong tdb_backpressure_timeout_ms(THD *thd);
static ulong tdb_lock_wait_timeout_ms(THD *thd);

/*
  Per-statement back-pressure deadline.  external_lock(F_WRLCK) seeds it to
  now() + timeout_ms; F_UNLCK clears it.  When valid every backpressure call
  in the statement charges against the same shared deadline so a 5000-row
  INSERT with N indexes cannot burn (1+N) * 5000 * timeout_ms of wall-clock.
  Thread-local because each statement runs on one connection thread.
*/
static thread_local std::chrono::steady_clock::time_point tdb_stmt_bp_deadline_{};
static thread_local bool tdb_stmt_bp_deadline_valid_ = false;

/*
  Run op() and, if the library reports back-pressure, sleep with exponential
  backoff and retry until success, timeout exhaustion, or connection kill.
  The kill-check cadence is bounded so even a long sleep responds promptly
  to KILL QUERY.  After the deadline the unmodified TDB_ERR_MEMORY_LIMIT is
  returned so the caller's existing error mapping still applies.
*/
template <typename Op>
static int tdb_with_backpressure_wait(THD *thd, Op &&op)
{
    int rc = op();
    if (likely(rc != TDB_ERR_MEMORY_LIMIT && rc != TDB_ERR_BUSY)) return rc;

    const ulong timeout_ms = tdb_backpressure_timeout_ms(thd);
    if (timeout_ms == 0) return rc;

    /* Prefer the per-statement deadline when external_lock has seeded it
       so a multi-call statement (bulk INSERT/UPDATE/DELETE, ALTER) does
       not multiply the budget by row count. */
    const auto deadline =
        tdb_stmt_bp_deadline_valid_
            ? tdb_stmt_bp_deadline_
            : (std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms));
    uint sleep_us = TDB_BACKPRESSURE_BACKOFF_MIN_US;
    bool counted = false;
    long long waited_us = 0;

    while (rc == TDB_ERR_MEMORY_LIMIT || rc == TDB_ERR_BUSY)
    {
        if (thd && thd_killed(thd)) break;

        auto now = std::chrono::steady_clock::now();
        if (now >= deadline) break;
        auto remaining_us =
            std::chrono::duration_cast<std::chrono::microseconds>(deadline - now).count();
        uint capped_sleep_us = std::min(sleep_us, TDB_BACKPRESSURE_KILL_CHECK_INTERVAL_US);
        if ((long long)capped_sleep_us > remaining_us) capped_sleep_us = (uint)remaining_us;

        std::this_thread::sleep_for(std::chrono::microseconds(capped_sleep_us));
        waited_us += capped_sleep_us;
        if (!counted)
        {
            srv_stat_backpressure_waits.fetch_add(1, std::memory_order_relaxed);
            counted = true;
        }
        sleep_us = std::min<uint>(sleep_us * TDB_BACKPRESSURE_BACKOFF_MULTIPLIER,
                                  TDB_BACKPRESSURE_BACKOFF_MAX_US);
        rc = op();
    }

    if (waited_us > 0)
        srv_stat_backpressure_wait_us.fetch_add(waited_us, std::memory_order_relaxed);
    return rc;
}

/* Thin wrappers around the three library write entry points that can return
   TDB_ERR_MEMORY_LIMIT under sustained write load.  Other callers that do
   not go through these still get the accurate HA_ERR_LOCK_WAIT_TIMEOUT
   mapping from tdb_rc_to_ha but without the in-plugin block. */
static inline int tdb_txn_put_blocking(THD *thd, tidesdb_txn_t *txn, tidesdb_column_family_t *cf,
                                       const uint8_t *key, size_t key_size, const uint8_t *value,
                                       size_t value_size, time_t ttl)
{
    return tdb_with_backpressure_wait(
        thd, [&]() { return tidesdb_txn_put(txn, cf, key, key_size, value, value_size, ttl); });
}

static inline int tdb_txn_commit_blocking(THD *thd, tidesdb_txn_t *txn)
{
    return tdb_with_backpressure_wait(thd, [&]() { return tidesdb_txn_commit(txn); });
}

static inline int tdb_txn_delete_cf_blocking(THD *thd, tidesdb_txn_t *txn,
                                             tidesdb_column_family_t *cf, const uint8_t *key,
                                             size_t key_size, bool use_single_delete)
{
    return tdb_with_backpressure_wait(
        thd, [&]() { return tidesdb_txn_delete_cf(txn, cf, key, key_size, use_single_delete); });
}

/* Iterator construction can return TDB_ERR_BUSY when the library's reader fd
   soft cap is exhausted, and TDB_ERR_IO when an SSTable open fails after the
   budget check passed (EMFILE between the check and the open).  The library
   documents both as retryable -- the in-line comment at the IO site reads
   "let the caller retry once descriptors free."  Routing iter_new through
   the backpressure helper waits it out instead of immediately surfacing
   HA_ERR_LOCK_WAIT_TIMEOUT (BUSY) or HA_ERR_CRASHED (IO).  The IO -> BUSY
   translation is scoped to this wrapper so other call sites still treat a
   real TDB_ERR_IO as a hard fault via tdb_rc_to_ha; the wrapper's existing
   tidesdb_backpressure_wait_timeout_ms bound stops a genuine disk failure
   from hanging forever. */
static inline int tdb_iter_new_blocking(THD *thd, tidesdb_txn_t *txn, tidesdb_column_family_t *cf,
                                        tidesdb_iter_t **out)
{
    return tdb_with_backpressure_wait(thd,
                                      [&]()
                                      {
                                          int rc = tidesdb_iter_new(txn, cf, out);
                                          if (rc == TDB_ERR_IO) rc = TDB_ERR_BUSY;
                                          return rc;
                                      });
}

/* MariaDB data directory */
extern MYSQL_PLUGIN_IMPORT char mysql_real_data_home[];

/* Global TidesDB database handle */
static tidesdb_t *tdb_global = NULL;
static std::string tdb_path;

/* Schema discovery CF for object store mode (NULL when local-only) */
static tidesdb_column_family_t *schema_cf = NULL;

static handlerton *tidesdb_hton;

/* ******************** Plugin-level row lock table ******************** */
/*
  Hash-table-based row-level lock manager with two modes (S, X), wait queue
  for fairness, and best-effort deadlock detection.

  Design:
  - Hash partitions over XXH3 of the row key, sized at init from hardware
    concurrency.  Each partition has its own mutex, an active hash chain
    of lock entries, and a per-partition freelist of slots whose granted
    and waiting lists are both empty.
  - Each lock entry has two intrusive lists, both mutex-guarded:
      granted_head -- currently-granted requests on this row
      waiting_head -- FIFO of requests still waiting
  - Each request (tdb_lock_request_t) ties (trx, lock, mode) together and
    threads onto trx->held_locks_head (granted) or trx->waiting_on (waiting).
  - Compatibility S/S is compatible; S/X and X/X are not.  A new S also
    blocks when an X is waiting, so writers cannot be starved by a stream
    of readers.
  - Re-entry on the same lock, if this trx already holds it in a mode
    compatible with the request (X subsumes S; S satisfies S), return 0.
    Upgrade S->X is allowed only when this trx is the sole granted holder
    AND no waiters exist; otherwise we reject as HA_ERR_LOCK_DEADLOCK
    rather than introduce a self-deadlock with our own S-grant.
  - For deadlock detection, when we wait on a lock, walk every granted holder's
    wait-for chain.  Loads are atomic, lock entry memory is never my_free'd
    during runtime, so a stale read can only produce a false-positive
    (caller retries) or a false-negative (caller times out via
    lock-wait-timeout) -- never memory corruption.
  - Release walks the trx's held_locks_head, unlinks each request from its
    lock's granted list, promotes any waiting requests now compatible with
    the remaining granted set, broadcasts the lock's cond, and moves the
    lock entry onto the partition's freelist if no granted or waiting
    requests remain.  Slot memory is retained for the deadlock walker but
    the entry leaves the hash chain so lookups stay O(active locks per
    partition) rather than O(lifetime keys).
*/

/* Number of hash partitions for the row lock table.  Sized at init from
   hardware_concurrency to 8 * cores, clamped to [128, 65536].  The
   upper cap stays at the historical value so a huge box still gets
   plenty of partitions; the lower bound guarantees decent stripe count
   even on single-vCPU containers.  Each partition is cache-line
   aligned so unrelated stripes do not false-share. */
static ulong row_lock_partitions = 0;
static constexpr ulong ROW_LOCK_PARTITIONS_MIN = 128;
static constexpr ulong ROW_LOCK_PARTITIONS_MAX = 65536;

/* Maximum depth for wait-for-graph traversal during deadlock detection. */
static constexpr int DEADLOCK_MAX_DEPTH = 100;

/* tdb_lock_mode_t is declared in ha_tidesdb.h so the trx struct can name it. */

/* Lock request -- one per (trx, lock, mode) instance.
   Lifetime is allocated in row_lock_acquire, freed when the trx releases
   the lock (commit/rollback) or when the wait is aborted (deadlock,
   timeout, kill).  Lives on exactly one of:
     - lock->granted_head  (after grant)        + trx->held_locks_head
     - lock->waiting_head  (before grant)       + trx->waiting_on
   list_next chains the per-lock list (granted or waiting).
   held_next chains the per-trx held-list. */
struct tdb_lock_request_t
{
    tidesdb_trx_t *trx;
    struct tdb_row_lock_t *lock;
    tdb_lock_mode_t mode;
    bool granted;
    tdb_lock_request_t *list_next; /* in lock->granted_head OR lock->waiting_head */
    tdb_lock_request_t *held_next; /* in trx->held_locks_head (granted requests only) */
};

/* Lock-table entry.  Granted and waiting lists are mutex-guarded by the
   owning partition's mutex.  Lock entry memory is never my_free'd during
   runtime (only at plugin deinit), so deadlock walkers can read these
   pointers from other partitions without worrying about freed memory.
   An entry is either threaded into part->chain (active) or part->freelist
   (idle); the hash_next field doubles as the freelist link when idle. */
struct tdb_row_lock_t
{
    uchar *pk;                        /* heap-allocated key bytes */
    uint pk_len;                      /* length of key bytes */
    tdb_lock_request_t *granted_head; /* mutex-guarded */
    tdb_lock_request_t *waiting_head; /* mutex-guarded FIFO head */
    tdb_lock_request_t *waiting_tail; /* mutex-guarded FIFO tail; lets append
                                         skip the O(n) walk to find it */
    mysql_cond_t cond;                /* waiters sleep on this */
    tdb_row_lock_t *hash_next;        /* mutex-guarded; chain when active,
                                         freelist when idle */
    uint partition;                   /* which partition (cached for release) */
};

/* Cache-line aligned so unrelated partitions never share a 64 B line and
   ping-pong on every acquire.  alignas(64) also forces sizeof(struct) to
   round up to a 64 B multiple, so the partition array indexes line up
   with cache lines. */
struct alignas(64) tdb_lock_partition_t
{
    mysql_mutex_t mutex;
    tdb_row_lock_t *chain;    /* head of active hash chain */
    tdb_row_lock_t *freelist; /* head of idle-slot list, reuse before malloc */
};

static tdb_lock_partition_t *lock_partitions = NULL;

static inline uint tdb_lock_part(const uchar *key, uint len)
{
    uint64_t h = XXH3_64bits(key, len);
    return (uint)(h % row_lock_partitions);
}

/* S/S compatible; everything else conflicts. */
static inline bool tdb_lock_modes_compatible(tdb_lock_mode_t held, tdb_lock_mode_t want)
{
    return held == TDB_LOCK_MODE_S && want == TDB_LOCK_MODE_S;
}

/* If the slot has no granted or waiting requests, unlink it from the
   partition's active chain and push it onto the freelist.  Caller must
   hold the partition mutex.  Slot memory survives so the deadlock
   walker can still safely dereference any cross-partition pointer it
   captured before we dropped the cross-partition mutex. */
static inline void tdb_lock_freelist_if_empty(tdb_lock_partition_t *part, tdb_row_lock_t *lock)
{
    if (lock->granted_head != NULL || lock->waiting_head != NULL) return;
    tdb_row_lock_t **cp = &part->chain;
    while (*cp && *cp != lock) cp = &(*cp)->hash_next;
    if (*cp == lock)
    {
        *cp = lock->hash_next;
        lock->hash_next = part->freelist;
        part->freelist = lock;
    }
}

/* Find or create a lock entry in the partition's hash chain.
   Caller must hold partition mutex.

   The chain holds only entries with at least one granted or waiting
   request, so its length tracks concurrent active locks for this
   partition, not lifetime keys.  Released slots are unlinked from the
   chain and pushed onto part->freelist by row_locks_release_all; we pop
   from the freelist before mallocing.  Slot memory is retained across
   reuse so lock-free deadlock walkers from other partitions can still
   safely dereference any tdb_row_lock_t pointer they captured before we
   dropped the cross-partition mutex.  Walkers never dereference an
   entry's pk, so a key rewrite during freelist reuse is invisible to
   them, and the partition field stays stable because slots are only
   ever reused within their original partition. */
static tdb_row_lock_t *tdb_lock_find_or_create(tdb_lock_partition_t *part, uint part_idx,
                                               const uchar *pk, uint pk_len)
{
    ulong chain_len = 0;
    for (tdb_row_lock_t *e = part->chain; e; e = e->hash_next)
    {
        chain_len++;
        if (e->pk_len == pk_len && memcmp(e->pk, pk, pk_len) == 0)
        {
            long long prev = srv_stat_lock_chain_max.load(std::memory_order_relaxed);
            while ((long long)chain_len > prev &&
                   !srv_stat_lock_chain_max.compare_exchange_weak(prev, (long long)chain_len,
                                                                  std::memory_order_relaxed))
                ;
            return e;
        }
    }
    /* Sample chain depth after a miss as well, so a single-row hotspot
       behind a long chain still surfaces in status. */
    {
        long long prev = srv_stat_lock_chain_max.load(std::memory_order_relaxed);
        while ((long long)chain_len > prev &&
               !srv_stat_lock_chain_max.compare_exchange_weak(prev, (long long)chain_len,
                                                              std::memory_order_relaxed))
            ;
    }

    if (part->freelist)
    {
        tdb_row_lock_t *e = part->freelist;
        part->freelist = e->hash_next;
        uchar *new_pk = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, pk_len, MYF(0));
        if (!new_pk)
        {
            /* Put the slot back so the next caller can try again. */
            e->hash_next = part->freelist;
            part->freelist = e;
            return NULL;
        }
        memcpy(new_pk, pk, pk_len);
        my_free(e->pk);
        e->pk = new_pk;
        e->pk_len = pk_len;
        /* granted_head and waiting_head were NULL when the slot was
           freelisted; cond/partition stay across reuse. */
        e->hash_next = part->chain;
        part->chain = e;
        srv_stat_lock_entry_recycles.fetch_add(1, std::memory_order_relaxed);
        return e;
    }

    tdb_row_lock_t *e =
        (tdb_row_lock_t *)my_malloc(PSI_NOT_INSTRUMENTED, sizeof(tdb_row_lock_t), MYF(MY_ZEROFILL));
    if (!e) return NULL;
    e->pk = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, pk_len, MYF(0));
    if (!e->pk)
    {
        my_free(e);
        return NULL;
    }
    memcpy(e->pk, pk, pk_len);
    e->pk_len = pk_len;
    e->granted_head = NULL;
    e->waiting_head = NULL;
    e->partition = part_idx;
    mysql_cond_init(0, &e->cond, NULL);
    e->hash_next = part->chain;
    part->chain = e;
    srv_stat_lock_entries.fetch_add(1, std::memory_order_relaxed);
    return e;
}

static tdb_lock_request_t *tdb_lock_request_alloc(tidesdb_trx_t *trx, tdb_row_lock_t *lock,
                                                  tdb_lock_mode_t mode, bool granted)
{
    tdb_lock_request_t *req = (tdb_lock_request_t *)my_malloc(
        PSI_NOT_INSTRUMENTED, sizeof(tdb_lock_request_t), MYF(MY_ZEROFILL));
    if (!req) return NULL;
    req->trx = trx;
    req->lock = lock;
    req->mode = mode;
    req->granted = granted;
    req->list_next = NULL;
    req->held_next = NULL;
    return req;
}

/* Find the granted request held by `trx` on `lock`, or NULL.
   Caller must hold partition mutex. */
static tdb_lock_request_t *tdb_lock_find_self_granted(tdb_row_lock_t *lock, tidesdb_trx_t *trx)
{
    for (tdb_lock_request_t *r = lock->granted_head; r; r = r->list_next)
    {
        if (r->trx == trx) return r;
    }
    return NULL;
}

/* Append req to the lock's waiting FIFO.  Caller must hold partition mutex.
   The lock keeps a tail pointer so the append is O(1) instead of walking
   the queue, which under contention could turn appending into O(n^2). */
static void tdb_lock_waiting_append(tdb_row_lock_t *lock, tdb_lock_request_t *req)
{
    req->list_next = NULL;
    if (!lock->waiting_head)
    {
        lock->waiting_head = req;
        lock->waiting_tail = req;
        return;
    }
    lock->waiting_tail->list_next = req;
    lock->waiting_tail = req;
}

/* Remove req from the lock's waiting list (if present).  Caller must hold
   partition mutex.  Safe to call when req is not on the list. */
static void tdb_lock_waiting_remove(tdb_row_lock_t *lock, tdb_lock_request_t *req)
{
    tdb_lock_request_t **pp = &lock->waiting_head;
    tdb_lock_request_t *prev = NULL;
    while (*pp && *pp != req)
    {
        prev = *pp;
        pp = &(*pp)->list_next;
    }
    if (*pp == req)
    {
        *pp = req->list_next;
        if (lock->waiting_tail == req) lock->waiting_tail = prev;
        req->list_next = NULL;
    }
}

/* Can a new request of mode `want` be granted given the current granted set?
   For S, also blocks if any waiting X exists (writer fairness).
   Caller must hold partition mutex. */
static bool tdb_lock_can_grant(tdb_row_lock_t *lock, tdb_lock_mode_t want, tidesdb_trx_t *self)
{
    for (tdb_lock_request_t *r = lock->granted_head; r; r = r->list_next)
    {
        if (r->trx == self) continue; /* self never blocks self */
        if (!tdb_lock_modes_compatible(r->mode, want)) return false;
    }
    if (want == TDB_LOCK_MODE_S)
    {
        for (tdb_lock_request_t *r = lock->waiting_head; r; r = r->list_next)
        {
            if (r->trx == self) continue;
            if (r->mode == TDB_LOCK_MODE_X) return false;
        }
    }
    return true;
}

/* Move newly-grantable waiters from waiting_head to granted_head.
   Caller must hold partition mutex; caller is responsible for broadcasting
   the lock's cond after this returns so promoted waiters wake up and link
   themselves into their trx->held_locks_head. */
static void tdb_lock_promote_waiters(tdb_row_lock_t *lock)
{
    while (lock->waiting_head)
    {
        tdb_lock_request_t *head = lock->waiting_head;
        if (!tdb_lock_can_grant(lock, head->mode, head->trx)) break;
        lock->waiting_head = head->list_next;
        if (!lock->waiting_head) lock->waiting_tail = NULL;
        head->list_next = lock->granted_head;
        lock->granted_head = head;
        head->granted = true;
    }
}

/* Deadlock detection over the wait-for graph.  DFS across every
   conflicting holder per hop -- the previous single-hop walker followed
   only the first conflicting holder and silently missed cycles that
   passed through later holders.  Frontier is a small fixed-capacity
   stack so we keep the same "no allocation under the lock partition
   mutex" property the original walker had.

   Lock entries are never freed at runtime so the pointers stored on the
   stack and in `visited` are always safe to follow; trx structs outlive
   any held lock so holder->waiting_on_lock dereferences stay valid.
   Bounded by DEADLOCK_MAX_DEPTH.  When the frontier overflows the cap
   we return true: a false-positive triggers a cheap retry, while a
   false-negative becomes a 50 s lock-wait-timeout stall. */
static bool tdb_lock_would_deadlock(tidesdb_trx_t *requestor, tdb_row_lock_t *target_lock,
                                    tdb_lock_mode_t want_mode)
{
    struct frame_t
    {
        tdb_row_lock_t *lock;
        tdb_lock_mode_t mode;
    };
    /* Cap the frontier at DEADLOCK_MAX_DEPTH*4 so that even highly-fanned
       wait-for graphs fit without allocation; pop-order is LIFO so we
       still bound the longest single path at DEADLOCK_MAX_DEPTH. */
    constexpr int FRONTIER_CAP = DEADLOCK_MAX_DEPTH * 4;
    frame_t frontier[FRONTIER_CAP];
    int top = 0;
    frontier[top++] = {target_lock, want_mode};

    /* Visited set, also fixed capacity.  Same overflow contract --
       hitting it means "give up and call it a deadlock". */
    tdb_row_lock_t *visited[FRONTIER_CAP];
    int visited_count = 0;

    int hops = 0;
    while (top > 0)
    {
        if (++hops > DEADLOCK_MAX_DEPTH) return true;

        frame_t f = frontier[--top];
        tdb_row_lock_t *cur_lock = f.lock;
        tdb_lock_mode_t cur_mode = f.mode;

        /* Skip locks we've already inspected at this or higher fanout. */
        bool already = false;
        for (int i = 0; i < visited_count; i++)
            if (visited[i] == cur_lock)
            {
                already = true;
                break;
            }
        if (already) continue;
        if (visited_count >= FRONTIER_CAP) return true;
        visited[visited_count++] = cur_lock;

        tdb_lock_partition_t *part = &lock_partitions[cur_lock->partition];

        mysql_mutex_lock(&part->mutex);
        for (tdb_lock_request_t *h = cur_lock->granted_head; h; h = h->list_next)
        {
            tidesdb_trx_t *holder = h->trx;
            if (!holder) continue;
            if (tdb_lock_modes_compatible(h->mode, cur_mode)) continue;
            if (holder == requestor)
            {
                mysql_mutex_unlock(&part->mutex);
                return true;
            }
            tdb_row_lock_t *next_lock = holder->waiting_on_lock.load(std::memory_order_acquire);
            if (!next_lock) continue;
            if (top >= FRONTIER_CAP)
            {
                mysql_mutex_unlock(&part->mutex);
                return true;
            }
            frontier[top++] = {next_lock, holder->waiting_on_mode};
        }
        mysql_mutex_unlock(&part->mutex);
    }
    return false;
}

/*
  Acquire a row lock in the given mode.  Returns 0 on success, an
  HA_ERR_* code on failure.  Re-entrant for same/weaker mode; rejects
  S->X upgrades that would self-deadlock as HA_ERR_LOCK_DEADLOCK.
*/
static int row_lock_acquire(tidesdb_trx_t *trx, const uchar *key, uint len, THD *thd,
                            tdb_lock_mode_t mode)
{
    if (!lock_partitions || !trx) return 0;

    uint part_idx = tdb_lock_part(key, len);
    tdb_lock_partition_t *part = &lock_partitions[part_idx];

    mysql_mutex_lock(&part->mutex);

    tdb_row_lock_t *lock = tdb_lock_find_or_create(part, part_idx, key, len);
    if (!lock)
    {
        mysql_mutex_unlock(&part->mutex);
        return HA_ERR_OUT_OF_MEM;
    }

    /* Re-entry, do we already hold this lock? */
    tdb_lock_request_t *self = tdb_lock_find_self_granted(lock, trx);
    if (self)
    {
        if (self->mode == TDB_LOCK_MODE_X || self->mode == mode)
        {
            /* X subsumes S; same-mode is identity. */
            mysql_mutex_unlock(&part->mutex);
            return 0;
        }
        /* self->mode == S, want X -- upgrade.  Allowed only when we are the
           sole granted holder AND no waiters are queued; otherwise we'd
           block on ourselves indirectly through our own S-grant. */
        if (lock->granted_head == self && self->list_next == NULL && !lock->waiting_head)
        {
            self->mode = TDB_LOCK_MODE_X;
            mysql_mutex_unlock(&part->mutex);
            return 0;
        }
        mysql_mutex_unlock(&part->mutex);
        srv_stat_lock_deadlocks.fetch_add(1, std::memory_order_relaxed);
        return HA_ERR_LOCK_DEADLOCK;
    }

    /* Fresh request from this trx. */
    if (tdb_lock_can_grant(lock, mode, trx))
    {
        tdb_lock_request_t *req = tdb_lock_request_alloc(trx, lock, mode, true);
        if (!req)
        {
            mysql_mutex_unlock(&part->mutex);
            return HA_ERR_OUT_OF_MEM;
        }
        req->list_next = lock->granted_head;
        lock->granted_head = req;
        req->held_next = trx->held_locks_head;
        trx->held_locks_head = req;
        mysql_mutex_unlock(&part->mutex);
        srv_stat_lock_held.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    /* Need to wait.  Append to the lock's FIFO waiting queue and publish
       the lock and mode this trx is blocked on so the deadlock walker can
       follow the wait-for edge without ever dereferencing a request
       struct from another partition. */
    tdb_lock_request_t *req = tdb_lock_request_alloc(trx, lock, mode, false);
    if (!req)
    {
        mysql_mutex_unlock(&part->mutex);
        return HA_ERR_OUT_OF_MEM;
    }
    tdb_lock_waiting_append(lock, req);
    trx->waiting_on_mode = mode;
    trx->waiting_on_lock.store(lock, std::memory_order_release);
    mysql_mutex_unlock(&part->mutex);

    bool deadlock = tdb_lock_would_deadlock(trx, lock, mode);

    mysql_mutex_lock(&part->mutex);

    if (deadlock)
    {
        /* Between dropping the mutex for the wait-for walk and re-acquiring
           it, another transaction's release path may have called
           promote_waiters and moved our request from waiting_head onto
           granted_head, flipping req->granted to true.  In that case the
           walker's verdict is based on stale state and the lock is already
           ours.  Taking the grant is correct and avoids a serious UAF --
           freeing the request while it sits on granted_head would leave a
           dangling pointer that the next acquire walks into. */
        if (req->granted)
        {
            req->held_next = trx->held_locks_head;
            trx->held_locks_head = req;
            trx->waiting_on_lock.store(NULL, std::memory_order_relaxed);
            mysql_mutex_unlock(&part->mutex);
            srv_stat_lock_held.fetch_add(1, std::memory_order_relaxed);
            return 0;
        }
        tdb_lock_waiting_remove(lock, req);
        trx->waiting_on_lock.store(NULL, std::memory_order_relaxed);
        tdb_lock_freelist_if_empty(part, lock);
        mysql_mutex_unlock(&part->mutex);
        my_free(req);
        srv_stat_lock_deadlocks.fetch_add(1, std::memory_order_relaxed);
        return HA_ERR_LOCK_DEADLOCK;
    }

    /* Holders may have released while we were walking the wait-for graph.
       Promote any newly-grantable waiters, then check whether we got our
       grant in that pass. */
    tdb_lock_promote_waiters(lock);

    /* Bounded wait until our request is granted, the wait times out, or
       the connection is killed.  kill_query wakes us by broadcasting on
       lock->cond. */
    bool killed = false;
    bool timed_out = false;
    const ulong timeout_ms = tdb_lock_wait_timeout_ms(thd);
    const bool bounded = (timeout_ms > 0);
    struct timespec deadline;
    if (bounded) set_timespec_nsec(deadline, (ulonglong)timeout_ms * TDB_NS_PER_MS);

    auto wait_t0 = std::chrono::steady_clock::now();
    srv_stat_lock_waits.fetch_add(1, std::memory_order_relaxed);

    while (!req->granted)
    {
        if (thd && thd_killed(thd))
        {
            killed = true;
            break;
        }
        if (bounded)
        {
            int wrc = mysql_cond_timedwait(&lock->cond, &part->mutex, &deadline);
            if (wrc == ETIMEDOUT && !req->granted)
            {
                timed_out = true;
                break;
            }
        }
        else
        {
            mysql_cond_wait(&lock->cond, &part->mutex);
        }
    }

    auto wait_us = std::chrono::duration_cast<std::chrono::microseconds>(
                       std::chrono::steady_clock::now() - wait_t0)
                       .count();
    srv_stat_lock_wait_us.fetch_add(wait_us, std::memory_order_relaxed);

    if (killed || timed_out)
    {
        tdb_lock_waiting_remove(lock, req);
        trx->waiting_on_lock.store(NULL, std::memory_order_relaxed);
        /* Removing us may have unblocked an X behind a string of S
           waiters.  Re-evaluate and broadcast so any newly-granted
           waiter wakes up. */
        tdb_lock_promote_waiters(lock);
        bool wake = (lock->waiting_head != NULL) || (lock->granted_head != NULL);
        tdb_lock_freelist_if_empty(part, lock);
        mysql_mutex_unlock(&part->mutex);
        if (wake) mysql_cond_broadcast(&lock->cond);
        my_free(req);
        if (timed_out) srv_stat_lock_timeouts.fetch_add(1, std::memory_order_relaxed);
        return HA_ERR_LOCK_WAIT_TIMEOUT;
    }

    /* Granted.  tdb_lock_promote_waiters moved us onto granted_head;
       link onto trx->held_locks_head and clear waiting_on_lock so the
       walker no longer treats this trx as waiting. */
    req->held_next = trx->held_locks_head;
    trx->held_locks_head = req;
    trx->waiting_on_lock.store(NULL, std::memory_order_relaxed);
    mysql_mutex_unlock(&part->mutex);
    srv_stat_lock_held.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

/*
  Release all row locks held by this transaction.  Walks the trx's
  held-list of requests, unlinks each from its lock's granted list,
  promotes any waiters now compatible with the remaining granted set,
  and broadcasts the lock's cond.  Called from commit and rollback.
*/
static void row_locks_release_all(tidesdb_trx_t *trx)
{
    if (!lock_partitions || !trx) return;

    long long released = 0;
    tdb_lock_request_t *req = trx->held_locks_head;
    while (req)
    {
        tdb_lock_request_t *next = req->held_next;
        tdb_row_lock_t *lock = req->lock;
        uint part_idx = lock->partition;
        tdb_lock_partition_t *part = &lock_partitions[part_idx];

        mysql_mutex_lock(&part->mutex);

        /* Unlink req from lock->granted_head. */
        tdb_lock_request_t **pp = &lock->granted_head;
        while (*pp && *pp != req) pp = &(*pp)->list_next;
        if (*pp == req) *pp = req->list_next;

        /* Promote any waiters now grantable, then wake them up. */
        bool had_waiters = (lock->waiting_head != NULL);
        tdb_lock_promote_waiters(lock);
        bool promoted_any = had_waiters && (lock->granted_head != NULL);

        /* If nothing references this slot any more, unlink it from the
           hash chain and stash it on the partition freelist so the next
           acquire can reuse it without growing the chain.  Slot memory
           is retained across reuse for the deadlock walker. */
        tdb_lock_freelist_if_empty(part, lock);

        mysql_mutex_unlock(&part->mutex);

        if (had_waiters && (promoted_any || lock->waiting_head == NULL))
            mysql_cond_broadcast(&lock->cond);

        my_free(req);
        released++;
        req = next;
    }
    trx->held_locks_head = NULL;
    trx->waiting_on_lock.store(NULL, std::memory_order_relaxed);
    if (released > 0) srv_stat_lock_held.fetch_sub(released, std::memory_order_relaxed);
}

/* Pick the lock mode for a row materialised on a read path, or report
   that no lock is needed.
     - write_intent ........ X (covers SELECT FOR UPDATE / UPDATE / DELETE)
     - REPEATABLE_READ / SERIALIZABLE ... S (prevents concurrent modification
       of read rows within the txn; phantom prevention is incomplete
       because we have no range/gap locks, only row locks)
     - READ_COMMITTED / SNAPSHOT ... no lock (MVCC snapshot suffices) */
static inline bool tdb_lock_mode_for_read(THD *thd, bool write_intent, tdb_lock_mode_t *mode)
{
    if (write_intent)
    {
        *mode = TDB_LOCK_MODE_X;
        return true;
    }
    int iso = thd ? thd_tx_isolation(thd) : ISO_READ_COMMITTED;
    if (iso == ISO_REPEATABLE_READ || iso == ISO_SERIALIZABLE)
    {
        *mode = TDB_LOCK_MODE_S;
        return true;
    }
    return false;
}

static handler *tidesdb_create_handler(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root);
static void tidesdb_refresh_status_vars();

/* Forward declarations for the tombstone aggregates so tidesdb_show_status
   (defined earlier than the storage block) can read them. */
static long long srv_stat_total_tombstones;
static double srv_stat_tombstone_ratio;
static double srv_stat_max_sst_density;
static long long srv_stat_max_sst_density_level;

/* File extensions -- TidesDB manages its own files */
static const char *ha_tidesdb_exts[] = {NullS};

/* ******************** Full-Text Search helpers ******************** */

/* MariaDB renamed HA_FULLTEXT -> HA_FULLTEXT_legacy after the 11.x series
   (flag bit 128 unchanged).  Detect via the flag: KEY::algorithm is only set
   to HA_KEY_ALG_FULLTEXT on newer servers, and notably not in the ALTER
   key_info_buffer on 11.4, so the algorithm-only check missed FULLTEXT adds. */
#ifndef HA_FULLTEXT
#define HA_FULLTEXT HA_FULLTEXT_legacy
#endif

static inline bool is_fts_index(const KEY *ki)
{
    return (ki->flags & HA_FULLTEXT) || ki->algorithm == HA_KEY_ALG_FULLTEXT;
}

/* FTS result entry -- one per matching document */
struct tdb_fts_result_t
{
    uchar *pk; /* heap-allocated comparable PK bytes */
    uint pk_len;
    float rank; /* BM25 score */
};

/* FTS search context returned by ft_init_ext as FT_INFO* */
struct tdb_ft_info_t
{
    struct _ft_vft *please;                /* required by MariaDB FT_INFO layout */
    struct _ft_vft_ext *could_you;         /* extended FT API (HA_CAN_FULLTEXT_EXT) */
    ha_tidesdb *handler;                   /* back-pointer for row fetching */
    uint keynr;                            /* which FTS index */
    std::vector<tdb_fts_result_t> results; /* sorted by rank descending */
    size_t current_idx;                    /* iteration position */
    float current_rank;                    /* rank of last-returned row */
    ulonglong match_count;                 /* total matches for count_matches() */
};

/* Forward declarations of FT_INFO vtable callbacks */
static int tdb_fts_read_next(FT_INFO *, char *);
static float tdb_fts_find_relevance(FT_INFO *, uchar *, uint);
static void tdb_fts_close_search(FT_INFO *);
static float tdb_fts_get_relevance(FT_INFO *);
static void tdb_fts_reinit_search(FT_INFO *);

static const struct _ft_vft tdb_ft_vft = {tdb_fts_read_next, tdb_fts_find_relevance,
                                          tdb_fts_close_search, tdb_fts_get_relevance,
                                          tdb_fts_reinit_search};

/* Extended FT API callbacks for HA_CAN_FULLTEXT_EXT */
static uint tdb_fts_get_version()
{
    return 2;
}

static ulonglong tdb_fts_get_flags()
{
    return FTS_ORDERED_RESULT;
}

static ulonglong tdb_fts_get_docid(FT_INFO_EXT *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    if (info->current_idx > 0 && info->current_idx <= info->results.size())
        return (ulonglong)(info->current_idx); /* 1-based doc ID */
    return 0;
}

static ulonglong tdb_fts_count_matches(FT_INFO_EXT *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    return info->match_count;
}

static struct _ft_vft_ext tdb_ft_vft_ext = {tdb_fts_get_version, tdb_fts_get_flags,
                                            tdb_fts_get_docid, tdb_fts_count_matches};

/* FT_INFO vtable callback implementations */
static int tdb_fts_read_next(FT_INFO *, char *)
{
    return HA_ERR_END_OF_FILE; /* not used -- ft_read() is the entry point */
}

static float tdb_fts_find_relevance(FT_INFO *fts, uchar *, uint)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    return info->current_rank;
}

static float tdb_fts_get_relevance(FT_INFO *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    return info->current_rank;
}

static void tdb_fts_close_search(FT_INFO *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    for (auto &r : info->results) my_free(r.pk);
    delete info;
}

static void tdb_fts_reinit_search(FT_INFO *fts)
{
    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(fts);
    info->current_idx = 0;
}

/* Maximum term byte length in the FTS index.  Terms longer than this
   are truncated.  512 bytes accommodates even long CJK compound words
   (170+ 3-byte UTF-8 characters). */
static constexpr uint FTS_MAX_TERM_BYTES = 512;

/* Size of the leading 2-byte little-endian term-length field on every
   FTS inverted-index entry key. */
static constexpr uint FTS_TERM_LEN_PREFIX = 2;

/* Worst-case FTS entry key buffer-- [2B term_len][term bytes][PK]. */
static constexpr uint FTS_KEY_BUF_LEN = FTS_TERM_LEN_PREFIX + FTS_MAX_TERM_BYTES + MAX_KEY_LENGTH;

/* FTS entry value layout-- [2B tf LE][4B doc_len LE] = 6 bytes. */
static constexpr uint FTS_VALUE_TF_LEN = 2;
static constexpr uint FTS_VALUE_DOC_LEN_OFFSET = FTS_VALUE_TF_LEN;
static constexpr uint FTS_VALUE_DOC_LEN_LEN = 4;
static constexpr uint FTS_VALUE_LEN = FTS_VALUE_TF_LEN + FTS_VALUE_DOC_LEN_LEN;

/* FTS per-index meta key layout:
   [KEY_NS_META(1B)][FTS tag(4B incl NUL)][keynr(1B)] = 6 bytes.
   Meta value layout-- [8B total_docs][8B total_words] = 16 bytes. */
static constexpr const char FTS_META_KEY_TAG[] = "FTS\x00";
static constexpr uint FTS_META_KEY_TAG_LEN = 4; /* 3 letters + trailing NUL */
static constexpr uint FTS_META_KEY_TAG_OFFSET = KEY_NAMESPACE_LEN;
static constexpr uint FTS_META_KEY_KEYNR_OFFSET = FTS_META_KEY_TAG_OFFSET + FTS_META_KEY_TAG_LEN;
static constexpr uint FTS_META_KEY_LEN = FTS_META_KEY_KEYNR_OFFSET + 1;
static constexpr uint FTS_META_VALUE_DOCS_LEN = 8;
static constexpr uint FTS_META_VALUE_WORDS_OFFSET = FTS_META_VALUE_DOCS_LEN;
static constexpr uint FTS_META_VALUE_WORDS_LEN = 8;
static constexpr uint FTS_META_VALUE_LEN = FTS_META_VALUE_DOCS_LEN + FTS_META_VALUE_WORDS_LEN;

/* Build an FTS inverted index key:
   [2-byte term_len LE][lowercased term bytes][comparable PK bytes]
   Returns total key length.  Term is silently truncated to FTS_MAX_TERM_BYTES. */
static uint fts_build_key(const char *term, uint term_len, const uchar *pk, uint pk_len, uchar *out)
{
    if (term_len > FTS_MAX_TERM_BYTES) term_len = FTS_MAX_TERM_BYTES;
    uint pos = 0;
    int2store(out + pos, (uint16)term_len);
    pos += FTS_TERM_LEN_PREFIX;
    memcpy(out + pos, term, term_len);
    pos += term_len;
    memcpy(out + pos, pk, pk_len);
    pos += pk_len;
    return pos;
}

/* Build FTS value ( [2-byte tf LE][4-byte doc_len LE] ) = FTS_VALUE_LEN bytes */
static uint fts_build_value(uint16 tf, uint32 doc_len, uchar *out)
{
    int2store(out, tf);
    int4store(out + FTS_VALUE_DOC_LEN_OFFSET, doc_len);
    return FTS_VALUE_LEN;
}

/* Read or initialize FTS metadata counters from the data CF.
   Key format-- [KEY_NS_META][FTS tag][keynr].
   Returns TDB_SUCCESS on a found row, TDB_ERR_NOT_FOUND for a fresh index
   (totals zeroed), or the library's error code otherwise.  Callers must
   not write back a zeroed total derived from a transient read failure --
   that would clobber the real counters and degrade BM25 IDF. */
static int fts_load_meta(tidesdb_txn_t *txn, tidesdb_column_family_t *data_cf, uint keynr,
                         int64_t *total_docs, int64_t *total_words)
{
    uchar mk[FTS_META_KEY_LEN];
    mk[0] = KEY_NS_META;
    memcpy(mk + FTS_META_KEY_TAG_OFFSET, FTS_META_KEY_TAG, FTS_META_KEY_TAG_LEN);
    mk[FTS_META_KEY_KEYNR_OFFSET] = (uchar)keynr;

    uint8_t *val = NULL;
    size_t vlen = 0;
    *total_docs = 0;
    *total_words = 0;

    int rc = tidesdb_txn_get(txn, data_cf, mk, FTS_META_KEY_LEN, &val, &vlen);
    if (rc == TDB_SUCCESS && vlen >= FTS_META_VALUE_LEN)
    {
        *total_docs = sint8korr(val);
        *total_words = sint8korr(val + FTS_META_VALUE_WORDS_OFFSET);
        tidesdb_free(val);
        return TDB_SUCCESS;
    }
    if (val) tidesdb_free(val);
    /* TDB_ERR_NOT_FOUND is the legitimate empty-index case. */
    return rc;
}

/* Update FTS metadata counters atomically within the current transaction.
   thd may be NULL for paths where no session is available (e.g. recovery);
   in that case the back-pressure block falls through to the unwrapped put. */
static int fts_update_meta(THD *thd, tidesdb_txn_t *txn, tidesdb_column_family_t *data_cf,
                           uint keynr, int64_t delta_docs, int64_t delta_words)
{
    int64_t total_docs = 0, total_words = 0;
    /* A transient read failure must not be turned into a zero-based
       write-back: zero - delta clamped at 0 would persist garbage and
       only manual rebuild would fix BM25.  Fresh index (NOT_FOUND) is
       the only "no prior value" case we treat as zero. */
    int rrc = fts_load_meta(txn, data_cf, keynr, &total_docs, &total_words);
    if (rrc != TDB_SUCCESS && rrc != TDB_ERR_NOT_FOUND)
    {
        sql_print_error(
            "[TIDESDB] fts_update_meta: skipping meta write for keynr=%u "
            "because fts_load_meta failed (rc=%d); BM25 totals are unchanged",
            keynr, rrc);
        return rrc;
    }

    total_docs += delta_docs;
    total_words += delta_words;
    if (total_docs < 0) total_docs = 0;
    if (total_words < 0) total_words = 0;

    uchar mk[FTS_META_KEY_LEN];
    mk[0] = KEY_NS_META;
    memcpy(mk + FTS_META_KEY_TAG_OFFSET, FTS_META_KEY_TAG, FTS_META_KEY_TAG_LEN);
    mk[FTS_META_KEY_KEYNR_OFFSET] = (uchar)keynr;

    uchar mv[FTS_META_VALUE_LEN];
    int8store(mv, total_docs);
    int8store(mv + FTS_META_VALUE_WORDS_OFFSET, total_words);
    return tdb_txn_put_blocking(thd, txn, data_cf, mk, FTS_META_KEY_LEN, mv, FTS_META_VALUE_LEN,
                                TIDESDB_TTL_NONE);
}

/* Fold a per-row FTS meta delta into the txn-level accumulator.  Find the
   matching (data_cf, keynr) entry and combine, or append a new one.  The
   list is typically tiny (one or two FTS indexes per touched table), so
   linear scan beats a hash. */
static inline void trx_fts_meta_accumulate(tidesdb_trx_t *trx, tidesdb_column_family_t *cf,
                                           uint keynr, int64_t doc_delta, int64_t word_delta)
{
    if (!trx) return;
    for (auto &e : trx->fts_meta_pending)
    {
        if (e.data_cf == cf && e.keynr == keynr)
        {
            e.doc_delta += doc_delta;
            e.word_delta += word_delta;
            trx->fts_meta_dirty = true;
            return;
        }
    }
    trx->fts_meta_pending.push_back({cf, keynr, doc_delta, word_delta});
    trx->fts_meta_dirty = true;
}

/* Apply every accumulated FTS meta delta to its index's meta key inside
   the current txn.  Called before tidesdb_commit hands the txn to the
   library and before maybe_bulk_commit's mid-statement commit so the meta
   update is part of the same commit as the row puts that produced it.
   Returns a TDB_* error code on the first failure; the accumulator is
   cleared in every case since the txn it tracks is about to commit or be
   rolled back. */
static int flush_trx_fts_meta_pending(THD *thd, tidesdb_trx_t *trx)
{
    if (!trx) return TDB_SUCCESS;
    if (!trx->fts_meta_dirty || trx->fts_meta_pending.empty() || !trx->txn)
    {
        trx->fts_meta_pending.clear();
        trx->fts_meta_dirty = false;
        return TDB_SUCCESS;
    }
    int rc = TDB_SUCCESS;
    for (const auto &e : trx->fts_meta_pending)
    {
        rc = fts_update_meta(thd, trx->txn, e.data_cf, e.keynr, e.doc_delta, e.word_delta);
        if (rc != TDB_SUCCESS) break;
    }
    trx->fts_meta_pending.clear();
    trx->fts_meta_dirty = false;
    return rc;
}

/* Tokenize a text string using MariaDB's default FT parser.
   Returns lowercased tokens suitable for FTS indexing. */
struct fts_token_t
{
    std::string word;
};

/* Minimum and maximum word length for FTS indexing (in characters).
   These mirror InnoDB's innodb_ft_min_token_size / innodb_ft_max_token_size
   defaults.  Exposed as session variables below for tuning. */
static ulong srv_fts_min_word_len = 3;
static ulong srv_fts_max_word_len = 84;

/* Blend characters -- characters that are indexed as both separators and valid
   word characters.  When a blend char appears inside a token, the tokenizer
   emits three tokens -- the full blended form, and the two parts on each side.
   For example, with blend_chars="'" and input "l'aria":
     -- "l'aria" (full blended token)
     -- "l"      (left part, may be filtered by min_word_len)
     -- "aria"   (right part)
   This allows Italian/French elision (dell'aria, l'homme) and Irish/Scottish
   names (O'Malley) to be searchable by any component or the full form.
   Default is empty (no blend characters). Set to "'" for Romance languages. */
static char *srv_fts_blend_chars = NULL;

/* Fast lookup table for blend characters, indexed by raw byte value
   (covers the full 8-bit range).  Rebuilt when the sysvar changes. */
static constexpr uint TDB_BLEND_MAP_SIZE = 256;
static bool tdb_blend_char_map[TDB_BLEND_MAP_SIZE] = {false};
static mysql_rwlock_t tdb_blend_lock;
static PSI_rwlock_key tdb_blend_lock_key;

static void tdb_rebuild_blend_map(const char *chars)
{
    memset(tdb_blend_char_map, 0, sizeof(tdb_blend_char_map));
    if (!chars) return;
    for (const char *p = chars; *p; p++) tdb_blend_char_map[(unsigned char)*p] = true;
}

static void tdb_fts_blend_chars_update(MYSQL_THD thd, struct st_mysql_sys_var *var, void *var_ptr,
                                       const void *save)
{
    const char *new_val = *static_cast<const char *const *>(save);
    mysql_rwlock_wrlock(&tdb_blend_lock);
    tdb_rebuild_blend_map(new_val);
    mysql_rwlock_unlock(&tdb_blend_lock);
    *static_cast<const char **>(var_ptr) = new_val;
    if (new_val && new_val[0])
        sql_print_information("[TIDESDB] FTS blend_chars set to '%s'", new_val);
    else
        sql_print_information("[TIDESDB] FTS blend_chars cleared");
}

/* Stop word support
   Mirrors InnoDB's innodb_ft_server_stopword_table.  When NULL, we use the
   36-word default list from information_schema.INNODB_FT_DEFAULT_STOPWORD.
   When set to "db/table", we read the 'value' column at next FTS rebuild.
   The stop word set is stored in a global unordered_set protected by a
   read-mostly rwlock (writes are rare -- only on SET GLOBAL or plugin init). */
static char *srv_ft_stopword_table = NULL; /* db/table or NULL for defaults */

/* InnoDB's default 36 stop words, matching INNODB_FT_DEFAULT_STOPWORD */
static const char *tdb_default_stopwords[] = {
    "a",   "about", "an",   "are", "as",   "at",  "be",  "by",   "com",  "de",
    "en",  "for",   "from", "how", "i",    "in",  "is",  "it",   "la",   "of",
    "on",  "or",    "that", "the", "this", "to",  "was", "what", "when", "where",
    "who", "will",  "with", "und", "the",  "www", NULL};

static std::unordered_set<std::string> tdb_stopwords;
static mysql_rwlock_t tdb_stopword_lock;
static PSI_rwlock_key tdb_stopword_lock_key;

/* Load stop words from the default list */
static void tdb_load_default_stopwords()
{
    tdb_stopwords.clear();
    for (const char **w = tdb_default_stopwords; *w; w++) tdb_stopwords.insert(*w);
}

/* Check if a lowercased token is a stop word.
   PRECONDITION caller holds tdb_stopword_lock for reading (taken once per
   fts_tokenize call to avoid N lock pairs per document). */
static inline bool tdb_is_stopword_locked(const std::string &word)
{
    return tdb_stopwords.count(word) > 0;
}

/* Load stop words from a user table specified as "db_name/table_name".
   Must be called with tdb_stopword_lock held for writing.
   Uses TidesDB's own CF to read the table if it's a TidesDB table,
   or falls back to an empty set with a warning for other engines.
   For simplicity, the table must store one word per row in a column named 'value'
   and be accessible as a TidesDB CF named "db_name__table_name". */
static bool tdb_load_stopwords_from_table_spec(const char *table_spec)
{
    if (!table_spec || !table_spec[0]) return false;

    const char *slash = strchr(table_spec, '/');
    if (!slash)
    {
        sql_print_warning(
            "[TIDESDB] ft_stopword_table format must be 'db_name/table_name', got '%s'",
            table_spec);
        return false;
    }

    std::string db_name(table_spec, slash - table_spec);
    std::string tbl_name(slash + 1);

    /* CF names join the database and table with CF_DB_TABLE_SEP, the same
       way path_to_cf_name builds them, so the lookup has to use that
       separator rather than the slash from the user-facing spec. */
    std::string cf_name = db_name + CF_DB_TABLE_SEP + tbl_name;
    tidesdb_column_family_t *sw_cf =
        tdb_global ? tidesdb_get_column_family(tdb_global, cf_name.c_str()) : NULL;

    if (!sw_cf)
    {
        sql_print_warning(
            "[TIDESDB] Stop word table '%s' not found as TidesDB CF '%s'. "
            "The table must be a TidesDB ENGINE table. Keeping current stop words.",
            table_spec, cf_name.c_str());
        return false;
    }

    /* We scan the CF for all keys with DATA namespace prefix.
       Each row should have a 'value' field which we extract via full table scan. */
    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return false;

    tidesdb_iter_t *iter = NULL;
    if (tdb_iter_new_blocking(current_thd, txn, sw_cf, &iter) != TDB_SUCCESS)
    {
        tidesdb_txn_free(txn);
        return false;
    }

    tidesdb_iter_seek_to_first(iter);
    tdb_stopwords.clear();

    while (tidesdb_iter_valid(iter))
    {
        uint8_t *val = NULL;
        size_t val_size = 0;
        if (tidesdb_iter_value(iter, &val, &val_size) == TDB_SUCCESS && val &&
            val_size > ROW_HEADER_SIZE && val[0] == ROW_HEADER_MAGIC)
        {
            /* The row carries the self-describing header written by
               serialize_row, so the null bitmap width is read from the
               header rather than assumed.  After the header and the bitmap
               a single-column table holds just the one packed VARCHAR. */
            uint stored_null_bytes = uint2korr(val + 1);
            size_t off = (size_t)ROW_HEADER_SIZE + stored_null_bytes;
            if (off < val_size)
            {
                const uint8_t *data = val + off;
                size_t data_len = val_size - off;

                /* Field::pack stores a VARCHAR with a one-byte length prefix
                   when the column is at most 255 chars wide and a two-byte
                   prefix otherwise.  The packed field of a single-column row
                   spans the whole remaining buffer, so the prefix width is
                   the one whose recorded length consumes exactly the rest. */
                uint prefix = 0;
                size_t str_len = 0;
                if (data_len >= 1 && (size_t)data[0] + 1 == data_len)
                {
                    prefix = 1;
                    str_len = data[0];
                }
                else if (data_len >= FIELD_VARCHAR_LEN_PREFIX &&
                         (size_t)uint2korr(data) + FIELD_VARCHAR_LEN_PREFIX == data_len)
                {
                    prefix = FIELD_VARCHAR_LEN_PREFIX;
                    str_len = uint2korr(data);
                }
                if (prefix && str_len > 0)
                {
                    std::string word((const char *)(data + prefix), str_len);
                    std::transform(word.begin(), word.end(), word.begin(), ::tolower);
                    tdb_stopwords.insert(std::move(word));
                }
            }
        }
        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);
    tidesdb_txn_free(txn);

    sql_print_information("[TIDESDB] Loaded %zu stop words from table '%s'", tdb_stopwords.size(),
                          table_spec);
    return true;
}

/* Sysvar update callback for tidesdb_ft_stopword_table */
static void tdb_ft_stopword_table_update(MYSQL_THD thd, struct st_mysql_sys_var *var, void *var_ptr,
                                         const void *save)
{
    const char *new_val = *static_cast<const char *const *>(save);
    mysql_rwlock_wrlock(&tdb_stopword_lock);

    if (!new_val || !new_val[0])
    {
        /* NULL or empty string -- we reset to defaults */
        tdb_load_default_stopwords();
        sql_print_information("[TIDESDB] Stop words reset to defaults (%zu words)",
                              tdb_stopwords.size());
    }
    else
    {
        if (!tdb_load_stopwords_from_table_spec(new_val))
        {
            sql_print_warning("[TIDESDB] Failed to load stop words from '%s', keeping current set",
                              new_val);
        }
    }

    *static_cast<const char **>(var_ptr) = new_val;
    mysql_rwlock_unlock(&tdb_stopword_lock);
}

/* BM25 tuning parameters.  k1 controls term-frequency saturation
   (higher = more weight to repeated terms).  b controls document-length
   normalization (0 = no normalization, 1 = full normalization). */
static double srv_fts_bm25_k1 = 1.2;
static double srv_fts_bm25_b = 0.75;

/* Helper to lowercase, check stop words, length filter, and emit a token */
static inline void fts_emit_token(const char *word_start, size_t byte_len, uint char_count,
                                  CHARSET_INFO *cs, std::vector<fts_token_t> &out)
{
    if (char_count < srv_fts_min_word_len || char_count > srv_fts_max_word_len) return;

    fts_token_t tok;
    tok.word.assign(word_start, byte_len);
    size_t lowered_len =
        cs->cset->casedn(cs, &tok.word[0], tok.word.size(), &tok.word[0], tok.word.size());
    tok.word.resize(lowered_len);

    if (tdb_is_stopword_locked(tok.word)) return;
    out.push_back(std::move(tok));
}

/* Charset-aware tokenizer with blend character support.
   Uses MariaDB's charset API to correctly handle multi-byte characters
   (UTF-8, UTF-16, CJK character sets, etc.).  Splits on word boundaries
   using the charset's ctype classification, lowercases using the charset's
   case-folding tables, and filters by configurable word length bounds.

   Blend characters (configured via tidesdb_fts_blend_chars) are treated as
   both word characters and separators.  When a blend char appears inside a
   token, the tokenizer emits three forms-- the full blended token, and the
   two parts on each side of the blend char.  This enables Romance language
   elision (l'aria -> l'aria + aria) and names (O'Malley -> o'malley + malley)
   to be searchable by any component or the full form. */
static void fts_tokenize(const char *text, size_t text_len, CHARSET_INFO *cs,
                         std::vector<fts_token_t> &out)
{
    const char *p = text;
    const char *end = text + text_len;
    uint mblen;

    /* We snapshot blend chars under read lock once per tokenize call */
    bool has_blend = false;
    bool blend_map_copy[TDB_BLEND_MAP_SIZE];
    {
        mysql_rwlock_rdlock(&tdb_blend_lock);
        memcpy(blend_map_copy, tdb_blend_char_map, sizeof(blend_map_copy));
        mysql_rwlock_unlock(&tdb_blend_lock);
        for (uint i = 0; i < TDB_BLEND_MAP_SIZE && !has_blend; i++)
            if (blend_map_copy[i]) has_blend = true;
    }

    /* We hold the stopword rdlock once for the whole tokenize pass.
       fts_emit_token calls tdb_is_stopword_locked which assumes the read
       lock is held -- this avoids the N lock-pair cost the previous
       per-token acquisition incurred (1000-word doc = 1000 lock pairs). */
    mysql_rwlock_rdlock(&tdb_stopword_lock);

    while (p < end)
    {
        while (p < end)
        {
            mblen = my_ismbchar(cs, p, end);
            if (mblen) break; /* multi-byte = word char */
            if (my_isalnum(cs, (uchar)*p)) break;
            if (has_blend && blend_map_copy[(uchar)*p]) break;
            p++;
        }
        if (p >= end) break;

        const char *word_start = p;
        uint char_count = 0;
        bool contains_blend = false;

        while (p < end)
        {
            mblen = my_ismbchar(cs, p, end);
            if (mblen)
            {
                p += mblen;
                char_count++;
                continue;
            }
            if (my_isalnum(cs, (uchar)*p))
            {
                p++;
                char_count++;
                continue;
            }
            if (has_blend && blend_map_copy[(uchar)*p])
            {
                contains_blend = true;
                p++;
                char_count++;
                continue;
            }
            break;
        }
        size_t byte_len = (size_t)(p - word_start);

        if (!contains_blend)
        {
            fts_emit_token(word_start, byte_len, char_count, cs, out);
        }
        else
        {
            /* Blend char found -- emit full blended token plus sub-parts.
               We split on blend chars and emit each sub-part that meets
               the minimum length requirement. */
            fts_emit_token(word_start, byte_len, char_count, cs, out);

            const char *sub_start = word_start;
            uint sub_chars = 0;
            for (const char *s = word_start; s < word_start + byte_len; s++)
            {
                if (blend_map_copy[(uchar)*s])
                {
                    size_t sub_len = (size_t)(s - sub_start);
                    if (sub_len > 0) fts_emit_token(sub_start, sub_len, sub_chars, cs, out);
                    sub_start = s + 1;
                    sub_chars = 0;
                }
                else
                {
                    sub_chars++;
                }
            }
            size_t sub_len = (size_t)((word_start + byte_len) - sub_start);
            if (sub_len > 0) fts_emit_token(sub_start, sub_len, sub_chars, cs, out);
        }
    }

    mysql_rwlock_unlock(&tdb_stopword_lock);
}

/* Extract and tokenize the document from all FULLTEXT key_part fields.
   Returns the token list and word count. */
static void fts_extract_and_tokenize(TABLE *table, const KEY *key_info, const uchar *record,
                                     CHARSET_INFO *cs, std::vector<fts_token_t> &out_tokens)
{
    std::string doc;
    my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(record - table->record[0]);

    for (uint p = 0; p < key_info->user_defined_key_parts; p++)
    {
        Field *f = key_info->key_part[p].field;
        if (ptrdiff) f->move_field_offset(ptrdiff);
        if (!f->is_null())
        {
            String val;
            f->val_str(&val);
            if (!doc.empty()) doc += ' ';
            doc.append(val.ptr(), val.length());
        }
        if (ptrdiff) f->move_field_offset(-ptrdiff);
    }

    fts_tokenize(doc.data(), doc.size(), cs, out_tokens);
}

/* Boolean query term with yesno/trunc/phrase flags from the parser */
struct fts_query_term_t
{
    std::string term;
    int yesno;  /* FTS_TERM_REQUIRED / FTS_TERM_EXCLUDED / FTS_TERM_NEUTRAL */
    bool trunc; /* prefix match (wildcard) */
    bool is_phrase;
    std::vector<std::string> phrase_words;
};

/* Boolean query parser.
   Handles               +required -excluded word* (truncated), "exact phrase", plain terms.
   Charset-aware         uses multi-byte character scanning for word boundaries. */
static void fts_parse_boolean(const char *query, size_t len, CHARSET_INFO *cs,
                              std::vector<fts_query_term_t> &out)
{
    const char *p = query;
    const char *end = query + len;

    while (p < end)
    {
        while (p < end && *p == ' ') p++;
        if (p >= end) break;

        int yesno = FTS_TERM_NEUTRAL;
        if (*p == FTS_BOOL_OP_REQUIRED)
        {
            yesno = FTS_TERM_REQUIRED;
            p++;
        }
        else if (*p == FTS_BOOL_OP_EXCLUDED)
        {
            yesno = FTS_TERM_EXCLUDED;
            p++;
        }

        while (p < end && *p == ' ') p++;
        if (p >= end) break;

        /* "word1 word2 word3" */
        if (*p == FTS_BOOL_OP_PHRASE)
        {
            p++; /* skip opening quote */
            const char *phrase_start = p;
            while (p < end && *p != FTS_BOOL_OP_PHRASE) p++;
            size_t phrase_len = (size_t)(p - phrase_start);
            if (p < end) p++; /* skip closing quote */

            if (phrase_len == 0) continue;

            std::vector<fts_token_t> phrase_tokens;
            fts_tokenize(phrase_start, phrase_len, cs, phrase_tokens);
            if (phrase_tokens.empty()) continue;

            /* Each phrase word becomes a required term for candidate filtering.
               The first word carries the phrase metadata for verification. */
            fts_query_term_t qt;
            qt.term = phrase_tokens[0].word;
            qt.yesno = yesno ? yesno : FTS_TERM_REQUIRED; /* phrases are implicitly required */
            qt.trunc = false;
            qt.is_phrase = true;
            for (auto &tok : phrase_tokens)
                qt.phrase_words.push_back(tok.word); /* copy, don't move */

            /* Also add the remaining phrase words as required terms so the
               candidate set is narrowed before phrase verification */
            out.push_back(std::move(qt));
            for (size_t i = 1; i < phrase_tokens.size(); i++)
            {
                fts_query_term_t wt;
                wt.term = phrase_tokens[i].word;
                wt.yesno = FTS_TERM_REQUIRED;
                wt.trunc = false;
                wt.is_phrase = false;
                out.push_back(std::move(wt));
            }
            continue;
        }

        while (p < end && !my_isalnum(cs, (uchar)*p) && !my_ismbchar(cs, p, end) &&
               *p != FTS_BOOL_OP_TRUNC)
            p++;
        if (p >= end) break;

        const char *word_start = p;
        while (p < end)
        {
            uint mblen = my_ismbchar(cs, p, end);
            if (mblen)
            {
                p += mblen;
                continue;
            }
            if (my_isalnum(cs, (uchar)*p) || *p == FTS_BOOL_OP_TRUNC)
            {
                p++;
                continue;
            }
            break;
        }
        size_t wlen = (size_t)(p - word_start);
        if (wlen == 0) continue;

        bool trunc = false;
        if (wlen > 0 && word_start[wlen - 1] == FTS_BOOL_OP_TRUNC)
        {
            trunc = true;
            wlen--;
        }
        if (wlen == 0) continue;

        fts_query_term_t qt;
        qt.term.assign(word_start, wlen);
        size_t lowered =
            cs->cset->casedn(cs, &qt.term[0], qt.term.size(), &qt.term[0], qt.term.size());
        qt.term.resize(lowered);
        qt.yesno = yesno;
        qt.trunc = trunc;
        qt.is_phrase = false;
        out.push_back(std::move(qt));
    }
}

/* Verify that a phrase appears as a consecutive subsequence within an
   already-tokenized document.  Callers tokenize a candidate once and check
   many phrases against the same token vector. */
static bool fts_phrase_in_tokens(const std::vector<fts_token_t> &doc_tokens,
                                 const std::vector<std::string> &phrase_words)
{
    if (phrase_words.empty()) return true;
    if (doc_tokens.size() < phrase_words.size()) return false;

    size_t limit = doc_tokens.size() - phrase_words.size();
    for (size_t i = 0; i <= limit; i++)
    {
        bool match = true;
        for (size_t j = 0; j < phrase_words.size(); j++)
        {
            if (doc_tokens[i + j].word != phrase_words[j])
            {
                match = false;
                break;
            }
        }
        if (match) return true;
    }
    return false;
}

/* ******************** Spatial Index helpers ******************** */

/* MariaDB renamed HA_SPATIAL -> HA_SPATIAL_legacy after the 11.x series; the
   key-flag bit (1024) is unchanged. Spatial keys reliably carry this flag on
   every version, whereas KEY::algorithm is only set to HA_KEY_ALG_RTREE on
   newer servers (it is HA_KEY_ALG_UNDEF on 11.4), so detect via the flag. */
#ifndef HA_SPATIAL
#define HA_SPATIAL HA_SPATIAL_legacy
#endif

static inline bool is_spatial_index(const KEY *ki)
{
    return (ki->flags & HA_SPATIAL) || ki->algorithm == HA_KEY_ALG_RTREE;
}

/* MBR (Minimum Bounding Rectangle) for spatial predicates */
struct tdb_mbr_t
{
    double xmin, ymin, xmax, ymax;
};

/* Hilbert curve constants */
static constexpr uint HILBERT_ORDER = 32;                           /* bits per axis */
static constexpr uint HILBERT_DIM = 2;                              /* 2D curve (x, y) */
static constexpr uint64_t HILBERT_N = (uint64_t)1 << HILBERT_ORDER; /* 2^32 */
static constexpr uint SPATIAL_HILBERT_KEY_LEN = 8;                  /* 64-bit Hilbert value */
static constexpr uint SPATIAL_MBR_VALUE_LEN = 32;                   /* 4 doubles */

/* Convert IEEE 754 double to a uint32 that preserves sort order under
   unsigned integer comparison.  Handles negative values correctly by
   flipping all bits (negative doubles have sign bit set in IEEE 754;
   flipping makes them sort before positive values). */
static inline uint32_t double_to_lex_uint32(double val)
{
    uint64_t bits;
    memcpy(&bits, &val, sizeof(bits));
    if (bits & IEEE754_DOUBLE_SIGN_MASK)
        bits = ~bits; /* negative, flip all bits */
    else
        bits ^= IEEE754_DOUBLE_SIGN_MASK;           /* positive, flip sign bit only */
    return (uint32_t)(bits >> LEX_UINT32_HI_SHIFT); /* top 32 bits for precision */
}

/* Hilbert curve, rotate quadrant coordinates.  Implements the inner
   rotation step of the iterative xy2d transform from Skilling 2004
   ("Programming the Hilbert curve") -- see also the canonical
   Wikipedia pseudocode at https://en.wikipedia.org/wiki/Hilbert_curve.
   The literal (n - 1) is the standard reflection around the centre
   of an n-cell axis; rx and ry carry the binary quadrant flags from
   the caller. */
static inline void hilbert_rot(uint32_t n, uint32_t *x, uint32_t *y, uint32_t rx, uint32_t ry)
{
    if (ry == 0)
    {
        if (rx == 1)
        {
            *x = n - 1 - *x;
            *y = n - 1 - *y;
        }
        uint32_t t = *x;
        *x = *y;
        *y = t;
    }
}

/* Convert 2D coordinates (x, y) to a 64-bit Hilbert curve value.  Order
   32, each axis 32-bit precision, output 64-bit.  Iterative algorithm
   per Skilling 2004 / Wikipedia, O(32) loop, no recursion.  The literal
   `3` and the XOR encode the four-quadrant visit order of the Hilbert
   d-value (rx, ry) = (0,0)->0, (0,1)->1, (1,1)->2, (1,0)->3.  The
   `s << 1` doubles s so hilbert_rot receives the full sub-grid size
   for this level, not the half-size step. */
static uint64_t hilbert_xy2d_64(uint32_t x, uint32_t y)
{
    uint64_t d = 0;
    for (uint64_t s = HILBERT_N >> 1; s > 0; s >>= 1)
    {
        uint32_t rx = (x & s) > 0 ? 1 : 0;
        uint32_t ry = (y & s) > 0 ? 1 : 0;
        d += s * s * (uint64_t)((3 * rx) ^ ry);
        hilbert_rot((uint32_t)s << 1, &x, &y, rx, ry);
    }
    return d;
}

/* Store uint64 as 8-byte big-endian (for lexicographic ordering in LSM).
   Most significant byte first so that memcmp on the encoded bytes matches
   the natural numeric ordering of the Hilbert value. */
static inline void encode_hilbert_be(uint64_t h, uchar *out)
{
    for (uint i = 0; i < SPATIAL_HILBERT_KEY_LEN; i++)
        out[i] = (uchar)(h >> ((SPATIAL_HILBERT_KEY_LEN - 1 - i) * BITS_PER_BYTE));
}

/* Decode 8-byte big-endian uint64 */
static inline uint64_t decode_hilbert_be(const uchar *in)
{
    uint64_t h = 0;
    for (uint i = 0; i < SPATIAL_HILBERT_KEY_LEN; i++) h = (h << BITS_PER_BYTE) | (uint64_t)in[i];
    return h;
}

/* WKB geometry type constants */
static constexpr uint32_t WKB_POINT = 1;
static constexpr uint32_t WKB_LINESTRING = 2;
static constexpr uint32_t WKB_POLYGON = 3;
static constexpr uint32_t WKB_MULTIPOINT = 4;
static constexpr uint32_t WKB_MULTILINESTRING = 5;
static constexpr uint32_t WKB_MULTIPOLYGON = 6;
static constexpr uint32_t WKB_GEOMETRYCOLLECTION = 7;

/* Limits to reject malformed WKB data */
static constexpr uint32_t WKB_MAX_POINTS = 1000000;
static constexpr uint32_t WKB_MAX_RINGS = 10000;
static constexpr uint32_t WKB_MAX_GEOMS = 100000;
static constexpr uint SPATIAL_SRID_SIZE = 4;
static constexpr uint SPATIAL_WKB_HEADER_SIZE = 5;  /* 1 byte_order + 4 type */
static constexpr uint SPATIAL_POINT_DATA_SIZE = 16; /* 2 doubles (x, y) */

/* WKB encodes its count fields (point/ring/geometry counts) as uint32_t. */
static constexpr uint WKB_COUNT_SIZE = sizeof(uint32_t);

/* Parts-of-MBR encoding.  spatial_build_value writes [xmin,ymin,xmax,ymax]
   as native doubles, and spatial_parse_query_mbr reads MariaDB's
   [xmin,xmax,ymin,ymax] layout.  Sentinels for offset arithmetic. */
static constexpr uint MBR_DOUBLE_SIZE = sizeof(double);
static constexpr uint MBR_OFFSET_SECOND = 1 * MBR_DOUBLE_SIZE;
static constexpr uint MBR_OFFSET_THIRD = 2 * MBR_DOUBLE_SIZE;
static constexpr uint MBR_OFFSET_FOURTH = 3 * MBR_DOUBLE_SIZE;

/* Read a coordinate pair from WKB and expand MBR.
   Advances pp by SPATIAL_POINT_DATA_SIZE bytes.
   Skips NaN/Inf coordinates. */
static inline bool wkb_read_point(const uchar *&pp, const uchar *ee, double &mn_x, double &mn_y,
                                  double &mx_x, double &mx_y)
{
    if (pp + SPATIAL_POINT_DATA_SIZE > ee) return false;
    double x, y;
    float8get(x, pp);
    float8get(y, pp + MBR_DOUBLE_SIZE);
    pp += SPATIAL_POINT_DATA_SIZE;
    if (std::isfinite(x) && std::isfinite(y))
    {
        if (x < mn_x) mn_x = x;
        if (x > mx_x) mx_x = x;
        if (y < mn_y) mn_y = y;
        if (y > mx_y) mx_y = y;
    }
    return true;
}

/* Read a point sequence ([num_points 4B][x,y pairs...]) and expand MBR.
   Used by LINESTRING and each POLYGON ring. */
static inline bool wkb_read_point_sequence(const uchar *&pp, const uchar *ee, double &mn_x,
                                           double &mn_y, double &mx_x, double &mx_y)
{
    if (pp + WKB_COUNT_SIZE > ee) return false;
    uint32_t n_pts;
    memcpy(&n_pts, pp, WKB_COUNT_SIZE);
    pp += WKB_COUNT_SIZE;
    if (n_pts > WKB_MAX_POINTS) return false;
    for (uint32_t i = 0; i < n_pts; i++)
    {
        if (!wkb_read_point(pp, ee, mn_x, mn_y, mx_x, mx_y)) return false;
    }
    return true;
}

/* Maximum nesting depth for a GEOMETRYCOLLECTION (or any of the MULTI
   types).  Stops a pathologically nested geometry from blowing the stack
   through wkb_parse_geometry's recursion; far above any real geometry
   the server would actually accept. */
static constexpr int WKB_MAX_RECURSION_DEPTH = 32;

/* Recursive WKB geometry parser.  Reads one geometry object from pp,
   expanding the MBR to include all coordinate pairs.  Advances pp past
   the consumed bytes.  Supports all 7 OGC geometry types.  The depth
   argument bounds recursive descent into GEOMETRYCOLLECTION children. */
static bool wkb_parse_geometry(const uchar *&pp, const uchar *ee, double &mn_x, double &mn_y,
                               double &mx_x, double &mx_y, int depth)
{
    if (depth > WKB_MAX_RECURSION_DEPTH) return false;
    if (pp + SPATIAL_WKB_HEADER_SIZE > ee) return false;
        /* MariaDB stores WKB in native byte order, so the leading byte is the
           native endianness marker (0 = big, 1 = little).  We rely on native
           order for the memcpy reads of the geometry type and coordinates
           below; if MariaDB ever changed to store non-native WKB, this assert
           would fire instead of silently returning garbage MBRs.  Release
           builds simply trust the convention. */
#ifndef DBUG_OFF
    {
        const uint32_t endian_probe = 1;
        uchar native_byte_order = *(const uchar *)&endian_probe; /* 1 on LE, 0 on BE */
        DBUG_ASSERT(*pp == native_byte_order);
    }
#endif
    pp++; /* we skip byte_order (MariaDB stores in native order) */
    uint32_t gt;
    memcpy(&gt, pp, WKB_COUNT_SIZE);
    pp += WKB_COUNT_SIZE;

    switch (gt)
    {
        case WKB_POINT:
            return wkb_read_point(pp, ee, mn_x, mn_y, mx_x, mx_y);

        case WKB_LINESTRING:
            return wkb_read_point_sequence(pp, ee, mn_x, mn_y, mx_x, mx_y);

        case WKB_POLYGON:
        {
            if (pp + WKB_COUNT_SIZE > ee) return false;
            uint32_t n_rings;
            memcpy(&n_rings, pp, WKB_COUNT_SIZE);
            pp += WKB_COUNT_SIZE;
            if (n_rings > WKB_MAX_RINGS) return false;
            for (uint32_t r = 0; r < n_rings; r++)
            {
                if (!wkb_read_point_sequence(pp, ee, mn_x, mn_y, mx_x, mx_y)) return false;
            }
            return true;
        }

        case WKB_MULTIPOINT:
        case WKB_MULTILINESTRING:
        case WKB_MULTIPOLYGON:
        case WKB_GEOMETRYCOLLECTION:
        {
            if (pp + WKB_COUNT_SIZE > ee) return false;
            uint32_t n_geoms;
            memcpy(&n_geoms, pp, WKB_COUNT_SIZE);
            pp += WKB_COUNT_SIZE;
            if (n_geoms > WKB_MAX_GEOMS) return false;
            for (uint32_t i = 0; i < n_geoms; i++)
            {
                if (!wkb_parse_geometry(pp, ee, mn_x, mn_y, mx_x, mx_y, depth + 1)) return false;
            }
            return true;
        }

        default:
            return false;
    }
}

/* Extract MBR from a GEOMETRY field's raw data (SRID prefix + WKB).
   Supports all OGC geometry types.  Rejects malformed data and
   coordinates with NaN/Inf values.
   Returns true on success, false on malformed data. */
static bool spatial_compute_mbr(const uchar *data, size_t len, double *xmin, double *ymin,
                                double *xmax, double *ymax)
{
    if (len < SPATIAL_SRID_SIZE + SPATIAL_WKB_HEADER_SIZE) return false;

    const uchar *p = data + SPATIAL_SRID_SIZE;
    const uchar *end = data + len;

    *xmin = *ymin = DBL_MAX;
    *xmax = *ymax = -DBL_MAX;

    if (!wkb_parse_geometry(p, end, *xmin, *ymin, *xmax, *ymax, 0)) return false;

    return *xmin <= *xmax && *ymin <= *ymax;
}

/* Build spatial index key ( [hilbert_value 8B BE][pk_bytes] )
   Returns total key length. */
static uint spatial_build_key(double cx, double cy, const uchar *pk, uint pk_len, uchar *out)
{
    uint32_t qx = double_to_lex_uint32(cx);
    uint32_t qy = double_to_lex_uint32(cy);
    uint64_t h = hilbert_xy2d_64(qx, qy);
    encode_hilbert_be(h, out);
    memcpy(out + SPATIAL_HILBERT_KEY_LEN, pk, pk_len);
    return SPATIAL_HILBERT_KEY_LEN + pk_len;
}

/* Build spatial index value( [xmin 8B][ymin 8B][xmax 8B][ymax 8B] ) = 32 bytes.
   Stored as native doubles (little-endian on x86). */
static void spatial_build_value(double xmin, double ymin, double xmax, double ymax, uchar *out)
{
    memcpy(out, &xmin, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_SECOND, &ymin, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_THIRD, &xmax, MBR_DOUBLE_SIZE);
    memcpy(out + MBR_OFFSET_FOURTH, &ymax, MBR_DOUBLE_SIZE);
}

/* Parse MBR from MariaDB's spatial key buffer.
   MariaDB format( [xmin 8B][xmax 8B][ymin 8B][ymax 8B] ).  A malformed
   key whose stored min exceeds its max would underflow the grid-cell
   subtraction in spatial_decompose_ranges and ask reserve for a billion
   slots, so the corners are normalised here at the parse boundary. */
static void spatial_parse_query_mbr(const uchar *key, tdb_mbr_t *mbr)
{
    float8get(mbr->xmin, key);
    float8get(mbr->xmax, key + MBR_OFFSET_SECOND);
    float8get(mbr->ymin, key + MBR_OFFSET_THIRD);
    float8get(mbr->ymax, key + MBR_OFFSET_FOURTH);
    if (mbr->xmin > mbr->xmax) std::swap(mbr->xmin, mbr->xmax);
    if (mbr->ymin > mbr->ymax) std::swap(mbr->ymin, mbr->ymax);
}

/* MBR spatial predicates -- match MariaDB MBR class semantics exactly */
static inline bool mbr_intersects(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return !(a->xmax < b->xmin || a->xmin > b->xmax || a->ymax < b->ymin || a->ymin > b->ymax);
}

static inline bool mbr_within(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return a->xmin >= b->xmin && a->xmax <= b->xmax && a->ymin >= b->ymin && a->ymax <= b->ymax;
}

static inline bool mbr_equals(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return a->xmin == b->xmin && a->xmax == b->xmax && a->ymin == b->ymin && a->ymax == b->ymax;
}

static inline bool mbr_disjoint(const tdb_mbr_t *a, const tdb_mbr_t *b)
{
    return !mbr_intersects(a, b);
}

/* Dispatch MBR predicate based on ha_rkey_function spatial mode.
   Returns true if the entry MBR matches the query predicate. */
static bool spatial_mbr_predicate(enum ha_rkey_function mode, const tdb_mbr_t *query,
                                  const tdb_mbr_t *entry)
{
    /* MariaDB's CONTAIN and WITHIN both reduce to "row MBR is within the
       query MBR" once the SQL-layer argument order is normalised, so they
       map to the same mbr_within(entry, query) call below.  Intersect is
       symmetric. */
    switch (mode)
    {
        case HA_READ_MBR_INTERSECT:
            return mbr_intersects(entry, query);
        case HA_READ_MBR_CONTAIN:
            return mbr_within(entry, query);
        case HA_READ_MBR_WITHIN:
            return mbr_within(entry, query);
        case HA_READ_MBR_EQUAL:
            return mbr_equals(entry, query);
        case HA_READ_MBR_DISJOINT:
            return mbr_disjoint(entry, query);
        default:
            return false;
    }
}

/* Hilbert range decomposition resolution.  At SPATIAL_DECOMP_BITS bits per
   axis, the coordinate space is divided into a 2^N x 2^N grid.  Higher
   values produce tighter ranges (fewer false positives) but more ranges
   to scan (more seeks).  8 bits = 256x256 grid, at most 65536 cells but
   typically 10-50 merged ranges for a small query box. */
static constexpr uint SPATIAL_DECOMP_BITS = 8;
static constexpr uint SPATIAL_DECOMP_N = 1u << SPATIAL_DECOMP_BITS;
static_assert(SPATIAL_DECOMP_BITS < HILBERT_ORDER,
              "SPATIAL_DECOMP_BITS must be < HILBERT_ORDER or shift underflows");

/* Cell-count cap.  Above this we fall back to a single full-range scan;
   the MBR post-filter rejects non-overlapping rows on the read side, so
   the only cost is reading more keys, not returning wrong rows.  Without
   this cap a query MBR covering the whole universe allocates ~512 KB of
   uint64_t for the cells vector and then sorts it -- the full-scan path
   does that work for free. */
static constexpr uint SPATIAL_DECOMP_FULL_SCAN_THRESHOLD = SPATIAL_DECOMP_N * 16;

/* Compute the Hilbert ranges that cover a quantized bounding box.
   Enumerates grid cells at SPATIAL_DECOMP_BITS resolution, computes
   the Hilbert value for each, sorts, and merges contiguous values
   into non-overlapping ranges.  Each range maps back to the full
   32-bit Hilbert space by shifting. */
static void spatial_decompose_ranges(uint32_t qx_min, uint32_t qy_min, uint32_t qx_max,
                                     uint32_t qy_max,
                                     std::vector<std::pair<uint64_t, uint64_t>> &out)
{
    out.clear();

    uint shift = HILBERT_ORDER - SPATIAL_DECOMP_BITS;
    uint gx0 = qx_min >> shift;
    uint gy0 = qy_min >> shift;
    uint gx1 = qx_max >> shift;
    uint gy1 = qy_max >> shift;

    if (gx1 >= SPATIAL_DECOMP_N) gx1 = SPATIAL_DECOMP_N - 1;
    if (gy1 >= SPATIAL_DECOMP_N) gy1 = SPATIAL_DECOMP_N - 1;

    /* Wide query box.  Falling back to a single full-range scan beats
       enumerating + sorting 65k cells when the post-filter is going to
       reject most of them anyway. */
    const uint64_t cell_count = (uint64_t)(gx1 - gx0 + 1) * (uint64_t)(gy1 - gy0 + 1);
    if (cell_count > SPATIAL_DECOMP_FULL_SCAN_THRESHOLD)
    {
        out.push_back({HILBERT_RANGE_FULL_LO, HILBERT_RANGE_FULL_HI});
        return;
    }

    std::vector<uint64_t> cells;
    cells.reserve((size_t)cell_count);
    for (uint gx = gx0; gx <= gx1; gx++)
    {
        for (uint gy = gy0; gy <= gy1; gy++)
        {
            /* We compute coarse hilbert value and scale to full 64-bit space.
               The coarse cell (gx, gy) at SPATIAL_DECOMP_BITS resolution
               maps to hilbert values in [h_coarse << (2*shift), (h_coarse+1) << (2*shift) - 1] */
            uint64_t h = hilbert_xy2d_64(gx << shift, gy << shift);
            cells.push_back(h);
        }
    }

    if (cells.empty())
    {
        /* Degenerate query box -- fall back to a full scan. */
        out.push_back({HILBERT_RANGE_FULL_LO, HILBERT_RANGE_FULL_HI});
        return;
    }

    std::sort(cells.begin(), cells.end());

    /* Each coarse cell covers a range of 2^(HILBERT_DIM*shift) fine hilbert
       values, er shift bits per axis times HILBERT_DIM axes. */
    uint64_t cell_span = (uint64_t)1 << (HILBERT_DIM * shift);

    uint64_t range_lo = cells[0];
    uint64_t range_hi = cells[0] + cell_span - 1;

    for (size_t i = 1; i < cells.size(); i++)
    {
        uint64_t lo = cells[i];
        uint64_t hi = cells[i] + cell_span - 1;

        if (lo <= range_hi + 1)
        {
            if (hi > range_hi) range_hi = hi;
        }
        else
        {
            out.push_back({range_lo, range_hi});
            range_lo = lo;
            range_hi = hi;
        }
    }
    out.push_back({range_lo, range_hi});
}

/* ******************** System variables (global DB config) ******************** */

static ulong srv_flush_threads = 4;
static ulong srv_max_concurrent_flushes = 0; /* 0 = align with srv_flush_threads */
static ulong srv_compaction_threads = 4;
static ulong srv_log_level = 0;                                      /* TDB_LOG_DEBUG */
static ulonglong srv_block_cache_size = TIDESDB_DEFAULT_BLOCK_CACHE; /* 256M */
static ulong srv_max_open_sstables = 256;
static ulonglong srv_max_memory_usage = 0; /* 0 = auto (library decides) */
static my_bool srv_log_to_file = 1;        /* write TidesDB logs to file (default is yes) */
static ulonglong srv_log_truncation_at = 24ULL * 1024 * 1024; /* log file truncation size (24MB) */
static my_bool srv_unified_memtable = 1; /* 1 = unified WAL+memtable (default), 0 = per-CF */
static ulonglong srv_unified_memtable_write_buffer_size = 256ULL * 1024 * 1024; /* 256MB */

/* Per-session TTL override (seconds).  0 = use table default. */
static MYSQL_THDVAR_ULONGLONG(ttl, PLUGIN_VAR_RQCMDARG,
                              "Per-session TTL in seconds applied to INSERT/UPDATE; "
                              "0 means use the table-level TTL option; "
                              "can be set with SET [SESSION] tidesdb_ttl=N or "
                              "SET STATEMENT tidesdb_ttl=N FOR INSERT",
                              NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

/* Per-session skip unique check (for bulk loads where PK duplicates
   are known impossible).  Same pattern as MyRocks rocksdb_skip_unique_check. */
static MYSQL_THDVAR_BOOL(skip_unique_check, PLUGIN_VAR_RQCMDARG,
                         "Skip uniqueness check on primary key and unique secondary indexes "
                         "during INSERT.  Only safe when the application guarantees no "
                         "duplicates (e.g. bulk loads with monotonic PKs).  "
                         "SET SESSION tidesdb_skip_unique_check=1",
                         NULL, NULL, 0);

/* Per-session row-count threshold for the post-delete range compaction
   trigger.  Zero disables the feature.  When non-zero, the engine tracks
   the comparable min/max PK bytes touched by a single multi-row DELETE
   statement (the start_bulk_delete / end_bulk_delete envelope around
   range deletes) and, if the deleted row count is at least the threshold,
   calls tidesdb_compact_range on the primary CF over the touched range
   at end-of-statement to physically reclaim the freshly-tombstoned range
   without waiting for a structural compaction trigger.  Threshold avoids
   making small DELETEs pay synchronous compaction cost. */
static MYSQL_THDVAR_ULONGLONG(
    compact_after_range_delete_min_rows, PLUGIN_VAR_RQCMDARG,
    "If non-zero, after a multi-row DELETE statement that touches at least "
    "this many rows, call tidesdb_compact_range over the touched primary-key "
    "range to physically reclaim tombstoned space.  Default 0 disables the "
    "feature; set to 0 to keep the post-DELETE behavior unchanged",
    NULL, NULL, 0, 0, ULONGLONG_MAX, 1);

/* Per-session opt-in for single-delete semantics on the primary row CF.
   Secondary-index deletes always use tidesdb_txn_single_delete because
   each (col_values, pk) / (term, pk) / (hilbert, pk) composite is written
   exactly once per row lifetime and deleted exactly once -- the
   single-delete contract holds unconditionally for those.
   For the primary CF the contract is narrower, UPDATE ... SET non_pk_col
   writes tidesdb_txn_put(share->cf, data_key(pk), ...) with the same PK,
   producing a put-over-put, and REPLACE INTO / INSERT ... ON DUPLICATE
   KEY UPDATE on tables with no secondary indexes does the same via a
   silent overwrite.  Under either pattern, dropping a put+single-delete
   pair at compaction can re-expose an older put.  Enabling this variable
   is the caller's promise that the session does none of the above --
   typical insert-then-delete, log-style, append-only workloads. */
static MYSQL_THDVAR_BOOL(single_delete_primary, PLUGIN_VAR_RQCMDARG,
                         "Use single-delete semantics for the primary row CF on DELETE. "
                         "Caller promises no UPDATE on non-PK columns, no REPLACE INTO, "
                         "and no INSERT ... ON DUPLICATE KEY UPDATE on tables without "
                         "secondary indexes for this session.  Violating the contract may "
                         "re-expose older row versions after compaction.  Safe choice: "
                         "leave OFF unless the session is INSERT-and-DELETE only.  "
                         "SET SESSION tidesdb_single_delete_primary=1",
                         NULL, NULL, 0);

static MYSQL_THDVAR_ULONG(backpressure_wait_timeout_ms, PLUGIN_VAR_RQCMDARG,
                          "Milliseconds the plugin will block a writer on TidesDB "
                          "back-pressure (memtable/flush queue/L0 backlog at soft cap) "
                          "before surfacing it to the SQL layer as a lock-wait-timeout. "
                          "0 disables blocking and returns the timeout immediately",
                          NULL, NULL, TDB_BACKPRESSURE_DEFAULT_TIMEOUT_MS,
                          TDB_BACKPRESSURE_MIN_TIMEOUT_MS, TDB_BACKPRESSURE_MAX_TIMEOUT_MS, 0);

static MYSQL_THDVAR_ULONG(lock_wait_timeout_ms, PLUGIN_VAR_RQCMDARG,
                          "Milliseconds a pessimistic row-lock acquire will wait "
                          "before returning HA_ERR_LOCK_WAIT_TIMEOUT.  Mirrors "
                          "innodb_lock_wait_timeout (default 50000 = 50 s).  "
                          "0 disables the timeout (wait bounded only by KILL QUERY)",
                          NULL, NULL, TDB_LOCK_WAIT_DEFAULT_TIMEOUT_MS,
                          TDB_LOCK_WAIT_MIN_TIMEOUT_MS, TDB_LOCK_WAIT_MAX_TIMEOUT_MS, 0);

/* Definitions for the forward decls near tidesdb_txn_delete_cf -- placed here
   so they have the THDVAR macros in scope.  Each returns the configured wait
   budget for the session, or the compile-time default when called without a
   THD (e.g. background paths). */
static ulong tdb_backpressure_timeout_ms(THD *thd)
{
    if (!thd) return TDB_BACKPRESSURE_DEFAULT_TIMEOUT_MS;
    return THDVAR(thd, backpressure_wait_timeout_ms);
}

static ulong tdb_lock_wait_timeout_ms(THD *thd)
{
    if (!thd) return TDB_LOCK_WAIT_DEFAULT_TIMEOUT_MS;
    return THDVAR(thd, lock_wait_timeout_ms);
}

/* Session-level defaults for table options.
   These are used by HA_TOPTION_SYSVAR so that CREATE TABLE without
   explicit options inherits the session/global default.  Dynamic and
   session-scoped, matching InnoDB's innodb_default_* pattern. */

static const char *compression_names[] = {"NONE", "SNAPPY", "LZ4", "ZSTD", "LZ4_FAST", NullS};
static TYPELIB compression_typelib = {array_elements(compression_names) - 1, "compression_typelib",
                                      compression_names, NULL, NULL};

static MYSQL_THDVAR_ENUM(default_compression, PLUGIN_VAR_RQCMDARG,
                         "Default compression algorithm for new tables "
                         "(NONE, SNAPPY, LZ4, ZSTD, LZ4_FAST)",
                         NULL, NULL, 2 /* LZ4 */, &compression_typelib);

static MYSQL_THDVAR_ULONGLONG(default_write_buffer_size, PLUGIN_VAR_RQCMDARG,
                              "Default write buffer size in bytes for new tables", NULL, NULL,
                              TIDESQL_DEFAULT_WRITE_BUFFER_SIZE, 1024, ULONGLONG_MAX, 1024);

static MYSQL_THDVAR_BOOL(default_bloom_filter, PLUGIN_VAR_RQCMDARG,
                         "Default bloom filter setting for new tables", NULL, NULL, 1);

static MYSQL_THDVAR_BOOL(default_use_btree, PLUGIN_VAR_RQCMDARG,
                         "Default USE_BTREE setting for new tables (0=LSM, 1=B-tree)", NULL, NULL,
                         0);

static MYSQL_THDVAR_BOOL(default_block_indexes, PLUGIN_VAR_RQCMDARG,
                         "Default block indexes setting for new tables", NULL, NULL, 1);

static const char *sync_mode_names[] = {"NONE", "INTERVAL", "FULL", NullS};
static TYPELIB sync_mode_typelib = {array_elements(sync_mode_names) - 1, "sync_mode_typelib",
                                    sync_mode_names, NULL, NULL};

static MYSQL_THDVAR_ENUM(default_sync_mode, PLUGIN_VAR_RQCMDARG,
                         "Default sync mode for new tables.  Governs SSTable file sync "
                         "(klog and vlog).  Under tidesdb_unified_memtable=ON the shared "
                         "WAL is fsynced according to tidesdb_unified_memtable_sync_mode "
                         "instead, so this option does not control WAL durability for "
                         "new tables.  Choose NONE, INTERVAL or FULL",
                         NULL, NULL, 2 /* FULL */, &sync_mode_typelib);

static MYSQL_THDVAR_ULONGLONG(default_sync_interval_us, PLUGIN_VAR_RQCMDARG,
                              "Default sync interval in microseconds for new tables "
                              "(used when SYNC_MODE=INTERVAL)",
                              NULL, NULL, TIDESQL_DEFAULT_SYNC_INTERVAL_US, 0, ULONGLONG_MAX, 1);

static MYSQL_THDVAR_ULONGLONG(default_bloom_fpr, PLUGIN_VAR_RQCMDARG,
                              "Default bloom filter false positive rate for new tables "
                              "(parts per 10000; 100 = 1%%)",
                              NULL, NULL, 100, 1, 10000, 1);

static MYSQL_THDVAR_ULONGLONG(default_klog_value_threshold, PLUGIN_VAR_RQCMDARG,
                              "Default klog value threshold in bytes for new tables "
                              "(values >= this go to vlog)",
                              NULL, NULL, TIDESQL_DEFAULT_KLOG_VALUE_THRESHOLD, 0, ULONGLONG_MAX,
                              1);

static MYSQL_THDVAR_ULONGLONG(default_l0_queue_stall_threshold, PLUGIN_VAR_RQCMDARG,
                              "Default L0 queue stall threshold for new tables", NULL, NULL, 10, 1,
                              1024, 1);

static MYSQL_THDVAR_ULONGLONG(default_l1_file_count_trigger, PLUGIN_VAR_RQCMDARG,
                              "Default L1 file count compaction trigger for new tables", NULL, NULL,
                              4, 1, 1024, 1);

static MYSQL_THDVAR_ULONGLONG(default_level_size_ratio, PLUGIN_VAR_RQCMDARG,
                              "Default level size ratio for new tables", NULL, NULL,
                              TIDESQL_DEFAULT_LEVEL_SIZE_RATIO, 2, 100, 1);

static MYSQL_THDVAR_ULONGLONG(default_min_levels, PLUGIN_VAR_RQCMDARG,
                              "Default minimum LSM-tree levels for new tables.  Matches "
                              "TIDESQL_DEFAULT_MIN_LEVELS in the TidesDB library",
                              NULL, NULL, TIDESQL_DEFAULT_MIN_LEVELS, 1, 64, 1);

static MYSQL_THDVAR_ULONGLONG(default_dividing_level_offset, PLUGIN_VAR_RQCMDARG,
                              "Default dividing level offset for new tables.  Matches "
                              "TIDESQL_DEFAULT_DIVIDING_LEVEL_OFFSET in the TidesDB library",
                              NULL, NULL, TIDESQL_DEFAULT_DIVIDING_LEVEL_OFFSET, 0, 64, 1);

static MYSQL_THDVAR_ULONGLONG(default_skip_list_max_level, PLUGIN_VAR_RQCMDARG,
                              "Default skip list max level for new tables", NULL, NULL, 12, 1, 64,
                              1);

static MYSQL_THDVAR_ULONGLONG(
    default_skip_list_probability, PLUGIN_VAR_RQCMDARG,
    "Default skip list probability for new tables (percentage; 25 = 0.25)", NULL, NULL, 25, 1, 100,
    1);

static MYSQL_THDVAR_ULONGLONG(default_index_sample_ratio, PLUGIN_VAR_RQCMDARG,
                              "Default block index sample ratio for new tables", NULL, NULL,
                              TIDESQL_DEFAULT_INDEX_SAMPLE_RATIO, 1, 1024, 1);

static MYSQL_THDVAR_ULONGLONG(default_block_index_prefix_len, PLUGIN_VAR_RQCMDARG,
                              "Default block index prefix length for new tables", NULL, NULL,
                              TIDESQL_DEFAULT_BLOCK_INDEX_PREFIX_LEN, 1, 256, 1);

static MYSQL_THDVAR_ULONGLONG(default_min_disk_space, PLUGIN_VAR_RQCMDARG,
                              "Default minimum disk space in bytes for new tables", NULL, NULL,
                              TIDESQL_DEFAULT_MIN_DISK_SPACE, 0, ULONGLONG_MAX, 1024);

static MYSQL_THDVAR_BOOL(default_object_lazy_compaction, PLUGIN_VAR_RQCMDARG,
                         "Default object store lazy compaction for new tables. "
                         "When enabled, doubles the L1 file count compaction trigger "
                         "to reduce remote I/O at the cost of higher read amplification",
                         NULL, NULL, 0);

static MYSQL_THDVAR_BOOL(default_object_prefetch_compaction, PLUGIN_VAR_RQCMDARG,
                         "Default object store prefetch compaction for new tables. "
                         "When enabled, downloads all input SSTables in parallel "
                         "before compaction merge begins",
                         NULL, NULL, 1);

/* Tombstone-density compaction trigger (parts per 10000 -- 5000 = 0.50 ratio).
   When non-zero, after each flush the engine inspects level-1 SSTables and
   escalates compaction for any single SST whose tombstone count divided by
   entry count exceeds this ratio while having at least
   tombstone_density_min_entries entries.  Default 0 keeps the existing
   structural-trigger behavior. */
static MYSQL_THDVAR_ULONGLONG(default_tombstone_density_trigger, PLUGIN_VAR_RQCMDARG,
                              "Default tombstone-density compaction trigger ratio for new tables, "
                              "expressed as parts per 10000 (5000 = 0.50, 0 disables).  When set, "
                              "compaction is escalated for any level-1 SSTable whose tombstone "
                              "count divided by entry count exceeds the ratio",
                              NULL, NULL, 0, 0, 10000, 1);

static MYSQL_THDVAR_ULONGLONG(default_tombstone_density_min_entries, PLUGIN_VAR_RQCMDARG,
                              "Minimum entry count for an SSTable to be considered by the "
                              "tombstone-density trigger; smaller SSTables are ignored",
                              NULL, NULL, 1024, 0, ULONGLONG_MAX, 1);

static const char *isolation_level_names[] = {
    "READ_UNCOMMITTED", "READ_COMMITTED", "REPEATABLE_READ", "SNAPSHOT", "SERIALIZABLE", NullS};
static TYPELIB isolation_level_typelib = {array_elements(isolation_level_names) - 1,
                                          "isolation_level_typelib", isolation_level_names, NULL,
                                          NULL};

static MYSQL_THDVAR_ENUM(default_isolation_level, PLUGIN_VAR_RQCMDARG,
                         "Default isolation level for new tables "
                         "(READ_UNCOMMITTED, READ_COMMITTED, REPEATABLE_READ, SNAPSHOT, "
                         "SERIALIZABLE)",
                         NULL, NULL, 2 /* REPEATABLE_READ */, &isolation_level_typelib);

static const char *log_level_names[] = {"DEBUG", "INFO", "WARN", "ERROR", "FATAL", "NONE", NullS};
static TYPELIB log_level_typelib = {array_elements(log_level_names) - 1, "log_level_typelib",
                                    log_level_names, NULL, NULL};

static MYSQL_SYSVAR_ULONG(flush_threads, srv_flush_threads,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Number of TidesDB flush threads", NULL, NULL, 4, 1, 64, 0);

static MYSQL_SYSVAR_ULONG(max_concurrent_flushes, srv_max_concurrent_flushes,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Global cap on in-flight memtable flushes.  0 (default) "
                          "aligns the cap with tidesdb_flush_threads so every "
                          "configured flush worker can run.  Setting a cap below "
                          "tidesdb_flush_threads leaves workers idle and logs a "
                          "startup warning",
                          NULL, NULL, 0, 0, 1024, 0);

static MYSQL_SYSVAR_ULONG(compaction_threads, srv_compaction_threads,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Number of TidesDB compaction threads", NULL, NULL, 4, 1, 64, 0);

static MYSQL_SYSVAR_ENUM(log_level, srv_log_level, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "TidesDB log level (DEBUG, INFO, WARN, ERROR, FATAL, NONE)", NULL, NULL, 0,
                         &log_level_typelib);

/* Conflict information logging.
   Similar to innodb_print_all_deadlocks -- logs all TDB_ERR_CONFLICT
   events to the error log with transaction and table details.
   (srv_print_all_conflicts, last_conflict_mutex, last_conflict_info
    are forward-declared near tdb_rc_to_ha().) */
static MYSQL_SYSVAR_BOOL(print_all_conflicts, srv_print_all_conflicts, PLUGIN_VAR_RQCMDARG,
                         "Log all TidesDB conflict errors to the error log "
                         "(similar to innodb_print_all_deadlocks)",
                         NULL, NULL, 0);

static MYSQL_SYSVAR_BOOL(pessimistic_locking, srv_pessimistic_locking, PLUGIN_VAR_RQCMDARG,
                         "Enable plugin-level row locks for SELECT ... FOR UPDATE, "
                         "UPDATE, DELETE, and INSERT on user-defined primary keys. "
                         "ON (default): write-intent statements acquire per-row X locks "
                         "and plain reads under REPEATABLE_READ / SERIALIZABLE acquire "
                         "S locks; multiple S holders coexist, S blocks while an X is "
                         "waiting (writer fairness).  Deadlock detection via wait-for "
                         "graph traversal; bounded by tidesdb_lock_wait_timeout_ms.  "
                         "Locks held until COMMIT or ROLLBACK.  Both explicit and "
                         "autocommit transactions participate.  Locks can be acquired "
                         "on non-existing keys (e.g. SFU on a missing row blocks INSERT "
                         "of that key). "
                         "OFF: pure optimistic MVCC -- concurrent writers on the same "
                         "row are detected at COMMIT time (TDB_ERR_CONFLICT) and the "
                         "application must retry",
                         NULL, NULL, 1);

static MYSQL_SYSVAR_ULONG(fts_min_word_len, srv_fts_min_word_len, PLUGIN_VAR_RQCMDARG,
                          "Minimum word length (in characters) for full-text indexing. "
                          "Shorter words are excluded from the index and search queries",
                          NULL, NULL, 3, 1, 84, 0);

static MYSQL_SYSVAR_ULONG(fts_max_word_len, srv_fts_max_word_len, PLUGIN_VAR_RQCMDARG,
                          "Maximum word length (in characters) for full-text indexing. "
                          "Longer words are excluded from the index and search queries",
                          NULL, NULL, 84, 1, 512, 0);

static MYSQL_SYSVAR_DOUBLE(fts_bm25_k1, srv_fts_bm25_k1, PLUGIN_VAR_RQCMDARG,
                           "BM25 k1 parameter controlling term-frequency saturation. "
                           "Higher values give more weight to repeated terms. "
                           "Standard default is 1.2",
                           NULL, NULL, 1.2, 0.0, 10.0, 0);

static MYSQL_SYSVAR_DOUBLE(fts_bm25_b, srv_fts_bm25_b, PLUGIN_VAR_RQCMDARG,
                           "BM25 b parameter controlling document-length normalization. "
                           "0 = no normalization, 1 = full normalization. "
                           "Standard default is 0.75",
                           NULL, NULL, 0.75, 0.0, 1.0, 0);

static MYSQL_SYSVAR_STR(fts_blend_chars, srv_fts_blend_chars,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Characters treated as both separators and valid word characters "
                        "in full-text indexing.  When a blend character appears inside a "
                        "token, the tokenizer emits the full blended form plus each "
                        "sub-part on either side.  For example, with blend_chars=\"'\" "
                        "the input \"l'aria\" produces three tokens (l'aria, l, aria) "
                        "and the single-character \"l\" is then dropped by the default "
                        "tidesdb_fts_min_word_len=3.  Set to \"'\" for Italian/French "
                        "elision support.  Default is empty (no blend characters)",
                        NULL, tdb_fts_blend_chars_update, NULL);

static MYSQL_SYSVAR_STR(ft_stopword_table, srv_ft_stopword_table,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "User-defined stop word table in 'db_name/table_name' format. "
                        "The table must have a VARCHAR column named 'value'. "
                        "When NULL (default), uses the same 36 default stop words as "
                        "information_schema.INNODB_FT_DEFAULT_STOPWORD. "
                        "Set to empty string to disable stop word filtering entirely",
                        NULL, tdb_ft_stopword_table_update, NULL);

static MYSQL_SYSVAR_ULONGLONG(block_cache_size, srv_block_cache_size,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "TidesDB global block cache size in bytes", NULL, NULL,
                              TIDESDB_DEFAULT_BLOCK_CACHE, 0, ULONGLONG_MAX, 0);

static MYSQL_SYSVAR_ULONG(max_open_sstables, srv_max_open_sstables,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Max cached SSTable structures in LRU cache", NULL, NULL, 256, 1, 65536,
                          0);

static MYSQL_SYSVAR_ULONGLONG(max_memory_usage, srv_max_memory_usage,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "TidesDB global memory limit in bytes "
                              "(0 = auto, 50% of system RAM; minimum 5% of system RAM)",
                              NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static MYSQL_SYSVAR_BOOL(log_to_file, srv_log_to_file, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Write TidesDB logs to a LOG file in the data directory "
                         "instead of stderr (default: ON)",
                         NULL, NULL, 1);

static MYSQL_SYSVAR_ULONGLONG(log_truncation_at, srv_log_truncation_at,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "TidesDB log file truncation size in bytes "
                              "(0 disables truncation)",
                              NULL, NULL, 24ULL * 1024 * 1024, 0, ULONGLONG_MAX, 0);

static MYSQL_SYSVAR_BOOL(unified_memtable, srv_unified_memtable,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Use a single unified WAL and memtable across all column families. "
                         "Reduces WAL fsync overhead from O(num_tables) to O(1) and provides "
                         "atomic cross-CF commits. Best for multi-table OLTP workloads. "
                         "Requires all CFs to use the same comparator (default: ON)",
                         NULL, NULL, 1);

static MYSQL_SYSVAR_ULONGLONG(unified_memtable_write_buffer_size,
                              srv_unified_memtable_write_buffer_size,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "Write buffer size in bytes for the unified memtable. "
                              "0 = automatic (library default). Only meaningful when "
                              "tidesdb_unified_memtable=ON",
                              NULL, NULL, 256ULL * 1024 * 1024, 0, ULONGLONG_MAX, 0);

static ulong srv_unified_memtable_sync_mode = 2; /* FULL */

static MYSQL_SYSVAR_ENUM(unified_memtable_sync_mode, srv_unified_memtable_sync_mode,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Sync mode for the unified WAL when tidesdb_unified_memtable=ON.  "
                         "NONE relies on the OS page cache and is the fastest.  INTERVAL "
                         "syncs periodically every unified_memtable_sync_interval_us.  FULL "
                         "fsyncs on every commit and is the most durable.  This setting "
                         "governs WAL durability for every table under unified mode "
                         "regardless of any per-table SYNC_MODE option, which only "
                         "controls SSTable file sync",
                         NULL, NULL, 2 /* FULL */, &sync_mode_typelib);

static ulonglong srv_unified_memtable_sync_interval = 128000;

static MYSQL_SYSVAR_ULONGLONG(unified_memtable_sync_interval, srv_unified_memtable_sync_interval,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "Sync interval in microseconds for the unified WAL "
                              "(only used when unified_memtable_sync_mode=INTERVAL)",
                              NULL, NULL, 128000, 0, ULONGLONG_MAX, 0);

/* Skip-list tuning for the unified memtable.  Per-CF equivalents
   (skip_list_max_level, skip_list_probability) exist as table options;
   the unified-mode memtable uses a single skiplist for the whole DB so
   it needs its own global knob.  Default 0 / 0.0 keeps the library
   default. */
static ulong srv_unified_memtable_skip_list_max_level = 0;
static MYSQL_SYSVAR_ULONG(
    unified_memtable_skip_list_max_level, srv_unified_memtable_skip_list_max_level,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Skip-list max level for the unified memtable; 0 keeps the library default", NULL, NULL, 0, 0,
    32, 0);

static double srv_unified_memtable_skip_list_probability = 0.0;
static MYSQL_SYSVAR_DOUBLE(
    unified_memtable_skip_list_probability, srv_unified_memtable_skip_list_probability,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Skip-list level promotion probability for the unified memtable; 0.0 keeps the library default",
    NULL, NULL, 0.0, 0.0, 1.0, 0);

/* Configurable data directory.
   Defaults to NULL which means the plugin computes a sibling directory
   of mysql_real_data_home.  Setting this overrides the auto-computed path. */
static char *srv_data_home_dir = NULL;

static MYSQL_SYSVAR_STR(data_home_dir, srv_data_home_dir, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "Directory where TidesDB stores its data files; "
                        "defaults to <mysql_datadir>/../tidesdb_data; "
                        "must be set before server startup (read-only)",
                        NULL, NULL, NULL);

/* ******************** Object Store Configuration ******************** */

/* Object store backend (0=LOCAL (no object store), 1=S3) */
static ulong srv_object_store_backend = 0;
static const char *object_store_backend_names[] = {"LOCAL", "S3", NullS};
static TYPELIB object_store_backend_typelib = {array_elements(object_store_backend_names) - 1,
                                               "object_store_backend_typelib",
                                               object_store_backend_names, NULL, NULL};
static MYSQL_SYSVAR_ENUM(object_store_backend, srv_object_store_backend,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Object store backend (LOCAL=disabled, S3=S3-compatible)", NULL, NULL, 0,
                         &object_store_backend_typelib);

static char *srv_s3_endpoint = NULL;
static MYSQL_SYSVAR_STR(s3_endpoint, srv_s3_endpoint, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 endpoint (e.g. s3.amazonaws.com or minio.local:9000)", NULL, NULL,
                        NULL);

static char *srv_s3_bucket = NULL;
static MYSQL_SYSVAR_STR(s3_bucket, srv_s3_bucket, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 bucket name", NULL, NULL, NULL);

static char *srv_s3_prefix = NULL;
static MYSQL_SYSVAR_STR(s3_prefix, srv_s3_prefix, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 key prefix (e.g. production/db1/)", NULL, NULL, NULL);

static char *srv_s3_access_key = NULL;
static MYSQL_SYSVAR_STR(s3_access_key, srv_s3_access_key, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 access key ID", NULL, NULL, NULL);

static char *srv_s3_secret_key = NULL;
static MYSQL_SYSVAR_STR(s3_secret_key, srv_s3_secret_key, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 secret access key", NULL, NULL, NULL);

static char *srv_s3_region = NULL;
static MYSQL_SYSVAR_STR(s3_region, srv_s3_region, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                        "S3 region (e.g. us-east-1, NULL for MinIO)", NULL, NULL, NULL);

static my_bool srv_s3_use_ssl = 1;
static MYSQL_SYSVAR_BOOL(s3_use_ssl, srv_s3_use_ssl, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Use HTTPS for S3 connections (default ON)", NULL, NULL, 1);

static my_bool srv_s3_path_style = 0;
static MYSQL_SYSVAR_BOOL(s3_path_style, srv_s3_path_style,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Use path-style S3 URLs (required for MinIO, default OFF)", NULL, NULL, 0);

static char *srv_s3_tls_ca_path = NULL;
static MYSQL_SYSVAR_STR(
    s3_tls_ca_path, srv_s3_tls_ca_path, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Path to a custom CA bundle for the S3 TLS handshake, or empty to use the system bundle", NULL,
    NULL, NULL);

static my_bool srv_s3_tls_insecure_skip_verify = 0;
static MYSQL_SYSVAR_BOOL(
    s3_tls_insecure_skip_verify, srv_s3_tls_insecure_skip_verify,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Disable S3 TLS peer/host verification. INSECURE, intended for test endpoints only.", NULL,
    NULL, 0);

static ulonglong srv_s3_multipart_threshold = 0;
static MYSQL_SYSVAR_ULONGLONG(
    s3_multipart_threshold, srv_s3_multipart_threshold, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Object size in bytes at which S3 multipart upload activates. 0 keeps the library default.",
    NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static ulonglong srv_s3_multipart_part_size = 0;
static MYSQL_SYSVAR_ULONGLONG(s3_multipart_part_size, srv_s3_multipart_part_size,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "S3 multipart chunk size in bytes. 0 keeps the library default.",
                              NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static ulonglong srv_objstore_local_cache_max = 0;
static MYSQL_SYSVAR_ULONGLONG(
    objstore_local_cache_max, srv_objstore_local_cache_max,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Maximum local cache size in bytes for object store mode (0=unlimited)", NULL, NULL, 0, 0,
    ULONGLONG_MAX, 0);

static ulonglong srv_objstore_wal_sync_threshold = 1048576;
static MYSQL_SYSVAR_ULONGLONG(
    objstore_wal_sync_threshold, srv_objstore_wal_sync_threshold,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Sync active WAL to object store when it grows by this many bytes (default 1MB, 0=disable)",
    NULL, NULL, 1048576, 0, ULONGLONG_MAX, 0);

static my_bool srv_objstore_wal_sync_on_commit = 0;
static MYSQL_SYSVAR_BOOL(objstore_wal_sync_on_commit, srv_objstore_wal_sync_on_commit,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Upload WAL after every commit for RPO=0 replication (default OFF)", NULL,
                         NULL, 0);

static my_bool srv_replica_mode = 0;
static MYSQL_SYSVAR_BOOL(replica_mode, srv_replica_mode, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Enable read-only replica mode (default OFF)", NULL, NULL, 0);

/* When ON, deinit calls tidesdb_cancel_background_work before tidesdb_close
   so in-flight compactions bail at their next checkpoint (uncommitted output
   discarded, inputs intact) and shutdown returns quickly even with a multi-GB
   compaction backlog.  Default OFF restores pre-4.5.4 behaviour where
   tidesdb_close drains background work naturally; this is the safer setting
   for object-store / replica setups where a mid-compaction cancel can leave
   S3 in an inconsistent state that confuses a syncing replica. */
static my_bool srv_fast_shutdown = 0;
static MYSQL_SYSVAR_BOOL(fast_shutdown, srv_fast_shutdown,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Call tidesdb_cancel_background_work at deinit so shutdown does not "
                         "wait for in-flight compactions to drain.  Default OFF; turn ON only "
                         "when shutdown latency on a large compaction backlog matters more "
                         "than clean handoff to replicas reading the object store",
                         NULL, NULL, 0);

static my_bool srv_objstore_cache_on_read = 1;
static MYSQL_SYSVAR_BOOL(objstore_cache_on_read, srv_objstore_cache_on_read,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Cache downloaded objects in the local cache (default ON)", NULL, NULL, 1);

static my_bool srv_objstore_cache_on_write = 1;
static MYSQL_SYSVAR_BOOL(objstore_cache_on_write, srv_objstore_cache_on_write,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Cache uploaded objects in the local cache (default ON)", NULL, NULL, 1);

static ulong srv_objstore_max_concurrent_uploads = 0;
static MYSQL_SYSVAR_ULONG(objstore_max_concurrent_uploads, srv_objstore_max_concurrent_uploads,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Concurrent upload threads; 0 uses the library default", NULL, NULL, 0, 0,
                          1024, 0);

static ulong srv_objstore_max_concurrent_downloads = 0;
static MYSQL_SYSVAR_ULONG(objstore_max_concurrent_downloads, srv_objstore_max_concurrent_downloads,
                          PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                          "Concurrent download threads; 0 uses the library default", NULL, NULL, 0,
                          0, 1024, 0);

static ulonglong srv_objstore_multipart_threshold = 0;
static MYSQL_SYSVAR_ULONGLONG(
    objstore_multipart_threshold, srv_objstore_multipart_threshold,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Object size in bytes that triggers multipart upload; 0 keeps the library default", NULL, NULL,
    0, 0, ULONGLONG_MAX, 0);

static ulonglong srv_objstore_multipart_part_size = 0;
static MYSQL_SYSVAR_ULONGLONG(objstore_multipart_part_size, srv_objstore_multipart_part_size,
                              PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                              "Multipart upload chunk size in bytes; 0 keeps the library default",
                              NULL, NULL, 0, 0, ULONGLONG_MAX, 0);

static my_bool srv_objstore_sync_manifest_to_object = 1;
static MYSQL_SYSVAR_BOOL(objstore_sync_manifest_to_object, srv_objstore_sync_manifest_to_object,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Upload MANIFEST after each compaction (default ON)", NULL, NULL, 1);

static my_bool srv_objstore_wal_upload_sync = 0;
static MYSQL_SYSVAR_BOOL(
    objstore_wal_upload_sync, srv_objstore_wal_upload_sync,
    PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "Block memtable flush on WAL upload (default OFF for background WAL upload)", NULL, NULL, 0);

static my_bool srv_objstore_replicate_wal = 1;
static MYSQL_SYSVAR_BOOL(objstore_replicate_wal, srv_objstore_replicate_wal,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Upload WAL segments for replica recovery (default ON)", NULL, NULL, 1);

static my_bool srv_objstore_replica_replay_wal = 1;
static MYSQL_SYSVAR_BOOL(objstore_replica_replay_wal, srv_objstore_replica_replay_wal,
                         PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
                         "Replay WAL on replicas for near-real-time visibility (default ON)", NULL,
                         NULL, 1);

static ulonglong srv_replica_sync_interval = 5000000;
static MYSQL_SYSVAR_ULONGLONG(
    replica_sync_interval, srv_replica_sync_interval, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_READONLY,
    "MANIFEST poll interval for replica sync in microseconds (default 5s)", NULL, NULL, 5000000,
    100000, ULONGLONG_MAX, 0);

/* Promote replica to primary -- trigger variable (like backup_dir) */
static my_bool srv_promote_primary = 0;
static void tidesdb_promote_primary_update(THD *thd, struct st_mysql_sys_var *, void *var_ptr,
                                           const void *save)
{
    my_bool val = *static_cast<const my_bool *>(save);
    if (!val) return; /* only act on SET ... = ON */

    if (!tdb_global)
    {
        my_error(ER_UNKNOWN_ERROR, MYF(0));
        return;
    }

    int rc = tidesdb_promote_to_primary(tdb_global);
    if (rc == TDB_SUCCESS)
    {
        sql_print_information("[TIDESDB] Replica promoted to primary successfully");
    }
    else
    {
        sql_print_error("[TIDESDB] Failed to promote replica (err=%d)", rc);
    }

    /* reset to OFF so it can be triggered again */
    *static_cast<my_bool *>(var_ptr) = 0;
}

static MYSQL_SYSVAR_BOOL(promote_primary, srv_promote_primary, PLUGIN_VAR_RQCMDARG,
                         "Set to ON to promote this replica to primary (trigger, resets to OFF)",
                         NULL, tidesdb_promote_primary_update, 0);

/* ******************** Online backup via system variable ******************** */

static char *srv_backup_dir = NULL;

static void tidesdb_backup_dir_update(THD *thd, struct st_mysql_sys_var *, void *var_ptr,
                                      const void *save)
{
    const char *new_dir = *static_cast<const char *const *>(save);

    if (!new_dir || !new_dir[0])
    {
        /* Empty string -- we just clear the variable */
        *static_cast<char **>(var_ptr) = NULL;
        return;
    }

    if (!tdb_global)
    {
        my_error(ER_UNKNOWN_ERROR, MYF(0), "TidesDB is not open");
        return;
    }

    /* Free the calling connection's TidesDB transaction before backup.
       tidesdb_backup() waits for all open transactions to drain.  The
       connection may still hold an open txn (created in external_lock
       but not yet committed).  If we don't free it here, the backup
       self-deadlocks waiting for our own txn. */
    {
        tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
        if (trx && trx->txn)
        {
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->dirty = false;
            trx->txn_generation++;
            trx->fts_meta_pending.clear();
            trx->fts_meta_dirty = false;
        }
    }

    /* We copy the path before releasing the sysvar lock -- the save pointer
       is only valid while LOCK_global_system_variables is held. */
    std::string backup_path(new_dir);

    /* tidesdb_backup() spins waiting for all CF flushes to complete.
       The library's flush threads call sql_print_information() which
       internally acquires LOCK_global_system_variables.  This sysvar
       update callback is called WITH that mutex held, so tidesdb_backup()
       deadlocks (flush thread waits for lock, we wait for flush thread).
       Release the mutex around the blocking backup call. */
    mysql_mutex_unlock(&LOCK_global_system_variables);

    /* Backup started -- no log (user-triggered, success/failure reported via return code) */

    char *backup_path_c = const_cast<char *>(backup_path.c_str());
    int rc = tidesdb_backup(tdb_global, backup_path_c);

    mysql_mutex_lock(&LOCK_global_system_variables);

    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] Backup to '%s' failed (err=%d)", backup_path.c_str(), rc);
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] Backup to '%s' failed (err=%d)", MYF(0),
                        backup_path.c_str(), rc);
        return;
    }

    /* For PLUGIN_VAR_MEMALLOC strings, the framework manages memory.
       We set var_ptr to the save value so the framework copies it. */
    *static_cast<const char **>(var_ptr) = new_dir;
}

static MYSQL_SYSVAR_STR(backup_dir, srv_backup_dir, PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Set to a directory path to trigger an online TidesDB backup. "
                        "The directory must not exist or be empty. "
                        "Example: SET GLOBAL tidesdb_backup_dir = '/path/to/backup'",
                        NULL, tidesdb_backup_dir_update, NULL);

/* Checkpoint (hard-link snapshot) via system variable */

static char *srv_checkpoint_dir = NULL;

static void tidesdb_checkpoint_dir_update(THD *thd, struct st_mysql_sys_var *, void *var_ptr,
                                          const void *save)
{
    const char *new_dir = *static_cast<const char *const *>(save);

    if (!new_dir || !new_dir[0])
    {
        *static_cast<char **>(var_ptr) = NULL;
        return;
    }

    if (!tdb_global)
    {
        my_error(ER_UNKNOWN_ERROR, MYF(0), "TidesDB is not open");
        return;
    }

    /* Checkpoint started -- no log */

    int rc = tidesdb_checkpoint(tdb_global, new_dir);

    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] Checkpoint to '%s' failed (err=%d)", new_dir, rc);
        my_printf_error(ER_UNKNOWN_ERROR, "[TIDESDB] Checkpoint to '%s' failed (err=%d)", MYF(0),
                        new_dir, rc);
        return;
    }

    /* Checkpoint completed -- no log */
    *static_cast<const char **>(var_ptr) = new_dir;
}

static MYSQL_SYSVAR_STR(checkpoint_dir, srv_checkpoint_dir,
                        PLUGIN_VAR_RQCMDARG | PLUGIN_VAR_MEMALLOC,
                        "Set to a directory path to trigger a TidesDB checkpoint "
                        "(hard-link snapshot, near-instant). "
                        "The directory must not exist or be empty. "
                        "Example: SET GLOBAL tidesdb_checkpoint_dir = '/path/to/checkpoint'",
                        NULL, tidesdb_checkpoint_dir_update, NULL);

static struct st_mysql_sys_var *tidesdb_system_variables[] = {
    MYSQL_SYSVAR(flush_threads),
    MYSQL_SYSVAR(max_concurrent_flushes),
    MYSQL_SYSVAR(compaction_threads),
    MYSQL_SYSVAR(log_level),
    MYSQL_SYSVAR(block_cache_size),
    MYSQL_SYSVAR(max_open_sstables),
    MYSQL_SYSVAR(max_memory_usage),
    MYSQL_SYSVAR(backup_dir),
    MYSQL_SYSVAR(checkpoint_dir),
    MYSQL_SYSVAR(print_all_conflicts),
    MYSQL_SYSVAR(pessimistic_locking),
    MYSQL_SYSVAR(fts_min_word_len),
    MYSQL_SYSVAR(fts_max_word_len),
    MYSQL_SYSVAR(fts_bm25_k1),
    MYSQL_SYSVAR(fts_bm25_b),
    MYSQL_SYSVAR(ft_stopword_table),
    MYSQL_SYSVAR(fts_blend_chars),
    MYSQL_SYSVAR(data_home_dir),
    MYSQL_SYSVAR(ttl),
    MYSQL_SYSVAR(skip_unique_check),
    MYSQL_SYSVAR(single_delete_primary),
    MYSQL_SYSVAR(backpressure_wait_timeout_ms),
    MYSQL_SYSVAR(lock_wait_timeout_ms),
    MYSQL_SYSVAR(compact_after_range_delete_min_rows),
    MYSQL_SYSVAR(default_compression),
    MYSQL_SYSVAR(default_write_buffer_size),
    MYSQL_SYSVAR(default_bloom_filter),
    MYSQL_SYSVAR(default_use_btree),
    MYSQL_SYSVAR(default_block_indexes),
    MYSQL_SYSVAR(default_sync_mode),
    MYSQL_SYSVAR(default_sync_interval_us),
    MYSQL_SYSVAR(default_bloom_fpr),
    MYSQL_SYSVAR(default_klog_value_threshold),
    MYSQL_SYSVAR(default_l0_queue_stall_threshold),
    MYSQL_SYSVAR(default_l1_file_count_trigger),
    MYSQL_SYSVAR(default_level_size_ratio),
    MYSQL_SYSVAR(default_min_levels),
    MYSQL_SYSVAR(default_dividing_level_offset),
    MYSQL_SYSVAR(default_skip_list_max_level),
    MYSQL_SYSVAR(default_skip_list_probability),
    MYSQL_SYSVAR(default_index_sample_ratio),
    MYSQL_SYSVAR(default_block_index_prefix_len),
    MYSQL_SYSVAR(default_min_disk_space),
    MYSQL_SYSVAR(default_isolation_level),
    MYSQL_SYSVAR(log_to_file),
    MYSQL_SYSVAR(log_truncation_at),
    MYSQL_SYSVAR(unified_memtable),
    MYSQL_SYSVAR(unified_memtable_write_buffer_size),
    MYSQL_SYSVAR(unified_memtable_sync_mode),
    MYSQL_SYSVAR(unified_memtable_sync_interval),
    MYSQL_SYSVAR(unified_memtable_skip_list_max_level),
    MYSQL_SYSVAR(unified_memtable_skip_list_probability),
    MYSQL_SYSVAR(object_store_backend),
    MYSQL_SYSVAR(s3_endpoint),
    MYSQL_SYSVAR(s3_bucket),
    MYSQL_SYSVAR(s3_prefix),
    MYSQL_SYSVAR(s3_access_key),
    MYSQL_SYSVAR(s3_secret_key),
    MYSQL_SYSVAR(s3_region),
    MYSQL_SYSVAR(s3_use_ssl),
    MYSQL_SYSVAR(s3_path_style),
    MYSQL_SYSVAR(s3_tls_ca_path),
    MYSQL_SYSVAR(s3_tls_insecure_skip_verify),
    MYSQL_SYSVAR(s3_multipart_threshold),
    MYSQL_SYSVAR(s3_multipart_part_size),
    MYSQL_SYSVAR(objstore_local_cache_max),
    MYSQL_SYSVAR(objstore_wal_sync_threshold),
    MYSQL_SYSVAR(objstore_wal_sync_on_commit),
    MYSQL_SYSVAR(objstore_cache_on_read),
    MYSQL_SYSVAR(objstore_cache_on_write),
    MYSQL_SYSVAR(objstore_max_concurrent_uploads),
    MYSQL_SYSVAR(objstore_max_concurrent_downloads),
    MYSQL_SYSVAR(objstore_multipart_threshold),
    MYSQL_SYSVAR(objstore_multipart_part_size),
    MYSQL_SYSVAR(objstore_sync_manifest_to_object),
    MYSQL_SYSVAR(objstore_wal_upload_sync),
    MYSQL_SYSVAR(objstore_replicate_wal),
    MYSQL_SYSVAR(objstore_replica_replay_wal),
    MYSQL_SYSVAR(replica_mode),
    MYSQL_SYSVAR(fast_shutdown),
    MYSQL_SYSVAR(replica_sync_interval),
    MYSQL_SYSVAR(promote_primary),
    MYSQL_SYSVAR(default_object_lazy_compaction),
    MYSQL_SYSVAR(default_object_prefetch_compaction),
    MYSQL_SYSVAR(default_tombstone_density_trigger),
    MYSQL_SYSVAR(default_tombstone_density_min_entries),
    NULL};

/* ******************** Table options (per-table CF config) ******************** */

struct ha_table_option_struct
{
    ulonglong write_buffer_size;
    ulonglong min_disk_space;
    ulonglong klog_value_threshold;
    ulonglong sync_interval_us;
    ulonglong index_sample_ratio;
    ulonglong block_index_prefix_len;
    ulonglong level_size_ratio;
    ulonglong min_levels;
    ulonglong dividing_level_offset;
    ulonglong skip_list_max_level;
    ulonglong skip_list_probability; /* percentage      -- 25 = 0.25 */
    ulonglong bloom_fpr;             /* parts per 10000 -- 100 = 1% */
    ulonglong l1_file_count_trigger;
    ulonglong l0_queue_stall_threshold;
    uint compression;
    uint sync_mode;
    uint isolation_level;
    bool bloom_filter;
    bool block_indexes;
    bool use_btree;
    bool object_lazy_compaction;     /* double L1 file count trigger in object store mode */
    bool object_prefetch_compaction; /* prefetch input SSTables before compaction merge */
    ulonglong ttl;                   /* default TTL in seconds (0 = no expiration) */
    bool encrypted;                  /* ENCRYPTED=YES enables data-at-rest encryption */
    ulonglong encryption_key_id;     /* ENCRYPTION_KEY_ID (default 1) */
    /* Tombstone-density compaction trigger.  Stored as parts-per-10000
       (e.g. 5000 = 0.50 ratio) so the option list can use integer storage;
       converted to a double at build_cf_config time. */
    ulonglong tombstone_density_trigger;
    ulonglong tombstone_density_min_entries;
};

ha_create_table_option tidesdb_table_option_list[] = {
    /* Options with SYSVAR defaults inherit from session variables
       (e.g. SET SESSION tidesdb_default_write_buffer_size=64*1024*1024).
       When not explicitly set in CREATE TABLE, the session default is used. */
    HA_TOPTION_SYSVAR("WRITE_BUFFER_SIZE", write_buffer_size, default_write_buffer_size),
    HA_TOPTION_SYSVAR("MIN_DISK_SPACE", min_disk_space, default_min_disk_space),
    HA_TOPTION_SYSVAR("KLOG_VALUE_THRESHOLD", klog_value_threshold, default_klog_value_threshold),
    HA_TOPTION_SYSVAR("SYNC_INTERVAL_US", sync_interval_us, default_sync_interval_us),
    HA_TOPTION_SYSVAR("INDEX_SAMPLE_RATIO", index_sample_ratio, default_index_sample_ratio),
    HA_TOPTION_SYSVAR("BLOCK_INDEX_PREFIX_LEN", block_index_prefix_len,
                      default_block_index_prefix_len),
    HA_TOPTION_SYSVAR("LEVEL_SIZE_RATIO", level_size_ratio, default_level_size_ratio),
    HA_TOPTION_SYSVAR("MIN_LEVELS", min_levels, default_min_levels),
    HA_TOPTION_SYSVAR("DIVIDING_LEVEL_OFFSET", dividing_level_offset,
                      default_dividing_level_offset),
    HA_TOPTION_SYSVAR("SKIP_LIST_MAX_LEVEL", skip_list_max_level, default_skip_list_max_level),
    HA_TOPTION_SYSVAR("SKIP_LIST_PROBABILITY", skip_list_probability,
                      default_skip_list_probability),
    HA_TOPTION_SYSVAR("BLOOM_FPR", bloom_fpr, default_bloom_fpr),
    HA_TOPTION_SYSVAR("L1_FILE_COUNT_TRIGGER", l1_file_count_trigger,
                      default_l1_file_count_trigger),
    HA_TOPTION_SYSVAR("L0_QUEUE_STALL_THRESHOLD", l0_queue_stall_threshold,
                      default_l0_queue_stall_threshold),
    HA_TOPTION_SYSVAR("COMPRESSION", compression, default_compression),
    HA_TOPTION_SYSVAR("SYNC_MODE", sync_mode, default_sync_mode),
    HA_TOPTION_SYSVAR("ISOLATION_LEVEL", isolation_level, default_isolation_level),
    HA_TOPTION_SYSVAR("BLOOM_FILTER", bloom_filter, default_bloom_filter),
    HA_TOPTION_SYSVAR("BLOCK_INDEXES", block_indexes, default_block_indexes),
    HA_TOPTION_SYSVAR("USE_BTREE", use_btree, default_use_btree),
    HA_TOPTION_SYSVAR("OBJECT_LAZY_COMPACTION", object_lazy_compaction,
                      default_object_lazy_compaction),
    HA_TOPTION_SYSVAR("OBJECT_PREFETCH_COMPACTION", object_prefetch_compaction,
                      default_object_prefetch_compaction),
    HA_TOPTION_SYSVAR("TOMBSTONE_DENSITY_TRIGGER", tombstone_density_trigger,
                      default_tombstone_density_trigger),
    HA_TOPTION_SYSVAR("TOMBSTONE_DENSITY_MIN_ENTRIES", tombstone_density_min_entries,
                      default_tombstone_density_min_entries),
    HA_TOPTION_NUMBER("TTL", ttl, 0, 0, ULONGLONG_MAX, 1),
    HA_TOPTION_BOOL("ENCRYPTED", encrypted, 0),
    HA_TOPTION_NUMBER("ENCRYPTION_KEY_ID", encryption_key_id, 1, 1, 255, 1),
    HA_TOPTION_END};

/* ******************** Field options (per-column) ******************** */

struct ha_field_option_struct
{
    bool ttl; /* marks this column as the per-row TTL source (seconds) */
};

ha_create_table_option tidesdb_field_option_list[] = {HA_FOPTION_BOOL("TTL", ttl, 0),
                                                      HA_FOPTION_END};

/* ******************** Index options (per-index) ******************** */

struct ha_index_option_struct
{
    bool use_btree; /* per-index B-tree override */
};

ha_create_table_option tidesdb_index_option_list[] = {HA_IOPTION_BOOL("USE_BTREE", use_btree, 0),
                                                      HA_IOPTION_END};

/* ******************** Big-endian helpers for hidden PK ********************
   Hidden-PK rows are keyed by an 8-byte big-endian uint64 so that memcmp
   on the encoded bytes matches numeric ordering of the row id. */

static void encode_be64(uint64_t id, uint8_t *buf)
{
    for (uint i = 0; i < sizeof(uint64_t); i++)
        buf[i] = (uint8_t)(id >> ((sizeof(uint64_t) - 1 - i) * BITS_PER_BYTE));
}

static uint64_t decode_be64(const uint8_t *buf)
{
    uint64_t id = 0;
    for (uint i = 0; i < sizeof(uint64_t); i++) id = (id << BITS_PER_BYTE) | (uint64_t)buf[i];
    return id;
}

/*
  Return true if a TidesDB key is a data key (starts with KEY_NS_DATA).
*/
static inline bool is_data_key(const uint8_t *key, size_t key_size)
{
    return key_size > 0 && key[0] == KEY_NS_DATA;
}

/* Shared enum-to-constant maps (used by create, open, prepare_inplace) */

static const int tdb_compression_map[] = {TDB_COMPRESS_NONE, TDB_COMPRESS_SNAPPY, TDB_COMPRESS_LZ4,
                                          TDB_COMPRESS_ZSTD, TDB_COMPRESS_LZ4_FAST};

static const int tdb_sync_mode_map[] = {TDB_SYNC_NONE, TDB_SYNC_INTERVAL, TDB_SYNC_FULL};

static const int tdb_isolation_map[] = {TDB_ISOLATION_READ_UNCOMMITTED,
                                        TDB_ISOLATION_READ_COMMITTED, TDB_ISOLATION_REPEATABLE_READ,
                                        TDB_ISOLATION_SNAPSHOT, TDB_ISOLATION_SERIALIZABLE};

/*
  Map the MariaDB session isolation level (from SET TRANSACTION ISOLATION
  LEVEL) to a TidesDB isolation level.  An explicitly chosen session level
  always wins.  When the session is left at the SQL default of REPEATABLE
  READ the table-level ISOLATION_LEVEL option decides, because that is the
  signal that the client expressed no preference of its own.

  The MariaDB enum_tx_isolation values are ISO_READ_UNCOMMITTED 0,
  ISO_READ_COMMITTED 1, ISO_REPEATABLE_READ 2 and ISO_SERIALIZABLE 3.

  TidesDB has a fifth level, SNAPSHOT, with no SQL equivalent.  A table
  that leaves ISOLATION_LEVEL at REPEATABLE READ resolves to SNAPSHOT for
  InnoDB parity, since TidesDB's strict REPEATABLE_READ tracks the read
  set and produces excessive TDB_ERR_CONFLICT under normal OLTP.  A table
  that sets SNAPSHOT, SERIALIZABLE, READ COMMITTED or READ UNCOMMITTED is
  honored as written.
*/
static tidesdb_isolation_level_t resolve_effective_isolation(THD *thd,
                                                             tidesdb_isolation_level_t table_iso)
{
    int session_iso = thd_tx_isolation(thd);

    switch (session_iso)
    {
        case ISO_READ_UNCOMMITTED:
            return TDB_ISOLATION_READ_UNCOMMITTED;
        case ISO_READ_COMMITTED:
            return TDB_ISOLATION_READ_COMMITTED;
        case ISO_REPEATABLE_READ:
            /* The session is at the SQL default, so the table-level
               ISOLATION_LEVEL option decides.  A table left at REPEATABLE
               READ maps to TidesDB SNAPSHOT for InnoDB parity, since
               TidesDB's strict REPEATABLE_READ tracks the read set and
               produces excessive TDB_ERR_CONFLICT under normal OLTP.  An
               explicit SNAPSHOT, SERIALIZABLE, READ COMMITTED or READ
               UNCOMMITTED table option is honored as written. */
            return table_iso == TDB_ISOLATION_REPEATABLE_READ ? TDB_ISOLATION_SNAPSHOT : table_iso;
        case ISO_SERIALIZABLE:
            return TDB_ISOLATION_SERIALIZABLE;
        default:
            return TDB_ISOLATION_READ_COMMITTED;
    }
}

/* Single-byte placeholder value for secondary index entries (all info is in the key) */
static const uint8_t tdb_empty_val = 0;

/*
  Build a tidesdb_column_family_config_t from table options.
  Centralises the option-to-config mapping so create() and
  prepare_inplace_alter_table() stay in sync.
*/
static tidesdb_column_family_config_t build_cf_config(const ha_table_option_struct *opts)
{
    tidesdb_column_family_config_t cfg = tidesdb_default_column_family_config();
    if (!opts) return cfg;

    cfg.write_buffer_size = (size_t)opts->write_buffer_size;
    cfg.compression_algorithm = (compression_algorithm)tdb_compression_map[opts->compression];
    cfg.enable_bloom_filter = opts->bloom_filter ? 1 : 0;
    cfg.bloom_fpr = (double)opts->bloom_fpr / TIDESDB_BLOOM_FPR_DIVISOR;
    cfg.enable_block_indexes = opts->block_indexes ? 1 : 0;
    cfg.index_sample_ratio = (int)opts->index_sample_ratio;
    cfg.block_index_prefix_len = (int)opts->block_index_prefix_len;
    cfg.sync_mode = tdb_sync_mode_map[opts->sync_mode];
    cfg.sync_interval_us = (uint64_t)opts->sync_interval_us;
    cfg.klog_value_threshold = (size_t)opts->klog_value_threshold;
    cfg.min_disk_space = (size_t)opts->min_disk_space;
    cfg.default_isolation_level =
        (tidesdb_isolation_level_t)tdb_isolation_map[opts->isolation_level];
    cfg.level_size_ratio = (int)opts->level_size_ratio;
    cfg.min_levels = (int)opts->min_levels;
    cfg.dividing_level_offset = (int)opts->dividing_level_offset;
    cfg.skip_list_max_level = (int)opts->skip_list_max_level;
    cfg.skip_list_probability = (float)opts->skip_list_probability / TIDESDB_SKIP_LIST_PROB_DIV;
    cfg.l1_file_count_trigger = (int)opts->l1_file_count_trigger;
    cfg.l0_queue_stall_threshold = (int)opts->l0_queue_stall_threshold;
    cfg.use_btree = opts->use_btree ? 1 : 0;
    cfg.object_lazy_compaction = opts->object_lazy_compaction ? 1 : 0;
    cfg.object_prefetch_compaction = opts->object_prefetch_compaction ? 1 : 0;
    cfg.tombstone_density_trigger =
        (double)opts->tombstone_density_trigger / TIDESDB_TOMBSTONE_DENSITY_DIVISOR;
    cfg.tombstone_density_min_entries = (uint64_t)opts->tombstone_density_min_entries;
    return cfg;
}

/*
  Resolve a secondary index CF by name.
  Returns the CF pointer (may be NULL if not found).
  Writes the CF name into out_name.
*/
static tidesdb_column_family_t *resolve_idx_cf(tidesdb_t *db, const std::string &table_cf,
                                               const char *key_name, std::string &out_name)
{
    out_name = table_cf + CF_INDEX_INFIX + key_name;
    return tidesdb_get_column_family(db, out_name.c_str());
}

/* ******************** TidesDB_share ******************** */

TidesDB_share::TidesDB_share()
    : cf(NULL),
      has_user_pk(false),
      pk_index(0),
      pk_key_len(0),
      next_row_id(1),
      isolation_level(TDB_ISOLATION_REPEATABLE_READ),
      default_ttl(0),
      ttl_field_idx(TIDESDB_TTL_FIELD_NONE),
      has_blobs(false),
      has_ttl(false),
      num_secondary_indexes(0)
{
    memset(idx_comp_key_len, 0, sizeof(idx_comp_key_len));
    memset(idx_is_fts, 0, sizeof(idx_is_fts));
    memset(idx_is_spatial, 0, sizeof(idx_is_spatial));
    for (uint i = 0; i < MAX_KEY; i++) cached_rec_per_key[i].store(0, std::memory_order_relaxed);
}

TidesDB_share::~TidesDB_share()
{
}

/* ******************** Per-connection transaction helpers ******************** */

/*
  Get or create the per-connection TidesDB transaction context.
  The txn lives for the entire BEGIN...COMMIT block (or single auto-commit
  statement).  All handler objects on the same connection share it.
*/
static tidesdb_trx_t *get_or_create_trx(THD *thd, handlerton *hton, tidesdb_isolation_level_t iso)
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, hton);
    if (trx)
    {
        if (!trx->txn)
        {
            int rc = tidesdb_txn_begin_with_isolation(tdb_global, iso, &trx->txn);
            if (rc != TDB_SUCCESS)
            {
                (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(reuse)");
                return NULL;
            }
            trx->dirty = false;
            trx->isolation_level = iso;
            trx->txn_generation++;
        }
        else if (trx->needs_reset)
        {
            /* Txn object kept alive from previous commit/rollback (see
               tidesdb_commit).  We reset it to get a fresh MVCC snapshot at
               current-transaction-start.  This avoids the expensive
               free+begin cycle while ensuring we see the latest data.
               The bulk-insert path already uses commit+reset successfully.
               Only reset when needs_reset is true (set after real commit/
               rollback) to preserve snapshot within multi-statement txns. */
            int rrc = tidesdb_txn_reset(trx->txn, iso);
            if (rrc != TDB_SUCCESS)
            {
                /* Reset failed -- we fall back to free + begin.  Surface the
                   failure so we can spot regressions in txn recycling instead
                   of silently degrading to per-statement free+begin. */
                sql_print_warning(
                    "[TIDESDB] tidesdb_txn_reset failed (rc=%d), falling back to "
                    "free+begin -- expect higher per-statement overhead until "
                    "this is investigated",
                    rrc);
                tidesdb_txn_free(trx->txn);
                trx->txn = NULL;
                int rc = tidesdb_txn_begin_with_isolation(tdb_global, iso, &trx->txn);
                if (rc != TDB_SUCCESS)
                {
                    (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(reset_fallback)");
                    return NULL;
                }
            }
            trx->needs_reset = false;
            trx->isolation_level = iso;
            trx->txn_generation++;
        }
        return trx;
    }

    /* The trx struct owns a std::vector (fts_meta_pending), so it must be
       constructed and destroyed properly.  Switching from MY_ZEROFILL/my_free
       to new/delete runs the std::vector's ctor/dtor and gives every field
       its default value via the header's member initialisers. */
    trx = new tidesdb_trx_t{};
    if (!trx) return NULL;

    int rc = tidesdb_txn_begin_with_isolation(tdb_global, iso, &trx->txn);
    if (rc != TDB_SUCCESS)
    {
        delete trx;
        (void)tdb_rc_to_ha(rc, "get_or_create_trx txn_begin(new)");
        return NULL;
    }
    trx->isolation_level = iso;
    trx->txn_generation = 1;
    thd_set_ha_data(thd, hton, trx);
    return trx;
}

/* ******************** Handlerton transaction callbacks ******************** */

/* Maximum length of a TidesDB savepoint name, including the trailing NUL.
   Names are synthesized via TIDESDB_SAVEPOINT_NAME_FMT below; 32 bytes
   fits the decoded pointer plus prefix on all supported platforms. */
static constexpr uint TIDESDB_SAVEPOINT_NAME_MAX = 32;
/* Format used to synthesize a unique savepoint name for the TidesDB
   transaction layer.  The pointer to the SQL-layer savepoint slot is
   the only handle we have that survives across the set/rollback/release
   callbacks, so we encode it as the engine-level savepoint name. */
static constexpr const char TIDESDB_SAVEPOINT_NAME_FMT[] = "sv_%p";

struct tidesdb_savepoint_t
{
    char name[TIDESDB_SAVEPOINT_NAME_MAX];
};

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_savepoint_set(THD *thd, void *sv)
#else
static int tidesdb_savepoint_set(handlerton *, THD *thd, void *sv)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS) return 0;
    return tdb_rc_to_ha(rc, "savepoint_set");
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_savepoint_rollback(THD *thd, void *sv)
#else
static int tidesdb_savepoint_rollback(handlerton *, THD *thd, void *sv)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    if (!sp->name[0]) snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_rollback_to_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS)
    {
        /* The TidesDB library may drop the savepoint as part of the rollback.
           SQL semantics require the savepoint to still exist after rollback,
           so we re-create it here to allow RELEASE SAVEPOINT to succeed. */
        (void)tidesdb_txn_savepoint(trx->txn, sp->name);
        return 0;
    }
    if (rc == TDB_ERR_NOT_FOUND) return HA_ERR_NO_SAVEPOINT;
    return tdb_rc_to_ha(rc, "savepoint_rollback");
}

#if MYSQL_VERSION_ID >= 110800
static bool tidesdb_savepoint_rollback_can_release_mdl(THD *)
#else
static bool tidesdb_savepoint_rollback_can_release_mdl(handlerton *, THD *)
#endif
{
    return true;
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_savepoint_release(THD *thd, void *sv)
#else
static int tidesdb_savepoint_release(handlerton *, THD *thd, void *sv)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn || !sv) return 0;

    tidesdb_savepoint_t *sp = (tidesdb_savepoint_t *)sv;
    if (!sp->name[0]) snprintf(sp->name, sizeof(sp->name), TIDESDB_SAVEPOINT_NAME_FMT, sv);

    int rc = tidesdb_txn_release_savepoint(trx->txn, sp->name);
    if (rc == TDB_SUCCESS) return 0;
    if (rc == TDB_ERR_NOT_FOUND) return HA_ERR_NO_SAVEPOINT;
    return tdb_rc_to_ha(rc, "savepoint_release");
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_commit(THD *thd, bool all)
#else
static int tidesdb_commit(handlerton *, THD *thd, bool all)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn)
    {
        return 0;
    }

    /* We determine whether this is the final commit for the transaction.
       all=true         -> explicit COMMIT or transaction-level end
       all=false        -> statement-level; only a real commit when autocommit */
    bool is_real_commit = all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    if (!is_real_commit)
    {
        /* Statement-level commit inside a multi-statement transaction.
           Defer the actual commit -- writes stay buffered in the txn,
           avoiding expensive txn_begin + commit per statement.

           tidesdb_txn_savepoint() deep-copies the entire
           write-set (malloc+memcpy for every key/value).  For a txn
           with N ops across S statements, total copy cost is
           O(S * N * avg_kv_size) -- quadratic and devastating for
           multi-statement OLTP transactions.

           We skip the per-statement savepoint entirely.  This means
           statement-level rollback inside BEGIN...COMMIT falls back to
           full transaction rollback (same as many simple SE's).
           The trade-off is a statement failure aborts the entire txn
           instead of undoing just that statement.  For OLTP this is
           acceptable since the client will retry the whole transaction
           anyway after a conflict/error. */
        return 0;
    }

    /* We must release any active statement savepoint before final commit/rollback.
       Savepoints must be explicitly released before txn_commit. */
    if (trx->stmt_savepoint_active)
    {
        tidesdb_txn_release_savepoint(trx->txn, "stmt");
        trx->stmt_savepoint_active = false;
    }

    /* Real commit -- flush to storage.
       After a successful commit, we keep the txn object alive and let
       get_or_create_trx() call tidesdb_txn_reset() to get a fresh
       snapshot.  This avoids the expensive free+begin cycle on every
       autocommit statement (saves malloc/free + internal buffer
       reallocation).  The bulk-insert path already uses commit+reset
       successfully, so the pattern is proven safe.
       If commit fails, fall back to rollback+free. */
    if (trx->dirty)
    {
        /* Fold the per-txn FTS meta deltas into this same txn before it
           commits so the meta update is atomic with the row writes that
           produced it. */
        int frc = flush_trx_fts_meta_pending(thd, trx);
        if (frc != TDB_SUCCESS)
        {
            sql_print_error(
                "[TIDESDB] hton_commit: flush_trx_fts_meta_pending returned %d (gen=%lu)", frc,
                (unsigned long)trx->txn_generation);
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->txn_generation++;
            trx->dirty = false;
            trx->stmt_savepoint_active = false;
            row_locks_release_all(trx);
            return tdb_rc_to_ha(frc, "hton_commit fts_meta_flush");
        }

        int rc = tdb_txn_commit_blocking(thd, trx->txn);
        if (rc != TDB_SUCCESS)
        {
            /* Only log truly unexpected errors (not transient conflicts). */
            if (rc != TDB_ERR_CONFLICT && rc != TDB_ERR_LOCKED && rc != TDB_ERR_MEMORY_LIMIT &&
                rc != TDB_ERR_BUSY)
                sql_print_error(
                    "[TIDESDB] hton_commit: tidesdb_txn_commit returned %d "
                    "(dirty=%d gen=%lu)",
                    rc, trx->dirty, (unsigned long)trx->txn_generation);
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->txn_generation++;
            trx->dirty = false;
            trx->stmt_savepoint_active = false;
            row_locks_release_all(trx);
            return tdb_rc_to_ha(rc, "hton_commit");
        }
        /* We keep txn alive for reuse via txn_reset on next use. */
        trx->txn_generation++;
        trx->needs_reset = true;
    }
    else
    {
        /* Read-only transaction -- we rollback, keep alive for reuse. */
        trx->fts_meta_pending.clear();
        trx->fts_meta_dirty = false;
        tidesdb_txn_rollback(trx->txn);
        trx->txn_generation++;
        trx->needs_reset = true;
    }
    trx->dirty = false;
    trx->stmt_savepoint_active = false;
    row_locks_release_all(trx);
    return 0;
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_rollback(THD *thd, bool all)
#else
static int tidesdb_rollback(handlerton *, THD *thd, bool all)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx || !trx->txn) return 0;

    bool is_real_rollback = all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

    if (!is_real_rollback)
    {
        /* Statement-level rollback inside a multi-statement transaction.
           Without per-statement savepoints (see tidesdb_commit note),
           we fall through to full transaction rollback.  This is the
           same behavior as many simple storage engines and is correct --
           OLTP clients retry the entire transaction after any error. */
    }

    if (trx->stmt_savepoint_active)
    {
        tidesdb_txn_release_savepoint(trx->txn, "stmt");
        trx->stmt_savepoint_active = false;
    }

    /* The accumulated FTS meta deltas track the rows being rolled back,
       so discard them along with the txn's other write state. */
    trx->fts_meta_pending.clear();
    trx->fts_meta_dirty = false;

    /* Full rollback -- we keep txn alive for reuse via reset on next use. */
    tidesdb_txn_rollback(trx->txn);
    trx->txn_generation++;
    trx->needs_reset = true;
    trx->dirty = false;
    trx->stmt_savepoint_active = false;
    row_locks_release_all(trx);
    return 0;
}

#if MYSQL_VERSION_ID >= 110800
static int tidesdb_close_connection(THD *thd)
#else
static int tidesdb_close_connection(handlerton *, THD *thd)
#endif
{
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (trx)
    {
        row_locks_release_all(trx);
        if (trx->txn)
        {
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
        }
        delete trx;
        thd_set_ha_data(thd, tidesdb_hton, NULL);
    }
    return 0;
}

/*
  START TRANSACTION WITH CONSISTENT SNAPSHOT callback.
  Eagerly creates a TidesDB transaction so the snapshot sequence number
  is captured now, not lazily at first data access.  Without this, rows
  committed by other connections between START TRANSACTION and the first
  SELECT would be visible.

  Uses the session's isolation level (SET TRANSACTION ISOLATION LEVEL)
  rather than hard-coding REPEATABLE_READ.  Falls back to RR if the
  session is at the default.
*/
#if MYSQL_VERSION_ID >= 110800
static int tidesdb_start_consistent_snapshot(THD *thd)
#else
static int tidesdb_start_consistent_snapshot(handlerton *, THD *thd)
#endif
{
    /* START TRANSACTION WITH CONSISTENT SNAPSHOT explicitly requests a
       point-in-time snapshot.  Always use at least SNAPSHOT isolation
       so the snapshot persists for the entire transaction, regardless of
       the session's default isolation level (e.g. READ_COMMITTED would
       refresh the snapshot on each read, violating CONSISTENT_SNAPSHOT
       semantics). */
    tidesdb_isolation_level_t iso = resolve_effective_isolation(thd, TDB_ISOLATION_REPEATABLE_READ);
    if (iso < TDB_ISOLATION_SNAPSHOT) iso = TDB_ISOLATION_SNAPSHOT;
    tidesdb_trx_t *trx = get_or_create_trx(thd, tidesdb_hton, iso);
    if (!trx) return 1;

    /* We register at both statement and transaction level so the server
       knows TidesDB is participating in this BEGIN block. */
    trans_register_ha(thd, false, tidesdb_hton, 0);
    trans_register_ha(thd, true, tidesdb_hton, 0);
    return 0;
}

/* ******************** SHOW ENGINE TIDESDB STATUS ******************** */

static bool tidesdb_show_status(handlerton *hton, THD *thd, stat_print_fn *print,
                                enum ha_stat_type stat)
{
    if (stat != HA_ENGINE_STATUS) return false;
    if (!tdb_global) return false;

    tidesdb_refresh_status_vars();

    /* Database-level stats */
    tidesdb_db_stats_t db_st;
    memset(&db_st, 0, sizeof(db_st));
    tidesdb_get_db_stats(tdb_global, &db_st);

    /* Cache stats */
    tidesdb_cache_stats_t cache_st;
    memset(&cache_st, 0, sizeof(cache_st));
    tidesdb_get_cache_stats(tdb_global, &cache_st);

    /* Output buffer for SHOW ENGINE TIDESDB STATUS.  8 KiB is enough to
       hold the fixed-format sections plus an optional object-store block
       and the last-conflict line without truncating. */
    static constexpr uint TIDESDB_STATUS_BUF_LEN = 8192;
    char buf[TIDESDB_STATUS_BUF_LEN];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - pos,
                    "================== TidesDB Engine Status ==================\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Data directory: %s\n", tdb_path.c_str());
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Unified memtable: %s\n",
                    srv_unified_memtable ? "ON" : "OFF");
    pos +=
        snprintf(buf + pos, sizeof(buf) - pos, "Column families: %d\n", db_st.num_column_families);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Global sequence: %lu\n",
                    (unsigned long)db_st.global_seq);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Memory ---\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Total system memory: %lu MB\n",
                    (unsigned long)(db_st.total_memory / (1024 * 1024)));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Resolved memory limit: %lu MB\n",
                    (unsigned long)(db_st.resolved_memory_limit / (1024 * 1024)));
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Memory pressure level: %d\n",
                    db_st.memory_pressure_level);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Total memtable bytes: %ld\n",
                    (long)db_st.total_memtable_bytes);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Transaction memory bytes: %ld\n",
                    (long)db_st.txn_memory_bytes);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Storage ---\n");
    pos +=
        snprintf(buf + pos, sizeof(buf) - pos, "Total SSTables: %d\n", db_st.total_sstable_count);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Open SSTable handles: %d\n",
                    db_st.num_open_sstables);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Total data size: %lu bytes\n",
                    (unsigned long)db_st.total_data_size_bytes);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Immutable memtables: %d\n",
                    db_st.total_immutable_count);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Background ---\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Flush pending: %d\n", db_st.flush_pending_count);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Flush queue size: %lu\n",
                    (unsigned long)db_st.flush_queue_size);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Compaction queue size: %lu\n",
                    (unsigned long)db_st.compaction_queue_size);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Block Cache ---\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Enabled: %s\n", cache_st.enabled ? "YES" : "NO");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Entries: %lu\n",
                    (unsigned long)cache_st.total_entries);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Size: %lu bytes\n",
                    (unsigned long)cache_st.total_bytes);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Hits: %lu\n", (unsigned long)cache_st.hits);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Misses: %lu\n", (unsigned long)cache_st.misses);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Hit rate: %.1f%%\n",
                    cache_st.hit_rate * PERCENT_SCALE);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Partitions: %lu\n",
                    (unsigned long)cache_st.num_partitions);

    /* Tombstone observability.  Aggregates are populated by the
       tidesdb_refresh_status_vars call at the top of this function, which
       walks all CFs once. */
    pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Tombstones ---\n");
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Total tombstones: %ld\n",
                    (long)srv_stat_total_tombstones);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Tombstone ratio: %.2f%%\n",
                    srv_stat_tombstone_ratio * PERCENT_SCALE);
    pos += snprintf(buf + pos, sizeof(buf) - pos, "Worst SSTable density: %.2f%% at level %ld\n",
                    srv_stat_max_sst_density * PERCENT_SCALE, (long)srv_stat_max_sst_density_level);

    /* Object store stats */
    if (db_st.object_store_enabled)
    {
        pos += snprintf(buf + pos, sizeof(buf) - pos, "\n--- Object Store ---\n");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Connector: %s\n",
                        db_st.object_store_connector ? db_st.object_store_connector : "unknown");
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Total uploads: %lu\n",
                        (unsigned long)db_st.total_uploads);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Upload failures: %lu\n",
                        (unsigned long)db_st.total_upload_failures);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Upload queue depth: %lu\n",
                        (unsigned long)db_st.upload_queue_depth);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Local cache: %lu / %lu bytes (%d files)\n",
                        (unsigned long)db_st.local_cache_bytes_used,
                        (unsigned long)db_st.local_cache_bytes_max, db_st.local_cache_num_files);
        pos += snprintf(buf + pos, sizeof(buf) - pos, "Replica mode: %s\n",
                        db_st.replica_mode ? "ON" : "OFF");
    }

    /* Last conflict info */
    mysql_mutex_lock(&last_conflict_mutex);
    if (last_conflict_info[0])
        pos +=
            snprintf(buf + pos, sizeof(buf) - pos, "\n--- Conflicts ---\n%s\n", last_conflict_info);
    mysql_mutex_unlock(&last_conflict_mutex);

    static constexpr const char TIDESDB_ENGINE_NAME[] = "TIDESDB";
    static constexpr uint TIDESDB_ENGINE_NAME_LEN = sizeof(TIDESDB_ENGINE_NAME) - 1;
    return print(thd, TIDESDB_ENGINE_NAME, TIDESDB_ENGINE_NAME_LEN, "", 0, buf, (size_t)pos);
}

/* ******************** Schema discovery (object store mode) ******************** */
/*
  The __tidesql_schema column family stores .frm binaries so that replicas
  can discover table definitions via the handlerton discovery API.  On
  local-only mode schema_cf is NULL and all helpers are no-ops.
*/

/*
  Build a schema CF key from db + table LEX_CSTRINGs.
  Format-- "db_name\0table_name" (null byte separator, no trailing null).
*/
static std::string schema_cf_key(const LEX_CSTRING &db, const LEX_CSTRING &tbl)
{
    std::string k;
    k.reserve(db.length + sizeof(SCHEMA_CF_KEY_SEP) + tbl.length);
    k.append(db.str, db.length);
    k.push_back(SCHEMA_CF_KEY_SEP);
    k.append(tbl.str, tbl.length);
    return k;
}

/*
  Build a schema CF key from a MariaDB table path (e.g. "./db/table").
  Extracts the db and table components using the same logic as path_to_cf_name.
*/
static std::string schema_cf_key_from_path(const char *path)
{
    std::string p(path);

    if (p.size() >= MARIADB_REL_PATH_PREFIX_LEN &&
        p.compare(0, MARIADB_REL_PATH_PREFIX_LEN, MARIADB_REL_PATH_PREFIX) == 0)
        p = p.substr(MARIADB_REL_PATH_PREFIX_LEN);

    size_t last_slash = p.rfind('/');
    if (last_slash == std::string::npos)
    {
        /* No slashes -- we treat entire path as table name with empty db */
        std::string k;
        k.push_back(SCHEMA_CF_KEY_SEP);
        k.append(p);
        return k;
    }

    std::string tblname = p.substr(last_slash + 1);

    size_t prev_slash = (last_slash > 0) ? p.rfind('/', last_slash - 1) : std::string::npos;
    std::string dbname;
    if (prev_slash == std::string::npos)
        dbname = p.substr(0, last_slash);
    else
        dbname = p.substr(prev_slash + 1, last_slash - prev_slash - 1);

    std::string k;
    k.reserve(dbname.size() + sizeof(SCHEMA_CF_KEY_SEP) + tblname.size());
    k.append(dbname);
    k.push_back(SCHEMA_CF_KEY_SEP);
    k.append(tblname);
    return k;
}

/*
  Store a .frm image in the schema CF.

  When frm_data/frm_len are provided the image is used directly (this is
  the normal path during CREATE TABLE -- MariaDB skips writing .frm to
  disk when discover_table is registered on the handlerton).

  When frm_data is NULL, the .frm is read from disk (ALTER TABLE path
  where MariaDB writes the updated .frm before calling commit).

  No-op when schema_cf is NULL (local-only mode).
*/
static int schema_cf_store_frm(const char *path, const uchar *frm_data = NULL, size_t frm_len = 0)
{
    /* Replica mode is read-only against the object store.  Even a single
       successful insert into the schema CF lands in the unified memtable
       and gets flushed to a new SSTable when the bootstrap mariadbd
       drains on shutdown, which then triggers a compaction whose
       MANIFEST upload overwrites the primary's authoritative state.
       Refuse all schema writes here so the bucket stays clean. */
    if (srv_replica_mode) return 0;
    if (!schema_cf) return 0;

    uchar *alloc_buf = NULL;

    if (!frm_data)
    {
        char frm_path[FN_REFLEN];
        fn_format(frm_path, path, "", reg_ext, MY_UNPACK_FILENAME | MY_APPEND_EXT);

        MY_STAT st;
        if (!my_stat(frm_path, &st, MYF(0))) return 0; /* .frm not on disk -- not fatal */

        File fd = my_open(frm_path, O_RDONLY, MYF(0));
        if (fd < 0) return 0;

        frm_len = (size_t)st.st_size;
        alloc_buf = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, frm_len, MYF(0));
        if (!alloc_buf)
        {
            my_close(fd, MYF(0));
            return -1;
        }

        if (my_read(fd, alloc_buf, frm_len, MYF(MY_NABP)) != 0)
        {
            my_free(alloc_buf);
            my_close(fd, MYF(0));
            return 0;
        }
        my_close(fd, MYF(0));
        frm_data = alloc_buf;
    }

    std::string key = schema_cf_key_from_path(path);

    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin(tdb_global, &txn);
    if (rc == TDB_SUCCESS)
    {
        rc = tidesdb_txn_put(txn, schema_cf, (const uint8_t *)key.data(), key.size(), frm_data,
                             frm_len, TIDESDB_TTL_NONE);
        if (rc == TDB_SUCCESS)
            rc = tidesdb_txn_commit(txn);
        else
            tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
    }

    if (alloc_buf) my_free(alloc_buf);
    return (rc == TDB_SUCCESS) ? 0 : -1;
}

/*
  Remove a table's .frm entry from the schema CF on DROP TABLE.
*/
static void schema_cf_delete(const char *path)
{
    /* See the rationale in schema_cf_store_frm -- replica writes must not
       reach the unified memtable or the bootstrap mariadbd's shutdown
       drain will flush + compact + upload a MANIFEST that overwrites the
       primary's. */
    if (srv_replica_mode) return;
    if (!schema_cf) return;

    std::string key = schema_cf_key_from_path(path);
    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) == TDB_SUCCESS)
    {
        tidesdb_txn_delete(txn, schema_cf, (const uint8_t *)key.data(), key.size());
        tidesdb_txn_commit(txn);
        tidesdb_txn_free(txn);
    }
}

/*
  Remove every schema CF entry belonging to a dropped database.
  Keys are "db_name\0table_name" so we iterate the CF and delete entries
  whose prefix matches.  No-op in local-only mode (schema_cf is NULL).
*/
static void schema_cf_delete_db(const std::string &db_name)
{
    /* Same rationale as schema_cf_store_frm -- never let a replica land
       writes in the unified memtable. */
    if (srv_replica_mode) return;
    if (!schema_cf || db_name.empty()) return;

    /* Match keys beginning with "db_name<SCHEMA_CF_KEY_SEP>". */
    std::string prefix = db_name;
    prefix.push_back(SCHEMA_CF_KEY_SEP);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    tidesdb_iter_t *it = NULL;
    if (tdb_iter_new_blocking(current_thd, txn, schema_cf, &it) != TDB_SUCCESS)
    {
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return;
    }

    std::vector<std::string> to_delete;
    tidesdb_iter_seek(it, (const uint8_t *)prefix.data(), prefix.size());
    while (tidesdb_iter_valid(it))
    {
        uint8_t *k = NULL;
        size_t klen = 0;
        if (tidesdb_iter_key(it, &k, &klen) != TDB_SUCCESS) break;
        if (klen < prefix.size() || memcmp(k, prefix.data(), prefix.size()) != 0) break;
        to_delete.emplace_back((const char *)k, klen);
        tidesdb_iter_next(it);
    }
    tidesdb_iter_free(it);

    for (const auto &k : to_delete)
        tidesdb_txn_delete(txn, schema_cf, (const uint8_t *)k.data(), k.size());

    if (!to_delete.empty())
        tidesdb_txn_commit(txn);
    else
        tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
}

/*
  Rename a table's schema CF entry (delete old key, insert under new key).
  Called from rename_table().
*/
static void schema_cf_rename(const char *from, const char *to)
{
    /* Same rationale as schema_cf_store_frm -- never let a replica land
       writes in the unified memtable. */
    if (srv_replica_mode) return;
    if (!schema_cf) return;

    std::string old_key = schema_cf_key_from_path(from);
    std::string new_key = schema_cf_key_from_path(to);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    uint8_t *val = NULL;
    size_t val_len = 0;
    int rc = tidesdb_txn_get(txn, schema_cf, (const uint8_t *)old_key.data(), old_key.size(), &val,
                             &val_len);
    if (rc == TDB_SUCCESS && val)
    {
        tidesdb_txn_put(txn, schema_cf, (const uint8_t *)new_key.data(), new_key.size(), val,
                        val_len, TIDESDB_TTL_NONE);
        tidesdb_txn_delete(txn, schema_cf, (const uint8_t *)old_key.data(), old_key.size());
        tidesdb_txn_commit(txn);
        tidesdb_free(val);
    }
    else
    {
        tidesdb_txn_rollback(txn);
        if (val) tidesdb_free(val);

        /* We fallback, old key missing? we read .frm from disk at new path */
        schema_cf_store_frm(to);
    }

    tidesdb_txn_free(txn);
}

static void schema_cf_ensure_databases();

/*
  Handlerton discover_table callback.
  Called when MariaDB cannot find a .frm file on disk for a TidesDB table.
  Reads the .frm binary from the schema CF and initializes the TABLE_SHARE.
*/
static int tidesdb_discover_table(handlerton *, THD *thd, TABLE_SHARE *share)
{
    if (!schema_cf) return HA_ERR_NO_SUCH_TABLE;

    std::string key = schema_cf_key(share->db, share->table_name);

    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin(tdb_global, &txn);
    if (rc != TDB_SUCCESS) return HA_ERR_NO_SUCH_TABLE;

    uint8_t *val = NULL;
    size_t val_len = 0;
    /* Wrap in the backpressure helper so reader-fd starvation or memtable
       backpressure waits instead of immediately reporting "table missing".
       Returning HA_ERR_NO_SUCH_TABLE for a transient BUSY puts MariaDB
       in the discover loop the comment below warns about. */
    rc = tdb_with_backpressure_wait(thd,
                                    [&]()
                                    {
                                        return tidesdb_txn_get(txn, schema_cf,
                                                               (const uint8_t *)key.data(),
                                                               key.size(), &val, &val_len);
                                    });
    tidesdb_txn_rollback(txn); /* read-only, no commit needed */
    tidesdb_txn_free(txn);

    if (rc == TDB_ERR_NOT_FOUND || !val) return HA_ERR_NO_SUCH_TABLE;
    if (rc != TDB_SUCCESS)
    {
        /* IO / corruption / persistent BUSY surfaces as HA_ERR_CRASHED so
           the operator sees the real cause instead of an opaque "table
           not found". */
        if (val) tidesdb_free(val);
        return tdb_rc_to_ha(rc, "tidesdb_discover_table");
    }

    /* We ensure the database directory exists.  The primary may have created
       this database after the replica started, and schema_cf_ensure_databases()
       only runs at plugin init.  A single stat() + conditional mkdir(). */
    {
        char db_dir[FN_REFLEN];
        size_t dh_len = strlen(mysql_real_data_home);
        snprintf(db_dir, sizeof(db_dir), "%s%s%.*s", mysql_real_data_home,
                 (dh_len > 0 && mysql_real_data_home[dh_len - 1] != '/') ? "/" : "",
                 (int)share->db.length, share->db.str);
        MY_STAT st;
        if (!my_stat(db_dir, &st, MYF(0))) my_mkdir(db_dir, TIDESDB_DB_DIR_MODE, MYF(0));
    }

    /* We verify the data CF actually exists before returning the .frm.
       If the .frm is in the schema CF but the data CF hasn't been synced
       yet (e.g. replica hasn't downloaded it from S3), returning the .frm
       would cause handler::open() to fail with HA_ERR_NO_SUCH_TABLE.
       MariaDB then retries discovery in an infinite loop (delete .frm ->
       discover -> write .frm -> open fails -> delete .frm -> ...). */
    {
        std::string cf_name = std::string(share->db.str, share->db.length) + CF_DB_TABLE_SEP +
                              std::string(share->table_name.str, share->table_name.length);
        if (!tidesdb_get_column_family(tdb_global, cf_name.c_str()))
        {
            tidesdb_free(val);
            return HA_ERR_NO_SUCH_TABLE;
        }
    }

    /* We parse .frm binary into TABLE_SHARE.
       write=true causes MariaDB to cache the .frm on disk so subsequent
       opens skip discovery. */
    rc = share->init_from_binary_frm_image(thd, true, val, val_len);

    tidesdb_free(val);
    return rc;
}

/*
  Handlerton discover_table_names callback.
  Lists all TidesDB tables in a given database by scanning the schema CF
  for keys with the matching "db\0" prefix.
*/
static int tidesdb_discover_table_names(handlerton *, const LEX_CSTRING *db, MY_DIR *,
                                        handlerton::discovered_list *result)
{
    if (!schema_cf) return 0;

    /* We ensure database directories are up-to-date.  Picks up databases
       created by the primary after this replica started. */
    schema_cf_ensure_databases();

    std::string prefix;
    prefix.reserve(db->length + sizeof(SCHEMA_CF_KEY_SEP));
    prefix.append(db->str, db->length);
    prefix.push_back(SCHEMA_CF_KEY_SEP);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return 0;

    tidesdb_iter_t *iter = NULL;
    if (tdb_iter_new_blocking(current_thd, txn, schema_cf, &iter) != TDB_SUCCESS || !iter)
    {
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return 0;
    }

    tidesdb_iter_seek(iter, (const uint8_t *)prefix.data(), prefix.size());
    while (tidesdb_iter_valid(iter))
    {
        uint8_t *kp = NULL;
        size_t klen = 0;
        if (tidesdb_iter_key(iter, &kp, &klen) != TDB_SUCCESS || !kp) break;

        if (klen < prefix.size() || memcmp(kp, prefix.data(), prefix.size()) != 0) break;

        /* Table name is everything after the "db\0" prefix */
        const char *tname = (const char *)kp + prefix.size();
        size_t tlen = klen - prefix.size();
        result->add_table(tname, tlen);

        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);
    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    return 0;
}

/*
  Handlerton discover_table_existence callback.
  Returns 1 if the table has an entry in the schema CF, 0 otherwise.
*/
static int tidesdb_discover_table_existence(handlerton *, const char *db, const char *table_name)
{
    if (!schema_cf) return 0;

    /* Ensure database directories are up-to-date for replica discovery. */
    schema_cf_ensure_databases();

    LEX_CSTRING db_lex = {db, strlen(db)};
    LEX_CSTRING tbl_lex = {table_name, strlen(table_name)};
    std::string key = schema_cf_key(db_lex, tbl_lex);

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return 0;

    uint8_t *val = NULL;
    size_t val_len = 0;
    /* Same backpressure rationale as tidesdb_discover_table-- a transient
       BUSY answer here lies to the SQL layer about the table's existence. */
    int rc = tdb_with_backpressure_wait(current_thd,
                                        [&]()
                                        {
                                            return tidesdb_txn_get(txn, schema_cf,
                                                                   (const uint8_t *)key.data(),
                                                                   key.size(), &val, &val_len);
                                        });
    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
    if (val) tidesdb_free(val);

    return (rc == TDB_SUCCESS) ? 1 : 0;
}

/*
  Scan the schema CF for all unique database names and create any missing
  database directories under mysql_real_data_home.  This ensures that
  replicas (which receive table definitions via S3) have the database
  directory present so MariaDB will call discover_table_names for them.
  Without the directory, MariaDB doesn't know the database exists and
  never asks TidesDB about its tables.
*/
static void schema_cf_ensure_databases()
{
    if (!schema_cf) return;

    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    tidesdb_iter_t *iter = NULL;
    if (tdb_iter_new_blocking(current_thd, txn, schema_cf, &iter) != TDB_SUCCESS || !iter)
    {
        tidesdb_txn_rollback(txn);
        tidesdb_txn_free(txn);
        return;
    }

    std::unordered_set<std::string> seen_dbs;

    tidesdb_iter_seek_to_first(iter);
    while (tidesdb_iter_valid(iter))
    {
        uint8_t *kp = NULL;
        size_t klen = 0;
        if (tidesdb_iter_key(iter, &kp, &klen) != TDB_SUCCESS || !kp) break;

        /* Key format-- "db_name<SCHEMA_CF_KEY_SEP>table_name" --
           we find the separator */
        const char *kstr = (const char *)kp;
        size_t sep = 0;
        for (; sep < klen; sep++)
        {
            if (kstr[sep] == SCHEMA_CF_KEY_SEP) break;
        }
        if (sep > 0 && sep < klen)
        {
            std::string dbname(kstr, sep);
            if (seen_dbs.insert(dbname).second)
            {
                char db_dir[FN_REFLEN];
                size_t dh_len = strlen(mysql_real_data_home);
                snprintf(db_dir, sizeof(db_dir), "%s%s%s", mysql_real_data_home,
                         (dh_len > 0 && mysql_real_data_home[dh_len - 1] != '/') ? "/" : "",
                         dbname.c_str());

                MY_STAT st;
                if (!my_stat(db_dir, &st, MYF(0)))
                {
                    if (my_mkdir(db_dir, TIDESDB_DB_DIR_MODE, MYF(0)) == 0)
                        sql_print_information(
                            "[TIDESDB] Created database directory '%s' for schema discovery",
                            dbname.c_str());
                }
            }
        }

        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);
    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
}

/* ******************** Plugin init / deinit ******************** */

static int tidesdb_hton_drop_table(handlerton *, const char *path);
static void tidesdb_hton_drop_database(handlerton *, char *path);
static bool tidesdb_hton_flush_logs(handlerton *);
static int tidesdb_hton_panic(handlerton *, enum ha_panic_function flag);
static void tidesdb_hton_pre_shutdown(void);
static void tidesdb_hton_kill_query(handlerton *, THD *thd, enum thd_kill_levels level);

static int tidesdb_init_func(void *p)
{
    DBUG_ENTER("tidesdb_init_func");

    tidesdb_hton = (handlerton *)p;
    tidesdb_hton->create = tidesdb_create_handler;
    tidesdb_hton->flags = 0;
    tidesdb_hton->savepoint_offset = sizeof(tidesdb_savepoint_t);
    tidesdb_hton->tablefile_extensions = ha_tidesdb_exts;
    tidesdb_hton->table_options = tidesdb_table_option_list;
    tidesdb_hton->field_options = tidesdb_field_option_list;
    tidesdb_hton->index_options = tidesdb_index_option_list;
    tidesdb_hton->drop_table = tidesdb_hton_drop_table;
    tidesdb_hton->drop_database = tidesdb_hton_drop_database;

    /* Handlerton transaction callbacks -- one TidesDB txn per BEGIN..COMMIT */
    tidesdb_hton->commit = tidesdb_commit;
    tidesdb_hton->rollback = tidesdb_rollback;
    tidesdb_hton->close_connection = tidesdb_close_connection;

    tidesdb_hton->savepoint_set = tidesdb_savepoint_set;
    tidesdb_hton->savepoint_rollback = tidesdb_savepoint_rollback;
    tidesdb_hton->savepoint_rollback_can_release_mdl = tidesdb_savepoint_rollback_can_release_mdl;
    tidesdb_hton->savepoint_release = tidesdb_savepoint_release;
    tidesdb_hton->start_consistent_snapshot = tidesdb_start_consistent_snapshot;
    tidesdb_hton->show_status = tidesdb_show_status;

    /* Durability / lifecycle / cancellation hooks. */
    tidesdb_hton->flush_logs = tidesdb_hton_flush_logs;
    tidesdb_hton->panic = tidesdb_hton_panic;
    tidesdb_hton->pre_shutdown = tidesdb_hton_pre_shutdown;
    tidesdb_hton->kill_query = tidesdb_hton_kill_query;

    mysql_mutex_init(0, &last_conflict_mutex, MY_MUTEX_INIT_FAST);

    /* Size the lock table to 8 * hardware threads, clamped into a
       sensible range.  Below the floor the hash collisions hurt; above
       the ceiling we just burn memory without buying contention relief. */
    {
        unsigned int hw = std::thread::hardware_concurrency();
        if (hw == 0) hw = 8;
        ulong desired = (ulong)hw * 8;
        if (desired < ROW_LOCK_PARTITIONS_MIN) desired = ROW_LOCK_PARTITIONS_MIN;
        if (desired > ROW_LOCK_PARTITIONS_MAX) desired = ROW_LOCK_PARTITIONS_MAX;
        row_lock_partitions = desired;
    }

    /* my_malloc returns only malloc-default alignment (8 or 16 bytes), so
       a struct declared alignas(64) can land misaligned in the array and
       any 16-byte SSE store the compiler emits against it segfaults.
       posix_memalign / _aligned_malloc give us the alignment the struct
       actually requires; the matching free in deinit must use the same
       allocator family. */
    {
        size_t sz = (size_t)row_lock_partitions * sizeof(tdb_lock_partition_t);
        void *p = NULL;
#ifdef _WIN32
        p = _aligned_malloc(sz, alignof(tdb_lock_partition_t));
#else
        if (posix_memalign(&p, alignof(tdb_lock_partition_t), sz) != 0) p = NULL;
#endif
        if (p)
        {
            memset(p, 0, sz);
            lock_partitions = (tdb_lock_partition_t *)p;
        }
        else
        {
            lock_partitions = NULL;
        }
    }
    if (lock_partitions)
    {
        for (ulong i = 0; i < row_lock_partitions; i++)
        {
            mysql_mutex_init(0, &lock_partitions[i].mutex, MY_MUTEX_INIT_FAST);
            lock_partitions[i].chain = NULL;
            lock_partitions[i].freelist = NULL;
        }
    }

    /* Initialize FTS stop word set with defaults */
    mysql_rwlock_init(tdb_stopword_lock_key, &tdb_stopword_lock);
    tdb_load_default_stopwords();
    sql_print_information("[TIDESDB] Loaded %zu default stop words", tdb_stopwords.size());

    /* Initialize FTS blend chars */
    mysql_rwlock_init(tdb_blend_lock_key, &tdb_blend_lock);
    tdb_rebuild_blend_map(srv_fts_blend_chars);

    /* We use tidesdb_data_home_dir if set, otherwise compute
       a sibling directory of the MariaDB data directory. */
    if (srv_data_home_dir && srv_data_home_dir[0])
    {
        tdb_path = srv_data_home_dir;
        while (!tdb_path.empty() && tdb_path.back() == '/') tdb_path.pop_back();
    }
    else
    {
        std::string data_home(mysql_real_data_home);
        while (!data_home.empty() && data_home.back() == '/') data_home.pop_back();
        size_t slash_pos = data_home.rfind('/');
        if (slash_pos != std::string::npos)
            tdb_path = data_home.substr(0, slash_pos + 1) + "tidesdb_data";
        else
            tdb_path = "tidesdb_data";
    }

    static const int log_level_map[] = {TDB_LOG_DEBUG, TDB_LOG_INFO,  TDB_LOG_WARN,
                                        TDB_LOG_ERROR, TDB_LOG_FATAL, TDB_LOG_NONE};

    tidesdb_config_t cfg = tidesdb_default_config();
    cfg.db_path = const_cast<char *>(tdb_path.c_str());
    cfg.num_flush_threads = (int)srv_flush_threads;
    cfg.num_compaction_threads = (int)srv_compaction_threads;
    cfg.log_level = (tidesdb_log_level_t)log_level_map[srv_log_level];
    /* The library caps concurrent flushes by config.max_concurrent_flushes
       (default 4 in the library), independent of num_flush_threads, so
       leaving the cap below the worker count would silently idle workers.
       Default tidesdb_max_concurrent_flushes=0 means align the cap with
       tidesdb_flush_threads so every worker can run.  A non-zero user
       value is honoured but warned when it leaves workers idle. */
    if (srv_max_concurrent_flushes == 0)
    {
        cfg.max_concurrent_flushes = (int)srv_flush_threads;
    }
    else
    {
        cfg.max_concurrent_flushes = (int)srv_max_concurrent_flushes;
        if (srv_max_concurrent_flushes < srv_flush_threads)
            sql_print_warning(
                "[TIDESDB] tidesdb_max_concurrent_flushes=%lu is lower than "
                "tidesdb_flush_threads=%lu, %lu flush worker(s) will remain idle.  "
                "Raise tidesdb_max_concurrent_flushes to at least %lu (or leave it "
                "at 0 to align automatically) to use every configured worker",
                srv_max_concurrent_flushes, srv_flush_threads,
                srv_flush_threads - srv_max_concurrent_flushes, srv_flush_threads);
    }
    cfg.block_cache_size = (size_t)srv_block_cache_size;
    cfg.max_open_sstables = (int)srv_max_open_sstables;
    cfg.log_to_file = srv_log_to_file ? 1 : 0;
    cfg.log_truncation_at = (size_t)srv_log_truncation_at;
    cfg.max_memory_usage = (size_t)srv_max_memory_usage;
    cfg.unified_memtable = srv_unified_memtable ? 1 : 0;
    cfg.unified_memtable_write_buffer_size = (size_t)srv_unified_memtable_write_buffer_size;
    cfg.unified_memtable_sync_mode = tdb_sync_mode_map[srv_unified_memtable_sync_mode];
    cfg.unified_memtable_sync_interval_us = (uint64_t)srv_unified_memtable_sync_interval;
    cfg.unified_memtable_skip_list_max_level = (int)srv_unified_memtable_skip_list_max_level;
    cfg.unified_memtable_skip_list_probability = (float)srv_unified_memtable_skip_list_probability;

    /* Object store connector setup */
    tidesdb_objstore_t *objstore_connector = NULL;
    static tidesdb_objstore_config_t objstore_cfg;

    if (srv_object_store_backend == OBJSTORE_BACKEND_S3)
    {
#ifdef TIDESDB_WITH_S3
        if (!srv_s3_endpoint || !srv_s3_bucket || !srv_s3_access_key || !srv_s3_secret_key)
        {
            sql_print_error(
                "[TIDESDB] S3 backend requires s3_endpoint, s3_bucket, "
                "s3_access_key, and s3_secret_key");
            DBUG_RETURN(1);
        }

        /* Modern config-struct entry exposes the TLS + multipart knobs the
           legacy positional create cannot.  Zero-initialize then fill so any
           field added by the library in the future stays at its
           secure-default value until the plugin surfaces it. */
        tidesdb_objstore_s3_config_t s3cfg;
        memset(&s3cfg, 0, sizeof(s3cfg));
        s3cfg.endpoint = srv_s3_endpoint;
        s3cfg.bucket = srv_s3_bucket;
        s3cfg.prefix = srv_s3_prefix;
        s3cfg.access_key = srv_s3_access_key;
        s3cfg.secret_key = srv_s3_secret_key;
        s3cfg.region = srv_s3_region;
        s3cfg.use_ssl = srv_s3_use_ssl ? 1 : 0;
        s3cfg.use_path_style = srv_s3_path_style ? 1 : 0;
        s3cfg.tls_ca_path =
            (srv_s3_tls_ca_path && srv_s3_tls_ca_path[0]) ? srv_s3_tls_ca_path : NULL;
        s3cfg.tls_insecure_skip_verify = srv_s3_tls_insecure_skip_verify ? 1 : 0;
        s3cfg.multipart_threshold = (size_t)srv_s3_multipart_threshold;
        s3cfg.multipart_part_size = (size_t)srv_s3_multipart_part_size;

        if (s3cfg.tls_insecure_skip_verify)
        {
            sql_print_warning(
                "[TIDESDB] s3_tls_insecure_skip_verify is ON; the S3 endpoint's "
                "TLS certificate is not validated. Use only for trusted test endpoints.");
        }

        objstore_connector = tidesdb_objstore_s3_create_config(&s3cfg);

        if (!objstore_connector)
        {
            sql_print_error("[TIDESDB] Failed to create S3 connector for %s/%s", srv_s3_endpoint,
                            srv_s3_bucket);
            DBUG_RETURN(1);
        }

        sql_print_information("[TIDESDB] S3 connector created (endpoint=%s, bucket=%s, ssl=%s)",
                              srv_s3_endpoint, srv_s3_bucket, srv_s3_use_ssl ? "yes" : "no");
#else
        sql_print_error(
            "[TIDESDB] S3 backend requested but TidesDB was not built with "
            "-DTIDESDB_WITH_S3=ON");
        DBUG_RETURN(1);
#endif
    }

    if (objstore_connector)
    {
        objstore_cfg = tidesdb_objstore_default_config();
        objstore_cfg.local_cache_max_bytes = (size_t)srv_objstore_local_cache_max;
        objstore_cfg.wal_sync_threshold_bytes = (size_t)srv_objstore_wal_sync_threshold;
        objstore_cfg.wal_sync_on_commit = srv_objstore_wal_sync_on_commit ? 1 : 0;
        objstore_cfg.cache_on_read = srv_objstore_cache_on_read ? 1 : 0;
        objstore_cfg.cache_on_write = srv_objstore_cache_on_write ? 1 : 0;
        if (srv_objstore_max_concurrent_uploads > 0)
            objstore_cfg.max_concurrent_uploads = (int)srv_objstore_max_concurrent_uploads;
        if (srv_objstore_max_concurrent_downloads > 0)
            objstore_cfg.max_concurrent_downloads = (int)srv_objstore_max_concurrent_downloads;
        if (srv_objstore_multipart_threshold > 0)
            objstore_cfg.multipart_threshold = (size_t)srv_objstore_multipart_threshold;
        if (srv_objstore_multipart_part_size > 0)
            objstore_cfg.multipart_part_size = (size_t)srv_objstore_multipart_part_size;
        objstore_cfg.sync_manifest_to_object = srv_objstore_sync_manifest_to_object ? 1 : 0;
        objstore_cfg.wal_upload_sync = srv_objstore_wal_upload_sync ? 1 : 0;
        objstore_cfg.replicate_wal = srv_objstore_replicate_wal ? 1 : 0;
        objstore_cfg.replica_mode = srv_replica_mode ? 1 : 0;
        objstore_cfg.replica_sync_interval_us = (uint64_t)srv_replica_sync_interval;
        objstore_cfg.replica_replay_wal = srv_objstore_replica_replay_wal ? 1 : 0;

        cfg.object_store = objstore_connector;
        cfg.object_store_config = &objstore_cfg;
    }

    int rc = tidesdb_open(&cfg, &tdb_global);
    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] Failed to open TidesDB at %s (err=%d)", tdb_path.c_str(), rc);
        DBUG_RETURN(1);
    }

    sql_print_information("[TIDESDB] TidesDB opened at %s", tdb_path.c_str());

    /* Schema discovery CF -- created when object store is active so that
       replicas can discover table definitions from the shared storage. */
    if (objstore_connector)
    {
        tidesdb_column_family_config_t schema_cfg = tidesdb_default_column_family_config();
        if (!tidesdb_get_column_family(tdb_global, SCHEMA_CF_NAME))
            tidesdb_create_column_family(tdb_global, SCHEMA_CF_NAME, &schema_cfg);

        schema_cf = tidesdb_get_column_family(tdb_global, SCHEMA_CF_NAME);

        if (schema_cf)
        {
            tidesdb_hton->discover_table = tidesdb_discover_table;
            tidesdb_hton->discover_table_names = tidesdb_discover_table_names;
            tidesdb_hton->discover_table_existence = tidesdb_discover_table_existence;

            /* We ensure database directories exist for all tables in the schema
               CF so MariaDB discovers them (relevant for replicas). */
            schema_cf_ensure_databases();

            sql_print_information("[TIDESDB] Schema discovery enabled (object store mode)");
        }
    }

    DBUG_RETURN(0);
}

/*
  Handlerton-level FLUSH LOGS callback.  Called on FLUSH LOGS and by
  mariadb-backup before copying files so the on-disk WAL is a consistent
  snapshot.  With unified-memtable mode one sync covers all CFs.  In
  per-CF mode we sync the schema CF (always present in object-store
  mode; otherwise we try the first registered CF).  Returns false on
  success (handlerton convention).
*/
static bool tidesdb_hton_flush_logs(handlerton *)
{
    if (!tdb_global) return false;

    tidesdb_column_family_t *target = schema_cf;
    if (!target)
    {
        char **names = NULL;
        int count = 0;
        if (tidesdb_list_column_families(tdb_global, &names, &count) == TDB_SUCCESS && names)
        {
            if (count > 0 && names[0]) target = tidesdb_get_column_family(tdb_global, names[0]);
            for (int i = 0; i < count; i++)
                if (names[i]) tidesdb_free(names[i]);
            tidesdb_free(names);
        }
    }
    if (!target) return false; /* empty database -- nothing to sync */

    int rc = tidesdb_sync_wal(target);
    if (rc != TDB_SUCCESS)
    {
        sql_print_warning("[TIDESDB] flush_logs: tidesdb_sync_wal failed (rc=%d)", rc);
        return true; /* error */
    }
    return false;
}

/*
  Handlerton-level panic callback.  MariaDB calls this on signal-driven or
  abnormal shutdown paths where tidesdb_deinit_func may not run.  We only
  react to HA_PANIC_CLOSE -- the other flags are legacy ISAM-era.
*/
static int tidesdb_hton_panic(handlerton *, enum ha_panic_function flag)
{
    if (flag != HA_PANIC_CLOSE) return 0;
    if (tdb_global)
    {
        tidesdb_close(tdb_global);
        tdb_global = NULL;
        schema_cf = NULL;
    }
    return 0;
}

/*
  Handlerton-level pre_shutdown callback.  Runs before the deinit path so
  background threads that still need a fully-functional server (compaction,
  flush) get a clean signal to drain.  We flush the unified WAL synchronously
  and let tidesdb_close() in deinit finish the teardown.
*/
static void tidesdb_hton_pre_shutdown(void)
{
    if (!tdb_global) return;

    /* Sync the unified WAL so durability is preserved if deinit is racing
       a forced exit.  The call is cheap when there's nothing to sync. */
    (void)tidesdb_hton_flush_logs(tidesdb_hton);
}

/*
  Handlerton-level kill_query callback.  MariaDB calls this on KILL QUERY
  and on connection shutdown.  When the victim is blocked in
  row_lock_acquire we wake it by broadcasting on the lock entry's cond,
  and the wait loop sees thd_killed() on the next pass and bails out.
  Spurious wake-ups are harmless because the wait loop re-checks
  req->granted before exiting.

  trx->waiting_on_lock points directly at the lock entry, which is never
  freed at runtime, so dereferencing it here is always safe.
*/
static void tidesdb_hton_kill_query(handlerton *, THD *thd, enum thd_kill_levels)
{
    if (!thd) return;
    tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, tidesdb_hton);
    if (!trx) return;

    tdb_row_lock_t *wait = trx->waiting_on_lock.load(std::memory_order_acquire);
    if (!wait) return;

    /* We broadcast under the owning partition's mutex so the wake-up is
       serialized against the holder's release path.  Partition index is
       cached on the lock entry so we don't have to recompute the hash. */
    if (lock_partitions && wait->partition < row_lock_partitions)
    {
        tdb_lock_partition_t *part = &lock_partitions[wait->partition];
        mysql_mutex_lock(&part->mutex);
        mysql_cond_broadcast(&wait->cond);
        mysql_mutex_unlock(&part->mutex);
    }
}

static int tidesdb_deinit_func(void *p)
{
    DBUG_ENTER("tidesdb_deinit_func");

    schema_cf = NULL;

    if (tdb_global)
    {
        /* Opt-in fast-shutdown: cancel in-flight compactions and refuse new
           background work so tidesdb_close does not block for minutes on a
           multi-GB compaction backlog.  Uncommitted compaction output is
           discarded (inputs intact -- recovery is safe), but a mid-compaction
           cancel can leave the object-store side with referenced-but-orphan
           SSTables that confuse a syncing replica, so this is OFF by default
           and tidesdb_close drains naturally. */
        if (srv_fast_shutdown)
        {
            int crc = tidesdb_cancel_background_work(tdb_global);
            if (crc != TDB_SUCCESS)
                sql_print_warning(
                    "[TIDESDB] tidesdb_cancel_background_work returned rc=%d at "
                    "shutdown; tidesdb_close may block waiting for in-flight work",
                    crc);
        }
        tidesdb_close(tdb_global);
        tdb_global = NULL;
    }

    mysql_mutex_destroy(&last_conflict_mutex);
    mysql_rwlock_destroy(&tdb_stopword_lock);
    mysql_rwlock_destroy(&tdb_blend_lock);
    tdb_stopwords.clear();

    if (lock_partitions)
    {
        for (ulong i = 0; i < row_lock_partitions; i++)
        {
            /* Free everything on the active chain and the freelist; both
               lists thread through hash_next and the freelist holds slots
               that were unlinked from the chain at release time. */
            for (tdb_row_lock_t *e = lock_partitions[i].chain; e;)
            {
                tdb_row_lock_t *next = e->hash_next;
                mysql_cond_destroy(&e->cond);
                my_free(e->pk);
                my_free(e);
                e = next;
            }
            for (tdb_row_lock_t *e = lock_partitions[i].freelist; e;)
            {
                tdb_row_lock_t *next = e->hash_next;
                mysql_cond_destroy(&e->cond);
                my_free(e->pk);
                my_free(e);
                e = next;
            }
            mysql_mutex_destroy(&lock_partitions[i].mutex);
        }
        /* Allocated via posix_memalign / _aligned_malloc in tidesdb_init_func;
           pair with the matching free. */
#ifdef _WIN32
        _aligned_free(lock_partitions);
#else
        free(lock_partitions);
#endif
        lock_partitions = NULL;
    }

    sql_print_information("[TIDESDB] TidesDB closed");
    DBUG_RETURN(0);
}

/* ******************** path_to_cf_name ******************** */

std::string ha_tidesdb::path_to_cf_name(const char *path)
{
    std::string p(path);

    if (p.size() >= MARIADB_REL_PATH_PREFIX_LEN &&
        p.compare(0, MARIADB_REL_PATH_PREFIX_LEN, MARIADB_REL_PATH_PREFIX) == 0)
        p = p.substr(MARIADB_REL_PATH_PREFIX_LEN);

    size_t last_slash = p.rfind('/');
    if (last_slash == std::string::npos) return p;

    std::string tblname = p.substr(last_slash + 1);

    size_t prev_slash = (last_slash > 0) ? p.rfind('/', last_slash - 1) : std::string::npos;
    std::string dbname;
    if (prev_slash == std::string::npos)
        dbname = p.substr(0, last_slash);
    else
        dbname = p.substr(prev_slash + 1, last_slash - prev_slash - 1);

    std::string result = dbname + CF_DB_TABLE_SEP + tblname;

    /* MariaDB temp table names embed '#'; substitute so the CF name
       remains a valid identifier in the underlying TidesDB layer. */
    for (size_t i = 0; i < result.size(); i++)
        if (result[i] == MARIADB_TEMP_NAME_MARKER) result[i] = MARIADB_TEMP_NAME_REPLACEMENT;

    return result;
}

/* ******************** Factory / Constructor ******************** */

static handler *tidesdb_create_handler(handlerton *hton, TABLE_SHARE *table, MEM_ROOT *mem_root)
{
    return new (mem_root) ha_tidesdb(hton, table);
}

ha_tidesdb::ha_tidesdb(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg),
      share(NULL),
      stmt_txn(NULL),
      stmt_txn_dirty(false),
      scan_txn(NULL),
      scan_iter(NULL),
      scan_cf_(NULL),
      scan_iter_cf_(NULL),
      scan_iter_txn_(NULL),
      scan_iter_txn_gen_(0),
      idx_pk_exact_done_(false),
      scan_dir_(DIR_NONE),
      current_pk_len_(0),
      idx_search_comp_len_(0),
      dup_iter_count_(0),
      cached_enc_key_ver_(0),
      enc_key_ver_valid_(false),
      cached_time_(0),
      cached_time_valid_(false),
      cached_sess_ttl_(0),
      cached_skip_unique_(false),
      cached_single_delete_primary_(false),
      cached_thdvars_valid_(false),
      stmt_has_write_lock_(false),
      is_pk_(false),
      scan_iter_last_err_(0),
      scan_iter_last_err_cf_(NULL),
      scan_iter_last_err_txn_(NULL),
      has_blobs_(false),
      encrypted_(false),
      record1_lo_(NULL),
      record1_hi_(NULL),
      cached_sql_cmd_(0),
      cached_is_autocommit_(false),
      cached_stmt_shape_valid_(false),
      cached_thd_(NULL),
      cached_trx_(NULL),
      in_bulk_insert_(false),
      in_bulk_update_(false),
      in_bulk_delete_(false),
      bulk_insert_ops_(0),
      cached_compact_after_range_delete_min_rows_(0),
      bulk_delete_rows_(0),
      mrr_custom_active_(false),
      mrr_no_assoc_(false),
      mrr_keyno_(MAX_KEY),
      mrr_next_idx_(0),
      keyread_only_(false),
      write_can_replace_(false)
{
    memset(dup_iter_cache_, 0, sizeof(dup_iter_cache_));
    memset(dup_iter_txn_, 0, sizeof(dup_iter_txn_));
    memset(dup_iter_txn_gen_, 0, sizeof(dup_iter_txn_gen_));
}

/* ******************** free_dup_iter_cache ******************** */

void ha_tidesdb::free_dup_iter_cache()
{
    for (uint i = 0; i < MAX_KEY; i++)
    {
        if (dup_iter_cache_[i])
        {
            tidesdb_iter_free(dup_iter_cache_[i]);
            dup_iter_cache_[i] = NULL;
            dup_iter_txn_[i] = NULL;
            dup_iter_txn_gen_[i] = 0;
        }
    }
    dup_iter_count_ = 0;
}

/* ******************** get_share ******************** */

TidesDB_share *ha_tidesdb::get_share()
{
    TidesDB_share *tmp_share;
    DBUG_ENTER("ha_tidesdb::get_share");

    lock_shared_ha_data();
    if (!(tmp_share = static_cast<TidesDB_share *>(get_ha_share_ptr())))
    {
        tmp_share = new TidesDB_share;
        if (!tmp_share) goto err;
        set_ha_share_ptr(static_cast<Handler_share *>(tmp_share));
    }
err:
    unlock_shared_ha_data();
    DBUG_RETURN(tmp_share);
}

/* ******************** PK / Index key helpers ******************** */

/*
  We build memcmp-comparable key bytes from record fields for a given KEY.
  Uses Field::make_sort_key_part() so that big-endian, sign-bit-flipped encoding
  is produced for numeric types -- which sorts correctly under memcmp.

  The record may point to record[0] or record[1]; we adjust field pointers
  via move_field_offset to read from the correct buffer.
*/
uint ha_tidesdb::make_comparable_key(KEY *key_info, const uchar *record, uint num_parts, uchar *out)
{
    uint pos = 0;
    my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(record - table->record[0]);

    for (uint p = 0; p < num_parts && p < key_info->user_defined_key_parts; p++)
    {
        KEY_PART_INFO *kp = &key_info->key_part[p];
        Field *field = kp->field;

        /* We handle the null indicator ourselves using real_maybe_null()
           (which checks field-level nullability only) instead of relying on
           make_sort_key_part() which uses maybe_null() (includes
           table->maybe_null).  For inner tables of outer joins,
           table->maybe_null is true, causing make_sort_key_part to write
           a spurious null indicator byte even for NOT NULL PK fields.
           Using make_sort_key() directly avoids this mismatch. */
        field->move_field_offset(ptrdiff);
        if (field->real_maybe_null())
        {
            if (field->is_null())
            {
                out[pos++] = SORT_KEY_NULL;
                bzero(out + pos, kp->length);
                pos += kp->length;
                field->move_field_offset(-ptrdiff);
                continue;
            }
            out[pos++] = SORT_KEY_NOT_NULL;
        }
        /* For VARBINARY (binary charset variable-length fields), sort_string()
           stores the value length in the last length_bytes of the output,
           truncating trailing data bytes when the value fills the field.
           This causes false duplicate detection on UNIQUE indexes because
           different values produce identical sort keys.
           Thus for binary charset varstrings, write all data bytes zero-padded
           followed by the length, so the full value is preserved. */
        if (field->type() == MYSQL_TYPE_VARCHAR && field->charset() == &my_charset_bin)
        {
            Field_varstring *fvs = static_cast<Field_varstring *>(field);
            String buf;
            fvs->val_str(&buf, &buf);
            uint data_len = (uint)buf.length();
            uint len_bytes = fvs->length_bytes;
            uint data_space = kp->length - len_bytes;

            uint copy_len = MY_MIN(data_len, data_space);
            memcpy(out + pos, buf.ptr(), copy_len);
            if (copy_len < data_space) bzero(out + pos + copy_len, data_space - copy_len);
            pos += data_space;

            /* For values that overflow data_space (value is exactly field_length
               bytes), write the overflow bytes into the length area first */
            if (data_len > data_space)
            {
                uint overflow = MY_MIN(data_len - data_space, len_bytes);
                memcpy(out + pos, buf.ptr() + data_space, overflow);
                pos += len_bytes;
            }
            else
            {
                /* Length suffix in high-byte order (preserves sort order) */
                if (len_bytes == 1)
                    out[pos] = (uchar)data_len;
                else
                    mi_int2store(out + pos, data_len);
                pos += len_bytes;
            }

            field->move_field_offset(-ptrdiff);
            continue;
        }

        field->sort_string(out + pos, kp->length);
        field->move_field_offset(-ptrdiff);
        pos += kp->length;
    }

    return pos;
}

/*
  Convert a key_copy-format search key (as passed to index_read_map)
  into the comparable format that we store in TidesDB.
  Uses key_restore to unpack into record[1], then make_comparable_key.
*/
uint ha_tidesdb::key_copy_to_comparable(KEY *key_info, const uchar *key_buf, uint key_len,
                                        uchar *out)
{
    key_restore(table->record[1], key_buf, key_info, key_len);

    uint parts = 0;
    uint len = 0;
    for (parts = 0; parts < key_info->user_defined_key_parts; parts++)
    {
        uint part_len = key_info->key_part[parts].store_length;
        if (len + part_len > key_len) break;
        len += part_len;
    }
    if (parts == 0) parts = 1;

    return make_comparable_key(key_info, table->record[1], parts, out);
}

/*
  Build PK bytes from a record.
  -- With user PK     use make_comparable_key for memcmp-correct ordering.
  -- Without PK       not applicable for NEW rows (caller generates hidden id);
                      for EXISTING rows current_pk already holds the key.
*/
uint ha_tidesdb::pk_from_record(const uchar *record, uchar *out)
{
    if (share->has_user_pk)
    {
        return make_comparable_key(&table->key_info[share->pk_index], record,
                                   table->key_info[share->pk_index].user_defined_key_parts, out);
    }
    else
    {
        /* Hidden PK -- we copy current_pk (must have been set by a prior read) */
        memcpy(out, current_pk_buf_, current_pk_len_);
        return current_pk_len_;
    }
}

/*
  Compute the comparable key byte length for a KEY.
  Matches what make_comparable_key() actually produces:
    sum of (nullable ? 1 : 0) + kp->length for each key part.

  NOTE -- ki->key_length includes store_length overhead (e.g. 2 bytes
  per VARCHAR part for length prefix in key_copy format) which is
  not present in the comparable key output.
*/
static uint comparable_key_length(const KEY *ki)
{
    /* Spatial indexes use a fixed 8-byte Hilbert value as the comparable key.. */
    if (is_spatial_index(ki)) return SPATIAL_HILBERT_KEY_LEN;

    uint len = 0;
    for (uint p = 0; p < ki->user_defined_key_parts; p++)
    {
        if (ki->key_part[p].field->real_maybe_null()) len++;
        len += ki->key_part[p].length;
    }
    return len;
}

/*
  Build a secondary index CF entry key:
    [comparable index-column bytes] + [comparable PK bytes]
*/
uint ha_tidesdb::sec_idx_key(uint idx, const uchar *record, uchar *out)
{
    KEY *key_info = &table->key_info[idx];
    uint pos = make_comparable_key(key_info, record, key_info->user_defined_key_parts, out);
    pos += pk_from_record(record, out + pos);
    return pos;
}

/*
  Try to fill record buf with column values decoded from the secondary
  index key, avoiding the expensive PK point-lookup.  Used when
  keyread_only_ is true (covering index scan).

  The secondary index key layout is:
    [comparable_idx_cols | comparable_pk]

  Uses decode_sort_key_part() which supports integers, DATE, DATETIME,
  TIMESTAMP, YEAR, and fixed-length CHAR/BINARY (binary/latin1).
  Returns true on success.
*/
bool ha_tidesdb::try_keyread_from_index(const uint8_t *ik, size_t iks, uint idx, uchar *buf)
{
    if (!share->has_user_pk) return false;

    KEY *pk_key = &table->key_info[share->pk_index];
    KEY *idx_key = &table->key_info[idx];
    uint idx_col_len = share->idx_comp_key_len[idx];

    /* We check every column in read_set against the precomputed coverage
       bitmap for this index.  O(read_set set-bits) instead of the prior
       O(set-bits * (pk_parts + idx_parts)) nested scan. */
    if (idx < share->idx_cover.size())
    {
        const std::vector<bool> &cover = share->idx_cover[idx];
        for (uint c = bitmap_get_first_set(table->read_set); c != MY_BIT_NONE;
             c = bitmap_get_next_set(table->read_set, c))
        {
            if (c >= cover.size() || !cover[c]) return false;
        }
    }
    else
    {
        /* Share not populated for this index (shouldn't happen). */
        return false;
    }

    const uint8_t *pos = ik;
    for (uint p = 0; p < idx_key->user_defined_key_parts; p++)
    {
        KEY_PART_INFO *kp = &idx_key->key_part[p];
        Field *f = kp->field;
        if (f->real_maybe_null())
        {
            if (pos >= ik + iks) return false;
            if (*pos == 0)
            {
                f->set_null();
                pos++;
                continue;
            }
            f->set_notnull();
            pos++;
        }
        if (pos + kp->length > ik + iks) return false;
        if (bitmap_is_set(table->read_set, kp->fieldnr - 1))
        {
            if (!decode_sort_key_part(pos, kp->length, f, buf)) return false;
        }
        pos += kp->length;
    }

    const uint8_t *pk_start = ik + idx_col_len;
    pos = pk_start;
    for (uint p = 0; p < pk_key->user_defined_key_parts; p++)
    {
        KEY_PART_INFO *kp = &pk_key->key_part[p];
        Field *f = kp->field;
        if (f->real_maybe_null())
        {
            if (pos >= ik + iks) return false;
            if (*pos == 0)
            {
                f->set_null();
                pos++;
                continue;
            }
            f->set_notnull();
            pos++;
        }
        if (pos + kp->length > ik + iks) return false;
        if (bitmap_is_set(table->read_set, kp->fieldnr - 1))
        {
            if (!decode_sort_key_part(pos, kp->length, f, buf)) return false;
        }
        pos += kp->length;
    }

    uint pk_bytes = (uint)(iks - idx_col_len);
    memcpy(current_pk_buf_, pk_start, pk_bytes);
    current_pk_len_ = pk_bytes;

    return true;
}

/* ******************** ICP (Index Condition Pushdown) helpers ******************** */

/*
  Reverse a single integer sort-key part (big-endian, sign-bit-flipped)
  back to native little-endian at `to`.  Caller precomputes `to` so we
  don't re-walk f->ptr/f->table->record[0] on every decode.

  MariaDB integer pack widths are TINY=1, SHORT=2, INT24=3, LONG=4,
  LONGLONG=8 -- any other width is rejected.  The decode is a plain
  byte-reverse, with the most-significant byte XORed with the sign
  flip mask for signed types so the original native value is recovered.
*/
bool ha_tidesdb::decode_int_sort_key(const uint8_t *src, uint sort_len, bool is_signed, uchar *to)
{
    if (sort_len == 0 || (sort_len > 4 && sort_len != 8)) return false;

    for (uint i = 0; i < sort_len; i++) to[i] = src[sort_len - 1 - i];
    if (is_signed) to[sort_len - 1] ^= INT_SORT_SIGN_FLIP_MASK;
    return true;
}

/*
  Extended sort-key decoder -- handles integers (via decode_int_sort_key),
  DATE (3 bytes big-endian), DATETIME/TIMESTAMP (4-8 bytes big-endian),
  YEAR (1 byte), and fixed-length CHAR/BINARY (direct memcpy of sort key).

  For integer types, delegates to decode_int_sort_key which handles the
  sign-bit-flip + endian reversal.

  For DATE/DATETIME/TIMESTAMP/YEAR, the sort key is big-endian unsigned;
  we reverse the byte order to native little-endian without sign-flip
  (these types are always unsigned internally).

  For CHAR/BINARY (MYSQL_TYPE_STRING), the sort key produced by
  Field_string::sort_string is the charset's sort weight sequence.
  For binary/latin1 charsets this is identical to the field content
  (padded with spaces to kp->length).  We copy it directly.
  For multi-byte charsets (utf8) the sort weights differ from the
  stored bytes, so we cannot reverse -- return false.

  Returns true on success, false for unsupported types.
*/
bool ha_tidesdb::decode_sort_key_part(const uint8_t *src, uint sort_len, Field *f, uchar *buf)
{
    /* Compute the destination pointer exactly once per call.  Every branch
       below wrote `buf + (f->ptr - f->table->record[0])` independently. */
    uchar *to = buf + (uintptr_t)(f->ptr - f->table->record[0]);

    switch (f->real_type())
    {
        case MYSQL_TYPE_TINY:
        case MYSQL_TYPE_SHORT:
        case MYSQL_TYPE_INT24:
        case MYSQL_TYPE_LONG:
        case MYSQL_TYPE_LONGLONG:
            return decode_int_sort_key(src, sort_len, !f->is_unsigned(), to);

        case MYSQL_TYPE_YEAR:
            /* YEAR is 1 byte unsigned, sort key is identity */
            to[0] = src[0];
            return true;

        case MYSQL_TYPE_DATE:
        case MYSQL_TYPE_NEWDATE:
            /* DATE is DATE_PACK_LEN bytes, sort key is big-endian unsigned.
               Reverse to native little-endian. */
            if (sort_len == DATE_PACK_LEN)
            {
                for (uint b = 0; b < sort_len; b++) to[b] = src[sort_len - 1 - b];
                return true;
            }
            return false;

        case MYSQL_TYPE_DATETIME:
        case MYSQL_TYPE_DATETIME2:
        case MYSQL_TYPE_TIMESTAMP:
        case MYSQL_TYPE_TIMESTAMP2:
            /* DATETIME/TIMESTAMP sort keys are big-endian unsigned, at most
               DATETIME_MAX_PACK_LEN bytes.  Reverse to native little-endian. */
            if (sort_len <= DATETIME_MAX_PACK_LEN)
            {
                for (uint b = 0; b < sort_len; b++) to[b] = src[sort_len - 1 - b];
                return true;
            }
            return false;

        case MYSQL_TYPE_STRING:
            /* Fixed-length CHAR/BINARY.  For binary/latin1 charsets the
               sort key is identical to the stored content (space-padded).
               For multi-byte charsets we cannot reverse. */
            if (f->charset() == &my_charset_bin || f->charset() == &my_charset_latin1)
            {
                uint flen = f->pack_length();
                uint copy_len = (sort_len < flen) ? sort_len : flen;
                memcpy(to, src, copy_len);
                if (copy_len < flen) memset(to + copy_len, ' ', flen - copy_len);
                return true;
            }
            return false;

        default:
            return false;
    }
}

/*
  Evaluate pushed index condition on a secondary-index entry before
  the expensive PK point-lookup (InnoDB pattern).

  Decodes the index key column values and PK column values from the
  comparable-format index key into the record buffer, then calls
  handler_index_cond_check() which evaluates the pushed condition,
  checks end_range, and handles THD kill signals.

  Supports integer types, DATE, DATETIME, TIMESTAMP, YEAR, and
  fixed-length CHAR/BINARY (binary/latin1 charset) via
  decode_sort_key_part().  For unsupported types, ICP is skipped and
  CHECK_POS is returned so the caller falls through to the PK lookup.
*/
check_result_t ha_tidesdb::icp_check_secondary(const uint8_t *ik, size_t iks, uint idx, uchar *buf)
{
    if (!pushed_idx_cond || pushed_idx_cond_keyno != idx) return CHECK_POS;

    KEY *idx_key = &table->key_info[idx];
    uint idx_col_len = share->idx_comp_key_len[idx];
    bool decode_ok = true;

    /* Decode index column parts from the comparable-format key.
       If any part can't be decoded (DECIMAL, VARCHAR, etc.), we fall
       back to a full PK row fetch so the condition evaluates correctly. */
    const uint8_t *pos = ik;
    for (uint p = 0; p < idx_key->user_defined_key_parts && decode_ok; p++)
    {
        KEY_PART_INFO *kp = &idx_key->key_part[p];
        Field *f = kp->field;

        if (f->real_maybe_null())
        {
            if (pos >= ik + iks)
            {
                decode_ok = false;
                break;
            }
            if (*pos == 0)
            {
                f->set_null();
                pos++;
                continue;
            }
            f->set_notnull();
            pos++;
        }
        if (pos + kp->length > ik + iks)
        {
            decode_ok = false;
            break;
        }
        if (!decode_sort_key_part(pos, kp->length, f, buf)) decode_ok = false;
        pos += kp->length;
    }

    /* Decode PK parts from the tail (pushed condition may reference PK columns). */
    if (decode_ok && share->has_user_pk)
    {
        KEY *pk_key = &table->key_info[share->pk_index];
        pos = ik + idx_col_len;
        for (uint p = 0; p < pk_key->user_defined_key_parts && decode_ok; p++)
        {
            KEY_PART_INFO *kp = &pk_key->key_part[p];
            Field *f = kp->field;

            if (f->real_maybe_null())
            {
                if (pos >= ik + iks)
                {
                    decode_ok = false;
                    break;
                }
                if (*pos == 0)
                {
                    f->set_null();
                    pos++;
                    continue;
                }
                f->set_notnull();
                pos++;
            }
            if (pos + kp->length > ik + iks)
            {
                decode_ok = false;
                break;
            }
            if (!decode_sort_key_part(pos, kp->length, f, buf)) decode_ok = false;
            pos += kp->length;
        }
    }

    if (!decode_ok)
    {
        /* Could not decode all key parts from the sort key (unsupported type
           like DECIMAL, VARCHAR, multi-byte CHAR).  Fall back to a full PK
           row fetch so ALL columns are available for condition evaluation.
           This is more expensive than pure ICP (still does the PK lookup)
           but is correct, the server won't re-evaluate pushed conditions. */
        if (iks > idx_col_len)
        {
            const uchar *pk = ik + idx_col_len;
            uint pk_len = (uint)(iks - idx_col_len);
            if (fetch_row_by_pk(scan_txn, pk, pk_len, buf) != 0)
                return CHECK_POS; /* PK lookup failed -- we accept row, let caller handle */
        }
        else
        {
            return CHECK_POS; /* malformed key -- we accept */
        }
    }

    /* Delegate to MariaDB's ICP evaluator which checks kill state,
       end_range, and pushed_idx_cond->val_bool(). */
    return handler_index_cond_check(this);
}

/* ******************** Counter recovery ******************** */

/*
  Recover hidden-PK next_row_id from the last data key.
  Also seed auto_inc_val for tables with AUTO_INCREMENT user-defined PKs
  so that get_auto_increment() can return O(1) instead of doing index_last()
  on every INSERT.
*/
void ha_tidesdb::recover_counters()
{
    tidesdb_txn_t *txn = NULL;
    if (tidesdb_txn_begin(tdb_global, &txn) != TDB_SUCCESS) return;

    tidesdb_iter_t *iter = NULL;
    if (tdb_iter_new_blocking(ha_thd(), txn, share->cf, &iter) == TDB_SUCCESS)
    {
        tidesdb_iter_seek_to_last(iter);
        if (tidesdb_iter_valid(iter))
        {
            uint8_t *key = NULL;
            size_t key_size = 0;
            if (tidesdb_iter_key(iter, &key, &key_size) == TDB_SUCCESS &&
                is_data_key(key, key_size))
            {
                if (!share->has_user_pk && key_size == KEY_NAMESPACE_LEN + HIDDEN_PK_SIZE)
                {
                    /* Hidden PK -- we decode the big-endian row-id */
                    uint64_t max_id = decode_be64(key + KEY_NAMESPACE_LEN);
                    share->next_row_id.store(max_id + 1, std::memory_order_relaxed);
                }

                /* Seeding auto_inc_val from the last row in primary-key order
                   is only correct when the AUTO_INCREMENT column is the
                   leftmost part of the primary key, since only then does the
                   PK-order maximum coincide with the auto-inc maximum.  When
                   the auto-inc column lives elsewhere (a different unique
                   key) the seed would underestimate the next value and let
                   get_auto_increment hand out colliding ids, so leave the
                   counter at zero and let MariaDB seed it on demand. */
                bool auto_inc_is_pk_leftmost = false;
                if (share->has_user_pk && table->found_next_number_field)
                {
                    const KEY *pk = &table->key_info[share->pk_index];
                    if (pk->user_defined_key_parts > 0 &&
                        pk->key_part[0].field == table->found_next_number_field)
                        auto_inc_is_pk_leftmost = true;
                }
                if (auto_inc_is_pk_leftmost)
                {
                    /* User PK with AUTO_INCREMENT -- we read the last row to seed
                       the in-memory counter from the max PK value. */
                    uint8_t *val = NULL;
                    size_t val_size = 0;
                    if (tidesdb_iter_value(iter, &val, &val_size) == TDB_SUCCESS)
                    {
                        /* We just unpack the packed row into record[1] using the proper
                           deserialize path so field offsets are correct even when
                           variable-length fields (CHAR/VARCHAR) precede the
                           AUTO_INCREMENT column. */
                        if (share->has_blobs || share->encrypted)
                        {
                            std::string row_data((const char *)val, val_size);
                            deserialize_row(table->record[1], row_data);
                        }
                        else
                        {
                            deserialize_row(table->record[1], (const uchar *)val, val_size);
                        }
                        /* deserialize_row writes into table->record[1];
                           val_int_offset wants the byte offset from record[0]
                           to record[1].  table->s->rec_buff_length is
                           normally equal but the API does not guarantee it,
                           so use the explicit subtraction the deserialize
                           path already relies on. */
                        ulonglong max_val = table->found_next_number_field->val_int_offset(
                            (my_ptrdiff_t)(table->record[1] - table->record[0]));
                        share->auto_inc_val.store(max_val, std::memory_order_relaxed);
                    }
                }
            }
        }
        tidesdb_iter_free(iter);
    }

    if (!share->has_user_pk && share->next_row_id.load(std::memory_order_relaxed) == 0)
        share->next_row_id.store(HIDDEN_PK_FIRST_ROW_ID, std::memory_order_relaxed);

    tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);
}

/* ******************** open / close / create ******************** */

int ha_tidesdb::open(const char *name, int mode, uint test_if_locked)
{
    DBUG_ENTER("ha_tidesdb::open");

    if (!(share = get_share())) DBUG_RETURN(1);

    /*
      We resolve CF pointers only once (first open).  Subsequent opens by
      other connections reuse the already-resolved share.  We hold
      lock_shared_ha_data() to prevent concurrent open() calls from
      racing on the shared vectors.
    */
    lock_shared_ha_data();
    if (!share->cf)
    {
        share->cf_name = path_to_cf_name(name);
        share->cf = tidesdb_get_column_family(tdb_global, share->cf_name.c_str());
        if (!share->cf)
        {
            unlock_shared_ha_data();
            sql_print_error("[TIDESDB] CF '%s' not found for table '%s'", share->cf_name.c_str(),
                            name);
            DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
        }

        if (table->s->primary_key != MAX_KEY)
        {
            share->has_user_pk = true;
            share->pk_index = table->s->primary_key;
            share->pk_key_len = comparable_key_length(&table->key_info[share->pk_index]);
        }
        else
        {
            share->has_user_pk = false;
            share->pk_index = MAX_KEY;
            share->pk_key_len = HIDDEN_PK_SIZE;
        }

        if (TDB_TABLE_OPTIONS(table))
        {
            uint iso_idx = TDB_TABLE_OPTIONS(table)->isolation_level;
            if (iso_idx < array_elements(tdb_isolation_map))
                share->isolation_level = (tidesdb_isolation_level_t)tdb_isolation_map[iso_idx];
        }

        if (TDB_TABLE_OPTIONS(table)) share->default_ttl = TDB_TABLE_OPTIONS(table)->ttl;

        share->encrypted = false;
        share->encryption_key_id = TIDESDB_DEFAULT_ENCRYPTION_KEY_ID;
        share->encryption_key_version = 0;
        if (TDB_TABLE_OPTIONS(table) && TDB_TABLE_OPTIONS(table)->encrypted)
        {
            share->encrypted = true;
            share->encryption_key_id = (uint)TDB_TABLE_OPTIONS(table)->encryption_key_id;
            uint ver = encryption_key_get_latest_version(share->encryption_key_id);
            if (ver == ENCRYPTION_KEY_VERSION_INVALID)
            {
                sql_print_error("[TIDESDB] encryption key %u not available",
                                share->encryption_key_id);
                DBUG_RETURN(HA_ERR_NO_SUCH_TABLE);
            }
            share->encryption_key_version = ver;
        }

        share->ttl_field_idx = TIDESDB_TTL_FIELD_NONE;
        for (uint i = 0; i < table->s->fields; i++)
        {
            if (table->s->field[i]->option_struct && table->s->field[i]->option_struct->ttl)
            {
                share->ttl_field_idx = (int)i;
                break;
            }
        }

        /* We cache table shape flags for hot-path short-circuiting.  We also
           capture the BLOB field indices so serialize_row's size estimate can
           iterate that short list instead of every field on every INSERT. */
        share->has_blobs = false;
        share->blob_field_indices.clear();
        for (uint i = 0; i < table->s->fields; i++)
        {
            if (table->s->field[i]->flags & BLOB_FLAG)
            {
                share->has_blobs = true;
                share->blob_field_indices.push_back((uint16)i);
            }
        }
        share->has_ttl = (share->default_ttl > 0 || share->ttl_field_idx >= 0);

        /* Per-field serialize/deserialize plan.  For each field cache its
           offset within record[0] and whether its pack format is a pure
           memcpy of pack_length() bytes -- if so the hot loops skip the
           Field::pack/unpack vtable dispatch entirely.

           The whitelist below covers the field types whose pack() output
           is byte-identical to memcpy(pack_length()) on a little-endian
           host.  CHAR / VARCHAR / BLOB / GEOMETRY / JSON / BIT / DECIMAL
           keep the slow path, their pack() trims trailing pad bytes,
           emits a length prefix, or layers a null-bit on top.

           Field_long / Field_longlong / Field_short etc. emit data in
           on-disk little-endian via the mi_int*store macros which equal
           memcpy on x86_64.  TIDESDB_FAST_SERDES_LE_ONLY guards the fast
           path so a big-endian build cleanly falls back to Field::pack. */
        share->field_plan.clear();
        share->field_plan.reserve(table->s->fields);
        share->null_bytes_cached = (uint8)table->s->null_bytes;
        share->fields_cached = (uint16)table->s->fields;
        share->has_no_nullable = (table->s->null_bytes == 0);
        for (uint i = 0; i < table->s->fields; i++)
        {
            /* We use the per-instance Field (table->field[i]), not the share
               prototype (table->s->field[i]).  maybe_null() / real_type()
               read through Field::table -- the share prototype is created
               with a null table pointer and crashes on those calls.  The
               per-instance Field has table = this handler's TABLE and is
               safe to query. */
            Field *f = table->field[i];
            TidesDB_share::field_plan_t fp;
            fp.src_off = (uint32)(f->ptr - table->record[0]);
            fp.pack_len = (uint16)f->pack_length();
            fp.maybe_null = f->maybe_null();
            fp.memcpy_ok = false;
#ifndef WORDS_BIGENDIAN
            switch (f->real_type())
            {
                case MYSQL_TYPE_TINY:
                case MYSQL_TYPE_SHORT:
                case MYSQL_TYPE_INT24:
                case MYSQL_TYPE_LONG:
                case MYSQL_TYPE_LONGLONG:
                case MYSQL_TYPE_FLOAT:
                case MYSQL_TYPE_DOUBLE:
                case MYSQL_TYPE_DATE:
                case MYSQL_TYPE_NEWDATE:
                case MYSQL_TYPE_TIME:
                case MYSQL_TYPE_TIME2:
                case MYSQL_TYPE_DATETIME:
                case MYSQL_TYPE_DATETIME2:
                case MYSQL_TYPE_TIMESTAMP:
                case MYSQL_TYPE_TIMESTAMP2:
                case MYSQL_TYPE_YEAR:
                case MYSQL_TYPE_NEWDECIMAL:
                    fp.memcpy_ok = true;
                    break;
                default:
                    fp.memcpy_ok = false;
                    break;
            }
            /* BLOB columns share MYSQL_TYPE_LONGLONG underneath in older
               codepaths; never fast-path anything carrying BLOB_FLAG. */
            if (f->flags & BLOB_FLAG) fp.memcpy_ok = false;
#endif
            share->field_plan.push_back(fp);
        }

        /* We precompute comparable key lengths and index-type flags per index.
           Caching the type flags avoids a ki->algorithm dereference per row
           in write_row's dup-check loop and in update_row/delete_row. */
        for (uint i = 0; i < table->s->keys; i++)
        {
            share->idx_comp_key_len[i] = comparable_key_length(&table->key_info[i]);
            share->idx_is_fts[i] = is_fts_index(&table->key_info[i]);
            share->idx_is_spatial[i] = is_spatial_index(&table->key_info[i]);
        }

        /* Precompute per-index coverage bitmaps so try_keyread_from_index is
           O(set bits in read_set) instead of nested scans over key parts. */
        share->idx_cover.assign(table->s->keys, std::vector<bool>(table->s->fields, false));
        for (uint i = 0; i < table->s->keys; i++)
        {
            const KEY *ki = &table->key_info[i];
            for (uint p = 0; p < ki->user_defined_key_parts; p++)
            {
                uint fnr = ki->key_part[p].fieldnr;
                if (fnr > 0 && fnr - 1 < table->s->fields) share->idx_cover[i][fnr - 1] = true;
            }
            /* Secondary indexes also cover the PK columns appended to the key. */
            if (table->s->primary_key != MAX_KEY && i != table->s->primary_key)
            {
                const KEY *pk_key = &table->key_info[table->s->primary_key];
                for (uint p = 0; p < pk_key->user_defined_key_parts; p++)
                {
                    uint fnr = pk_key->key_part[p].fieldnr;
                    if (fnr > 0 && fnr - 1 < table->s->fields) share->idx_cover[i][fnr - 1] = true;
                }
            }
        }

        for (uint i = 0; i < table->s->keys; i++)
        {
            if (share->has_user_pk && i == share->pk_index)
            {
                share->idx_cfs.push_back(NULL);
                share->idx_cf_names.push_back("");
                continue;
            }
            std::string idx_name;
            tidesdb_column_family_t *icf =
                resolve_idx_cf(tdb_global, share->cf_name, table->key_info[i].name.str, idx_name);
            share->idx_cfs.push_back(icf);
            share->idx_cf_names.push_back(idx_name);
        }

        share->num_secondary_indexes = 0;
        for (uint i = 0; i < share->idx_cfs.size(); i++)
            if (share->idx_cfs[i]) share->num_secondary_indexes++;

        /* Allocate the per-index full-cost cache; sized once so the array
           can be addressed by index number without a lock. */
        if (!share->idx_cfs.empty())
        {
            share->cached_idx_full_cost_n = (uint)share->idx_cfs.size();
            share->cached_idx_full_cost.reset(
                new std::atomic<double>[share->cached_idx_full_cost_n]);
            share->cached_idx_full_cost_time.reset(
                new std::atomic<long long>[share->cached_idx_full_cost_n]);
            for (uint i = 0; i < share->cached_idx_full_cost_n; i++)
            {
                share->cached_idx_full_cost[i].store(0.0, std::memory_order_relaxed);
                share->cached_idx_full_cost_time[i].store(0, std::memory_order_relaxed);
            }
        }

        /* We recover hidden-PK counter (auto-inc is derived at runtime via index_last) */
        recover_counters();

        {
            char frm_path[FN_REFLEN];
            fn_format(frm_path, name, "", reg_ext, MY_UNPACK_FILENAME | MY_APPEND_EXT);
            MY_STAT st_buf;
            if (mysql_file_stat(0, frm_path, &st_buf, MYF(0))) share->create_time = st_buf.st_mtime;
        }
    }
    unlock_shared_ha_data();

    ref_length = share->pk_key_len;

    /* We mirror shape flags onto the handler so the row-fetch hot paths
       read from a local member instead of chasing `share` into shared
       memory on every row.  These mirror constants that never change for
       the open handler. */
    has_blobs_ = share->has_blobs;
    encrypted_ = share->encrypted;

    /* We precompute the record[1] pointer range so the BLOB path of
       fetch_row_by_pk/iter_read_current doesn't rebuild it per row. */
    if (table->record[1])
    {
        record1_lo_ = table->record[1];
        record1_hi_ = table->record[1] + table->s->reclength;
    }
    else
    {
        record1_lo_ = NULL;
        record1_hi_ = NULL;
    }

    DBUG_RETURN(0);
}

int ha_tidesdb::close(void)
{
    DBUG_ENTER("ha_tidesdb::close");
    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();
    /* stmt_txn is a borrowed pointer into the per-connection trx->txn.
       We do not free it here -- the txn is owned by the per-connection trx
       and will be freed in tidesdb_close_connection(). */
    stmt_txn = NULL;
    stmt_txn_dirty = false;
    DBUG_RETURN(0);
}

int ha_tidesdb::create(const char *name, TABLE *table_arg, HA_CREATE_INFO *create_info)
{
    DBUG_ENTER("ha_tidesdb::create");

    std::string cf_name = path_to_cf_name(name);

    ha_table_option_struct *opts = TDB_TABLE_OPTIONS(table_arg);
    DBUG_ASSERT(opts);

    /* Under unified-memtable mode the shared WAL's fsync behaviour is owned
       by tidesdb_unified_memtable_sync_mode; the per-table SYNC_MODE option
       only governs SSTable file sync (klog and vlog).  Warn the user when
       the two differ so they do not assume the table option controls WAL
       durability for this table. */
    if (srv_unified_memtable && opts->sync_mode != srv_unified_memtable_sync_mode)
    {
        push_warning_printf(ha_thd(), Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                            "[TIDESDB] Table SYNC_MODE=%s governs SSTable file sync only.  Under "
                            "tidesdb_unified_memtable=ON the shared WAL is fsynced according to "
                            "tidesdb_unified_memtable_sync_mode=%s, so the table option does not "
                            "change WAL durability for this table",
                            sync_mode_names[opts->sync_mode],
                            sync_mode_names[srv_unified_memtable_sync_mode]);
    }

    tidesdb_column_family_config_t cfg = build_cf_config(opts);

    /* We create main data CF (we simply skip if it already exists, e.g. crash recovery) */
    if (!tidesdb_get_column_family(tdb_global, cf_name.c_str()))
    {
        int rc = tidesdb_create_column_family(tdb_global, cf_name.c_str(), &cfg);
        if (rc != TDB_SUCCESS)
        {
            sql_print_error("[TIDESDB] Failed to create CF '%s' (err=%d)", cf_name.c_str(), rc);
            DBUG_RETURN(tdb_rc_to_ha(rc, "create main_cf"));
        }
    }

    /* Per-index USE_BTREE overrides the table-level setting. */
    for (uint i = 0; i < table_arg->s->keys; i++)
    {
        if (table_arg->s->primary_key != MAX_KEY && i == table_arg->s->primary_key) continue;

        std::string idx_cf = cf_name + CF_INDEX_INFIX + table_arg->key_info[i].name.str;
        if (!tidesdb_get_column_family(tdb_global, idx_cf.c_str()))
        {
            tidesdb_column_family_config_t idx_cfg = cfg;
            ha_index_option_struct *iopts = table_arg->key_info[i].option_struct;
            if (iopts) idx_cfg.use_btree = iopts->use_btree ? 1 : 0;

            int rc = tidesdb_create_column_family(tdb_global, idx_cf.c_str(), &idx_cfg);
            if (rc != TDB_SUCCESS)
            {
                sql_print_error("[TIDESDB] Failed to create index CF '%s' (err=%d)", idx_cf.c_str(),
                                rc);
                DBUG_RETURN(tdb_rc_to_ha(rc, "create idx_cf"));
            }
        }
    }

    /* We store .frm in schema CF for object store discovery.
       When discover_table is registered, MariaDB skips writing .frm to disk
       and provides it via TABLE_SHARE::frm_image instead. */
    if (table_arg->s->frm_image)
        schema_cf_store_frm(name, table_arg->s->frm_image->str, table_arg->s->frm_image->length);
    else
        schema_cf_store_frm(name);

    DBUG_RETURN(0);
}

/* ******************** Data-at-rest encryption helpers ******************** */

/*
  Encrypt plaintext into out.  The on-disk blob is the 4-byte little-endian
  key version, then the 16-byte IV, then the ciphertext.  Storing the key
  version lets tidesdb_decrypt_row recover the exact key a row was written
  under, so encrypted rows remain readable across a key rotation.
*/
static bool tidesdb_encrypt_row_into(const std::string &plain, uint key_id, uint key_version,
                                     std::string &out)
{
    unsigned char key[TIDESDB_ENC_KEY_LEN];
    unsigned int klen = sizeof(key);
    /* Fail closed if the keyring cannot satisfy the request (missing version,
       buffer too small, plugin not loaded).  Without this check the local key
       buffer holds uninitialized stack bytes and encryption_crypt would
       proceed as if the request had succeeded, producing rows nobody can
       decrypt. */
    if (encryption_key_get(key_id, key_version, key, &klen) != 0)
    {
        sql_print_error("[TIDESDB] encryption_key_get failed for key_id=%u version=%u", key_id,
                        key_version);
        out.clear();
        return false;
    }

    unsigned char iv[TIDESDB_ENC_IV_LEN];
    my_random_bytes(iv, TIDESDB_ENC_IV_LEN);

    unsigned int slen = (unsigned int)plain.size();
    unsigned int enc_len = encryption_encrypted_length(slen, key_id, key_version);
    out.resize(TIDESDB_ENC_VERSION_LEN + TIDESDB_ENC_IV_LEN + enc_len);

    int4store(&out[0], (uint32)key_version);
    memcpy(&out[TIDESDB_ENC_VERSION_LEN], iv, TIDESDB_ENC_IV_LEN);

    unsigned int dlen = enc_len;
    int rc = encryption_crypt((const unsigned char *)plain.data(), slen,
                              (unsigned char *)&out[TIDESDB_ENC_VERSION_LEN + TIDESDB_ENC_IV_LEN],
                              &dlen, key, klen, iv, TIDESDB_ENC_IV_LEN, ENCRYPTION_FLAG_ENCRYPT,
                              key_id, key_version);
    if (rc != 0)
    {
        sql_print_error("[TIDESDB] encryption_crypt(encrypt) failed rc=%d", rc);
        out.clear();
        return false;
    }
    out.resize(TIDESDB_ENC_VERSION_LEN + TIDESDB_ENC_IV_LEN + dlen);
    return true;
}

/*
  Decrypt a row stored as [key version (4)] [IV (16)] [ciphertext].  The key
  version is read back from the blob so a row encrypted before a key rotation
  is decrypted with the key it was actually written under, not the latest.
*/
static std::string tidesdb_decrypt_row(const char *data, size_t len, uint key_id)
{
    if (len <= TIDESDB_ENC_VERSION_LEN + TIDESDB_ENC_IV_LEN)
    {
        sql_print_error("[TIDESDB] encrypted row too short (%zu bytes)", len);
        return std::string(); /* signal failure */
    }

    uint key_version = (uint)uint4korr(data);

    unsigned char key[TIDESDB_ENC_KEY_LEN];
    unsigned int klen = sizeof(key);
    /* Fail closed if the keyring cannot return the version this row was
       written under (rotated-out key, plugin not loaded, version never
       existed).  Falling through with an uninitialized key buffer would
       feed garbage into encryption_crypt and silently corrupt the
       deserialize path. */
    if (encryption_key_get(key_id, key_version, key, &klen) != 0)
    {
        sql_print_error("[TIDESDB] encryption_key_get failed for key_id=%u version=%u", key_id,
                        key_version);
        return std::string(); /* signal failure to caller */
    }

    const unsigned char *iv = (const unsigned char *)data + TIDESDB_ENC_VERSION_LEN;
    const unsigned char *src =
        (const unsigned char *)data + TIDESDB_ENC_VERSION_LEN + TIDESDB_ENC_IV_LEN;
    unsigned int slen = (unsigned int)(len - TIDESDB_ENC_VERSION_LEN - TIDESDB_ENC_IV_LEN);

    std::string out;
    unsigned int dlen = slen + TIDESDB_ENC_KEY_LEN; /* padding slack */
    out.resize(dlen);

    int rc = encryption_crypt(src, slen, (unsigned char *)&out[0], &dlen, key, klen, iv,
                              TIDESDB_ENC_IV_LEN, ENCRYPTION_FLAG_DECRYPT, key_id, key_version);
    if (rc != 0)
    {
        sql_print_error("[TIDESDB] encryption_crypt(decrypt) failed rc=%d", rc);
        return std::string(); /* signal failure */
    }
    out.resize(dlen);
    return out;
}

/* ******************** serialize / deserialize (BLOB deep-copy) ******************** */

/* Row format header constants live in ha_tidesdb.h so the stop-word
   loader and other callers can reference them without forward decls.
   Layout is [ROW_HEADER_MAGIC] [null_bytes_stored (2 LE)] [field_count (2 LE)]
   for ROW_HEADER_SIZE bytes total.  Enables instant ADD/DROP COLUMN. */

const std::string &ha_tidesdb::serialize_row(const uchar *buf)
{
    my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(buf - table->record[0]);

    /* Upper-bound packed size.  For non-BLOB tables the estimate is constant
       (header + null_bytes + reclength + 2 bytes per field for length-prefix
       overhead from Field_string::pack).  Cache it to avoid recomputing on
       every row.  For BLOB tables we must add the actual blob data sizes. */
    size_t est = share->cached_row_est;
    if (unlikely(est == 0))
    {
        est = ROW_HEADER_SIZE + table->s->null_bytes + table->s->reclength +
              FIELD_VARCHAR_LEN_PREFIX * table->s->fields;
        if (!share->has_blobs)
            share->cached_row_est = est; /* safe to cache -- constant for non-BLOB tables */
    }
    if (share->has_blobs)
    {
        /* Walk only the precomputed BLOB field list instead of every field. */
        for (uint16 idx : share->blob_field_indices)
        {
            Field *f = table->field[idx];
            if (f->is_real_null(ptrdiff)) continue;
            Field_blob *blob = (Field_blob *)f;
            est += blob->get_length(buf + (uintptr_t)(f->ptr - table->record[0]));
        }
    }

    row_buf_.resize(est);
    uchar *start = (uchar *)&row_buf_[0];
    uchar *pos = start;

    /* Row header -- enables instant ADD/DROP COLUMN by recording the
       null bitmap size and field count at write time. */
    *pos++ = ROW_HEADER_MAGIC;
    const uint nb = share->null_bytes_cached;
    const uint nf = share->fields_cached;
    int2store(pos, (uint16)nb);
    pos += sizeof(uint16);
    int2store(pos, (uint16)nf);
    pos += sizeof(uint16);

    /* Null bitmap */
    if (nb) memcpy(pos, buf, nb);
    pos += nb;

    /* We pack each non-null field.  We use a precomputed per-field plan
       (built once at open()) so the hot path skips the Field::pack vtable
       dispatch for fields whose pack format is a pure memcpy of
       pack_length() bytes -- integers, fixed-precision datetimes,
       NEWDECIMAL, FLOAT, DOUBLE.  CHAR / VARCHAR / BLOB still go through
       Field::pack because their format trims pad bytes or emits a length
       prefix.  The plan also caches `f->ptr - record[0]` so that
       subtraction does not run per row.

       When the table has no nullable fields (share->has_no_nullable),
       skip the per-field real_maybe_null branch entirely. */
    const TidesDB_share::field_plan_t *plan = share->field_plan.data();
    const bool all_not_null = share->has_no_nullable;
    for (uint i = 0; i < nf; i++)
    {
        const TidesDB_share::field_plan_t &fp = plan[i];
        if (!all_not_null && fp.maybe_null)
        {
            if (table->field[i]->is_real_null(ptrdiff)) continue;
        }
        const uchar *src = buf + fp.src_off;
        if (fp.memcpy_ok)
        {
            memcpy(pos, src, fp.pack_len);
            pos += fp.pack_len;
        }
        else
        {
            pos = table->field[i]->pack(pos, src);
        }
    }

    row_buf_.resize((size_t)(pos - start));

    if (share->encrypted)
    {
        /* We cache the encryption key version per-statement to avoid the
           expensive encryption_key_get_latest_version() syscall on every
           single row.  The cache is invalidated at statement start
           (enc_key_ver_valid_ = false in external_lock). */
        if (!enc_key_ver_valid_)
        {
            uint cur_ver = encryption_key_get_latest_version(share->encryption_key_id);
            if (cur_ver != ENCRYPTION_KEY_VERSION_INVALID)
            {
                share->encryption_key_version = cur_ver;
                cached_enc_key_ver_ = cur_ver;
            }
            else
            {
                cached_enc_key_ver_ = share->encryption_key_version;
            }
            enc_key_ver_valid_ = true;
        }
        /* We encrypt into enc_buf_ instead of replacing row_buf_, so that
           row_buf_'s heap capacity is preserved across calls.
           Writing directly into enc_buf_ reuses its heap capacity across rows,
           avoiding a per-row allocation when the encrypted size is stable. */
        if (!tidesdb_encrypt_row_into(row_buf_, share->encryption_key_id, cached_enc_key_ver_,
                                      enc_buf_))
        {
            enc_buf_.clear(); /* signal failure */
        }
        return enc_buf_;
    }

    return row_buf_;
}

void ha_tidesdb::deserialize_row(uchar *buf, const uchar *data, size_t len)
{
    const uchar *from = data;
    const uchar *from_end = data + len;

    /* All rows have the header([0xFE] [null_bytes(2)] [field_count(2)]) */
    if (unlikely(len < ROW_HEADER_SIZE || data[0] != ROW_HEADER_MAGIC))
    {
        /* Corrupted or truncated row, we zero the record to avoid garbage */
        memset(buf, 0, table->s->reclength);
        return;
    }

    from++;
    uint stored_null_bytes = uint2korr(from);
    from += sizeof(uint16);
    uint stored_fields = uint2korr(from);
    from += sizeof(uint16);

    /* Null bitmap -- we copy the smaller of stored vs current.
       When columns were added (stored_null_bytes < table->s->null_bytes),
       fill the extra null bitmap bytes from the table's default record
       so that new columns inherit their correct DEFAULT / NOT NULL state
       rather than blindly marking them NULL. */
    if ((size_t)(from_end - from) < stored_null_bytes) return;
    const uint cur_nb = share->null_bytes_cached;
    uint copy_nb = MY_MIN(stored_null_bytes, cur_nb);
    if (copy_nb) memcpy(buf, from, copy_nb);
    if (copy_nb < cur_nb)
        memcpy(buf + copy_nb, table->s->default_values + copy_nb, cur_nb - copy_nb);
    from += stored_null_bytes;

    /* We unpack.  Only unpack up to MIN(stored_fields, current_fields).
       If the row has more fields than the current schema (DROP COLUMN),
       the extra packed data is simply skipped.
       If the row has fewer fields (ADD COLUMN), fill the missing fields
       from the table's default record so they get their DEFAULT value. */
    const uint cur_nf = share->fields_cached;
    uint unpack_count = MY_MIN(stored_fields, cur_nf);

    /* Pre-fill default values for columns added after this row was written.
       Copy each new field's bytes from default_values into buf so that
       they have the correct DEFAULT even when the field is NOT NULL. */
    if (stored_fields < cur_nf)
    {
        const TidesDB_share::field_plan_t *plan_d = share->field_plan.data();
        for (uint i = stored_fields; i < cur_nf; i++)
        {
            const TidesDB_share::field_plan_t &fp = plan_d[i];
            memcpy(buf + fp.src_off, table->s->default_values + fp.src_off, fp.pack_len);
        }
    }

    /* memcpy_ok fields write directly to `to` via memcpy, so they never
       need move_field_offset.  The slow-path branch covers CHAR / VARCHAR
       / BLOB; only Field_blob::unpack writes through field->ptr (via
       set_ptr), so we only pay the virtual move_field_offset pair when
       the destination buffer is not record[0] AND the field needs the
       slow path.  buf == record[0] (ptrdiff == 0) is the common case
       for index scans and PK reads, so the loop avoids the vcall pair
       entirely there. */
    const my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(buf - table->record[0]);
    const TidesDB_share::field_plan_t *plan = share->field_plan.data();
    const bool all_not_null = share->has_no_nullable;
    for (uint i = 0; i < unpack_count; i++)
    {
        const TidesDB_share::field_plan_t &fp = plan[i];
        if (!all_not_null && fp.maybe_null)
        {
            if (table->field[i]->is_real_null(ptrdiff)) continue;
        }
        if (from >= from_end) break;
        uchar *to = buf + fp.src_off;
        if (fp.memcpy_ok)
        {
            if (from + fp.pack_len > from_end) break;
            memcpy(to, from, fp.pack_len);
            from += fp.pack_len;
        }
        else
        {
            Field *f = table->field[i];
            const uchar *next;
            if (ptrdiff != 0)
            {
                f->move_field_offset(ptrdiff);
                next = f->unpack(to, from, from_end);
                f->move_field_offset(-ptrdiff);
            }
            else
            {
                next = f->unpack(to, from, from_end);
            }
            if (!next) break;
            from = next;
        }
    }
}

void ha_tidesdb::deserialize_row(uchar *buf, const std::string &row)
{
    const std::string *plain = &row;
    std::string decrypted;

    if (share->encrypted)
    {
        decrypted = tidesdb_decrypt_row(row.data(), row.size(), share->encryption_key_id);
        if (decrypted.empty())
        {
            /* Decryption failed! we zero record to avoid returning garbage */
            memset(buf, 0, table->s->reclength);
            return;
        }
        last_row = std::move(decrypted);
        plain = &last_row;
    }

    deserialize_row(buf, (const uchar *)plain->data(), plain->size());
}

/* ******************** fetch_row_by_pk ******************** */

/*
  Point-lookup a row by its PK bytes (without namespace prefix).
  Sets current_pk + last_row.  Returns 0, HA_ERR_KEY_NOT_FOUND,
  or HA_ERR_LOCK_DEADLOCK (on TDB_ERR_CONFLICT).
*/
int ha_tidesdb::fetch_row_by_pk(tidesdb_txn_t *txn, const uchar *pk, uint pk_len, uchar *buf)
{
    /* Pessimistic row lock for point reads.  Covers both the direct PK
       lookup path (HA_READ_KEY_EXACT) and the secondary-index resolved-PK
       path (sec idx returns [prefix][pk]; caller passes the suffix here).
       Mode is X for write-intent, S under RR/SR for plain reads; RC/SI
       reads take no lock (snapshot suffices).  Re-entrant -- a no-op when
       the caller already holds the lock in a compatible-or-stronger mode. */
    if (unlikely(srv_pessimistic_locking) && cached_trx_)
    {
        tdb_lock_mode_t mode;
        if (tdb_lock_mode_for_read(cached_thd_, stmt_has_write_lock_, &mode))
        {
            int lrc = row_lock_acquire(cached_trx_, pk, pk_len, cached_thd_, mode);
            if (lrc) return lrc;
        }
    }

    uchar dk[DATA_KEY_BUF_LEN];
    uint dk_len = build_data_key(pk, pk_len, dk);

    uint8_t *value = NULL;
    size_t value_size = 0;
    int rc = tidesdb_txn_get(txn, share->cf, dk, dk_len, &value, &value_size);
    if (rc == TDB_ERR_NOT_FOUND) return HA_ERR_KEY_NOT_FOUND;
    if (rc != TDB_SUCCESS) return tdb_rc_to_ha(rc, "fetch_row_by_pk");

    if (likely(!has_blobs_ && !encrypted_))
    {
        /* Zero-copy path, we deserialize directly from API buffer */
        deserialize_row(buf, (const uchar *)value, value_size);
        tidesdb_free(value);
    }
    else
    {
        /* For BLOB tables, Field_blob::unpack() stores pointers into the
           source buffer.  These pointers must remain valid until the next
           fetch into the SAME record buffer.  The MariaDB handler API
           (e.g., mhnsw vector index maintenance) may interleave reads into
           record[0] and record[1], so we maintain two backing buffers:
           last_row for record[0] fetches, last_row2 for record[1] fetches.
           This prevents a fetch into record[1] from invalidating BLOB
           pointers that record[0] still references.

           We identify record[1] using the precomputed bounds set in open(). */
        bool is_rec1 = record1_lo_ && buf >= record1_lo_ && buf < record1_hi_;
        std::string &backing = is_rec1 ? last_row2 : last_row;
        backing.assign((const char *)value, value_size);
        tidesdb_free(value);
        deserialize_row(buf, backing);
    }
    memcpy(current_pk_buf_, pk, pk_len);
    current_pk_len_ = pk_len;

    return 0;
}

/* ******************** compute_row_ttl ******************** */

/*
  Compute the absolute TTL timestamp for a row being written.
  Priority -- per-row TTL_COL value > table-level TTL option > no expiration.
  Returns -1 (no expiration) or a future absolute Unix timestamp.
*/
time_t ha_tidesdb::compute_row_ttl(const uchar *buf)
{
    long long ttl_seconds = 0;

    if (share->ttl_field_idx >= 0)
    {
        Field *f = table->field[share->ttl_field_idx];
        my_ptrdiff_t ptrdiff = (my_ptrdiff_t)(buf - table->record[0]);
        if (!f->is_real_null(ptrdiff))
        {
            f->move_field_offset(ptrdiff);
            ttl_seconds = f->val_int();
            f->move_field_offset(-ptrdiff);
        }
    }

    /* Session TTL override, we use cached value to avoid THDVAR + ha_thd()
       on every row.  The cache is populated once per statement in write_row
       / update_row and invalidated in external_lock(F_UNLCK). */
    if (ttl_seconds <= 0)
    {
        if (cached_sess_ttl_ > 0) ttl_seconds = (long long)cached_sess_ttl_;
    }

    if (ttl_seconds <= 0 && share->default_ttl > 0) ttl_seconds = (long long)share->default_ttl;

    if (ttl_seconds <= 0) return TIDESDB_TTL_NONE;

    /* We use cached time(NULL) to avoid the vDSO/syscall per row.
       n-second granularity is more than sufficient for TTL. */
    if (!cached_time_valid_)
    {
        cached_time_ = time(NULL);
        cached_time_valid_ = true;
    }

    return (time_t)(cached_time_ + ttl_seconds);
}

/* ******************** iter_read_current ******************** */

/*
  Read the current iterator position in the main data CF.
  Skips non-data keys (meta keys).  Sets current_pk + last_row.
  Does not advance the iterator.
*/
int ha_tidesdb::iter_read_current(uchar *buf)
{
    while (scan_iter && tidesdb_iter_valid(scan_iter))
    {
        uint8_t *key = NULL;
        size_t key_size = 0;
        uint8_t *value = NULL;
        size_t value_size = 0;
        if (tidesdb_iter_key_value(scan_iter, &key, &key_size, &value, &value_size) != TDB_SUCCESS)
            return HA_ERR_END_OF_FILE;

        if (!is_data_key(key, key_size))
        {
            tidesdb_iter_next(scan_iter);
            continue;
        }

        current_pk_len_ = (uint)(key_size - KEY_NAMESPACE_LEN);
        memcpy(current_pk_buf_, key + KEY_NAMESPACE_LEN, current_pk_len_);

        /* Pessimistic row lock for range/prefix scans.  Mode chosen by
           write-intent + session isolation; covers SELECT ... FOR UPDATE
           plus plain SELECT under RR/SR.

           Under UPDATE/DELETE we deliberately skip the lock here so a
           secondary-index scan with ICP does not X-lock every PK it walks
           past during filtering; update_row/delete_row reacquire on the
           row they actually mutate.  SELECT ... FOR UPDATE keeps the
           per-row lock because the SQL semantics require locking every
           row the cursor exposes, even rows the client never reads. */
        if (unlikely(srv_pessimistic_locking) && cached_trx_ && !stmt_is_update_or_delete_)
        {
            tdb_lock_mode_t mode;
            if (tdb_lock_mode_for_read(cached_thd_, stmt_has_write_lock_, &mode))
            {
                int lrc = row_lock_acquire(cached_trx_, current_pk_buf_, current_pk_len_,
                                           cached_thd_, mode);
                if (lrc) return lrc;
            }
        }

        if (likely(!has_blobs_ && !encrypted_))
        {
            deserialize_row(buf, (const uchar *)value, value_size);
        }
        else
        {
            bool is_rec1 = record1_lo_ && buf >= record1_lo_ && buf < record1_hi_;
            std::string &backing = is_rec1 ? last_row2 : last_row;
            backing.assign((const char *)value, value_size);
            deserialize_row(buf, backing);
        }
        return 0;
    }
    return HA_ERR_END_OF_FILE;
}

/* ******************** write_row (INSERT) ******************** */

int ha_tidesdb::write_row(const uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::write_row");

    /* We need all columns readable for PK extraction, secondary index
       key building, serialization, and TTL computation. */
    MY_BITMAP *old_map = tmp_use_all_columns(table, &table->read_set);

    bool pk_auto_generated = false; /* true when PK was auto-generated (guaranteed unique) */
    if (table->next_number_field && buf == table->record[0])
    {
        /* If the PK field is 0/NULL, MariaDB's update_auto_increment() will
           generate a unique value from our atomic counter.  We can skip the
           expensive PK uniqueness point-get in that case.
           Only safe when the auto-inc field is the ENTIRE PK (single-column).
           For composite PKs, auto-inc only guarantees uniqueness within
           the auto-inc column, not the full composite key. */
        if (table->next_number_field->val_int() == 0 && share->has_user_pk &&
            table->key_info[share->pk_index].user_defined_key_parts == 1)
            pk_auto_generated = true;
        int ai_err = update_auto_increment();
        if (ai_err)
        {
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(ai_err);
        }
        /* We keep the shared counter ahead of any explicitly-supplied value
           so that future auto-generated values don't collide. */
        ulonglong val = table->next_number_field->val_int();
        ulonglong cur = share->auto_inc_val.load(std::memory_order_relaxed);
        while (val > cur)
        {
            if (share->auto_inc_val.compare_exchange_weak(cur, val, std::memory_order_relaxed))
                break;
        }
    }

    uchar pk[MAX_KEY_LENGTH];
    uint pk_len;
    if (share->has_user_pk)
    {
        pk_len = pk_from_record(buf, pk);
    }
    else
    {
        /* Hidden PK -- we generate next row-id */
        uint64_t row_id = share->next_row_id.fetch_add(1, std::memory_order_relaxed);
        encode_be64(row_id, pk);
        pk_len = HIDDEN_PK_SIZE;
    }

    uchar dk[DATA_KEY_BUF_LEN];
    uint dk_len = build_data_key(pk, pk_len, dk);

    const std::string &row_data = serialize_row(buf);
    if (share->encrypted && row_data.empty())
    {
        tmp_restore_column_map(&table->read_set, old_map);
        DBUG_RETURN(HA_ERR_GENERIC);
    }
    const uint8_t *row_ptr = (const uint8_t *)row_data.data();
    size_t row_len = row_data.size();

    /* Lazy txn -- we ensure stmt_txn exists on first data access */
    {
        int erc = ensure_stmt_txn();
        if (erc)
        {
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(erc);
        }
    }
    tidesdb_txn_t *txn = stmt_txn;
    stmt_txn_dirty = true;

    /* We use cached pointers from external_lock to avoid per-row overhead. */
    tidesdb_trx_t *trx = cached_trx_;
    if (trx)
    {
        trx->dirty = true;
    }

    /* We acquire pessimistic row lock for INSERT when pessimistic_locking=ON.
       Without this, INSERT bypasses locks held by SELECT ... FOR UPDATE,
       UPDATE, and DELETE on the same PK -- breaking the serialization
       guarantee that pessimistic locking is supposed to provide.
       We use the comparable PK bytes (pk, pk_len) which are the same key
       format used by index_read_map() for lock acquisition. */
    if (unlikely(srv_pessimistic_locking) && share->has_user_pk && trx)
    {
        int lrc = row_lock_acquire(trx, pk, pk_len, cached_thd_, TDB_LOCK_MODE_X);
        if (lrc)
        {
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(lrc);
        }
    }

    /* We cache THDVAR lookups once per statement. */
    if (!cached_thdvars_valid_)
    {
        cached_skip_unique_ = THDVAR(cached_thd_, skip_unique_check);
        cached_sess_ttl_ = THDVAR(cached_thd_, ttl);
        cached_single_delete_primary_ = THDVAR(cached_thd_, single_delete_primary);
        cached_thdvars_valid_ = true;
    }

    /* We check PK uniqueness before inserting (TidesDB put overwrites silently).
       IODKU needs HA_ERR_FOUND_DUPP_KEY so the server can run the UPDATE clause.
       REPLACE INTO also needs it when secondary indexes exist (old index entries
       must be cleaned up via delete+reinsert).  When write_can_replace_ is set
       and the table has no secondary indexes, we skip the dup check entirely --
       tidesdb_txn_put will overwrite the old value, which is exactly what REPLACE
       wants, saving a full point-lookup per row.
       SET SESSION tidesdb_skip_unique_check=1 (bulk load) also bypasses this.
       When the PK was auto-generated by our O(1) atomic counter, the value is
       guaranteed unique (seeded from max existing value) -- skip the point-get.
       The auto-generated guarantee covers only the primary key, so it must
       never skip the UNIQUE secondary-index check further down. */
    bool skip_pk_unique = cached_skip_unique_ || pk_auto_generated;
    if (share->has_user_pk && !skip_pk_unique &&
        !(write_can_replace_ && share->num_secondary_indexes == 0))
    {
        uint8_t *dup_val = NULL;
        size_t dup_len = 0;
        int grc = tidesdb_txn_get(txn, share->cf, dk, dk_len, &dup_val, &dup_len);
        if (grc == TDB_SUCCESS)
        {
            tidesdb_free(dup_val);
            errkey = lookup_errkey = share->pk_index;
            memcpy(dup_ref, pk, pk_len);
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
        }
        if (grc != TDB_ERR_NOT_FOUND)
        {
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(tdb_rc_to_ha(grc, "write_row pk_dup_check"));
        }
    }

    /* We check UNIQUE secondary index uniqueness.  This honours the
       explicit tidesdb_skip_unique_check session contract but never the
       pk_auto_generated optimization, which only proves the primary key is
       unique and tells us nothing about secondary unique values.
       Cached dup-check iterators avoid the catastrophically expensive
       tidesdb_iter_new() (O(num_sstables) merge-heap construction) on
       every single INSERT.  The iterator per unique index is created
       once and reused via seek() across rows within the same txn. */
    if (share->num_secondary_indexes > 0 && !cached_skip_unique_)
    {
        /* trx already cached at top of write_row */
        uint64_t cur_gen = trx ? trx->txn_generation : 0;

        for (uint i = 0; i < table->s->keys; i++)
        {
            if (share->has_user_pk && i == share->pk_index) continue;
            if (i >= share->idx_cfs.size() || !share->idx_cfs[i]) continue;
            if (share->idx_is_fts[i] || share->idx_is_spatial[i]) continue;
            if (!(table->key_info[i].flags & HA_NOSAME)) continue;

            uchar idx_prefix[MAX_KEY_LENGTH];
            uint idx_prefix_len = make_comparable_key(
                &table->key_info[i], buf, table->key_info[i].user_defined_key_parts, idx_prefix);

            /* Pessimistic row lock on the UNIQUE-secondary prefix.  Without
               this, the dup-check below uses the txn's MVCC view and two
               concurrent INSERTs of the same unique value can both pass
               the check and both commit, producing a logical UNIQUE
               violation.  Locking the prefix serialises the check+put on
               the same value across writers. */
            if (unlikely(srv_pessimistic_locking) && trx)
            {
                int lrc =
                    row_lock_acquire(trx, idx_prefix, idx_prefix_len, cached_thd_, TDB_LOCK_MODE_X);
                if (lrc)
                {
                    tmp_restore_column_map(&table->read_set, old_map);
                    DBUG_RETURN(lrc);
                }
            }

            /* We get or create cached dup-check iterator for this index.
               Invalidate if the txn changed (commit/reset frees txn ops
               that the iterator's MERGE_SOURCE_TXN_OPS depends on). */
            tidesdb_iter_t *dup_iter = dup_iter_cache_[i];
            if (dup_iter && (dup_iter_txn_[i] != txn || dup_iter_txn_gen_[i] != cur_gen))
            {
                tidesdb_iter_free(dup_iter);
                dup_iter = NULL;
                dup_iter_cache_[i] = NULL;
            }
            if (!dup_iter)
            {
                {
                    int irc = tdb_iter_new_blocking(ha_thd(), txn, share->idx_cfs[i], &dup_iter);
                    if (irc != TDB_SUCCESS || !dup_iter)
                    {
                        /* Iterator creation failed, thus cannot safely skip the
                           uniqueness check or we risk silent UNIQUE violations.
                           Propagate the error to the caller. */
                        tmp_restore_column_map(&table->read_set, old_map);
                        DBUG_RETURN(tdb_rc_to_ha(irc, "write_row dup_iter_new"));
                    }
                }
                dup_iter_cache_[i] = dup_iter;
                dup_iter_txn_[i] = txn;
                dup_iter_txn_gen_[i] = cur_gen;
                dup_iter_count_++;
            }

            tidesdb_iter_seek(dup_iter, idx_prefix, idx_prefix_len);
            if (tidesdb_iter_valid(dup_iter))
            {
                uint8_t *fk = NULL;
                size_t fks = 0;
                if (tidesdb_iter_key(dup_iter, &fk, &fks) == TDB_SUCCESS && fks >= idx_prefix_len &&
                    memcmp(fk, idx_prefix, idx_prefix_len) == 0)
                {
                    /* We extract PK suffix from the index key for dup_ref */
                    size_t dup_pk_len = fks - idx_prefix_len;
                    if (dup_pk_len > 0 && dup_pk_len <= ref_length)
                        memcpy(dup_ref, fk + idx_prefix_len, dup_pk_len);
                    errkey = lookup_errkey = i;
                    tmp_restore_column_map(&table->read_set, old_map);
                    DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
                }
            }
        }
    }

    /* We compute TTL when the table has TTL configured or the session overrides it.
       Uses cached_sess_ttl_ to avoid THDVAR + ha_thd() per row. */
    time_t row_ttl =
        (share->has_ttl || cached_sess_ttl_ > 0) ? compute_row_ttl(buf) : TIDESDB_TTL_NONE;

    int rc =
        tdb_txn_put_blocking(cached_thd_, txn, share->cf, dk, dk_len, row_ptr, row_len, row_ttl);
    if (rc != TDB_SUCCESS) goto err;

    memcpy(current_pk_buf_, pk, pk_len);
    current_pk_len_ = pk_len;
    /* We maintain all secondary indexes in a single consolidated loop.
       Loop invariants are hoisted to avoid redundant pointer dereferences
       per iteration. Regular, FTS, and spatial indexes are dispatched
       inline to eliminate 2/3 of loop overhead vs 3 separate loops. */
    if (share->num_secondary_indexes > 0)
    {
        const uint num_keys = table->s->keys;
        const bool has_user_pk = share->has_user_pk;
        const uint pk_index = share->pk_index;
        const size_t idx_cfs_sz = share->idx_cfs.size();

        for (uint i = 0; i < num_keys; i++)
        {
            if (has_user_pk && i == pk_index) continue;
            if (i >= idx_cfs_sz || !share->idx_cfs[i]) continue;

            const KEY *ki = &table->key_info[i];

            if (ki->algorithm == HA_KEY_ALG_FULLTEXT)
            {
                /* FTS index maintenance */
                CHARSET_INFO *fts_cs = ki->key_part[0].field->charset();
                std::vector<fts_token_t> fts_tokens;
                fts_extract_and_tokenize(table, ki, buf, fts_cs, fts_tokens);

                std::unordered_map<std::string, uint16> tf_map;
                for (auto &tok : fts_tokens) tf_map[tok.word]++;
                uint32 word_count = (uint32)fts_tokens.size();

                for (auto &[term, tf] : tf_map)
                {
                    uchar fk[FTS_KEY_BUF_LEN];
                    uint fk_len = fts_build_key(term.data(), (uint)term.size(), pk, pk_len, fk);
                    uchar fv[FTS_VALUE_LEN];
                    fts_build_value(tf, word_count, fv);
                    rc = tdb_txn_put_blocking(cached_thd_, txn, share->idx_cfs[i], fk, fk_len, fv,
                                              FTS_VALUE_LEN, row_ttl);
                    if (rc != TDB_SUCCESS) goto err;
                }

                trx_fts_meta_accumulate(trx, share->cf, i, FTS_DOC_DELTA_ADD, (int64_t)word_count);
            }
            else if (is_spatial_index(ki))
            {
                /* Spatial index maintenance */
                Field *geom_field = ki->key_part[0].field;
                my_ptrdiff_t ptd = (my_ptrdiff_t)(buf - table->record[0]);
                if (ptd) geom_field->move_field_offset(ptd);
                String geom_str;
                geom_field->val_str(&geom_str, &geom_str);
                if (ptd) geom_field->move_field_offset(-ptd);

                double xmin, ymin, xmax, ymax;
                if (geom_str.length() > 0 &&
                    spatial_compute_mbr((const uchar *)geom_str.ptr(), geom_str.length(), &xmin,
                                        &ymin, &xmax, &ymax))
                {
                    double cx = (xmin + xmax) / MBR_CENTROID_DIV;
                    double cy = (ymin + ymax) / MBR_CENTROID_DIV;
                    uchar sk[SPATIAL_HILBERT_KEY_LEN + MAX_KEY_LENGTH];
                    uint sk_len = spatial_build_key(cx, cy, pk, pk_len, sk);
                    uchar sv[SPATIAL_MBR_VALUE_LEN];
                    spatial_build_value(xmin, ymin, xmax, ymax, sv);
                    rc = tdb_txn_put_blocking(cached_thd_, txn, share->idx_cfs[i], sk, sk_len, sv,
                                              SPATIAL_MBR_VALUE_LEN, row_ttl);
                    if (rc != TDB_SUCCESS) goto err;
                }
            }
            else
            {
                /* Regular secondary index maintenance */
                uchar ik[SEC_IDX_KEY_BUF_LEN];
                uint ik_len = sec_idx_key(i, buf, ik);
                rc = tdb_txn_put_blocking(cached_thd_, txn, share->idx_cfs[i], ik, ik_len,
                                          &tdb_empty_val, sizeof(tdb_empty_val), row_ttl);
                if (rc != TDB_SUCCESS) goto err;
            }
        }
    }

    /* We track ops for bulk insert batching (1 data + N secondary index puts) */
    if (in_bulk_insert_)
    {
        bulk_insert_ops_ += 1 + share->num_secondary_indexes;
        if (bulk_insert_ops_ >= TIDESDB_BULK_INSERT_BATCH_OPS)
        {
            int mrc = maybe_bulk_commit(trx);
            if (mrc)
            {
                tmp_restore_column_map(&table->read_set, old_map);
                DBUG_RETURN(mrc);
            }
            bulk_insert_ops_ = 0;
        }
    }

    /* Commit happens in external_lock(F_UNLCK). */
    tmp_restore_column_map(&table->read_set, old_map);
    DBUG_RETURN(0);

err:
    tmp_restore_column_map(&table->read_set, old_map);
    DBUG_RETURN(tdb_rc_to_ha(rc, "write_row"));
}

/* ******************** AUTO_INCREMENT (O(1) atomic counter) ******************** */

/*
  Override the default get_auto_increment() which calls index_last() on every
  single auto-commit INSERT.  That creates and destroys a TidesDB merge-heap
  iterator each time -- O(N sources).  Instead, we maintain an in-memory atomic
  counter on TidesDB_share that is seeded once from the table data at open time
  and atomically incremented thereafter -- O(1).
*/
void ha_tidesdb::get_auto_increment(ulonglong offset, ulonglong increment,
                                    ulonglong nb_desired_values, ulonglong *first_value,
                                    ulonglong *nb_reserved_values)
{
    DBUG_ENTER("ha_tidesdb::get_auto_increment");

    /* Atomic fetch-and-add -- each caller gets a unique range.
       The counter stores the last value that was handed out. */
    ulonglong cur = share->auto_inc_val.load(std::memory_order_relaxed);
    ulonglong next;
    do
    {
        next = cur + nb_desired_values;
    } while (!share->auto_inc_val.compare_exchange_weak(cur, next, std::memory_order_relaxed));

    *first_value = cur + 1;
    /*
      We reserve exactly what was asked for.  MariaDB's update_auto_increment()
      will call us again when the interval is exhausted.
    */
    *nb_reserved_values = nb_desired_values;

    DBUG_VOID_RETURN;
}

/*
  Reset the auto-increment counter(s) to the given value.  MariaDB's default
  truncate() path calls this after delete_all_rows, and ALTER TABLE ...
  AUTO_INCREMENT=N routes here as well.  The next auto-generated ID equals
  `value` itself, so we store `value - 1` (get_auto_increment does
  fetch-add and returns cur+1).  `value == 0` is the TRUNCATE case reset
  to 1.  Hidden-PK row-id gets the same treatment for consistency.
*/
int ha_tidesdb::reset_auto_increment(ulonglong value)
{
    DBUG_ENTER("ha_tidesdb::reset_auto_increment");
    if (!share) DBUG_RETURN(0);

    ulonglong new_val = value > 0 ? value - 1 : 0;
    share->auto_inc_val.store(new_val, std::memory_order_relaxed);

    /* Hidden PK row-ids are one-based (delete_all_rows stores
       HIDDEN_PK_FIRST_ROW_ID for empty tables).  Treat value==0 as restart. */
    uint64_t new_rowid = value > 0 ? (uint64_t)value : HIDDEN_PK_FIRST_ROW_ID;
    share->next_row_id.store(new_rowid, std::memory_order_relaxed);

    DBUG_RETURN(0);
}

/* ******************** Table scan (SELECT) ******************** */

int ha_tidesdb::rnd_init(bool scan)
{
    DBUG_ENTER("ha_tidesdb::rnd_init");

    current_pk_len_ = 0;
    scan_dir_ = DIR_NONE;

    /* Lazy txn, we ensure stmt_txn exists */
    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(erc);
    }
    scan_txn = stmt_txn;

    /* We use cached trx pointer (set in external_lock) to avoid
       ha_thd() virtual dispatch + thd_get_ha_data() hash lookup
       on every scan init -- this is a hot path in nested-loop joins. */
    uint64_t cur_gen = cached_trx_ ? cached_trx_->txn_generation : 0;

    if (scan_iter &&
        (scan_iter_cf_ != share->cf || scan_iter_txn_ != scan_txn || scan_iter_txn_gen_ != cur_gen))
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }

    if (!scan_iter)
    {
        int rc = tdb_iter_new_blocking(ha_thd(), scan_txn, share->cf, &scan_iter);
        if (rc != TDB_SUCCESS)
        {
            scan_txn = NULL;
            DBUG_RETURN(tdb_rc_to_ha(rc, "rnd_init txn_begin"));
        }
        scan_iter_cf_ = share->cf;
        scan_iter_txn_ = scan_txn;
        scan_iter_txn_gen_ = cur_gen;
    }

    uint8_t data_prefix = KEY_NS_DATA;
    tidesdb_iter_seek(scan_iter, &data_prefix, 1);

    DBUG_RETURN(0);
}

int ha_tidesdb::rnd_end()
{
    DBUG_ENTER("ha_tidesdb::rnd_end");

    /* We do not free scan_iter, we keep cached for reuse within this statement.
       Iterator is freed in external_lock(F_UNLCK) or close(). */
    scan_txn = NULL;

    DBUG_RETURN(0);
}

int ha_tidesdb::rnd_next(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::rnd_next");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* We advance past the last-read entry.  on the first call after rnd_init
     * the iterator is already positioned at the first data key by the seek
     * in rnd_init, so we skip the advance (scan_dir_ == DIR_NONE). */
    if (scan_dir_ != DIR_NONE) tidesdb_iter_next(scan_iter);

    int ret = iter_read_current(buf);
    if (ret == 0) scan_dir_ = DIR_FORWARD;

    DBUG_RETURN(ret);
}

/* ******************** position / rnd_pos ******************** */

void ha_tidesdb::position(const uchar *record)
{
    DBUG_ENTER("ha_tidesdb::position");
    memcpy(ref, current_pk_buf_, current_pk_len_);
    DBUG_VOID_RETURN;
}

int ha_tidesdb::rnd_pos(uchar *buf, uchar *pos)
{
    DBUG_ENTER("ha_tidesdb::rnd_pos");

    /* Lazy txn, we ensure stmt_txn exists */
    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(erc);
    }

    int ret = fetch_row_by_pk(stmt_txn, pos, ref_length, buf);
    DBUG_RETURN(ret);
}

/* ******************** Index scan ******************** */

int ha_tidesdb::index_init(uint idx, bool sorted)
{
    DBUG_ENTER("ha_tidesdb::index_init");
    active_index = idx;
    idx_pk_exact_done_ = false;
    scan_dir_ = DIR_NONE;
    spatial_scan_active_ = false;
    /* Cache is_pk for the duration of the scan so navigation methods can
       read a member instead of re-deriving the answer per row. */
    is_pk_ = share->has_user_pk && idx == share->pk_index;

    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(erc);
    }
    scan_txn = stmt_txn;

    tidesdb_column_family_t *target_cf;
    if (share->has_user_pk && idx == share->pk_index)
        target_cf = share->cf;
    else if (idx < share->idx_cfs.size() && share->idx_cfs[idx])
        target_cf = share->idx_cfs[idx];
    else
    {
        scan_txn = NULL;
        scan_cf_ = NULL;
        sql_print_error("[TIDESDB] index_init: no CF for index %u", idx);
        DBUG_RETURN(HA_ERR_GENERIC);
    }

    scan_cf_ = target_cf;

    /* We reuse cached iterator if it belongs to the same CF and same txn.
       In nested-loop joins, index_init/index_end cycle N times on the
       same index; reusing the iterator avoids N expensive iter_new() calls
       (each builds a merge heap from all SSTables).

       If the txn changed (e.g. after COMMIT created a new one), the
       iterator holds a stale txn pointer and must be recreated.
       We compare both the pointer and a monotonic generation counter
       because the allocator can reuse the same address for a new txn.

       We use cached_trx_ (set in external_lock) to avoid ha_thd() virtual
       dispatch + thd_get_ha_data() hash lookup on every iteration of
       the outer loop in nested-loop joins. */
    uint64_t cur_gen = cached_trx_ ? cached_trx_->txn_generation : 0;

    if (scan_iter &&
        (scan_iter_cf_ != target_cf || scan_iter_txn_ != scan_txn || scan_iter_txn_gen_ != cur_gen))
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    /* If scan_iter is non-NULL here, ensure_scan_iter() will reuse it. */

    DBUG_RETURN(0);
}

/*
  Lazily create the scan iterator from scan_cf_ when first needed.
  Returns 0 on success or a handler error code.
*/
int ha_tidesdb::ensure_scan_iter()
{
    if (scan_iter) return 0;

    /* If a prior attempt with this exact (scan_cf_, scan_txn) combination
       already failed, short-circuit instead of re-logging and re-failing.
       The cache is invalidated whenever the caller changes scan_cf_ or
       scan_txn (natural since those moves imply a new attempt). */
    if (scan_iter_last_err_ && scan_iter_last_err_cf_ == scan_cf_ &&
        scan_iter_last_err_txn_ == scan_txn)
        return scan_iter_last_err_;

    if (!scan_txn || !scan_cf_)
    {
        sql_print_error("[TIDESDB] ensure_scan_iter: no txn or CF");
        scan_iter_last_err_ = HA_ERR_GENERIC;
        scan_iter_last_err_cf_ = scan_cf_;
        scan_iter_last_err_txn_ = scan_txn;
        return HA_ERR_GENERIC;
    }
    int rc = tdb_iter_new_blocking(ha_thd(), scan_txn, scan_cf_, &scan_iter);
    if (rc == TDB_SUCCESS)
    {
        scan_iter_cf_ = scan_cf_;
        scan_iter_txn_ = scan_txn;
        scan_iter_txn_gen_ = cached_trx_ ? cached_trx_->txn_generation : 0;
        scan_iter_last_err_ = 0;
        return 0;
    }
    int herr = tdb_rc_to_ha(rc, "ensure_scan_iter");
    scan_iter_last_err_ = herr;
    scan_iter_last_err_cf_ = scan_cf_;
    scan_iter_last_err_txn_ = scan_txn;
    return herr;
}

int ha_tidesdb::index_end()
{
    DBUG_ENTER("ha_tidesdb::index_end");

    scan_txn = NULL;
    active_index = MAX_KEY;
    spatial_scan_active_ = false;
    pk_partial_exact_active_ = false;

    DBUG_RETURN(0);
}

int ha_tidesdb::index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                               enum ha_rkey_function find_flag)
{
    DBUG_ENTER("ha_tidesdb::index_read_map");

    /* key_copy_to_comparable uses key_restore + make_comparable_key,
       which reads fields via make_sort_key_part. */
    MY_BITMAP *old_map = tmp_use_all_columns(table, &table->read_set);

    uint key_len = calculate_key_len(table, active_index, key, keypart_map);

    /* We convert the key_copy-format search key to our comparable format */
    KEY *ki = &table->key_info[active_index];
    uchar comp_key[MAX_KEY_LENGTH];
    uint comp_len = key_copy_to_comparable(ki, key, key_len, comp_key);

    tmp_restore_column_map(&table->read_set, old_map);

    memcpy(idx_search_comp_, comp_key, comp_len);
    idx_search_comp_len_ = comp_len;
    /* Reset by default; only the partial-PK exact branch below re-sets it. */
    pk_partial_exact_active_ = false;

    if (is_pk_)
    {
        uchar seek_key[DATA_KEY_BUF_LEN];
        uint seek_len = build_data_key(comp_key, comp_len, seek_key);

        if (find_flag == HA_READ_KEY_EXACT)
        {
            uint full_pk_comp_len = share->idx_comp_key_len[share->pk_index];
            if (comp_len >= full_pk_comp_len)
            {
                /* Full PK match, point lookup only, no iterator needed.
                   Pessimistic row locking happens inside fetch_row_by_pk
                   (covers the autocommit UPDATE bypass case as well, since
                   stmt_has_write_lock_ gates write-intent reads regardless
                   of multi-statement context). */
                int ret = fetch_row_by_pk(scan_txn, comp_key, comp_len, buf);
                if (ret == 0) idx_pk_exact_done_ = true;
                DBUG_RETURN(ret);
            }

            /* Partial PK prefix (e.g. first column of composite PK).
               We need an iterator-based prefix scan -- seek to the first
               matching data key and let index_next_same iterate through
               all entries sharing this prefix. */
            {
                int irc = ensure_scan_iter();
                if (irc) DBUG_RETURN(irc);
            }
            tidesdb_iter_seek(scan_iter, seek_key, seek_len);
            pk_partial_exact_active_ = true;
            int ret = iter_read_current(buf);
            if (ret == 0) scan_dir_ = DIR_FORWARD;
            DBUG_RETURN(ret);
        }

        /* All other PK scan modes need the iterator */
        {
            int irc = ensure_scan_iter();
            if (irc) DBUG_RETURN(irc);
        }

        if (find_flag == HA_READ_KEY_OR_NEXT || find_flag == HA_READ_AFTER_KEY)
        {
            tidesdb_iter_seek(scan_iter, seek_key, seek_len);

            if (find_flag == HA_READ_AFTER_KEY && tidesdb_iter_valid(scan_iter))
            {
                uint8_t *ik = NULL;
                size_t iks = 0;
                if (tidesdb_iter_key(scan_iter, &ik, &iks) == TDB_SUCCESS && iks == seek_len &&
                    memcmp(ik, seek_key, iks) == 0)
                    tidesdb_iter_next(scan_iter);
            }

            int ret = iter_read_current(buf);
            if (ret == 0) scan_dir_ = DIR_FORWARD;
            DBUG_RETURN(ret);
        }
        else if (find_flag == HA_READ_KEY_OR_PREV || find_flag == HA_READ_BEFORE_KEY ||
                 find_flag == HA_READ_PREFIX_LAST || find_flag == HA_READ_PREFIX_LAST_OR_PREV)
        {
            tidesdb_iter_seek_for_prev(scan_iter, seek_key, seek_len);
            if (find_flag == HA_READ_BEFORE_KEY && tidesdb_iter_valid(scan_iter))
            {
                uint8_t *ik = NULL;
                size_t iks = 0;
                if (tidesdb_iter_key(scan_iter, &ik, &iks) == TDB_SUCCESS && iks == seek_len &&
                    memcmp(ik, seek_key, iks) == 0)
                    tidesdb_iter_prev(scan_iter);
            }

            int ret = iter_read_current(buf);
            if (ret == 0) scan_dir_ = DIR_BACKWARD;
            DBUG_RETURN(ret);
        }

        /* Fallback is to seek forward */
        tidesdb_iter_seek(scan_iter, seek_key, seek_len);
        int ret = iter_read_current(buf);
        if (ret == 0) scan_dir_ = DIR_FORWARD;
        DBUG_RETURN(ret);
    }
    else
    {
        /* -- Spatial index MBR query, hilbert range scan with MBR post-filter */
        if (is_spatial_index(&table->key_info[active_index]) && find_flag >= HA_READ_MBR_CONTAIN &&
            find_flag <= HA_READ_MBR_EQUAL)
        {
            tdb_mbr_t qmbr;
            spatial_parse_query_mbr(key, &qmbr);
            spatial_qmbr_[MBR_XMIN_IDX] = qmbr.xmin;
            spatial_qmbr_[MBR_YMIN_IDX] = qmbr.ymin;
            spatial_qmbr_[MBR_XMAX_IDX] = qmbr.xmax;
            spatial_qmbr_[MBR_YMAX_IDX] = qmbr.ymax;
            spatial_mode_ = find_flag;

            spatial_scan_active_ = true;

            int irc = ensure_scan_iter();
            if (irc) DBUG_RETURN(irc);

            /* We decompose the query box into hilbert curve ranges.
               For DISJOINT, we must scan everything (disjoint entries
               can be anywhere on the curve). For other predicates,
               we compute a tight set of ranges covering only the cells
               that overlap the query box. */
            if (find_flag == HA_READ_MBR_DISJOINT)
            {
                spatial_ranges_.clear();
                spatial_ranges_.push_back({HILBERT_RANGE_FULL_LO, HILBERT_RANGE_FULL_HI});
            }
            else
            {
                uint32_t qx0 = double_to_lex_uint32(qmbr.xmin);
                uint32_t qy0 = double_to_lex_uint32(qmbr.ymin);
                uint32_t qx1 = double_to_lex_uint32(qmbr.xmax);
                uint32_t qy1 = double_to_lex_uint32(qmbr.ymax);
                spatial_decompose_ranges(qx0, qy0, qx1, qy1, spatial_ranges_);
            }
            spatial_range_idx_ = 0;

            if (!spatial_ranges_.empty())
            {
                uchar seek_key[SPATIAL_HILBERT_KEY_LEN];
                encode_hilbert_be(spatial_ranges_[0].first, seek_key);
                tidesdb_iter_seek(scan_iter, seek_key, SPATIAL_HILBERT_KEY_LEN);
            }

            DBUG_RETURN(spatial_scan_next(buf));
        }

        /* Secondary index read, needs an iterator */
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);

        if (find_flag == HA_READ_KEY_EXACT || find_flag == HA_READ_KEY_OR_NEXT)
        {
            tidesdb_iter_seek(scan_iter, comp_key, comp_len);
        }
        else if (find_flag == HA_READ_AFTER_KEY)
        {
            /* We seek, then skip past any exact prefix matches */
            tidesdb_iter_seek(scan_iter, comp_key, comp_len);
            while (tidesdb_iter_valid(scan_iter))
            {
                uint8_t *ik = NULL;
                size_t iks = 0;
                if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) break;
                if (iks < comp_len || memcmp(ik, comp_key, comp_len) != 0) break;
                tidesdb_iter_next(scan_iter);
            }
        }
        else if (find_flag == HA_READ_KEY_OR_PREV || find_flag == HA_READ_BEFORE_KEY ||
                 find_flag == HA_READ_PREFIX_LAST || find_flag == HA_READ_PREFIX_LAST_OR_PREV)
        {
            /* We build upper bound, comp_key with all 0xFF appended for pk portion */
            uchar upper[SEC_IDX_KEY_BUF_LEN];
            memcpy(upper, comp_key, comp_len);
            memset(upper + comp_len, KEY_INF_HI_BYTE, share->pk_key_len);
            uint upper_len = comp_len + share->pk_key_len;
            tidesdb_iter_seek_for_prev(scan_iter, upper, upper_len);
        }
        else
        {
            tidesdb_iter_seek(scan_iter, comp_key, comp_len);
        }

        /* We read the current entry from the secondary index.
           ICP loop, we evaluate pushed index condition before the expensive
           PK point-lookup.  Entries that fail the condition are skipped
           without touching the data CF (same pattern as InnoDB). */
        bool is_backward =
            (find_flag == HA_READ_KEY_OR_PREV || find_flag == HA_READ_BEFORE_KEY ||
             find_flag == HA_READ_PREFIX_LAST || find_flag == HA_READ_PREFIX_LAST_OR_PREV);

        uint idx_col_len = share->idx_comp_key_len[active_index];

        for (;;)
        {
            if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);

            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);

            /* For EXACT match, we verify the index prefix matches */
            if (find_flag == HA_READ_KEY_EXACT)
            {
                if (iks < comp_len || memcmp(ik, comp_key, comp_len) != 0)
                    DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);
            }

            if (iks <= idx_col_len) DBUG_RETURN(HA_ERR_KEY_NOT_FOUND);

            /* ICP -- we evaluate pushed condition on index columns before PK lookup */
            check_result_t icp = icp_check_secondary(ik, iks, active_index, buf);
            if (icp == CHECK_NEG)
            {
                if (is_backward)
                    tidesdb_iter_prev(scan_iter);
                else
                    tidesdb_iter_next(scan_iter);
                continue; /* skip this entry */
            }
            if (icp == CHECK_OUT_OF_RANGE) DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (icp == CHECK_ABORTED_BY_USER) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

            /* CHECK_POS -- condition satisfied (or ICP not applicable) */
            int ret;
            if (keyread_only_ && try_keyread_from_index(ik, iks, active_index, buf))
                ret = 0;
            else
                ret = fetch_row_by_pk(scan_txn, ik + idx_col_len, (uint)(iks - idx_col_len), buf);
            if (ret == 0)
            {
                scan_dir_ = is_backward ? DIR_BACKWARD : DIR_FORWARD;
            }
            DBUG_RETURN(ret);
        }
    }
}

int ha_tidesdb::index_next(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::index_next");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* Spatial idx continuation */
    if (spatial_scan_active_)
    {
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
        if (scan_dir_ != DIR_NONE) tidesdb_iter_next(scan_iter);
        DBUG_RETURN(spatial_scan_next(buf));
    }

    if (idx_pk_exact_done_)
    {
        idx_pk_exact_done_ = false;
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
        uchar seek_key[DATA_KEY_BUF_LEN];
        uint seek_len = build_data_key(current_pk_buf_, current_pk_len_, seek_key);
        tidesdb_iter_seek(scan_iter, seek_key, seek_len);
        if (tidesdb_iter_valid(scan_iter)) tidesdb_iter_next(scan_iter);
        /* iterator is now past the PK exact match -- advance+read below */
    }
    else
    {
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
        /* We advance past the last-read entry (iterator stays at current
         * with no pre-advance).  On the first call after index_first
         * sets DIR_NONE, the iterator is already at the correct position
         * so we must not advance. */
        if (scan_dir_ != DIR_NONE) tidesdb_iter_next(scan_iter);
    }

    if (is_pk_)
    {
        int ret = iter_read_current(buf);
        if (ret == 0 && pk_partial_exact_active_ && idx_search_comp_len_ > 0)
        {
            /* Continuation of a partial-PK HA_READ_KEY_EXACT scan: the
               iterator might have stepped past the prefix.  Validate the
               PK still starts with the original search bytes; if not the
               scan is finished.  index_next_same already does this; the
               PK branch never did, so a plan that called index_next
               (not index_next_same) after a partial-PK exact seek would
               return rows from beyond the requested prefix. */
            if (current_pk_len_ < idx_search_comp_len_ ||
                memcmp(current_pk_buf_, idx_search_comp_, idx_search_comp_len_) != 0)
            {
                DBUG_RETURN(HA_ERR_END_OF_FILE);
            }
        }
        scan_dir_ = DIR_FORWARD;
        DBUG_RETURN(ret);
    }
    else
    {
        /* Secondary index -- ICP loop -- we skip entries that fail the pushed
           condition without the expensive PK point-lookup. */
        uint idx_key_len = share->idx_comp_key_len[active_index];
        for (;;)
        {
            if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_END_OF_FILE);

            if (iks <= idx_key_len) DBUG_RETURN(HA_ERR_END_OF_FILE);

            /* ICP -- we evaluate pushed condition before PK lookup */
            check_result_t icp = icp_check_secondary(ik, iks, active_index, buf);
            if (icp == CHECK_NEG)
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }
            if (icp == CHECK_OUT_OF_RANGE) DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (icp == CHECK_ABORTED_BY_USER) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

            int ret;
            if (keyread_only_ && try_keyread_from_index(ik, iks, active_index, buf))
                ret = 0;
            else
                ret = fetch_row_by_pk(scan_txn, ik + idx_key_len, (uint)(iks - idx_key_len), buf);
            scan_dir_ = DIR_FORWARD;
            DBUG_RETURN(ret);
        }
    }
}

int ha_tidesdb::index_prev(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::index_prev");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* If PK exact match was done without iterator, we create it now and
       seek to the matched key so that prev() steps before it. */
    if (idx_pk_exact_done_)
    {
        idx_pk_exact_done_ = false;
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
        uchar seek_key[DATA_KEY_BUF_LEN];
        uint seek_len = build_data_key(current_pk_buf_, current_pk_len_, seek_key);
        tidesdb_iter_seek(scan_iter, seek_key, seek_len);
        /* iterator is at the matched key -- fall through to prev() */
    }
    else
    {
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);
    }

    tidesdb_iter_prev(scan_iter);

    if (is_pk_)
    {
        while (tidesdb_iter_valid(scan_iter))
        {
            uint8_t *key = NULL;
            size_t ks = 0;
            if (tidesdb_iter_key(scan_iter, &key, &ks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (is_data_key(key, ks)) break;
            tidesdb_iter_prev(scan_iter);
        }
        scan_dir_ = DIR_BACKWARD;
        DBUG_RETURN(iter_read_current(buf));
    }
    else
    {
        /* Secondary index -- ICP loop (backward direction) */
        uint idx_key_len = share->idx_comp_key_len[active_index];
        for (;;)
        {
            if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_END_OF_FILE);

            if (iks <= idx_key_len) DBUG_RETURN(HA_ERR_END_OF_FILE);

            /* ICP -- we evaluate pushed condition before PK lookup */
            check_result_t icp = icp_check_secondary(ik, iks, active_index, buf);
            if (icp == CHECK_NEG)
            {
                tidesdb_iter_prev(scan_iter);
                continue;
            }
            if (icp == CHECK_OUT_OF_RANGE) DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (icp == CHECK_ABORTED_BY_USER) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

            scan_dir_ = DIR_BACKWARD;
            int ret;
            if (keyread_only_ && try_keyread_from_index(ik, iks, active_index, buf))
                ret = 0;
            else
                ret = fetch_row_by_pk(scan_txn, ik + idx_key_len, (uint)(iks - idx_key_len), buf);
            DBUG_RETURN(ret);
        }
    }
}

int ha_tidesdb::index_first(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::index_first");

    idx_pk_exact_done_ = false;
    int irc = ensure_scan_iter();
    if (irc) DBUG_RETURN(irc);

    if (is_pk_)
    {
        uint8_t data_prefix = KEY_NS_DATA;
        tidesdb_iter_seek(scan_iter, &data_prefix, 1);
        int ret = iter_read_current(buf);
        if (ret == 0) scan_dir_ = DIR_FORWARD;
        DBUG_RETURN(ret);
    }
    else
    {
        tidesdb_iter_seek_to_first(scan_iter);
        scan_dir_ = DIR_NONE; /* index_next will set DIR_FORWARD */
        DBUG_RETURN(index_next(buf));
    }
}

int ha_tidesdb::index_last(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::index_last");

    idx_pk_exact_done_ = false;
    int irc = ensure_scan_iter();
    if (irc) DBUG_RETURN(irc);

    if (is_pk_)
    {
        /* Seek-for-prev(sentinel) lands on the last existing data key in one
           operation, where seek_to_last walks every source's max key first
           and then we'd still need a backward scan past KEY_NS_META.
           KEY_NS_DATA (0x01) sorts after KEY_NS_META (0x00) so any data
           key is greater than every meta key, and the sentinel below is
           larger than any real data key in the CF. */
        uchar sentinel[DATA_KEY_BUF_LEN];
        sentinel[0] = KEY_NS_DATA;
        uint sentinel_len = KEY_NAMESPACE_LEN + share->pk_key_len;
        if (sentinel_len > sizeof(sentinel)) sentinel_len = sizeof(sentinel);
        memset(sentinel + KEY_NAMESPACE_LEN, KEY_INF_HI_BYTE, sentinel_len - KEY_NAMESPACE_LEN);
        tidesdb_iter_seek_for_prev(scan_iter, sentinel, sentinel_len);
        /* Defensive backward scan in case the seek lands on a meta key
           in a CF with no data rows yet. */
        while (tidesdb_iter_valid(scan_iter))
        {
            uint8_t *key = NULL;
            size_t ks = 0;
            if (tidesdb_iter_key(scan_iter, &key, &ks) != TDB_SUCCESS)
                DBUG_RETURN(HA_ERR_END_OF_FILE);
            if (is_data_key(key, ks)) break;
            tidesdb_iter_prev(scan_iter);
        }
        scan_dir_ = DIR_BACKWARD;
        DBUG_RETURN(iter_read_current(buf));
    }
    else
    {
        /* Secondary CFs hold only index entries; seek_for_prev(0xFF...)
           lands on the last one without the per-source max-key walk that
           seek_to_last performs. */
        uchar sentinel[SEC_IDX_KEY_BUF_LEN];
        uint sentinel_len = share->idx_comp_key_len[active_index] + share->pk_key_len;
        if (sentinel_len > sizeof(sentinel)) sentinel_len = sizeof(sentinel);
        memset(sentinel, KEY_INF_HI_BYTE, sentinel_len);
        tidesdb_iter_seek_for_prev(scan_iter, sentinel, sentinel_len);
        if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

        uint8_t *ik = NULL;
        size_t iks = 0;
        if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) DBUG_RETURN(HA_ERR_END_OF_FILE);

        uint idx_key_len = share->idx_comp_key_len[active_index];
        if (iks <= idx_key_len) DBUG_RETURN(HA_ERR_END_OF_FILE);

        scan_dir_ = DIR_BACKWARD;
        DBUG_RETURN(fetch_row_by_pk(scan_txn, ik + idx_key_len, (uint)(iks - idx_key_len), buf));
    }
}

int ha_tidesdb::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
    DBUG_ENTER("ha_tidesdb::index_next_same");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* Spatial index continuation */
    if (spatial_scan_active_)
    {
        if (!scan_iter) DBUG_RETURN(HA_ERR_END_OF_FILE);
        tidesdb_iter_next(scan_iter);
        DBUG_RETURN(spatial_scan_next(buf));
    }

    if (is_pk_)
    {
        uint full_pk_comp_len = share->idx_comp_key_len[share->pk_index];
        if (idx_search_comp_len_ >= full_pk_comp_len)
        {
            /* Full PK is unique -- after the first match there are no more */
            DBUG_RETURN(HA_ERR_END_OF_FILE);
        }

        /* Partial PK prefix on a composite PK -- we iterate through data keys
           that share this prefix-- KEY_NS_DATA + comparable_pk_prefix... */
        if (!scan_iter) DBUG_RETURN(HA_ERR_END_OF_FILE);

        tidesdb_iter_next(scan_iter);
        if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

        uint8_t *ik = NULL;
        size_t iks = 0;
        if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) DBUG_RETURN(HA_ERR_END_OF_FILE);

        /* Data key format-- KEY_NS_DATA + comparable_pk.
           We check if the PK prefix still matches (skip the namespace byte). */
        if (iks < KEY_NAMESPACE_LEN + idx_search_comp_len_ ||
            memcmp(ik + KEY_NAMESPACE_LEN, idx_search_comp_, idx_search_comp_len_) != 0)
            DBUG_RETURN(HA_ERR_END_OF_FILE);

        int ret = iter_read_current(buf);
        if (ret == 0) scan_dir_ = DIR_FORWARD;
        DBUG_RETURN(ret);
    }

    /* Secondary index -- we advance past the last-read entry, then ICP loop */
    if (!scan_iter) DBUG_RETURN(HA_ERR_END_OF_FILE);
    tidesdb_iter_next(scan_iter);

    uint idx_col_len = share->idx_comp_key_len[active_index];
    for (;;)
    {
        if (!tidesdb_iter_valid(scan_iter)) DBUG_RETURN(HA_ERR_END_OF_FILE);

        uint8_t *ik = NULL;
        size_t iks = 0;
        if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) DBUG_RETURN(HA_ERR_END_OF_FILE);

        if (iks < idx_search_comp_len_ || memcmp(ik, idx_search_comp_, idx_search_comp_len_) != 0)
        {
            DBUG_RETURN(HA_ERR_END_OF_FILE);
        }

        if (iks <= idx_col_len) DBUG_RETURN(HA_ERR_END_OF_FILE);

        /* ICP -- we evaluate pushed condition before PK lookup */
        check_result_t icp = icp_check_secondary(ik, iks, active_index, buf);
        if (icp == CHECK_NEG)
        {
            tidesdb_iter_next(scan_iter);
            continue;
        }
        if (icp == CHECK_OUT_OF_RANGE) DBUG_RETURN(HA_ERR_END_OF_FILE);
        if (icp == CHECK_ABORTED_BY_USER) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

        int ret;
        if (keyread_only_ && try_keyread_from_index(ik, iks, active_index, buf))
            ret = 0;
        else
            ret = fetch_row_by_pk(scan_txn, ik + idx_col_len, (uint)(iks - idx_col_len), buf);
        DBUG_RETURN(ret);
    }
}

/* ******************** update_row (UPDATE) ******************** */

int ha_tidesdb::update_row(const uchar *old_data, const uchar *new_data)
{
    DBUG_ENTER("ha_tidesdb::update_row");

    MY_BITMAP *old_map = tmp_use_all_columns(table, &table->read_set);

    /* We cache THD and trx once to avoid repeated ha_thd() virtual calls
       and thd_get_ha_data() indirect lookups throughout this function.
       We use cached_thd_/cached_trx_ set in external_lock to avoid
       per-row ha_thd() virtual dispatch and thd_get_ha_data() hash lookup. */
    tidesdb_trx_t *trx = cached_trx_;

    /* We use handler-owned pk buffer for old/new PK to avoid large stack arrays.
       old_pk is saved from current_pk_buf_ before we overwrite it. */
    uchar old_pk[MAX_KEY_LENGTH];
    uint old_pk_len = current_pk_len_;
    memcpy(old_pk, current_pk_buf_, old_pk_len);

    /* Acquire the X lock on the row we are about to mutate.  Under
       SELECT ... FOR UPDATE the scan already took it; under UPDATE
       reached via a secondary-index/PK range scan we skipped it during
       ICP filtering and reacquire here on the actual target.  The lock
       manager treats repeat acquisitions of an already-held mode as a
       cheap no-op so the FOR UPDATE path doesn't pay twice. */
    if (unlikely(srv_pessimistic_locking) && trx)
    {
        int lrc = row_lock_acquire(trx, old_pk, old_pk_len, cached_thd_, TDB_LOCK_MODE_X);
        if (lrc)
        {
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(lrc);
        }
    }

    /* new_pk uses its own stack buffer so it survives the current_pk_buf_
       manipulations in the secondary index loop (avoids overlapping memcpy UB) */
    uchar new_pk[MAX_KEY_LENGTH];
    uint new_pk_len = pk_from_record(new_data, new_pk);

    const std::string &new_row = serialize_row(new_data);
    if (share->encrypted && new_row.empty())
    {
        tmp_restore_column_map(&table->read_set, old_map);
        DBUG_RETURN(HA_ERR_GENERIC);
    }
    const uint8_t *row_ptr = (const uint8_t *)new_row.data();
    size_t row_len = new_row.size();

    {
        int erc = ensure_stmt_txn();
        if (erc)
        {
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(erc);
        }
    }
    tidesdb_txn_t *txn = stmt_txn;
    stmt_txn_dirty = true;
    if (trx)
    {
        trx->dirty = true;
    }

    /* We populate THDVAR cache if not yet done this statement */
    if (!cached_thdvars_valid_)
    {
        cached_skip_unique_ = THDVAR(cached_thd_, skip_unique_check);
        cached_sess_ttl_ = THDVAR(cached_thd_, ttl);
        cached_single_delete_primary_ = THDVAR(cached_thd_, single_delete_primary);
        cached_thdvars_valid_ = true;
    }

    int rc;
    bool pk_changed = (old_pk_len != new_pk_len || memcmp(old_pk, new_pk, old_pk_len) != 0);

    /* We compute TTL when the table has TTL configured or the session overrides it.
       Uses cached_sess_ttl_ to avoid THDVAR + ha_thd() per row. */
    time_t row_ttl =
        (share->has_ttl || cached_sess_ttl_ > 0) ? compute_row_ttl(new_data) : TIDESDB_TTL_NONE;

    /* Uniqueness enforcement.  A TidesDB put silently overwrites, so an
       UPDATE that moves a row onto an existing primary key would destroy
       the colliding row, and one that moves it onto an existing UNIQUE
       secondary value would create a duplicate.  The server relies on the
       engine to surface HA_ERR_FOUND_DUPP_KEY, so these checks run before
       any txn mutation and leave the txn untouched on a violation.  A
       session that set tidesdb_skip_unique_check bypasses them by caller
       contract, matching write_row. */
    if (!cached_skip_unique_)
    {
        if (pk_changed && share->has_user_pk)
        {
            uchar chk_dk[DATA_KEY_BUF_LEN];
            uint chk_dk_len = build_data_key(new_pk, new_pk_len, chk_dk);
            uint8_t *dup_val = NULL;
            size_t dup_len = 0;
            int grc = tidesdb_txn_get(txn, share->cf, chk_dk, chk_dk_len, &dup_val, &dup_len);
            if (grc == TDB_SUCCESS)
            {
                tidesdb_free(dup_val);
                errkey = lookup_errkey = share->pk_index;
                memcpy(dup_ref, new_pk, new_pk_len);
                tmp_restore_column_map(&table->read_set, old_map);
                DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
            }
            if (grc != TDB_ERR_NOT_FOUND)
            {
                tmp_restore_column_map(&table->read_set, old_map);
                DBUG_RETURN(tdb_rc_to_ha(grc, "update_row pk_dup_check"));
            }
        }

        if (share->num_secondary_indexes > 0)
        {
            const my_ptrdiff_t nd_ptrdiff = (my_ptrdiff_t)(new_data - table->record[0]);
            for (uint i = 0; i < table->s->keys; i++)
            {
                if (share->has_user_pk && i == share->pk_index) continue;
                if (i >= share->idx_cfs.size() || !share->idx_cfs[i]) continue;
                if (share->idx_is_fts[i] || share->idx_is_spatial[i]) continue;
                if (!(table->key_info[i].flags & HA_NOSAME)) continue;

                KEY *ki = &table->key_info[i];

                /* SQL gives NULL no identity, so a UNIQUE index never
                   constrains a row whose indexed value is NULL in any part.
                   Skip the check entirely in that case, matching InnoDB.
                   This also keeps the engine off the server's internal
                   MHNSW graph table, whose UNIQUE(tref) column is NULL for
                   the graph metadata rows. */
                bool any_null = false;
                for (uint p = 0; p < ki->user_defined_key_parts; p++)
                {
                    Field *f = ki->key_part[p].field;
                    if (f->real_maybe_null() && f->is_real_null(nd_ptrdiff))
                    {
                        any_null = true;
                        break;
                    }
                }
                if (any_null) continue;

                /* Compare the old and new comparable index keys.  Equal
                   keys mean the indexed value did not change, so no new
                   collision is possible no matter whether the primary key
                   moved.  When they differ, this row's own existing entry
                   sits under the old key, so any entry found under the new
                   key necessarily belongs to a different row. */
                uchar *old_prefix = upd_old_ik_;
                uchar *new_prefix = upd_new_ik_;
                uint old_prefix_len =
                    make_comparable_key(ki, old_data, ki->user_defined_key_parts, old_prefix);
                uint new_prefix_len =
                    make_comparable_key(ki, new_data, ki->user_defined_key_parts, new_prefix);
                if (old_prefix_len == new_prefix_len &&
                    memcmp(old_prefix, new_prefix, new_prefix_len) == 0)
                    continue;

                tidesdb_iter_t *dup_iter = NULL;
                int irc = tdb_iter_new_blocking(ha_thd(), txn, share->idx_cfs[i], &dup_iter);
                if (irc != TDB_SUCCESS || !dup_iter)
                {
                    tmp_restore_column_map(&table->read_set, old_map);
                    DBUG_RETURN(tdb_rc_to_ha(irc, "update_row dup_iter_new"));
                }

                tidesdb_iter_seek(dup_iter, new_prefix, new_prefix_len);
                bool dup = false;
                if (tidesdb_iter_valid(dup_iter))
                {
                    uint8_t *fk = NULL;
                    size_t fks = 0;
                    if (tidesdb_iter_key(dup_iter, &fk, &fks) == TDB_SUCCESS &&
                        fks >= new_prefix_len && memcmp(fk, new_prefix, new_prefix_len) == 0)
                    {
                        dup = true;
                        size_t suffix_len = fks - new_prefix_len;
                        if (suffix_len > 0 && suffix_len <= ref_length)
                            memcpy(dup_ref, fk + new_prefix_len, suffix_len);
                    }
                }
                tidesdb_iter_free(dup_iter);

                if (dup)
                {
                    errkey = lookup_errkey = i;
                    tmp_restore_column_map(&table->read_set, old_map);
                    DBUG_RETURN(HA_ERR_FOUND_DUPP_KEY);
                }
            }
        }
    }

    /* If PK changed, we delete old entry and insert new */
    if (pk_changed)
    {
        uchar old_dk[DATA_KEY_BUF_LEN];
        uint old_dk_len = build_data_key(old_pk, old_pk_len, old_dk);
        rc = tdb_txn_delete_cf_blocking(cached_thd_, txn, share->cf, old_dk, old_dk_len,
                                        cached_single_delete_primary_);
        if (rc != TDB_SUCCESS) goto err;
    }

    {
        uchar new_dk[DATA_KEY_BUF_LEN];
        uint new_dk_len = build_data_key(new_pk, new_pk_len, new_dk);
        rc = tdb_txn_put_blocking(cached_thd_, txn, share->cf, new_dk, new_dk_len, row_ptr, row_len,
                                  row_ttl);
        if (rc != TDB_SUCCESS) goto err;
    }

    /* Single consolidated dispatch over secondary indexes.  Regular, FTS,
       and spatial branches share one walk of table->s->keys.  Each branch
       short-circuits via a write_set pre-check so unchanged indexes skip
       both key construction and LSM writes. */
    if (share->num_secondary_indexes > 0)
    {
        /* We use handler-owned buffers to avoid per-row heap allocation
           and keep the stack frame within -Wframe-larger-than limits. */
        uchar *old_ik = upd_old_ik_;
        uchar *new_ik = upd_new_ik_;
        const uint num_keys = table->s->keys;
        const bool has_user_pk = share->has_user_pk;
        const uint pk_index = share->pk_index;
        const size_t idx_cfs_sz = share->idx_cfs.size();

        for (uint i = 0; i < num_keys; i++)
        {
            if (has_user_pk && i == pk_index) continue;
            if (i >= idx_cfs_sz || !share->idx_cfs[i]) continue;

            KEY *ki = &table->key_info[i];

            if (share->idx_is_fts[i])
            {
                /* We skip if no indexed column actually changed */
                bool fts_changed = false;
                for (uint p = 0; p < ki->user_defined_key_parts; p++)
                {
                    uint fieldnr = ki->key_part[p].fieldnr - 1;
                    if (bitmap_is_set(table->write_set, fieldnr))
                    {
                        fts_changed = true;
                        break;
                    }
                }
                if (!fts_changed) continue;

                CHARSET_INFO *fts_cs = ki->key_part[0].field->charset();

                /* Tokenize both old and new docs, build term-frequency maps,
                   then emit only the minimum set of deletes/puts needed.
                   For a small edit to a large document this avoids
                   rewriting every term entry. */
                std::vector<fts_token_t> old_tokens, new_tokens;
                fts_extract_and_tokenize(table, ki, old_data, fts_cs, old_tokens);
                fts_extract_and_tokenize(table, ki, new_data, fts_cs, new_tokens);

                std::unordered_map<std::string, uint16> old_tf, new_tf;
                for (auto &tok : old_tokens) old_tf[tok.word]++;
                for (auto &tok : new_tokens) new_tf[tok.word]++;
                uint32 old_wc = (uint32)old_tokens.size();
                uint32 new_wc = (uint32)new_tokens.size();

                if (pk_changed)
                {
                    /* PK changed -- the row identity changed so every old
                       (term, old_pk) must be deleted and every new (term, new_pk)
                       inserted.  No diffing possible across different PKs. */
                    for (auto &[term, tf] : old_tf)
                    {
                        uchar fk[FTS_KEY_BUF_LEN];
                        uint fk_len =
                            fts_build_key(term.data(), (uint)term.size(), old_pk, old_pk_len, fk);
                        tdb_txn_delete_cf_blocking(cached_thd_, txn, share->idx_cfs[i], fk, fk_len,
                                                   true);
                    }
                    for (auto &[term, tf] : new_tf)
                    {
                        uchar fk[FTS_KEY_BUF_LEN];
                        uint fk_len =
                            fts_build_key(term.data(), (uint)term.size(), new_pk, new_pk_len, fk);
                        uchar fv[FTS_VALUE_LEN];
                        fts_build_value(tf, new_wc, fv);
                        rc = tdb_txn_put_blocking(cached_thd_, txn, share->idx_cfs[i], fk, fk_len,
                                                  fv, FTS_VALUE_LEN, row_ttl);
                        if (rc != TDB_SUCCESS) goto err;
                    }
                }
                else
                {
                    /* PK stable -- apply term-level diff.  Only delete a term
                       when it disappears and only write a term when it is new,
                       its tf changes, or doc_len changes (doc_len is part of
                       the stored value used by BM25). */
                    bool doc_len_changed = (old_wc != new_wc);

                    for (auto &[term, old_cnt] : old_tf)
                    {
                        if (new_tf.find(term) != new_tf.end()) continue;
                        uchar fk[FTS_KEY_BUF_LEN];
                        uint fk_len =
                            fts_build_key(term.data(), (uint)term.size(), old_pk, old_pk_len, fk);
                        tdb_txn_delete_cf_blocking(cached_thd_, txn, share->idx_cfs[i], fk, fk_len,
                                                   true);
                    }

                    for (auto &[term, new_cnt] : new_tf)
                    {
                        auto it = old_tf.find(term);
                        bool need_put;
                        if (it == old_tf.end())
                            need_put = true;
                        else if (doc_len_changed)
                            need_put = true;
                        else
                            need_put = (it->second != new_cnt);

                        if (!need_put) continue;

                        uchar fk[FTS_KEY_BUF_LEN];
                        uint fk_len =
                            fts_build_key(term.data(), (uint)term.size(), new_pk, new_pk_len, fk);
                        uchar fv[FTS_VALUE_LEN];
                        fts_build_value(new_cnt, new_wc, fv);
                        rc = tdb_txn_put_blocking(cached_thd_, txn, share->idx_cfs[i], fk, fk_len,
                                                  fv, FTS_VALUE_LEN, row_ttl);
                        if (rc != TDB_SUCCESS) goto err;
                    }
                }

                /* The doc count stays the same, only the word count moves.
                   Fold into the txn-level accumulator which flushes before
                   commit so the meta update lands in the same txn as the
                   row updates that produced it. */
                int64_t wc_delta = (int64_t)new_wc - (int64_t)old_wc;
                if (wc_delta != 0) trx_fts_meta_accumulate(trx, share->cf, i, 0, wc_delta);
            }
            else if (share->idx_is_spatial[i])
            {
                /* Skip when the geometry column is unchanged. */
                uint fieldnr = ki->key_part[0].fieldnr - 1;
                if (!bitmap_is_set(table->write_set, fieldnr)) continue;

                Field *geom_field = ki->key_part[0].field;

                /* Delete old spatial entry */
                {
                    my_ptrdiff_t ptd = (my_ptrdiff_t)(old_data - table->record[0]);
                    if (ptd) geom_field->move_field_offset(ptd);
                    String gs;
                    geom_field->val_str(&gs, &gs);
                    if (ptd) geom_field->move_field_offset(-ptd);
                    double xmn, ymn, xmx, ymx;
                    if (gs.length() > 0 && spatial_compute_mbr((const uchar *)gs.ptr(), gs.length(),
                                                               &xmn, &ymn, &xmx, &ymx))
                    {
                        uchar sk[SPATIAL_HILBERT_KEY_LEN + MAX_KEY_LENGTH];
                        uint sk_len = spatial_build_key((xmn + xmx) / MBR_CENTROID_DIV,
                                                        (ymn + ymx) / MBR_CENTROID_DIV, old_pk,
                                                        old_pk_len, sk);
                        tdb_txn_delete_cf_blocking(cached_thd_, txn, share->idx_cfs[i], sk, sk_len,
                                                   true);
                    }
                }

                /* Insert new spatial entry */
                {
                    my_ptrdiff_t ptd = (my_ptrdiff_t)(new_data - table->record[0]);
                    if (ptd) geom_field->move_field_offset(ptd);
                    String gs;
                    geom_field->val_str(&gs, &gs);
                    if (ptd) geom_field->move_field_offset(-ptd);
                    double xmn, ymn, xmx, ymx;
                    if (gs.length() > 0 && spatial_compute_mbr((const uchar *)gs.ptr(), gs.length(),
                                                               &xmn, &ymn, &xmx, &ymx))
                    {
                        uchar sk[SPATIAL_HILBERT_KEY_LEN + MAX_KEY_LENGTH];
                        uint sk_len = spatial_build_key((xmn + xmx) / MBR_CENTROID_DIV,
                                                        (ymn + ymx) / MBR_CENTROID_DIV, new_pk,
                                                        new_pk_len, sk);
                        uchar sv[SPATIAL_MBR_VALUE_LEN];
                        spatial_build_value(xmn, ymn, xmx, ymx, sv);
                        rc = tdb_txn_put_blocking(cached_thd_, txn, share->idx_cfs[i], sk, sk_len,
                                                  sv, SPATIAL_MBR_VALUE_LEN, row_ttl);
                        if (rc != TDB_SUCCESS) goto err;
                    }
                }
            }
            else
            {
                /* Regular secondary index -- skip before building keys when
                   no indexed column changed and the PK is stable.  Saves
                   the per-row make_comparable_key / sec_idx_key cost on
                   wide updates that touch only unrelated columns. */
                if (!pk_changed)
                {
                    bool idx_changed = false;
                    for (uint p = 0; p < ki->user_defined_key_parts; p++)
                    {
                        uint fieldnr = ki->key_part[p].fieldnr - 1;
                        if (bitmap_is_set(table->write_set, fieldnr))
                        {
                            idx_changed = true;
                            break;
                        }
                    }
                    if (!idx_changed) continue;
                }

                /* We build old index entry key.  current_pk_buf_ is transiently
                   set to old/new PK so sec_idx_key's pk_from_record path works
                   for hidden-PK tables. */
                memcpy(current_pk_buf_, old_pk, old_pk_len);
                current_pk_len_ = old_pk_len;
                uint old_ik_len =
                    make_comparable_key(ki, old_data, ki->user_defined_key_parts, old_ik);
                memcpy(old_ik + old_ik_len, old_pk, old_pk_len);
                old_ik_len += old_pk_len;

                memcpy(current_pk_buf_, new_pk, new_pk_len);
                current_pk_len_ = new_pk_len;
                uint new_ik_len = sec_idx_key(i, new_data, new_ik);

                if (old_ik_len == new_ik_len && memcmp(old_ik, new_ik, old_ik_len) == 0) continue;

                rc = tdb_txn_delete_cf_blocking(cached_thd_, txn, share->idx_cfs[i], old_ik,
                                                old_ik_len, true);
                if (rc != TDB_SUCCESS) goto err;
                rc = tdb_txn_put_blocking(cached_thd_, txn, share->idx_cfs[i], new_ik, new_ik_len,
                                          &tdb_empty_val, sizeof(tdb_empty_val), row_ttl);
                if (rc != TDB_SUCCESS) goto err;
            }
        }
    }

    memcpy(current_pk_buf_, new_pk, new_pk_len);
    current_pk_len_ = new_pk_len;

    /* Bulk UPDATE mid-txn commit.  Symmetric to write_row's bulk path.
       One UPDATE op counts as 1 data put + 1 data delete (when PK changed)
       + up to num_secondary_indexes entries rewritten.  We overestimate as
       `1 + 2 * num_secondary_indexes` */
    if (in_bulk_update_)
    {
        bulk_insert_ops_ += 1 + 2 * (ha_rows)share->num_secondary_indexes;
        if (bulk_insert_ops_ >= TIDESDB_BULK_INSERT_BATCH_OPS)
        {
            int mrc = maybe_bulk_commit(trx);
            if (mrc)
            {
                tmp_restore_column_map(&table->read_set, old_map);
                DBUG_RETURN(mrc);
            }
            bulk_insert_ops_ = 0;
        }
    }

    /* Commit happens in external_lock(F_UNLCK). */
    tmp_restore_column_map(&table->read_set, old_map);
    DBUG_RETURN(0);

err:
    tmp_restore_column_map(&table->read_set, old_map);
    DBUG_RETURN(tdb_rc_to_ha(rc, "update_row"));
}

/* ******************** delete_row (DELETE) ******************** */

int ha_tidesdb::delete_row(const uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::delete_row");

    MY_BITMAP *old_map = tmp_use_all_columns(table, &table->read_set);

    /* We use cached_trx_ from external_lock to avoid per-row hash lookups. */
    tidesdb_trx_t *trx = cached_trx_;

    /* Acquire the X lock on the target row.  See update_row for the
       rationale-- iter_read_current skips the lock during UPDATE/DELETE
       ICP filtering so unrelated rows on a range scan are not blocked. */
    if (unlikely(srv_pessimistic_locking) && trx)
    {
        int lrc =
            row_lock_acquire(trx, current_pk_buf_, current_pk_len_, cached_thd_, TDB_LOCK_MODE_X);
        if (lrc)
        {
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(lrc);
        }
    }

    /* We populate THDVAR cache if not yet done this statement.  A pure DELETE
       reaches delete_row without first going through write_row/update_row, so
       the cache may still be stale from the prior statement. */
    if (!cached_thdvars_valid_)
    {
        cached_skip_unique_ = THDVAR(cached_thd_, skip_unique_check);
        cached_sess_ttl_ = THDVAR(cached_thd_, ttl);
        cached_single_delete_primary_ = THDVAR(cached_thd_, single_delete_primary);
        cached_compact_after_range_delete_min_rows_ =
            THDVAR(cached_thd_, compact_after_range_delete_min_rows);
        cached_thdvars_valid_ = true;
    }

    {
        int erc = ensure_stmt_txn();
        if (erc)
        {
            tmp_restore_column_map(&table->read_set, old_map);
            DBUG_RETURN(erc);
        }
    }
    tidesdb_txn_t *txn = stmt_txn;
    stmt_txn_dirty = true;
    if (trx)
    {
        trx->dirty = true;
    }

    uchar dk[DATA_KEY_BUF_LEN];
    uint dk_len = build_data_key(current_pk_buf_, current_pk_len_, dk);

    /* Track the touched data-key range when the auto-compact session var
       is on and we are inside a multi-row DELETE.  We compare the full
       data keys (KEY_NS_DATA + comparable_pk) so the recorded bounds can
       be passed to tidesdb_compact_range without further conversion. */
    if (in_bulk_delete_ && cached_compact_after_range_delete_min_rows_ > 0)
    {
        const std::string this_key((const char *)dk, dk_len);
        if (bulk_delete_rows_ == 0)
        {
            bulk_delete_min_pk_ = this_key;
            bulk_delete_max_pk_ = this_key;
        }
        else
        {
            if (this_key < bulk_delete_min_pk_) bulk_delete_min_pk_ = this_key;
            if (this_key > bulk_delete_max_pk_) bulk_delete_max_pk_ = this_key;
        }
        bulk_delete_rows_++;
    }

    int rc = tdb_txn_delete_cf_blocking(cached_thd_, txn, share->cf, dk, dk_len,
                                        cached_single_delete_primary_);
    if (rc != TDB_SUCCESS)
    {
        tmp_restore_column_map(&table->read_set, old_map);
        DBUG_RETURN(tdb_rc_to_ha(rc, "delete_row"));
    }

    /* We delete secondary index entries in a single consolidated dispatch loop.
       Regular, FTS, and spatial indexes are handled inline. */
    if (share->num_secondary_indexes > 0)
    {
        const uint num_keys = table->s->keys;
        const bool has_user_pk = share->has_user_pk;
        const uint pk_index = share->pk_index;
        const size_t idx_cfs_sz = share->idx_cfs.size();

        for (uint i = 0; i < num_keys; i++)
        {
            if (has_user_pk && i == pk_index) continue;
            if (i >= idx_cfs_sz || !share->idx_cfs[i]) continue;

            KEY *ki = &table->key_info[i];

            if (share->idx_is_fts[i])
            {
                CHARSET_INFO *fts_cs = ki->key_part[0].field->charset();
                std::vector<fts_token_t> fts_tokens;
                fts_extract_and_tokenize(table, ki, buf, fts_cs, fts_tokens);

                std::unordered_map<std::string, uint16> tf_map;
                for (auto &tok : fts_tokens) tf_map[tok.word]++;
                uint32 word_count = (uint32)fts_tokens.size();

                for (auto &[term, tf] : tf_map)
                {
                    uchar fk[FTS_KEY_BUF_LEN];
                    uint fk_len = fts_build_key(term.data(), (uint)term.size(), current_pk_buf_,
                                                current_pk_len_, fk);
                    tdb_txn_delete_cf_blocking(cached_thd_, txn, share->idx_cfs[i], fk, fk_len,
                                               true);
                }

                trx_fts_meta_accumulate(trx, share->cf, i, FTS_DOC_DELTA_DEL, -(int64_t)word_count);
            }
            else if (share->idx_is_spatial[i])
            {
                Field *geom_field = ki->key_part[0].field;
                my_ptrdiff_t ptd = (my_ptrdiff_t)(buf - table->record[0]);
                if (ptd) geom_field->move_field_offset(ptd);
                String geom_str;
                geom_field->val_str(&geom_str, &geom_str);
                if (ptd) geom_field->move_field_offset(-ptd);

                double xmin, ymin, xmax, ymax;
                if (geom_str.length() > 0 &&
                    spatial_compute_mbr((const uchar *)geom_str.ptr(), geom_str.length(), &xmin,
                                        &ymin, &xmax, &ymax))
                {
                    double cx = (xmin + xmax) / MBR_CENTROID_DIV;
                    double cy = (ymin + ymax) / MBR_CENTROID_DIV;
                    uchar sk[SPATIAL_HILBERT_KEY_LEN + MAX_KEY_LENGTH];
                    uint sk_len = spatial_build_key(cx, cy, current_pk_buf_, current_pk_len_, sk);
                    tdb_txn_delete_cf_blocking(cached_thd_, txn, share->idx_cfs[i], sk, sk_len,
                                               true);
                }
            }
            else
            {
                uchar ik[SEC_IDX_KEY_BUF_LEN];
                uint ik_len = sec_idx_key(i, buf, ik);
                rc = tdb_txn_delete_cf_blocking(cached_thd_, txn, share->idx_cfs[i], ik, ik_len,
                                                true);
                if (rc != TDB_SUCCESS)
                {
                    tmp_restore_column_map(&table->read_set, old_map);
                    DBUG_RETURN(tdb_rc_to_ha(rc, "delete_row idx"));
                }
            }
        }
    }

    /* Bulk DELETE mid-txn commit-- 1 data delete + num_secondary_indexes
       secondary-index deletes per row. */
    if (in_bulk_delete_)
    {
        bulk_insert_ops_ += 1 + (ha_rows)share->num_secondary_indexes;
        if (bulk_insert_ops_ >= TIDESDB_BULK_INSERT_BATCH_OPS)
        {
            int mrc = maybe_bulk_commit(trx);
            if (mrc)
            {
                tmp_restore_column_map(&table->read_set, old_map);
                DBUG_RETURN(mrc);
            }
            bulk_insert_ops_ = 0;
        }
    }

    tmp_restore_column_map(&table->read_set, old_map);
    DBUG_RETURN(0);
}

/* ******************** delete_all_rows (TRUNCATE) ******************** */

int ha_tidesdb::delete_all_rows(void)
{
    DBUG_ENTER("ha_tidesdb::delete_all_rows");

    /* We free cached iterators before dropping/recreating CFs.
       The iterators hold refs to SSTables in the CFs being dropped. */
    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();

    /* We discard the connection txn before drop/recreate.  The txn may have
       buffered INSERT/UPDATE ops from earlier statements; committing them
       after the CF is recreated would re-insert stale data. */
    {
        THD *thd = ha_thd();
        tidesdb_trx_t *trx = (tidesdb_trx_t *)thd_get_ha_data(thd, ht);
        if (trx && trx->txn)
        {
            tidesdb_txn_rollback(trx->txn);
            tidesdb_txn_free(trx->txn);
            trx->txn = NULL;
            trx->dirty = false;
            trx->fts_meta_pending.clear();
            trx->fts_meta_dirty = false;
        }
        stmt_txn = NULL;
        stmt_txn_dirty = false;
    }

    tidesdb_column_family_config_t cfg = build_cf_config(TDB_TABLE_OPTIONS(table));

    {
        std::string cf_name = share->cf_name;
        int rc = tidesdb_drop_column_family(tdb_global, cf_name.c_str());
        if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
        {
            sql_print_error("[TIDESDB] truncate: failed to drop CF '%s' (err=%d)", cf_name.c_str(),
                            rc);
            DBUG_RETURN(tdb_rc_to_ha(rc, "truncate drop_cf"));
        }

        rc = tidesdb_create_column_family(tdb_global, cf_name.c_str(), &cfg);
        if (rc != TDB_SUCCESS)
        {
            sql_print_error("[TIDESDB] truncate: failed to recreate CF '%s' (err=%d)",
                            cf_name.c_str(), rc);
            DBUG_RETURN(tdb_rc_to_ha(rc, "truncate create_cf"));
        }

        share->cf = tidesdb_get_column_family(tdb_global, cf_name.c_str());
        if (!share->cf)
        {
            sql_print_error("[TIDESDB] truncate: CF '%s' not found after recreate",
                            cf_name.c_str());
            DBUG_RETURN(HA_ERR_GENERIC);
        }
    }

    for (uint i = 0; i < share->idx_cfs.size(); i++)
    {
        if (!share->idx_cfs[i]) continue;

        const std::string &idx_name = share->idx_cf_names[i];
        tidesdb_drop_column_family(tdb_global, idx_name.c_str());

        tidesdb_column_family_config_t idx_cfg = cfg;
        if (i < table->s->keys && table->key_info[i].option_struct)
        {
            ha_index_option_struct *iopts = table->key_info[i].option_struct;
            idx_cfg.use_btree = iopts->use_btree ? 1 : 0;
        }

        int rc = tidesdb_create_column_family(tdb_global, idx_name.c_str(), &idx_cfg);
        if (rc != TDB_SUCCESS)
        {
            sql_print_warning("[TIDESDB] truncate: failed to recreate idx CF '%s' (err=%d)",
                              idx_name.c_str(), rc);
            share->idx_cfs[i] = NULL;
            continue;
        }

        share->idx_cfs[i] = tidesdb_get_column_family(tdb_global, idx_name.c_str());
    }

    share->next_row_id.store(HIDDEN_PK_FIRST_ROW_ID, std::memory_order_relaxed);

    DBUG_RETURN(0);
}

/* ******************** Bulk DML ******************** */

/*
  Commit the current txn mid-statement and reset it with READ_COMMITTED so
  the next batch starts fresh.  Shared by bulk INSERT/UPDATE/DELETE once
  buffered ops cross TIDESDB_BULK_INSERT_BATCH_OPS -- keeps us under
  TDB_MAX_TXN_OPS and bounds txn memory.  Higher isolation levels would
  cause unbounded read-set growth across batches.

  Any cached iterators and dup-check iterators are invalidated, they hold
  references to MERGE_SOURCE_TXN_OPS that txn_reset clears.

  If the inner commit fails (e.g. transient TDB_ERR_UNKNOWN from a unified
  memtable rotation race) we MUST surface that to the SQL layer.  Returning
  0 here while the buffered ops are gone causes silent data loss -- the
  caller (write_row / update_row / delete_row) reports success even though
  up to TIDESDB_BULK_INSERT_BATCH_OPS rows were dropped on the floor.
  Instead, rollback to release the txn's state, swap in a fresh txn so the
  connection is left in a valid state for any retry, and propagate the
  error code so MariaDB rolls the statement back and surfaces it to the
  client (typically as ER_ERROR_DURING_COMMIT).
*/
int ha_tidesdb::maybe_bulk_commit(tidesdb_trx_t *trx)
{
    if (!trx || !trx->txn) return 0;

    /* Folded FTS meta deltas have to land in the same txn as the row puts
       they account for, so flush them before the mid-statement commit. */
    int frc = flush_trx_fts_meta_pending(cached_thd_, trx);
    if (frc != TDB_SUCCESS) return tdb_rc_to_ha(frc, "bulk_commit fts_meta_flush");

    int crc = tdb_txn_commit_blocking(cached_thd_, trx->txn);
    if (crc != TDB_SUCCESS)
    {
        sql_print_error(
            "[TIDESDB] bulk mid-commit failed rc=%d -- aborting statement to "
            "avoid silent row loss",
            crc);
        /* Release plugin-level row locks.  The lock-request structs were
           allocated against the txn that just failed; leaving them on
           held_locks_head would expose dangling memory once we tidesdb_txn_free
           the underlying txn below.  After release we are safe to swap in a
           fresh txn. */
        row_locks_release_all(trx);
        /* Release the txn's buffered state.  Even if rollback itself fails
           we still free+begin below so the connection is usable. */
        (void)tidesdb_txn_rollback(trx->txn);
        tidesdb_txn_free(trx->txn);
        trx->txn = NULL;
        int brc =
            tidesdb_txn_begin_with_isolation(tdb_global, TDB_ISOLATION_READ_COMMITTED, &trx->txn);
        if (brc != TDB_SUCCESS) return tdb_rc_to_ha(brc, "bulk_commit txn_begin(after_fail)");
        trx->txn_generation++;
        stmt_txn = trx->txn;
        scan_txn = trx->txn;
        if (scan_iter)
        {
            tidesdb_iter_free(scan_iter);
            scan_iter = NULL;
            scan_iter_cf_ = NULL;
            scan_iter_txn_ = NULL;
        }
        free_dup_iter_cache();
        return tdb_rc_to_ha(crc, "bulk_commit");
    }

    /* Successful mid-statement commit.  The library has released its
       internal locks for the just-committed txn, so the plugin-level
       locks no longer correspond to anything serializable.  Drop them
       before the reset so a stalled cursor on the same connection cannot
       see locks attributed to a txn that no longer exists. */
    row_locks_release_all(trx);

    int rrc = tidesdb_txn_reset(trx->txn, TDB_ISOLATION_READ_COMMITTED);
    if (rrc != TDB_SUCCESS)
    {
        sql_print_warning(
            "[TIDESDB] bulk tidesdb_txn_reset failed (rc=%d), falling back to "
            "free+begin",
            rrc);
        tidesdb_txn_free(trx->txn);
        trx->txn = NULL;
        int rc =
            tidesdb_txn_begin_with_isolation(tdb_global, TDB_ISOLATION_READ_COMMITTED, &trx->txn);
        if (rc != TDB_SUCCESS) return tdb_rc_to_ha(rc, "bulk_commit txn_begin");
    }

    stmt_txn = trx->txn;
    trx->txn_generation++;

    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();
    scan_txn = trx->txn;
    return 0;
}

void ha_tidesdb::start_bulk_insert(ha_rows rows, uint flags)
{
    in_bulk_insert_ = true;
    bulk_insert_ops_ = 0;
}

int ha_tidesdb::end_bulk_insert()
{
    in_bulk_insert_ = false;
    return 0;
}

/*
  start_bulk_update returns 0 when the engine will handle bulk batching.
  We then flip the flag that update_row checks at its tail so every row
  contributes to the shared ops counter.
*/
bool ha_tidesdb::start_bulk_update()
{
    in_bulk_update_ = true;
    bulk_insert_ops_ = 0;
    return 0;
}

int ha_tidesdb::end_bulk_update()
{
    in_bulk_update_ = false;
    return 0;
}

/*
  MariaDB calls bulk_update_row instead of update_row when start_bulk_update
  returned 0.  We don't actually buffer rows (TidesDB's txn is the buffer);
  we just delegate so the standard update_row path runs and its tail-side
  mid-commit block batches.  dup_key_found tracks duplicate-key collisions
  found in buffered-but-not-yet-applied rows -- since we apply immediately,
  it's always zero.
*/
int ha_tidesdb::bulk_update_row(const uchar *old_data, const uchar *new_data,
                                ha_rows *dup_key_found)
{
    DBUG_ENTER("ha_tidesdb::bulk_update_row");
    if (dup_key_found) *dup_key_found = 0;
    DBUG_RETURN(update_row(old_data, new_data));
}

bool ha_tidesdb::start_bulk_delete()
{
    in_bulk_delete_ = true;
    bulk_insert_ops_ = 0;
    bulk_delete_rows_ = 0;
    bulk_delete_min_pk_.clear();
    bulk_delete_max_pk_.clear();
    return 0;
}

int ha_tidesdb::end_bulk_delete()
{
    in_bulk_delete_ = false;

    /* Auto compact-after-range-delete.  Threshold zero (default) keeps the
       previous behavior, i.e. no synchronous compaction at end-of-statement.
       When the threshold is met we call tidesdb_compact_range over the
       observed [min_pk, max_pk] data-key range on the primary CF.  Secondary
       index tombstones are reclaimed by the per-CF tombstone_density_trigger
       on those CFs. */
    if (cached_compact_after_range_delete_min_rows_ > 0 &&
        bulk_delete_rows_ >= cached_compact_after_range_delete_min_rows_ && share && share->cf &&
        !bulk_delete_min_pk_.empty() && !bulk_delete_max_pk_.empty())
    {
        int crc = tidesdb_compact_range(
            share->cf, (const uint8_t *)bulk_delete_min_pk_.data(), bulk_delete_min_pk_.size(),
            (const uint8_t *)bulk_delete_max_pk_.data(), bulk_delete_max_pk_.size());
        /* TDB_ERR_LOCKED is benign here -- another compaction is already
           running over a superset of our range, so our reclamation
           request will be absorbed by it.  Only log real failures. */
        if (crc != TDB_SUCCESS && crc != TDB_ERR_LOCKED)
        {
            sql_print_warning(
                "[TIDESDB] post-DELETE compact_range on '%s' failed (rows=%llu, err=%d)",
                share->cf_name.c_str(), (unsigned long long)bulk_delete_rows_, crc);
        }
    }

    bulk_delete_rows_ = 0;
    bulk_delete_min_pk_.clear();
    bulk_delete_max_pk_.clear();
    return 0;
}

/* ******************** Index Condition Pushdown (ICP) ******************** */

Item *ha_tidesdb::idx_cond_push(uint keyno, Item *idx_cond)
{
    DBUG_ENTER("ha_tidesdb::idx_cond_push");

    /* We accept the pushed condition, the server will evaluate it for us
       during index scans via handler::pushed_idx_cond.  For secondary
       index scans the condition is checked before the PK lookup, saving
       the most expensive operation when the condition filters rows. */
    pushed_idx_cond = idx_cond;
    pushed_idx_cond_keyno = keyno;
    in_range_check_pushed_down = true;

    DBUG_RETURN(NULL);
}

/* ******************** Multi-Range Read (MRR) ******************** */

/*
  Decide whether to accept a custom MRR strategy.  We only handle the case
  where every range the optimizer hands us is a full-key point lookup
  (UNIQUE_RANGE|EQ_RANGE) -- typically `WHERE col IN (v1, v2, ...)` on a
  PK or full-key unique index.  For mixed or true-range sequences we leave
  HA_MRR_USE_DEFAULT_IMPL set so the handler::multi_range_read_* default
  path runs unchanged.

  Iterating the sequence here consumes it; MariaDB re-initialises it before
  calling multi_range_read_init, so probing is safe.
*/
ha_rows ha_tidesdb::multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq, void *seq_init_param,
                                                uint n_ranges_arg, uint *bufsz, uint *mrr_mode,
                                                ha_rows limit, Cost_estimate *cost)
{
    /* We compute the default cost + flags first so non-accepted sequences fall
       through to the server's MRR->read_range_first path with correct costing. */
    ha_rows rows = handler::multi_range_read_info_const(keyno, seq, seq_init_param, n_ranges_arg,
                                                        bufsz, mrr_mode, limit, cost);
    if (rows == HA_POS_ERROR) return rows;

    /* Partitioned tables are served by ha_partition, which dispatches
       multi_range_read_* across child handlers using its own DS-MRR-backed
       logic.  If we clear HA_MRR_USE_DEFAULT_IMPL here, ha_partition's
       ordered-index-scan path ends up invoking our custom _next without
       the state its own ordering logic expects and crashes.  Refuse to
       accept MRR for partitioned tables -- the default path runs correctly. */
    if (table && table->part_info) return rows;

    /* Probe the sequence, we accept only if every range is a full single-point
       equality.  A single non-point range forces us back to the default path. */
    KEY_MULTI_RANGE range;
    range_seq_t it = seq->init(seq_init_param, n_ranges_arg, *mrr_mode);
    bool all_point = true;
    uint count = 0;
    while (!seq->next(it, &range))
    {
        count++;
        if (!(range.range_flag & UNIQUE_RANGE) || (range.range_flag & NULL_RANGE) ||
            !(range.range_flag & EQ_RANGE))
        {
            all_point = false;
            break;
        }
    }

    /* We only accept when there are multiple ranges.  For a single point lookup
       the optimizer's eq_ref plan (plain index_read_map) is a better fit and
       -- critically -- also the only path where pessimistic row locking
       engages.  Accepting MRR for 1-range scans silently converts UPDATE
       WHERE pk=v into a range scan that bypasses that lock. */
    if (all_point && count >= MRR_ACCEPT_MIN_RANGES)
    {
        *mrr_mode &= ~HA_MRR_USE_DEFAULT_IMPL;
        *bufsz = 0; /* we use our own std::vector, not HANDLER_BUFFER */
    }
    return rows;
}

/*
  Build the sorted list of point lookups, or fall through to the default
  impl if HA_MRR_USE_DEFAULT_IMPL is still set.  Sorting by comparable
  bytes converts N scattered LSM seeks into a monotone stream -- much
  friendlier to the block cache and the merge-heap.
*/
int ha_tidesdb::multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges,
                                      uint mrr_mode, HANDLER_BUFFER *buf)
{
    DBUG_ENTER("ha_tidesdb::multi_range_read_init");

    mrr_custom_active_ = !(mrr_mode & HA_MRR_USE_DEFAULT_IMPL);
    if (!mrr_custom_active_)
        DBUG_RETURN(handler::multi_range_read_init(seq, seq_init_param, n_ranges, mrr_mode, buf));

    mrr_entries_.clear();
    mrr_next_idx_ = 0;
    mrr_keyno_ = active_index;
    mrr_no_assoc_ = MY_TEST(mrr_mode & HA_MRR_NO_ASSOCIATION);
    if (n_ranges > 0) mrr_entries_.reserve(n_ranges);

    KEY *ki = &table->key_info[mrr_keyno_];

    /* We need all columns readable while translating the caller's key_copy
       bytes into our comparable format (key_copy_to_comparable calls
       key_restore into record[1] and reads fields). */
    MY_BITMAP *old_map = tmp_use_all_columns(table, &table->read_set);

    KEY_MULTI_RANGE range;
    range_seq_t it = seq->init(seq_init_param, n_ranges, mrr_mode);
    while (!seq->next(it, &range))
    {
        uchar comp[MAX_KEY_LENGTH];
        uint comp_len =
            key_copy_to_comparable(ki, range.start_key.key, range.start_key.length, comp);

        tdb_mrr_entry e;
        e.comp_key.assign((const char *)comp, comp_len);
        e.ptr = range.ptr;
        mrr_entries_.push_back(std::move(e));
    }

    tmp_restore_column_map(&table->read_set, old_map);

    std::sort(mrr_entries_.begin(), mrr_entries_.end(),
              [](const tdb_mrr_entry &a, const tdb_mrr_entry &b)
              { return a.comp_key < b.comp_key; });

    DBUG_RETURN(0);
}

/*
  Deliver the next row from the sorted list of point lookups.  PK lookups
  bypass the iterator entirely via fetch_row_by_pk; secondary index lookups
  reuse the cached scan iterator and a single seek per entry.  Rows that
  the index knew about but the data CF no longer has (stale entries after
  concurrent delete) are silently skipped.
*/
int ha_tidesdb::multi_range_read_next(range_id_t *range_info)
{
    DBUG_ENTER("ha_tidesdb::multi_range_read_next");

    if (!mrr_custom_active_) DBUG_RETURN(handler::multi_range_read_next(range_info));

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    /* Lazy txn -- the optimizer may invoke MRR without a prior rnd_init / index_init. */
    int erc = ensure_stmt_txn();
    if (erc) DBUG_RETURN(erc);
    if (!scan_txn) scan_txn = stmt_txn;

    bool is_pk_scan = share->has_user_pk && mrr_keyno_ == share->pk_index;
    uint idx_col_len = share->idx_comp_key_len[mrr_keyno_];

    while (mrr_next_idx_ < mrr_entries_.size())
    {
        const tdb_mrr_entry &e = mrr_entries_[mrr_next_idx_++];
        if (!mrr_no_assoc_) *range_info = e.ptr;

        if (is_pk_scan)
        {
            int rc = fetch_row_by_pk(scan_txn, (const uchar *)e.comp_key.data(),
                                     (uint)e.comp_key.size(), table->record[0]);
            if (rc == HA_ERR_KEY_NOT_FOUND) continue; /* stale range, try next */
            DBUG_RETURN(rc);
        }

        /* Secondary index point lookup -- seek, verify prefix match, then
           either cover-read from the index or PK-fetch. */
        if (mrr_keyno_ >= share->idx_cfs.size() || !share->idx_cfs[mrr_keyno_])
            continue; /* missing CF for this index -- skip defensively */
        scan_cf_ = share->idx_cfs[mrr_keyno_];
        int irc = ensure_scan_iter();
        if (irc) DBUG_RETURN(irc);

        tidesdb_iter_seek(scan_iter, (const uint8_t *)e.comp_key.data(), (uint)e.comp_key.size());
        if (!tidesdb_iter_valid(scan_iter)) continue;

        uint8_t *ik = NULL;
        size_t iks = 0;
        if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) continue;
        if (iks < e.comp_key.size() || memcmp(ik, e.comp_key.data(), e.comp_key.size()) != 0)
            continue; /* no entry for this point */
        if (iks <= idx_col_len) continue;

        int rc;
        if (keyread_only_ && try_keyread_from_index(ik, iks, mrr_keyno_, table->record[0]))
            rc = 0;
        else
            rc = fetch_row_by_pk(scan_txn, ik + idx_col_len, (uint)(iks - idx_col_len),
                                 table->record[0]);
        if (rc == HA_ERR_KEY_NOT_FOUND) continue;
        DBUG_RETURN(rc);
    }

    DBUG_RETURN(HA_ERR_END_OF_FILE);
}

/* ******************** info ******************** */

int ha_tidesdb::info(uint flag)
{
    DBUG_ENTER("ha_tidesdb::info");

    if (share) ref_length = share->pk_key_len;

    if ((flag & (HA_STATUS_VARIABLE | HA_STATUS_CONST)) && share && share->cf)
    {
        long long now = (long long)microsecond_interval_timer();
        long long last = share->stats_refresh_us.load(std::memory_order_relaxed);
        if (now - last > TIDESDB_STATS_REFRESH_US &&
            share->stats_refresh_us.compare_exchange_weak(last, now, std::memory_order_relaxed))
        {
            tidesdb_stats_t *st = NULL;
            if (tidesdb_get_stats(share->cf, &st) == TDB_SUCCESS && st)
            {
                share->cached_records.store(st->total_keys, std::memory_order_relaxed);

                /* total_data_size only counts SSTable klog+vlog; memtable_size
                   holds the active memtable footprint.  Sum both so that
                   DATA_LENGTH in information_schema.TABLES is non-zero even
                   before the first flush.  When both are 0 (library gap),
                   fall back to total_keys * avg entry size. */
                uint64_t data_sz = st->total_data_size + (uint64_t)st->memtable_size;
                if (data_sz == 0 && st->total_keys > 0)
                    data_sz = (uint64_t)(st->total_keys * (st->avg_key_size + st->avg_value_size));
                share->cached_data_size.store(data_sz, std::memory_order_relaxed);
                uint32_t mrl = (uint32_t)(st->avg_key_size + st->avg_value_size);
                if (mrl == 0) mrl = table->s->reclength;
                share->cached_mean_rec_len.store(mrl, std::memory_order_relaxed);
                share->cached_read_amp.store(st->read_amp > 0 ? st->read_amp : READ_AMP_NONE,
                                             std::memory_order_relaxed);

                /* We sum secondary index CF sizes for index_file_length */
                uint64_t idx_total = 0;
                for (uint i = 0; i < share->idx_cfs.size(); i++)
                {
                    if (!share->idx_cfs[i]) continue;
                    tidesdb_stats_t *ist = NULL;
                    if (tidesdb_get_stats(share->idx_cfs[i], &ist) == TDB_SUCCESS && ist)
                    {
                        uint64_t isz = ist->total_data_size + (uint64_t)ist->memtable_size;
                        if (isz == 0 && ist->total_keys > 0)
                            isz = (uint64_t)(ist->total_keys *
                                             (ist->avg_key_size + ist->avg_value_size));
                        idx_total += isz;
                        tidesdb_free_stats(ist);
                    }
                }
                share->cached_idx_data_size.store(idx_total, std::memory_order_relaxed);

                tidesdb_free_stats(st);
            }

            /* Also refresh SHOW GLOBAL STATUS variables while we're updating stats */
            tidesdb_refresh_status_vars();
        }

        stats.records = share->cached_records.load(std::memory_order_relaxed);
        if (stats.records == 0) stats.records = TIDESDB_MIN_STATS_RECORDS;
        stats.data_file_length = share->cached_data_size.load(std::memory_order_relaxed);
        stats.index_file_length = share->cached_idx_data_size.load(std::memory_order_relaxed);
        stats.mean_rec_length = share->cached_mean_rec_len.load(std::memory_order_relaxed);
        stats.delete_length = 0;
        stats.mrr_length_per_rec = ref_length + sizeof(uint64_t);
    }

    /* HA_STATUS_TIME -- we create_time from .frm stat and update_time from last DML */
    if ((flag & HA_STATUS_TIME) && share)
    {
        stats.create_time = share->create_time;
        stats.update_time = share->update_time.load(std::memory_order_relaxed);
    }

    /* HA_STATUS_CONST              -- set rec_per_key for index selectivity estimates.
       PK and UNIQUE indexes        -- rec_per_key = 1.
       Non-unique secondary indexes -- use cached_rec_per_key if populated
       by ANALYZE TABLE, else use a heuristic
       (total_keys / STATS_REC_PER_KEY_FALLBACK_DIVISOR). */
    if ((flag & HA_STATUS_CONST) && share)
    {
        for (uint i = 0; i < table->s->keys; i++)
        {
            KEY *key = &table->key_info[i];
            bool is_pk = share->has_user_pk && i == share->pk_index;
            bool is_unique = (key->flags & HA_NOSAME);
            ulong cached_rpk =
                (i < MAX_KEY) ? share->cached_rec_per_key[i].load(std::memory_order_relaxed) : 0;
            for (uint j = 0; j < key->ext_key_parts; j++)
            {
                if (is_pk || is_unique)
                {
                    if (j + 1 >= key->user_defined_key_parts)
                    {
                        /* Full unique key, exactly 1 row per distinct value */
                        key->rec_per_key[j] = REC_PER_KEY_UNIQUE;
                    }
                    else
                    {
                        /* Intermediate prefix of a composite unique key.
                           Estimate assuming uniform distribution:
                             cardinality(prefix_k) ≈ total^(k/N)
                             rec_per_key[j] = total^((N - j - 1) / N)
                           E.g. for PK(a,b,c) with 300K rows:
                             rec_per_key[0] ≈ 4481  (per distinct a)
                             rec_per_key[1] ≈ 67    (per distinct a,b)
                             rec_per_key[2] = 1     (unique)          */
                        uint N = key->user_defined_key_parts;
                        double rpk = pow((double)stats.records, (double)(N - j - 1) / (double)N);
                        key->rec_per_key[j] = (ulong)MY_MAX((ulong)rpk, REC_PER_KEY_FLOOR);
                    }
                }
                else if (j + 1 == key->user_defined_key_parts)
                {
                    /* Last user key part of a non-unique index.
                       We use ANALYZE-sampled value if available, else heuristic. */
                    if (cached_rpk > 0)
                        key->rec_per_key[j] = cached_rpk;
                    else
                        key->rec_per_key[j] =
                            (ulong)MY_MAX(stats.records / STATS_REC_PER_KEY_FALLBACK_DIVISOR + 1,
                                          REC_PER_KEY_FLOOR);
                }
                else
                {
                    /* Intermediate prefix of a non-unique index.
                       Geometrically interpolate between stats.records
                       (single leading column) and the last-part rec_per_key.
                       Formula is total / (total/last_rpk)^((j+1)/N) */
                    ulong last_rpk =
                        (cached_rpk > 0)
                            ? cached_rpk
                            : (ulong)(stats.records / STATS_REC_PER_KEY_FALLBACK_DIVISOR + 1);
                    uint N = key->user_defined_key_parts;
                    double base = (last_rpk > 0) ? (double)stats.records / (double)last_rpk
                                                 : (double)stats.records;
                    double rpk = (double)stats.records / pow(base, (double)(j + 1) / (double)N);
                    key->rec_per_key[j] =
                        (ulong)MY_MAX(MY_MIN((ulong)rpk, stats.records), REC_PER_KEY_FLOOR);
                }
            }
        }
    }

    DBUG_RETURN(0);
}

/* ******************** analyze ******************** */

/*
  ANALYZE TABLE -- refresh cached stats and output CF statistics as notes.
  The notes appear as additional Msg_type='note' rows in the ANALYZE TABLE
  result set, giving the user visibility into TidesDB internals.
*/
int ha_tidesdb::analyze(THD *thd, HA_CHECK_OPT *check_opt)
{
    DBUG_ENTER("ha_tidesdb::analyze");

    if (!share || !share->cf) DBUG_RETURN(HA_ADMIN_FAILED);

    share->stats_refresh_us.store(0, std::memory_order_relaxed);
    info(HA_STATUS_VARIABLE | HA_STATUS_CONST);

    tidesdb_stats_t *st = NULL;
    if (tidesdb_get_stats(share->cf, &st) != TDB_SUCCESS || !st)
    {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                            "[TIDESDB] unable to retrieve column family stats");
        DBUG_RETURN(HA_ADMIN_OK);
    }

    /* Summary line */
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                        "[TIDESDB] CF '%s'  total_keys=%llu  data_size=%llu bytes"
                        "  memtable=%zu bytes  levels=%d  read_amp=%.2f"
                        "  cache_hit=%.1f%%",
                        share->cf_name.c_str(), (unsigned long long)st->total_keys,
                        (unsigned long long)st->total_data_size, st->memtable_size, st->num_levels,
                        st->read_amp, st->hit_rate * PERCENT_SCALE);

    /* Average sizes */
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                        "[TIDESDB] avg_key=%.1f bytes  avg_value=%.1f bytes", st->avg_key_size,
                        st->avg_value_size);

    /* Per-level detail */
    for (int i = 0; i < st->num_levels; i++)
    {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                            "[TIDESDB] level %d  sstables=%d  size=%zu bytes"
                            "  keys=%llu",
                            i + 1, st->level_num_sstables[i], st->level_sizes[i],
                            (unsigned long long)st->level_key_counts[i]);
    }

    /* B+tree stats (only when use_btree=1) */
    if (st->use_btree)
    {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                            "[TIDESDB] btree  nodes=%llu  max_height=%u"
                            "  avg_height=%.2f",
                            (unsigned long long)st->btree_total_nodes, st->btree_max_height,
                            st->btree_avg_height);
    }

    tidesdb_free_stats(st);

    /* Secondary index CF stats + cardinality sampling.
       We iterate each secondary index CF, counting distinct index-column
       prefixes (everything before the PK suffix) to compute rec_per_key. */
    {
        int erc = ensure_stmt_txn();
        if (erc)
        {
            DBUG_RETURN(HA_ADMIN_OK); /* non-fatal -- stats just won't be updated */
        }
    }
    for (uint i = 0; i < table->s->keys; i++)
    {
        if (share->has_user_pk && i == share->pk_index) continue;
        if (i >= share->idx_cfs.size() || !share->idx_cfs[i]) continue;
        KEY *ki = &table->key_info[i];

        tidesdb_stats_t *ist = NULL;
        uint64_t idx_total_keys = 0;
        if (tidesdb_get_stats(share->idx_cfs[i], &ist) == TDB_SUCCESS && ist)
        {
            idx_total_keys = ist->total_keys;
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                                "[TIDESDB] idx CF '%s'  keys=%llu  data_size=%llu bytes"
                                "  levels=%d",
                                share->idx_cf_names[i].c_str(), (unsigned long long)ist->total_keys,
                                (unsigned long long)ist->total_data_size, ist->num_levels);
            tidesdb_free_stats(ist);
        }

        /* We sample the index to estimate distinct prefix count.
           For unique indexes rec_per_key is always 1.
           For non-unique indexes, scan up to ANALYZE_SAMPLE_LIMIT entries
           and count distinct index-column prefixes. */
        if (ki->flags & HA_NOSAME)
        {
            share->cached_rec_per_key[i].store(REC_PER_KEY_UNIQUE, std::memory_order_relaxed);
            continue;
        }

        uint idx_prefix_len = share->idx_comp_key_len[i];
        if (idx_prefix_len == 0) continue;

        tidesdb_iter_t *ait = NULL;
        if (tdb_iter_new_blocking(ha_thd(), stmt_txn, share->idx_cfs[i], &ait) != TDB_SUCCESS ||
            !ait)
            continue;

        tidesdb_iter_seek_to_first(ait);

        static constexpr uint64_t ANALYZE_SAMPLE_LIMIT = 100000;
        uint64_t sampled = 0, distinct = 0;
        uchar prev_prefix[MAX_KEY_LENGTH];
        uint prev_len = 0;

        while (tidesdb_iter_valid(ait) && sampled < ANALYZE_SAMPLE_LIMIT)
        {
            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(ait, &ik, &iks) != TDB_SUCCESS) break;

            uint cmp_len = (iks >= idx_prefix_len) ? idx_prefix_len : (uint)iks;
            if (sampled == 0 || cmp_len != prev_len || memcmp(ik, prev_prefix, cmp_len) != 0)
            {
                distinct++;
                prev_len = cmp_len;
                memcpy(prev_prefix, ik, cmp_len);
            }
            sampled++;
            tidesdb_iter_next(ait);
        }
        tidesdb_iter_free(ait);

        if (distinct > 0)
        {
            uint64_t total = (idx_total_keys > 0) ? idx_total_keys : sampled;
            if (sampled < total)
            {
                /* Extrapolate -- distinct_full ≈ distinct * (total / sampled) */
                double ratio = (double)total / (double)sampled;
                uint64_t est_distinct = (uint64_t)(distinct * ratio);
                if (est_distinct == 0) est_distinct = 1; /* divide-by-zero guard */
                ulong rpk = (ulong)(total / est_distinct);
                if (rpk == 0) rpk = REC_PER_KEY_FLOOR;
                share->cached_rec_per_key[i].store(rpk, std::memory_order_relaxed);
            }
            else
            {
                ulong rpk = (ulong)(sampled / distinct);
                if (rpk == 0) rpk = REC_PER_KEY_FLOOR;
                share->cached_rec_per_key[i].store(rpk, std::memory_order_relaxed);
            }

            push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, ER_UNKNOWN_ERROR,
                                "[TIDESDB] idx '%s' sampled=%llu distinct=%llu rec_per_key=%lu",
                                ki->name.str, (unsigned long long)sampled,
                                (unsigned long long)distinct,
                                share->cached_rec_per_key[i].load(std::memory_order_relaxed));
        }
    }

    info(HA_STATUS_CONST);

    DBUG_RETURN(HA_ADMIN_OK);
}

/* ******************** optimize ******************** */

/*
  OPTIMIZE TABLE -- trigger compaction on all CFs (data + secondary indexes).
  Compaction merges SSTables, removes tombstones, and reduces read
  amplification.  TidesDB enqueues the work to background compaction
  threads and returns immediately.
*/
int ha_tidesdb::optimize(THD *thd, HA_CHECK_OPT *check_opt)
{
    DBUG_ENTER("ha_tidesdb::optimize");

    if (!share || !share->cf) DBUG_RETURN(HA_ADMIN_FAILED);

    /* tidesdb_purge_cf() is synchronous -- flushes memtable to disk, then
       runs a full compaction inline, blocking until complete.  This is
       the right semantic for OPTIMIZE TABLE -- the caller expects the
       table to be fully compacted when the statement returns. */
    bool any_locked = false;
    int rc = tidesdb_purge_cf(share->cf);
    if (rc == TDB_ERR_LOCKED)
        any_locked = true;
    else if (rc != TDB_SUCCESS)
        sql_print_warning("[TIDESDB] optimize: purge data CF '%s' failed (err=%d)",
                          share->cf_name.c_str(), rc);

    for (uint i = 0; i < share->idx_cfs.size(); i++)
    {
        if (!share->idx_cfs[i]) continue;
        rc = tidesdb_purge_cf(share->idx_cfs[i]);
        if (rc == TDB_ERR_LOCKED)
            any_locked = true;
        else if (rc != TDB_SUCCESS)
            sql_print_warning("[TIDESDB] optimize: purge idx CF '%s' failed (err=%d)",
                              share->idx_cf_names[i].c_str(), rc);
    }

    share->stats_refresh_us.store(0, std::memory_order_relaxed);

    /* TDB_ERR_LOCKED means "another compaction was already running and
       will subsume this work".  Surface HA_ADMIN_TRY_ALTER so the user
       sees something other than silent success -- they can retry, or
       confirm via SHOW ENGINE TIDESDB STATUS that compaction finished. */
    if (any_locked)
    {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE, HA_ADMIN_TRY_ALTER,
                            "OPTIMIZE TABLE: one or more column families had a "
                            "compaction already in flight; retry shortly if you "
                            "need the post-OPTIMIZE state.");
        DBUG_RETURN(HA_ADMIN_TRY_ALTER);
    }
    DBUG_RETURN(HA_ADMIN_OK);
}

int ha_tidesdb::check(THD *thd, HA_CHECK_OPT *check_opt)
{
    DBUG_ENTER("ha_tidesdb::check");

    if (!share || !share->cf) DBUG_RETURN(HA_ADMIN_CORRUPT);

    /* CHECK TABLE verifies all CFs are readable by fetching stats.
       tidesdb_get_stats reads metadata from all SSTables, which validates
       that manifests, block indexes, bloom filters, and metadata blocks
       are intact. For a deeper check, users can run REPAIR TABLE which
       does a full compaction pass that reads and re-checksums every block. */
    tidesdb_stats_t *st = NULL;
    int rc = tidesdb_get_stats(share->cf, &st);
    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] CHECK TABLE '%s': data CF check failed (err=%d)",
                        share->cf_name.c_str(), rc);
        DBUG_RETURN(HA_ADMIN_CORRUPT);
    }
    tidesdb_free_stats(st);

    for (uint i = 0; i < share->idx_cfs.size(); i++)
    {
        if (!share->idx_cfs[i]) continue;
        tidesdb_stats_t *ist = NULL;
        rc = tidesdb_get_stats(share->idx_cfs[i], &ist);
        if (rc != TDB_SUCCESS)
        {
            sql_print_error("[TIDESDB] CHECK TABLE '%s': index CF '%s' check failed (err=%d)",
                            share->cf_name.c_str(), share->idx_cf_names[i].c_str(), rc);
            DBUG_RETURN(HA_ADMIN_CORRUPT);
        }
        tidesdb_free_stats(ist);
    }

    DBUG_RETURN(HA_ADMIN_OK);
}

int ha_tidesdb::repair(THD *thd, HA_CHECK_OPT *check_opt)
{
    DBUG_ENTER("ha_tidesdb::repair");

    if (!share || !share->cf) DBUG_RETURN(HA_ADMIN_FAILED);

    /* REPAIR TABLE triggers a full purge (flush + compaction) of all CFs.
       In unified memtable mode, the first purge_cf call rotates the shared
       unified memtable and waits for the flush to complete. Subsequent
       purge_cf calls on index CFs skip the rotation (already done) and
       just run per-CF compaction. tidesdb_purge_cf is unified-mode aware
       and handles this idempotently. */
    int rc = tidesdb_purge_cf(share->cf);
    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] REPAIR TABLE '%s': purge data CF failed (err=%d)",
                        share->cf_name.c_str(), rc);
        DBUG_RETURN(HA_ADMIN_FAILED);
    }

    for (uint i = 0; i < share->idx_cfs.size(); i++)
    {
        if (!share->idx_cfs[i]) continue;
        rc = tidesdb_purge_cf(share->idx_cfs[i]);
        if (rc != TDB_SUCCESS)
            sql_print_warning("[TIDESDB] REPAIR TABLE '%s': purge idx CF '%s' failed (err=%d)",
                              share->cf_name.c_str(), share->idx_cf_names[i].c_str(), rc);
    }

    share->stats_refresh_us.store(0, std::memory_order_relaxed);
    DBUG_RETURN(HA_ADMIN_OK);
}

IO_AND_CPU_COST ha_tidesdb::scan_time()
{
    IO_AND_CPU_COST cost;
    cost.io = 0.0;
    cost.cpu = 0.0;

    if (!share || !share->cf) return cost;

    /* Cache the range_cost result on the share with the same refresh
       interval as stats (TIDESDB_STATS_REFRESH_US = 2 seconds).
       tidesdb_range_cost examines in-memory metadata (block indexes,
       SSTable min/max keys) without disk I/O, but the computation
       still has measurable CPU cost when called per query plan. */
    auto now = std::chrono::steady_clock::now();
    auto cached_time = share->scan_cost_time.load(std::memory_order_relaxed);
    double cached_cost = share->cached_scan_cost.load(std::memory_order_relaxed);

    bool stale =
        (cached_cost <= 0.0) ||
        (std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch()).count() -
             cached_time >
         TIDESDB_STATS_REFRESH_US);

    if (stale)
    {
        uchar lo[KEY_NAMESPACE_LEN] = {KEY_NS_DATA};
        uchar hi[DATA_KEY_BUF_LEN];
        memset(hi, KEY_INF_HI_BYTE, sizeof(hi));
        uint hi_len = KEY_NAMESPACE_LEN + share->pk_key_len;
        if (hi_len > sizeof(hi)) hi_len = sizeof(hi);

        double full_cost = 0.0;
        if (tidesdb_range_cost(share->cf, lo, KEY_NAMESPACE_LEN, hi, hi_len, &full_cost) ==
                TDB_SUCCESS &&
            full_cost > 0.0)
        {
            cached_cost = full_cost;
            share->cached_scan_cost.store(cached_cost, std::memory_order_relaxed);
            share->scan_cost_time.store(
                std::chrono::duration_cast<std::chrono::microseconds>(now.time_since_epoch())
                    .count(),
                std::memory_order_relaxed);
        }
    }

    if (cached_cost > 0.0)
    {
        cost.io = cached_cost * TIDESDB_SCAN_IO_WEIGHT;
        cost.cpu = cached_cost * TIDESDB_SCAN_CPU_WEIGHT;
    }
    else
    {
        cost = handler::scan_time();
    }

    return cost;
}

ha_rows ha_tidesdb::records_in_range(uint inx, const key_range *min_key, const key_range *max_key,
                                     page_range *pages)
{
    if (!share) return TIDESDB_RIR_DEFAULT_EST;

    ha_rows total = share->cached_records.load(std::memory_order_relaxed);
    if (total == 0) total = TIDESDB_MIN_STATS_RECORDS;

    tidesdb_column_family_t *cf;
    bool is_pk = share->has_user_pk && inx == share->pk_index;
    if (is_pk)
        cf = share->cf;
    else if (inx < share->idx_cfs.size() && share->idx_cfs[inx])
        cf = share->idx_cfs[inx];
    else
        return (total / TIDESDB_RIR_UNKNOWN_DENOM) + REC_PER_KEY_FLOOR; /* no CF for this index */

    /* We convert min_key / max_key to our comparable format.
       If a bound is missing we use the natural boundary of the key space. */
    uchar lo_buf[DATA_KEY_BUF_LEN];
    uchar hi_buf[DATA_KEY_BUF_LEN];
    uint lo_len = 0, hi_len = 0;

    MY_BITMAP *old_map = tmp_use_all_columns(table, &table->read_set);

    if (min_key && min_key->key)
    {
        KEY *ki = &table->key_info[inx];
        uint kl = calculate_key_len(table, inx, min_key->key, min_key->keypart_map);
        if (is_pk)
        {
            uchar comp[MAX_KEY_LENGTH];
            uint comp_len = key_copy_to_comparable(ki, min_key->key, kl, comp);
            lo_len = build_data_key(comp, comp_len, lo_buf);
        }
        else
        {
            lo_len = key_copy_to_comparable(ki, min_key->key, kl, lo_buf);
        }
    }
    else
    {
        /* No lower bound, we use smallest possible key */
        if (is_pk)
        {
            lo_buf[0] = KEY_NS_DATA;
            lo_len = KEY_NAMESPACE_LEN;
        }
        else
        {
            lo_buf[0] = KEY_INF_LO_BYTE;
            lo_len = KEY_NAMESPACE_LEN;
        }
    }

    if (max_key && max_key->key)
    {
        KEY *ki = &table->key_info[inx];
        uint kl = calculate_key_len(table, inx, max_key->key, max_key->keypart_map);
        if (is_pk)
        {
            uchar comp[MAX_KEY_LENGTH];
            uint comp_len = key_copy_to_comparable(ki, max_key->key, kl, comp);
            hi_len = build_data_key(comp, comp_len, hi_buf);
        }
        else
        {
            hi_len = key_copy_to_comparable(ki, max_key->key, kl, hi_buf);
        }
    }
    else
    {
        /* No upper bound, we use largest possible key */
        memset(hi_buf, KEY_INF_HI_BYTE, sizeof(hi_buf));
        hi_len = is_pk ? (KEY_NAMESPACE_LEN + share->pk_key_len)
                       : share->idx_comp_key_len[inx] + share->pk_key_len;
        if (hi_len > sizeof(hi_buf)) hi_len = sizeof(hi_buf);
    }

    tmp_restore_column_map(&table->read_set, old_map);

    /* We detect point equality, both bounds provided with identical comparable
       bytes.  tidesdb_range_cost is an I/O cost metric, not a cardinality
       metric -- for memtable-only data it cannot distinguish a point range
       from a full scan.  For equalities we return rec_per_key directly. */
    if (min_key && max_key && lo_len > 0 && hi_len > 0 && lo_len == hi_len &&
        memcmp(lo_buf, hi_buf, lo_len) == 0)
    {
        KEY *ki = &table->key_info[inx];
        uint parts_used = my_count_bits(min_key->keypart_map);
        if (parts_used > 0 && parts_used <= ki->user_defined_key_parts)
        {
            ulong rpk = ki->rec_per_key[parts_used - 1];
            ha_rows est = (rpk > 0) ? (ha_rows)rpk : REC_PER_KEY_FLOOR;
            if (est > total) est = total;
            return est;
        }
        return REC_PER_KEY_FLOOR;
    }

    /* We ask TidesDB for the range cost (no disk I/O -- uses in-memory
       block indexes, SSTable min/max keys, and entry counts). */
    double range_cost = 0.0;
    int rc = tidesdb_range_cost(cf, lo_buf, lo_len, hi_buf, hi_len, &range_cost);
    if (rc != TDB_SUCCESS || range_cost <= 0.0)
        return (total / TIDESDB_RIR_UNKNOWN_DENOM) + REC_PER_KEY_FLOOR; /* fallback */

    /* We get full-range cost for normalization.  We use the natural boundaries
       of the key space so that range_cost / full_cost ≈ fraction of data.
       Cached per-CF and refreshed on the same TIDESDB_STATS_REFRESH_US
       window as scan_time so a plan probing N alternatives only computes
       the normalizer once. */
    double full_cost = 0.0;
    {
        std::atomic<double> *cache_val =
            is_pk ? &share->cached_pk_full_cost
                  : (inx < share->cached_idx_full_cost_n ? &share->cached_idx_full_cost[inx]
                                                         : nullptr);
        std::atomic<long long> *cache_time =
            is_pk ? &share->cached_pk_full_cost_time
                  : (inx < share->cached_idx_full_cost_n ? &share->cached_idx_full_cost_time[inx]
                                                         : nullptr);

        long long now_us = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();

        if (cache_val && cache_time)
        {
            double cached = cache_val->load(std::memory_order_relaxed);
            long long when = cache_time->load(std::memory_order_relaxed);
            if (cached > 0.0 && now_us - when < TIDESDB_STATS_REFRESH_US)
            {
                full_cost = cached;
            }
        }

        if (full_cost <= 0.0)
        {
            uchar full_lo[KEY_NAMESPACE_LEN] = {(uchar)(is_pk ? KEY_NS_DATA : KEY_INF_LO_BYTE)};
            uchar full_hi[DATA_KEY_BUF_LEN];
            memset(full_hi, KEY_INF_HI_BYTE, sizeof(full_hi));
            uint full_hi_len = hi_len; /* same width as hi_buf */
            tidesdb_range_cost(cf, full_lo, KEY_NAMESPACE_LEN, full_hi, full_hi_len, &full_cost);

            if (cache_val && cache_time && full_cost > 0.0)
            {
                cache_val->store(full_cost, std::memory_order_relaxed);
                cache_time->store(now_us, std::memory_order_relaxed);
            }
        }
    }

    if (full_cost <= 0.0)
        return (total / TIDESDB_RIR_UNKNOWN_DENOM) + REC_PER_KEY_FLOOR; /* fallback */

    /* We estimate records proportionally -- narrower range -> fewer records */
    double fraction = range_cost / full_cost;
    if (fraction > FRACTION_MAX) fraction = FRACTION_MAX;
    if (fraction < FRACTION_MIN) fraction = FRACTION_MIN;

    ha_rows est = (ha_rows)(total * fraction);
    if (est == 0) est = REC_PER_KEY_FLOOR; /* never return 0 -- optimizer treats it as "empty" */

    /* When both bounds are provided but the estimated fraction is very
       high (>TIDESDB_RIR_FRACTION_UNRELIABLE), tidesdb_range_cost is
       likely unreliable -- this happens with memtable-only data where
       the cost function cannot distinguish a narrow range from a full
       scan.  Fall back to a rec_per_key-based estimate for the prefix. */
    if (min_key && max_key && fraction > TIDESDB_RIR_FRACTION_UNRELIABLE)
    {
        KEY *ki = &table->key_info[inx];
        uint parts = my_count_bits(min_key->keypart_map);
        if (parts > 0 && parts <= ki->user_defined_key_parts)
        {
            ulong rpk = ki->rec_per_key[parts - 1];
            if (rpk > 0)
            {
                ha_rows capped;
                if (lo_len == hi_len && memcmp(lo_buf, hi_buf, lo_len) == 0)
                {
                    /* Point equality, we use rec_per_key directly */
                    capped = (ha_rows)rpk;
                }
                else
                {
                    /* With range scans we multiply rec_per_key by a conservative
                       range-width factor.  Typical OLTP ranges span tens of
                       key values; the multiplier keeps the estimate tight while
                       still being vastly better than the unreliable full ratio. */
                    capped = (ha_rows)rpk * TIDESDB_RIR_RANGE_RPK_MULTIPLIER;
                    const ha_rows cap = total / TIDESDB_RIR_RANGE_CAP_DENOM;
                    if (capped > cap) capped = cap;
                }
                if (capped < est) est = MY_MAX(capped, REC_PER_KEY_FLOOR);
            }
        }
    }

    return est;
}

ulong ha_tidesdb::index_flags(uint idx, uint part, bool all_parts) const
{
    /* FULLTEXT indexes do not support ordered reads or ICP */
    if (table_share && idx < table_share->keys &&
        table_share->key_info[idx].algorithm == HA_KEY_ALG_FULLTEXT)
        return 0;

    /* SPATIAL indexes support MBR range scans and forward iteration */
    if (table_share && idx < table_share->keys && is_spatial_index(&table_share->key_info[idx]))
        return HA_READ_NEXT | HA_READ_RANGE;

    ulong flags =
        HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE | HA_DO_INDEX_COND_PUSHDOWN;
    if (table_share && table_share->primary_key != MAX_KEY && idx == table_share->primary_key)
        flags |= HA_CLUSTERED_INDEX;
    else
        flags |= HA_KEYREAD_ONLY;
    return flags;
}

const char *ha_tidesdb::index_type(uint key_number)
{
    if (key_number < table->s->keys)
    {
        if (table->key_info[key_number].algorithm == HA_KEY_ALG_FULLTEXT) return "FULLTEXT";
        if (is_spatial_index(&table->key_info[key_number])) return "RTREE";
        ha_index_option_struct *iopts = table->key_info[key_number].option_struct;
        if (iopts && iopts->use_btree) return "BTREE";
    }
    ha_table_option_struct *opts = TDB_TABLE_OPTIONS(table);
    return (opts && opts->use_btree) ? "BTREE" : "LSM";
}

/* ******************** Spatial scan continuation ******************** */

int ha_tidesdb::spatial_scan_next(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::spatial_scan_next");

    tdb_mbr_t query_mbr;
    query_mbr.xmin = spatial_qmbr_[MBR_XMIN_IDX];
    query_mbr.ymin = spatial_qmbr_[MBR_YMIN_IDX];
    query_mbr.xmax = spatial_qmbr_[MBR_XMAX_IDX];
    query_mbr.ymax = spatial_qmbr_[MBR_YMAX_IDX];

    while (spatial_range_idx_ < spatial_ranges_.size())
    {
        uint64_t cur_hi = spatial_ranges_[spatial_range_idx_].second;

        while (tidesdb_iter_valid(scan_iter))
        {
            if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

            uint8_t *ik = NULL;
            size_t iks = 0;
            if (tidesdb_iter_key(scan_iter, &ik, &iks) != TDB_SUCCESS) break;

            if (iks <= SPATIAL_HILBERT_KEY_LEN)
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }

            uint64_t h = decode_hilbert_be(ik);
            if (h > cur_hi) break; /* advance to next range */

            uint8_t *val = NULL;
            size_t vlen = 0;
            if (tidesdb_iter_value(scan_iter, &val, &vlen) != TDB_SUCCESS ||
                vlen < SPATIAL_MBR_VALUE_LEN)
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }

            /* The on-disk spatial value is exactly SPATIAL_MBR_VALUE_LEN bytes
               laid out as [xmin,ymin,xmax,ymax] (4 doubles in native order),
               matching tdb_mbr_t's field order.  We assert the struct size
               against the wire size so adding a field to tdb_mbr_t will
               fire the static_assert rather than silently corrupt reads. */
            static_assert(sizeof(tdb_mbr_t) == SPATIAL_MBR_VALUE_LEN,
                          "tdb_mbr_t must match on-disk spatial value layout");
            tdb_mbr_t entry_mbr;
            memcpy(&entry_mbr, val, SPATIAL_MBR_VALUE_LEN);

            /* We apply MBR predicate */
            if (!spatial_mbr_predicate(spatial_mode_, &query_mbr, &entry_mbr))
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }

            /* A match, we extract PK from key suffix and fetch full row */
            const uchar *pk = ik + SPATIAL_HILBERT_KEY_LEN;
            uint pk_len = (uint)(iks - SPATIAL_HILBERT_KEY_LEN);

            int ret = fetch_row_by_pk(scan_txn, pk, pk_len, buf);
            if (ret == HA_ERR_KEY_NOT_FOUND)
            {
                tidesdb_iter_next(scan_iter);
                continue;
            }
            if (ret)
            {
                table->status = STATUS_NOT_FOUND;
                DBUG_RETURN(ret);
            }

            scan_dir_ = DIR_FORWARD;
            table->status = 0;
            DBUG_RETURN(0);
        }

        /* The current range exhausted, thus we advance to next range and seek */
        spatial_range_idx_++;
        if (spatial_range_idx_ < spatial_ranges_.size())
        {
            uchar seek_key[SPATIAL_HILBERT_KEY_LEN];
            encode_hilbert_be(spatial_ranges_[spatial_range_idx_].first, seek_key);
            tidesdb_iter_seek(scan_iter, seek_key, SPATIAL_HILBERT_KEY_LEN);
        }
    }

    table->status = STATUS_NOT_FOUND;
    DBUG_RETURN(HA_ERR_END_OF_FILE);
}

/* ******************** Full-Text Search methods ******************** */

int ha_tidesdb::ft_init()
{
    DBUG_ENTER("ha_tidesdb::ft_init");
    if (ft_handler)
    {
        tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(ft_handler);
        info->current_idx = 0;
    }
    DBUG_RETURN(0);
}

void ha_tidesdb::ft_end()
{
    DBUG_ENTER("ha_tidesdb::ft_end");
    DBUG_VOID_RETURN;
}

FT_INFO *ha_tidesdb::ft_init_ext(uint flags, uint inx, String *key)
{
    DBUG_ENTER("ha_tidesdb::ft_init_ext");

    if (!share || inx >= share->idx_cfs.size() || !share->idx_cfs[inx] ||
        !is_fts_index(&table->key_info[inx]))
        DBUG_RETURN(NULL);

    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(NULL);
    }

    CHARSET_INFO *cs = table->key_info[inx].key_part[0].field->charset();

    std::vector<fts_query_term_t> query_terms;
    if (flags & FT_BOOL)
    {
        fts_parse_boolean(key->ptr(), key->length(), cs, query_terms);
    }
    else
    {
        std::vector<fts_token_t> tokens;
        fts_tokenize(key->ptr(), key->length(), cs, tokens);
        for (auto &tok : tokens)
        {
            fts_query_term_t qt;
            qt.term = std::move(tok.word);
            qt.yesno = FTS_TERM_NEUTRAL;
            qt.trunc = false;
            qt.is_phrase = false;
            query_terms.push_back(std::move(qt));
        }
    }

    /* A query that tokenises down to nothing (e.g. all stop words, all
       characters below the min-word-len threshold) still has to return a
       usable FT_INFO, not NULL.  The server's execution path assumes an
       FT_INFO was handed back and leaves the diagnostics area in an
       inconsistent state when ft_init_ext yields NULL for reasons other
       than an outright error; debug builds trip Protocol::end_statement's
       DBUG_ASSERT(0).  We return an empty result set instead, which the
       optimizer folds into zero matched rows. */
    if (query_terms.empty())
    {
        tdb_ft_info_t *empty = new tdb_ft_info_t();
        empty->please = const_cast<_ft_vft *>(&tdb_ft_vft);
        empty->could_you = &tdb_ft_vft_ext;
        empty->handler = this;
        empty->keynr = inx;
        empty->current_idx = 0;
        empty->current_rank = 0.0f;
        empty->match_count = 0;
        DBUG_RETURN(reinterpret_cast<FT_INFO *>(empty));
    }

    int64_t total_docs = 0, total_words = 0;
    fts_load_meta(stmt_txn, share->cf, inx, &total_docs, &total_words);
    double avgdl = total_docs > 0 ? (double)total_words / (double)total_docs : BM25_DEFAULT_AVGDL;
    if (total_docs == 0) total_docs = BM25_MIN_TOTAL_DOCS; /* avoid division by zero */
    /* We precompute 1/avgdl so the per-posting BM25 loop multiplies instead of
       dividing.  Divisions are expensive on modern CPUs (~20 cycles vs ~5
       for a multiply) and this runs per term per matched document. */
    const double inv_avgdl = 1.0 / avgdl;

    /* For each query term, prefix-scan the FTS CF to gather postings and score */
    std::unordered_map<std::string, double> doc_scores;
    std::unordered_map<std::string, uint> doc_required_hits;
    uint num_required = 0;

    /* Open one iterator over the FTS CF and reuse it across every query
       term -- iterator construction does an O(num_sstables) merge-heap
       build, so doing it per term made the heap work scale linearly with
       term count.  All terms in this MATCH AGAINST live in the same
       index, so a single iterator is reseeked for each term. */
    tidesdb_iter_t *shared_it = NULL;
    {
        int sirc = tdb_iter_new_blocking(ha_thd(), stmt_txn, share->idx_cfs[inx], &shared_it);
        if (sirc != TDB_SUCCESS) shared_it = NULL;
    }

    for (auto &qt : query_terms)
    {
        if (qt.yesno > FTS_TERM_NEUTRAL) num_required++;

        /* fts_build_key truncates inserted terms to FTS_MAX_TERM_BYTES, so on-disk
           keys never carry more than that.  The query term lives in a std::string
           that has no such cap (a 512-character CJK token packs ~1.5 KB of UTF-8),
           and copying it raw into the 514-byte stack prefix would overrun. */
        size_t qlen = qt.term.size();
        if (qlen > FTS_MAX_TERM_BYTES) qlen = FTS_MAX_TERM_BYTES;

        uchar prefix[FTS_TERM_LEN_PREFIX + FTS_MAX_TERM_BYTES];
        uint prefix_len = 0;
        int2store(prefix, (uint16)qlen);
        prefix_len += FTS_TERM_LEN_PREFIX;
        memcpy(prefix + prefix_len, qt.term.data(), qlen);
        prefix_len += (uint)qlen;

        struct posting_entry
        {
            std::string pk;
            uint16 tf;
            uint32 doc_len;
        };
        std::vector<posting_entry> postings;

        if (!shared_it) continue;
        tidesdb_iter_t *it = shared_it;

        if (qt.trunc)
        {
            /* Wildcard search keys are sorted by [2B term_len][term][pk],
               so terms of different lengths are in different regions.
               We iterate over each possible term length from the prefix
               length up to max_word_len, seeking directly to [len][prefix]
               for each bucket.  This is O(max_word_len) seeks, each precise. */
            uint min_len = (uint)qlen;
            uint max_len = (uint)srv_fts_max_word_len;
            if (max_len > FTS_MAX_TERM_BYTES) max_len = FTS_MAX_TERM_BYTES;

            for (uint tlen = min_len; tlen <= max_len; tlen++)
            {
                uchar seek[FTS_TERM_LEN_PREFIX + FTS_MAX_TERM_BYTES];
                int2store(seek, (uint16)tlen);
                memcpy(seek + FTS_TERM_LEN_PREFIX, qt.term.data(), qlen);
                uint seek_len = FTS_TERM_LEN_PREFIX + (uint)qlen;

                tidesdb_iter_seek(it, seek, seek_len);
                while (tidesdb_iter_valid(it))
                {
                    uint8_t *ik = NULL;
                    size_t iks = 0;
                    uint8_t *iv = NULL;
                    size_t ivs = 0;
                    if (tidesdb_iter_key_value(it, &ik, &iks, &iv, &ivs) != TDB_SUCCESS) break;

                    if (iks < FTS_TERM_LEN_PREFIX) break;
                    uint16 stored_len = uint2korr(ik);
                    if (stored_len != tlen) break;

                    if (iks < (size_t)(FTS_TERM_LEN_PREFIX + stored_len)) break;
                    if (memcmp(ik + FTS_TERM_LEN_PREFIX, qt.term.data(), qlen) != 0) break;

                    uint pk_off = FTS_TERM_LEN_PREFIX + stored_len;
                    if (iks <= pk_off)
                    {
                        tidesdb_iter_next(it);
                        continue;
                    }
                    std::string pk((char *)(ik + pk_off), iks - pk_off);

                    if (ivs >= FTS_VALUE_LEN)
                        postings.push_back({pk, (uint16)uint2korr(iv),
                                            (uint32)uint4korr(iv + FTS_VALUE_DOC_LEN_OFFSET)});

                    tidesdb_iter_next(it);
                }
            }
        }
        else
        {
            tidesdb_iter_seek(it, prefix, prefix_len);
            /* exact-match path (non-truncated) */
            while (tidesdb_iter_valid(it))
            {
                uint8_t *ik = NULL;
                size_t iks = 0;
                uint8_t *iv = NULL;
                size_t ivs = 0;
                if (tidesdb_iter_key_value(it, &ik, &iks, &iv, &ivs) != TDB_SUCCESS) break;

                if (iks < prefix_len || memcmp(ik, prefix, prefix_len) != 0) break;
                std::string pk((char *)(ik + prefix_len), iks - prefix_len);

                if (ivs >= FTS_VALUE_LEN)
                    postings.push_back({pk, (uint16)uint2korr(iv),
                                        (uint32)uint4korr(iv + FTS_VALUE_DOC_LEN_OFFSET)});
                tidesdb_iter_next(it);
            }
        }

        uint32 df = (uint32)postings.size();
        double idf = std::log(((double)total_docs - (double)df + BM25_IDF_EPSILON) /
                                  ((double)df + BM25_IDF_EPSILON) +
                              BM25_IDF_NONNEG_SHIFT);
        const double k1 = srv_fts_bm25_k1, b = srv_fts_bm25_b;

        /* Pre-fold per-term constants so the inner loop is one MAD + one
           divide + one multiply, instead of recomputing k1*(1-b) and
           k1*b*inv_avgdl on every posting. */
        const double idf_x_k1_plus_1 = idf * (k1 + BM25_TF_SATURATION_BOOST);
        const double k1_one_minus_b = k1 * (BM25_LENGTH_NORM_BASE - b);
        const double k1_b_inv_avgdl = k1 * b * inv_avgdl;

        /* Reserve approximate growth so the per-posting unordered_map
           insert doesn't rehash; modest win on terms with many matches. */
        if (qt.yesno >= FTS_TERM_NEUTRAL)
        {
            doc_scores.reserve(doc_scores.size() + postings.size());
            if (qt.yesno > FTS_TERM_NEUTRAL)
                doc_required_hits.reserve(doc_required_hits.size() + postings.size());
        }

        for (auto &p : postings)
        {
            double denom = (double)p.tf + k1_one_minus_b + k1_b_inv_avgdl * (double)p.doc_len;
            double score = ((double)p.tf * idf_x_k1_plus_1) / denom;

            if (qt.yesno < FTS_TERM_NEUTRAL)
            {
                /* excluded term! we remove from results */
                doc_scores.erase(p.pk);
            }
            else
            {
                doc_scores[p.pk] += score;
                if (qt.yesno > FTS_TERM_NEUTRAL) doc_required_hits[p.pk]++;
            }
        }
    }

    if (shared_it) tidesdb_iter_free(shared_it);

    /* bool mode -- we filter docs that don't match all required terms */
    if (num_required > 0)
    {
        for (auto it = doc_scores.begin(); it != doc_scores.end();)
        {
            auto rh = doc_required_hits.find(it->first);
            if (rh == doc_required_hits.end() || rh->second < num_required)
                it = doc_scores.erase(it);
            else
                ++it;
        }
    }

    tdb_ft_info_t *info = new tdb_ft_info_t();
    info->please = const_cast<_ft_vft *>(&tdb_ft_vft);
    info->could_you = &tdb_ft_vft_ext;
    info->handler = this;
    info->keynr = inx;
    info->current_idx = 0;
    info->current_rank = 0.0f;
    info->match_count = 0;

    for (auto &[pk_str, score] : doc_scores)
    {
        tdb_fts_result_t r;
        r.pk_len = (uint)pk_str.size();
        r.pk = (uchar *)my_malloc(PSI_NOT_INSTRUMENTED, r.pk_len, MYF(0));
        if (!r.pk) continue;
        memcpy(r.pk, pk_str.data(), r.pk_len);
        r.rank = (float)score;
        info->results.push_back(r);
    }

    /* For any phrase terms in the query, we fetch each
       candidate row, re-tokenize the document, and check for the exact
       phrase as a consecutive word sequence.  We remove non-matching candidates. */
    bool has_phrases = false;
    std::vector<const fts_query_term_t *> phrases;
    for (auto &qt : query_terms)
    {
        if (qt.is_phrase)
        {
            has_phrases = true;
            phrases.push_back(&qt);
        }
    }

    if (has_phrases && !info->results.empty())
    {
        CHARSET_INFO *vcs = table->key_info[inx].key_part[0].field->charset();
        std::vector<tdb_fts_result_t> verified;
        /* Tokenize each candidate once and check all phrases against the
           single token vector, so an M-phrase query doesn't tokenize each
           doc M times. */
        std::vector<fts_token_t> doc_tokens;
        for (auto &r : info->results)
        {
            int err = fetch_row_by_pk(stmt_txn, r.pk, r.pk_len, table->record[0]);
            if (err) continue;

            doc_tokens.clear();
            fts_extract_and_tokenize(table, &table->key_info[inx], table->record[0], vcs,
                                     doc_tokens);

            bool all_phrases_match = true;
            for (auto *ph : phrases)
            {
                if (!fts_phrase_in_tokens(doc_tokens, ph->phrase_words))
                {
                    all_phrases_match = false;
                    break;
                }
            }

            if (all_phrases_match)
                verified.push_back(r);
            else
                my_free(r.pk); /* free PK of non-matching result */
        }
        info->results = std::move(verified);
    }

    std::sort(info->results.begin(), info->results.end(),
              [](const tdb_fts_result_t &a, const tdb_fts_result_t &b) { return a.rank > b.rank; });

    info->match_count = (ulonglong)info->results.size();

    DBUG_RETURN(reinterpret_cast<FT_INFO *>(info));
}

int ha_tidesdb::ft_read(uchar *buf)
{
    DBUG_ENTER("ha_tidesdb::ft_read");

    if (cached_thd_ && thd_killed(cached_thd_)) DBUG_RETURN(HA_ERR_ABORTED_BY_USER);

    tdb_ft_info_t *info = reinterpret_cast<tdb_ft_info_t *>(ft_handler);
    if (!info)
    {
        table->status = STATUS_NOT_FOUND;
        DBUG_RETURN(HA_ERR_END_OF_FILE);
    }

    {
        int erc = ensure_stmt_txn();
        if (erc) DBUG_RETURN(erc);
    }

    while (info->current_idx < info->results.size())
    {
        tdb_fts_result_t &r = info->results[info->current_idx];
        info->current_rank = r.rank;

        int err = fetch_row_by_pk(stmt_txn, r.pk, r.pk_len, buf);
        if (err == HA_ERR_KEY_NOT_FOUND)
        {
            info->current_idx++;
            continue; /* skip stale entry */
        }
        if (err)
        {
            table->status = STATUS_NOT_FOUND;
            DBUG_RETURN(err);
        }

        info->current_idx++;
        table->status = 0;
        DBUG_RETURN(0);
    }

    table->status = STATUS_NOT_FOUND;
    DBUG_RETURN(HA_ERR_END_OF_FILE);
}

int ha_tidesdb::extra(enum ha_extra_function operation)
{
    switch (operation)
    {
        case HA_EXTRA_KEYREAD:
            keyread_only_ = true;
            break;
        case HA_EXTRA_NO_KEYREAD:
            keyread_only_ = false;
            break;
        case HA_EXTRA_WRITE_CAN_REPLACE:
            /* REPLACE INTO -- write_row may skip the dup-check and let
               tidesdb_txn_put overwrite silently.  Only safe when there
               are no secondary indexes (otherwise old index entries must
               still be cleaned up via delete+reinsert). */
            write_can_replace_ = true;
            break;
        case HA_EXTRA_INSERT_WITH_UPDATE:
            /* INSERT ON DUPLICATE KEY UPDATE -- the server needs write_row
               to return HA_ERR_FOUND_DUPP_KEY so it can switch to update_row. */
            break;
        case HA_EXTRA_WRITE_CANNOT_REPLACE:
            write_can_replace_ = false;
            break;
        case HA_EXTRA_PREPARE_FOR_DROP:
            /* Table is about to be dropped -- skip fsync overhead */
            break;
        default:
            break;
    }
    return 0;
}

/* ******************** Locking ******************** */

/*
  Lazy txn creation.  Gets the per-connection TidesDB txn (shared by
  all handler objects on this connection).  The txn spans the entire
  BEGIN...COMMIT block, not just one statement.
*/
int ha_tidesdb::ensure_stmt_txn()
{
    if (stmt_txn) return 0;

    THD *thd = cached_thd_ ? cached_thd_ : ha_thd();

    /* Isolation resolution mirrors the external_lock path:
         DDL                        -> READ_COMMITTED (avoids unbounded
                                       read-set growth across a long scan).
         autocommit single-stmt DML -> READ_COMMITTED (no concurrent
                                       modification within the same txn).
         multi-statement txn        -> session isolation so write-write
                                       conflict detection stays active.
       Prefer the per-statement cache populated by external_lock; fall
       back to the live THD call only when external_lock hasn't run yet
       (e.g. some DDL callbacks). */
    int sql_cmd;
    bool is_autocommit;
    if (cached_stmt_shape_valid_)
    {
        sql_cmd = cached_sql_cmd_;
        is_autocommit = cached_is_autocommit_;
    }
    else
    {
        sql_cmd = thd_sql_command(thd);
        is_autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
    }
    bool is_ddl =
        (sql_cmd == SQLCOM_ALTER_TABLE || sql_cmd == SQLCOM_CREATE_INDEX ||
         sql_cmd == SQLCOM_DROP_INDEX || sql_cmd == SQLCOM_TRUNCATE || sql_cmd == SQLCOM_OPTIMIZE ||
         sql_cmd == SQLCOM_CREATE_TABLE || sql_cmd == SQLCOM_DROP_TABLE);
    tidesdb_isolation_level_t effective_iso;
    if (is_ddl || is_autocommit)
        effective_iso = TDB_ISOLATION_READ_COMMITTED;
    else
        /* Honour session isolation regardless of pessimistic_locking.
           Lock manager and library OCC compose -- the locks serialise
           hot-row write contention, and OCC continues to enforce the
           session's chosen isolation semantics (snapshot reads under
           SNAPSHOT, read-set tracking under REPEATABLE_READ, full SSI
           under SERIALIZABLE).  Earlier revisions silently downgraded
           to READ_COMMITTED here, which broke higher isolation levels
           when pessimistic_locking was on. */
        effective_iso = resolve_effective_isolation(
            thd, share ? share->isolation_level : TDB_ISOLATION_SNAPSHOT);
    tidesdb_trx_t *trx = get_or_create_trx(thd, ht, effective_iso);
    if (!trx) return HA_ERR_OUT_OF_MEM;

    stmt_txn = trx->txn;
    return 0;
}

int ha_tidesdb::external_lock(THD *thd, int lock_type)
{
    DBUG_ENTER("ha_tidesdb::external_lock");

    if (lock_type != F_UNLCK)
    {
        /* We resolve per-statement THD shape once and cache to ensure_stmt_txn
           reads the cache instead of re-calling thd_sql_command() and
           thd_test_options(). */
        int sql_cmd = thd_sql_command(thd);
        bool is_ddl = (sql_cmd == SQLCOM_ALTER_TABLE || sql_cmd == SQLCOM_CREATE_INDEX ||
                       sql_cmd == SQLCOM_DROP_INDEX || sql_cmd == SQLCOM_TRUNCATE ||
                       sql_cmd == SQLCOM_OPTIMIZE || sql_cmd == SQLCOM_CREATE_TABLE ||
                       sql_cmd == SQLCOM_DROP_TABLE);
        bool is_autocommit = !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);

        cached_sql_cmd_ = sql_cmd;
        cached_is_autocommit_ = is_autocommit;
        cached_stmt_shape_valid_ = true;
        stmt_is_update_or_delete_ = (sql_cmd == SQLCOM_UPDATE || sql_cmd == SQLCOM_UPDATE_MULTI ||
                                     sql_cmd == SQLCOM_DELETE || sql_cmd == SQLCOM_DELETE_MULTI);

        /* Anchor the per-statement back-pressure deadline so a multi-call
           statement charges all its waits against the same budget. */
        {
            ulong bp_ms = tdb_backpressure_timeout_ms(thd);
            if (bp_ms > 0)
            {
                tdb_stmt_bp_deadline_ =
                    std::chrono::steady_clock::now() + std::chrono::milliseconds(bp_ms);
                tdb_stmt_bp_deadline_valid_ = true;
            }
        }

        tidesdb_isolation_level_t effective_iso;
        if (is_ddl || is_autocommit)
            effective_iso = TDB_ISOLATION_READ_COMMITTED;
        else
            effective_iso = resolve_effective_isolation(
                thd, share ? share->isolation_level : TDB_ISOLATION_SNAPSHOT);
        tidesdb_trx_t *trx = get_or_create_trx(thd, ht, effective_iso);
        if (!trx) DBUG_RETURN(HA_ERR_OUT_OF_MEM);

        stmt_txn = trx->txn;
        stmt_txn_dirty = false;
        stmt_has_write_lock_ |= (lock_type == F_WRLCK);

        /* We cache THD and trx pointers for fast access in hot paths
           (index_read_map, update_row, delete_row, ensure_stmt_txn).
           Eliminates ha_thd() virtual dispatch and thd_get_ha_data()
           hash lookup on every row operation. */
        cached_thd_ = thd;
        cached_trx_ = trx;

        trans_register_ha(thd, false, ht, 0);

        if (!is_autocommit) trans_register_ha(thd, true, ht, 0);
    }
    else
    {
        /* For multi-statement transactions (BEGIN...COMMIT), the txn stays
           the same across statements.  Preserve scan_iter and dup_iter_cache
           across READ-ONLY statements so the next statement can reuse them
           (avoids O(sstables) merge-heap rebuild).
           After WRITE statements, iterators must be invalidated because
           new txn ops (puts/deletes) are not visible to iterators created
           before those ops were added.  For autocommit, always free. */
        bool in_multi_stmt =
            cached_stmt_shape_valid_
                ? !cached_is_autocommit_
                : (bool)thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN);
        if (!in_multi_stmt || stmt_txn_dirty)
        {
            if (scan_iter)
            {
                tidesdb_iter_free(scan_iter);
                scan_iter = NULL;
                scan_iter_cf_ = NULL;
                scan_iter_txn_ = NULL;
            }
            if (dup_iter_count_ > 0) free_dup_iter_cache();
        }

        /* We bump update_time once per write-statement for information_schema.
           We use cached_time_ if available to avoid another time() syscall. */
        if (stmt_txn_dirty && share)
            share->update_time.store(cached_time_valid_ ? cached_time_ : time(0),
                                     std::memory_order_relaxed);

        /* We invalidate all per-statement caches so the next statement
           picks up any changes (key rotation, session variable changes,
           clock advance). */
        enc_key_ver_valid_ = false;
        cached_time_valid_ = false;
        cached_thdvars_valid_ = false;

        stmt_txn = NULL;
        stmt_txn_dirty = false;
        stmt_has_write_lock_ = false;
        stmt_is_update_or_delete_ = false;
        tdb_stmt_bp_deadline_valid_ = false;
        cached_thd_ = NULL;
        cached_trx_ = NULL;

        /* We invalidate statement shape cache last so the above checks still
           see it. */
        cached_stmt_shape_valid_ = false;
    }

    DBUG_RETURN(0);
}

THR_LOCK_DATA **ha_tidesdb::store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type)
{
    /* With lock_count()=0 MariaDB skips THR_LOCK entirely.
       store_lock is still called for informational purposes but we
       do not push into the 'to' array (same pattern as InnoDB).

       However, we use this callback to detect locking reads
       (SELECT ... FOR UPDATE, SELECT ... IN SHARE MODE) and
       data-modifying statements.  MariaDB calls store_lock() before
       external_lock(), so we can set stmt_has_write_lock_ here for
       the pessimistic row lock path.

       InnoDB uses store_lock() to set m_prebuilt->select_lock_type
       to LOCK_S/LOCK_X for these cases.  We emulate this by detecting
       the same lock_type values and setting our write-lock flag.

       We flag locking READS (SELECT ... FOR UPDATE, SELECT ... IN
       SHARE MODE) so the pessimistic row lock path in index_read_map()
       acquires locks for serialization.

       SELECT ... FOR UPDATE passes lock_type >= TL_FIRST_WRITE.
       SELECT ... IN SHARE MODE passes TL_READ_WITH_SHARED_LOCKS.

       Inside stored procedures, thd_sql_command() returns SQLCOM_CALL
       (not SQLCOM_SELECT), so we cannot filter by SQL command.
       Instead we use the lock_type directly.  This means UPDATE/DELETE
       statements also set stmt_has_write_lock_=true, but that's OK
       because we removed lock acquisition from update_row()/delete_row()
       - only index_read_map() acquires pessimistic locks now, and only
       for PK exact matches (the SELECT ... FOR UPDATE pattern). */
    if (lock_type == TL_READ_WITH_SHARED_LOCKS || lock_type >= TL_FIRST_WRITE)
    {
        stmt_has_write_lock_ = true;
    }
    return to;
}

/* ******************** Online DDL ******************** */

/*
  Classify ALTER TABLE operations into INSTANT / INPLACE / COPY.

  INSTANT     metadata-only changes (.frm rewrite, no engine work):
              rename column/index, change default, change table options,
              ADD COLUMN, DROP COLUMN (row format is self-describing via
              the ROW_HEADER_MAGIC header written by serialize_row)
  INPLACE     add/drop secondary indexes (create/drop CFs, populate)
  COPY        column type changes, PK changes
*/
enum_alter_inplace_result ha_tidesdb::check_if_supported_inplace_alter(
    TABLE *altered_table, Alter_inplace_info *ha_alter_info)
{
    DBUG_ENTER("ha_tidesdb::check_if_supported_inplace_alter");

    alter_table_operations flags = ha_alter_info->handler_flags;

    /* Operations that are pure metadata (INSTANT).
       ADD/DROP COLUMN is instant because the packed row format includes
       a header with the stored null_bytes and field_count, so
       deserialize_row adapts to rows written with any prior schema. */
    static const alter_table_operations TIDESDB_INSTANT =
        ALTER_COLUMN_NAME | ALTER_RENAME_COLUMN | ALTER_CHANGE_COLUMN_DEFAULT |
        ALTER_COLUMN_DEFAULT | ALTER_COLUMN_OPTION | ALTER_CHANGE_CREATE_OPTION |
        ALTER_DROP_CHECK_CONSTRAINT | ALTER_VIRTUAL_GCOL_EXPR | ALTER_RENAME | ALTER_RENAME_INDEX |
        ALTER_INDEX_IGNORABILITY | ALTER_ADD_COLUMN | ALTER_DROP_COLUMN |
        ALTER_STORED_COLUMN_ORDER | ALTER_VIRTUAL_COLUMN_ORDER;

    /* Operations we can do inplace (add/drop secondary indexes) */
    static const alter_table_operations TIDESDB_INPLACE_INDEX =
        ALTER_ADD_NON_UNIQUE_NON_PRIM_INDEX | ALTER_DROP_NON_UNIQUE_NON_PRIM_INDEX |
        ALTER_ADD_UNIQUE_INDEX | ALTER_DROP_UNIQUE_INDEX | ALTER_ADD_INDEX | ALTER_DROP_INDEX |
        ALTER_INDEX_ORDER;

    /* If only instant operations, return INSTANT */
    if (!(flags & ~TIDESDB_INSTANT)) DBUG_RETURN(HA_ALTER_INPLACE_INSTANT);

    /* If only instant + index operations, return INPLACE with no lock.
       TidesDB handles all concurrency via MVCC internally -- the index
       population scan runs inside its own transaction and does not need
       server-level MDL blocking. */
    if (!(flags & ~(TIDESDB_INSTANT | TIDESDB_INPLACE_INDEX)))
    {
        /**** Changing PK requires full rebuild */
        if (flags & (ALTER_ADD_PK_INDEX | ALTER_DROP_PK_INDEX))
        {
            ha_alter_info->unsupported_reason = "TidesDB cannot change PRIMARY KEY inplace";
            DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
        }
        /* FULLTEXT and SPATIAL indexes ride ALTER_ADD_INDEX but cannot be
           populated by the inplace builder -- the per-row loops in
           inplace_alter_table skip them, so the CF would stay empty and
           later MATCH AGAINST / MBRWithin would silently return no rows
           against pre-existing data.  Forcing COPY routes every row
           through write_row, which knows how to maintain these CFs. */
        if (ha_alter_info->index_add_count > 0)
        {
            for (uint a = 0; a < ha_alter_info->index_add_count; a++)
            {
                uint key_num = ha_alter_info->index_add_buffer[a];
                KEY *new_key = &ha_alter_info->key_info_buffer[key_num];
                if (is_fts_index(new_key))
                {
                    ha_alter_info->unsupported_reason = "TidesDB cannot add FULLTEXT index inplace";
                    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
                }
                if (is_spatial_index(new_key))
                {
                    ha_alter_info->unsupported_reason = "TidesDB cannot add SPATIAL index inplace";
                    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
                }
            }
        }
        DBUG_RETURN(HA_ALTER_INPLACE_NO_LOCK);
    }

    /* Everything else requires COPY */
    DBUG_RETURN(HA_ALTER_INPLACE_NOT_SUPPORTED);
}

/*
  Create CFs for newly added indexes.
  Called with shared MDL lock (concurrent DML is allowed).
*/
bool ha_tidesdb::prepare_inplace_alter_table(TABLE *altered_table,
                                             Alter_inplace_info *ha_alter_info)
{
    DBUG_ENTER("ha_tidesdb::prepare_inplace_alter_table");

    ha_tidesdb_inplace_ctx *ctx;
    try
    {
        ctx = new ha_tidesdb_inplace_ctx();
    }
    catch (...)
    {
        DBUG_RETURN(true);
    }
    ha_alter_info->handler_ctx = ctx;

    tidesdb_column_family_config_t cfg = build_cf_config(TDB_TABLE_OPTIONS(table));

    std::string base_cf = share->cf_name;

    if (ha_alter_info->index_add_count > 0)
    {
        for (uint a = 0; a < ha_alter_info->index_add_count; a++)
        {
            uint key_num = ha_alter_info->index_add_buffer[a];
            KEY *new_key = &ha_alter_info->key_info_buffer[key_num];

            if (new_key->flags & HA_NOSAME &&
                altered_table->s->primary_key < altered_table->s->keys &&
                key_num == altered_table->s->primary_key)
                continue;

            std::string idx_cf = base_cf + CF_INDEX_INFIX + new_key->name.str;

            tidesdb_drop_column_family(tdb_global, idx_cf.c_str());

            tidesdb_column_family_config_t idx_cfg = cfg;
            ha_index_option_struct *iopts = new_key->option_struct;
            if (iopts) idx_cfg.use_btree = iopts->use_btree ? 1 : 0;

            int rc = tidesdb_create_column_family(tdb_global, idx_cf.c_str(), &idx_cfg);
            if (rc != TDB_SUCCESS)
            {
                sql_print_error("[TIDESDB] inplace ADD INDEX: failed to create CF '%s' (err=%d)",
                                idx_cf.c_str(), rc);
                my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to create index CF");
                DBUG_RETURN(true);
            }

            tidesdb_column_family_t *icf = tidesdb_get_column_family(tdb_global, idx_cf.c_str());
            if (!icf)
            {
                sql_print_error("[TIDESDB] inplace ADD INDEX: CF '%s' not found after create",
                                idx_cf.c_str());
                my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] index CF not found after create");
                DBUG_RETURN(true);
            }

            ctx->add_cfs.push_back(icf);
            ctx->add_cf_names.push_back(idx_cf);
            ctx->add_key_nums.push_back(key_num);
        }
    }

    if (ha_alter_info->index_drop_count > 0)
    {
        for (uint d = 0; d < ha_alter_info->index_drop_count; d++)
        {
            KEY *old_key = ha_alter_info->index_drop_buffer[d];
            uint old_key_num = (uint)(old_key - table->key_info);
            if (old_key_num < share->idx_cf_names.size() &&
                !share->idx_cf_names[old_key_num].empty())
            {
                ctx->drop_cf_names.push_back(share->idx_cf_names[old_key_num]);
            }
        }
    }

    DBUG_RETURN(false);
}

/*
  Inplace phase -- we populate newly added indexes by scanning the table.
  Called with no MDL lock blocking (HA_ALTER_INPLACE_NO_LOCK).
*/
bool ha_tidesdb::inplace_alter_table(TABLE *altered_table, Alter_inplace_info *ha_alter_info)
{
    DBUG_ENTER("ha_tidesdb::inplace_alter_table");

    ha_tidesdb_inplace_ctx *ctx = static_cast<ha_tidesdb_inplace_ctx *>(ha_alter_info->handler_ctx);

    if (!ctx || ctx->add_cfs.empty())
        DBUG_RETURN(false); /* Nothing to populate (drop-only or instant) */

    /* We mark all columns readable on the altered table since we read
       fields via make_sort_key_part during index key construction. */
    MY_BITMAP *old_map = tmp_use_all_columns(altered_table, &altered_table->read_set);

    /* We do a full table scan to populate the new secondary indexes.
       We use the altered_table's key_info for building index keys,
       since that matches the new key numbering. */

    /* We always use READ_COMMITTED for index population.  The scan reads
       potentially millions of rows; higher isolation levels would track
       each key in the read-set, causing unbounded memory growth.  Index
       builds are DDL and never need OCC conflict detection. */
    tidesdb_txn_t *txn = NULL;
    int rc = tidesdb_txn_begin_with_isolation(tdb_global, TDB_ISOLATION_READ_COMMITTED, &txn);
    if (rc != TDB_SUCCESS || !txn)
    {
        sql_print_error("[TIDESDB] inplace ADD INDEX: txn_begin failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to begin txn for index build");
        tmp_restore_column_map(&altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }

    tidesdb_iter_t *iter = NULL;
    rc = tdb_iter_new_blocking(ha_thd(), txn, share->cf, &iter);
    if (rc != TDB_SUCCESS || !iter)
    {
        tidesdb_txn_free(txn);
        sql_print_error("[TIDESDB] inplace ADD INDEX: iter_new failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] failed to create iterator for index build");
        tmp_restore_column_map(&altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }
    tidesdb_iter_seek_to_first(iter);

    ha_rows rows_processed = 0;

    /* For UNIQUE indexes, we track seen index-column prefixes to detect
       duplicates.  If a duplicate is found we must abort the ALTER.
       unordered_set gives O(1) amortized lookup vs O(log n) for std::set,
       which matters for tables with millions of rows. */
    std::vector<bool> idx_is_unique(ctx->add_cfs.size(), false);
    std::vector<std::unordered_set<std::string>> idx_seen(ctx->add_cfs.size());
    for (uint a = 0; a < ctx->add_cfs.size(); a++)
    {
        uint key_num = ctx->add_key_nums[a];
        KEY *ki = &altered_table->key_info[key_num];
        if (ki->flags & HA_NOSAME) idx_is_unique[a] = true;
    }

    /* We remember the last data key so we can seek directly to it after
       a batch commit (O(n²)). */
    uchar last_data_key[DATA_KEY_BUF_LEN];
    size_t last_data_key_len = 0;

    while (tidesdb_iter_valid(iter))
    {
        uint8_t *key_data = NULL;
        size_t key_size = 0;
        uint8_t *val_data = NULL;
        size_t val_size = 0;

        if (tidesdb_iter_key_value(iter, &key_data, &key_size, &val_data, &val_size) != TDB_SUCCESS)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        if (key_size < KEY_NAMESPACE_LEN || key_data[0] != KEY_NS_DATA)
        {
            tidesdb_iter_next(iter);
            continue;
        }

        if (key_size <= sizeof(last_data_key))
        {
            memcpy(last_data_key, key_data, key_size);
            last_data_key_len = key_size;
        }

        const uchar *pk = key_data + KEY_NAMESPACE_LEN;
        uint pk_len = (uint)(key_size - KEY_NAMESPACE_LEN);

        /* We decode the row into table->record[0].  The field pointers from
           altered_table->key_info will be temporarily repointed (via
           move_field_offset) to read from this buffer. */
        if (share->has_blobs || share->encrypted)
        {
            std::string row_data((const char *)val_data, val_size);
            deserialize_row(table->record[0], row_data);
        }
        else
        {
            deserialize_row(table->record[0], (const uchar *)val_data, val_size);
        }

        /* For each newly added index, build the index entry key.
           altered_table->key_info fields have ptr into altered_table->record[0],
           but the data lives in table->record[0].  We compute ptdiff to
           rebase field pointers to read from the correct buffer.
           Key format matches make_comparable_key()= [null_byte] + sort_string. */
        my_ptrdiff_t ptdiff = (my_ptrdiff_t)(table->record[0] - altered_table->record[0]);

        for (uint a = 0; a < ctx->add_cfs.size(); a++)
        {
            uint key_num = ctx->add_key_nums[a];
            KEY *ki = &altered_table->key_info[key_num];

            /* FULLTEXT and SPATIAL indexes use different population paths */
            if (is_fts_index(ki)) continue;
            if (is_spatial_index(ki)) continue;

            uchar ik[SEC_IDX_KEY_BUF_LEN];
            uint pos = 0;
            for (uint p = 0; p < ki->user_defined_key_parts; p++)
            {
                KEY_PART_INFO *kp = &ki->key_part[p];
                Field *field = kp->field;

                field->move_field_offset(ptdiff);
                if (field->real_maybe_null())
                {
                    if (field->is_null())
                    {
                        ik[pos++] = SORT_KEY_NULL;
                        bzero(ik + pos, kp->length);
                        pos += kp->length;
                        field->move_field_offset(-ptdiff);
                        continue;
                    }
                    ik[pos++] = SORT_KEY_NOT_NULL;
                }
                field->sort_string(ik + pos, kp->length);
                field->move_field_offset(-ptdiff);
                pos += kp->length;
            }

            if (idx_is_unique[a])
            {
                std::string prefix((const char *)ik, pos);
                if (!idx_seen[a].insert(prefix).second)
                {
                    tidesdb_iter_free(iter);
                    tidesdb_txn_rollback(txn);
                    tidesdb_txn_free(txn);
                    tmp_restore_column_map(&altered_table->read_set, old_map);
                    my_error(ER_DUP_ENTRY, MYF(0), "?", altered_table->key_info[key_num].name.str);
                    DBUG_RETURN(true);
                }
            }

            memcpy(ik + pos, pk, pk_len);
            pos += pk_len;

            rc = tdb_txn_put_blocking(ha_thd(), txn, ctx->add_cfs[a], ik, pos, &tdb_empty_val,
                                      sizeof(tdb_empty_val), TIDESDB_TTL_NONE);
            if (rc != TDB_SUCCESS)
            {
                /* A per-row put failure leaves a hole in the new index.
                   Continuing would let commit_inplace_alter_table report
                   success on an index that silently lacks rows, matching
                   the failure mode the batch-commit guard below also
                   refuses to ship.  Abort the ALTER instead. */
                sql_print_error(
                    "[TIDESDB] inplace ADD INDEX: put failed for key %u (err=%d), "
                    "aborting to avoid a partial index",
                    key_num, rc);
                tidesdb_iter_free(iter);
                tidesdb_txn_rollback(txn);
                tidesdb_txn_free(txn);
                tmp_restore_column_map(&altered_table->read_set, old_map);
                my_error(ER_INTERNAL_ERROR, MYF(0),
                         "[TIDESDB] per-row put failed during index build");
                DBUG_RETURN(true);
            }
        }

        rows_processed++;

        /* We check for KILL signal periodically so the user can cancel
           long-running index builds via KILL <thread_id>. */
        if ((rows_processed % TIDESDB_INDEX_BUILD_BATCH) == 0 && thd_killed(ha_thd()))
        {
            tidesdb_iter_free(iter);
            tidesdb_txn_rollback(txn);
            tidesdb_txn_free(txn);
            tmp_restore_column_map(&altered_table->read_set, old_map);
            my_error(ER_QUERY_INTERRUPTED, MYF(0));
            DBUG_RETURN(true);
        }

        if (rows_processed % TIDESDB_INDEX_BUILD_BATCH == 0)
        {
            {
                int crc = tidesdb_txn_commit(txn);
                if (crc != TDB_SUCCESS)
                {
                    /* A failed batch commit drops this batch of index entries.
                       Carrying on would finish the build and report success
                       with an index that is silently missing rows, so abort
                       the ALTER instead. */
                    sql_print_error(
                        "[TIDESDB] inplace ADD INDEX: batch commit failed rc=%d, "
                        "aborting to avoid a partial index",
                        crc);
                    tidesdb_iter_free(iter);
                    tidesdb_txn_rollback(txn);
                    tidesdb_txn_free(txn);
                    tmp_restore_column_map(&altered_table->read_set, old_map);
                    my_error(ER_INTERNAL_ERROR, MYF(0),
                             "[TIDESDB] batch commit failed during index build");
                    DBUG_RETURN(true);
                }
            }
            tidesdb_iter_free(iter);

            /* We reset the txn with READ_COMMITTED -- index builds
               don't need snapshot consistency across batches. */
            int rrc = tidesdb_txn_reset(txn, TDB_ISOLATION_READ_COMMITTED);
            if (rrc != TDB_SUCCESS)
            {
                sql_print_warning(
                    "[TIDESDB] inplace ADD INDEX: tidesdb_txn_reset failed (rc=%d), "
                    "falling back to free+begin",
                    rrc);
                tidesdb_txn_free(txn);
                txn = NULL;
                rc = tidesdb_txn_begin_with_isolation(tdb_global, TDB_ISOLATION_READ_COMMITTED,
                                                      &txn);
                if (rc != TDB_SUCCESS || !txn)
                {
                    sql_print_error("[TIDESDB] inplace ADD INDEX: batch txn_begin failed");
                    my_error(ER_INTERNAL_ERROR, MYF(0),
                             "[TIDESDB] batch txn failed during index build");
                    tmp_restore_column_map(&altered_table->read_set, old_map);
                    DBUG_RETURN(true);
                }
            }
            iter = NULL;
            rc = tdb_iter_new_blocking(ha_thd(), txn, share->cf, &iter);
            if (rc != TDB_SUCCESS || !iter)
            {
                tidesdb_txn_free(txn);
                my_error(ER_INTERNAL_ERROR, MYF(0),
                         "[TIDESDB] batch iter failed during index build");
                tmp_restore_column_map(&altered_table->read_set, old_map);
                DBUG_RETURN(true);
            }
            int src = tidesdb_iter_seek(iter, last_data_key, last_data_key_len);
            if (src != TDB_SUCCESS)
            {
                sql_print_warning("[TIDESDB] inplace ADD INDEX: iter_seek failed rc=%d", src);
                break; /* end scan gracefully */
            }
            if (tidesdb_iter_valid(iter)) tidesdb_iter_next(iter);
            continue; /* Don't call iter_next again */
        }

        tidesdb_iter_next(iter);
    }

    tidesdb_iter_free(iter);

    rc = tidesdb_txn_commit(txn);
    if (rc != TDB_SUCCESS) tidesdb_txn_rollback(txn);
    tidesdb_txn_free(txn);

    if (rc != TDB_SUCCESS)
    {
        sql_print_error("[TIDESDB] inplace ADD INDEX: final commit failed (err=%d)", rc);
        my_error(ER_INTERNAL_ERROR, MYF(0), "[TIDESDB] final commit failed during index build");
        tmp_restore_column_map(&altered_table->read_set, old_map);
        DBUG_RETURN(true);
    }
    tmp_restore_column_map(&altered_table->read_set, old_map);
    DBUG_RETURN(false);
}

/*
  Commit or rollback the inplace ALTER.
  On commit       drop old index CFs, update share->idx_cfs for new table shape.
  On rollback     drop newly created CFs.
*/
bool ha_tidesdb::commit_inplace_alter_table(TABLE *altered_table, Alter_inplace_info *ha_alter_info,
                                            bool commit)
{
    DBUG_ENTER("ha_tidesdb::commit_inplace_alter_table");

    ha_tidesdb_inplace_ctx *ctx = static_cast<ha_tidesdb_inplace_ctx *>(ha_alter_info->handler_ctx);

    ha_alter_info->group_commit_ctx = NULL;

    if (!ctx) DBUG_RETURN(false);

    /* We free any cached iterators before dropping CFs.  The connection's
       scan_iter and dup_iter_cache_ may hold merge-heap references to
       SSTables in CFs about to be dropped. */
    if (scan_iter)
    {
        tidesdb_iter_free(scan_iter);
        scan_iter = NULL;
        scan_iter_cf_ = NULL;
        scan_iter_txn_ = NULL;
    }
    free_dup_iter_cache();

    if (!commit)
    {
        /* Rollback, we drop any CFs we created for new indexes */
        for (const auto &cf_name : ctx->add_cf_names)
            tidesdb_drop_column_family(tdb_global, cf_name.c_str());
        DBUG_RETURN(false);
    }

    /* Commit, we drop CFs for removed indexes */
    for (const auto &cf_name : ctx->drop_cf_names)
    {
        int rc = tidesdb_drop_column_family(tdb_global, cf_name.c_str());
        if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
            sql_print_warning("[TIDESDB] commit ALTER: failed to drop CF '%s' (err=%d)",
                              cf_name.c_str(), rc);
    }

    /* We rebuild share->idx_cfs and idx_cf_names based on the new table's keys.
       Since we hold exclusive MDL, no other handler is using the share. */
    lock_shared_ha_data();
    share->idx_cfs.clear();
    share->idx_cf_names.clear();

    uint new_pk = altered_table->s->primary_key;
    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        if (new_pk != MAX_KEY && i == new_pk)
        {
            share->idx_cfs.push_back(NULL);
            share->idx_cf_names.push_back("");
            continue;
        }
        std::string idx_name;
        tidesdb_column_family_t *icf = resolve_idx_cf(
            tdb_global, share->cf_name, altered_table->key_info[i].name.str, idx_name);
        share->idx_cfs.push_back(icf);
        share->idx_cf_names.push_back(idx_name);
    }

    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        share->idx_comp_key_len[i] = comparable_key_length(&altered_table->key_info[i]);
        share->idx_is_fts[i] = is_fts_index(&altered_table->key_info[i]);
        share->idx_is_spatial[i] = is_spatial_index(&altered_table->key_info[i]);
    }

    share->idx_cover.assign(altered_table->s->keys,
                            std::vector<bool>(altered_table->s->fields, false));
    for (uint i = 0; i < altered_table->s->keys; i++)
    {
        const KEY *ki = &altered_table->key_info[i];
        for (uint p = 0; p < ki->user_defined_key_parts; p++)
        {
            uint fnr = ki->key_part[p].fieldnr;
            if (fnr > 0 && fnr - 1 < altered_table->s->fields) share->idx_cover[i][fnr - 1] = true;
        }
        if (altered_table->s->primary_key != MAX_KEY && i != altered_table->s->primary_key)
        {
            const KEY *pk_key = &altered_table->key_info[altered_table->s->primary_key];
            for (uint p = 0; p < pk_key->user_defined_key_parts; p++)
            {
                uint fnr = pk_key->key_part[p].fieldnr;
                if (fnr > 0 && fnr - 1 < altered_table->s->fields)
                    share->idx_cover[i][fnr - 1] = true;
            }
        }
    }
    share->num_secondary_indexes = 0;
    for (uint i = 0; i < share->idx_cfs.size(); i++)
        if (share->idx_cfs[i]) share->num_secondary_indexes++;

    /* If table options changed (SYNC_MODE, COMPRESSION, BLOOM_FPR, etc.),
       we apply them to the live CF(s) so they take effect immediately instead
       of only being persisted in the .frm. */
    if (ha_alter_info->handler_flags & ALTER_CHANGE_CREATE_OPTION)
    {
        tidesdb_column_family_config_t cfg = build_cf_config(TDB_TABLE_OPTIONS(altered_table));

        /* Main data CF */
        if (share->cf)
        {
            int rc = tidesdb_cf_update_runtime_config(share->cf, &cfg, 1);
            if (rc != TDB_SUCCESS)
                sql_print_warning(
                    "[TIDESDB] ALTER: failed to update runtime config for "
                    "data CF '%s' (err=%d)",
                    share->cf_name.c_str(), rc);
        }

        for (uint i = 0; i < share->idx_cfs.size(); i++)
        {
            if (share->idx_cfs[i])
            {
                tidesdb_column_family_config_t idx_cfg = cfg;
                if (i < altered_table->s->keys && altered_table->key_info[i].option_struct)
                {
                    ha_index_option_struct *iopts = altered_table->key_info[i].option_struct;
                    idx_cfg.use_btree = iopts->use_btree ? 1 : 0;
                }

                int rc = tidesdb_cf_update_runtime_config(share->idx_cfs[i], &idx_cfg, 1);
                if (rc != TDB_SUCCESS)
                    sql_print_warning(
                        "[TIDESDB] ALTER: failed to update runtime config for "
                        "index CF '%s' (err=%d)",
                        share->idx_cf_names[i].c_str(), rc);
            }
        }

        if (TDB_TABLE_OPTIONS(altered_table))
        {
            uint iso_idx = TDB_TABLE_OPTIONS(altered_table)->isolation_level;
            if (iso_idx < array_elements(tdb_isolation_map))
                share->isolation_level = (tidesdb_isolation_level_t)tdb_isolation_map[iso_idx];
            share->default_ttl = TDB_TABLE_OPTIONS(altered_table)->ttl;
            share->has_ttl = (share->default_ttl > 0 || share->ttl_field_idx >= 0);
            share->encrypted = TDB_TABLE_OPTIONS(altered_table)->encrypted;
            if (share->encrypted)
                share->encryption_key_id =
                    (uint)TDB_TABLE_OPTIONS(altered_table)->encryption_key_id;
        }
    }

    share->stats_refresh_us.store(0, std::memory_order_relaxed);
    unlock_shared_ha_data();

    /* We update .frm in schema CF after ALTER.  When discover_table is
       registered MariaDB may skip writing .frm to disk, so prefer the
       in-memory image from the altered TABLE_SHARE. */
    if (altered_table->s->frm_image)
        schema_cf_store_frm(table->s->path.str, altered_table->s->frm_image->str,
                            altered_table->s->frm_image->length);
    else
        schema_cf_store_frm(table->s->path.str);

    DBUG_RETURN(false);
}

/*
  Tell MariaDB whether changing table options requires a rebuild.
  For TidesDB, changing options like SYNC_MODE, TTL, etc. is always
  compatible -- the .frm is rewritten and re-read on next open().
*/
bool ha_tidesdb::check_if_incompatible_data(HA_CREATE_INFO *create_info, uint table_changes)
{
    /* If only table options changed (not column types), data is compatible */
    if (table_changes == IS_EQUAL_YES) return COMPATIBLE_DATA_YES;
    return COMPATIBLE_DATA_NO;
}

/* ******************** rename_table (ALTER TABLE / RENAME) ******************** */

int ha_tidesdb::rename_table(const char *from, const char *to)
{
    DBUG_ENTER("ha_tidesdb::rename_table");

    std::string old_cf = path_to_cf_name(from);
    std::string new_cf = path_to_cf_name(to);

    /* If the destination CF already exists (stale from a previous ALTER),
       drop it first so the rename can proceed. */
    tidesdb_drop_column_family(tdb_global, new_cf.c_str());

    int rc = tidesdb_rename_column_family(tdb_global, old_cf.c_str(), new_cf.c_str());
    if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
    {
        sql_print_error("[TIDESDB] Failed to rename CF '%s' -> '%s' (err=%d)", old_cf.c_str(),
                        new_cf.c_str(), rc);
        DBUG_RETURN(tdb_rc_to_ha(rc, "rename_table"));
    }

    {
        std::string prefix = old_cf + CF_INDEX_INFIX;
        char **names = NULL;
        int count = 0;
        if (tidesdb_list_column_families(tdb_global, &names, &count) == TDB_SUCCESS && names)
        {
            for (int i = 0; i < count; i++)
            {
                if (!names[i]) continue;
                std::string cf_str(names[i]);
                if (cf_str.compare(0, prefix.size(), prefix) == 0)
                {
                    std::string suffix = cf_str.substr(prefix.size());
                    std::string new_idx = new_cf + CF_INDEX_INFIX + suffix;

                    tidesdb_drop_column_family(tdb_global, new_idx.c_str());
                    rc = tidesdb_rename_column_family(tdb_global, cf_str.c_str(), new_idx.c_str());
                    if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
                        sql_print_error("[TIDESDB] Failed to rename idx CF '%s' -> '%s' (err=%d)",
                                        cf_str.c_str(), new_idx.c_str(), rc);
                }
                tidesdb_free(names[i]);
            }
            tidesdb_free(names);
        }
    }

    schema_cf_rename(from, to);

    DBUG_RETURN(0);
}

/* ******************** delete_table (DROP TABLE) ******************** */

/*
  Force-remove a directory tree from disk.  Used as a safety net after
  tidesdb_drop_column_family() because the library's internal
  remove_directory() can fail silently (e.g. open fds from block cache,
  mmap, or background workers).  If stale SSTables survive, the next
  CREATE TABLE with the same name inherits them -- catastrophic for
  performance (bloom filters pass on every SSTable since keys overlap).
*/
static void force_remove_cf_dir(const std::string &cf_name)
{
    char dir[FN_REFLEN];
    const char sep[] = {FN_LIBCHAR, 0};
    strxnmov(dir, sizeof(dir) - 1, tdb_path.c_str(), sep, cf_name.c_str(), NullS);

    MY_STAT st;
    if (!my_stat(dir, &st, MYF(0))) return; /* already gone */

    /* my_rmtree() is MariaDB's portable recursive directory removal
       (handles Windows, symlinks, read-only attrs, etc.). */
    if (my_rmtree(dir, MYF(0)) != 0)
        sql_print_warning("[TIDESDB] force_remove_cf_dir failed for %s", dir);
}

/*
  Shared drop logic used by both the handlerton callback (hton->drop_table)
  and the handler method (ha_tidesdb::delete_table).  Drops the main data CF
  and all secondary index CFs, then force-removes their directories.
  Returns 0 on success.
*/
static int tidesdb_drop_table_impl(const char *path)
{
    if (!tdb_global) return 0;

    /* Replica mode is read-only against the object store, so the library
       rejects tidesdb_drop_column_family with TDB_ERR_READONLY.  MariaDB's
       own init/upgrade paths invoke drop_table on stale system tables and
       repeatedly trigger that rejection, which surfaces as scary [ERROR]
       lines in the server log even though the work is genuinely a no-op
       for a replica.  Skip the library call entirely on replicas and let
       the local directory cleanup (if any) be driven by the next sync. */
    if (srv_replica_mode)
    {
        sql_print_information(
            "[TIDESDB] drop_table skipped on replica for '%s' (replica is read-only)", path);
        return 0;
    }

    std::string cf_name = ha_tidesdb::path_to_cf_name(path);

    /* We collect secondary index CF names before dropping so we can
       force-remove their directories afterwards. */
    std::vector<std::string> idx_cf_names;
    {
        std::string prefix = cf_name + CF_INDEX_INFIX;
        char **names = NULL;
        int count = 0;
        if (tidesdb_list_column_families(tdb_global, &names, &count) == TDB_SUCCESS && names)
        {
            for (int i = 0; i < count; i++)
            {
                if (!names[i]) continue;
                if (strncmp(names[i], prefix.c_str(), prefix.size()) == 0)
                    idx_cf_names.push_back(names[i]);
                tidesdb_free(names[i]);
            }
            tidesdb_free(names);
        }
    }

    int rc = tidesdb_drop_column_family(tdb_global, cf_name.c_str());
    if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
    {
        sql_print_error("[TIDESDB] Failed to drop CF '%s' (err=%d)", cf_name.c_str(), rc);
        return rc;
    }

    for (const auto &idx_name : idx_cf_names)
        tidesdb_drop_column_family(tdb_global, idx_name.c_str());

    force_remove_cf_dir(cf_name);
    for (const auto &idx_name : idx_cf_names) force_remove_cf_dir(idx_name);

    schema_cf_delete(path);

    return 0;
}

/*
  Handlerton-level drop_table callback.  MariaDB 12.x calls hton->drop_table
  instead of handler::delete_table.  Must return 0 on success, not -1.
*/
static int tidesdb_hton_drop_table(handlerton *, const char *path)
{
    return tidesdb_drop_table_impl(path);
}

/*
  Extract the database name from a directory path handed to drop_database.
  The server passes something like "./test/" or "/var/lib/mysql/test/";
  we strip trailing separators and return the final path component.
*/
static std::string tidesdb_path_to_db_name(const char *path)
{
    if (!path) return std::string();
    std::string p(path);
    while (!p.empty() && (p.back() == FN_LIBCHAR || p.back() == '/')) p.pop_back();
    size_t slash = p.find_last_of("/\\");
    if (slash != std::string::npos) p = p.substr(slash + 1);
    return p;
}

/*
  Handlerton-level drop_database callback.  MariaDB calls this when the
  server-side DROP DATABASE has finished removing .frm files from the db
  directory.  Without this hook, TidesDB column families whose .frm was
  already unlinked (and any object-store-mode entries in schema_cf) would
  outlive the database and accumulate on disk.

  We enumerate every CF whose name starts with "<db_name>__" (the prefix
  path_to_cf_name builds for a table in that database -- which also
  captures all "db__tbl__idx_*" secondary-index CFs) and drop each.
*/
static void tidesdb_hton_drop_database(handlerton *, char *path)
{
    if (!tdb_global || !path) return;

    /* Same rationale as tidesdb_drop_table_impl -- replica mode is
       read-only and the library rejects every drop with TDB_ERR_READONLY,
       so skip the call rather than spamming the log. */
    if (srv_replica_mode)
    {
        sql_print_information(
            "[TIDESDB] drop_database skipped on replica for '%s' (replica is read-only)", path);
        return;
    }

    std::string db = tidesdb_path_to_db_name(path);
    if (db.empty()) return;

    std::string prefix = db + CF_DB_TABLE_SEP;

    std::vector<std::string> to_drop;
    {
        char **names = NULL;
        int count = 0;
        if (tidesdb_list_column_families(tdb_global, &names, &count) == TDB_SUCCESS && names)
        {
            for (int i = 0; i < count; i++)
            {
                if (!names[i]) continue;
                if (strncmp(names[i], prefix.c_str(), prefix.size()) == 0)
                    to_drop.emplace_back(names[i]);
                tidesdb_free(names[i]);
            }
            tidesdb_free(names);
        }
    }

    for (const auto &cf_name : to_drop)
    {
        int rc = tidesdb_drop_column_family(tdb_global, cf_name.c_str());
        if (rc != TDB_SUCCESS && rc != TDB_ERR_NOT_FOUND)
            sql_print_warning("[TIDESDB] drop_database: failed to drop CF '%s' (err=%d)",
                              cf_name.c_str(), rc);
        force_remove_cf_dir(cf_name);
    }

    /* We clean up schema CF entries for this database (object-store mode).
       No-op when schema_cf is NULL (local-only mode). */
    schema_cf_delete_db(db);

    if (!to_drop.empty())
        sql_print_information("[TIDESDB] drop_database: removed %zu column famil%s for '%s'",
                              to_drop.size(), to_drop.size() == 1 ? "y" : "ies", db.c_str());
}

int ha_tidesdb::delete_table(const char *name)
{
    DBUG_ENTER("ha_tidesdb::delete_table");
    DBUG_RETURN(tidesdb_drop_table_impl(name));
}

/* ******************** Status variables (SHOW GLOBAL STATUS LIKE 'tidesdb%') ********************
 */

/* Static holders for status variable values.  Populated by the SHOW_FUNC
   callback which queries tidesdb_get_db_stats / tidesdb_get_cache_stats.
   These are global (not per-connection) since they reflect database-wide state. */
static long long srv_stat_column_families;
static long long srv_stat_global_seq;
static long long srv_stat_memtable_bytes;
static long long srv_stat_txn_memory_bytes;
static long long srv_stat_memory_limit;
static long long srv_stat_memory_pressure;
static long long srv_stat_total_sstables;
static long long srv_stat_open_sstables;
static long long srv_stat_data_size_bytes;
static long long srv_stat_immutable_memtables;
static long long srv_stat_flush_pending;
static long long srv_stat_flush_queue;
static long long srv_stat_compaction_queue;
static long long srv_stat_cache_entries;
static long long srv_stat_cache_bytes;
static long long srv_stat_cache_hits;
static long long srv_stat_cache_misses;
static double srv_stat_cache_hit_rate;
static long long srv_stat_cache_partitions;
/* Tombstone aggregates are forward-declared near the top of this file so
   tidesdb_show_status can read them directly.  Their definitions live up
   there. */

#define TIDESQL_VERSION_STR "4.5.4"
#define TIDESQL_VERSION_HEX 0x40504

static const char *srv_stat_version = TIDESQL_VERSION_STR;
static long long srv_stat_version_hex = TIDESQL_VERSION_HEX;

static struct st_mysql_show_var tidesdb_status_variables[] = {
    {"tidesdb_version", (char *)&srv_stat_version, SHOW_CHAR_PTR},
    {"tidesdb_version_hex", (char *)&srv_stat_version_hex, SHOW_LONGLONG},
    {"tidesdb_column_families", (char *)&srv_stat_column_families, SHOW_LONGLONG},
    {"tidesdb_global_sequence", (char *)&srv_stat_global_seq, SHOW_LONGLONG},
    {"tidesdb_memtable_bytes", (char *)&srv_stat_memtable_bytes, SHOW_LONGLONG},
    {"tidesdb_txn_memory_bytes", (char *)&srv_stat_txn_memory_bytes, SHOW_LONGLONG},
    {"tidesdb_memory_limit", (char *)&srv_stat_memory_limit, SHOW_LONGLONG},
    {"tidesdb_memory_pressure", (char *)&srv_stat_memory_pressure, SHOW_LONGLONG},
    {"tidesdb_total_sstables", (char *)&srv_stat_total_sstables, SHOW_LONGLONG},
    {"tidesdb_open_sstables", (char *)&srv_stat_open_sstables, SHOW_LONGLONG},
    {"tidesdb_data_size_bytes", (char *)&srv_stat_data_size_bytes, SHOW_LONGLONG},
    {"tidesdb_immutable_memtables", (char *)&srv_stat_immutable_memtables, SHOW_LONGLONG},
    {"tidesdb_flush_pending", (char *)&srv_stat_flush_pending, SHOW_LONGLONG},
    {"tidesdb_flush_queue", (char *)&srv_stat_flush_queue, SHOW_LONGLONG},
    {"tidesdb_compaction_queue", (char *)&srv_stat_compaction_queue, SHOW_LONGLONG},
    {"tidesdb_cache_entries", (char *)&srv_stat_cache_entries, SHOW_LONGLONG},
    {"tidesdb_cache_bytes", (char *)&srv_stat_cache_bytes, SHOW_LONGLONG},
    {"tidesdb_cache_hits", (char *)&srv_stat_cache_hits, SHOW_LONGLONG},
    {"tidesdb_cache_misses", (char *)&srv_stat_cache_misses, SHOW_LONGLONG},
    {"tidesdb_cache_hit_rate", (char *)&srv_stat_cache_hit_rate, SHOW_DOUBLE},
    {"tidesdb_cache_partitions", (char *)&srv_stat_cache_partitions, SHOW_LONGLONG},
    {"tidesdb_total_tombstones", (char *)&srv_stat_total_tombstones, SHOW_LONGLONG},
    {"tidesdb_tombstone_ratio", (char *)&srv_stat_tombstone_ratio, SHOW_DOUBLE},
    {"tidesdb_max_sst_tombstone_density", (char *)&srv_stat_max_sst_density, SHOW_DOUBLE},
    {"tidesdb_max_sst_tombstone_density_level", (char *)&srv_stat_max_sst_density_level,
     SHOW_LONGLONG},
    {"tidesdb_backpressure_waits", (char *)&srv_stat_backpressure_waits, SHOW_LONGLONG},
    {"tidesdb_backpressure_wait_us", (char *)&srv_stat_backpressure_wait_us, SHOW_LONGLONG},
    {"tidesdb_lock_waits", (char *)&srv_stat_lock_waits, SHOW_LONGLONG},
    {"tidesdb_lock_wait_us", (char *)&srv_stat_lock_wait_us, SHOW_LONGLONG},
    {"tidesdb_lock_deadlocks", (char *)&srv_stat_lock_deadlocks, SHOW_LONGLONG},
    {"tidesdb_lock_timeouts", (char *)&srv_stat_lock_timeouts, SHOW_LONGLONG},
    {"tidesdb_lock_held", (char *)&srv_stat_lock_held, SHOW_LONGLONG},
    {"tidesdb_lock_entries", (char *)&srv_stat_lock_entries, SHOW_LONGLONG},
    {"tidesdb_lock_entry_recycles", (char *)&srv_stat_lock_entry_recycles, SHOW_LONGLONG},
    {"tidesdb_lock_chain_max", (char *)&srv_stat_lock_chain_max, SHOW_LONGLONG},
    {NullS, NullS, SHOW_ULONG}};

/* Refresh the static status variables from live tidesdb stats.  Cost is
   paid by the caller (SHOW ENGINE STATUS / SHOW GLOBAL STATUS), never on
   the write path. */
static void tidesdb_refresh_status_vars()
{
    if (!tdb_global) return;

    tidesdb_db_stats_t db_st;
    memset(&db_st, 0, sizeof(db_st));
    tidesdb_get_db_stats(tdb_global, &db_st);

    tidesdb_cache_stats_t cache_st;
    memset(&cache_st, 0, sizeof(cache_st));
    tidesdb_get_cache_stats(tdb_global, &cache_st);

    srv_stat_column_families = db_st.num_column_families;
    srv_stat_global_seq = (long long)db_st.global_seq;
    srv_stat_memtable_bytes = (long long)db_st.total_memtable_bytes;
    srv_stat_txn_memory_bytes = (long long)db_st.txn_memory_bytes;
    srv_stat_memory_limit = (long long)db_st.resolved_memory_limit;
    srv_stat_memory_pressure = db_st.memory_pressure_level;
    srv_stat_total_sstables = db_st.total_sstable_count;
    srv_stat_open_sstables = db_st.num_open_sstables;
    srv_stat_data_size_bytes = (long long)db_st.total_data_size_bytes;
    srv_stat_immutable_memtables = db_st.total_immutable_count;
    srv_stat_flush_pending = db_st.flush_pending_count;
    srv_stat_flush_queue = (long long)db_st.flush_queue_size;
    srv_stat_compaction_queue = (long long)db_st.compaction_queue_size;
    srv_stat_cache_entries = (long long)cache_st.total_entries;
    srv_stat_cache_bytes = (long long)cache_st.total_bytes;
    srv_stat_cache_hits = (long long)cache_st.hits;
    srv_stat_cache_misses = (long long)cache_st.misses;
    srv_stat_cache_hit_rate = cache_st.hit_rate * PERCENT_SCALE;
    srv_stat_cache_partitions = (long long)cache_st.num_partitions;

    /* Tombstone aggregates -- we walk every CF once, summing total_tombstones
       and tracking the worst single-SSTable density.
       tidesdb_db_stats_t does not surface tombstone counters, so the CF
       list is iterated here.  SHOW GLOBAL STATUS reads the resulting
       statics. */
    char **cf_names = NULL;
    int cf_count = 0;
    if (tidesdb_list_column_families(tdb_global, &cf_names, &cf_count) == TDB_SUCCESS && cf_names)
    {
        uint64_t total_tomb = 0, total_keys = 0;
        double max_density = 0.0;
        int max_density_level = 0;
        for (int i = 0; i < cf_count; i++)
        {
            if (!cf_names[i]) continue;
            tidesdb_column_family_t *cf = tidesdb_get_column_family(tdb_global, cf_names[i]);
            if (!cf) continue;
            tidesdb_stats_t *st = NULL;
            if (tidesdb_get_stats(cf, &st) == TDB_SUCCESS && st)
            {
                total_tomb += st->total_tombstones;
                total_keys += st->total_keys;
                if (st->max_sst_density > max_density)
                {
                    max_density = st->max_sst_density;
                    max_density_level = st->max_sst_density_level;
                }
                tidesdb_free_stats(st);
            }
        }
        for (int i = 0; i < cf_count; i++) tidesdb_free(cf_names[i]);
        tidesdb_free(cf_names);

        srv_stat_total_tombstones = (long long)total_tomb;
        srv_stat_tombstone_ratio = total_keys > 0 ? (double)total_tomb / (double)total_keys : 0.0;
        srv_stat_max_sst_density = max_density;
        srv_stat_max_sst_density_level = (long long)max_density_level;
    }
}

/* ******************** Plugin declaration ******************** */

static struct st_mysql_storage_engine tidesdb_storage_engine = {MYSQL_HANDLERTON_INTERFACE_VERSION};

maria_declare_plugin(tidesdb){MYSQL_STORAGE_ENGINE_PLUGIN,
                              &tidesdb_storage_engine,
                              "TidesDB",
                              "TidesDB",
                              "LSM-tree engine with ACID transactions, MVCC concurrency, "
                              "secondary/spatial/full-text/vector indexes, and encryption",
                              PLUGIN_LICENSE_GPL,
                              tidesdb_init_func,
                              tidesdb_deinit_func,
                              TIDESQL_VERSION_HEX,
                              tidesdb_status_variables,
                              tidesdb_system_variables,
                              TIDESQL_VERSION_STR,
                              MariaDB_PLUGIN_MATURITY_GAMMA} maria_declare_plugin_end;
