/*****************************************************************************
Copyright (c) 2019, 2020, MariaDB Corporation.
*****************************************************************************/

#ifndef _ha_xpand_h
#define _ha_xpand_h

#ifdef USE_PRAGMA_INTERFACE
#pragma interface     /* gcc class implementation */
#endif

#define MYSQL_SERVER 1
#include "xpand_connection.h"
#include "my_bitmap.h"
#include "table.h"
#include "rpl_rli.h"
#include "handler.h"
#include "sql_class.h"
#include "sql_show.h"
#include "mysql.h"
#include "../../sql/rpl_record.h"

size_t estimate_row_size(TABLE *table);
xpand_connection *get_trx(THD *thd, int *error_code);
bool get_enable_sh(THD* thd);
void add_current_table_to_rpl_table_list(rpl_group_info **_rgi, THD *thd,
                                         TABLE *table);
void remove_current_table_from_rpl_table_list(rpl_group_info *rgi);
int unpack_row_to_buf(rpl_group_info *rgi, TABLE *table, uchar *data,
                      uchar const *const row_data, MY_BITMAP const *cols,
                      uchar const *const row_end);
void xpand_mark_tables_for_discovery(LEX *lex);
ulonglong *xpand_extract_table_oids(THD *thd, LEX *lex);


class Xpand_share : public Handler_share {
public:
  Xpand_share(): xpand_table_oid(0), rediscover_table(false) {}

  std::atomic<ulonglong> xpand_table_oid;
  std::atomic<bool> rediscover_table;
};

class ha_xpand : public handler
{
private:
  // TODO: do we need this here or one in share would be sufficient?
  ulonglong xpand_table_oid;
  rpl_group_info *rgi;

  Field *auto_inc_field;
  ulonglong auto_inc_value;

  bool has_hidden_key;
  ulonglong last_hidden_key;
  xpand_connection_cursor *scan_cur;
  bool is_scan;
  MY_BITMAP scan_fields;
  bool sorted_scan;
  xpand_lock_mode_t xpd_lock_type;

  uint last_dup_errkey;

  typedef enum xpand_upsert_flags {
    XPAND_HAS_UPSERT= 1,
    XPAND_BULK_UPSERT= 2,
    XPAND_UPSERT_SENT= 4
  } xpd_upsert_flags_t;
  int upsert_flag;

  Xpand_share *get_share(); ///< Get the share

public:
  ha_xpand(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_xpand();
  int create(const char *name, TABLE *form, HA_CREATE_INFO *info) override;
  int delete_table(const char *name) override;
  int rename_table(const char* from, const char* to) override;
  int open(const char *name, int mode, uint test_if_locked) override;
  int close(void) override;
  int reset() override;
  int extra(enum ha_extra_function operation) override;
  int write_row(const uchar *buf) override;
  // start_bulk_update exec_bulk_update
  int update_row(const uchar *old_data, const uchar *new_data) override;
  // start_bulk_delete exec_bulk_delete
  int delete_row(const uchar *buf) override;
  int direct_update_rows_init(List<Item> *update_fields) override;
  int direct_update_rows(ha_rows *update_rows, ha_rows *found_rows) override;
  void start_bulk_insert(ha_rows rows, uint flags = 0) override;
  int end_bulk_insert() override;

  Table_flags table_flags(void) const override;
  ulong index_flags(uint idx, uint part, bool all_parts) const override;
  uint max_supported_keys() const override { return MAX_KEY; }

  ha_rows records() override;
  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key) override;

  int info(uint flag) override; // see my_base.h for full description

  // multi_read_range
  // read_range
  int index_init(uint idx, bool sorted) override;
  int index_read(uchar * buf, const uchar * key, uint key_len,
                 enum ha_rkey_function find_flag) override;
  int index_first(uchar *buf) override;
  int index_prev(uchar *buf) override;
  int index_last(uchar *buf) override;
  int index_next(uchar *buf) override;
  //int index_next_same(uchar *buf, const uchar *key, uint keylen) override;
  int index_end() override;

  int rnd_init(bool scan) override;
  int rnd_next(uchar *buf) override;
  int rnd_pos(uchar * buf, uchar *pos) override;
  int rnd_end() override;

  void position(const uchar *record) override;
  uint lock_count(void) const override;
  THR_LOCK_DATA **store_lock(THD *thd,
                             THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override;
  int external_lock(THD *thd, int lock_type) override;

  uint8 table_cache_type() override
  {
    return(HA_CACHE_TBL_NOCACHE);
  }

  const COND *cond_push(const COND *cond) override;
  void cond_pop() override;
  int info_push(uint info_type, void *info) override;

  ulonglong get_table_oid();
private:
  void build_key_packed_row(uint index, const uchar *buf,
                            uchar *packed_key, size_t *packed_key_len);
};

bool select_handler_setting(THD* thd);
bool derived_handler_setting(THD* thd);
uint row_buffer_setting(THD* thd);
#endif  // _ha_xpand_h
