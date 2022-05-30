/*
   Copyright (c) 2002, 2018, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

#include "mariadb.h"
#include "sql_priv.h"
#include "unireg.h"
#include "sp.h"
#include "sql_base.h"                           // close_thread_tables
#include "sql_lex.h"                            // empty_clex_str
#include "sql_parse.h"                          // parse_sql
#include "key.h"                                // key_copy
#include "sql_show.h"             // append_definer, append_identifier
#include "sql_db.h" // get_default_db_collation, mysql_opt_change_db,
                    // mysql_change_db, check_db_dir_existence,
                    // load_db_opt_by_name
#include "sql_table.h"                          // write_bin_log
#include "sp_head.h"
#include "sp_cache.h"
#include "transaction.h"
#include "lock.h"                               // lock_object_name

#include <my_user.h>
#include "mysql/psi/mysql_sp.h"

sp_cache **Sp_handler_procedure::get_cache(THD *thd) const
{
  return &thd->sp_proc_cache;
}

sp_cache **Sp_handler_function::get_cache(THD *thd) const
{
  return &thd->sp_func_cache;
}

sp_cache **Sp_handler_package_spec::get_cache(THD *thd) const
{
  return &thd->sp_package_spec_cache;
}

sp_cache **Sp_handler_package_body::get_cache(THD *thd) const
{
  return &thd->sp_package_body_cache;
}


ulong Sp_handler_procedure::recursion_depth(THD *thd) const
{
  return thd->variables.max_sp_recursion_depth;
}


bool Sp_handler::add_instr_freturn(THD *thd, sp_head *sp,
                                   sp_pcontext *spcont,
                                   Item *item, LEX *lex) const
{
  my_error(ER_SP_BADRETURN, MYF(0));
  return true;
}


bool Sp_handler::add_instr_preturn(THD *thd, sp_head *sp,
                                   sp_pcontext *spcont) const
{
  thd->parse_error();
  return true;
}


bool Sp_handler_function::add_instr_freturn(THD *thd, sp_head *sp,
                                            sp_pcontext *spcont,
                                            Item *item, LEX *lex) const
{
  return sp->add_instr_freturn(thd, spcont, item, lex);
}


bool Sp_handler_procedure::add_instr_preturn(THD *thd, sp_head *sp,
                                             sp_pcontext *spcont) const
{
  return sp->add_instr_preturn(thd, spcont);
}


Sp_handler_procedure sp_handler_procedure;
Sp_handler_function sp_handler_function;
Sp_handler_package_spec sp_handler_package_spec;
Sp_handler_package_body sp_handler_package_body;
Sp_handler_trigger sp_handler_trigger;
Sp_handler_package_procedure sp_handler_package_procedure;
Sp_handler_package_function sp_handler_package_function;


const Sp_handler *Sp_handler_procedure::package_routine_handler() const
{
  return &sp_handler_package_procedure;
}


const Sp_handler *Sp_handler_function::package_routine_handler() const
{
  return &sp_handler_package_function;
}


static const
TABLE_FIELD_TYPE proc_table_fields[MYSQL_PROC_FIELD_COUNT] =
{
  {
    { STRING_WITH_LEN("db") },
    { STRING_WITH_LEN("char(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("name") },
    { STRING_WITH_LEN("char(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("type") },
    { STRING_WITH_LEN("enum('FUNCTION','PROCEDURE')") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("specific_name") },
    { STRING_WITH_LEN("char(64)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("language") },
    { STRING_WITH_LEN("enum('SQL')") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("sql_data_access") },
    { STRING_WITH_LEN("enum('CONTAINS_SQL','NO_SQL','READS_SQL_DATA','MODIFIES_SQL_DATA')") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("is_deterministic") },
    { STRING_WITH_LEN("enum('YES','NO')") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("security_type") },
    { STRING_WITH_LEN("enum('INVOKER','DEFINER')") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("param_list") },
    { STRING_WITH_LEN("blob") },
    { NULL, 0 }
  },

  {
    { STRING_WITH_LEN("returns") },
    { STRING_WITH_LEN("longblob") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("body") },
    { STRING_WITH_LEN("longblob") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("definer") },
    { STRING_WITH_LEN("varchar(") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("created") },
    { STRING_WITH_LEN("timestamp") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("modified") },
    { STRING_WITH_LEN("timestamp") },
    { NULL, 0 }
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
    "'EMPTY_STRING_IS_NULL','SIMULTANEOUS_ASSIGNMENT',"
    "'TIME_ROUND_FRACTIONAL')") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("comment") },
    { STRING_WITH_LEN("text") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("character_set_client") },
    { STRING_WITH_LEN("char(32)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("collation_connection") },
    { STRING_WITH_LEN("char(32)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("db_collation") },
    { STRING_WITH_LEN("char(32)") },
    { STRING_WITH_LEN("utf8mb3") }
  },
  {
    { STRING_WITH_LEN("body_utf8") },
    { STRING_WITH_LEN("longblob") },
    { NULL, 0 }
  },
  {
    { STRING_WITH_LEN("aggregate") },
    { STRING_WITH_LEN("enum('NONE','GROUP')") },
    { NULL, 0 }
  }
};

static const TABLE_FIELD_DEF
proc_table_def= {MYSQL_PROC_FIELD_COUNT, proc_table_fields, 0, (uint*) 0 };

/*************************************************************************/

/**
  Stored_routine_creation_ctx -- creation context of stored routines
  (stored procedures and functions).
*/

class Stored_routine_creation_ctx : public Stored_program_creation_ctx,
                                    public Sql_alloc
{
public:
  static Stored_routine_creation_ctx *
  load_from_db(THD *thd, const Database_qualified_name *name, TABLE *proc_tbl);

public:
  virtual Stored_program_creation_ctx *clone(MEM_ROOT *mem_root)
  {
    return new (mem_root) Stored_routine_creation_ctx(m_client_cs,
                                                      m_connection_cl,
                                                      m_db_cl);
  }

protected:
  virtual Object_creation_ctx *create_backup_ctx(THD *thd) const
  {
    DBUG_ENTER("Stored_routine_creation_ctx::create_backup_ctx");
    DBUG_RETURN(new Stored_routine_creation_ctx(thd));
  }

private:
  Stored_routine_creation_ctx(THD *thd)
    : Stored_program_creation_ctx(thd)
  { }

  Stored_routine_creation_ctx(CHARSET_INFO *client_cs,
                              CHARSET_INFO *connection_cl,
                              CHARSET_INFO *db_cl)
    : Stored_program_creation_ctx(client_cs, connection_cl, db_cl)
  { }
};

/**************************************************************************
  Stored_routine_creation_ctx implementation.
**************************************************************************/

bool load_charset(THD *thd,
                  MEM_ROOT *mem_root,
                  Field *field,
                  CHARSET_INFO *dflt_cs,
                  CHARSET_INFO **cs)
{
  LEX_CSTRING cs_name;
  myf utf8_flag= thd->get_utf8_flag();

  if (field->val_str_nopad(mem_root, &cs_name))
  {
    *cs= dflt_cs;
    return TRUE;
  }

  DBUG_ASSERT(cs_name.str[cs_name.length] == 0);
  *cs= get_charset_by_csname(cs_name.str, MY_CS_PRIMARY, MYF(utf8_flag));

  if (*cs == NULL)
  {
    *cs= dflt_cs;
    return TRUE;
  }

  return FALSE;
}

/*************************************************************************/

bool load_collation(THD *thd, MEM_ROOT *mem_root,
                    Field *field,
                    CHARSET_INFO *dflt_cl,
                    CHARSET_INFO **cl)
{
  LEX_CSTRING cl_name;

  if (field->val_str_nopad(mem_root, &cl_name))
  {
    *cl= dflt_cl;
    return TRUE;
  }
  myf utf8_flag= thd->get_utf8_flag();

  DBUG_ASSERT(cl_name.str[cl_name.length] == 0);
  *cl= get_charset_by_name(cl_name.str, MYF(utf8_flag));

  if (*cl == NULL)
  {
    *cl= dflt_cl;
    return TRUE;
  }

  return FALSE;
}

/*************************************************************************/

Stored_routine_creation_ctx *
Stored_routine_creation_ctx::load_from_db(THD *thd,
                                          const Database_qualified_name *name,
                                          TABLE *proc_tbl)
{
  /* Load character set/collation attributes. */

  CHARSET_INFO *client_cs;
  CHARSET_INFO *connection_cl;
  CHARSET_INFO *db_cl;

  const char *db_name= thd->strmake(name->m_db.str, name->m_db.length);
  const char *sr_name= thd->strmake(name->m_name.str, name->m_name.length);

  bool invalid_creation_ctx= FALSE;

  if (load_charset(thd, thd->mem_root,
                   proc_tbl->field[MYSQL_PROC_FIELD_CHARACTER_SET_CLIENT],
                   thd->variables.character_set_client,
                   &client_cs))
  {
    sql_print_warning("Stored routine '%s'.'%s': invalid value "
                      "in column mysql.proc.character_set_client.",
                      (const char *) db_name,
                      (const char *) sr_name);

    invalid_creation_ctx= TRUE;
  }

  if (load_collation(thd,thd->mem_root,
                     proc_tbl->field[MYSQL_PROC_FIELD_COLLATION_CONNECTION],
                     thd->variables.collation_connection,
                     &connection_cl))
  {
    sql_print_warning("Stored routine '%s'.'%s': invalid value "
                      "in column mysql.proc.collation_connection.",
                      (const char *) db_name,
                      (const char *) sr_name);

    invalid_creation_ctx= TRUE;
  }

  if (load_collation(thd,thd->mem_root,
                     proc_tbl->field[MYSQL_PROC_FIELD_DB_COLLATION],
                     NULL,
                     &db_cl))
  {
    sql_print_warning("Stored routine '%s'.'%s': invalid value "
                      "in column mysql.proc.db_collation.",
                      (const char *) db_name,
                      (const char *) sr_name);

    invalid_creation_ctx= TRUE;
  }

  if (invalid_creation_ctx)
  {
    push_warning_printf(thd,
                        Sql_condition::WARN_LEVEL_WARN,
                        ER_SR_INVALID_CREATION_CTX,
                        ER_THD(thd, ER_SR_INVALID_CREATION_CTX),
                        (const char *) db_name,
                        (const char *) sr_name);
  }

  /*
    If we failed to retrieve the database collation, load the default one
    from the disk.
  */

  if (!db_cl)
    db_cl= get_default_db_collation(thd, name->m_db.str);

  /* Create the context. */

  return new Stored_routine_creation_ctx(client_cs, connection_cl, db_cl);
}

/*************************************************************************/

class Proc_table_intact : public Table_check_intact
{
private:
  bool m_print_once;

public:
  Proc_table_intact() : m_print_once(TRUE) { has_keys= TRUE; }

protected:
  void report_error(uint code, const char *fmt, ...);
};


/**
  Report failure to validate the mysql.proc table definition.
  Print a message to the error log only once.
*/

void Proc_table_intact::report_error(uint code, const char *fmt, ...)
{
  va_list args;
  char buf[512];

  va_start(args, fmt);
  my_vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);

  if (code)
    my_message(code, buf, MYF(0));
  else
    my_error(ER_CANNOT_LOAD_FROM_TABLE_V2, MYF(0), "mysql", "proc");

  if (m_print_once)
  {
    m_print_once= FALSE;
    sql_print_error("%s", buf);
  }
};


/** Single instance used to control printing to the error log. */
static Proc_table_intact proc_table_intact;


/**
  Open the mysql.proc table for read.

  @param thd     Thread context
  @param backup  Pointer to Open_tables_state instance where information about
                 currently open tables will be saved, and from which will be
                 restored when we will end work with mysql.proc.

  NOTES
    On must have a start_new_trans object active when calling this function

  @retval
    0	Error
  @retval
    \#	Pointer to TABLE object of mysql.proc
*/

TABLE *open_proc_table_for_read(THD *thd)
{
  TABLE_LIST table;
  DBUG_ENTER("open_proc_table_for_read");

  DBUG_ASSERT(thd->internal_transaction());

  table.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_PROC_NAME, NULL, TL_READ);

  if (open_system_tables_for_read(thd, &table))
    DBUG_RETURN(NULL);

  if (!proc_table_intact.check(table.table, &proc_table_def))
    DBUG_RETURN(table.table);

  thd->commit_whole_transaction_and_close_tables();

  DBUG_RETURN(NULL);
}


/**
  Open the mysql.proc table for update.

  @param thd  Thread context

  @note
    Table opened with this call should closed using close_thread_tables().

    We don't need to use the start_new_transaction object when calling this
    as there can't be any active transactions when we create or alter
    stored procedures

  @retval
    0	Error
  @retval
    \#	Pointer to TABLE object of mysql.proc
*/

static TABLE *open_proc_table_for_update(THD *thd)
{
  TABLE_LIST table_list;
  TABLE *table;
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();
  DBUG_ENTER("open_proc_table_for_update");

  DBUG_ASSERT(!thd->internal_transaction());

  table_list.init_one_table(&MYSQL_SCHEMA_NAME, &MYSQL_PROC_NAME, NULL,
                            TL_WRITE);

  if (!(table= open_system_table_for_update(thd, &table_list)))
    DBUG_RETURN(NULL);

  if (!proc_table_intact.check(table, &proc_table_def))
    DBUG_RETURN(table);

  thd->commit_whole_transaction_and_close_tables();
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);

  DBUG_RETURN(NULL);
}


/**
  Find row in open mysql.proc table representing stored routine.

  @param thd    Thread context
  @param name   Name of routine
  @param table  TABLE object for open mysql.proc table.

  @retval
    SP_OK             Routine found
  @retval
    SP_KEY_NOT_FOUND  No routine with given name
*/

int
Sp_handler::db_find_routine_aux(THD *thd,
                                const Database_qualified_name *name,
                                TABLE *table) const
{
  uchar key[MAX_KEY_LENGTH];	// db, name, optional key length type
  DBUG_ENTER("db_find_routine_aux");
  DBUG_PRINT("enter", ("type: %s  name: %.*s",
		       type_str(),
		       (int) name->m_name.length, name->m_name.str));

  /*
    Create key to find row. We have to use field->store() to be able to
    handle VARCHAR and CHAR fields.
    Assumption here is that the three first fields in the table are
    'db', 'name' and 'type' and the first key is the primary key over the
    same fields.
  */
  if (name->m_name.length > table->field[1]->field_length)
    DBUG_RETURN(SP_KEY_NOT_FOUND);
  table->field[0]->store(name->m_db, &my_charset_bin);
  table->field[1]->store(name->m_name, &my_charset_bin);
  table->field[2]->store((longlong) type(), true);
  key_copy(key, table->record[0], table->key_info,
           table->key_info->key_length);

  if (table->file->ha_index_read_idx_map(table->record[0], 0, key,
                                         HA_WHOLE_KEY,
                                         HA_READ_KEY_EXACT))
    DBUG_RETURN(SP_KEY_NOT_FOUND);

  DBUG_RETURN(SP_OK);
}


bool st_sp_chistics::read_from_mysql_proc_row(THD *thd, TABLE *table)
{
  LEX_CSTRING str;

  if (table->field[MYSQL_PROC_FIELD_ACCESS]->val_str_nopad(thd->mem_root,
                                                           &str))
    return true;

  switch (str.str[0]) {
  case 'N':
    daccess= SP_NO_SQL;
    break;
  case 'C':
    daccess= SP_CONTAINS_SQL;
    break;
  case 'R':
    daccess= SP_READS_SQL_DATA;
    break;
  case 'M':
    daccess= SP_MODIFIES_SQL_DATA;
    break;
  default:
    daccess= SP_DEFAULT_ACCESS_MAPPING;
  }

  if (table->field[MYSQL_PROC_FIELD_DETERMINISTIC]->val_str_nopad(thd->mem_root,
                                                                  &str))
    return true;
  detistic= str.str[0] == 'N' ? false : true;

  if (table->field[MYSQL_PROC_FIELD_SECURITY_TYPE]->val_str_nopad(thd->mem_root,
                                                                  &str))
    return true;
  suid= str.str[0] == 'I' ? SP_IS_NOT_SUID : SP_IS_SUID;

  if (table->field[MYSQL_PROC_FIELD_AGGREGATE]->val_str_nopad(thd->mem_root,
                                                              &str))
    return true;

  switch (str.str[0]) {
  case 'N':
    agg_type= NOT_AGGREGATE;
    break;
  case 'G':
    agg_type= GROUP_AGGREGATE;
    break;
  default:
    agg_type= DEFAULT_AGGREGATE;
  }


  if (table->field[MYSQL_PROC_FIELD_COMMENT]->val_str_nopad(thd->mem_root,
                                                            &comment))
    return true;

  return false;
}


bool AUTHID::read_from_mysql_proc_row(THD *thd, TABLE *table)
{
  LEX_CSTRING str;
  if (table->field[MYSQL_PROC_FIELD_DEFINER]->val_str_nopad(thd->mem_root,
                                                            &str))
    return true;
  parse(str.str, str.length);
  if (user.str[user.length])
    ((char *) user.str)[user.length]= '\0'; // 0-terminate if was truncated
  return false;
}


/**
  Find routine definition in mysql.proc table and create corresponding
  sp_head object for it.

  @param thd   Thread context
  @param name  Name of routine
  @param sphp  Out parameter in which pointer to created sp_head
               object is returned (0 in case of error).

  @note
    This function may damage current LEX during execution, so it is good
    idea to create temporary LEX and make it active before calling it.

  @retval
    0       Success
  @retval
    non-0   Error (may be one of special codes like SP_KEY_NOT_FOUND)
*/

int
Sp_handler::db_find_routine(THD *thd,
                            const Database_qualified_name *name,
                            sp_head **sphp) const
{
  TABLE *table;
  LEX_CSTRING params, returns, body;
  int ret;
  longlong created;
  longlong modified;
  Sp_chistics chistics;
  bool saved_time_zone_used= thd->time_zone_used;
  bool trans_commited= 0;
  sql_mode_t sql_mode;
  Stored_program_creation_ctx *creation_ctx;
  AUTHID definer;
  DBUG_ENTER("db_find_routine");
  DBUG_PRINT("enter", ("type: %s name: %.*s",
		       type_str(),
		       (int) name->m_name.length, name->m_name.str));

  *sphp= 0;                                     // In case of errors

  start_new_trans new_trans(thd);
  Sql_mode_instant_set sms(thd, 0);

  if (!(table= open_proc_table_for_read(thd)))
  {
    ret= SP_OPEN_TABLE_FAILED;
    goto done;
  }

  if ((ret= db_find_routine_aux(thd, name, table)) != SP_OK)
    goto done;

  if (table->s->fields < MYSQL_PROC_FIELD_COUNT)
  {
    ret= SP_GET_FIELD_FAILED;
    goto done;
  }

  if (chistics.read_from_mysql_proc_row(thd, table) ||
      definer.read_from_mysql_proc_row(thd, table))
  {
    ret= SP_GET_FIELD_FAILED;
    goto done;
  }

  table->field[MYSQL_PROC_FIELD_PARAM_LIST]->val_str_nopad(thd->mem_root,
                                                           &params);
  if (type() != SP_TYPE_FUNCTION)
    returns= empty_clex_str;
  else if (table->field[MYSQL_PROC_FIELD_RETURNS]->val_str_nopad(thd->mem_root,
                                                                 &returns))
  {
    ret= SP_GET_FIELD_FAILED;
    goto done;
  }

  if (table->field[MYSQL_PROC_FIELD_BODY]->val_str_nopad(thd->mem_root,
                                                         &body))
  {
    ret= SP_GET_FIELD_FAILED;
    goto done;
  }

  // Get additional information
  modified= table->field[MYSQL_PROC_FIELD_MODIFIED]->val_int();
  created= table->field[MYSQL_PROC_FIELD_CREATED]->val_int();
  sql_mode= (sql_mode_t) table->field[MYSQL_PROC_FIELD_SQL_MODE]->val_int();

  creation_ctx= Stored_routine_creation_ctx::load_from_db(thd, name, table);

  trans_commited= 1;
  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();

  ret= db_load_routine(thd, name, sphp,
                       sql_mode, params, returns, body, chistics, definer,
                       created, modified, NULL, creation_ctx);
 done:
  /* 
    Restore the time zone flag as the timezone usage in proc table
    does not affect replication.
  */  
  thd->time_zone_used= saved_time_zone_used;
  if (!trans_commited)
  {
    if (table)
      thd->commit_whole_transaction_and_close_tables();
    new_trans.restore_old_transaction();
  }
  DBUG_RETURN(ret);
}


int
Sp_handler::db_find_and_cache_routine(THD *thd,
                                      const Database_qualified_name *name,
                                      sp_head **sp) const
{
  int rc= db_find_routine(thd, name, sp);
  if (rc == SP_OK)
  {
    sp_cache_insert(get_cache(thd), *sp);
    DBUG_PRINT("info", ("added new: %p, level: %lu, flags %x",
                        sp[0], sp[0]->m_recursion_level,
                        sp[0]->m_flags));
  }
  return rc;
}


/**
  Silence DEPRECATED SYNTAX warnings when loading a stored procedure
  into the cache.
*/

struct Silence_deprecated_warning : public Internal_error_handler
{
public:
  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_warning_level *level,
                                const char* msg,
                                Sql_condition ** cond_hdl);
};

bool
Silence_deprecated_warning::handle_condition(
  THD *,
  uint sql_errno,
  const char*,
  Sql_condition::enum_warning_level *level,
  const char*,
  Sql_condition ** cond_hdl)
{
  *cond_hdl= NULL;
  if (sql_errno == ER_WARN_DEPRECATED_SYNTAX &&
      *level == Sql_condition::WARN_LEVEL_WARN)
    return TRUE;

  return FALSE;
}


/**
  @brief    The function parses input strings and returns SP stucture.

  @param[in]      thd               Thread handler
  @param[in]      defstr            CREATE... string
  @param[in]      sql_mode          SQL mode
  @param[in]      parent            The owner package for package routines,
                                    or NULL for standalone routines.
  @param[in]      creation_ctx      Creation context of stored routines
                                    
  @return     Pointer on sp_head struct
    @retval   #                     Pointer on sp_head struct
    @retval   0                     error
*/

static sp_head *sp_compile(THD *thd, String *defstr, sql_mode_t sql_mode,
                           sp_package *parent,
                           Stored_program_creation_ctx *creation_ctx)
{
  sp_head *sp;
  sql_mode_t old_sql_mode= thd->variables.sql_mode;
  ha_rows old_select_limit= thd->variables.select_limit;
  sp_rcontext *old_spcont= thd->spcont;
  Silence_deprecated_warning warning_handler;
  Parser_state parser_state;

  thd->variables.sql_mode= sql_mode;
  thd->variables.select_limit= HA_POS_ERROR;

  if (parser_state.init(thd, defstr->c_ptr_safe(), defstr->length()))
  {
    thd->variables.sql_mode= old_sql_mode;
    thd->variables.select_limit= old_select_limit;
    return NULL;
  }

  lex_start(thd);
  thd->lex->sphead= parent;
  thd->push_internal_handler(&warning_handler);
  thd->spcont= 0;

  if (parse_sql(thd, & parser_state, creation_ctx) || thd->lex == NULL)
  {
    sp= thd->lex->sphead;
    sp_head::destroy(sp);
    sp= 0;
  }
  else
  {
    sp= thd->lex->sphead;
  }

  thd->pop_internal_handler();
  thd->spcont= old_spcont;
  thd->variables.sql_mode= old_sql_mode;
  thd->variables.select_limit= old_select_limit;
  if (sp != NULL)
    sp->init_psi_share();
  return sp;
}


class Bad_db_error_handler : public Internal_error_handler
{
public:
  Bad_db_error_handler()
    :m_error_caught(false)
  {}

  virtual bool handle_condition(THD *thd,
                                uint sql_errno,
                                const char* sqlstate,
                                Sql_condition::enum_warning_level *level,
                                const char* message,
                                Sql_condition ** cond_hdl);

  bool error_caught() const { return m_error_caught; }

private:
  bool m_error_caught;
};

bool
Bad_db_error_handler::handle_condition(THD *thd,
                                       uint sql_errno,
                                       const char* sqlstate,
                                       Sql_condition::enum_warning_level
                                       *level,
                                       const char* message,
                                       Sql_condition ** cond_hdl)
{
  if (sql_errno == ER_BAD_DB_ERROR)
  {
    m_error_caught= true;
    return true;
  }
  return false;
}


int
Sp_handler::db_load_routine(THD *thd, const Database_qualified_name *name,
                            sp_head **sphp,
                            sql_mode_t sql_mode,
                            const LEX_CSTRING &params,
                            const LEX_CSTRING &returns,
                            const LEX_CSTRING &body,
                            const st_sp_chistics &chistics,
                            const AUTHID &definer,
                            longlong created, longlong modified,
                            sp_package *parent,
                            Stored_program_creation_ctx *creation_ctx) const
{
  LEX *old_lex= thd->lex, newlex;
  String defstr;
  char saved_cur_db_name_buf[SAFE_NAME_LEN+1];
  LEX_STRING saved_cur_db_name=
    { saved_cur_db_name_buf, sizeof(saved_cur_db_name_buf) };
  bool cur_db_changed;
  Bad_db_error_handler db_not_exists_handler;

  int ret= 0;

  thd->lex= &newlex;
  newlex.current_select= NULL;

  defstr.set_charset(creation_ctx->get_client_cs());
  defstr.set_thread_specific();

  /*
    We have to add DEFINER clause and provide proper routine characterstics in
    routine definition statement that we build here to be able to use this
    definition for SHOW CREATE PROCEDURE later.
   */

  if (show_create_sp(thd, &defstr,
                     null_clex_str, name->m_name,
                     params, returns, body,
                     chistics, definer, DDL_options(), sql_mode))
  {
    ret= SP_INTERNAL_ERROR;
    goto end;
  }

  thd->push_internal_handler(&db_not_exists_handler);
  /*
    Change the current database (if needed).

    TODO: why do we force switch here?
  */

  if (mysql_opt_change_db(thd, &name->m_db, &saved_cur_db_name, TRUE,
                          &cur_db_changed))
  {
    ret= SP_INTERNAL_ERROR;
    thd->pop_internal_handler();
    goto end;
  }
  thd->pop_internal_handler();
  if (db_not_exists_handler.error_caught())
  {
    ret= SP_INTERNAL_ERROR;
    my_error(ER_BAD_DB_ERROR, MYF(0), name->m_db.str);

    goto end;
  }

  {
    *sphp= sp_compile(thd, &defstr, sql_mode, parent, creation_ctx);
    /*
      Force switching back to the saved current database (if changed),
      because it may be NULL. In this case, mysql_change_db() would
      generate an error.
    */

    if (cur_db_changed && mysql_change_db(thd,
                                          (LEX_CSTRING*) &saved_cur_db_name,
                                          TRUE))
    {
      ret= SP_INTERNAL_ERROR;
      goto end;
    }

    if (!*sphp)
    {
      ret= SP_PARSE_ERROR;
      goto end;
    }

    (*sphp)->set_definer(&definer.user, &definer.host);
    (*sphp)->set_info(created, modified, chistics, sql_mode);
    (*sphp)->set_creation_ctx(creation_ctx);
    (*sphp)->optimize();

    if (type() == SP_TYPE_PACKAGE_BODY)
    {
      sp_package *package= (*sphp)->get_package();
      List_iterator<LEX> it(package->m_routine_implementations);
      for (LEX *lex; (lex= it++); )
      {
        DBUG_ASSERT(lex->sphead);
        lex->sphead->set_definer(&definer.user, &definer.host);
        lex->sphead->set_suid(package->suid());
        lex->sphead->m_sql_mode= sql_mode;
        lex->sphead->set_creation_ctx(creation_ctx);
        lex->sphead->optimize();
      }
    }

    /*
      Not strictly necessary to invoke this method here, since we know
      that we've parsed CREATE PROCEDURE/FUNCTION and not an
      UPDATE/DELETE/INSERT/REPLACE/LOAD/CREATE TABLE, but we try to
      maintain the invariant that this method is called for each
      distinct statement, in case its logic is extended with other
      types of analyses in future.
    */
    newlex.set_trg_event_type_for_tables();
  }

end:
  thd->lex->sphead= NULL;
  lex_end(thd->lex);
  thd->lex= old_lex;
  return ret;
}


void
sp_returns_type(THD *thd, String &result, const sp_head *sp)
{
  TABLE table;
  TABLE_SHARE share;
  Field *field;
  bzero((char*) &table, sizeof(table));
  bzero((char*) &share, sizeof(share));
  table.in_use= thd;
  table.s = &share;
  field= sp->create_result_field(0, 0, &table);
  field->sql_type(result);

  if (field->has_charset())
  {
    result.append(STRING_WITH_LEN(" CHARSET "));
    result.append(field->charset()->cs_name);
    if (!(field->charset()->state & MY_CS_PRIMARY))
    {
      result.append(STRING_WITH_LEN(" COLLATE "));
      result.append(field->charset()->coll_name);
    }
  }

  delete field;
}


/**
  Delete the record for the stored routine object from mysql.proc,
  which is already opened, locked, and positioned to the record with the
  record to be deleted.

  The operation deletes the record for the current record in "table"
  and invalidates the stored-routine cache.

  @param thd    Thread context.
  @param name   Stored routine name.
  @param table  A pointer to the opened mysql.proc table

  @returns      Error code.
  @return       SP_OK on success, or SP_DELETE_ROW_FAILED on error.
  used to indicate about errors.
*/

int
Sp_handler::sp_drop_routine_internal(THD *thd,
                                     const Database_qualified_name *name,
                                     TABLE *table) const
{
  DBUG_ENTER("sp_drop_routine_internal");

  if (table->file->ha_delete_row(table->record[0]))
    DBUG_RETURN(SP_DELETE_ROW_FAILED);

  /* Make change permanent and avoid 'table is marked as crashed' errors */
  table->file->extra(HA_EXTRA_FLUSH);

  sp_cache_invalidate();
  /*
    A lame workaround for lack of cache flush:
    make sure the routine is at least gone from the
    local cache.
  */
  sp_head *sp;
  sp_cache **spc= get_cache(thd);
  DBUG_ASSERT(spc);
  if ((sp= sp_cache_lookup(spc, name)))
    sp_cache_flush_obsolete(spc, &sp);
  /* Drop statistics for this stored program from performance schema. */
  MYSQL_DROP_SP(type(), name->m_db.str, static_cast<uint>(name->m_db.length),
                        name->m_name.str, static_cast<uint>(name->m_name.length));
  DBUG_RETURN(SP_OK);
}


int
Sp_handler::sp_find_and_drop_routine(THD *thd, TABLE *table,
                                     const Database_qualified_name *name) const
{
  int ret;
  if ((ret= db_find_routine_aux(thd, name, table)) != SP_OK)
    return ret;
  return sp_drop_routine_internal(thd, name, table);
}


int
Sp_handler_package_spec::
  sp_find_and_drop_routine(THD *thd, TABLE *table,
                           const Database_qualified_name *name) const
{
  int ret;
  if ((ret= db_find_routine_aux(thd, name, table)) != SP_OK)
    return ret;
  /*
    When we do "DROP PACKAGE pkg", we should also perform
    "DROP PACKAGE BODY pkg" automatically.
  */
  ret= sp_handler_package_body.sp_find_and_drop_routine(thd, table, name);
  if (ret != SP_KEY_NOT_FOUND && ret != SP_OK)
  {
    /*
      - SP_KEY_NOT_FOUND means that "CREATE PACKAGE pkg" did not
        have a correspoinding "CREATE PACKAGE BODY pkg" yet.
      - SP_OK means that "CREATE PACKAGE pkg" had a correspoinding
        "CREATE PACKAGE BODY pkg", which was successfully dropped.
    */
    return ret; // Other codes mean an unexpecte error
  }
  return Sp_handler::sp_find_and_drop_routine(thd, table, name);
}


/**
  Write stored-routine object into mysql.proc.

  This operation stores attributes of the stored procedure/function into
  the mysql.proc.

  @param thd  Thread context.
  @param sp   Stored routine object to store.

  @note Opens and closes the thread tables. Therefore assumes
  that there are no locked tables in this thread at the time of
  invocation.
  Unlike some other DDL statements, *does* close the tables
  in the end, since the call to this function is normally
  followed by an implicit grant (sp_grant_privileges())
  and this subsequent call opens and closes mysql.procs_priv.

  @return Error status.
    @retval FALSE on success
    @retval TRUE on error
*/

bool
Sp_handler::sp_create_routine(THD *thd, const sp_head *sp) const
{
  LEX *lex= thd->lex;
  bool ret= TRUE;
  TABLE *table;
  char definer_buf[USER_HOST_BUFF_SIZE];
  LEX_CSTRING definer;
  sql_mode_t org_sql_mode= thd->variables.sql_mode;
  enum_check_fields org_count_cuted_fields= thd->count_cuted_fields;
  CHARSET_INFO *db_cs= get_default_db_collation(thd, sp->m_db.str);
  bool store_failed= FALSE;
  DBUG_ENTER("sp_create_routine");
  DBUG_PRINT("enter", ("type: %s  name: %.*s",
                       type_str(),
                       (int) sp->m_name.length,
                       sp->m_name.str));
  MDL_key::enum_mdl_namespace mdl_type= get_mdl_type();
  LEX_CSTRING returns= empty_clex_str;
  String retstr(64);
  retstr.set_charset(system_charset_info);

  /* Grab an exclusive MDL lock. */
  if (lock_object_name(thd, mdl_type, sp->m_db.str, sp->m_name.str))
  {
    my_error(ER_BAD_DB_ERROR, MYF(0), sp->m_db.str);
    DBUG_RETURN(TRUE);
  }

  /*
    Check that a database directory with this name
    exists. Design note: This won't work on virtual databases
    like information_schema.
  */
  if (check_db_dir_existence(sp->m_db.str))
  {
    my_error(ER_BAD_DB_ERROR, MYF(0), sp->m_db.str);
    DBUG_RETURN(TRUE);
  }


  /* Reset sql_mode during data dictionary operations. */
  thd->variables.sql_mode= 0;
  thd->count_cuted_fields= CHECK_FIELD_WARN;

  if (!(table= open_proc_table_for_update(thd)))
  {
    my_error(ER_SP_STORE_FAILED, MYF(0), type_str(), sp->m_name.str);
    goto done;
  }
  else
  {
    /* Checking if the routine already exists */
    if (db_find_routine_aux(thd, sp, table) == SP_OK)
    {
      if (lex->create_info.or_replace())
      {
        switch (type()) {
        case SP_TYPE_PACKAGE:
          // Drop together with its PACKAGE BODY mysql.proc record
          if (sp_handler_package_spec.sp_find_and_drop_routine(thd, table, sp))
            goto done;
          break;
        case SP_TYPE_PACKAGE_BODY:
        case SP_TYPE_FUNCTION:
        case SP_TYPE_PROCEDURE:
          if (sp_drop_routine_internal(thd, sp, table))
            goto done;
          break;
        case SP_TYPE_TRIGGER:
        case SP_TYPE_EVENT:
          DBUG_ASSERT(0);
          ret= SP_OK;
        }
      }
      else if (lex->create_info.if_not_exists())
      {
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_SP_ALREADY_EXISTS,
                            ER_THD(thd, ER_SP_ALREADY_EXISTS),
                            type_str(), sp->m_name.str);

        ret= FALSE;

        // Setting retstr as it is used for logging.
        if (type() == SP_TYPE_FUNCTION)
        {
          sp_returns_type(thd, retstr, sp);
          retstr.get_value(&returns);
        }
        goto log;
      }
      else
      {
        my_error(ER_SP_ALREADY_EXISTS, MYF(0), type_str(), sp->m_name.str);
        goto done;
      }
    }

    restore_record(table, s->default_values); // Get default values for fields

    /* NOTE: all needed privilege checks have been already done. */
    thd->lex->definer->set_lex_string(&definer, definer_buf);

    if (table->s->fields < MYSQL_PROC_FIELD_COUNT)
    {
      my_error(ER_SP_STORE_FAILED, MYF(0), type_str(), sp->m_name.str);
      goto done;
    }

    if (system_charset_info->numchars(sp->m_name.str,
                                      sp->m_name.str + sp->m_name.length) >
        table->field[MYSQL_PROC_FIELD_NAME]->char_length())
    {
      my_error(ER_TOO_LONG_IDENT, MYF(0), sp->m_name.str);
      goto done;
    }
    if (sp->m_body.length > table->field[MYSQL_PROC_FIELD_BODY]->field_length)
    {
      my_error(ER_TOO_LONG_BODY, MYF(0), sp->m_name.str);
      goto done;
    }

    store_failed=
      table->field[MYSQL_PROC_FIELD_DB]->
        store(sp->m_db, system_charset_info);

    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_NAME]->
        store(sp->m_name, system_charset_info);

    if (sp->agg_type() != DEFAULT_AGGREGATE)
    {
      store_failed= store_failed ||
        table->field[MYSQL_PROC_FIELD_AGGREGATE]->
          store((longlong)sp->agg_type(),TRUE);
    }

    store_failed= store_failed ||
      table->field[MYSQL_PROC_MYSQL_TYPE]->
        store((longlong) type(), true);

    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_SPECIFIC_NAME]->
        store(sp->m_name, system_charset_info);

    if (sp->daccess() != SP_DEFAULT_ACCESS)
    {
      store_failed= store_failed ||
        table->field[MYSQL_PROC_FIELD_ACCESS]->
          store((longlong)sp->daccess(), TRUE);
    }

    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_DETERMINISTIC]->
        store((longlong)(sp->detistic() ? 1 : 2), TRUE);

    if (sp->suid() != SP_IS_DEFAULT_SUID)
    {
      store_failed= store_failed ||
        table->field[MYSQL_PROC_FIELD_SECURITY_TYPE]->
          store((longlong)sp->suid(), TRUE);
    }

    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_PARAM_LIST]->
        store(sp->m_params, system_charset_info);

    if (type() == SP_TYPE_FUNCTION)
    {
      sp_returns_type(thd, retstr, sp);
      retstr.get_value(&returns);

      store_failed= store_failed ||
        table->field[MYSQL_PROC_FIELD_RETURNS]->
          store(retstr.ptr(), retstr.length(), system_charset_info);
    }

    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_BODY]->
        store(sp->m_body, system_charset_info);

    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_DEFINER]->
        store(definer, system_charset_info);

    table->field[MYSQL_PROC_FIELD_CREATED]->set_time();
    table->field[MYSQL_PROC_FIELD_MODIFIED]->set_time();

    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_SQL_MODE]->
        store((longlong) org_sql_mode, TRUE);

    if (sp->comment().str)
    {
      store_failed= store_failed ||
        table->field[MYSQL_PROC_FIELD_COMMENT]->
          store(sp->comment(), system_charset_info);
    }

    if (type() == SP_TYPE_FUNCTION &&
        !trust_function_creators && mysql_bin_log.is_open())
    {
      if (!sp->detistic())
      {
	/*
	  Note that this test is not perfect; one could use
	  a non-deterministic read-only function in an update statement.
	*/
	enum enum_sp_data_access access=
	  (sp->daccess() == SP_DEFAULT_ACCESS) ?
	  SP_DEFAULT_ACCESS_MAPPING : sp->daccess();
	if (access == SP_CONTAINS_SQL ||
	    access == SP_MODIFIES_SQL_DATA)
	{
          my_error(ER_BINLOG_UNSAFE_ROUTINE, MYF(0));
	  goto done;
	}
      }
      if (!(thd->security_ctx->master_access & PRIV_LOG_BIN_TRUSTED_SP_CREATOR))
      {
        my_error(ER_BINLOG_CREATE_ROUTINE_NEED_SUPER,MYF(0));
	goto done;
      }
    }

    table->field[MYSQL_PROC_FIELD_CHARACTER_SET_CLIENT]->set_notnull();
    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_CHARACTER_SET_CLIENT]->
      store(&thd->charset()->cs_name, system_charset_info);

    table->field[MYSQL_PROC_FIELD_COLLATION_CONNECTION]->set_notnull();
    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_COLLATION_CONNECTION]->
      store(&thd->variables.collation_connection->coll_name,
            system_charset_info);

    table->field[MYSQL_PROC_FIELD_DB_COLLATION]->set_notnull();
    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_DB_COLLATION]->
      store(&db_cs->coll_name, system_charset_info);

    table->field[MYSQL_PROC_FIELD_BODY_UTF8]->set_notnull();
    store_failed= store_failed ||
      table->field[MYSQL_PROC_FIELD_BODY_UTF8]->store(
        sp->m_body_utf8, system_charset_info);

    if (store_failed)
    {
      my_error(ER_CANT_CREATE_SROUTINE, MYF(0), sp->m_name.str);
      goto done;
    }

    if (table->file->ha_write_row(table->record[0]))
    {
      my_error(ER_SP_ALREADY_EXISTS, MYF(0), type_str(), sp->m_name.str);
      goto done;
    }
    /* Make change permanent and avoid 'table is marked as crashed' errors */
    table->file->extra(HA_EXTRA_FLUSH);

    sp_cache_invalidate();
  }

log:
  if (mysql_bin_log.is_open())
  {
    thd->clear_error();

    StringBuffer<128> log_query(thd->variables.character_set_client);
    DBUG_ASSERT(log_query.charset()->mbminlen == 1);

    if (show_create_sp(thd, &log_query,
                       sp->m_explicit_name ? sp->m_db : null_clex_str,
                       sp->m_name,
                       sp->m_params, returns, sp->m_body,
                       sp->chistics(),
                       thd->lex->definer[0],
                       thd->lex->create_info,
                       org_sql_mode))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      goto done;
    }
    /* restore sql_mode when binloging */
    thd->variables.sql_mode= org_sql_mode;
    /* Such a statement can always go directly to binlog, no trans cache */
    if (thd->binlog_query(THD::STMT_QUERY_TYPE,
                          log_query.ptr(), log_query.length(),
                          FALSE, FALSE, FALSE, 0) > 0)
    {
      my_error(ER_ERROR_ON_WRITE, MYF(0), "binary log", -1);
      goto done;
    }
  }
  ret= FALSE;

done:
  thd->variables.sql_mode= org_sql_mode;
  thd->count_cuted_fields= org_count_cuted_fields;
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  DBUG_RETURN(ret);
}


static bool
append_suid(String *buf, enum_sp_suid_behaviour suid)
{
  return suid == SP_IS_NOT_SUID &&
         buf->append(STRING_WITH_LEN("    SQL SECURITY INVOKER\n"));
}


static bool
append_comment(String *buf, const LEX_CSTRING &comment)
{
  if (!comment.length)
    return false;
  if (buf->append(STRING_WITH_LEN("    COMMENT ")))
    return true;
  append_unescaped(buf, comment.str, comment.length);
  return buf->append('\n');
}


static bool
append_package_chistics(String *buf, const st_sp_chistics &chistics)
{
  return append_suid(buf, chistics.suid) ||
         append_comment(buf, chistics.comment);
}


bool
Sp_handler_package::show_create_sp(THD *thd, String *buf,
                                   const LEX_CSTRING &db,
                                   const LEX_CSTRING &name,
                                   const LEX_CSTRING &params,
                                   const LEX_CSTRING &returns,
                                   const LEX_CSTRING &body,
                                   const st_sp_chistics &chistics,
                                   const AUTHID &definer,
                                   const DDL_options_st ddl_options,
                                   sql_mode_t sql_mode) const
{
  Sql_mode_instant_set sms(thd, sql_mode);
  bool rc=
    buf->append(STRING_WITH_LEN("CREATE ")) ||
    (ddl_options.or_replace() &&
     buf->append(STRING_WITH_LEN("OR REPLACE "))) ||
    append_definer(thd, buf, &definer.user, &definer.host) ||
    buf->append(type_lex_cstring()) ||
    buf->append(' ') ||
    (ddl_options.if_not_exists() &&
     buf->append(STRING_WITH_LEN("IF NOT EXISTS "))) ||
    (db.length > 0 &&
     (append_identifier(thd, buf, db.str, db.length) ||
      buf->append('.'))) ||
    append_identifier(thd, buf, name.str, name.length) ||
    append_package_chistics(buf, chistics) ||
    buf->append(' ') ||
    buf->append(body.str, body.length);
  return rc;
}


/**
  Delete the record for the stored routine object from mysql.proc
  and do binary logging.

  The operation deletes the record for the stored routine specified by name
  from the mysql.proc table and invalidates the stored-routine cache.

  @param thd  Thread context.
  @param name Stored routine name.

  @return Error code. SP_OK is returned on success. Other SP_ constants are
  used to indicate about errors.
*/

int
Sp_handler::sp_drop_routine(THD *thd,
                            const Database_qualified_name *name) const
{
  TABLE *table;
  int ret;
  DBUG_ENTER("sp_drop_routine");
  DBUG_PRINT("enter", ("type: %s  name: %.*s",
		       type_str(),
		       (int) name->m_name.length, name->m_name.str));
  MDL_key::enum_mdl_namespace mdl_type= get_mdl_type();

  /* Grab an exclusive MDL lock. */
  if (lock_object_name(thd, mdl_type, name->m_db.str, name->m_name.str))
    DBUG_RETURN(SP_DELETE_ROW_FAILED);

  if (!(table= open_proc_table_for_update(thd)))
    DBUG_RETURN(SP_OPEN_TABLE_FAILED);

  if ((ret= sp_find_and_drop_routine(thd, table, name)) == SP_OK &&
      write_bin_log(thd, TRUE, thd->query(), thd->query_length()))
    ret= SP_INTERNAL_ERROR;
  /*
    This statement will be replicated as a statement, even when using
    row-based replication.  The flag will be reset at the end of the
    statement.
  */
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  DBUG_RETURN(ret);
}


/**
  Find and updated the record for the stored routine object in mysql.proc.

  The operation finds the record for the stored routine specified by name
  in the mysql.proc table and updates it with new attributes. After
  successful update, the cache is invalidated.

  @param thd      Thread context.
  @param name     Stored routine name.
  @param chistics New values of stored routine attributes to write.

  @return Error code. SP_OK is returned on success. Other SP_ constants are
  used to indicate about errors.
*/

int
Sp_handler::sp_update_routine(THD *thd, const Database_qualified_name *name,
                              const st_sp_chistics *chistics) const
{
  TABLE *table;
  int ret;
  DBUG_ENTER("sp_update_routine");
  DBUG_PRINT("enter", ("type: %s  name: %.*s",
                       type_str(),
                       (int) name->m_name.length, name->m_name.str));
  MDL_key::enum_mdl_namespace mdl_type= get_mdl_type();

  /* Grab an exclusive MDL lock. */
  if (lock_object_name(thd, mdl_type, name->m_db.str, name->m_name.str))
    DBUG_RETURN(SP_OPEN_TABLE_FAILED);

  if (!(table= open_proc_table_for_update(thd)))
    DBUG_RETURN(SP_OPEN_TABLE_FAILED);

  if ((ret= db_find_routine_aux(thd, name, table)) == SP_OK)
  {
    if (type() == SP_TYPE_FUNCTION && ! trust_function_creators &&
        mysql_bin_log.is_open() &&
        (chistics->daccess == SP_CONTAINS_SQL ||
         chistics->daccess == SP_MODIFIES_SQL_DATA))
    {
      char *ptr;
      bool is_deterministic;
      ptr= get_field(thd->mem_root,
                     table->field[MYSQL_PROC_FIELD_DETERMINISTIC]);
      if (ptr == NULL)
      {
        ret= SP_INTERNAL_ERROR;
        goto err;
      }
      is_deterministic= ptr[0] == 'N' ? FALSE : TRUE;
      if (!is_deterministic)
      {
        my_message(ER_BINLOG_UNSAFE_ROUTINE,
                   ER_THD(thd, ER_BINLOG_UNSAFE_ROUTINE), MYF(0));
        ret= SP_INTERNAL_ERROR;
        goto err;
      }
    }

    store_record(table,record[1]);
    table->field[MYSQL_PROC_FIELD_MODIFIED]->set_time();
    if (chistics->suid != SP_IS_DEFAULT_SUID)
      table->field[MYSQL_PROC_FIELD_SECURITY_TYPE]->
	store((longlong)chistics->suid, TRUE);
    if (chistics->daccess != SP_DEFAULT_ACCESS)
      table->field[MYSQL_PROC_FIELD_ACCESS]->
	store((longlong)chistics->daccess, TRUE);
    if (chistics->comment.str)
      table->field[MYSQL_PROC_FIELD_COMMENT]->store(chistics->comment,
						    system_charset_info);
    if (chistics->agg_type != DEFAULT_AGGREGATE)
      table->field[MYSQL_PROC_FIELD_AGGREGATE]->
         store((longlong)chistics->agg_type, TRUE);
    if ((ret= table->file->ha_update_row(table->record[1],table->record[0])) &&
        ret != HA_ERR_RECORD_IS_THE_SAME)
      ret= SP_WRITE_ROW_FAILED;
    else
      ret= 0;
    /* Make change permanent and avoid 'table is marked as crashed' errors */
    table->file->extra(HA_EXTRA_FLUSH);
  }

  if (ret == SP_OK)
  {
    if (write_bin_log(thd, TRUE, thd->query(), thd->query_length()))
      ret= SP_INTERNAL_ERROR;
    sp_cache_invalidate();
  }
err:
  DBUG_ASSERT(!thd->is_current_stmt_binlog_format_row());
  DBUG_RETURN(ret);
}


/**
  This internal handler is used to trap errors from opening mysql.proc.
*/

class Lock_db_routines_error_handler : public Internal_error_handler
{
public:
  bool handle_condition(THD *thd,
                        uint sql_errno,
                        const char* sqlstate,
                        Sql_condition::enum_warning_level *level,
                        const char* msg,
                        Sql_condition ** cond_hdl)
  {
    if (sql_errno == ER_NO_SUCH_TABLE ||
        sql_errno == ER_NO_SUCH_TABLE_IN_ENGINE ||
        sql_errno == ER_CANNOT_LOAD_FROM_TABLE_V2 ||
        sql_errno == ER_COL_COUNT_DOESNT_MATCH_PLEASE_UPDATE ||
        sql_errno == ER_COL_COUNT_DOESNT_MATCH_CORRUPTED_V2)
      return true;
    return false;
  }
};


/**
   Acquires exclusive metadata lock on all stored routines in the
   given database.

   @note Will also return false (=success) if mysql.proc can't be opened
         or is outdated. This allows DROP DATABASE to continue in these
         cases.
 */

bool lock_db_routines(THD *thd, const char *db)
{
  TABLE *table;
  uint key_len;
  MDL_request_list mdl_requests;
  Lock_db_routines_error_handler err_handler;
  uchar keybuf[MAX_KEY_LENGTH];
  DBUG_ENTER("lock_db_routines");

  DBUG_SLOW_ASSERT(ok_for_lower_case_names(db));

  start_new_trans new_trans(thd);

  /*
    mysql.proc will be re-opened during deletion, so we can ignore
    errors when opening the table here. The error handler is
    used to avoid getting the same warning twice.
  */
  thd->push_internal_handler(&err_handler);
  table= open_proc_table_for_read(thd);
  thd->pop_internal_handler();
  if (!table)
  {
    /*
      DROP DATABASE should not fail even if mysql.proc does not exist
      or is outdated. We therefore only abort mysql_rm_db() if we
      have errors not handled by the error handler.
    */
    new_trans.restore_old_transaction();
    DBUG_RETURN(thd->is_error() || thd->killed);
  }

  table->field[MYSQL_PROC_FIELD_DB]->store(db, strlen(db), system_charset_info);
  key_len= table->key_info->key_part[0].store_length;
  table->field[MYSQL_PROC_FIELD_DB]->get_key_image(keybuf, key_len, Field::itRAW);
  int nxtres= table->file->ha_index_init(0, 1);
  if (nxtres)
  {
    table->file->print_error(nxtres, MYF(0));
    goto error;
  }

  if (!table->file->ha_index_read_map(table->record[0], keybuf, (key_part_map)1,
                                       HA_READ_KEY_EXACT))
  {
    do
    {
      char *sp_name= get_field(thd->mem_root,
                               table->field[MYSQL_PROC_FIELD_NAME]);
      if (sp_name == NULL) // skip invalid sp names (hand-edited mysql.proc?)
        continue;

      longlong sp_type= table->field[MYSQL_PROC_MYSQL_TYPE]->val_int();
      MDL_request *mdl_request= new (thd->mem_root) MDL_request;
      const Sp_handler *sph= Sp_handler::handler((enum_sp_type)
                                                 sp_type);
      if (!sph)
        sph= &sp_handler_procedure;
      MDL_REQUEST_INIT(mdl_request, sph->get_mdl_type(), db, sp_name,
                        MDL_EXCLUSIVE, MDL_TRANSACTION);
      mdl_requests.push_front(mdl_request);
    } while (! (nxtres= table->file->ha_index_next_same(table->record[0], keybuf, key_len)));
  }
  table->file->ha_index_end();
  if (nxtres != 0 && nxtres != HA_ERR_END_OF_FILE)
  {
    table->file->print_error(nxtres, MYF(0));
    goto error;
  }
  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();

  /* We should already hold a global IX lock and a schema X lock. */
  DBUG_ASSERT(thd->mdl_context.is_lock_owner(MDL_key::BACKUP, "", "",
                                             MDL_BACKUP_DDL) &&
              thd->mdl_context.is_lock_owner(MDL_key::SCHEMA, db, "",
                                             MDL_EXCLUSIVE));
  DBUG_RETURN(thd->mdl_context.acquire_locks(&mdl_requests,
                                             thd->variables.lock_wait_timeout));
error:
  thd->commit_whole_transaction_and_close_tables();
  new_trans.restore_old_transaction();
  DBUG_RETURN(true);
}


/**
  Drop all routines in database 'db'

  @note Close the thread tables, the calling code might want to
  delete from other system tables afterwards.
*/

int
sp_drop_db_routines(THD *thd, const char *db)
{
  TABLE *table;
  int ret;
  uint key_len;
  MDL_savepoint mdl_savepoint= thd->mdl_context.mdl_savepoint();
  uchar keybuf[MAX_KEY_LENGTH];
  size_t db_length= strlen(db);
  Sql_mode_instant_remove smir(thd, MODE_PAD_CHAR_TO_FULL_LENGTH); // see below
  DBUG_ENTER("sp_drop_db_routines");
  DBUG_PRINT("enter", ("db: %s", db));

  ret= SP_OPEN_TABLE_FAILED;
  if (!(table= open_proc_table_for_update(thd)))
    goto err;

  table->field[MYSQL_PROC_FIELD_DB]->store(db, db_length, system_charset_info);
  key_len= table->key_info->key_part[0].store_length;
  table->field[MYSQL_PROC_FIELD_DB]->get_key_image(keybuf, key_len, Field::itRAW);

  ret= SP_OK;
  if (table->file->ha_index_init(0, 1))
  {
    ret= SP_KEY_NOT_FOUND;
    goto err_idx_init;
  }
  if (!table->file->ha_index_read_map(table->record[0], keybuf, (key_part_map)1,
                                      HA_READ_KEY_EXACT))
  {
    int nxtres;
    bool deleted= FALSE;

    do
    {
      if (! table->file->ha_delete_row(table->record[0]))
      {
	deleted= TRUE;		/* We deleted something */
#ifdef HAVE_PSI_SP_INTERFACE
        String buf;
        // the following assumes MODE_PAD_CHAR_TO_FULL_LENGTH being *unset*
        String *name= table->field[MYSQL_PROC_FIELD_NAME]->val_str(&buf);

        enum_sp_type sp_type= (enum_sp_type) table->field[MYSQL_PROC_MYSQL_TYPE]->ptr[0];
        /* Drop statistics for this stored program from performance schema. */
        MYSQL_DROP_SP(sp_type, db, static_cast<uint>(db_length), name->ptr(), name->length());
#endif
      }
      else
      {
	ret= SP_DELETE_ROW_FAILED;
	nxtres= 0;
	break;
      }
    } while (!(nxtres= table->file->ha_index_next_same(table->record[0],
                                                       keybuf, key_len)));
    if (nxtres != HA_ERR_END_OF_FILE)
      ret= SP_KEY_NOT_FOUND;
    if (deleted)
    {
      sp_cache_invalidate();
      /* Make change permanent and avoid 'table is marked as crashed' errors */
      table->file->extra(HA_EXTRA_FLUSH);
    }
  }
  table->file->ha_index_end();

err_idx_init:
  trans_commit_stmt(thd);
  close_thread_tables(thd);
  /*
    Make sure to only release the MDL lock on mysql.proc, not other
    metadata locks DROP DATABASE might have acquired.
  */
  thd->mdl_context.rollback_to_savepoint(mdl_savepoint);

err:
  DBUG_RETURN(ret);
}


/**
  Implement SHOW CREATE statement for stored routines.

  The operation finds the stored routine object specified by name and then
  calls sp_head::show_create_routine() for the object.

  @param thd  Thread context.
  @param name Stored routine name.

  @return Error status.
    @retval FALSE on success
    @retval TRUE on error
*/

bool
Sp_handler::sp_show_create_routine(THD *thd,
                                   const Database_qualified_name *name) const
{
  DBUG_ENTER("sp_show_create_routine");
  DBUG_PRINT("enter", ("type: %s name: %.*s",
                       type_str(),
                       (int) name->m_name.length,
                       name->m_name.str));
  /*
    @todo: Consider using prelocking for this code as well. Currently
    SHOW CREATE PROCEDURE/FUNCTION is a dirty read of the data
    dictionary, i.e. takes no metadata locks.
    It is "safe" to do as long as it doesn't affect the results
    of the binary log or the query cache, which currently it does not.
  */
  sp_head *sp= 0;

  DBUG_EXECUTE_IF("cache_sp_in_show_create",
    /* Some tests need just need a way to cache SP without other side-effects.*/
    sp_cache_routine(thd, name, false, &sp);
    sp->show_create_routine(thd, this);
    DBUG_RETURN(false);
  );

  bool free_sp= db_find_routine(thd, name, &sp) == SP_OK;
  bool ret= !sp || sp->show_create_routine(thd, this);
  if (ret)
  {
    /*
      If we have insufficient privileges, pretend the routine
      does not exist.
    */
    my_error(ER_SP_DOES_NOT_EXIST, MYF(0), type_str(), name->m_name.str);
  }
  if (free_sp)
    sp_head::destroy(sp);
  DBUG_RETURN(ret);
}


/*
  A helper class to split package name from a dot-qualified name
  and return it as a 0-terminated string
    'pkg.name' -> 'pkg\0'
*/

class Prefix_name_buf: public LEX_CSTRING
{
  char m_buf[SAFE_NAME_LEN + 1];
public:
  Prefix_name_buf(const THD *thd, const LEX_CSTRING &name)
  {
    const char *end;
    if (!(end= strrchr(name.str, '.')))
    {
      static_cast<LEX_CSTRING*>(this)[0]= null_clex_str;
    }
    else
    {
      str= m_buf;
      length= end - name.str;
      set_if_smaller(length, sizeof(m_buf) - 1);
      memcpy(m_buf, name.str, length);
      m_buf[length]= '\0';
    }
  }
};


/*
  In case of recursions, we create multiple copies of the same SP.
  This methods checks the current recursion depth.
  In case if the recursion limit exceeded, it throws an error
  and returns NULL.
  Otherwise, depending on the current recursion level, it:
  - either returns the original SP,
  - or makes and returns a new clone of SP
*/

sp_head *
Sp_handler::sp_clone_and_link_routine(THD *thd,
                                      const Database_qualified_name *name,
                                      sp_head *sp) const
{
  DBUG_ENTER("sp_link_routine");
  int rc;
  ulong level;
  sp_head *new_sp;
  LEX_CSTRING returns= empty_clex_str;
  Database_qualified_name lname(name->m_db, name->m_name);
#ifndef DBUG_OFF
  uint parent_subroutine_count=
    !sp->m_parent ? 0 :
     sp->m_parent->m_routine_declarations.elements +
     sp->m_parent->m_routine_implementations.elements;
#endif

  /*
    String buffer for RETURNS data type must have system charset;
    64 -- size of "returns" column of mysql.proc.
  */
  String retstr(64);
  retstr.set_charset(sp->get_creation_ctx()->get_client_cs());

  DBUG_PRINT("info", ("found: %p", sp));
  if (sp->m_first_free_instance)
  {
    DBUG_PRINT("info", ("first free: %p  level: %lu  flags %x",
                        sp->m_first_free_instance,
                        sp->m_first_free_instance->m_recursion_level,
                        sp->m_first_free_instance->m_flags));
    DBUG_ASSERT(!(sp->m_first_free_instance->m_flags & sp_head::IS_INVOKED));
    if (sp->m_first_free_instance->m_recursion_level > recursion_depth(thd))
    {
      recursion_level_error(thd, sp);
      DBUG_RETURN(0);
    }
    DBUG_RETURN(sp->m_first_free_instance);
  }
  /*
    Actually depth could be +1 than the actual value in case a SP calls
    SHOW CREATE PROCEDURE. Hence, the linked list could hold up to one more
    instance.
  */

  level= sp->m_last_cached_sp->m_recursion_level + 1;
  if (level > recursion_depth(thd))
  {
    recursion_level_error(thd, sp);
    DBUG_RETURN(0);
  }

  if (type() == SP_TYPE_FUNCTION)
  {
    sp_returns_type(thd, retstr, sp);
    retstr.get_value(&returns);
  }

  if (sp->m_parent)
  {
    /*
      If we're cloning a recursively called package routine,
      we need to take some special measures:
      1. Cut the package name prefix from the routine name: 'pkg1.p1' -> 'p1',
         to have db_load_routine() generate and parse a query like this:
           CREATE PROCEDURE p1 ...;
         rather than:
           CREATE PROCEDURE pkg1.p1 ...;
         The latter would be misinterpreted by the parser as a standalone
         routine 'p1' in the database 'pkg1', which is not what we need.
      2. We pass m_parent to db_load_routine() to have it set
         thd->lex->sphead to sp->m_parent before calling parse_sql().
      These two measures allow to parse a package subroutine using
      the grammar for standalone routines, e.g.:
        CREATE PROCEDURE p1 ... END;
      instead of going through a more complex query, e.g.:
        CREATE PACKAGE BODY pkg1 AS
          PROCEDURE p1 ... END;
        END;
    */
    size_t prefix_length= sp->m_parent->m_name.length + 1;
    DBUG_ASSERT(prefix_length < lname.m_name.length);
    DBUG_ASSERT(lname.m_name.str[sp->m_parent->m_name.length] == '.');
    lname.m_name.str+= prefix_length;
    lname.m_name.length-= prefix_length;
    sp->m_parent->m_is_cloning_routine= true;
  }


  rc= db_load_routine(thd, &lname, &new_sp,
                      sp->m_sql_mode, sp->m_params, returns,
                      sp->m_body, sp->chistics(),
                      sp->m_definer,
                      sp->m_created, sp->m_modified,
                      sp->m_parent,
                      sp->get_creation_ctx());
  if (sp->m_parent)
    sp->m_parent->m_is_cloning_routine= false;

  if (rc == SP_OK)
  {
#ifndef DBUG_OFF
    /*
      We've just called the parser to clone the routine.
      In case of a package routine, make sure that the parser
      has not added any new subroutines directly to the parent package.
      The cloned subroutine instances get linked below to the first instance,
      they must have no direct links from the parent package.
    */
    DBUG_ASSERT(!sp->m_parent ||
                parent_subroutine_count ==
                sp->m_parent->m_routine_declarations.elements +
                sp->m_parent->m_routine_implementations.elements);
#endif
    sp->m_last_cached_sp->m_next_cached_sp= new_sp;
    new_sp->m_recursion_level= level;
    new_sp->m_first_instance= sp;
    sp->m_last_cached_sp= sp->m_first_free_instance= new_sp;
    DBUG_PRINT("info", ("added level: %p, level: %lu, flags %x",
                        new_sp, new_sp->m_recursion_level,
                        new_sp->m_flags));
    DBUG_RETURN(new_sp);
  }
  DBUG_RETURN(0);
}


/**
  Obtain object representing stored procedure/function by its name from
  stored procedures cache and looking into mysql.proc if needed.

  @param thd          thread context
  @param name         name of procedure
  @param cp           hash to look routine in
  @param cache_only   if true perform cache-only lookup
                      (Don't look in mysql.proc).

  @retval
    NonNULL pointer to sp_head object for the procedure
  @retval
    NULL    in case of error.
*/

sp_head *
Sp_handler::sp_find_routine(THD *thd, const Database_qualified_name *name,
                            bool cache_only) const
{
  DBUG_ENTER("Sp_handler::sp_find_routine");
  DBUG_PRINT("enter", ("name:  %.*s.%.*s  type: %s  cache only %d",
                       (int) name->m_db.length, name->m_db.str,
                       (int) name->m_name.length, name->m_name.str,
                       type_str(), cache_only));
  sp_cache **cp= get_cache(thd);
  sp_head *sp;

  if ((sp= sp_cache_lookup(cp, name)))
    DBUG_RETURN(sp_clone_and_link_routine(thd, name, sp));
  if (!cache_only)
    db_find_and_cache_routine(thd, name, &sp);
  DBUG_RETURN(sp);
}


/**
  Find a package routine.
  See sp_cache_routine() for more information on parameters and return value.

  @param thd         - current THD
  @param pkgname_str - package name
  @param name        - a mixed qualified name, with:
                       * name->m_db set to the database, e.g. "dbname"
                       * name->m_name set to a package-qualified name,
                         e.g. "pkgname.spname".
  @param cache_only  - don't load mysql.proc if not cached
  @retval non-NULL   - a pointer to an sp_head object
  @retval NULL       - an error happened.
*/

sp_head *
Sp_handler::sp_find_package_routine(THD *thd,
                                    const LEX_CSTRING pkgname_str,
                                    const Database_qualified_name *name,
                                    bool cache_only) const
{
  DBUG_ENTER("sp_find_package_routine");
  Database_qualified_name pkgname(&name->m_db, &pkgname_str);
  sp_head *ph= sp_cache_lookup(&thd->sp_package_body_cache, &pkgname);
  if (!ph && !cache_only)
    sp_handler_package_body.db_find_and_cache_routine(thd, &pkgname, &ph);
  if (ph)
  {
    LEX_CSTRING tmp= name->m_name;
    const char *dot= strrchr(tmp.str, '.');
    size_t prefix_length= dot ? dot - tmp.str + 1 : 0;
    sp_package *pkg= ph->get_package();
    tmp.str+= prefix_length;
    tmp.length-= prefix_length;
    LEX *plex= pkg ? pkg->m_routine_implementations.find(tmp, type()) : NULL;
    sp_head *sp= plex ? plex->sphead : NULL;
    if (sp)
      DBUG_RETURN(sp_clone_and_link_routine(thd, name, sp));
  }
  DBUG_RETURN(NULL);
}


/**
  Find a package routine.
  See sp_cache_routine() for more information on parameters and return value.

  @param thd        - current THD
  @param name       - Qualified name with the following format:
                      * name->m_db is set to the database name, e.g. "dbname"
                      * name->m_name is set to a package-qualified name,
                        e.g. "pkgname.spname", as a single string with a
                        dot character as a separator.
  @param cache_only - don't load mysql.proc if not cached
  @retval non-NULL  - a pointer to an sp_head object
  @retval NULL      - an error happened
*/

sp_head *
Sp_handler::sp_find_package_routine(THD *thd,
                                    const Database_qualified_name *name,
                                    bool cache_only) const
{
  DBUG_ENTER("Sp_handler::sp_find_package_routine");
  Prefix_name_buf pkgname(thd, name->m_name);
  DBUG_ASSERT(pkgname.length);
  DBUG_RETURN(sp_find_package_routine(thd, pkgname, name, cache_only));
}


/**
  This is used by sql_acl.cc:mysql_routine_grant() and is used to find
  the routines in 'routines'.

  @param thd Thread handler
  @param routines List of needles in the hay stack

  @return
    @retval FALSE Found.
    @retval TRUE  Not found
*/

bool
Sp_handler::sp_exist_routines(THD *thd, TABLE_LIST *routines) const
{
  TABLE_LIST *routine;
  bool sp_object_found;
  DBUG_ENTER("sp_exists_routine");
  for (routine= routines; routine; routine= routine->next_global)
  {
    sp_name *name;
    LEX_CSTRING lex_db;
    LEX_CSTRING lex_name;
    thd->make_lex_string(&lex_db, routine->db.str, routine->db.length);
    thd->make_lex_string(&lex_name, routine->table_name.str,
                         routine->table_name.length);
    name= new sp_name(&lex_db, &lex_name, true);
    sp_object_found= sp_find_routine(thd, name, false) != NULL;
    thd->get_stmt_da()->clear_warning_info(thd->query_id);
    if (! sp_object_found)
    {
      my_error(ER_SP_DOES_NOT_EXIST, MYF(0), "FUNCTION or PROCEDURE",
               routine->table_name.str);
      DBUG_RETURN(TRUE);
    }
  }
  DBUG_RETURN(FALSE);
}


extern "C" uchar* sp_sroutine_key(const uchar *ptr, size_t *plen,
                                  my_bool first)
{
  Sroutine_hash_entry *rn= (Sroutine_hash_entry *)ptr;
  *plen= rn->mdl_request.key.length();
  return (uchar *)rn->mdl_request.key.ptr();
}


/**
  Auxilary function that adds new element to the set of stored routines
  used by statement.

  In case when statement uses stored routines but does not need
  prelocking (i.e. it does not use any tables) we will access the
  elements of Query_tables_list::sroutines set on prepared statement
  re-execution. Because of this we have to allocate memory for both
  hash element and copy of its key in persistent arena.

  @param prelocking_ctx  Prelocking context of the statement
  @param arena           Arena in which memory for new element will be
                         allocated
  @param key             Key for the hash representing set
  @param belong_to_view  Uppermost view which uses this routine
                         (0 if routine is not used by view)

  @note
    Will also add element to end of 'Query_tables_list::sroutines_list' list.

  @todo
    When we will got rid of these accesses on re-executions we will be
    able to allocate memory for hash elements in non-persitent arena
    and directly use key values from sp_head::m_sroutines sets instead
    of making their copies.

  @retval
    TRUE   new element was added.
  @retval
    FALSE  element was not added (because it is already present in
    the set).
*/

bool sp_add_used_routine(Query_tables_list *prelocking_ctx, Query_arena *arena,
                         const MDL_key *key,
                         const Sp_handler *handler,
                         TABLE_LIST *belong_to_view)
{
  my_hash_init_opt(PSI_INSTRUMENT_ME, &prelocking_ctx->sroutines, system_charset_info,
                   Query_tables_list::START_SROUTINES_HASH_SIZE,
                   0, 0, sp_sroutine_key, 0, 0);

  if (!my_hash_search(&prelocking_ctx->sroutines, key->ptr(), key->length()))
  {
    Sroutine_hash_entry *rn=
      (Sroutine_hash_entry *)arena->alloc(sizeof(Sroutine_hash_entry));
    if (unlikely(!rn)) // OOM. Error will be reported using fatal_error().
      return FALSE;
    MDL_REQUEST_INIT_BY_KEY(&rn->mdl_request, key, MDL_SHARED, MDL_TRANSACTION);
    if (my_hash_insert(&prelocking_ctx->sroutines, (uchar *)rn))
      return FALSE;
    prelocking_ctx->sroutines_list.link_in_list(rn, &rn->next);
    rn->belong_to_view= belong_to_view;
    rn->m_handler= handler;
    rn->m_sp_cache_version= 0;
    return TRUE;
  }
  return FALSE;
}


/*
  Find and cache a routine in a parser-safe reentrant mode.

  If sp_head is not in the cache,
  its loaded from mysql.proc, parsed using parse_sql(), and cached.
  Note, as it is called from inside parse_sql() itself,
  we need to preserve and restore the parser state.

  It's used during parsing of CREATE PACKAGE BODY,
  to load the corresponding CREATE PACKAGE.
*/

int
Sp_handler::sp_cache_routine_reentrant(THD *thd,
                                       const Database_qualified_name *name,
                                       sp_head **sp) const
{
  int ret;
  Parser_state *oldps= thd->m_parser_state;
  thd->m_parser_state= NULL;
  ret= sp_cache_routine(thd, name, false, sp);
  thd->m_parser_state= oldps;
  return ret;
}


/**
  Check if a routine has a declaration in the CREATE PACKAGE statement,
  by looking up in thd->sp_package_spec_cache, and by loading from mysql.proc
  if needed.

    @param thd      current thd
    @param db       the database name
    @param package  the package name
    @param name     the routine name
    @param type     the routine type
    @retval         true, if the routine has a declaration
    @retval         false, if the routine does not have a declaration

  This function can be called in arbitrary context:
  - inside a package routine
  - inside a standalone routine
  - inside a anonymous block
  - outside of any routines

  The state of the package specification (i.e. the CREATE PACKAGE statement)
  for "package" before the call of this function is not known:
   it can be cached, or not cached.
  After the call of this function, the package specification is always cached,
  unless a fatal error happens.
*/

static bool
is_package_public_routine(THD *thd,
                          const LEX_CSTRING &db,
                          const LEX_CSTRING &package,
                          const LEX_CSTRING &routine,
                          enum_sp_type type)
{
  sp_head *sp= NULL;
  Database_qualified_name tmp(db, package);
  bool ret= sp_handler_package_spec.
              sp_cache_routine_reentrant(thd, &tmp, &sp);
  sp_package *spec= (!ret && sp) ? sp->get_package() : NULL;
  return spec && spec->m_routine_declarations.find(routine, type);
}


/**
  Check if a routine has a declaration in the CREATE PACKAGE statement
  by looking up in sp_package_spec_cache.

    @param thd      current thd
    @param db       the database name
    @param pkgname  the package name
    @param name     the routine name
    @param type     the routine type
    @retval         true, if the routine has a declaration
    @retval         false, if the routine does not have a declaration

  This function is called in the middle of CREATE PACKAGE BODY parsing,
  to lookup the current package routines.
  The package specification (i.e. the CREATE PACKAGE statement) for
  the current package body must already be loaded and cached at this point.
*/

static bool
is_package_public_routine_quick(THD *thd,
                                const LEX_CSTRING &db,
                                const LEX_CSTRING &pkgname,
                                const LEX_CSTRING &name,
                                enum_sp_type type)
{
  Database_qualified_name tmp(db, pkgname);
  sp_head *sp= sp_cache_lookup(&thd->sp_package_spec_cache, &tmp);
  sp_package *pkg= sp ? sp->get_package() : NULL;
  DBUG_ASSERT(pkg); // Must already be cached
  return pkg && pkg->m_routine_declarations.find(name, type);
}


/**
  Check if a qualified name, e.g. "CALL name1.name2",
  refers to a known routine in the package body "pkg".
*/

static bool
is_package_body_routine(THD *thd, sp_package *pkg,
                        const LEX_CSTRING &name1,
                        const LEX_CSTRING &name2,
                        enum_sp_type type)
{
  return Sp_handler::eq_routine_name(pkg->m_name, name1) &&
         (pkg->m_routine_declarations.find(name2, type) ||
          pkg->m_routine_implementations.find(name2, type));
}


/**
  Resolve a qualified routine reference xxx.yyy(), between:
  - A standalone routine: xxx.yyy
  - A package routine:    current_database.xxx.yyy
*/

bool Sp_handler::
  sp_resolve_package_routine_explicit(THD *thd,
                                      sp_head *caller,
                                      sp_name *name,
                                      const Sp_handler **pkg_routine_handler,
                                      Database_qualified_name *pkgname) const
{
  sp_package *pkg;

  /*
    If a qualified routine name was used, e.g. xxx.yyy(),
    we possibly have a call to a package routine.
    Rewrite name if name->m_db (xxx) is a known package,
    and name->m_name (yyy) is a known routine in this package.
  */
  LEX_CSTRING tmpdb= thd->db;
  if (is_package_public_routine(thd, tmpdb, name->m_db, name->m_name, type()) ||
      // Check if a package routine calls a private routine
      (caller && caller->m_parent &&
       is_package_body_routine(thd, caller->m_parent,
                               name->m_db, name->m_name, type())) ||
      // Check if a package initialization sections calls a private routine
      (caller && (pkg= caller->get_package()) &&
       is_package_body_routine(thd, pkg, name->m_db, name->m_name, type())))
  {
    pkgname->m_db= tmpdb;
    pkgname->m_name= name->m_db;
    *pkg_routine_handler= package_routine_handler();
    return name->make_package_routine_name(thd->mem_root, tmpdb,
                                           name->m_db, name->m_name);
  }
  return false;
}


/**
  Resolve a non-qualified routine reference yyy(), between:
  - A standalone routine: current_database.yyy
  - A package routine:    current_database.current_package.yyy
*/

bool Sp_handler::
  sp_resolve_package_routine_implicit(THD *thd,
                                      sp_head *caller,
                                      sp_name *name,
                                      const Sp_handler **pkg_routine_handler,
                                      Database_qualified_name *pkgname) const
{
  sp_package *pkg;

  if (!caller || !caller->m_name.length)
  {
    /*
      We are either in a an anonymous block,
      or not in a routine at all.
    */
    return false; // A standalone routine is called
  }

  if (caller->m_parent)
  {
    // A package routine calls a non-qualified routine
    int ret= SP_OK;
    Prefix_name_buf pkgstr(thd, caller->m_name);
    DBUG_ASSERT(pkgstr.length);
    LEX_CSTRING tmpname; // Non-qualified m_name
    tmpname.str= caller->m_name.str + pkgstr.length + 1;
    tmpname.length= caller->m_name.length - pkgstr.length - 1;

    /*
      We're here if a package routine calls another non-qualified
      function or procedure, e.g. yyy().
      We need to distinguish two cases:
      - yyy() is another routine from the same package
      - yyy() is a standalone routine from the same database
      To detect if yyy() is a package (rather than a standalone) routine,
      we check if:
      - yyy() recursively calls itself
      - yyy() is earlier implemented in the current CREATE PACKAGE BODY
      - yyy() has a forward declaration
      - yyy() is declared in the corresponding CREATE PACKAGE
    */
    if (eq_routine_name(tmpname, name->m_name) ||
        caller->m_parent->m_routine_implementations.find(name->m_name, type()) ||
        caller->m_parent->m_routine_declarations.find(name->m_name, type()) ||
        is_package_public_routine_quick(thd, caller->m_db,
                                        pkgstr, name->m_name, type()))
    {
      DBUG_ASSERT(ret == SP_OK);
      pkgname->copy(thd->mem_root, caller->m_db, pkgstr);
      *pkg_routine_handler= package_routine_handler();
      if (name->make_package_routine_name(thd->mem_root, pkgstr, name->m_name))
        return true;
    }
    return ret != SP_OK;
  }

  if ((pkg= caller->get_package()) &&
       pkg->m_routine_implementations.find(name->m_name, type()))
  {
    pkgname->m_db= caller->m_db;
    pkgname->m_name= caller->m_name;
    // Package initialization section is calling a non-qualified routine
    *pkg_routine_handler= package_routine_handler();
    return name->make_package_routine_name(thd->mem_root,
                                           caller->m_name, name->m_name);
  }

  return false; // A standalone routine is called

}


/**
  Detect cases when a package routine (rather than a standalone routine)
  is called, and rewrite sp_name accordingly.

  @param thd              Current thd
  @param caller           The caller routine (or NULL if outside of a routine)
  @param [IN/OUT] name    The called routine name
  @param [OUT]    pkgname If the routine is found to be a package routine,
                          pkgname is populated with the package name.
                          Otherwise, it's not touched.
  @retval         false   on success
  @retval         true    on error (e.g. EOM, could not read CREATE PACKAGE)
*/

bool
Sp_handler::sp_resolve_package_routine(THD *thd,
                                       sp_head *caller,
                                       sp_name *name,
                                       const Sp_handler **pkg_routine_handler,
                                       Database_qualified_name *pkgname) const
{
  if (!thd->db.length || !(thd->variables.sql_mode & MODE_ORACLE))
    return false;

  return name->m_explicit_name ?
         sp_resolve_package_routine_explicit(thd, caller, name,
                                             pkg_routine_handler, pkgname) :
         sp_resolve_package_routine_implicit(thd, caller, name,
                                             pkg_routine_handler, pkgname);
}


/**
  Add routine which is explicitly used by statement to the set of stored
  routines used by this statement.

  To be friendly towards prepared statements one should pass
  persistent arena as second argument.

  @param prelocking_ctx  Prelocking context of the statement
  @param arena           Arena in which memory for new element of the set
                         will be allocated
  @param rt              Routine name

  @note
    Will also add element to end of 'Query_tables_list::sroutines_list' list
    (and will take into account that this is an explicitly used routine).
*/

void Sp_handler::add_used_routine(Query_tables_list *prelocking_ctx,
                                  Query_arena *arena,
                                  const Database_qualified_name *rt) const
{
  MDL_key key(get_mdl_type(), rt->m_db.str, rt->m_name.str);
  (void) sp_add_used_routine(prelocking_ctx, arena, &key, this, 0);
  prelocking_ctx->sroutines_list_own_last= prelocking_ctx->sroutines_list.next;
  prelocking_ctx->sroutines_list_own_elements=
                    prelocking_ctx->sroutines_list.elements;
}


/**
  Remove routines which are only indirectly used by statement from
  the set of routines used by this statement.

  @param prelocking_ctx  Prelocking context of the statement
*/

void sp_remove_not_own_routines(Query_tables_list *prelocking_ctx)
{
  Sroutine_hash_entry *not_own_rt, *next_rt;
  for (not_own_rt= *prelocking_ctx->sroutines_list_own_last;
       not_own_rt; not_own_rt= next_rt)
  {
    /*
      It is safe to obtain not_own_rt->next after calling hash_delete() now
      but we want to be more future-proof.
    */
    next_rt= not_own_rt->next;
    my_hash_delete(&prelocking_ctx->sroutines, (uchar *)not_own_rt);
  }

  *prelocking_ctx->sroutines_list_own_last= NULL;
  prelocking_ctx->sroutines_list.next= prelocking_ctx->sroutines_list_own_last;
  prelocking_ctx->sroutines_list.elements= 
                    prelocking_ctx->sroutines_list_own_elements;
}


/**
  Merge contents of two hashes representing sets of routines used
  by statements or by other routines.

  @param dst   hash to which elements should be added
  @param src   hash from which elements merged

  @note
    This procedure won't create new Sroutine_hash_entry objects,
    instead it will simply add elements from source to destination
    hash. Thus time of life of elements in destination hash becomes
    dependant on time of life of elements from source hash. It also
    won't touch lists linking elements in source and destination
    hashes.

  @returns
    @return TRUE Failure
    @return FALSE Success
*/

bool sp_update_sp_used_routines(HASH *dst, HASH *src)
{
  for (uint i=0 ; i < src->records ; i++)
  {
    Sroutine_hash_entry *rt= (Sroutine_hash_entry *)my_hash_element(src, i);
    if (!my_hash_search(dst, (uchar *)rt->mdl_request.key.ptr(),
                        rt->mdl_request.key.length()))
    {
      if (my_hash_insert(dst, (uchar *)rt))
        return TRUE;
    }
  }
  return FALSE;
}


/**
  Add contents of hash representing set of routines to the set of
  routines used by statement.

  @param thd             Thread context
  @param prelocking_ctx  Prelocking context of the statement
  @param src             Hash representing set from which routines will
                         be added
  @param belong_to_view  Uppermost view which uses these routines, 0 if none

  @note It will also add elements to end of
        'Query_tables_list::sroutines_list' list.
*/

void
sp_update_stmt_used_routines(THD *thd, Query_tables_list *prelocking_ctx,
                             HASH *src, TABLE_LIST *belong_to_view)
{
  for (uint i=0 ; i < src->records ; i++)
  {
    Sroutine_hash_entry *rt= (Sroutine_hash_entry *)my_hash_element(src, i);
    (void)sp_add_used_routine(prelocking_ctx, thd->stmt_arena,
                              &rt->mdl_request.key, rt->m_handler,
                              belong_to_view);
  }
}


/**
  Add contents of list representing set of routines to the set of
  routines used by statement.

  @param thd             Thread context
  @param prelocking_ctx  Prelocking context of the statement
  @param src             List representing set from which routines will
                         be added
  @param belong_to_view  Uppermost view which uses these routines, 0 if none

  @note It will also add elements to end of
        'Query_tables_list::sroutines_list' list.
*/

void sp_update_stmt_used_routines(THD *thd, Query_tables_list *prelocking_ctx,
                                  SQL_I_List<Sroutine_hash_entry> *src,
                                  TABLE_LIST *belong_to_view)
{
  for (Sroutine_hash_entry *rt= src->first; rt; rt= rt->next)
    (void)sp_add_used_routine(prelocking_ctx, thd->stmt_arena,
                              &rt->mdl_request.key, rt->m_handler,
                              belong_to_view);
}


/**
  A helper wrapper around sp_cache_routine() to use from
  prelocking until 'sp_name' is eradicated as a class.
*/

int Sroutine_hash_entry::sp_cache_routine(THD *thd,
                                          bool lookup_only,
                                          sp_head **sp) const
{
  char qname_buff[NAME_LEN*2+1+1];
  sp_name name(&mdl_request.key, qname_buff);
  /*
    Check that we have an MDL lock on this routine, unless it's a top-level
    CALL. The assert below should be unambiguous: the first element
    in sroutines_list has an MDL lock unless it's a top-level call, or a
    trigger, but triggers can't occur here (see the preceding assert).
  */
  DBUG_ASSERT(mdl_request.ticket || this == thd->lex->sroutines_list.first);

  return m_handler->sp_cache_routine(thd, &name, lookup_only, sp);
}


/**
  Ensure that routine is present in cache by loading it from the mysql.proc
  table if needed. If the routine is present but old, reload it.
  Emit an appropriate error if there was a problem during
  loading.

  @param[in]  thd   Thread context.
  @param[in]  name  Name of routine.
  @param[in]  lookup_only Only check that the routine is in the cache.
                    If it's not, don't try to load. If it is present,
                    but old, don't try to reload.
  @param[out] sp    Pointer to sp_head object for routine, NULL if routine was
                    not found.

  @retval 0      Either routine is found and was successfully loaded into cache
                 or it does not exist.
  @retval non-0  Error while loading routine from mysql,proc table.
*/

int Sp_handler::sp_cache_routine(THD *thd,
                                 const Database_qualified_name *name,
                                 bool lookup_only,
                                 sp_head **sp) const
{
  int ret= 0;
  sp_cache **spc= get_cache(thd);

  DBUG_ENTER("Sp_handler::sp_cache_routine");

  DBUG_ASSERT(spc);

  *sp= sp_cache_lookup(spc, name);

  if (lookup_only)
    DBUG_RETURN(SP_OK);

  if (*sp)
  {
    sp_cache_flush_obsolete(spc, sp);
    if (*sp)
      DBUG_RETURN(SP_OK);
  }

  switch ((ret= db_find_and_cache_routine(thd, name, sp)))
  {
    case SP_OK:
      break;
    case SP_KEY_NOT_FOUND:
      ret= SP_OK;
      break;
    default:
      /* Query might have been killed, don't set error. */
      if (thd->killed)
        break;
      /*
        Any error when loading an existing routine is either some problem
        with the mysql.proc table, or a parse error because the contents
        has been tampered with (in which case we clear that error).
      */
      if (ret == SP_PARSE_ERROR)
        thd->clear_error();
      /*
        If we cleared the parse error, or when db_find_routine() flagged
        an error with it's return value without calling my_error(), we
        set the generic "mysql.proc table corrupt" error here.
      */
      if (!thd->is_error())
      {
        my_error(ER_SP_PROC_TABLE_CORRUPT, MYF(0),
                 ErrConvDQName(name).ptr(), ret);
      }
      break;
  }
  DBUG_RETURN(ret);
}


/**
  Cache a package routine using its package name and a qualified name.
  See sp_cache_routine() for more information on parameters and return values.

  @param thd         - current THD
  @param pkgname_str - package name, e.g. "pkgname"
  @param name        - name with the following format:
                       * name->m_db is a database name, e.g. "dbname"
                       * name->m_name is a package-qualified name,
                         e.g. "pkgname.spname"
  @param lookup_only - don't load mysql.proc if not cached
  @param [OUT] sp    - the result is returned here.
  @retval false      - loaded or does not exists
  @retval true       - error while loading mysql.proc
*/

int
Sp_handler::sp_cache_package_routine(THD *thd,
                                     const LEX_CSTRING &pkgname_cstr,
                                     const Database_qualified_name *name,
                                     bool lookup_only, sp_head **sp) const
{
  DBUG_ENTER("sp_cache_package_routine");
  DBUG_ASSERT(type() == SP_TYPE_FUNCTION || type() == SP_TYPE_PROCEDURE);
  sp_name pkgname(&name->m_db, &pkgname_cstr, false);
  sp_head *ph= NULL;
  int ret= sp_handler_package_body.sp_cache_routine(thd, &pkgname,
                                                    lookup_only,
                                                    &ph);
  if (!ret)
  {
    sp_package *pkg= ph ? ph->get_package() : NULL;
    LEX_CSTRING tmp= name->m_name;
    const char *dot= strrchr(tmp.str, '.');
    size_t prefix_length= dot ? dot - tmp.str + 1 : 0;
    tmp.str+= prefix_length;
    tmp.length-= prefix_length;
    LEX *rlex= pkg ? pkg->m_routine_implementations.find(tmp, type()) : NULL;
    *sp= rlex ? rlex->sphead : NULL;
  }

  DBUG_RETURN(ret);
}


/**
  Cache a package routine by its fully qualified name.
  See sp_cache_routine() for more information on parameters and return values.

  @param thd       - current THD
  @param name      - name with the following format:
                     * name->m_db is a database name, e.g. "dbname"
                     * name->m_name is a package-qualified name,
                       e.g. "pkgname.spname"
  @param lookup_only - don't load mysql.proc if not cached
  @param [OUT] sp    -  the result is returned here
  @retval false      - loaded or does not exists
  @retval true       - error while loading mysql.proc
*/

int Sp_handler::sp_cache_package_routine(THD *thd,
                                         const Database_qualified_name *name,
                                         bool lookup_only, sp_head **sp) const
{
  DBUG_ENTER("Sp_handler::sp_cache_package_routine");
  Prefix_name_buf pkgname(thd, name->m_name);
  DBUG_ASSERT(pkgname.length);
  DBUG_RETURN(sp_cache_package_routine(thd, pkgname, name, lookup_only, sp));
}


/**
  Generates the CREATE... string from the table information.

  @return
    Returns false on success, true on (alloc) failure.
*/

bool
Sp_handler::show_create_sp(THD *thd, String *buf,
                           const LEX_CSTRING &db,
                           const LEX_CSTRING &name,
                           const LEX_CSTRING &params,
                           const LEX_CSTRING &returns,
                           const LEX_CSTRING &body,
                           const st_sp_chistics &chistics,
                           const AUTHID &definer,
                           const DDL_options_st ddl_options,
                           sql_mode_t sql_mode) const
{
  size_t agglen= (chistics.agg_type == GROUP_AGGREGATE)? 10 : 0;
  LEX_CSTRING tmp;

  /* Make some room to begin with */
  if (buf->alloc(100 + db.length + 1 + name.length +
                 params.length + returns.length +
                 chistics.comment.length + 10 /* length of " DEFINER= "*/ +
                 agglen + USER_HOST_BUFF_SIZE))
    return true;

  Sql_mode_instant_set sms(thd, sql_mode);
  buf->append(STRING_WITH_LEN("CREATE "));
  if (ddl_options.or_replace())
    buf->append(STRING_WITH_LEN("OR REPLACE "));
  append_definer(thd, buf, &definer.user, &definer.host);
  if (chistics.agg_type == GROUP_AGGREGATE)
    buf->append(STRING_WITH_LEN("AGGREGATE "));
  tmp= type_lex_cstring();
  buf->append(&tmp);
  buf->append(STRING_WITH_LEN(" "));
  if (ddl_options.if_not_exists())
    buf->append(STRING_WITH_LEN("IF NOT EXISTS "));

  if (db.length > 0)
  {
    append_identifier(thd, buf, &db);
    buf->append('.');
  }
  append_identifier(thd, buf, &name);
  buf->append('(');
  buf->append(&params);
  buf->append(')');
  if (type() == SP_TYPE_FUNCTION)
  {
    if (sql_mode & MODE_ORACLE)
      buf->append(STRING_WITH_LEN(" RETURN "));
    else
      buf->append(STRING_WITH_LEN(" RETURNS "));
    buf->append(returns.str, returns.length);   // Not \0 terminated
  }
  buf->append('\n');
  switch (chistics.daccess) {
  case SP_NO_SQL:
    buf->append(STRING_WITH_LEN("    NO SQL\n"));
    break;
  case SP_READS_SQL_DATA:
    buf->append(STRING_WITH_LEN("    READS SQL DATA\n"));
    break;
  case SP_MODIFIES_SQL_DATA:
    buf->append(STRING_WITH_LEN("    MODIFIES SQL DATA\n"));
    break;
  case SP_DEFAULT_ACCESS:
  case SP_CONTAINS_SQL:
    /* Do nothing */
    break;
  }
  if (chistics.detistic)
    buf->append(STRING_WITH_LEN("    DETERMINISTIC\n"));
  append_suid(buf, chistics.suid);
  append_comment(buf, chistics.comment);
  buf->append(body.str, body.length);           // Not \0 terminated
  return false;
}


/**
  @brief    The function loads sp_head struct for information schema purposes
            (used for I_S ROUTINES & PARAMETERS tables).

  @param[in]      thd               thread handler
  @param[in]      proc_table        mysql.proc table structurte
  @param[in]      db                database name
  @param[in]      name              sp name
  @param[in]      sql_mode          SQL mode
  @param[in]      type              Routine type
  @param[in]      returns           'returns' string
  @param[in]      params            parameters definition string
  @param[out]     free_sp_head      returns 1 if we need to free sp_head struct
                                    otherwise returns 0
                                    
  @return     Pointer on sp_head struct
    @retval   #                     Pointer on sp_head struct
    @retval   0                     error
*/

sp_head *
Sp_handler::sp_load_for_information_schema(THD *thd, TABLE *proc_table,
                                           const LEX_CSTRING &db,
                                           const LEX_CSTRING &name,
                                           const LEX_CSTRING &params,
                                           const LEX_CSTRING &returns,
                                           sql_mode_t sql_mode,
                                           bool *free_sp_head) const
{
  String defstr;
  const AUTHID definer= {{STRING_WITH_LEN("")}, {STRING_WITH_LEN("")}};
  sp_head *sp;
  sp_cache **spc= get_cache(thd);
  sp_name sp_name_obj(&db, &name, true); // This can change "name"
  *free_sp_head= 0;
  if ((sp= sp_cache_lookup(spc, &sp_name_obj)))
  {
    return sp;
  }

  LEX *old_lex= thd->lex, newlex;
  Stored_program_creation_ctx *creation_ctx= 
    Stored_routine_creation_ctx::load_from_db(thd, &sp_name_obj, proc_table);
  defstr.set_charset(creation_ctx->get_client_cs());
  if (show_create_sp(thd, &defstr,
                     sp_name_obj.m_db, sp_name_obj.m_name,
                     params, returns, empty_body_lex_cstring(sql_mode),
                     Sp_chistics(), definer, DDL_options(), sql_mode))
    return 0;

  thd->lex= &newlex;
  newlex.current_select= NULL; 
  sp= sp_compile(thd, &defstr, sql_mode, NULL, creation_ctx);
  *free_sp_head= 1;
  thd->lex->sphead= NULL;
  lex_end(thd->lex);
  thd->lex= old_lex;
  return sp;
}


LEX_CSTRING Sp_handler_procedure::empty_body_lex_cstring(sql_mode_t mode) const
{
  static LEX_CSTRING m_empty_body_std= {STRING_WITH_LEN("BEGIN END")};
  static LEX_CSTRING m_empty_body_ora= {STRING_WITH_LEN("AS BEGIN NULL; END")};
  return mode & MODE_ORACLE ? m_empty_body_ora : m_empty_body_std;
}


LEX_CSTRING Sp_handler_function::empty_body_lex_cstring(sql_mode_t mode) const
{
  static LEX_CSTRING m_empty_body_std= {STRING_WITH_LEN("RETURN NULL")};
  static LEX_CSTRING m_empty_body_ora= {STRING_WITH_LEN("AS BEGIN RETURN NULL; END")};
  return mode & MODE_ORACLE ? m_empty_body_ora : m_empty_body_std;
}
