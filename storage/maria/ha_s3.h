#ifndef HA_S3_INCLUDED
#define HA_S3_INCLUDED
/* Copyright (C) 2019 MariaDB Corppration AB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the
   Free Software Foundation, Inc.
   51 Franklin Street, Fifth Floor, Boston, MA 02111-1301 USA
*/

#include "ha_maria.h"

class ha_s3 :public ha_maria
{
  enum alter_table_op
  { S3_NO_ALTER, S3_ALTER_TABLE, S3_ADD_PARTITION, S3_ADD_TMP_PARTITION };
  alter_table_op in_alter_table;
  S3_INFO *open_args;

public:
  ha_s3(handlerton *hton, TABLE_SHARE * table_arg);
  ~ha_s3() {}

  int create(const char *name, TABLE *table_arg, HA_CREATE_INFO *ha_create_info);
  int open(const char *name, int mode, uint open_flags);
  int write_row(const uchar *buf);
  int update_row(const uchar * old_data, const uchar * new_data)
  {
    DBUG_ENTER("update_row");
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  int delete_row(const uchar * buf)
  {
    DBUG_ENTER("delete_row");
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  int check(THD * thd, HA_CHECK_OPT * check_opt)
  {
    DBUG_ENTER("delete_row");
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  int analyze(THD * thd, HA_CHECK_OPT * check_opt)
  {
    DBUG_ENTER("analyze");
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  int repair(THD * thd, HA_CHECK_OPT * check_opt)
  {
    DBUG_ENTER("repair");
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  int preload_keys(THD * thd, HA_CHECK_OPT * check_opt)
  {
    DBUG_ENTER("preload_keys");
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  }
  int external_lock(THD * thd, int lock_type);
  /*
    drop_table() is only used for internal temporary tables,
    not applicable for s3
  */
  void drop_table(const char *name)
  {
  }
  int delete_table(const char *name);
  int rename_table(const char *from, const char *to);
  int discover_check_version() override;
  int rebind();
  S3_INFO *s3_open_args() { return open_args; }
  void register_handler(MARIA_HA *file);
};
#endif /* HA_S3_INCLUDED */
