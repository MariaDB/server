#ifndef HA_MARIA_INCLUDED
#define HA_MARIA_INCLUDED
/* Copyright (C) 2006, 2004 MySQL AB & MySQL Finland AB & TCX DataKonsult AB
   Copyright (c) 2009, 2020, MariaDB Corporation Ab

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335 USA */

#ifdef USE_PRAGMA_INTERFACE
#pragma interface                               /* gcc class implementation */
#endif

/* class for the maria handler */

#include "maria_def.h"
#include "handler.h"
#include "table.h"

#define HA_RECOVER_NONE         0       /* No automatic recover */
#define HA_RECOVER_DEFAULT      1       /* Automatic recover active */
#define HA_RECOVER_BACKUP       2       /* Make a backupfile on recover */
#define HA_RECOVER_FORCE        4       /* Recover even if we loose rows */
#define HA_RECOVER_QUICK        8       /* Don't check rows in data file */

C_MODE_START
check_result_t index_cond_func_maria(void *arg);
C_MODE_END

extern TYPELIB maria_recover_typelib;
extern ulonglong maria_recover_options;

/*
  In the ha_maria class there are a few virtual methods that are not marked as
  'final'. This is because they are re-defined by the ha_s3 engine.
*/

class __attribute__((visibility("default"))) ha_maria :public handler
{
public:
  MARIA_HA *file;
private:
  ulonglong int_table_flags;
  MARIA_RECORD_POS remember_pos;
  char *data_file_name, *index_file_name;
  enum data_file_type data_file_type;
  bool can_enable_indexes;
  /**
    If a transactional table is doing bulk insert with a single
    UNDO_BULK_INSERT with/without repair.
  */
  uint8 bulk_insert_single_undo;
  int repair(THD * thd, HA_CHECK *param, bool optimize);
  int zerofill(THD * thd, HA_CHECK_OPT *check_opt);

public:
  ha_maria(handlerton *hton, TABLE_SHARE * table_arg);
  ~ha_maria() {}
  handler *clone(const char *name, MEM_ROOT *mem_root) override final;
  const char *index_type(uint key_number) override final;
  ulonglong table_flags() const override final
  { return int_table_flags; }
  ulong index_flags(uint inx, uint part, bool all_parts) const override final;
  uint max_supported_keys() const override final
  { return MARIA_MAX_KEY; }
  uint max_supported_key_length() const override final;
  uint max_supported_key_part_length() const override final
  { return max_supported_key_length(); }
  enum row_type get_row_type() const override final;
  void change_table_ptr(TABLE *table_arg, TABLE_SHARE *share) override final;
  virtual double scan_time() override final;

  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override final;
  int write_row(const uchar * buf) override;
  int update_row(const uchar * old_data, const uchar * new_data) override;
  int delete_row(const uchar * buf) override;
  int index_read_map(uchar * buf, const uchar * key, key_part_map keypart_map,
		     enum ha_rkey_function find_flag) override final;
  int index_read_idx_map(uchar * buf, uint idx, const uchar * key,
			 key_part_map keypart_map,
			 enum ha_rkey_function find_flag) override final;
  int index_read_last_map(uchar * buf, const uchar * key,
			  key_part_map keypart_map) override final;
  int index_next(uchar * buf) override final;
  int index_prev(uchar * buf) override final;
  int index_first(uchar * buf) override final;
  int index_last(uchar * buf) override final;
  int index_next_same(uchar * buf, const uchar * key, uint keylen) override final;
  int ft_init() override final
  {
    if (!ft_handler)
      return 1;
    ft_handler->please->reinit_search(ft_handler);
    return 0;
  }
  FT_INFO *ft_init_ext(uint flags, uint inx, String * key) override final;
  int ft_read(uchar * buf) override final;
  int index_init(uint idx, bool sorted) override final;
  int index_end() override final;
  int rnd_init(bool scan) override final;
  int rnd_end(void) override final;
  int rnd_next(uchar * buf) override final;
  int rnd_pos(uchar * buf, uchar * pos) override final;
  int remember_rnd_pos() override final;
  int restart_rnd_next(uchar * buf) override final;
  void position(const uchar * record) override final;
  int info(uint) override final;
  int info(uint, my_bool);
  int extra(enum ha_extra_function operation) override final;
  int extra_opt(enum ha_extra_function operation, ulong cache_size) override final;
  int reset(void) override final;
  int external_lock(THD * thd, int lock_type) override;
  int start_stmt(THD *thd, thr_lock_type lock_type) override final;
  int delete_all_rows(void) override final;
  int disable_indexes(uint mode) override final;
  int enable_indexes(uint mode) override final;
  int indexes_are_disabled(void) override final;
  void start_bulk_insert(ha_rows rows, uint flags) override final;
  int end_bulk_insert() override final;
  ha_rows records_in_range(uint inx, const key_range *min_key,
                           const key_range *max_key,
                           page_range *pages) override final;
  void update_create_info(HA_CREATE_INFO * create_info) override final;
  int create(const char *name, TABLE * form, HA_CREATE_INFO * create_info) override;
  THR_LOCK_DATA **store_lock(THD * thd, THR_LOCK_DATA ** to,
                             enum thr_lock_type lock_type) override final;
  virtual void get_auto_increment(ulonglong offset, ulonglong increment,
                                  ulonglong nb_desired_values,
                                  ulonglong *first_value,
                                  ulonglong *nb_reserved_values) override final;
  int rename_table(const char *from, const char *to) override;
  int delete_table(const char *name) override;
  void drop_table(const char *name) override;
  int check(THD * thd, HA_CHECK_OPT * check_opt) override;
  int analyze(THD * thd, HA_CHECK_OPT * check_opt) override;
  int repair(THD * thd, HA_CHECK_OPT * check_opt) override;
  int check_for_upgrade(HA_CHECK_OPT *check_opt) override;
  bool check_and_repair(THD * thd) override final;
  bool is_crashed() const override final;
  bool is_changed() const;
  bool auto_repair(int error) const override final;
  int optimize(THD * thd, HA_CHECK_OPT * check_opt) override final;
  int assign_to_keycache(THD * thd, HA_CHECK_OPT * check_opt) override final;
  int preload_keys(THD * thd, HA_CHECK_OPT * check_opt) override;
  bool check_if_incompatible_data(HA_CREATE_INFO * info, uint table_changes) override final;
#ifdef HAVE_QUERY_CACHE
  my_bool register_query_cache_table(THD *thd, const char *table_key,
                                     uint key_length,
                                     qc_engine_callback
                                     *engine_callback,
                                     ulonglong *engine_data) override final;
#endif
  MARIA_HA *file_ptr(void)
  {
    return file;
  }
  static bool has_active_transaction(THD *thd);
  static int implicit_commit(THD *thd, bool new_trn);
  /**
   * Multi Range Read interface
   */
  int multi_range_read_init(RANGE_SEQ_IF *seq, void *seq_init_param,
                            uint n_ranges, uint mode, HANDLER_BUFFER *buf) override final;
  int multi_range_read_next(range_id_t *range_info) override final;
  ha_rows multi_range_read_info_const(uint keyno, RANGE_SEQ_IF *seq,
                                      void *seq_init_param,
                                      uint n_ranges, uint *bufsz,
                                      uint *flags, Cost_estimate *cost) override final;
  ha_rows multi_range_read_info(uint keyno, uint n_ranges, uint keys,
                                uint key_parts, uint *bufsz,
                                uint *flags, Cost_estimate *cost) override final;
  int multi_range_read_explain_info(uint mrr_mode, char *str, size_t size) override final;

  /* Index condition pushdown implementation */
  Item *idx_cond_push(uint keyno, Item* idx_cond) override final;

  int find_unique_row(uchar *record, uint unique_idx) override final;

  /* Following functions are needed by the S3 handler */
  virtual S3_INFO *s3_open_args() { return 0; }
  virtual void register_handler(MARIA_HA *file) {}

private:
  DsMrr_impl ds_mrr;
  friend check_result_t index_cond_func_maria(void *arg);
  friend void reset_thd_trn(THD *thd);
  friend class ha_s3;
};

#endif /* HA_MARIA_INCLUDED */
