/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/

#ifndef _ha_clustrixdb_h
#define _ha_clustrixdb_h

#ifdef USE_PRAGMA_INTERFACE
#pragma interface     /* gcc class implementation */
#endif

#define MYSQL_SERVER 1
#include "clustrix_connection.h"
#include "my_bitmap.h"
#include "table.h"
#include "rpl_rli.h"
#include "handler.h"
#include "sql_class.h"
#include "sql_show.h"
#include "mysql.h"
#include "../../sql/rpl_record.h"

size_t estimate_row_size(TABLE *table);
clustrix_connection *get_trx(THD *thd, int *error_code);
bool get_enable_sh(THD* thd);

class ha_clustrixdb : public handler
{
private:
# define CLUSTRIXDB_ROW_LIMIT 1024

  ulonglong clustrix_table_oid;
  rpl_group_info *rgi;
  Relay_log_info *rli;

  Field *auto_inc_field;
  ulonglong auto_inc_value;

  bool has_hidden_key;
  ulonglong last_hidden_key;
  ulonglong scan_refid;
  bool is_scan;
  MY_BITMAP scan_fields;
  bool sorted_scan;

public:
  ha_clustrixdb(handlerton *hton, TABLE_SHARE *table_arg);
  ~ha_clustrixdb();
  int create(const char *name, TABLE *form, HA_CREATE_INFO *info);
  int delete_table(const char *name);
  int rename_table(const char* from, const char* to);
  int open(const char *name, int mode, uint test_if_locked);
  int close(void);
  int reset();
  int write_row(const uchar *buf);
  // start_bulk_update exec_bulk_update
  int update_row(const uchar *old_data, const uchar *new_data);
  // start_bulk_delete exec_bulk_delete
  int delete_row(const uchar *buf);

  Table_flags table_flags(void) const;
  ulong index_flags(uint idx, uint part, bool all_parts) const;
  uint max_supported_keys() const { return MAX_KEY; }

  ha_rows records();
  ha_rows records_in_range(uint inx, key_range *min_key,
                           key_range *max_key);

  int info(uint flag); // see my_base.h for full description

  // multi_read_range
  // read_range
  int index_init(uint idx, bool sorted);
  int index_read(uchar * buf, const uchar * key, uint key_len,
                 enum ha_rkey_function find_flag);
  int index_first(uchar *buf);
  int index_prev(uchar *buf);
  int index_last(uchar *buf);
  int index_next(uchar *buf);
  //int index_next_same(uchar *buf, const uchar *key, uint keylen);
  int index_end();

  int rnd_init(bool scan);
  int rnd_next(uchar *buf);
  int rnd_pos(uchar * buf, uchar *pos);
  int rnd_end();

  void position(const uchar *record);
  uint lock_count(void) const;
  THR_LOCK_DATA **store_lock(THD *thd,
                             THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type);
  int external_lock(THD *thd, int lock_type);

  uint8 table_cache_type()
  {
    return(HA_CACHE_TBL_NOCACHE);
  }

  const COND *cond_push(const COND *cond);
  void cond_pop();
  int info_push(uint info_type, void *info);

private:
  void add_current_table_to_rpl_table_list();
  void remove_current_table_from_rpl_table_list();
  void build_key_packed_row(uint index, const uchar *buf,
                            uchar *packed_key, size_t *packed_key_len);
};

bool select_handler_setting(THD* thd);
bool derived_handler_setting(THD* thd);
#endif  // _ha_clustrixdb_h
