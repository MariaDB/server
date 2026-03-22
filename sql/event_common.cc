#include "mariadb.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */

#include <my_alloc.h>              // MEM_ROOT
#include "event_common.h"
#include "event_db_repository.h"   // enum enum_events_table_field
#include "sp.h"                    // load_charset
#include "sql_base.h"              // MYSQL_LOCK_IGNORE_TIMEOUT
#include "sql_class.h"             // THD
#include "sql_db.h"                // get_default_db_collation

/**************************************************************************
  Event_creation_ctx implementation.
**************************************************************************/

bool
Event_creation_ctx::load_from_db(THD *thd,
                                 MEM_ROOT *event_mem_root,
                                 const char *db_name,
                                 const char *event_name,
                                 TABLE *event_tbl,
                                 Stored_program_creation_ctx **ctx)
{
  /* Load character set/collation attributes. */

  CHARSET_INFO *client_cs;
  CHARSET_INFO *connection_cl;
  CHARSET_INFO *db_cl;

  bool invalid_creation_ctx= FALSE;

  if (load_charset(thd, event_mem_root,
                   event_tbl->field[ET_FIELD_CHARACTER_SET_CLIENT],
                   thd->variables.character_set_client,
                   &client_cs))
  {
    sql_print_warning("Event '%s'.'%s': invalid value "
                      "in column mysql.event.character_set_client.",
                      (const char *) db_name,
                      (const char *) event_name);

    invalid_creation_ctx= TRUE;
  }

  if (load_collation(thd, event_mem_root,
                     event_tbl->field[ET_FIELD_COLLATION_CONNECTION],
                     thd->variables.collation_connection,
                     &connection_cl))
  {
    sql_print_warning("Event '%s'.'%s': invalid value "
                      "in column mysql.event.collation_connection.",
                      (const char *) db_name,
                      (const char *) event_name);

    invalid_creation_ctx= TRUE;
  }

  if (load_collation(thd, event_mem_root,
                     event_tbl->field[ET_FIELD_DB_COLLATION],
                     NULL,
                     &db_cl))
  {
    sql_print_warning("Event '%s'.'%s': invalid value "
                      "in column mysql.event.db_collation.",
                      (const char *) db_name,
                      (const char *) event_name);

    invalid_creation_ctx= TRUE;
  }

  /*
    If we failed to resolve the database collation, load the default one
    from the disk.
  */

  if (!db_cl)
    db_cl= get_default_db_collation(thd, db_name);

  /* Create the context. */

  *ctx= new Event_creation_ctx(client_cs, connection_cl, db_cl);

  return invalid_creation_ctx;
}


/**
  Wrapper around Event_creation_ctx::load_from_db() to make it visible
  from sql_sys_or_ddl_triggers()
*/

bool
load_creation_context_for_sys_trg(THD *thd,
                                  MEM_ROOT *event_mem_root,
                                  const char *db_name,
                                  const char *event_name,
                                  TABLE *event_tbl,
                                  Stored_program_creation_ctx **ctx)
{
  return Event_creation_ctx::load_from_db(thd, event_mem_root,
                                          db_name,
                                          event_name,
                                          event_tbl,
                                          ctx);
}

static const
TABLE_FIELD_TYPE event_table_fields[ET_FIELD_COUNT] =
{
  {
    { STRING_WITH_LEN("db") },
    { STRING_WITH_LEN("char(64)") },
    { STRING_WITH_LEN("utf8mb") }
  },
  {
    { STRING_WITH_LEN("name") },
    { STRING_WITH_LEN("char(64)") },
    { STRING_WITH_LEN("utf8mb") }
  },
  {
    { STRING_WITH_LEN("body") },
    { STRING_WITH_LEN("longblob") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("definer") },
    { STRING_WITH_LEN("varchar(") },
    { STRING_WITH_LEN("utf8mb") }
  },
  {
    { STRING_WITH_LEN("execute_at") },
    { STRING_WITH_LEN("datetime") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("interval_value") },
    { STRING_WITH_LEN("int(11)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("interval_field") },
    { STRING_WITH_LEN("enum('YEAR','QUARTER','MONTH','DAY',"
    "'HOUR','MINUTE','WEEK','SECOND','MICROSECOND','YEAR_MONTH','DAY_HOUR',"
    "'DAY_MINUTE','DAY_SECOND','HOUR_MINUTE','HOUR_SECOND','MINUTE_SECOND',"
    "'DAY_MICROSECOND','HOUR_MICROSECOND','MINUTE_MICROSECOND',"
    "'SECOND_MICROSECOND')") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("created") },
    { STRING_WITH_LEN("timestamp") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("modified") },
    { STRING_WITH_LEN("timestamp") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("last_executed") },
    { STRING_WITH_LEN("datetime") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("starts") },
    { STRING_WITH_LEN("datetime") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("ends") },
    { STRING_WITH_LEN("datetime") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("status") },
    { STRING_WITH_LEN("enum('ENABLED','DISABLED','SLAVESIDE_DISABLED')") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("on_completion") },
    { STRING_WITH_LEN("enum('DROP','PRESERVE')") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("sql_mode") },
    { STRING_WITH_LEN("set('REAL_AS_FLOAT','PIPES_AS_CONCAT','ANSI_QUOTES',"
    "'IGNORE_SPACE','IGNORE_BAD_TABLE_OPTIONS','ONLY_FULL_GROUP_BY',"
    "'NO_UNSIGNED_SUBTRACTION',"
    "'NO_DIR_IN_CREATE','POSTGRESQL','ORACLE','MSSQL','DB2','MAXDB',"
    "'NO_KEY_OPTIONS','NO_TABLE_OPTIONS','NO_FIELD_OPTIONS','MYSQL323','MYSQL40',"
    "'ANSI','NO_AUTO_VALUE_ON_ZERO','NO_BACKSLASH_ESCAPES','STRICT_TRANS_TABLES',"
    "'STRICT_ALL_TABLES','NO_ZERO_IN_DATE','NO_ZERO_DATE','INVALID_DATES',"
    "'ERROR_FOR_DIVISION_BY_ZERO','TRADITIONAL','NO_AUTO_CREATE_USER',"
    "'HIGH_NOT_PRECEDENCE','NO_ENGINE_SUBSTITUTION','PAD_CHAR_TO_FULL_LENGTH',"
    "'EMPTY_STRING_IS_NULL','SIMULTANEOUS_ASSIGNMENT')") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("comment") },
    { STRING_WITH_LEN("char(64)") },
    { STRING_WITH_LEN("utf8mb") }
  },
  {
    { STRING_WITH_LEN("originator") },
    { STRING_WITH_LEN("int(10)") },
    {NULL, 0}
  },
  {
    { STRING_WITH_LEN("time_zone") },
    { STRING_WITH_LEN("char(64)") },
    { STRING_WITH_LEN("latin1") }
  },
  {
    { STRING_WITH_LEN("character_set_client") },
    { STRING_WITH_LEN("char(32)") },
    { STRING_WITH_LEN("utf8mb") }
  },
  {
    { STRING_WITH_LEN("collation_connection") },
    { STRING_WITH_LEN("char(") },
    { STRING_WITH_LEN("utf8mb") }
  },
  {
    { STRING_WITH_LEN("db_collation") },
    { STRING_WITH_LEN("char(") },
    { STRING_WITH_LEN("utf8mb") }
  },
  {
    { STRING_WITH_LEN("body_utf8") },
    { STRING_WITH_LEN("longblob") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("kind") },
    { STRING_WITH_LEN("set('SCHEDULE','STARTUP','SHUTDOWN',"
                      "'LOGON','LOGOFF','DDL')") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("when") },
    { STRING_WITH_LEN("enum('BEFORE','AFTER')") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("ddl_type") },
    { STRING_WITH_LEN("set('CREATE','ALTER','DROP','TRUNCATE',"
                      "'ANALYZE','RENAME','GRANT','REVOKE')") },
    { NULL, 0 }
  }
};

LEX_CSTRING
Event_db_repository_common::MYSQL_EVENT_NAME= { STRING_WITH_LEN("event") };


const TABLE_FIELD_DEF
Event_db_repository_common::event_table_def= {ET_FIELD_COUNT, event_table_fields, 0, (uint*) 0};


/**
  Open mysql.event table for read.

  It's assumed that the caller knows what they are doing:
  - whether it was necessary to reset-and-backup the open tables state
  - whether the requested lock does not lead to a deadlock
  - whether this open mode would work under LOCK TABLES, or inside a
  stored function or trigger.

  Note that if the table can't be locked successfully this operation will
  close it. Therefore it provides guarantee that it either opens and locks
  table or fails without leaving any tables open.

  @param[in]  thd  Thread context
  @param[in]  lock_type  How to lock the table
  @param[out] table  We will store the open table here

  @retval TRUE open and lock failed - an error message is pushed into the
               stack
  @retval FALSE success
*/

bool
Event_db_repository_common::open_event_table(THD *thd,
                                             enum thr_lock_type lock_type,
                                             TABLE **table)
{
  TABLE_LIST tables;
  DBUG_ENTER("Event_db_repository::open_event_table");

  tables.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_EVENT_NAME, 0, lock_type);

  if (open_and_lock_tables(thd, &tables, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT))
    DBUG_RETURN(TRUE);

  *table= tables.table;
  tables.table->use_all_columns();
  /* NOTE: &tables pointer will be invalid after return */
  tables.table->pos_in_table_list= NULL;

  if (table_intact.check(*table, &event_table_def))
  {
    thd->commit_whole_transaction_and_close_tables();
    *table= 0;                                  // Table is now closed
    my_error(ER_EVENT_OPEN_TABLE_FAILED, MYF(0));
    DBUG_RETURN(TRUE);
  }

  DBUG_RETURN(FALSE);
}


/**
  Open mysql.db, mysql.user and mysql.event and check whether:
    - mysql.db exists and is up to date (or from a newer version of MySQL),
    - mysql.user has column Event_priv at an expected position,
    - mysql.event exists and is up to date (or from a newer version of
      MySQL)

  This function is called only when the server is started.
  @pre The passed in thread handle has no open tables.

  @retval FALSE  OK
  @retval TRUE   Error, an error message is output to the error log.
*/

bool
Event_db_repository_common::check_system_tables(THD *thd)
{
  TABLE_LIST tables;
  int ret= FALSE;
  DBUG_ENTER("Event_db_repository::check_system_tables");
  DBUG_PRINT("enter", ("thd: %p", thd));

  /* Check mysql.event */
  tables.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_EVENT_NAME, 0, TL_READ);

  if (open_and_lock_tables(thd, &tables, FALSE, MYSQL_LOCK_IGNORE_TIMEOUT))
  {
    ret= 1;
    sql_print_error("Cannot open mysql.event");
  }
  else
  {
    if (table_intact.check(tables.table, &event_table_def))
      ret= 1;
    close_mysql_tables(thd);
  }

  DBUG_RETURN(MY_TEST(ret));
}

Table_check_intact_log_error Event_db_repository_common::table_intact;
ulong Events_common::startup_state= Events_common::EVENTS_OFF;
