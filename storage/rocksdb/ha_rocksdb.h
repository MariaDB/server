/*
   Copyright (c) 2012,2013 Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA */
#pragma once

#ifdef USE_PRAGMA_INTERFACE
#pragma interface /* gcc class implementation */
#endif

/* C++ standard header files */
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

/* MySQL header files */
#include "./handler.h"   /* handler */
#include "./my_global.h" /* ulonglong */
#include "./sql_string.h"
#include "./ut0counter.h"

/* RocksDB header files */
#include "rocksdb/cache.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/statistics.h"
#include "rocksdb/utilities/options_util.h"
#include "rocksdb/utilities/transaction_db.h"
#include "rocksdb/utilities/write_batch_with_index.h"

/* MyRocks header files */
#include "./rdb_comparator.h"
#include "./rdb_index_merge.h"
#include "./rdb_perf_context.h"
#include "./rdb_sst_info.h"
#include "./rdb_utils.h"

/**
  @note MyRocks Coding Conventions:
  MyRocks code follows the baseline MySQL coding conventions, available at
  http://dev.mysql.com/doc/internals/en/coding-guidelines.html, with several
  refinements (@see /storage/rocksdb/README file).
*/

/**
  @note MyRocks Coding Conventions:
  MyRocks code follows the baseline MySQL coding conventions, available at
  http://dev.mysql.com/doc/internals/en/coding-guidelines.html, with several
  refinements (@see /storage/rocksdb/README file).
*/

namespace myrocks {

/*
 * class for exporting transaction information for
 * information_schema.rocksdb_trx
 */
struct Rdb_trx_info {
  std::string name;
  ulonglong trx_id;
  ulonglong write_count;
  ulonglong lock_count;
  int timeout_sec;
  std::string state;
  std::string waiting_key;
  ulonglong waiting_cf_id;
  int is_replication;
  int skip_trx_api;
  int read_only;
  int deadlock_detect;
  int num_ongoing_bulk_load;
  ulong thread_id;
  std::string query_str;
};

std::vector<Rdb_trx_info> rdb_get_all_trx_info();

/*
  This is
  - the name of the default Column Family (the CF which stores indexes which
    didn't explicitly specify which CF they are in)
  - the name used to set the default column family parameter for per-cf
    arguments.
*/
const char *const DEFAULT_CF_NAME = "default";

/*
  This is the name of the Column Family used for storing the data dictionary.
*/
const char *const DEFAULT_SYSTEM_CF_NAME = "__system__";

/*
  This is the name of the hidden primary key for tables with no pk.
*/
const char *const HIDDEN_PK_NAME = "HIDDEN_PK_ID";

/*
  Column family name which means "put this index into its own column family".
  See Rdb_cf_manager::get_per_index_cf_name().
*/
const char *const PER_INDEX_CF_NAME = "$per_index_cf";

/*
  Name for the background thread.
*/
const char *const BG_THREAD_NAME = "myrocks-bg";

/*
  Name for the drop index thread.
*/
const char *const INDEX_THREAD_NAME = "myrocks-index";

/*
  Default, minimal valid, and maximum valid sampling rate values when collecting
  statistics about table.
*/
#define RDB_DEFAULT_TBL_STATS_SAMPLE_PCT 10
#define RDB_TBL_STATS_SAMPLE_PCT_MIN 1
#define RDB_TBL_STATS_SAMPLE_PCT_MAX 100

/*
  Default and maximum values for rocksdb-compaction-sequential-deletes and
  rocksdb-compaction-sequential-deletes-window to add basic boundary checking.
*/
#define DEFAULT_COMPACTION_SEQUENTIAL_DELETES 0
#define MAX_COMPACTION_SEQUENTIAL_DELETES 2000000

#define DEFAULT_COMPACTION_SEQUENTIAL_DELETES_WINDOW 0
#define MAX_COMPACTION_SEQUENTIAL_DELETES_WINDOW 2000000

/*
  Default and maximum values for various compaction and flushing related
  options. Numbers are based on the hardware we currently use and our internal
  benchmarks which indicate that parallelization helps with the speed of
  compactions.

  Ideally of course we'll use heuristic technique to determine the number of
  CPU-s and derive the values from there. This however has its own set of
  problems and we'll choose simplicity for now.
*/
#define MAX_BACKGROUND_COMPACTIONS 64
#define MAX_BACKGROUND_FLUSHES 64

#define DEFAULT_SUBCOMPACTIONS 1
#define MAX_SUBCOMPACTIONS 64

/*
  Defines the field sizes for serializing XID object to a string representation.
  string byte format: [field_size: field_value, ...]
  [
    8: XID.formatID,
    1: XID.gtrid_length,
    1: XID.bqual_length,
    XID.gtrid_length + XID.bqual_length: XID.data
  ]
*/
#define RDB_FORMATID_SZ 8
#define RDB_GTRID_SZ 1
#define RDB_BQUAL_SZ 1
#define RDB_XIDHDR_LEN (RDB_FORMATID_SZ + RDB_GTRID_SZ + RDB_BQUAL_SZ)

/*
  To fix an unhandled exception we specify the upper bound as LONGLONGMAX
  instead of ULONGLONGMAX because the latter is -1 and causes an exception when
  cast to jlong (signed) of JNI

  The reason behind the cast issue is the lack of unsigned int support in Java.
*/
#define MAX_RATE_LIMITER_BYTES_PER_SEC static_cast<uint64_t>(LONGLONG_MAX)

/*
  Hidden PK column (for tables with no primary key) is a longlong (aka 8 bytes).
  static_assert() in code will validate this assumption.
*/
#define ROCKSDB_SIZEOF_HIDDEN_PK_COLUMN sizeof(longlong)

/*
  MyRocks specific error codes. NB! Please make sure that you will update
  HA_ERR_ROCKSDB_LAST when adding new ones.
*/
#define HA_ERR_ROCKSDB_UNIQUE_NOT_SUPPORTED (HA_ERR_LAST + 1)
#define HA_ERR_ROCKSDB_PK_REQUIRED (HA_ERR_LAST + 2)
#define HA_ERR_ROCKSDB_TOO_MANY_LOCKS (HA_ERR_LAST + 3)
#define HA_ERR_ROCKSDB_LAST HA_ERR_ROCKSDB_TOO_MANY_LOCKS

inline bool looks_like_per_index_cf_typo(const char *const name) {
  return (name && name[0] == '$' && strcmp(name, PER_INDEX_CF_NAME));
}

/**
  @brief
  Rdb_table_handler is a reference-counted structure storing information for
  each open table. All the objects are stored in a global hash map.

  //TODO: join this with Rdb_tbl_def ?
*/
struct Rdb_table_handler {
  char *m_table_name;
  uint m_table_name_length;
  int m_ref_count;

  my_core::THR_LOCK m_thr_lock; ///< MySQL latch needed by m_db_lock

  /* Stores cumulative table statistics */
  my_io_perf_atomic_t m_io_perf_read;
  Rdb_atomic_perf_counters m_table_perf_context;
};

class Rdb_key_def;
class Rdb_tbl_def;
class Rdb_transaction;
class Rdb_transaction_impl;
class Rdb_writebatch_impl;
class Rdb_field_encoder;

const char *const rocksdb_hton_name = "ROCKSDB";

typedef struct _gl_index_id_s {
  uint32_t cf_id;
  uint32_t index_id;
  bool operator==(const struct _gl_index_id_s &other) const {
    return cf_id == other.cf_id && index_id == other.index_id;
  }
  bool operator!=(const struct _gl_index_id_s &other) const {
    return cf_id != other.cf_id || index_id != other.index_id;
  }
  bool operator<(const struct _gl_index_id_s &other) const {
    return cf_id < other.cf_id ||
           (cf_id == other.cf_id && index_id < other.index_id);
  }
  bool operator<=(const struct _gl_index_id_s &other) const {
    return cf_id < other.cf_id ||
           (cf_id == other.cf_id && index_id <= other.index_id);
  }
  bool operator>(const struct _gl_index_id_s &other) const {
    return cf_id > other.cf_id ||
           (cf_id == other.cf_id && index_id > other.index_id);
  }
  bool operator>=(const struct _gl_index_id_s &other) const {
    return cf_id > other.cf_id ||
           (cf_id == other.cf_id && index_id >= other.index_id);
  }
} GL_INDEX_ID;

enum operation_type {
  ROWS_DELETED = 0,
  ROWS_INSERTED,
  ROWS_READ,
  ROWS_UPDATED,
  ROWS_MAX
};

#if defined(HAVE_SCHED_GETCPU)
#define RDB_INDEXER get_sched_indexer_t
#else
#define RDB_INDEXER thread_id_indexer_t
#endif

/* Global statistics struct used inside MyRocks */
struct st_global_stats {
  ib_counter_t<ulonglong, 64, RDB_INDEXER> rows[ROWS_MAX];

  // system_rows_ stats are only for system
  // tables. They are not counted in rows_* stats.
  ib_counter_t<ulonglong, 64, RDB_INDEXER> system_rows[ROWS_MAX];
};

/* Struct used for exporting status to MySQL */
struct st_export_stats {
  ulonglong rows_deleted;
  ulonglong rows_inserted;
  ulonglong rows_read;
  ulonglong rows_updated;

  ulonglong system_rows_deleted;
  ulonglong system_rows_inserted;
  ulonglong system_rows_read;
  ulonglong system_rows_updated;
};

} // namespace myrocks

#include "./rdb_buff.h"

/* Provide hash function for GL_INDEX_ID so we can include it in sets */
namespace std {
template <> struct hash<myrocks::GL_INDEX_ID> {
  std::size_t operator()(const myrocks::GL_INDEX_ID &gl_index_id) const {
    const uint64_t val =
        ((uint64_t)gl_index_id.cf_id << 32 | (uint64_t)gl_index_id.index_id);
    return std::hash<uint64_t>()(val);
  }
};
} // namespace std

namespace myrocks {

/**
  @brief
  Class definition for ROCKSDB storage engine plugin handler
*/

class ha_rocksdb : public my_core::handler {
  my_core::THR_LOCK_DATA m_db_lock; ///< MySQL database lock

  Rdb_table_handler *m_table_handler; ///< Open table handler

  /* Iterator used for range scans and for full table/index scans */
  rocksdb::Iterator *m_scan_it;

  /* Whether m_scan_it was created with skip_bloom=true */
  bool m_scan_it_skips_bloom;

  const rocksdb::Snapshot *m_scan_it_snapshot;

  Rdb_tbl_def *m_tbl_def;

  /* Primary Key encoder from KeyTupleFormat to StorageFormat */
  std::shared_ptr<Rdb_key_def> m_pk_descr;

  /* Array of index descriptors */
  std::shared_ptr<Rdb_key_def> *m_key_descr_arr;

  bool check_keyread_allowed(uint inx, uint part, bool all_parts) const;

  /*
    Number of key parts in PK. This is the same as
      table->key_info[table->s->primary_key].keyparts
  */
  uint m_pk_key_parts;

  /*
    TRUE <=> Primary Key columns can be decoded from the index
  */
  mutable bool m_pk_can_be_decoded;

  /*
   TRUE <=> Some fields in the PK may require unpack_info.
  */
  bool m_maybe_unpack_info;

  uchar *m_pk_tuple;        /* Buffer for storing PK in KeyTupleFormat */
  uchar *m_pk_packed_tuple; /* Buffer for storing PK in StorageFormat */
  // ^^ todo: change it to 'char*'? TODO: ^ can we join this with last_rowkey?

  /*
    Temporary buffers for storing the key part of the Key/Value pair
    for secondary indexes.
  */
  uchar *m_sk_packed_tuple;

  /*
    Temporary buffers for storing end key part of the Key/Value pair.
    This is used for range scan only.
  */
  uchar *m_end_key_packed_tuple;

  Rdb_string_writer m_sk_tails;
  Rdb_string_writer m_pk_unpack_info;

  /*
    ha_rockdb->index_read_map(.. HA_READ_KEY_EXACT or similar) will save here
    mem-comparable form of the index lookup tuple.
  */
  uchar *m_sk_match_prefix;
  uint m_sk_match_length;

  /* Buffer space for the above */
  uchar *m_sk_match_prefix_buf;

  /* Second buffers, used by UPDATE. */
  uchar *m_sk_packed_tuple_old;
  Rdb_string_writer m_sk_tails_old;

  /* Buffers used for duplicate checking during unique_index_creation */
  uchar *m_dup_sk_packed_tuple;
  uchar *m_dup_sk_packed_tuple_old;

  /*
    Temporary space for packing VARCHARs (we provide it to
    pack_record()/pack_index_tuple() calls).
  */
  uchar *m_pack_buffer;

  /* rowkey of the last record we've read, in StorageFormat. */
  String m_last_rowkey;

  /* Buffer used by convert_record_to_storage_format() */
  String m_storage_record;

  /*
    Last retrieved record, in table->record[0] data format.

    This is used only when we get the record with rocksdb's Get() call (The
    other option is when we get a rocksdb::Slice from an iterator)
  */
  std::string m_retrieved_record;

  /* Type of locking to apply to rows */
  enum { RDB_LOCK_NONE, RDB_LOCK_READ, RDB_LOCK_WRITE } m_lock_rows;

  /* TRUE means we're doing an index-only read. FALSE means otherwise. */
  bool m_keyread_only;

  bool m_skip_scan_it_next_call;

  /* TRUE means we are accessing the first row after a snapshot was created */
  bool m_rnd_scan_is_new_snapshot;

  /* TRUE means the replication slave will use Read Free Replication */
  bool m_use_read_free_rpl;

  /*
    TRUE means we should skip unique key checks for this table if the
    replication lag gets too large
   */
  bool m_skip_unique_check;

  /**
    @brief
    This is a bitmap of indexes (i.e. a set) whose keys (in future, values) may
    be changed by this statement. Indexes that are not in the bitmap do not need
    to be updated.
    @note Valid inside UPDATE statements, IIF(m_update_scope_is_valid == true).
  */
  my_core::key_map m_update_scope;
  bool m_update_scope_is_valid;

  /* SST information used for bulk loading the primary key */
  std::shared_ptr<Rdb_sst_info> m_sst_info;
  Rdb_transaction *m_bulk_load_tx;
  /* Mutex to protect finalizing bulk load */
  mysql_mutex_t m_bulk_load_mutex;

  /*
    MySQL index number for duplicate key error
  */
  int m_dupp_errkey;

  int create_key_defs(const TABLE *const table_arg,
                      Rdb_tbl_def *const tbl_def_arg,
                      const TABLE *const old_table_arg = nullptr,
                      const Rdb_tbl_def *const old_tbl_def_arg = nullptr) const
      MY_ATTRIBUTE((__nonnull__(2, 3), __warn_unused_result__));
  int secondary_index_read(const int keyno, uchar *const buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void setup_iterator_for_rnd_scan();
  void setup_scan_iterator(const Rdb_key_def &kd, rocksdb::Slice *const slice)
      MY_ATTRIBUTE((__nonnull__)) {
    setup_scan_iterator(kd, slice, false, false, 0);
  }
  bool is_ascending(const Rdb_key_def &keydef,
                    enum ha_rkey_function find_flag) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void setup_scan_iterator(const Rdb_key_def &kd, rocksdb::Slice *slice,
                           const bool use_all_keys, const bool is_ascending,
                           const uint eq_cond_len) MY_ATTRIBUTE((__nonnull__));
  void release_scan_iterator(void);

  rocksdb::Status
  get_for_update(Rdb_transaction *const tx,
                 rocksdb::ColumnFamilyHandle *const column_family,
                 const rocksdb::Slice &key, std::string *const value) const;

  int get_row_by_rowid(uchar *const buf, const char *const rowid,
                       const uint rowid_size)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int get_row_by_rowid(uchar *const buf, const uchar *const rowid,
                       const uint rowid_size)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__)) {
    return get_row_by_rowid(buf, reinterpret_cast<const char *>(rowid),
                            rowid_size);
  }

  void update_auto_incr_val();
  void load_auto_incr_value();
  longlong update_hidden_pk_val();
  int load_hidden_pk_value() MY_ATTRIBUTE((__warn_unused_result__));
  int read_hidden_pk_id_from_rowkey(longlong *const hidden_pk_id)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  bool can_use_single_delete(const uint &index) const
      MY_ATTRIBUTE((__warn_unused_result__));
  bool skip_unique_check() const MY_ATTRIBUTE((__warn_unused_result__));
  void set_force_skip_unique_check(bool skip) override;
  bool commit_in_the_middle() MY_ATTRIBUTE((__warn_unused_result__));
  bool do_bulk_commit(Rdb_transaction *const tx)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  bool has_hidden_pk(const TABLE *const table) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  void update_row_stats(const operation_type &type);

  void set_last_rowkey(const uchar *const old_data);

  /*
    Array of table->s->fields elements telling how to store fields in the
    record.
  */
  Rdb_field_encoder *m_encoder_arr;

  /* Describes instructions on how to decode the field */
  class READ_FIELD {
  public:
    /* Points to Rdb_field_encoder describing the field */
    Rdb_field_encoder *m_field_enc;
    /* if true, decode the field, otherwise skip it */
    bool m_decode;
    /* Skip this many bytes before reading (or skipping) this field */
    int m_skip;
  };

  /*
    This tells which table fields should be decoded (or skipped) when
    decoding table row from (pk, encoded_row) pair. (Secondary keys are
    just always decoded in full currently)
  */
  std::vector<READ_FIELD> m_decoders_vect;

  /* Setup field_decoders based on type of scan and table->read_set */
  void setup_read_decoders();

  /*
    Number of bytes in on-disk (storage) record format that are used for
    storing SQL NULL flags.
  */
  uint m_null_bytes_in_rec;

  void get_storage_type(Rdb_field_encoder *const encoder, const uint &kp);
  void setup_field_converters();
  int alloc_key_buffers(const TABLE *const table_arg,
                        const Rdb_tbl_def *const tbl_def_arg,
                        bool alloc_alter_buffers = false)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void free_key_buffers();

  // the buffer size should be at least 2*Rdb_key_def::INDEX_NUMBER_SIZE
  rocksdb::Range get_range(const int &i, uchar buf[]) const;

  /*
    Perf timers for data reads
  */
  Rdb_io_perf m_io_perf;

  /*
    A counter of how many row checksums were checked for this table. Note that
    this does not include checksums for secondary index entries.
  */
  my_core::ha_rows m_row_checksums_checked;

  /*
    Update stats
  */
  void update_stats(void);

public:
  /*
    Controls whether writes include checksums. This is updated from the session
    variable
    at the start of each query.
  */
  bool m_store_row_debug_checksums;

  /* Same as above but for verifying checksums when reading */
  bool m_verify_row_debug_checksums;
  int m_checksums_pct;

  ha_rocksdb(my_core::handlerton *const hton,
             my_core::TABLE_SHARE *const table_arg);
  ~ha_rocksdb() {
    int err MY_ATTRIBUTE((__unused__));
    err = finalize_bulk_load();
    DBUG_ASSERT(err == 0);
    mysql_mutex_destroy(&m_bulk_load_mutex);
  }

  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(rocksdb_hton_name);
  }

  /* The following is only used by SHOW KEYS: */
  const char *index_type(uint inx) override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN("LSMTREE");
  }

  /** @brief
    The file extensions.
   */
  const char **bas_ext() const override;

  /*
    See if this is the same base table - this should only be true for different
    partitions of the same table.
  */
  bool same_table(const ha_rocksdb &other) const;

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const override {
    DBUG_ENTER_FUNC();

    /*
      HA_BINLOG_STMT_CAPABLE
        We are saying that this engine is just statement capable to have
        an engine that can only handle statement-based logging. This is
        used in testing.
      HA_REC_NOT_IN_SEQ
        If we don't set it, filesort crashes, because it assumes rowids are
        1..8 byte numbers
    */
    DBUG_RETURN(HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
                HA_REC_NOT_IN_SEQ | HA_CAN_INDEX_BLOBS |
                (m_pk_can_be_decoded ? HA_PRIMARY_KEY_IN_READ_INDEX : 0) |
                HA_PRIMARY_KEY_REQUIRED_FOR_POSITION | HA_NULL_IN_KEY |
                HA_PARTIAL_COLUMN_READ);
  }

  bool init_with_fields() override;

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

    @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MySQL wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint inx, uint part, bool all_parts) const override;

  const key_map *keys_to_use_for_scanning() override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(&key_map_full);
  }

  bool primary_key_is_clustered() override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(true);
  }

  bool should_store_row_debug_checksums() const {
    return m_store_row_debug_checksums && (rand() % 100 < m_checksums_pct);
  }

  int rename_table(const char *const from, const char *const to) override
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int convert_record_from_storage_format(const rocksdb::Slice *const key,
                                         const rocksdb::Slice *const value,
                                         uchar *const buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int convert_record_from_storage_format(const rocksdb::Slice *const key,
                                         uchar *const buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  void convert_record_to_storage_format(const rocksdb::Slice &pk_packed_slice,
                                        Rdb_string_writer *const pk_unpack_info,
                                        rocksdb::Slice *const packed_rec)
      MY_ATTRIBUTE((__nonnull__));

  static const char *get_key_name(const uint index,
                                  const TABLE *const table_arg,
                                  const Rdb_tbl_def *const tbl_def_arg)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  static const char *get_key_comment(const uint index,
                                     const TABLE *const table_arg,
                                     const Rdb_tbl_def *const tbl_def_arg)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  static bool is_hidden_pk(const uint index, const TABLE *const table_arg,
                           const Rdb_tbl_def *const tbl_def_arg)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  static uint pk_index(const TABLE *const table_arg,
                       const Rdb_tbl_def *const tbl_def_arg)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  static bool is_pk(const uint index, const TABLE *table_arg,
                    const Rdb_tbl_def *tbl_def_arg)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  /** @brief
    unireg.cc will call max_supported_record_length(), max_supported_keys(),
    max_supported_key_parts(), uint max_supported_key_length()
    to make sure that the storage engine can handle the data it is about to
    send. Return *real* limits of your storage engine here; MySQL will do
    min(your_limits, MySQL_limits) automatically.
   */
  uint max_supported_record_length() const override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(HA_MAX_REC_LENGTH);
  }

  uint max_supported_keys() const override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(MAX_INDEXES);
  }

  uint max_supported_key_parts() const override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(MAX_REF_PARTS);
  }

  uint max_supported_key_part_length() const override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(2048);
  }

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_key_length() const override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(16 * 1024); /* just to return something*/
  }

  /**
    TODO: return actual upper bound of number of records in the table.
    (e.g. save number of records seen on full table scan and/or use file size
    as upper bound)
  */
  ha_rows estimate_rows_upper_bound() override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(HA_POS_ERROR);
  }

  /* At the moment, we're ok with default handler::index_init() implementation.
   */
  int index_read_map(uchar *const buf, const uchar *const key,
                     key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override
      MY_ATTRIBUTE((__warn_unused_result__));

  int index_read_map_impl(uchar *const buf, const uchar *const key,
                          key_part_map keypart_map,
                          enum ha_rkey_function find_flag,
                          const key_range *end_key)
      MY_ATTRIBUTE((__warn_unused_result__));

  int index_read_last_map(uchar *const buf, const uchar *const key,
                          key_part_map keypart_map) override
      MY_ATTRIBUTE((__warn_unused_result__));

  int read_range_first(const key_range *const start_key,
                       const key_range *const end_key, bool eq_range,
                       bool sorted) override
      MY_ATTRIBUTE((__warn_unused_result__));

  virtual double scan_time() override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN(
        static_cast<double>((stats.records + stats.deleted) / 20.0 + 10));
  }

  virtual double read_time(uint, uint, ha_rows rows) override;

  int open(const char *const name, int mode, uint test_if_locked) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int close(void) override MY_ATTRIBUTE((__warn_unused_result__));

  int write_row(uchar *const buf) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int update_row(const uchar *const old_data, uchar *const new_data) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int delete_row(const uchar *const buf) override
      MY_ATTRIBUTE((__warn_unused_result__));
  rocksdb::Status delete_or_singledelete(uint index, Rdb_transaction *const tx,
                                         rocksdb::ColumnFamilyHandle *const cf,
                                         const rocksdb::Slice &key)
      MY_ATTRIBUTE((__warn_unused_result__));

  int index_next(uchar *const buf) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int index_next_with_direction(uchar *const buf, bool move_forward)
      MY_ATTRIBUTE((__warn_unused_result__));
  int index_prev(uchar *const buf) override
      MY_ATTRIBUTE((__warn_unused_result__));

  int index_first(uchar *const buf) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int index_last(uchar *const buf) override
      MY_ATTRIBUTE((__warn_unused_result__));

  class Item *idx_cond_push(uint keyno, class Item *const idx_cond) override;
  /*
    Default implementation from cancel_pushed_idx_cond() suits us
  */
private:
  struct key_def_cf_info {
    rocksdb::ColumnFamilyHandle *cf_handle;
    bool is_reverse_cf;
    bool is_auto_cf;
  };

  struct update_row_info {
    Rdb_transaction *tx;
    const uchar *new_data;
    const uchar *old_data;
    rocksdb::Slice new_pk_slice;
    rocksdb::Slice old_pk_slice;

    // "unpack_info" data for the new PK value
    Rdb_string_writer *new_pk_unpack_info;

    longlong hidden_pk_id;
    bool skip_unique_check;
  };

  /*
    Used to check for duplicate entries during fast unique secondary index
    creation.
  */
  struct unique_sk_buf_info {
    bool sk_buf_switch = false;
    rocksdb::Slice sk_memcmp_key;
    rocksdb::Slice sk_memcmp_key_old;
    uchar *dup_sk_buf;
    uchar *dup_sk_buf_old;

    /*
      This method is meant to be called back to back during inplace creation
      of unique indexes.  It will switch between two buffers, which
      will each store the memcmp form of secondary keys, which are then
      converted to slices in sk_memcmp_key or sk_memcmp_key_old.

      Switching buffers on each iteration allows us to retain the
      sk_memcmp_key_old value for duplicate comparison.
    */
    inline uchar *swap_and_get_sk_buf() {
      sk_buf_switch = !sk_buf_switch;
      return sk_buf_switch ? dup_sk_buf : dup_sk_buf_old;
    }
  };

  int create_cfs(const TABLE *const table_arg, Rdb_tbl_def *const tbl_def_arg,
                 std::array<struct key_def_cf_info, MAX_INDEXES + 1> *const cfs)
      const MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int create_key_def(const TABLE *const table_arg, const uint &i,
                     const Rdb_tbl_def *const tbl_def_arg,
                     std::shared_ptr<Rdb_key_def> *const new_key_def,
                     const struct key_def_cf_info &cf_info) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int create_inplace_key_defs(
      const TABLE *const table_arg, Rdb_tbl_def *vtbl_def_arg,
      const TABLE *const old_table_arg,
      const Rdb_tbl_def *const old_tbl_def_arg,
      const std::array<key_def_cf_info, MAX_INDEXES + 1> &cfs) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  std::unordered_map<std::string, uint>
  get_old_key_positions(const TABLE *table_arg, const Rdb_tbl_def *tbl_def_arg,
                        const TABLE *old_table_arg,
                        const Rdb_tbl_def *old_tbl_def_arg) const
      MY_ATTRIBUTE((__nonnull__));

  int compare_key_parts(const KEY *const old_key,
                        const KEY *const new_key) const;
  MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int index_first_intern(uchar *buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int index_last_intern(uchar *buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  enum icp_result check_index_cond() const;
  int find_icp_matching_index_rec(const bool &move_forward, uchar *const buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  void calc_updated_indexes();
  int update_write_row(const uchar *const old_data, const uchar *const new_data,
                       const bool skip_unique_check)
      MY_ATTRIBUTE((__warn_unused_result__));
  int get_pk_for_update(struct update_row_info *const row_info);
  int check_and_lock_unique_pk(const uint &key_id,
                               const struct update_row_info &row_info,
                               bool *const found, bool *const pk_changed)
      MY_ATTRIBUTE((__warn_unused_result__));
  int check_and_lock_sk(const uint &key_id,
                        const struct update_row_info &row_info,
                        bool *const found) const
      MY_ATTRIBUTE((__warn_unused_result__));
  int check_uniqueness_and_lock(const struct update_row_info &row_info,
                                bool *const pk_changed)
      MY_ATTRIBUTE((__warn_unused_result__));
  bool over_bulk_load_threshold(int *err)
      MY_ATTRIBUTE((__warn_unused_result__));
  int check_duplicate_sk(const TABLE *table_arg, const Rdb_key_def &index,
                         const rocksdb::Slice *key,
                         struct unique_sk_buf_info *sk_info)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int bulk_load_key(Rdb_transaction *const tx, const Rdb_key_def &kd,
                    const rocksdb::Slice &key, const rocksdb::Slice &value)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int update_pk(const Rdb_key_def &kd, const struct update_row_info &row_info,
                const bool &pk_changed) MY_ATTRIBUTE((__warn_unused_result__));
  int update_sk(const TABLE *const table_arg, const Rdb_key_def &kd,
                const struct update_row_info &row_info)
      MY_ATTRIBUTE((__warn_unused_result__));
  int update_indexes(const struct update_row_info &row_info,
                     const bool &pk_changed)
      MY_ATTRIBUTE((__warn_unused_result__));

  int read_key_exact(const Rdb_key_def &kd, rocksdb::Iterator *const iter,
                     const bool &using_full_key,
                     const rocksdb::Slice &key_slice) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int read_before_key(const Rdb_key_def &kd, const bool &using_full_key,
                      const rocksdb::Slice &key_slice)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int read_after_key(const Rdb_key_def &kd, const bool &using_full_key,
                     const rocksdb::Slice &key_slice)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int position_to_correct_key(
      const Rdb_key_def &kd, const enum ha_rkey_function &find_flag,
      const bool &full_key_match, const uchar *const key,
      const key_part_map &keypart_map, const rocksdb::Slice &key_slice,
      bool *const move_forward) MY_ATTRIBUTE((__warn_unused_result__));

  int read_row_from_primary_key(uchar *const buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int read_row_from_secondary_key(uchar *const buf, const Rdb_key_def &kd,
                                  bool move_forward)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int calc_eq_cond_len(const Rdb_key_def &kd,
                       const enum ha_rkey_function &find_flag,
                       const rocksdb::Slice &slice,
                       const int &bytes_changed_by_succ,
                       const key_range *const end_key,
                       uint *const end_key_packed_size)
      MY_ATTRIBUTE((__warn_unused_result__));

  Rdb_tbl_def *get_table_if_exists(const char *const tablename)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void read_thd_vars(THD *const thd) MY_ATTRIBUTE((__nonnull__));
  const char *thd_rocksdb_tmpdir()
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  bool contains_foreign_key(THD *const thd)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int inplace_populate_sk(
      TABLE *const table_arg,
      const std::unordered_set<std::shared_ptr<Rdb_key_def>> &indexes)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

public:
  int index_init(uint idx, bool sorted) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int index_end() override MY_ATTRIBUTE((__warn_unused_result__));

  void unlock_row() override;

  /** @brief
    Unlike index_init(), rnd_init() can be called two consecutive times
    without rnd_end() in between (it only makes sense if scan=1). In this
    case, the second call should prepare for the new table scan (e.g if
    rnd_init() allocates the cursor, the second call should position the
    cursor to the start of the table; no need to deallocate and allocate
    it again. This is a required method.
  */
  int rnd_init(bool scan) override MY_ATTRIBUTE((__warn_unused_result__));
  int rnd_end() override MY_ATTRIBUTE((__warn_unused_result__));

  int rnd_next(uchar *const buf) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int rnd_next_with_direction(uchar *const buf, bool move_forward)
      MY_ATTRIBUTE((__warn_unused_result__));

  int rnd_pos(uchar *const buf, uchar *const pos) override
      MY_ATTRIBUTE((__warn_unused_result__));
  void position(const uchar *const record) override;
  int info(uint) override;

  /* This function will always return success, therefore no annotation related
   * to checking the return value. Can't change the signature because it's
   * required by the interface. */
  int extra(enum ha_extra_function operation) override;

  int start_stmt(THD *const thd, thr_lock_type lock_type) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int external_lock(THD *const thd, int lock_type) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int truncate() override MY_ATTRIBUTE((__warn_unused_result__));

  int reset() override {
    DBUG_ENTER_FUNC();

    /* Free blob data */
    m_retrieved_record.clear();

    DBUG_RETURN(HA_EXIT_SUCCESS);
  }

  int check(THD *const thd, HA_CHECK_OPT *const check_opt) override
      MY_ATTRIBUTE((__warn_unused_result__));
  void remove_rows(Rdb_tbl_def *const tbl);
  ha_rows records_in_range(uint inx, key_range *const min_key,
                           key_range *const max_key) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int delete_table(const char *const from) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int create(const char *const name, TABLE *const form,
             HA_CREATE_INFO *const create_info) override
      MY_ATTRIBUTE((__warn_unused_result__));
  bool check_if_incompatible_data(HA_CREATE_INFO *const info,
                                  uint table_changes) override
      MY_ATTRIBUTE((__warn_unused_result__));

  THR_LOCK_DATA **store_lock(THD *const thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override
      MY_ATTRIBUTE((__warn_unused_result__));

  my_bool register_query_cache_table(THD *const thd, char *const table_key,
                                     uint key_length,
                                     qc_engine_callback *const engine_callback,
                                     ulonglong *const engine_data) override {
    DBUG_ENTER_FUNC();

    /* Currently, we don't support query cache */
    DBUG_RETURN(FALSE);
  }

  bool get_error_message(const int error, String *const buf) override
      MY_ATTRIBUTE((__nonnull__));

  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *const first_value,
                          ulonglong *const nb_reserved_values) override;
  void update_create_info(HA_CREATE_INFO *const create_info) override;
  int optimize(THD *const thd, HA_CHECK_OPT *const check_opt) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int analyze(THD *const thd, HA_CHECK_OPT *const check_opt) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int calculate_stats(const TABLE *const table_arg, THD *const thd,
                      HA_CHECK_OPT *const check_opt)
      MY_ATTRIBUTE((__warn_unused_result__));

  enum_alter_inplace_result check_if_supported_inplace_alter(
      TABLE *altered_table,
      my_core::Alter_inplace_info *const ha_alter_info) override;

  bool prepare_inplace_alter_table(
      TABLE *const altered_table,
      my_core::Alter_inplace_info *const ha_alter_info) override;

  bool inplace_alter_table(
      TABLE *const altered_table,
      my_core::Alter_inplace_info *const ha_alter_info) override;

  bool
  commit_inplace_alter_table(TABLE *const altered_table,
                             my_core::Alter_inplace_info *const ha_alter_info,
                             bool commit) override;

  int finalize_bulk_load() MY_ATTRIBUTE((__warn_unused_result__));

  void set_use_read_free_rpl(const char *const whitelist);
  void set_skip_unique_check_tables(const char *const whitelist);

public:
  virtual void rpl_before_delete_rows() override;
  virtual void rpl_after_delete_rows() override;
  virtual void rpl_before_update_rows() override;
  virtual void rpl_after_update_rows() override;
  virtual bool use_read_free_rpl();

private:
  /* Flags tracking if we are inside different replication operation */
  bool m_in_rpl_delete_rows;
  bool m_in_rpl_update_rows;

  bool m_force_skip_unique_check;
};

/*
  Helper class for in-place alter, for storing handler context between inplace
  alter calls
*/
struct Rdb_inplace_alter_ctx : public my_core::inplace_alter_handler_ctx {
  /* The new table definition */
  Rdb_tbl_def *const m_new_tdef;

  /* Stores the original key definitions */
  std::shared_ptr<Rdb_key_def> *const m_old_key_descr;

  /* Stores the new key definitions */
  std::shared_ptr<Rdb_key_def> *m_new_key_descr;

  /* Stores the old number of key definitions */
  const uint m_old_n_keys;

  /* Stores the new number of key definitions */
  const uint m_new_n_keys;

  /* Stores the added key glids */
  const std::unordered_set<std::shared_ptr<Rdb_key_def>> m_added_indexes;

  /* Stores the dropped key glids */
  const std::unordered_set<GL_INDEX_ID> m_dropped_index_ids;

  /* Stores number of keys to add */
  const uint m_n_added_keys;

  /* Stores number of keys to drop */
  const uint m_n_dropped_keys;

  Rdb_inplace_alter_ctx(
      Rdb_tbl_def *new_tdef, std::shared_ptr<Rdb_key_def> *old_key_descr,
      std::shared_ptr<Rdb_key_def> *new_key_descr, uint old_n_keys,
      uint new_n_keys,
      std::unordered_set<std::shared_ptr<Rdb_key_def>> added_indexes,
      std::unordered_set<GL_INDEX_ID> dropped_index_ids, uint n_added_keys,
      uint n_dropped_keys)
      : my_core::inplace_alter_handler_ctx(), m_new_tdef(new_tdef),
        m_old_key_descr(old_key_descr), m_new_key_descr(new_key_descr),
        m_old_n_keys(old_n_keys), m_new_n_keys(new_n_keys),
        m_added_indexes(added_indexes), m_dropped_index_ids(dropped_index_ids),
        m_n_added_keys(n_added_keys), m_n_dropped_keys(n_dropped_keys) {}

  ~Rdb_inplace_alter_ctx() {}

private:
  /* Disable Copying */
  Rdb_inplace_alter_ctx(const Rdb_inplace_alter_ctx &);
  Rdb_inplace_alter_ctx &operator=(const Rdb_inplace_alter_ctx &);
};

} // namespace myrocks
