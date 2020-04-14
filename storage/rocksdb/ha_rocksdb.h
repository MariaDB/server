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
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA */
#pragma once

#ifdef USE_PRAGMA_INTERFACE
#pragma interface /* gcc class implementation */
#endif

/* C++ standard header files */
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

/* MySQL header files */
#include "./handler.h"   /* handler */
#include "./my_global.h" /* ulonglong */
#include "./sql_string.h"

#ifdef _WIN32
#undef pthread_key_create
#undef pthread_key_delete
#undef pthread_setspecific
#undef pthread_getspecific
#endif


/* RocksDB header files */
#include "rocksdb/cache.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/perf_context.h"
#include "rocksdb/sst_file_manager.h"
#include "rocksdb/statistics.h"
#include "rocksdb/utilities/options_util.h"
#include "rocksdb/utilities/transaction_db.h"
#include "rocksdb/utilities/write_batch_with_index.h"

/* MyRocks header files */
#include "./rdb_buff.h"
#include "./rdb_comparator.h"
#include "./rdb_global.h"
#include "./rdb_index_merge.h"
#include "./rdb_io_watchdog.h"
#include "./rdb_perf_context.h"
#include "./rdb_sst_info.h"
#include "./rdb_utils.h"
#include "./ut0counter.h"

#include "rdb_mariadb_server_port.h"

class Field_blob;
class Field_varstring;
/**
  @note MyRocks Coding Conventions:
  MyRocks code follows the baseline MySQL coding conventions, available at
  http://dev.mysql.com/doc/internals/en/coding-guidelines.html, with several
  refinements (@see /storage/rocksdb/README file).
*/

namespace myrocks {

class Rdb_converter;
class Rdb_key_def;
class Rdb_tbl_def;
class Rdb_transaction;
class Rdb_transaction_impl;
class Rdb_writebatch_impl;
class Rdb_field_encoder;
/* collations, used in MariaRocks */
enum collations_used {
  COLLATION_UTF8MB4_BIN = 46,
  COLLATION_LATIN1_BIN  = 47,
  COLLATION_UTF16LE_BIN = 55,
  COLLATION_UTF32_BIN   = 61,
  COLLATION_UTF16_BIN   = 62,
  COLLATION_BINARY      = 63,
  COLLATION_UTF8_BIN    = 83
};

#if 0 // MARIAROCKS_NOT_YET : read-free replication is not supported
extern char *rocksdb_read_free_rpl_tables;
#if defined(HAVE_PSI_INTERFACE)
extern PSI_rwlock_key key_rwlock_read_free_rpl_tables;
#endif
extern Regex_list_handler rdb_read_free_regex_handler;
#endif

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
  atomic_stat<int> m_lock_wait_timeout_counter;
  atomic_stat<int> m_deadlock_counter;

  my_core::THR_LOCK m_thr_lock;  ///< MySQL latch needed by m_db_lock

  /* Stores cumulative table statistics */
  my_io_perf_atomic_t m_io_perf_read;
  my_io_perf_atomic_t m_io_perf_write;
  Rdb_atomic_perf_counters m_table_perf_context;

  /* Stores cached memtable estimate statistics */
  std::atomic_uint m_mtcache_lock;
  uint64_t m_mtcache_count;
  uint64_t m_mtcache_size;
  uint64_t m_mtcache_last_update;
};

}  // namespace myrocks

/* Provide hash function for GL_INDEX_ID so we can include it in sets */
namespace std {
template <>
struct hash<myrocks::GL_INDEX_ID> {
  std::size_t operator()(const myrocks::GL_INDEX_ID &gl_index_id) const {
    const uint64_t val =
        ((uint64_t)gl_index_id.cf_id << 32 | (uint64_t)gl_index_id.index_id);
    return std::hash<uint64_t>()(val);
  }
};
}  // namespace std

namespace myrocks {

/**
  @brief
  Class definition for ROCKSDB storage engine plugin handler
*/

class ha_rocksdb : public my_core::handler {
  my_core::THR_LOCK_DATA m_db_lock;  ///< MySQL database lock

  Rdb_table_handler *m_table_handler;  ///< Open table handler

  /* Iterator used for range scans and for full table/index scans */
  rocksdb::Iterator *m_scan_it;
  
  /* Same as handler::end_key but for start. Reverse-ordered scans need it */ 
  key_range m_save_start_range;
  const key_range *m_start_range;

  /* Whether m_scan_it was created with skip_bloom=true */
  bool m_scan_it_skips_bloom;

  const rocksdb::Snapshot *m_scan_it_snapshot;

  /* Buffers used for upper/lower bounds for m_scan_it. */
  uchar *m_scan_it_lower_bound;
  uchar *m_scan_it_upper_bound;
  rocksdb::Slice m_scan_it_lower_bound_slice;
  rocksdb::Slice m_scan_it_upper_bound_slice;

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

  /* class to convert between Mysql format and RocksDB format*/
  std::shared_ptr<Rdb_converter> m_converter;

  /*
    Pointer to the original TTL timestamp value (8 bytes) during UPDATE.
  */
  char *m_ttl_bytes;
  /*
    The TTL timestamp value can change if the explicit TTL column is
    updated. If we detect this when updating the PK, we indicate it here so
    we know we must always update any SK's.
  */
  bool m_ttl_bytes_updated;

  /* rowkey of the last record we've read, in StorageFormat. */
  String m_last_rowkey;

  /*
    Last retrieved record, in table->record[0] data format.

    This is used only when we get the record with rocksdb's Get() call (The
    other option is when we get a rocksdb::Slice from an iterator)
  */
  rocksdb::PinnableSlice m_retrieved_record;

  /* Type of locking to apply to rows */
  enum { RDB_LOCK_NONE, RDB_LOCK_READ, RDB_LOCK_WRITE } m_lock_rows;

  /* TRUE means we're doing an index-only read. FALSE means otherwise. */
  bool m_keyread_only;

  bool m_skip_scan_it_next_call;

  /* TRUE means we are accessing the first row after a snapshot was created */
  bool m_rnd_scan_is_new_snapshot;

  /*
    TRUE means we should skip unique key checks for this table if the
    replication lag gets too large
   */
  bool m_skip_unique_check;

  /*
    TRUE means INSERT ON DUPLICATE KEY UPDATE. In such case we can optimize by
    remember the failed attempt (if there is one that violates uniqueness check)
    in write_row and in the following index_read to skip the lock check and read
    entirely
   */
  bool m_insert_with_update;

  /* TRUE if last time the insertion failed due to duplicated PK */
  bool m_dup_pk_found;

#ifndef DBUG_OFF
  /* Last retreived record for sanity checking */
  String m_dup_pk_retrieved_record;
#endif

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

  /*
    MySQL index number for duplicate key error
  */
  uint m_dupp_errkey;

  int create_key_defs(const TABLE *const table_arg,
                      Rdb_tbl_def *const tbl_def_arg,
                      const TABLE *const old_table_arg = nullptr,
                      const Rdb_tbl_def *const old_tbl_def_arg = nullptr) const
      MY_ATTRIBUTE((__nonnull__(2, 3), __warn_unused_result__));
  int secondary_index_read(const int keyno, uchar *const buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void setup_iterator_for_rnd_scan();
  bool is_ascending(const Rdb_key_def &keydef,
                    enum ha_rkey_function find_flag) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void setup_iterator_bounds(const Rdb_key_def &kd,
                             const rocksdb::Slice &eq_cond, size_t bound_len,
                             uchar *const lower_bound, uchar *const upper_bound,
                             rocksdb::Slice *lower_bound_slice,
                             rocksdb::Slice *upper_bound_slice);
  bool can_use_bloom_filter(THD *thd, const Rdb_key_def &kd,
                            const rocksdb::Slice &eq_cond,
                            const bool use_all_keys);
  bool check_bloom_and_set_bounds(THD *thd, const Rdb_key_def &kd,
                                  const rocksdb::Slice &eq_cond,
                                  const bool use_all_keys, size_t bound_len,
                                  uchar *const lower_bound,
                                  uchar *const upper_bound,
                                  rocksdb::Slice *lower_bound_slice,
                                  rocksdb::Slice *upper_bound_slice);
  void setup_scan_iterator(const Rdb_key_def &kd, rocksdb::Slice *slice,
                           const bool use_all_keys, const uint eq_cond_len)
      MY_ATTRIBUTE((__nonnull__));
  void release_scan_iterator(void);

  rocksdb::Status get_for_update(
      Rdb_transaction *const tx,
      rocksdb::ColumnFamilyHandle *const column_family,
      const rocksdb::Slice &key, rocksdb::PinnableSlice *value) const;

  int get_row_by_rowid(uchar *const buf, const char *const rowid,
                       const uint rowid_size, const bool skip_lookup = false,
                       const bool skip_ttl_check = true)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int get_row_by_rowid(uchar *const buf, const uchar *const rowid,
                       const uint rowid_size, const bool skip_lookup = false,
                       const bool skip_ttl_check = true)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__)) {
    return get_row_by_rowid(buf, reinterpret_cast<const char *>(rowid),
                            rowid_size, skip_lookup, skip_ttl_check);
  }

  void load_auto_incr_value();
  ulonglong load_auto_incr_value_from_index();
  void update_auto_incr_val(ulonglong val);
  void update_auto_incr_val_from_field();
  rocksdb::Status get_datadic_auto_incr(Rdb_transaction *const tx,
                                        const GL_INDEX_ID &gl_index_id,
                                        ulonglong *new_val) const;
  longlong update_hidden_pk_val();
  int load_hidden_pk_value() MY_ATTRIBUTE((__warn_unused_result__));
  int read_hidden_pk_id_from_rowkey(longlong *const hidden_pk_id)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  bool can_use_single_delete(const uint index) const
      MY_ATTRIBUTE((__warn_unused_result__));
  bool is_blind_delete_enabled();
  bool skip_unique_check() const MY_ATTRIBUTE((__warn_unused_result__));
#ifdef MARIAROCKS_NOT_YET // MDEV-10975
  void set_force_skip_unique_check(bool skip) override;
#endif
  bool commit_in_the_middle() MY_ATTRIBUTE((__warn_unused_result__));
  bool do_bulk_commit(Rdb_transaction *const tx)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  bool has_hidden_pk(const TABLE *const table) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  void update_row_stats(const operation_type &type);

  void set_last_rowkey(const uchar *const old_data);

  /*
    For the active index, indicates which columns must be covered for the
    current lookup to be covered. If the bitmap field is null, that means this
    index does not cover the current lookup for any record.
   */
  MY_BITMAP m_lookup_bitmap = {nullptr, nullptr, nullptr, 0, 0};

  int alloc_key_buffers(const TABLE *const table_arg,
                        const Rdb_tbl_def *const tbl_def_arg,
                        bool alloc_alter_buffers = false)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void free_key_buffers();

  // the buffer size should be at least 2*Rdb_key_def::INDEX_NUMBER_SIZE
  rocksdb::Range get_range(const int i, uchar buf[]) const;

  /*
    Perf timers for data reads
  */
  Rdb_io_perf m_io_perf;

  /*
    Update stats
  */
  void update_stats(void);

 public:
  /*
    The following two are currently only used for getting the range bounds
    from QUICK_SELECT_DESC.
    We don't need to implement prepare_index_key_scan[_map] because it is
    only used with HA_READ_KEY_EXACT and HA_READ_PREFIX_LAST where one
    can infer the bounds of the range being scanned, anyway.
  */
  int prepare_index_scan() override;
  int prepare_range_scan(const key_range *start_key,
                         const key_range *end_key) override;

  /*
    Controls whether writes include checksums. This is updated from the session
    variable
    at the start of each query.
  */
  bool m_store_row_debug_checksums;

  int m_checksums_pct;

  ha_rocksdb(my_core::handlerton *const hton,
             my_core::TABLE_SHARE *const table_arg);
  virtual ~ha_rocksdb() override {
    int err MY_ATTRIBUTE((__unused__));
    err = finalize_bulk_load(false);
    if (err != 0) {
      // NO_LINT_DEBUG
      sql_print_error(
          "RocksDB: Error %d finalizing bulk load while closing "
          "handler.",
          err);
    }
  }

  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const /*override*/ {
    DBUG_ENTER_FUNC();
   // MariaDB: this function is not virtual, however ha_innodb
   // declares it (and then never uses!) psergey-merge-todo:.
    DBUG_RETURN(rocksdb_hton_name);
  }

  /* The following is only used by SHOW KEYS: */
  const char *index_type(uint inx) override {
    DBUG_ENTER_FUNC();

    DBUG_RETURN("LSMTREE");
  }

  /* 
    Not present in MariaDB:
    const char **bas_ext() const override;
  */

  /*
    Returns the name of the table's base name
  */
  const std::string &get_table_basename() const;

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const override ;
private:
  bool init_with_fields(); /* no 'override' in MariaDB */
public:
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

  static const std::vector<std::string> parse_into_tokens(const std::string &s,
                                                          const char delim);

  static const std::string generate_cf_name(
      const uint index, const TABLE *const table_arg,
      const Rdb_tbl_def *const tbl_def_arg, bool *per_part_match_found);

  static const char *get_key_name(const uint index,
                                  const TABLE *const table_arg,
                                  const Rdb_tbl_def *const tbl_def_arg)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  static const char *get_key_comment(const uint index,
                                     const TABLE *const table_arg,
                                     const Rdb_tbl_def *const tbl_def_arg)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  static const std::string get_table_comment(const TABLE *const table_arg)
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

  uint max_supported_key_part_length() const override;

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

  bool is_using_full_key(key_part_map keypart_map, uint actual_key_parts);
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
  virtual void print_error(int error, myf errflag) override;

  int open(const char *const name, int mode, uint test_if_locked) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int close(void) override MY_ATTRIBUTE((__warn_unused_result__));

  int write_row(const uchar *const buf) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int update_row(const uchar *const old_data, const uchar *const new_data) override
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
    bool is_per_partition_cf;
  };

  struct update_row_info {
    Rdb_transaction *tx;
    const uchar *new_data;
    const uchar *old_data;
    rocksdb::Slice new_pk_slice;
    rocksdb::Slice old_pk_slice;
    rocksdb::Slice old_pk_rec;

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

  int create_key_def(const TABLE *const table_arg, const uint i,
                     const Rdb_tbl_def *const tbl_def_arg,
                     std::shared_ptr<Rdb_key_def> *const new_key_def,
                     const struct key_def_cf_info &cf_info, uint64 ttl_duration,
                     const std::string &ttl_column) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int create_inplace_key_defs(
      const TABLE *const table_arg, Rdb_tbl_def *vtbl_def_arg,
      const TABLE *const old_table_arg,
      const Rdb_tbl_def *const old_tbl_def_arg,
      const std::array<key_def_cf_info, MAX_INDEXES + 1> &cf,
      uint64 ttl_duration, const std::string &ttl_column) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  std::unordered_map<std::string, uint> get_old_key_positions(
      const TABLE *table_arg, const Rdb_tbl_def *tbl_def_arg,
      const TABLE *old_table_arg, const Rdb_tbl_def *old_tbl_def_arg) const
      MY_ATTRIBUTE((__nonnull__));

  using handler::compare_key_parts;
  int compare_key_parts(const KEY *const old_key,
                        const KEY *const new_key) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int compare_keys(const KEY *const old_key, const KEY *const new_key) const
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  bool should_hide_ttl_rec(const Rdb_key_def &kd,
                           const rocksdb::Slice &ttl_rec_val,
                           const int64_t curr_ts)
      MY_ATTRIBUTE((__warn_unused_result__));
  int rocksdb_skip_expired_records(const Rdb_key_def &kd,
                                   rocksdb::Iterator *const iter,
                                   bool seek_backward);

  int index_first_intern(uchar *buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int index_last_intern(uchar *buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int find_icp_matching_index_rec(const bool move_forward, uchar *const buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  void calc_updated_indexes();
  int update_write_row(const uchar *const old_data, const uchar *const new_data,
                       const bool skip_unique_check)
      MY_ATTRIBUTE((__warn_unused_result__));
  int get_pk_for_update(struct update_row_info *const row_info);
  int check_and_lock_unique_pk(const uint key_id,
                               const struct update_row_info &row_info,
                               bool *const found)
      MY_ATTRIBUTE((__warn_unused_result__));
  int check_and_lock_sk(const uint key_id,
                        const struct update_row_info &row_info,
                        bool *const found)
      MY_ATTRIBUTE((__warn_unused_result__));
  int check_uniqueness_and_lock(const struct update_row_info &row_info,
                                bool pk_changed)
      MY_ATTRIBUTE((__warn_unused_result__));
  bool over_bulk_load_threshold(int *err)
      MY_ATTRIBUTE((__warn_unused_result__));
  int check_duplicate_sk(const TABLE *table_arg, const Rdb_key_def &key_def,
                         const rocksdb::Slice *key,
                         struct unique_sk_buf_info *sk_info)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int bulk_load_key(Rdb_transaction *const tx, const Rdb_key_def &kd,
                    const rocksdb::Slice &key, const rocksdb::Slice &value,
                    bool sort)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void update_bytes_written(ulonglong bytes_written);
  int update_write_pk(const Rdb_key_def &kd,
                      const struct update_row_info &row_info,
                      const bool pk_changed)
      MY_ATTRIBUTE((__warn_unused_result__));
  int update_write_sk(const TABLE *const table_arg, const Rdb_key_def &kd,
                      const struct update_row_info &row_info,
                      const bool bulk_load_sk)
      MY_ATTRIBUTE((__warn_unused_result__));
  int update_write_indexes(const struct update_row_info &row_info,
                           const bool pk_changed)
      MY_ATTRIBUTE((__warn_unused_result__));

  int read_key_exact(const Rdb_key_def &kd, rocksdb::Iterator *const iter,
                     const bool using_full_key, const rocksdb::Slice &key_slice,
                     const int64_t ttl_filter_ts)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int read_before_key(const Rdb_key_def &kd, const bool using_full_key,
                      const rocksdb::Slice &key_slice,
                      const int64_t ttl_filter_ts)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int read_after_key(const Rdb_key_def &kd, const rocksdb::Slice &key_slice,
                     const int64_t ttl_filter_ts)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int position_to_correct_key(const Rdb_key_def &kd,
                              const enum ha_rkey_function &find_flag,
                              const bool full_key_match, const uchar *const key,
                              const key_part_map &keypart_map,
                              const rocksdb::Slice &key_slice,
                              bool *const move_forward,
                              const int64_t ttl_filter_ts)
      MY_ATTRIBUTE((__warn_unused_result__));

  int read_row_from_primary_key(uchar *const buf)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  int read_row_from_secondary_key(uchar *const buf, const Rdb_key_def &kd,
                                  bool move_forward)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int calc_eq_cond_len(const Rdb_key_def &kd,
                       const enum ha_rkey_function &find_flag,
                       const rocksdb::Slice &slice,
                       const int bytes_changed_by_succ,
                       const key_range *const end_key,
                       uint *const end_key_packed_size)
      MY_ATTRIBUTE((__warn_unused_result__));

  Rdb_tbl_def *get_table_if_exists(const char *const tablename)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));
  void read_thd_vars(THD *const thd) MY_ATTRIBUTE((__nonnull__));

  bool contains_foreign_key(THD *const thd)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int inplace_populate_sk(
      TABLE *const table_arg,
      const std::unordered_set<std::shared_ptr<Rdb_key_def>> &indexes)
      MY_ATTRIBUTE((__nonnull__, __warn_unused_result__));

  int finalize_bulk_load(bool print_client_error = true)
      MY_ATTRIBUTE((__warn_unused_result__));

  int calculate_stats_for_table() MY_ATTRIBUTE((__warn_unused_result__));

  bool should_skip_invalidated_record(const int rc);
  bool should_recreate_snapshot(const int rc, const bool is_new_snapshot);
  bool can_assume_tracked(THD *thd);

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
    m_retrieved_record.Reset();

    DBUG_RETURN(HA_EXIT_SUCCESS);
  }

  int check(THD *const thd, HA_CHECK_OPT *const check_opt) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int remove_rows(Rdb_tbl_def *const tbl);
  ha_rows records_in_range(uint inx,
                           const key_range *const min_key,
                           const key_range *const max_key,
                           page_range *pages) override
      MY_ATTRIBUTE((__warn_unused_result__));

  int delete_table(Rdb_tbl_def *const tbl);
  int delete_table(const char *const from) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int create(const char *const name, TABLE *const form,
             HA_CREATE_INFO *const create_info) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int create_table(const std::string &table_name, const TABLE *table_arg,
                   ulonglong auto_increment_value);
  bool check_if_incompatible_data(HA_CREATE_INFO *const info,
                                  uint table_changes) override
      MY_ATTRIBUTE((__warn_unused_result__));

  THR_LOCK_DATA **store_lock(THD *const thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override
      MY_ATTRIBUTE((__warn_unused_result__));

  my_bool register_query_cache_table(THD *const thd, const char *table_key,
                                     uint key_length,
                                     qc_engine_callback *const engine_callback,
                                     ulonglong *const engine_data) override {
    DBUG_ENTER_FUNC();

    /* Currently, we don't support query cache */
    DBUG_RETURN(FALSE);
  }

  bool get_error_message(const int error, String *const buf) override
      MY_ATTRIBUTE((__nonnull__));

  static int rdb_error_to_mysql(const rocksdb::Status &s,
                                const char *msg = nullptr)
      MY_ATTRIBUTE((__warn_unused_result__));

  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *const first_value,
                          ulonglong *const nb_reserved_values) override;
  void update_create_info(HA_CREATE_INFO *const create_info) override;
  int optimize(THD *const thd, HA_CHECK_OPT *const check_opt) override
      MY_ATTRIBUTE((__warn_unused_result__));
  int analyze(THD *const thd, HA_CHECK_OPT *const check_opt) override
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

  bool commit_inplace_alter_table(
      TABLE *const altered_table,
      my_core::Alter_inplace_info *const ha_alter_info, bool commit) override;

  void set_skip_unique_check_tables(const char *const whitelist);
#ifdef MARIAROCKS_NOT_YET // MDEV-10976
  bool is_read_free_rpl_table() const;
#endif

#ifdef MARIAROCKS_NOT_YET // MDEV-10976
 public:
  virtual void rpl_before_delete_rows() override;
  virtual void rpl_after_delete_rows() override;
  virtual void rpl_before_update_rows() override;
  virtual void rpl_after_update_rows() override;
  virtual bool use_read_free_rpl() const override;
#endif // MARIAROCKS_NOT_YET

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

  /* Stores the largest current auto increment value in the index */
  const ulonglong m_max_auto_incr;

  Rdb_inplace_alter_ctx(
      Rdb_tbl_def *new_tdef, std::shared_ptr<Rdb_key_def> *old_key_descr,
      std::shared_ptr<Rdb_key_def> *new_key_descr, uint old_n_keys,
      uint new_n_keys,
      std::unordered_set<std::shared_ptr<Rdb_key_def>> added_indexes,
      std::unordered_set<GL_INDEX_ID> dropped_index_ids, uint n_added_keys,
      uint n_dropped_keys, ulonglong max_auto_incr)
      : my_core::inplace_alter_handler_ctx(),
        m_new_tdef(new_tdef),
        m_old_key_descr(old_key_descr),
        m_new_key_descr(new_key_descr),
        m_old_n_keys(old_n_keys),
        m_new_n_keys(new_n_keys),
        m_added_indexes(added_indexes),
        m_dropped_index_ids(dropped_index_ids),
        m_n_added_keys(n_added_keys),
        m_n_dropped_keys(n_dropped_keys),
        m_max_auto_incr(max_auto_incr) {}

  ~Rdb_inplace_alter_ctx() {}

 private:
  /* Disable Copying */
  Rdb_inplace_alter_ctx(const Rdb_inplace_alter_ctx &);
  Rdb_inplace_alter_ctx &operator=(const Rdb_inplace_alter_ctx &);
};

// file name indicating RocksDB data corruption
std::string rdb_corruption_marker_file_name();

const int MYROCKS_MARIADB_PLUGIN_MATURITY_LEVEL= MariaDB_PLUGIN_MATURITY_STABLE;

extern bool prevent_myrocks_loading;

void sql_print_verbose_info(const char *format, ...);

}  // namespace myrocks

