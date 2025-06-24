/* Copyright (c) 2005, 2010, Oracle and/or its affiliates. All rights reserved.

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

#include "thr_lock.h"                           /* THR_LOCK */
#include "handler.h"                            /* handler */
#include "table.h"                              /* TABLE_SHARE */
#include "sql_const.h"                          /* MAX_KEY */

/*
  Shared structure for correct LOCK operation
*/
struct st_blackhole_share {
  THR_LOCK lock;
  uint use_count;
  uint table_name_length;
  char table_name[1];
};


/*
  Class definition for the blackhole storage engine
  "Dumbest named feature ever"
*/
class ha_blackhole final : public handler
{
  THR_LOCK_DATA lock;      /* MySQL lock */
  st_blackhole_share *share;

public:
  ha_blackhole(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_blackhole() = default;
  /*
    The name of the index type that will be used for display
    don't implement this method unless you really have indexes
  */
  ulonglong table_flags() const override
  {
    return(HA_NULL_IN_KEY | HA_CAN_FULLTEXT | HA_CAN_SQL_HANDLER |
           HA_BINLOG_STMT_CAPABLE | HA_BINLOG_ROW_CAPABLE |
           HA_CAN_INDEX_BLOBS | HA_AUTO_PART_KEY | HA_CAN_ONLINE_BACKUPS |
           HA_FILE_BASED | HA_CAN_GEOMETRY | HA_CAN_INSERT_DELAYED);
  }
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  {
    return ((table_share->key_info[inx].algorithm == HA_KEY_ALG_FULLTEXT) ?
            0 : HA_READ_NEXT | HA_READ_PREV | HA_READ_RANGE |
            HA_READ_ORDER | HA_KEYREAD_ONLY);
  }
  /* The following defines can be increased if necessary */
#define BLACKHOLE_MAX_KEY	MAX_KEY		/* Max allowed keys */
#define BLACKHOLE_MAX_KEY_SEG	16		/* Max segments for key */
#define BLACKHOLE_MAX_KEY_LENGTH	3500		/* Like in InnoDB */
  uint max_supported_keys() const       override { return BLACKHOLE_MAX_KEY; }
  uint max_supported_key_length() const override { return BLACKHOLE_MAX_KEY_LENGTH; }
  uint max_supported_key_part_length() const override { return BLACKHOLE_MAX_KEY_LENGTH; }
  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  int truncate() override;
  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar * buf, uchar *pos) override;
  int index_read_map(uchar * buf, const uchar * key, key_part_map keypart_map,
                     enum ha_rkey_function find_flag) override;
  int index_read_idx_map(uchar * buf, uint idx, const uchar * key,
                         key_part_map keypart_map,
                         enum ha_rkey_function find_flag) override;
  int index_read_last_map(uchar * buf, const uchar * key, key_part_map keypart_map) override;
  int index_next(uchar * buf) override;
  int index_prev(uchar * buf) override;
  int index_first(uchar * buf) override;
  int index_last(uchar * buf) override;
  void position(const uchar *record) override;
  int info(uint flag) override;
  int external_lock(THD *thd, int lock_type) override;
  int create(const char *name, TABLE *table_arg,
             HA_CREATE_INFO *create_info) override;
  THR_LOCK_DATA **store_lock(THD *thd,
                             THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;
  int delete_table(const char *name) override
  {
    return 0;
  }
private:
  int write_row(const uchar *buf) override;
  int update_row(const uchar *old_data, const uchar *new_data) override;
  int delete_row(const uchar *buf) override;
};
