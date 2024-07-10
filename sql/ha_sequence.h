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
  /* Set when handler is write locked */
  bool write_locked;

  ha_sequence(handlerton *hton, TABLE_SHARE *share);
  ~ha_sequence();

  /* virtual function that are re-implemented for sequence */
  int open(const char *name, int mode, uint test_if_locked) override;
  int create(const char *name, TABLE *form,
             HA_CREATE_INFO *create_info) override;
  handler *clone(const char *name, MEM_ROOT *mem_root) override;
  int write_row(const uchar *buf) override;
  Table_flags table_flags() const override;
  /* One can't update or delete from sequence engine */
  int update_row(const uchar *old_data, const uchar *new_data) override
  { return HA_ERR_WRONG_COMMAND; }
  int delete_row(const uchar *buf) override
  { return HA_ERR_WRONG_COMMAND; }
  /* One can't delete from sequence engine */
  int truncate() override
  { return HA_ERR_WRONG_COMMAND; }
  /* Can't use query cache */
  uint8 table_cache_type() override
  { return HA_CACHE_TBL_NOCACHE; }
  void print_error(int error, myf errflag) override;
  int info(uint) override;
  LEX_CSTRING *engine_name() override { return hton_name(file->ht); }
  int external_lock(THD *thd, int lock_type) override;
  int extra(enum ha_extra_function operation) override;
  /* For ALTER ONLINE TABLE */
  bool check_if_incompatible_data(HA_CREATE_INFO *create_info,
                                  uint table_changes) override;
  void write_lock() { write_locked= 1;}
  void unlock() { write_locked= 0; }
  bool is_locked() { return write_locked; }

  /* Functions that are directly mapped to the underlying handler */
  int rnd_init(bool scan) override
  { return file->rnd_init(scan); }
  /*
    We need to have a lock here to protect engines like MyISAM from
    simultaneous read and write. For sequence's this is not critical
    as this function is used extremely seldom.
  */
  int rnd_next(uchar *buf) override
  {
    int error;
    table->s->sequence->read_lock(table);
    error= file->rnd_next(buf);
    table->s->sequence->read_unlock(table);
    return error;
  }
  int rnd_end() override
  { return file->rnd_end(); }
  int rnd_pos(uchar *buf, uchar *pos) override
  {
    int error;
    table->s->sequence->read_lock(table);
    error= file->rnd_pos(buf, pos);
    table->s->sequence->read_unlock(table);
    return error;
  }
  void position(const uchar *record) override
  { return file->position(record); }
  const char *table_type() const override
  { return file->table_type(); }
  ulong index_flags(uint inx, uint part, bool all_parts) const override
  { return file->index_flags(inx, part, all_parts); }
  THR_LOCK_DATA **store_lock(THD *thd, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override
  { return file->store_lock(thd, to, lock_type); }
  int close(void) override
  { return file->close(); }
  const char **bas_ext() const
  { return file->bas_ext(); }
  int delete_table(const char*name) override
  { return file->delete_table(name); }
  int rename_table(const char *from, const char *to) override
  { return file->rename_table(from, to); }
  void unbind_psi() override
  { file->unbind_psi(); }
  void rebind_psi() override
  { file->rebind_psi(); }

  bool auto_repair(int error) const override
  { return file->auto_repair(error); }
  int repair(THD* thd, HA_CHECK_OPT* check_opt) override
  { return file->repair(thd, check_opt); }
  bool check_and_repair(THD *thd) override
  { return file->check_and_repair(thd); }
  bool is_crashed() const override
  { return file->is_crashed(); }
  void column_bitmaps_signal() override
  { return file->column_bitmaps_signal(); }

  /* New methods */
  void register_original_handler(handler *file_arg)
  {
    file= file_arg;
    init();                                     /* Update cached_table_flags */
  }
};
#endif
