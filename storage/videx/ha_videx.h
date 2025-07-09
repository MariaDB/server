/* Copyright (c) 2024 Bytedance Ltd. and/or its affiliates

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License, version 2.0,
  as published by the Free Software Foundation.

  This program is also distributed with certain software (including
  but not limited to OpenSSL) that is licensed under separate terms,
  as designated in a particular file or component or in included license
  documentation.  The authors of MySQL hereby grant you an additional
  permission to link the program and your derivative works with the
  separately licensed software that they have included with MySQL.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License, version 2.0, for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/** @file ha_videx.h

    @brief
  The ha_videx engine is a stubbed storage engine for example purposes only;
  it does nothing at this point. Its purpose is to provide a source
  code illustration of how to begin writing new storage engines; see also
  /storage/videx/ha_videx.cc.

    @note
  Please read ha_videx.cc before reading this file.
  Reminder: The videx storage engine implements all methods that are
  *required* to be implemented. For a full list of all methods that you can
  implement, see handler.h.

   @see
  /sql/handler.h and /storage/videx/ha_videx.cc
*/

#include <sys/types.h>

#include "my_base.h"
#include "my_compiler.h"
#include "my_inttypes.h"
#include "sql/handler.h"
#include "thr_lock.h"
#include <iostream>
#include "dd/types/table.h"
#include "videx_log_utils.h"
#include <table.h>
#include "dd/types/index_element.h"
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include "log.h"
#include "my_dbug.h"
#include "mysql/plugin.h"
#include "sql/sql_class.h"
#include "sql/sql_plugin.h"
#include "sql/sql_thd_internal_api.h"
#include "typelib.h"
#include "sql/field.h"
#include <curl/curl.h>
#include "opt_trace.h"
#include "replication.h"
#include <sql_table.h>

/** @brief
  videx_share is a class that will be shared among all open handlers.
  This videx implements the minimum of what you will probably need.
*/
class videx_share : public Handler_share {
 public:
  THR_LOCK lock;
  videx_share();
  ~videx_share() override { thr_lock_delete(&lock); }
};

namespace dd {
namespace cache {
class Dictionary_client;
}
}  // namespace dd
/** @brief
  Class definition for the storage engine
*/
class ha_videx : public handler {
  THR_LOCK_DATA lock;          ///< MySQL lock
  videx_share *share;        ///< Shared lock info
  videx_share *get_share();  ///< Get the share
  ha_rows index_next_cnt;

 public:
  ha_videx(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_videx() override = default;


  /** @brief
    The name that will be used for display purposes.
   */
  const char *table_type() const override { return "VIDEX"; }

  /**
    Replace key algorithm with one supported by SE, return the default key
    algorithm for SE if explicit key algorithm was not provided.

    @sa handler::adjust_index_algorithm().
  */
  enum ha_key_alg get_default_index_algorithm() const override {
    return HA_KEY_ALG_BTREE;
  }
  bool is_index_algorithm_supported(enum ha_key_alg key_alg) const override {
    //    return key_alg == HA_KEY_ALG_HASH;
    assert(key_alg != HA_KEY_ALG_FULLTEXT && key_alg != HA_KEY_ALG_RTREE);
    return key_alg == HA_KEY_ALG_BTREE;
  }

  /** @brief
    This is a list of flags that indicate what functionality the storage engine
    implements. The current table flags are documented in handler.h
  */
  ulonglong table_flags() const override;

  /** @brief
    This is a bitmap of flags that indicates how the storage engine
    implements indexes. The current index flags are documented in
    handler.h. If you do not implement indexes, just return zero here.

      @details
    part is the key part to check. First key part is 0.
    If all_parts is set, MySQL wants to know the flags for the combined
    index, up to and including 'part'.
  */
  ulong index_flags(uint inx [[maybe_unused]], uint part [[maybe_unused]],
                    bool all_parts [[maybe_unused]]) const override;

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_key_length() const override;

  /** @brief
    unireg.cc will call this to make sure that the storage engine can handle
    the data it is about to send. Return *real* limits of your storage engine
    here; MySQL will do min(your_limits, MySQL_limits) automatically.

      @details
    There is no need to implement ..._key_... methods if your engine doesn't
    support indexes.
   */
  uint max_supported_keys() const override;

  /** @name Multi Range Read interface
  @{ */

  /** Initialize multi range read @see DsMrr_impl::dsmrr_init */
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode,
                            HANDLER_BUFFER *buf) override;

  /** Process next multi range read @see DsMrr_impl::dsmrr_next */
  int multi_range_read_next(char **range_info) override;

  /** Initialize multi range read and get information.
  @see ha_myisam::multi_range_read_info_const
  @see DsMrr_impl::dsmrr_info_const */
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param, uint n_ranges,
                                      uint *bufsz, uint *flags,
                                      Cost_estimate *cost) override;

  /** Initialize multi range read and get information.
  @see DsMrr_impl::dsmrr_info */
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint *bufsz, uint *flags,
                                Cost_estimate *cost) override;


  /** Attempt to push down an index condition.
  @param[in] keyno MySQL key number
  @param[in] idx_cond Index condition to be checked
  @return idx_cond if pushed; NULL if not pushed */
  Item *idx_cond_push(uint keyno, Item *idx_cond) override;

// NT_SAME_TO storage/innobase/include/rem0types.h:66
uint32_t REC_ANTELOPE_MAX_INDEX_COL_LEN = 768;
uint32_t REC_VERSION_56_MAX_INDEX_COL_LEN = 3072;

uint max_supported_key_part_length(
      HA_CREATE_INFO *create_info) const override {
  /* A table format specific index column length check will be performed
  at ha_innobase::add_index() and row_create_index_for_mysql() */
  switch (create_info->row_type) {
    case ROW_TYPE_REDUNDANT:
    case ROW_TYPE_COMPACT:
      return (REC_ANTELOPE_MAX_INDEX_COL_LEN - 1);
      break;
    default:
      return (REC_VERSION_56_MAX_INDEX_COL_LEN);
  }
}
  /** @brief
    Called in test_quick_select to determine if indexes should be used.
  */
  double scan_time() override;

  /** @brief
    This method will never be called if you do not implement indexes.
  */
  double read_time(uint index, uint ranges, ha_rows rows) override {
    videx_log_ins.markHaFuncPassby(FUNC_FILE_LINE);
    // ------------------------------
    // example raw implementation
    // return (double)rows / 20.0 + 1;
    // ------------------------------
    // innodb implementation
    ha_rows total_rows;

    if (index != table->s->primary_key) {
      /* Not clustered */
      return (handler::read_time(index, ranges, rows));
    }

    if (rows <= 2) {
      return ((double)rows);
    }

    /* Assume that the read time is proportional to the scan time for all
    rows + at most one seek per range. */

    double time_for_scan = scan_time();

    if ((total_rows = estimate_rows_upper_bound()) < rows) {
      return (time_for_scan);
    }

    return (ranges + (double)rows / (double)total_rows * time_for_scan);
  }


  longlong get_memory_buffer_size() const override;

  int records(ha_rows *num_rows) override{
    *num_rows = stats.records;
    videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, stats.records);
    return 0;
  }
  int records_from_index(ha_rows *num_rows, uint) override {
    /* Force use of cluster index until we implement sec index parallel scan. */
    int res = ha_videx::records(num_rows);
    videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, res);
    return res;
  }

 /**
    Return upper bound of current number of records in the table
    (max. of how many records one will retrieve when doing a full table scan)
    If upper bound is not known, HA_POS_ERROR should be returned as a max
    possible upper bound.
  */
  virtual ha_rows estimate_rows_upper_bound() override {
    ha_rows res = stats.records + EXTRA_RECORDS;
    videx_log_ins.markPassby_otherType(FUNC_FILE_LINE, res);
    return res;
  }

  int get_extra_columns_and_keys(const HA_CREATE_INFO *,
                                 const List<Create_field> *, const KEY *, uint,
                                 dd::Table *dd_table) override;

  // ######################## 新 mock 函数 end ##########################
  /*
    Everything below are methods that we implement in ha_videx.cc.

    Most of these methods are not obligatory, skip them and
    MySQL will treat them as not implemented
  */
  /** @brief
    We implement this in ha_videx.cc; it's a required method.
  */
  int open(const char *name, int mode, uint test_if_locked,
           const dd::Table *table_def) override;  // required

  /** @brief
    We implement this in ha_videx.cc; it's a required method.
  */
  int close(void) override;  // required

  /** @brief
    We implement this in ha_videx.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int write_row(uchar *buf) override;

  /** @brief
    We implement this in ha_videx.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int update_row(const uchar *old_data, uchar *new_data) override;

  /** @brief
    We implement this in ha_videx.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int delete_row(const uchar *buf) override;

//  /** @brief
//    We implement this in ha_videx.cc. It's not an obligatory method;
//    skip it and and MySQL will treat it as not implemented.
//  */
//  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
//                     enum ha_rkey_function find_flag) override;

  int index_read(uchar *buf, const uchar *key, uint key_len,
                 ha_rkey_function find_flag) override;

  /** @brief
    We implement this in ha_videx.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_next(uchar *buf) override;

  /** @brief
    We implement this in ha_videx.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_prev(uchar *buf) override;

  /** @brief
    We implement this in ha_videx.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_first(uchar *buf) override;

  /** @brief
    We implement this in ha_videx.cc. It's not an obligatory method;
    skip it and and MySQL will treat it as not implemented.
  */
  int index_last(uchar *buf) override;

  bool has_gap_locks() const noexcept override {
    videx_log_ins.markPassbyUnexpected(FUNC_FILE_LINE);
    return true; }

  /** @brief
    Unlike index_init(), rnd_init() can be called two consecutive times
    without rnd_end() in between (it only makes sense if scan=1). In this
    case, the second call should prepare for the new table scan (e.g if
    rnd_init() allocates the cursor, the second call should position the
    cursor to the start of the table; no need to deallocate and allocate
    it again. This is a required method.
  */
  int rnd_init(bool scan) override;  // required
  int rnd_end() override;
  int rnd_next(uchar *buf) override;             ///< required
  int rnd_pos(uchar *buf, uchar *pos) override;  ///< required
  void position(const uchar *record) override;   ///< required

  bool primary_key_is_clustered() const override;
  /** Returns statistics information of the table to the MySQL interpreter, in
                                               various fields of the handle
     object.
                                               @param[in]    flag what
     information is requested
                                               @param[in]    is_analyze True if
     called from "::analyze()".
                                               @return HA_ERR_* error code or 0
   */
  virtual int info_low(uint flag, bool is_analyze);
  int info(uint) override;                       ///< required
  int extra(enum ha_extra_function operation) override;
  int external_lock(THD *thd, int lock_type) override;  ///< required
  int delete_all_rows(void) override;
  ha_rows records_in_range(uint keynr, key_range *min_key,
                           key_range *max_key) override;
  int delete_table(const char *from, const dd::Table *table_def) override;
  int rename_table(const char *from, const char *to,
                   const dd::Table *from_table_def,
                   dd::Table *to_table_def) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info,
             dd::Table *table_def) override;  ///< required

  THR_LOCK_DATA **store_lock(
      THD *thd, THR_LOCK_DATA **to,
      enum thr_lock_type lock_type) override;  ///< required


  /** The multi range read session object */
  DsMrr_impl m_ds_mrr;

  /** Save CPU time with prebuilt/cached data structures */
//  row_prebuilt_t *m_prebuilt;

  /** Thread handle of the user currently using the handler;
  this is set in external_lock function */
  THD *m_user_thd;

//  /** information for MySQL table locking */
//  INNOBASE_SHARE *m_share;

  /** buffer used in updates */
  uchar *m_upd_buf;

  /** the size of upd_buf in bytes */
//  ulint m_upd_buf_size;

  /** Flags that specify the handler instance (table) capability. */
  Table_flags m_int_table_flags;

  /** this is set to 1 when we are starting a table scan but have
  not yet fetched any row, else false */
  bool m_start_of_scan;

  /*!< match mode of the latest search: ROW_SEL_EXACT,
  ROW_SEL_EXACT_PREFIX, or undefined */
  uint m_last_match_mode{0};

  /** this field is used to remember the original select_lock_type that
  was decided in ha_innodb.cc,":: store_lock()", "::external_lock()",
  etc. */
//  ulint m_stored_select_lock_type;

  /** If mysql has locked with external_lock() */
  bool m_mysql_has_locked;
};
