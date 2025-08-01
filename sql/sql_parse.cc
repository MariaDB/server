/* Copyright (c) 2000, 2017, Oracle and/or its affiliates.
   Copyright (c) 2008, 2024, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1335  USA */

#define MYSQL_LEX 1
#include "mariadb.h"
#include "sql_priv.h"
#include "sql_parse.h"        // sql_kill, *_precheck, *_prepare
#include "lock.h"             // try_transactional_lock,
                              // check_transactional_lock,
                              // set_handler_table_locks,
                              // lock_global_read_lock,
                              // make_global_read_lock_block_commit
#include "sql_base.h"         // open_tables, open_and_lock_tables,
                              // lock_tables, unique_table,
                              // close_thread_tables, is_temporary_table
                              // table_cache.h
#include "sql_cache.h"        // QUERY_CACHE_FLAGS_SIZE, query_cache_*
#include "sql_show.h"         // mysqld_list_*, mysqld_show_*,
                              // calc_sum_of_all_status
#include "mysqld.h"
#include "sql_locale.h"                         // my_locale_en_US
#include "log.h"                                // flush_error_log
#include "sql_view.h"         // mysql_create_view, mysql_drop_view
#include "sql_insert.h"       // mysql_insert
#include "sql_partition.h"    // struct partition_info
#include "sql_db.h"           // mysql_change_db, mysql_create_db,
                              // mysql_rm_db, mysql_upgrade_db,
                              // mysql_alter_db,
                              // check_db_dir_existence,
                              // my_dbopt_cleanup
#include "sql_table.h"        // mysql_alter_table,
                              // mysql_backup_table,
                              // mysql_restore_table
#include "sql_reload.h"       // reload_acl_and_cache
#include "sql_admin.h"        // mysql_assign_to_keycache
#include "sql_connect.h"      // decrease_user_connections,
                              // check_mqh,
                              // reset_mqh
#include "sql_rename.h"       // mysql_rename_tables
#include "hostname.h"         // hostname_cache_refresh
#include "sql_test.h"         // mysql_print_status
#include "sql_select.h"       // handle_select, mysql_select,
                              // mysql_explain_union
#include "sql_load.h"         // mysql_load
#include "sql_servers.h"      // create_servers, alter_servers,
                              // drop_servers, servers_reload
#include "sql_handler.h"      // mysql_ha_open, mysql_ha_close,
                              // mysql_ha_read
#include "sql_binlog.h"       // mysql_client_binlog_statement
#include "sql_do.h"           // mysql_do
#include "sql_help.h"         // mysqld_help
#include "rpl_constants.h"    // Incident, INCIDENT_LOST_EVENTS
#include "log_event.h"
#include "sql_repl.h"
#include "rpl_filter.h"
#include "repl_failsafe.h"
#include <m_ctype.h>
#include <myisam.h>
#include <my_dir.h>
#include "rpl_mi.h"

#include "sql_digest.h"

#include "sp_head.h"
#include "sp.h"
#include "sp_cache.h"
#include "events.h"
#include "sql_trigger.h"
#include "transaction.h"
#include "sql_alter.h"
#include "sql_audit.h"
#include "sql_prepare.h"
#include "sql_cte.h"
#include "debug_sync.h"
#include "probes_mysql.h"
#include "set_var.h"
#include "sql_bootstrap.h"
#include "sql_sequence.h"
#include "opt_trace.h"
#include "mysql/psi/mysql_sp.h"

#include "my_json_writer.h"
#include "opt_trace_ddl_info.h"
#define FLAGSTR(V,F) ((V)&(F)?#F" ":"")

#ifdef WITH_ARIA_STORAGE_ENGINE
#include "../storage/maria/ha_maria.h"
#endif

#include "wsrep.h"
#include "wsrep_mysqld.h"
#ifdef WITH_WSREP
#include "wsrep_thd.h"
#include "wsrep_trans_observer.h" /* wsrep transaction hooks */

static bool wsrep_mysql_parse(THD *thd, char *rawbuf, uint length,
                              Parser_state *parser_state);

#endif /* WITH_WSREP */
/**
  @defgroup Runtime_Environment Runtime Environment
  @{
*/

static bool execute_sqlcom_select(THD *thd, TABLE_LIST *all_tables);
static void sql_kill(THD *thd, my_thread_id id, killed_state state, killed_type type);
static void sql_kill_user(THD *thd, LEX_USER *user, killed_state state);
static bool lock_tables_precheck(THD *thd, TABLE_LIST *tables);
static bool execute_show_status(THD *, TABLE_LIST *);
static bool check_rename_table(THD *, TABLE_LIST *, TABLE_LIST *);
static bool generate_incident_event(THD *thd);
static int  show_create_db(THD *thd, LEX *lex);
static bool alter_routine(THD *thd, LEX *lex);
static bool drop_routine(THD *thd, LEX *lex);

const Lex_ident_db_normalized any_db(STRING_WITH_LEN("*any*"));

const LEX_CSTRING command_name[257]={
  { STRING_WITH_LEN("Sleep") },           //0
  { STRING_WITH_LEN("Quit") },            //1
  { STRING_WITH_LEN("Init DB") },         //2
  { STRING_WITH_LEN("Query") },           //3
  { STRING_WITH_LEN("Field List") },      //4
  { STRING_WITH_LEN("Create DB") },       //5
  { STRING_WITH_LEN("Drop DB") },         //6
  { STRING_WITH_LEN("Refresh") },         //7
  { STRING_WITH_LEN("Shutdown") },        //8
  { STRING_WITH_LEN("Statistics") },      //9
  { STRING_WITH_LEN("Processlist") },     //10
  { STRING_WITH_LEN("Connect") },         //11
  { STRING_WITH_LEN("Kill") },            //12
  { STRING_WITH_LEN("Debug") },           //13
  { STRING_WITH_LEN("Ping") },            //14
  { STRING_WITH_LEN("Time") },            //15
  { STRING_WITH_LEN("Delayed insert") },  //16
  { STRING_WITH_LEN("Change user") },     //17
  { STRING_WITH_LEN("Binlog Dump") },     //18
  { STRING_WITH_LEN("Table Dump") },      //19
  { STRING_WITH_LEN("Connect Out") },     //20
  { STRING_WITH_LEN("Register Slave") },  //21
  { STRING_WITH_LEN("Prepare") },         //22
  { STRING_WITH_LEN("Execute") },         //23
  { STRING_WITH_LEN("Long Data") },       //24
  { STRING_WITH_LEN("Close stmt") },      //25
  { STRING_WITH_LEN("Reset stmt") },      //26
  { STRING_WITH_LEN("Set option") },      //27
  { STRING_WITH_LEN("Fetch") },           //28
  { STRING_WITH_LEN("Daemon") },          //29
  { STRING_WITH_LEN("Unimpl get tid") },  //30
  { STRING_WITH_LEN("Reset connection") },//31
  { 0, 0 }, //32
  { 0, 0 }, //33
  { 0, 0 }, //34
  { 0, 0 }, //35
  { 0, 0 }, //36
  { 0, 0 }, //37
  { 0, 0 }, //38
  { 0, 0 }, //39
  { 0, 0 }, //40
  { 0, 0 }, //41
  { 0, 0 }, //42
  { 0, 0 }, //43
  { 0, 0 }, //44
  { 0, 0 }, //45
  { 0, 0 }, //46
  { 0, 0 }, //47
  { 0, 0 }, //48
  { 0, 0 }, //49
  { 0, 0 }, //50
  { 0, 0 }, //51
  { 0, 0 }, //52
  { 0, 0 }, //53
  { 0, 0 }, //54
  { 0, 0 }, //55
  { 0, 0 }, //56
  { 0, 0 }, //57
  { 0, 0 }, //58
  { 0, 0 }, //59
  { 0, 0 }, //60
  { 0, 0 }, //61
  { 0, 0 }, //62
  { 0, 0 }, //63
  { 0, 0 }, //64
  { 0, 0 }, //65
  { 0, 0 }, //66
  { 0, 0 }, //67
  { 0, 0 }, //68
  { 0, 0 }, //69
  { 0, 0 }, //70
  { 0, 0 }, //71
  { 0, 0 }, //72
  { 0, 0 }, //73
  { 0, 0 }, //74
  { 0, 0 }, //75
  { 0, 0 }, //76
  { 0, 0 }, //77
  { 0, 0 }, //78
  { 0, 0 }, //79
  { 0, 0 }, //80
  { 0, 0 }, //81
  { 0, 0 }, //82
  { 0, 0 }, //83
  { 0, 0 }, //84
  { 0, 0 }, //85
  { 0, 0 }, //86
  { 0, 0 }, //87
  { 0, 0 }, //88
  { 0, 0 }, //89
  { 0, 0 }, //90
  { 0, 0 }, //91
  { 0, 0 }, //92
  { 0, 0 }, //93
  { 0, 0 }, //94
  { 0, 0 }, //95
  { 0, 0 }, //96
  { 0, 0 }, //97
  { 0, 0 }, //98
  { 0, 0 }, //99
  { 0, 0 }, //100
  { 0, 0 }, //101
  { 0, 0 }, //102
  { 0, 0 }, //103
  { 0, 0 }, //104
  { 0, 0 }, //105
  { 0, 0 }, //106
  { 0, 0 }, //107
  { 0, 0 }, //108
  { 0, 0 }, //109
  { 0, 0 }, //110
  { 0, 0 }, //111
  { 0, 0 }, //112
  { 0, 0 }, //113
  { 0, 0 }, //114
  { 0, 0 }, //115
  { 0, 0 }, //116
  { 0, 0 }, //117
  { 0, 0 }, //118
  { 0, 0 }, //119
  { 0, 0 }, //120
  { 0, 0 }, //121
  { 0, 0 }, //122
  { 0, 0 }, //123
  { 0, 0 }, //124
  { 0, 0 }, //125
  { 0, 0 }, //126
  { 0, 0 }, //127
  { 0, 0 }, //128
  { 0, 0 }, //129
  { 0, 0 }, //130
  { 0, 0 }, //131
  { 0, 0 }, //132
  { 0, 0 }, //133
  { 0, 0 }, //134
  { 0, 0 }, //135
  { 0, 0 }, //136
  { 0, 0 }, //137
  { 0, 0 }, //138
  { 0, 0 }, //139
  { 0, 0 }, //140
  { 0, 0 }, //141
  { 0, 0 }, //142
  { 0, 0 }, //143
  { 0, 0 }, //144
  { 0, 0 }, //145
  { 0, 0 }, //146
  { 0, 0 }, //147
  { 0, 0 }, //148
  { 0, 0 }, //149
  { 0, 0 }, //150
  { 0, 0 }, //151
  { 0, 0 }, //152
  { 0, 0 }, //153
  { 0, 0 }, //154
  { 0, 0 }, //155
  { 0, 0 }, //156
  { 0, 0 }, //157
  { 0, 0 }, //158
  { 0, 0 }, //159
  { 0, 0 }, //160
  { 0, 0 }, //161
  { 0, 0 }, //162
  { 0, 0 }, //163
  { 0, 0 }, //164
  { 0, 0 }, //165
  { 0, 0 }, //166
  { 0, 0 }, //167
  { 0, 0 }, //168
  { 0, 0 }, //169
  { 0, 0 }, //170
  { 0, 0 }, //171
  { 0, 0 }, //172
  { 0, 0 }, //173
  { 0, 0 }, //174
  { 0, 0 }, //175
  { 0, 0 }, //176
  { 0, 0 }, //177
  { 0, 0 }, //178
  { 0, 0 }, //179
  { 0, 0 }, //180
  { 0, 0 }, //181
  { 0, 0 }, //182
  { 0, 0 }, //183
  { 0, 0 }, //184
  { 0, 0 }, //185
  { 0, 0 }, //186
  { 0, 0 }, //187
  { 0, 0 }, //188
  { 0, 0 }, //189
  { 0, 0 }, //190
  { 0, 0 }, //191
  { 0, 0 }, //192
  { 0, 0 }, //193
  { 0, 0 }, //194
  { 0, 0 }, //195
  { 0, 0 }, //196
  { 0, 0 }, //197
  { 0, 0 }, //198
  { 0, 0 }, //199
  { 0, 0 }, //200
  { 0, 0 }, //201
  { 0, 0 }, //202
  { 0, 0 }, //203
  { 0, 0 }, //204
  { 0, 0 }, //205
  { 0, 0 }, //206
  { 0, 0 }, //207
  { 0, 0 }, //208
  { 0, 0 }, //209
  { 0, 0 }, //210
  { 0, 0 }, //211
  { 0, 0 }, //212
  { 0, 0 }, //213
  { 0, 0 }, //214
  { 0, 0 }, //215
  { 0, 0 }, //216
  { 0, 0 }, //217
  { 0, 0 }, //218
  { 0, 0 }, //219
  { 0, 0 }, //220
  { 0, 0 }, //221
  { 0, 0 }, //222
  { 0, 0 }, //223
  { 0, 0 }, //224
  { 0, 0 }, //225
  { 0, 0 }, //226
  { 0, 0 }, //227
  { 0, 0 }, //228
  { 0, 0 }, //229
  { 0, 0 }, //230
  { 0, 0 }, //231
  { 0, 0 }, //232
  { 0, 0 }, //233
  { 0, 0 }, //234
  { 0, 0 }, //235
  { 0, 0 }, //236
  { 0, 0 }, //237
  { 0, 0 }, //238
  { 0, 0 }, //239
  { 0, 0 }, //240
  { 0, 0 }, //241
  { 0, 0 }, //242
  { 0, 0 }, //243
  { 0, 0 }, //244
  { 0, 0 }, //245
  { 0, 0 }, //246
  { 0, 0 }, //247
  { 0, 0 }, //248
  { 0, 0 }, //249
  { STRING_WITH_LEN("Bulk_execute") }, //250
  { STRING_WITH_LEN("Slave_worker") }, //251
  { STRING_WITH_LEN("Slave_IO") }, //252
  { STRING_WITH_LEN("Slave_SQL") }, //253
  { 0, 0},
  { STRING_WITH_LEN("Error") }  // Last command number 255
};

#ifdef HAVE_REPLICATION
/**
  Returns true if all tables should be ignored.
*/
inline bool all_tables_not_ok(THD *thd, TABLE_LIST *tables)
{
  Rpl_filter *rpl_filter= thd->system_thread_info.rpl_sql_info->rpl_filter;
  return rpl_filter->is_on() && tables && !thd->spcont &&
         !rpl_filter->tables_ok(thd->db.str, tables);
}
#endif


static bool some_non_temp_table_to_be_updated(THD *thd, TABLE_LIST *tables)
{
  for (TABLE_LIST *table= tables; table; table= table->next_global)
  {
    DBUG_ASSERT(table->db.str && table->table_name.str);
    if (table->updating && !thd->find_tmp_table_share(table))
      return 1;
  }
  return 0;
}


/*
  Check whether the statement implicitly commits an active transaction.

  @param thd    Thread handle.
  @param mask   Bitmask used for the SQL command match.

  @return 0     No implicit commit
  @return 1     Do a commit
*/
bool stmt_causes_implicit_commit(THD *thd, uint mask)
{
  LEX *lex= thd->lex;
  bool skip= FALSE;
  DBUG_ENTER("stmt_causes_implicit_commit");

  if (!(sql_command_flags[lex->sql_command] & mask))
    DBUG_RETURN(FALSE);

  switch (lex->sql_command) {
  case SQLCOM_ALTER_TABLE:
  case SQLCOM_ALTER_SEQUENCE:
    /* If ALTER TABLE of non-temporary table, do implicit commit */
    skip= (lex->tmp_table());
    break;
  case SQLCOM_DROP_TABLE:
  case SQLCOM_DROP_SEQUENCE:
  case SQLCOM_CREATE_TABLE:
    /*
      If CREATE TABLE of non-temporary table and the table is not part
      if a BEGIN GTID ... COMMIT group, do a implicit commit.
      This ensures that CREATE ... SELECT will in the same GTID group on the
      master and slave.
    */
    skip= (lex->tmp_table() ||
           (thd->variables.option_bits & OPTION_GTID_BEGIN));
    break;
  case SQLCOM_SET_OPTION:
    skip= lex->autocommit ? FALSE : TRUE;
    break;
  default:
    break;
  }

  DBUG_RETURN(!skip);
}


/**
  Mark all commands that somehow changes a table.

  This is used to check number of updates / hour.

  sql_command is actually set to SQLCOM_END sometimes
  so we need the +1 to include it in the array.

  See COMMAND_FLAG_xxx for different type of commands
     2  - query that returns meaningful ROW_COUNT() -
          a number of modified rows
*/

uint sql_command_flags[SQLCOM_END+1];
uint server_command_flags[COM_END+1];

void init_update_queries(void)
{
  /* Initialize the server command flags array. */
  memset(server_command_flags, 0, sizeof(server_command_flags));

  server_command_flags[COM_STATISTICS]= CF_SKIP_QUERY_ID | CF_SKIP_QUESTIONS | CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_PING]=       CF_SKIP_QUERY_ID | CF_SKIP_QUESTIONS | CF_SKIP_WSREP_CHECK;

  server_command_flags[COM_QUIT]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_PROCESS_INFO]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_PROCESS_KILL]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_SHUTDOWN]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_SLEEP]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_TIME]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_INIT_DB]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_END]= CF_SKIP_WSREP_CHECK;
  for (uint i= COM_MDB_GAP_BEG; i <= COM_MDB_GAP_END; i++)
  {
    server_command_flags[i]= CF_SKIP_WSREP_CHECK;
  }

  /*
    COM_QUERY, COM_SET_OPTION and COM_STMT_XXX are allowed to pass the early
    COM_xxx filter, they're checked later in mysql_execute_command().
  */
  server_command_flags[COM_QUERY]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_SET_OPTION]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_STMT_PREPARE]= CF_SKIP_QUESTIONS | CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_STMT_EXECUTE]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_STMT_FETCH]=   CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_STMT_CLOSE]= CF_SKIP_QUESTIONS | CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_STMT_RESET]= CF_SKIP_QUESTIONS | CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_STMT_EXECUTE]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_STMT_SEND_LONG_DATA]= CF_SKIP_WSREP_CHECK;
  server_command_flags[COM_REGISTER_SLAVE]= CF_SKIP_WSREP_CHECK;

  /* Initialize the sql command flags array. */
  memset(sql_command_flags, 0, sizeof(sql_command_flags));

  /*
    In general, DDL statements do not generate row events and do not go
    through a cache before being written to the binary log. However, the
    CREATE TABLE...SELECT is an exception because it may generate row
    events. For that reason,  the SQLCOM_CREATE_TABLE  which represents
    a CREATE TABLE, including the CREATE TABLE...SELECT, has the
    CF_CAN_GENERATE_ROW_EVENTS flag. The distinction between a regular
    CREATE TABLE and the CREATE TABLE...SELECT is made in other parts of
    the code, in particular in the Query_log_event's constructor.
  */
  sql_command_flags[SQLCOM_CREATE_TABLE]=   CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_AUTO_COMMIT_TRANS | CF_REPORT_PROGRESS |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_SCHEMA_CHANGE;
  sql_command_flags[SQLCOM_CREATE_SEQUENCE]=  (CF_CHANGES_DATA |
                                            CF_REEXECUTION_FRAGILE |
                                            CF_FORCE_ORIGINAL_BINLOG_FORMAT |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_SCHEMA_CHANGE);
  sql_command_flags[SQLCOM_CREATE_INDEX]=   CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS |
                                            CF_ADMIN_COMMAND | CF_REPORT_PROGRESS;
  sql_command_flags[SQLCOM_ALTER_TABLE]=    CF_CHANGES_DATA | CF_WRITE_LOGS_COMMAND |
                                            CF_AUTO_COMMIT_TRANS | CF_REPORT_PROGRESS |
                                            CF_INSERTS_DATA | CF_ADMIN_COMMAND;
  sql_command_flags[SQLCOM_ALTER_SEQUENCE]= CF_CHANGES_DATA | CF_WRITE_LOGS_COMMAND |
                                            CF_AUTO_COMMIT_TRANS | CF_SCHEMA_CHANGE |
                                            CF_ADMIN_COMMAND;
  sql_command_flags[SQLCOM_TRUNCATE]=       CF_CHANGES_DATA | CF_WRITE_LOGS_COMMAND |
                                            CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_TABLE]=     CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS | CF_SCHEMA_CHANGE;
  sql_command_flags[SQLCOM_DROP_SEQUENCE]=  CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS | CF_SCHEMA_CHANGE;
  sql_command_flags[SQLCOM_LOAD]=           CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS | CF_REPORT_PROGRESS |
                                            CF_INSERTS_DATA;
  sql_command_flags[SQLCOM_CREATE_DB]=      CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS | CF_DB_CHANGE;
  sql_command_flags[SQLCOM_DROP_DB]=        CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS | CF_DB_CHANGE;
  sql_command_flags[SQLCOM_CREATE_PACKAGE]= CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_PACKAGE]=   CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_PACKAGE_BODY]= CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_PACKAGE_BODY]= CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_DB_UPGRADE]= CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_DB]=       CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS | CF_DB_CHANGE;
  sql_command_flags[SQLCOM_RENAME_TABLE]=   CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS | CF_ADMIN_COMMAND;
  sql_command_flags[SQLCOM_DROP_INDEX]=     CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS |
                                            CF_REPORT_PROGRESS | CF_ADMIN_COMMAND;
  sql_command_flags[SQLCOM_CREATE_VIEW]=    CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_VIEW]=      CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_TRIGGER]= CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_TRIGGER]=   CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_EVENT]=   CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_EVENT]=    CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_EVENT]=     CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;

  sql_command_flags[SQLCOM_UPDATE]=	    CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED |
                                            CF_UPDATES_DATA |
                                            CF_PS_ARRAY_BINDING_SAFE;
  sql_command_flags[SQLCOM_UPDATE_MULTI]=   CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED |
                                            CF_UPDATES_DATA |
                                            CF_PS_ARRAY_BINDING_SAFE;
  sql_command_flags[SQLCOM_INSERT]=	    CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED |
                                            CF_INSERTS_DATA |
                                            CF_PS_ARRAY_BINDING_SAFE |
                                            CF_PS_ARRAY_BINDING_OPTIMIZED;
  sql_command_flags[SQLCOM_INSERT_SELECT]=  CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED |
                                            CF_INSERTS_DATA;
  sql_command_flags[SQLCOM_DELETE]=         CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED |
                                            CF_DELETES_DATA |
                                            CF_PS_ARRAY_BINDING_SAFE;
  sql_command_flags[SQLCOM_DELETE_MULTI]=   CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED |
                                            CF_DELETES_DATA;
  sql_command_flags[SQLCOM_REPLACE]=        CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED |
                                            CF_INSERTS_DATA |
                                            CF_PS_ARRAY_BINDING_SAFE |
                                            CF_PS_ARRAY_BINDING_OPTIMIZED;
  sql_command_flags[SQLCOM_REPLACE_SELECT]= CF_CHANGES_DATA | CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED |
                                            CF_INSERTS_DATA;
  sql_command_flags[SQLCOM_SELECT]=         CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE |
                                            CF_CAN_BE_EXPLAINED;
  // (1) so that subquery is traced when doing "SET @var = (subquery)"
  /*
    @todo SQLCOM_SET_OPTION should have CF_CAN_GENERATE_ROW_EVENTS
    set, because it may invoke a stored function that generates row
    events. /Sven
  */
  sql_command_flags[SQLCOM_SET_OPTION]=     CF_REEXECUTION_FRAGILE |
                                            CF_AUTO_COMMIT_TRANS |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE; // (1)
  // (1) so that subquery is traced when doing "DO @var := (subquery)"
  sql_command_flags[SQLCOM_DO]=             CF_REEXECUTION_FRAGILE |
                                            CF_CAN_GENERATE_ROW_EVENTS |
                                            CF_OPTIMIZER_TRACE; // (1)

  sql_command_flags[SQLCOM_SHOW_STATUS_PROC]= CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_STATUS_PACKAGE]= CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_STATUS_PACKAGE_BODY]= CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_STATUS]=      CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_DATABASES]=   CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_TRIGGERS]=    CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_EVENTS]=      CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_OPEN_TABLES]= CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_PLUGINS]=     CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_GENERIC]=     CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_FIELDS]=      CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_KEYS]=        CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_VARIABLES]=   CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_CHARSETS]=    CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_COLLATIONS]=  CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_BINLOGS]=     CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_SLAVE_HOSTS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_BINLOG_EVENTS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_STORAGE_ENGINES]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_AUTHORS]=     CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CONTRIBUTORS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PRIVILEGES]=  CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_WARNS]=       CF_STATUS_COMMAND | CF_DIAGNOSTIC_STMT;
  sql_command_flags[SQLCOM_SHOW_ERRORS]=      CF_STATUS_COMMAND | CF_DIAGNOSTIC_STMT;
  sql_command_flags[SQLCOM_SHOW_ENGINE_STATUS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_ENGINE_MUTEX]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_ENGINE_LOGS]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_EXPLAIN]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_ANALYZE]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PROCESSLIST]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_GRANTS]=      CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_USER]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_DB]=   CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE]=  CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_BINLOG_STAT]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_SLAVE_STAT]=  CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_PROC]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_FUNC]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_PACKAGE]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_PACKAGE_BODY]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_TRIGGER]=  CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_STATUS_FUNC]= CF_STATUS_COMMAND | CF_REEXECUTION_FRAGILE;
  sql_command_flags[SQLCOM_SHOW_PROC_CODE]=   CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_FUNC_CODE]=   CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PACKAGE_BODY_CODE]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_CREATE_EVENT]= CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PROFILES]=    CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_SHOW_PROFILE]=     CF_STATUS_COMMAND;
  sql_command_flags[SQLCOM_BINLOG_BASE64_EVENT]= CF_STATUS_COMMAND | CF_CAN_GENERATE_ROW_EVENTS;
  sql_command_flags[SQLCOM_SHOW_TABLES]=       (CF_STATUS_COMMAND | CF_SHOW_TABLE_COMMAND | CF_REEXECUTION_FRAGILE);
  sql_command_flags[SQLCOM_SHOW_TABLE_STATUS]= (CF_STATUS_COMMAND | CF_SHOW_TABLE_COMMAND | CF_REEXECUTION_FRAGILE);


  sql_command_flags[SQLCOM_CREATE_USER]=       CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_RENAME_USER]=       CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_DROP_USER]=         CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_ALTER_USER]=        CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_CREATE_ROLE]=       CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_GRANT]=             CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_GRANT_ROLE]=        CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_REVOKE]=            CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_REVOKE_ROLE]=       CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_OPTIMIZE]=          CF_CHANGES_DATA;
  sql_command_flags[SQLCOM_CREATE_FUNCTION]=   CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_PROCEDURE]=  CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_SPFUNCTION]= CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_PROCEDURE]=    CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_FUNCTION]=     CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_PROCEDURE]=   CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_FUNCTION]=    CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_INSTALL_PLUGIN]=    CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_UNINSTALL_PLUGIN]=  CF_CHANGES_DATA | CF_AUTO_COMMIT_TRANS;

  /*
    The following is used to preserver CF_ROW_COUNT during the
    a CALL or EXECUTE statement, so the value generated by the
    last called (or executed) statement is preserved.
    See mysql_execute_command() for how CF_ROW_COUNT is used.
  */
  /*
    (1): without it, in "CALL some_proc((subq))", subquery would not be
    traced.
  */
  sql_command_flags[SQLCOM_CALL]=      CF_REEXECUTION_FRAGILE |
                                       CF_CAN_GENERATE_ROW_EVENTS |
                                       CF_OPTIMIZER_TRACE; // (1)
  sql_command_flags[SQLCOM_EXECUTE]=   CF_CAN_GENERATE_ROW_EVENTS;
  sql_command_flags[SQLCOM_EXECUTE_IMMEDIATE]= CF_CAN_GENERATE_ROW_EVENTS;
  sql_command_flags[SQLCOM_COMPOUND]=  CF_CAN_GENERATE_ROW_EVENTS;

  /*
    We don't want to change to statement based replication for these commands
  */
  sql_command_flags[SQLCOM_ROLLBACK]|= CF_FORCE_ORIGINAL_BINLOG_FORMAT;
  /* We don't want to replicate ALTER TABLE for temp tables in row format */
  sql_command_flags[SQLCOM_ALTER_TABLE]|= CF_FORCE_ORIGINAL_BINLOG_FORMAT;
  /* We don't want to replicate TRUNCATE for temp tables in row format */
  sql_command_flags[SQLCOM_TRUNCATE]|= CF_FORCE_ORIGINAL_BINLOG_FORMAT;
  /* We don't want to replicate DROP for temp tables in row format */
  sql_command_flags[SQLCOM_DROP_TABLE]|= CF_FORCE_ORIGINAL_BINLOG_FORMAT;
  sql_command_flags[SQLCOM_DROP_SEQUENCE]|= CF_FORCE_ORIGINAL_BINLOG_FORMAT;
  /* We don't want to replicate CREATE/DROP INDEX for temp tables in row format */
  sql_command_flags[SQLCOM_CREATE_INDEX]|= CF_FORCE_ORIGINAL_BINLOG_FORMAT;
  sql_command_flags[SQLCOM_DROP_INDEX]|= CF_FORCE_ORIGINAL_BINLOG_FORMAT;
  /* One can change replication mode with SET */
  sql_command_flags[SQLCOM_SET_OPTION]|= CF_FORCE_ORIGINAL_BINLOG_FORMAT;

  /*
    The following admin table operations are allowed
    on log tables.
  */
  sql_command_flags[SQLCOM_REPAIR]=    CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS |
                                       CF_REPORT_PROGRESS | CF_ADMIN_COMMAND;
  sql_command_flags[SQLCOM_OPTIMIZE]|= CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS |
                                       CF_REPORT_PROGRESS | CF_ADMIN_COMMAND;
  sql_command_flags[SQLCOM_ANALYZE]=   CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS |
                                       CF_REPORT_PROGRESS | CF_ADMIN_COMMAND;
  sql_command_flags[SQLCOM_CHECK]=     CF_WRITE_LOGS_COMMAND | CF_AUTO_COMMIT_TRANS |
                                       CF_REPORT_PROGRESS | CF_ADMIN_COMMAND;
  sql_command_flags[SQLCOM_CHECKSUM]=  CF_REPORT_PROGRESS;

  sql_command_flags[SQLCOM_CREATE_USER]|=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_USER]|=        CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_USER]|=         CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_RENAME_USER]|=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_ROLE]|=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_ROLE]|=         CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_REVOKE]|=            CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_REVOKE_ALL]=         CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_REVOKE_ROLE]|=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_GRANT]|=             CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_GRANT_ROLE]|=        CF_AUTO_COMMIT_TRANS;

  sql_command_flags[SQLCOM_FLUSH]=              CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_RESET]=              CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_CREATE_SERVER]=      CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_ALTER_SERVER]=       CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_DROP_SERVER]=        CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_BACKUP]=             CF_AUTO_COMMIT_TRANS;
  sql_command_flags[SQLCOM_BACKUP_LOCK]=        CF_AUTO_COMMIT_TRANS;

  /*
    The following statements can deal with temporary tables,
    so temporary tables should be pre-opened for those statements to
    simplify privilege checking.

    There are other statements that deal with temporary tables and open
    them, but which are not listed here. The thing is that the order of
    pre-opening temporary tables for those statements is somewhat custom.

    Note that SQLCOM_RENAME_TABLE should not be in this list!
  */
  sql_command_flags[SQLCOM_CREATE_TABLE]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CREATE_SEQUENCE]|= CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CREATE_INDEX]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=     CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_TRUNCATE]|=        CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_LOAD]|=            CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DROP_INDEX]|=      CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_UPDATE]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_UPDATE_MULTI]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_INSERT_SELECT]|=   CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DELETE]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DELETE_MULTI]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_REPLACE_SELECT]|=  CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_SELECT]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_SET_OPTION]|=      CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_DO]|=              CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_HA_OPEN]|=         CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CALL]|=            CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CHECKSUM]|=        CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_ANALYZE]|=         CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_CHECK]|=           CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_OPTIMIZE]|=        CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_REPAIR]|=          CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_PRELOAD_KEYS]|=    CF_PREOPEN_TMP_TABLES;
  sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE]|= CF_PREOPEN_TMP_TABLES;

  /*
    DDL statements that should start with closing opened handlers.

    We use this flag only for statements for which open HANDLERs
    have to be closed before temporary tables are pre-opened.
  */
  sql_command_flags[SQLCOM_CREATE_TABLE]|=    CF_HA_CLOSE;
  sql_command_flags[SQLCOM_CREATE_SEQUENCE]|= CF_HA_CLOSE;
  sql_command_flags[SQLCOM_DROP_TABLE]|=      CF_HA_CLOSE;
  sql_command_flags[SQLCOM_DROP_SEQUENCE]|=   CF_HA_CLOSE;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=     CF_HA_CLOSE;
  sql_command_flags[SQLCOM_TRUNCATE]|=        CF_HA_CLOSE;
  sql_command_flags[SQLCOM_REPAIR]|=          CF_HA_CLOSE;
  sql_command_flags[SQLCOM_OPTIMIZE]|=        CF_HA_CLOSE;
  sql_command_flags[SQLCOM_ANALYZE]|=         CF_HA_CLOSE;
  sql_command_flags[SQLCOM_CHECK]|=           CF_HA_CLOSE;
  sql_command_flags[SQLCOM_CREATE_INDEX]|=    CF_HA_CLOSE;
  sql_command_flags[SQLCOM_DROP_INDEX]|=      CF_HA_CLOSE;
  sql_command_flags[SQLCOM_PRELOAD_KEYS]|=    CF_HA_CLOSE;
  sql_command_flags[SQLCOM_ASSIGN_TO_KEYCACHE]|=  CF_HA_CLOSE;

  /*
    Mark statements that always are disallowed in read-only
    transactions. Note that according to the SQL standard,
    even temporary table DDL should be disallowed.
  */
  sql_command_flags[SQLCOM_CREATE_TABLE]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_SEQUENCE]|=  CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_TABLE]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_SEQUENCE]|=    CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_RENAME_TABLE]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_INDEX]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_INDEX]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_DB]|=        CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_DB]|=          CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_PACKAGE]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_PACKAGE]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_PACKAGE_BODY]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_PACKAGE_BODY]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_DB_UPGRADE]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_DB]|=         CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_VIEW]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_VIEW]|=        CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_TRIGGER]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_TRIGGER]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_EVENT]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_EVENT]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_EVENT]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_USER]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_USER]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_RENAME_USER]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_USER]|=        CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_SERVER]|=    CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_SERVER]|=     CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_SERVER]|=      CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_FUNCTION]|=  CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_PROCEDURE]|= CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_CREATE_SPFUNCTION]|=CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_PROCEDURE]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_DROP_FUNCTION]|=    CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_PROCEDURE]|=  CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_ALTER_FUNCTION]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_TRUNCATE]|=         CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_REPAIR]|=           CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_OPTIMIZE]|=         CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_GRANT]|=            CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_REVOKE]|=           CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_REVOKE_ALL]|=       CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_INSTALL_PLUGIN]|=   CF_DISALLOW_IN_RO_TRANS;
  sql_command_flags[SQLCOM_UNINSTALL_PLUGIN]|= CF_DISALLOW_IN_RO_TRANS;
#ifdef WITH_WSREP
  /*
    Statements for which some errors are ignored when
    wsrep_ignore_apply_errors = WSREP_IGNORE_ERRORS_ON_RECONCILING_DDL
  */
  sql_command_flags[SQLCOM_DROP_DB]|=          CF_WSREP_MAY_IGNORE_ERRORS;
  sql_command_flags[SQLCOM_DROP_TABLE]|=       CF_WSREP_MAY_IGNORE_ERRORS;
  sql_command_flags[SQLCOM_DROP_INDEX]|=       CF_WSREP_MAY_IGNORE_ERRORS;
  sql_command_flags[SQLCOM_ALTER_TABLE]|=      CF_WSREP_MAY_IGNORE_ERRORS;
  /*
    Basic DML-statements that create writeset.
  */
  sql_command_flags[SQLCOM_INSERT]|=           CF_WSREP_BASIC_DML;
  sql_command_flags[SQLCOM_INSERT_SELECT]|=    CF_WSREP_BASIC_DML;
  sql_command_flags[SQLCOM_REPLACE]|=          CF_WSREP_BASIC_DML;
  sql_command_flags[SQLCOM_REPLACE_SELECT]|=   CF_WSREP_BASIC_DML;
  sql_command_flags[SQLCOM_UPDATE]|=           CF_WSREP_BASIC_DML;
  sql_command_flags[SQLCOM_UPDATE_MULTI]|=     CF_WSREP_BASIC_DML;
  sql_command_flags[SQLCOM_LOAD]|=             CF_WSREP_BASIC_DML;
  sql_command_flags[SQLCOM_DELETE]|=           CF_WSREP_BASIC_DML;
  sql_command_flags[SQLCOM_DELETE_MULTI]|=     CF_WSREP_BASIC_DML;
#endif /* WITH_WSREP */
}

bool sqlcom_can_generate_row_events(const THD *thd)
{
  return (sql_command_flags[thd->lex->sql_command] &
          CF_CAN_GENERATE_ROW_EVENTS);
}
 
bool is_update_query(enum enum_sql_command command)
{
  DBUG_ASSERT(command <= SQLCOM_END);
  return (sql_command_flags[command] & CF_CHANGES_DATA) != 0;
}

/**
  Check if a sql command is allowed to write to log tables.
  @param command The SQL command
  @return true if writing is allowed
*/
bool is_log_table_write_query(enum enum_sql_command command)
{
  DBUG_ASSERT(command <= SQLCOM_END);
  return (sql_command_flags[command] & CF_WRITE_LOGS_COMMAND) != 0;
}

void execute_init_command(THD *thd, LEX_STRING *init_command,
                          mysql_rwlock_t *var_lock)
{
  Vio* save_vio;
  ulonglong save_client_capabilities;

  mysql_rwlock_rdlock(var_lock);
  if (!init_command->length)
  {
    mysql_rwlock_unlock(var_lock);
    return;
  }

  /*
    copy the value under a lock, and release the lock.
    init_command has to be executed without a lock held,
    as it may try to change itself
  */
  size_t len= init_command->length;
  char *buf= thd->strmake(init_command->str, len);
  mysql_rwlock_unlock(var_lock);

  THD_STAGE_INFO(thd, stage_execution_of_init_command);
  save_client_capabilities= thd->client_capabilities;
  thd->client_capabilities|= CLIENT_MULTI_QUERIES;
  /*
    We don't need return result of execution to client side.
    To forbid this we should set thd->net.vio to 0.
  */
  save_vio= thd->net.vio;
  thd->net.vio= 0;
  thd->clear_error(1);
  dispatch_command(COM_QUERY, thd, buf, (uint)len);
  thd->client_capabilities= save_client_capabilities;
  thd->net.vio= save_vio;

}


static char *fgets_fn(char *buffer, size_t size, fgets_input_t input, int *error)
{
  MYSQL_FILE *in= static_cast<MYSQL_FILE*> (input);
  char *line= mysql_file_fgets(buffer, (int)size, in);
  if (unlikely(error))
    *error= (line == NULL) ? ferror(in->m_file) : 0;
  return line;
}


int bootstrap(MYSQL_FILE *file)
{
  int bootstrap_error= 0;
  DBUG_ENTER("handle_bootstrap");

  THD *thd= new THD(next_thread_id());
  char *buffer= new char[MAX_BOOTSTRAP_QUERY_SIZE];
#ifdef WITH_WSREP
  thd->variables.wsrep_on= 0;
#endif
  thd->bootstrap=1;
  my_net_init(&thd->net,(st_vio*) 0, thd, MYF(0));
  thd->max_client_packet_length= thd->net.max_packet;
  thd->security_ctx->master_access= ALL_KNOWN_ACL;

#ifndef EMBEDDED_LIBRARY
  mysql_thread_set_psi_id(thd->thread_id);
#else
  thd->mysql= 0;
#endif

  /* The following must be called before DBUG_ENTER */
  thd->store_globals();

  thd->security_ctx->user= (char*) my_strdup(key_memory_MPVIO_EXT_auth_info,
                                             "boot", MYF(MY_WME));
  thd->security_ctx->priv_user[0]= thd->security_ctx->priv_host[0]=
    thd->security_ctx->priv_role[0]= 0;
  /*
    Make the "client" handle multiple results. This is necessary
    to enable stored procedures with SELECTs and Dynamic SQL
    in init-file.
  */
  thd->client_capabilities|= CLIENT_MULTI_RESULTS;

  thd->init_for_queries();

  for ( ; ; )
  {
    buffer[0]= 0;
    int rc, length;
    char *query;
    int error= 0;

    rc= read_bootstrap_query(buffer, &length, file, fgets_fn, 0, &error);

    if (rc == READ_BOOTSTRAP_EOF)
      break;
    /*
      Check for bootstrap file errors. SQL syntax errors will be
      caught below.
    */
    if (rc != READ_BOOTSTRAP_SUCCESS)
    {
      /*
        mysql_parse() may have set a successful error status for the previous
        query. We must clear the error status to report the bootstrap error.
      */
      thd->get_stmt_da()->reset_diagnostics_area();

      /* Get the nearest query text for reference. */
      char *err_ptr= buffer + (length <= MAX_BOOTSTRAP_ERROR_LEN ?
                                        0 : (length - MAX_BOOTSTRAP_ERROR_LEN));
      switch (rc)
      {
      case READ_BOOTSTRAP_ERROR:
        my_printf_error(ER_UNKNOWN_ERROR, "Bootstrap file error, return code (%d). "
                        "Nearest query: '%s'", MYF(0), error, err_ptr);
        break;

      case READ_BOOTSTRAP_QUERY_SIZE:
        my_printf_error(ER_UNKNOWN_ERROR, "Bootstrap file error. Query size "
                        "exceeded %d bytes near '%s'.", MYF(0),
                        MAX_BOOTSTRAP_QUERY_SIZE, err_ptr);
        break;

      default:
        DBUG_ASSERT(false);
        break;
      }

      thd->protocol->end_statement();
      bootstrap_error= 1;
      break;
    }

    query= (char *) thd->memdup_w_gap(buffer, length + 1,
                                      thd->db.length + 1 +
                                      QUERY_CACHE_DB_LENGTH_SIZE +
                                      QUERY_CACHE_FLAGS_SIZE);
    size_t db_len= 0;
    memcpy(query + length + 1, (char *) &db_len, sizeof(size_t));
    thd->set_query_and_id(query, length, thd->charset(), next_query_id());
    int2store(query + length + 1, 0);           // No db in bootstrap
    DBUG_PRINT("query",("%-.4096s",thd->query()));
#if defined(ENABLED_PROFILING)
    thd->profiling.start_new_query();
    thd->profiling.set_query_source(thd->query(), length);
#endif

    thd->set_time();
    Parser_state parser_state;
    if (parser_state.init(thd, thd->query(), length))
    {
      thd->protocol->end_statement();
      bootstrap_error= 1;
      break;
    }

    mysql_parse(thd, thd->query(), length, &parser_state);

    bootstrap_error= thd->is_error();
    thd->protocol->end_statement();

#if defined(ENABLED_PROFILING)
    thd->profiling.finish_current_query();
#endif
    delete_explain_query(thd->lex);

    if (unlikely(bootstrap_error))
      break;

    thd->reset_kill_query();  /* Ensure that killed_errmsg is released */
    free_root(thd->mem_root,MYF(MY_KEEP_PREALLOC));
    thd->lex->restore_set_statement_var();
  }
  delete thd;
  delete[] buffer;
  DBUG_RETURN(bootstrap_error);
}


/* This works because items are allocated on THD::mem_root */

void free_items(Item *item)
{
  Item *next;
  DBUG_ENTER("free_items");
  for (; item ; item=next)
  {
    next=item->next;
    item->delete_self();
  }
  DBUG_VOID_RETURN;
}

/**
   This works because items are allocated on THD::mem_root.
   @note The function also handles null pointers (empty list).
*/
void cleanup_items(Item *item)
{
  DBUG_ENTER("cleanup_items");  
  for (; item ; item=item->next)
    item->cleanup();
  DBUG_VOID_RETURN;
}

#ifdef WITH_WSREP
static bool wsrep_tables_accessible_when_detached(const TABLE_LIST *tables)
{
  for (const TABLE_LIST *t= tables; t; t= t->next_global)
  {
    if (get_table_category(t->db, t->table_name) < TABLE_CATEGORY_INFORMATION)
      return false;
  }
  return tables != NULL;
}

static bool wsrep_command_no_result(char command)
{
  return (command == COM_STMT_FETCH            ||
          command == COM_STMT_SEND_LONG_DATA   ||
          command == COM_STMT_CLOSE);
}
#endif /* WITH_WSREP */
#ifndef EMBEDDED_LIBRARY
static enum enum_server_command fetch_command(THD *thd, char *packet)
{
  enum enum_server_command
    command= (enum enum_server_command) (uchar) packet[0];
  DBUG_ENTER("fetch_command");

  if (command >= COM_END ||
      (command >= COM_MDB_GAP_BEG && command <= COM_MDB_GAP_END))
    command= COM_END;				// Wrong command

  DBUG_PRINT("info",("Command on %s = %d (%s)",
                     vio_description(thd->net.vio), command,
                     command_name[command].str));
  DBUG_RETURN(command);
}

/**
  Read one command from connection and execute it (query or simple command).
  This function is to be used by different schedulers (one-thread-per-connection,
  pool-of-threads)

  For profiling to work, it must never be called recursively.

  @param thd - client connection context

  @param blocking - wait for command to finish.
                    if false (nonblocking), then the function might
                    return when command is "half-finished", with
                    DISPATCH_COMMAND_WOULDBLOCK.
                    Currenly, this can *only* happen when using
                    threadpool. The command will resume, after all outstanding
                    async operations (i.e group commit) finish.
                    Threadpool scheduler takes care of "resume".

  @retval
    DISPATCH_COMMAND_SUCCESS - success
  @retval
    DISPATCH_COMMAND_CLOSE_CONNECTION  request of THD shutdown
          (s. dispatch_command() description)
  @retval
    DISPATCH_COMMAND_WOULDBLOCK - need to wait for asynchronous operations
                                  to finish. Only returned if parameter
                                  'blocking' is false.
*/

dispatch_command_return do_command(THD *thd, bool blocking)
{
  dispatch_command_return return_value;
  char *packet= 0;
  ulong packet_length;
  NET *net= &thd->net;
  enum enum_server_command command;
  DBUG_ENTER("do_command");

#ifdef WITH_WSREP
  DBUG_ASSERT(!thd->async_state.pending_ops() ||
              (WSREP(thd) &&
               thd->wsrep_trx().state() == wsrep::transaction::s_aborted));
#else
  DBUG_ASSERT(!thd->async_state.pending_ops());
#endif

  if (thd->async_state.m_state == thd_async_state::enum_async_state::RESUMED)
  {
    /*
     Resuming previously suspended command.
     Restore the state
    */
    command = thd->async_state.m_command;
    packet = thd->async_state.m_packet.str;
    packet_length = (ulong)thd->async_state.m_packet.length;
    goto resume;
  }

  /*
    indicator of uninitialized lex => normal flow of errors handling
    (see my_message_sql)
  */
  thd->lex->current_select= 0;

  /*
    This thread will do a blocking read from the client which
    will be interrupted when the next command is received from
    the client, the connection is closed or "net_wait_timeout"
    number of seconds has passed.
  */
  if (!thd->skip_wait_timeout)
    my_net_set_read_timeout(net, thd->get_net_wait_timeout());

  /* Errors and diagnostics are cleared once here before query */
  thd->clear_error(1);

  net_new_transaction(net);

  /* Save for user statistics */
  thd->start_bytes_received= thd->status_var.bytes_received;

  /*
    Synchronization point for testing of KILL_CONNECTION.
    This sync point can wait here, to simulate slow code execution
    between the last test of thd->killed and blocking in read().

    The goal of this test is to verify that a connection does not
    hang, if it is killed at this point of execution.
    (Bug#37780 - main.kill fails randomly)

    Note that the sync point wait itself will be terminated by a
    kill. In this case it consumes a condition broadcast, but does
    not change anything else. The consumed broadcast should not
    matter here, because the read/recv() below doesn't use it.
  */
  DEBUG_SYNC(thd, "before_do_command_net_read");

  packet_length= my_net_read_packet(net, 1);

  if (unlikely(packet_length == packet_error))
  {
    DBUG_PRINT("info",("Got error %d reading command from socket %s",
		       net->error,
		       vio_description(net->vio)));

    /* Instrument this broken statement as "statement/com/error" */
    thd->m_statement_psi= MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                                                 com_statement_info[COM_END].
                                                 m_key);


    /* Check if we can continue without closing the connection */

    /* The error must be set. */
    DBUG_ASSERT(thd->is_error());
    thd->protocol->end_statement();

    /* Mark the statement completed. */
    MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
    thd->m_statement_psi= NULL;
    thd->m_digest= NULL;

    if (net->error != 3)
    {
      return_value= DISPATCH_COMMAND_CLOSE_CONNECTION;     // We have to close it.
      goto out;
    }

    net->error= 0;
    return_value= DISPATCH_COMMAND_SUCCESS;
    goto out;
  }

  packet= (char*) net->read_pos;
  /*
    'packet_length' contains length of data, as it was stored in packet
    header. In case of malformed header, my_net_read returns zero.
    If packet_length is not zero, my_net_read ensures that the returned
    number of bytes was actually read from network.
    There is also an extra safety measure in my_net_read:
    it sets packet[packet_length]= 0, but only for non-zero packets.
  */
  if (packet_length == 0)                       /* safety */
  {
    /* Initialize with COM_SLEEP packet */
    packet[0]= (uchar) COM_SLEEP;
    packet_length= 1;
  }
  /* Do not rely on my_net_read, extra safety against programming errors. */
  packet[packet_length]= '\0';                  /* safety */


  command= fetch_command(thd, packet);

#ifdef WITH_WSREP
  DEBUG_SYNC(thd, "wsrep_before_before_command");
  /*
    If this command does not return a result, then we
    instruct wsrep_before_command() to skip result handling.
    This causes BF aborted transaction to roll back but keep
    the error state until next command which is able to return
    a result to the client.
  */
  if (unlikely(wsrep_service_started) &&
      wsrep_before_command(thd, wsrep_command_no_result(command)))
  {
    /*
      Aborted by background rollbacker thread.
      Handle error here and jump straight to out.
      Notice that thd->store_globals() is called
      in wsrep_before_command().
    */
    WSREP_LOG_THD(thd, "enter found BF aborted");
    DBUG_ASSERT(!thd->mdl_context.has_transactional_locks());
    DBUG_ASSERT(!thd->get_stmt_da()->is_set());
    /* We let COM_QUIT and COM_STMT_CLOSE to execute even if wsrep aborted. */
    if (command == COM_STMT_EXECUTE)
    {
      WSREP_DEBUG("PS BF aborted at do_command");
      thd->wsrep_delayed_BF_abort= true;
    }
    if (command != COM_STMT_CLOSE   &&
	command != COM_STMT_EXECUTE &&
        command != COM_QUIT)
    {
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      WSREP_DEBUG("Deadlock error for: %s", thd->query());
      thd->reset_killed();
      thd->mysys_var->abort     = 0;
      thd->wsrep_retry_counter  = 0;

      /* Instrument this broken statement as "statement/com/error" */
      thd->m_statement_psi= MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                                                 com_statement_info[COM_END].
                                                 m_key);

      thd->protocol->end_statement();

      /* Mark the statement completed. */
      MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
      thd->m_statement_psi= NULL;
      thd->m_digest= NULL;
      return_value= DISPATCH_COMMAND_SUCCESS;

      wsrep_after_command_before_result(thd);
      goto out;
    }
  }

  if (WSREP(thd))
  {
    /*
     * bail out if DB snapshot has not been installed. We however,
     * allow queries "SET" and "SHOW", they are trapped later in execute_command
     */
    if (!(thd->wsrep_applier) &&
        (!wsrep_ready_get() || wsrep_reject_queries != WSREP_REJECT_NONE) &&
        (server_command_flags[command] & CF_SKIP_WSREP_CHECK) == 0)
    {
      my_message(ER_UNKNOWN_COM_ERROR,
                 "WSREP has not yet prepared node for application use", MYF(0));
      thd->protocol->end_statement();

      /* Performance Schema Interface instrumentation end. */
      MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
      thd->m_statement_psi= NULL;
      thd->m_digest= NULL;

      return_value= DISPATCH_COMMAND_SUCCESS;
      wsrep_after_command_before_result(thd);
      goto out;
    }
  }
#endif /* WITH_WSREP */
  /* Restore read timeout value */
  my_net_set_read_timeout(net, thd->variables.net_read_timeout);

  DBUG_ASSERT(packet_length);
  DBUG_ASSERT(!thd->apc_target.is_enabled());

resume:
  return_value= dispatch_command(command, thd, packet+1,
                                 (uint) (packet_length-1), blocking);
  if (return_value == DISPATCH_COMMAND_WOULDBLOCK)
  {
    /* Save current state, and resume later.*/
    thd->async_state.m_command= command;
    thd->async_state.m_packet={packet,packet_length};
    DBUG_RETURN(return_value);
  }

  DBUG_ASSERT(!thd->apc_target.is_enabled());

out:
  thd->lex->restore_set_statement_var();
  /* The statement instrumentation must be closed in all cases. */
  DBUG_ASSERT(thd->m_digest == NULL);
  DBUG_ASSERT(thd->m_statement_psi == NULL);
#ifdef WITH_WSREP
  if (packet_length != packet_error)
  {
    /* there was a command to process, and before_command() has been called */
    if (unlikely(wsrep_service_started))
      wsrep_after_command_after_result(thd);
  }

  if (thd->wsrep_delayed_BF_abort)
  {
      my_error(ER_LOCK_DEADLOCK, MYF(0));
      WSREP_DEBUG("Deadlock error for PS query: %s", thd->query());
      thd->reset_killed();
      thd->mysys_var->abort     = 0;
      thd->wsrep_retry_counter  = 0;

      thd->wsrep_delayed_BF_abort= false;
  }
#endif /* WITH_WSREP */
  DBUG_RETURN(return_value);
}
#endif  /* EMBEDDED_LIBRARY */

/**
  @brief Determine if an attempt to update a non-temporary table while the
    read-only option was enabled has been made.

  This is a helper function to mysql_execute_command.

  @note SQLCOM_MULTI_UPDATE is an exception and dealt with elsewhere.

  @see mysql_execute_command
  @returns Status code
    @retval TRUE The statement should be denied.
    @retval FALSE The statement isn't updating any relevant tables.
*/

static bool deny_updates_if_read_only_option(THD *thd, TABLE_LIST *all_tables)
{
  DBUG_ENTER("deny_updates_if_read_only_option");
  DBUG_ASSERT(!thd->slave_thread);              // Checked by caller

  if (!opt_readonly)
    DBUG_RETURN(FALSE);

  LEX *lex= thd->lex;

  /* Super user is allowed to do changes in some cases */
  if ((thd->security_ctx->master_access & PRIV_IGNORE_READ_ONLY) != NO_ACL &&
      opt_readonly < READONLY_NO_LOCK_NO_ADMIN)
    DBUG_RETURN(FALSE);

  /* Check if command doesn't update anything */
  if (!(sql_command_flags[lex->sql_command] & CF_CHANGES_DATA))
    DBUG_RETURN(FALSE);

  /* Multi update is an exception and is dealt with later. */
  if (lex->sql_command == SQLCOM_UPDATE_MULTI)
    DBUG_RETURN(FALSE);

  /*
    a table-to-be-created is not in the temp table list yet,
    so CREATE TABLE needs a special treatment
  */
  if (lex->sql_command == SQLCOM_CREATE_TABLE)
    DBUG_RETURN(!lex->tmp_table());

  /*
    a table-to-be-dropped might not exist (DROP TEMPORARY TABLE IF EXISTS),
    cannot use the temp table list either.
  */
  if (lex->sql_command == SQLCOM_DROP_TABLE && lex->tmp_table())
    DBUG_RETURN(FALSE);

  /* Check if we created, dropped, or renamed a database */
  if ((sql_command_flags[lex->sql_command] & CF_DB_CHANGE))
    DBUG_RETURN(TRUE);

  if (some_non_temp_table_to_be_updated(thd, all_tables))
    DBUG_RETURN(TRUE);

  /* Assuming that only temporary tables are modified. */
  DBUG_RETURN(FALSE);
}

#ifdef WITH_WSREP
static void wsrep_copy_query(THD *thd)
{
  thd->wsrep_retry_command   = thd->get_command();
  thd->wsrep_retry_query_len = thd->query_length();
  if (thd->wsrep_retry_query) {
      my_free(thd->wsrep_retry_query);
  }
  thd->wsrep_retry_query = (char *)my_malloc(PSI_INSTRUMENT_ME,
                                 thd->wsrep_retry_query_len + 1, MYF(0));
  strncpy(thd->wsrep_retry_query, thd->query(), thd->wsrep_retry_query_len);
  thd->wsrep_retry_query[thd->wsrep_retry_query_len] = '\0';
}
#endif /* WITH_WSREP */



#if defined(WITH_ARIA_STORAGE_ENGINE)
class Silence_all_errors : public Internal_error_handler
{
  char m_message[MYSQL_ERRMSG_SIZE];
  int error;
public:
  Silence_all_errors():error(0) {}
  ~Silence_all_errors() override {}

  bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sql_state,
                                Sql_condition::enum_warning_level *level,
                                const char* msg,
                                Sql_condition ** cond_hdl) override
  {
    error= sql_errno;
    *cond_hdl= NULL;
    strmake_buf(m_message, msg);
    return true;                              // Error handled
  }
};
#endif


/**
  Perform one connection-level (COM_XXXX) command.

  @param command         type of command to perform
  @param thd             connection handle
  @param packet          data for the command, packet is always null-terminated
  @param packet_length   length of packet + 1 (to show that data is
                         null-terminated) except for COM_SLEEP, where it
                         can be zero.
  @param blocking        if false (nonblocking), then the function might
                         return when command is "half-finished", with
                         DISPATCH_COMMAND_WOULDBLOCK.
                         Currenly, this can *only* happen when using threadpool.
                         The current command will resume, after all outstanding
                         async operations (i.e group commit) finish.
                         Threadpool scheduler takes care of "resume".

  @todo
    set thd->lex->sql_command to SQLCOM_END here.
  @todo
    The following has to be changed to an 8 byte integer

  @retval
    0   ok
  @retval
    1   request of thread shutdown, i. e. if command is
        COM_QUIT/COM_SHUTDOWN
*/
dispatch_command_return dispatch_command(enum enum_server_command command, THD *thd,
		      char* packet, uint packet_length, bool blocking)
{
  NET *net= &thd->net;
  bool error= 0;
  bool do_end_of_statement= true;
  DBUG_ENTER("dispatch_command");
  DBUG_PRINT("info", ("command: %d %s", command,
                      (command_name[command].str != 0 ?
                       command_name[command].str :
                       "<?>")));
  bool drop_more_results= 0;

  if (thd->async_state.m_state == thd_async_state::enum_async_state::RESUMED)
  {
    thd->async_state.m_state = thd_async_state::enum_async_state::NONE;
    goto resume;
  }

  /* keep it withing 1 byte */
  compile_time_assert(COM_END == 255);

#if defined(ENABLED_PROFILING)
  thd->profiling.start_new_query();
#endif
  MYSQL_COMMAND_START(thd->thread_id, command,
                      &thd->security_ctx->priv_user[0],
                      (char *) thd->security_ctx->host_or_ip);
  
  DBUG_EXECUTE_IF("crash_dispatch_command_before",
                  { DBUG_PRINT("crash_dispatch_command_before", ("now"));
                    DBUG_SUICIDE(); });

  /* Performance Schema Interface instrumentation, begin */
  thd->m_statement_psi= MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                                               com_statement_info[command].
                                               m_key);
  /*
    We should always call reset_for_next_command() before a query.
    mysql_parse() will do this for queries. Ensure it's also done
    for other commands.
  */
  if (command != COM_QUERY)
    thd->reset_for_next_command();
  thd->set_command(command);

  thd->enable_slow_log= true;
  thd->query_plan_flags= QPLAN_INIT;
  thd->lex->sql_command= SQLCOM_END; /* to avoid confusing VIEW detectors */
  thd->reset_kill_query();

  DEBUG_SYNC(thd,"dispatch_command_before_set_time");

  thd->set_time();
  if (!(server_command_flags[command] & CF_SKIP_QUERY_ID))
    thd->set_query_id(next_query_id());
  else
  {
    /*
      ping, get statistics or similar stateless command.
      No reason to increase query id here.
    */
    thd->set_query_id(get_query_id());
  }
#ifdef WITH_WSREP
  if (WSREP(thd) && thd->wsrep_next_trx_id() == WSREP_UNDEFINED_TRX_ID)
  {
    thd->set_wsrep_next_trx_id(thd->query_id);
    WSREP_DEBUG("assigned new next trx id: %" PRIu64, thd->wsrep_next_trx_id());
  }
#endif /* WITH_WSREP */

  if (!(server_command_flags[command] & CF_SKIP_QUESTIONS))
    statistic_increment(thd->status_var.questions, &LOCK_status);

  /* Copy data for user stats */
  if ((thd->userstat_running= opt_userstat_running))
  {
    thd->start_cpu_time= my_getcputime();
    memcpy(&thd->org_status_var, &thd->status_var, sizeof(thd->status_var));
    thd->select_commands= thd->update_commands= thd->other_commands= 0;
  }

  /**
    Clear the set of flags that are expected to be cleared at the
    beginning of each command.
  */
  thd->server_status&= ~SERVER_STATUS_CLEAR_SET;

  if (unlikely(thd->security_ctx->password_expired &&
               command != COM_QUERY &&
               command != COM_PING &&
               command != COM_QUIT &&
               command != COM_STMT_PREPARE &&
               command != COM_STMT_EXECUTE &&
               command != COM_STMT_CLOSE))
  {
    my_error(ER_MUST_CHANGE_PASSWORD, MYF(0));
    goto dispatch_end;
  }

  switch (command) {
  case COM_INIT_DB:
  {
    LEX_CSTRING tmp;
    status_var_increment(thd->status_var.com_stat[SQLCOM_CHANGE_DB]);
    if (unlikely(thd->copy_with_error(system_charset_info, (LEX_STRING*) &tmp,
                                      thd->charset(), packet, packet_length)))
      break;
    if (!mysql_change_db(thd, &tmp, FALSE))
    {
      general_log_write(thd, command, thd->db.str, thd->db.length);
      my_ok(thd);
    }
    break;
  }
#ifdef HAVE_REPLICATION
  case COM_REGISTER_SLAVE:
  {
    status_var_increment(thd->status_var.com_register_slave);
    if (!thd->register_slave((uchar*) packet, packet_length))
      my_ok(thd);
    break;
  }
#endif
  case COM_RESET_CONNECTION:
  {
    thd->status_var.com_other++;
    thd->change_user();
    thd->clear_error();                         // if errors from rollback
    /* Restore original charset from client authentication packet.*/
    if(thd->org_charset)
      thd->update_charset(thd->org_charset,thd->org_charset,thd->org_charset);
    my_ok(thd, 0, 0, 0);
    break;
  }
  case COM_CHANGE_USER:
  {
    int auth_rc;
    status_var_increment(thd->status_var.com_other);

    thd->change_user();
    thd->clear_error();                         // if errors from rollback

    /* acl_authenticate() takes the data from net->read_pos */
    net->read_pos= (uchar*)packet;

    LEX_CSTRING save_db= thd->db;
    USER_CONN *save_user_connect= thd->user_connect;
    Security_context save_security_ctx= *thd->security_ctx;
    CHARSET_INFO *save_character_set_client=
      thd->variables.character_set_client;
    CHARSET_INFO *save_collation_connection=
      thd->variables.collation_connection;
    CHARSET_INFO *save_character_set_results=
      thd->variables.character_set_results;

    /* Ensure we don't free security_ctx->user in case we have to revert */
    thd->security_ctx->user= 0;
    thd->user_connect= 0;

    /*
      to limit COM_CHANGE_USER ability to brute-force passwords,
      we only allow three unsuccessful COM_CHANGE_USER per connection.
    */
    if (thd->failed_com_change_user >= 3)
    {
      my_message(ER_UNKNOWN_COM_ERROR, ER_THD(thd,ER_UNKNOWN_COM_ERROR),
                 MYF(0));
      auth_rc= 1;
    }
    else
      auth_rc= acl_authenticate(thd, packet_length);

    mysql_audit_notify_connection_change_user(thd, &save_security_ctx);
    if (auth_rc)
    {
      /* Free user if allocated by acl_authenticate */
      my_free(const_cast<char*>(thd->security_ctx->user));
      *thd->security_ctx= save_security_ctx;
      if (thd->user_connect)
	decrease_user_connections(thd->user_connect);
      thd->user_connect= save_user_connect;
      thd->reset_db(&save_db);
      thd->update_charset(save_character_set_client, save_collation_connection,
                          save_character_set_results);
      thd->failed_com_change_user++;
      my_sleep(1000000);
    }
    else
    {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
      /* we've authenticated new user */
      if (save_user_connect)
	decrease_user_connections(save_user_connect);
#endif /* NO_EMBEDDED_ACCESS_CHECKS */
      my_free((char*) save_db.str);
      my_free(const_cast<char*>(save_security_ctx.user));
    }
    break;
  }
  case COM_STMT_BULK_EXECUTE:
  {
    mysqld_stmt_bulk_execute(thd, packet, packet_length);
#ifdef WITH_WSREP
    if (WSREP(thd))
    {
        (void)wsrep_after_statement(thd);
    }
#endif /* WITH_WSREP */
    break;
  }
  case COM_STMT_EXECUTE:
  {
    mysqld_stmt_execute(thd, packet, packet_length);
#ifdef WITH_WSREP
    if (WSREP(thd))
    {
        (void)wsrep_after_statement(thd);
    }
#endif /* WITH_WSREP */
    break;
  }
  case COM_STMT_FETCH:
  {
    mysqld_stmt_fetch(thd, packet, packet_length);
    break;
  }
  case COM_STMT_SEND_LONG_DATA:
  {
    mysql_stmt_get_longdata(thd, packet, packet_length);
    break;
  }
  case COM_STMT_PREPARE:
  {
    mysqld_stmt_prepare(thd, packet, packet_length);
    break;
  }
  case COM_STMT_CLOSE:
  {
    mysqld_stmt_close(thd, packet);
    break;
  }
  case COM_STMT_RESET:
  {
    mysqld_stmt_reset(thd, packet);
    break;
  }
  case COM_QUERY:
  {
    DBUG_ASSERT(thd->m_digest == NULL);
    thd->m_digest= & thd->m_digest_state;
    thd->m_digest->reset(thd->m_token_array, max_digest_length);

    if (unlikely(alloc_query(thd, packet, packet_length)))
      break;					// fatal error is set
    MYSQL_QUERY_START(thd->query(), thd->thread_id,
                      thd->get_db(),
                      &thd->security_ctx->priv_user[0],
                      (char *) thd->security_ctx->host_or_ip);
    char *packet_end= thd->query() + thd->query_length();
    general_log_write(thd, command, thd->query(), thd->query_length());
    DBUG_PRINT("query",("%-.4096s",thd->query()));
#if defined(ENABLED_PROFILING)
    thd->profiling.set_query_source(thd->query(), thd->query_length());
#endif
    MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi, thd->query(),
                             thd->query_length());

    Parser_state parser_state;
    if (unlikely(parser_state.init(thd, thd->query(), thd->query_length())))
      break;

#ifdef WITH_WSREP
    if (WSREP(thd))
    {
      if (wsrep_mysql_parse(thd, thd->query(), thd->query_length(),
                            &parser_state))
      {
        WSREP_DEBUG("Deadlock error for: %s", thd->query());
        mysql_mutex_lock(&thd->LOCK_thd_data);
        thd->reset_kill_query();
        thd->wsrep_retry_counter  = 0;
        mysql_mutex_unlock(&thd->LOCK_thd_data);
        goto dispatch_end;
      }
    }
    else
#endif /* WITH_WSREP */
      mysql_parse(thd, thd->query(), thd->query_length(), &parser_state);

    while (!thd->killed && (parser_state.m_lip.found_semicolon != NULL) &&
           ! thd->is_error())
    {
      /*
        Multiple queries exist, execute them individually
      */
      char *beginning_of_next_stmt= (char*) parser_state.m_lip.found_semicolon;

      /* Finalize server status flags after executing a statement. */
      thd->update_server_status();
      thd->protocol->end_statement();
      query_cache_end_of_result(thd);

      mysql_audit_general(thd, MYSQL_AUDIT_GENERAL_STATUS,
                          thd->get_stmt_da()->is_error()
                            ? thd->get_stmt_da()->sql_errno()
                            : 0,
                          command_name[command].str);

      ulong length= (ulong)(packet_end - beginning_of_next_stmt);

      log_slow_statement(thd);
      DBUG_ASSERT(!thd->apc_target.is_enabled());

      /* Remove garbage at start of query */
      while (length > 0 && my_isspace(thd->charset(), *beginning_of_next_stmt))
      {
        beginning_of_next_stmt++;
        length--;
      }

      /* PSI end */
      MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
      thd->m_statement_psi= NULL;
      thd->m_digest= NULL;

      /* DTRACE end */
      if (MYSQL_QUERY_DONE_ENABLED())
      {
        MYSQL_QUERY_DONE(thd->is_error());
      }

      thd->lex->restore_set_statement_var();

#if defined(ENABLED_PROFILING)
      thd->profiling.finish_current_query();
      thd->profiling.start_new_query("continuing");
      thd->profiling.set_query_source(beginning_of_next_stmt, length);
#endif

      /* DTRACE begin */
      MYSQL_QUERY_START(beginning_of_next_stmt, thd->thread_id,
                        thd->get_db(),
                        &thd->security_ctx->priv_user[0],
                        (char *) thd->security_ctx->host_or_ip);

      /* PSI begin */
      thd->m_digest= & thd->m_digest_state;

      thd->m_statement_psi= MYSQL_START_STATEMENT(&thd->m_statement_state,
                                                  com_statement_info[command].m_key,
                                                  thd->db.str, thd->db.length,
                                                  thd->charset(), NULL);
      THD_STAGE_INFO(thd, stage_starting);
      MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi, beginning_of_next_stmt,
                               length);

      thd->set_query_and_id(beginning_of_next_stmt, length,
                            thd->charset(), next_query_id());

      /*
        Count each statement from the client.
      */
      statistic_increment(thd->status_var.questions, &LOCK_status);

      if (!WSREP(thd))
        thd->set_time(); /* Reset the query start time. */

      parser_state.reset(beginning_of_next_stmt, length);

#ifdef WITH_WSREP
      if (WSREP(thd))
      {
        if (wsrep_mysql_parse(thd, beginning_of_next_stmt,
                              length, &parser_state))
        {
          WSREP_DEBUG("Deadlock error for: %s", thd->query());
          mysql_mutex_lock(&thd->LOCK_thd_data);
          thd->reset_kill_query();
          thd->wsrep_retry_counter  = 0;
          mysql_mutex_unlock(&thd->LOCK_thd_data);

          goto dispatch_end;
        }
      }
      else
#endif /* WITH_WSREP */
      mysql_parse(thd, beginning_of_next_stmt, length, &parser_state);

    }

    DBUG_PRINT("info",("query ready"));
    break;
  }
  case COM_FIELD_LIST:				// This isn't actually needed
#ifdef DONT_ALLOW_SHOW_COMMANDS
    my_message(ER_NOT_ALLOWED_COMMAND, ER_THD(thd, ER_NOT_ALLOWED_COMMAND),
               MYF(0));	/* purecov: inspected */
    break;
#else
  {
    char *fields, *packet_end= packet + packet_length, *arg_end;
    /* Locked closure of all tables */
    TABLE_LIST table_list;
    LEX_STRING table_name;
    LEX_CSTRING db;
    /*
      SHOW statements should not add the used tables to the list of tables
      used in a transaction.
    */
    MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();

    status_var_increment(thd->status_var.com_stat[SQLCOM_SHOW_FIELDS]);
    if (thd->copy_db_to(&db))
      break;
    /*
      We have name + wildcard in packet, separated by endzero
      (The packet is guaranteed to end with an end zero)
    */
    arg_end= strend(packet);
    uint arg_length= (uint)(arg_end - packet);

    /* Check given table name length. */
    if (packet_length - arg_length > NAME_LEN + 1 || arg_length > SAFE_NAME_LEN)
    {
      my_message(ER_UNKNOWN_COM_ERROR, ER_THD(thd, ER_UNKNOWN_COM_ERROR),
                 MYF(0));
      break;
    }
    thd->convert_string(&table_name, system_charset_info,
			packet, arg_length, thd->charset());
    if (Lex_ident_table::check_name(table_name, false))
    {
      /* this is OK due to convert_string() null-terminating the string */
      my_error(ER_WRONG_TABLE_NAME, MYF(0), table_name.str);
      break;
    }
    packet= arg_end + 1;

    lex_start(thd);
    /* Must be before we init the table list. */
    if (lower_case_table_names)
    {
      table_name= thd->make_ident_casedn(table_name);
      db= thd->make_ident_casedn(db);
    }
    table_list.init_one_table(&db, (LEX_CSTRING*) &table_name, 0, TL_READ);
    /*
      Init TABLE_LIST members necessary when the undelrying
      table is view.
    */
    table_list.select_lex= thd->lex->first_select_lex();
    thd->lex->
      first_select_lex()->table_list.insert(&table_list,
                                                  &table_list.next_local);
    thd->lex->add_to_query_tables(&table_list);

    if (is_infoschema_db(&table_list.db))
    {
      ST_SCHEMA_TABLE *schema_table= find_schema_table(thd, &table_list.alias);
      if (schema_table)
        table_list.schema_table= schema_table;
    }

    uint query_length= (uint) (packet_end - packet); // Don't count end \0
    if (!(fields= (char *) thd->memdup(packet, query_length + 1)))
      break;
    thd->set_query(fields, query_length);
    general_log_print(thd, command, "%s %s", table_list.table_name.str,
                      fields);

    if (thd->open_temporary_tables(&table_list))
      break;

    if (check_table_access(thd, SELECT_ACL, &table_list,
                           TRUE, UINT_MAX, FALSE))
      break;
    /*
      Turn on an optimization relevant if the underlying table
      is a view: do not fill derived tables.
    */
    thd->lex->sql_command= SQLCOM_SHOW_FIELDS;

    mysqld_list_fields(thd,&table_list,fields);
    thd->lex->unit.cleanup();
    /* No need to rollback statement transaction, it's not started. */
    DBUG_ASSERT(thd->transaction->stmt.is_empty());
    close_thread_tables(thd);
    thd->mdl_context.rollback_to_savepoint(mdl_savepoint);

    if (thd->transaction_rollback_request)
    {
      /*
        Transaction rollback was requested since MDL deadlock was
        discovered while trying to open tables. Rollback transaction
        in all storage engines including binary log and release all
        locks.
      */
      trans_rollback_implicit(thd);
      thd->release_transactional_locks();
    }

    thd->cleanup_after_query();
    break;
  }
#endif
  case COM_QUIT:
    /* Note: We don't calculate statistics for this command */

    /* Ensure that quit works even if max_mem_used is set */
    thd->variables.max_mem_used= LONGLONG_MAX;
    general_log_print(thd, command, NullS);
    net->error=0;				// Don't give 'abort' message
    thd->get_stmt_da()->disable_status();       // Don't send anything back
    error=TRUE;					// End server
    break;
#ifndef EMBEDDED_LIBRARY
  case COM_BINLOG_DUMP:
    {
      ulong pos;
      ushort flags;

      status_var_increment(thd->status_var.com_other);

      thd->query_plan_flags|= QPLAN_ADMIN;
      if (check_global_access(thd, PRIV_COM_BINLOG_DUMP))
	break;

      /* TODO: The following has to be changed to an 8 byte integer */
      pos = uint4korr(packet);
      flags = uint2korr(packet + 4);
      if ((thd->variables.server_id= uint4korr(packet+6)))
      {
        bool got_error;

        got_error= kill_zombie_dump_threads(thd,
                                            thd->variables.server_id);
        if (got_error || thd->killed)
        {
          if (!thd->killed)
            my_printf_error(ER_MASTER_FATAL_ERROR_READING_BINLOG,
                            "Could not start dump thread for slave: %u as "
                            "it has already a running dump thread",
                            MYF(0), (uint) thd->variables.server_id);
          else if (! thd->get_stmt_da()->is_set())
            thd->send_kill_message();
          error= TRUE;
          thd->unregister_slave(); // todo: can be extraneous
          break;
        }
      }

      const char *name= packet + 10;
      size_t nlen= strlen(name);

      general_log_print(thd, command, "Log: '%s'  Pos: %lu", name, pos);
      if (nlen < FN_REFLEN)
        mysql_binlog_send(thd, thd->strmake(name, nlen), (my_off_t)pos, flags);
      if (thd->killed && ! thd->get_stmt_da()->is_set())
        thd->send_kill_message();
      thd->unregister_slave(); // todo: can be extraneous
      /*  fake COM_QUIT -- if we get here, the thread needs to terminate */
      error = TRUE;
      break;
    }
#endif
  case COM_REFRESH:
  {
    int not_used;

    /*
      Initialize thd->lex since it's used in many base functions, such as
      open_tables(). Otherwise, it remains uninitialized and may cause crash
      during execution of COM_REFRESH.
    */
    lex_start(thd);
    
    status_var_increment(thd->status_var.com_stat[SQLCOM_FLUSH]);
    ulonglong options= (ulonglong) (uchar) packet[0];
    if (trans_commit_implicit(thd))
      break;
    thd->release_transactional_locks();
    if (options & REFRESH_STATUS &&
        !(thd->variables.old_behavior & OLD_MODE_OLD_FLUSH_STATUS))
      options= (options & ~REFRESH_STATUS) | REFRESH_SESSION_STATUS;
    if ((options & ~REFRESH_SESSION_STATUS) &&
        check_global_access(thd,RELOAD_ACL))
      break;
    general_log_print(thd, command, NullS);
#ifndef DBUG_OFF
    bool debug_simulate= FALSE;
    DBUG_EXECUTE_IF("simulate_detached_thread_refresh", debug_simulate= TRUE;);
    if (debug_simulate)
    {
      /* This code doesn't work under FTWRL */
      DBUG_ASSERT(! (options & REFRESH_READ_LOCK));
      /*
        Simulate a reload without a attached thread session.
        Provides a environment similar to that of when the
        server receives a SIGHUP signal and reloads caches
        and flushes tables.
      */
      bool res;
      set_current_thd(0);
      res= reload_acl_and_cache(NULL, options | REFRESH_FAST,
                                NULL, &not_used);
      set_current_thd(thd);
      if (res)
        break;
    }
    else
#endif
    {
      thd->lex->relay_log_connection_name= empty_clex_str;
      if (reload_acl_and_cache(thd, options, (TABLE_LIST*) 0, &not_used))
        break;
    }
    if (trans_commit_implicit(thd))
      break;
    close_thread_tables(thd);
    thd->release_transactional_locks();
    my_ok(thd);
    break;
  }
#ifndef EMBEDDED_LIBRARY
  case COM_SHUTDOWN:
  {
    status_var_increment(thd->status_var.com_other);
    if (check_global_access(thd,SHUTDOWN_ACL))
      break; /* purecov: inspected */
    /*
      If the client is < 4.1.3, it is going to send us no argument; then
      packet_length is 0, packet[0] is the end 0 of the packet. Note that
      SHUTDOWN_DEFAULT is 0. If client is >= 4.1.3, the shutdown level is in
      packet[0].
    */
    enum mysql_enum_shutdown_level level;
    level= (enum mysql_enum_shutdown_level) (uchar) packet[0];
    thd->lex->is_shutdown_wait_for_slaves= false;  // "deferred" cleanup
    if (level == SHUTDOWN_DEFAULT)
      level= SHUTDOWN_WAIT_ALL_BUFFERS; // soon default will be configurable
    else if (level != SHUTDOWN_WAIT_ALL_BUFFERS)
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0), "this shutdown level");
      break;
    }
    DBUG_PRINT("quit",("Got shutdown command for level %u", level));
    general_log_print(thd, command, NullS);
    my_eof(thd);
    kill_mysql(thd);
    error=TRUE;
    DBUG_EXECUTE_IF("simulate_slow_client_at_shutdown", my_sleep(2000000););
    break;
  }
#endif
  case COM_STATISTICS:
  {
    STATUS_VAR *current_global_status_var;      // Big; Don't allocate on stack
    ulong uptime;
    ulonglong queries_per_second1000;
    char buff[250];
    uint buff_len= sizeof(buff);

    if (!(current_global_status_var= thd->alloc<STATUS_VAR>(1)))
      break;
    general_log_print(thd, command, NullS);
    status_var_increment(thd->status_var.com_stat[SQLCOM_SHOW_STATUS]);
    *current_global_status_var= global_status_var;
    calc_sum_of_all_status(current_global_status_var);
    if (!(uptime= (ulong) (thd->start_time - server_start_time)))
      queries_per_second1000= 0;
    else
      queries_per_second1000= thd->query_id * 1000 / uptime;
#ifndef EMBEDDED_LIBRARY
    size_t length=
#endif
    my_snprintf(buff, buff_len - 1,
                        "Uptime: %lu  Threads: %u  Questions: %lu  "
                        "Slow queries: %lu  Opens: %lu  "
                        "Open tables: %u  Queries per second avg: %u.%03u",
                        uptime, THD_count::value(), (ulong) thd->query_id,
                        current_global_status_var->long_query_count,
                        current_global_status_var->opened_tables,
                        tc_records(),
                        (uint) (queries_per_second1000 / 1000),
                        (uint) (queries_per_second1000 % 1000));
#ifdef EMBEDDED_LIBRARY
    /* Store the buffer in permanent memory */
    my_ok(thd, 0, 0, buff);
#else
    (void) my_net_write(net, (uchar*) buff, length);
    (void) net_flush(net);
    thd->get_stmt_da()->disable_status();
#endif
    break;
  }
  case COM_PING:
    status_var_increment(thd->status_var.com_other);
    my_ok(thd);				// Tell client we are alive
    break;
  case COM_PROCESS_INFO:
    status_var_increment(thd->status_var.com_stat[SQLCOM_SHOW_PROCESSLIST]);
    if (!thd->security_ctx->priv_user[0] &&
        check_global_access(thd, PRIV_COM_PROCESS_INFO))
      break;
    general_log_print(thd, command, NullS);
    mysqld_list_processes(thd,
                     thd->security_ctx->master_access & PRIV_COM_PROCESS_INFO ?
                     NullS : thd->security_ctx->priv_user, 0);
    break;
  case COM_PROCESS_KILL:
  {
    status_var_increment(thd->status_var.com_stat[SQLCOM_KILL]);
    ulong id=(ulong) uint4korr(packet);
    sql_kill(thd, id, KILL_CONNECTION_HARD, KILL_TYPE_ID);
    break;
  }
  case COM_SET_OPTION:
  {
    status_var_increment(thd->status_var.com_stat[SQLCOM_SET_OPTION]);
    uint opt_command= uint2korr(packet);

    switch (opt_command) {
    case (int) MYSQL_OPTION_MULTI_STATEMENTS_ON:
      thd->client_capabilities|= CLIENT_MULTI_STATEMENTS;
      my_eof(thd);
      break;
    case (int) MYSQL_OPTION_MULTI_STATEMENTS_OFF:
      thd->client_capabilities&= ~CLIENT_MULTI_STATEMENTS;
      my_eof(thd);
      break;
    default:
      my_message(ER_UNKNOWN_COM_ERROR, ER_THD(thd, ER_UNKNOWN_COM_ERROR),
                 MYF(0));
      break;
    }
    break;
  }
  case COM_DEBUG:
    status_var_increment(thd->status_var.com_other);
    if (check_global_access(thd, PRIV_DEBUG))
      break;					/* purecov: inspected */
    mysql_print_status();
    general_log_print(thd, command, NullS);
    my_eof(thd);
    break;

  case COM_SLEEP:
  case COM_CONNECT:				// Impossible here
  case COM_TIME:				// Impossible from client
  case COM_DELAYED_INSERT:
  case COM_END:
  case COM_UNIMPLEMENTED:
  default:
    my_message(ER_UNKNOWN_COM_ERROR, ER_THD(thd, ER_UNKNOWN_COM_ERROR),
               MYF(0));
    break;
  }

dispatch_end:
  /*
   For the threadpool i.e if non-blocking call, if not all async operations
   are finished, return without cleanup. The cleanup will be done on
   later, when command execution is resumed.
  */
  if (!blocking && !error && thd->async_state.pending_ops())
  {
    DBUG_RETURN(DISPATCH_COMMAND_WOULDBLOCK);
  }

resume:

#ifdef WITH_WSREP
  /*
    Next test should really be WSREP(thd), but that causes a failure when doing
    'set WSREP_ON=0'
  */
  if (unlikely(wsrep_service_started))
  {
    if (thd->killed == KILL_QUERY)
    {
      WSREP_DEBUG("THD is killed at dispatch_end");
    }
    if (thd->lex->sql_command != SQLCOM_SET_OPTION)
    {
      DEBUG_SYNC(thd, "wsrep_at_dispatch_end_before_result");
    }
    if (thd->wsrep_cs().state() == wsrep::client_state::s_exec)
    {
      wsrep_after_command_before_result(thd);
      if (wsrep_current_error(thd) && !wsrep_command_no_result(command))
      {
        /* todo: Pass wsrep client state current error to override */
        wsrep_override_error(thd, wsrep_current_error(thd),
                             wsrep_current_error_status(thd));
        WSREP_LOG_THD(thd, "leave");
      }
    }
    else
    {
      /* wsrep_after_command_before_result() already called elsewhere
         or not necessary to call it */
      assert(thd->wsrep_cs().state() == wsrep::client_state::s_none ||
             thd->wsrep_cs().state() == wsrep::client_state::s_result);
    }
    if (WSREP(thd))
    {
      /*
        MDEV-10812
        In the case of COM_QUIT/COM_STMT_CLOSE thread status should be disabled.
      */
      DBUG_ASSERT((command != COM_QUIT && command != COM_STMT_CLOSE)
                  || thd->get_stmt_da()->is_disabled());
      DBUG_ASSERT(thd->wsrep_trx().state() != wsrep::transaction::s_replaying);
      /* wsrep BF abort in query exec phase */
      mysql_mutex_lock(&thd->LOCK_thd_kill);
      do_end_of_statement= thd_is_connection_alive(thd);
      mysql_mutex_unlock(&thd->LOCK_thd_kill);
    }
  }
#endif /* WITH_WSREP */

  if (thd->reset_sp_cache)
  {
    thd->sp_caches_empty();
    thd->reset_sp_cache= false;
  }

  if (do_end_of_statement)
  {
    DBUG_ASSERT(thd->derived_tables == NULL &&
               (thd->open_tables == NULL ||
               (thd->locked_tables_mode == LTM_LOCK_TABLES)));

    thd_proc_info(thd, "Updating status");
    /* Finalize server status flags after executing a command. */
    thd->update_server_status();
    thd->protocol->end_statement();
    query_cache_end_of_result(thd);
  }
  if (drop_more_results)
    thd->server_status&= ~SERVER_MORE_RESULTS_EXISTS;

  if (likely(!thd->is_error() && !thd->killed_errno()))
    mysql_audit_general(thd, MYSQL_AUDIT_GENERAL_RESULT, 0, 0);

  mysql_audit_general(thd, MYSQL_AUDIT_GENERAL_STATUS,
                      thd->get_stmt_da()->is_error() ?
                      thd->get_stmt_da()->sql_errno() : 0,
                      command_name[command].str);

  thd->update_all_stats();

  /*
    Write to slow query log only those statements that received via the text
    protocol except the EXECUTE statement. The reason we do that way is
    that for statements received via binary protocol and for the EXECUTE
    statement, the slow statements have been already written to slow query log
    inside the method Prepared_statement::execute().
  */
  if(command == COM_QUERY &&
     thd->lex->sql_command != SQLCOM_EXECUTE)
    log_slow_statement(thd);
  else
    delete_explain_query(thd->lex);

  THD_STAGE_INFO(thd, stage_cleaning_up);
  thd->reset_query();

  /* Performance Schema Interface instrumentation, end */
  MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
  /* Reset values shown in processlist */
  thd->examined_row_count_for_statement= thd->sent_row_count_for_statement= 0;
  thd->mark_connection_idle();

  thd->m_statement_psi= NULL;
  thd->m_digest= NULL;

  thd->packet.shrink(thd->variables.net_buffer_length); // Reclaim some memory

  thd->reset_kill_query();  /* Ensure that killed_errmsg is released */
  /*
    LEX::m_sql_cmd can point to Sql_cmd allocated on thd->mem_root.
    Unlink it now, before freeing the root.
  */
  thd->lex->m_sql_cmd= NULL;
  free_root(thd->mem_root,MYF(MY_KEEP_PREALLOC));
  DBUG_EXECUTE_IF("print_allocated_thread_memory",
                  SAFEMALLOC_REPORT_MEMORY(sf_malloc_dbug_id()););

#if defined(ENABLED_PROFILING)
  thd->profiling.finish_current_query();
#endif
  if (MYSQL_QUERY_DONE_ENABLED() || MYSQL_COMMAND_DONE_ENABLED())
  {
    int res __attribute__((unused));
    res= (int) thd->is_error();
    if (command == COM_QUERY)
    {
      MYSQL_QUERY_DONE(res);
    }
    MYSQL_COMMAND_DONE(res);
  }
  DEBUG_SYNC(thd,"dispatch_command_end");
  DEBUG_SYNC(thd,"dispatch_command_end2");

  /* Check that some variables are reset properly */
  DBUG_ASSERT(thd->abort_on_warning == 0);
  thd->lex->restore_set_statement_var();
  DBUG_RETURN(error?DISPATCH_COMMAND_CLOSE_CONNECTION: DISPATCH_COMMAND_SUCCESS);
}

static bool slow_filter_masked(THD *thd, ulonglong mask)
{
  return thd->variables.log_slow_filter && !(thd->variables.log_slow_filter & mask);
}

/*
  Log query to slow queries, if it passes filtering

  @note
    This function must call delete_explain_query().
*/

void log_slow_statement(THD *thd)
{
  DBUG_ENTER("log_slow_statement");

  /*
    The following should never be true with our current code base,
    but better to keep this here so we don't accidently try to log a
    statement in a trigger or stored function
  */
  if (unlikely(thd->in_sub_stmt))
    goto end;                           // Don't set time for sub stmt
  /*
    Skip both long_query_count increment and logging if the current
    statement forces slow log suppression (e.g. an SP statement).

    Note, we don't check for global_system_variables.sql_log_slow here.
    According to the manual, the "Slow_queries" status variable does not require
    sql_log_slow to be ON. So even if sql_log_slow is OFF, we still need to
    continue and increment long_query_count (and skip only logging, see below):
  */
  if (!thd->enable_slow_log)
    goto end; // E.g. SP statement

  DBUG_EXECUTE_IF("simulate_slow_query", {
                  if (thd->get_command() == COM_QUERY ||
                      thd->get_command() == COM_STMT_EXECUTE)
                    thd->server_status|= SERVER_QUERY_WAS_SLOW;
                  });

  if ((thd->server_status &
       (SERVER_QUERY_NO_INDEX_USED | SERVER_QUERY_NO_GOOD_INDEX_USED)) &&
      !(thd->query_plan_flags & QPLAN_STATUS) &&
      (thd->variables.log_slow_filter & QPLAN_NOT_USING_INDEX))
  {
    thd->query_plan_flags|= QPLAN_NOT_USING_INDEX;
    /* We are always logging no index queries if enabled in filter */
    thd->server_status|= SERVER_QUERY_WAS_SLOW;
  }

  if ((thd->server_status & SERVER_QUERY_WAS_SLOW) &&
      (thd->get_examined_row_count() >= thd->variables.min_examined_row_limit ||
       thd->log_slow_always_query_time()))
  {
    thd->status_var.long_query_count++;

    /*
      until log_slow_disabled_statements=admin is removed, it
      duplicates slow_log_filter=admin
    */
    if ((thd->query_plan_flags & QPLAN_ADMIN) &&
        (thd->variables.log_slow_disabled_statements & LOG_SLOW_DISABLE_ADMIN))
      goto end;

    if (!global_system_variables.sql_log_slow || !thd->variables.sql_log_slow)
      goto end;

    /*
      If rate limiting of slow log writes is enabled, decide whether to log
      this query to the log or not.
    */ 
    if (thd->variables.log_slow_rate_limit > 1 &&
        !thd->log_slow_always_query_time() &&
        (global_query_id % thd->variables.log_slow_rate_limit) != 0)
      goto end;

    /*
      Follow the slow log filter configuration:
      skip logging if the current statement matches the filter.
    */
    if (slow_filter_masked(thd, thd->query_plan_flags))
      goto end;

    THD_STAGE_INFO(thd, stage_logging_slow_query);
    slow_log_print(thd, thd->query(), thd->query_length(), 
                   thd->utime_after_query);
  }

end:
  delete_explain_query(thd->lex);
  DBUG_VOID_RETURN;
}


/**
  Create a TABLE_LIST object for an INFORMATION_SCHEMA table.

    This function is used in the parser to convert a SHOW or DESCRIBE
    table_name command to a SELECT from INFORMATION_SCHEMA.
    It prepares a SELECT_LEX and a TABLE_LIST object to represent the
    given command as a SELECT parse tree.

  @param thd              thread handle
  @param lex              current lex
  @param table_ident      table alias if it's used
  @param schema_table_idx the type of the INFORMATION_SCHEMA table to be
                          created

  @note
    Due to the way this function works with memory and LEX it cannot
    be used outside the parser (parse tree transformations outside
    the parser break PS and SP).

  @retval
    0                 success
  @retval
    1                 out of memory or SHOW commands are not allowed
                      in this version of the server.
*/

int prepare_schema_table(THD *thd, LEX *lex, Table_ident *table_ident,
                         enum enum_schema_tables schema_table_idx)
{
  SELECT_LEX *schema_select_lex= NULL;
  DBUG_ENTER("prepare_schema_table");

  switch (schema_table_idx) {
  case SCH_SCHEMATA:
#if defined(DONT_ALLOW_SHOW_COMMANDS)
    my_message(ER_NOT_ALLOWED_COMMAND,
               ER_THD(thd, ER_NOT_ALLOWED_COMMAND), MYF(0));
    DBUG_RETURN(1);
#else
    break;
#endif

  case SCH_TABLE_NAMES:
  case SCH_TABLES:
  case SCH_CHECK_CONSTRAINTS:
  case SCH_VIEWS:
  case SCH_TRIGGERS:
  case SCH_EVENTS:
#ifdef DONT_ALLOW_SHOW_COMMANDS
    my_message(ER_NOT_ALLOWED_COMMAND,
               ER_THD(thd, ER_NOT_ALLOWED_COMMAND), MYF(0));
    DBUG_RETURN(1);
#else
    {
      if (lex->first_select_lex()->db.str == NULL &&
          lex->copy_db_to(&lex->first_select_lex()->db))
      {
        DBUG_RETURN(1);
      }
      schema_select_lex= new (thd->mem_root) SELECT_LEX();
      schema_select_lex->table_list.first= NULL;
      if (lower_case_table_names == 1)
        lex->first_select_lex()->db=
          thd->make_ident_casedn(lex->first_select_lex()->db);
      schema_select_lex->db= lex->first_select_lex()->db;
      if (Lex_ident_db::check_name_with_error(lex->first_select_lex()->db))
        DBUG_RETURN(1);
      break;
    }
#endif
  case SCH_COLUMNS:
  case SCH_STATISTICS:
#ifdef DONT_ALLOW_SHOW_COMMANDS
    my_message(ER_NOT_ALLOWED_COMMAND,
               ER_THD(thd, ER_NOT_ALLOWED_COMMAND), MYF(0));
    DBUG_RETURN(1);
#else
  {
    DBUG_ASSERT(table_ident);
    TABLE_LIST **query_tables_last= lex->query_tables_last;
    schema_select_lex= new (thd->mem_root) SELECT_LEX();
    /* 'parent_lex' is used in init_query() so it must be before it. */
    schema_select_lex->parent_lex= lex;
    schema_select_lex->init_query();
    schema_select_lex->select_number= 0;
    if (!schema_select_lex->add_table_to_list(thd, table_ident, 0, 0, TL_READ,
                                              MDL_SHARED_READ))
      DBUG_RETURN(1);
    lex->query_tables_last= query_tables_last;
    break;
  }
#endif
  case SCH_PROFILES:
    /* 
      Mark this current profiling record to be discarded.  We don't
      wish to have SHOW commands show up in profiling.
    */
#if defined(ENABLED_PROFILING)
    thd->profiling.discard_current_query();
#endif
    break;
  default:
    break;
  }
  if (schema_select_lex)
    schema_select_lex->set_master_unit(&lex->unit);
  SELECT_LEX *select_lex= lex->current_select;
  if (make_schema_select(thd, select_lex, get_schema_table(schema_table_idx)))
    DBUG_RETURN(1);

  select_lex->table_list.first->schema_select_lex= schema_select_lex;
  DBUG_RETURN(0);
}


/**
  Read query from packet and store in thd->query.
  Used in COM_QUERY and COM_STMT_PREPARE.

    Sets the following THD variables:
  - query
  - query_length

  @retval
    FALSE ok
  @retval
    TRUE  error;  In this case thd->fatal_error is set
*/

bool alloc_query(THD *thd, const char *packet, size_t packet_length)
{
  char *query;
  /* Remove garbage at start and end of query */
  while (packet_length > 0 && my_isspace(thd->charset(), packet[0]))
  {
    packet++;
    packet_length--;
  }
  const char *pos= packet + packet_length;     // Point at end null
  while (packet_length > 0 &&
	 (pos[-1] == ';' || my_isspace(thd->charset() ,pos[-1])))
  {
    pos--;
    packet_length--;
  }
  /* We must allocate some extra memory for query cache 

    The query buffer layout is:
       buffer :==
            <statement>   The input statement(s)
            '\0'          Terminating null char  (1 byte)
            <length>      Length of following current database name (size_t)
            <db_name>     Name of current database
            <flags>       Flags struct
  */
  if (! (query= (char*) thd->memdup_w_gap(packet,
                                          packet_length,
                                          1 + thd->db.length +
                                          QUERY_CACHE_DB_LENGTH_SIZE +
                                          QUERY_CACHE_FLAGS_SIZE)))
      return TRUE;
  query[packet_length]= '\0';
  /*
    Space to hold the name of the current database is allocated.  We
    also store this length, in case current database is changed during
    execution.  We might need to reallocate the 'query' buffer
  */
  int2store(query + packet_length + 1, thd->db.length);
    
  thd->set_query(query, packet_length);

  /* Reclaim some memory */
  thd->packet.shrink(thd->variables.net_buffer_length);
  thd->convert_buffer.shrink(thd->variables.net_buffer_length);

  return FALSE;
}


bool sp_process_definer(THD *thd)
{
  DBUG_ENTER("sp_process_definer");

  LEX *lex= thd->lex;

  /*
    If the definer is not specified, this means that CREATE-statement missed
    DEFINER-clause. DEFINER-clause can be missed in two cases:

      - The user submitted a statement w/o the clause. This is a normal
        case, we should assign CURRENT_USER as definer.

      - Our slave received an updated from the master, that does not
        replicate definer for stored routines. We should also assign
        CURRENT_USER as definer here, but also we should mark this routine
        as NON-SUID. This is essential for the sake of backward
        compatibility.

        The problem is the slave thread is running under "special" user (@),
        that actually does not exist. In the older versions we do not fail
        execution of a stored routine if its definer does not exist and
        continue the execution under the authorization of the invoker
        (BUG#13198). And now if we try to switch to slave-current-user (@),
        we will fail.

        Actually, this leads to the inconsistent state of master and
        slave (different definers, different SUID behaviour), but it seems,
        this is the best we can do.
  */

  if (!lex->definer)
  {
    Query_arena original_arena;
    Query_arena *ps_arena= thd->activate_stmt_arena_if_needed(&original_arena);

    lex->definer= create_default_definer(thd, false);

    if (ps_arena)
      thd->restore_active_arena(ps_arena, &original_arena);

    /* Error has been already reported. */
    if (lex->definer == NULL)
      DBUG_RETURN(TRUE);

    if (thd->slave_thread && lex->sphead)
      lex->sphead->set_suid(SP_IS_NOT_SUID);
  }
  else
  {
    LEX_USER *d= get_current_user(thd, lex->definer);
    if (!d)
      DBUG_RETURN(TRUE);
    if (d->user.str == public_name.str)
    {
      my_error(ER_INVALID_ROLE, MYF(0), lex->definer->user.str);
      DBUG_RETURN(TRUE);
    }
    thd->change_item_tree((Item**)&lex->definer, (Item*)d);

    /*
      If the specified definer differs from the current user or role, we
      should check that the current user has SUPER privilege (in order
      to create a stored routine under another user one must have
      SUPER privilege).
    */
    bool curuser= !strcmp(d->user.str, thd->security_ctx->priv_user);
    bool currole= !curuser && !strcmp(d->user.str, thd->security_ctx->priv_role);
    bool curuserhost= curuser && d->host.str &&
                      Lex_ident_host(d->host).
                        streq(Lex_cstring_strlen(thd->security_ctx->priv_host));
    if (!curuserhost && !currole &&
        check_global_access(thd, PRIV_DEFINER_CLAUSE, false))
      DBUG_RETURN(TRUE);
  }

  /* Check that the specified definer exists. Emit a warning if not. */

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (!is_acl_user(lex->definer->host, lex->definer->user))
  {
    push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                        ER_MALFORMED_DEFINER, ER_THD(thd, ER_MALFORMED_DEFINER),
                        lex->definer->user.str, lex->definer->host.str);
  }
#endif /* NO_EMBEDDED_ACCESS_CHECKS */

  DBUG_RETURN(FALSE);
}


/**
  Auxiliary call that opens and locks tables for LOCK TABLES statement
  and initializes the list of locked tables.

  @param thd     Thread context.
  @param tables  List of tables to be locked.

  @return FALSE in case of success, TRUE in case of error.
*/

static bool __attribute__ ((noinline))
lock_tables_open_and_lock_tables(THD *thd, TABLE_LIST *tables)
{
  Lock_tables_prelocking_strategy lock_tables_prelocking_strategy;
  MDL_deadlock_and_lock_abort_error_handler deadlock_handler;
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();
  uint counter;
  TABLE_LIST *table;

  thd->in_lock_tables= 1;

retry:

  if (open_tables(thd, &tables, &counter, 0, &lock_tables_prelocking_strategy))
    goto err;

  for (table= tables; table; table= table->next_global)
  {
    if (!table->placeholder())
    {
      if (table->table->s->tmp_table)
      {
        /*
          We allow to change temporary tables even if they were locked for read
          by LOCK TABLES. To avoid a discrepancy between lock acquired at LOCK
          TABLES time and by the statement which is later executed under LOCK
          TABLES we ensure that for temporary tables we always request a write
          lock (such discrepancy can cause problems for the storage engine).
          We don't set TABLE_LIST::lock_type in this case as this might result
          in extra warnings from THD::decide_logging_format() even though
          binary logging is totally irrelevant for LOCK TABLES.
        */
        table->table->reginfo.lock_type= TL_WRITE;
      }
      else if (table->mdl_request.type == MDL_SHARED_READ &&
               ! table->prelocking_placeholder &&
               table->table->file->lock_count() == 0)
      {
        enum enum_mdl_type lock_type;
        /*
          In case when LOCK TABLE ... READ LOCAL was issued for table with
          storage engine which doesn't support READ LOCAL option and doesn't
          use THR_LOCK locks we need to upgrade weak SR metadata lock acquired
          in open_tables() to stronger SRO metadata lock.
          This is not needed for tables used through stored routines or
          triggers as we always acquire SRO (or even stronger SNRW) metadata
          lock for them.
        */
        deadlock_handler.init();
        thd->push_internal_handler(&deadlock_handler);

        lock_type= table->table->mdl_ticket->get_type() == MDL_SHARED_WRITE ?
                   MDL_SHARED_NO_READ_WRITE : MDL_SHARED_READ_ONLY;

        bool result= thd->mdl_context.upgrade_shared_lock(
                                        table->table->mdl_ticket,
                                        lock_type,
                                        thd->variables.lock_wait_timeout);

        thd->pop_internal_handler();

        if (deadlock_handler.need_reopen())
        {
          /*
            Deadlock occurred during upgrade of metadata lock.
            Let us restart acquiring and opening tables for LOCK TABLES.
          */
          close_tables_for_reopen(thd, &tables, mdl_savepoint, true);
          if (thd->open_temporary_tables(tables))
            goto err;
          goto retry;
        }

        if (result)
          goto err;
      }

#ifdef WITH_WSREP
      if (WSREP(thd) && table->table->s->table_type == TABLE_TYPE_SEQUENCE)
      {
        my_error(ER_NOT_SUPPORTED_YET, MYF(0),
                 "LOCK TABLE on SEQUENCES in Galera cluster");
        goto err;
      }
#endif

    }
    /*
       Check privileges of view tables here, after views were opened.
       Either definer or invoker has to have PRIV_LOCK_TABLES to be able
       to lock view and its tables. For mysqldump (that locks views
       before dumping their structures) compatibility we allow locking
       views that select from I_S or P_S tables, but downgrade the lock
       to TL_READ
     */
    if (table->belong_to_view &&
        check_single_table_access(thd, PRIV_LOCK_TABLES, table, 1))
    {
      if (table->grant.m_internal.m_schema_access)
        table->lock_type= TL_READ;
      else
      {
        bool error= true;
        if (Security_context *sctx= table->security_ctx)
        {
          table->security_ctx= 0;
          error= check_single_table_access(thd, PRIV_LOCK_TABLES, table, 1);
          table->security_ctx= sctx;
        }
        if (error)
        {
          my_error(ER_VIEW_INVALID, MYF(0), table->belong_to_view->view_db.str,
                   table->belong_to_view->view_name.str);
          goto err;
        }
      }
    }
  }

  if (lock_tables(thd, tables, counter, 0) ||
      thd->locked_tables_list.init_locked_tables(thd))
    goto err;

  thd->in_lock_tables= 0;

  return FALSE;

err:
  thd->in_lock_tables= 0;

  trans_rollback_stmt(thd);
  /*
    Need to end the current transaction, so the storage engine (InnoDB)
    can free its locks if LOCK TABLES locked some tables before finding
    that it can't lock a table in its list
  */
  trans_rollback(thd);
  /* Close tables and release metadata locks. */
  close_thread_tables(thd);
  DBUG_ASSERT(!thd->locked_tables_mode);
  thd->release_transactional_locks();
  return TRUE;
}


static bool do_execute_sp(THD *thd, sp_head *sp)
{
  /* bits that should be cleared in thd->server_status */
  uint bits_to_be_cleared= 0;
  ulonglong affected_rows;
  if (sp->m_flags & sp_head::MULTI_RESULTS)
  {
    if (!(thd->client_capabilities & CLIENT_MULTI_RESULTS))
    {
      /* The client does not support multiple result sets being sent back */
      my_error(ER_SP_BADSELECT, MYF(0), ErrConvDQName(sp).ptr());
      return 1;
    }
  }
  /*
    If SERVER_MORE_RESULTS_EXISTS is not set,
    then remember that it should be cleared
  */
  bits_to_be_cleared= (~thd->server_status &
                       SERVER_MORE_RESULTS_EXISTS);
  thd->server_status|= SERVER_MORE_RESULTS_EXISTS;
  ha_rows select_limit= thd->variables.select_limit;
  thd->variables.select_limit= HA_POS_ERROR;

  /*
    Reset current_select as it may point to random data as a
    result of previous parsing.
  */
  thd->lex->current_select= NULL;
  thd->lex->in_sum_func= 0;                     // For Item_field::fix_fields()

  /*
    We never write CALL statements into binlog:
     - If the mode is non-prelocked, each statement will be logged
       separately.
     - If the mode is prelocked, the invoking statement will care
       about writing into binlog.
    So just execute the statement.
  */
  int res= sp->execute_procedure(thd, &thd->lex->value_list);

  thd->variables.select_limit= select_limit;
  thd->server_status&= ~bits_to_be_cleared;

  if (res)
  {
    DBUG_ASSERT(thd->is_error() || thd->killed);
    return 1;  		// Substatement should already have sent error
  }

  affected_rows= thd->affected_rows; // Affected rows for all sub statements
  thd->affected_rows= 0;             // Reset total, as my_ok() adds to it
  my_ok(thd, affected_rows);
  return 0;
}


static int __attribute__ ((noinline))
mysql_create_routine(THD *thd, LEX *lex)
{
  DBUG_ASSERT(lex->sphead != 0);
  DBUG_ASSERT(lex->sphead->m_db.str); /* Must be initialized in the parser */
  DBUG_ASSERT(lower_case_table_names != 1 ||
              Lex_ident_fs(lex->sphead->m_db).is_in_lower_case());

  if (Lex_ident_db::check_name_with_error(lex->sphead->m_db))
    return true;

  if (check_access(thd, CREATE_PROC_ACL, lex->sphead->m_db.str,
                   NULL, NULL, 0, 0))
    return true;

  /* Checking the drop permissions if CREATE OR REPLACE is used */
  if (lex->create_info.or_replace())
  {
    if (check_routine_access(thd, ALTER_PROC_ACL, &lex->sphead->m_db,
                             &lex->sphead->m_name,
                             Sp_handler::handler(lex->sql_command), 0))
      return true;
  }

  const Lex_ident_routine name= Lex_ident_routine(*lex->sphead->name());
#ifdef HAVE_DLOPEN
  if (lex->sphead->m_handler->type() == SP_TYPE_FUNCTION)
  {
    udf_func *udf= find_udf(name.str, name.length);

    if (udf)
    {
      my_error(ER_UDF_EXISTS, MYF(0), name.str);
      return true;
    }
  }
#endif

  if (sp_process_definer(thd))
    return true;

  WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

  if (!lex->sphead->m_handler->sp_create_routine(thd, lex->sphead))
  {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
    /* only add privileges if really neccessary */

    Security_context security_context;
    bool restore_backup_context= false;
    Security_context *backup= NULL;
    LEX_USER *definer= thd->lex->definer;
    /*
      We're going to issue an implicit GRANT statement so we close all
      open tables. We have to keep metadata locks as this ensures that
      this statement is atomic against concurent FLUSH TABLES WITH READ
      LOCK. Deadlocks which can arise due to fact that this implicit
      statement takes metadata locks should be detected by a deadlock
      detector in MDL subsystem and reported as errors.

      TODO: Long-term we should either ensure that implicit GRANT statement
            is written into binary log as a separate statement or make both
            creation of routine and implicit GRANT parts of one fully atomic
            statement.
      */
    if (trans_commit_stmt(thd))
      goto wsrep_error_label;
    close_thread_tables(thd);
    /*
      Check if the definer exists on slave,
      then use definer privilege to insert routine privileges to mysql.procs_priv.

      For current user of SQL thread has GLOBAL_ACL privilege,
      which doesn't any check routine privileges,
      so no routine privilege record  will insert into mysql.procs_priv.
    */
    if (thd->slave_thread && is_acl_user(definer->host, definer->user))
    {
      security_context.change_security_context(thd, &thd->lex->definer->user,
                                               &thd->lex->definer->host,
                                               &thd->lex->sphead->m_db,
                                               &backup);
      restore_backup_context= true;
    }

    if (sp_automatic_privileges && !opt_noacl &&
        check_routine_access(thd, DEFAULT_CREATE_PROC_ACLS,
                             &lex->sphead->m_db, &name,
                             Sp_handler::handler(lex->sql_command), 1))
    {
      if (sp_grant_privileges(thd, lex->sphead->m_db, name,
                              Sp_handler::handler(lex->sql_command)))
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                     ER_PROC_AUTO_GRANT_FAIL, ER_THD(thd, ER_PROC_AUTO_GRANT_FAIL));
      thd->clear_error();
    }

    /*
      Restore current user with GLOBAL_ACL privilege of SQL thread
    */
    if (restore_backup_context)
    {
      DBUG_ASSERT(thd->slave_thread == 1);
      thd->security_ctx->restore_security_context(thd, backup);
    }

#endif
    return false;
  }
  (void) trans_commit_stmt(thd);

#if !defined(NO_EMBEDDED_ACCESS_CHECKS) || defined(WITH_WSREP)
wsrep_error_label:
#endif
  return true;
}


/**
  Prepare for CREATE DATABASE, ALTER DATABASE, DROP DATABASE.

  @param thd         - current THD
  @param want_access - access needed
  @param dbname      - the database name

  @retval false      - Ok to proceed with CREATE/ALTER/DROP
  @retval true       - not OK to proceed (error, or filtered)

  Note, on slave this function returns true if the database
  is in the ignore filter. The caller must distinguish this case
  from other cases: bad database error, no access error.
  This can be done by testing thd->is_error().
*/
static bool prepare_db_action(THD *thd, privilege_t want_access,
                              const Lex_ident_db &dbname)
{
  /*
    If in a slave thread :
    - CREATE DATABASE DB was certainly not preceded by USE DB.
    - ALTER DATABASE DB may not be preceded by USE DB.
    - DROP DATABASE DB may not be preceded by USE DB.
    For that reason, db_ok() in sql/slave.cc did not check the
    do_db/ignore_db. And as this query involves no tables, tables_ok()
    was not called. So we have to check rules again here.
  */
  return thd->check_slave_ignored_db_with_error(dbname) ||
         check_access(thd, want_access, dbname.str, NULL, NULL, 1, 0);
}


bool Sql_cmd_call::execute(THD *thd)
{
  TABLE_LIST *all_tables= thd->lex->query_tables;
  sp_head *sp;
  /*
    This will cache all SP and SF and open and lock all tables
    required for execution.
  */
  if (check_table_access(thd, SELECT_ACL, all_tables, FALSE,
                         UINT_MAX, FALSE) ||
      open_and_lock_tables(thd, all_tables, TRUE, 0))
   return true;

  /*
    By this moment all needed SPs should be in cache so no need to look
    into DB.
  */
  if (!(sp= m_handler->sp_find_routine(thd, m_name, true)))
  {
    /*
      If the routine is not found, let's still check EXECUTE_ACL to decide
      whether to return "Access denied" or "Routine does not exist".
    */
    if (check_routine_access(thd, EXECUTE_ACL, &m_name->m_db,
                             &m_name->m_name,
                             &sp_handler_procedure,
                             false))
      return true;
    /*
      sp_find_routine can have issued an ER_SP_RECURSION_LIMIT error.
      Send message ER_SP_DOES_NOT_EXIST only if procedure is not found in
      cache.
    */
    if (!sp_cache_lookup(&thd->sp_proc_cache, m_name))
      my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "PROCEDURE",
               ErrConvDQName(m_name).ptr());
    return true;
  }
  else
  {
    if (sp->check_execute_access(thd))
      return true;
    /*
      Check that the stored procedure doesn't contain Dynamic SQL
      and doesn't return result sets: such stored procedures can't
      be called from a function or trigger.
    */
    if (thd->in_sub_stmt)
    {
      const char *where= (thd->in_sub_stmt & SUB_STMT_TRIGGER ?
                          "trigger" : "function");
      if (sp->is_not_allowed_in_function(where))
        return true;
    }

    if (do_execute_sp(thd, sp))
      return true;

    /*
      Disable slow log for the above call(), if calls are disabled.
      Instead we will log the executed statements to the slow log.
    */
    if (thd->variables.log_slow_disabled_statements & LOG_SLOW_DISABLE_CALL)
      thd->enable_slow_log= 0;
  }
  return false;
}


/**
  Check whether the SQL statement being processed is prepended by
  SET STATEMENT clause and handle variables assignment if it is.

  @param thd  thread handle
  @param lex  current lex

  @return false in case of success, true in case of error.
*/

bool run_set_statement_if_requested(THD *thd, LEX *lex)
{
  if (!lex->stmt_var_list.is_empty() && !thd->slave_thread)
  {
    Query_arena backup;
    DBUG_PRINT("info", ("SET STATEMENT %d vars", lex->stmt_var_list.elements));

    lex->old_var_list.empty();
    List_iterator_fast<set_var_base> it(lex->stmt_var_list);
    set_var_base *var;

    if (lex->set_arena_for_set_stmt(&backup))
      return true;

    MEM_ROOT *mem_root= thd->mem_root;
    while ((var= it++))
    {
      DBUG_ASSERT(var->is_system());
      set_var *o= NULL, *v= (set_var*)var;
      if (!v->var->is_set_stmt_ok())
      {
        my_error(ER_SET_STATEMENT_NOT_SUPPORTED, MYF(0), v->var->name.str);
        lex->reset_arena_for_set_stmt(&backup);
        lex->old_var_list.empty();
        lex->free_arena_for_set_stmt();
        return true;
      }
      if (v->var->session_is_default(thd))
        o= new set_var(thd,v->type, v->var, &v->base, NULL);
      else
      {
        switch (v->var->option.var_type & GET_TYPE_MASK)
        {
          case GET_BIT:
          case GET_BOOL:
          case GET_INT:
          case GET_LONG:
          case GET_LL:
          {
            bool null_value;
            longlong val= v->var->val_int(&null_value, thd, v->type, &v->base);
            o= new set_var(thd, v->type, v->var, &v->base,
                           (null_value ?
                            (Item *) new (mem_root) Item_null(thd) :
                            (Item *) new (mem_root) Item_int(thd, val)));
          }
          break;
          case GET_UINT:
          case GET_ULONG:
          case GET_ULL:
          {
            bool null_value;
            ulonglong val= v->var->val_int(&null_value, thd, v->type, &v->base);
            o= new set_var(thd, v->type, v->var, &v->base,
                           (null_value ?
                            (Item *) new (mem_root) Item_null(thd) :
                            (Item *) new (mem_root) Item_uint(thd, val)));
          }
          break;
          case GET_DOUBLE:
          {
            bool null_value;
            double val= v->var->val_real(&null_value, thd, v->type, &v->base);
            o= new set_var(thd, v->type, v->var, &v->base,
                           (null_value ?
                            (Item *) new (mem_root) Item_null(thd) :
                            (Item *) new (mem_root) Item_float(thd, val, 1)));
          }
          break;
          default:
          case GET_NO_ARG:
          case GET_DISABLED:
            DBUG_ASSERT(0);
            /* fall through */
          case 0:
          case GET_FLAGSET:
          case GET_ENUM:
          case GET_SET:
          case GET_STR:
          case GET_STR_ALLOC:
          {
            char buff[STRING_BUFFER_USUAL_SIZE];
            String tmp(buff, sizeof(buff), v->var->charset(thd)),*val;
            val= v->var->val_str(&tmp, thd, v->type, &v->base);
            if (val)
            {
              Item_string *str=
		new (mem_root) Item_string(thd, v->var->charset(thd),
                                           val->ptr(), val->length());
              o= new set_var(thd, v->type, v->var, &v->base, str);
            }
            else
              o= new set_var(thd, v->type, v->var, &v->base,
                             new (mem_root) Item_null(thd));
          }
          break;
        }
      }
      DBUG_ASSERT(o);
      lex->old_var_list.push_back(o, thd->mem_root);
    }
    lex->reset_arena_for_set_stmt(&backup);

    if (lex->old_var_list.is_empty())
      lex->free_arena_for_set_stmt();

    if (thd->is_error() ||
        sql_set_variables(thd, &lex->stmt_var_list, false))
    {
      if (!thd->is_error())
        my_error(ER_WRONG_ARGUMENTS, MYF(0), "SET");
      lex->restore_set_statement_var();
      return true;
    }
    /*
      The value of last_insert_id is remembered in THD to be written to binlog
      when it's used *the first time* in the statement. But SET STATEMENT
      must read the old value of last_insert_id to be able to restore it at
      the end. This should not count at "reading of last_insert_id" and
      should not remember last_insert_id for binlog. That is, it should clear
      stmt_depends_on_first_successful_insert_id_in_prev_stmt flag.
    */
    if (!thd->in_sub_stmt)
    {
      thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt= 0;
    }
  }
  return false;
}


/**
  Execute command saved in thd and lex->sql_command.

  @param thd                       Thread handle

  @todo
    - Invalidate the table in the query cache if something changed
    after unlocking when changes become visible.
    TODO: this is workaround. right way will be move invalidating in
    the unlock procedure.
    - TODO: use check_change_password()

  @retval
    FALSE       OK
  @retval
    TRUE        Error
*/

int
mysql_execute_command(THD *thd, bool is_called_from_prepared_stmt)
{
  int res= 0;
  LEX  *lex= thd->lex;
  /* first SELECT_LEX (have special meaning for many of non-SELECTcommands) */
  SELECT_LEX *select_lex= lex->first_select_lex();
  /* first table of first SELECT_LEX */
  TABLE_LIST *first_table= select_lex->table_list.first;
  /* list of all tables in query */
  TABLE_LIST *all_tables;
  /* most outer SELECT_LEX_UNIT of query */
  SELECT_LEX_UNIT *unit= &lex->unit;
  DBUG_ENTER("mysql_execute_command");

  // check that we correctly marked first table for data insertion
  DBUG_ASSERT(!(sql_command_flags[lex->sql_command] & CF_INSERTS_DATA) ||
              first_table->for_insert_data);

  if (thd->security_ctx->password_expired &&
      lex->sql_command != SQLCOM_SET_OPTION &&
      lex->sql_command != SQLCOM_PREPARE &&
      lex->sql_command != SQLCOM_EXECUTE &&
      lex->sql_command != SQLCOM_DEALLOCATE_PREPARE)
  {
    my_error(ER_MUST_CHANGE_PASSWORD, MYF(0));
    DBUG_RETURN(1);
  }

  DBUG_ASSERT(thd->transaction->stmt.is_empty() || thd->in_sub_stmt);
  /*
    Each statement or replication event which might produce deadlock
    should handle transaction rollback on its own. So by the start of
    the next statement transaction rollback request should be fulfilled
    already.
  */
  DBUG_ASSERT(! thd->transaction_rollback_request || thd->in_sub_stmt);
  /*
    In many cases first table of main SELECT_LEX have special meaning =>
    check that it is first table in global list and relink it first in 
    queries_tables list if it is necessary (we need such relinking only
    for queries with subqueries in select list, in this case tables of
    subqueries will go to global list first)

    all_tables will differ from first_table only if most upper SELECT_LEX
    do not contain tables.

    Because of above in place where should be at least one table in most
    outer SELECT_LEX we have following check:
    DBUG_ASSERT(first_table == all_tables);
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
  */
  lex->first_lists_tables_same();
  lex->fix_first_select_number();
  /* should be assigned after making first tables same */
  all_tables= lex->query_tables;
  /* set context for commands which do not use setup_tables */
  select_lex->
    context.resolve_in_table_list_only(select_lex->
                                       table_list.first);

  /*
    Remember last commmand executed, so that we can use it in places
    like mysql_audit_plugin.
  */
  thd->last_sql_command= lex->sql_command;

  /*
    Reset warning count for each query that uses tables
    A better approach would be to reset this for any commands
    that is not a SHOW command or a select that only access local
    variables, but for now this is probably good enough.
  */
  if ((sql_command_flags[lex->sql_command] & CF_DIAGNOSTIC_STMT) != 0)
    thd->get_stmt_da()->set_warning_info_read_only(TRUE);
  else
  {
    thd->get_stmt_da()->set_warning_info_read_only(FALSE);
    if (all_tables)
      thd->get_stmt_da()->opt_clear_warning_info(thd->query_id);
  }

#ifdef HAVE_REPLICATION
  if (unlikely(thd->slave_thread))
  {
    if (lex->sql_command == SQLCOM_DROP_TRIGGER)
    {
      /*
        When dropping a trigger, we need to load its table name
        before checking slave filter rules.
      */
      add_table_for_trigger(thd, thd->lex->spname, 1, &all_tables);
      
      if (!all_tables)
      {
        /*
          If table name cannot be loaded,
          it means the trigger does not exists possibly because
          CREATE TRIGGER was previously skipped for this trigger
          according to slave filtering rules.
          Returning success without producing any errors in this case.
        */
        if (!thd->lex->create_info.if_exists() &&
            !(thd->variables.option_bits & OPTION_IF_EXISTS))
          DBUG_RETURN(0);
        /*
          DROP TRIGGER IF NOT EXISTS will return without an error later
          after possibly writing the query to a binlog
        */
      }
      else // force searching in slave.cc:tables_ok()
        all_tables->updating= 1;
    }

    /*
      For fix of BUG#37051, the master stores the table map for update
      in the Query_log_event, and the value is assigned to
      thd->variables.table_map_for_update before executing the update
      query.

      If thd->variables.table_map_for_update is set, then we are
      replicating from a new master, we can use this value to apply
      filter rules without opening all the tables. However If
      thd->variables.table_map_for_update is not set, then we are
      replicating from an old master, so we just skip this and
      continue with the old method. And of course, the bug would still
      exist for old masters.
    */
    if (lex->sql_command == SQLCOM_UPDATE_MULTI &&
        thd->table_map_for_update)
    {
      table_map table_map_for_update= thd->table_map_for_update;
      uint nr= 0;
      TABLE_LIST *table;
      for (table=all_tables; table; table=table->next_global, nr++)
      {
        if (table_map_for_update & ((table_map)1 << nr))
          table->updating= TRUE;
        else
          table->updating= FALSE;
      }

      if (all_tables_not_ok(thd, all_tables))
      {
        /* we warn the slave SQL thread */
        my_message(ER_SLAVE_IGNORED_TABLE, ER_THD(thd, ER_SLAVE_IGNORED_TABLE),
                   MYF(0));
      }
    }
    
    /*
      Check if statement should be skipped because of slave filtering
      rules

      Exceptions are:
      - UPDATE MULTI: For this statement, we want to check the filtering
        rules later in the code
      - SET: we always execute it (Not that many SET commands exists in
        the binary log anyway -- only 4.1 masters write SET statements,
	in 5.0 there are no SET statements in the binary log)
      - DROP TEMPORARY TABLE IF EXISTS: we always execute it (otherwise we
        have stale files on slave caused by exclusion of one tmp table).
    */
    if (!(lex->sql_command == SQLCOM_UPDATE_MULTI) &&
	!(lex->sql_command == SQLCOM_SET_OPTION) &&
	!((lex->sql_command == SQLCOM_DROP_TABLE ||
           lex->sql_command == SQLCOM_DROP_SEQUENCE) &&
          lex->tmp_table() && lex->if_exists()) &&
        all_tables_not_ok(thd, all_tables))
    {
      /* we warn the slave SQL thread */
      my_message(ER_SLAVE_IGNORED_TABLE, ER_THD(thd, ER_SLAVE_IGNORED_TABLE),
                 MYF(0));
      DBUG_RETURN(0);
    }
    /* 
       Execute deferred events first
    */
    if (slave_execute_deferred_events(thd))
      DBUG_RETURN(-1);
  }
  else
  {
#endif /* HAVE_REPLICATION */
    /*
      When option readonly is set deny operations which change non-temporary
      tables. Except for the replication thread and the 'super' users.
    */
    if (deny_updates_if_read_only_option(thd, all_tables))
    {
      mariadb_error_read_only();
      DBUG_RETURN(-1);
    }
#ifdef HAVE_REPLICATION
  } /* endif unlikely slave */
#endif
  Opt_trace_start ots(thd);

  /* store old value of binlog format */
  enum_binlog_format orig_binlog_format,orig_current_stmt_binlog_format;

  thd->get_binlog_format(&orig_binlog_format,
                         &orig_current_stmt_binlog_format);
#ifdef WITH_WSREP
  if (WSREP(thd))
  {
    /*
      change LOCK TABLE WRITE to transaction
    */
    if (lex->sql_command == SQLCOM_LOCK_TABLES && wsrep_convert_LOCK_to_trx)
    {
      for (TABLE_LIST *table= all_tables; table; table= table->next_global)
      {
	if (table->lock_type >= TL_FIRST_WRITE)
        {
	  lex->sql_command= SQLCOM_BEGIN;
	  thd->wsrep_converted_lock_session= true;
	  break;
	}
      }
    }
    if (lex->sql_command == SQLCOM_UNLOCK_TABLES &&
	thd->wsrep_converted_lock_session)
    {
      thd->wsrep_converted_lock_session= false;
      lex->sql_command= SQLCOM_COMMIT;
      lex->tx_release= TVL_NO;
    }

    /*
     * Bail out if DB snapshot has not been installed. We however,
     * allow SET and SHOW queries and reads from information schema
     * and dirty reads (if configured)
     */
    if (!(thd->wsrep_applier) &&
        !(wsrep_ready_get() && wsrep_reject_queries == WSREP_REJECT_NONE)  &&
        !(thd->variables.wsrep_dirty_reads &&
          (sql_command_flags[lex->sql_command] & CF_CHANGES_DATA) == 0)    &&
        !wsrep_tables_accessible_when_detached(all_tables)                 &&
        lex->sql_command != SQLCOM_SET_OPTION                              &&
        lex->sql_command != SQLCOM_CHANGE_DB                               &&
        !(lex->sql_command == SQLCOM_SELECT && !all_tables)                &&
        !wsrep_is_show_query(lex->sql_command))
    {
      my_message(ER_UNKNOWN_COM_ERROR,
                 "WSREP has not yet prepared node for application use", MYF(0));
      goto error;
    }
  }
#endif /* WITH_WSREP */
  status_var_increment(thd->status_var.com_stat[lex->sql_command]);
  thd->progress.report_to_client= MY_TEST(sql_command_flags[lex->sql_command] &
                                          CF_REPORT_PROGRESS);

  DBUG_ASSERT(thd->transaction->stmt.modified_non_trans_table == FALSE);

  /*
    Assign system variables with values specified by the clause
    SET STATEMENT var1=value1 [, var2=value2, ...] FOR <statement>
    if they are any.
  */
  if (run_set_statement_if_requested(thd, lex))
    goto error;

  /* After SET STATEMENT is done, we can initialize the Optimizer Trace: */
  ots.init(thd, all_tables, lex->sql_command, &lex->var_list, thd->query(),
           thd->query_length(), thd->variables.character_set_client);

  if (thd->lex->mi.connection_name.str == NULL)
      thd->lex->mi.connection_name= thd->variables.default_master_connection;

  /*
    Force statement logging for DDL commands to allow us to update
    privilege, system or statistic tables directly without the updates
    getting logged.
  */
  if (!(sql_command_flags[lex->sql_command] &
        (CF_CAN_GENERATE_ROW_EVENTS | CF_FORCE_ORIGINAL_BINLOG_FORMAT |
         CF_STATUS_COMMAND)))
    thd->set_binlog_format_stmt();

  /*
    End a active transaction so that this command will have it's
    own transaction and will also sync the binary log. If a DDL is
    not run in it's own transaction it may simply never appear on
    the slave in case the outside transaction rolls back.
  */
  if (stmt_causes_implicit_commit(thd, CF_IMPLICIT_COMMIT_BEGIN))
  {
    /*
      Note that this should never happen inside of stored functions
      or triggers as all such statements prohibited there.
    */
    DBUG_ASSERT(! thd->in_sub_stmt);
    /* Statement transaction still should not be started. */
    DBUG_ASSERT(thd->transaction->stmt.is_empty());
    if (!(thd->variables.option_bits & OPTION_GTID_BEGIN))
    {
      /* Commit the normal transaction if one is active. */
      bool commit_failed= trans_commit_implicit(thd);
      /* Release metadata locks acquired in this transaction. */
      thd->release_transactional_locks();
      if (commit_failed)
      {
        WSREP_DEBUG("implicit commit failed, MDL released: %lld",
                    (longlong) thd->thread_id);
        goto error;
      }
    }
    thd->transaction->stmt.mark_trans_did_ddl();
#ifdef WITH_WSREP
    /* Clean up the previous transaction on implicit commit */
    if (WSREP_NNULL(thd) && wsrep_thd_is_local(thd) &&
        wsrep_after_statement(thd))
    {
      goto error;
    }
#endif /* WITH_WSREP */
  }

#ifndef DBUG_OFF
  if (lex->sql_command != SQLCOM_SET_OPTION)
    DEBUG_SYNC(thd,"before_execute_sql_command");
#endif

  /*
    Check if we are in a read-only transaction and we're trying to
    execute a statement which should always be disallowed in such cases.

    Note that this check is done after any implicit commits.
  */
  if (thd->tx_read_only &&
      (sql_command_flags[lex->sql_command] & CF_DISALLOW_IN_RO_TRANS))
  {
    my_error(ER_CANT_EXECUTE_IN_READ_ONLY_TRANSACTION, MYF(0));
    goto error;
  }

  /*
    Close tables open by HANDLERs before executing DDL statement
    which is going to affect those tables.

    This should happen before temporary tables are pre-opened as
    otherwise we will get errors about attempt to re-open tables
    if table to be changed is open through HANDLER.

    Note that even although this is done before any privilege
    checks there is no security problem here as closing open
    HANDLER doesn't require any privileges anyway.
  */
  if (sql_command_flags[lex->sql_command] & CF_HA_CLOSE)
    mysql_ha_rm_tables(thd, all_tables);

  /*
    Pre-open temporary tables to simplify privilege checking
    for statements which need this.
  */
  if (sql_command_flags[lex->sql_command] & CF_PREOPEN_TMP_TABLES)
  {
    if (thd->open_temporary_tables(all_tables))
      goto error;
  }

  if (sql_command_flags[lex->sql_command] & CF_STATUS_COMMAND)
    thd->query_plan_flags|= QPLAN_STATUS;
  if (sql_command_flags[lex->sql_command] & CF_ADMIN_COMMAND)
    thd->query_plan_flags|= QPLAN_ADMIN;

  /* Start timeouts */
  thd->set_query_timer_if_needed();

#ifdef WITH_WSREP
  /* Check wsrep_mode rules before command execution. */
  if (WSREP_NNULL(thd) &&
      wsrep_thd_is_local(thd) && !wsrep_check_mode_before_cmd_execute(thd))
    goto error;

  /*
    Always start a new transaction for a wsrep THD unless the
    current command is DDL or explicit BEGIN. This will guarantee that
    the THD is BF abortable even if it does not generate any
    changes and takes only read locks. If the statement does not
    start a multi STMT transaction, the wsrep_transaction is
    committed as empty at the end of this function.

    Transaction is started for BEGIN in trans_begin(), for DDL the
    implicit commit took care of committing previous transaction
    above and a new transaction should not be started.

    Do not start transaction for stored procedures, it will be handled
    internally in SP processing.
  */
  if (WSREP_NNULL(thd)                    &&
      wsrep_thd_is_local(thd)             &&
      lex->sql_command != SQLCOM_BEGIN    &&
      lex->sql_command != SQLCOM_CALL     &&
      lex->sql_command != SQLCOM_EXECUTE  &&
      lex->sql_command != SQLCOM_EXECUTE_IMMEDIATE &&
      !(sql_command_flags[lex->sql_command] & CF_AUTO_COMMIT_TRANS))
  {
    wsrep_start_trx_if_not_started(thd);
  }
#endif /* WITH_WSREP */

  switch (lex->sql_command) {

  case SQLCOM_SHOW_EVENTS:
#ifndef HAVE_EVENT_SCHEDULER
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "embedded server");
    break;
#endif
  case SQLCOM_SHOW_STATUS:
  {
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
    execute_show_status(thd, all_tables);

    break;
  }
  case SQLCOM_SHOW_EXPLAIN:
  case SQLCOM_SHOW_ANALYZE:
  {
    if (!thd->security_ctx->priv_user[0] &&
        check_global_access(thd, PRIV_STMT_SHOW_EXPLAIN))
      break;

    /*
      The select should use only one table, it's the SHOW EXPLAIN pseudo-table
    */
    if (lex->sroutines.records || lex->query_tables->next_global)
    {
      my_message(ER_SET_CONSTANTS_ONLY, ER_THD(thd, ER_SET_CONSTANTS_ONLY),
		 MYF(0));
      goto error;
    }

    Item **it= lex->value_list.head_ref();
    if (!(*it)->basic_const_item() ||
        (*it)->fix_fields_if_needed_for_scalar(lex->thd, it))
    {
      my_message(ER_SET_CONSTANTS_ONLY, ER_THD(thd, ER_SET_CONSTANTS_ONLY),
		 MYF(0));
      goto error;
    }
  }
    /* fall through */
  case SQLCOM_SHOW_STATUS_PROC:
  case SQLCOM_SHOW_STATUS_FUNC:
  case SQLCOM_SHOW_STATUS_PACKAGE:
  case SQLCOM_SHOW_STATUS_PACKAGE_BODY:
  case SQLCOM_SHOW_DATABASES:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_SHOW_TRIGGERS:
  case SQLCOM_SHOW_TABLE_STATUS:
  case SQLCOM_SHOW_OPEN_TABLES:
  case SQLCOM_SHOW_GENERIC:
  case SQLCOM_SHOW_PLUGINS:
  case SQLCOM_SHOW_FIELDS:
  case SQLCOM_SHOW_KEYS:
  case SQLCOM_SHOW_VARIABLES:
  case SQLCOM_SHOW_CHARSETS:
  case SQLCOM_SHOW_COLLATIONS:
  case SQLCOM_SHOW_STORAGE_ENGINES:
  case SQLCOM_SHOW_PROFILE:
  case SQLCOM_SHOW_SLAVE_STAT:
  case SQLCOM_SELECT:
  {
#ifdef WITH_WSREP
    if (lex->sql_command == SQLCOM_SELECT)
    {
      WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_READ);
    }
    else
    {
      WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
# ifdef ENABLED_PROFILING
      if (lex->sql_command == SQLCOM_SHOW_PROFILE)
        thd->profiling.discard_current_query();
# endif
    }
#endif /* WITH_WSREP */

    thd->status_var.last_query_cost= 0.0;

    /*
      lex->exchange != NULL implies SELECT .. INTO OUTFILE and this
      requires FILE_ACL access.
    */
    privilege_t privileges_requested= lex->exchange ? SELECT_ACL | FILE_ACL :
                                                      SELECT_ACL;

    if (all_tables)
      res= check_table_access(thd,
                              privileges_requested,
                              all_tables, FALSE, UINT_MAX, FALSE);
    else
      res= check_access(thd, privileges_requested, any_db.str, NULL,NULL,0,0);

    if (!res)
      res= execute_sqlcom_select(thd, all_tables);

    break;
  }
  case SQLCOM_EXECUTE_IMMEDIATE:
  {
    mysql_sql_stmt_execute_immediate(thd);
    break;
  }
  case SQLCOM_PREPARE:
  {
    mysql_sql_stmt_prepare(thd);
    break;
  }
  case SQLCOM_EXECUTE:
  {
    mysql_sql_stmt_execute(thd);
    break;
  }
  case SQLCOM_DEALLOCATE_PREPARE:
  {
    mysql_sql_stmt_close(thd);
    break;
  }
  case SQLCOM_DO:
    if (check_table_access(thd, SELECT_ACL, all_tables, FALSE, UINT_MAX, FALSE)
        || open_and_lock_tables(thd, all_tables, TRUE, 0))
      goto error;

    res= mysql_do(thd, *lex->insert_list);
    break;

  case SQLCOM_EMPTY_QUERY:
    my_ok(thd);
    break;

  case SQLCOM_HELP:
    res= mysqld_help(thd,lex->help_arg);
    break;

#ifndef EMBEDDED_LIBRARY
  case SQLCOM_PURGE:
  {
    if (check_global_access(thd, PRIV_STMT_PURGE_BINLOG))
      goto error;
    /* PURGE MASTER LOGS TO 'file' */
    res = purge_master_logs(thd, lex->to_log);
    break;
  }
  case SQLCOM_PURGE_BEFORE:
  {
    Item *it;

    if (check_global_access(thd, PRIV_STMT_PURGE_BINLOG))
      goto error;
    /* PURGE MASTER LOGS BEFORE 'data' */
    it= (Item *)lex->value_list.head();
    if (it->fix_fields_if_needed_for_scalar(lex->thd, &it))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), "PURGE LOGS BEFORE");
      goto error;
    }
    it= new (thd->mem_root) Item_func_unix_timestamp(thd, it);
    it->fix_fields(thd, &it);
    res = purge_master_logs_before_date(thd, (ulong)it->val_int());
    break;
  }
#endif
  case SQLCOM_SHOW_WARNS:
  {
    res= mysqld_show_warnings(thd, (ulong)
			      ((1L << (uint) Sql_condition::WARN_LEVEL_NOTE) |
			       (1L << (uint) Sql_condition::WARN_LEVEL_WARN) |
			       (1L << (uint) Sql_condition::WARN_LEVEL_ERROR)
			       ));
    break;
  }
  case SQLCOM_SHOW_ERRORS:
  {
    res= mysqld_show_warnings(thd, (ulong)
			      (1L << (uint) Sql_condition::WARN_LEVEL_ERROR));
    break;
  }
  case SQLCOM_SHOW_PROFILES:
  {
#if defined(ENABLED_PROFILING)
    thd->profiling.discard_current_query();
    res= thd->profiling.show_profiles();
    if (res)
      goto error;
#else
    my_error(ER_FEATURE_DISABLED, MYF(0), "SHOW PROFILES", "enable-profiling");
    goto error;
#endif
    break;
  }

#ifdef HAVE_REPLICATION
  case SQLCOM_SHOW_SLAVE_HOSTS:
  {
    if (check_global_access(thd, PRIV_STMT_SHOW_SLAVE_HOSTS))
      goto error;
    res = show_slave_hosts(thd);
    break;
  }
  case SQLCOM_SHOW_RELAYLOG_EVENTS:
  {
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
    if (check_global_access(thd, PRIV_STMT_SHOW_RELAYLOG_EVENTS))
      goto error;
    res = mysql_show_binlog_events(thd);
    break;
  }
  case SQLCOM_SHOW_BINLOG_EVENTS:
  {
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
    if (check_global_access(thd, PRIV_STMT_SHOW_BINLOG_EVENTS))
      goto error;
    res = mysql_show_binlog_events(thd);
    break;
  }
#endif

  case SQLCOM_ASSIGN_TO_KEYCACHE:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    if (check_access(thd, INDEX_ACL, first_table->db.str,
                     &first_table->grant.privilege,
                     &first_table->grant.m_internal,
                     0, 0))
      goto error;
    res= mysql_assign_to_keycache(thd, first_table, &lex->ident);
    break;
  }
  case SQLCOM_PRELOAD_KEYS:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    if (check_access(thd, INDEX_ACL, first_table->db.str,
                     &first_table->grant.privilege,
                     &first_table->grant.m_internal,
                     0, 0))
      goto error;
    res = mysql_preload_keys(thd, first_table);
    break;
  }
#ifdef HAVE_REPLICATION
  case SQLCOM_CHANGE_MASTER:
  {
    LEX_MASTER_INFO *lex_mi= &thd->lex->mi;
    Master_info *mi;
    bool new_master= 0;
    bool master_info_added;

    if (check_global_access(thd, PRIV_STMT_CHANGE_MASTER))
      goto error;
    /*
      In this code it's ok to use LOCK_active_mi as we are adding new things
      into master_info_index
    */
    mysql_mutex_lock(&LOCK_active_mi);
    if (!master_info_index)
    {
      mysql_mutex_unlock(&LOCK_active_mi);
      my_error(ER_SERVER_SHUTDOWN, MYF(0));
      goto error;
    }

    mi= master_info_index->get_master_info(&lex_mi->connection_name,
                                           Sql_condition::WARN_LEVEL_NOTE);

    if (mi == NULL)
    {
      /* New replication created */
      mi= new Master_info(&lex_mi->connection_name, relay_log_recovery); 
      if (unlikely(!mi || mi->error()))
      {
        delete mi;
        res= 1;
        mysql_mutex_unlock(&LOCK_active_mi);
        break;
      }
      new_master= 1;
    }

    res= change_master(thd, mi, &master_info_added);
    if (res && new_master)
    {
      /*
        If the new master was added by change_master(), remove it as it didn't
        work (this will free mi as well).

        If new master was not added, we still need to free mi.
      */
      if (master_info_added)
        master_info_index->remove_master_info(mi, 1);
      else
        delete mi;
    }
    else
    {
      mi->rpl_filter= get_or_create_rpl_filter(lex_mi->connection_name.str,
                                               lex_mi->connection_name.length);
    }

    mysql_mutex_unlock(&LOCK_active_mi);
    break;
  }

  case SQLCOM_SHOW_BINLOG_STAT:
  {
    /* Accept one of two privileges */
    if (check_global_access(thd, PRIV_STMT_SHOW_BINLOG_STATUS))
      goto error;
    res = show_binlog_info(thd);
    break;
  }

#endif /* HAVE_REPLICATION */
  case SQLCOM_SHOW_ENGINE_STATUS:
    {
      if (check_global_access(thd, PRIV_STMT_SHOW_ENGINE_STATUS))
        goto error;
      res = ha_show_status(thd, lex->create_info.db_type, HA_ENGINE_STATUS);
      break;
    }
  case SQLCOM_SHOW_ENGINE_MUTEX:
    {
      if (check_global_access(thd, PRIV_STMT_SHOW_ENGINE_MUTEX))
        goto error;
      res = ha_show_status(thd, lex->create_info.db_type, HA_ENGINE_MUTEX);
      break;
    }
  case SQLCOM_DROP_INDEX:
    if (thd->variables.option_bits & OPTION_IF_EXISTS)
      lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);
    /* fall through */
  case SQLCOM_CREATE_INDEX:
  /*
    CREATE INDEX and DROP INDEX are implemented by calling ALTER
    TABLE with proper arguments.

    In the future ALTER TABLE will notice that the request is to
    only add indexes and create these one by one for the existing
    table without having to do a full rebuild.
  */
  {
    /* Prepare stack copies to be re-execution safe */
    Table_specification_st create_info;
    Alter_info alter_info(lex->alter_info, thd->mem_root);

    if (unlikely(thd->is_fatal_error)) /* out of memory creating alter_info */
      goto error;

    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    if (check_one_table_access(thd, INDEX_ACL, all_tables))
      goto error; /* purecov: inspected */

    create_info.init();
    create_info.db_type= 0;
    create_info.row_type= ROW_TYPE_NOT_USED;
    create_info.alter_info= &alter_info;

    WSREP_TO_ISOLATION_BEGIN(first_table->db.str, first_table->table_name.str, NULL);

    Recreate_info recreate_info;
    res= mysql_alter_table(thd, &first_table->db, &first_table->table_name,
                           &create_info, first_table,
                           &recreate_info, &alter_info,
                           0, (ORDER*) 0, 0, lex->if_exists());
    break;
  }
#ifdef HAVE_REPLICATION
  case SQLCOM_SLAVE_START:
  {
    LEX_MASTER_INFO* lex_mi= &thd->lex->mi;
    Master_info *mi;
    int load_error;

    load_error= rpl_load_gtid_slave_state(thd);

    /*
      We don't need to ensure that only one user is using master_info
      as start_slave is protected against simultaneous usage
    */
    if (unlikely((mi= get_master_info(&lex_mi->connection_name,
                                      Sql_condition::WARN_LEVEL_ERROR))))
    {
      if (load_error)
      {
        /*
          We cannot start a slave using GTID if we cannot load the
          GTID position from the mysql.gtid_slave_pos table. But we
          can allow non-GTID replication (useful eg. during upgrade).
        */
        if (mi->using_gtid != Master_info::USE_GTID_NO)
        {
          mi->release();
          break;
        }
        else
          thd->clear_error();
      }
      if (!start_slave(thd, mi, 1 /* net report*/))
        my_ok(thd);
      mi->release();
    }
    break;
  }
  case SQLCOM_SLAVE_STOP:
  {
    LEX_MASTER_INFO *lex_mi;
    Master_info *mi;
    /*
      If the client thread has locked tables, a deadlock is possible.
      Assume that
      - the client thread does LOCK TABLE t READ.
      - then the master updates t.
      - then the SQL slave thread wants to update t,
        so it waits for the client thread because t is locked by it.
    - then the client thread does SLAVE STOP.
      SLAVE STOP waits for the SQL slave thread to terminate its
      update t, which waits for the client thread because t is locked by it.
      To prevent that, refuse SLAVE STOP if the
      client thread has locked tables
    */
    if (thd->locked_tables_mode ||
        thd->in_active_multi_stmt_transaction() ||
        thd->global_read_lock.is_acquired())
    {
      my_message(ER_LOCK_OR_ACTIVE_TRANSACTION,
                 ER_THD(thd, ER_LOCK_OR_ACTIVE_TRANSACTION), MYF(0));
      goto error;
    }

    lex_mi= &thd->lex->mi;
    if ((mi= get_master_info(&lex_mi->connection_name,
                             Sql_condition::WARN_LEVEL_ERROR)))
    {
      if (stop_slave(thd, mi, 1/* net report*/))
        res= 1;
      mi->release();
      if (rpl_parallel_resize_pool_if_no_slaves())
        res= 1;
      if (!res)
        my_ok(thd);
    }
    break;
  }
  case SQLCOM_SLAVE_ALL_START:
  {
    mysql_mutex_lock(&LOCK_active_mi);
    if (master_info_index && !master_info_index->start_all_slaves(thd))
      my_ok(thd);
    mysql_mutex_unlock(&LOCK_active_mi);
    break;
  }
  case SQLCOM_SLAVE_ALL_STOP:
  {
    if (thd->locked_tables_mode ||
        thd->in_active_multi_stmt_transaction() ||
        thd->global_read_lock.is_acquired())
    {
      my_message(ER_LOCK_OR_ACTIVE_TRANSACTION,
                 ER_THD(thd, ER_LOCK_OR_ACTIVE_TRANSACTION), MYF(0));
      goto error;
    }
    mysql_mutex_lock(&LOCK_active_mi);
    if (master_info_index && !master_info_index->stop_all_slaves(thd))
      my_ok(thd);      
    mysql_mutex_unlock(&LOCK_active_mi);
    break;
  }
#endif /* HAVE_REPLICATION */
  case SQLCOM_RENAME_TABLE:
  {
    if (check_rename_table(thd, first_table, all_tables))
      goto error;

    WSREP_TO_ISOLATION_BEGIN(0, 0, first_table);

    if (thd->variables.option_bits & OPTION_IF_EXISTS)
      lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);

    if (mysql_rename_tables(thd, first_table, 0, lex->if_exists()))
      goto error;
    break;
  }
#ifndef EMBEDDED_LIBRARY
  case SQLCOM_SHOW_BINLOGS:
#ifdef DONT_ALLOW_SHOW_COMMANDS
    my_message(ER_NOT_ALLOWED_COMMAND, ER_THD(thd, ER_NOT_ALLOWED_COMMAND),
               MYF(0)); /* purecov: inspected */
    goto error;
#else
    {
      if (check_global_access(thd, PRIV_STMT_SHOW_BINARY_LOGS))
	goto error;
      WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
      res = show_binlogs(thd);
      break;
    }
#endif
#endif /* EMBEDDED_LIBRARY */
  case SQLCOM_SHOW_CREATE:
  {
     DBUG_ASSERT(first_table == all_tables && first_table != 0);
#ifdef DONT_ALLOW_SHOW_COMMANDS
    my_message(ER_NOT_ALLOWED_COMMAND, ER_THD(thd, ER_NOT_ALLOWED_COMMAND),
               MYF(0)); /* purecov: inspected */
    goto error;
#else
      WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);

     /*
        Access check:
        SHOW CREATE TABLE require any privileges on the table level (ie
        effecting all columns in the table).
        SHOW CREATE VIEW require the SHOW_VIEW and SELECT ACLs on the table
        level.
        NOTE: SHOW_VIEW ACL is checked when the view is created.
      */

      DBUG_PRINT("debug", ("lex->only_view: %d, table: %s.%s",
                           lex->table_type == TABLE_TYPE_VIEW,
                           first_table->db.str, first_table->table_name.str));
      res= mysqld_show_create(thd, first_table);
      break;
#endif
  }
  case SQLCOM_CHECKSUM:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_READ);

    if (check_table_access(thd, SELECT_ACL, all_tables,
                           FALSE, UINT_MAX, FALSE))
      goto error; /* purecov: inspected */

    res = mysql_checksum_table(thd, first_table, &lex->check_opt);
    break;
  }
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    DBUG_ASSERT(lex->m_sql_cmd != NULL);

    res = lex->m_sql_cmd->execute(thd);
    thd->abort_on_warning= 0;
    break;
  }
  case SQLCOM_REPLACE:
    if ((res= generate_incident_event(thd)))
      break;
    /* fall through */
  case SQLCOM_INSERT:
  {
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_INSERT_REPLACE);
    select_result *sel_result= NULL;
    DBUG_ASSERT(first_table == all_tables && first_table != 0);

    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_INSERT_REPLACE);

    /*
      Since INSERT DELAYED doesn't support temporary tables, we could
      not pre-open temporary tables for SQLCOM_INSERT / SQLCOM_REPLACE.
      Open them here instead.
    */
    if (first_table->lock_type != TL_WRITE_DELAYED)
    {
      res= (thd->open_temporary_tables(all_tables)) ? TRUE : FALSE;
      if (res)
        break;
    }

    if ((res= insert_precheck(thd, all_tables)))
      break;

    MYSQL_INSERT_START(thd->query());
    Protocol* save_protocol=NULL;

    if (lex->has_returning())
    {
      status_var_increment(thd->status_var.feature_insert_returning);

      /* This is INSERT ... RETURNING. It will return output to the client */
      if (thd->lex->analyze_stmt)
      {
        /*
          Actually, it is ANALYZE .. INSERT .. RETURNING. We need to produce
          output and then discard it.
        */
        sel_result= new (thd->mem_root) select_send_analyze(thd);
        save_protocol= thd->protocol;
        thd->protocol= new Protocol_discard(thd);
      }
      else
      {
        if (!(sel_result= new (thd->mem_root) select_send(thd)))
          goto error;
      }
    }

    res= mysql_insert(thd, all_tables, lex->field_list, lex->many_values,
                      lex->update_list, lex->value_list,
                      lex->duplicates, lex->ignore, sel_result);
    status_var_add(thd->status_var.rows_sent, thd->get_sent_row_count());
    if (save_protocol)
    {
      delete thd->protocol;
      thd->protocol= save_protocol;
    }
    if (!res && thd->lex->analyze_stmt)
    {
      bool extended= thd->lex->describe & DESCRIBE_EXTENDED;
      res= thd->lex->explain->send_explain(thd, extended);
    }
    delete sel_result;
    MYSQL_INSERT_DONE(res, (ulong) thd->get_row_count_func());
    /*
      If we have inserted into a VIEW, and the base table has
      AUTO_INCREMENT column, but this column is not accessible through
      a view, then we should restore LAST_INSERT_ID to the value it
      had before the statement.
    */
    if (first_table->view && !first_table->contain_auto_increment)
      thd->first_successful_insert_id_in_cur_stmt=
        thd->first_successful_insert_id_in_prev_stmt;

#ifdef ENABLED_DEBUG_SYNC
    DBUG_EXECUTE_IF("after_mysql_insert",
                    {
                      const char act1[]= "now wait_for signal.continue";
                      const char act2[]= "now signal signal.continued";
                      DBUG_ASSERT(debug_sync_service);
                      DBUG_ASSERT(!debug_sync_set_action(thd,
                                                         STRING_WITH_LEN(act1)));
                      DBUG_ASSERT(!debug_sync_set_action(thd,
                                                         STRING_WITH_LEN(act2)));
                    };);
    DEBUG_SYNC(thd, "after_mysql_insert");
#endif
    break;
  }
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_INSERT_SELECT:
  {
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_INSERT_REPLACE);
    select_insert *sel_result;
    select_result *result= NULL;
    bool explain= MY_TEST(lex->describe);
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_UPDATE_DELETE);

    if ((res= insert_precheck(thd, all_tables)))
      break;

#ifdef WITH_WSREP
    bool wsrep_toi= false;
    const bool wsrep= WSREP(thd);

    if (wsrep && thd->wsrep_consistency_check == CONSISTENCY_CHECK_DECLARED)
    {
      thd->wsrep_consistency_check = CONSISTENCY_CHECK_RUNNING;
      wsrep_toi= true;
      WSREP_TO_ISOLATION_BEGIN(first_table->db.str, first_table->table_name.str, NULL);
    }
#endif /* WITH_WSREP */

    /*
      INSERT...SELECT...ON DUPLICATE KEY UPDATE/REPLACE SELECT/
      INSERT...IGNORE...SELECT can be unsafe, unless ORDER BY PRIMARY KEY
      clause is used in SELECT statement. We therefore use row based
      logging if mixed or row based logging is available.
      TODO: Check if the order of the output of the select statement is
      deterministic. Waiting for BUG#42415
    */
    if (lex->sql_command == SQLCOM_INSERT_SELECT &&
        lex->duplicates == DUP_UPDATE)
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_INSERT_SELECT_UPDATE);

    if (lex->sql_command == SQLCOM_INSERT_SELECT && lex->ignore)
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_INSERT_IGNORE_SELECT);

    if (lex->sql_command == SQLCOM_REPLACE_SELECT)
      lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_REPLACE_SELECT);

    /* Fix lock for first table */
    if (first_table->lock_type == TL_WRITE_DELAYED)
      first_table->lock_type= TL_WRITE;

    /* Don't unlock tables until command is written to binary log */
    select_lex->options|= SELECT_NO_UNLOCK;

    unit->set_limit(select_lex);

    if (!(res=open_and_lock_tables(thd, all_tables, TRUE, 0)))
    {
      MYSQL_INSERT_SELECT_START(thd->query());

#ifdef WITH_WSREP
      if (wsrep && !first_table->view)
      {
        const legacy_db_type db_type= first_table->table->file->partition_ht()->db_type;
        // For InnoDB we don't need to worry about anything here:
        if (db_type != DB_TYPE_INNODB)
        {
          // For consistency check inserted table needs to be InnoDB
          if (thd->wsrep_consistency_check != NO_CONSISTENCY_CHECK)
          {
            push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                HA_ERR_UNSUPPORTED,
                                "Galera cluster does support consistency check only"
                                " for InnoDB tables.");
            thd->wsrep_consistency_check= NO_CONSISTENCY_CHECK;
          }
          /* Only TOI allowed to !InnoDB tables */
          if (wsrep_OSU_method_get(thd) != WSREP_OSU_TOI)
          {
            my_error(ER_NOT_SUPPORTED_YET, MYF(0), "RSU on this table engine");
            break;
          }
          // For !InnoDB we start TOI if it is not yet started and hope for the best
          if (!wsrep_toi)
          {
            /* Currently we support TOI for MyISAM only. */
            if ((db_type == DB_TYPE_MYISAM && wsrep_check_mode(WSREP_MODE_REPLICATE_MYISAM)) ||
                (db_type == DB_TYPE_ARIA   && wsrep_check_mode(WSREP_MODE_REPLICATE_ARIA)))
            {
              WSREP_TO_ISOLATION_BEGIN(first_table->db.str, first_table->table_name.str, NULL);
            }
          }
        }
      }
#endif /* WITH_WSREP */

      /*
        Only the INSERT table should be merged. Other will be handled by
        select.
      */

      Protocol* save_protocol=NULL;

      if (lex->has_returning())
      {
        status_var_increment(thd->status_var.feature_insert_returning);

        /* This is INSERT ... RETURNING. It will return output to the client */
        if (thd->lex->analyze_stmt)
        {
          /*
            Actually, it is ANALYZE .. INSERT .. RETURNING. We need to produce
            output and then discard it.
          */
          result= new (thd->mem_root) select_send_analyze(thd);
          save_protocol= thd->protocol;
          thd->protocol= new Protocol_discard(thd);
        }
        else
        {
          if (!(result= new (thd->mem_root) select_send(thd)))
            goto error;
        }
      }

      /* Skip first table, which is the table we are inserting in */
      TABLE_LIST *second_table= first_table->next_local;
      /*
        This is a hack: this leaves select_lex->table_list in an inconsistent
        state as 'elements' does not contain number of elements in the list.
        Moreover, if second_table == NULL then 'next' becomes invalid.
        TODO: fix it by removing the front element (restoring of it should
        be done properly as well)
      */
      select_lex->table_list.first= second_table;
      select_lex->context.table_list=
        select_lex->context.first_name_resolution_table= second_table;
      res= mysql_insert_select_prepare(thd, result);
      if (!res &&
          (sel_result= new (thd->mem_root)
                       select_insert(thd, first_table,
                                    first_table->table,
                                    &lex->field_list,
                                    &lex->update_list,
                                    &lex->value_list,
                                    lex->duplicates,
                                    lex->ignore,
                                    result)))
      {
        if (lex->analyze_stmt)
          ((select_result_interceptor*)sel_result)->disable_my_ok_calls();

        if (explain)
          res= mysql_explain_union(thd, &thd->lex->unit, sel_result);
        else
          res= handle_select(thd, lex, sel_result, OPTION_SETUP_TABLES_DONE);
        /*
          Invalidate the table in the query cache if something changed
          after unlocking when changes become visible.
          TODO: this is workaround. right way will be move invalidating in
          the unlock procedure.
        */
        if (!res && first_table->lock_type ==  TL_WRITE_CONCURRENT_INSERT &&
            thd->lock)
        {
          /* INSERT ... SELECT should invalidate only the very first table */
          TABLE_LIST *save_table= first_table->next_local;
          first_table->next_local= 0;
          query_cache_invalidate3(thd, first_table, 1);
          first_table->next_local= save_table;
        }
        if (explain)
        {
          /*
            sel_result needs to be cleaned up properly.
            INSERT... SELECT statement will call either send_eof() or
            abort_result_set(). EXPLAIN doesn't call either, so we need
            to cleanup manually.
          */
          sel_result->abort_result_set();
        }
        delete sel_result;
      }
      else if (res < 0)
      {
        /*
          Insert should be ignored but we have to log the query in statement
          format in the binary log
        */
        res= thd->binlog_current_query_unfiltered();
      }
      delete result;
      if (save_protocol)
      {
        delete thd->protocol;
        thd->protocol= save_protocol;
      }
      if (!res && (explain || lex->analyze_stmt))
      {
        bool extended= thd->lex->describe & DESCRIBE_EXTENDED;
        res= thd->lex->explain->send_explain(thd, extended);
      }

      /* revert changes for SP */
      MYSQL_INSERT_SELECT_DONE(res, (ulong) thd->get_row_count_func());
      select_lex->table_list.first= first_table;

      status_var_add(thd->status_var.rows_sent, thd->get_sent_row_count());
    }
    /*
      If we have inserted into a VIEW, and the base table has
      AUTO_INCREMENT column, but this column is not accessible through
      a view, then we should restore LAST_INSERT_ID to the value it
      had before the statement.
    */
    if (first_table->view && !first_table->contain_auto_increment)
      thd->first_successful_insert_id_in_cur_stmt=
        thd->first_successful_insert_id_in_prev_stmt;

    break;
  }
  case SQLCOM_DROP_SEQUENCE:
  case SQLCOM_DROP_TABLE:
  {
    int result;
    DBUG_ASSERT(first_table == all_tables && first_table != 0);

    thd->open_options|= HA_OPEN_FOR_REPAIR;
    result= thd->open_temporary_tables(all_tables);
    thd->open_options&= ~HA_OPEN_FOR_REPAIR;
    if (result)
      goto error;
    if (!lex->tmp_table())
    {
      if (check_table_access(thd, DROP_ACL, all_tables, FALSE, UINT_MAX, FALSE))
	goto error;				/* purecov: inspected */
    }
    else
    {
      if (thd->transaction->xid_state.check_has_uncommitted_xa())
        goto error;
      status_var_decrement(thd->status_var.com_stat[lex->sql_command]);
      status_var_increment(thd->status_var.com_drop_tmp_table);

      /* So that DROP TEMPORARY TABLE gets to binlog at commit/rollback */
      thd->variables.option_bits|= OPTION_BINLOG_THIS_TRX;
    }
    /*
      If we are a slave, we should add IF EXISTS if the query executed
      on the master without an error. This will help a slave to
      recover from multi-table DROP TABLE that was aborted in the
      middle.
    */
    if ((thd->slave_thread && !thd->slave_expected_error &&
         slave_ddl_exec_mode_options == SLAVE_EXEC_MODE_IDEMPOTENT) ||
        thd->variables.option_bits & OPTION_IF_EXISTS)
      lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);

#ifdef WITH_WSREP
    if (WSREP(thd) && !lex->tmp_table() && wsrep_thd_is_local(thd) &&
        (!thd->is_current_stmt_binlog_format_row() ||
         wsrep_table_list_has_non_temp_tables(thd, all_tables)))
    {
      wsrep::key_array keys;
      if (wsrep_append_fk_parent_table(thd, all_tables, &keys))
      {
        goto wsrep_error_label;
      }
      if (wsrep_to_isolation_begin(thd, NULL, NULL, all_tables, NULL, &keys))
      {
        goto wsrep_error_label;
      }
    }
#endif /* WITH_WSREP */

    /* DDL and binlog write order are protected by metadata locks. */
    res= mysql_rm_table(thd, first_table, lex->if_exists(), lex->tmp_table(),
                        lex->table_type == TABLE_TYPE_SEQUENCE, 0);

    /*
      When dropping temporary tables if @@session_track_state_change is ON
      then send the boolean tracker in the OK packet
    */
    if(!res && (lex->create_info.options & HA_LEX_CREATE_TMP_TABLE))
    {
      thd->session_tracker.state_change.mark_as_changed(thd);
    }
    break;
  }
  case SQLCOM_SHOW_PROCESSLIST:
    if (!thd->security_ctx->priv_user[0] &&
        check_global_access(thd, PRIV_STMT_SHOW_PROCESSLIST))
      break;
    mysqld_list_processes(thd,
                (thd->security_ctx->master_access & PRIV_STMT_SHOW_PROCESSLIST ?
                 NullS :
                 thd->security_ctx->priv_user),
                lex->verbose);
    break;
  case SQLCOM_SHOW_AUTHORS:
    res= mysqld_show_authors(thd);
    break;
  case SQLCOM_SHOW_CONTRIBUTORS:
    res= mysqld_show_contributors(thd);
    break;
  case SQLCOM_SHOW_PRIVILEGES:
    res= mysqld_show_privileges(thd);
    break;
  case SQLCOM_SHOW_ENGINE_LOGS:
#ifdef DONT_ALLOW_SHOW_COMMANDS
    my_message(ER_NOT_ALLOWED_COMMAND, ER_THD(thd, ER_NOT_ALLOWED_COMMAND),
               MYF(0));	/* purecov: inspected */
    goto error;
#else
    {
      if (check_access(thd, FILE_ACL, any_db.str, NULL, NULL, 0, 0))
	goto error;
      res= ha_show_status(thd, lex->create_info.db_type, HA_ENGINE_LOGS);
      break;
    }
#endif
  case SQLCOM_CHANGE_DB:
  {
    if (!mysql_change_db(thd, &select_lex->db, FALSE))
      my_ok(thd);

    break;
  }

  case SQLCOM_LOAD:
  {
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    privilege_t privilege= (lex->duplicates == DUP_REPLACE ?
                            INSERT_ACL | DELETE_ACL : INSERT_ACL) |
                           (lex->local_file ? NO_ACL : FILE_ACL);

    if (lex->local_file)
    {
      if (!(thd->client_capabilities & CLIENT_LOCAL_FILES) ||
          !opt_local_infile)
      {
        my_message(ER_LOAD_INFILE_CAPABILITY_DISABLED, 
                   ER_THD(thd, ER_LOAD_INFILE_CAPABILITY_DISABLED), MYF(0));
        goto error;
      }
    }

    if (check_one_table_access(thd, privilege, all_tables))
      goto error;

    res= mysql_load(thd, lex->exchange, first_table, lex->field_list,
                    lex->update_list, lex->value_list, lex->duplicates,
                    lex->ignore, (bool) lex->local_file);
    break;
  }

  case SQLCOM_SET_OPTION:
  {
    List<set_var_base> *lex_var_list= &lex->var_list;

    if ((check_table_access(thd, SELECT_ACL, all_tables, FALSE, UINT_MAX, FALSE)
         || open_and_lock_tables(thd, all_tables, TRUE, 0)))
      goto error;
    if (likely(!(res= sql_set_variables(thd, lex_var_list, true))))
    {
      if (likely(!thd->is_error()))
        my_ok(thd);
    }
    else
    {
      /*
        We encountered some sort of error, but no message was sent.
        Send something semi-generic here since we don't know which
        assignment in the list caused the error.
      */
      if (!thd->is_error())
        my_error(ER_WRONG_ARGUMENTS,MYF(0),"SET");
      goto error;
    }

    break;
  }

  case SQLCOM_UNLOCK_TABLES:
    /*
      It is critical for mysqldump --single-transaction --master-data that
      UNLOCK TABLES does not implicitely commit a connection which has only
      done FLUSH TABLES WITH READ LOCK + BEGIN. If this assumption becomes
      false, mysqldump will not work.
    */
    if (thd->variables.option_bits & OPTION_TABLE_LOCK)
    {
      res= trans_commit_implicit(thd);
      if (thd->locked_tables_list.unlock_locked_tables(thd))
        res= 1;
      thd->release_transactional_locks();
      thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
      thd->reset_binlog_for_next_statement();
    }
    if (thd->global_read_lock.is_acquired() &&
        thd->current_backup_stage == BACKUP_FINISHED)
      thd->global_read_lock.unlock_global_read_lock(thd);
    if (res)
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_LOCK_TABLES:
    /* We must end the transaction first, regardless of anything */
    res= trans_commit_implicit(thd);
    if (thd->locked_tables_list.unlock_locked_tables(thd))
      res= 1;
    /* Release transactional metadata locks. */
    thd->release_transactional_locks();
    if (res)
      goto error;

#ifdef WITH_WSREP
    /* Clean up the previous transaction on implicit commit. */
    if (wsrep_on(thd) && !wsrep_not_committed(thd) && wsrep_after_statement(thd))
      goto error;
#endif

    /* We can't have any kind of table locks while backup is active */
    if (thd->current_backup_stage != BACKUP_FINISHED)
    {
      my_error(ER_BACKUP_LOCK_IS_ACTIVE, MYF(0));
      goto error;
    }

    /* Should not lock tables while BACKUP LOCK is active */
    if (thd->mdl_backup_lock)
    {
      my_error(ER_LOCK_OR_ACTIVE_TRANSACTION, MYF(0));
      goto error;
    }

    /*
      Here we have to pre-open temporary tables for LOCK TABLES.

      CF_PREOPEN_TMP_TABLES is not set for this SQL statement simply
      because LOCK TABLES calls close_thread_tables() as a first thing
      (it's called from unlock_locked_tables() above). So even if
      CF_PREOPEN_TMP_TABLES was set and the tables would be pre-opened
      in a usual way, they would have been closed.
    */
    if (thd->open_temporary_tables(all_tables))
      goto error;

    if (lock_tables_precheck(thd, all_tables))
      goto error;

    thd->variables.option_bits|= OPTION_TABLE_LOCK;

    res= lock_tables_open_and_lock_tables(thd, all_tables);

    if (res)
    {
      thd->variables.option_bits&= ~(OPTION_TABLE_LOCK);
    }
    else
    {
      if (thd->variables.query_cache_wlock_invalidate)
	query_cache_invalidate_locked_for_write(thd, first_table);
      my_ok(thd);
    }
    break;
  case SQLCOM_BACKUP:
    if (check_global_access(thd, RELOAD_ACL))
      goto error;
    if (!(res= run_backup_stage(thd, lex->backup_stage)))
      my_ok(thd);
    break;
  case SQLCOM_BACKUP_LOCK:
    if (check_global_access(thd, RELOAD_ACL, true))
    {
#ifndef NO_EMBEDDED_ACCESS_CHECKS
      /*
        In case there is no global privilege, check DB privilege for LOCK TABLES.
      */
      if (first_table) // BACKUP LOCK
      {
        if (check_single_table_access(thd, LOCK_TABLES_ACL, first_table, true))
        {
          char command[30];
          get_privilege_desc(command, sizeof(command), RELOAD_ACL|LOCK_TABLES_ACL);
          my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), command);
          goto error;
        }
      }
      else // BACKUP UNLOCK
      {
        /*
          We test mdl_backup_lock here because, if a user could obtain a lock
          it would be silly to error and say `you can't BACKUP UNLOCK`
          (because its obvious you did a `BACKUP LOCK`).
          As `BACKUP UNLOCK` doesn't have a database reference,
          there's no way we can check if the `BACKUP LOCK` privilege is missing.
          Testing `thd->db` would involve faking a `TABLE_LIST` structure,
          which because of the depth of inspection
          in `check_single_table_access` makes the faking likely to cause crashes,
          or unintended effects. The outcome of this is,
          if a user does an `BACKUP UNLOCK` without a `BACKUP LOCKED` table,
          there may be a` ER_SPECIFIC_ACCESS_DENIED` error even though
          user has the privilege.
          Its a bit different to what happens if the user has RELOAD_ACL,
          where the error is silently ignored.
        */
        if (!thd->mdl_backup_lock)
        {

          char command[30];
          get_privilege_desc(command, sizeof(command), RELOAD_ACL|LOCK_TABLES_ACL);
          my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), command);
          goto error;
        }
      }
#endif
    }
    /*
      There is reload privilege, first table is set for lock.
      For unlock the list is empty
    */
    if (first_table)
      res= backup_lock(thd, first_table);
    else
      backup_unlock(thd);
    if (!res)
      my_ok(thd);
    break;
  case SQLCOM_CREATE_DB:
  {
    const DBNameBuffer dbbuf(lex->name, lower_case_table_names == 1);
    const Lex_ident_db db= dbbuf.to_lex_ident_db_with_error();

    if (!db.str ||
        prepare_db_action(thd, lex->create_info.or_replace() ?
                          (CREATE_ACL | DROP_ACL) : CREATE_ACL,
                          db) ||
        (res= lex->create_info.resolve_to_charset_collation_context(thd,
                                 thd->charset_collation_context_create_db())))
      break;

    WSREP_TO_ISOLATION_BEGIN(db.str, NULL, NULL);

    res= mysql_create_db(thd, db, lex->create_info, &lex->create_info);
    break;
  }
  case SQLCOM_DROP_DB:
  {
    if (thd->variables.option_bits & OPTION_IF_EXISTS)
      lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);

    const DBNameBuffer dbbuf(lex->name, lower_case_table_names == 1);
    const Lex_ident_db db= dbbuf.to_lex_ident_db_with_error();

    if (!db.str || prepare_db_action(thd, DROP_ACL, db))
      break;

    WSREP_TO_ISOLATION_BEGIN(db.str, NULL, NULL);

    res= mysql_rm_db(thd, db, lex->if_exists());
    break;
  }
  case SQLCOM_ALTER_DB_UPGRADE:
  {
    const DBNameBuffer dbbuf(lex->name, lower_case_table_names == 1);
    const Lex_ident_db db= dbbuf.to_lex_ident_db_with_error();

    if (!db.str ||
        thd->check_slave_ignored_db_with_error(db) ||
        check_access(thd, ALTER_ACL, db.str, NULL, NULL, 1, 0) ||
        check_access(thd, DROP_ACL, db.str, NULL, NULL, 1, 0) ||
        check_access(thd, CREATE_ACL, db.str, NULL, NULL, 1, 0))
    {
      res= 1;
      break;
    }

    WSREP_TO_ISOLATION_BEGIN(db.str, NULL, NULL);

    res= mysql_upgrade_db(thd, db);
    if (!res)
      my_ok(thd);
    break;
  }
  case SQLCOM_ALTER_DB:
  {
    const DBNameBuffer dbbuf(lex->name, lower_case_table_names == 1);
    const Lex_ident_db db= dbbuf.to_lex_ident_db_with_error();

    if (!db.str ||
        prepare_db_action(thd, ALTER_ACL, db) ||
        (res= lex->create_info.resolve_to_charset_collation_context(thd,
                     thd->charset_collation_context_alter_db(db.str))))
      break;

    WSREP_TO_ISOLATION_BEGIN(db.str, NULL, NULL);

    res= mysql_alter_db(thd, db, &lex->create_info);
    break;
  }
  case SQLCOM_SHOW_CREATE_DB:
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
    res= show_create_db(thd, lex);
    break;
  case SQLCOM_SHOW_CREATE_SERVER:
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
    res= mysql_show_create_server(thd, &lex->name);
    break;
  case SQLCOM_CREATE_EVENT:
  case SQLCOM_ALTER_EVENT:
  #ifdef HAVE_EVENT_SCHEDULER
  do
  {
    DBUG_ASSERT(lex->event_parse_data);
    if (lex->table_or_sp_used())
    {
      my_error(ER_SUBQUERIES_NOT_SUPPORTED, MYF(0), "CREATE/ALTER EVENT");
      break;
    }

    res= sp_process_definer(thd);
    if (res)
      break;

    switch (lex->sql_command) {
    case SQLCOM_CREATE_EVENT:
    {
      res= Events::create_event(thd, lex->event_parse_data);
      break;
    }
    case SQLCOM_ALTER_EVENT:
      res= Events::update_event(thd, lex->event_parse_data,
                                lex->spname ? &lex->spname->m_db : NULL,
                                lex->spname ? &lex->spname->m_name : NULL);
      break;
    default:
      DBUG_ASSERT(0);
    }
    DBUG_PRINT("info",("DDL error code=%d", res));
    if (!res)
      my_ok(thd);

  } while (0);
  /* Don't do it, if we are inside a SP */
  if (!thd->spcont && !is_called_from_prepared_stmt)
  {
    sp_head::destroy(lex->sphead);
    lex->sphead= NULL;
  }
  /* lex->unit.cleanup() is called outside, no need to call it here */
  break;
  case SQLCOM_SHOW_CREATE_EVENT:
    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
    res= Events::show_create_event(thd, &lex->spname->m_db,
                                   &lex->spname->m_name);
    break;
  case SQLCOM_DROP_EVENT:
    if (!(res= Events::drop_event(thd,
                                  &lex->spname->m_db, &lex->spname->m_name,
                                  lex->if_exists())))
      my_ok(thd);
    break;
#else
    my_error(ER_NOT_SUPPORTED_YET,MYF(0),"embedded server");
    break;
#endif
  case SQLCOM_CREATE_FUNCTION:                  // UDF function
  {
    if (check_access(thd, lex->create_info.or_replace() ?
                          (INSERT_ACL | DELETE_ACL) : INSERT_ACL,
                     "mysql", NULL, NULL, 1, 0))
      break;
#ifdef HAVE_DLOPEN
    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    if (!(res = mysql_create_function(thd, &lex->udf)))
      my_ok(thd);
#else
    my_error(ER_CANT_OPEN_LIBRARY, MYF(0), lex->udf.dl, 0, "feature disabled");
    res= TRUE;
#endif
    break;
  }
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  case SQLCOM_CREATE_USER:
  case SQLCOM_CREATE_ROLE:
  {
    if (check_access(thd, lex->create_info.or_replace() ?
                          INSERT_ACL | DELETE_ACL : INSERT_ACL,
                     "mysql", NULL, NULL, 1, 1) &&
        check_global_access(thd,CREATE_USER_ACL))
      break;

    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    /* Conditionally writes to binlog */
    if (!(res= mysql_create_user(thd, lex->users_list,
                                 lex->sql_command == SQLCOM_CREATE_ROLE)))
      my_ok(thd);
    break;
  }
  case SQLCOM_DROP_USER:
  case SQLCOM_DROP_ROLE:
  {
    if (check_access(thd, DELETE_ACL, "mysql", NULL, NULL, 1, 1) &&
        check_global_access(thd,CREATE_USER_ACL))
      break;

    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    /* Conditionally writes to binlog */
    if (!(res= mysql_drop_user(thd, lex->users_list,
                               lex->sql_command == SQLCOM_DROP_ROLE)))
      my_ok(thd);
    break;
  }
  case SQLCOM_ALTER_USER:
  case SQLCOM_RENAME_USER:
  {
    if (check_access(thd, UPDATE_ACL, "mysql", NULL, NULL, 1, 1) &&
        check_global_access(thd,CREATE_USER_ACL))
      break;

    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    /* Conditionally writes to binlog */
    if (lex->sql_command == SQLCOM_ALTER_USER)
      res= mysql_alter_user(thd, lex->users_list);
    else
      res= mysql_rename_user(thd, lex->users_list);
    if (!res)
      my_ok(thd);
    break;
  }
  case SQLCOM_REVOKE_ALL:
  {
    if (check_access(thd, UPDATE_ACL, "mysql", NULL, NULL, 1, 1) &&
        check_global_access(thd,CREATE_USER_ACL))
      break;

    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    /* Conditionally writes to binlog */
    if (!(res = mysql_revoke_all(thd, lex->users_list)))
      my_ok(thd);
    break;
  }

  case SQLCOM_REVOKE_ROLE:
  case SQLCOM_GRANT_ROLE:
  {
    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    if (!(res= mysql_grant_role(thd, lex->users_list,
                                lex->sql_command != SQLCOM_GRANT_ROLE)))
      my_ok(thd);
    break;
  }
#endif /*!NO_EMBEDDED_ACCESS_CHECKS*/
  case SQLCOM_RESET:
    /*
      RESET commands are never written to the binary log, so we have to
      initialize this variable because RESET shares the same code as FLUSH
    */
    lex->no_write_to_binlog= 1;
    /* fall through */
  case SQLCOM_FLUSH:
  {
    int write_to_binlog;
    if ((lex->type & ~REFRESH_SESSION_STATUS) &&
        check_global_access(thd,RELOAD_ACL))
      goto error;

    if (first_table && lex->type & (REFRESH_READ_LOCK|REFRESH_FOR_EXPORT))
    {
      /* Check table-level privileges. */
      if (check_table_access(thd, PRIV_LOCK_TABLES, all_tables,
                             FALSE, UINT_MAX, FALSE))
        goto error;

      if (flush_tables_with_read_lock(thd, all_tables))
        goto error;

      my_ok(thd);
      break;
    }

#ifdef WITH_WSREP
    if (lex->type & (
    REFRESH_GRANT                           |
    REFRESH_HOSTS                           |
#ifdef HAVE_OPENSSL
    REFRESH_DES_KEY_FILE                    |
#endif
    /*
      Write all flush log statements except
      FLUSH LOGS
      FLUSH BINARY LOGS
      Check reload_acl_and_cache for why.
    */
    REFRESH_RELAY_LOG                       |
    REFRESH_SLOW_LOG                        |
    REFRESH_GENERAL_LOG                     |
    REFRESH_ENGINE_LOG                      |
    REFRESH_ERROR_LOG                       |
    REFRESH_QUERY_CACHE_FREE                |
    REFRESH_STATUS                          |
    REFRESH_SESSION_STATUS                  |
    REFRESH_GLOBAL_STATUS                   |
    REFRESH_USER_RESOURCES))
    {
      WSREP_TO_ISOLATION_BEGIN_WRTCHK(WSREP_MYSQL_DB, NULL, NULL);
    }
#endif /* WITH_WSREP*/

#ifdef HAVE_REPLICATION
    if (lex->type & REFRESH_READ_LOCK)
    {
      /*
        We need to pause any parallel replication slave workers during FLUSH
        TABLES WITH READ LOCK. Otherwise we might cause a deadlock, as
        worker threads eun run in arbitrary order but need to commit in a
        specific given order.
      */
      if (rpl_pause_for_ftwrl(thd))
        goto error;
    }
#endif
    /*
      reload_acl_and_cache() will tell us if we are allowed to write to the
      binlog or not.
    */
    if (!reload_acl_and_cache(thd, lex->type, first_table, &write_to_binlog))
    {
#ifdef WITH_WSREP
      if ((lex->type & REFRESH_TABLES) && !(lex->type & (REFRESH_FOR_EXPORT|REFRESH_READ_LOCK)))
      {
        /*
          This is done after reload_acl_and_cache is because
          LOCK TABLES is not replicated in galera, the upgrade of which
          is checked in reload_acl_and_cache.
          Hence, done after/if we are able to upgrade locks.
        */
        if (first_table)
        {
          WSREP_TO_ISOLATION_BEGIN_WRTCHK(NULL, NULL, first_table);
        }
        else
        {
          WSREP_TO_ISOLATION_BEGIN_WRTCHK(WSREP_MYSQL_DB, NULL, NULL);
        }
      }
#endif /* WITH_WSREP */
      /*
        We WANT to write and we CAN write.
        ! we write after unlocking the table.
      */
      /*
        Presumably, RESET and binlog writing doesn't require synchronization
      */

      if (write_to_binlog > 0)  // we should write
      { 
        if (!lex->no_write_to_binlog)
          res= write_bin_log(thd, FALSE, thd->query(), thd->query_length());
      } else if (write_to_binlog < 0) 
      {
        /* 
           We should not write, but rather report error because 
           reload_acl_and_cache binlog interactions failed 
         */
        res= 1;
      }

      if (!res)
        my_ok(thd);
    } 
    else
      res= 1;                                   // reload_acl_and_cache failed
#ifdef HAVE_REPLICATION
    if (lex->type & REFRESH_READ_LOCK)
      rpl_unpause_after_ftwrl(thd);
#endif
    
    break;
  }
  case SQLCOM_KILL:
  {
    if (lex->table_or_sp_used())
    {
      my_error(ER_SUBQUERIES_NOT_SUPPORTED, MYF(0), "KILL");
      break;
    }

    if (lex->kill_type == KILL_TYPE_ID || lex->kill_type == KILL_TYPE_QUERY)
    {
      Item *it= (Item *)lex->value_list.head();
      if (it->fix_fields_if_needed_for_scalar(lex->thd, &it))
      {
        my_message(ER_SET_CONSTANTS_ONLY, ER_THD(thd, ER_SET_CONSTANTS_ONLY),
                   MYF(0));
        goto error;
      }
      sql_kill(thd, (my_thread_id) it->val_int(), lex->kill_signal, lex->kill_type);
    }
    else
      sql_kill_user(thd, get_current_user(thd, lex->users_list.head()),
                    lex->kill_signal);
    break;
  }
  case SQLCOM_SHUTDOWN:
#ifndef EMBEDDED_LIBRARY
    DBUG_EXECUTE_IF("crash_shutdown", DBUG_SUICIDE(););
    if (check_global_access(thd,SHUTDOWN_ACL))
      goto error;
    kill_mysql(thd);
    my_ok(thd);
#else
    my_error(ER_NOT_SUPPORTED_YET, MYF(0), "embedded server");
#endif
    break;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  case SQLCOM_SHOW_CREATE_USER:
  {
    LEX_USER *grant_user= lex->grant_user;
    if (!grant_user)
      goto error;

    res = mysql_show_create_user(thd, grant_user);
    break;
  }
  case SQLCOM_SHOW_GRANTS:
  {
    LEX_USER *grant_user= lex->grant_user;
    if (!grant_user)
      goto error;

    WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
    res = mysql_show_grants(thd, grant_user);
    break;
  }
#endif
  case SQLCOM_HA_OPEN:
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    if (check_table_access(thd, SELECT_ACL, all_tables, FALSE, UINT_MAX, FALSE))
      goto error;
    /* Close temporary tables which were pre-opened for privilege checking. */
    close_thread_tables(thd);
    all_tables->table= NULL;
    res= mysql_ha_open(thd, first_table, 0);
    break;
  case SQLCOM_HA_CLOSE:
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    res= mysql_ha_close(thd, first_table);
    break;
  case SQLCOM_HA_READ:
    DBUG_ASSERT(first_table == all_tables && first_table != 0);
    /*
      There is no need to check for table permissions here, because
      if a user has no permissions to read a table, he won't be
      able to open it (with SQLCOM_HA_OPEN) in the first place.
    */
    unit->set_limit(select_lex);

    res= mysql_ha_read(thd, first_table, lex->ha_read_mode, lex->ident.str,
                       lex->insert_list, lex->ha_rkey_mode, select_lex->where,
                       unit->lim.get_select_limit(),
                       unit->lim.get_offset_limit());
    break;

  case SQLCOM_BEGIN:
    DBUG_PRINT("info", ("Executing SQLCOM_BEGIN  thd: %p", thd));
    if (trans_begin(thd, lex->start_transaction_opt))
    {
      thd->release_transactional_locks();
      WSREP_DEBUG("BEGIN failed, MDL released: %lld",
                  (longlong) thd->thread_id);
      WSREP_DEBUG("stmt_da, sql_errno: %d", (thd->get_stmt_da()->is_error()) ? thd->get_stmt_da()->sql_errno() : 0);
      goto error;
    }
    my_ok(thd);
    break;
  case SQLCOM_COMMIT:
  {
    DBUG_ASSERT(thd->lock == NULL ||
                thd->locked_tables_mode == LTM_LOCK_TABLES);
    bool tx_chain= (lex->tx_chain == TVL_YES ||
                    (thd->variables.completion_type == 1 &&
                     lex->tx_chain != TVL_NO));
    bool tx_release= (lex->tx_release == TVL_YES ||
                      (thd->variables.completion_type == 2 &&
                       lex->tx_release != TVL_NO));
    bool commit_failed= trans_commit(thd);
    thd->release_transactional_locks();
    if (commit_failed)
    {
      WSREP_DEBUG("COMMIT failed, MDL released: %lld",
                  (longlong) thd->thread_id);
      goto error;
    }
    /* Begin transaction with the same isolation level. */
    if (tx_chain)
    {
      if (trans_begin(thd))
        goto error;
    }
    else
    {
      /* Reset the isolation level and access mode if no chaining transaction.*/
      trans_reset_one_shot_chistics(thd);
    }
    /* Disconnect the current client connection. */
    if (tx_release)
    {
      thd->set_killed(KILL_CONNECTION);
      thd->print_aborted_warning(3, "RELEASE");
    }
    my_ok(thd);
    break;
  }
  case SQLCOM_ROLLBACK:
  {
    DBUG_ASSERT(thd->lock == NULL ||
                thd->locked_tables_mode == LTM_LOCK_TABLES);
    bool tx_chain= (lex->tx_chain == TVL_YES ||
                    (thd->variables.completion_type == 1 &&
                     lex->tx_chain != TVL_NO));
    bool tx_release= (lex->tx_release == TVL_YES ||
                      (thd->variables.completion_type == 2 &&
                       lex->tx_release != TVL_NO));
    bool rollback_failed= trans_rollback(thd);
    thd->release_transactional_locks();

    if (rollback_failed)
    {
      WSREP_DEBUG("rollback failed, MDL released: %lld",
                  (longlong) thd->thread_id);
      goto error;
    }
    /* Begin transaction with the same isolation level. */
    if (tx_chain)
    {
#ifdef WITH_WSREP
      /* If there are pending changes after rollback we should clear them */
      if (wsrep_on(thd) && wsrep_has_changes(thd))
        wsrep_after_statement(thd);
#endif
      if (trans_begin(thd))
        goto error;
    }
    else
    {
      /* Reset the isolation level and access mode if no chaining transaction.*/
      trans_reset_one_shot_chistics(thd);
    }
    /* Disconnect the current client connection. */
    if (tx_release)
      thd->set_killed(KILL_CONNECTION);
    my_ok(thd);
   break;
  }
  case SQLCOM_RELEASE_SAVEPOINT:
    if (trans_release_savepoint(thd, lex->ident))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_ROLLBACK_TO_SAVEPOINT:
    if (trans_rollback_to_savepoint(thd, lex->ident))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_SAVEPOINT:
    if (trans_savepoint(thd, lex->ident))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_CREATE_PROCEDURE:
  case SQLCOM_CREATE_SPFUNCTION:
  case SQLCOM_CREATE_PACKAGE:
  case SQLCOM_CREATE_PACKAGE_BODY:
  {
    if (mysql_create_routine(thd, lex))
      goto error;
    my_ok(thd);
    break; /* break super switch */
  } /* end case group bracket */
  case SQLCOM_COMPOUND:
  {
    sp_head *sp= lex->sphead;
    DBUG_ASSERT(all_tables == 0);
    DBUG_ASSERT(thd->in_sub_stmt == 0);
    sp->m_sql_mode= thd->variables.sql_mode;
    sp->m_sp_share= MYSQL_GET_SP_SHARE(sp->m_handler->type(),
                                       sp->m_db.str, static_cast<uint>(sp->m_db.length),
                                       sp->m_name.str, static_cast<uint>(sp->m_name.length));
    if (do_execute_sp(thd, lex->sphead))
      goto error;
    break;
  }

  case SQLCOM_ALTER_PROCEDURE:
  case SQLCOM_ALTER_FUNCTION:
    if (thd->variables.option_bits & OPTION_IF_EXISTS)
      lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);
    if (alter_routine(thd, lex))
      goto error;
    break;
  case SQLCOM_DROP_PROCEDURE:
  case SQLCOM_DROP_FUNCTION:
  case SQLCOM_DROP_PACKAGE:
  case SQLCOM_DROP_PACKAGE_BODY:
    if (thd->variables.option_bits & OPTION_IF_EXISTS)
      lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);
    if (drop_routine(thd, lex))
      goto error;
    break;
  case SQLCOM_SHOW_CREATE_PROC:
  case SQLCOM_SHOW_CREATE_FUNC:
  case SQLCOM_SHOW_CREATE_PACKAGE:
  case SQLCOM_SHOW_CREATE_PACKAGE_BODY:
    {
      WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
      const Sp_handler *sph= Sp_handler::handler(lex->sql_command);
      if (sph->sp_show_create_routine(thd, lex->spname))
        goto error;
      break;
    }
  case SQLCOM_SHOW_PROC_CODE:
  case SQLCOM_SHOW_FUNC_CODE:
  case SQLCOM_SHOW_PACKAGE_BODY_CODE:
    {
#ifndef DBUG_OFF
      Database_qualified_name pkgname;
      sp_head *sp;
      const Sp_handler *sph= Sp_handler::handler(lex->sql_command);
      WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
      if (sph->sp_resolve_package_routine(thd, thd->lex->sphead,
                                          lex->spname, &sph, &pkgname))
        return true;
      if (sph->sp_cache_routine(thd, lex->spname, &sp))
        goto error;
      if (!sp || sp->show_routine_code(thd))
      {
        /* We don't distinguish between errors for now */
        my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
                 sph->type_str(), lex->spname->m_name.str);
        goto error;
      }
      break;
#else
      my_error(ER_FEATURE_DISABLED, MYF(0),
               "SHOW PROCEDURE|FUNCTION CODE", "--with-debug");
      goto error;
#endif // ifndef DBUG_OFF
    }
  case SQLCOM_SHOW_CREATE_TRIGGER:
    {
      if (check_ident_length(&lex->spname->m_name))
        goto error;

      WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
      if (show_create_trigger(thd, lex->spname))
        goto error; /* Error has been already logged. */

      break;
    }
  case SQLCOM_CREATE_VIEW:
    {
      /*
        Note: SQLCOM_CREATE_VIEW also handles 'ALTER VIEW' commands
        as specified through the thd->lex->create_view->mode flag.
      */
      res= mysql_create_view(thd, first_table, thd->lex->create_view->mode);
      break;
    }
  case SQLCOM_DROP_VIEW:
    {
      if (check_table_access(thd, DROP_ACL, all_tables, FALSE, UINT_MAX, FALSE))
        goto error;

      WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

      if (thd->variables.option_bits & OPTION_IF_EXISTS)
        lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);

      /* Conditionally writes to binlog. */
      res= mysql_drop_view(thd, first_table, thd->lex->drop_mode);
      break;
    }
  case SQLCOM_CREATE_TRIGGER:
  {
    /* Conditionally writes to binlog. */
    res= mysql_create_or_drop_trigger(thd, all_tables, 1);

    break;
  }
  case SQLCOM_DROP_TRIGGER:
  {
    if (thd->variables.option_bits & OPTION_IF_EXISTS)
      lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);

    /* Conditionally writes to binlog. */
    res= mysql_create_or_drop_trigger(thd, all_tables, 0);
    break;
  }
  case SQLCOM_XA_START:
#ifdef WITH_WSREP
    if (WSREP_ON)
    {
      my_error(ER_NOT_SUPPORTED_YET, MYF(0),
               "XA transactions with Galera replication");
      break;
    }
#endif /* WITH_WSREP */
    if (trans_xa_start(thd))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_XA_END:
    if (trans_xa_end(thd))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_XA_PREPARE:
    if (trans_xa_prepare(thd))
      goto error;
    my_ok(thd);
    break;
  case SQLCOM_XA_COMMIT:
  {
    bool commit_failed= trans_xa_commit(thd);
    if (commit_failed)
    {
      WSREP_DEBUG("XA commit failed, MDL released: %lld",
                  (longlong) thd->thread_id);
      goto error;
    }
    /*
      We've just done a commit, reset transaction
      isolation level and access mode to the session default.
    */
    trans_reset_one_shot_chistics(thd);
    my_ok(thd);
    break;
  }
  case SQLCOM_XA_ROLLBACK:
  {
    bool rollback_failed= trans_xa_rollback(thd);
    if (rollback_failed)
    {
      WSREP_DEBUG("XA rollback failed, MDL released: %lld",
                  (longlong) thd->thread_id);
      goto error;
    }
    /*
      We've just done a rollback, reset transaction
      isolation level and access mode to the session default.
    */
    trans_reset_one_shot_chistics(thd);
    my_ok(thd);
    break;
  }
  case SQLCOM_XA_RECOVER:
    res= mysql_xa_recover(thd);
    break;
  case SQLCOM_INSTALL_PLUGIN:
    if (! (res= mysql_install_plugin(thd, &thd->lex->comment,
                                     &thd->lex->ident)))
      my_ok(thd);
    break;
  case SQLCOM_UNINSTALL_PLUGIN:
    if (! (res= mysql_uninstall_plugin(thd, &thd->lex->comment,
                                       &thd->lex->ident)))
      my_ok(thd);
    break;
  case SQLCOM_BINLOG_BASE64_EVENT:
  {
#ifndef EMBEDDED_LIBRARY
    mysql_client_binlog_statement(thd);
#else /* EMBEDDED_LIBRARY */
    my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "embedded");
#endif /* EMBEDDED_LIBRARY */
    break;
  }
  case SQLCOM_CREATE_SERVER:
  {
    DBUG_PRINT("info", ("case SQLCOM_CREATE_SERVER"));

    if (check_global_access(thd, PRIV_STMT_CREATE_SERVER))
      break;

    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    res= create_server(thd, &lex->server_options);
    break;
  }
  case SQLCOM_ALTER_SERVER:
  {
    int error;
    DBUG_PRINT("info", ("case SQLCOM_ALTER_SERVER"));

    if (check_global_access(thd, PRIV_STMT_ALTER_SERVER))
      break;

    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    if (unlikely((error= alter_server(thd, &lex->server_options))))
    {
      DBUG_PRINT("info", ("problem altering server <%s>",
                          lex->server_options.server_name.str));
      my_error(error, MYF(0), lex->server_options.server_name.str);
      break;
    }
    my_ok(thd, 1);
    break;
  }
  case SQLCOM_DROP_SERVER:
  {
    int err_code;
    DBUG_PRINT("info", ("case SQLCOM_DROP_SERVER"));

    if (check_global_access(thd, PRIV_STMT_DROP_SERVER))
      break;

    WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

    if ((err_code= drop_server(thd, &lex->server_options)))
    {
      if (! lex->if_exists() && err_code == ER_FOREIGN_SERVER_DOESNT_EXIST)
      {
        DBUG_PRINT("info", ("problem dropping server %s",
                            lex->server_options.server_name.str));
        my_error(err_code, MYF(0), lex->server_options.server_name.str);
      }
      else
      {
        my_ok(thd, 0);
      }
      break;
    }
    my_ok(thd, 1);
    break;
  }
  case SQLCOM_ANALYZE:
  case SQLCOM_CHECK:
  case SQLCOM_OPTIMIZE:
  case SQLCOM_REPAIR:
  case SQLCOM_TRUNCATE:
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_CREATE_SEQUENCE:
  case SQLCOM_ALTER_TABLE:
      DBUG_ASSERT(first_table == all_tables && first_table != 0);
    /* fall through */
  case SQLCOM_ALTER_SEQUENCE:
  case SQLCOM_SIGNAL:
  case SQLCOM_RESIGNAL:
  case SQLCOM_GET_DIAGNOSTICS:
  case SQLCOM_CALL:
  case SQLCOM_REVOKE:
  case SQLCOM_GRANT:
    if (thd->variables.option_bits & OPTION_IF_EXISTS)
      lex->create_info.set(DDL_options_st::OPT_IF_EXISTS);
    DBUG_ASSERT(lex->m_sql_cmd != NULL);
    res= lex->m_sql_cmd->execute(thd);
    DBUG_PRINT("result", ("res: %d  killed: %d  is_error(): %d",
                          res, thd->killed, thd->is_error()));
    break;
  default:

#ifndef EMBEDDED_LIBRARY
    DBUG_ASSERT(0);                             /* Impossible */
#endif
    my_ok(thd);
    break;
  }
  THD_STAGE_INFO(thd, stage_query_end);
  thd->update_stats();

  goto finish;

error:
#ifdef WITH_WSREP
wsrep_error_label:
#endif
  res= true;

finish:
  if (!thd->is_error() && !res)
    res= store_table_definitions_in_trace(thd);

  thd->reset_query_timer();
  DBUG_ASSERT(!thd->in_active_multi_stmt_transaction() ||
               thd->in_multi_stmt_transaction_mode());

  lex->unit.cleanup();

  /* close/reopen tables that were marked to need reopen under LOCK TABLES */
  if (unlikely(thd->locked_tables_list.some_table_marked_for_reopen) &&
      !thd->lex->requires_prelocking())
    thd->locked_tables_list.reopen_tables(thd, true);

  if (! thd->in_sub_stmt)
  {
    if (thd->killed != NOT_KILLED)
    {
      /* report error issued during command execution */
      if (thd->killed_errno())
      {
        /* If we already sent 'ok', we can ignore any kill query statements */
        if (! thd->get_stmt_da()->is_set())
          thd->send_kill_message();
      }
      thd->reset_kill_query();
    }

    /*
      Binary logging is now done. Unset the "used" flags to avoid
      flags leaking to the next event (and to the COMMIT statement
      in the end of the current event).

      Example:

      Suppose a non-default collation (in @@character_set_collations)
      was used during the statement, the mysqlbinlog output for
      the current statement will contain a sequence like this:

        SET character_set_collations='utf8mb3=utf8mb3_bin';
        INSERT INTO t1 VALUES (_utf8mb3'test');
        COMMIT;

      The statement (INSERT in this example) is already in binlog at this point, and the
      and the "SET character_set_collations" is written inside a
      Q_CHARACTER_SET_COLLATIONS chunk in its log entry header.
      The flag CHARACTER_SET_COLLATIONS_USED is not needed any more.
      The COMMIT can be printed without "SET character_set_collations".

      The same logic applies to the other _USED flags.
    */
    thd->used= 0;

    if (unlikely(thd->is_error()) ||
        (thd->variables.option_bits & OPTION_MASTER_SQL_ERROR))
    {
      trans_rollback_stmt(thd);
    }
    else
    {
      /* If commit fails, we should be able to reset the OK status. */
      thd->get_stmt_da()->set_overwrite_status(true);
      trans_commit_stmt(thd);
      thd->get_stmt_da()->set_overwrite_status(false);
    }
  }

  /* Free tables. Set stage 'closing tables' */
  close_thread_tables_for_query(thd);

#ifndef DBUG_OFF
  if (lex->sql_command != SQLCOM_SET_OPTION && ! thd->in_sub_stmt)
    DEBUG_SYNC(thd, "execute_command_after_close_tables");
#endif
  if (!(sql_command_flags[lex->sql_command] &
        (CF_CAN_GENERATE_ROW_EVENTS | CF_FORCE_ORIGINAL_BINLOG_FORMAT |
         CF_STATUS_COMMAND)))
    thd->set_binlog_format(orig_binlog_format,
                           orig_current_stmt_binlog_format);

  if (! thd->in_sub_stmt && thd->transaction_rollback_request)
  {
    /*
      We are not in sub-statement and transaction rollback was requested by
      one of storage engines (e.g. due to deadlock). Rollback transaction in
      all storage engines including binary log.
    */
    trans_rollback_implicit(thd);
    thd->release_transactional_locks();
  }
  else if (stmt_causes_implicit_commit(thd, CF_IMPLICIT_COMMIT_END))
  {
    /* No transaction control allowed in sub-statements. */
    DBUG_ASSERT(! thd->in_sub_stmt);
    if (!(thd->variables.option_bits & OPTION_GTID_BEGIN))
    {
      /* If commit fails, we should be able to reset the OK status. */
      thd->get_stmt_da()->set_overwrite_status(true);
      /* Commit the normal transaction if one is active. */
      trans_commit_implicit(thd);
      thd->get_stmt_da()->set_overwrite_status(false);
      thd->release_transactional_locks();
    }
  }
  else if (! thd->in_sub_stmt && ! thd->in_active_multi_stmt_transaction())
  {
    /*
      - If inside a multi-statement transaction,
      defer the release of metadata locks until the current
      transaction is either committed or rolled back. This prevents
      other statements from modifying the table for the entire
      duration of this transaction.  This provides commit ordering
      and guarantees serializability across multiple transactions.
      - If in autocommit mode, or outside a transactional context,
      automatically release metadata locks of the current statement.
    */
    thd->release_transactional_locks();
  }
  else if (! thd->in_sub_stmt)
  {
    thd->mdl_context.release_statement_locks();
  }

  THD_STAGE_INFO(thd, stage_starting_cleanup);

  TRANSACT_TRACKER(add_trx_state_from_thd(thd));

#ifdef WITH_WSREP
  thd->wsrep_consistency_check= NO_CONSISTENCY_CHECK;

  if (wsrep_thd_is_toi(thd) || wsrep_thd_is_in_rsu(thd))
  {
    WSREP_DEBUG("mysql_execute_command for %s", wsrep_thd_query(thd));
    THD_STAGE_INFO(thd, stage_waiting_isolation);
    wsrep_to_isolation_end(thd);
  }

  /*
    Force release of transactional locks if not in active MST and wsrep is on.
  */
  if (WSREP(thd) &&
      ! thd->in_sub_stmt &&
      ! thd->in_active_multi_stmt_transaction() &&
      thd->mdl_context.has_transactional_locks())
  {
    WSREP_DEBUG("Forcing release of transactional locks for thd: %lld",
                (longlong) thd->thread_id);
    thd->release_transactional_locks();
  }

  /*
    Current command did not start multi STMT transaction and the command
    did not cause commit to happen (e.g. read only). Commit the wsrep
    transaction as empty.
  */
  if (!thd->in_active_multi_stmt_transaction() &&
      !thd->in_sub_stmt &&
      thd->wsrep_trx().active() &&
      thd->wsrep_trx().state() == wsrep::transaction::s_executing)
  {
    wsrep_commit_empty(thd, true);
  }

  /* assume PA safety for next transaction */
  thd->wsrep_PA_safe= true;
#endif /* WITH_WSREP */

  /*
    Reset the connection_name to contain a null string, if the
    pointer points to the same space as that of the system variable
    default_master_connection.

    We do this because the system variable may be updated which could
    free the pointer and create a new one, causing use-after-free for
    re-execution of prepared statements and stored procedures where
    the LEX may be reused.

    This allows connection_name to be set again be to the system
    variable pointer in the next call of this function (see earlier in
    this function), after any possible updates to the system variable.
  */
  if (thd->lex->mi.connection_name.str ==
      thd->variables.default_master_connection.str)
    thd->lex->mi.connection_name= null_clex_str;

  if (lex->sql_command != SQLCOM_SET_OPTION)
    DEBUG_SYNC(thd, "end_of_statement");
  DBUG_RETURN(res || thd->is_error());
 }

static bool execute_sqlcom_select(THD *thd, TABLE_LIST *all_tables)
{
  LEX	*lex= thd->lex;
  select_result *result=lex->result;
  bool res;
  /* assign global limit variable if limit is not given */
  {
    SELECT_LEX *param= lex->unit.global_parameters();
    if (!param->limit_params.explicit_limit)
      param->limit_params.select_limit=
        new (thd->mem_root) Item_int(thd,
                                     (ulonglong) thd->variables.select_limit);
  }

  if (!(res= open_and_lock_tables(thd, all_tables, TRUE, 0)))
  {
    if (lex->describe)
    {
      /*
        We always use select_send for EXPLAIN, even if it's an EXPLAIN
        for SELECT ... INTO OUTFILE: a user application should be able
        to prepend EXPLAIN to any query and receive output for it,
        even if the query itself redirects the output.
      */
      if (unlikely(!(result= new (thd->mem_root) select_send(thd))))
        return 1;                               /* purecov: inspected */
      thd->send_explain_fields(result, lex->describe, lex->analyze_stmt);
        
      /*
        This will call optimize() for all parts of query. The query plan is
        printed out below.
      */
      res= mysql_explain_union(thd, &lex->unit, result);
      
      /* Print EXPLAIN only if we don't have an error */
      if (likely(!res))
      {
        /* 
          Do like the original select_describe did: remove OFFSET from the
          top-level LIMIT
        */
        result->remove_offset_limit();
        if (lex->explain_json)
        {
          lex->explain->print_explain_json(result, lex->analyze_stmt);
        }
        else
        {
          lex->explain->print_explain(result, thd->lex->describe,
                                      thd->lex->analyze_stmt);
          if (lex->describe & DESCRIBE_EXTENDED)
          {
            char buff[1024];
            String str(buff,(uint32) sizeof(buff), system_charset_info);
            str.length(0);
            /*
              The warnings system requires input in utf8, @see
              mysqld_show_warnings().
            */
            lex->unit.print(&str, QT_EXPLAIN_EXTENDED);
            push_warning(thd, Sql_condition::WARN_LEVEL_NOTE,
                         ER_YES, str.c_ptr_safe());
          }
        }
      }

      if (res)
        result->abort_result_set();
      else
        result->send_eof();
      delete result;
    }
    else
    {
      Protocol *save_protocol= NULL;
      if (lex->analyze_stmt)
      {
        if (result && result->result_interceptor())
          result->result_interceptor()->disable_my_ok_calls();
        else 
        {
          DBUG_ASSERT(thd->protocol);
          result= new (thd->mem_root) select_send_analyze(thd);
          save_protocol= thd->protocol;
          thd->protocol= new Protocol_discard(thd);
        }
      }
      else
      {
        if (!result && !(result= new (thd->mem_root) select_send(thd)))
          return 1;                               /* purecov: inspected */
      }
      query_cache_store_query(thd, all_tables);
      res= handle_select(thd, lex, result, 0);
      if (result != lex->result)
        delete result;

      if (lex->analyze_stmt)
      {
        if (save_protocol)
        {
          delete thd->protocol;
          thd->protocol= save_protocol;
        }
        if (!res)
	{
          bool extended= thd->lex->describe & DESCRIBE_EXTENDED;
          res= thd->lex->explain->send_explain(thd, extended);
        }
      }
    }
  }
  /* Count number of empty select queries */
  if (!thd->get_sent_row_count() && !res)
    status_var_increment(thd->status_var.empty_queries);
  else
    status_var_add(thd->status_var.rows_sent, thd->get_sent_row_count());

  return res;
}


/**
   SHOW STATUS

   Notes: This is noinline as we don't want to have system_status_var (> 3K)
   to be on the stack of mysql_execute_command()
*/

static bool __attribute__ ((noinline))
execute_show_status(THD *thd, TABLE_LIST *all_tables)
{
  bool res;
  system_status_var old_status_var= thd->status_var;
  thd->initial_status_var= &old_status_var;
  WSREP_SYNC_WAIT(thd, WSREP_SYNC_WAIT_BEFORE_SHOW);
  if (!(res= check_table_access(thd, SELECT_ACL, all_tables, FALSE,
                                UINT_MAX, FALSE)))
    res= execute_sqlcom_select(thd, all_tables);

  thd->initial_status_var= NULL;
  /* Don't log SHOW STATUS commands to slow query log */
  thd->server_status&= ~(SERVER_QUERY_NO_INDEX_USED |
                         SERVER_QUERY_NO_GOOD_INDEX_USED);
  /*
    restore status variables, as we don't want 'show status' to cause
    changes
  */
  mysql_mutex_lock(&LOCK_status);
  add_diff_to_status(&global_status_var, &thd->status_var,
                     &old_status_var);
  memcpy(&thd->status_var, &old_status_var, last_restored_status_var);
  mysql_mutex_unlock(&LOCK_status);
  thd->initial_status_var= NULL;
  return res;
#ifdef WITH_WSREP
wsrep_error_label: /* see WSREP_SYNC_WAIT() macro above */
  thd->initial_status_var= NULL;
  return true;
#endif /* WITH_WSREP */
}


/*
  Find out if a table is a temporary table

  A table is a temporary table if it's a temporary table or
  there has been before a temporary table that has been renamed
  to the current name.

  Some examples:
  A->B          B is a temporary table if and only if A is a temp.
  A->B, B->C    Second B is temp if A is temp
  A->B, A->C    Second A can't be temp as if A was temp then B is temp
                and Second A can only be a normal table. C is also not temp
*/

static TABLE *find_temporary_table_for_rename(THD *thd,
                                              TABLE_LIST *first_table,
                                              TABLE_LIST *cur_table)
{
  TABLE_LIST *table;
  TABLE *res= 0;
  bool found= 0;
  DBUG_ENTER("find_temporary_table_for_rename");

  /* Find last instance when cur_table is in TO part */
  for (table= first_table;
       table != cur_table;
       table= table->next_local->next_local)
  {
    TABLE_LIST *next= table->next_local;

    if (!strcmp(table->get_db_name().str,    cur_table->get_db_name().str) &&
        !strcmp(table->get_table_name().str, cur_table->get_table_name().str))
    {
      /* Table was moved away, can't be same as 'table' */
      found= 1;
      res= 0;                      // Table can't be a temporary table
    }
    if (!strcmp(next->get_db_name().str,    cur_table->get_db_name().str) &&
        !strcmp(next->get_table_name().str, cur_table->get_table_name().str))
    {
      /*
        Table has matching name with new name of this table. cur_table should
        have same temporary type as this table.
      */
      found= 1;
      res= table->table;
    }
  }
  if (!found)
    res= thd->find_temporary_table(table, THD::TMP_TABLE_ANY);
  DBUG_RETURN(res);
}


static bool __attribute__ ((noinline))
check_rename_table(THD *thd, TABLE_LIST *first_table,
                   TABLE_LIST *all_tables)
{
  DBUG_ASSERT(first_table == all_tables && first_table != 0);
  TABLE_LIST *table;
  for (table= first_table; table; table= table->next_local->next_local)
  {
    if (check_access(thd, ALTER_ACL | DROP_ACL, table->db.str,
                     &table->grant.privilege,
                     &table->grant.m_internal,
                     0, 0) ||
        check_access(thd, INSERT_ACL | CREATE_ACL, table->next_local->db.str,
                     &table->next_local->grant.privilege,
                     &table->next_local->grant.m_internal,
                     0, 0))
      return 1;

    /* check if these are referring to temporary tables */
    table->table= find_temporary_table_for_rename(thd, first_table, table);
    table->next_local->table= table->table;

    TABLE_LIST old_list, new_list;
    /*
      we do not need initialize old_list and new_list because we will
      copy table[0] and table->next[0] there
    */
    old_list= table[0];
    new_list= table->next_local[0];

    if (check_grant(thd, ALTER_ACL | DROP_ACL, &old_list, FALSE, 1, FALSE) ||
       (!test_all_bits(table->next_local->grant.privilege,
                       INSERT_ACL | CREATE_ACL) &&
        check_grant(thd, INSERT_ACL | CREATE_ACL, &new_list, FALSE, 1,
                    FALSE)))
      return 1;
  }

  return 0;
}

/*
  Generate an incident log event before writing the real event
  to the binary log.  We put this event is before the statement
  since that makes it simpler to check that the statement was
  not executed on the slave (since incidents usually stop the
  slave).

  Observe that any row events that are generated will be generated before.

  This is only for testing purposes and will not be present in a release build.
*/

#ifndef DBUG_OFF
static bool __attribute__ ((noinline)) generate_incident_event(THD *thd)
{
  if (mysql_bin_log.is_open())
  {

    Incident incident= INCIDENT_NONE;
    DBUG_PRINT("debug", ("Just before generate_incident()"));
    DBUG_EXECUTE_IF("incident_database_resync_on_replace",
                    incident= INCIDENT_LOST_EVENTS;);
    if (incident)
    {
      Incident_log_event ev(thd, incident);
      (void) mysql_bin_log.write(&ev);        /* error is ignored */
      if (mysql_bin_log.rotate_and_purge(true))
        return 1;
    }
    DBUG_PRINT("debug", ("Just after generate_incident()"));
  }
  return 0;
}
#else
static bool generate_incident_event(THD *thd)
{
  return 0;
}
#endif


static int __attribute__ ((noinline))
show_create_db(THD *thd, LEX *lex)
{
  DBUG_EXECUTE_IF("4x_server_emul",
                  my_error(ER_UNKNOWN_ERROR, MYF(0)); return 1;);

  const DBNameBuffer dbbuf(lex->name, lower_case_table_names == 1);
  if (Lex_ident_db::check_name_with_error(dbbuf.to_lex_cstring()))
    return 1;
  LEX_CSTRING db= dbbuf.to_lex_cstring();
  return mysqld_show_create_db(thd, &db, &lex->name, lex->create_info);
}


/**
   Called on SQLCOM_ALTER_PROCEDURE and SQLCOM_ALTER_FUNCTION
*/

static bool __attribute__ ((noinline))
alter_routine(THD *thd, LEX *lex)
{
  int sp_result;
  const Sp_handler *sph= Sp_handler::handler(lex->sql_command);
  if (check_routine_access(thd, ALTER_PROC_ACL, &lex->spname->m_db,
                           &lex->spname->m_name, sph, 0))
    return 1;
  /*
    Note that if you implement the capability of ALTER FUNCTION to
    alter the body of the function, this command should be made to
    follow the restrictions that log-bin-trust-function-creators=0
    already puts on CREATE FUNCTION.
  */
  /* Conditionally writes to binlog */
  sp_result= sph->sp_update_routine(thd, lex->spname, &lex->sp_chistics);
  switch (sp_result) {
  case SP_OK:
    my_ok(thd);
    return 0;
  case SP_KEY_NOT_FOUND:
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
             sph->type_str(), ErrConvDQName(lex->spname).ptr());
    return 1;
  default:
    my_error(ER_SP_CANT_ALTER, MYF(0),
             sph->type_str(), ErrConvDQName(lex->spname).ptr());
    return 1;
  }
  return 0;                                     /* purecov: deadcode */
}


static bool __attribute__ ((noinline))
drop_routine(THD *thd, LEX *lex)
{
  int sp_result;
#ifdef HAVE_DLOPEN
  if (lex->sql_command == SQLCOM_DROP_FUNCTION &&
      ! lex->spname->m_explicit_name)
  {
    /* DROP FUNCTION <non qualified name> */
    enum drop_udf_result rc= mysql_drop_function(thd, &lex->spname->m_name);
    switch (rc) {
    case UDF_DEL_RESULT_DELETED:
      my_ok(thd);
      return 0;
    case UDF_DEL_RESULT_ERROR:
      return 1;
    case UDF_DEL_RESULT_ABSENT:
      goto absent;
    }

    DBUG_ASSERT("wrong return code" == 0);
absent:
    // If there was no current database, so it cannot be SP
    if (!lex->spname->m_db.str)
    {
      if (lex->if_exists())
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_SP_DOES_NOT_EXIST,
                            ER_THD(thd, ER_SP_DOES_NOT_EXIST),
                            "FUNCTION (UDF)", lex->spname->m_name.str);
        my_ok(thd);
        return 0;
      }
      my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
               "FUNCTION (UDF)", lex->spname->m_name.str);
      return 1;
    }
    /* Fall trough to test for a stored function */
  }
#endif /* HAVE_DLOPEN */

  const Sp_handler *sph= Sp_handler::handler(lex->sql_command);

  if (check_routine_access(thd, ALTER_PROC_ACL, &lex->spname->m_db,
                           &lex->spname->m_name,
                           Sp_handler::handler(lex->sql_command), 0))
    return 1;

  WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, NULL);

  /* Conditionally writes to binlog */
  sp_result= sph->sp_drop_routine(thd, lex->spname);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  /*
    We're going to issue an implicit REVOKE statement so we close all
    open tables. We have to keep metadata locks as this ensures that
    this statement is atomic against concurent FLUSH TABLES WITH READ
    LOCK. Deadlocks which can arise due to fact that this implicit
    statement takes metadata locks should be detected by a deadlock
    detector in MDL subsystem and reported as errors.

    TODO: Long-term we should either ensure that implicit REVOKE statement
    is written into binary log as a separate statement or make both
    dropping of routine and implicit REVOKE parts of one fully atomic
    statement.
  */
  if (trans_commit_stmt(thd))
    sp_result= SP_INTERNAL_ERROR;
  close_thread_tables(thd);

  if (sp_result != SP_KEY_NOT_FOUND &&
      sp_automatic_privileges && !opt_noacl &&
      sp_revoke_privileges(thd, lex->spname->m_db,
                           Lex_ident_routine(lex->spname->m_name),
                           Sp_handler::handler(lex->sql_command)))
  {
    push_warning(thd, Sql_condition::WARN_LEVEL_WARN,
                 ER_PROC_AUTO_REVOKE_FAIL,
                 ER_THD(thd, ER_PROC_AUTO_REVOKE_FAIL));
    /* If this happens, an error should have been reported. */
    return 1;
  }
#endif /* NO_EMBEDDED_ACCESS_CHECKS */

  switch (sp_result) {
  case SP_OK:
    my_ok(thd);
    return 0;
  case SP_KEY_NOT_FOUND:
    int res;
    if (lex->if_exists())
    {
      res= write_bin_log(thd, TRUE, thd->query(), thd->query_length());
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_SP_DOES_NOT_EXIST,
                          ER_THD(thd, ER_SP_DOES_NOT_EXIST),
                          sph->type_str(),
                          ErrConvDQName(lex->spname).ptr());
      if (res)
        return 1;
      my_ok(thd);
      return 0;
    }
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
             sph->type_str(), ErrConvDQName(lex->spname).ptr());
    return 1;
  default:
    my_error(ER_SP_DROP_FAILED, MYF(0),
             sph->type_str(), ErrConvDQName(lex->spname).ptr());
    return 1;
  }

#ifdef WITH_WSREP
wsrep_error_label:
  return 1;
#endif
}

/**
  @brief Compare requested privileges with the privileges acquired from the
    User- and Db-tables.
  @param thd          Thread handler
  @param want_access  The requested access privileges.
  @param db           A pointer to the Db name.
  @param[out] save_priv A pointer to the granted privileges will be stored.
  @param grant_internal_info A pointer to the internal grant cache.
  @param dont_check_global_grants True if no global grants are checked.
  @param no_error     True if no errors should be sent to the client.

  'save_priv' is used to save the User-table (global) and Db-table grants for
  the supplied db name. Note that we don't store db level grants if the global
  grants is enough to satisfy the request AND the global grants contains a
  SELECT grant.

  For internal databases (INFORMATION_SCHEMA, PERFORMANCE_SCHEMA),
  additional rules apply, see ACL_internal_schema_access.

  @see check_grant

  @return Status of denial of access by exclusive ACLs.
    @retval FALSE Access can't exclusively be denied by Db- and User-table
      access unless Column- and Table-grants are checked too.
    @retval TRUE Access denied.
*/

bool
check_access(THD *thd, privilege_t want_access,
             const char *db, privilege_t *save_priv,
             GRANT_INTERNAL_INFO *grant_internal_info,
             bool dont_check_global_grants, bool no_errors)
{
#ifdef NO_EMBEDDED_ACCESS_CHECKS
  if (save_priv)
    *save_priv= GLOBAL_ACLS;
  return false;
#else
  Security_context *sctx= thd->security_ctx;
  privilege_t db_access(NO_ACL);

  /*
    GRANT command:
    In case of database level grant the database name may be a pattern,
    in case of table|column level grant the database name can not be a pattern.
    We use 'dont_check_global_grants' as a flag to determine
    if it's database level grant command
    (see SQLCOM_GRANT case, mysql_execute_command() function) and
    set db_is_pattern according to 'dont_check_global_grants' value.
  */
  bool  db_is_pattern= ((want_access & GRANT_ACL) && dont_check_global_grants);
  privilege_t dummy(NO_ACL);
  DBUG_ENTER("check_access");
  DBUG_PRINT("enter",("db: %s  want_access: %llx  master_access: %llx",
                      db ? db : "",
                      (longlong) want_access,
                      (longlong) sctx->master_access));

  if (save_priv)
    *save_priv= NO_ACL;
  else
  {
    save_priv= &dummy;
    dummy= NO_ACL;
  }

  /* check access may be called twice in a row. Don't change to same stage */
  if (thd->proc_info != stage_checking_permissions.m_name)
    THD_STAGE_INFO(thd, stage_checking_permissions);
  if (unlikely((!db || !db[0]) && !thd->db.str && !dont_check_global_grants))
  {
    DBUG_RETURN(FALSE); // CTE reference or an error later
  }

  if (likely((db != NULL) && (db != any_db.str)))
  {
    /*
      Check if this is reserved database, like information schema or
      performance schema
    */
    const ACL_internal_schema_access *access;
    access= get_cached_schema_access(grant_internal_info, db);
    if (access)
    {
      switch (access->check(want_access, save_priv))
      {
      case ACL_INTERNAL_ACCESS_GRANTED:
        /*
          All the privileges requested have been granted internally.
          [out] *save_privileges= Internal privileges.
        */
        DBUG_RETURN(FALSE);
      case ACL_INTERNAL_ACCESS_DENIED:
        if (! no_errors)
        {
          status_var_increment(thd->status_var.access_denied_errors);
          my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
                   sctx->priv_user, sctx->priv_host, db);
        }
        DBUG_RETURN(TRUE);
      case ACL_INTERNAL_ACCESS_CHECK_GRANT:
        /*
          Only some of the privilege requested have been granted internally,
          proceed with the remaining bits of the request (want_access).
        */
        want_access&= ~(*save_priv);
        break;
      }
    }
  }

  if ((sctx->master_access & want_access) == want_access)
  {
    /*
      1. If we don't have a global SELECT privilege, we have to get the
      database specific access rights to be able to handle queries of type
      UPDATE t1 SET a=1 WHERE b > 0
      2. Change db access if it isn't current db which is being addressed
    */
    if (!(sctx->master_access & SELECT_ACL))
    {
      if (db && (!thd->db.str || db_is_pattern || strcmp(db, thd->db.str)))
      {
        db_access= acl_get_all3(sctx, db, db_is_pattern);
      }
      else
      {
        /* get access for current db */
        db_access= sctx->db_access;
      }
      /*
        The effective privileges are the union of the global privileges
        and the intersection of db- and host-privileges,
        plus the internal privileges.
      */
      *save_priv|= sctx->master_access | db_access;
    }
    else
      *save_priv|= sctx->master_access;
    DBUG_RETURN(FALSE);
  }
  if (unlikely(((want_access & ~sctx->master_access) & ~DB_ACLS) ||
               (! db && dont_check_global_grants)))
  {						// We can never grant this
    DBUG_PRINT("error",("No possible access"));
    if (!no_errors)
    {
      status_var_increment(thd->status_var.access_denied_errors);
      my_error(access_denied_error_code(thd->password), MYF(0),
               sctx->priv_user,
               sctx->priv_host,
               (thd->password ?
                ER_THD(thd, ER_YES) :
                ER_THD(thd, ER_NO)));                    /* purecov: tested */
    }
    DBUG_RETURN(TRUE);				/* purecov: tested */
  }

  if (unlikely(db == any_db.str))
  {
    /*
      Access granted; Allow select on *any* db.
      [out] *save_privileges= 0
    */
    DBUG_RETURN(FALSE);
  }

  if (db && (!thd->db.str || db_is_pattern || strcmp(db, thd->db.str)))
    db_access= acl_get_all3(sctx, db, db_is_pattern);
  else
    db_access= sctx->db_access;
  DBUG_PRINT("info",("db_access: %llx  want_access: %llx",
                     (longlong) db_access, (longlong) want_access));

  /*
    Save the union of User-table and the intersection between Db-table and
    Host-table privileges, with the already saved internal privileges.
  */
  db_access= (db_access | sctx->master_access);
  *save_priv|= db_access;

  /*
    We need to investigate column- and table access if all requested privileges
    belongs to the bit set of .
  */
  bool need_table_or_column_check=
    (want_access & (TABLE_ACLS | PROC_ACLS | db_access)) == want_access;

  /*
    Grant access if the requested access is in the intersection of
    host- and db-privileges (as retrieved from the acl cache),
    also grant access if all the requested privileges are in the union of
    TABLES_ACLS and PROC_ACLS; see check_grant.
  */
  if ( (db_access & want_access) == want_access ||
      (!dont_check_global_grants &&
       need_table_or_column_check))
  {
    /*
       Ok; but need to check table- and column privileges.
       [out] *save_privileges is (User-priv | (Db-priv & Host-priv) | Internal-priv)
    */
    DBUG_RETURN(FALSE);
  }

  /*
    Access is denied;
    [out] *save_privileges is (User-priv | (Db-priv & Host-priv) | Internal-priv)
  */
  DBUG_PRINT("error",("Access denied"));
  if (!no_errors)
  {
    status_var_increment(thd->status_var.access_denied_errors);
    my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
             sctx->priv_user, sctx->priv_host,
             (db ? db : (thd->db.str ?
                         thd->db.str :
                         "unknown")));
  }
  DBUG_RETURN(TRUE);
#endif // NO_EMBEDDED_ACCESS_CHECKS
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS
/**
  Check grants for commands which work only with one table.

  @param thd                    Thread handler
  @param privilege              requested privilege
  @param tables                 global table list of query
  @param no_errors              FALSE/TRUE - report/don't report error to
                            the client (using my_error() call).

  @retval
    0   OK
  @retval
    1   access denied, error is sent to client
*/

bool check_single_table_access(THD *thd, privilege_t privilege,
                               TABLE_LIST *tables, bool no_errors)
{
  if (tables->derived)
    return 0;

  Switch_to_definer_security_ctx backup_sctx(thd, tables);

  const char *db_name;
  if ((tables->view || tables->field_translation) && !tables->schema_table)
    db_name= tables->view_db.str;
  else
    db_name= tables->db.str;

  if (check_access(thd, privilege, db_name, &tables->grant.privilege,
                   &tables->grant.m_internal, 0, no_errors))
    return 1;

  /* Show only 1 table for check_grant */
  if (!(tables->belong_to_view &&
       (thd->lex->sql_command == SQLCOM_SHOW_FIELDS)) &&
      check_grant(thd, privilege, tables, FALSE, 1, no_errors))
    return 1;

  return 0;
}

/**
  Check grants for commands which work only with one table and all other
  tables belonging to subselects or implicitly opened tables.

  @param thd			Thread handler
  @param privilege		requested privilege
  @param all_tables		global table list of query

  @retval
    0   OK
  @retval
    1   access denied, error is sent to client
*/

bool check_one_table_access(THD *thd, privilege_t privilege,
                            TABLE_LIST *all_tables)
{
  if (check_single_table_access (thd,privilege,all_tables, FALSE))
    return 1;

  /* Check rights on tables of subselects and implicitly opened tables */
  TABLE_LIST *subselects_tables, *view= all_tables->view ? all_tables : 0;
  if ((subselects_tables= all_tables->next_global))
  {
    /*
      Access rights asked for the first table of a view should be the same
      as for the view
    */
    if (view && subselects_tables->belong_to_view == view)
    {
      if (check_single_table_access (thd, privilege, subselects_tables, FALSE))
        return 1;
      subselects_tables= subselects_tables->next_global;
    }
    if (subselects_tables &&
        (check_table_access(thd, SELECT_ACL, subselects_tables, FALSE,
                            UINT_MAX, FALSE)))
      return 1;
  }
  return 0;
}


static bool check_show_access(THD *thd, TABLE_LIST *table)
{
  /*
    This is a SHOW command using an INFORMATION_SCHEMA table.
    check_access() has not been called for 'table',
    and SELECT is currently always granted on the I_S, so we automatically
    grant SELECT on table here, to bypass a call to check_access().
    Note that not calling check_access(table) is an optimization,
    which needs to be revisited if the INFORMATION_SCHEMA does
    not always automatically grant SELECT but use the grant tables.
    See Bug#38837 need a way to disable information_schema for security
  */
  table->grant.privilege= SELECT_ACL;

  switch (get_schema_table_idx(table->schema_table)) {
  case SCH_SCHEMATA:
    return (specialflag & SPECIAL_SKIP_SHOW_DB) &&
      check_global_access(thd, SHOW_DB_ACL);

  case SCH_TABLE_NAMES:
  case SCH_TABLES:
  case SCH_VIEWS:
  case SCH_TRIGGERS:
  case SCH_EVENTS:
  {
    const char *dst_db_name= table->schema_select_lex->db.str;

    DBUG_ASSERT(dst_db_name);

    if (check_access(thd, SELECT_ACL, dst_db_name,
                     &thd->col_access, NULL, FALSE, FALSE))
      return TRUE;

    if (!thd->col_access && check_grant_db(thd, dst_db_name))
    {
      status_var_increment(thd->status_var.access_denied_errors);
      my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
               thd->security_ctx->priv_user,
               thd->security_ctx->priv_host,
               dst_db_name);
      return TRUE;
    }

    return FALSE;
  }

  case SCH_COLUMNS:
  case SCH_STATISTICS:
  {
    TABLE_LIST *dst_table;
    dst_table= table->schema_select_lex->table_list.first;

    DBUG_ASSERT(dst_table);

    /*
      Open temporary tables to be able to detect them during privilege check.
    */
    if (thd->open_temporary_tables(dst_table))
      return TRUE;

    if (check_access(thd, SELECT_ACL, dst_table->db.str,
                     &dst_table->grant.privilege,
                     &dst_table->grant.m_internal,
                     FALSE, FALSE))
          return TRUE; /* Access denied */

    thd->col_access= dst_table->grant.privilege; // for sql_show.cc
    /*
      Check_grant will grant access if there is any column privileges on
      all of the tables thanks to the fourth parameter (bool show_table).
    */
    if (check_grant(thd, SELECT_ACL, dst_table, TRUE, 1, FALSE))
      return TRUE; /* Access denied */

    close_thread_tables(thd);
    dst_table->table= NULL;

    /* Access granted */
    return FALSE;
  }
  default:
    break;
  }

  return FALSE;
}



/**
  @brief Check if the requested privileges exists in either User-, Host- or
    Db-tables.
  @param thd          Thread context
  @param requirements Privileges requested
  @param tables       List of tables to be compared against
  @param any_combination_of_privileges_will_do TRUE if any privileges on any
    column combination is enough.
  @param number       Only the first 'number' tables in the linked list are
                      relevant.
  @param no_errors    Don't report error to the client (using my_error() call).

  The suppled table list contains cached privileges. This functions calls the
  help functions check_access and check_grant to verify the first three steps
  in the privileges check queue:
  1. Global privileges
  2. OR (db privileges AND host privileges)
  3. OR table privileges
  4. OR column privileges (not checked by this function!)
  5. OR routine privileges (not checked by this function!)

  @see check_access
  @see check_grant

  @note This functions assumes that table list used and
  thd->lex->query_tables_own_last value correspond to each other
  (the latter should be either 0 or point to next_global member
  of one of elements of this table list).

  @return
    @retval FALSE OK
    @retval TRUE  Access denied; But column or routine privileges might need to
      be checked also.
*/

bool
check_table_access(THD *thd, privilege_t requirements, TABLE_LIST *tables,
                   bool any_combination_of_privileges_will_do,
                   uint number, bool no_errors)
{
  TABLE_LIST *org_tables= tables;
  TABLE_LIST *first_not_own_table= thd->lex->first_not_own_table();
  uint i= 0;
  /*
    The check that first_not_own_table is not reached is for the case when
    the given table list refers to the list for prelocking (contains tables
    of other queries). For simple queries first_not_own_table is 0.
  */
  for (; i < number && tables != first_not_own_table && tables;
       tables= tables->next_global, i++)
  {
    TABLE_LIST *const table_ref= tables->correspondent_table ?
      tables->correspondent_table : tables;
    Switch_to_definer_security_ctx backup_ctx(thd, table_ref);

    privilege_t want_access(requirements);

    /*
       Register access for view underlying table.
       Remove SHOW_VIEW_ACL, because it will be checked during making view
     */
    table_ref->grant.orig_want_privilege= (want_access & ~SHOW_VIEW_ACL);

    if (table_ref->schema_table_reformed)
    {
      if (check_show_access(thd, table_ref))
        return 1;
      continue;
    }

    DBUG_PRINT("info", ("derived: %d  view: %d", table_ref->derived != 0,
                        table_ref->view != 0));

    if (table_ref->is_anonymous_derived_table() || table_ref->sequence)
      continue;

    if (check_access(thd, want_access, table_ref->get_db_name().str,
                     &table_ref->grant.privilege,
                     &table_ref->grant.m_internal,
                     0, no_errors))
      return 1;
  }
  return check_grant(thd,requirements,org_tables,
                     any_combination_of_privileges_will_do,
                     number, no_errors);
}


bool
check_routine_access(THD *thd, privilege_t want_access, const LEX_CSTRING *db,
                     const LEX_CSTRING *name,
                     const Sp_handler *sph, bool no_errors)
{
  TABLE_LIST tables[1];
  
  bzero((char *)tables, sizeof(TABLE_LIST));
  tables->db= Lex_ident_db(*db);
  tables->table_name= tables->alias= Lex_ident_table(*name);
  
  /*
    The following test is just a shortcut for check_access() (to avoid
    calculating db_access) under the assumption that it's common to
    give persons global right to execute all stored SP (but not
    necessary to create them).
    Note that this effectively bypasses the ACL_internal_schema_access checks
    that are implemented for the INFORMATION_SCHEMA and PERFORMANCE_SCHEMA,
    which are located in check_access().
    Since the I_S and P_S do not contain routines, this bypass is ok,
    as long as this code path is not abused to create routines.
    The assert enforce that.
  */
  DBUG_ASSERT((want_access & CREATE_PROC_ACL) == NO_ACL);
  if ((thd->security_ctx->master_access & want_access) == want_access)
    tables->grant.privilege= want_access;
  else if (check_access(thd, want_access, db->str,
                        &tables->grant.privilege,
                        &tables->grant.m_internal,
                        0, no_errors))
    return TRUE;
  
  return check_grant_routine(thd, want_access, tables, sph, no_errors);
}


/**
  Check if the routine has any of the routine privileges.

  @param thd	       Thread handler
  @param db           Database name
  @param name         Routine name

  @retval
    0            ok
  @retval
    1            error
*/

bool check_some_routine_access(THD *thd, const char *db, const char *name,
                               const Sp_handler *sph)
{
  privilege_t save_priv(NO_ACL);
  /*
    The following test is just a shortcut for check_access() (to avoid
    calculating db_access)
    Note that this effectively bypasses the ACL_internal_schema_access checks
    that are implemented for the INFORMATION_SCHEMA and PERFORMANCE_SCHEMA,
    which are located in check_access().
    Since the I_S and P_S do not contain routines, this bypass is ok,
    as it only opens SHOW_PROC_WITHOUT_DEFINITION_ACLS.
  */
  if (thd->security_ctx->master_access & SHOW_PROC_WITHOUT_DEFINITION_ACLS)
    return FALSE;
  if (!check_access(thd, SHOW_PROC_WITHOUT_DEFINITION_ACLS,
                    db, &save_priv, NULL, 0, 1) ||
      (save_priv & SHOW_PROC_WITHOUT_DEFINITION_ACLS))
    return FALSE;
  return check_routine_level_acl(thd, SHOW_PROC_WITHOUT_DEFINITION_ACLS,
                                 db, name, sph);
}


/*
  Check if the given table has any of the asked privileges

  @param thd		 Thread handler
  @param want_access	 Bitmap of possible privileges to check for

  @retval
    0  ok
  @retval
    1  error
*/

bool check_some_access(THD *thd, privilege_t want_access, TABLE_LIST *table)
{
  DBUG_ENTER("check_some_access");

  for (ulonglong bit= 1; bit < (ulonglong) want_access ; bit<<= 1)
  {
    if (bit & want_access)
    {
      privilege_t access= ALL_KNOWN_ACL & bit;
      if (!check_access(thd, access, table->db.str,
                        &table->grant.privilege,
                        &table->grant.m_internal,
                        0, 1) &&
           !check_grant(thd, access, table, FALSE, 1, TRUE))
        DBUG_RETURN(0);
    }
  }
  DBUG_PRINT("exit",("no matching access rights"));
  DBUG_RETURN(1);
}

#endif /*NO_EMBEDDED_ACCESS_CHECKS*/


/**
  check for global access and give descriptive error message if it fails.

  @param thd			Thread handler
  @param want_access		Use should have any of these global rights

  @warning
    Starting from 10.5.2 only one bit is allowed in want_access.
    Access denied error is returned if want_access has multiple bits set.

  @retval
    0	ok
  @retval
    1	Access denied.  In this case an error is sent to the client
*/

bool check_global_access(THD *thd, privilege_t want_access, bool no_errors)
{
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  char command[128];
  if (thd->security_ctx->master_access & want_access)
    return 0;
  if (unlikely(!no_errors))
  {
    get_privilege_desc(command, sizeof(command), want_access);
    my_error(ER_SPECIFIC_ACCESS_DENIED_ERROR, MYF(0), command);
  }
  status_var_increment(thd->status_var.access_denied_errors);
  return 1;
#else
  return 0;
#endif
}


/**
  Checks foreign key's parent table access.

  @param thd	       [in]	Thread handler
  @param create_info   [in]     Create information (like MAX_ROWS, ENGINE or
                                temporary table flag)
  @param alter_info    [in]     Initial list of columns and indexes for the
                                table to be created
  @param create_db     [in]     Database of the created table

  @retval
   false  ok.
  @retval
   true	  error or access denied. Error is sent to client in this case.
*/
bool check_fk_parent_table_access(THD *thd,
                                  HA_CREATE_INFO *create_info,
                                  Alter_info *alter_info,
                                  const LEX_CSTRING &create_db)
{
  Key *key;
  List_iterator<Key> key_iterator(alter_info->key_list);

  while ((key= key_iterator++))
  {
    if (key->type == Key::FOREIGN_KEY)
    {
      TABLE_LIST parent_table;
      Foreign_key *fk_key= (Foreign_key *)key;
      LEX_CSTRING db_name;
      LEX_CSTRING table_name= { fk_key->ref_table.str,
                               fk_key->ref_table.length };
      const privilege_t privileges(COL_DML_ACLS | REFERENCES_ACL);

      // Check if tablename is valid or not.
      DBUG_ASSERT(table_name.str != NULL);
      if (Lex_ident_table::check_name(table_name, false))
      {
        my_error(ER_WRONG_TABLE_NAME, MYF(0), table_name.str);
        return true;
      }
      // if lower_case_table_names is set then convert tablename to lower case.
      if (lower_case_table_names &&
          !(table_name= thd->make_ident_casedn(fk_key->ref_table)).str)
        return true;

      if (fk_key->ref_db.str)
      {
        if (Lex_ident_db::check_name_with_error(fk_key->ref_db) ||
            !(db_name= thd->make_ident_opt_casedn(fk_key->ref_db,
                                                  lower_case_table_names)).str)
          return true;
      }
      else
      {
        if (!thd->db.str)
        {
          DBUG_ASSERT(create_db.str);
          if (Lex_ident_db::check_name_with_error(create_db) ||
              !(db_name= thd->make_ident_opt_casedn(create_db,
                                                 lower_case_table_names)).str)
            return true;
        }
        else
        {
          if (thd->lex->copy_db_to(&db_name) ||
              (lower_case_table_names &&
               !(db_name= thd->make_ident_casedn(db_name)).str))
            return true;
        }
      }

      parent_table.init_one_table(&db_name, &table_name, 0, TL_IGNORE);

      /*
       Check if user has any of the "privileges" at table level on
       "parent_table".
       Having privilege on any of the parent_table column is not
       enough so checking whether user has any of the "privileges"
       at table level only here.
      */
      if (check_some_access(thd, privileges, &parent_table) ||
          parent_table.grant.want_privilege)
      {
        my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0),
                "REFERENCES",
                thd->security_ctx->priv_user,
                thd->security_ctx->host_or_ip,
                db_name.str, table_name.str);
        return true;
      }
    }
  }

  return false;
}


/****************************************************************************
	Check stack size; Send error if there isn't enough stack to continue
****************************************************************************/


#ifndef DBUG_OFF
long max_stack_used;
#endif

/**
  @note
  Note: The 'buf' parameter is necessary, even if it is unused here.
  - fix_fields functions has a "dummy" buffer large enough for the
    corresponding exec. (Thus we only have to check in fix_fields.)
  - Passing to check_stack_overrun() prevents the compiler from removing it.
*/

bool
#if defined __GNUC__ && !defined __clang__
/*
  Do not optimize the function in order to preserve a stack variable creation.
  Otherwise, the variable pointed as "buf" can be removed due to a missing
  usage.
 */
__attribute__((optimize("-O0")))
#endif
check_stack_overrun(THD *thd, long margin, uchar *buf __attribute__((unused)))
{
#ifndef __SANITIZE_ADDRESS__
  long stack_used;
  DBUG_ASSERT(thd == current_thd);
  DBUG_ASSERT(thd->thread_stack);
  if ((stack_used= available_stack_size(thd->thread_stack,
                                        my_get_stack_pointer(&stack_used))) >=
      (long) (my_thread_stack_size - margin))
  {
    thd->is_fatal_error= 1;
    /*
      Do not use stack for the message buffer to ensure correct
      behaviour in cases we have close to no stack left.
    */
    char* ebuff= new char[MYSQL_ERRMSG_SIZE];
    if (ebuff) {
      my_snprintf(ebuff, MYSQL_ERRMSG_SIZE, ER_THD(thd, ER_STACK_OVERRUN_NEED_MORE),
                  stack_used, my_thread_stack_size, margin);
      my_message(ER_STACK_OVERRUN_NEED_MORE, ebuff, MYF(ME_FATAL));
      delete [] ebuff;
    }
    return 1;
  }
#ifndef DBUG_OFF
  max_stack_used= MY_MAX(max_stack_used, stack_used);
#endif
#endif /* __SANITIZE_ADDRESS__ */
  return 0;
}


#define MY_YACC_INIT 1000			// Start with big alloc
#define MY_YACC_MAX  32000			// Because of 'short'

bool my_yyoverflow(short **yyss, YYSTYPE **yyvs, size_t *yystacksize)
{
  Yacc_state *state= & current_thd->m_parser_state->m_yacc;
  size_t old_info=0;
  DBUG_ASSERT(state);
  if ( *yystacksize >= MY_YACC_MAX)
    return 1;
  if (!state->yacc_yyvs)
    old_info= *yystacksize;
  *yystacksize= set_zone((int)(*yystacksize)*2,MY_YACC_INIT,MY_YACC_MAX);
  if (!(state->yacc_yyvs= (uchar*)
        my_realloc(key_memory_bison_stack, state->yacc_yyvs,
                   *yystacksize*sizeof(**yyvs),
                   MYF(MY_ALLOW_ZERO_PTR | MY_FREE_ON_ERROR))) ||
      !(state->yacc_yyss= (uchar*)
        my_realloc(key_memory_bison_stack, state->yacc_yyss,
                   *yystacksize*sizeof(**yyss),
                   MYF(MY_ALLOW_ZERO_PTR | MY_FREE_ON_ERROR))))
    return 1;
  if (old_info)
  {
    /*
      Only copy the old stack on the first call to my_yyoverflow(),
      when replacing a static stack (YYINITDEPTH) by a dynamic stack.
      For subsequent calls, my_realloc already did preserve the old stack.
    */
    memcpy(state->yacc_yyss, *yyss, old_info*sizeof(**yyss));
    memcpy(state->yacc_yyvs, *yyvs, old_info*sizeof(**yyvs));
  }
  *yyss= (short*) state->yacc_yyss;
  *yyvs= (YYSTYPE*) state->yacc_yyvs;
  return 0;
}


/**
  Reset the part of THD responsible for the state of command
  processing.

  @param do_clear_error  Set if we should clear errors

  This needs to be called before execution of every statement
  (prepared or conventional).  It is not called by substatements of
  routines.

  @todo Call it after we use THD for queries, not before.
*/

void THD::reset_for_next_command(bool do_clear_error)
{
  DBUG_ENTER("THD::reset_for_next_command");
  DBUG_ASSERT(!spcont); /* not for substatements of routines */
  DBUG_ASSERT(!in_sub_stmt);
  /*
    Table maps should have been reset after previous statement except in the
    case where we have locked tables
  */
  DBUG_ASSERT(binlog_table_maps == 0 ||
              locked_tables_mode == LTM_LOCK_TABLES);

  if (likely(do_clear_error))
  {
    clear_error(1);
    /*
      The following variable can't be reset in clear_error() as
      clear_error() is called during auto_repair of table
    */
    error_printed_to_log= 0;
  }
  free_list= 0;
  /*
    We also assign stmt_lex in lex_start(), but during bootstrap this
    code is executed first.
  */
  DBUG_ASSERT(lex == &main_lex);
  main_lex.stmt_lex= &main_lex; main_lex.current_select_number= 0;
  /*
    Those two lines below are theoretically unneeded as
    THD::cleanup_after_query() should take care of this already.
  */
  auto_inc_intervals_in_cur_stmt_for_binlog.empty();
  stmt_depends_on_first_successful_insert_id_in_prev_stmt= 0;

#ifdef WITH_WSREP
  /*
    Autoinc variables should be adjusted only for locally executed
    transactions. Appliers and replayers are either processing ROW
    events or get autoinc variable values from Query_log_event and
    mysql slave may be processing STATEMENT format events, but he should
    use autoinc values passed in binlog events, not the values forced by
    the cluster.
  */
  if (WSREP_NNULL(this) && wsrep_thd_is_local(this) &&
      !slave_thread && wsrep_auto_increment_control)
  {
    variables.auto_increment_offset=
      global_system_variables.auto_increment_offset;
    variables.auto_increment_increment=
      global_system_variables.auto_increment_increment;
  }
#endif /* WITH_WSREP */

  used= 0;
  is_fatal_error= 0;
  variables.option_bits&= ~OPTION_BINLOG_THIS_STMT;

  /*
    Clear the status flag that are expected to be cleared at the
    beginning of each SQL statement.
  */
  server_status&= ~SERVER_STATUS_CLEAR_SET;
  /*
    If in autocommit mode and not in a transaction, reset
    OPTION_STATUS_NO_TRANS_UPDATE | OPTION_BINLOG_THIS_TRX to not get warnings
    in ha_rollback_trans() about some tables couldn't be rolled back.
  */
  if (!in_multi_stmt_transaction_mode())
  {
    variables.option_bits&= ~OPTION_BINLOG_THIS_TRX;
    transaction->all.reset();
  }
  DBUG_ASSERT(security_ctx== &main_security_ctx);

  if (opt_bin_log)
    reset_dynamic(&user_var_events);
  DBUG_ASSERT(user_var_events_alloc == &main_mem_root);
  enable_slow_log= true;
  get_stmt_da()->reset_for_next_command();
  sent_row_count_for_statement= examined_row_count_for_statement= 0;
  accessed_rows_and_keys= 0;
  tmp_table_binlog_handled= 0;

  reset_slow_query_state(0);

  reset_current_stmt_binlog_format_row();
  binlog_unsafe_warning_flags= 0;

  save_prep_leaf_list= false;

#if defined(WITH_WSREP) && !defined(DBUG_OFF)
  if (mysql_bin_log.is_open())
    DBUG_PRINT("info",
               ("is_current_stmt_binlog_format_row(): %d",
                 is_current_stmt_binlog_format_row()));
#endif
  DBUG_VOID_RETURN;
}


/**
  Used to allocate a new SELECT_LEX object on the current thd mem_root and
  link it into the relevant lists.

  This function is always followed by mysql_init_select.

  @see mysql_init_select

  @retval TRUE An error occurred
  @retval FALSE The new SELECT_LEX was successfully allocated.
*/

bool
mysql_new_select(LEX *lex, bool move_down, SELECT_LEX *select_lex)
{
  THD *thd= lex->thd;
  bool new_select= select_lex == NULL;
  int old_nest_level= lex->current_select->nest_level;
  DBUG_ENTER("mysql_new_select");

  if (new_select)
  {
    if (!(select_lex= new (thd->mem_root) SELECT_LEX()))
      DBUG_RETURN(1);
    select_lex->select_number= ++thd->lex->stmt_lex->current_select_number;
    select_lex->parent_lex= lex; /* Used in init_query. */
    select_lex->init_query();
    select_lex->init_select();
  }
  select_lex->nest_level_base= &thd->lex->unit;
  if (move_down)
  {
    lex->nest_level++;
    if (select_lex->set_nest_level(old_nest_level + 1))
      DBUG_RETURN(1);
    SELECT_LEX_UNIT *unit;
    /* first select_lex of subselect or derived table */
    if (!(unit= lex->alloc_unit()))
      DBUG_RETURN(1);

    unit->include_down(lex->current_select);
    unit->return_to= lex->current_select;
    select_lex->include_down(unit);
    /*
      By default we assume that it is usual subselect and we have outer name
      resolution context, if no we will assign it to 0 later
    */
    select_lex->context.outer_context= &select_lex->outer_select()->context;
  }
  else
  {
    bool const outer_most= (lex->current_select->master_unit() == &lex->unit);
    if (outer_most && lex->result)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "UNION", "INTO");
      DBUG_RETURN(TRUE);
    }

    /*
      This type of query is not possible in the grammar:
        SELECT 1 FROM t1 PROCEDURE ANALYSE() UNION ... ;

      But this type of query is still possible:
        (SELECT 1 FROM t1 PROCEDURE ANALYSE()) UNION ... ;
      and it's not easy to disallow this grammatically,
      because there can be any parenthesis nest level:
        (((SELECT 1 FROM t1 PROCEDURE ANALYSE()))) UNION ... ;
    */
    if (lex->proc_list.elements!=0)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "UNION",
               "SELECT ... PROCEDURE ANALYSE()");
      DBUG_RETURN(TRUE);
    }

    SELECT_LEX_NODE *save_slave= select_lex->slave;
    select_lex->include_neighbour(lex->current_select);
    select_lex->slave= save_slave;
    SELECT_LEX_UNIT *unit= select_lex->master_unit();
    if (select_lex->set_nest_level(old_nest_level))
      DBUG_RETURN(1);
    if (!unit->fake_select_lex && unit->add_fake_select_lex(lex->thd))
      DBUG_RETURN(1);
    select_lex->context.outer_context= 
                unit->first_select()->context.outer_context;
  }

  if (new_select)
    select_lex->include_global((st_select_lex_node**)&lex->all_selects_list);
  lex->current_select= select_lex;
  /*
    in subquery is SELECT query and we allow resolution of names in SELECT
    list
  */
  select_lex->context.resolve_in_select_list= TRUE;
  DBUG_RETURN(0);
}

/**
  Create a select to return the same output as 'SELECT @@var_name'.

  Used for SHOW COUNT(*) [ WARNINGS | ERROR].

  This will crash with a core dump if the variable doesn't exists.

  @param var_name		Variable name
*/

void create_select_for_variable(THD *thd, LEX_CSTRING *var_name)
{
  LEX *lex;
  Item *var;
  char buff[MAX_SYS_VAR_LENGTH*2+4+8], *end;
  DBUG_ENTER("create_select_for_variable");

  lex= thd->lex;
  lex->init_select();
  lex->sql_command= SQLCOM_SELECT;
  /*
    We set the name of Item to @@session.var_name because that then is used
    as the column name in the output.
  */
  if ((var= get_system_var(thd, OPT_SESSION, var_name, &null_clex_str)))
  {
    end= strxmov(buff, "@@session.", var_name->str, NullS);
    var->set_name(thd, buff, (uint)(end-buff), system_charset_info);
    add_item_to_list(thd, var);
  }
  DBUG_VOID_RETURN;
}


void mysql_init_delete(LEX *lex)
{
  lex->init_select();
  lex->first_select_lex()->limit_params.clear();
  lex->unit.lim.clear();
}

void mysql_init_multi_delete(LEX *lex)
{
  lex->sql_command=  SQLCOM_DELETE_MULTI;
  lex->first_select_lex()->table_list.
    save_and_clear(&lex->auxiliary_table_list);
  lex->query_tables= 0;
  lex->query_tables_last= &lex->query_tables;
}

#ifdef WITH_WSREP
static void wsrep_prepare_for_autocommit_retry(THD* thd,
                                               char* rawbuf,
                                               uint length,
                                               Parser_state* parser_state)
{
  thd->clear_error();
  close_thread_tables(thd);
  thd->wsrep_retry_counter++;            // grow
  wsrep_copy_query(thd);
  thd->set_time();
  parser_state->reset(rawbuf, length);

  /* PSI end */
  MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
  thd->m_statement_psi= NULL;
  thd->m_digest= NULL;

  /* DTRACE end */
  if (MYSQL_QUERY_DONE_ENABLED())
  {
    MYSQL_QUERY_DONE(thd->is_error());
  }

  /* SHOW PROFILE end */
#if defined(ENABLED_PROFILING)
  thd->profiling.finish_current_query();
#endif

  /* SHOW PROFILE begin */
#if defined(ENABLED_PROFILING)
  thd->profiling.start_new_query("continuing");
  thd->profiling.set_query_source(rawbuf, length);
#endif

  /* DTRACE begin */
  MYSQL_QUERY_START(rawbuf, thd->thread_id,
                    thd->get_db(),
                    &thd->security_ctx->priv_user[0],
                    (char *) thd->security_ctx->host_or_ip);

  /* Performance Schema Interface instrumentation, begin */
  thd->m_statement_psi= MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                                               com_statement_info[thd->get_command()].m_key);
  MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi, thd->query(),
                           thd->query_length());
 
  DBUG_ASSERT(thd->wsrep_trx().active() == false);
  thd->wsrep_cs().reset_error();
  thd->set_query_id(next_query_id());
}

static bool wsrep_mysql_parse(THD *thd, char *rawbuf, uint length,
                              Parser_state *parser_state)
{
  bool is_autocommit=
    !thd->in_multi_stmt_transaction_mode()                  &&
    !thd->wsrep_applier;
  bool retry_autocommit;
  do
  {
    retry_autocommit= false;
    mysql_parse(thd, rawbuf, length, parser_state);

    /*
      Convert all ER_QUERY_INTERRUPTED errors to ER_LOCK_DEADLOCK
      if the transaction was BF aborted. This can happen when the
      transaction is being BF aborted via thd->awake() while it is
      still executing.

      Note that this must be done before wsrep_after_statement() call
      since it clears the transaction for autocommit queries.
     */
    if (((thd->get_stmt_da()->is_error() &&
          thd->get_stmt_da()->sql_errno() == ER_QUERY_INTERRUPTED) ||
         !thd->get_stmt_da()->is_set()) &&
        thd->wsrep_trx().bf_aborted())
    {
      WSREP_DEBUG("overriding error: %d with DEADLOCK",
                  (thd->get_stmt_da()->is_error()) ?
                   thd->get_stmt_da()->sql_errno() : 0);

      thd->reset_kill_query();
      wsrep_override_error(thd, ER_LOCK_DEADLOCK);
    }

#ifdef ENABLED_DEBUG_SYNC
    /* we need the test otherwise we get stuck in the "SET DEBUG_SYNC" itself */
    if (thd->lex->sql_command != SQLCOM_SET_OPTION)
      DEBUG_SYNC(thd, "wsrep_after_statement_enter");
#endif

    if (wsrep_after_statement(thd) &&
        is_autocommit              &&
        thd_is_connection_alive(thd))
    {
      thd->reset_for_next_command();
      thd->reset_kill_query();
      if (is_autocommit                           &&
          thd->lex->sql_command != SQLCOM_SELECT  &&
          thd->wsrep_retry_counter < thd->variables.wsrep_retry_autocommit)
      {
#ifdef ENABLED_DEBUG_SYNC
        DBUG_EXECUTE_IF("sync.wsrep_retry_autocommit",
                    {
                      const char act[]=
                        "now "
                        "SIGNAL wsrep_retry_autocommit_reached "
                        "WAIT_FOR wsrep_retry_autocommit_continue";
                      DBUG_ASSERT(!debug_sync_set_action(thd, STRING_WITH_LEN(act)));
                    });
#endif
        WSREP_DEBUG("wsrep retrying AC query: %lu  %s",
                    thd->wsrep_retry_counter,
                    wsrep_thd_query(thd));
        wsrep_prepare_for_autocommit_retry(thd, rawbuf, length, parser_state);
        if (thd->lex->explain)
          delete_explain_query(thd->lex);
        retry_autocommit= true;
      }
      else
      {
        WSREP_DEBUG("%s, thd: %llu is_AC: %d, retry: %lu - %lu SQL: %s",
                    wsrep_thd_transaction_state_str(thd),
                    thd->thread_id,
                    is_autocommit,
                    thd->wsrep_retry_counter,
                    thd->variables.wsrep_retry_autocommit,
                    wsrep_thd_query(thd));
        my_error(ER_LOCK_DEADLOCK, MYF(0));
        thd->reset_kill_query();
        thd->wsrep_retry_counter= 0;             //  reset
      }
    }
    else
    {
      set_if_smaller(thd->wsrep_retry_counter, 0); // reset; eventually ok
    }
  }  while (retry_autocommit);

  if (thd->wsrep_retry_query)
  {
    WSREP_DEBUG("releasing retry_query: "
                "conf %s sent %d kill %d  errno %d SQL %s",
                wsrep_thd_transaction_state_str(thd),
                thd->get_stmt_da()->is_sent(),
                thd->killed,
                thd->get_stmt_da()->is_error() ?
                thd->get_stmt_da()->sql_errno() : 0,
                thd->wsrep_retry_query);
    my_free(thd->wsrep_retry_query);
    thd->wsrep_retry_query      = NULL;
    thd->wsrep_retry_query_len  = 0;
    thd->wsrep_retry_command    = COM_CONNECT;
    thd->proc_info= 0;
  }
  return false;
}
#endif /* WITH_WSREP */


/*
  When you modify mysql_parse(), you may need to modify
  mysql_test_parse_for_slave() in this same file.
*/

/**
  Parse a query.

  @param       thd     Current thread
  @param       rawbuf  Begining of the query text
  @param       length  Length of the query text
*/

void mysql_parse(THD *thd, char *rawbuf, uint length,
                 Parser_state *parser_state)
{
  DBUG_ENTER("mysql_parse");
  DBUG_EXECUTE_IF("parser_debug", turn_parser_debug_on_MYSQLparse(););
  DBUG_EXECUTE_IF("parser_debug", turn_parser_debug_on_ORAparse(););

  /*
    Warning.
    The purpose of query_cache_send_result_to_client() is to lookup the
    query in the query cache first, to avoid parsing and executing it.
    So, the natural implementation would be to:
    - first, call query_cache_send_result_to_client,
    - second, if caching failed, initialise the lexical and syntactic parser.
    The problem is that the query cache depends on a clean initialization
    of (among others) lex->safe_to_cache_query and thd->server_status,
    which are reset respectively in
    - lex_start()
    - THD::reset_for_next_command()
    So, initializing the lexical analyser *before* using the query cache
    is required for the cache to work properly.
    FIXME: cleanup the dependencies in the code to simplify this.
  */
  lex_start(thd);
  thd->reset_for_next_command();

  if (query_cache_send_result_to_client(thd, rawbuf, length) <= 0)
  {
    LEX *lex= thd->lex;

    bool err= parse_sql(thd, parser_state, NULL, true);

    if (likely(!err))
    {
      thd->m_statement_psi=
        MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                               sql_statement_info[thd->lex->sql_command].
                               m_key);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
      if (mqh_used && thd->user_connect &&
	  check_mqh(thd, lex->sql_command))
      {
	thd->net.error = 0;
      }
      else
#endif
      {
	if (likely(! thd->is_error()))
	{
          const char *found_semicolon= parser_state->m_lip.found_semicolon;
          /*
            Binlog logs a string starting from thd->query and having length
            thd->query_length; so we set thd->query_length correctly (to not
            log several statements in one event, when we executed only first).
            We set it to not see the ';' (otherwise it would get into binlog
            and Query_log_event::print() would give ';;' output).
            This also helps display only the current query in SHOW
            PROCESSLIST.
          */
          if (found_semicolon && (ulong) (found_semicolon - thd->query()))
            thd->set_query(thd->query(),
                           (uint32) (found_semicolon - thd->query() - 1),
                           thd->charset());
          /* Actually execute the query */
          if (found_semicolon)
          {
            lex->safe_to_cache_query= 0;
            thd->server_status|= SERVER_MORE_RESULTS_EXISTS;
          }
          lex->set_trg_event_type_for_tables();
          MYSQL_QUERY_EXEC_START(thd->query(),
                                 thd->thread_id,
                                 thd->get_db(),
                                 &thd->security_ctx->priv_user[0],
                                 (char *) thd->security_ctx->host_or_ip,
                                 0);

          int error __attribute__((unused));
          error= mysql_execute_command(thd);
          MYSQL_QUERY_EXEC_DONE(error);
	}
      }
    }
    else
    {
      /* Instrument this broken statement as "statement/sql/error" */
      thd->m_statement_psi=
        MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                               sql_statement_info[SQLCOM_END].m_key);
      DBUG_ASSERT(thd->is_error());
      DBUG_PRINT("info",("Command aborted. Fatal_error: %d",
			 thd->is_fatal_error));

      query_cache_abort(thd, &thd->query_cache_tls);
    }
    THD_STAGE_INFO(thd, stage_freeing_items);
    sp_cache_enforce_limit(thd->sp_proc_cache, stored_program_cache_size);
    sp_cache_enforce_limit(thd->sp_func_cache, stored_program_cache_size);
    sp_cache_enforce_limit(thd->sp_package_spec_cache, stored_program_cache_size);
    sp_cache_enforce_limit(thd->sp_package_body_cache, stored_program_cache_size);
    thd->end_statement();
    thd->Item_change_list::rollback_item_tree_changes();
    thd->cleanup_after_query();
  }
  else
  {
    /* Update statistics for getting the query from the cache */
    thd->lex->sql_command= SQLCOM_SELECT;
    thd->m_statement_psi=
      MYSQL_REFINE_STATEMENT(thd->m_statement_psi,
                             sql_statement_info[SQLCOM_SELECT].m_key);
    status_var_increment(thd->status_var.com_stat[SQLCOM_SELECT]);
    thd->update_stats();
#ifdef WITH_WSREP
    if (WSREP_CLIENT(thd))
    {
      thd->wsrep_sync_wait_gtid= WSREP_GTID_UNDEFINED;
    }
#endif /* WITH_WSREP */
  }
  DBUG_VOID_RETURN;
}


#ifdef HAVE_REPLICATION
/*
  Usable by the replication SQL thread only: just parse a query to know if it
  can be ignored because of replicate-*-table rules.

  @retval
    0	cannot be ignored
  @retval
    1	can be ignored
*/

bool mysql_test_parse_for_slave(THD *thd, char *rawbuf, uint length)
{
  LEX *lex= thd->lex;
  bool error= 0;
  DBUG_ENTER("mysql_test_parse_for_slave");

  Parser_state parser_state;
  if (likely(!(error= parser_state.init(thd, rawbuf, length))))
  {
    lex_start(thd);
    thd->reset_for_next_command();

    if (!parse_sql(thd, & parser_state, NULL, true) &&
        all_tables_not_ok(thd, lex->first_select_lex()->table_list.first))
      error= 1;                  /* Ignore question */
    thd->end_statement();
  }
  thd->cleanup_after_query();
  DBUG_RETURN(error);
}
#endif


bool
add_proc_to_list(THD* thd, Item *item)
{
  ORDER *order;
  Item	**item_ptr;

  if (unlikely(!(order= (ORDER *) thd->alloc(sizeof(ORDER)+sizeof(Item*)))))
    return 1;
  item_ptr = (Item**) (order+1);
  *item_ptr= item;
  order->item=item_ptr;
  thd->lex->proc_list.insert(order, &order->next);
  return 0;
}


/**
  save order by and tables in own lists.
*/

bool add_to_list(THD *thd, SQL_I_List<ORDER> &list, Item *item,bool asc)
{
  ORDER *order;
  DBUG_ENTER("add_to_list");
  if (unlikely(!(order= thd->alloc<ORDER>(1))))
    DBUG_RETURN(1);
  order->item_ptr= item;
  order->item= &order->item_ptr;
  order->direction= (asc ? ORDER::ORDER_ASC : ORDER::ORDER_DESC);
  order->used=0;
  order->counter_used= 0;
  order->fast_field_copier_setup= 0; 
  list.insert(order, &order->next);
  DBUG_RETURN(0);
}


/**
  Add a table to list of used tables.

  @param table		Table to add
  @param alias		alias for table (or null if no alias)
  @param table_options	A set of the following bits:
                         - TL_OPTION_UPDATING : Table will be updated
                         - TL_OPTION_FORCE_INDEX : Force usage of index
                         - TL_OPTION_ALIAS : an alias in multi table DELETE
  @param lock_type	How table should be locked
  @param mdl_type       Type of metadata lock to acquire on the table.
  @param use_index	List of indexed used in USE INDEX
  @param ignore_index	List of indexed used in IGNORE INDEX

  @retval
      0		Error
  @retval
    \#	Pointer to TABLE_LIST element added to the total table list


  This method can be called in contexts when the "table" argument has a longer
  life cycle than TABLE_LIST and belongs to a different MEM_ROOT than
  the current THD::mem_root.

  For example, it's called from Table_ident::resolve_table_rowtype_ref()
  during sp_head::rcontext_create() during a CALL statement.
  "table" in this case belongs to sp_pcontext, which must stay valid
  (inside its SP cache sp_head entry) after the end of the current statement.

  Let's allocate normalized copies of table.db and table.table on the current
  THD::mem_root and store them in the TABLE_LIST.

  We should not touch "table" and replace table.db and table.table to their
  normalized copies allocated on the current THD::mem_root, because it'll be
  freed at the end of the current statement, while table.db and table.table
  should stay valid. Let's keep them in the original state.

*/

TABLE_LIST *st_select_lex::add_table_to_list(THD *thd,
					     Table_ident *table,
					     const LEX_CSTRING *alias,
					     ulong table_options,
					     thr_lock_type lock_type,
					     enum_mdl_type mdl_type,
					     List<Index_hint> *index_hints_arg,
                                             List<String> *partition_names,
                                             LEX_STRING *option)
{
  DBUG_ENTER("add_table_to_list");
  DBUG_PRINT("enter", ("Table '%s' (%p)  Select %p (%u)",
                        (alias ? alias->str : table->table.str),
                        table,
                        this, select_number));
  DBUG_ASSERT(!is_service_select  || (table_options & TL_OPTION_SEQUENCE));

  if (unlikely(!table))
    DBUG_RETURN(0);				// End of memory
  if (!(table_options & TL_OPTION_ALIAS) &&
      unlikely(Lex_ident_table::check_name(table->table, FALSE)))
  {
    my_error(ER_WRONG_TABLE_NAME, MYF(0), table->table.str);
    DBUG_RETURN(0);
  }

  if (unlikely(table->is_derived_table() == FALSE && table->db.str &&
               !(table_options & TL_OPTION_TABLE_FUNCTION) &&
               Lex_ident_db::check_name_with_error(table->db)))
    DBUG_RETURN(0);

  Lex_ident_db db{0, 0};
  bool fqtn= false;
  LEX *lex= thd->lex;
  if (table->db.str)
  {
    fqtn= TRUE;
    db= Lex_ident_db(table->db);
  }
  else if (!lex->with_cte_resolution && lex->copy_db_to(&db))
    DBUG_RETURN(0);
  else
    fqtn= FALSE;
  bool info_schema= (db.is_null() || db.is_empty())
	            ? false : is_infoschema_db(&db);
  if (!table->sel && info_schema &&
      (table_options & TL_OPTION_UPDATING) &&
      /* Special cases which are processed by commands itself */
      lex->sql_command != SQLCOM_CHECK &&
      lex->sql_command != SQLCOM_CHECKSUM)
  {
    my_error(ER_DBACCESS_DENIED_ERROR, MYF(0),
             thd->security_ctx->priv_user,
             thd->security_ctx->priv_host,
             INFORMATION_SCHEMA_NAME.str);
    DBUG_RETURN(0);
  }

  Lex_ident_table alias_str= alias ? Lex_ident_table(*alias) :
                                     Lex_ident_table(table->table);
  DBUG_ASSERT(alias_str.str);
  if (!alias)                            /* Alias is case sensitive */
  {
    if (unlikely(table->sel))
    {
      my_message(ER_DERIVED_MUST_HAVE_ALIAS,
                 ER_THD(thd, ER_DERIVED_MUST_HAVE_ALIAS), MYF(0));
      DBUG_RETURN(0);
    }
    /* alias_str points to table->table;  Let's make a copy */
    if (unlikely(!(alias_str.str= (char*) thd->memdup(alias_str.str, alias_str.length+1))))
      DBUG_RETURN(0);
  }

  bool has_alias_ptr= alias != nullptr;
  void *memregion= thd->alloc(sizeof(TABLE_LIST));
  TABLE_LIST *ptr= new (memregion) TABLE_LIST(thd, db, fqtn, alias_str,
                                              has_alias_ptr, table, lock_type,
                                              mdl_type, table_options,
                                              info_schema, this,
                                              index_hints_arg, option);
  if (!ptr->table_name.str)
    DBUG_RETURN(0); // EOM

  /* check that used name is unique. Sequences are ignored */
  if (lock_type != TL_IGNORE && !ptr->sequence)
  {
    TABLE_LIST *first_table= table_list.first;
    if (lex->sql_command == SQLCOM_CREATE_VIEW)
      first_table= first_table ? first_table->next_local : NULL;
    for (TABLE_LIST *tables= first_table ;
	 tables ;
	 tables=tables->next_local)
    {
      if (unlikely(alias_str.streq(tables->alias) &&
                   (tables->db.str == any_db.str || ptr->db.str == any_db.str ||
                    !cmp(&ptr->db, &tables->db)) &&
                   !tables->sequence))
      {
        my_error(ER_NONUNIQ_TABLE, MYF(0), alias_str.str); /* purecov: tested */
        DBUG_RETURN(0);				/* purecov: tested */
      }
    }
  }
  /* Store the table reference preceding the current one. */
  TABLE_LIST *UNINIT_VAR(previous_table_ref); /* The table preceding the current one. */
  if (table_list.elements > 0 && likely(!ptr->sequence))
  {
    /*
      table_list.next points to the last inserted TABLE_LIST->next_local'
      element
      We don't use the offsetof() macro here to avoid warnings from gcc
    */
    previous_table_ref= (TABLE_LIST*) ((char*) table_list.next -
                                       ((char*) &(ptr->next_local) -
                                        (char*) ptr));
    /*
      Set next_name_resolution_table of the previous table reference to point
      to the current table reference. In effect the list
      TABLE_LIST::next_name_resolution_table coincides with
      TABLE_LIST::next_local. Later this may be changed in
      store_top_level_join_columns() for NATURAL/USING joins.
    */
    previous_table_ref->next_name_resolution_table= ptr;
  }

  /*
    Link the current table reference in a local list (list for current select).
    Notice that as a side effect here we set the next_local field of the
    previous table reference to 'ptr'. Here we also add one element to the
    list 'table_list'.
    We don't store sequences into the local list to hide them from INSERT
    and SELECT.
  */
  if (likely(!ptr->sequence))
    table_list.insert(ptr, &ptr->next_local);
  ptr->next_name_resolution_table= NULL;
#ifdef WITH_PARTITION_STORAGE_ENGINE
  ptr->partition_names= partition_names;
#endif /* WITH_PARTITION_STORAGE_ENGINE */
  /* Link table in global list (all used tables) */
  lex->add_to_query_tables(ptr);

  // Pure table aliases do not need to be locked:
  if (ptr->db.str && !(table_options & TL_OPTION_ALIAS))
  {
    MDL_REQUEST_INIT(&ptr->mdl_request, MDL_key::TABLE, ptr->db.str,
                     ptr->table_name.str, mdl_type, MDL_TRANSACTION);
  }
  DBUG_RETURN(ptr);
}


/**
  Initialize a new table list for a nested join.

    The function initializes a structure of the TABLE_LIST type
    for a nested join. It sets up its nested join list as empty.
    The created structure is added to the front of the current
    join list in the st_select_lex object. Then the function
    changes the current nest level for joins to refer to the newly
    created empty list after having saved the info on the old level
    in the initialized structure.

  @param thd         current thread

  @retval
    0   if success
  @retval
    1   otherwise
*/

bool st_select_lex::init_nested_join(THD *thd)
{
  TABLE_LIST *ptr;
  NESTED_JOIN *nested_join;
  DBUG_ENTER("init_nested_join");

  if (unlikely(!(ptr= (TABLE_LIST*) thd->calloc(ALIGN_SIZE(sizeof(TABLE_LIST))+
                                                sizeof(NESTED_JOIN)))))
    DBUG_RETURN(1);
  nested_join= ptr->nested_join=
    ((NESTED_JOIN*) ((uchar*) ptr + ALIGN_SIZE(sizeof(TABLE_LIST))));

  ptr->embedding= embedding;
  ptr->join_list= join_list;
  ptr->alias.str="(nested_join)";
  ptr->alias.length= sizeof("(nested_join)")-1;
  embedding= ptr;
  join_list= &nested_join->join_list;
  join_list->empty();
  DBUG_RETURN(0);
}


/**
  End a nested join table list.

    The function returns to the previous join nest level.
    If the current level contains only one member, the function
    moves it one level up, eliminating the nest.

  @param thd         current thread

  @return
    - Pointer to TABLE_LIST element added to the total table list, if success
    - 0, otherwise
*/

TABLE_LIST *st_select_lex::end_nested_join(THD *thd)
{
  TABLE_LIST *ptr;
  NESTED_JOIN *nested_join;
  DBUG_ENTER("end_nested_join");

  DBUG_ASSERT(embedding);
  ptr= embedding;
  join_list= ptr->join_list;
  embedding= ptr->embedding;
  nested_join= ptr->nested_join;
  if (nested_join->join_list.elements == 1)
  {
    TABLE_LIST *embedded= nested_join->join_list.head();
    join_list->pop();
    embedded->join_list= join_list;
    embedded->embedding= embedding;
    join_list->push_front(embedded, thd->mem_root);
    ptr= embedded;
    embedded->lifted= 1;
  }
  else if (nested_join->join_list.elements == 0)
  {
    join_list->pop();
    ptr= 0;                                     // return value
  }
  DBUG_RETURN(ptr);
}


/**
  Nest last join operation.

    The function nest last join operation as if it was enclosed in braces.

  @param thd         current thread

  @retval
    0  Error
  @retval
    \#  Pointer to TABLE_LIST element created for the new nested join
*/

TABLE_LIST *st_select_lex::nest_last_join(THD *thd)
{
  TABLE_LIST *ptr;
  NESTED_JOIN *nested_join;
  List<TABLE_LIST> *embedded_list;
  DBUG_ENTER("nest_last_join");

  TABLE_LIST *head= join_list->head();
  if (head->nested_join && (head->nested_join->nest_type & REBALANCED_NEST))
  {
    head= join_list->pop();
    DBUG_RETURN(head);
  }

  if (unlikely(!(ptr= (TABLE_LIST*) thd->calloc(ALIGN_SIZE(sizeof(TABLE_LIST))+
                                                sizeof(NESTED_JOIN)))))
    DBUG_RETURN(0);
  nested_join= ptr->nested_join=
    ((NESTED_JOIN*) ((uchar*) ptr + ALIGN_SIZE(sizeof(TABLE_LIST))));

  ptr->embedding= embedding;
  ptr->join_list= join_list;
  ptr->alias.str= "(nest_last_join)";
  ptr->alias.length= sizeof("(nest_last_join)")-1;
  embedded_list= &nested_join->join_list;
  embedded_list->empty();
  nested_join->nest_type= JOIN_OP_NEST;

  for (uint i=0; i < 2; i++)
  {
    TABLE_LIST *table= join_list->pop();
    if (unlikely(!table))
      DBUG_RETURN(NULL);
    table->join_list= embedded_list;
    table->embedding= ptr;
    embedded_list->push_back(table);
    if (table->natural_join)
    {
      ptr->is_natural_join= TRUE;
      /*
        If this is a JOIN ... USING, move the list of joined fields to the
        table reference that describes the join.
      */
      if (prev_join_using)
        ptr->join_using_fields= prev_join_using;
    }
  }
  nested_join->used_tables= nested_join->not_null_tables= (table_map) 0;
  DBUG_RETURN(ptr);
}


/**
  Add a table to the current join list.

    The function puts a table in front of the current join list
    of st_select_lex object.
    Thus, joined tables are put into this list in the reverse order
    (the most outer join operation follows first).

  @param table       the table to add

  @return
    None
*/

void st_select_lex::add_joined_table(TABLE_LIST *table)
{
  DBUG_ENTER("add_joined_table");
  join_list->push_front(table, parent_lex->thd->mem_root);
  table->join_list= join_list;
  table->embedding= embedding;
  DBUG_VOID_RETURN;
}


/**
  @brief
    Create a node for JOIN/INNER JOIN/CROSS JOIN/STRAIGHT_JOIN operation

  @param left_op     the node for the left operand constructed by the parser
  @param right_op    the node for the right operand constructed by the parser
  @param straight_fl TRUE if STRAIGHT_JOIN is used

  @retval
    false on success
    true  otherwise

  @details

    JOIN operator can be left-associative with other join operators in one
    context and right-associative in another context.

    In this query
      SELECT * FROM t1 JOIN t2 LEFT JOIN t3 ON t2.a=t3.a  (Q1)
    JOIN is left-associative and the query Q1 is interpreted as
      SELECT * FROM (t1 JOIN t2) LEFT JOIN t3 ON t2.a=t3.a.
    While in this query
      SELECT * FROM t1 JOIN t2 LEFT JOIN t3 ON t2.a=t3.a ON t1.b=t2.b (Q2)
    JOIN is right-associative and the query Q2 is interpreted as
      SELECT * FROM t1 JOIN (t2 LEFT JOIN t3 ON t2.a=t3.a) ON t1.b=t2.b

    JOIN is right-associative if it is used with ON clause or with USING clause.
    Otherwise it is left-associative.
    When parsing a join expression with JOIN operator we can't determine
    whether this operation left or right associative until either we read the
    corresponding ON clause or we reach the end of the expression. This creates
    a problem for the parser to build a proper internal representation of the
    used join expression.

    For Q1 and Q2 the trees representing the used join expressions look like

            LJ - ON                   J - ON
           /  \                      / \
          J    t3   (TQ1)          t1   LJ - ON      (TQ2)
         / \                           /  \
       t1   t2                       t2    t3

    To build TQ1 the parser has to reduce the expression for JOIN right after
    it has read the reference to t2. To build TQ2 the parser reduces JOIN
    when he has read the whole join expression. There is no way to determine
    whether an early reduction is needed until the whole join expression is
    read.
    A solution here is always to do a late reduction. In this case the parser
    first builds an incorrect tree TQ1* that has to be rebalanced right after
    it has been constructed.

             J                               LJ - ON
            / \                             /  \
          t1   LJ - ON    (TQ1*)    =>     J    t3
              /  \                        / \
            t2    t3                    t1   t2

    Actually the transformation is performed over the nodes t1 and LJ before the
    node for J is created in the function st_select_lex::add_cross_joined_table.
    The function creates a node for J which replaces the node t2. Then it
    attaches the nodes t1 and t2 to this newly created node. The node LJ becomes
    the top node of the tree.

    For the query
      SELECT * FROM t1 JOIN t2 RIGHT JOIN t3 ON t2.a=t3.a  (Q3)
    the transformation looks slightly differently because the parser
    replaces the RIGHT JOIN tree for an equivalent LEFT JOIN tree.

             J                               LJ - ON
            / \                             /  \
          t1   LJ - ON    (TQ3*)    =>    t3    J
              /  \                             / \
            t3    t2                         t1   t2

    With several left associative JOINs
      SELECT * FROM t1 JOIN t2 JOIN t3 LEFT JOIN t4 ON t3.a=t4.a (Q4)
    the newly created node for JOIN replaces the left most node of the tree:

          J1                         LJ - ON
         /  \                       /  \
       t1    J2                    J2   t4
            /  \          =>      /  \
           t2  LJ - ON          J1    t3
              /  \             /  \
            t3   t4          t1    t2

    Here's another example:
      SELECT *
      FROM t1 JOIN t2 LEFT JOIN t3 JOIN t4 ON t3.a=t4.a ON t2.b=t3.b (Q5)

          J                       LJ - ON
         / \                     /   \
       t1   LJ - ON             J     J - ON
           /  \          =>    / \   / \
         t2    J - ON         t1 t2 t3 t4
              / \
            t3   t4

    If the transformed nested join node node is a natural join node like in
    the following query
      SELECT * FROM t1 JOIN t2 LEFT JOIN t3 USING(a)  (Q6)
    the transformation additionally has to take care about setting proper
    references in the field natural_join for both operands of the natural
    join operation.

    The queries that combine comma syntax for join operation with
    JOIN expression require a special care. Consider the query
      SELECT * FROM t1, t2 JOIN t3 LEFT JOIN t4 ON t3.a=t4.a (Q7)
    This query is equivalent to the query
      SELECT * FROM (t1, t2) JOIN t3 LEFT JOIN t4 ON t3.a=t4.a
    The latter is transformed in the same way as query Q1

             J                               LJ - ON
            / \                             /  \
      (t1,t2)  LJ - ON      =>             J    t4
              /  \                        / \
            t3    t4                (t1,t2)   t3

    A transformation similar to the transformation for Q3 is done for
    the following query with RIGHT JOIN
      SELECT * FROM t1, t2 JOIN t3 RIGHT JOIN t4 ON t3.a=t4.a (Q8)

             J                               LJ - ON
            / \                             /  \
          t3   LJ - ON      =>            t4    J
              /  \                             / \
            t4   (t1,t2)                 (t1,t2)  t3

    The function also has to change the name resolution context for ON
    expressions used in the transformed join expression to take into
    account the tables of the left_op node.

  TODO:
    A more elegant solution would be to implement the transformation that
    eliminates nests for cross join operations. For Q7 it would work like this:

             J                               LJ - ON
            / \                             /  \
      (t1,t2)  LJ - ON      =>     (t1,t2,t3)   t4
              /  \
            t3    t4

    For Q8 with RIGHT JOIN the transformation would work similarly:

             J                               LJ - ON
            / \                             /  \
          t3   LJ - ON      =>            t4   (t1,t2,t3)
              /  \
            t4   (t1,t2)

*/

bool st_select_lex::add_cross_joined_table(TABLE_LIST *left_op,
                                           TABLE_LIST *right_op,
                                           bool straight_fl)
{
  DBUG_ENTER("add_cross_joined_table");
  THD *thd= parent_lex->thd;
  if (!(right_op->nested_join &&
	(right_op->nested_join->nest_type & JOIN_OP_NEST)))
  {
    /*
      This handles the cases when the right operand is not a nested join.
      like in queries
        SELECT * FROM t1 JOIN t2;
        SELECT * FROM t1 LEFT JOIN t2 ON t1.a=t2.a JOIN t3
    */
    add_joined_table(left_op);
    add_joined_table(right_op);
    right_op->straight= straight_fl;
    DBUG_RETURN(false);
  }

  TABLE_LIST *tbl;
  List<TABLE_LIST> *right_op_jl= right_op->join_list;
  TABLE_LIST *cj_nest;

  /*
    Create the node NJ for a new nested join for the future inclusion
    of left_op in it. Initially the nest is empty.
  */
  if (unlikely(!(cj_nest=
                 (TABLE_LIST*) thd->calloc(ALIGN_SIZE(sizeof(TABLE_LIST))+
                                           sizeof(NESTED_JOIN)))))
    DBUG_RETURN(true);
  cj_nest->nested_join=
    ((NESTED_JOIN*) ((uchar*) cj_nest + ALIGN_SIZE(sizeof(TABLE_LIST))));
  cj_nest->nested_join->nest_type= JOIN_OP_NEST;
  List<TABLE_LIST> *cjl=  &cj_nest->nested_join->join_list;
  cjl->empty();

  List<TABLE_LIST> *jl= &right_op->nested_join->join_list;
  DBUG_ASSERT(jl->elements == 2);
  /* Look for the left most node tbl of the right_op tree */
  for ( ; ; )
  {
    TABLE_LIST *pair_tbl= 0;  /* useful only for operands of natural joins */

    List_iterator<TABLE_LIST> li(*jl);
    tbl= li++;

    /* Expand name resolution context */
    Name_resolution_context *on_context;
    if ((on_context= tbl->on_context))
    {
      on_context->first_name_resolution_table=
        left_op->first_leaf_for_name_resolution();
    }

    if (!(tbl->outer_join & JOIN_TYPE_RIGHT))
    {
      pair_tbl= tbl;
      tbl= li++;
    }
    if (tbl->nested_join &&
        tbl->nested_join->nest_type & JOIN_OP_NEST)
    {
      jl= &tbl->nested_join->join_list;
      continue;
    }

    /* Replace the tbl node in the tree for the newly created NJ node */
    cj_nest->outer_join= tbl->outer_join;
    cj_nest->on_expr= tbl->on_expr;
    cj_nest->embedding= tbl->embedding;
    cj_nest->join_list= jl;
    cj_nest->alias.str= "(nest_last_join)";
    cj_nest->alias.length= sizeof("(nest_last_join)")-1;
    li.replace(cj_nest);

    /*
      If tbl is an operand of a natural join set properly the references
      in the fields natural_join for both operands of the operation.
    */
    if(tbl->embedding && tbl->embedding->is_natural_join)
    {
      if (!pair_tbl)
        pair_tbl= li++;
      pair_tbl->natural_join= cj_nest;
      cj_nest->natural_join= pair_tbl;
    }
    break;
  }

  /* Attach tbl as the right operand of NJ */
  if (unlikely(cjl->push_back(tbl, thd->mem_root)))
    DBUG_RETURN(true);
  tbl->outer_join= 0;
  tbl->on_expr= 0;
  tbl->straight= straight_fl;
  tbl->natural_join= 0;
  tbl->embedding= cj_nest;
  tbl->join_list= cjl;

  /* Add left_op as the left operand of NJ */
  if (unlikely(cjl->push_back(left_op, thd->mem_root)))
    DBUG_RETURN(true);
  left_op->embedding= cj_nest;
  left_op->join_list= cjl;

  /*
    Mark right_op as a rebalanced nested join in order not to
    create a new top level nested join node.
  */
  right_op->nested_join->nest_type|= REBALANCED_NEST;
  if (unlikely(right_op_jl->push_front(right_op)))
    DBUG_RETURN(true);
  DBUG_RETURN(false);
}


/**
  Convert a right join into equivalent left join.

    The function takes the current join list t[0],t[1] ... and
    effectively converts it into the list t[1],t[0] ...
    Although the outer_join flag for the new nested table contains
    JOIN_TYPE_RIGHT, it will be handled as the inner table of a left join
    operation.

  EXAMPLES
  @verbatim
    SELECT * FROM t1 RIGHT JOIN t2 ON on_expr =>
      SELECT * FROM t2 LEFT JOIN t1 ON on_expr

    SELECT * FROM t1,t2 RIGHT JOIN t3 ON on_expr =>
      SELECT * FROM t1,t3 LEFT JOIN t2 ON on_expr

    SELECT * FROM t1,t2 RIGHT JOIN (t3,t4) ON on_expr =>
      SELECT * FROM t1,(t3,t4) LEFT JOIN t2 ON on_expr

    SELECT * FROM t1 LEFT JOIN t2 ON on_expr1 RIGHT JOIN t3  ON on_expr2 =>
      SELECT * FROM t3 LEFT JOIN (t1 LEFT JOIN t2 ON on_expr2) ON on_expr1
   @endverbatim

  @param thd         current thread

  @return
    - Pointer to the table representing the inner table, if success
    - 0, otherwise
*/

TABLE_LIST *st_select_lex::convert_right_join()
{
  TABLE_LIST *tab2= join_list->pop();
  TABLE_LIST *tab1= join_list->pop();
  DBUG_ENTER("convert_right_join");

  join_list->push_front(tab2, parent_lex->thd->mem_root);
  join_list->push_front(tab1, parent_lex->thd->mem_root);
  tab1->outer_join|= JOIN_TYPE_RIGHT;

  DBUG_RETURN(tab1);
}


void st_select_lex::prepare_add_window_spec(THD *thd)
{
  LEX *lex= thd->lex;
  save_group_list= group_list;
  save_order_list= order_list;
  lex->win_ref= NULL;
  lex->win_frame= NULL;
  lex->frame_top_bound= NULL;
  lex->frame_bottom_bound= NULL;
  group_list.empty();
  order_list.empty();
}

bool st_select_lex::add_window_def(THD *thd,
                                   LEX_CSTRING *win_name,
                                   LEX_CSTRING *win_ref,
                                   SQL_I_List<ORDER> win_partition_list,
                                   SQL_I_List<ORDER> win_order_list,
                                   Window_frame *win_frame)
{
  SQL_I_List<ORDER> *win_part_list_ptr=
    new (thd->mem_root) SQL_I_List<ORDER> (win_partition_list);
  SQL_I_List<ORDER> *win_order_list_ptr=
    new (thd->mem_root) SQL_I_List<ORDER> (win_order_list);
  if (!(win_part_list_ptr && win_order_list_ptr))
    return true;
  Window_def *win_def= new (thd->mem_root) Window_def(win_name,
                                                      win_ref,
                                                      win_part_list_ptr,
                                                      win_order_list_ptr,
                                                      win_frame);
  group_list= save_group_list;
  order_list= save_order_list;
  if (parsing_place != SELECT_LIST)
  {
    fields_in_window_functions+= win_part_list_ptr->elements +
                                 win_order_list_ptr->elements;
  }
  win_def->win_spec_number= window_specs.elements;
  return (win_def == NULL || window_specs.push_back(win_def));
}

bool st_select_lex::add_window_spec(THD *thd, 
                                    LEX_CSTRING *win_ref,
                                    SQL_I_List<ORDER> win_partition_list,
                                    SQL_I_List<ORDER> win_order_list,
                                    Window_frame *win_frame)
{
  SQL_I_List<ORDER> *win_part_list_ptr=
    new (thd->mem_root) SQL_I_List<ORDER> (win_partition_list);
  SQL_I_List<ORDER> *win_order_list_ptr=
    new (thd->mem_root) SQL_I_List<ORDER> (win_order_list);
  if (!(win_part_list_ptr && win_order_list_ptr))
    return true;
  Window_spec *win_spec= new (thd->mem_root) Window_spec(win_ref,
                                                         win_part_list_ptr,
                                                         win_order_list_ptr,
                                                         win_frame);
  group_list= save_group_list;
  order_list= save_order_list;
  if (parsing_place != SELECT_LIST)
  {
    fields_in_window_functions+= win_part_list_ptr->elements +
                                 win_order_list_ptr->elements;
  }
  thd->lex->win_spec= win_spec;
  win_spec->win_spec_number= window_specs.elements;
  return (win_spec == NULL || window_specs.push_back(win_spec));
}

/**
  Set lock for all tables in current select level.

  @param lock_type		Lock to set for tables
  @param skip_locked		(SELECT {FOR UPDATE/LOCK IN SHARED MODE} SKIP LOCKED)

  @note
    If lock is a write lock, then tables->updating is set 1
    This is to get tables_ok to know that the table is updated by the
    query
*/

void st_select_lex::set_lock_for_tables(thr_lock_type lock_type, bool for_update,
					bool skip_locked_arg)
{
  DBUG_ENTER("set_lock_for_tables");
  DBUG_PRINT("enter", ("lock_type: %d  for_update: %d  skip_locked %d",
                       lock_type, for_update, skip_locked));
  skip_locked= skip_locked_arg;
  for (TABLE_LIST *tables= table_list.first;
       tables;
       tables= tables->next_local)
  {
    tables->lock_type= lock_type;
    tables->skip_locked= skip_locked;
    tables->updating=  for_update;

    if (tables->db.length)
      tables->mdl_request.set_type((lock_type >= TL_FIRST_WRITE) ?
                                   MDL_SHARED_WRITE : MDL_SHARED_READ);
  }
  DBUG_VOID_RETURN;
}


/**
  Create a fake SELECT_LEX for a unit.

    The method create a fake SELECT_LEX object for a unit.
    This object is created for any union construct containing a union
    operation and also for any single select union construct of the form
    @verbatim
    (SELECT ... ORDER BY order_list [LIMIT n]) ORDER BY ... 
    @endvarbatim
    or of the form
    @varbatim
    (SELECT ... ORDER BY LIMIT n) ORDER BY ...
    @endvarbatim
  
  @param thd_arg		   thread handle

  @note
    The object is used to retrieve rows from the temporary table
    where the result on the union is obtained.

  @retval
    1     on failure to create the object
  @retval
    0     on success
*/

bool st_select_lex_unit::add_fake_select_lex(THD *thd_arg)
{
  SELECT_LEX *first_sl= first_select();
  DBUG_ENTER("st_select_lex_unit::add_fake_select_lex");
  DBUG_ASSERT(!fake_select_lex);

  if (!(fake_select_lex= new (thd_arg->mem_root) SELECT_LEX()))
      DBUG_RETURN(1);
  fake_select_lex->include_standalone(this, 
                                      (SELECT_LEX_NODE**)&fake_select_lex);
  fake_select_lex->select_number= FAKE_SELECT_LEX_ID;
  fake_select_lex->parent_lex= thd_arg->lex; /* Used in init_query. */
  fake_select_lex->make_empty_select();
  fake_select_lex->set_linkage(GLOBAL_OPTIONS_TYPE);

  fake_select_lex->no_table_names_allowed= 1;

  fake_select_lex->context.outer_context=first_sl->context.outer_context;
  /* allow item list resolving in fake select for ORDER BY */
  fake_select_lex->context.resolve_in_select_list= TRUE;
  fake_select_lex->context.select_lex= fake_select_lex;  

  fake_select_lex->nest_level_base= first_select()->nest_level_base;
  if (fake_select_lex->set_nest_level(first_select()->nest_level))
    DBUG_RETURN(1);

  if (!is_unit_op())
  {
    /* 
      This works only for 
      (SELECT ... ORDER BY list [LIMIT n]) ORDER BY order_list [LIMIT m],
      (SELECT ... LIMIT n) ORDER BY order_list [LIMIT m]
      just before the parser starts processing order_list
    */ 
    fake_select_lex->no_table_names_allowed= 1;
    thd_arg->lex->current_select= fake_select_lex;
  }
  //thd_arg->lex->pop_context("add fake");
  DBUG_RETURN(0);
}


/**
  Push a new name resolution context for a JOIN ... ON clause to the
  context stack of a query block.

    Create a new name resolution context for a JOIN ... ON clause,
    set the first and last leaves of the list of table references
    to be used for name resolution, and push the newly created
    context to the stack of contexts of the query.

  @param thd       pointer to current thread
  @param left_op   left  operand of the JOIN
  @param right_op  rigth operand of the JOIN

  @seealso
    push_table_function_arg_context() serves similar purpose for table
    functions

  @retval
    FALSE  if all is OK
  @retval
    TRUE   if a memory allocation error occurred
*/

bool
push_new_name_resolution_context(THD *thd,
                                 TABLE_LIST *left_op, TABLE_LIST *right_op)
{
  Name_resolution_context *on_context;
  if (!(on_context= new (thd->mem_root) Name_resolution_context))
    return TRUE;
  on_context->first_name_resolution_table=
    left_op->first_leaf_for_name_resolution();
  on_context->last_name_resolution_table=
    right_op->last_leaf_for_name_resolution();
  LEX *lex= thd->lex;
  on_context->select_lex = lex->current_select;
  st_select_lex *outer_sel= lex->parser_current_outer_select();
  on_context->outer_context = outer_sel ? &outer_sel->context : 0;
  return lex->push_context(on_context);
}


/**
  Fix condition which contains only field (f turns to  f <> 0 )
    or only contains the function NOT field (not f turns to  f == 0)

  @param cond            The condition to fix

  @return fixed condition
*/

Item *normalize_cond(THD *thd, Item *cond)
{
  if (cond)
  {
    Item::Type type= cond->type();
    if (type == Item::FIELD_ITEM || type == Item::REF_ITEM)
    {
      item_base_t is_cond_flag= cond->base_flags & item_base_t::IS_COND;
      cond->base_flags&= ~item_base_t::IS_COND;
      cond= new (thd->mem_root) Item_func_istrue(thd, cond);
      if (cond)
        cond->base_flags|= is_cond_flag;
    }
    else
    {
      if (type == Item::FUNC_ITEM)
      {
        Item_func *func_item= (Item_func *)cond;
        if (func_item->functype() == Item_func::NOT_FUNC)
        {
          Item *arg= func_item->arguments()[0];
          if (arg->type() == Item::FIELD_ITEM ||
              arg->type() == Item::REF_ITEM)
            cond= new (thd->mem_root) Item_func_isfalse(thd, arg);
        }
      }
    }
  }
  return cond;
}


/**
  Add an ON condition to the second operand of a JOIN ... ON.

    Add an ON condition to the right operand of a JOIN ... ON clause.

  @param b     the second operand of a JOIN ... ON
  @param expr  the condition to be added to the ON clause

  @retval
    FALSE  if there was some error
  @retval
    TRUE   if all is OK
*/

void add_join_on(THD *thd, TABLE_LIST *b, Item *expr)
{
  if (expr)
  {
    expr= normalize_cond(thd, expr);
    if (!b->on_expr)
      b->on_expr= expr;
    else
    {
      /*
        If called from the parser, this happens if you have both a
        right and left join. If called later, it happens if we add more
        than one condition to the ON clause.
      */
      b->on_expr= new (thd->mem_root) Item_cond_and(thd, b->on_expr,expr);
    }
    b->on_expr->top_level_item();
  }
}


/**
  Mark that there is a NATURAL JOIN or JOIN ... USING between two
  tables.

    This function marks that table b should be joined with a either via
    a NATURAL JOIN or via JOIN ... USING. Both join types are special
    cases of each other, so we treat them together. The function
    setup_conds() creates a list of equal condition between all fields
    of the same name for NATURAL JOIN or the fields in 'using_fields'
    for JOIN ... USING. The list of equality conditions is stored
    either in b->on_expr, or in JOIN::conds, depending on whether there
    was an outer join.

  EXAMPLE
  @verbatim
    SELECT * FROM t1 NATURAL LEFT JOIN t2
     <=>
    SELECT * FROM t1 LEFT JOIN t2 ON (t1.i=t2.i and t1.j=t2.j ... )

    SELECT * FROM t1 NATURAL JOIN t2 WHERE <some_cond>
     <=>
    SELECT * FROM t1, t2 WHERE (t1.i=t2.i and t1.j=t2.j and <some_cond>)

    SELECT * FROM t1 JOIN t2 USING(j) WHERE <some_cond>
     <=>
    SELECT * FROM t1, t2 WHERE (t1.j=t2.j and <some_cond>)
   @endverbatim

  @param a		  Left join argumentex
  @param b		  Right join argument
  @param using_fields    Field names from USING clause
*/

void add_join_natural(TABLE_LIST *a, TABLE_LIST *b, List<String> *using_fields,
                      SELECT_LEX *lex)
{
  b->natural_join= a;
  lex->prev_join_using= using_fields;
}


/**
  Find a thread by id and return it, locking it LOCK_thd_kill

  @param id  Identifier of the thread we're looking for
  @param query_id If true, search by query_id instead of thread_id

  @return NULL    - not found
          pointer - thread found, and its LOCK_thd_kill is locked.
*/

struct find_thread_callback_arg
{
  find_thread_callback_arg(longlong id_arg, bool query_id_arg):
    thd(0), id(id_arg), query_id(query_id_arg) {}
  THD *thd;
  longlong id;
  bool query_id;
};


static my_bool find_thread_callback(THD *thd, find_thread_callback_arg *arg)
{
  if (arg->id == (arg->query_id ? thd->query_id : (longlong) thd->thread_id))
  {
    mysql_mutex_lock(&thd->LOCK_thd_kill);    // Lock from delete
    arg->thd= thd;
    return 1;
  }
  return 0;
}


THD *find_thread_by_id(longlong id, bool query_id)
{
  find_thread_callback_arg arg(id, query_id);
  server_threads.iterate(find_thread_callback, &arg);
  return arg.thd;
}


/**
  kill one thread.

  @param thd			Thread class
  @param id                     Thread id or query id
  @param kill_signal            Should it kill the query or the connection
  @param type                   Type of id: thread id or query id
*/

static uint
kill_one_thread(THD *thd, my_thread_id id, killed_state kill_signal, killed_type type
#ifdef WITH_WSREP
                , bool &wsrep_high_priority
#endif
)
{
  THD *tmp;
  uint error= (type == KILL_TYPE_QUERY ? ER_NO_SUCH_QUERY : ER_NO_SUCH_THREAD);
  DBUG_ENTER("kill_one_thread");
  DBUG_PRINT("enter", ("id: %lld  signal: %d", (long long) id, kill_signal));
  tmp= find_thread_by_id(id, type == KILL_TYPE_QUERY);
  if (!tmp)
    DBUG_RETURN(error);
  DEBUG_SYNC(thd, "found_killee");
  if (tmp->get_command() != COM_DAEMON)
  {
    /*
      If we're SUPER, we can KILL anything, including system-threads.
      No further checks.

      KILLer: thd->security_ctx->user could in theory be NULL while
      we're still in "unauthenticated" state. This is a theoretical
      case (the code suggests this could happen, so we play it safe).

      KILLee: tmp->security_ctx->user will be NULL for system threads.
      We need to check so Jane Random User doesn't crash the server
      when trying to kill a) system threads or b) unauthenticated users'
      threads (Bug#43748).

      If user of both killer and killee are non-NULL, proceed with
      slayage if both are string-equal.

      It's ok to also kill DELAYED threads with KILL_CONNECTION instead of
      KILL_SYSTEM_THREAD; The difference is that KILL_CONNECTION may be
      faster and do a harder kill than KILL_SYSTEM_THREAD;
    */

    mysql_mutex_lock(&tmp->LOCK_thd_data); // Lock from concurrent usage

    if ((thd->security_ctx->master_access & PRIV_KILL_OTHER_USER_PROCESS) ||
        thd->security_ctx->user_matches(tmp->security_ctx))
    {
#ifdef WITH_WSREP
      if (wsrep_thd_is_BF(tmp, false) || tmp->wsrep_applier)
      {
        error= ER_KILL_DENIED_ERROR;
        wsrep_high_priority= true;
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_KILL_DENIED_ERROR,
                            "Thread %lld is %s and cannot be killed",
                            tmp->thread_id,
                           (tmp->wsrep_applier ? "wsrep applier" : "high priority"));
      }
      else
      {
        if (WSREP(tmp))
        {
          error = wsrep_kill_thd(thd, tmp, kill_signal);
        }
        else
        {
#endif /* WITH_WSREP */
        tmp->awake_no_mutex(kill_signal);
        error= 0;
#ifdef WITH_WSREP
        }
      }
#endif /* WITH_WSREP */
    }
    else
      error= (type == KILL_TYPE_QUERY ? ER_KILL_QUERY_DENIED_ERROR :
                                        ER_KILL_DENIED_ERROR);

    mysql_mutex_unlock(&tmp->LOCK_thd_data);
  }
  mysql_mutex_unlock(&tmp->LOCK_thd_kill);
  DBUG_PRINT("exit", ("%u", error));
  DBUG_RETURN(error);
}


/**
  kill all threads from one user

  @param thd			Thread class
  @param user_name		User name for threads we should kill
  @param only_kill_query        Should it kill the query or the connection

  @note
    If we can't kill all threads because of security issues, no threads
    are killed.
*/

struct kill_threads_callback_arg
{
  kill_threads_callback_arg(THD *thd_arg, LEX_USER *user_arg,
                            killed_state kill_signal_arg):
    thd(thd_arg), user(user_arg), kill_signal(kill_signal_arg), counter(0) {}
  THD *thd;
  LEX_USER *user;
  killed_state kill_signal;
  uint counter;
};


static my_bool kill_threads_callback(THD *thd, kill_threads_callback_arg *arg)
{
  if (thd->security_ctx->user)
  {
    /*
      Check that hostname (if given) and user name matches.

      host.str[0] == '%' means that host name was not given. See sql_yacc.yy
    */
    if (((arg->user->host.str[0] == '%' && !arg->user->host.str[1]) ||
         !strcmp(thd->security_ctx->host_or_ip, arg->user->host.str)) &&
        !strcmp(thd->security_ctx->user, arg->user->user.str))
    {
      if (!(arg->thd->security_ctx->master_access &
            PRIV_KILL_OTHER_USER_PROCESS) &&
          !arg->thd->security_ctx->user_matches(thd->security_ctx))
      {
        return MY_TEST(arg->thd->security_ctx->master_access & PROCESS_ACL);
      }
      arg->counter++;
      mysql_mutex_lock(&thd->LOCK_thd_kill); // Lock from delete
      mysql_mutex_lock(&thd->LOCK_thd_data);
      thd->awake_no_mutex(arg->kill_signal);
      mysql_mutex_unlock(&thd->LOCK_thd_data);
      mysql_mutex_unlock(&thd->LOCK_thd_kill);
    }
  }
  return 0;
}


static uint kill_threads_for_user(THD *thd, LEX_USER *user,
                                  killed_state kill_signal, ha_rows *rows)
{
  kill_threads_callback_arg arg(thd, user, kill_signal);
  DBUG_ENTER("kill_threads_for_user");
  DBUG_PRINT("enter", ("user: %s  signal: %u", user->user.str,
                       (uint) kill_signal));

  *rows= 0;

  if (server_threads.iterate(kill_threads_callback, &arg))
    DBUG_RETURN(ER_KILL_DENIED_ERROR);

  *rows= arg.counter;
  DBUG_RETURN(0);
}


/**
  kills a thread and sends response.

  @param thd                    Thread class
  @param id                     Thread id or query id
  @param state                  Should it kill the query or the connection
  @param type                   Type of id: thread id or query id
*/

static
void sql_kill(THD *thd, my_thread_id id, killed_state state, killed_type type)
{
#ifdef WITH_WSREP
  if (WSREP(thd))
  {
    if (!(thd->variables.option_bits & OPTION_GTID_BEGIN))
    {
      WSREP_DEBUG("implicit commit before KILL");
      /* Commit the normal transaction if one is active. */
      bool commit_failed= trans_commit_implicit(thd);
      /* Release metadata locks acquired in this transaction. */
      thd->release_transactional_locks();
      if (commit_failed || wsrep_after_statement(thd))
      {
        WSREP_DEBUG("implicit commit failed, MDL released: %lld",
                    (longlong) thd->thread_id);
        return;
      }
      thd->transaction->stmt.mark_trans_did_ddl();
    }
  }

  bool wsrep_high_priority= false;
#endif /* WITH_WSREP */
  uint error= kill_one_thread(thd, id, state, type
#ifdef WITH_WSREP
                              , wsrep_high_priority
#endif
                              );

  if (likely(!error))
  {
    if (!thd->killed)
      my_ok(thd);
    else
      thd->send_kill_message();
  }
#ifdef WITH_WSREP
  else if (wsrep_high_priority)
    my_printf_error(error, "This is a high priority thread/query and"
                    " cannot be killed without compromising"
                    " the consistency of the cluster", MYF(0));
#endif
  else
  {
    my_error(error, MYF(0), id);
  }
}


static void __attribute__ ((noinline))
sql_kill_user(THD *thd, LEX_USER *user, killed_state state)
{
  uint error;
  ha_rows rows;
#ifdef WITH_WSREP
  if (WSREP(thd))
  {
    if (!(thd->variables.option_bits & OPTION_GTID_BEGIN))
    {
      WSREP_DEBUG("implicit commit before KILL");
      /* Commit the normal transaction if one is active. */
      bool commit_failed= trans_commit_implicit(thd);
      /* Release metadata locks acquired in this transaction. */
      thd->release_transactional_locks();
      if (commit_failed || wsrep_after_statement(thd))
      {
        WSREP_DEBUG("implicit commit failed, MDL released: %lld",
                    (longlong) thd->thread_id);
        return;
      }
      thd->transaction->stmt.mark_trans_did_ddl();
    }
  }
#endif /* WITH_WSREP */
  switch (error= kill_threads_for_user(thd, user, state, &rows))
  {
  case 0:
    my_ok(thd, rows);
    break;
  case ER_KILL_DENIED_ERROR:
    char buf[DEFINER_LENGTH+1];
    strxnmov(buf, sizeof(buf)-1, user->user.str, "@", user->host.str, NULL);
    my_printf_error(ER_KILL_DENIED_ERROR, ER_THD(thd, ER_CANNOT_USER), MYF(0),
                    "KILL USER", buf);
    break;
  case ER_OUT_OF_RESOURCES:
  default:
    my_error(error, MYF(0));
  }
}


/** If pointer is not a null pointer, append filename to it. */

bool append_file_to_dir(THD *thd, const char **filename_ptr,
                        const LEX_CSTRING *table_name)
{
  char buff[FN_REFLEN],*ptr, *end;
  if (!*filename_ptr)
    return 0;					// nothing to do

  /* Check that the filename is not too long and it's a hard path */
  if (strlen(*filename_ptr)+table_name->length >= FN_REFLEN-1 ||
      !test_if_hard_path(*filename_ptr))
  {
    my_error(ER_WRONG_TABLE_NAME, MYF(0), *filename_ptr);
    return 1;
  }
  /* Fix is using unix filename format on dos */
  strmov(buff,*filename_ptr);
  end=convert_dirname(buff, *filename_ptr, NullS);
  if (unlikely(!(ptr= thd->alloc((size_t)(end-buff) + table_name->length + 1))))
    return 1;					// End of memory
  *filename_ptr=ptr;
  strxmov(ptr,buff,table_name->str,NullS);
  return 0;
}


Comp_creator *comp_eq_creator(bool invert)
{
  return invert?(Comp_creator *)&ne_creator:(Comp_creator *)&eq_creator;
}


Comp_creator *comp_ge_creator(bool invert)
{
  return invert?(Comp_creator *)&lt_creator:(Comp_creator *)&ge_creator;
}


Comp_creator *comp_gt_creator(bool invert)
{
  return invert?(Comp_creator *)&le_creator:(Comp_creator *)&gt_creator;
}


Comp_creator *comp_le_creator(bool invert)
{
  return invert?(Comp_creator *)&gt_creator:(Comp_creator *)&le_creator;
}


Comp_creator *comp_lt_creator(bool invert)
{
  return invert?(Comp_creator *)&ge_creator:(Comp_creator *)&lt_creator;
}


Comp_creator *comp_ne_creator(bool invert)
{
  return invert?(Comp_creator *)&eq_creator:(Comp_creator *)&ne_creator;
}


/**
  Construct ALL/ANY/SOME subquery Item.

  @param left_expr   pointer to left expression
  @param cmp         compare function creator
  @param all         true if we create ALL subquery
  @param select_lex  pointer on parsed subquery structure

  @return
    constructed Item (or 0 if out of memory)
*/
Item * all_any_subquery_creator(THD *thd, Item *left_expr,
				chooser_compare_func_creator cmp,
				bool all,
				SELECT_LEX *select_lex)
{
  if ((cmp == &comp_eq_creator) && !all)       //  = ANY <=> IN
    return new (thd->mem_root) Item_in_subselect(thd, left_expr, select_lex);

  if ((cmp == &comp_ne_creator) && all)        // <> ALL <=> NOT IN
    return new (thd->mem_root) Item_func_not(thd,
             new (thd->mem_root) Item_in_subselect(thd, left_expr, select_lex));

  Item_allany_subselect *it=
    new (thd->mem_root) Item_allany_subselect(thd, left_expr, cmp, select_lex,
                                              all);
  if (all) /* ALL */
    return it->upper_item= new (thd->mem_root) Item_func_not_all(thd, it);

  /* ANY/SOME */
  return it->upper_item= new (thd->mem_root) Item_func_nop_all(thd, it);
}


/**
  Multi update query pre-check.

  @param thd		Thread handler
  @param tables	Global/local table list (have to be the same)

  @retval
    FALSE OK
  @retval
    TRUE  Error
*/

bool multi_update_precheck(THD *thd, TABLE_LIST *tables)
{
  TABLE_LIST *table;
  LEX *lex= thd->lex;
  SELECT_LEX *select_lex= lex->first_select_lex();
  DBUG_ENTER("multi_update_precheck");

  if (select_lex->item_list.elements != lex->value_list.elements)
  {
    my_message(ER_WRONG_VALUE_COUNT, ER_THD(thd, ER_WRONG_VALUE_COUNT), MYF(0));
    DBUG_RETURN(TRUE);
  }
  /*
    Ensure that we have UPDATE or SELECT privilege for each table
    The exact privilege is checked in mysql_multi_update()
  */
  for (table= tables; table; table= table->next_local)
  {
    if (table->is_jtbm())
      continue;
    if (table->derived)
      table->grant.privilege= SELECT_ACL;
    else if ((check_access(thd, UPDATE_ACL, table->db.str,
                           &table->grant.privilege,
                           &table->grant.m_internal,
                           0, 1) ||
              check_grant(thd, UPDATE_ACL, table, FALSE, 1, TRUE)) &&
             (check_access(thd, SELECT_ACL, table->db.str,
                           &table->grant.privilege,
                           &table->grant.m_internal,
                           0, 0) ||
              check_grant(thd, SELECT_ACL, table, FALSE, 1, FALSE)))
      DBUG_RETURN(TRUE);

    table->grant.orig_want_privilege= NO_ACL;
    table->table_in_first_from_clause= 1;
  }
  /*
    Is there tables of subqueries?
  */
  if (lex->first_select_lex() != lex->all_selects_list)
  {
    DBUG_PRINT("info",("Checking sub query list"));
    for (table= tables; table; table= table->next_global)
    {
      if (!table->table_in_first_from_clause)
      {
	if (check_access(thd, SELECT_ACL, table->db.str,
                         &table->grant.privilege,
                         &table->grant.m_internal,
                         0, 0) ||
	    check_grant(thd, SELECT_ACL, table, FALSE, 1, FALSE))
	  DBUG_RETURN(TRUE);
      }
    }
  }

  DBUG_RETURN(FALSE);
}

/**
  Multi delete query pre-check.

  @param thd			Thread handler
  @param tables		Global/local table list

  @retval
    FALSE OK
  @retval
    TRUE  error
*/

bool multi_delete_precheck(THD *thd, TABLE_LIST *tables)
{
  SELECT_LEX *select_lex= thd->lex->first_select_lex();
  TABLE_LIST *aux_tables= thd->lex->auxiliary_table_list.first;
  TABLE_LIST **save_query_tables_own_last= thd->lex->query_tables_own_last;
  DBUG_ENTER("multi_delete_precheck");

  /*
    Temporary tables are pre-opened in 'tables' list only. Here we need to
    initialize TABLE instances in 'aux_tables' list.
  */
  for (TABLE_LIST *tl= aux_tables; tl; tl= tl->next_global)
  {
    if (tl->table)
      continue;

    if (tl->correspondent_table)
      tl->table= tl->correspondent_table->table;
  }

  /* sql_yacc guarantees that tables and aux_tables are not zero */
  DBUG_ASSERT(aux_tables != 0);
  if (check_table_access(thd, SELECT_ACL, tables, FALSE, UINT_MAX, FALSE))
    DBUG_RETURN(TRUE);

  /*
    Since aux_tables list is not part of LEX::query_tables list we
    have to juggle with LEX::query_tables_own_last value to be able
    call check_table_access() safely.
  */
  thd->lex->query_tables_own_last= 0;
  if (check_table_access(thd, DELETE_ACL, aux_tables, FALSE, UINT_MAX, FALSE))
  {
    thd->lex->query_tables_own_last= save_query_tables_own_last;
    DBUG_RETURN(TRUE);
  }
  thd->lex->query_tables_own_last= save_query_tables_own_last;

  if ((thd->variables.option_bits & OPTION_SAFE_UPDATES) && !select_lex->where)
  {
    my_message(ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE,
               ER_THD(thd, ER_UPDATE_WITHOUT_KEY_IN_SAFE_MODE), MYF(0));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/*
  Given a table in the source list, find a correspondent table in the
  table references list.

  @param lex Pointer to LEX representing multi-delete.
  @param src Source table to match.
  @param ref Table references list.

  @remark The source table list (tables listed before the FROM clause
  or tables listed in the FROM clause before the USING clause) may
  contain table names or aliases that must match unambiguously one,
  and only one, table in the target table list (table references list,
  after FROM/USING clause).

  @return Matching table, NULL otherwise.
*/

static TABLE_LIST *multi_delete_table_match(LEX *lex, TABLE_LIST *tbl,
                                            TABLE_LIST *tables)
{
  TABLE_LIST *match= NULL;
  DBUG_ENTER("multi_delete_table_match");

  for (TABLE_LIST *elem= tables; elem; elem= elem->next_local)
  {
    int res;

    if (tbl->is_fqtn && elem->is_alias)
      continue; /* no match */
    if (tbl->is_fqtn && elem->is_fqtn)
      res= (!tbl->table_name.streq(elem->table_name) ||
            cmp(&tbl->db, &elem->db));
    else if (elem->is_alias)
      res= !tbl->alias.streq(elem->alias);
    else
      res= (!tbl->table_name.streq(elem->table_name) ||
            cmp(&tbl->db, &elem->db));

    if (res)
      continue;

    if (match)
    {
      my_error(ER_NONUNIQ_TABLE, MYF(0), elem->alias.str);
      DBUG_RETURN(NULL);
    }

    match= elem;
  }

  if (!match)
    my_error(ER_UNKNOWN_TABLE, MYF(0), tbl->table_name.str, "MULTI DELETE");

  DBUG_RETURN(match);
}


/**
  Link tables in auxilary table list of multi-delete with corresponding
  elements in main table list, and set proper locks for them.

  @param lex   pointer to LEX representing multi-delete

  @retval
    FALSE   success
  @retval
    TRUE    error
*/

bool multi_delete_set_locks_and_link_aux_tables(LEX *lex)
{
  TABLE_LIST *tables= lex->first_select_lex()->table_list.first;
  TABLE_LIST *target_tbl;
  DBUG_ENTER("multi_delete_set_locks_and_link_aux_tables");

  lex->table_count_update= 0;

  for (target_tbl= lex->auxiliary_table_list.first;
       target_tbl; target_tbl= target_tbl->next_local)
  {
    lex->table_count_update++;
    /* All tables in aux_tables must be found in FROM PART */
    TABLE_LIST *walk= multi_delete_table_match(lex, target_tbl, tables);
    if (!walk)
      DBUG_RETURN(TRUE);
    if (!walk->derived)
      target_tbl->table_name= walk->table_name;
    walk->updating= target_tbl->updating;
    walk->lock_type= target_tbl->lock_type;
    /* We can assume that tables to be deleted from are locked for write. */
    DBUG_ASSERT(walk->lock_type >= TL_FIRST_WRITE);
    walk->mdl_request.set_type(MDL_SHARED_WRITE);
    target_tbl->correspondent_table= walk;	// Remember corresponding table
  }
  DBUG_RETURN(FALSE);
}


/**
  simple UPDATE query pre-check.

  @param thd		Thread handler
  @param tables	Global table list

  @retval
    FALSE OK
  @retval
    TRUE  Error
*/

bool update_precheck(THD *thd, TABLE_LIST *tables)
{
  DBUG_ENTER("update_precheck");
  if (thd->lex->first_select_lex()->item_list.elements !=
      thd->lex->value_list.elements)
  {
    my_message(ER_WRONG_VALUE_COUNT, ER_THD(thd, ER_WRONG_VALUE_COUNT), MYF(0));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(check_one_table_access(thd, UPDATE_ACL, tables));
}


/**
  simple DELETE query pre-check.

  @param thd		Thread handler
  @param tables	Global table list

  @retval
    FALSE  OK
  @retval
    TRUE   error
*/

bool delete_precheck(THD *thd, TABLE_LIST *tables)
{
  DBUG_ENTER("delete_precheck");
  if (tables->vers_conditions.delete_history)
  {
    if (check_one_table_access(thd, DELETE_HISTORY_ACL, tables))
      DBUG_RETURN(TRUE);
  }
  else
  {
    if (check_one_table_access(thd, DELETE_ACL, tables))
      DBUG_RETURN(TRUE);
    /* Set privilege for the WHERE clause */
    tables->grant.want_privilege=(SELECT_ACL & ~tables->grant.privilege);
  }
  DBUG_RETURN(FALSE);
}


/**
  simple INSERT query pre-check.

  @param thd		Thread handler
  @param tables	Global table list

  @retval
    FALSE  OK
  @retval
    TRUE   error
*/

bool insert_precheck(THD *thd, TABLE_LIST *tables)
{
  LEX *lex= thd->lex;
  DBUG_ENTER("insert_precheck");

  /*
    Check that we have modify privileges for the first table and
    select privileges for the rest
  */
  privilege_t privilege= (INSERT_ACL |
                    (lex->duplicates == DUP_REPLACE ? DELETE_ACL : NO_ACL) |
                    (lex->value_list.elements ? UPDATE_ACL : NO_ACL));

  if (check_one_table_access(thd, privilege, tables))
    DBUG_RETURN(TRUE);

  if (lex->update_list.elements != lex->value_list.elements)
  {
    my_message(ER_WRONG_VALUE_COUNT, ER_THD(thd, ER_WRONG_VALUE_COUNT), MYF(0));
    DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


/**
   Set proper open mode and table type for element representing target table
   of CREATE TABLE statement, also adjust statement table list if necessary.
*/

void create_table_set_open_action_and_adjust_tables(LEX *lex)
{
  TABLE_LIST *create_table= lex->query_tables;

  if (lex->tmp_table())
    create_table->open_type= OT_TEMPORARY_ONLY;
  else
    create_table->open_type= OT_BASE_ONLY;

  if (!lex->first_select_lex()->item_list.elements)
  {
    /*
      Avoid opening and locking target table for ordinary CREATE TABLE
      or CREATE TABLE LIKE for write (unlike in CREATE ... SELECT we
      won't do any insertions in it anyway). Not doing this causes
      problems when running CREATE TABLE IF NOT EXISTS for already
      existing log table.
    */
    create_table->lock_type= TL_READ;
  }
}


/**
  CREATE TABLE query pre-check.

  @param thd			Thread handler
  @param tables		Global table list
  @param create_table	        Table which will be created

  @retval
    FALSE   OK
  @retval
    TRUE   Error
*/

bool create_table_precheck(THD *thd, TABLE_LIST *tables,
                           TABLE_LIST *create_table)
{
  LEX *lex= thd->lex;
  SELECT_LEX *select_lex= lex->first_select_lex();
  privilege_t want_priv{CREATE_ACL};
  bool error= TRUE;                                 // Error message is given
  DBUG_ENTER("create_table_precheck");

  /*
    Require CREATE [TEMPORARY] privilege on new table; for
    CREATE TABLE ... SELECT, also require INSERT.
  */

  if (lex->tmp_table())
    want_priv= CREATE_TMP_ACL;
  else if (select_lex->item_list.elements || select_lex->tvc)
    want_priv|= INSERT_ACL;

  /* CREATE OR REPLACE on not temporary tables require DROP_ACL */
  if (lex->create_info.or_replace() && !lex->tmp_table())
    want_priv|= DROP_ACL;
                          
  if (check_access(thd, want_priv, create_table->db.str,
                   &create_table->grant.privilege,
                   &create_table->grant.m_internal,
                   0, 0))
    goto err;

  /* If it is a merge table, check privileges for merge children. */
  if (lex->create_info.merge_list)
  {
    /*
      The user must have (SELECT_ACL | UPDATE_ACL | DELETE_ACL) on the
      underlying base tables, even if there are temporary tables with the same
      names.

      From user's point of view, it might look as if the user must have these
      privileges on temporary tables to create a merge table over them. This is
      one of two cases when a set of privileges is required for operations on
      temporary tables (see also CREATE TABLE).

      The reason for this behavior stems from the following facts:

        - For merge tables, the underlying table privileges are checked only
          at CREATE TABLE / ALTER TABLE time.

          In other words, once a merge table is created, the privileges of
          the underlying tables can be revoked, but the user will still have
          access to the merge table (provided that the user has privileges on
          the merge table itself). 

        - Temporary tables shadow base tables.

          I.e. there might be temporary and base tables with the same name, and
          the temporary table takes the precedence in all operations.

        - For temporary MERGE tables we do not track if their child tables are
          base or temporary. As result we can't guarantee that privilege check
          which was done in presence of temporary child will stay relevant
          later as this temporary table might be removed.

      If SELECT_ACL | UPDATE_ACL | DELETE_ACL privileges were not checked for
      the underlying *base* tables, it would create a security breach as in
      Bug#12771903.
    */

    if (check_table_access(thd, SELECT_ACL | UPDATE_ACL | DELETE_ACL,
                           lex->create_info.merge_list, FALSE, UINT_MAX, FALSE))
      goto err;
  }

  if (want_priv != CREATE_TMP_ACL &&
      check_grant(thd, want_priv, create_table, FALSE, 1, FALSE))
    goto err;

  if (select_lex->item_list.elements)
  {
    /* Check permissions for used tables in CREATE TABLE ... SELECT */
    if (tables && check_table_access(thd, SELECT_ACL, tables, FALSE,
                                     UINT_MAX, FALSE))
      goto err;
  }
  else if (lex->create_info.like())
  {
    if (check_table_access(thd, SELECT_ACL, tables, FALSE, UINT_MAX, FALSE))
      goto err;
  }

  if (check_fk_parent_table_access(thd, &lex->create_info, &lex->alter_info,
                                   create_table->db))
    goto err;

  error= FALSE;

err:
  DBUG_RETURN(error);
}


/**
  Check privileges for LOCK TABLES statement.

  @param thd     Thread context.
  @param tables  List of tables to be locked.

  @retval FALSE - Success.
  @retval TRUE  - Failure.
*/

static bool lock_tables_precheck(THD *thd, TABLE_LIST *tables)
{
  TABLE_LIST *first_not_own_table= thd->lex->first_not_own_table();

  for (TABLE_LIST *table= tables; table != first_not_own_table && table;
       table= table->next_global)
  {
    if (is_temporary_table(table))
      continue;

    if (check_table_access(thd, PRIV_LOCK_TABLES, table,
                           FALSE, 1, FALSE))
      return TRUE;
  }

  return FALSE;
}


/**
  negate given expression.

  @param thd  thread handler
  @param expr expression for negation

  @return
    negated expression
*/

Item *negate_expression(THD *thd, Item *expr)
{
  Item *negated;
  if (expr->type() == Item::FUNC_ITEM &&
      ((Item_func *) expr)->functype() == Item_func::NOT_FUNC)
  {
    /* it is NOT(NOT( ... )) */
    Item *arg= ((Item_func *) expr)->arguments()[0];
    const Type_handler *fh= arg->fixed_type_handler();
    enum_parsing_place place= thd->lex->current_select->parsing_place;
    if ((fh && fh->is_bool_type()) || place == IN_WHERE || place == IN_HAVING)
      return arg;
    /*
      if it is not boolean function then we have to emulate value of
      not(not(a)), it will be a != 0
    */
    return new (thd->mem_root) Item_func_ne(thd, arg, new (thd->mem_root) Item_int(thd, (char*) "0", 0, 1));
  }

  if ((negated= expr->neg_transformer(thd)) != 0)
    return negated;
  return new (thd->mem_root) Item_func_not(thd, expr);
}

/**
  Set the specified definer to the default value, which is the
  current user in the thread.
 
  @param[in]  thd       thread handler
  @param[out] definer   definer
*/
 
void get_default_definer(THD *thd, LEX_USER *definer, bool role)
{
  const Security_context *sctx= thd->security_ctx;

  if (role)
  {
    definer->user.str= const_cast<char*>(sctx->priv_role);
    definer->host= empty_clex_str;
  }
  else
  {
    definer->user.str= const_cast<char*>(sctx->priv_user);
    definer->host.str= const_cast<char*>(sctx->priv_host);
    definer->host.length= strlen(definer->host.str);
  }
  definer->user.length= strlen(definer->user.str);
  definer->auth= NULL;
}


/**
  Create default definer for the specified THD.

  @param[in] thd         thread handler

  @return
    - On success, return a valid pointer to the created and initialized
    LEX_USER, which contains definer information.
    - On error, return 0.
*/

LEX_USER *create_default_definer(THD *thd, bool role)
{
  LEX_USER *definer;

  if (unlikely(!(definer= thd->alloc<LEX_USER>(1))))
    return 0;

  thd->get_definer(definer, role);

  if (role && definer->user.length == 0)
  {
    my_error(ER_INVALID_ROLE, MYF(0), "NONE");
    return 0;
  }
  else
    return definer;
}


/**
  Create definer with the given user and host names.

  @param[in] thd          thread handler
  @param[in] user_name    user name
  @param[in] host_name    host name

  @return
    - On success, return a valid pointer to the created and initialized
    LEX_USER, which contains definer information.
    - On error, return 0.
*/

LEX_USER *create_definer(THD *thd, LEX_CSTRING *user_name,
                         LEX_CSTRING *host_name)
{
  LEX_USER *definer;

  /* Create and initialize. */

  if (unlikely(!(definer= thd->alloc<LEX_USER>(1))))
    return 0;

  definer->user= *user_name;
  definer->host= *host_name;
  definer->auth= NULL;

  return definer;
}


/**
  Check that byte length of a string does not exceed some limit.

  @param str         string to be checked
  @param err_msg     Number of error message to be displayed if the string
		     is too long.  0 if empty error message.
  @param max_length  max length

  @retval
    FALSE   the passed string is not longer than max_length
  @retval
    TRUE    the passed string is longer than max_length

  NOTE
    The function is not used in existing code but can be useful later?
*/

bool check_string_byte_length(const LEX_CSTRING *str, uint err_msg,
                              size_t max_byte_length)
{
  if (str->length <= max_byte_length)
    return FALSE;

  my_error(ER_WRONG_STRING_LENGTH, MYF(0), str->str,
           err_msg ? ER(err_msg) : "", max_byte_length);

  return TRUE;
}


/*
  Check that char length of a string does not exceed some limit.

  SYNOPSIS
  check_string_char_length()
      str              string to be checked
      err_msg          Number of error message to be displayed if the string
		       is too long.  0 if empty error message.
      max_char_length  max length in symbols
      cs               string charset

  RETURN
    FALSE   the passed string is not longer than max_char_length
    TRUE    the passed string is longer than max_char_length
*/


bool check_string_char_length(const LEX_CSTRING *str, uint err_msg,
                              size_t max_char_length, CHARSET_INFO *cs,
                              bool no_error)
{
  Well_formed_prefix prefix(cs, str->str, str->length, max_char_length);
  if (likely(!prefix.well_formed_error_pos() &&
             str->length == prefix.length()))
    return FALSE;

  if (!no_error)
  {
    ErrConvString err(str->str, str->length, cs);
    my_error(ER_WRONG_STRING_LENGTH, MYF(0), err.ptr(),
             err_msg ? ER(err_msg) : "",
             max_char_length);
  }
  return TRUE;
}


bool check_ident_length(const LEX_CSTRING *ident)
{
  /*
    string_char_length desite the names, goes into Well_formed_prefix_status
    so this is more than just a length comparison. Things like a primary key
    doesn't have a name, therefore no length. Also the ident grammar allows
    empty backtick. Check quickly the length, and if 0, accept that.
  */
  if (ident->length && check_string_char_length(ident, 0, NAME_CHAR_LEN,
                                                Lex_ident_ci::charset_info(), 1))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), ident->str);
    return 1;
  }
  return 0;
}


/*
  Check if path does not contain mysql data home directory

  SYNOPSIS
    path_starts_from_data_home_dir()
    dir                     directory, with all symlinks resolved

  RETURN VALUES
    0	ok
    1	error ;  Given path contains data directory
*/
extern "C" {

int path_starts_from_data_home_dir(const char *path)
{
  size_t dir_len= strlen(path);
  DBUG_ENTER("path_starts_from_data_home_dir");

  if (mysql_unpacked_real_data_home_len<= dir_len)
  {
    if (dir_len > mysql_unpacked_real_data_home_len &&
        path[mysql_unpacked_real_data_home_len] != FN_LIBCHAR)
      DBUG_RETURN(0);

    if (lower_case_file_system)
    {
      if (!default_charset_info->strnncoll(path,
                                           mysql_unpacked_real_data_home_len,
                                           mysql_unpacked_real_data_home,
                                           mysql_unpacked_real_data_home_len))
      {
        DBUG_PRINT("error", ("Path is part of mysql_real_data_home"));
        DBUG_RETURN(1);
      }
    }
    else if (!memcmp(path, mysql_unpacked_real_data_home,
                     mysql_unpacked_real_data_home_len))
    {
      DBUG_PRINT("error", ("Path is part of mysql_real_data_home"));
      DBUG_RETURN(1);
    }
  }
  DBUG_RETURN(0);
}

}

/*
  Check if path does not contain mysql data home directory

  SYNOPSIS
    test_if_data_home_dir()
    dir                     directory

  RETURN VALUES
    0	ok
    1	error ;  Given path contains data directory
*/

int test_if_data_home_dir(const char *dir)
{
  char path[FN_REFLEN];
  DBUG_ENTER("test_if_data_home_dir");

  if (!dir)
    DBUG_RETURN(0);

  (void) fn_format(path, dir, "", "", MY_RETURN_REAL_PATH);
  DBUG_RETURN(path_starts_from_data_home_dir(path));
}


int error_if_data_home_dir(const char *path, const char *what)
{
  size_t dirlen;
  char   dirpath[FN_REFLEN];
  if (path)
  {
    dirname_part(dirpath, path, &dirlen);
    if (test_if_data_home_dir(dirpath))
    {
      my_error(ER_WRONG_ARGUMENTS, MYF(0), what);
      return 1;
    }
  }
  return 0;
}

/**
  Check that host name string is valid.

  @param[in] str string to be checked

  @return             Operation status
    @retval  FALSE    host name is ok
    @retval  TRUE     host name string is longer than max_length or
                      has invalid symbols
*/

bool check_host_name(LEX_CSTRING *str)
{
  const char *name= str->str;
  const char *end= str->str + str->length;
  if (check_string_byte_length(str, ER_HOSTNAME, HOSTNAME_LENGTH))
    return TRUE;

  while (name != end)
  {
    if (*name == '@')
    {
      my_printf_error(ER_UNKNOWN_ERROR, 
                      "Malformed hostname (illegal symbol: '%c')", MYF(0),
                      *name);
      return TRUE;
    }
    name++;
  }
  return FALSE;
}


extern int MYSQLparse(THD *thd); // from yy_mariadb.cc
extern int ORAparse(THD *thd);   // from yy_oracle.cc


/**
  This is a wrapper of MYSQLparse(). All the code should call parse_sql()
  instead of MYSQLparse().

  @param thd Thread context.
  @param parser_state Parser state.
  @param creation_ctx Object creation context.

  @return Error status.
    @retval FALSE on success.
    @retval TRUE on parsing error.
*/

bool parse_sql(THD *thd, Parser_state *parser_state,
               Object_creation_ctx *creation_ctx, bool do_pfs_digest)
{
  bool ret_value;
  DBUG_ENTER("parse_sql");
  DBUG_ASSERT(thd->m_parser_state == NULL);
  DBUG_ASSERT(thd->lex->m_sql_cmd == NULL);

  MYSQL_QUERY_PARSE_START(thd->query());
  /* Backup creation context. */

  Object_creation_ctx *backup_ctx= NULL;

  if (creation_ctx)
    backup_ctx= creation_ctx->set_n_backup(thd);

  /* Set parser state. */

  thd->m_parser_state= parser_state;

  parser_state->m_digest_psi= NULL;
  parser_state->m_lip.m_digest= NULL;

  if (do_pfs_digest)
  {
    /* Start Digest */
    parser_state->m_digest_psi= MYSQL_DIGEST_START(thd->m_statement_psi);

    if (parser_state->m_digest_psi != NULL)
    {
      /*
        If either:
        - the caller wants to compute a digest
        - the performance schema wants to compute a digest
        set the digest listener in the lexer.
      */
      parser_state->m_lip.m_digest= thd->m_digest;
      parser_state->m_lip.m_digest->m_digest_storage.m_charset_number= thd->charset()->number;
    }
  }

  /* Parse the query. */

  bool mysql_parse_status= thd->variables.sql_mode & MODE_ORACLE
                           ? ORAparse(thd) : MYSQLparse(thd);

  if (mysql_parse_status)
    /*
      Restore the original LEX if it was replaced when parsing
      a stored procedure. We must ensure that a parsing error
      does not leave any side effects in the THD.
    */
    LEX::cleanup_lex_after_parse_error(thd);

  DBUG_ASSERT(opt_bootstrap || mysql_parse_status ||
              thd->lex->select_stack_top == 0);
  thd->lex->current_select= thd->lex->first_select_lex();

  /*
    Check that if MYSQLparse() failed either thd->is_error() is set, or an
    internal error handler is set.

    The assert will not catch a situation where parsing fails without an
    error reported if an error handler exists. The problem is that the
    error handler might have intercepted the error, so thd->is_error() is
    not set. However, there is no way to be 100% sure here (the error
    handler might be for other errors than parsing one).
  */

  DBUG_ASSERT(!mysql_parse_status ||
              thd->is_error() ||
              thd->get_internal_handler());

  /* Reset parser state. */

  thd->m_parser_state= NULL;

  /* Restore creation context. */

  if (creation_ctx)
    creation_ctx->restore_env(thd, backup_ctx);

  /* That's it. */

  ret_value= mysql_parse_status || thd->is_fatal_error;

  if ((ret_value == 0) && (parser_state->m_digest_psi != NULL))
  {
    /*
      On parsing success, record the digest in the performance schema.
    */
    DBUG_ASSERT(do_pfs_digest);
    DBUG_ASSERT(thd->m_digest != NULL);
    MYSQL_DIGEST_END(parser_state->m_digest_psi,
                     & thd->m_digest->m_digest_storage);
  }

  MYSQL_QUERY_PARSE_DONE(ret_value);
  DBUG_RETURN(ret_value);
}

/**
  @} (end of group Runtime_Environment)
*/


void LEX::mark_first_table_as_inserting()
{
  TABLE_LIST *t= first_select_lex()->table_list.first;
  DBUG_ENTER("Query_tables_list::mark_tables_with_important_flags");
  DBUG_ASSERT(sql_command_flags[sql_command] & CF_INSERTS_DATA);
  t->for_insert_data= TRUE;
  DBUG_PRINT("info", ("table_list: %p  name: %s  db: %s  command: %u",
                      t, t->table_name.str,t->db.str, sql_command));
  DBUG_VOID_RETURN;
}
