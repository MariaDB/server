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
#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* my_global.h MUST be included before handler.h / my_base.h: handler.h pulls
   in server headers that use typedefs (ulonglong, int64, sql_mode_t, ...)
   defined by my_global.h.  A wrong order breaks the build on MariaDB 11.4+
   with missing-declaration errors.  The IncludeCategories rule in .clang-format
   pins my_global.h to sort first so the formatter preserves this order. */
#include "my_global.h"

#include "handler.h"
#include "my_base.h"
#include "thr_lock.h"

extern "C"
{
#include <tidesdb/db.h>
}

/* Mirror constants for the library's TDB_DEFAULT_* values defined in
   <tidesdb/tidesdb.h>.  We don't include that header directly because it
   leaks a `realloc` macro that conflicts with MariaDB's String::realloc()
   method.  Keep these in sync with src/tidesdb.h on every library bump --
   sysvar defaults reference the TIDESQL_* names so drift is caught here
   rather than scattered across the sysvar declarations. */
static constexpr unsigned long long TIDESQL_DEFAULT_WRITE_BUFFER_SIZE = 64ULL * 1024 * 1024;
static constexpr unsigned long long TIDESQL_DEFAULT_SYNC_INTERVAL_US = 128000;
static constexpr unsigned long long TIDESQL_DEFAULT_KLOG_VALUE_THRESHOLD = 512;
static constexpr unsigned long long TIDESQL_DEFAULT_LEVEL_SIZE_RATIO = 10;
static constexpr unsigned long long TIDESQL_DEFAULT_MIN_LEVELS = 1;
static constexpr unsigned long long TIDESQL_DEFAULT_DIVIDING_LEVEL_OFFSET = 1;
static constexpr unsigned long long TIDESQL_DEFAULT_INDEX_SAMPLE_RATIO = 1;
static constexpr unsigned long long TIDESQL_DEFAULT_BLOCK_INDEX_PREFIX_LEN = 16;
static constexpr unsigned long long TIDESQL_DEFAULT_MIN_DISK_SPACE = 100ULL * 1024 * 1024;

/* Key namespace prefixes (first byte of every TidesDB key) */
static constexpr uint8_t KEY_NS_META = 0x00;
static constexpr uint8_t KEY_NS_DATA = 0x01;

/* Size of the namespace prefix that every TidesDB key starts with. */
static constexpr uint KEY_NAMESPACE_LEN = 1;

/* Buffer size for a data CF key, namespace byte + comparable PK + 1 byte slack.
   Used by every site that builds KEY_NS_DATA + pk via build_data_key. */
static constexpr uint DATA_KEY_BUF_LEN = KEY_NAMESPACE_LEN + MAX_KEY_LENGTH + 1;

/* Buffer size for a secondary-index CF entry key, comparable index-column
   bytes (up to MAX_KEY_LENGTH) + appended PK bytes (up to MAX_KEY_LENGTH)
   + 2 bytes of slack that covers VARBINARY length-byte overflow emitted
   by make_comparable_key. */
static constexpr uint SEC_IDX_KEY_BUF_LEN = (MAX_KEY_LENGTH * 2) + 2;

/* Number of doubles in a 2-D minimum bounding rectangle.  Always four
   (xmin, ymin, xmax, ymax); used for the on-disk spatial value layout
   and the in-memory query-MBR cache on the handler. */
static constexpr uint SPATIAL_MBR_DIMS = 4;

/* CF naming */
static constexpr const char CF_INDEX_INFIX[] = "__idx_";

/* Reserved CF for schema discovery (object store mode only) */
static constexpr const char SCHEMA_CF_NAME[] = "__tidesql_schema";

/* Hidden primary key size (tables without explicit PK) */
static constexpr size_t HIDDEN_PK_SIZE = sizeof(uint64_t);

/* Maximum number of secondary indexes we support */
static constexpr uint MAX_TIDESDB_KEYS = MAX_KEY;

/* Cost model constants for the optimizer */
static constexpr double TIDESDB_COST_SEQ_READ = 0.00005;
static constexpr double TIDESDB_COST_KEY_READ = 0.00003;
static constexpr double TIDESDB_COST_RANGE_SETUP = 0.0001;
static constexpr double TIDESDB_DEFAULT_READ_AMP = 1.0;

/* Stats cache refresh interval (microseconds) */
static constexpr long long TIDESDB_STATS_REFRESH_US = 2000000LL; /* 2 seconds */

/* Minimum stats.records to avoid optimizer edge cases with 0 rows */
static constexpr ha_rows TIDESDB_MIN_STATS_RECORDS = 2;

/* scan_time() -- split the opaque cost returned by tidesdb_range_cost
   between MariaDB's I/O and CPU cost buckets.  LSM scans are mostly
   block-read bound, so 90% I/O / 10% CPU matches observed profiles. */
static constexpr double TIDESDB_SCAN_IO_WEIGHT = 0.9;
static constexpr double TIDESDB_SCAN_CPU_WEIGHT = 0.1;

/* records_in_range() fallbacks when we can't get a useful estimate. */
static constexpr ha_rows TIDESDB_RIR_DEFAULT_EST = 10;         /* no share available */
static constexpr ha_rows TIDESDB_RIR_UNKNOWN_DENOM = 4;        /* total/4 + 1 quarter fallback */
static constexpr double TIDESDB_RIR_FRACTION_UNRELIABLE = 0.8; /* fall back to rec_per_key */

/* Range-width multiplier applied to rec_per_key when tidesdb_range_cost
   returned an unreliably high fraction (memtable-only data, narrow range
   indistinguishable from full scan).  Typical OLTP ranges span tens of
   key values; 20 keeps the estimate tight while still being vastly
   better than the full ratio. */
static constexpr ha_rows TIDESDB_RIR_RANGE_RPK_MULTIPLIER = 20;

/* Cap the rec_per_key range fallback at total / N so it never claims
   more than this fraction of the table. */
static constexpr ha_rows TIDESDB_RIR_RANGE_CAP_DENOM = 2;

/* Sentinel bytes for building full-range bounds that pass through
   tidesdb_range_cost or seek primitives.  KEY_INF_HI_BYTE fills upper
   bound buffers with 0xFF.  KEY_INF_LO_BYTE seeds the smallest possible
   first byte for secondary-index lower bounds (primary uses KEY_NS_DATA). */
static constexpr uint8_t KEY_INF_HI_BYTE = 0xFF;
static constexpr uint8_t KEY_INF_LO_BYTE = 0x00;

/* Row format constants.  Every row written by serialize_row carries the
   header [ROW_HEADER_MAGIC][null_bytes(2 LE)][field_count(2 LE)] for a
   total of ROW_HEADER_SIZE bytes; deserialize_row reads them back to
   support instant ADD/DROP COLUMN. */
static constexpr uchar ROW_HEADER_MAGIC = 0xFE;
static constexpr uint ROW_HEADER_SIZE = 5;

/* Length prefix Field::pack writes ahead of a wide VARCHAR payload.
   Two bytes covers VARCHAR above 255 chars; narrower columns use a
   single-byte prefix. */
static constexpr uint FIELD_VARCHAR_LEN_PREFIX = 2;

/* Sign-bit XOR mask used to translate a signed integer's MSB into
   sortable form (and back).  Big-endian sort keys flip this bit so
   negative values sort below positive ones lexicographically. */
static constexpr uint8_t INT_SORT_SIGN_FLIP_MASK = 0x80;

/* MariaDB packed-field widths used by sort-key decoders. */
static constexpr uint DATE_PACK_LEN = 3;
static constexpr uint DATETIME_MAX_PACK_LEN = 8;

/* Sysvar enum index for tidesdb_object_store_backend.  0 = LOCAL, 1 = S3. */
static constexpr uint OBJSTORE_BACKEND_LOCAL = 0;
static constexpr uint OBJSTORE_BACKEND_S3 = 1;

/* Separator that joins db and table names when forming a TidesDB CF name
   from a MariaDB path (e.g. "test/foo" -> "test__foo").  Centralized so
   path_to_cf_name, schema_cf, and discover stay in sync. */
static constexpr const char CF_DB_TABLE_SEP[] = "__";

/* Schema CF key encoding "db_name<SEP>table_name" with no trailing NUL.
   The null byte separator is unambiguous because MariaDB identifiers
   cannot contain NUL.  Used by schema_cf_key, schema_cf_key_from_path,
   the discover prefix builders, and the schema_cf_ensure_databases scan. */
static constexpr char SCHEMA_CF_KEY_SEP = '\0';

/* MariaDB temp-table marker character.  Internal temp/exchange tables
   carry one or more '#' in their on-disk name (e.g. "#sql-..."); we
   substitute '_' so the resulting CF name remains valid. */
static constexpr char MARIADB_TEMP_NAME_MARKER = '#';
static constexpr char MARIADB_TEMP_NAME_REPLACEMENT = '_';

/* Relative-path prefix that MariaDB prepends to table paths handed
   to handler callbacks ("./db/table").  schema_cf_key_from_path and
   path_to_cf_name strip it before extracting db/table components. */
static constexpr const char MARIADB_REL_PATH_PREFIX[] = "./";
static constexpr size_t MARIADB_REL_PATH_PREFIX_LEN = 2;

/* MariaDB sort-key null-indicator bytes prepended to nullable key parts
   in make_comparable_key.  Convention 0 sorts NULLs first under memcmp,
   1 marks a present value. */
static constexpr uchar SORT_KEY_NULL = 0;
static constexpr uchar SORT_KEY_NOT_NULL = 1;

/* Slot indices into the 4-double MBR layout used by spatial_qmbr_ and
   tdb_mbr_t-shaped buffers.  Order matches the on-disk SPATIAL_MBR_VALUE_LEN
   layout [xmin, ymin, xmax, ymax]. */
static constexpr uint MBR_XMIN_IDX = 0;
static constexpr uint MBR_YMIN_IDX = 1;
static constexpr uint MBR_XMAX_IDX = 2;
static constexpr uint MBR_YMAX_IDX = 3;

/* Inclusive bounds of the full 64-bit Hilbert value space.  Used when a
   spatial query has no decomposable cells (e.g. HA_READ_MBR_DISJOINT) and
   we have to scan the entire curve. */
static constexpr uint64_t HILBERT_RANGE_FULL_LO = 0;
static constexpr uint64_t HILBERT_RANGE_FULL_HI = UINT64_MAX;

/* Minimum number of point ranges in a multi-range request before our
   custom MRR path takes over from MariaDB's default implementation.
   Single-range plans bypass MRR so pessimistic row locking still
   engages on the index_read_map fast path. */
static constexpr uint MRR_ACCEPT_MIN_RANGES = 2;

/* Selectivity values used in info() / analyze() for index rec_per_key.
   UNIQUE exactly one row per distinct value.  FLOOR smallest plausible
   estimate so the optimizer never sees rec_per_key=0 (treated as "unknown"). */
static constexpr ulong REC_PER_KEY_UNIQUE = 1;
static constexpr ulong REC_PER_KEY_FLOOR = 1;

/* Divisor used to compute the centroid of an MBR ((min + max) / 2) when
   building a Hilbert spatial index key.  The centroid is the point that
   feeds hilbert_xy2d_64 -- the MBR corners themselves are stored in the
   value, not the key. */
static constexpr double MBR_CENTROID_DIV = 2.0;

/* Multiplier used to convert a 0..1 ratio (cache hit rate, etc.) into
   a percentage for human-readable status output. */
static constexpr double PERCENT_SCALE = 100.0;

/* First row id assigned to a freshly created (or fully truncated)
   hidden-PK table.  Row ids are one-based so that "0" remains a clean
   sentinel for "no row id yet" / "uninitialized". */
static constexpr uint64_t HIDDEN_PK_FIRST_ROW_ID = 1;

/* Inclusive bounds of a probability / cost fraction in [0, 1].  Used to
   clamp tidesdb_range_cost ratios in records_in_range so floating-point
   noise from the cost estimator can't push the fraction outside its
   semantic range. */
static constexpr double FRACTION_MIN = 0.0;
static constexpr double FRACTION_MAX = 1.0;

/* Read-amplification value reported when TidesDB has not yet collected
   enough statistics to compute a real read_amp.  1.0 means "one disk op
   per logical op" -- the optimistic baseline that won't penalize plans. */
static constexpr double READ_AMP_NONE = 1.0;

/* Per-document delta values for fts_update_meta when maintaining the
   FTS metadata row alongside DML.  ADD/DEL track whether a document
   was inserted or removed; word-count deltas use the matching sign. */
static constexpr int FTS_DOC_DELTA_ADD = 1;
static constexpr int FTS_DOC_DELTA_DEL = -1;

/* mkdir mode used when the discover_table callback creates a missing
   database directory under datadir. */
static constexpr int TIDESDB_DB_DIR_MODE = 0755;

/* Default ENCRYPTION_KEY_ID applied when a table is opened with
   encryption enabled but no explicit key id is provided.  Mirrors the
   default in the ENCRYPTION_KEY_ID HA_TOPTION_NUMBER declaration. */
static constexpr uint TIDESDB_DEFAULT_ENCRYPTION_KEY_ID = 1;

/* Sentinel value stored in TidesDB_share::ttl_field_idx when no TTL
   column is configured for the table.  Valid TTL field indexes are
   non-negative; >= 0 implies a TTL_COL column is present. */
static constexpr int TIDESDB_TTL_FIELD_NONE = -1;

/* Fallback divisor when rec_per_key is unset for a non-unique secondary
   index in info().  Estimate is total_records / N, biasing toward more
   selective lookups (10 ~= one decimal order of magnitude). */
static constexpr ha_rows STATS_REC_PER_KEY_FALLBACK_DIVISOR = 10;

/* IEEE-754 double-precision bit layout used by the spatial code's
   lexicographic-orderable encoding.  The sign bit is bit 63 of the 64-bit
   representation; LEX_UINT32_HI_SHIFT extracts the high 32 bits after
   sign-flipping for big-endian comparison. */
static constexpr uint64_t IEEE754_DOUBLE_SIGN_MASK = (uint64_t)1 << 63;
static constexpr uint LEX_UINT32_HI_SHIFT = 32;

/* Number of bits per byte for shift-based byte (de)serialization in the
   spatial encoder/decoder loops.  Equivalent to CHAR_BIT on POSIX. */
static constexpr uint BITS_PER_BYTE = 8;

/* yesno flag values used by the FTS boolean-mode parser to mark each
   query term as required (`+term`), excluded (`-term`), or neutral
   (just `term`).  Compared with `> 0` and `< 0` in the BM25 reducer. */
static constexpr int FTS_TERM_REQUIRED = 1;
static constexpr int FTS_TERM_EXCLUDED = -1;
static constexpr int FTS_TERM_NEUTRAL = 0;

/* Operator characters recognized by fts_parse_boolean for queries
   issued in `IN BOOLEAN MODE`.  These are part of the MariaDB FTS
   query DSL, not arbitrary punctuation. */
static constexpr char FTS_BOOL_OP_REQUIRED = '+';
static constexpr char FTS_BOOL_OP_EXCLUDED = '-';
static constexpr char FTS_BOOL_OP_PHRASE = '"';
static constexpr char FTS_BOOL_OP_TRUNC = '*';

/* BM25 (Okapi / Robertson Walker) ranking formula constants.
   Used in ft_init_ext to score postings.  IDF uses the Lucene
   smoothed form, log((N - df + EPS) / (df + EPS) + SHIFT).  TF
   normalization uses (tf * (k1 + BOOST)) / (tf + k1 * (BASE - b +
   b * dl / avgdl)). */
static constexpr double BM25_IDF_EPSILON = 0.5;
static constexpr double BM25_IDF_NONNEG_SHIFT = 1.0;
static constexpr double BM25_TF_SATURATION_BOOST = 1.0;
static constexpr double BM25_LENGTH_NORM_BASE = 1.0;
/* Fallback average document length when the FTS metadata reports
   zero total documents.  A value of 1.0 collapses the length
   normalization term to neutral so scoring still proceeds. */
static constexpr double BM25_DEFAULT_AVGDL = 1.0;
/* Floor for total_docs in the IDF denominator.  Guards std::log
   from a divide-by-zero when no documents have been indexed yet. */
static constexpr int64_t BM25_MIN_TOTAL_DOCS = 1;

/* Inplace index builds rows between mid-txn commits and between
   thd_killed polls. */
static constexpr ha_rows TIDESDB_INDEX_BUILD_BATCH = 100;

/* Bulk DML ops between mid-txn commits during start_bulk_insert /
   start_bulk_update / start_bulk_delete.  Counts both the primary put
   and each secondary-index put. */
static constexpr ha_rows TIDESDB_BULK_INSERT_BATCH_OPS = 500;

/* Encryption */
static constexpr uint TIDESDB_ENC_IV_LEN = 16;
static constexpr uint TIDESDB_ENC_KEY_LEN = 32;

/* Bytes of key-version prefix on every encrypted row blob.  The on-disk
   layout is the 4-byte little-endian key version, then the IV, then the
   ciphertext, so a row always decrypts under the exact key version it was
   written with and survives an encryption key rotation. */
static constexpr uint TIDESDB_ENC_VERSION_LEN = 4;

/* Bloom filter FPR conversion (table option stores parts per 10000) */
static constexpr double TIDESDB_BLOOM_FPR_DIVISOR = 10000.0;

/* Tombstone density trigger conversion (table option stores parts per
   10000; library config is a 0.0..1.0 ratio). */
static constexpr double TIDESDB_TOMBSTONE_DENSITY_DIVISOR = 10000.0;

/* Skip list probability conversion (table option stores percentage) */
static constexpr float TIDESDB_SKIP_LIST_PROB_DIV = 100.0f;

/* TTL sentinel value meaning no expiration */
static constexpr time_t TIDESDB_TTL_NONE = (time_t)-1;

/* Default block cache size (bytes) */
static constexpr ulonglong TIDESDB_DEFAULT_BLOCK_CACHE = 256ULL * 1024 * 1024; /* 256M */

/*
  TidesDB_share -- shared state for one table, visible to all handler objects.
*/
class TidesDB_share : public Handler_share
{
   public:
    /* Main data CF */
    tidesdb_column_family_t *cf;
    std::string cf_name;

    /* Primary key info */
    bool has_user_pk;
    uint pk_index; /* MariaDB key number of the PK (usually 0)   */
    uint pk_key_len;

    /* Hidden PK row-id generator (used when has_user_pk == false) */
    std::atomic<uint64_t> next_row_id;

    /* In-memory AUTO_INCREMENT counter (avoids index_last() per INSERT).
       Seeded once from index_last() at open time; incremented atomically. */
    std::atomic<ulonglong> auto_inc_val{0};

    /* Per-table isolation level (from CREATE TABLE options) */
    tidesdb_isolation_level_t isolation_level;

    /* TTL support */
    ulonglong default_ttl; /* table-level default TTL in seconds (0 = none) */
    int ttl_field_idx;     /* field index of TTL_COL column (-1 = none)     */

    /* Data-at-rest encryption */
    bool encrypted;
    uint encryption_key_id;      /* ENCRYPTION_KEY_ID table option (default 1) */
    uint encryption_key_version; /* cached latest key version */

    /* Cached table shape flags (set once at open time) */
    bool has_blobs;
    bool has_ttl;
    uint num_secondary_indexes; /* count of non-NULL secondary index CFs */
    size_t cached_row_est{0};   /* cached serialize_row size estimate for non-BLOB tables */

    /* Field indices of BLOB/TEXT columns -- populated at open() when
       has_blobs is true.  serialize_row iterates only these instead of
       scanning all fields for the BLOB_FLAG. */
    std::vector<uint16> blob_field_indices;

    /* Per-field plan for the serialize/deserialize hot path.
       Built once at open() so the row loops avoid per-row recomputation
       of `f->ptr - table->record[0]` and skip the Field::pack/unpack
       vtable dispatch for fields whose pack() is the default memcpy.

       memcpy_ok is true when the field's pack format is exactly
       `pack_length()` bytes of memcpy (the Field::pack default, used by
       all integer, FLOAT/DOUBLE, fixed DATETIME/DATE/TIME/TIMESTAMP,
       YEAR, ENUM, SET, BIT and NEWDECIMAL types).  CHAR/VARCHAR/BLOB/
       VARBINARY/GEOMETRY/JSON keep the slow path because their pack()
       trims trailing spaces or emits a length prefix.

       maybe_null is cached so the loop branches off a single bool
       instead of calling Field::real_maybe_null() per row.

       src_off is the field's offset within table->record[0] -- the loops
       still rebase by ptrdiff at runtime so the same plan serves reads
       and writes that target record[1] too. */
    struct field_plan_t
    {
        uint32 src_off;  /* offset within table->record[0]                */
        uint16 pack_len; /* f->pack_length(), used when memcpy_ok         */
        bool memcpy_ok;  /* true -> inline memcpy; false -> Field::pack   */
        bool maybe_null; /* cached f->maybe_null() (NOT real_maybe_null)  */
    };
    std::vector<field_plan_t> field_plan;
    bool has_no_nullable{false};
    uint8 null_bytes_cached{0}; /* cached table->s->null_bytes            */
    uint16 fields_cached{0};    /* cached table->s->fields                */

    /* Cached scan_time range cost (refreshed every TIDESDB_STATS_REFRESH_US) */
    std::atomic<double> cached_scan_cost{0.0};
    std::atomic<long long> scan_cost_time{0};

    /* records_in_range needs a full-range cost as the normalizer; without
       a cache it recomputes that for every probe of every alternative
       plan.  Stored per CF -- one atomic for the data CF, one array per
       secondary index -- refreshed with the same TIDESDB_STATS_REFRESH_US
       window.  std::atomic<double> is not move-constructible so the
       per-index storage uses a fixed unique_ptr<atomic[]> sized in
       open().  A stale read just produces a slightly stale estimate. */
    std::atomic<double> cached_pk_full_cost{0.0};
    std::atomic<long long> cached_pk_full_cost_time{0};
    std::unique_ptr<std::atomic<double>[]> cached_idx_full_cost;
    std::unique_ptr<std::atomic<long long>[]> cached_idx_full_cost_time;
    uint cached_idx_full_cost_n{0};

    /* Table timestamps for information_schema.TABLES */
    time_t create_time{0};              /* from .frm stat at first open */
    std::atomic<time_t> update_time{0}; /* bumped on DML (write/update/delete) */

    /* Cached stats -- avoid expensive tidesdb_get_stats per statement.
       Refreshed at most every 2 seconds; read with relaxed atomics. */
    std::atomic<ha_rows> cached_records{0};
    std::atomic<uint64_t> cached_data_size{0};     /* total_data_size from CF */
    std::atomic<uint64_t> cached_idx_data_size{0}; /* sum of secondary CF sizes */
    std::atomic<uint32_t> cached_mean_rec_len{0};  /* avg_key_size + avg_value_size */
    std::atomic<long long> stats_refresh_us{0};
    std::atomic<double> cached_read_amp{1.0}; /* read amplification factor */

    /* Precomputed comparable key length per index (avoids per-row recomputation) */
    uint idx_comp_key_len[MAX_KEY];

    /* Precomputed index-type flags (avoid ki->algorithm dereference per row
       in DML secondary-index loops).  Populated at open() and refreshed
       during online DDL. */
    bool idx_is_fts[MAX_KEY];
    bool idx_is_spatial[MAX_KEY];

    /* Cached rec_per_key for secondary indexes (populated by ANALYZE TABLE).
       0 = not yet computed, use heuristic; >0 = sampled value. */
    std::atomic<ulong> cached_rec_per_key[MAX_KEY];

    /* Secondary index CFs (one per secondary key) */
    std::vector<tidesdb_column_family_t *> idx_cfs;
    std::vector<std::string> idx_cf_names;

    /* Per-index covered-field map used by try_keyread_from_index.  For each
       index i, idx_cover[i][field_c] == true when field `c` can be
       reconstructed from the index key bytes (i.e. field is in the index's
       key parts or -- for secondary indexes -- in the PK parts appended
       to the key).  Replaces the O(read_set_bits * (pk_parts + idx_parts))
       nested scan the old code did on every covered read. */
    std::vector<std::vector<bool>> idx_cover;

    TidesDB_share();
    ~TidesDB_share();
};

/*
  Context passed between Online DDL phases (prepare -> inplace -> commit).
  Holds the new/dropped CF pointers so commit can finalize atomically.
*/
class ha_tidesdb_inplace_ctx : public inplace_alter_handler_ctx
{
   public:
    /* CFs created for newly added indexes (populated during inplace phase) */
    std::vector<tidesdb_column_family_t *> add_cfs;
    std::vector<std::string> add_cf_names;
    std::vector<uint> add_key_nums; /* position in new key_info */

    /* CF names to drop for removed indexes (dropped during commit phase) */
    std::vector<std::string> drop_cf_names;

    virtual ~ha_tidesdb_inplace_ctx()
    {
    }
};

/* Pessimistic lock mode.  Shared is read-intent and compatible with itself,
   exclusive is write-intent and conflicts with everything.  Declared here
   because tidesdb_trx_t carries a waiting_on_mode field. */
enum tdb_lock_mode_t
{
    TDB_LOCK_MODE_S = 0,
    TDB_LOCK_MODE_X = 1,
};

/* Per-txn accumulator entry for one FTS index's metadata key.  The
   plugin folds the per-row delta_docs and delta_words contributions
   from every write_row / update_row / delete_row in a transaction here
   and writes one combined update at commit time, so the FTS meta key
   does not become a write-write serialisation point under concurrent
   writers and a long statement does not produce N read-modify-writes
   on the same key. */
struct fts_meta_delta_t
{
    tidesdb_column_family_t *data_cf;
    uint keynr;
    int64_t doc_delta;
    int64_t word_delta;
};

/*
  Per-connection TidesDB transaction context.
  Stored via thd_set_ha_data(); shared by all handler objects on the
  same connection.  The TidesDB txn spans the entire BEGIN...COMMIT
  block (or a single auto-commit statement).
*/
struct tidesdb_trx_t
{
    tidesdb_txn_t *txn{nullptr};
    bool dirty{false};                 /* true once any DML uses txn */
    bool stmt_savepoint_active{false}; /* true while a "stmt" savepoint exists */
    bool needs_reset{false};           /* true after commit/rollback; cleared after txn_reset */
    tidesdb_isolation_level_t isolation_level{TDB_ISOLATION_REPEATABLE_READ};
    uint64_t txn_generation{0};

    /* Plugin-level row lock state for this txn.  The lock manager supports
       shared (read-intent) and exclusive (write-intent) modes; multiple S
       holders coexist on the same lock, X blocks any other holder, and a
       new S blocks while an X is queued so writers are never starved by a
       stream of readers.  Locks are acquired from write_row, fetch_row_by_pk,
       and iter_read_current depending on session isolation and write intent,
       and released en masse at commit or rollback. */

    struct tdb_lock_request_t *held_locks_head{nullptr};

    /* What this txn is currently waiting for, published as two fields the
       deadlock walker can read lock-free from other partitions without ever
       dereferencing a request struct.  Lock entries themselves are never
       freed at runtime (find_or_create recycles empty slots in place), so
       waiting_on_lock is always a safe pointer to follow.  The writing
       thread stores waiting_on_mode before waiting_on_lock with release
       ordering, and walkers load waiting_on_lock with acquire then read the
       mode, so a walker that sees a non-null lock pointer also sees the
       matching mode. */
    std::atomic<struct tdb_row_lock_t *> waiting_on_lock{nullptr};
    tdb_lock_mode_t waiting_on_mode{TDB_LOCK_MODE_S};

    /* Per-statement FTS meta deltas, applied before tidesdb_commit hands
       the txn to the library so the meta update lands in the same commit
       as the row writes that produced it. */
    std::vector<fts_meta_delta_t> fts_meta_pending;
    bool fts_meta_dirty{false};
};

/*
  ha_tidesdb -- per-connection handler object.
*/
class ha_tidesdb : public handler
{
    TidesDB_share *share;

    /* Points into the per-connection tidesdb_trx_t::txn.
       Set in external_lock(), cleared in external_lock(F_UNLCK). */
    tidesdb_txn_t *stmt_txn;
    bool stmt_txn_dirty; /* true once any DML uses stmt_txn */

    /* Scan / index-scan state (iterator lives on stmt_txn when available) */
    tidesdb_txn_t *scan_txn;
    tidesdb_iter_t *scan_iter;
    tidesdb_column_family_t *scan_cf_;      /* CF for lazy iterator creation */
    tidesdb_column_family_t *scan_iter_cf_; /* CF the cached scan_iter was created for */
    tidesdb_txn_t *scan_iter_txn_;          /* txn the cached scan_iter was created on */
    uint64_t scan_iter_txn_gen_;            /* txn_generation when scan_iter was created */
    bool idx_pk_exact_done_;                /* deferred seek after PK exact */
    enum scan_dir_t
    {
        DIR_NONE,
        DIR_FORWARD,
        DIR_BACKWARD
    } scan_dir_;
    std::string last_row;  /* keeps BLOB data alive for record[0] */
    std::string last_row2; /* keeps BLOB data alive for record[1] */

    /* Spatial index scan state */
    bool spatial_scan_active_{false};
    enum ha_rkey_function spatial_mode_
    {
        HA_READ_KEY_EXACT
    };
    double spatial_qmbr_[SPATIAL_MBR_DIMS]{}; /* query MBR (xmin, ymin, xmax, ymax) */

    /* Hilbert range decomposition are sorted non-overlapping [lo, hi] ranges
       covering the query box.  spatial_range_idx_ tracks which range we're
       currently scanning. */
    std::vector<std::pair<uint64_t, uint64_t>> spatial_ranges_; /* {lo, hi} */
    size_t spatial_range_idx_{0};

    /* Spatial scan continuation -- scans Hilbert range with MBR post-filter */
    int spatial_scan_next(uchar *buf);

    /* Current row's PK key bytes (without namespace prefix).
       Fixed buffer eliminates std::string heap allocation per row. */
    uchar current_pk_buf_[MAX_KEY_LENGTH];
    uint current_pk_len_;

    /* Reusable buffer for serialize_row (retains heap capacity) */
    std::string row_buf_;

    /* Cached comparable search key from index_read_map for index_next_same */
    uchar idx_search_comp_[MAX_KEY_LENGTH];
    uint idx_search_comp_len_;

    /* True when index_read_map landed on a partial-PK exact prefix scan and
       defers iteration to index_next.  index_next's PK branch must then
       re-validate the prefix after each step, the same way the secondary
       branch and index_next_same already do, or it would walk off the
       prefix and return unrelated rows. */
    bool pk_partial_exact_active_{false};

    /* Reusable buffers for secondary index key construction in update_row.
       Avoids heap allocation per row and keeps the stack frame small. */
    uchar upd_old_ik_[SEC_IDX_KEY_BUF_LEN];
    uchar upd_new_ik_[SEC_IDX_KEY_BUF_LEN];

    /* Cached dup-check iterators for UNIQUE secondary indexes.
       tidesdb_iter_new() is O(num_sstables) -- caching avoids rebuilding
       the merge heap on every INSERT for tables with unique indexes. */
    tidesdb_iter_t *dup_iter_cache_[MAX_KEY];
    tidesdb_txn_t *dup_iter_txn_[MAX_KEY]; /* txn each was created on */
    uint64_t dup_iter_txn_gen_[MAX_KEY];   /* txn_generation when created */
    uint dup_iter_count_;                  /* number of slots populated */

    /* Reusable buffer for tidesdb_txn_get values -- avoids malloc/free per
       point-lookup.  Retains heap capacity across calls. */
    std::string get_val_buf_;

    /* Separate encryption output buffer so row_buf_ retains its heap
       capacity across rows.  serialize_row writes plaintext into row_buf_
       and the encrypted blob into enc_buf_. */
    std::string enc_buf_;

    /* Per-statement cached encryption key version -- avoids calling
       encryption_key_get_latest_version() on every single row write. */
    uint cached_enc_key_ver_;
    bool enc_key_ver_valid_;

    /* Per-statement cached time(NULL) -- avoids the vDSO/syscall on every
       row for TTL computation.  1-second granularity is sufficient for TTL. */
    time_t cached_time_;
    bool cached_time_valid_;

    /* Per-statement cached THDVAR lookups -- avoids the indirect
       thd_get_ha_data + offset computation on every row. */
    ulonglong cached_sess_ttl_;
    bool cached_skip_unique_;
    bool cached_single_delete_primary_;
    bool cached_thdvars_valid_;

    /* Write-lock mode -- set when store_lock detects FOR UPDATE / write intent.
       Used to decide whether to acquire row locks in index_read_map. */
    bool stmt_has_write_lock_;

    /* True for UPDATE / DELETE statements -- set in external_lock(F_WRLCK)
       from cached_sql_cmd_.  iter_read_current uses this to skip the
       per-row X lock during ICP filtering; update_row / delete_row
       reacquire on the row they actually mutate.  SELECT ... FOR UPDATE
       leaves this false so the locking-cursor contract is preserved. */
    bool stmt_is_update_or_delete_{false};

    /* Cached "is this scan on the primary key" flag.  Set once in index_init
       so the navigation methods (index_next/prev/first/last/next_same) skip
       the per-row `share->has_user_pk && active_index == share->pk_index`
       recomputation. */
    bool is_pk_;

    /* Cached last tidesdb_iter_new failure for the current scan CF/txn.
       When non-zero and the scan_cf_/scan_txn haven't changed, ensure_scan_iter
       returns the prior error immediately instead of retrying + re-logging. */
    int scan_iter_last_err_;
    tidesdb_column_family_t *scan_iter_last_err_cf_;
    tidesdb_txn_t *scan_iter_last_err_txn_;

    /* Handler mirrors of share->has_blobs / share->encrypted.  Per-row
       fetches and scans branch on these; reading them from a handler member
       avoids the shared-memory dereference that dominates when the L1 line
       for `share` isn't already hot. */
    bool has_blobs_;
    bool encrypted_;

    /* Cached bounds of table->record[1] so the BLOB path of fetch_row_by_pk
       and iter_read_current can classify `buf` against record[0] vs record[1]
       without dereferencing `table->record[1]` and `table->s->reclength` on
       every row. */
    const uchar *record1_lo_;
    const uchar *record1_hi_;

    /* Cached per-statement THD query shape so ensure_stmt_txn() and
       external_lock() don't each re-evaluate thd_sql_command() and
       thd_test_options().  Populated by external_lock(F_WRLCK/F_RDLCK);
       invalidated by external_lock(F_UNLCK) along with the other per-stmt
       caches. */
    int cached_sql_cmd_;
    bool cached_is_autocommit_;
    bool cached_stmt_shape_valid_;

    /* Cached per-statement pointers to avoid repeated hash lookups.
       Set in external_lock(lock), cleared in external_lock(F_UNLCK).
       InnoDB caches these as m_user_thd / m_prebuilt->trx. */
    THD *cached_thd_;           /* avoids ha_thd() virtual dispatch */
    tidesdb_trx_t *cached_trx_; /* avoids thd_get_ha_data() hash lookup */

    /* Bulk DML state.  The ops counter is shared across insert/update/delete
       bulk modes since only one can be active at a time and they all use the
       same TIDESDB_BULK_INSERT_BATCH_OPS threshold. */
    bool in_bulk_insert_;
    bool in_bulk_update_;
    bool in_bulk_delete_;
    ha_rows bulk_insert_ops_; /* ops buffered since last mid-txn commit */

    /* Auto-compact-after-range-delete tracking.  When the session var
       tidesdb_compact_after_range_delete_min_rows is non-zero, delete_row
       updates these fields with the comparable PK bytes of each deleted
       row, and end_bulk_delete fires tidesdb_compact_range over the
       observed [min_pk, max_pk] range if the deleted-row count meets the
       threshold.  Cleared on start_bulk_delete and on each
       cached-THDVAR refresh. */
    ulonglong cached_compact_after_range_delete_min_rows_;
    ha_rows bulk_delete_rows_;
    std::string bulk_delete_min_pk_;
    std::string bulk_delete_max_pk_;

    /* Multi-Range Read state.  We accept MRR when every range the optimizer
       hands us is UNIQUE_RANGE|EQ_RANGE (i.e. the WHERE col IN (...) case on
       a full key) and fall back to the default MRR->read_range_first path for
       everything else.  Accepted ranges are buffered + sorted by comparable
       key bytes so the LSM sees a monotone stream of seeks. */
    struct tdb_mrr_entry
    {
        std::string comp_key; /* comparable PK / index bytes */
        range_id_t ptr;       /* value returned to caller as *range_info */
    };
    bool mrr_custom_active_;
    bool mrr_no_assoc_;
    uint mrr_keyno_;
    std::vector<tdb_mrr_entry> mrr_entries_;
    size_t mrr_next_idx_;

    /* Covering-index mode (HA_EXTRA_KEYREAD) */
    bool keyread_only_;
    bool write_can_replace_; /* true during REPLACE INTO (HA_EXTRA_WRITE_CAN_REPLACE) */

    /* private helpers
     */
    int ensure_stmt_txn(); /* lazy txn creation on first data access */
    TidesDB_share *get_share();
    const std::string &serialize_row(const uchar *buf);
    void deserialize_row(uchar *buf, const uchar *data, size_t len);
    void deserialize_row(uchar *buf, const std::string &row);

    /* Build memcmp-comparable key bytes into out[]; returns byte count */
    uint make_comparable_key(KEY *key_info, const uchar *record, uint num_parts, uchar *out);

    /* Convert key_copy-format search key directly to comparable bytes */
    uint key_copy_to_comparable(KEY *key_info, const uchar *key_buf, uint key_len, uchar *out);

    /* Build PK bytes from a record buffer into out[]; returns byte count */
    uint pk_from_record(const uchar *record, uchar *out);

    /* Build KEY_NS_DATA + pk into out[]; returns byte count */
    static uint build_data_key(const uchar *pk, uint pk_len, uchar *out)
    {
        out[0] = KEY_NS_DATA;
        memcpy(out + KEY_NAMESPACE_LEN, pk, pk_len);
        return pk_len + KEY_NAMESPACE_LEN;
    }

    /* Build a secondary-index entry key into out[]; returns byte count */
    uint sec_idx_key(uint idx, const uchar *record, uchar *out);

    /* Fetch a row by its PK bytes into buf; sets current_pk + last_row */
    int fetch_row_by_pk(tidesdb_txn_t *txn, const uchar *pk, uint pk_len, uchar *buf);

    /* Compute the absolute TTL timestamp for a row being written.
       Reads per-row TTL_COL value if present, else uses table default.
       Returns -1 (no expiration) or a future Unix timestamp. */
    time_t compute_row_ttl(const uchar *buf);

    /* Read current iterator entry (data-CF), decode row into buf.
       Returns 0 or HA_ERR_END_OF_FILE / HA_ERR_KEY_NOT_FOUND. */
    int iter_read_current(uchar *buf);

    /* Lazily create scan_iter from scan_cf_ when first needed */
    int ensure_scan_iter();

    /* Try to decode record from secondary index key (keyread-only) */
    bool try_keyread_from_index(const uint8_t *ik, size_t iks, uint idx, uchar *buf);

    /* Evaluate pushed index condition on a secondary-index entry before
       the expensive PK point-lookup.  Decodes the index key columns into
       buf and calls handler_index_cond_check().
       Returns CHECK_POS                -- condition satisfied, proceed with PK lookup
               CHECK_NEG                -- condition not satisfied, skip this entry
               CHECK_OUT_OF_RANGE       -- past end of scan range
               CHECK_ABORTED_BY_USER    -- query killed */
    check_result_t icp_check_secondary(const uint8_t *ik, size_t iks, uint idx, uchar *buf);

    /* Reverse a single integer sort-key part back to native little-endian
       at `to` (destination byte pointer computed once by the caller).
       Returns true on success, false for unsupported sort_len. */
    static bool decode_int_sort_key(const uint8_t *src, uint sort_len, bool is_signed, uchar *to);

    /* Extended sort-key decoder -- handles integers, DATE, DATETIME,
       TIMESTAMP, YEAR, and fixed-length CHAR/BINARY.  Returns true on
       success, false for unsupported types.  Used by covering index
       reads and ICP evaluation to avoid PK point-lookups. */
    static bool decode_sort_key_part(const uint8_t *src, uint sort_len, Field *f, uchar *buf);

    /* Free all cached dup-check iterators */
    void free_dup_iter_cache();

    /* Commit the current txn mid-statement when a bulk op crosses the batch
       threshold, then reset it to READ_COMMITTED for the next batch.  Shared
       between bulk INSERT/UPDATE/DELETE.  Invalidates cached iterators.
       Returns 0 on success, handler error code on fatal failure. */
    int maybe_bulk_commit(tidesdb_trx_t *trx);

    /* Recover hidden-PK counter by scanning the CF */
    void recover_counters();

   public:
    ha_tidesdb(handlerton *hton, TABLE_SHARE *table_arg);
    ~ha_tidesdb() = default;

    ulonglong table_flags() const override
    {
        return HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE | HA_NULL_IN_KEY |
               HA_PRIMARY_KEY_IN_READ_INDEX | HA_TABLE_SCAN_ON_INDEX | HA_CAN_VIRTUAL_COLUMNS |
               HA_FAST_KEY_READ | HA_REC_NOT_IN_SEQ | HA_CAN_SQL_HANDLER |
               HA_REQUIRES_KEY_COLUMNS_FOR_DELETE | HA_PRIMARY_KEY_REQUIRED_FOR_POSITION |
               HA_ONLINE_ANALYZE | HA_CAN_ONLINE_BACKUPS | HA_CONCURRENT_OPTIMIZE |
               HA_CAN_TABLES_WITHOUT_ROLLBACK | HA_CAN_FULLTEXT | HA_CAN_FULLTEXT_EXT |
               HA_CAN_GEOMETRY | HA_CAN_RTREEKEYS | HA_CAN_EXPORT;
    }

    ulong index_flags(uint idx, uint part, bool all_parts) const override;

    const char *index_type(uint key_number) override;

    uint max_supported_record_length() const override
    {
        return HA_MAX_REC_LENGTH;
    }
    uint max_supported_keys() const override
    {
        return MAX_TIDESDB_KEYS;
    }
    uint max_supported_key_parts() const override
    {
        return MAX_REF_PARTS;
    }
    uint max_supported_key_length() const override
    {
        return MAX_KEY_LENGTH;
    }

    IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows, ulonglong blocks) override
    {
        /* Index read -- each point lookup touches read_amp levels.
           Range scans amortize the merge-heap cost across rows. */
        IO_AND_CPU_COST cost;
        cost.io = 0;
        double amp = share ? share->cached_read_amp.load(std::memory_order_relaxed)
                           : TIDESDB_DEFAULT_READ_AMP;
        cost.cpu =
            (double)rows * TIDESDB_COST_KEY_READ * amp + (double)ranges * TIDESDB_COST_RANGE_SETUP;
        return cost;
    }

    IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override
    {
        /* Random position lookup -- each is a point-get through LSM levels.
           More expensive than sequential due to read amplification. */
        IO_AND_CPU_COST cost;
        cost.io = 0;
        double amp = share ? share->cached_read_amp.load(std::memory_order_relaxed)
                           : TIDESDB_DEFAULT_READ_AMP;
        cost.cpu = (double)rows * TIDESDB_COST_SEQ_READ * amp;
        return cost;
    }

    /* Convert a MariaDB table path to a TidesDB column family name */
    static std::string path_to_cf_name(const char *path);

    /* DDL */
    int open(const char *name, int mode, uint test_if_locked) override;
    int close(void) override;
    int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
    int delete_table(const char *name) override;
    int rename_table(const char *from, const char *to) override;

    /* Full table scan */
    int rnd_init(bool scan) override;
    int rnd_end() override;
    int rnd_next(uchar *buf) override;
    int rnd_pos(uchar *buf, uchar *pos) override;
    void position(const uchar *record) override;

    /* Index scan */
    int index_init(uint idx, bool sorted) override;
    int index_end() override;
    int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                       enum ha_rkey_function find_flag) override;
    int index_next(uchar *buf) override;
    int index_prev(uchar *buf) override;
    int index_first(uchar *buf) override;
    int index_last(uchar *buf) override;
    int index_next_same(uchar *buf, const uchar *key, uint keylen) override;

    /* DML */
    int write_row(const uchar *buf) override;
    int update_row(const uchar *old_data, const uchar *new_data) override;
    int delete_row(const uchar *buf) override;
    int delete_all_rows(void) override;

    /* Full-text search */
    int ft_init() override;
    void ft_end() override;
    FT_INFO *ft_init_ext(uint flags, uint inx, String *key) override;
    int ft_read(uchar *buf) override;

    /* Bulk insert hint (LOAD DATA, multi-row INSERT) */
    void start_bulk_insert(ha_rows rows, uint flags) override;
    int end_bulk_insert() override;

    /* Bulk UPDATE / DELETE hints -- let multi-row UPDATE/DELETE share the
       same mid-txn commit batching as bulk INSERT so long statements don't
       blow past TDB_MAX_TXN_OPS or balloon txn memory. */
    bool start_bulk_update() override;
    int end_bulk_update() override;
    int bulk_update_row(const uchar *old_data, const uchar *new_data,
                        ha_rows *dup_key_found) override;
    bool start_bulk_delete() override;
    int end_bulk_delete() override;

    /* Index Condition Pushdown (ICP) */
    Item *idx_cond_push(uint keyno, Item *idx_cond) override;

    /* Multi-Range Read (MRR).  We opt into a custom implementation for
       point-only range sequences and defer to the base handler for
       everything else by leaving HA_MRR_USE_DEFAULT_IMPL set. */
    ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq, void *seq_init_param,
                                        uint n_ranges, uint *bufsz, uint *mrr_mode, ha_rows limit,
                                        Cost_estimate *cost) override;
    int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param, uint n_ranges, uint mrr_mode,
                              HANDLER_BUFFER *buf) override;
    int multi_range_read_next(range_id_t *range_info) override;

    /* AUTO_INCREMENT -- O(1) atomic counter */
    void get_auto_increment(ulonglong offset, ulonglong increment, ulonglong nb_desired_values,
                            ulonglong *first_value, ulonglong *nb_reserved_values) override;

    /* Reset the in-memory auto-increment counter so `TRUNCATE TABLE t` and
       `ALTER TABLE t AUTO_INCREMENT=N` take effect.  Base default is a no-op,
       which left TidesDB's cached counter running past TRUNCATE -- the next
       INSERT would return a stale value instead of restarting at 1 (or N). */
    int reset_auto_increment(ulonglong value) override;

    /* Stats / Maintenance */
    int info(uint flag) override;
    int analyze(THD *thd, HA_CHECK_OPT *check_opt) override;
    int optimize(THD *thd, HA_CHECK_OPT *check_opt) override;
    int check(THD *thd, HA_CHECK_OPT *check_opt) override;
    int repair(THD *thd, HA_CHECK_OPT *check_opt) override;
    ha_rows records_in_range(uint inx, const key_range *min_key, const key_range *max_key,
                             page_range *pages) override;
    int extra(enum ha_extra_function operation) override;

   private:
   public:
   protected:
    IO_AND_CPU_COST scan_time() override;

   public:
    /* Locking -- TidesDB handles concurrency via MVCC internally.
       lock_count()=0 bypasses MariaDB's THR_LOCK. */
    uint lock_count(void) const override
    {
        return 0;
    }
    int external_lock(THD *thd, int lock_type) override;
    THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to, enum thr_lock_type lock_type) override;

    /* Online DDL -- instant metadata, inplace indexes, copy for columns */
    enum_alter_inplace_result check_if_supported_inplace_alter(
        TABLE *altered_table, Alter_inplace_info *ha_alter_info) override;
    bool prepare_inplace_alter_table(TABLE *altered_table,
                                     Alter_inplace_info *ha_alter_info) override;
    bool inplace_alter_table(TABLE *altered_table, Alter_inplace_info *ha_alter_info) override;
    bool commit_inplace_alter_table(TABLE *altered_table, Alter_inplace_info *ha_alter_info,
                                    bool commit) override;
    bool check_if_incompatible_data(HA_CREATE_INFO *create_info, uint table_changes) override;
};
