/*
   Copyright (c) 2004, 2012, Oracle and/or its affiliates.
   Copyright (c) 2010, 2018, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335  USA */


#define MYSQL_LEX 1
#include "mariadb.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "unireg.h"
#include "sp_head.h"
#include "sql_trigger.h"
#include "sql_parse.h"                          // parse_sql
#include "parse_file.h"
#include "sp.h"
#include "sql_base.h"
#include "sql_show.h"                // append_definer, append_identifier
#include "sql_table.h"                        // build_table_filename,
                                              // check_n_cut_mysql50_prefix
#include "sql_db.h"                        // get_default_db_collation
#include "sql_handler.h"                        // mysql_ha_rm_tables
#include "sp_cache.h"                     // sp_invalidate_cache
#include <mysys_err.h>
#include "debug_sync.h"
#include "mysql/psi/mysql_sp.h"

/*************************************************************************/

/**
  Trigger_creation_ctx -- creation context of triggers.
*/

class Trigger_creation_ctx : public Stored_program_creation_ctx,
                             public Sql_alloc
{
public:
  static Trigger_creation_ctx *create(THD *thd,
                                      const char *db_name,
                                      const char *table_name,
                                      const LEX_CSTRING *client_cs_name,
                                      const LEX_CSTRING *connection_cl_name,
                                      const LEX_CSTRING *db_cl_name);

  Trigger_creation_ctx(CHARSET_INFO *client_cs,
                       CHARSET_INFO *connection_cl,
                       CHARSET_INFO *db_cl)
    :Stored_program_creation_ctx(client_cs, connection_cl, db_cl)
  { }

  virtual Stored_program_creation_ctx *clone(MEM_ROOT *mem_root)
  {
    return new (mem_root) Trigger_creation_ctx(m_client_cs,
                                               m_connection_cl,
                                               m_db_cl);
  }

protected:
  virtual Object_creation_ctx *create_backup_ctx(THD *thd) const
  {
    return new Trigger_creation_ctx(thd);
  }

  Trigger_creation_ctx(THD *thd)
    :Stored_program_creation_ctx(thd)
  { }
};

/**************************************************************************
  Trigger_creation_ctx implementation.
**************************************************************************/

Trigger_creation_ctx *
Trigger_creation_ctx::create(THD *thd,
                             const char *db_name,
                             const char *table_name,
                             const LEX_CSTRING *client_cs_name,
                             const LEX_CSTRING *connection_cl_name,
                             const LEX_CSTRING *db_cl_name)
{
  CHARSET_INFO *client_cs;
  CHARSET_INFO *connection_cl;
  CHARSET_INFO *db_cl;

  bool invalid_creation_ctx= FALSE;

  if (resolve_charset(client_cs_name->str,
                      thd->variables.character_set_client,
                      &client_cs))
  {
    sql_print_warning("Trigger for table '%s'.'%s': "
                      "invalid character_set_client value (%s).",
                      (const char *) db_name,
                      (const char *) table_name,
                      (const char *) client_cs_name->str);

    invalid_creation_ctx= TRUE;
  }

  if (resolve_collation(connection_cl_name->str,
                        thd->variables.collation_connection,
                        &connection_cl))
  {
    sql_print_warning("Trigger for table '%s'.'%s': "
                      "invalid collation_connection value (%s).",
                      (const char *) db_name,
                      (const char *) table_name,
                      (const char *) connection_cl_name->str);

    invalid_creation_ctx= TRUE;
  }

  if (resolve_collation(db_cl_name->str, NULL, &db_cl))
  {
    sql_print_warning("Trigger for table '%s'.'%s': "
                      "invalid database_collation value (%s).",
                      (const char *) db_name,
                      (const char *) table_name,
                      (const char *) db_cl_name->str);

    invalid_creation_ctx= TRUE;
  }

  if (invalid_creation_ctx)
  {
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_WARN,
                        ER_TRG_INVALID_CREATION_CTX,
                        ER_THD(thd, ER_TRG_INVALID_CREATION_CTX),
                        (const char *) db_name,
                        (const char *) table_name);
  }

  /*
    If we failed to resolve the database collation, load the default one
    from the disk.
  */

  if (!db_cl)
    db_cl= get_default_db_collation(thd, db_name);

  return new Trigger_creation_ctx(client_cs, connection_cl, db_cl);
}

/*************************************************************************/

static const LEX_CSTRING triggers_file_type=
  { STRING_WITH_LEN("TRIGGERS") };

const char * const TRG_EXT= ".TRG";

/**
  Table of .TRG file field descriptors.
  We have here only one field now because in nearest future .TRG
  files will be merged into .FRM files (so we don't need something
  like md5 or created fields).
*/
static File_option triggers_file_parameters[]=
{
  {
    { STRING_WITH_LEN("triggers") },
    my_offsetof(class Table_triggers_list, definitions_list),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("sql_modes") },
    my_offsetof(class Table_triggers_list, definition_modes_list),
    FILE_OPTIONS_ULLLIST
  },
  {
    { STRING_WITH_LEN("definers") },
    my_offsetof(class Table_triggers_list, definers_list),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("client_cs_names") },
    my_offsetof(class Table_triggers_list, client_cs_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("connection_cl_names") },
    my_offsetof(class Table_triggers_list, connection_cl_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("db_cl_names") },
    my_offsetof(class Table_triggers_list, db_cl_names),
    FILE_OPTIONS_STRLIST
  },
  {
    { STRING_WITH_LEN("created") },
    my_offsetof(class Table_triggers_list, create_times),
    FILE_OPTIONS_ULLLIST
  },
  { { 0, 0 }, 0, FILE_OPTIONS_STRING }
};

File_option sql_modes_parameters=
{
  { STRING_WITH_LEN("sql_modes") },
  my_offsetof(class Table_triggers_list, definition_modes_list),
  FILE_OPTIONS_ULLLIST
};

/**
  This must be kept up to date whenever a new option is added to the list
  above, as it specifies the number of required parameters of the trigger in
  .trg file.
  This defines the maximum number of parameters that is read.  If there are
  more paramaters in the file they are ignored.  Less number of parameters
  is regarded as ok.
*/

static const int TRG_NUM_REQUIRED_PARAMETERS= 7;

/*
  Structure representing contents of .TRN file which are used to support
  database wide trigger namespace.
*/

struct st_trigname
{
  LEX_CSTRING trigger_table;
};

static const LEX_CSTRING trigname_file_type=
  { STRING_WITH_LEN("TRIGGERNAME") };

const char * const TRN_EXT= ".TRN";

static File_option trigname_file_parameters[]=
{
  {
    { STRING_WITH_LEN("trigger_table")},
    offsetof(struct st_trigname, trigger_table),
    FILE_OPTIONS_ESTRING
  },
  { { 0, 0 }, 0, FILE_OPTIONS_STRING }
};


class Handle_old_incorrect_sql_modes_hook: public Unknown_key_hook
{
private:
  const char *path;
public:
  Handle_old_incorrect_sql_modes_hook(const char *file_path)
    :path(file_path)
  {};
  virtual bool process_unknown_string(const char *&unknown_key, uchar* base,
                                      MEM_ROOT *mem_root, const char *end);
};


class Handle_old_incorrect_trigger_table_hook: public Unknown_key_hook
{
public:
  Handle_old_incorrect_trigger_table_hook(const char *file_path,
                                          LEX_CSTRING *trigger_table_arg)
    :path(file_path), trigger_table_value(trigger_table_arg)
  {};
  virtual bool process_unknown_string(const char *&unknown_key, uchar* base,
                                      MEM_ROOT *mem_root, const char *end);
private:
  const char *path;
  LEX_CSTRING *trigger_table_value;
};


/**
  An error handler that catches all non-OOM errors which can occur during
  parsing of trigger body. Such errors are ignored and corresponding error
  message is used to construct a more verbose error message which contains
  name of problematic trigger. This error message is later emitted when
  one tries to perform DML or some of DDL on this table.
  Also, if possible, grabs name of the trigger being parsed so it can be
  used to correctly drop problematic trigger.
*/
class Deprecated_trigger_syntax_handler : public Internal_error_handler
{
private:

  char m_message[MYSQL_ERRMSG_SIZE];
  LEX_CSTRING *m_trigger_name;

public:

  Deprecated_trigger_syntax_handler() : m_trigger_name(NULL) {}

  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_warning_level *level,
                                const char* message,
                                Sql_condition ** cond_hdl)
  {
    if (sql_errno != EE_OUTOFMEMORY &&
        sql_errno != ER_OUT_OF_RESOURCES)
    {
      if(thd->lex->spname)
        m_trigger_name= &thd->lex->spname->m_name;
      if (m_trigger_name)
        my_snprintf(m_message, sizeof(m_message),
                    ER_THD(thd, ER_ERROR_IN_TRIGGER_BODY),
                    m_trigger_name->str, message);
      else
        my_snprintf(m_message, sizeof(m_message),
                    ER_THD(thd, ER_ERROR_IN_UNKNOWN_TRIGGER_BODY), message);
      return true;
    }
    return false;
  }

  LEX_CSTRING *get_trigger_name() { return m_trigger_name; }
  char *get_error_message() { return m_message; }
};


Trigger::~Trigger()
{
  sp_head::destroy(body);
}


/**
  Call a Table_triggers_list function for all triggers

  @return 0 ok
  @return # Something went wrong. Pointer to the trigger that mailfuncted
            returned
*/

Trigger* Table_triggers_list::for_all_triggers(Triggers_processor func,
                                               void *arg)
{
  for (uint i= 0; i < (uint)TRG_EVENT_MAX; i++)
  {
    for (uint j= 0; j < (uint)TRG_ACTION_MAX; j++)
    {
      for (Trigger *trigger= get_trigger(i,j) ;
           trigger ;
           trigger= trigger->next)
        if ((trigger->*func)(arg))
          return trigger;
    }
  }
  return 0;
}


/**
  Create or drop trigger for table.

  @param thd     current thread context (including trigger definition in LEX)
  @param tables  table list containing one table for which trigger is created.
  @param create  whenever we create (TRUE) or drop (FALSE) trigger

  @note
    This function is mainly responsible for opening and locking of table and
    invalidation of all its instances in table cache after trigger creation.
    Real work on trigger creation/dropping is done inside Table_triggers_list
    methods.

  @todo
    TODO: We should check if user has TRIGGER privilege for table here.
    Now we just require SUPER privilege for creating/dropping because
    we don't have proper privilege checking for triggers in place yet.

  @retval
    FALSE Success
  @retval
    TRUE  error
*/

bool mysql_create_or_drop_trigger(THD *thd, TABLE_LIST *tables, bool create)
{
  /*
    FIXME: The code below takes too many different paths depending on the
    'create' flag, so that the justification for a single function
    'mysql_create_or_drop_trigger', compared to two separate functions
    'mysql_create_trigger' and 'mysql_drop_trigger' is not apparent.
    This is a good candidate for a minor refactoring.
  */
  TABLE *table;
  bool result= TRUE;
  String stmt_query;
  bool lock_upgrade_done= FALSE;
  MDL_ticket *mdl_ticket= NULL;
  Query_tables_list backup;
  DBUG_ENTER("mysql_create_or_drop_trigger");

  /* Charset of the buffer for statement must be system one. */
  stmt_query.set_charset(system_charset_info);

  /*
    QQ: This function could be merged in mysql_alter_table() function
    But do we want this ?
  */

  /*
    Note that once we will have check for TRIGGER privilege in place we won't
    need second part of condition below, since check_access() function also
    checks that db is specified.
  */
  if (!thd->lex->spname->m_db.length || (create && !tables->db.length))
  {
    my_error(ER_NO_DB_ERROR, MYF(0));
    DBUG_RETURN(TRUE);
  }

  /*
    We don't allow creating triggers on tables in the 'mysql' schema
  */
  if (create && lex_string_eq(&tables->db, STRING_WITH_LEN("mysql")))
  {
    my_error(ER_NO_TRIGGERS_ON_SYSTEM_SCHEMA, MYF(0));
    DBUG_RETURN(TRUE);
  }

  /*
    There is no DETERMINISTIC clause for triggers, so can't check it.
    But a trigger can in theory be used to do nasty things (if it supported
    DROP for example) so we do the check for privileges. For now there is
    already a stronger test right above; but when this stronger test will
    be removed, the test below will hold. Because triggers have the same
    nature as functions regarding binlogging: their body is implicitly
    binlogged, so they share the same danger, so trust_function_creators
    applies to them too.
  */
  if (!trust_function_creators                               &&
      (WSREP_EMULATE_BINLOG(thd) || mysql_bin_log.is_open()) &&
      !(thd->security_ctx->master_access & PRIV_LOG_BIN_TRUSTED_SP_CREATOR))
  {
    my_error(ER_BINLOG_CREATE_ROUTINE_NEED_SUPER, MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (!create)
  {
    bool if_exists= thd->lex->if_exists();

    /*
      Protect the query table list from the temporary and potentially
      destructive changes necessary to open the trigger's table.
    */
    thd->lex->reset_n_backup_query_tables_list(&backup);
    /*
      Restore Query_tables_list::sql_command, which was
      reset above, as the code that writes the query to the
      binary log assumes that this value corresponds to the
      statement that is being executed.
    */
    thd->lex->sql_command= backup.sql_command;

    if (opt_readonly &&
        !(thd->security_ctx->master_access & PRIV_IGNORE_READ_ONLY) &&
        !thd->slave_thread)
    {
      my_error(ER_OPTION_PREVENTS_STATEMENT, MYF(0), "--read-only");
      goto end;
    }

    if (add_table_for_trigger(thd, thd->lex->spname, if_exists, & tables))
      goto end;

    if (!tables)
    {
      DBUG_ASSERT(if_exists);
      /*
        Since the trigger does not exist, there is no associated table,
        and therefore :
        - no TRIGGER privileges to check,
        - no trigger to drop,
        - no table to lock/modify,
        so the drop statement is successful.
      */
      result= FALSE;
      /* Still, we need to log the query ... */
      stmt_query.append(thd->query(), thd->query_length());
      goto end;
    }
  }

  /*
    Check that the user has TRIGGER privilege on the subject table.
  */
  {
    bool err_status;
    TABLE_LIST **save_query_tables_own_last= thd->lex->query_tables_own_last;
    thd->lex->query_tables_own_last= 0;

    err_status= check_table_access(thd, TRIGGER_ACL, tables, FALSE, 1, FALSE);

    thd->lex->query_tables_own_last= save_query_tables_own_last;

    if (err_status)
      goto end;
  }

  WSREP_TO_ISOLATION_BEGIN(WSREP_MYSQL_DB, NULL, tables);

  /* We should have only one table in table list. */
  DBUG_ASSERT(tables->next_global == 0);

  /* We do not allow creation of triggers on temporary tables. */
  if (create && thd->find_tmp_table_share(tables))
  {
    my_error(ER_TRG_ON_VIEW_OR_TEMP_TABLE, MYF(0), tables->alias.str);
    goto end;
  }

  /* We also don't allow creation of triggers on views. */
  tables->required_type= TABLE_TYPE_NORMAL;
  /*
    Also prevent DROP TRIGGER from opening temporary table which might
    shadow the subject table on which trigger to be dropped is defined.
  */
  tables->open_type= OT_BASE_ONLY;

  /* Keep consistent with respect to other DDL statements */
  mysql_ha_rm_tables(thd, tables);

  if (thd->locked_tables_mode)
  {
    /* Under LOCK TABLES we must only accept write locked tables. */
    if (!(tables->table= find_table_for_mdl_upgrade(thd, tables->db.str,
                                                    tables->table_name.str,
                                                    NULL)))
      goto end;
  }
  else
  {
    tables->table= open_n_lock_single_table(thd, tables,
                                            TL_READ_NO_INSERT, 0);
    if (! tables->table)
      goto end;
    tables->table->use_all_columns();
  }
  table= tables->table;

#ifdef WITH_WSREP
  if (WSREP(thd) &&
      !wsrep_should_replicate_ddl(thd, table->s->db_type()->db_type))
    goto wsrep_error_label;
#endif

  /* Later on we will need it to downgrade the lock */
  mdl_ticket= table->mdl_ticket;

  if (wait_while_table_is_used(thd, table, HA_EXTRA_FORCE_REOPEN))
    goto end;

  lock_upgrade_done= TRUE;

  if (!table->triggers)
  {
    if (!create)
    {
      my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
      goto end;
    }

    if (!(table->triggers= new (&table->mem_root) Table_triggers_list(table)))
      goto end;
  }

#ifdef WITH_WSREP
  DBUG_EXECUTE_IF("sync.mdev_20225",
                  {
                    const char act[]=
                      "now "
                      "wait_for signal.mdev_20225_continue";
                    DBUG_ASSERT(!debug_sync_set_action(thd,
                                                       STRING_WITH_LEN(act)));
                  };);
#endif /* WITH_WSREP */

  result= (create ?
           table->triggers->create_trigger(thd, tables, &stmt_query):
           table->triggers->drop_trigger(thd, tables, &stmt_query));

  close_all_tables_for_name(thd, table->s, HA_EXTRA_NOT_USED, NULL);

  /*
    Reopen the table if we were under LOCK TABLES.
    Ignore the return value for now. It's better to
    keep master/slave in consistent state.
  */
  if (thd->locked_tables_list.reopen_tables(thd, false))
    thd->clear_error();

  /*
    Invalidate SP-cache. That's needed because triggers may change list of
    pre-locking tables.
  */
  sp_cache_invalidate();

end:
  if (!result)
    result= write_bin_log(thd, TRUE, stmt_query.ptr(), stmt_query.length());

  /*
    If we are under LOCK TABLES we should restore original state of
    meta-data locks. Otherwise all locks will be released along
    with the implicit commit.
  */
  if (thd->locked_tables_mode && tables && lock_upgrade_done)
    mdl_ticket->downgrade_lock(MDL_SHARED_NO_READ_WRITE);

  /* Restore the query table list. Used only for drop trigger. */
  if (!create)
    thd->lex->restore_backup_query_tables_list(&backup);

  if (!result)
  {
    my_ok(thd);
    /* Drop statistics for this stored program from performance schema. */
    MYSQL_DROP_SP(SP_TYPE_TRIGGER,
                  thd->lex->spname->m_db.str, static_cast<uint>(thd->lex->spname->m_db.length),
                  thd->lex->spname->m_name.str, static_cast<uint>(thd->lex->spname->m_name.length));
  }

  DBUG_RETURN(result);
#ifdef WITH_WSREP
wsrep_error_label:
  DBUG_RETURN(true);
#endif
}


/**
  Build stmt_query to write it in the bin-log, the statement to write in
  the trigger file and the trigger definer.

  @param thd           current thread context (including trigger definition in
                       LEX)
  @param tables        table list containing one open table for which the
                       trigger is created.
  @param[out] stmt_query    after successful return, this string contains
                            well-formed statement for creation this trigger.
  @param[out] trigger_def  query to be stored in trigger file. As stmt_query,
		           but without "OR REPLACE" and no FOLLOWS/PRECEDES.
  @param[out] trg_definer         The triggger definer.
  @param[out] trg_definer_holder  Used as a buffer for definer.

  @note
    - Assumes that trigger name is fully qualified.
    - NULL-string means the following LEX_STRING instance:
    { str = 0; length = 0 }.
    - In other words, definer_user and definer_host should contain
    simultaneously NULL-strings (non-SUID/old trigger) or valid strings
    (SUID/new trigger).
*/

static void build_trig_stmt_query(THD *thd, TABLE_LIST *tables,
                                  String *stmt_query, String *trigger_def,
                                  LEX_CSTRING *trg_definer,
                                  char trg_definer_holder[])
{
  LEX_CSTRING stmt_definition;
  LEX *lex= thd->lex;
  size_t prefix_trimmed, suffix_trimmed;
  size_t original_length;

  /*
    Create a query with the full trigger definition.
    The original query is not appropriate, as it can miss the DEFINER=XXX part.
  */
  stmt_query->append(STRING_WITH_LEN("CREATE "));

  trigger_def->copy(*stmt_query);

  if (lex->create_info.or_replace())
    stmt_query->append(STRING_WITH_LEN("OR REPLACE "));

  if (lex->sphead->suid() != SP_IS_NOT_SUID)
  {
    /* SUID trigger */
    lex->definer->set_lex_string(trg_definer, trg_definer_holder);
    append_definer(thd, stmt_query, &lex->definer->user, &lex->definer->host);
    append_definer(thd, trigger_def, &lex->definer->user, &lex->definer->host);
  }
  else
  {
    *trg_definer= empty_clex_str;
  }


  /* Create statement for binary logging */
  stmt_definition.str=    lex->stmt_definition_begin;
  stmt_definition.length= (lex->stmt_definition_end -
                           lex->stmt_definition_begin);
  original_length= stmt_definition.length;
  trim_whitespace(thd->charset(), &stmt_definition, &prefix_trimmed);
  suffix_trimmed= original_length - stmt_definition.length - prefix_trimmed;

  stmt_query->append(stmt_definition.str, stmt_definition.length);

  /* Create statement for storing trigger (without trigger order) */
  if (lex->trg_chistics.ordering_clause == TRG_ORDER_NONE)
  {
    /*
      Not that here stmt_definition doesn't end with a \0, which is
      normally expected from a LEX_CSTRING
    */
    trigger_def->append(stmt_definition.str, stmt_definition.length);
  }
  else
  {
    /* Copy data before FOLLOWS/PRECEDES trigger_name */
    trigger_def->append(stmt_definition.str,
                        (lex->trg_chistics.ordering_clause_begin -
                         lex->stmt_definition_begin) - prefix_trimmed);
    /* Copy data after FOLLOWS/PRECEDES trigger_name */
    trigger_def->append(stmt_definition.str +
                        (lex->trg_chistics.ordering_clause_end -
                         lex->stmt_definition_begin)
                        - prefix_trimmed,
                        (lex->stmt_definition_end -
                         lex->trg_chistics.ordering_clause_end) -
                        suffix_trimmed);
  }
}


/**
  Create trigger for table.

  @param thd           current thread context (including trigger definition in
                       LEX)
  @param tables        table list containing one open table for which the
                       trigger is created.
  @param[out] stmt_query    after successful return, this string contains
                            well-formed statement for creation this trigger.

  @note
    - Assumes that trigger name is fully qualified.
    - NULL-string means the following LEX_STRING instance:
    { str = 0; length = 0 }.
    - In other words, definer_user and definer_host should contain
    simultaneously NULL-strings (non-SUID/old trigger) or valid strings
    (SUID/new trigger).

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::create_trigger(THD *thd, TABLE_LIST *tables,
                                         String *stmt_query)
{
  LEX *lex= thd->lex;
  TABLE *table= tables->table;
  char file_buff[FN_REFLEN], trigname_buff[FN_REFLEN];
  LEX_CSTRING file, trigname_file;
  char trg_definer_holder[USER_HOST_BUFF_SIZE];
  Item_trigger_field *trg_field;
  struct st_trigname trigname;
  String trigger_definition;
  Trigger *trigger= 0;
  bool trigger_dropped= 0;
  DBUG_ENTER("create_trigger");

  if (check_for_broken_triggers())
    DBUG_RETURN(true);

  /* Trigger must be in the same schema as target table. */
  if (lex_string_cmp(table_alias_charset, &table->s->db, &lex->spname->m_db))
  {
    my_error(ER_TRG_IN_WRONG_SCHEMA, MYF(0));
    DBUG_RETURN(true);
  }

  if (sp_process_definer(thd))
    DBUG_RETURN(true);

  /*
    Let us check if all references to fields in old/new versions of row in
    this trigger are ok.

    NOTE: We do it here more from ease of use standpoint. We still have to
    do some checks on each execution. E.g. we can catch privilege changes
    only during execution. Also in near future, when we will allow access
    to other tables from trigger we won't be able to catch changes in other
    tables...

    Since we don't plan to access to contents of the fields it does not
    matter that we choose for both OLD and NEW values the same versions
    of Field objects here.
  */
  old_field= new_field= table->field;

  for (trg_field= lex->trg_table_fields.first;
       trg_field; trg_field= trg_field->next_trg_field)
  {
    /*
      NOTE: now we do not check privileges at CREATE TRIGGER time. This will
      be changed in the future.
    */
    trg_field->setup_field(thd, table, NULL);

    if (trg_field->fix_fields_if_needed(thd, (Item **)0))
      DBUG_RETURN(true);
  }

  /* Ensure anchor trigger exists */
  if (lex->trg_chistics.ordering_clause != TRG_ORDER_NONE)
  {
    if (!(trigger= find_trigger(&lex->trg_chistics.anchor_trigger_name, 0)) ||
        trigger->event != lex->trg_chistics.event ||
        trigger->action_time != lex->trg_chistics.action_time)
    {
      my_error(ER_REFERENCED_TRG_DOES_NOT_EXIST, MYF(0),
               lex->trg_chistics.anchor_trigger_name.str);
      DBUG_RETURN(true);
    }
  }

  /*
    Here we are creating file with triggers and save all triggers in it.
    sql_create_definition_file() files handles renaming and backup of older
    versions
  */
  file.length= build_table_filename(file_buff, FN_REFLEN - 1,
                                    tables->db.str, tables->table_name.str,
                                    TRG_EXT, 0);
  file.str= file_buff;
  trigname_file.length= build_table_filename(trigname_buff, FN_REFLEN-1,
                                             tables->db.str,
                                             lex->spname->m_name.str,
                                             TRN_EXT, 0);
  trigname_file.str= trigname_buff;

  /* Use the filesystem to enforce trigger namespace constraints. */
  if (!access(trigname_buff, F_OK))
  {
    if (lex->create_info.or_replace())
    {
      String drop_trg_query;
      /*
        The following can fail if the trigger is for another table or
        there exists a .TRN file but there was no trigger for it in
        the .TRG file
      */
      if (unlikely(drop_trigger(thd, tables, &drop_trg_query)))
        DBUG_RETURN(true);
    }
    else if (lex->create_info.if_not_exists())
    {
      strxnmov(trigname_buff, sizeof(trigname_buff) - 1, tables->db.str, ".",
               lex->spname->m_name.str, NullS);
      push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                          ER_TRG_ALREADY_EXISTS,
                          ER_THD(thd, ER_TRG_ALREADY_EXISTS),
                          trigname_buff);
      LEX_CSTRING trg_definer_tmp;
      String trigger_def;

      /*
        Log query with IF NOT EXISTS to binary log. This is in line with
        CREATE TABLE IF NOT EXISTS.
      */
      build_trig_stmt_query(thd, tables, stmt_query, &trigger_def,
                            &trg_definer_tmp, trg_definer_holder);
      DBUG_RETURN(false);
    }
    else
    {
      strxnmov(trigname_buff, sizeof(trigname_buff) - 1, tables->db.str, ".",
               lex->spname->m_name.str, NullS);
      my_error(ER_TRG_ALREADY_EXISTS, MYF(0), trigname_buff);
      DBUG_RETURN(true);
    }
  }

  trigname.trigger_table= tables->table_name;

  /*
    We are not using lex->sphead here as an argument to Trigger() as we are
    going to access lex->sphead later in build_trig_stmt_query()
  */
  if (!(trigger= new (&table->mem_root) Trigger(this, 0)))
    goto err_without_cleanup;

  /* Create trigger_name.TRN file to ensure trigger name is unique */
  if (sql_create_definition_file(NULL, &trigname_file, &trigname_file_type,
                                 (uchar*)&trigname, trigname_file_parameters))
    goto err_without_cleanup;

  /* Populate the trigger object */

  trigger->sql_mode= thd->variables.sql_mode;
  /* Time with 2 decimals, like in MySQL 5.7 */
  trigger->create_time= ((ulonglong) thd->query_start())*100 + thd->query_start_sec_part()/10000;
  build_trig_stmt_query(thd, tables, stmt_query, &trigger_definition,
                        &trigger->definer, trg_definer_holder);

  trigger->definition.str=    trigger_definition.c_ptr();
  trigger->definition.length= trigger_definition.length();

  /*
    Fill character set information:
      - client character set contains charset info only;
      - connection collation contains pair {character set, collation};
      - database collation contains pair {character set, collation};
  */
  lex_string_set(&trigger->client_cs_name, thd->charset()->csname);
  lex_string_set(&trigger->connection_cl_name,
                 thd->variables.collation_connection->name);
  lex_string_set(&trigger->db_cl_name,
                 get_default_db_collation(thd, tables->db.str)->name);

  /* Add trigger in it's correct place */
  add_trigger(lex->trg_chistics.event,
              lex->trg_chistics.action_time,
              lex->trg_chistics.ordering_clause,
              &lex->trg_chistics.anchor_trigger_name,
              trigger);

  /* Create trigger definition file .TRG */
  if (unlikely(create_lists_needed_for_files(thd->mem_root)))
    goto err_with_cleanup;

  if (!sql_create_definition_file(NULL, &file, &triggers_file_type,
                                  (uchar*)this, triggers_file_parameters))
    DBUG_RETURN(false);

err_with_cleanup:
  /* Delete .TRN file */
  mysql_file_delete(key_file_trn, trigname_buff, MYF(MY_WME));

err_without_cleanup:
  delete trigger;                               // Safety, not critical

  if (trigger_dropped)
  {
    String drop_trg_query;
    drop_trg_query.append(STRING_WITH_LEN("DROP TRIGGER /* generated by failed CREATE TRIGGER */ "));
    drop_trg_query.append(&lex->spname->m_name);
    /*
      We dropped an existing trigger and was not able to recreate it because
      of an internal error. Ensure it's also dropped on the slave.
    */
    write_bin_log(thd, FALSE, drop_trg_query.ptr(), drop_trg_query.length());
  }
  DBUG_RETURN(true);
}


/**
   Empty all list used to load and create .TRG file
*/

void Table_triggers_list::empty_lists()
{
  definitions_list.empty();
  definition_modes_list.empty();
  definers_list.empty();
  client_cs_names.empty();
  connection_cl_names.empty();
  db_cl_names.empty();
  create_times.empty();
}


/**
   Create list of all trigger parameters for sql_create_definition_file()
*/

struct create_lists_param
{
  MEM_ROOT *root;
};


bool Table_triggers_list::create_lists_needed_for_files(MEM_ROOT *root)
{
  create_lists_param param;

  empty_lists();
  param.root= root;

  return for_all_triggers(&Trigger::add_to_file_list, &param);
}


bool Trigger::add_to_file_list(void* param_arg)
{
  create_lists_param *param= (create_lists_param*) param_arg;
  MEM_ROOT *mem_root= param->root;

  if (base->definitions_list.push_back(&definition, mem_root) ||
      base->definition_modes_list.push_back(&sql_mode, mem_root) ||
      base->definers_list.push_back(&definer, mem_root) ||
      base->client_cs_names.push_back(&client_cs_name, mem_root) ||
      base->connection_cl_names.push_back(&connection_cl_name, mem_root) ||
      base->db_cl_names.push_back(&db_cl_name, mem_root) ||
      base->create_times.push_back(&create_time, mem_root))
    return 1;
  return 0;
}



/**
  Deletes the .TRG file for a table.

  @param path         char buffer of size FN_REFLEN to be used
                      for constructing path to .TRG file.
  @param db           table's database name
  @param table_name   table's name

  @retval
    False   success
  @retval
    True    error
*/

static bool rm_trigger_file(char *path, const LEX_CSTRING *db,
                            const LEX_CSTRING *table_name)
{
  build_table_filename(path, FN_REFLEN-1, db->str, table_name->str, TRG_EXT, 0);
  return mysql_file_delete(key_file_trg, path, MYF(MY_WME));
}


/**
  Deletes the .TRN file for a trigger.

  @param path         char buffer of size FN_REFLEN to be used
                      for constructing path to .TRN file.
  @param db           trigger's database name
  @param trigger_name trigger's name

  @retval
    False   success
  @retval
    True    error
*/

static bool rm_trigname_file(char *path, const LEX_CSTRING *db,
                             const LEX_CSTRING *trigger_name)
{
  build_table_filename(path, FN_REFLEN - 1, db->str, trigger_name->str, TRN_EXT, 0);
  return mysql_file_delete(key_file_trn, path, MYF(MY_WME));
}


/**
  Helper function that saves .TRG file for Table_triggers_list object.

  @param triggers    Table_triggers_list object for which file should be saved
  @param db          Name of database for subject table
  @param table_name  Name of subject table

  @retval
    FALSE  Success
  @retval
    TRUE   Error
*/

bool Table_triggers_list::save_trigger_file(THD *thd, const LEX_CSTRING *db,
                                            const LEX_CSTRING *table_name)
{
  char file_buff[FN_REFLEN];
  LEX_CSTRING file;

  if (create_lists_needed_for_files(thd->mem_root))
    return true;

  file.length= build_table_filename(file_buff, FN_REFLEN - 1, db->str, table_name->str,
                                    TRG_EXT, 0);
  file.str= file_buff;
  return sql_create_definition_file(NULL, &file, &triggers_file_type,
                                    (uchar*) this, triggers_file_parameters);
}


/**
  Find a trigger with a given name

  @param name	 		Name of trigger
  @param remove_from_list	If set, remove trigger if found
*/

Trigger *Table_triggers_list::find_trigger(const LEX_CSTRING *name,
                                           bool remove_from_list)
{
  for (uint i= 0; i < (uint)TRG_EVENT_MAX; i++)
  {
    for (uint j= 0; j < (uint)TRG_ACTION_MAX; j++)
    {
      Trigger **parent, *trigger;

      for (parent= &triggers[i][j];
           (trigger= *parent);
           parent= &trigger->next)
      {
        if (lex_string_cmp(table_alias_charset,
                           &trigger->name, name) == 0)
        {
          if (remove_from_list)
          {
            *parent= trigger->next;
            count--;
          }
          return trigger;
        }
      }
    }
  }
  return 0;
}


/**
  Drop trigger for table.

  @param thd           current thread context
                       (including trigger definition in LEX)
  @param tables        table list containing one open table for which trigger
                       is dropped.
  @param[out] stmt_query    after successful return, this string contains
                            well-formed statement for creation this trigger.

  @todo
    Probably instead of removing .TRG file we should move
    to archive directory but this should be done as part of
    parse_file.cc functionality (because we will need it
    elsewhere).

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::drop_trigger(THD *thd, TABLE_LIST *tables,
                                       String *stmt_query)
{
  const LEX_CSTRING *sp_name= &thd->lex->spname->m_name; // alias
  char path[FN_REFLEN];
  Trigger *trigger;

  stmt_query->set(thd->query(), thd->query_length(), stmt_query->charset());

  /* Find and delete trigger from list */
  if (!(trigger= find_trigger(sp_name, true)))
  {
    my_message(ER_TRG_DOES_NOT_EXIST, ER_THD(thd, ER_TRG_DOES_NOT_EXIST),
               MYF(0));
    return 1;
  }

  if (!count)                                   // If no more triggers
  {
    /*
      TODO: Probably instead of removing .TRG file we should move
      to archive directory but this should be done as part of
      parse_file.cc functionality (because we will need it
      elsewhere).
    */
    if (rm_trigger_file(path, &tables->db, &tables->table_name))
      return 1;
  }
  else
  {
    if (save_trigger_file(thd, &tables->db, &tables->table_name))
      return 1;
  }

  if (rm_trigname_file(path, &tables->db, sp_name))
    return 1;

  delete trigger;
  return 0;
}


Table_triggers_list::~Table_triggers_list()
{
  DBUG_ENTER("Table_triggers_list::~Table_triggers_list");

  for (uint i= 0; i < (uint)TRG_EVENT_MAX; i++)
  {
    for (uint j= 0; j < (uint)TRG_ACTION_MAX; j++)
    {
      Trigger *next, *trigger;
      for (trigger= get_trigger(i,j) ; trigger ; trigger= next)
      {
        next= trigger->next;
        delete trigger;
      }
    }
  }

  /* Free blobs used in insert */
  if (record0_field)
    for (Field **fld_ptr= record0_field; *fld_ptr; fld_ptr++)
      (*fld_ptr)->free();

  if (record1_field)
    for (Field **fld_ptr= record1_field; *fld_ptr; fld_ptr++)
      delete *fld_ptr;

  DBUG_VOID_RETURN;
}


/**
  Prepare array of Field objects referencing to TABLE::record[1] instead
  of record[0] (they will represent OLD.* row values in ON UPDATE trigger
  and in ON DELETE trigger which will be called during REPLACE execution).

  @param table   pointer to TABLE object for which we are creating fields.

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::prepare_record_accessors(TABLE *table)
{
  Field **fld, **trg_fld;

  if ((has_triggers(TRG_EVENT_INSERT,TRG_ACTION_BEFORE) ||
       has_triggers(TRG_EVENT_UPDATE,TRG_ACTION_BEFORE)) &&
      (table->s->stored_fields != table->s->null_fields))

  {
    int null_bytes= (table->s->fields - table->s->null_fields + 7)/8;
    if (!(extra_null_bitmap= (uchar*)alloc_root(&table->mem_root, null_bytes)))
      return 1;
    if (!(record0_field= (Field **)alloc_root(&table->mem_root,
                                              (table->s->fields + 1) *
                                              sizeof(Field*))))
      return 1;

    uchar *null_ptr= extra_null_bitmap;
    uchar null_bit= 1;
    for (fld= table->field, trg_fld= record0_field; *fld; fld++, trg_fld++)
    {
      if (!(*fld)->null_ptr && !(*fld)->vcol_info && !(*fld)->vers_sys_field())
      {
        Field *f;
        if (!(f= *trg_fld= (*fld)->make_new_field(&table->mem_root, table,
                                                  table == (*fld)->table)))
          return 1;

        f->flags= (*fld)->flags;
        f->invisible= (*fld)->invisible;
        f->null_ptr= null_ptr;
        f->null_bit= null_bit;
        if (null_bit == 128)
          null_ptr++, null_bit= 1;
        else
          null_bit*= 2;
      }
      else
        *trg_fld= *fld;
    }
    *trg_fld= 0;
    DBUG_ASSERT(null_ptr <= extra_null_bitmap + null_bytes);
    bzero(extra_null_bitmap, null_bytes);
  }
  else
  {
    record0_field= table->field;
  }

  if (has_triggers(TRG_EVENT_UPDATE,TRG_ACTION_BEFORE) ||
      has_triggers(TRG_EVENT_UPDATE,TRG_ACTION_AFTER)  ||
      has_triggers(TRG_EVENT_DELETE,TRG_ACTION_BEFORE) ||
      has_triggers(TRG_EVENT_DELETE,TRG_ACTION_AFTER))
  {
    if (!(record1_field= (Field **)alloc_root(&table->mem_root,
                                              (table->s->fields + 1) *
                                              sizeof(Field*))))
      return 1;

    for (fld= table->field, trg_fld= record1_field; *fld; fld++, trg_fld++)
    {
      if (!(*trg_fld= (*fld)->make_new_field(&table->mem_root, table,
                                             table == (*fld)->table)))
        return 1;
      (*trg_fld)->move_field_offset((my_ptrdiff_t)(table->record[1] -
                                                   table->record[0]));
    }
    *trg_fld= 0;
  }
  return 0;
}


/**
  Check whenever .TRG file for table exist and load all triggers it contains.

  @param thd          current thread context
  @param db           table's database name
  @param table_name   table's name
  @param table        pointer to table object
  @param names_only   stop after loading trigger names

  @todo
    A lot of things to do here e.g. how about other funcs and being
    more paranoical ?

  @todo
    This could be avoided if there is no triggers for UPDATE and DELETE.

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::check_n_load(THD *thd, const LEX_CSTRING *db,
                                       const LEX_CSTRING *table_name,
                                       TABLE *table,
                                       bool names_only)
{
  char path_buff[FN_REFLEN];
  LEX_CSTRING path;
  File_parser *parser;
  LEX_CSTRING save_db;
  DBUG_ENTER("Table_triggers_list::check_n_load");

  path.length= build_table_filename(path_buff, FN_REFLEN - 1,
                                    db->str, table_name->str, TRG_EXT, 0);
  path.str= path_buff;

  // QQ: should we analyze errno somehow ?
  if (access(path_buff, F_OK))
    DBUG_RETURN(0);

  /* File exists so we got to load triggers */

  if ((parser= sql_parse_prepare(&path, &table->mem_root, 1)))
  {
    if (is_equal(&triggers_file_type, parser->type()))
    {
      Handle_old_incorrect_sql_modes_hook sql_modes_hook(path.str);
      LEX_CSTRING *trg_create_str;
      ulonglong *trg_sql_mode, *trg_create_time;
      Trigger *trigger;
      Table_triggers_list *trigger_list=
        new (&table->mem_root) Table_triggers_list(table);
      if (unlikely(!trigger_list))
        goto error;

      if (parser->parse((uchar*)trigger_list, &table->mem_root,
                        triggers_file_parameters,
                        TRG_NUM_REQUIRED_PARAMETERS,
                        &sql_modes_hook))
        goto error;

      List_iterator_fast<LEX_CSTRING> it(trigger_list->definitions_list);

      if (!trigger_list->definitions_list.is_empty() &&
          (trigger_list->client_cs_names.is_empty() ||
           trigger_list->connection_cl_names.is_empty() ||
           trigger_list->db_cl_names.is_empty()))
      {
        /* We will later use the current character sets */
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                            ER_TRG_NO_CREATION_CTX,
                            ER_THD(thd, ER_TRG_NO_CREATION_CTX),
                            db->str,
                            table_name->str);
      }

      table->triggers= trigger_list;
      status_var_increment(thd->status_var.feature_trigger);

      List_iterator_fast<ulonglong> itm(trigger_list->definition_modes_list);
      List_iterator_fast<LEX_CSTRING> it_definer(trigger_list->definers_list);
      List_iterator_fast<LEX_CSTRING> it_client_cs_name(trigger_list->client_cs_names);
      List_iterator_fast<LEX_CSTRING> it_connection_cl_name(trigger_list->connection_cl_names);
      List_iterator_fast<LEX_CSTRING> it_db_cl_name(trigger_list->db_cl_names);
      List_iterator_fast<ulonglong> it_create_times(trigger_list->create_times);
      LEX *old_lex= thd->lex;
      LEX lex;
      sp_rcontext *save_spcont= thd->spcont;
      sql_mode_t save_sql_mode= thd->variables.sql_mode;

      thd->lex= &lex;

      save_db= thd->db;
      thd->reset_db(db);
      while ((trg_create_str= it++))
      {
        sp_head *sp;
        sql_mode_t sql_mode;
        LEX_CSTRING *trg_definer;
        Trigger_creation_ctx *creation_ctx;

        /*
          It is old file format then sql_mode may not be filled in.
          We use one mode (current) for all triggers, because we have not
          information about mode in old format.
        */
        sql_mode= ((trg_sql_mode= itm++) ? *trg_sql_mode :
                   (ulonglong) global_system_variables.sql_mode);

        trg_create_time= it_create_times++;     // May be NULL if old file
        trg_definer= it_definer++;              // May be NULL if old file

        thd->variables.sql_mode= sql_mode;

        Parser_state parser_state;
        if (parser_state.init(thd, (char*) trg_create_str->str,
                              trg_create_str->length))
          goto err_with_lex_cleanup;

        if (!trigger_list->client_cs_names.is_empty())
          creation_ctx= Trigger_creation_ctx::create(thd,
                                                     db->str,
                                                     table_name->str,
                                                     it_client_cs_name++,
                                                     it_connection_cl_name++,
                                                     it_db_cl_name++);
        else
        {
          /* Old file with not stored character sets. Use current */
          creation_ctx=  new
            Trigger_creation_ctx(thd->variables.character_set_client,
                                 thd->variables.collation_connection,
                                 thd->variables.collation_database);
        }

        lex_start(thd);
        thd->spcont= NULL;

        /* The following is for catching parse errors */
        lex.trg_chistics.event= TRG_EVENT_MAX;
        lex.trg_chistics.action_time= TRG_ACTION_MAX;
        Deprecated_trigger_syntax_handler error_handler;
        thd->push_internal_handler(&error_handler);

        bool parse_error= parse_sql(thd, & parser_state, creation_ctx);
        thd->pop_internal_handler();
        DBUG_ASSERT(!parse_error || lex.sphead == 0);

        /*
          Not strictly necessary to invoke this method here, since we know
          that we've parsed CREATE TRIGGER and not an
          UPDATE/DELETE/INSERT/REPLACE/LOAD/CREATE TABLE, but we try to
          maintain the invariant that this method is called for each
          distinct statement, in case its logic is extended with other
          types of analyses in future.
        */
        lex.set_trg_event_type_for_tables();

        if (lex.sphead)
          lex.sphead->m_sql_mode= sql_mode;

        if (unlikely(!(trigger= (new (&table->mem_root)
                                 Trigger(trigger_list, lex.sphead)))))
          goto err_with_lex_cleanup;
        lex.sphead= NULL; /* Prevent double cleanup. */

        sp= trigger->body;

        trigger->sql_mode= sql_mode;
        trigger->definition= *trg_create_str;
        trigger->create_time= trg_create_time ? *trg_create_time : 0;
        trigger->name= sp ? sp->m_name : empty_clex_str;
        trigger->on_table_name.str= (char*) lex.raw_trg_on_table_name_begin;
        trigger->on_table_name.length= (lex.raw_trg_on_table_name_end -
                                        lex.raw_trg_on_table_name_begin);

        /* Copy pointers to character sets to make trigger easier to use */
        lex_string_set(&trigger->client_cs_name,
                       creation_ctx->get_client_cs()->csname);
        lex_string_set(&trigger->connection_cl_name,
                       creation_ctx->get_connection_cl()->name);
        lex_string_set(&trigger->db_cl_name,
                       creation_ctx->get_db_cl()->name);

        /* event can only be TRG_EVENT_MAX in case of fatal parse errors */
        if (lex.trg_chistics.event != TRG_EVENT_MAX)
          trigger_list->add_trigger(lex.trg_chistics.event,
                                    lex.trg_chistics.action_time,
                                    TRG_ORDER_NONE,
                                    &lex.trg_chistics.anchor_trigger_name,
                                    trigger);

        if (unlikely(parse_error))
        {
          LEX_CSTRING *name;

          /*
            In case of errors, disable all triggers for the table, but keep
            the wrong trigger around to allow the user to fix it
          */
          if (!trigger_list->m_has_unparseable_trigger)
            trigger_list->set_parse_error_message(error_handler.get_error_message());
          /* Currently sphead is always set to NULL in case of a parse error */
          DBUG_ASSERT(lex.sphead == 0);
          lex_end(&lex);

          if (likely((name= error_handler.get_trigger_name())))
          {
            trigger->name= safe_lexcstrdup_root(&table->mem_root, *name);
            if (unlikely(!trigger->name.str))
              goto err_with_lex_cleanup;
          }
          trigger->definer= ((!trg_definer || !trg_definer->length) ?
                             empty_clex_str : *trg_definer);
          continue;
        }

        sp->m_sql_mode= sql_mode;
        sp->set_creation_ctx(creation_ctx);

        if (!trg_definer || !trg_definer->length)
        {
          /*
            This trigger was created/imported from the previous version of
            MySQL, which does not support trigger_list definers. We should emit
            warning here.
          */

          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_TRG_NO_DEFINER,
                              ER_THD(thd, ER_TRG_NO_DEFINER),
                              db->str, sp->m_name.str);

          /*
            Set definer to the '' to correct displaying in the information
            schema.
          */

          sp->set_definer("", 0);
          trigger->definer= empty_clex_str;

          /*
            trigger_list without definer information are executed under the
            authorization of the invoker.
          */

          sp->set_suid(SP_IS_NOT_SUID);
        }
        else
        {
          sp->set_definer(trg_definer->str, trg_definer->length);
          trigger->definer= *trg_definer;
        }

        sp->m_sp_share= MYSQL_GET_SP_SHARE(SP_TYPE_TRIGGER,
                                           sp->m_db.str, static_cast<uint>(sp->m_db.length),
                                           sp->m_name.str, static_cast<uint>(sp->m_name.length));

#ifndef DBUG_OFF
        /*
          Let us check that we correctly update trigger definitions when we
          rename tables with trigger_list.

          In special cases like "RENAME TABLE `#mysql50#somename` TO `somename`"
          or "ALTER DATABASE `#mysql50#somename` UPGRADE DATA DIRECTORY NAME"
          we might be given table or database name with "#mysql50#" prefix (and
          trigger's definiton contains un-prefixed version of the same name).
          To remove this prefix we use check_n_cut_mysql50_prefix().
        */

        char fname[SAFE_NAME_LEN + 1];
        DBUG_ASSERT((!my_strcasecmp(table_alias_charset, lex.query_tables->db.str, db->str) ||
                     (check_n_cut_mysql50_prefix(db->str, fname, sizeof(fname)) &&
                      !my_strcasecmp(table_alias_charset, lex.query_tables->db.str, fname))));
        DBUG_ASSERT((!my_strcasecmp(table_alias_charset, lex.query_tables->table_name.str, table_name->str) ||
                     (check_n_cut_mysql50_prefix(table_name->str, fname, sizeof(fname)) &&
                      !my_strcasecmp(table_alias_charset, lex.query_tables->table_name.str, fname))));
#endif
        if (names_only)
        {
          lex_end(&lex);
          continue;
        }

        /*
          Gather all Item_trigger_field objects representing access to fields
          in old/new versions of row in trigger into lists containing all such
          objects for the trigger_list with same action and timing.
        */
        trigger->trigger_fields= lex.trg_table_fields.first;
        /*
          Also let us bind these objects to Field objects in table being
          opened.

          We ignore errors here, because if even something is wrong we still
          will be willing to open table to perform some operations (e.g.
          SELECT)...
          Anyway some things can be checked only during trigger execution.
        */
        for (Item_trigger_field *trg_field= lex.trg_table_fields.first;
             trg_field;
             trg_field= trg_field->next_trg_field)
        {
          trg_field->setup_field(thd, table,
                                 &trigger->subject_table_grants);
        }

        lex_end(&lex);
      }
      thd->reset_db(&save_db);
      thd->lex= old_lex;
      thd->spcont= save_spcont;
      thd->variables.sql_mode= save_sql_mode;

      if (!names_only && trigger_list->prepare_record_accessors(table))
        goto error;

      /* Ensure no one is accidently using the temporary load lists */
      trigger_list->empty_lists();
      DBUG_RETURN(0);

err_with_lex_cleanup:
      lex_end(&lex);
      thd->lex= old_lex;
      thd->spcont= save_spcont;
      thd->variables.sql_mode= save_sql_mode;
      thd->reset_db(&save_db);
      /* Fall trough to error */
    }
  }

error:
    if (unlikely(!thd->is_error()))
  {
    /*
      We don't care about this error message much because .TRG files will
      be merged into .FRM anyway.
    */
    my_error(ER_WRONG_OBJECT, MYF(0),
             table_name->str, TRG_EXT + 1, "TRIGGER");
  }
  DBUG_RETURN(1);
}


/**
   Add trigger in the correct position according to ordering clause
   Also update action order

   If anchor_trigger doesn't exist, add it last.
*/

void Table_triggers_list::add_trigger(trg_event_type event,
                                      trg_action_time_type action_time,
                                      trigger_order_type ordering_clause,
                                      LEX_CSTRING *anchor_trigger_name,
                                      Trigger *trigger)
{
  Trigger **parent= &triggers[event][action_time];
  uint position= 0;

  for ( ; *parent ; parent= &(*parent)->next, position++)
  {
    if (ordering_clause != TRG_ORDER_NONE &&
        !lex_string_cmp(table_alias_charset, anchor_trigger_name,
                        &(*parent)->name))
    {
      if (ordering_clause == TRG_ORDER_FOLLOWS)
      {
        parent= &(*parent)->next;               // Add after this one
        position++;
      }
      break;
    }
  }

  /* Add trigger where parent points to */
  trigger->next= *parent;
  *parent= trigger;

  /* Update action_orders and position */
  trigger->event= event;
  trigger->action_time= action_time;
  trigger->action_order= ++position;
  while ((trigger= trigger->next))
    trigger->action_order= ++position;

  count++;
}


/**
  Obtains and returns trigger metadata.

  @param trigger_stmt  returns statement of trigger
  @param body          returns body of trigger
  @param definer       returns definer/creator of trigger. The caller is
                       responsible to allocate enough space for storing
                       definer information.

  @retval
    False   success
  @retval
    True    error
*/

void Trigger::get_trigger_info(LEX_CSTRING *trigger_stmt,
                               LEX_CSTRING *trigger_body,
                               LEX_STRING *definer)
{
  DBUG_ENTER("get_trigger_info");

  *trigger_stmt= definition;
  if (!body)
  {
    /* Parse error */
    *trigger_body= definition;
    *definer= empty_lex_str;
    DBUG_VOID_RETURN;
  }
  *trigger_body= body->m_body_utf8;

  if (body->suid() == SP_IS_NOT_SUID)
  {
    *definer= empty_lex_str;
  }
  else
  {
    definer->length= strxmov(definer->str, body->m_definer.user.str, "@",
                             body->m_definer.host.str, NullS) - definer->str;
  }
  DBUG_VOID_RETURN;
}


/**
  Find trigger's table from trigger identifier and add it to
  the statement table list.

  @param[in] thd       Thread context.
  @param[in] trg_name  Trigger name.
  @param[in] if_exists TRUE if SQL statement contains "IF EXISTS" clause.
                       That means a warning instead of error should be
                       thrown if trigger with given name does not exist.
  @param[out] table    Pointer to TABLE_LIST object for the
                       table trigger.

  @return Operation status
    @retval FALSE On success.
    @retval TRUE  Otherwise.
*/

bool add_table_for_trigger(THD *thd,
                           const sp_name *trg_name,
                           bool if_exists,
                           TABLE_LIST **table)
{
  LEX *lex= thd->lex;
  char trn_path_buff[FN_REFLEN];
  LEX_CSTRING trn_path= { trn_path_buff, 0 };
  LEX_CSTRING tbl_name= null_clex_str;

  DBUG_ENTER("add_table_for_trigger");

  build_trn_path(thd, trg_name, (LEX_STRING*) &trn_path);

  if (check_trn_exists(&trn_path))
  {
    if (if_exists)
    {
      push_warning_printf(thd,
                          Sql_condition::WARN_LEVEL_NOTE,
                          ER_TRG_DOES_NOT_EXIST,
                          ER_THD(thd, ER_TRG_DOES_NOT_EXIST));

      *table= NULL;

      DBUG_RETURN(FALSE);
    }

    my_error(ER_TRG_DOES_NOT_EXIST, MYF(0));
    DBUG_RETURN(TRUE);
  }

  if (load_table_name_for_trigger(thd, trg_name, &trn_path, &tbl_name))
    DBUG_RETURN(TRUE);

  *table= sp_add_to_query_tables(thd, lex, &trg_name->m_db,
                                 &tbl_name, TL_IGNORE,
                                 MDL_SHARED_NO_WRITE);

  DBUG_RETURN(*table ? FALSE : TRUE);
}


/**
  Drop all triggers for table.

  @param thd      current thread context
  @param db       schema for table
  @param name     name for table

  @retval
    False   success
  @retval
    True    error
*/

bool Table_triggers_list::drop_all_triggers(THD *thd, const LEX_CSTRING *db,
                                            const LEX_CSTRING *name)
{
  TABLE table;
  char path[FN_REFLEN];
  bool result= 0;
  DBUG_ENTER("Triggers::drop_all_triggers");

  table.reset();
  init_sql_alloc(key_memory_Table_trigger_dispatcher,
                 &table.mem_root, 8192, 0, MYF(0));

  if (Table_triggers_list::check_n_load(thd, db, name, &table, 1))
  {
    result= 1;
    goto end;
  }
  if (table.triggers)
  {
    for (uint i= 0; i < (uint)TRG_EVENT_MAX; i++)
    {
      for (uint j= 0; j < (uint)TRG_ACTION_MAX; j++)
      {
        Trigger *trigger;
        for (trigger= table.triggers->get_trigger(i,j) ;
             trigger ;
             trigger= trigger->next)
        {
          /*
            Trigger, which body we failed to parse during call
            Table_triggers_list::check_n_load(), might be missing name.
            Such triggers have zero-length name and are skipped here.
          */
          if (trigger->name.length &&
              rm_trigname_file(path, db, &trigger->name))
          {
            /*
              Instead of immediately bailing out with error if we were unable
              to remove .TRN file we will try to drop other files.
            */
            result= 1;
          }
          /* Drop statistics for this stored program from performance schema. */
          MYSQL_DROP_SP(SP_TYPE_TRIGGER, db->str, static_cast<uint>(db->length),
                        trigger->name.str, static_cast<uint>(trigger->name.length));
        }
      }
    }
    if (rm_trigger_file(path, db, name))
      result= 1;
    delete table.triggers;
  }
end:
  free_root(&table.mem_root, MYF(0));
  DBUG_RETURN(result);
}


/**
  Update .TRG file after renaming triggers' subject table
  (change name of table in triggers' definitions).

  @param thd                 Thread context
  @param old_db_name         Old database of subject table
  @param new_db_name         New database of subject table
  @param old_table_name      Old subject table's name
  @param new_table_name      New subject table's name

  @retval
    FALSE  Success
  @retval
    TRUE   Failure
*/

struct change_table_name_param
{
  THD *thd;
  LEX_CSTRING *old_db_name;
  LEX_CSTRING *new_db_name;
  LEX_CSTRING *new_table_name;
  Trigger *stopper;
};


bool
Table_triggers_list::
change_table_name_in_triggers(THD *thd,
                              const LEX_CSTRING *old_db_name,
                              const LEX_CSTRING *new_db_name,
                              const LEX_CSTRING *old_table_name,
                              const LEX_CSTRING *new_table_name)
{
  struct change_table_name_param param;
  sql_mode_t save_sql_mode= thd->variables.sql_mode;
  char path_buff[FN_REFLEN];

  param.thd= thd;
  param.new_table_name= const_cast<LEX_CSTRING*>(new_table_name);

  for_all_triggers(&Trigger::change_table_name, &param);

  thd->variables.sql_mode= save_sql_mode;

  if (unlikely(thd->is_fatal_error))
    return TRUE; /* OOM */

  if (save_trigger_file(thd, new_db_name, new_table_name))
    return TRUE;

  if (rm_trigger_file(path_buff, old_db_name, old_table_name))
  {
    (void) rm_trigger_file(path_buff, new_db_name, new_table_name);
    return TRUE;
  }
  return FALSE;
}


bool Trigger::change_table_name(void* param_arg)
{
  change_table_name_param *param= (change_table_name_param*) param_arg;
  THD *thd= param->thd;
  LEX_CSTRING *new_table_name= param->new_table_name;
  LEX_CSTRING *def= &definition, new_def;
  size_t on_q_table_name_len, before_on_len;
  String buff;

  thd->variables.sql_mode= sql_mode;

  /* Construct CREATE TRIGGER statement with new table name. */
  buff.length(0);

  /* WARNING: 'on_table_name' is supposed to point inside 'def' */
  DBUG_ASSERT(on_table_name.str > def->str);
  DBUG_ASSERT(on_table_name.str < (def->str + def->length));
  before_on_len= on_table_name.str - def->str;

  buff.append(def->str, before_on_len);
  buff.append(STRING_WITH_LEN("ON "));
  append_identifier(thd, &buff, new_table_name);
  buff.append(STRING_WITH_LEN(" "));
  on_q_table_name_len= buff.length() - before_on_len;
  buff.append(on_table_name.str + on_table_name.length,
              def->length - (before_on_len + on_table_name.length));
  /*
    It is OK to allocate some memory on table's MEM_ROOT since this
    table instance will be thrown out at the end of rename anyway.
  */
  new_def.str= (char*) memdup_root(&base->trigger_table->mem_root, buff.ptr(),
                                   buff.length());
  new_def.length= buff.length();
  on_table_name.str= new_def.str + before_on_len;
  on_table_name.length= on_q_table_name_len;
  definition= new_def;
  return 0;
}


/**
  Iterate though Table_triggers_list::names_list list and update
  .TRN files after renaming triggers' subject table.

  @param old_db_name         Old database of subject table
  @param new_db_name         New database of subject table
  @param new_table_name      New subject table's name
  @param stopper             Pointer to Table_triggers_list::names_list at
                             which we should stop updating.

  @retval
    0      Success
  @retval
    non-0  Failure, pointer to Table_triggers_list::names_list element
    for which update failed.
*/

Trigger *
Table_triggers_list::
change_table_name_in_trignames(const LEX_CSTRING *old_db_name,
                               const LEX_CSTRING *new_db_name,
                               const LEX_CSTRING *new_table_name,
                               Trigger *trigger)
{
  struct change_table_name_param param;
  param.old_db_name=    const_cast<LEX_CSTRING*>(old_db_name);
  param.new_db_name=    const_cast<LEX_CSTRING*>(new_db_name);
  param.new_table_name= const_cast<LEX_CSTRING*>(new_table_name);
  param.stopper= trigger;

  return for_all_triggers(&Trigger::change_on_table_name, &param);
}


bool Trigger::change_on_table_name(void* param_arg)
{
  change_table_name_param *param= (change_table_name_param*) param_arg;

  char trigname_buff[FN_REFLEN];
  struct st_trigname trigname;
  LEX_CSTRING trigname_file;

  if (param->stopper == this)
    return 0;                                   // Stop processing

  trigname_file.length= build_table_filename(trigname_buff, FN_REFLEN-1,
                                             param->new_db_name->str, name.str,
                                             TRN_EXT, 0);
  trigname_file.str= trigname_buff;

  trigname.trigger_table= *param->new_table_name;

  if (base->create_lists_needed_for_files(current_thd->mem_root))
    return true;

  if (sql_create_definition_file(NULL, &trigname_file, &trigname_file_type,
                                 (uchar*)&trigname, trigname_file_parameters))
    return true;

  /* Remove stale .TRN file in case of database upgrade */
  if (param->old_db_name)
  {
    if (rm_trigname_file(trigname_buff, param->old_db_name, &name))
    {
      (void) rm_trigname_file(trigname_buff, param->new_db_name, &name);
      return 1;
    }
  }
  return 0;
}


/**
  Update .TRG and .TRN files after renaming triggers' subject table.

  @param[in,out] thd Thread context
  @param[in] db Old database of subject table
  @param[in] old_alias Old alias of subject table
  @param[in] old_table Old name of subject table
  @param[in] new_db New database for subject table
  @param[in] new_table New name of subject table

  @note
    This method tries to leave trigger related files in consistent state,
    i.e. it either will complete successfully, or will fail leaving files
    in their initial state.
    Also this method assumes that subject table is not renamed to itself.
    This method needs to be called under an exclusive table metadata lock.

  @retval FALSE Success
  @retval TRUE  Error
*/

bool Table_triggers_list::change_table_name(THD *thd, const LEX_CSTRING *db,
                                            const LEX_CSTRING *old_alias,
                                            const LEX_CSTRING *old_table,
                                            const LEX_CSTRING *new_db,
                                            const LEX_CSTRING *new_table)
{
  TABLE table;
  bool result= 0;
  bool upgrading50to51= FALSE;
  Trigger *err_trigger;
  DBUG_ENTER("Triggers::change_table_name");

  table.reset();
  init_sql_alloc(key_memory_Table_trigger_dispatcher,
                 &table.mem_root, 8192, 0, MYF(0));

  /*
    This method interfaces the mysql server code protected by
    an exclusive metadata lock.
  */
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::TABLE, db->str,
                                             old_table->str,
                                             MDL_EXCLUSIVE));

  DBUG_ASSERT(my_strcasecmp(table_alias_charset, db->str, new_db->str) ||
              my_strcasecmp(table_alias_charset, old_alias->str, new_table->str));

  if (Table_triggers_list::check_n_load(thd, db, old_table, &table, TRUE))
  {
    result= 1;
    goto end;
  }
  if (table.triggers)
  {
    if (table.triggers->check_for_broken_triggers())
    {
      result= 1;
      goto end;
    }
    /*
      Since triggers should be in the same schema as their subject tables
      moving table with them between two schemas raises too many questions.
      (E.g. what should happen if in new schema we already have trigger
       with same name ?).

      In case of "ALTER DATABASE `#mysql50#db1` UPGRADE DATA DIRECTORY NAME"
      we will be given table name with "#mysql50#" prefix
      To remove this prefix we use check_n_cut_mysql50_prefix().
    */
    if (my_strcasecmp(table_alias_charset, db->str, new_db->str))
    {
      char dbname[SAFE_NAME_LEN + 1];
      if (check_n_cut_mysql50_prefix(db->str, dbname, sizeof(dbname)) &&
          !my_strcasecmp(table_alias_charset, dbname, new_db->str))
      {
        upgrading50to51= TRUE;
      }
      else
      {
        my_error(ER_TRG_IN_WRONG_SCHEMA, MYF(0));
        result= 1;
        goto end;
      }
    }
    if (unlikely(table.triggers->change_table_name_in_triggers(thd, db, new_db,
                                                               old_alias,
                                                               new_table)))
    {
      result= 1;
      goto end;
    }
    if ((err_trigger= table.triggers->
         change_table_name_in_trignames( upgrading50to51 ? db : NULL,
                                         new_db, new_table, 0)))
    {
      /*
        If we were unable to update one of .TRN files properly we will
        revert all changes that we have done and report about error.
        We assume that we will be able to undo our changes without errors
        (we can't do much if there will be an error anyway).
      */
      (void) table.triggers->change_table_name_in_trignames(
                               upgrading50to51 ? new_db : NULL, db,
                               old_alias, err_trigger);
      (void) table.triggers->change_table_name_in_triggers(
                               thd, db, new_db,
                               new_table, old_alias);
      result= 1;
      goto end;
    }
  }

end:
  delete table.triggers;
  free_root(&table.mem_root, MYF(0));
  DBUG_RETURN(result);
}


/**
  Execute trigger for given (event, time) pair.

  The operation executes trigger for the specified event (insert, update,
  delete) and time (after, before) if it is set.

  @param thd
  @param event
  @param time_type
  @param old_row_is_record1

  @return Error status.
    @retval FALSE on success.
    @retval TRUE  on error.
*/

bool Table_triggers_list::process_triggers(THD *thd,
                                           trg_event_type event,
                                           trg_action_time_type time_type,
                                           bool old_row_is_record1)
{
  bool err_status;
  Sub_statement_state statement_state;
  Trigger *trigger;
  SELECT_LEX *save_current_select;

  if (check_for_broken_triggers())
    return TRUE;

  if (!(trigger= get_trigger(event, time_type)))
    return FALSE;

  if (old_row_is_record1)
  {
    old_field= record1_field;
    new_field= record0_field;
  }
  else
  {
    DBUG_ASSERT(event == TRG_EVENT_DELETE);
    new_field= record1_field;
    old_field= record0_field;
  }
  /*
    This trigger must have been processed by the pre-locking
    algorithm.
  */
  DBUG_ASSERT(trigger_table->pos_in_table_list->trg_event_map & trg2bit(event));

  thd->reset_sub_statement_state(&statement_state, SUB_STMT_TRIGGER);

  /*
    Reset current_select before call execute_trigger() and
    restore it after return from one. This way error is set
    in case of failure during trigger execution.
  */
  save_current_select= thd->lex->current_select;

  do {
    thd->lex->current_select= NULL;
    err_status=
      trigger->body->execute_trigger(thd,
                                     &trigger_table->s->db,
                                     &trigger_table->s->table_name,
                                     &trigger->subject_table_grants);
    status_var_increment(thd->status_var.executed_triggers);
  } while (!err_status && (trigger= trigger->next));
  thd->lex->current_select= save_current_select;

  thd->restore_sub_statement_state(&statement_state);

  return err_status;
}


/**
  Add triggers for table to the set of routines used by statement.
  Add tables used by them to statement table list. Do the same for
  routines used by triggers.

  @param thd             Thread context.
  @param prelocking_ctx  Prelocking context of the statement.
  @param table_list      Table list element for table with trigger.

  @retval FALSE  Success.
  @retval TRUE   Failure.
*/

bool
Table_triggers_list::
add_tables_and_routines_for_triggers(THD *thd,
                                     Query_tables_list *prelocking_ctx,
                                     TABLE_LIST *table_list)
{
  DBUG_ASSERT(static_cast<int>(table_list->lock_type) >=
              static_cast<int>(TL_WRITE_ALLOW_WRITE));

  for (int i= 0; i < (int)TRG_EVENT_MAX; i++)
  {
    if (table_list->trg_event_map & trg2bit(static_cast<trg_event_type>(i)))
    {
      for (int j= 0; j < (int)TRG_ACTION_MAX; j++)
      {
        Trigger *triggers= table_list->table->triggers->get_trigger(i,j);

        for ( ; triggers ; triggers= triggers->next)
        {
          sp_head *trigger= triggers->body;

          if (unlikely(!triggers->body))                  // Parse error
            continue;

          MDL_key key(MDL_key::TRIGGER, trigger->m_db.str, trigger->m_name.str);

          if (sp_add_used_routine(prelocking_ctx, thd->stmt_arena,
                                  &key, &sp_handler_trigger,
                                  table_list->belong_to_view))
          {
            trigger->add_used_tables_to_table_list(thd,
                       &prelocking_ctx->query_tables_last,
                       table_list->belong_to_view);
            sp_update_stmt_used_routines(thd, prelocking_ctx,
                                         &trigger->m_sroutines,
                                         table_list->belong_to_view);
            trigger->propagate_attributes(prelocking_ctx);
          }
        }
      }
    }
  }
  return FALSE;
}


/**
  Mark fields of subject table which we read/set in its triggers
  as such.

  This method marks fields of subject table which are read/set in its
  triggers as such (by properly updating TABLE::read_set/write_set)
  and thus informs handler that values for these fields should be
  retrieved/stored during execution of statement.

  @param thd    Current thread context
  @param event  Type of event triggers for which we are going to inspect
*/

void Table_triggers_list::mark_fields_used(trg_event_type event)
{
  int action_time;
  Item_trigger_field *trg_field;
  DBUG_ENTER("Table_triggers_list::mark_fields_used");

  for (action_time= 0; action_time < (int)TRG_ACTION_MAX; action_time++)
  {
    for (Trigger *trigger= get_trigger(event,action_time);
         trigger ;
         trigger= trigger->next)
    {
      for (trg_field= trigger->trigger_fields;
           trg_field;
           trg_field= trg_field->next_trg_field)
      {
        /* We cannot mark fields which does not present in table. */
        if (trg_field->field_idx != (uint)-1)
        {
          DBUG_PRINT("info", ("marking field: %d", trg_field->field_idx));
          if (trg_field->get_settable_routine_parameter())
            bitmap_set_bit(trigger_table->write_set, trg_field->field_idx);
          trigger_table->mark_column_with_deps(
                                  trigger_table->field[trg_field->field_idx]);
        }
      }
    }
  }
  trigger_table->file->column_bitmaps_signal();
  DBUG_VOID_RETURN;
}


/**
   Signals to the Table_triggers_list that a parse error has occurred when
   reading a trigger from file. This makes the Table_triggers_list enter an
   error state flagged by m_has_unparseable_trigger == true. The error message
   will be used whenever a statement invoking or manipulating triggers is
   issued against the Table_triggers_list's table.

   @param error_message The error message thrown by the parser.
 */
void Table_triggers_list::set_parse_error_message(char *error_message)
{
  m_has_unparseable_trigger= true;
  strnmov(m_parse_error_message, error_message,
          sizeof(m_parse_error_message)-1);
}


/**
  Trigger BUG#14090 compatibility hook.

  @param[in,out] unknown_key       reference on the line with unknown
    parameter and the parsing point
  @param[in]     base              base address for parameter writing
    (structure like TABLE)
  @param[in]     mem_root          MEM_ROOT for parameters allocation
  @param[in]     end               the end of the configuration

  @note
    NOTE: this hook process back compatibility for incorrectly written
    sql_modes parameter (see BUG#14090).

  @retval
    FALSE OK
  @retval
    TRUE  Error
*/

#define INVALID_SQL_MODES_LENGTH 13

bool
Handle_old_incorrect_sql_modes_hook::
process_unknown_string(const char *&unknown_key, uchar* base,
                       MEM_ROOT *mem_root, const char *end)
{
  DBUG_ENTER("Handle_old_incorrect_sql_modes_hook::process_unknown_string");
  DBUG_PRINT("info", ("unknown key: %60s", unknown_key));

  if (unknown_key + INVALID_SQL_MODES_LENGTH + 1 < end &&
      unknown_key[INVALID_SQL_MODES_LENGTH] == '=' &&
      !memcmp(unknown_key, STRING_WITH_LEN("sql_modes")))
  {
    THD *thd= current_thd;
    const char *ptr= unknown_key + INVALID_SQL_MODES_LENGTH + 1;

    DBUG_PRINT("info", ("sql_modes affected by BUG#14090 detected"));
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_NOTE,
                        ER_OLD_FILE_FORMAT,
                        ER_THD(thd, ER_OLD_FILE_FORMAT),
                        (char *)path, "TRIGGER");
    if (get_file_options_ulllist(ptr, end, unknown_key, base,
                                 &sql_modes_parameters, mem_root))
    {
      DBUG_RETURN(TRUE);
    }
    /*
      Set parsing pointer to the last symbol of string (\n)
      1) to avoid problem with \0 in the junk after sql_modes
      2) to speed up skipping this line by parser.
    */
    unknown_key= ptr-1;
  }
  DBUG_RETURN(FALSE);
}

#define INVALID_TRIGGER_TABLE_LENGTH 15

/**
  Trigger BUG#15921 compatibility hook. For details see
  Handle_old_incorrect_sql_modes_hook::process_unknown_string().
*/
bool
Handle_old_incorrect_trigger_table_hook::
process_unknown_string(const char *&unknown_key, uchar* base,
                       MEM_ROOT *mem_root, const char *end)
{
  DBUG_ENTER("Handle_old_incorrect_trigger_table_hook::process_unknown_string");
  DBUG_PRINT("info", ("unknown key: %60s", unknown_key));

  if (unknown_key + INVALID_TRIGGER_TABLE_LENGTH + 1 < end &&
      unknown_key[INVALID_TRIGGER_TABLE_LENGTH] == '=' &&
      !memcmp(unknown_key, STRING_WITH_LEN("trigger_table")))
  {
    THD *thd= current_thd;
    const char *ptr= unknown_key + INVALID_TRIGGER_TABLE_LENGTH + 1;

    DBUG_PRINT("info", ("trigger_table affected by BUG#15921 detected"));
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_NOTE,
                        ER_OLD_FILE_FORMAT,
                        ER_THD(thd, ER_OLD_FILE_FORMAT),
                        (char *)path, "TRIGGER");

    if (!(ptr= parse_escaped_string(ptr, end, mem_root, trigger_table_value)))
    {
      my_error(ER_FPARSER_ERROR_IN_PARAMETER, MYF(0), "trigger_table",
               unknown_key);
      DBUG_RETURN(TRUE);
    }

    /* Set parsing pointer to the last symbol of string (\n). */
    unknown_key= ptr-1;
  }
  DBUG_RETURN(FALSE);
}


/**
  Contruct path to TRN-file.

  @param thd[in]        Thread context.
  @param trg_name[in]   Trigger name.
  @param trn_path[out]  Variable to store constructed path
*/

void build_trn_path(THD *thd, const sp_name *trg_name, LEX_STRING *trn_path)
{
  /* Construct path to the TRN-file. */

  trn_path->length= build_table_filename(trn_path->str,
                                         FN_REFLEN - 1,
                                         trg_name->m_db.str,
                                         trg_name->m_name.str,
                                         TRN_EXT,
                                         0);
}


/**
  Check if TRN-file exists.

  @return
    @retval TRUE  if TRN-file does not exist.
    @retval FALSE if TRN-file exists.
*/

bool check_trn_exists(const LEX_CSTRING *trn_path)
{
  return access(trn_path->str, F_OK) != 0;
}


/**
  Retrieve table name for given trigger.

  @param thd[in]        Thread context.
  @param trg_name[in]   Trigger name.
  @param trn_path[in]   Path to the corresponding TRN-file.
  @param tbl_name[out]  Variable to store retrieved table name.

  @return Error status.
    @retval FALSE on success.
    @retval TRUE  if table name could not be retrieved.
*/

bool load_table_name_for_trigger(THD *thd,
                                 const sp_name *trg_name,
                                 const LEX_CSTRING *trn_path,
                                 LEX_CSTRING *tbl_name)
{
  File_parser *parser;
  struct st_trigname trn_data;
  Handle_old_incorrect_trigger_table_hook trigger_table_hook(
                                          trn_path->str,
                                          &trn_data.trigger_table);
  DBUG_ENTER("load_table_name_for_trigger");

  /* Parse the TRN-file. */

  if (!(parser= sql_parse_prepare(trn_path, thd->mem_root, TRUE)))
    DBUG_RETURN(TRUE);

  if (!is_equal(&trigname_file_type, parser->type()))
  {
    my_error(ER_WRONG_OBJECT, MYF(0),
             trg_name->m_name.str,
             TRN_EXT + 1,
             "TRIGGERNAME");

    DBUG_RETURN(TRUE);
  }

  if (parser->parse((uchar*) &trn_data, thd->mem_root,
                    trigname_file_parameters, 1,
                    &trigger_table_hook))
    DBUG_RETURN(TRUE);

  /* Copy trigger table name. */

  *tbl_name= trn_data.trigger_table;

  /* That's all. */

  DBUG_RETURN(FALSE);
}
