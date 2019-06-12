/*****************************************************************************
Copyright (c) 2019, MariaDB Corporation.
*****************************************************************************/

/** @file ha_clustrixdb.cc */

#include "ha_clustrixdb.h"
#include "key.h"

handlerton *clustrixdb_hton = NULL;

int clustrix_connect_timeout;
static MYSQL_SYSVAR_INT
(
  connect_timeout,
  clustrix_connect_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Timeout for connecting to Clustrix",
  NULL, NULL, -1, -1, 2147483647, 0
);

int clustrix_read_timeout;
static MYSQL_SYSVAR_INT
(
  read_timeout,
  clustrix_read_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Timeout for receiving data from Clustrix",
  NULL, NULL, -1, -1, 2147483647, 0
);

int clustrix_write_timeout;
static MYSQL_SYSVAR_INT
(
  write_timeout,
  clustrix_write_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Timeout for sending data to Clustrix",
  NULL, NULL, -1, -1, 2147483647, 0
);

char *clustrix_host;
static MYSQL_SYSVAR_STR
(
  host,
  clustrix_host,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "Clustrix host",
  NULL, NULL, "127.0.0.1"
);

char *clustrix_username;
static MYSQL_SYSVAR_STR
(
  username,
  clustrix_username,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "Clustrix user name",
  NULL, NULL, "root"
);

char *clustrix_password;
static MYSQL_SYSVAR_STR
(
  password,
  clustrix_password,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "Clustrix password",
  NULL, NULL, ""
);

uint clustrix_port;
static MYSQL_SYSVAR_UINT
(
  port,
  clustrix_port,
  PLUGIN_VAR_RQCMDARG,
  "Clustrix port",
  NULL, NULL, MYSQL_PORT_DEFAULT, MYSQL_PORT_DEFAULT, 65535, 0
);

char *clustrix_socket;
static MYSQL_SYSVAR_STR
(
  socket,
  clustrix_socket,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "Clustrix socket",
  NULL, NULL, ""
);

/****************************************************************************
** Class ha_clustrixdb_trx
****************************************************************************/

st_clustrixdb_trx::st_clustrixdb_trx(THD *trx_thd)
{
  thd = trx_thd;
  clustrix_net = NULL;
  //query_id = 0;
  //mem_root = NULL;
  has_transaction = FALSE;
}

st_clustrixdb_trx::~st_clustrixdb_trx()
{
  if (clustrix_net)
    delete clustrix_net;
}


int st_clustrixdb_trx::net_init()
{
  if (!this->clustrix_net)
  {
    this->clustrix_net = new clustrix_connection();
    int error_code = this->clustrix_net->connect();
    if (error_code)
      return error_code;
  }

  return 0;
}

int st_clustrixdb_trx::begin_trans()
{
  // XXX: What were these for?
  //if (thd->transaction.stmt.trans_did_ddl() ||
  //  thd->transaction.stmt.modified_non_trans_table)

  if (!has_transaction) {
    int error_code = this->clustrix_net->begin_trans();
    if (error_code)
      return error_code;

    /* Register for commit/rollback on the transaction */
    if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
      trans_register_ha(thd, FALSE, clustrixdb_hton);
    else
      trans_register_ha(thd, TRUE, clustrixdb_hton);

    has_transaction = TRUE;
  }

  return 0;
}

/****************************************************************************
** Utility functions
****************************************************************************/
// This is a wastefull aproach but better then fixed sized buffer.RALLEL
size_t estimate_row_size(TABLE *table)
{
  size_t row_size = 0;
  size_t null_byte_count = (bitmap_bits_set(table->write_set) + 7) / 8;
  row_size += null_byte_count;
  Field **p_field= table->field, *field;
  for ( ; (field= *p_field) ; p_field++) {
    row_size += field->max_data_length();
  }
  return row_size;
}

/****************************************************************************
** Class ha_clustrixdb
****************************************************************************/

ha_clustrixdb::ha_clustrixdb(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg)
{
  DBUG_ENTER("ha_clustrixdb::ha_clustrixdb");
  rli = NULL;
  rgi = NULL;
  scan_refid = 0;
  clustrix_table_oid = 0;
  DBUG_VOID_RETURN;
}

ha_clustrixdb::~ha_clustrixdb()
{
  if (rli)
    ha_clustrixdb::remove_current_table_from_rpl_table_list();
}

int ha_clustrixdb::create(const char *name, TABLE *form, HA_CREATE_INFO *info)
{
  int error_code;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  enum tmp_table_type saved_tmp_table_type = form->s->tmp_table;
  Table_specification_st *create_info = &thd->lex->create_info;
  const bool is_tmp_table = info->options & HA_LEX_CREATE_TMP_TABLE;
  String create_table_stmt;

  /* Create a copy of the CREATE TABLE statement */
  if (!is_tmp_table)
    form->s->tmp_table = NO_TMP_TABLE;
  const char *old_dbstr = thd->db.str;
  thd->db.str = NULL;
  ulong old = create_info->used_fields;
  create_info->used_fields &= ~HA_CREATE_USED_ENGINE;

  TABLE_LIST table_list;
  memset(&table_list, 0, sizeof(table_list));
  table_list.table = form;
  error_code = show_create_table(thd, &table_list, &create_table_stmt,
                                 create_info, WITH_DB_NAME);

  if (!is_tmp_table)
    form->s->tmp_table = saved_tmp_table_type;
  create_info->used_fields = old;
  thd->db.str = old_dbstr;
  if (error_code)
    return error_code;

  error_code = trx->clustrix_net->create_table(create_table_stmt);
  return error_code;
}

int ha_clustrixdb::delete_table(const char *name)
{
  int error_code;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  // This block isn't UTF aware yet.
  // The format contains './' in the beginning of a path.
  char *ptr = (char*) name + 2;
  while (*ptr != '/')
  {
    ptr++;
  }
  *ptr = '\0';
  String db_name;
  db_name.append(name + 2);
  *ptr = '/';
  ptr++;
  String tbl_name;
  tbl_name.append(ptr);

  String delete_cmd;
  delete_cmd.append("DROP TABLE `");
  delete_cmd.append(db_name);
  delete_cmd.append("`.`");
  delete_cmd.append(tbl_name);
  delete_cmd.append("`");

  return trx->clustrix_net->delete_table(delete_cmd);
}

int ha_clustrixdb::open(const char *name, int mode, uint test_if_locked)
{
  DBUG_ENTER("ha_clustrixdb::open");
  DBUG_PRINT("oid",
             ("%s", table->s->tabledef_version.str));

  if (!table->s->tabledef_version.str)
    return HA_ERR_TABLE_DEF_CHANGED;
  if (!clustrix_table_oid)
    clustrix_table_oid = atoll((const char *)table->s->tabledef_version.str);

  has_hidden_key = table->s->primary_key == MAX_KEY;
  if (has_hidden_key) {
    ref_length = 8;
  } else {
    KEY* key_info = table->key_info + table->s->primary_key;
    ref_length = key_info->key_length;
  }

  DBUG_PRINT("open finished",
             ("oid: %llu, ref_length: %u", clustrix_table_oid, ref_length));
  DBUG_RETURN(0);
}

int ha_clustrixdb::close(void)
{
  return 0;
}

int ha_clustrixdb::reset()
{
  return 0;
}

int ha_clustrixdb::write_row(uchar *buf)
{
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  assert(trx->has_transaction);

  /* Convert the row format to binlog (packed) format */
  uchar *packed_new_row = (uchar*) my_alloca(estimate_row_size(table));
  size_t packed_size = pack_row(table, table->write_set, packed_new_row, buf);

  /* XXX: Clustrix may needs to return HA_ERR_AUTOINC_ERANGE if we hit that
     error. */
  if ((error_code = trx->clustrix_net->write_row(clustrix_table_oid,
                                                 packed_new_row, packed_size)))
    goto err;

  {
    Field *auto_inc_field = table->next_number_field;
    if (auto_inc_field)
      insert_id_for_cur_row = trx->clustrix_net->last_insert_id;
  }

err:
    if (packed_size)
      my_afree(packed_new_row);

    return error_code;
}

int ha_clustrixdb::update_row(const uchar *old_data, const uchar *new_data)
{
  int error_code;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  assert(trx->has_transaction);

  size_t row_size = estimate_row_size(table);
  uchar *packed_new_row = (uchar*) my_alloca(row_size);
  // Add checks for actual size of the packed data
  /*size_t packed_new_size =*/ pack_row(table, table->write_set, packed_new_row, new_data);
  uchar *packed_old_row = (uchar*) my_alloca(row_size);
  /*size_t packed_old_size =*/ pack_row(table, table->write_set, packed_old_row, old_data);

  /* Send the packed rows to Clustrix */

  if(packed_new_row)
    my_afree(packed_new_row);

  if(packed_old_row)
    my_afree(packed_old_row);

  return error_code;
}

int ha_clustrixdb::delete_row(const uchar *buf)
{
  int error_code;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  assert(trx->has_transaction);

  // The estimate should consider only key fields widths.
  size_t packed_key_len;
  uchar *packed_key = (uchar*) my_alloca(estimate_row_size(table));
  build_key_packed_row(table->s->primary_key, packed_key, &packed_key_len);

  if ((error_code = trx->clustrix_net->key_delete(clustrix_table_oid,
                                                  packed_key, packed_key_len)))
    goto err;

err:
    if (packed_key)
      my_afree(packed_key);

  return error_code;
}

ha_clustrixdb::Table_flags ha_clustrixdb::table_flags(void) const
{
  Table_flags flags = HA_PARTIAL_COLUMN_READ |
                      HA_REC_NOT_IN_SEQ |
                      HA_FAST_KEY_READ |
                      HA_NULL_IN_KEY |
                      HA_CAN_INDEX_BLOBS |
                      HA_AUTO_PART_KEY |
                      HA_CAN_SQL_HANDLER |
                      HA_BINLOG_ROW_CAPABLE |
                      HA_BINLOG_STMT_CAPABLE |
                      HA_CAN_TABLE_CONDITION_PUSHDOWN;

  return flags;
}

ulong ha_clustrixdb::index_flags(uint idx, uint part, bool all_parts) const
{
  ulong flags = HA_READ_NEXT |
                HA_READ_PREV |
                HA_READ_ORDER;

  return flags;
}

ha_rows ha_clustrixdb::records()
{
  return 10000;
}

ha_rows ha_clustrixdb::records_in_range(uint inx, key_range *min_key,
                                        key_range *max_key)
{
  return 2;
}

int ha_clustrixdb::info(uint flag)
{
  //THD *thd = ha_thd();
  if (flag & HA_STATUS_TIME)
  {
    /* Retrieve the time of the most recent update to the table */
    // stats.update_time =
  }

  if (flag & HA_STATUS_AUTO)
  {
    /* Retrieve the latest auto_increment value */
    stats.auto_increment_value = next_insert_id;
  }

  if (flag & HA_STATUS_VARIABLE)
  {
    /* Retrieve variable info, such as row counts and file lengths */
    stats.records = records();
    stats.deleted = 0;
    // stats.data_file_length =
    // stats.index_file_length =
    // stats.delete_length =
    stats.check_time = 0;
    // stats.mrr_length_per_rec =

    if (stats.records == 0)
      stats.mean_rec_length = 0;
    else
      stats.mean_rec_length = (ulong)
                              (stats.data_file_length / stats.records);
  }

  if (flag & HA_STATUS_CONST)
  {
    /*
      Retrieve constant info, such as file names, max file lengths,
      create time, block size
    */
    // stats.max_data_file_length =
    // stats.create_time =
    // stats.block_size =
  }

  return 0;
}

int ha_clustrixdb::index_init(uint idx, bool sorted)
{
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  active_index = idx;
  add_current_table_to_rpl_table_list();
  scan_refid = 0;

  return 0;

}

int ha_clustrixdb::index_read(uchar * buf, const uchar * key, uint key_len,
                              enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_clustrixdb::index_read");
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  is_scan = false;
  key_restore(table->record[0], key, &table->key_info[active_index], key_len);
  // The estimate should consider only key fields widths.
  size_t packed_key_len;
  uchar *packed_key = (uchar*) my_alloca(estimate_row_size(table));
  build_key_packed_row(active_index, packed_key, &packed_key_len);

  uchar *rowdata;
  ulong rowdata_length;
  if ((error_code = trx->clustrix_net->key_read(clustrix_table_oid,
                                                active_index, table->read_set,
                                                packed_key, packed_key_len,
                                                &rowdata, &rowdata_length)))
    goto err;

  uchar const *current_row_end;
  ulong master_reclength;
  if ((error_code = unpack_row(rgi, table, table->s->fields, rowdata,
                              table->read_set, &current_row_end,
                              &master_reclength, rowdata + rowdata_length)))
    goto err;

  error_code = 0;
err:
  if (packed_key)
    my_afree(packed_key);

  DBUG_RETURN(error_code);
}

int ha_clustrixdb::index_first(uchar *buf)
{
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  is_scan = true;

  if (my_bitmap_init(&scan_fields, NULL, table->read_set->n_bits, false))
    return ER_OUTOFMEMORY;
#if 0
  if (table->s->keys)
    table->mark_columns_used_by_index(table->s->primary_key, &scan_fields);
  else
    bitmap_clear_all(&scan_fields);

  bitmap_union(&scan_fields, table->read_set);
#else
  /* Why is read_set not setup correctly? */
  bitmap_set_all(&scan_fields);
#endif

  if ((error_code = trx->clustrix_net->scan_init(clustrix_table_oid,
                                                 active_index,
                                                 clustrix_connection::SORT_NONE,
                                                 &scan_fields,
                                                 &scan_refid)))
    return error_code;


  return rnd_next(buf);
}

int ha_clustrixdb::index_last(uchar *buf)
{
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  is_scan = true;

  if (my_bitmap_init(&scan_fields, NULL, table->read_set->n_bits, false))
    return ER_OUTOFMEMORY;
#if 0
  if (table->s->keys)
    table->mark_columns_used_by_index(table->s->primary_key, &scan_fields);
  else
    bitmap_clear_all(&scan_fields);

  bitmap_union(&scan_fields, table->read_set);
#else
  /* Why is read_set not setup correctly? */
  bitmap_set_all(&scan_fields);
#endif

  if ((error_code = trx->clustrix_net->scan_init(clustrix_table_oid,
                                                 active_index,
                                                 clustrix_connection::SORT_NONE,
                                                 &scan_fields,
                                                 &scan_refid)))
    return error_code;


  return rnd_next(buf);

}

int ha_clustrixdb::index_next(uchar *buf)
{
  DBUG_ENTER("index_next");
  DBUG_RETURN(rnd_next(buf));
}

#if 0
int ha_clustrixdb::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  DBUG_ENTER("index_next_same");
  DBUG_RETURN(rnd_next(buf));
}
#endif

int ha_clustrixdb::index_prev(uchar *buf)
{
  DBUG_ENTER("index_prev");
  DBUG_RETURN(rnd_next(buf));
}

int ha_clustrixdb::index_end()
{
  DBUG_ENTER("index_prev");
  if (scan_refid)
    DBUG_RETURN(rnd_end());
  else
    DBUG_RETURN(0);

}

int ha_clustrixdb::rnd_init(bool scan)
{
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  add_current_table_to_rpl_table_list();
  is_scan = scan;
  scan_refid = 0;

  if (my_bitmap_init(&scan_fields, NULL, table->read_set->n_bits, false))
    return ER_OUTOFMEMORY;

#if 0
  if (table->s->keys)
    table->mark_columns_used_by_index(table->s->primary_key, &scan_fields);
  else
    bitmap_clear_all(&scan_fields);

  bitmap_union(&scan_fields, table->read_set);
#else
  /* Why is read_set not setup correctly? */
  bitmap_set_all(&scan_fields);
#endif

  if ((error_code = trx->clustrix_net->scan_init(clustrix_table_oid,
                                                 0,
                                                 clustrix_connection::SORT_NONE,
                                                 &scan_fields,
                                                 &scan_refid)))
    return error_code;

  return 0;
}

int ha_clustrixdb::rnd_next(uchar *buf)
{
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  assert(is_scan);
  assert(scan_refid);

  uchar *rowdata;
  ulong rowdata_length;
  if ((error_code = trx->clustrix_net->scan_next(scan_refid, &rowdata,
                                                 &rowdata_length)))
    return error_code;

  if (has_hidden_key) {
    last_hidden_key = *(ulonglong *)rowdata;
    rowdata += 8;
    rowdata_length -= 8;
  }

  uchar const *current_row_end;
  ulong master_reclength;

  error_code = unpack_row(rgi, table, table->s->fields, rowdata,
                          &scan_fields, &current_row_end,
                          &master_reclength, rowdata + rowdata_length);

  if (error_code)
    return error_code;

  return 0;
}

int ha_clustrixdb::rnd_pos(uchar * buf, uchar *pos)
{
  DBUG_ENTER("clx_rnd_pos");
  DBUG_DUMP("pos", pos, ref_length);

  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  /* WDD: We need a way to convert key buffers directy to rbr buffers. */

  if (has_hidden_key) {
    memcpy(&last_hidden_key, pos, sizeof(ulonglong));
  } else {
    uint keyno = table->s->primary_key;
    uint len = calculate_key_len(table, keyno, pos, table->const_key_parts[keyno]);
    key_restore(table->record[0], pos, &table->key_info[keyno], len);
  }

  // The estimate should consider only key fields widths.
  uchar *packed_key = (uchar*) my_alloca(estimate_row_size(table));
  size_t packed_key_len;
  build_key_packed_row(table->s->primary_key, packed_key, &packed_key_len);

  uchar *rowdata;
  ulong rowdata_length;
  if ((error_code = trx->clustrix_net->key_read(clustrix_table_oid, 0,
                                                table->read_set,
                                                packed_key, packed_key_len,
                                                &rowdata, &rowdata_length)))
    goto err;

  uchar const *current_row_end;
  ulong master_reclength;
  if ((error_code = unpack_row(rgi, table, table->s->fields, rowdata,
                              table->read_set, &current_row_end,
                              &master_reclength, rowdata + rowdata_length)))
    goto err;

  error_code = 0;
err:

  if (packed_key)
    my_afree(packed_key);

  DBUG_RETURN(error_code);
}

int ha_clustrixdb::rnd_end()
{
  int error_code = 0;
  THD *thd = ha_thd();
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  my_bitmap_free(&scan_fields);
  if (scan_refid && (error_code = trx->clustrix_net->scan_end(scan_refid)))
    return error_code;
  scan_refid = 0;

  return 0;
}

void ha_clustrixdb::position(const uchar *record)
{
  DBUG_ENTER("clx_position");
  if (has_hidden_key) {
    memcpy(ref, &last_hidden_key, sizeof(ulonglong));
  } else {
    KEY* key_info = table->key_info + table->s->primary_key;
    key_copy(ref, record, key_info, key_info->key_length);
  }
  DBUG_DUMP("key", ref, ref_length);
  DBUG_VOID_RETURN;
}

uint ha_clustrixdb::lock_count(void) const
{
  /* Hopefully, we don't need to use thread locks */
  return 0;
}

THR_LOCK_DATA **ha_clustrixdb::store_lock(THD *thd,
                                          THR_LOCK_DATA **to,
                                          enum thr_lock_type lock_type)
{
  /* Hopefully, we don't need to use thread locks */
  return to;
}

int ha_clustrixdb::external_lock(THD *thd, int lock_type)
{
  int error_code;
  st_clustrixdb_trx *trx = get_trx(thd, &error_code);
  if (lock_type != F_UNLCK)
    trx->begin_trans();

  //if (lock_type != F_UNLCK)
    //DBUG_ASSERT(trx && trx == get_trx(thd, &error_code));

  return 0;
}

/****************************************************************************
  Engine Condition Pushdown
****************************************************************************/

const COND *ha_clustrixdb::cond_push(const COND *cond)
{
  return cond;
}

void ha_clustrixdb::cond_pop()
{
}

int ha_clustrixdb::info_push(uint info_type, void *info)
{
  return 0;
}

void ha_clustrixdb::add_current_table_to_rpl_table_list()
{
  if (rli)
    return;

  THD *thd = ha_thd();
  rli = new Relay_log_info(FALSE);
  rli->sql_driver_thd = thd;

  rgi = new rpl_group_info(rli);
  rgi->thd = thd;
  rgi->tables_to_lock_count = 0;
  rgi->tables_to_lock = NULL;
  if (rgi->tables_to_lock_count)
    return;

  rgi->tables_to_lock = (RPL_TABLE_LIST *)my_malloc(sizeof(RPL_TABLE_LIST),
                                                    MYF(MY_WME));
  rgi->tables_to_lock->init_one_table(&table->s->db, &table->s->table_name, 0,
                                      TL_READ);
  rgi->tables_to_lock->table = table;
  rgi->tables_to_lock->table_id = table->tablenr;
  rgi->tables_to_lock->m_conv_table = NULL;
  rgi->tables_to_lock->master_had_triggers = FALSE;
  rgi->tables_to_lock->m_tabledef_valid = TRUE;
  // We need one byte per column to save a column's binlog type.
  uchar *col_type = (uchar*) my_alloca(table->s->fields);
  for (uint i = 0 ; i < table->s->fields ; ++i)
    col_type[i] = table->field[i]->binlog_type();

  table_def *tabledef = &rgi->tables_to_lock->m_tabledef;
  new (tabledef) table_def(col_type, table->s->fields, NULL, 0, NULL, 0);
  rgi->tables_to_lock_count++;
  if (col_type)
    my_afree(col_type);
}

void ha_clustrixdb::remove_current_table_from_rpl_table_list()
{
  if (!rgi->tables_to_lock)
    return;

  rgi->tables_to_lock->m_tabledef.table_def::~table_def();
  rgi->tables_to_lock->m_tabledef_valid = FALSE;
  my_free(rgi->tables_to_lock);
  rgi->tables_to_lock_count--;
  rgi->tables_to_lock = NULL;
  delete rli;
  delete rgi;
}

st_clustrixdb_trx *ha_clustrixdb::get_trx(THD *thd, int *error_code)
{
  *error_code = 0;
  st_clustrixdb_trx *trx;
  if (!(trx = (st_clustrixdb_trx *)thd_get_ha_data(thd, clustrixdb_hton)))
  {
    if (!(trx = new st_clustrixdb_trx(thd))) {
      *error_code = HA_ERR_OUT_OF_MEM;
      return NULL;
    }

    if ((*error_code = trx->net_init())) {
      delete trx;
      return NULL;
    }

    thd_set_ha_data(thd, clustrixdb_hton, trx);
  }

  return trx;
}

void ha_clustrixdb::build_key_packed_row(uint index, uchar *packed_key,
                                         size_t *packed_key_len)
{
  if (index == table->s->primary_key && has_hidden_key) {
    memcpy(packed_key, &last_hidden_key, sizeof(ulonglong));
    *packed_key_len = sizeof(ulonglong);
  } else {
    // make a row from the table
    table->mark_columns_used_by_index(index, &table->tmp_set);
    *packed_key_len = pack_row(table, &table->tmp_set, packed_key,
                               table->record[0]);
  }
}

/****************************************************************************
** Plugin Functions
****************************************************************************/

static int clustrixdb_commit(handlerton *hton, THD *thd, bool all)
{
  int error_code = 0;
  st_clustrixdb_trx* trx = (st_clustrixdb_trx *) thd_get_ha_data(thd, hton);
  assert(trx);

  if (trx->has_transaction)
  {
    if (all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
    {
      error_code = trx->clustrix_net->commit_trans();
      trx->has_transaction = FALSE;
    }
  }

  return error_code;
}

static int clustrixdb_rollback(handlerton *hton, THD *thd, bool all)
{
  int error_code = 0;
  st_clustrixdb_trx* trx = (st_clustrixdb_trx *) thd_get_ha_data(thd, hton);
  assert(trx);

  if (trx->has_transaction)
  {
    if (all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
    {
      error_code = trx->clustrix_net->rollback_trans();
      trx->has_transaction = FALSE;
    }
  }

  return error_code;
}

static handler* clustrixdb_create_handler(handlerton *hton, TABLE_SHARE *table,
                                   MEM_ROOT *mem_root)
{
  return new (mem_root) ha_clustrixdb(hton, table);
}

static int clustrixdb_close_connection(handlerton* hton, THD* thd)
{
  st_clustrixdb_trx* trx = (st_clustrixdb_trx *) thd_get_ha_data(thd, hton);
  if (!trx)
    return 0; /* Transaction is not started */

  if (trx->has_transaction)
    clustrixdb_rollback(clustrixdb_hton, thd, TRUE);

  delete trx;

  return 0;
}

static int clustrixdb_panic(handlerton *hton, ha_panic_function type)
{
  return 0;
}


static bool clustrixdb_show_status(handlerton *hton, THD *thd,
                            stat_print_fn *stat_print,
                            enum ha_stat_type stat_type)
{
  return FALSE;
}

static int clustrixdb_discover_table_names(handlerton *hton, LEX_CSTRING *db,
                                           MY_DIR *dir,
                                           handlerton::discovered_list *result)
{
  clustrix_connection *clustrix_net = new clustrix_connection();
  int error_code = clustrix_net->connect();
  if (error_code)
    return error_code;

  error_code = clustrix_net->populate_table_list(db, result);
  delete clustrix_net;
  return 0; // error_code;
}

int clustrixdb_discover_table(handlerton *hton, THD *thd, TABLE_SHARE *share)
{
  clustrix_connection *clustrix_net = new clustrix_connection();
  int error_code = clustrix_net->connect();
  if (error_code)
      return error_code;

  error_code = clustrix_net->discover_table_details(&share->db,
                                                    &share->table_name,
                                                    thd, share);

  delete clustrix_net;
  return error_code;
}

static int clustrixdb_init(void *p)
{
  clustrixdb_hton = (handlerton *) p;
  clustrixdb_hton->state = SHOW_OPTION_YES;
  clustrixdb_hton->flags = HTON_NO_FLAGS;
  clustrixdb_hton->panic = clustrixdb_panic;
  clustrixdb_hton->close_connection = clustrixdb_close_connection;
  clustrixdb_hton->commit = clustrixdb_commit;
  clustrixdb_hton->rollback = clustrixdb_rollback;
  clustrixdb_hton->create = clustrixdb_create_handler;
  clustrixdb_hton->show_status = clustrixdb_show_status;
  clustrixdb_hton->discover_table_names = clustrixdb_discover_table_names;
  clustrixdb_hton->discover_table = clustrixdb_discover_table;

  return 0;
}

struct st_mysql_show_var clustrixdb_status_vars[] =
{
  {NullS, NullS, SHOW_LONG}
};

static struct st_mysql_sys_var* clustrixdb_system_variables[] =
{
  MYSQL_SYSVAR(connect_timeout),
  MYSQL_SYSVAR(read_timeout),
  MYSQL_SYSVAR(write_timeout),
  MYSQL_SYSVAR(host),
  MYSQL_SYSVAR(username),
  MYSQL_SYSVAR(password),
  MYSQL_SYSVAR(port),
  MYSQL_SYSVAR(socket),
  NULL
};

static struct st_mysql_storage_engine clustrixdb_storage_engine =
  {MYSQL_HANDLERTON_INTERFACE_VERSION};

maria_declare_plugin(clustrixdb)
{
    MYSQL_STORAGE_ENGINE_PLUGIN,                /* Plugin Type */
    &clustrixdb_storage_engine,                 /* Plugin Descriptor */
    "CLUSTRIXDB",                               /* Plugin Name */
    "MariaDB",                                  /* Plugin Author */
    "ClustrixDB storage engine",                /* Plugin Description */
    PLUGIN_LICENSE_GPL,                         /* Plugin Licence */
    clustrixdb_init,                            /* Plugin Entry Point */
    NULL,                                       /* Plugin Deinitializer */
    0x0001,                                     /* Hex Version Number (0.1) */
    NULL /* clustrixdb_status_vars */,          /* Status Variables */
    clustrixdb_system_variables,                /* System Variables */
    "0.1",                                      /* String Version */
    MariaDB_PLUGIN_MATURITY_EXPERIMENTAL        /* Maturity Level */
}
maria_declare_plugin_end;
