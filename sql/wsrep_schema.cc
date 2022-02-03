/* Copyright (C) 2015-2021 Codership Oy <info@codership.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA. */

#include "mariadb.h"

#include "table.h"
#include "key.h"
#include "sql_base.h"
#include "sql_parse.h"
#include "sql_update.h"
#include "transaction.h"

#include "mysql/service_wsrep.h"
#include "wsrep_schema.h"
#include "wsrep_applier.h"
#include "wsrep_xid.h"
#include "wsrep_binlog.h"
#include "wsrep_high_priority_service.h"
#include "wsrep_storage_service.h"
#include "wsrep_thd.h"

#include <string>
#include <sstream>

#define WSREP_SCHEMA          "mysql"
#define WSREP_STREAMING_TABLE "wsrep_streaming_log"
#define WSREP_CLUSTER_TABLE   "wsrep_cluster"
#define WSREP_MEMBERS_TABLE   "wsrep_cluster_members"

const char* wsrep_sr_table_name_full= WSREP_SCHEMA "/" WSREP_STREAMING_TABLE;

static const std::string wsrep_schema_str= WSREP_SCHEMA;
static const std::string sr_table_str= WSREP_STREAMING_TABLE;
static const std::string cluster_table_str= WSREP_CLUSTER_TABLE;
static const std::string members_table_str= WSREP_MEMBERS_TABLE;

static const std::string create_cluster_table_str=
  "CREATE TABLE IF NOT EXISTS " + wsrep_schema_str + "." + cluster_table_str +
  "("
  "cluster_uuid CHAR(36) PRIMARY KEY,"
  "view_id BIGINT NOT NULL,"
  "view_seqno BIGINT NOT NULL,"
  "protocol_version INT NOT NULL,"
  "capabilities INT NOT NULL"
  ") ENGINE=InnoDB STATS_PERSISTENT=0";

static const std::string create_members_table_str=
  "CREATE TABLE IF NOT EXISTS " + wsrep_schema_str + "." + members_table_str +
  "("
  "node_uuid CHAR(36) PRIMARY KEY,"
  "cluster_uuid CHAR(36) NOT NULL,"
  "node_name CHAR(32) NOT NULL,"
  "node_incoming_address VARCHAR(256) NOT NULL"
  ") ENGINE=InnoDB STATS_PERSISTENT=0";

#ifdef WSREP_SCHEMA_MEMBERS_HISTORY
static const std::string cluster_member_history_table_str= "wsrep_cluster_member_history";
static const std::string create_members_history_table_str=
  "CREATE TABLE IF NOT EXISTS " + wsrep_schema_str + "." + cluster_member_history_table_str +
  "("
  "node_uuid CHAR(36) PRIMARY KEY,"
  "cluster_uuid CHAR(36) NOT NULL,"
  "last_view_id BIGINT NOT NULL,"
  "last_view_seqno BIGINT NOT NULL,"
  "node_name CHAR(32) NOT NULL,"
  "node_incoming_address VARCHAR(256) NOT NULL"
  ") ENGINE=InnoDB STATS_PERSISTENT=0";
#endif /* WSREP_SCHEMA_MEMBERS_HISTORY */

static const std::string create_frag_table_str=
  "CREATE TABLE IF NOT EXISTS " + wsrep_schema_str + "." + sr_table_str +
  "("
  "node_uuid CHAR(36), "
  "trx_id BIGINT, "
  "seqno BIGINT, "
  "flags INT NOT NULL, "
  "frag LONGBLOB NOT NULL, "
  "PRIMARY KEY (node_uuid, trx_id, seqno)"
  ") ENGINE=InnoDB STATS_PERSISTENT=0";

static const std::string delete_from_cluster_table=
  "DELETE FROM " + wsrep_schema_str + "." + cluster_table_str;

static const std::string delete_from_members_table=
  "DELETE FROM " + wsrep_schema_str + "." + members_table_str;

/* For rolling upgrade we need to use ALTER. We do not want
persistent statistics to be collected from these tables. */
static const std::string alter_cluster_table=
  "ALTER TABLE " + wsrep_schema_str + "." + cluster_table_str +
  " STATS_PERSISTENT=0";

static const std::string alter_members_table=
  "ALTER TABLE " + wsrep_schema_str + "." + members_table_str +
  " STATS_PERSISTENT=0";

#ifdef WSREP_SCHEMA_MEMBERS_HISTORY
static const std::string alter_members_history_table=
  "ALTER TABLE " + wsrep_schema_str + "." + members_history_table_str +
  " STATS_PERSISTENT=0";
#endif

static const std::string alter_frag_table=
  "ALTER TABLE " + wsrep_schema_str + "." + sr_table_str +
  " STATS_PERSISTENT=0";

namespace Wsrep_schema_impl
{

class binlog_off
{
public:
  binlog_off(THD* thd)
    : m_thd(thd)
    , m_option_bits(thd->variables.option_bits)
    , m_sql_log_bin(thd->variables.sql_log_bin)
  {
    thd->variables.option_bits&= ~OPTION_BIN_LOG;
    thd->variables.sql_log_bin= 0;
  }
  ~binlog_off()
  {
    m_thd->variables.option_bits= m_option_bits;
    m_thd->variables.sql_log_bin= m_sql_log_bin;
  }
private:
  THD* m_thd;
  ulonglong m_option_bits;
  my_bool m_sql_log_bin;
};

class wsrep_off
{
public:
  wsrep_off(THD* thd)
    : m_thd(thd)
    , m_wsrep_on(thd->variables.wsrep_on)
  {
    thd->variables.wsrep_on= 0;
  }
  ~wsrep_off()
  {
    m_thd->variables.wsrep_on= m_wsrep_on;
  }
private:
  THD* m_thd;
  my_bool m_wsrep_on;
};

class thd_server_status
{
public:
  thd_server_status(THD* thd, uint server_status, bool condition)
    : m_thd(thd)
    , m_thd_server_status(thd->server_status)
  {
    if (condition)
      thd->server_status= server_status;
  }
  ~thd_server_status()
  {
    m_thd->server_status= m_thd_server_status;
  }
private:
  THD* m_thd;
  uint m_thd_server_status;
};

class thd_context_switch
{
public:
  thd_context_switch(THD *orig_thd, THD *cur_thd)
    : m_orig_thd(orig_thd)
    , m_cur_thd(cur_thd)
  {
    wsrep_reset_threadvars(m_orig_thd);
    wsrep_store_threadvars(m_cur_thd);
  }
  ~thd_context_switch()
  {
    wsrep_reset_threadvars(m_cur_thd);
    wsrep_store_threadvars(m_orig_thd);
  }
private:
  THD *m_orig_thd;
  THD *m_cur_thd;
};

class sql_safe_updates
{
public:
  sql_safe_updates(THD* thd)
    : m_thd(thd)
    , m_option_bits(thd->variables.option_bits)
  {
    thd->variables.option_bits&= ~OPTION_SAFE_UPDATES;
  }
  ~sql_safe_updates()
  {
    m_thd->variables.option_bits= m_option_bits;
  }
private:
  THD* m_thd;
  ulonglong m_option_bits;
};

static int execute_SQL(THD* thd, const char* sql, uint length) {
  DBUG_ENTER("Wsrep_schema::execute_SQL()");
  int err= 0;

  PSI_statement_locker *parent_locker= thd->m_statement_psi;
  Parser_state parser_state;

  WSREP_DEBUG("SQL: %d %s thd: %lld", length, sql, (long long)thd->thread_id);

  if (parser_state.init(thd, (char*)sql, length) == 0) {
    thd->reset_for_next_command();
    lex_start(thd);

    thd->m_statement_psi= NULL;

    thd->set_query((char*)sql, length);
    thd->set_query_id(next_query_id());

    mysql_parse(thd, (char*)sql, length, & parser_state);

    if (thd->is_error()) {
      WSREP_WARN("Wsrep_schema::execute_sql() failed, %d %s\nSQL: %s",
                 thd->get_stmt_da()->sql_errno(),
                 thd->get_stmt_da()->message(),
                 sql);
      err= 1;
    }
    thd->m_statement_psi= parent_locker;
    thd->end_statement();
    thd->reset_query();
    close_thread_tables(thd);
    delete_explain_query(thd->lex);
  }
  else {
    WSREP_WARN("SR init failure");
  }
  thd->cleanup_after_query();
  DBUG_RETURN(err);
}

/*
  Initialize thd for next "statement"
 */
static void init_stmt(THD* thd) {
  thd->reset_for_next_command();
}

static void finish_stmt(THD* thd) {
  trans_commit_stmt(thd);
  close_thread_tables(thd);
}

static int open_table(THD* thd,
               const LEX_CSTRING *schema_name,
               const LEX_CSTRING *table_name,
               enum thr_lock_type const lock_type,
               TABLE** table) {
  assert(table);
  *table= NULL;

  DBUG_ENTER("Wsrep_schema::open_table()");

  TABLE_LIST tables;
  uint flags= (MYSQL_OPEN_IGNORE_GLOBAL_READ_LOCK |
               MYSQL_LOCK_IGNORE_GLOBAL_READ_ONLY |
               MYSQL_OPEN_IGNORE_FLUSH |
               MYSQL_LOCK_IGNORE_TIMEOUT);

  tables.init_one_table(schema_name,
                        table_name,
                        NULL, lock_type);
  thd->lex->query_tables_own_last= 0;

  // No need to open table if the query was bf aborted,
  // thd client will get ER_LOCK_DEADLOCK in the end.
  const bool interrupted= thd->killed ||
       (thd->is_error() &&
       (thd->get_stmt_da()->sql_errno() == ER_QUERY_INTERRUPTED));

  if (interrupted ||
      !open_n_lock_single_table(thd, &tables, tables.lock_type, flags)) {
    close_thread_tables(thd);
    DBUG_RETURN(1);
  }

  *table= tables.table;
  (*table)->use_all_columns();

  DBUG_RETURN(0);
}


static int open_for_write(THD* thd, const char* table_name, TABLE** table) {
  LEX_CSTRING schema_str= { wsrep_schema_str.c_str(), wsrep_schema_str.length() };
  LEX_CSTRING table_str= { table_name, strlen(table_name) };
  if (Wsrep_schema_impl::open_table(thd, &schema_str, &table_str, TL_WRITE,
                                    table)) {
    // No need to log an error if the query was bf aborted,
    // thd client will get ER_LOCK_DEADLOCK in the end.
    const bool interrupted= thd->killed ||
      (thd->is_error() &&
       (thd->get_stmt_da()->sql_errno() == ER_QUERY_INTERRUPTED));
    if (!interrupted) {
      WSREP_ERROR("Failed to open table %s.%s for writing",
                  schema_str.str, table_name);
    }
    return 1;
  }
  empty_record(*table);
  (*table)->use_all_columns();
  restore_record(*table, s->default_values);
  return 0;
}

static void store(TABLE* table, uint field, const Wsrep_id& id) {
  assert(field < table->s->fields);
  std::ostringstream os;
  os << id;
  table->field[field]->store(os.str().c_str(),
                             os.str().size(),
                             &my_charset_bin);
}


template <typename INTTYPE>
static void store(TABLE* table, uint field, const INTTYPE val) {
  assert(field < table->s->fields);
  table->field[field]->store(val);
}

template <typename CHARTYPE>
static void store(TABLE* table, uint field, const CHARTYPE* str, size_t str_len) {
  assert(field < table->s->fields);
  table->field[field]->store((const char*)str,
                             str_len,
                             &my_charset_bin);
}

static void store(TABLE* table, uint field, const std::string& str)
{
  store(table, field, str.c_str(), str.size());
}

static int update_or_insert(TABLE* table) {
  DBUG_ENTER("Wsrep_schema::update_or_insert()");
  int ret= 0;
  char* key;
  int error;

  /*
    Verify that the table has primary key defined.
  */
  if (table->s->primary_key >= MAX_KEY ||
      !table->s->keys_in_use.is_set(table->s->primary_key)) {
    WSREP_ERROR("No primary key for %s.%s",
                table->s->db.str, table->s->table_name.str);
    DBUG_RETURN(1);
  }

  /*
    Find the record and update or insert a new one if not found.
  */
  if (!(key= (char*) my_safe_alloca(table->s->max_unique_length))) {
    WSREP_ERROR("Error allocating %ud bytes for key",
                table->s->max_unique_length);
    DBUG_RETURN(1);
  }

  key_copy((uchar*) key, table->record[0],
           table->key_info + table->s->primary_key, 0);

  if ((error= table->file->ha_index_read_idx_map(table->record[1],
                                                 table->s->primary_key,
                                                 (uchar*) key,
                                                 HA_WHOLE_KEY,
                                                 HA_READ_KEY_EXACT))) {
    /*
      Row not found, insert a new one.
    */
    if ((error= table->file->ha_write_row(table->record[0]))) {
      WSREP_ERROR("Error writing into %s.%s: %d",
                  table->s->db.str,
                  table->s->table_name.str,
                  error);
      ret= 1;
    }
  }
  else if (!records_are_comparable(table) || compare_record(table)) {
    /*
      Record has changed
    */
    if ((error= table->file->ha_update_row(table->record[1],
                                           table->record[0])) &&
        error != HA_ERR_RECORD_IS_THE_SAME) {
      WSREP_ERROR("Error updating record in %s.%s: %d",
                  table->s->db.str,
                  table->s->table_name.str,
                  error);
      ret= 1;
    }
  }

  my_safe_afree(key, table->s->max_unique_length);

  DBUG_RETURN(ret);
}

static int insert(TABLE* table) {
  DBUG_ENTER("Wsrep_schema::insert()");
  int ret= 0;
  int error;

  /*
    Verify that the table has primary key defined.
  */
  if (table->s->primary_key >= MAX_KEY ||
      !table->s->keys_in_use.is_set(table->s->primary_key)) {
    WSREP_ERROR("No primary key for %s.%s",
                table->s->db.str, table->s->table_name.str);
    DBUG_RETURN(1);
  }

  if ((error= table->file->ha_write_row(table->record[0]))) {
    WSREP_ERROR("Error writing into %s.%s: %d",
                table->s->db.str,
                table->s->table_name.str,
                error);
    ret= 1;
  }

  DBUG_RETURN(ret);
}

static int delete_row(TABLE* table) {
  int error;
  int retry= 3;

  do {
    error= table->file->ha_delete_row(table->record[0]);
    retry--;
  } while (error && retry);

  if (error) {
    WSREP_ERROR("Error deleting row from %s.%s: %d",
                table->s->db.str,
                table->s->table_name.str,
                error);
    return 1;
  }
  return 0;
}

static int open_for_read(THD* thd, const char* table_name, TABLE** table) {

  LEX_CSTRING schema_str= { wsrep_schema_str.c_str(), wsrep_schema_str.length() };
  LEX_CSTRING table_str= { table_name, strlen(table_name) };
  if (Wsrep_schema_impl::open_table(thd, &schema_str, &table_str, TL_READ,
                                    table)) {
    WSREP_ERROR("Failed to open table %s.%s for reading",
                schema_str.str, table_name);
    return 1;
  }
  empty_record(*table);
  (*table)->use_all_columns();
  restore_record(*table, s->default_values);
  return 0;
}

/*
  Init table for sequential scan.

  @return 0 in case of success, 1 in case of error.
 */
static int init_for_scan(TABLE* table) {
  int error;
  if ((error= table->file->ha_rnd_init(TRUE))) {
    WSREP_ERROR("Failed to init table for scan: %d", error);
    return 1;
  }
  return 0;
}
/*
  Scan next record. For return codes see handler::ha_rnd_next()

  @return 0 in case of success, error code in case of error
 */
static int next_record(TABLE* table) {
  int error;
  if ((error= table->file->ha_rnd_next(table->record[0])) &&
      error != HA_ERR_END_OF_FILE) {
    WSREP_ERROR("Failed to read next record: %d", error);
  }
  return error;
}

/*
  End scan.

  @return 0 in case of success, 1 in case of error.
 */
static int end_scan(TABLE* table) {
  int error;
  if ((error= table->file->ha_rnd_end())) {
    WSREP_ERROR("Failed to end scan: %d", error);
    return 1;
  }
  return 0;
}

static int scan(TABLE* table, uint field, wsrep::id& id)
{
  assert(field < table->s->fields);
  String uuid_str;
  (void)table->field[field]->val_str(&uuid_str);
  id= wsrep::id(std::string(uuid_str.c_ptr(), uuid_str.length()));
  return 0;
}

template <typename INTTYPE>
static int scan(TABLE* table, uint field, INTTYPE& val)
{
  assert(field < table->s->fields);
  val= table->field[field]->val_int();
  return 0;
}

static int scan(TABLE* table, uint field, char* strbuf, uint strbuf_len)
{
  uint len;
  StringBuffer<STRING_BUFFER_USUAL_SIZE> str;
  (void) table->field[field]->val_str(&str);
  len= str.length();
  strmake(strbuf, str.ptr(), MY_MIN(len, strbuf_len-1));
  return 0;
}

/*
  Scan member
  TODO: filter members by cluster UUID
 */
static int scan_member(TABLE* table,
                       const Wsrep_id& cluster_uuid,
                       std::vector<Wsrep_view::member>& members)
{
  Wsrep_id member_id;
  char member_name[128]= { 0, };
  char member_incoming[128]= { 0, };

  if (scan(table, 0, member_id) ||
      scan(table, 2, member_name, sizeof(member_name)) ||
      scan(table, 3, member_incoming, sizeof(member_incoming))) {
    return 1;
  }

  if (members.empty() == false) {
    assert(members.rbegin()->id() < member_id);
  }

  try {
    members.push_back(Wsrep_view::member(member_id,
                                         member_name,
                                         member_incoming));
  }
  catch (...) {
    WSREP_ERROR("Caught exception while scanning members table");
    return 1;
  }
  return 0;
}

/*
  Init table for index scan and retrieve first record

  @return 0 in case of success, error code in case of error.
 */
static int init_for_index_scan(TABLE* table, const uchar* key,
                               key_part_map map) {
  int error;
  if ((error= table->file->ha_index_init(table->s->primary_key, true))) {
    WSREP_ERROR("Failed to init table for index scan: %d", error);
    return error;
  }

  error= table->file->ha_index_read_map(table->record[0],
                                        key, map, HA_READ_KEY_EXACT);
  switch(error) {
  case 0:
  case HA_ERR_END_OF_FILE:
  case HA_ERR_KEY_NOT_FOUND:
  case HA_ERR_ABORTED_BY_USER:
    break;
  case -1:
    WSREP_DEBUG("init_for_index_scan interrupted");
    break;
  default:
    WSREP_ERROR("init_for_index_scan failed to read first record, error %d", error);
  }
  return error;
}

/*
  End index scan.

  @return 0 in case of success, 1 in case of error.
 */
static int end_index_scan(TABLE* table) {
  int error;
  if (table->file->inited) {
    if ((error= table->file->ha_index_end())) {
      WSREP_ERROR("Failed to end scan: %d", error);
      return 1;
    }
  }
  return 0;
}

static void make_key(TABLE* table, uchar** key, key_part_map* map, int parts) {
  uint prefix_length= 0;
  KEY_PART_INFO* key_part= table->key_info->key_part;

  for (int i=0; i < parts; i++)
    prefix_length += key_part[i].store_length;

  *map= make_prev_keypart_map(parts);

  if (!(*key= (uchar *) my_malloc(PSI_NOT_INSTRUMENTED, prefix_length + 1, MYF(MY_WME))))
  {
    WSREP_ERROR("Failed to allocate memory for key prefix_length %u", prefix_length);
    assert(0);
  }

  key_copy(*key, table->record[0], table->key_info, prefix_length);
}

} /* namespace Wsrep_schema_impl */


Wsrep_schema::Wsrep_schema()
{
}

Wsrep_schema::~Wsrep_schema()
{ }

static void wsrep_init_thd_for_schema(THD *thd)
{
  thd->security_ctx->skip_grants();
  thd->system_thread= SYSTEM_THREAD_GENERIC;

  thd->real_id=pthread_self(); // Keep purify happy

  thd->prior_thr_create_utime= thd->start_utime= thd->thr_create_utime;

  /* No Galera replication */
  thd->variables.wsrep_on= 0;
  /* No binlogging */
  thd->variables.sql_log_bin= 0;
  thd->variables.option_bits&= ~OPTION_BIN_LOG;
  /* No safe updates */
  thd->variables.option_bits&= ~OPTION_SAFE_UPDATES;
  /* No general log */
  thd->variables.option_bits|= OPTION_LOG_OFF;
  /* Read committed isolation to avoid gap locking */
  thd->variables.tx_isolation= ISO_READ_COMMITTED;
  wsrep_assign_from_threadvars(thd);
  wsrep_store_threadvars(thd);
}

int Wsrep_schema::init()
{
  DBUG_ENTER("Wsrep_schema::init()");
  int ret;
  THD* thd= new THD(next_thread_id());
  if (!thd) {
    WSREP_ERROR("Unable to get thd");
    DBUG_RETURN(1);
  }
  thd->thread_stack= (char*)&thd;
  wsrep_init_thd_for_schema(thd);

  if (Wsrep_schema_impl::execute_SQL(thd, create_cluster_table_str.c_str(),
                                     create_cluster_table_str.size()) ||
      Wsrep_schema_impl::execute_SQL(thd, create_members_table_str.c_str(),
                                     create_members_table_str.size()) ||
#ifdef WSREP_SCHEMA_MEMBERS_HISTORY
      Wsrep_schema_impl::execute_SQL(thd,
                                     create_members_history_table_str.c_str(),
                                     create_members_history_table_str.size()) ||
      Wsrep_schema_impl::execute_SQL(thd,
                                     alter_members_history_table.c_str(),
                                     alter_members_history_table.size()) ||
#endif /* WSREP_SCHEMA_MEMBERS_HISTORY */
      Wsrep_schema_impl::execute_SQL(thd,
                                     create_frag_table_str.c_str(),
                                     create_frag_table_str.size()) ||
      Wsrep_schema_impl::execute_SQL(thd,
                                     alter_cluster_table.c_str(),
                                     alter_cluster_table.size()) ||
      Wsrep_schema_impl::execute_SQL(thd,
                                     alter_members_table.c_str(),
                                     alter_members_table.size()) ||
      Wsrep_schema_impl::execute_SQL(thd,
                                     alter_frag_table.c_str(),
	                             alter_frag_table.size()))
  {
    ret= 1;
  }
  else
  {
    ret= 0;
  }

  delete thd;
  DBUG_RETURN(ret);
}

int Wsrep_schema::store_view(THD* thd, const Wsrep_view& view)
{
  DBUG_ENTER("Wsrep_schema::store_view()");
  assert(view.status() == Wsrep_view::primary);
  int ret= 1;
  int error;
  TABLE* cluster_table= 0;
  TABLE* members_table= 0;
#ifdef WSREP_SCHEMA_MEMBERS_HISTORY
  TABLE* members_history_table= 0;
#endif /* WSREP_SCHEMA_MEMBERS_HISTORY */

  Wsrep_schema_impl::wsrep_off wsrep_off(thd);
  Wsrep_schema_impl::binlog_off binlog_off(thd);
  Wsrep_schema_impl::sql_safe_updates sql_safe_updates(thd);

  /*
    Clean up cluster table and members table.
  */
  if (Wsrep_schema_impl::execute_SQL(thd,
                                     delete_from_cluster_table.c_str(),
                                     delete_from_cluster_table.size()) ||
      Wsrep_schema_impl::execute_SQL(thd,
                                     delete_from_members_table.c_str(),
                                     delete_from_members_table.size())) {
    goto out;
  }

  /*
    Store cluster view info
  */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_write(thd, cluster_table_str.c_str(), &cluster_table))
  {
    goto out;
  }

  Wsrep_schema_impl::store(cluster_table, 0, view.state_id().id());
  Wsrep_schema_impl::store(cluster_table, 1, view.view_seqno().get());
  Wsrep_schema_impl::store(cluster_table, 2, view.state_id().seqno().get());
  Wsrep_schema_impl::store(cluster_table, 3, view.protocol_version());
  Wsrep_schema_impl::store(cluster_table, 4, view.capabilities());

  if ((error= Wsrep_schema_impl::update_or_insert(cluster_table)))
  {
    WSREP_ERROR("failed to write to cluster table: %d", error);
    goto out;
  }

  Wsrep_schema_impl::finish_stmt(thd);

  /*
    Store info about current members
  */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_write(thd, members_table_str.c_str(),
                                        &members_table))
  {
    WSREP_ERROR("failed to open wsrep.members table");
    goto out;
  }

  for (size_t i= 0; i < view.members().size(); ++i)
  {
    Wsrep_schema_impl::store(members_table, 0, view.members()[i].id());
    Wsrep_schema_impl::store(members_table, 1, view.state_id().id());
    Wsrep_schema_impl::store(members_table, 2, view.members()[i].name());
    Wsrep_schema_impl::store(members_table, 3, view.members()[i].incoming());
    if ((error= Wsrep_schema_impl::update_or_insert(members_table)))
    {
      WSREP_ERROR("failed to write wsrep.members table: %d", error);
      goto out;
    }
  }
  Wsrep_schema_impl::finish_stmt(thd);

#ifdef WSREP_SCHEMA_MEMBERS_HISTORY
  /*
    Store members history
  */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_write(thd, cluster_member_history.c_str(),
                                        &members_history_table)) {
    WSREP_ERROR("failed to open wsrep.members table");
    goto out;
  }

  for (size_t i= 0; i < view.members().size(); ++i) {
    Wsrep_schema_impl::store(members_history_table, 0, view.members()[i].id());
    Wsrep_schema_impl::store(members_history_table, 1, view.state_id().id());
    Wsrep_schema_impl::store(members_history_table, 2, view.view_seqno());
    Wsrep_schema_impl::store(members_history_table, 3, view.state_id().seqno());
    Wsrep_schema_impl::store(members_history_table, 4,
                             view.members()[i].name());
    Wsrep_schema_impl::store(members_history_table, 5,
                             view.members()[i].incoming());
    if ((error= Wsrep_schema_impl::update_or_insert(members_history_table))) {
      WSREP_ERROR("failed to write wsrep_cluster_member_history table: %d", error);
      goto out;
    }
  }
  Wsrep_schema_impl::finish_stmt(thd);
#endif /* WSREP_SCHEMA_MEMBERS_HISTORY */
  ret= 0;
 out:

  DBUG_RETURN(ret);
}

Wsrep_view Wsrep_schema::restore_view(THD* thd, const Wsrep_id& own_id) const {
  DBUG_ENTER("Wsrep_schema::restore_view()");

  int ret= 1;
  int error;

  TABLE* cluster_table= 0;
  bool end_cluster_scan= false;
  TABLE* members_table= 0;
  bool end_members_scan= false;

  /* variables below need to be initialized in case cluster table is empty */
  Wsrep_id cluster_uuid;
  wsrep_seqno_t view_id= -1;
  wsrep_seqno_t view_seqno= -1;
  int my_idx= -1;
  int proto_ver= 0;
  wsrep_cap_t capabilities= 0;
  std::vector<Wsrep_view::member> members;

  // we don't want causal waits for reading non-replicated private data
  int const wsrep_sync_wait_saved= thd->variables.wsrep_sync_wait;
  thd->variables.wsrep_sync_wait= 0;

  if (trans_begin(thd, MYSQL_START_TRANS_OPT_READ_ONLY)) {
    WSREP_ERROR("wsrep_schema::restore_view(): Failed to start transaction");
    goto out;
  }

  /*
    Read cluster info from cluster table
   */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_read(thd, cluster_table_str.c_str(), &cluster_table) ||
      Wsrep_schema_impl::init_for_scan(cluster_table)) {
    goto out;
  }

  if (((error= Wsrep_schema_impl::next_record(cluster_table)) != 0 ||
       Wsrep_schema_impl::scan(cluster_table, 0, cluster_uuid) ||
       Wsrep_schema_impl::scan(cluster_table, 1, view_id) ||
       Wsrep_schema_impl::scan(cluster_table, 2, view_seqno) ||
       Wsrep_schema_impl::scan(cluster_table, 3, proto_ver) ||
       Wsrep_schema_impl::scan(cluster_table, 4, capabilities)) &&
      error != HA_ERR_END_OF_FILE) {
    end_cluster_scan= true;
    goto out;
  }

  if (Wsrep_schema_impl::end_scan(cluster_table)) {
    goto out;
  }
  Wsrep_schema_impl::finish_stmt(thd);

  /*
    Read members from members table
  */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_read(thd, members_table_str.c_str(), &members_table) ||
      Wsrep_schema_impl::init_for_scan(members_table)) {
    goto out;
  }
  end_members_scan= true;

  while (true) {
    if ((error= Wsrep_schema_impl::next_record(members_table)) == 0) {
      if (Wsrep_schema_impl::scan_member(members_table,
                                         cluster_uuid,
                                         members)) {
        goto out;
      }
    }
    else if (error == HA_ERR_END_OF_FILE) {
      break;
    }
    else {
      goto out;
    }
  }

  end_members_scan= false;
  if (Wsrep_schema_impl::end_scan(members_table)) {
    goto out;
  }
  Wsrep_schema_impl::finish_stmt(thd);

  if (own_id.is_undefined() == false) {
    for (uint i= 0; i < members.size(); ++i) {
      if (members[i].id() == own_id) {
        my_idx= i;
        break;
      }
    }
  }

  (void)trans_commit(thd);
  ret= 0; /* Success*/
 out:

  if (end_cluster_scan) Wsrep_schema_impl::end_scan(cluster_table);
  if (end_members_scan) Wsrep_schema_impl::end_scan(members_table);

  if (0 != ret) {
    trans_rollback_stmt(thd);
    if (!trans_rollback(thd)) {
      close_thread_tables(thd);
    }
  }
  thd->release_transactional_locks();

  thd->variables.wsrep_sync_wait= wsrep_sync_wait_saved;

  if (0 == ret) {
    Wsrep_view ret_view(
      wsrep::gtid(cluster_uuid, Wsrep_seqno(view_seqno)),
      Wsrep_seqno(view_id),
      wsrep::view::primary,
      capabilities,
      my_idx,
      proto_ver,
      members
    );

    if (wsrep_debug) {
      std::ostringstream os;
      os << "Restored cluster view:\n" << ret_view;
      WSREP_INFO("%s", os.str().c_str());
    }
    DBUG_RETURN(ret_view);
  }
  else
  {
    WSREP_ERROR("wsrep_schema::restore_view() failed.");
    Wsrep_view ret_view;
    DBUG_RETURN(ret_view);
  }
}

int Wsrep_schema::append_fragment(THD* thd,
                                  const wsrep::id& server_id,
                                  wsrep::transaction_id transaction_id,
                                  wsrep::seqno seqno,
                                  int flags,
                                  const wsrep::const_buffer& data)
{
  DBUG_ENTER("Wsrep_schema::append_fragment");
  std::ostringstream os;
  os << server_id;
  WSREP_DEBUG("Append fragment(%llu) %s, %llu",
              thd->thread_id,
              os.str().c_str(),
              transaction_id.get());
  /* use private query table list for the duration of fragment storing,
     populated query table list from "parent DML" may cause problems .e.g
     for virtual column handling
 */
  Query_tables_list query_tables_list_backup;
  thd->lex->reset_n_backup_query_tables_list(&query_tables_list_backup);

  Wsrep_schema_impl::binlog_off binlog_off(thd);
  Wsrep_schema_impl::sql_safe_updates sql_safe_updates(thd);
  Wsrep_schema_impl::init_stmt(thd);

  TABLE* frag_table= 0;
  if (Wsrep_schema_impl::open_for_write(thd, sr_table_str.c_str(), &frag_table))
  {
    trans_rollback_stmt(thd);
    thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
    DBUG_RETURN(1);
  }

  Wsrep_schema_impl::store(frag_table, 0, server_id);
  Wsrep_schema_impl::store(frag_table, 1, transaction_id.get());
  Wsrep_schema_impl::store(frag_table, 2, seqno.get());
  Wsrep_schema_impl::store(frag_table, 3, flags);
  Wsrep_schema_impl::store(frag_table, 4, data.data(), data.size());

  int error;
  if ((error= Wsrep_schema_impl::insert(frag_table))) {
    WSREP_ERROR("Failed to write to frag table: %d", error);
    trans_rollback_stmt(thd);
    thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
    DBUG_RETURN(1);
  }
  Wsrep_schema_impl::finish_stmt(thd);
  thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
  DBUG_RETURN(0);
}

int Wsrep_schema::update_fragment_meta(THD* thd,
                                       const wsrep::ws_meta& ws_meta)
{
  DBUG_ENTER("Wsrep_schema::update_fragment_meta");
  std::ostringstream os;
  os << ws_meta.server_id();
  WSREP_DEBUG("update_frag_seqno(%llu) %s, %llu, seqno %lld",
              thd->thread_id,
              os.str().c_str(),
              ws_meta.transaction_id().get(),
              ws_meta.seqno().get());
  DBUG_ASSERT(ws_meta.seqno().is_undefined() == false);

  /* use private query table list for the duration of fragment storing,
     populated query table list from "parent DML" may cause problems .e.g
     for virtual column handling
 */
  Query_tables_list query_tables_list_backup;
  thd->lex->reset_n_backup_query_tables_list(&query_tables_list_backup);

  Wsrep_schema_impl::binlog_off binlog_off(thd);
  Wsrep_schema_impl::sql_safe_updates sql_safe_updates(thd);
  int error;
  uchar *key=NULL;
  key_part_map key_map= 0;
  TABLE* frag_table= 0;

  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_write(thd, sr_table_str.c_str(), &frag_table))
  {
    thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
    DBUG_RETURN(1);
  }

  /* Find record with the given uuid, trx id, and seqno -1 */
  Wsrep_schema_impl::store(frag_table, 0, ws_meta.server_id());
  Wsrep_schema_impl::store(frag_table, 1, ws_meta.transaction_id().get());
  Wsrep_schema_impl::store(frag_table, 2, -1);
  Wsrep_schema_impl::make_key(frag_table, &key, &key_map, 3);

  if ((error= Wsrep_schema_impl::init_for_index_scan(frag_table,
                                                     key, key_map)))
  {
    if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND)
    {
      WSREP_WARN("Record not found in %s.%s: %d",
                 frag_table->s->db.str,
                 frag_table->s->table_name.str,
                 error);
    }
    Wsrep_schema_impl::finish_stmt(thd);
    thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
    my_free(key);
    DBUG_RETURN(1);
  }

  my_free(key);
  /* Copy the original record to frag_table->record[1] */
  store_record(frag_table, record[1]);

  /* Store seqno in frag_table->record[0] and update the row */
  Wsrep_schema_impl::store(frag_table, 2, ws_meta.seqno().get());
  if ((error= frag_table->file->ha_update_row(frag_table->record[1],
                                              frag_table->record[0]))) {
    WSREP_ERROR("Error updating record in %s.%s: %d",
                frag_table->s->db.str,
                frag_table->s->table_name.str,
                error);
    Wsrep_schema_impl::finish_stmt(thd);
    thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
    DBUG_RETURN(1);
  }

  int ret= Wsrep_schema_impl::end_index_scan(frag_table);
  Wsrep_schema_impl::finish_stmt(thd);
  thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);
  DBUG_RETURN(ret);
}

static int remove_fragment(THD*                  thd,
                           TABLE*                frag_table,
                           const wsrep::id&      server_id,
                           wsrep::transaction_id transaction_id,
                           wsrep::seqno          seqno)
{
  WSREP_DEBUG("remove_fragment(%llu) trx %llu, seqno %lld",
              thd->thread_id,
              transaction_id.get(),
              seqno.get());
  int ret= 0;
  int error;
  uchar *key= NULL;
  key_part_map key_map= 0;

  DBUG_ASSERT(server_id.is_undefined() == false);
  DBUG_ASSERT(transaction_id.is_undefined() == false);
  DBUG_ASSERT(seqno.is_undefined() == false);

  /*
    Remove record with the given uuid, trx id, and seqno.
    Using a complete key here avoids gap locks.
  */
  Wsrep_schema_impl::store(frag_table, 0, server_id);
  Wsrep_schema_impl::store(frag_table, 1, transaction_id.get());
  Wsrep_schema_impl::store(frag_table, 2, seqno.get());
  Wsrep_schema_impl::make_key(frag_table, &key, &key_map, 3);

  if ((error= Wsrep_schema_impl::init_for_index_scan(frag_table,
                                                     key,
                                                     key_map)))
  {
    if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND)
    {
      WSREP_DEBUG("Record not found in %s.%s:trx %llu, seqno %lld, error %d",
                 frag_table->s->db.str,
                 frag_table->s->table_name.str,
                 transaction_id.get(),
                 seqno.get(),
                 error);
    }
    ret= error;
  }
  else if (Wsrep_schema_impl::delete_row(frag_table))
  {
    ret= 1;
  }

  if (key)
    my_free(key);
  Wsrep_schema_impl::end_index_scan(frag_table);
  return ret;
}

int Wsrep_schema::remove_fragments(THD* thd,
                                   const wsrep::id& server_id,
                                   wsrep::transaction_id transaction_id,
                                   const std::vector<wsrep::seqno>& fragments)
{
  DBUG_ENTER("Wsrep_schema::remove_fragments");
  int ret= 0;

  WSREP_DEBUG("Removing %zu fragments", fragments.size());
  Wsrep_schema_impl::wsrep_off  wsrep_off(thd);
  Wsrep_schema_impl::binlog_off binlog_off(thd);
  Wsrep_schema_impl::sql_safe_updates sql_safe_updates(thd);

  Query_tables_list query_tables_list_backup;
  Open_tables_backup open_tables_backup;
  thd->lex->reset_n_backup_query_tables_list(&query_tables_list_backup);
  thd->reset_n_backup_open_tables_state(&open_tables_backup);

  TABLE* frag_table= 0;
  if (Wsrep_schema_impl::open_for_write(thd, sr_table_str.c_str(), &frag_table))
  {
    ret= 1;
  }
  else
  {
    for (std::vector<wsrep::seqno>::const_iterator i= fragments.begin();
         i != fragments.end(); ++i)
    {
      if (remove_fragment(thd,
                          frag_table,
                          server_id,
                          transaction_id, *i))
      {
        ret= 1;
        break;
      }
    }
  }
  close_thread_tables(thd);
  thd->restore_backup_open_tables_state(&open_tables_backup);
  thd->lex->restore_backup_query_tables_list(&query_tables_list_backup);

  if (thd->wsrep_cs().mode() == wsrep::client_state::m_local &&
      !thd->in_multi_stmt_transaction_mode())
  {
    /*
      The ugly part: Locally executing autocommit statement is
      committing and it has removed a fragment from stable storage.
      Now calling finish_stmt() will call trans_commit_stmt(), which will
      actually commit the transaction, what we really don't want
      to do at this point.

      Doing nothing at this point seems to work ok, this block is
      intentionally no-op and for documentation purposes only.
    */
  }
  else
  {
    Wsrep_schema_impl::thd_server_status
      thd_server_status(thd, thd->server_status | SERVER_STATUS_IN_TRANS,
                        thd->in_multi_stmt_transaction_mode());
    Wsrep_schema_impl::finish_stmt(thd);
  }

  DBUG_RETURN(ret);
}

int Wsrep_schema::replay_transaction(THD* orig_thd,
                                     Relay_log_info* rli,
                                     const wsrep::ws_meta& ws_meta,
                                     const std::vector<wsrep::seqno>& fragments)
{
  DBUG_ENTER("Wsrep_schema::replay_transaction");
  DBUG_ASSERT(!fragments.empty());

  THD thd(next_thread_id(), true);
  thd.thread_stack= (orig_thd ? orig_thd->thread_stack :
                     (char*) &thd);
  wsrep_assign_from_threadvars(&thd);

  Wsrep_schema_impl::wsrep_off  wsrep_off(&thd);
  Wsrep_schema_impl::binlog_off binlog_off(&thd);
  Wsrep_schema_impl::sql_safe_updates sql_safe_updates(&thd);
  Wsrep_schema_impl::thd_context_switch thd_context_switch(orig_thd, &thd);

  int ret= 1;
  int error;
  TABLE* frag_table= 0;
  uchar *key=NULL;
  key_part_map key_map= 0;

  for (std::vector<wsrep::seqno>::const_iterator i= fragments.begin();
       i != fragments.end(); ++i)
  {
    Wsrep_schema_impl::init_stmt(&thd);
    if ((error= Wsrep_schema_impl::open_for_read(&thd, sr_table_str.c_str(), &frag_table)))
    {
      WSREP_WARN("Could not open SR table for read: %d", error);
      Wsrep_schema_impl::finish_stmt(&thd);
      DBUG_RETURN(1);
    }

    Wsrep_schema_impl::store(frag_table, 0, ws_meta.server_id());
    Wsrep_schema_impl::store(frag_table, 1, ws_meta.transaction_id().get());
    Wsrep_schema_impl::store(frag_table, 2, i->get());
    Wsrep_schema_impl::make_key(frag_table, &key, &key_map, 3);

    int error= Wsrep_schema_impl::init_for_index_scan(frag_table,
                                                      key,
                                                      key_map);
    if (error)
    {
      WSREP_WARN("Failed to init streaming log table for index scan: %d",
                 error);
      Wsrep_schema_impl::end_index_scan(frag_table);
      ret= 1;
      break;
    }

    int flags;
    Wsrep_schema_impl::scan(frag_table, 3, flags);
    WSREP_DEBUG("replay_fragment(%llu): seqno: %lld flags: %x",
                ws_meta.transaction_id().get(),
                i->get(),
                flags);
    String buf;
    frag_table->field[4]->val_str(&buf);

    {
      Wsrep_schema_impl::thd_context_switch thd_context_switch(&thd, orig_thd);

      ret= wsrep_apply_events(orig_thd, rli, buf.ptr(), buf.length());
      if (ret)
      {
        WSREP_WARN("Wsrep_schema::replay_transaction: failed to apply fragments");
        break;
      }
    }

    Wsrep_schema_impl::end_index_scan(frag_table);
    Wsrep_schema_impl::finish_stmt(&thd);

    Wsrep_schema_impl::init_stmt(&thd);

    if ((error= Wsrep_schema_impl::open_for_write(&thd,
                                                  sr_table_str.c_str(),
                                                  &frag_table)))
    {
      WSREP_WARN("Could not open SR table for write: %d", error);
      Wsrep_schema_impl::finish_stmt(&thd);
      DBUG_RETURN(1);
    }

    error= Wsrep_schema_impl::init_for_index_scan(frag_table,
                                                  key,
                                                  key_map);
    if (error)
    {
      WSREP_WARN("Failed to init streaming log table for index scan: %d",
                 error);
      Wsrep_schema_impl::end_index_scan(frag_table);
      ret= 1;
      break;
    }

    error= Wsrep_schema_impl::delete_row(frag_table);

    if (error)
    {
      WSREP_WARN("Could not delete row from streaming log table: %d", error);
      Wsrep_schema_impl::end_index_scan(frag_table);
      ret= 1;
      break;
    }
    Wsrep_schema_impl::end_index_scan(frag_table);
    Wsrep_schema_impl::finish_stmt(&thd);
    my_free(key);
    key= NULL;
  }

  if (key)
    my_free(key);
  DBUG_RETURN(ret);
}

int Wsrep_schema::recover_sr_transactions(THD *orig_thd)
{
  DBUG_ENTER("Wsrep_schema::recover_sr_transactions");
  THD storage_thd(next_thread_id(), true);
  storage_thd.thread_stack= (orig_thd ? orig_thd->thread_stack :
                             (char*) &storage_thd);
  wsrep_assign_from_threadvars(&storage_thd);
  TABLE* frag_table= 0;
  TABLE* cluster_table= 0;
  Wsrep_storage_service storage_service(&storage_thd);
  Wsrep_schema_impl::binlog_off binlog_off(&storage_thd);
  Wsrep_schema_impl::wsrep_off wsrep_off(&storage_thd);
  Wsrep_schema_impl::sql_safe_updates sql_safe_updates(&storage_thd);
  Wsrep_schema_impl::thd_context_switch thd_context_switch(orig_thd,
                                                           &storage_thd);
  Wsrep_server_state& server_state(Wsrep_server_state::instance());

  int ret= 1;
  int error;
  wsrep::id cluster_id;

  Wsrep_schema_impl::init_stmt(&storage_thd);
  storage_thd.wsrep_skip_locking= FALSE;
  if (Wsrep_schema_impl::open_for_read(&storage_thd,
                                       cluster_table_str.c_str(),
                                       &cluster_table) ||
      Wsrep_schema_impl::init_for_scan(cluster_table))
  {
    Wsrep_schema_impl::finish_stmt(&storage_thd);
    DBUG_RETURN(1);
  }

  if ((error= Wsrep_schema_impl::next_record(cluster_table)))
  {
    Wsrep_schema_impl::end_scan(cluster_table);
    Wsrep_schema_impl::finish_stmt(&storage_thd);
    trans_commit(&storage_thd);
    if (error == HA_ERR_END_OF_FILE)
    {
      WSREP_INFO("Cluster table is empty, not recovering transactions");
      DBUG_RETURN(0);
    }
    else
    {
      WSREP_ERROR("Failed to read cluster table: %d", error);
      DBUG_RETURN(1);
    }
  }

  Wsrep_schema_impl::scan(cluster_table, 0, cluster_id);
  Wsrep_schema_impl::end_scan(cluster_table);
  Wsrep_schema_impl::finish_stmt(&storage_thd);

  std::ostringstream os;
  os << cluster_id;
  WSREP_INFO("Recovered cluster id %s", os.str().c_str());

  storage_thd.wsrep_skip_locking= TRUE;
  Wsrep_schema_impl::init_stmt(&storage_thd);

  /*
    Open the table for reading and writing so that fragments without
    valid seqno can be deleted.
  */
  if (Wsrep_schema_impl::open_for_write(&storage_thd, sr_table_str.c_str(), &frag_table) ||
      Wsrep_schema_impl::init_for_scan(frag_table))
  {
    WSREP_ERROR("Failed to open SR table for write");
    goto out;
  }

  while (0 == error)
  {
    if ((error= Wsrep_schema_impl::next_record(frag_table)) == 0)
    {
      wsrep::id server_id;
      Wsrep_schema_impl::scan(frag_table, 0, server_id);
      wsrep::client_id client_id;
      unsigned long long transaction_id_ull;
      Wsrep_schema_impl::scan(frag_table, 1, transaction_id_ull);
      wsrep::transaction_id transaction_id(transaction_id_ull);
      long long seqno_ll;
      Wsrep_schema_impl::scan(frag_table, 2, seqno_ll);
      wsrep::seqno seqno(seqno_ll);

      /* This is possible if the server crashes between inserting the
         fragment into table and updating the fragment seqno after
         certification. */
      if (seqno.is_undefined())
      {
        Wsrep_schema_impl::delete_row(frag_table);
        continue;
      }

      wsrep::gtid gtid(cluster_id, seqno);
      int flags;
      Wsrep_schema_impl::scan(frag_table, 3, flags);
      String data_str;

      (void)frag_table->field[4]->val_str(&data_str);
      wsrep::const_buffer data(data_str.ptr(), data_str.length());
      wsrep::ws_meta ws_meta(gtid,
                             wsrep::stid(server_id,
                                         transaction_id,
                                         client_id),
                             wsrep::seqno::undefined(),
                             flags);

      wsrep::high_priority_service* applier;
      if (!(applier= server_state.find_streaming_applier(server_id,
                                                         transaction_id)))
      {
        DBUG_ASSERT(wsrep::starts_transaction(flags));
        applier = wsrep_create_streaming_applier(&storage_thd, "recovery");
        server_state.start_streaming_applier(server_id, transaction_id,
                                             applier);
        applier->start_transaction(wsrep::ws_handle(transaction_id, 0),
                                   ws_meta);
      }
      applier->store_globals();
      wsrep::mutable_buffer unused;
      if ((ret= applier->apply_write_set(ws_meta, data, unused)) != 0)
      {
        WSREP_ERROR("SR trx recovery applying returned %d", ret);
      }
      else
      {
        applier->after_apply();
      }
      storage_service.store_globals();
    }
    else if (error == HA_ERR_END_OF_FILE)
    {
      ret= 0;
    }
    else
    {
      WSREP_ERROR("SR table scan returned error %d", error);
    }
  }
  Wsrep_schema_impl::end_scan(frag_table);
  Wsrep_schema_impl::finish_stmt(&storage_thd);
  trans_commit(&storage_thd);
  storage_thd.set_mysys_var(0);
out:
  DBUG_RETURN(ret);
}
