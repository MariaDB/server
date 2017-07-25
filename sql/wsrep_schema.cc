/* Copyright (C) 2015-2017 Codership Oy <info@codership.com>

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

#include "wsrep_thd_pool.h"
#include "wsrep_schema.h"
#include "wsrep_applier.h"

#include <string>
#include <sstream>

const std::string wsrep_schema_str= "wsrep_schema";
static const std::string create_wsrep_schema_str=
  "CREATE DATABASE IF NOT EXISTS wsrep_schema";

static const std::string create_cluster_table_str=
  "CREATE TABLE IF NOT EXISTS wsrep_schema.cluster"
  "("
  "cluster_uuid CHAR(36) PRIMARY KEY,"
  "view_id BIGINT NOT NULL,"
  "view_seqno BIGINT NOT NULL,"
  "protocol_version INT NOT NULL"
  ") ENGINE=InnoDB";

static const std::string create_members_table_str=
  "CREATE TABLE IF NOT EXISTS wsrep_schema.members"
  "("
  "node_uuid CHAR(36) PRIMARY KEY,"
  "cluster_uuid CHAR(36) NOT NULL,"
  "node_name CHAR(32) NOT NULL,"
  "node_incoming_address VARCHAR(256) NOT NULL"
  ") ENGINE=InnoDB";

#ifdef WSREP_SCHEMA_MEMBERS_HISTORY
static const std::string create_members_history_table_str=
  "CREATE TABLE IF NOT EXISTS wsrep_schema.members_history"
  "("
  "node_uuid CHAR(36) PRIMARY KEY,"
  "cluster_uuid CHAR(36) NOT NULL,"
  "last_view_id BIGINT NOT NULL,"
  "last_view_seqno BIGINT NOT NULL,"
  "node_name CHAR(32) NOT NULL,"
  "node_incoming_address VARCHAR(256) NOT NULL"
  ") ENGINE=InnoDB";
#endif /* WSREP_SCHEMA_MEMBERS_HISTORY */

static const std::string sr_table_str= "SR";
const std::string sr_table_name_full_str= wsrep_schema_str + "/" + sr_table_str;
static const std::string create_frag_table_str=
  "CREATE TABLE IF NOT EXISTS " + wsrep_schema_str + "." + sr_table_str +
  "("
  "node_uuid CHAR(36), "
  "trx_id BIGINT, "
  "seqno BIGINT, "
  "flags INT NOT NULL, "
  "frag LONGBLOB NOT NULL, "
  "PRIMARY KEY (node_uuid, trx_id, seqno)"
  ") ENGINE=InnoDB";

static const std::string delete_from_cluster_table=
  "DELETE FROM wsrep_schema.cluster";

static const std::string delete_from_members_table=
  "DELETE FROM wsrep_schema.members";


namespace Wsrep_schema_impl
{

static int execute_SQL(THD* thd, const char* sql, uint length) {
  DBUG_ENTER("Wsrep_schema::execute_SQL()");
  int err= 0;

  PSI_statement_locker *parent_locker= thd->m_statement_psi;
  Parser_state parser_state;

  WSREP_DEBUG("SQL: %d %s thd: %lld", length, sql,
              (long long)thd->thread_id);
  if (parser_state.init(thd, (char*)sql, length) == 0) {
    thd->reset_for_next_command();
    lex_start(thd);

    thd->m_statement_psi= NULL;

    thd->set_query((char*)sql, length);
    thd->set_query_id(next_query_id());

    mysql_parse(thd, (char*)sql, length, & parser_state, FALSE, FALSE);

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
  lex_start(thd);
  thd->reset_for_next_command();
}

static void finish_stmt(THD* thd) {
  trans_commit_stmt(thd);
  thd->lex->unit.cleanup();
  close_thread_tables(thd);
}

static int open_table(THD* thd,
               const LEX_STRING& schema_name,
               const LEX_STRING& table_name,
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

  tables.init_one_table(schema_name.str, schema_name.length,
                        table_name.str, table_name.length,
                        table_name.str, lock_type);

  if (!open_n_lock_single_table(thd, &tables, tables.lock_type, flags)) {
    close_thread_tables(thd);
    my_error(ER_NO_SUCH_TABLE, MYF(0), schema_name.str, table_name.str);
    DBUG_RETURN(1);
  }

  *table= tables.table;
  (*table)->use_all_columns();

  DBUG_RETURN(0);
}


static int open_for_write(THD* thd, const char* table_name, TABLE** table) {
  LEX_STRING schema_str= { C_STRING_WITH_LEN(wsrep_schema_str.c_str()) };
  LEX_STRING table_str= { C_STRING_WITH_LEN(table_name) };
  if (Wsrep_schema_impl::open_table(thd, schema_str, table_str, TL_WRITE,
                                    table)) {
    WSREP_ERROR("Failed to open table %s.%s for writing",
                schema_str.str, table_name);
    return 1;
  }
  empty_record(*table);
  (*table)->use_all_columns();
  restore_record(*table, s->default_values);
  return 0;
}


static void store(TABLE* table, uint field, const wsrep_uuid_t& uuid) {
  assert(field < table->s->fields);
  char uuid_str[37]= {'\0', };
  wsrep_uuid_print(&uuid, uuid_str, sizeof(uuid_str));
  table->field[field]->store(uuid_str,
                             strlen(uuid_str),
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
      table->file->print_error(error, MYF(0));
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
      table->file->print_error(error, MYF(0));
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
    table->file->print_error(error, MYF(0));
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
    table->file->print_error(error, MYF(0));
    return 1;
  }
  return 0;
}

static int open_for_read(THD* thd, const char* table_name, TABLE** table) {
  LEX_STRING schema_str= { C_STRING_WITH_LEN(wsrep_schema_str.c_str()) };
  LEX_STRING table_str= { C_STRING_WITH_LEN(table_name) };
  if (Wsrep_schema_impl::open_table(thd, schema_str, table_str, TL_READ,
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

/*
  Scan wsrep uuid from given field.

  @return 0 in case of success, 1 in case of error.
 */
static int scan(TABLE* table, uint field, wsrep_uuid_t& uuid)
{
  assert(field < table->s->fields);
  int error;
  String uuid_str;
  (void)table->field[field]->val_str(&uuid_str);
  if ((error= wsrep_uuid_scan((const char*)uuid_str.c_ptr(),
                              uuid_str.length(),
                              &uuid) < 0)) {
    WSREP_ERROR("Failed to scan uuid: %d", -error);
    return 1;
  }
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
  String str;
  (void)table->field[field]->val_str(&str);
  strncpy(strbuf, str.c_ptr(), std::min(str.length(), strbuf_len));
  strbuf[strbuf_len - 1] = '\0';
  return 0;
}

/*
  Scan member
  TODO: filter members by cluster UUID
 */
static int scan_member(TABLE* table,
                       wsrep_uuid_t const cluster_uuid,
                       std::vector<wsrep_member_info_t>& members)
{
  wsrep_member_info_t member;

  memset(&member, 0, sizeof(member));

  if (scan(table, 0, member.id) ||
      scan(table, 2, member.name, sizeof(member.name)) ||
      scan(table, 3, member.incoming, sizeof(member.incoming))) {
    return 1;
  }

  if (members.empty() == false) {
    assert(memcmp(&members.rbegin()->id, &member.id, sizeof(member.id)) < 0);
  }

  try {
    members.push_back(member);
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
  Scan next index record. For return codes see handler::ha_index_next()

  @return 0 in case of success, error code in case of error
 */
/*
static int next_index_record(TABLE* table) {
  int error;
  if ((error= table->file->ha_index_next(table->record[0])) &&
      error != HA_ERR_END_OF_FILE) {
    WSREP_ERROR("Failed to read next record: %d", error);
  }
  return error;
}
*/

/*
  End index scan.

  @return 0 in case of success, 1 in case of error.
 */
static int end_index_scan(TABLE* table) {
  int error;
  if ((error= table->file->ha_index_end())) {
    WSREP_ERROR("Failed to end scan: %d", error);
    return 1;
  }
  return 0;
}

static void make_key(TABLE* table, uchar* key, key_part_map* map, int parts) {
  uint prefix_length= 0;
  KEY_PART_INFO* key_part= table->key_info->key_part;
  for (int i=0; i < parts; i++)
    prefix_length += key_part[i].store_length;
  *map = make_prev_keypart_map(parts);
  key_copy(key, table->record[0], table->key_info, prefix_length);
}


static int apply_frag(THD* thd, TABLE* table, wsrep_uuid_t cluster_uuid)
{
  int ret= 0;
  wsrep_trx_meta_t meta;
  String buf;
  int32_t flags;

  Wsrep_schema_impl::scan(table, 0, meta.stid.node);
  Wsrep_schema_impl::scan(table, 1, meta.stid.trx);
  Wsrep_schema_impl::scan(table, 2, meta.gtid.seqno);
  Wsrep_schema_impl::scan(table, 3, flags);
  meta.gtid.uuid= cluster_uuid;

  WSREP_INFO("apply frag %llu %lld", (unsigned long long)meta.stid.trx, (long long)meta.gtid.seqno);
  (void)table->field[4]->val_str(&buf);

  wsrep_buf_t const ws = { buf.c_ptr(), buf.length() };
  void*  err_buf;
  size_t err_len;
  if ((ret = wsrep_apply_cb(thd, flags, &ws, &meta, &err_buf, &err_len))
      != WSREP_RET_SUCCESS) {
    WSREP_WARN("Failed to apply frag: %d, %s",
               ret, err_buf ? (char*)err_buf : "(null)");
  }
  free(err_buf);

  thd->store_globals(); /* Restore orig thd context */
  return ret;
}

} /* namespace Wsrep_schema_impl */


Wsrep_schema::Wsrep_schema(Wsrep_thd_pool* thd_pool)
  :
  thd_pool_(thd_pool)
{
  assert(thd_pool_);
}

Wsrep_schema::~Wsrep_schema()
{ }

int Wsrep_schema::init()
{
  DBUG_ENTER("Wsrep_schema::init()");
  int ret;
  THD* thd= thd_pool_->get_thd(0);
  if (!thd) {
    WSREP_ERROR("Unable to get thd");
    DBUG_RETURN(1);
  }

  if (Wsrep_schema_impl::execute_SQL(thd, create_wsrep_schema_str.c_str(),
                                     create_wsrep_schema_str.size()) ||
      Wsrep_schema_impl::execute_SQL(thd, create_cluster_table_str.c_str(),
                                     create_cluster_table_str.size()) ||
      Wsrep_schema_impl::execute_SQL(thd, create_members_table_str.c_str(),
                                     create_members_table_str.size()) ||
#ifdef WSREP_SCHEMA_MEMBERS_HISTORY
      Wsrep_schema_impl::execute_SQL(thd,
                                     create_members_history_table_str.c_str(),
                                     create_members_history_table_str.size()) ||
#endif /* WSREP_SCHEMA_MEMBERS_HISTORY */
      Wsrep_schema_impl::execute_SQL(thd,
                                     create_frag_table_str.c_str(),
                                     create_frag_table_str.size())) {
    ret= 1;
  }
  else {
    ret= 0;
  }
  thd_pool_->release_thd(thd);
  DBUG_RETURN(ret);
}

int Wsrep_schema::store_view(const wsrep_view_info_t* view)
{
  DBUG_ENTER("Wsrep_schema::store_view()");
  assert(view->status == WSREP_VIEW_PRIMARY);
  int ret= 1;
  int error;

  TABLE* cluster_table= 0;
  TABLE* members_table= 0;
#ifdef WSREP_SCHEMA_MEMBERS_HISTORY
  TABLE* members_history_table= 0;
#endif /* WSREP_SCHEMA_MEMBERS_HISTORY */

  THD* thd= thd_pool_->get_thd(0);
  if (!thd) {
    WSREP_ERROR("Could not allocate thd");
    DBUG_RETURN(1);
  }

  if (trans_begin(thd, MYSQL_START_TRANS_OPT_READ_WRITE)) {
    WSREP_ERROR("failed to start transaction");
    goto out;
  }

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
  if (Wsrep_schema_impl::open_for_write(thd, "cluster", &cluster_table)) {
    goto out;
  }

  Wsrep_schema_impl::store(cluster_table, 0, view->state_id.uuid);
  Wsrep_schema_impl::store(cluster_table, 1, view->view);
  Wsrep_schema_impl::store(cluster_table, 2, view->state_id.seqno);
  Wsrep_schema_impl::store(cluster_table, 3, view->proto_ver);
  if ((error= Wsrep_schema_impl::update_or_insert(cluster_table))) {
    WSREP_ERROR("failed to write to cluster table: %d", error);
    goto out;
  }
  Wsrep_schema_impl::finish_stmt(thd);

  /*
    Store info about current members
  */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_write(thd, "members", &members_table)) {
    WSREP_ERROR("failed to open wsrep.members table");
    goto out;
  }

  for (int i= 0; i < view->memb_num; ++i) {
    Wsrep_schema_impl::store(members_table, 0, view->members[i].id);
    Wsrep_schema_impl::store(members_table, 1, view->state_id.uuid);
    Wsrep_schema_impl::store(members_table, 2,
                             view->members[i].name,
                             strlen(view->members[i].name));
    Wsrep_schema_impl::store(members_table, 3,
                             view->members[i].incoming,
                             strlen(view->members[i].incoming));
    if ((error= Wsrep_schema_impl::update_or_insert(members_table))) {
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
  if (Wsrep_schema_impl::open_for_write(thd, "members_history",
                                        &members_history_table)) {
    WSREP_ERROR("failed to open wsrep.members table");
    goto out;
  }

  for (int i= 0; i < view->memb_num; ++i) {
    Wsrep_schema_impl::store(members_history_table, 0, view->members[i].id);
    Wsrep_schema_impl::store(members_history_table, 1, view->state_id.uuid);
    Wsrep_schema_impl::store(members_history_table, 2, view->view);
    Wsrep_schema_impl::store(members_history_table, 3,
                             view->state_id.seqno);
    Wsrep_schema_impl::store(members_history_table, 4,
                             view->members[i].name,
                             strlen(view->members[i].name));
    Wsrep_schema_impl::store(members_history_table, 5,
                             view->members[i].incoming,
                             strlen(view->members[i].incoming));
    if ((error= Wsrep_schema_impl::update_or_insert(members_history_table))) {
      WSREP_ERROR("failed to write wsrep.members table: %d", error);
      goto out;
    }
  }
  Wsrep_schema_impl::finish_stmt(thd);
#endif /* WSREP_SCHEMA_MEMBERS_HISTORY */

  if (!trans_commit(thd)) {
    /* Success */
    ret= 0;
  }

 out:

  if (ret) {
    trans_rollback_stmt(thd);
    if (!trans_rollback(thd)) {
      close_thread_tables(thd);
    }
  }
  thd->mdl_context.release_transactional_locks();

  thd_pool_->release_thd(thd);

  DBUG_RETURN(ret);
}

int Wsrep_schema::restore_view(const wsrep_uuid_t& node_uuid,
                               wsrep_view_info_t** view_info) const {
  DBUG_ENTER("Wsrep_schema::restore_view()");
  assert(view_info);
  int ret= 1;
  int error;
  THD* thd= thd_pool_->get_thd(0);

  TABLE* cluster_table= 0;
  TABLE* members_table=0;

  wsrep_uuid_t cluster_uuid;
  wsrep_seqno_t view_id;
  wsrep_seqno_t view_seqno;
  int my_idx= -1;
  int proto_ver;
  std::vector<wsrep_member_info_t> members;

  if (!thd) {
    WSREP_ERROR("Failed to allocate THD for restore view");
    DBUG_RETURN(1);
  }

  if (trans_begin(thd, MYSQL_START_TRANS_OPT_READ_ONLY)) {
    WSREP_ERROR("Failed to start transaction");
    goto out;
  }

  /*
    Read cluster info from cluster table
   */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_read(thd, "cluster", &cluster_table) ||
      Wsrep_schema_impl::init_for_scan(cluster_table) ||
      Wsrep_schema_impl::next_record(cluster_table) ||
      Wsrep_schema_impl::scan(cluster_table, 0, cluster_uuid) ||
      Wsrep_schema_impl::scan(cluster_table, 1, view_id) ||
      Wsrep_schema_impl::scan(cluster_table, 2, view_seqno) ||
      Wsrep_schema_impl::scan(cluster_table, 3, proto_ver) ||
      Wsrep_schema_impl::end_scan(cluster_table)) {
    goto out;
  }
  Wsrep_schema_impl::finish_stmt(thd);

  /*
    Read members from members table
  */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_read(thd, "members", &members_table) ||
      Wsrep_schema_impl::init_for_scan(members_table)) {
    goto out;
  }

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

  if (Wsrep_schema_impl::end_scan(members_table)) {
    goto out;
  }
  Wsrep_schema_impl::finish_stmt(thd);

  for (uint i= 0; i < members.size(); ++i) {
    if (memcmp(&members[i].id, &node_uuid, sizeof(node_uuid)) == 0) {
      my_idx= i;
    }
  }

  *view_info= (wsrep_view_info_t*)
    malloc(sizeof(wsrep_view_info_t)
           + members.size() * sizeof(wsrep_member_info_t));
  if (!view_info) {
    WSREP_ERROR("Failed to allocate memory for view info");
    goto out;
  }

  (*view_info)->state_id.uuid= cluster_uuid;
  (*view_info)->state_id.seqno= view_seqno;
  (*view_info)->view= view_id;
  (*view_info)->status= WSREP_VIEW_PRIMARY;
  (*view_info)->my_idx= my_idx;
  (*view_info)->proto_ver= proto_ver;
  (*view_info)->memb_num= members.size();
  std::copy(members.begin(), members.end(), (*view_info)->members);

  (void)trans_commit(thd);
  ret= 0; /* Success*/
 out:

  if (ret) {
    trans_rollback_stmt(thd);
    if (!trans_rollback(thd)) {
      close_thread_tables(thd);
    }
  }
  thd->mdl_context.release_transactional_locks();

  thd_pool_->release_thd(thd);
  DBUG_RETURN(ret);
}

int Wsrep_schema::append_frag_apply(THD* thd,
                                    const wsrep_trx_meta_t& meta,
                                    const uint32_t          flags,
                                    const unsigned char*    frag,
                                    size_t                  frag_len)
{
  DBUG_ENTER("Wsrep_schema::append_frag_apply");
  int error, ret= 1;
  TABLE* frag_table= 0;
  int wsrep_on= thd->variables.wsrep_on;
  int sql_log_bin= thd->variables.sql_log_bin;
  int log_bin_option= (thd->variables.option_bits & OPTION_BIN_LOG);
  my_bool skip_locking= thd->wsrep_skip_locking;

  thd->variables.wsrep_on= 0;
  thd->variables.sql_log_bin= 0;
  thd->variables.option_bits&= ~OPTION_BIN_LOG;
  thd->wsrep_skip_locking= TRUE;

  assert(meta.stid.trx != WSREP_UNDEFINED_TRX_ID);
  assert(wsrep_uuid_compare(&meta.stid.node, &WSREP_UUID_UNDEFINED) != 0);

  if (trans_begin(thd, MYSQL_START_TRANS_OPT_READ_WRITE)) {
    WSREP_ERROR("Failed to start transaction");
    goto out;
  }

  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_write(thd, "SR", &frag_table)) {
    goto out;
  }

  Wsrep_schema_impl::store(frag_table, 0, meta.stid.node);
  Wsrep_schema_impl::store(frag_table, 1, meta.stid.trx);
  Wsrep_schema_impl::store(frag_table, 2, meta.gtid.seqno);
  Wsrep_schema_impl::store(frag_table, 3, flags);
  Wsrep_schema_impl::store(frag_table, 4, frag, frag_len);

  /* TODO: make this insert only */
  if ((error= Wsrep_schema_impl::insert(frag_table))) {
    WSREP_ERROR("Failed to write to frag table: %d", error);
    goto out;
  }
  Wsrep_schema_impl::finish_stmt(thd);
  ret= 0;

out:
  thd->variables.wsrep_on= wsrep_on;
  thd->variables.sql_log_bin= sql_log_bin;
  thd->variables.option_bits|= log_bin_option;
  thd->wsrep_skip_locking= skip_locking;
  DBUG_RETURN(ret);
}

THD* Wsrep_schema::append_frag(const wsrep_trx_meta_t& meta,
                               const uint32_t          flags,
                               const unsigned char*    frag,
                               size_t                  frag_len)
{
  DBUG_ENTER("Wsrep_schema::append_frag");

  THD* thd= thd_pool_->get_thd(0);

  if (!thd) {
    WSREP_ERROR("Could not allocate thd");
    goto out;
  }

  if (append_frag_apply(thd, meta, flags, frag, frag_len)) {
    goto out;
  }

  DBUG_RETURN(thd);

out:
  if (thd) release_SR_thd(thd);
  DBUG_RETURN(NULL);
}

int Wsrep_schema::append_frag_commit(const wsrep_trx_meta_t& meta,
                                     const uint32_t          flags,
                                     const unsigned char*    frag,
                                     size_t                  frag_len)
{
  DBUG_ENTER("Wsrep_schema::append_frag_commit");

  THD* thd;
  int ret= 1;

  if ((thd= append_frag(meta, flags, frag, frag_len)) == NULL)
    goto out;

  if (!trans_commit(thd)) {
    /* Success */
    ret= 0;
  }

out:
  if (thd) {
    thd->mdl_context.release_transactional_locks();
    thd_pool_->release_thd(thd);
  }
  DBUG_RETURN(ret);
}

int Wsrep_schema::update_frag_seqno(THD* thd, const wsrep_trx_meta_t& meta)
{
  DBUG_ENTER("Wsrep_schema::update_frag_seqno");
  WSREP_DEBUG("update_frag_seqno(%lld) trx %ld, seqno %lld",
              thd->thread_id, meta.stid.trx, (long long)meta.gtid.seqno);
  int error, ret= 1;
  uchar key[MAX_KEY_LENGTH];
  key_part_map key_map= 0;
  TABLE* frag_table= 0;
  my_bool skip_locking= thd->wsrep_skip_locking;
  thd->wsrep_skip_locking= TRUE;

  assert(meta.gtid.seqno != WSREP_SEQNO_UNDEFINED);

  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_write(thd, "SR", &frag_table)) {
    goto out;
  }

  /* Find record with the given uuid, trx id, and seqno -1 */
  Wsrep_schema_impl::store(frag_table, 0, meta.stid.node);
  Wsrep_schema_impl::store(frag_table, 1, meta.stid.trx);
  Wsrep_schema_impl::store(frag_table, 2, -1);
  Wsrep_schema_impl::make_key(frag_table, key, &key_map, 3);

  if ((error= Wsrep_schema_impl::init_for_index_scan(frag_table, key, key_map))) {
    if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND) {
      WSREP_ERROR("Record not found in %s.%s: %d",
                  frag_table->s->db.str,
                  frag_table->s->table_name.str,
                  error);
    }
    frag_table->file->print_error(error, MYF(0));
    goto out;
  }

  /* Copy the original record to frag_table->record[1] */
  store_record(frag_table, record[1]);

  /* Store seqno in frag_table->record[0] and update the row */
  Wsrep_schema_impl::store(frag_table, 2, meta.gtid.seqno);
  if ((error= frag_table->file->ha_update_row(frag_table->record[1],
                                              frag_table->record[0]))) {
    WSREP_ERROR("Error updating record in %s.%s: %d",
                frag_table->s->db.str,
                frag_table->s->table_name.str,
                error);
    frag_table->file->print_error(error, MYF(0));
    ret= error;
    goto out;
  }

  if (Wsrep_schema_impl::end_index_scan(frag_table)) {
    goto out;
  }

  Wsrep_schema_impl::finish_stmt(thd);

  if (!trans_commit(thd)) {
    /* Success */
    ret= 0;
  }

out:
  thd->wsrep_skip_locking= skip_locking;
  if (ret) {
    trans_rollback_stmt(thd);
    if (!trans_rollback(thd)) {
      close_thread_tables(thd);
    }
  }
  thd->mdl_context.release_transactional_locks();
  thd_pool_->release_thd(thd);
  DBUG_RETURN(ret);
}

void Wsrep_schema::release_SR_thd(THD* thd)
{
  trans_rollback_stmt(thd);
  if (!trans_rollback(thd)) {
    close_thread_tables(thd);
  }
  thd->mdl_context.release_transactional_locks();
  thd_pool_->release_thd(thd);
}

static int remove_fragment(THD* thd, TABLE* frag_table,
                           const wsrep_trx_meta& meta)
{
  WSREP_DEBUG("remove_fragment(%lld) trx %ld, seqno %lld",
              thd->thread_id, meta.stid.trx, (long long)meta.gtid.seqno);
  int error, ret= 1;
  uchar key[MAX_KEY_LENGTH];
  key_part_map key_map= 0;

  assert(meta.stid.trx != WSREP_UNDEFINED_TRX_ID);
  assert(wsrep_uuid_compare(&meta.stid.node, &WSREP_UUID_UNDEFINED) != 0);

  /*
    Remove record with the given uuid, trx id, and seqno.
    Using a complete key here avoids gap locks.
  */
  Wsrep_schema_impl::store(frag_table, 0, meta.stid.node);
  Wsrep_schema_impl::store(frag_table, 1, meta.stid.trx);
  Wsrep_schema_impl::store(frag_table, 2, meta.gtid.seqno);
  Wsrep_schema_impl::make_key(frag_table, key, &key_map, 3);

  if ((error= Wsrep_schema_impl::init_for_index_scan(frag_table, key, key_map))) {
      if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND) {
        WSREP_WARN("Record not found in %s.%s:trx %ld, seqno %lld, error %d",
                   frag_table->s->db.str,
                   frag_table->s->table_name.str,
                   meta.stid.trx,
                   (long long)meta.gtid.seqno,
                   error);
      }
    frag_table->file->print_error(error, MYF(0));
    ret= error;
    goto out;
  }

  if (Wsrep_schema_impl::delete_row(frag_table)) {
    goto out;
  }

  ret= 0;

 out:
  Wsrep_schema_impl::end_index_scan(frag_table);
  return ret;
}

int Wsrep_schema::remove_trx(THD* thd, wsrep_fragment_set* fragments)
{
  DBUG_ENTER("Wsrep_schema::remove_trx()");
  WSREP_DEBUG("Wsrep_schema::remove_trx(%lld)", thd->thread_id);
  int wsrep_on= thd->variables.wsrep_on;
  int sql_log_bin= thd->variables.sql_log_bin;
  int log_bin_option= (thd->variables.option_bits & OPTION_BIN_LOG);
  my_bool skip_locking= thd->wsrep_skip_locking;

  thd->variables.wsrep_on= 0;
  thd->variables.sql_log_bin= 0;
  thd->variables.option_bits&= ~OPTION_BIN_LOG;
  thd->wsrep_skip_locking= TRUE;

  TABLE* frag_table= 0;
  bool was_opened= false;
  if (thd->open_tables) {
    for (TABLE* t= thd->open_tables; t != NULL; t= t->next) {
      if (!strcmp(t->s->db.str, "wsrep_schema") &&
          !strcmp(t->s->table_name.str, "SR")) {
        frag_table= t;
        empty_record(frag_table);
        frag_table->use_all_columns();
        restore_record(frag_table, s->default_values);
        break;
      }
    }
  }
  else {
    (void)Wsrep_schema_impl::open_for_write(thd, "SR", &frag_table);
    was_opened= true;
  }
  DBUG_ASSERT(frag_table);

  int ret= 1;
  if (frag_table) {
    wsrep_fragment_set::const_iterator it= fragments->begin();
    for (; it!= fragments->end(); it++) {
      if (int error= remove_fragment(thd, frag_table, *it)) {
        ret= error;
        break;
      }
    }
    if (it == fragments->end()) {
      ret= 0;
    }
  }
  else {
    WSREP_WARN("SR table not open in remove_trx()");
  }

  if (was_opened) {
    trans_commit_stmt(thd);
    close_thread_tables(thd);
  }

  thd->variables.wsrep_on= wsrep_on;
  thd->variables.sql_log_bin= sql_log_bin;
  thd->variables.option_bits|= log_bin_option;
  thd->wsrep_skip_locking= skip_locking;

  DBUG_RETURN(ret);
}

int Wsrep_schema::rollback_trx(THD* caller)
{
  DBUG_ENTER("Wsrep_schema::rollback_trx");
  WSREP_DEBUG("Wsrep_schema::rollback_trx(%lld)", caller->thread_id);

  /*
     There are cases where rollback_trx is called unnecessarily from
     wsrep_client_rollback().
     For instance, after_command() -> wsrep_client_rollback() -> rollback_trx()
     is not necessary.
     However, wsrep_client_rollback() is also called when a server transitions
     from non-primary back to primary, in which case we want to clean up the
     SR table. We should fix this eventually, and replace the following if
     statement with assert.
   */
  if (caller->wsrep_SR_fragments.empty()) {
      WSREP_DEBUG("Wsrep_schema::rollback_trx(%lld) no fragments to remove",
                 caller->thread_id);
      DBUG_RETURN(0);
  }

  THD* thd= thd_pool_->get_thd(0);

  if (!thd) {
    WSREP_ERROR("Could not allocate thd");
    DBUG_RETURN(1);
  }

  if (trans_begin(thd, MYSQL_START_TRANS_OPT_READ_WRITE)) {
    WSREP_ERROR("Failed to start transaction");
    DBUG_RETURN(1);
  }

  Wsrep_schema_impl::init_stmt(thd);
  int ret= remove_trx(thd, &caller->wsrep_SR_fragments);
  Wsrep_schema_impl::finish_stmt(thd);

  if (!ret && !trans_commit(thd)) {
    /* Success */
    ret= 0;
  }

  if (ret) {
    trans_rollback_stmt(thd);
    if (!trans_rollback(thd)) {
      close_thread_tables(thd);
    }
  }
  thd->mdl_context.release_transactional_locks();

  thd_pool_->release_thd(thd);

  DBUG_RETURN(ret);
}

int Wsrep_schema::restore_frags()
{
  DBUG_ENTER("Wsrep_schema::restore_frags()");
  int ret= 1;
  int error;
  wsrep_uuid_t cluster_uuid;
  TABLE* frag_table= 0;
  TABLE* cluster_table= 0;


  THD* thd= thd_pool_->get_thd(0);

  WSREP_INFO("Restoring SR fragments from table storage");

  if (!thd) {
    WSREP_ERROR("Failed to allocate THD for restore view");
    DBUG_RETURN(1);
  }


  if (trans_begin(thd, MYSQL_START_TRANS_OPT_READ_ONLY)) {
    WSREP_ERROR("Failed to start transaction");
    goto out;
  }

  /*
    Scan cluster uuid first
   */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_read(thd, "cluster", &cluster_table) ||
      Wsrep_schema_impl::init_for_scan(cluster_table)) {
    goto out;
  }
  if ((error= Wsrep_schema_impl::next_record(cluster_table)) == 0) {
    if (Wsrep_schema_impl::scan(cluster_table, 0, cluster_uuid)) {
      goto out;
    }
  }
  else if (error == HA_ERR_END_OF_FILE) {
    WSREP_ERROR("Cluster table is empty!");
    goto out;
  }
  else {
    WSREP_ERROR("Cluster table scan returned error: %d", errno);
    goto out;
  }
  if (Wsrep_schema_impl::end_scan(cluster_table)) {
    goto out;
  }
  Wsrep_schema_impl::finish_stmt(thd);

  /*
    Scan all fragments and apply them
   */
  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_read(thd, "SR", &frag_table) ||
      Wsrep_schema_impl::init_for_scan(frag_table)) {
    goto out;
  }
  while (true) {
    if ((error= Wsrep_schema_impl::next_record(frag_table)) == 0) {
      if (Wsrep_schema_impl::apply_frag(thd, frag_table, cluster_uuid)) {
        goto out;
      }
    }
    else if (error == HA_ERR_END_OF_FILE) {
      break;
    }
    else {
      WSREP_ERROR("Frag table scan returned error: %d", error);
      goto out;
    }
  }
  if (Wsrep_schema_impl::end_scan(frag_table)) {
    goto out;
  }
  Wsrep_schema_impl::finish_stmt(thd);

  (void)trans_commit(thd);
  ret= 0; /* Success*/

  WSREP_INFO("SR fragments restored");
 out:

  if (ret) {
    trans_rollback_stmt(thd);
    if (!trans_rollback(thd)) {
      close_thread_tables(thd);
    }
  }
  thd->mdl_context.release_transactional_locks();

  thd_pool_->release_thd(thd);
  DBUG_RETURN(ret);

}


int Wsrep_schema::replay_trx(THD* real_thd, const wsrep_trx_meta_t& meta)
{
  int ret= 1;
  int error;
  TABLE* frag_table= 0;
  uchar key[MAX_KEY_LENGTH];
  key_part_map key_map= 0;

  THD* thd= thd_pool_->get_thd(0);
  wsrep_fragment_set* fragments= &real_thd->wsrep_SR_fragments;
  wsrep_fragment_set::const_iterator it= fragments->begin();

  assert(!fragments->empty());

  if (trans_begin(thd, MYSQL_START_TRANS_OPT_READ_ONLY)) {
    WSREP_ERROR("Failed to start transaction");
    goto out;
  }

  Wsrep_schema_impl::init_stmt(thd);
  if (Wsrep_schema_impl::open_for_read(thd, "SR", &frag_table)) {
    goto out;
  }

  for (; it!= fragments->end(); it++) {
    Wsrep_schema_impl::store(frag_table, 0, meta.stid.node);
    Wsrep_schema_impl::store(frag_table, 1, meta.stid.trx);
    Wsrep_schema_impl::store(frag_table, 2, (*it).gtid.seqno);
    Wsrep_schema_impl::make_key(frag_table, key, &key_map, 3);

    error= Wsrep_schema_impl::init_for_index_scan(frag_table, key, key_map);
    if (error == HA_ERR_END_OF_FILE || error == HA_ERR_KEY_NOT_FOUND) {
      break;
    } else if (error > 0) {
      WSREP_ERROR("Frag table scan returned error: %d", error);
      ret= error;
      goto out;
    }

    wsrep_trx_meta_t meta;
    int32_t flags;
    Wsrep_schema_impl::scan(frag_table, 0, meta.stid.node);
    Wsrep_schema_impl::scan(frag_table, 1, meta.stid.trx);
    Wsrep_schema_impl::scan(frag_table, 2, meta.gtid.seqno);
    Wsrep_schema_impl::scan(frag_table, 3, flags);

    WSREP_INFO("replay frag trx_id: %llu seqno: %lld flags: %x",
               (unsigned long long)meta.stid.trx,
               (long long)meta.gtid.seqno, flags);

    String buf;
    (void)frag_table->field[4]->val_str(&buf);
    /*
      Call wsrep_apply_events() directly to bypass SR processing in
      wsrep_apply_cb().
    */
    real_thd->store_globals();
    error= wsrep_apply_events(real_thd, buf.c_ptr_safe(), buf.length());
    if (error != WSREP_CB_SUCCESS) {
      ret= error;
      WSREP_ERROR("Failed to apply events during replay, error: %d", error);
      goto out;
    }
    thd->store_globals();
    if (Wsrep_schema_impl::end_index_scan(frag_table)) {
      goto out;
    }
  }
  Wsrep_schema_impl::finish_stmt(thd);

  thd->store_globals();
  (void)trans_commit(thd);
  real_thd->store_globals();
  remove_trx(real_thd, &real_thd->wsrep_SR_fragments);
  (void)trans_commit(real_thd);
  ret= 0; /* Success*/

  WSREP_INFO("SR transaction replayed");
 out:

  if (ret) {
    trans_rollback_stmt(thd);
    if (!trans_rollback(thd)) {
      close_thread_tables(thd);
    }
  }
  thd->mdl_context.release_transactional_locks();

  thd_pool_->release_thd(thd);

  real_thd->store_globals();
  real_thd->mdl_context.release_transactional_locks();

  return ret;
}

void Wsrep_schema::init_SR_table(TABLE_LIST *table)
{
  table->init_one_table(wsrep_schema_str.c_str(),
                        wsrep_schema_str.size(),
                        "SR", 2, "SR", TL_WRITE);
}
