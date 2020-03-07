/*****************************************************************************
Copyright (c) 2019, 2020, MariaDB Corporation.
*****************************************************************************/

/** @file ha_xpand.cc */

#include "ha_xpand.h"
#include "ha_xpand_pushdown.h"
#include "key.h"
#include <strfunc.h>                            /* strconvert */
#include "my_pthread.h"

handlerton *xpand_hton = NULL;

int xpand_connect_timeout;
static MYSQL_SYSVAR_INT
(
  connect_timeout,
  xpand_connect_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Timeout for connecting to Xpand",
  NULL, NULL, -1, -1, 2147483647, 0
);

int xpand_read_timeout;
static MYSQL_SYSVAR_INT
(
  read_timeout,
  xpand_read_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Timeout for receiving data from Xpand",
  NULL, NULL, -1, -1, 2147483647, 0
);

int xpand_write_timeout;
static MYSQL_SYSVAR_INT
(
  write_timeout,
  xpand_write_timeout,
  PLUGIN_VAR_OPCMDARG,
  "Timeout for sending data to Xpand",
  NULL, NULL, -1, -1, 2147483647, 0
);

//state for load balancing
int xpand_hosts_cur; //protected by my_atomic's
ulong xpand_balance_algorithm;
const char* balance_algorithm_names[]=
{
  "first", "round_robin", NullS
};

TYPELIB balance_algorithms=
{
  array_elements(balance_algorithm_names) - 1, "",
  balance_algorithm_names, NULL
};

static void update_balance_algorithm(MYSQL_THD thd, struct st_mysql_sys_var *var,
                                     void *var_ptr, const void *save)
{
  *static_cast<ulong *>(var_ptr) = *static_cast<const ulong *>(save);
  my_atomic_store32(&xpand_hosts_cur, 0);
}

static MYSQL_SYSVAR_ENUM
(
  balance_algorithm,
  xpand_balance_algorithm,
  PLUGIN_VAR_OPCMDARG,
  "Method for managing load balancing of Clustrix nodes, can take values FIRST or ROUND_ROBIN",
  NULL, update_balance_algorithm, XPAND_BALANCE_ROUND_ROBIN, &balance_algorithms
);

//current list of clx hosts
static PSI_rwlock_key key_xpand_hosts;
mysql_rwlock_t xpand_hosts_lock;
xpand_host_list *xpand_hosts;

static int check_hosts(MYSQL_THD thd, struct st_mysql_sys_var *var,
                       void *save, struct st_mysql_value *value)
{
  DBUG_ENTER("check_hosts");
  char b;
  int len = 0;
  const char *val = value->val_str(value, &b, &len);
  if (!val)
    DBUG_RETURN(HA_ERR_OUT_OF_MEM);

  xpand_host_list list;
  memset(&list, 0, sizeof(list));

  int error_code = 0;
  if ((error_code = list.fill(val)))
    DBUG_RETURN(error_code);
  list.empty();

  *static_cast<const char **>(save) = val;
  DBUG_RETURN(0);
}

static void update_hosts(MYSQL_THD thd, struct st_mysql_sys_var *var,
                         void *var_ptr, const void *save)
{
  DBUG_ENTER("update_hosts");
  const char *from_save = *static_cast<const char * const *>(save);

  mysql_rwlock_wrlock(&xpand_hosts_lock);

  xpand_host_list *list = static_cast<xpand_host_list*>(
                          my_malloc(sizeof(xpand_host_list), MYF(MY_WME | MY_ZEROFILL)));
  int error_code = list->fill(from_save);
  if (error_code) {
    my_free(list);
    sql_print_error("Unhandled error %d setting xpand hostlist", error_code);
    DBUG_VOID_RETURN;
  }

  xpand_hosts->empty();
  my_free(xpand_hosts);
  xpand_hosts = list;

  char **display_var = static_cast<char**>(var_ptr);
  my_free(*display_var);
  *display_var = my_strdup(from_save, MYF(MY_WME));

  mysql_rwlock_unlock(&xpand_hosts_lock);
  DBUG_VOID_RETURN;
}

static char *xpand_hosts_str;
static MYSQL_SYSVAR_STR
(
  hosts,
  xpand_hosts_str,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "List of xpand hostnames seperated by commas, semicolons or spaces",
  check_hosts, update_hosts, "localhost"
);

char *xpand_username;
static MYSQL_SYSVAR_STR
(
  username,
  xpand_username,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "Xpand user name",
  NULL, NULL, "root"
);

char *xpand_password;
static MYSQL_SYSVAR_STR
(
  password,
  xpand_password,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "Xpand password",
  NULL, NULL, ""
);

uint xpand_port;
static MYSQL_SYSVAR_UINT
(
  port,
  xpand_port,
  PLUGIN_VAR_RQCMDARG,
  "Xpand port",
  NULL, NULL, MYSQL_PORT_DEFAULT, MYSQL_PORT_DEFAULT, 65535, 0
);

char *xpand_socket;
static MYSQL_SYSVAR_STR
(
  socket,
  xpand_socket,
  PLUGIN_VAR_OPCMDARG | PLUGIN_VAR_MEMALLOC,
  "Xpand socket",
  NULL, NULL, ""
);

static MYSQL_THDVAR_UINT
(
  row_buffer,
  PLUGIN_VAR_RQCMDARG,
  "Xpand rowstore row buffer size",
  NULL, NULL, 20, 1, 65535, 0
);

// Per thread select handler knob
static MYSQL_THDVAR_BOOL(
  select_handler,
  PLUGIN_VAR_NOCMDARG,
  "",
  NULL,
  NULL,
  1
);

// Per thread derived handler knob
static MYSQL_THDVAR_BOOL(
  derived_handler,
  PLUGIN_VAR_NOCMDARG,
  "",
  NULL,
  NULL,
  1
);

static MYSQL_THDVAR_BOOL(
  enable_direct_update,
  PLUGIN_VAR_NOCMDARG,
  "",
  NULL,
  NULL,
  1
);

bool select_handler_setting(THD* thd)
{
  return ( thd == NULL ) ? false : THDVAR(thd, select_handler);
}

bool derived_handler_setting(THD* thd)
{
  return ( thd == NULL ) ? false : THDVAR(thd, derived_handler);
}

uint row_buffer_setting(THD* thd)
{
  return THDVAR(thd, row_buffer);
}


/*
  Get an Xpand_share object for this object. If it doesn't yet exist, create
  it.
*/

Xpand_share *ha_xpand::get_share()
{
  Xpand_share *tmp_share;

  DBUG_ENTER("ha_xpand::get_share()");

  lock_shared_ha_data();
  if (!(tmp_share= static_cast<Xpand_share*>(get_ha_share_ptr())))
  {
    tmp_share= new Xpand_share;
    if (!tmp_share)
      goto err;

    set_ha_share_ptr(static_cast<Handler_share*>(tmp_share));
  }
err:
  unlock_shared_ha_data();
  DBUG_RETURN(tmp_share);
}


/****************************************************************************
** Utility functions
****************************************************************************/
// This is a wastefull aproach but better then fixed sized buffer.
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


/*
  Try to decode a string from filename encoding, if that fails, return the
  original string.

  @detail
    This is used to get table (or database) name from file (or directory)
    name. Names of regular tables/databases are encoded using
    my_charset_filename encoding.
    Names of temporary tables are not encoded, and they start with '#sql'
    which is not a valid character sequence in my_charset_filename encoding.
    Our way to talkle this is to
    1. Try to convert the name back
    2. If that failed, assume it's a temporary object name and just use the
       name.
*/

static void decode_object_or_tmp_name(const char *from, uint size,
                                     std::string *out)
{
  uint errors, new_size;
  out->resize(size+1); // assume the decoded string is not longer
  new_size= strconvert(&my_charset_filename, from, size,
                       system_charset_info, (char*)out->c_str(), size+1,
                       &errors);
  if (errors)
    out->assign(from, size);
  else
    out->resize(new_size);
}

/*
  Take a "./db_name/table_name" and extract db_name and table_name from it

  @return
     0     OK
     other Error code
*/
static int normalize_tablename(const char *db_table,
                               std::string *norm_db, std::string *norm_table)
{
  std::string tablename(db_table);
  if (tablename.size() < 2 || tablename[0] != '.' ||
      (tablename[1] != FN_LIBCHAR && tablename[1] != FN_LIBCHAR2)) {
    DBUG_ASSERT(0);  // We were not passed table name?
    return HA_ERR_INTERNAL_ERROR;
  }

  size_t pos = tablename.find_first_of(FN_LIBCHAR, 2);
  if (pos == std::string::npos) {
    pos = tablename.find_first_of(FN_LIBCHAR2, 2);
  }

  if (pos == std::string::npos) {
    DBUG_ASSERT(0);  // We were not passed table name?
    return HA_ERR_INTERNAL_ERROR;
  }

  decode_object_or_tmp_name(tablename.c_str() + 2, pos - 2, norm_db);
  decode_object_or_tmp_name(tablename.c_str() + pos + 1,
                           tablename.size() - (pos + 1), norm_table);
  return 0;
}


xpand_connection *get_trx(THD *thd, int *error_code)
{
  *error_code = 0;
  xpand_connection *trx;
  if (!(trx = (xpand_connection *)thd_get_ha_data(thd, xpand_hton)))
  {
    if (!(trx = new xpand_connection())) {
      *error_code = HA_ERR_OUT_OF_MEM;
      return NULL;
    }

    *error_code = trx->connect();
    if (*error_code) {
      delete trx;
      return NULL;
    }

    thd_set_ha_data(thd, xpand_hton, trx);
  }

  return trx;
}
/****************************************************************************
** Class ha_xpand
****************************************************************************/

ha_xpand::ha_xpand(handlerton *hton, TABLE_SHARE *table_arg)
  : handler(hton, table_arg)
{
  DBUG_ENTER("ha_xpand::ha_xpand");
  rgi = NULL;
  scan_cur = NULL;
  xpand_table_oid = 0;
  upsert_flag = 0;
  DBUG_VOID_RETURN;
}

ha_xpand::~ha_xpand()
{
  if (rgi)
    remove_current_table_from_rpl_table_list(rgi);
}


int ha_xpand::create(const char *name, TABLE *form, HA_CREATE_INFO *info)
{
  int error_code;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
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

  std::string norm_db, norm_table;
  if ((error_code= normalize_tablename(name, &norm_db, &norm_table)))
    return error_code;

  TABLE_LIST table_list;
  memset(&table_list, 0, sizeof(table_list));
  table_list.table = form;
  error_code = show_create_table_ex(thd, &table_list,
                                    norm_db.c_str(), norm_table.c_str(),
                                    &create_table_stmt, create_info, WITH_DB_NAME);
  if (!is_tmp_table)
    form->s->tmp_table = saved_tmp_table_type;
  create_info->used_fields = old;
  thd->db.str = old_dbstr;
  if (error_code)
    return error_code;

  // To syncronize the schemas of MDB FE and XPD BE.
  if (form->s && form->s->db.length) {
    String createdb_stmt;
    createdb_stmt.append("CREATE DATABASE IF NOT EXISTS `");
    createdb_stmt.append(form->s->db.str, form->s->db.length);
    createdb_stmt.append("`");
    trx->run_query(createdb_stmt);
  }

  return trx->run_query(create_table_stmt);
}

int ha_xpand::delete_table(const char *path)
{
  int error_code;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  std::string decoded_dbname;
  std::string decoded_tbname;
  if ((error_code= normalize_tablename(path, &decoded_dbname,
                                       &decoded_tbname)))
    return error_code;

  String delete_cmd;
  delete_cmd.append("DROP TABLE `");
  delete_cmd.append(decoded_dbname.c_str());
  delete_cmd.append("`.`");
  delete_cmd.append(decoded_tbname.c_str());
  delete_cmd.append("`");

  return trx->run_query(delete_cmd);
}

int ha_xpand::rename_table(const char* from, const char* to)
{
  int error_code;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  std::string decoded_from_dbname;
  std::string decoded_from_tbname;
  if ((error_code= normalize_tablename(from, &decoded_from_dbname,
                                       &decoded_from_tbname)))
    return error_code;

  std::string decoded_to_dbname;
  std::string decoded_to_tbname;
  if ((error_code= normalize_tablename(to, &decoded_to_dbname,
                                       &decoded_to_tbname)))
    return error_code;

  String rename_cmd;
  rename_cmd.append("RENAME TABLE `");
  rename_cmd.append(decoded_from_dbname.c_str());
  rename_cmd.append("`.`");
  rename_cmd.append(decoded_from_tbname.c_str());
  rename_cmd.append("` TO `");
  rename_cmd.append(decoded_to_dbname.c_str());
  rename_cmd.append("`.`");
  rename_cmd.append(decoded_to_tbname.c_str());
  rename_cmd.append("`;");

  return trx->run_query(rename_cmd);
}

static void
xpand_mark_table_for_discovery(TABLE *table)
{
  table->m_needs_reopen = true;
  Xpand_share *xs;
  if ((xs= static_cast<Xpand_share*>(table->s->ha_share)))
    xs->rediscover_table = true;
}

void
xpand_mark_tables_for_discovery(LEX *lex)
{
  for (TABLE_LIST *tbl= lex->query_tables; tbl; tbl= tbl->next_global)
    if (tbl->table && tbl->table->file->ht == xpand_hton)
      xpand_mark_table_for_discovery(tbl->table);
}

ulonglong *
xpand_extract_table_oids(THD *thd, LEX *lex)
{
  int cnt = 1;
  for (TABLE_LIST *tbl = lex->query_tables; tbl; tbl= tbl->next_global)
    if (tbl->table && tbl->table->file->ht == xpand_hton)
        cnt++;

  ulonglong *oids = (ulonglong*)thd_alloc(thd, cnt * sizeof(ulonglong));
  ulonglong *ptr = oids;
  for (TABLE_LIST *tbl = lex->query_tables; tbl; tbl= tbl->next_global)
  {
    if (tbl->table && tbl->table->file->ht == xpand_hton)
    {
      ha_xpand *hndlr = static_cast<ha_xpand *>(tbl->table->file);
      *ptr++ = hndlr->get_table_oid();
    }
  }

  *ptr = 0;
  return oids;
}

int ha_xpand::open(const char *name, int mode, uint test_if_locked)
{
  THD *thd= ha_thd();
  DBUG_ENTER("ha_xpand::open");

  Xpand_share *share;
  if (!(share = get_share()))
    DBUG_RETURN(1);

  int error_code;
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  if (share->rediscover_table)
    DBUG_RETURN(HA_ERR_TABLE_DEF_CHANGED);

  if (!share->xpand_table_oid) {
    // We may end up with two threads executing this piece concurrently but
    // it's ok
    std::string norm_table;
    std::string norm_db;
    if ((error_code= normalize_tablename(name, &norm_db, &norm_table)))
      DBUG_RETURN(error_code);

    ulonglong oid = 0;
    error_code= trx->get_table_oid(norm_db.c_str(), strlen(norm_db.c_str()),
                                   norm_table.c_str(),
                                   strlen(norm_table.c_str()), &oid,
                                   table_share);
    if (error_code)
      DBUG_RETURN(error_code);

    share->xpand_table_oid = oid;
  }

  xpand_table_oid = share->xpand_table_oid;

  // Surrogate key marker
  has_hidden_key = table->s->primary_key == MAX_KEY;
  if (has_hidden_key) {
    ref_length = 8;
  } else {
    KEY* key_info = table->key_info + table->s->primary_key;
    ref_length = key_info->key_length;
  }

  DBUG_PRINT("open finished",
             ("oid: %llu, ref_length: %u", xpand_table_oid, ref_length));
  DBUG_RETURN(0);
}

int ha_xpand::close(void)
{
  return 0;
}

int ha_xpand::reset()
{
  upsert_flag &= ~XPAND_BULK_UPSERT;
  upsert_flag &= ~XPAND_HAS_UPSERT;
  upsert_flag &= ~XPAND_UPSERT_SENT;
  xpd_lock_type = XPAND_NO_LOCKS;
  pushdown_cond_list.empty();
  return 0;
}

int ha_xpand::extra(enum ha_extra_function operation)
{
  DBUG_ENTER("ha_xpand::extra");
  if (operation == HA_EXTRA_INSERT_WITH_UPDATE)
    upsert_flag |= XPAND_HAS_UPSERT;
  DBUG_RETURN(0);
}

/*@brief UPSERT State Machine*/
/*************************************************************
 * DESCRIPTION:
 * Fasttrack for UPSERT sends queries down to a XPD backend.
 * UPSERT could be of two kinds: singular and bulk. The plugin
 * re-/sets XPAND_BULK_UPSERT in end|start_bulk_insert
 * methods. XPAND_UPSERT_SENT is used to avoid multiple
 * execution at XPD backend.
 * Generic XPAND_HAS_UPSERT is set for bulk UPSERT only b/c
 * MDB calls write_row only once.
 ************************************************************/
int ha_xpand::write_row(const uchar *buf)
{
  int error_code = 0;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  if (upsert_flag & XPAND_HAS_UPSERT) {
    if (!(upsert_flag & XPAND_UPSERT_SENT)) {
      ha_rows update_rows;
      String update_stmt;
      update_stmt.append(thd->query_string.str());

      if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
        trx->auto_commit_next();

      ulonglong *oids = xpand_extract_table_oids(thd, thd->lex);
      error_code= trx->update_query(update_stmt, table->s->db, oids,
                                    &update_rows);
      if (upsert_flag & XPAND_BULK_UPSERT)
        upsert_flag |= XPAND_UPSERT_SENT;
      else
        upsert_flag &= ~XPAND_HAS_UPSERT;
    }

    if (error_code == HA_ERR_TABLE_DEF_CHANGED)
      xpand_mark_tables_for_discovery(thd->lex);
    return error_code;
  }

  /* Convert the row format to binlog (packed) format */
  uchar *packed_new_row = (uchar*) my_alloca(estimate_row_size(table));
  size_t packed_size = pack_row(table, table->write_set, packed_new_row, buf);

  /* XXX: Xpand may needs to return HA_ERR_AUTOINC_ERANGE if we hit that
     error. */
  ulonglong last_insert_id = 0;
  if ((error_code = trx->write_row(xpand_table_oid, packed_new_row, packed_size,
                                   &last_insert_id)))
    goto err;

  if (table->next_number_field)
    insert_id_for_cur_row = last_insert_id;

err:
  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_table_for_discovery(table);

  if (packed_size)
    my_afree(packed_new_row);

  return error_code;
}

int ha_xpand::update_row(const uchar *old_data, const uchar *new_data)
{
  DBUG_ENTER("ha_xpand::update_row");
  int error_code;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  size_t row_size = estimate_row_size(table);
  size_t packed_key_len;
  uchar *packed_key = (uchar*) my_alloca(row_size);
  build_key_packed_row(table->s->primary_key, old_data,
                       packed_key, &packed_key_len);

  uchar *packed_new_row = (uchar*) my_alloca(row_size);
  size_t packed_new_size = pack_row(table, table->write_set, packed_new_row,
                                    new_data);

  /* Send the packed rows to Xpand */
  error_code = trx->key_update(xpand_table_oid, packed_key, packed_key_len,
                               table->write_set,
                               packed_new_row, packed_new_size);

  if(packed_key)
    my_afree(packed_key);

  if(packed_new_row)
    my_afree(packed_new_row);

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_table_for_discovery(table);

  DBUG_RETURN(error_code);
}

int ha_xpand::direct_update_rows_init(List<Item> *update_fields)
{
  DBUG_ENTER("ha_xpand::direct_update_rows_init");
  THD *thd= ha_thd();
  if (!THDVAR(thd, enable_direct_update))
    DBUG_RETURN(HA_ERR_WRONG_COMMAND);
  DBUG_RETURN(0);
}

int ha_xpand::direct_update_rows(ha_rows *update_rows, ha_rows *found_rows)
{
  DBUG_ENTER("ha_xpand::direct_update_rows");
  int error_code= 0;
  THD *thd= ha_thd();
  xpand_connection *trx= get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  String update_stmt;
  // Do the same as create_xpand_select_handler does:
  thd->lex->print(&update_stmt, QT_ORDINARY);

  if (!thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
    trx->auto_commit_next();

  ulonglong *oids = xpand_extract_table_oids(thd, thd->lex);
  error_code = trx->update_query(update_stmt, table->s->db, oids, update_rows);
  *found_rows = *update_rows;
  
  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_tables_for_discovery(thd->lex);
  DBUG_RETURN(error_code);
}

void ha_xpand::start_bulk_insert(ha_rows rows, uint flags)
{
  DBUG_ENTER("ha_xpand::start_bulk_insert");
  int error_code= 0;
  THD *thd= ha_thd();
  xpand_connection *trx= get_trx(thd, &error_code);
  if (!trx) {
    // TBD log this
    DBUG_VOID_RETURN;
  }

  upsert_flag |= XPAND_BULK_UPSERT;

  DBUG_VOID_RETURN;
}

int ha_xpand::end_bulk_insert()
{
  DBUG_ENTER("ha_xpand::end_bulk_insert");
  upsert_flag &= ~XPAND_BULK_UPSERT;
  upsert_flag &= ~XPAND_HAS_UPSERT;
  upsert_flag &= ~XPAND_UPSERT_SENT;
  DBUG_RETURN(0);
}

int ha_xpand::delete_row(const uchar *buf)
{
  int error_code;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  // The estimate should consider only key fields widths.
  size_t packed_key_len;
  uchar *packed_key = (uchar*) my_alloca(estimate_row_size(table));
  build_key_packed_row(table->s->primary_key, buf, packed_key, &packed_key_len);

  error_code = trx->key_delete(xpand_table_oid, packed_key, packed_key_len);

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_table_for_discovery(table);

  if (packed_key)
    my_afree(packed_key);

  return error_code;
}

ha_xpand::Table_flags ha_xpand::table_flags(void) const
{
  Table_flags flags = HA_PARTIAL_COLUMN_READ |
                      HA_REC_NOT_IN_SEQ |
                      HA_FAST_KEY_READ |
                      HA_NULL_IN_KEY |
                      HA_CAN_INDEX_BLOBS |
                      HA_AUTO_PART_KEY |
                      HA_CAN_SQL_HANDLER |
                      HA_BINLOG_STMT_CAPABLE |
                      HA_CAN_TABLE_CONDITION_PUSHDOWN |
                      HA_CAN_DIRECT_UPDATE_AND_DELETE;

  return flags;
}

ulong ha_xpand::index_flags(uint idx, uint part, bool all_parts) const
{
  ulong flags = HA_READ_NEXT |
                HA_READ_PREV |
                HA_READ_ORDER |
                HA_READ_RANGE;

  return flags;
}

ha_rows ha_xpand::records()
{
  return 10000;
}

ha_rows ha_xpand::records_in_range(uint inx, key_range *min_key,
                                   key_range *max_key)
{
  return 2;
}

int ha_xpand::info(uint flag)
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
      stats.mean_rec_length = (ulong) (stats.data_file_length / stats.records);
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

int ha_xpand::index_init(uint idx, bool sorted)
{
  int error_code = 0;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  active_index = idx;
  add_current_table_to_rpl_table_list(&rgi, thd, table);
  scan_cur = NULL;

  /* Return all columns until there is a better understanding of
     requirements. */
  if (my_bitmap_init(&scan_fields, NULL, table->read_set->n_bits, false))
    return ER_OUTOFMEMORY;
  bitmap_set_all(&scan_fields);
  sorted_scan = sorted;

  return 0;
}

int ha_xpand::index_read(uchar * buf, const uchar * key, uint key_len,
                         enum ha_rkey_function find_flag)
{
  DBUG_ENTER("ha_xpand::index_read");
  int error_code = 0;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  key_restore(buf, key, &table->key_info[active_index], key_len);
  // The estimate should consider only key fields widths.
  size_t packed_key_len;
  uchar *packed_key = (uchar*) my_alloca(estimate_row_size(table));
  build_key_packed_row(active_index, buf, packed_key, &packed_key_len);

  bool exact = false;
  xpand_connection::scan_type st;
  switch (find_flag) {
    case HA_READ_KEY_EXACT:
      exact = true;
      break;
    case HA_READ_KEY_OR_NEXT:
      st = xpand_connection::READ_KEY_OR_NEXT;
      break;
    case HA_READ_KEY_OR_PREV:
      st = xpand_connection::READ_KEY_OR_PREV;
      break;
    case HA_READ_AFTER_KEY:
      st = xpand_connection::READ_AFTER_KEY;
      break;
    case HA_READ_BEFORE_KEY:
      st = xpand_connection::READ_BEFORE_KEY;
      break;
    case HA_READ_PREFIX:
    case HA_READ_PREFIX_LAST:
    case HA_READ_PREFIX_LAST_OR_PREV:
    case HA_READ_MBR_CONTAIN:
    case HA_READ_MBR_INTERSECT:
    case HA_READ_MBR_WITHIN:
    case HA_READ_MBR_DISJOINT:
    case HA_READ_MBR_EQUAL:
      DBUG_RETURN(ER_NOT_SUPPORTED_YET);
  }

  uchar *rowdata = NULL;
  if (exact) {
    is_scan = false;
    ulong rowdata_length;
    error_code = trx->key_read(xpand_table_oid, 0, xpd_lock_type,
                               table->read_set, packed_key, packed_key_len,
                               &rowdata, &rowdata_length);
    if (!error_code)
      error_code = unpack_row_to_buf(rgi, table, buf, rowdata, table->read_set,
                                       rowdata + rowdata_length);
  } else {
    is_scan = true;
    error_code = trx->scan_from_key(xpand_table_oid, active_index,
                                    xpd_lock_type, st, -1, sorted_scan,
                                    &scan_fields, packed_key, packed_key_len,
                                    THDVAR(thd, row_buffer), &scan_cur);
    if (!error_code)
      error_code = rnd_next(buf);
  }

  if (rowdata)
    my_free(rowdata);

  if (packed_key)
    my_afree(packed_key);

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_table_for_discovery(table);

  DBUG_RETURN(error_code);
}

int ha_xpand::index_first(uchar *buf)
{
  DBUG_ENTER("ha_xpand::index_first");
  int error_code = 0;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  error_code = trx->scan_from_key(xpand_table_oid, active_index, xpd_lock_type,
                                  xpand_connection::READ_FROM_START, -1,
                                  sorted_scan, &scan_fields, NULL, 0,
                                  THDVAR(thd, row_buffer), &scan_cur);

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_table_for_discovery(table);

  if (error_code)
    DBUG_RETURN(error_code);

  DBUG_RETURN(rnd_next(buf));
}

int ha_xpand::index_last(uchar *buf)
{
  DBUG_ENTER("ha_xpand::index_last");
  int error_code = 0;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  error_code = trx->scan_from_key(xpand_table_oid, active_index, xpd_lock_type,
                                  xpand_connection::READ_FROM_LAST, -1,
                                  sorted_scan, &scan_fields, NULL, 0,
                                  THDVAR(thd, row_buffer), &scan_cur);

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_table_for_discovery(table);

  if (error_code)
    DBUG_RETURN(error_code);

  DBUG_RETURN(rnd_next(buf));
}

int ha_xpand::index_next(uchar *buf)
{
  DBUG_ENTER("index_next");
  DBUG_RETURN(rnd_next(buf));
}

#if 0
int ha_xpand::index_next_same(uchar *buf, const uchar *key, uint keylen)
{
  DBUG_ENTER("index_next_same");
  DBUG_RETURN(rnd_next(buf));
}
#endif

int ha_xpand::index_prev(uchar *buf)
{
  DBUG_ENTER("index_prev");
  DBUG_RETURN(rnd_next(buf));
}

int ha_xpand::index_end()
{
  DBUG_ENTER("index_prev");
  if (scan_cur)
    DBUG_RETURN(rnd_end());
  else
  {
    my_bitmap_free(&scan_fields);
    DBUG_RETURN(0);
  }
}

int ha_xpand::rnd_init(bool scan)
{
  DBUG_ENTER("ha_xpand::rnd_init");
  int error_code = 0;
  THD *thd = ha_thd();
  if (thd->lex->sql_command == SQLCOM_UPDATE)
    DBUG_RETURN(error_code);
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  add_current_table_to_rpl_table_list(&rgi, thd, table);
  is_scan = scan;
  scan_cur = NULL;

  if (my_bitmap_init(&scan_fields, NULL, table->read_set->n_bits, false))
    DBUG_RETURN(ER_OUTOFMEMORY);

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

  String* pushdown_cond_sql = nullptr;
  if (pushdown_cond_list.elements) {
    pushdown_cond_sql = new String();
    while (pushdown_cond_list.elements > 0) {
      COND* cond = pushdown_cond_list.pop();
      String sql_predicate;
      cond->print_for_table_def(&sql_predicate);
      pushdown_cond_sql->append(sql_predicate);
      if ( pushdown_cond_list.elements > 0)
        pushdown_cond_sql->append(" AND ");
    }
  }
 
  error_code = trx->scan_table(xpand_table_oid, xpd_lock_type, &scan_fields,
                               THDVAR(thd, row_buffer), &scan_cur,
                               pushdown_cond_sql);

  if (pushdown_cond_sql != nullptr)
    delete pushdown_cond_sql;

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_table_for_discovery(table);

  if (error_code)
    DBUG_RETURN(error_code);

  DBUG_RETURN(0);
}

int ha_xpand::rnd_next(uchar *buf)
{
  int error_code = 0;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    return error_code;

  assert(is_scan);
  assert(scan_cur);

  uchar *rowdata;
  ulong rowdata_length;
  if ((error_code = trx->scan_next(scan_cur, &rowdata, &rowdata_length)))
    return error_code;

  if (has_hidden_key) {
    last_hidden_key = *(ulonglong *)rowdata;
    rowdata += 8;
    rowdata_length -= 8;
  }

  error_code = unpack_row_to_buf(rgi, table, buf, rowdata, &scan_fields,
                                 rowdata + rowdata_length);

  if (error_code)
    return error_code;

  return 0;
}

int ha_xpand::rnd_pos(uchar * buf, uchar *pos)
{
  DBUG_ENTER("xpd_rnd_pos");
  DBUG_DUMP("pos", pos, ref_length);

  int error_code = 0;
  THD *thd = ha_thd();
  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  /* WDD: We need a way to convert key buffers directy to rbr buffers. */

  if (has_hidden_key) {
    memcpy(&last_hidden_key, pos, sizeof(ulonglong));
  } else {
    uint keyno = table->s->primary_key;
    uint len = calculate_key_len(table, keyno, pos,
                                 table->const_key_parts[keyno]);
    key_restore(buf, pos, &table->key_info[keyno], len);
  }

  // The estimate should consider only key fields widths.
  uchar *packed_key = (uchar*) my_alloca(estimate_row_size(table));
  size_t packed_key_len;
  build_key_packed_row(table->s->primary_key, buf, packed_key, &packed_key_len);

  uchar *rowdata = NULL;
  ulong rowdata_length;
  if ((error_code = trx->key_read(xpand_table_oid, 0, xpd_lock_type,
                                  table->read_set, packed_key, packed_key_len,
                                  &rowdata, &rowdata_length)))
    goto err;

  if ((error_code = unpack_row_to_buf(rgi, table, buf, rowdata, table->read_set,
                                      rowdata + rowdata_length)))
    goto err;

err:
  if (rowdata)
    my_free(rowdata);

  if (packed_key)
    my_afree(packed_key);

  if (error_code == HA_ERR_TABLE_DEF_CHANGED)
    xpand_mark_table_for_discovery(table);

  DBUG_RETURN(error_code);
}

int ha_xpand::rnd_end()
{
  DBUG_ENTER("ha_xpand::rnd_end()");
  int error_code = 0;
  THD *thd = ha_thd();
  if (thd->lex->sql_command == SQLCOM_UPDATE)
    DBUG_RETURN(error_code);

  xpand_connection *trx = get_trx(thd, &error_code);
  if (!trx)
    DBUG_RETURN(error_code);

  my_bitmap_free(&scan_fields);
  if (scan_cur && (error_code = trx->scan_end(scan_cur)))
    DBUG_RETURN(error_code);
  scan_cur = NULL;

  DBUG_RETURN(0);
}

void ha_xpand::position(const uchar *record)
{
  DBUG_ENTER("xpd_position");
  if (has_hidden_key) {
    memcpy(ref, &last_hidden_key, sizeof(ulonglong));
  } else {
    KEY* key_info = table->key_info + table->s->primary_key;
    key_copy(ref, record, key_info, key_info->key_length);
  }
  DBUG_DUMP("key", ref, ref_length);
  DBUG_VOID_RETURN;
}

uint ha_xpand::lock_count(void) const
{
  /* Hopefully, we don't need to use thread locks */
  return 0;
}

THR_LOCK_DATA **ha_xpand::store_lock(THD *thd, THR_LOCK_DATA **to,
                                     enum thr_lock_type lock_type)
{
  /* Hopefully, we don't need to use thread locks */
  return to;
}

int ha_xpand::external_lock(THD *thd, int lock_type)
{
  DBUG_ENTER("ha_xpand::external_lock()");
  int error_code;
  xpand_connection *trx = get_trx(thd, &error_code);
  if (error_code)
    DBUG_RETURN(error_code);

  if (lock_type == F_WRLCK)
    xpd_lock_type = XPAND_EXCLUSIVE;
  else if (lock_type == F_RDLCK)
    xpd_lock_type = XPAND_SHARED;
  else if (lock_type == F_UNLCK)
    xpd_lock_type = XPAND_NO_LOCKS;

  if (lock_type != F_UNLCK) {
    if (!trx->has_open_transaction()) {
      error_code = trx->begin_transaction_next();
      if (error_code)
        DBUG_RETURN(error_code);
    }

    trans_register_ha(thd, FALSE, xpand_hton);
    if (thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
      trans_register_ha(thd, TRUE, xpand_hton);
  }

  DBUG_RETURN(error_code);
}

/****************************************************************************
  Engine Condition Pushdown
****************************************************************************/

const COND *ha_xpand::cond_push(const COND *cond)
{
  THD *thd= ha_thd();
  if (!thd->lex->describe) {
    pushdown_cond_list.push_front(const_cast<COND*>(cond));
  }

  return NULL;
}

void ha_xpand::cond_pop()
{
  pushdown_cond_list.pop();
}

int ha_xpand::info_push(uint info_type, void *info)
{
  return 0;
}

ulonglong ha_xpand::get_table_oid()
{
    return xpand_table_oid;
}

/****************************************************************************
** Row encoding functions
****************************************************************************/

void add_current_table_to_rpl_table_list(rpl_group_info **_rgi, THD *thd,
                                         TABLE *table)
{
  if (*_rgi)
    return;

  Relay_log_info *rli = new Relay_log_info(FALSE);
  rli->sql_driver_thd = thd;

  rpl_group_info *rgi = new rpl_group_info(rli);
  *_rgi = rgi;
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

void remove_current_table_from_rpl_table_list(rpl_group_info *rgi)
{
  if (!rgi->tables_to_lock)
    return;

  rgi->tables_to_lock->m_tabledef.table_def::~table_def();
  rgi->tables_to_lock->m_tabledef_valid = FALSE;
  my_free(rgi->tables_to_lock);
  rgi->tables_to_lock_count--;
  rgi->tables_to_lock = NULL;
  delete rgi->rli;
  delete rgi;
}

void ha_xpand::build_key_packed_row(uint index, const uchar *buf,
                                    uchar *packed_key, size_t *packed_key_len)
{
  if (index == table->s->primary_key && has_hidden_key) {
    memcpy(packed_key, &last_hidden_key, sizeof(ulonglong));
    *packed_key_len = sizeof(ulonglong);
  } else {
    // make a row from the table
    table->mark_columns_used_by_index(index, &table->tmp_set);
    *packed_key_len = pack_row(table, &table->tmp_set, packed_key, buf);
  }
}

int unpack_row_to_buf(rpl_group_info *rgi, TABLE *table, uchar *data,
                      uchar const *const row_data, MY_BITMAP const *cols,
                      uchar const *const row_end)
{
  /* Since unpack_row can only write to record[0], if 'data' does not point to
     table->record[0], we must back it up and then restore it afterwards. */
  uchar const *current_row_end;
  ulong master_reclength;
  uchar *backup_row = NULL;
  if (data != table->record[0]) {
    /* See Update_rows_log_event::do_exec_row(rpl_group_info *rgi)
       and the definitions of store_record and restore_record. */
    backup_row = (uchar*) my_alloca(table->s->reclength);
    memcpy(backup_row, table->record[0], table->s->reclength);
    restore_record(table, record[data == table->record[1] ? 1 : 2]);
  }

  int error_code = unpack_row(rgi, table, table->s->fields, row_data, cols,
                              &current_row_end, &master_reclength, row_end);

  if (backup_row) {
    store_record(table, record[data == table->record[1] ? 1 : 2]);
    memcpy(table->record[0], backup_row, table->s->reclength);
    my_afree(backup_row);
  }

  return error_code;
}

/****************************************************************************
** Plugin Functions
****************************************************************************/

static int xpand_commit(handlerton *hton, THD *thd, bool all)
{
  xpand_connection* trx = (xpand_connection *) thd_get_ha_data(thd, hton);
  assert(trx);

  int error_code = 0;
  if (trx->has_open_transaction()) {
    if (all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
      error_code = trx->commit_transaction();
    else
      error_code = trx->new_statement_next();
  }

  return error_code;
}

static int xpand_rollback(handlerton *hton, THD *thd, bool all)
{
  xpand_connection* trx = (xpand_connection *) thd_get_ha_data(thd, hton);
  assert(trx);

  int error_code = 0;
  if (trx->has_open_transaction()) {
    if (all || !thd_test_options(thd, OPTION_NOT_AUTOCOMMIT | OPTION_BEGIN))
      error_code = trx->rollback_transaction();
    else
      error_code = trx->rollback_statement_next();
  }

  return error_code;
}

static handler* xpand_create_handler(handlerton *hton, TABLE_SHARE *table,
                                     MEM_ROOT *mem_root)
{
  return new (mem_root) ha_xpand(hton, table);
}

static int xpand_close_connection(handlerton* hton, THD* thd)
{
  xpand_connection* trx = (xpand_connection *) thd_get_ha_data(thd, hton);
  if (!trx)
    return 0; /* Transaction is not started */

  int error_code = xpand_rollback(xpand_hton, thd, TRUE);

  delete trx;

  return error_code;
}

static int xpand_panic(handlerton *hton, ha_panic_function type)
{
  return 0;
}

static bool xpand_show_status(handlerton *hton, THD *thd,
                              stat_print_fn *stat_print,
                              enum ha_stat_type stat_type)
{
  return FALSE;
}

static int xpand_discover_table_names(handlerton *hton, LEX_CSTRING *db,
                                      MY_DIR *dir,
                                      handlerton::discovered_list *result)
{
  xpand_connection *xpand_net = new xpand_connection();
  int error_code = xpand_net->connect();
  if (error_code) {
    if (error_code == HA_ERR_NO_CONNECTION)
      error_code = 0;
    goto err;
  }

  error_code = xpand_net->populate_table_list(db, result);

err:
  delete xpand_net;
  return error_code;
}

int xpand_discover_table(handlerton *hton, THD *thd, TABLE_SHARE *share)
{
  xpand_connection *xpand_net = new xpand_connection();
  int error_code = xpand_net->connect();
  if (error_code) {
    if (error_code == HA_ERR_NO_CONNECTION)
      error_code = HA_ERR_NO_SUCH_TABLE;
    goto err;
  }

  error_code = xpand_net->discover_table_details(&share->db, &share->table_name,
                                                 thd, share);

err:
  delete xpand_net;
  return error_code;
}

static int xpand_init(void *p)
{
  DBUG_ENTER("xpand_init");

  xpand_hton = (handlerton *) p;
  xpand_hton->flags = HTON_NO_FLAGS;
  xpand_hton->panic = xpand_panic;
  xpand_hton->close_connection = xpand_close_connection;
  xpand_hton->commit = xpand_commit;
  xpand_hton->rollback = xpand_rollback;
  xpand_hton->create = xpand_create_handler;
  xpand_hton->show_status = xpand_show_status;
  xpand_hton->discover_table_names = xpand_discover_table_names;
  xpand_hton->discover_table = xpand_discover_table;
  xpand_hton->create_select = create_xpand_select_handler;
  xpand_hton->create_derived = create_xpand_derived_handler;

  mysql_rwlock_init(key_xpand_hosts, &xpand_hosts_lock);
  mysql_rwlock_wrlock(&xpand_hosts_lock);
  xpand_hosts = static_cast<xpand_host_list*>(
                my_malloc(sizeof(xpand_host_list), MYF(MY_WME | MY_ZEROFILL)));
  int error_code = xpand_hosts->fill(xpand_hosts_str);
  if (error_code) {
    my_free(xpand_hosts);
    xpand_hosts = NULL;
  }
  mysql_rwlock_unlock(&xpand_hosts_lock);
  DBUG_RETURN(error_code);
}

static int xpand_deinit(void *p)
{
  DBUG_ENTER("xpand_deinit");
  mysql_rwlock_wrlock(&xpand_hosts_lock);
  xpand_hosts->empty();
  my_free(xpand_hosts);
  xpand_hosts = NULL;
  mysql_rwlock_destroy(&xpand_hosts_lock);
  DBUG_RETURN(0);
}

struct st_mysql_show_var xpand_status_vars[] =
{
  {NullS, NullS, SHOW_LONG}
};

static struct st_mysql_sys_var* xpand_system_variables[] =
{
  MYSQL_SYSVAR(connect_timeout),
  MYSQL_SYSVAR(read_timeout),
  MYSQL_SYSVAR(write_timeout),
  MYSQL_SYSVAR(balance_algorithm),
  MYSQL_SYSVAR(hosts),
  MYSQL_SYSVAR(username),
  MYSQL_SYSVAR(password),
  MYSQL_SYSVAR(port),
  MYSQL_SYSVAR(socket),
  MYSQL_SYSVAR(row_buffer),
  MYSQL_SYSVAR(select_handler),
  MYSQL_SYSVAR(derived_handler),
  MYSQL_SYSVAR(enable_direct_update),
  NULL
};

static struct st_mysql_storage_engine xpand_storage_engine =
  {MYSQL_HANDLERTON_INTERFACE_VERSION};

maria_declare_plugin(xpand)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,                /* Plugin Type */
  &xpand_storage_engine,                      /* Plugin Descriptor */
  "XPAND",                                    /* Plugin Name */
  "MariaDB",                                  /* Plugin Author */
  "Xpand storage engine",                     /* Plugin Description */
  PLUGIN_LICENSE_GPL,                         /* Plugin Licence */
  xpand_init,                                 /* Plugin Entry Point */
  xpand_deinit,                               /* Plugin Deinitializer */
  0x0001,                                     /* Hex Version Number (0.1) */
  NULL /* xpand_status_vars */,               /* Status Variables */
  xpand_system_variables,                     /* System Variables */
  "0.1",                                      /* String Version */
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL        /* Maturity Level */
}
maria_declare_plugin_end;
