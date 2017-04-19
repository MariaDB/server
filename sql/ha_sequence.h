#ifndef HA_SEQUENCE_INCLUDED
#define HA_SEQUENCE_INCLUDED
/*
   Copyright (c) 2017 Aliyun and/or its affiliates.
   Copyright (c) 2017 MariaDB corporation

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

#include "sql_sequence.h"
#include "table.h"
#include "handler.h"

extern handlerton *sql_sequence_hton;

/*
  Sequence engine handler.

  The sequence engine is a logic engine. It doesn't store any data.
  All the sequence data stored into the base table which must support
  non rollback writes (HA_CAN_TABLES_WITHOUT_ROLLBACK)

  The sequence data (SEQUENCE class) is stored in TABLE_SHARE->sequence

  TABLE RULES:
      1. When table is created, one row is automaticlly inserted into
         the table. The table will always have one and only one row.
      2. Any inserts or updates to the table will be validated.
      3. Inserts will overwrite the original row.
      4. DELETE and TRUNCATE will not affect the table.
         Instead a warning will be given.
      5. Cache will be reset for any updates.

  CACHE RULES:
    SEQUENCE class is used to cache values that sequence defined.
      1. If hit cache, we can query back the sequence nextval directly
         instead of reading the underlying table.

      2. When run out of values, the sequence engine will reserve new values
         in update the base table.

      3. The cache is invalidated if any update on based table.
*/

class ha_sequence :public handler
{
private:
  handler *file;
  SEQUENCE *sequence;                     /* From table_share->sequence */

public:
  ha_sequence(handlerton *hton, TABLE_SHARE *share);
  ~ha_sequence();

  /* virtual function that are re-implemented for sequence */
  int open(const char *name, int mode, uint test_if_locked);
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info);
  handler *clone(const char *name, MEM_ROOT *mem_root);
  int write_row(uchar *buf);
  int update_row(const uchar *old_data, const uchar *new_data);
  Table_flags table_flags() const;
  /* One can't delete from sequence engine */
  int delete_row(const uchar *buf)
  { return HA_ERR_WRONG_COMMAND; }
  /* One can't delete from sequence engine */
  int truncate()
  { return HA_ERR_WRONG_COMMAND; }
  /* Can't use query cache */
  uint8 table_cache_type()
  { return HA_CACHE_TBL_NOCACHE; }
  void print_error(int error, myf errflag);
  int info(uint);
  LEX_STRING *engine_name() { return hton_name(file->ht); }
  int external_lock(THD *thd, int lock_type);

  /* Functions that are directly mapped to the underlying handler */
  int rnd_init(bool scan)
  { return file->rnd_init(scan); }
  int rnd_next(uchar *buf)
  { return file->rnd_next(buf); }
  int rnd_end()
  { return file->rnd_end(); }
  int rnd_pos(uchar *buf, uchar *pos)
  { return file->rnd_pos(buf, pos); }
  void position(const uchar *record)
  { return file->position(record); }
  const char *table_type() const
  { return file->table_type(); }
  ulong index_flags(uint inx, uint part, bool all_parts) const
  { return file->index_flags(inx, part, all_parts); }
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type)
  { return file->store_lock(thd, to, lock_type); }
  int close(void)
  { return file->close(); }
  const char **bas_ext() const
  { return file->bas_ext(); }
  int delete_table(const char*name)
  { return file->delete_table(name); }
  int rename_table(const char *from, const char *to)
  { return file->rename_table(from, to); }
  void unbind_psi()
  { return file->unbind_psi(); }
  void rebind_psi()
  { return file->rebind_psi(); }

  bool auto_repair(int error) const
  { return file->auto_repair(error); }
  int repair(THD* thd, HA_CHECK_OPT* check_opt)
  { return file->repair(thd, check_opt); }
  bool check_and_repair(THD *thd)
  { return file->check_and_repair(thd); }
  bool is_crashed() const
  { return file->is_crashed(); }

  /* New methods */
  void register_original_handler(handler *file_arg)
  {
    file= file_arg;
    init();                                     /* Update cached_table_flags */
  }

  /* To inform handler that sequence is already locked by called */
  bool sequence_locked;
};
#endif
