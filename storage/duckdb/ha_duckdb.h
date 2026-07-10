/*
  Copyright (c) 2026, MariaDB Foundation.
  Copyright (c) 2026, Roman Nozdrin <drrtuy@gmail.com>
  Copyright (c) 2026, Leonid Fedorov.

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; version 2 of the License.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335 USA
*/

#ifndef HA_DUCKDB_H
#define HA_DUCKDB_H

#include <sys/types.h>
#include <memory>

#include "my_global.h"
#include "my_base.h"
#include "my_compiler.h"
#include "handler.h"
#include "thr_lock.h"
#include "sql_class.h"
#include "sql_show.h"

#undef UNKNOWN

#include "duckdb.hpp"
#include "duckdb/common/types.hpp"

#include "duckdb_handler_errors.h"

extern handlerton *duckdb_hton;

/** @brief
  Duckdb_share is a class that will be shared among all open handlers.
*/
class Duckdb_share : public Handler_share
{
public:
  THR_LOCK lock;
  Duckdb_share();
  ~Duckdb_share() { thr_lock_delete(&lock); }
};

/** @brief
  Class definition for the DuckDB storage engine (MariaDB port)
*/
class ha_duckdb : public handler
{
  THR_LOCK_DATA lock;        ///< MariaDB lock
  Duckdb_share *share;       ///< Shared lock info
  Duckdb_share *get_share(); ///< Get the share

public:
  ha_duckdb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_duckdb();

  const char *table_type() const override { return "DUCKDB"; }

  ulonglong table_flags() const override
  {
    return (HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE |
            HA_NO_AUTO_INCREMENT | HA_NULL_IN_KEY | HA_CAN_INDEX_BLOBS |
            HA_CAN_DIRECT_UPDATE_AND_DELETE);
  }

  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return 0;
  }

  uint max_supported_record_length() const override
  {
    return HA_MAX_REC_LENGTH;
  }

  uint max_supported_keys() const override { return MAX_KEY; }

  uint max_supported_key_parts() const override { return MAX_REF_PARTS; }

  uint max_supported_key_length() const override { return 10240; }

  IO_AND_CPU_COST scan_time() override
  {
    IO_AND_CPU_COST cost;
    cost.io= (double) (stats.records + stats.deleted) * DISK_READ_COST;
    cost.cpu= 0;
    return cost;
  }

  IO_AND_CPU_COST keyread_time(uint, ulong, ha_rows rows,
                               ulonglong blocks) override
  {
    IO_AND_CPU_COST cost;
    cost.io= blocks * DISK_READ_COST;
    cost.cpu= (double) rows * 0.001;
    return cost;
  }

  /* Methods implemented in ha_duckdb.cc */
  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;

  int write_row(const uchar *buf) override;
  int update_row(const uchar *old_data, const uchar *new_data) override;
  int delete_row(const uchar *buf) override;

  int index_read_map(uchar *buf, const uchar *key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_next(uchar *buf) override;
  int index_prev(uchar *buf) override;
  int index_first(uchar *buf) override;
  int index_last(uchar *buf) override;

  int rnd_init(bool scan) override;
  int rnd_end() override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar *buf, uchar *pos) override;
  void position(const uchar *record) override;
  int info(uint) override;
  int extra(enum ha_extra_function operation) override;
  int external_lock(THD *thd, int lock_type) override;
  int delete_all_rows(void) override;
  const COND *cond_push(const COND *cond) override;
  int direct_delete_rows_init() override;
  int direct_delete_rows(ha_rows *delete_rows) override;
  int direct_update_rows_init(List<Item> *update_fields) override;
  int direct_update_rows(ha_rows *update_rows, ha_rows *found_rows) override;
  ha_rows records() override;
  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key,
                           page_range *pages) override;
  int delete_table(const char *from) override;
  int rename_table(const char *from, const char *to) override;
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info) override;
  int truncate() override;

  uint lock_count(void) const override;
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;

  enum_alter_inplace_result
  check_if_supported_inplace_alter(TABLE *altered_table,
                                   Alter_inplace_info *ha_alter_info) override;
  bool commit_inplace_alter_table(TABLE *altered_table,
                                  Alter_inplace_info *ha_alter_info,
                                  bool commit) override;

private:
  std::unique_ptr<duckdb::QueryResult> query_result;
  std::unique_ptr<duckdb::DataChunk> current_chunk;
  size_t current_row_index= 0;
  MY_BITMAP m_blob_map;
  bool m_first_write{true};
};

#endif // HA_DUCKDB_H
