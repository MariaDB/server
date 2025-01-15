/*
   Copyright (c) 2000, 2011, Oracle and/or its affiliates
   Copyright (c) 2009, 2011, Monty Program Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */


#ifdef USE_PRAGMA_INTERFACE
#pragma interface			/* gcc class implementation */
#endif

/* class for the the heap handler */

#include <heap.h>
#include "sql_class.h"                          /* THD */

class ha_heap final : public handler
{
  HP_INFO *file;
  HP_SHARE *internal_share;
  key_map btree_keys;
  /* number of records changed since last statistics update */
  ulong   records_changed;
  uint    key_stat_version;
  my_bool internal_table;
public:
  ha_heap(handlerton *hton, TABLE_SHARE *table);
  ~ha_heap() = default;
  handler *clone(const char *name, MEM_ROOT *mem_root) override;
  const char *index_type(uint inx) override
  {
    return ((table_share->key_info[inx].algorithm == HA_KEY_ALG_BTREE) ?
            "BTREE" : "HASH");
  }
  /* Rows also use a fixed-size format */
  enum row_type get_row_type() const override { return ROW_TYPE_FIXED; }
  ulonglong table_flags() const override
  {
    return (HA_FAST_KEY_READ | HA_NO_BLOBS | HA_NULL_IN_KEY |
            HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE |
            HA_CAN_SQL_HANDLER | HA_CAN_ONLINE_BACKUPS |
            HA_REC_NOT_IN_SEQ | HA_CAN_INSERT_DELAYED | HA_NO_TRANSACTIONS |
            HA_HAS_RECORDS | HA_STATS_RECORDS_IS_EXACT | HA_CAN_HASH_KEYS);
  }
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return ((table_share->key_info[inx].algorithm == HA_KEY_ALG_BTREE) ?
            HA_READ_NEXT | HA_READ_PREV | HA_READ_ORDER | HA_READ_RANGE :
            HA_ONLY_WHOLE_INDEX | HA_KEY_SCAN_NOT_ROR);
  }
  const key_map *keys_to_use_for_scanning() override;
  uint max_supported_keys()          const override { return MAX_KEY; }
  uint max_supported_key_part_length() const override { return MAX_KEY_LENGTH; }
  IO_AND_CPU_COST scan_time() override;
  IO_AND_CPU_COST keyread_time(uint index, ulong ranges, ha_rows rows,
                               ulonglong blocks) override;
  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override;
  /* 0 for avg_io_cost ensures that there are no read-block calculations */

  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  int write_row(const uchar * buf) override;
  int update_row(const uchar * old_data, const uchar * new_data) override;
  int delete_row(const uchar * buf) override;
  void get_auto_increment(ulonglong offset, ulonglong increment,
                          ulonglong nb_desired_values,
                          ulonglong *first_value,
                          ulonglong *nb_reserved_values) override;
  int index_read_map(uchar * buf, const uchar * key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_read_last_map(uchar *buf, const uchar *key, key_part_map keypart_map)
    override;
  int index_read_idx_map(uchar * buf, uint index, const uchar * key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override;
  int index_next(uchar * buf) override;
  int index_prev(uchar * buf) override;
  int index_first(uchar * buf) override;
  int index_last(uchar * buf) override;
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar * buf, uchar *pos) override;
  void position(const uchar *record) override;
  int can_continue_handler_scan() override;
  int info(uint) override;
  int extra(enum ha_extra_function operation) override;
  int reset() override;
  int external_lock(THD *thd, int lock_type) override;
  int delete_all_rows(void) override;
  int reset_auto_increment(ulonglong value) override;
  int disable_indexes(key_map map, bool persist) override;
  int enable_indexes(key_map map, bool persist) override;
  int indexes_are_disabled(void) override;
  ha_rows records_in_range(uint inx, const key_range *start_key,
                           const key_range *end_key, page_range *pages) override;
  int delete_table(const char *from) override;
  void drop_table(const char *name) override;
  int rename_table(const char * from, const char * to) override;
  int create(const char *name, TABLE *form, HA_CREATE_INFO *create_info) override;
  void update_create_info(HA_CREATE_INFO *create_info) override;

  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
			     enum thr_lock_type lock_type) override;
  int cmp_ref(const uchar *ref1, const uchar *ref2) override
  {
    return memcmp(ref1, ref2, sizeof(HEAP_PTR));
  }
  bool check_if_incompatible_data(HA_CREATE_INFO *info, uint table_changes)
    override;
  int find_unique_row(uchar *record, uint unique_idx) override;
private:
  void update_key_stats();
};
