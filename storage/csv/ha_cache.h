#ifndef HA_CACHE_INCLUDED
#define HA_CACHE3_INCLUDED
/* Copyright (C) 2020 MariaDB Corppration AB

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

#include "ha_tina.h"
#include "../maria/ha_maria.h"
#include <thr_lock.h>

class ha_cache_share
{
  ha_cache_share *next;                         /* Next open share */
  const char *name;
  uint open_count;
public:
  THR_LOCK org_lock;
  friend ha_cache_share *find_cache_share(const char *name);
  void close();
};


class ha_cache :public ha_tina
{
  typedef ha_tina parent;
  int original_lock_type;
  bool insert_command;

public:
  uint lock_counter;
  ha_maria *cache_handler;
  ha_cache_share *share;

  ha_cache(handlerton *hton, TABLE_SHARE *table_arg, MEM_ROOT *mem_root);
  ~ha_cache();

  /*
    The following functions duplicates calls to derived handler and
    cache handler
  */

  int create(const char *name, TABLE *table_arg,
             HA_CREATE_INFO *ha_create_info);
  int open(const char *name, int mode, uint open_flags);
  int delete_table(const char *name);
  int rename_table(const char *from, const char *to);
  int delete_all_rows(void);
  int close(void);

  uint lock_count(void) const;
  THR_LOCK_DATA **store_lock(THD *thd,
                             THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);
  int external_lock(THD *thd, int lock_type);
  int repair(THD *thd, HA_CHECK_OPT *check_opt);
  bool is_crashed() const;

  /*
    Write row uses cache_handler, for normal inserts,  otherwise derived
    handler
  */
  int write_row(const uchar *buf);
  void start_bulk_insert(ha_rows rows, uint flags);
  int end_bulk_insert();

  /* Cache functions */
  void free_locks();
  bool rows_cached();
  int flush_insert_cache();
  friend my_bool get_status_and_flush_cache(void *param,
                                            my_bool concurrent_insert);
};
#endif /* HA_S3_INCLUDED */
