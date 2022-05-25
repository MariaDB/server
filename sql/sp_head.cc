/*
   Copyright (c) 2002, 2016, Oracle and/or its affiliates.
   Copyright (c) 2011, 2020, MariaDB

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

#include "mariadb.h"                          /* NO_EMBEDDED_ACCESS_CHECKS */
#include "sql_priv.h"
#include "unireg.h"
#include "sql_prepare.h"
#include "sql_cache.h"                          // query_cache_*
#include "probes_mysql.h"
#include "sql_show.h"                           // append_identifier
#include "sql_db.h"            // mysql_opt_change_db, mysql_change_db
#include "sql_array.h"         // Dynamic_array
#include "log_event.h"         // Query_log_event
#include "sql_derived.h"       // mysql_handle_derived
#include "sql_cte.h"
#include "sql_select.h"        // Virtual_tmp_table
#include "opt_trace.h"
#include "my_json_writer.h"

#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation
#endif
#include "sp_head.h"
#include "sp.h"
#include "sp_pcontext.h"
#include "sp_rcontext.h"
#include "sp_cache.h"
#include "set_var.h"
#include "sql_parse.h"                          // cleanup_items
#include "sql_base.h"                           // close_thread_tables
#include "transaction.h"       // trans_commit_stmt
#include "sql_audit.h"
#include "debug_sync.h"
#ifdef WITH_WSREP
#include "wsrep_trans_observer.h"
#endif /* WITH_WSREP */

/*
  Sufficient max length of printed destinations and frame offsets (all uints).
*/
#define SP_INSTR_UINT_MAXLEN  8
#define SP_STMT_PRINT_MAXLEN 40

#include <my_user.h>
#include "mysql/psi/mysql_statement.h"
#include "mysql/psi/mysql_sp.h"

#ifdef HAVE_PSI_INTERFACE
void init_sp_psi_keys()
{
  const char *category= "sp";
  const int num __attribute__((unused)) = __LINE__ + 3;

  PSI_server->register_statement(category, & sp_instr_stmt::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_set::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_set_trigger_field::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_jump::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_jump_if_not::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_freturn::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_preturn::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_hpush_jump::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_hpop::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_hreturn::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_cpush::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_cpop::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_copen::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_cclose::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_cfetch::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_agg_cfetch::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_cursor_copy_struct::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_error::psi_info, 1);
  PSI_server->register_statement(category, & sp_instr_set_case_expr::psi_info, 1);

  DBUG_ASSERT(SP_PSI_STATEMENT_INFO_COUNT == __LINE__ - num);
}
#endif

#ifdef HAVE_PSI_SP_INTERFACE
#define MYSQL_RUN_SP(SP,CODE)                                           \
  do {                                                                  \
       PSI_sp_locker_state psi_state;                                   \
       PSI_sp_locker *locker= MYSQL_START_SP(&psi_state, (SP)->m_sp_share); \
       CODE;                                                            \
       MYSQL_END_SP(locker);                                            \
  } while(0)
#else
#define MYSQL_RUN_SP(SP, CODE) do { CODE; } while(0)
#endif

extern "C" uchar *sp_table_key(const uchar *ptr, size_t *plen, my_bool first);

/**
  Helper function which operates on a THD object to set the query start_time to
  the current time.

  @param[in, out] thd The session object

*/

static void reset_start_time_for_sp(THD *thd)
{
  if (!thd->in_sub_stmt)
    thd->set_start_time();
}


bool Item_splocal::append_for_log(THD *thd, String *str)
{
  if (fix_fields_if_needed(thd, NULL))
    return true;

  if (limit_clause_param)
    return str->append_ulonglong(val_uint());

  /*
    ROW variables are currently not allowed in select_list, e.g.:
      SELECT row_variable;
    ROW variables can appear in query parts where name is not important, e.g.:
      SELECT ROW(1,2)=row_variable FROM t1;
    So we can skip using NAME_CONST() and use ROW() constants directly.
  */
  if (type_handler() == &type_handler_row)
    return append_value_for_log(thd, str);

  if (str->append(STRING_WITH_LEN(" NAME_CONST('")) ||
      str->append(&m_name) ||
      str->append(STRING_WITH_LEN("',")))
    return true;
  return append_value_for_log(thd, str) || str->append(')');
}


bool Item_splocal::append_value_for_log(THD *thd, String *str)
{
  StringBuffer<STRING_BUFFER_USUAL_SIZE> str_value_holder(&my_charset_latin1);
  Item *item= this_item();
  String *str_value= item->type_handler()->print_item_value(thd, item,
                                                            &str_value_holder);
  return (str_value ?
          str->append(*str_value) :
          str->append(NULL_clex_str));
}


bool Item_splocal_row_field::append_for_log(THD *thd, String *str)
{
  if (fix_fields_if_needed(thd, NULL))
    return true;

  if (limit_clause_param)
    return str->append_ulonglong(val_uint());

  if (str->append(STRING_WITH_LEN(" NAME_CONST('")) ||
      str->append(&m_name) ||
      str->append('.') ||
      str->append(&m_field_name) ||
      str->append(STRING_WITH_LEN("',")))
    return true;
  return append_value_for_log(thd, str) || str->append(')');
}


/**
   Returns a combination of:
   - sp_head::MULTI_RESULTS: added if the 'cmd' is a command that might
     result in multiple result sets being sent back.
   - sp_head::CONTAINS_DYNAMIC_SQL: added if 'cmd' is one of PREPARE,
     EXECUTE, DEALLOCATE.
*/

uint
sp_get_flags_for_command(LEX *lex)
{
  uint flags;

  switch (lex->sql_command) {
  case SQLCOM_SELECT:
    if (lex->result && !lex->analyze_stmt)
    {
      flags= 0;                      /* This is a SELECT with INTO clause */
      break;
    }
    /* fallthrough */
  case SQLCOM_ANALYZE:
  case SQLCOM_OPTIMIZE:
  case SQLCOM_PRELOAD_KEYS:
  case SQLCOM_ASSIGN_TO_KEYCACHE:
  case SQLCOM_CHECKSUM:
  case SQLCOM_CHECK:
  case SQLCOM_HA_READ:
  case SQLCOM_SHOW_AUTHORS:
  case SQLCOM_SHOW_BINLOGS:
  case SQLCOM_SHOW_BINLOG_EVENTS:
  case SQLCOM_SHOW_RELAYLOG_EVENTS:
  case SQLCOM_SHOW_CHARSETS:
  case SQLCOM_SHOW_COLLATIONS:
  case SQLCOM_SHOW_CONTRIBUTORS:
  case SQLCOM_SHOW_CREATE:
  case SQLCOM_SHOW_CREATE_DB:
  case SQLCOM_SHOW_CREATE_FUNC:
  case SQLCOM_SHOW_CREATE_PROC:
  case SQLCOM_SHOW_CREATE_PACKAGE:
  case SQLCOM_SHOW_CREATE_PACKAGE_BODY:
  case SQLCOM_SHOW_CREATE_EVENT:
  case SQLCOM_SHOW_CREATE_TRIGGER:
  case SQLCOM_SHOW_CREATE_USER:
  case SQLCOM_SHOW_DATABASES:
  case SQLCOM_SHOW_ERRORS:
  case SQLCOM_SHOW_EXPLAIN:
  case SQLCOM_SHOW_ANALYZE:
  case SQLCOM_SHOW_FIELDS:
  case SQLCOM_SHOW_FUNC_CODE:
  case SQLCOM_SHOW_GENERIC:
  case SQLCOM_SHOW_GRANTS:
  case SQLCOM_SHOW_ENGINE_STATUS:
  case SQLCOM_SHOW_ENGINE_LOGS:
  case SQLCOM_SHOW_ENGINE_MUTEX:
  case SQLCOM_SHOW_EVENTS:
  case SQLCOM_SHOW_KEYS:
  case SQLCOM_SHOW_BINLOG_STAT:
  case SQLCOM_SHOW_OPEN_TABLES:
  case SQLCOM_SHOW_PRIVILEGES:
  case SQLCOM_SHOW_PROCESSLIST:
  case SQLCOM_SHOW_PROC_CODE:
  case SQLCOM_SHOW_PACKAGE_BODY_CODE:
  case SQLCOM_SHOW_SLAVE_HOSTS:
  case SQLCOM_SHOW_SLAVE_STAT:
  case SQLCOM_SHOW_STATUS:
  case SQLCOM_SHOW_STATUS_FUNC:
  case SQLCOM_SHOW_STATUS_PROC:
  case SQLCOM_SHOW_STATUS_PACKAGE:
  case SQLCOM_SHOW_STATUS_PACKAGE_BODY:
  case SQLCOM_SHOW_STORAGE_ENGINES:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_SHOW_TABLE_STATUS:
  case SQLCOM_SHOW_VARIABLES:
  case SQLCOM_SHOW_WARNS:
  case SQLCOM_REPAIR:
    flags= sp_head::MULTI_RESULTS;
    break;
  /*
    EXECUTE statement may return a result set, but doesn't have to.
    We can't, however, know it in advance, and therefore must add
    this statement here. This is ok, as is equivalent to a result-set
    statement within an IF condition.
  */
  case SQLCOM_EXECUTE:
  case SQLCOM_EXECUTE_IMMEDIATE:
    flags= sp_head::MULTI_RESULTS | sp_head::CONTAINS_DYNAMIC_SQL;
    break;
  case SQLCOM_PREPARE:
  case SQLCOM_DEALLOCATE_PREPARE:
    flags= sp_head::CONTAINS_DYNAMIC_SQL;
    break;
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_CREATE_SEQUENCE:
    if (lex->tmp_table())
      flags= 0;
    else
      flags= sp_head::HAS_COMMIT_OR_ROLLBACK;
    break;
  case SQLCOM_DROP_TABLE:
  case SQLCOM_DROP_SEQUENCE:
    if (lex->tmp_table())
      flags= 0;
    else
      flags= sp_head::HAS_COMMIT_OR_ROLLBACK;
    break;
  case SQLCOM_FLUSH:
    flags= sp_head::HAS_SQLCOM_FLUSH;
    break;
  case SQLCOM_RESET:
    flags= sp_head::HAS_SQLCOM_RESET;
    break;
  case SQLCOM_CREATE_INDEX:
  case SQLCOM_CREATE_DB:
  case SQLCOM_CREATE_PACKAGE:
  case SQLCOM_CREATE_PACKAGE_BODY:
  case SQLCOM_CREATE_VIEW:
  case SQLCOM_CREATE_TRIGGER:
  case SQLCOM_CREATE_USER:
  case SQLCOM_CREATE_ROLE:
  case SQLCOM_ALTER_TABLE:
  case SQLCOM_ALTER_SEQUENCE:
  case SQLCOM_ALTER_USER:
  case SQLCOM_GRANT:
  case SQLCOM_GRANT_ROLE:
  case SQLCOM_REVOKE:
  case SQLCOM_REVOKE_ROLE:
  case SQLCOM_BEGIN:
  case SQLCOM_RENAME_TABLE:
  case SQLCOM_RENAME_USER:
  case SQLCOM_DROP_INDEX:
  case SQLCOM_DROP_DB:
  case SQLCOM_DROP_PACKAGE:
  case SQLCOM_DROP_PACKAGE_BODY:
  case SQLCOM_REVOKE_ALL:
  case SQLCOM_DROP_USER:
  case SQLCOM_DROP_ROLE:
  case SQLCOM_DROP_VIEW:
  case SQLCOM_DROP_TRIGGER:
  case SQLCOM_TRUNCATE:
  case SQLCOM_COMMIT:
  case SQLCOM_ROLLBACK:
  case SQLCOM_LOAD:
  case SQLCOM_LOCK_TABLES:
  case SQLCOM_CREATE_PROCEDURE:
  case SQLCOM_CREATE_SPFUNCTION:
  case SQLCOM_ALTER_PROCEDURE:
  case SQLCOM_ALTER_FUNCTION:
  case SQLCOM_DROP_PROCEDURE:
  case SQLCOM_DROP_FUNCTION:
  case SQLCOM_CREATE_EVENT:
  case SQLCOM_ALTER_EVENT:
  case SQLCOM_DROP_EVENT:
  case SQLCOM_INSTALL_PLUGIN:
  case SQLCOM_UNINSTALL_PLUGIN:
    flags= sp_head::HAS_COMMIT_OR_ROLLBACK;
    break;
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_INSERT:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_INSERT_SELECT:
  {
    /* 
      DELETE normally doesn't return resultset, but there are 3 exceptions:
       - DELETE ... RETURNING
       - EXPLAIN DELETE ...
       - ANALYZE DELETE ...
    */
    if (!lex->has_returning() && !lex->describe && !lex->analyze_stmt)
      flags= 0;
    else
      flags= sp_head::MULTI_RESULTS; 
    break;
  }
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  {
    if (!lex->describe && !lex->analyze_stmt)
      flags= 0;
    else
      flags= sp_head::MULTI_RESULTS; 
    break;
  }
  default:
    flags= 0;
    break;
  }
  return flags;
}

/**
  Prepare an Item for evaluation (call of fix_fields).

  @param it_addr   pointer on item refernce
  @param cols      expected number of elements (1 for scalar, >=1 for ROWs)

  @retval
    NULL      error
  @retval
    non-NULL  prepared item
*/

Item *THD::sp_prepare_func_item(Item **it_addr, uint cols)
{
  DBUG_ENTER("THD::sp_prepare_func_item");
  Item *res= sp_fix_func_item(it_addr);
  if (res && res->check_cols(cols))
    DBUG_RETURN(NULL);
  DBUG_RETURN(res);
}


/**
  Fix an Item for evaluation for SP.
*/

Item *THD::sp_fix_func_item(Item **it_addr)
{
  DBUG_ENTER("THD::sp_fix_func_item");
  if ((*it_addr)->fix_fields_if_needed(this, it_addr))
  {
    DBUG_PRINT("info", ("fix_fields() failed"));
    DBUG_RETURN(NULL);
  }
  it_addr= (*it_addr)->this_item_addr(this, it_addr);

  if ((*it_addr)->fix_fields_if_needed(this, it_addr))
  {
    DBUG_PRINT("info", ("fix_fields() failed"));
    DBUG_RETURN(NULL);
  }
  DBUG_RETURN(*it_addr);
}


/**
  Evaluate an expression and store the result in the field.

  @param result_field           the field to store the result
  @param expr_item_ptr          the root item of the expression

  @retval
    FALSE  on success
  @retval
    TRUE   on error
*/

bool THD::sp_eval_expr(Field *result_field, Item **expr_item_ptr)
{
  DBUG_ENTER("THD::sp_eval_expr");
  DBUG_ASSERT(*expr_item_ptr);
  Sp_eval_expr_state state(this);
  /* Save the value in the field. Convert the value if needed. */
  DBUG_RETURN(result_field->sp_prepare_and_store_item(this, expr_item_ptr));
}


/**
  Create temporary sp_name object from MDL key.

  @note The lifetime of this object is bound to the lifetime of the MDL_key.
        This should be fine as sp_name objects created by this constructor
        are mainly used for SP-cache lookups.

  @param key         MDL key containing database and routine name.
  @param qname_buff  Buffer to be used for storing quoted routine name
                     (should be at least 2*NAME_LEN+1+1 bytes).
*/

sp_name::sp_name(const MDL_key *key, char *qname_buff)
 :Database_qualified_name(key->db_name(), key->db_name_length(),
                          key->name(),  key->name_length()),
  m_explicit_name(false)
{
  if (m_db.length)
    strxmov(qname_buff, m_db.str, ".", m_name.str, NullS);
  else
    strmov(qname_buff, m_name.str);
}


/**
  Check that the name 'ident' is ok.  It's assumed to be an 'ident'
  from the parser, so we only have to check length and trailing spaces.
  The former is a standard requirement (and 'show status' assumes a
  non-empty name), the latter is a mysql:ism as trailing spaces are
  removed by get_field().

  @retval
    TRUE    bad name
  @retval
    FALSE   name is ok
*/

bool
check_routine_name(const LEX_CSTRING *ident)
{
  DBUG_ASSERT(ident);
  DBUG_ASSERT(ident->str);

  if (!ident->str[0] || ident->str[ident->length-1] == ' ')
  {
    my_error(ER_SP_WRONG_NAME, MYF(0), ident->str);
    return TRUE;
  }
  if (check_ident_length(ident))
    return TRUE;

  return FALSE;
}


/*
 *
 *  sp_head
 *
 */
 
sp_head *sp_head::create(sp_package *parent, const Sp_handler *handler,
                         enum_sp_aggregate_type agg_type)
{
  MEM_ROOT own_root;
  init_sql_alloc(key_memory_sp_head_main_root, &own_root, MEM_ROOT_BLOCK_SIZE,
                 MEM_ROOT_PREALLOC, MYF(0));
  sp_head *sp;
  if (!(sp= new (&own_root) sp_head(&own_root, parent, handler, agg_type)))
    free_root(&own_root, MYF(0));

  return sp;
}


void sp_head::destroy(sp_head *sp)
{
  if (sp)
  {
    /* Make a copy of main_mem_root as free_root will free the sp */
    MEM_ROOT own_root= sp->main_mem_root;
    DBUG_PRINT("info", ("mem_root %p moved to %p",
                        &sp->mem_root, &own_root));
    delete sp;

 
    free_root(&own_root, MYF(0));
  }
}

/*
 *
 *  sp_head
 *
 */

sp_head::sp_head(MEM_ROOT *mem_root_arg, sp_package *parent,
                 const Sp_handler *sph, enum_sp_aggregate_type agg_type)
  :Query_arena(NULL, STMT_INITIALIZED_FOR_SP),
   Database_qualified_name(&null_clex_str, &null_clex_str),
   main_mem_root(*mem_root_arg),
   m_parent(parent),
   m_handler(sph),
   m_flags(0),
   m_tmp_query(NULL),
   m_explicit_name(false),
   /*
     FIXME: the only use case when name is NULL is events, and it should
     be rewritten soon. Remove the else part and replace 'if' with
     an assert when this is done.
   */
   m_qname(null_clex_str),
   m_params(null_clex_str),
   m_body(null_clex_str),
   m_body_utf8(null_clex_str),
   m_defstr(null_clex_str),
   m_sp_cache_version(0),
   m_creation_ctx(0),
   unsafe_flags(0),
   m_created(0),
   m_modified(0),
   m_recursion_level(0),
   m_next_cached_sp(0),
   m_param_begin(NULL),
   m_param_end(NULL),
   m_body_begin(NULL),
   m_thd_root(NULL),
   m_thd(NULL),
   m_pcont(new (&main_mem_root) sp_pcontext()),
   m_cont_level(0)
{
  mem_root= &main_mem_root;

  set_chistics_agg_type(agg_type);
  m_first_instance= this;
  m_first_free_instance= this;
  m_last_cached_sp= this;

  m_return_field_def.charset = NULL;

  DBUG_ENTER("sp_head::sp_head");

  m_security_ctx.init();
  m_backpatch.empty();
  m_backpatch_goto.empty();
  m_cont_backpatch.empty();
  m_lex.empty();
  my_init_dynamic_array(key_memory_sp_head_main_root, &m_instr,
                        sizeof(sp_instr *), 16, 8, MYF(0));
  my_hash_init(key_memory_sp_head_main_root, &m_sptabs, system_charset_info, 0,
               0, 0, sp_table_key, 0, 0);
  my_hash_init(key_memory_sp_head_main_root, &m_sroutines, system_charset_info,
               0, 0, 0, sp_sroutine_key, 0, 0);

  DBUG_VOID_RETURN;
}


sp_package *sp_package::create(LEX *top_level_lex, const sp_name *name,
                               const Sp_handler *sph)
{
  MEM_ROOT own_root;
  init_sql_alloc(key_memory_sp_head_main_root, &own_root, MEM_ROOT_BLOCK_SIZE,
                 MEM_ROOT_PREALLOC, MYF(0));
  sp_package *sp;
  if (!(sp= new (&own_root) sp_package(&own_root, top_level_lex, name, sph)))
    free_root(&own_root, MYF(0));

  return sp;
}


sp_package::sp_package(MEM_ROOT *mem_root_arg,
                       LEX *top_level_lex,
                       const sp_name *name,
                       const Sp_handler *sph)
 :sp_head(mem_root_arg, NULL, sph, DEFAULT_AGGREGATE),
  m_current_routine(NULL),
  m_top_level_lex(top_level_lex),
  m_rcontext(NULL),
  m_invoked_subroutine_count(0),
  m_is_instantiated(false),
  m_is_cloning_routine(false)
{
  init_sp_name(name);
}


sp_package::~sp_package()
{
  m_routine_implementations.cleanup();
  m_routine_declarations.cleanup();
  m_body= null_clex_str;
  if (m_current_routine)
    sp_head::destroy(m_current_routine->sphead);
  delete m_rcontext;
}


/*
  Test if two routines have equal specifications
*/

bool sp_head::eq_routine_spec(const sp_head *sp) const
{
  // TODO: Add tests for equal return data types (in case of FUNCTION)
  // TODO: Add tests for equal argument data types
  return
    m_handler->type() == sp->m_handler->type() &&
    m_pcont->context_var_count() == sp->m_pcont->context_var_count();
}


bool sp_package::validate_after_parser(THD *thd)
{
  if (m_handler->type() != SP_TYPE_PACKAGE_BODY)
    return false;
  sp_head *sp= sp_cache_lookup(&thd->sp_package_spec_cache, this);
  sp_package *spec= sp ? sp->get_package() : NULL;
  DBUG_ASSERT(spec); // CREATE PACKAGE must already be cached
  return validate_public_routines(thd, spec) ||
         validate_private_routines(thd);
}


bool sp_package::validate_public_routines(THD *thd, sp_package *spec)
{
  /*
    Check that all routines declared in CREATE PACKAGE
    have implementations in CREATE PACKAGE BODY.
  */
  List_iterator<LEX> it(spec->m_routine_declarations);
  for (LEX *lex; (lex= it++); )
  {
    bool found= false;
    DBUG_ASSERT(lex->sphead);
    List_iterator<LEX> it2(m_routine_implementations);
    for (LEX *lex2; (lex2= it2++); )
    {
      DBUG_ASSERT(lex2->sphead);
      if (Sp_handler::eq_routine_name(lex2->sphead->m_name,
                                      lex->sphead->m_name) &&
          lex2->sphead->eq_routine_spec(lex->sphead))
      {
        found= true;
        break;
      }
    }
    if (!found)
    {
      my_error(ER_PACKAGE_ROUTINE_IN_SPEC_NOT_DEFINED_IN_BODY, MYF(0),
               ErrConvDQName(lex->sphead).ptr());
      return true;
    }
  }
  return false;
}


bool sp_package::validate_private_routines(THD *thd)
{
  /*
    Check that all forwad declarations in
    CREATE PACKAGE BODY have implementations.
  */
  List_iterator<LEX> it(m_routine_declarations);
  for (LEX *lex; (lex= it++); )
  {
    bool found= false;
    DBUG_ASSERT(lex->sphead);
    List_iterator<LEX> it2(m_routine_implementations);
    for (LEX *lex2; (lex2= it2++); )
    {
      DBUG_ASSERT(lex2->sphead);
      if (Sp_handler::eq_routine_name(lex2->sphead->m_name,
                                      lex->sphead->m_name) &&
          lex2->sphead->eq_routine_spec(lex->sphead))
      {
        found= true;
        break;
      }
    }
    if (!found)
    {
      my_error(ER_PACKAGE_ROUTINE_FORWARD_DECLARATION_NOT_DEFINED, MYF(0),
               ErrConvDQName(lex->sphead).ptr());
      return true;
    }
  }
  return false;
}


LEX *sp_package::LexList::find(const LEX_CSTRING &name,
                               enum_sp_type type)
{
  List_iterator<LEX> it(*this);
  for (LEX *lex; (lex= it++); )
  {
    DBUG_ASSERT(lex->sphead);
    const char *dot;
    if (lex->sphead->m_handler->type() == type &&
        (dot= strrchr(lex->sphead->m_name.str, '.')))
    {
      size_t ofs= dot + 1 - lex->sphead->m_name.str;
      LEX_CSTRING non_qualified_sphead_name= lex->sphead->m_name;
      non_qualified_sphead_name.str+= ofs;
      non_qualified_sphead_name.length-= ofs;
      if (Sp_handler::eq_routine_name(non_qualified_sphead_name, name))
        return lex;
    }
  }
  return NULL;
}


LEX *sp_package::LexList::find_qualified(const LEX_CSTRING &name,
                                         enum_sp_type type)
{
  List_iterator<LEX> it(*this);
  for (LEX *lex; (lex= it++); )
  {
    DBUG_ASSERT(lex->sphead);
    if (lex->sphead->m_handler->type() == type &&
        Sp_handler::eq_routine_name(lex->sphead->m_name, name))
      return lex;
  }
  return NULL;
}


void sp_package::init_psi_share()
{
  List_iterator<LEX> it(m_routine_implementations);
  for (LEX *lex; (lex= it++); )
  {
    DBUG_ASSERT(lex->sphead);
    lex->sphead->init_psi_share();
  }
  sp_head::init_psi_share();
}

void
sp_head::init(LEX *lex)
{
  DBUG_ENTER("sp_head::init");

  lex->spcont= m_pcont;

  if (!lex->spcont)
    DBUG_VOID_RETURN;

  /*
    Altough trg_table_fields list is used only in triggers we init for all
    types of stored procedures to simplify reset_lex()/restore_lex() code.
  */
  lex->trg_table_fields.empty();

  DBUG_VOID_RETURN;
}


void
sp_head::init_sp_name(const sp_name *spname)
{
  DBUG_ENTER("sp_head::init_sp_name");

  /* Must be initialized in the parser. */

  DBUG_ASSERT(spname && spname->m_db.str && spname->m_db.length);

  /* We have to copy strings to get them into the right memroot. */
  Database_qualified_name::copy(&main_mem_root, spname->m_db, spname->m_name);
  m_explicit_name= spname->m_explicit_name;
  DBUG_VOID_RETURN;
}

void
sp_head::init_psi_share()
{
  m_sp_share= MYSQL_GET_SP_SHARE(m_handler->type(), m_db.str, static_cast<uint>(m_db.length),
                                 m_name.str, static_cast<uint>(m_name.length));
}


void
sp_head::set_body_start(THD *thd, const char *begin_ptr)
{
  m_body_begin= begin_ptr;
  thd->m_parser_state->m_lip.body_utf8_start(thd, begin_ptr);
}


void
sp_head::set_stmt_end(THD *thd)
{
  Lex_input_stream *lip= & thd->m_parser_state->m_lip; /* shortcut */
  const char *end_ptr= lip->get_cpp_tok_start(); /* shortcut */

  /* Make the string of parameters. */

  if (m_param_begin && m_param_end)
  {
    m_params.length= m_param_end - m_param_begin;
    m_params.str= thd->strmake(m_param_begin, m_params.length);
  }

  /* Remember end pointer for further dumping of whole statement. */

  thd->lex->stmt_definition_end= end_ptr;

  /* Make the string of body (in the original character set). */

  m_body.length= end_ptr - m_body_begin;
  m_body.str= thd->strmake(m_body_begin, m_body.length);
  trim_whitespace(thd->charset(), &m_body);

  /* Make the string of UTF-body. */

  lip->body_utf8_append(end_ptr);

  m_body_utf8.length= lip->get_body_utf8_length();
  m_body_utf8.str= thd->strmake(lip->get_body_utf8_str(), m_body_utf8.length);
  trim_whitespace(thd->charset(), &m_body_utf8);

  /*
    Make the string of whole stored-program-definition query (in the
    original character set).
  */

  m_defstr.length= end_ptr - lip->get_cpp_buf();
  m_defstr.str= thd->strmake(lip->get_cpp_buf(), m_defstr.length);
  trim_whitespace(thd->charset(), &m_defstr);
}


sp_head::~sp_head()
{
  LEX *lex;
  sp_instr *i;
  DBUG_ENTER("sp_head::~sp_head");

  /* sp_head::restore_thd_mem_root() must already have been called. */
  DBUG_ASSERT(m_thd == NULL);

  for (uint ip = 0 ; (i = get_instr(ip)) ; ip++)
    delete i;
  delete_dynamic(&m_instr);
  delete m_pcont;
  free_items();

  /*
    If we have non-empty LEX stack then we just came out of parser with
    error. Now we should delete all auxilary LEXes and restore original
    THD::lex. It is safe to not update LEX::ptr because further query
    string parsing and execution will be stopped anyway.
  */
  while ((lex= (LEX *)m_lex.pop()))
  {
    THD *thd= lex->thd;
    thd->lex->sphead= NULL;
    lex_end(thd->lex);
    delete thd->lex;
    thd->lex= lex;
  }

  my_hash_free(&m_sptabs);
  my_hash_free(&m_sroutines);

  sp_head::destroy(m_next_cached_sp);

  DBUG_VOID_RETURN;
}


void sp_package::LexList::cleanup()
{
  List_iterator<LEX> it(*this);
  for (LEX *lex; (lex= it++); )
  {
    lex_end(lex);
    delete lex;
  }
}


/**
  This is only used for result fields from functions (both during
  fix_length_and_dec() and evaluation).
*/

Field *
sp_head::create_result_field(uint field_max_length, const LEX_CSTRING *field_name,
                             TABLE *table) const
{
  Field *field;
  LEX_CSTRING name;

  DBUG_ENTER("sp_head::create_result_field");

  /*
    m_return_field_def.length is always set to the field length calculated
    by the parser, according to the RETURNS clause. See prepare_create_field()
    in sql_table.cc. Value examples, depending on data type:
    - 11 for INT                          (character representation length)
    - 20 for BIGINT                       (character representation length)
    - 22 for DOUBLE                       (character representation length)
    - N for CHAR(N) CHARACTER SET latin1  (octet length)
    - 3*N for CHAR(N) CHARACTER SET utf8  (octet length)
    - 8 for blob-alike data types         (packed length !!!)

    field_max_length is also set according to the data type in the RETURNS
    clause but can have different values depending on the execution stage:

    1. During direct execution:
    field_max_length is 0, because Item_func_sp::fix_length_and_dec() has
    not been called yet, so Item_func_sp::max_length is 0 by default.

    2a. During PREPARE:
    field_max_length is 0, because Item_func_sp::fix_length_and_dec()
    has not been called yet. It's called after create_result_field().

    2b. During EXEC:
    field_max_length is set to the maximum possible octet length of the
    RETURNS data type.
    - N for CHAR(N) CHARACTER SET latin1  (octet length)
    - 3*N for CHAR(N) CHARACTER SET utf8  (octet length)
    - 255 for TINYBLOB                    (octet length, not packed length !!!)

    Perhaps we should refactor prepare_create_field() to set
    Create_field::length to maximum octet length for BLOBs,
    instead of packed length).

    Note, for integer data types, field_max_length can be bigger
    than the user specified length, e.g. a field of the INT(1) data type
    is translated to the item with max_length=11.
  */
  DBUG_ASSERT(field_max_length <= m_return_field_def.length ||
              m_return_field_def.type_handler()->cmp_type() == INT_RESULT ||
              (current_thd->stmt_arena->is_stmt_execute() &&
               m_return_field_def.length == 8 &&
               (m_return_field_def.pack_flag &
                (FIELDFLAG_BLOB|FIELDFLAG_GEOM))));

  if (field_name)
    name= *field_name;
  else
    name= m_name;
  field= m_return_field_def.make_field(table->s, /* TABLE_SHARE ptr */
                                       table->in_use->mem_root,
                                       &name);

  field->vcol_info= m_return_field_def.vcol_info;
  if (field)
    field->init(table);

  DBUG_RETURN(field);
}


int cmp_rqp_locations(Rewritable_query_parameter * const *a,
                      Rewritable_query_parameter * const *b)
{
  return (int)((*a)->pos_in_query - (*b)->pos_in_query);
}


/*
  StoredRoutinesBinlogging
  This paragraph applies only to statement-based binlogging. Row-based
  binlogging does not need anything special like this.

  Top-down overview:

  1. Statements

  Statements that have is_update_query(stmt) == TRUE are written into the
  binary log verbatim.
  Examples:
    UPDATE tbl SET tbl.x = spfunc_w_side_effects()
    UPDATE tbl SET tbl.x=1 WHERE spfunc_w_side_effect_that_returns_false(tbl.y)

  Statements that have is_update_query(stmt) == FALSE (e.g. SELECTs) are not
  written into binary log. Instead we catch function calls the statement
  makes and write it into binary log separately (see #3).

  2. PROCEDURE calls

  CALL statements are not written into binary log. Instead
  * Any FUNCTION invocation (in SET, IF, WHILE, OPEN CURSOR and other SP
    instructions) is written into binlog separately.

  * Each statement executed in SP is binlogged separately, according to rules
    in #1, with the exception that we modify query string: we replace uses
    of SP local variables with NAME_CONST('spvar_name', <spvar-value>) calls.
    This substitution is done in subst_spvars().

  3. FUNCTION calls

  In sp_head::execute_function(), we check
   * If this function invocation is done from a statement that is written
     into the binary log.
   * If there were any attempts to write events to the binary log during
     function execution (grep for start_union_events and stop_union_events)

   If the answers are No and Yes, we write the function call into the binary
   log as "SELECT spfunc(<param1value>, <param2value>, ...)"


  4. Miscellaneous issues.

  4.1 User variables.

  When we call mysql_bin_log.write() for an SP statement, thd->user_var_events
  must hold set<{var_name, value}> pairs for all user variables used during
  the statement execution.
  This set is produced by tracking user variable reads during statement
  execution.

  For SPs, this has the following implications:
  1) thd->user_var_events may contain events from several SP statements and
     needs to be valid after exection of these statements was finished. In
     order to achieve that, we
     * Allocate user_var_events array elements on appropriate mem_root (grep
       for user_var_events_alloc).
     * Use is_query_in_union() to determine if user_var_event is created.

  2) We need to empty thd->user_var_events after we have wrote a function
     call. This is currently done by making
     reset_dynamic(&thd->user_var_events);
     calls in several different places. (TODO cosider moving this into
     mysql_bin_log.write() function)

  4.2 Auto_increment storage in binlog

  As we may write two statements to binlog from one single logical statement
  (case of "SELECT func1(),func2()": it is binlogged as "SELECT func1()" and
  then "SELECT func2()"), we need to reset auto_increment binlog variables
  after each binlogged SELECT. Otherwise, the auto_increment value of the
  first SELECT would be used for the second too.
*/


/**
  Replace thd->query{_length} with a string that one can write to
  the binlog.

  The binlog-suitable string is produced by replacing references to SP local
  variables with NAME_CONST('sp_var_name', value) calls.

  @param thd        Current thread.
  @param instr      Instruction (we look for Item_splocal instances in
                    instr->free_list)
  @param query_str  Original query string

  @return
    - FALSE  on success.
    thd->query{_length} either has been appropriately replaced or there
    is no need for replacements.
    - TRUE   out of memory error.
*/

static bool
subst_spvars(THD *thd, sp_instr *instr, LEX_STRING *query_str)
{
  DBUG_ENTER("subst_spvars");

  Dynamic_array<Rewritable_query_parameter*> rewritables(PSI_INSTRUMENT_MEM);
  char *pbuf;
  StringBuffer<512> qbuf;
  Copy_query_with_rewrite acc(thd, query_str->str, query_str->length, &qbuf);

  /* Find rewritable Items used in this statement */
  for (Item *item= instr->free_list; item; item= item->next)
  {
    Rewritable_query_parameter *rqp= item->get_rewritable_query_parameter();
    if (rqp && rqp->pos_in_query)
      rewritables.append(rqp);
  }
  if (!rewritables.elements())
    DBUG_RETURN(FALSE);

  rewritables.sort(cmp_rqp_locations);

  thd->query_name_consts= (uint)rewritables.elements();

  for (Rewritable_query_parameter **rqp= rewritables.front();
       rqp <= rewritables.back(); rqp++)
  {
    if (acc.append(*rqp))
      DBUG_RETURN(TRUE);
  }
  if (acc.finalize())
    DBUG_RETURN(TRUE);

  /*
    Allocate additional space at the end of the new query string for the
    query_cache_send_result_to_client function.

    The query buffer layout is:
       buffer :==
            <statement>   The input statement(s)
            '\0'          Terminating null char
            <length>      Length of following current database name 2
            <db_name>     Name of current database
            <flags>       Flags struct
  */
  size_t buf_len= (qbuf.length() + 1 + QUERY_CACHE_DB_LENGTH_SIZE +
                thd->db.length + QUERY_CACHE_FLAGS_SIZE + 1);
  if ((pbuf= (char *) alloc_root(thd->mem_root, buf_len)))
  {
    char *ptr= pbuf + qbuf.length();
    memcpy(pbuf, qbuf.ptr(), qbuf.length());
    *ptr= 0;
    int2store(ptr+1, thd->db.length);
  }
  else
    DBUG_RETURN(TRUE);

  thd->set_query(pbuf, qbuf.length());

  DBUG_RETURN(FALSE);
}


void Sp_handler_procedure::recursion_level_error(THD *thd,
                                                 const sp_head *sp) const
{
  my_error(ER_SP_RECURSION_LIMIT, MYF(0),
           static_cast<int>(thd->variables.max_sp_recursion_depth),
           sp->m_name.str);
}


/**
  Execute the routine. The main instruction jump loop is there.
  Assume the parameters already set.

  @param thd                  Thread context.
  @param merge_da_on_success  Flag specifying if Warning Info should be
                              propagated to the caller on Completion
                              Condition or not.

  @todo
    - Will write this SP statement into binlog separately
    (TODO: consider changing the condition to "not inside event union")

  @return Error status.
  @retval
    FALSE  on success
  @retval
    TRUE   on error
*/

bool
sp_head::execute(THD *thd, bool merge_da_on_success)
{
  DBUG_ENTER("sp_head::execute");
  char saved_cur_db_name_buf[SAFE_NAME_LEN+1];
  LEX_STRING saved_cur_db_name=
    { saved_cur_db_name_buf, sizeof(saved_cur_db_name_buf) };
  bool cur_db_changed= FALSE;
  sp_rcontext *ctx= thd->spcont;
  bool err_status= FALSE;
  uint ip= 0;
  sql_mode_t save_sql_mode;

  // TODO(cvicentiu) See if you can drop this bit. This is used to resume
  // execution from where we left off.
  if (m_chistics.agg_type == GROUP_AGGREGATE)
    ip= thd->spcont->instr_ptr;

  bool save_abort_on_warning;
  Query_arena *old_arena;
  /* per-instruction arena */
  MEM_ROOT execute_mem_root;
  Query_arena execute_arena(&execute_mem_root, STMT_INITIALIZED_FOR_SP),
              backup_arena;
  query_id_t old_query_id;
  CSET_STRING old_query;
  TABLE *old_derived_tables;
  TABLE *old_rec_tables;
  LEX *old_lex;
  Item_change_list old_change_list;
  String old_packet;
  uint old_server_status;
  const uint status_backup_mask= SERVER_STATUS_CURSOR_EXISTS |
                                 SERVER_STATUS_LAST_ROW_SENT;
  MEM_ROOT *user_var_events_alloc_saved= 0;
  Reprepare_observer *save_reprepare_observer= thd->m_reprepare_observer;
  Object_creation_ctx *UNINIT_VAR(saved_creation_ctx);
  Diagnostics_area *da= thd->get_stmt_da();
  Warning_info sp_wi(da->warning_info_id(), false, true);

  /* this 7*STACK_MIN_SIZE is a complex matter with a long history (see it!) */
  if (check_stack_overrun(thd, 7 * STACK_MIN_SIZE, (uchar*)&old_packet))
    DBUG_RETURN(TRUE);

  opt_trace_disable_if_no_security_context_access(thd);

  /* init per-instruction memroot */
  init_sql_alloc(key_memory_sp_head_execute_root, &execute_mem_root,
                 MEM_ROOT_BLOCK_SIZE, 0, MYF(0));

  DBUG_ASSERT(!(m_flags & IS_INVOKED));
  m_flags|= IS_INVOKED;
  if (m_parent)
    m_parent->m_invoked_subroutine_count++;
  m_first_instance->m_first_free_instance= m_next_cached_sp;
  if (m_next_cached_sp)
  {
    DBUG_PRINT("info",
               ("first free for %p ++: %p->%p  level: %lu  flags %x",
               m_first_instance, this,
                m_next_cached_sp,
                m_next_cached_sp->m_recursion_level,
                m_next_cached_sp->m_flags));
  }
  /*
    Check that if there are not any instances after this one then
    pointer to the last instance points on this instance or if there are
    some instances after this one then recursion level of next instance
    greater then recursion level of current instance on 1
  */
  DBUG_ASSERT((m_next_cached_sp == 0 &&
               m_first_instance->m_last_cached_sp == this) ||
              (m_recursion_level + 1 == m_next_cached_sp->m_recursion_level));

  /*
    NOTE: The SQL Standard does not specify the context that should be
    preserved for stored routines. However, at SAP/Walldorf meeting it was
    decided that current database should be preserved.
  */

  if (m_db.length &&
      (err_status= mysql_opt_change_db(thd, &m_db, &saved_cur_db_name, FALSE,
                                       &cur_db_changed)))
  {
    goto done;
  }

  thd->is_slave_error= 0;
  old_arena= thd->stmt_arena;

  /* Push a new warning information area. */
  da->copy_sql_conditions_to_wi(thd, &sp_wi);
  da->push_warning_info(&sp_wi);

  /*
    Switch query context. This has to be done early as this is sometimes
    allocated on THD::mem_root
  */
  if (m_creation_ctx)
    saved_creation_ctx= m_creation_ctx->set_n_backup(thd);

  /*
    We have to save/restore this info when we are changing call level to
    be able properly do close_thread_tables() in instructions.
  */
  old_query_id= thd->query_id;
  old_query= thd->query_string;
  old_derived_tables= thd->derived_tables;
  thd->derived_tables= 0;
  old_rec_tables= thd->rec_tables;
  thd->rec_tables= 0;
  save_sql_mode= thd->variables.sql_mode;
  thd->variables.sql_mode= m_sql_mode;
  save_abort_on_warning= thd->abort_on_warning;
  thd->abort_on_warning= 0;
  /**
    When inside a substatement (a stored function or trigger
    statement), clear the metadata observer in THD, if any.
    Remember the value of the observer here, to be able
    to restore it when leaving the substatement.

    We reset the observer to suppress errors when a substatement
    uses temporary tables. If a temporary table does not exist
    at start of the main statement, it's not prelocked
    and thus is not validated with other prelocked tables.

    Later on, when the temporary table is opened, metadata
    versions mismatch, expectedly.

    The proper solution for the problem is to re-validate tables
    of substatements (Bug#12257, Bug#27011, Bug#32868, Bug#33000),
    but it's not implemented yet.
  */
  thd->m_reprepare_observer= 0;

  /*
    It is also more efficient to save/restore current thd->lex once when
    do it in each instruction
  */
  old_lex= thd->lex;
  /*
    We should also save Item tree change list to avoid rollback something
    too early in the calling query.
  */
  thd->Item_change_list::move_elements_to(&old_change_list);
  /*
    Cursors will use thd->packet, so they may corrupt data which was prepared
    for sending by upper level. OTOH cursors in the same routine can share this
    buffer safely so let use use routine-local packet instead of having own
    packet buffer for each cursor.

    It is probably safe to use same thd->convert_buff everywhere.
  */
  old_packet.swap(thd->packet);
  old_server_status= thd->server_status & status_backup_mask;

  /*
    Switch to per-instruction arena here. We can do it since we cleanup
    arena after every instruction.
  */
  thd->set_n_backup_active_arena(&execute_arena, &backup_arena);

  /*
    Save callers arena in order to store instruction results and out
    parameters in it later during sp_eval_func_item()
  */
  thd->spcont->callers_arena= &backup_arena;

#if defined(ENABLED_PROFILING)
  /* Discard the initial part of executing routines. */
  thd->profiling.discard_current_query();
#endif
  sp_instr *i;
  DEBUG_SYNC(thd, "sp_head_execute_before_loop");
  do
  {
#if defined(ENABLED_PROFILING)
    /*
     Treat each "instr" of a routine as discrete unit that could be profiled.
     Profiling only records information for segments of code that set the
     source of the query, and almost all kinds of instructions in s-p do not.
    */
    thd->profiling.finish_current_query();
    thd->profiling.start_new_query("continuing inside routine");
#endif

    /* get_instr returns NULL when we're done. */
    i = get_instr(ip);
    if (i == NULL)
    {
#if defined(ENABLED_PROFILING)
      thd->profiling.discard_current_query();
#endif
      thd->spcont->quit_func= TRUE;
      break;
    }

    /* Reset number of warnings for this query. */
    thd->get_stmt_da()->reset_for_next_command();

    DBUG_PRINT("execute", ("Instruction %u", ip));

    /*
      We need to reset start_time to allow for time to flow inside a stored
      procedure. This is only done for SP since time is suppose to be constant
      during execution of triggers and functions.
    */
    reset_start_time_for_sp(thd);

    /*
      We have to set thd->stmt_arena before executing the instruction
      to store in the instruction free_list all new items, created
      during the first execution (for example expanding of '*' or the
      items made during other permanent subquery transformations).
    */
    thd->stmt_arena= i;

    /*
      Will write this SP statement into binlog separately.
      TODO: consider changing the condition to "not inside event union".
    */
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES)
    {
      user_var_events_alloc_saved= thd->user_var_events_alloc;
      thd->user_var_events_alloc= thd->mem_root;
    }

    sql_digest_state *parent_digest= thd->m_digest;
    thd->m_digest= NULL;

#ifdef WITH_WSREP
    if (WSREP(thd) && thd->wsrep_next_trx_id() == WSREP_UNDEFINED_TRX_ID)
    {
      thd->set_wsrep_next_trx_id(thd->query_id);
      WSREP_DEBUG("assigned new next trx ID for SP,  trx id: %" PRIu64, thd->wsrep_next_trx_id());
    }
#endif /* WITH_WSREP */

#ifdef HAVE_PSI_STATEMENT_INTERFACE
    PSI_statement_locker_state state;
    PSI_statement_locker *parent_locker;
    PSI_statement_info *psi_info = i->get_psi_info();

    parent_locker= thd->m_statement_psi;
    thd->m_statement_psi= MYSQL_START_STATEMENT(& state, psi_info->m_key,
      thd->db.str, thd->db.length, thd->charset(), m_sp_share);
#endif

    err_status= i->execute(thd, &ip);

#ifdef HAVE_PSI_STATEMENT_INTERFACE
    MYSQL_END_STATEMENT(thd->m_statement_psi, thd->get_stmt_da());
    thd->m_statement_psi= parent_locker;
#endif

#ifdef WITH_WSREP
    if (WSREP(thd))
    {
      if (((thd->wsrep_trx().state() == wsrep::transaction::s_executing || thd->in_sub_stmt) &&
           (thd->is_fatal_error || thd->killed)))
      {
        WSREP_DEBUG("SP abort err status %d in sub %d trx state %d",
                    err_status, thd->in_sub_stmt, thd->wsrep_trx().state());
        err_status= 1;
        thd->is_fatal_error= 1;
        /*
          SP was killed, and it is not due to a wsrep conflict.
          We skip after_command hook at this point because
          otherwise it clears the error, and cleans up the
          whole transaction. For now we just return and finish
          our handling once we are back to mysql_parse.

          Same applies to a SP execution, which was aborted due
          to wsrep related conflict, but which is executing as sub statement.
          SP in sub statement level should not commit not rollback,
          we have to call for rollback is up-most SP level.
        */
        WSREP_DEBUG("Skipping after_command hook for killed SP");
      }
      else
      {
        const bool must_replay= wsrep_must_replay(thd);
        if (must_replay)
        {
          WSREP_DEBUG("MUST_REPLAY set after SP, err_status %d trx state: %d",
                      err_status, thd->wsrep_trx().state());
        }

        if (wsrep_thd_is_local(thd))
          (void) wsrep_after_statement(thd);

        /*
          Reset the return code to zero if the transaction was
          replayed successfully.
        */
        if (must_replay && !wsrep_current_error(thd))
        {
          err_status= 0;
          thd->get_stmt_da()->reset_diagnostics_area();
        }
        /*
          Final wsrep error status for statement is known only after
          wsrep_after_statement() call. If the error is set, override
          error in thd diagnostics area and reset wsrep client_state error
          so that the error does not get propagated via client-server protocol.
        */
        if (wsrep_current_error(thd))
        {
          wsrep_override_error(thd, wsrep_current_error(thd),
                               wsrep_current_error_status(thd));
          thd->wsrep_cs().reset_error();
          /* Reset also thd->killed if it has been set during BF abort. */
          if (thd->killed == KILL_QUERY)
            thd->killed= NOT_KILLED;
          /* if failed transaction was not replayed, must return with error from here */
          if (!must_replay) err_status = 1;
        }
      }
    }
#endif /* WITH_WSREP */
    thd->m_digest= parent_digest;

    if (i->free_list)
      cleanup_items(i->free_list);

    /*
      If we've set thd->user_var_events_alloc to mem_root of this SP
      statement, clean all the events allocated in it.
    */
    if (thd->locked_tables_mode <= LTM_LOCK_TABLES)
    {
      reset_dynamic(&thd->user_var_events);
      thd->user_var_events_alloc= user_var_events_alloc_saved;
    }

    /* we should cleanup free_list and memroot, used by instruction */
    thd->cleanup_after_query();
    free_root(&execute_mem_root, MYF(0));

    /*
      Find and process SQL handlers unless it is a fatal error (fatal
      errors are not catchable by SQL handlers) or the connection has been
      killed during execution.
    */
    if (likely(!thd->is_fatal_error) && likely(!thd->killed_errno()) &&
        ctx->handle_sql_condition(thd, &ip, i))
    {
      err_status= FALSE;
    }

    /* Reset sp_rcontext::end_partial_result_set flag. */
    ctx->end_partial_result_set= FALSE;

  } while (!err_status && likely(!thd->killed) &&
           likely(!thd->is_fatal_error) &&
           !thd->spcont->pause_state);

#if defined(ENABLED_PROFILING)
  thd->profiling.finish_current_query();
  thd->profiling.start_new_query("tail end of routine");
#endif

  /* Restore query context. */

  if (m_creation_ctx)
    m_creation_ctx->restore_env(thd, saved_creation_ctx);

  /* Restore arena. */

  thd->restore_active_arena(&execute_arena, &backup_arena);

  /* Only pop cursors when we're done with group aggregate running. */
  if (m_chistics.agg_type != GROUP_AGGREGATE ||
      (m_chistics.agg_type == GROUP_AGGREGATE && thd->spcont->quit_func))
    thd->spcont->pop_all_cursors(thd); // To avoid memory leaks after an error

  /* Restore all saved */
  if (m_chistics.agg_type == GROUP_AGGREGATE)
    thd->spcont->instr_ptr= ip;
  thd->server_status= (thd->server_status & ~status_backup_mask) | old_server_status;
  old_packet.swap(thd->packet);
  DBUG_ASSERT(thd->Item_change_list::is_empty());
  old_change_list.move_elements_to(thd);
  thd->lex= old_lex;
  thd->set_query_id(old_query_id);
  thd->set_query_inner(old_query);
  DBUG_ASSERT(!thd->derived_tables);
  thd->derived_tables= old_derived_tables;
  thd->rec_tables= old_rec_tables;
  thd->variables.sql_mode= save_sql_mode;
  thd->abort_on_warning= save_abort_on_warning;
  thd->m_reprepare_observer= save_reprepare_observer;

  thd->stmt_arena= old_arena;
  state= STMT_EXECUTED;

  /*
    Restore the caller's original warning information area:
      - warnings generated during trigger execution should not be
        propagated to the caller on success;
      - if there was an exception during execution, warning info should be
        propagated to the caller in any case.
  */
  da->pop_warning_info();

  if (err_status || merge_da_on_success)
  {
    /*
      If a routine body is empty or if a routine did not generate any warnings,
      do not duplicate our own contents by appending the contents of the called
      routine. We know that the called routine did not change its warning info.

      On the other hand, if the routine body is not empty and some statement in
      the routine generates a warning or uses tables, warning info is guaranteed
      to have changed. In this case we know that the routine warning info
      contains only new warnings, and thus we perform a copy.
    */
    if (da->warning_info_changed(&sp_wi))
    {
      /*
        If the invocation of the routine was a standalone statement,
        rather than a sub-statement, in other words, if it's a CALL
        of a procedure, rather than invocation of a function or a
        trigger, we need to clear the current contents of the caller's
        warning info.

        This is per MySQL rules: if a statement generates a warning,
        warnings from the previous statement are flushed.  Normally
        it's done in push_warning(). However, here we don't use
        push_warning() to avoid invocation of condition handlers or
        escalation of warnings to errors.
      */
      da->opt_clear_warning_info(thd->query_id);
      da->copy_sql_conditions_from_wi(thd, &sp_wi);
      da->remove_marked_sql_conditions();
      if (i != NULL)
        push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                            ER_SP_STACK_TRACE,
                            ER_THD(thd, ER_SP_STACK_TRACE),
                            i->m_lineno,
                            m_qname.str != NULL ? m_qname.str :
                                                  "anonymous block");
    }
  }

 done:
  DBUG_PRINT("info", ("err_status: %d  killed: %d  is_slave_error: %d  report_error: %d",
                      err_status, thd->killed, thd->is_slave_error,
                      thd->is_error()));

  if (thd->killed)
    err_status= TRUE;
  /*
    If the DB has changed, the pointer has changed too, but the
    original thd->db will then have been freed
  */
  if (cur_db_changed && thd->killed != KILL_CONNECTION)
  {
    /*
      Force switching back to the saved current database, because it may be
      NULL. In this case, mysql_change_db() would generate an error.
    */

    err_status|= mysql_change_db(thd, (LEX_CSTRING*)&saved_cur_db_name, TRUE) != 0;
  }
  m_flags&= ~IS_INVOKED;
  if (m_parent)
    m_parent->m_invoked_subroutine_count--;
  DBUG_PRINT("info",
             ("first free for %p --: %p->%p, level: %lu, flags %x",
              m_first_instance,
              m_first_instance->m_first_free_instance,
              this, m_recursion_level, m_flags));
  /*
    Check that we have one of following:

    1) there are not free instances which means that this instance is last
    in the list of instances (pointer to the last instance point on it and
    ther are not other instances after this one in the list)

    2) There are some free instances which mean that first free instance
    should go just after this one and recursion level of that free instance
    should be on 1 more then recursion level of this instance.
  */
  DBUG_ASSERT((m_first_instance->m_first_free_instance == 0 &&
               this == m_first_instance->m_last_cached_sp &&
               m_next_cached_sp == 0) ||
              (m_first_instance->m_first_free_instance != 0 &&
               m_first_instance->m_first_free_instance == m_next_cached_sp &&
               m_first_instance->m_first_free_instance->m_recursion_level ==
               m_recursion_level + 1));
  m_first_instance->m_first_free_instance= this;

  DBUG_RETURN(err_status);
}


#ifndef NO_EMBEDDED_ACCESS_CHECKS
/**
  set_routine_security_ctx() changes routine security context, and
  checks if there is an EXECUTE privilege in new context.  If there is
  no EXECUTE privilege, it changes the context back and returns a
  error.

  @param thd         thread handle
  @param sp          stored routine to change the context for
  @param save_ctx    pointer to an old security context

  @todo
    - Cache if the definer has the right to use the object on the
    first usage and only reset the cache if someone does a GRANT
    statement that 'may' affect this.

  @retval
    TRUE   if there was a error, and the context wasn't changed.
  @retval
    FALSE  if the context was changed.
*/

bool
set_routine_security_ctx(THD *thd, sp_head *sp, Security_context **save_ctx)
{
  *save_ctx= 0;
  if (sp->suid() != SP_IS_NOT_SUID &&
      sp->m_security_ctx.change_security_context(thd, &sp->m_definer.user,
                                                 &sp->m_definer.host,
                                                 &sp->m_db,
                                                 save_ctx))
    return TRUE;

  /*
    If we changed context to run as another user, we need to check the
    access right for the new context again as someone may have revoked
    the right to use the procedure from this user.

    TODO:
      Cache if the definer has the right to use the object on the
      first usage and only reset the cache if someone does a GRANT
      statement that 'may' affect this.
  */
  if (*save_ctx &&
      sp->check_execute_access(thd))
  {
    sp->m_security_ctx.restore_security_context(thd, *save_ctx);
    *save_ctx= 0;
    return TRUE;
  }

  return FALSE;
}
#endif // ! NO_EMBEDDED_ACCESS_CHECKS


bool sp_head::check_execute_access(THD *thd) const
{
  return m_parent ? m_parent->check_execute_access(thd) :
                    check_routine_access(thd, EXECUTE_ACL,
                                         &m_db, &m_name,
                                         m_handler, false);
}


/**
  Create rcontext optionally using the routine security.
  This is important for sql_mode=ORACLE to make sure that the invoker has
  access to the tables mentioned in the %TYPE references.

  In non-Oracle sql_modes we do not need access to any tables,
  so we can omit the security context switch for performance purposes.

  @param thd
  @param ret_value
  @retval           NULL - error (access denided or EOM)
  @retval          !NULL - success (the invoker has rights to all %TYPE tables)
*/

sp_rcontext *sp_head::rcontext_create(THD *thd, Field *ret_value,
                                      Row_definition_list *defs,
                                      bool switch_security_ctx)
{
  if (!(m_flags & HAS_COLUMN_TYPE_REFS))
    return sp_rcontext::create(thd, this, m_pcont, ret_value, *defs);
  sp_rcontext *res= NULL;
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *save_security_ctx;
  if (switch_security_ctx &&
      set_routine_security_ctx(thd, this, &save_security_ctx))
    return NULL;
#endif
  if (!defs->resolve_type_refs(thd))
    res= sp_rcontext::create(thd, this, m_pcont, ret_value, *defs);
#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (switch_security_ctx)
    m_security_ctx.restore_security_context(thd, save_security_ctx);
#endif
  return res;
}


sp_rcontext *sp_head::rcontext_create(THD *thd, Field *ret_value,
                                      List<Item> *args)
{
  DBUG_ASSERT(args);
  Row_definition_list defs;
  m_pcont->retrieve_field_definitions(&defs);
  if (defs.adjust_formal_params_to_actual_params(thd, args))
    return NULL;
  return rcontext_create(thd, ret_value, &defs, true);
}


sp_rcontext *sp_head::rcontext_create(THD *thd, Field *ret_value,
                                      Item **args, uint arg_count)
{
  Row_definition_list defs;
  m_pcont->retrieve_field_definitions(&defs);
  if (defs.adjust_formal_params_to_actual_params(thd, args, arg_count))
    return NULL;
  return rcontext_create(thd, ret_value, &defs, true);
}


/**
  Execute trigger stored program.

  - changes security context for triggers
  - switch to new memroot
  - call sp_head::execute
  - restore old memroot
  - restores security context

  @param thd               Thread handle
  @param db                database name
  @param table             table name
  @param grant_info        GRANT_INFO structure to be filled with
                           information about definer's privileges
                           on subject table

  @todo
    - TODO: we should create sp_rcontext once per command and reuse it
    on subsequent executions of a trigger.

  @retval
    FALSE  on success
  @retval
    TRUE   on error
*/

bool
sp_head::execute_trigger(THD *thd,
                         const LEX_CSTRING *db_name,
                         const LEX_CSTRING *table_name,
                         GRANT_INFO *grant_info)
{
  sp_rcontext *octx = thd->spcont;
  sp_rcontext *nctx = NULL;
  bool err_status= FALSE;
  MEM_ROOT call_mem_root;
  Query_arena call_arena(&call_mem_root, Query_arena::STMT_INITIALIZED_FOR_SP);
  Query_arena backup_arena;
  DBUG_ENTER("sp_head::execute_trigger");
  DBUG_PRINT("info", ("trigger %s", m_name.str));

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *save_ctx= NULL;


  if (suid() != SP_IS_NOT_SUID &&
      m_security_ctx.change_security_context(thd,
                                             &m_definer.user,
                                             &m_definer.host,
                                             &m_db,
                                             &save_ctx))
    DBUG_RETURN(TRUE);

  /*
    Fetch information about table-level privileges for subject table into
    GRANT_INFO instance. The access check itself will happen in
    Item_trigger_field, where this information will be used along with
    information about column-level privileges.
  */

  fill_effective_table_privileges(thd,
                                  grant_info,
                                  db_name->str,
                                  table_name->str);

  /* Check that the definer has TRIGGER privilege on the subject table. */

  if (!(grant_info->privilege & TRIGGER_ACL))
  {
    char priv_desc[128];
    get_privilege_desc(priv_desc, sizeof(priv_desc), TRIGGER_ACL);

    my_error(ER_TABLEACCESS_DENIED_ERROR, MYF(0), priv_desc,
             thd->security_ctx->priv_user, thd->security_ctx->host_or_ip,
             table_name->str);

    m_security_ctx.restore_security_context(thd, save_ctx);
    DBUG_RETURN(TRUE);
  }
#endif // NO_EMBEDDED_ACCESS_CHECKS

  /*
    Prepare arena and memroot for objects which lifetime is whole
    duration of trigger call (sp_rcontext, it's tables and items,
    sp_cursor and Item_cache holders for case expressions).  We can't
    use caller's arena/memroot for those objects because in this case
    some fixed amount of memory will be consumed for each trigger
    invocation and so statements which involve lot of them will hog
    memory.

    TODO: we should create sp_rcontext once per command and reuse it
    on subsequent executions of a trigger.
  */
  init_sql_alloc(key_memory_sp_head_call_root,
                 &call_mem_root, MEM_ROOT_BLOCK_SIZE, 0, MYF(0));
  thd->set_n_backup_active_arena(&call_arena, &backup_arena);

  Row_definition_list defs;
  m_pcont->retrieve_field_definitions(&defs);
  if (!(nctx= rcontext_create(thd, NULL, &defs, false)))
  {
    err_status= TRUE;
    goto err_with_cleanup;
  }

  thd->spcont= nctx;

  MYSQL_RUN_SP(this, err_status= execute(thd, FALSE));

err_with_cleanup:
  thd->restore_active_arena(&call_arena, &backup_arena);

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  m_security_ctx.restore_security_context(thd, save_ctx);
#endif // NO_EMBEDDED_ACCESS_CHECKS

  delete nctx;
  call_arena.free_items();
  free_root(&call_mem_root, MYF(0));
  thd->spcont= octx;

  if (thd->killed)
    thd->send_kill_message();

  DBUG_RETURN(err_status);
}


/*
  Execute the package initialization section.
*/

bool sp_package::instantiate_if_needed(THD *thd)
{
  List<Item> args;
  if (m_is_instantiated)
    return false;
  /*
    Set m_is_instantiated to true early, to avoid recursion in case if
    the package initialization section calls routines from the same package.
  */
  m_is_instantiated= true;
  /*
    Check that the initialization section doesn't contain Dynamic SQL
    and doesn't return result sets: such stored procedures can't
    be called from a function or trigger.
  */
  if (thd->in_sub_stmt)
  {
    const char *where= (thd->in_sub_stmt & SUB_STMT_TRIGGER ?
                        "trigger" : "function");
    if (is_not_allowed_in_function(where))
      goto err;
  }

  args.elements= 0;
  if (execute_procedure(thd, &args))
    goto err;
  return false;
err:
  m_is_instantiated= false;
  return true;
}


/**
  Execute a function.

   - evaluate parameters
   - changes security context for SUID routines
   - switch to new memroot
   - call sp_head::execute
   - restore old memroot
   - evaluate the return value
   - restores security context

  @param thd               Thread handle
  @param argp              Passed arguments (these are items from containing
                           statement?)
  @param argcount          Number of passed arguments. We need to check if
                           this is correct.
  @param return_value_fld  Save result here.

  @todo
    We should create sp_rcontext once per command and reuse
    it on subsequent executions of a function/trigger.

  @todo
    In future we should associate call arena/mem_root with
    sp_rcontext and allocate all these objects (and sp_rcontext
    itself) on it directly rather than juggle with arenas.

  @retval
    FALSE  on success
  @retval
    TRUE   on error
*/

bool
sp_head::execute_function(THD *thd, Item **argp, uint argcount,
                          Field *return_value_fld, sp_rcontext **func_ctx,
                          Query_arena *call_arena)
{
  ulonglong UNINIT_VAR(binlog_save_options);
  bool need_binlog_call= FALSE;
  uint arg_no;
  sp_rcontext *octx = thd->spcont;
  char buf[STRING_BUFFER_USUAL_SIZE];
  String binlog_buf(buf, sizeof(buf), &my_charset_bin);
  bool err_status= FALSE;
  Query_arena backup_arena;
  DBUG_ENTER("sp_head::execute_function");
  DBUG_PRINT("info", ("function %s", m_name.str));

  if (m_parent && m_parent->instantiate_if_needed(thd))
    DBUG_RETURN(true);

  /*
    Check that the function is called with all specified arguments.

    If it is not, use my_error() to report an error, or it will not terminate
    the invoking query properly.
  */
  if (argcount != m_pcont->context_var_count())
  {
    /*
      Need to use my_error here, or it will not terminate the
      invoking query properly.
    */
    my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0),
             "FUNCTION", ErrConvDQName(this).ptr(),
             m_pcont->context_var_count(), argcount);
    DBUG_RETURN(TRUE);
  }
  /*
    Prepare arena and memroot for objects which lifetime is whole
    duration of function call (sp_rcontext, it's tables and items,
    sp_cursor and Item_cache holders for case expressions).
    We can't use caller's arena/memroot for those objects because
    in this case some fixed amount of memory will be consumed for
    each function/trigger invocation and so statements which involve
    lot of them will hog memory.
    TODO: we should create sp_rcontext once per command and reuse
    it on subsequent executions of a function/trigger.
  */
  if (!(*func_ctx))
  {
    thd->set_n_backup_active_arena(call_arena, &backup_arena);

    if (!(*func_ctx= rcontext_create(thd, return_value_fld, argp, argcount)))
    {
      thd->restore_active_arena(call_arena, &backup_arena);
      err_status= TRUE;
      goto err_with_cleanup;
    }

    /*
      We have to switch temporarily back to callers arena/memroot.
      Function arguments belong to the caller and so the may reference
      memory which they will allocate during calculation long after
      this function call will be finished (e.g. in Item::cleanup()).
    */
    thd->restore_active_arena(call_arena, &backup_arena);
  }

  /* Pass arguments. */
  for (arg_no= 0; arg_no < argcount; arg_no++)
  {
    /* Arguments must be fixed in Item_func_sp::fix_fields */
    DBUG_ASSERT(argp[arg_no]->fixed());

    err_status= bind_input_param(thd, argp[arg_no], arg_no, *func_ctx, TRUE);
    if (err_status)
      goto err_with_cleanup;
  }

  /*
    If row-based binlogging, we don't need to binlog the function's call, let
    each substatement be binlogged its way.
  */
  need_binlog_call= mysql_bin_log.is_open() &&
                    (thd->variables.option_bits & OPTION_BIN_LOG) &&
                    !thd->is_current_stmt_binlog_format_row();

  /*
    Remember the original arguments for unrolled replication of functions
    before they are changed by execution.
  */
  if (need_binlog_call)
  {
    binlog_buf.length(0);
    binlog_buf.append(STRING_WITH_LEN("SELECT "));
    append_identifier(thd, &binlog_buf, &m_db);
    binlog_buf.append('.');
    append_identifier(thd, &binlog_buf, &m_name);
    binlog_buf.append('(');
    for (arg_no= 0; arg_no < argcount; arg_no++)
    {
      String str_value_holder;
      String *str_value;

      if (arg_no)
        binlog_buf.append(',');

      Item_field *item= (*func_ctx)->get_parameter(arg_no);
      str_value= item->type_handler()->print_item_value(thd, item,
                                                        &str_value_holder);
      if (str_value)
        binlog_buf.append(*str_value);
      else
        binlog_buf.append(NULL_clex_str);
    }
    binlog_buf.append(')');
  }
  thd->spcont= *func_ctx;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *save_security_ctx;
  if (set_routine_security_ctx(thd, this, &save_security_ctx))
  {
    err_status= TRUE;
    goto err_with_cleanup;
  }
#endif

  if (need_binlog_call)
  {
    query_id_t q;
    reset_dynamic(&thd->user_var_events);
    /*
      In case of artificially constructed events for function calls
      we have separate union for each such event and hence can't use
      query_id of real calling statement as the start of all these
      unions (this will break logic of replication of user-defined
      variables). So we use artifical value which is guaranteed to
      be greater than all query_id's of all statements belonging
      to previous events/unions.
      Possible alternative to this is logging of all function invocations
      as one select and not resetting THD::user_var_events before
      each invocation.
    */
    q= get_query_id();
    mysql_bin_log.start_union_events(thd, q + 1);
    binlog_save_options= thd->variables.option_bits;
    thd->variables.option_bits&= ~OPTION_BIN_LOG;
  }

  opt_trace_disable_if_no_stored_proc_func_access(thd, this);
  /*
    Switch to call arena/mem_root so objects like sp_cursor or
    Item_cache holders for case expressions can be allocated on it.

    TODO: In future we should associate call arena/mem_root with
          sp_rcontext and allocate all these objects (and sp_rcontext
          itself) on it directly rather than juggle with arenas.
  */
  thd->set_n_backup_active_arena(call_arena, &backup_arena);

  MYSQL_RUN_SP(this, err_status= execute(thd, TRUE));

  thd->restore_active_arena(call_arena, &backup_arena);

  if (need_binlog_call)
  {
    mysql_bin_log.stop_union_events(thd);
    thd->variables.option_bits= binlog_save_options;
    if (thd->binlog_evt_union.unioned_events)
    {
      int errcode = query_error_code(thd, thd->killed == NOT_KILLED);
      Query_log_event qinfo(thd, binlog_buf.ptr(), binlog_buf.length(),
                            thd->binlog_evt_union.unioned_events_trans, FALSE, FALSE, errcode);
      if (mysql_bin_log.write(&qinfo) &&
          thd->binlog_evt_union.unioned_events_trans)
      {
        push_warning(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR,
                     "Invoked ROUTINE modified a transactional table but MySQL "
                     "failed to reflect this change in the binary log");
        err_status= TRUE;
      }
      reset_dynamic(&thd->user_var_events);
      /* Forget those values, in case more function calls are binlogged: */
      thd->stmt_depends_on_first_successful_insert_id_in_prev_stmt= 0;
      thd->auto_inc_intervals_in_cur_stmt_for_binlog.empty();
    }
  }

  if (!err_status && thd->spcont->quit_func)
  {
    /* We need result only in function but not in trigger */

    if (!(*func_ctx)->is_return_value_set())
    {
      my_error(ER_SP_NORETURNEND, MYF(0), m_name.str);
      err_status= TRUE;
    }
    else
    {
      /*
        Copy back all OUT or INOUT values to the previous frame, or
        set global user variables
      */
      for (arg_no= 0; arg_no < argcount; arg_no++)
      {
        err_status= bind_output_param(thd, argp[arg_no], arg_no, octx, *func_ctx);
        if (err_status)
          break;
      }
    }
  }

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  m_security_ctx.restore_security_context(thd, save_security_ctx);
#endif

err_with_cleanup:
  thd->spcont= octx;

  /*
    If not insided a procedure and a function printing warning
    messsages.
  */
  if (need_binlog_call && 
      thd->spcont == NULL && !thd->binlog_evt_union.do_union)
    thd->issue_unsafe_warnings();

  DBUG_RETURN(err_status);
}


/**
  Execute a procedure.

  The function does the following steps:
   - Set all parameters
   - changes security context for SUID routines
   - call sp_head::execute
   - copy back values of INOUT and OUT parameters
   - restores security context

  @param thd    Thread handle
  @param args   List of values passed as arguments.

  @retval
    FALSE  on success
  @retval
    TRUE   on error
*/

bool
sp_head::execute_procedure(THD *thd, List<Item> *args)
{
  bool err_status= FALSE;
  uint params = m_pcont->context_var_count();
  /* Query start time may be reset in a multi-stmt SP; keep this for later. */
  ulonglong utime_before_sp_exec= thd->utime_after_lock;
  sp_rcontext *save_spcont, *octx;
  sp_rcontext *nctx = NULL;
  bool save_enable_slow_log;
  bool save_log_general= false;
  sp_package *pkg= get_package();
  DBUG_ENTER("sp_head::execute_procedure");
  DBUG_PRINT("info", ("procedure %s", m_name.str));

  if (m_parent && m_parent->instantiate_if_needed(thd))
    DBUG_RETURN(true);

  if (args->elements != params)
  {
    my_error(ER_SP_WRONG_NO_OF_ARGS, MYF(0), "PROCEDURE",
             ErrConvDQName(this).ptr(), params, args->elements);
    DBUG_RETURN(TRUE);
  }

  save_spcont= octx= thd->spcont;
  if (! octx)
  {
    /* Create a temporary old context. */
    if (!(octx= rcontext_create(thd, NULL, args)))
    {
      DBUG_PRINT("error", ("Could not create octx"));
      DBUG_RETURN(TRUE);
    }

    thd->spcont= octx;

    /* set callers_arena to thd, for upper-level function to work */
    thd->spcont->callers_arena= thd;
  }

  if (!pkg)
  {
    if (!(nctx= rcontext_create(thd, NULL, args)))
    {
      delete nctx; /* Delete nctx if it was init() that failed. */
      thd->spcont= save_spcont;
      DBUG_RETURN(TRUE);
    }
  }
  else
  {
    if (!pkg->m_rcontext)
    {
      Query_arena backup_arena;
      thd->set_n_backup_active_arena(this, &backup_arena);
      nctx= pkg->rcontext_create(thd, NULL, args);
      thd->restore_active_arena(this, &backup_arena);
      if (!nctx)
      {
        thd->spcont= save_spcont;
        DBUG_RETURN(TRUE);
      }
      pkg->m_rcontext= nctx;
    }
    else
      nctx= pkg->m_rcontext;
  }

  if (params > 0)
  {
    List_iterator<Item> it_args(*args);

    DBUG_PRINT("info",(" %.*s: eval args", (int) m_name.length, m_name.str));

    for (uint i= 0 ; i < params ; i++)
    {
      Item *arg_item= it_args++;

      if (!arg_item)
        break;

      err_status= bind_input_param(thd, arg_item, i, nctx, FALSE);
      if (err_status)
        break;
    }

    /*
      Okay, got values for all arguments. Close tables that might be used by
      arguments evaluation. If arguments evaluation required prelocking mode,
      we'll leave it here.
    */
    thd->lex->unit.cleanup();

    if (!thd->in_sub_stmt)
    {
      thd->get_stmt_da()->set_overwrite_status(true);
      thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd);
      thd->get_stmt_da()->set_overwrite_status(false);
    }

    close_thread_tables(thd);
    thd_proc_info(thd, 0);

    if (! thd->in_sub_stmt)
    {
      if (thd->transaction_rollback_request)
      {
        trans_rollback_implicit(thd);
        thd->release_transactional_locks();
      }
      else if (! thd->in_multi_stmt_transaction_mode())
        thd->release_transactional_locks();
      else
        thd->mdl_context.release_statement_locks();
    }

    thd->rollback_item_tree_changes();

    DBUG_PRINT("info",(" %.*s: eval args done", (int) m_name.length, 
                       m_name.str));
  }

  save_enable_slow_log= thd->enable_slow_log;

  /*
    Disable slow log if:
    - Slow logging is enabled (no change needed)
    - This is a normal SP (not event log)
    - If we have not explicitely disabled logging of SP
  */
  if (save_enable_slow_log &&
      ((!(m_flags & LOG_SLOW_STATEMENTS) &&
        (thd->variables.log_slow_disabled_statements & LOG_SLOW_DISABLE_SP))))
  {
    DBUG_PRINT("info", ("Disabling slow log for the execution"));
    thd->enable_slow_log= FALSE;
  }

  /*
    Disable general log if:
    - If general log is enabled (no change needed)
    - This is a normal SP (not event log)
    - If we have not explicitely disabled logging of SP
  */
  if (!(thd->variables.option_bits & OPTION_LOG_OFF) &&
      (!(m_flags & LOG_GENERAL_LOG) &&
       (thd->variables.log_disabled_statements & LOG_DISABLE_SP)))
  {
    DBUG_PRINT("info", ("Disabling general log for the execution"));
    save_log_general= true;
    /* disable this bit */
    thd->variables.option_bits |= OPTION_LOG_OFF;
  }
  thd->spcont= nctx;

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  Security_context *save_security_ctx= 0;
  if (!err_status)
    err_status= set_routine_security_ctx(thd, this, &save_security_ctx);
#endif

  opt_trace_disable_if_no_stored_proc_func_access(thd, this);

  if (!err_status)
    MYSQL_RUN_SP(this, err_status= execute(thd, TRUE));

  if (save_log_general)
    thd->variables.option_bits &= ~OPTION_LOG_OFF;
  thd->enable_slow_log= save_enable_slow_log;

  /*
    In the case when we weren't able to employ reuse mechanism for
    OUT/INOUT paranmeters, we should reallocate memory. This
    allocation should be done on the arena which will live through
    all execution of calling routine.
  */
  thd->spcont->callers_arena= octx->callers_arena;

  if (!err_status && params > 0)
  {
    List_iterator<Item> it_args(*args);

    /*
      Copy back all OUT or INOUT values to the previous frame, or
      set global user variables
    */
    for (uint i= 0 ; i < params ; i++)
    {
      Item *arg_item= it_args++;

      if (!arg_item)
        break;

      err_status= bind_output_param(thd, arg_item, i, octx, nctx);
      if (err_status)
        break;
    }
  }

#ifndef NO_EMBEDDED_ACCESS_CHECKS
  if (save_security_ctx)
    m_security_ctx.restore_security_context(thd, save_security_ctx);
#endif

  if (!save_spcont)
    delete octx;

  if (!pkg)
    delete nctx;
  thd->spcont= save_spcont;
  thd->utime_after_lock= utime_before_sp_exec;

  /*
    If not insided a procedure and a function printing warning
    messsages.
  */ 
  bool need_binlog_call= mysql_bin_log.is_open() &&
                         (thd->variables.option_bits & OPTION_BIN_LOG) &&
                         !thd->is_current_stmt_binlog_format_row();
  if (need_binlog_call && thd->spcont == NULL &&
      !thd->binlog_evt_union.do_union)
    thd->issue_unsafe_warnings();

  DBUG_RETURN(err_status);
}

bool
sp_head::bind_input_param(THD *thd,
                          Item *arg_item,
                          uint arg_no,
                          sp_rcontext *nctx,
                          bool is_function)
{
  DBUG_ENTER("sp_head::bind_input_param");

  sp_variable *spvar= m_pcont->find_variable(arg_no);
  if (!spvar)
    DBUG_RETURN(FALSE);

  if (spvar->mode != sp_variable::MODE_IN)
  {
    Settable_routine_parameter *srp=
      arg_item->get_settable_routine_parameter();

    if (!srp)
    {
      my_error(ER_SP_NOT_VAR_ARG, MYF(0), arg_no+1, ErrConvDQName(this).ptr());
      DBUG_RETURN(TRUE);
    }

    if (is_function)
    {
      /*
        Check if the function is called from SELECT/INSERT/UPDATE/DELETE query
        and parameter is OUT or INOUT.
        If yes, it is an invalid call - throw error.
      */
      if (thd->lex->sql_command == SQLCOM_SELECT || 
          thd->lex->sql_command == SQLCOM_INSERT ||
          thd->lex->sql_command == SQLCOM_INSERT_SELECT ||
          thd->lex->sql_command == SQLCOM_UPDATE ||
          thd->lex->sql_command == SQLCOM_DELETE)
      {
        my_error(ER_SF_OUT_INOUT_ARG_NOT_ALLOWED, MYF(0), arg_no+1, m_name.str);
        DBUG_RETURN(TRUE);
      }
    }

    srp->set_required_privilege(spvar->mode == sp_variable::MODE_INOUT);
  }

  if (spvar->mode == sp_variable::MODE_OUT)
  {
    Item_null *null_item= new (thd->mem_root) Item_null(thd);
    Item *tmp_item= null_item;

    if (!null_item ||
        nctx->set_parameter(thd, arg_no, &tmp_item))
    {
      DBUG_PRINT("error", ("set variable failed"));
      DBUG_RETURN(TRUE);
    }
  }
  else
  {
    if (nctx->set_parameter(thd, arg_no, &arg_item))
    {
      DBUG_PRINT("error", ("set variable 2 failed"));
      DBUG_RETURN(TRUE);
    }
  }

  TRANSACT_TRACKER(add_trx_state_from_thd(thd));

  DBUG_RETURN(FALSE);
}

bool
sp_head::bind_output_param(THD *thd,
                           Item *arg_item,
                           uint arg_no,
                           sp_rcontext *octx,
                           sp_rcontext *nctx)
{
  DBUG_ENTER("sp_head::bind_output_param");

  sp_variable *spvar= m_pcont->find_variable(arg_no);
  if (spvar->mode == sp_variable::MODE_IN)
    DBUG_RETURN(FALSE);

  Settable_routine_parameter *srp=
    arg_item->get_settable_routine_parameter();

  DBUG_ASSERT(srp);

  if (srp->set_value(thd, octx, nctx->get_variable_addr(arg_no)))
  {
    DBUG_PRINT("error", ("set value failed"));
    DBUG_RETURN(TRUE);
  }

  Send_field *out_param_info= new (thd->mem_root) Send_field(thd, nctx->get_parameter(arg_no));
  out_param_info->db_name= m_db;
  out_param_info->table_name= m_name;
  out_param_info->org_table_name= m_name;
  out_param_info->col_name= spvar->name;
  out_param_info->org_col_name= spvar->name;

  srp->set_out_param_info(out_param_info);

  DBUG_RETURN(FALSE);
}

/**
  Reset lex during parsing, before we parse a sub statement.

  @param thd Thread handler.

  @return Error state
    @retval true An error occurred.
    @retval false Success.
*/

bool
sp_head::reset_lex(THD *thd, sp_lex_local *sublex)
{
  DBUG_ENTER("sp_head::reset_lex");
  LEX *oldlex= thd->lex;

  thd->set_local_lex(sublex);

  DBUG_RETURN(m_lex.push_front(oldlex));
}


bool
sp_head::reset_lex(THD *thd)
{
  DBUG_ENTER("sp_head::reset_lex");
  sp_lex_local *sublex= new (thd->mem_root) sp_lex_local(thd, thd->lex);
  DBUG_RETURN(sublex ? reset_lex(thd, sublex) : true);
}


/**
  Restore lex during parsing, after we have parsed a sub statement.

  @param thd Thread handle
  @param oldlex The upper level lex we're near to restore to
  @param sublex The local lex we're near to restore from

  @return
    @retval TRUE failure
    @retval FALSE success
*/

bool
sp_head::merge_lex(THD *thd, LEX *oldlex, LEX *sublex)
{
  DBUG_ENTER("sp_head::merge_lex");

  sublex->set_trg_event_type_for_tables();

  oldlex->trg_table_fields.push_back(&sublex->trg_table_fields);

  /* If this substatement is unsafe, the entire routine is too. */
  DBUG_PRINT("info", ("sublex->get_stmt_unsafe_flags: 0x%x",
                      sublex->get_stmt_unsafe_flags()));
  unsafe_flags|= sublex->get_stmt_unsafe_flags();

  /*
    Add routines which are used by statement to respective set for
    this routine.
  */
  if (sp_update_sp_used_routines(&m_sroutines, &sublex->sroutines))
    DBUG_RETURN(TRUE);

  /* If this substatement is a update query, then mark MODIFIES_DATA */
  if (is_update_query(sublex->sql_command))
    m_flags|= MODIFIES_DATA;

  /*
    Merge tables used by this statement (but not by its functions or
    procedures) to multiset of tables used by this routine.
  */
  merge_table_list(thd, sublex->query_tables, sublex);
  /* Merge lists of PS parameters. */
  oldlex->param_list.append(&sublex->param_list);

  DBUG_RETURN(FALSE);
}

/**
  Put the instruction on the backpatch list, associated with the label.
*/

int
sp_head::push_backpatch(THD *thd, sp_instr *i, sp_label *lab,
                        List<bp_t> *list, backpatch_instr_type itype)
{
  bp_t *bp= (bp_t *) thd->alloc(sizeof(bp_t));

  if (!bp)
    return 1;
  bp->lab= lab;
  bp->instr= i;
  bp->instr_type= itype;
  return list->push_front(bp);
}

int
sp_head::push_backpatch(THD *thd, sp_instr *i, sp_label *lab)
{
  return push_backpatch(thd, i, lab, &m_backpatch, GOTO);
}

int
sp_head::push_backpatch_goto(THD *thd, sp_pcontext *ctx, sp_label *lab)
{
  uint ip= instructions();

  /*
    Add cpop/hpop : they will be removed or updated later if target is in
    the same block or not
  */
  sp_instr_hpop *hpop= new (thd->mem_root) sp_instr_hpop(ip++, ctx, 0);
  if (hpop == NULL || add_instr(hpop))
    return true;
  if (push_backpatch(thd, hpop, lab, &m_backpatch_goto, HPOP))
    return true;

  sp_instr_cpop *cpop= new (thd->mem_root) sp_instr_cpop(ip++, ctx, 0);
  if (cpop == NULL || add_instr(cpop))
    return true;
  if (push_backpatch(thd, cpop, lab, &m_backpatch_goto, CPOP))
    return true;

  // Add jump with ip=0. IP will be updated when label is found.
  sp_instr_jump *i= new (thd->mem_root) sp_instr_jump(ip, ctx);
  if (i == NULL || add_instr(i))
    return true;
  if (push_backpatch(thd, i, lab, &m_backpatch_goto, GOTO))
    return true;

  return false;
}

/**
  Update all instruction with this label in the backpatch list to
  the current position.
*/

void
sp_head::backpatch(sp_label *lab)
{
  bp_t *bp;
  uint dest= instructions();
  List_iterator_fast<bp_t> li(m_backpatch);

  DBUG_ENTER("sp_head::backpatch");
  while ((bp= li++))
  {
    if (bp->lab == lab)
    {
      DBUG_PRINT("info", ("backpatch: (m_ip %d, label %p <%s>) to dest %d",
                          bp->instr->m_ip, lab, lab->name.str, dest));
      bp->instr->backpatch(dest, lab->ctx);
    }
  }
  DBUG_VOID_RETURN;
}

void
sp_head::backpatch_goto(THD *thd, sp_label *lab,sp_label *lab_begin_block)
{
  bp_t *bp;
  uint dest= instructions();
  List_iterator<bp_t> li(m_backpatch_goto);

  DBUG_ENTER("sp_head::backpatch_goto");
  while ((bp= li++))
  {
    if (bp->instr->m_ip < lab_begin_block->ip || bp->instr->m_ip > lab->ip)
    {
      /*
        Update only jump target from the beginning of the block where the
        label is defined.
      */
      continue;
    }
    if (lex_string_cmp(system_charset_info, &bp->lab->name, &lab->name) == 0)
    {
      if (bp->instr_type == GOTO)
      {
        DBUG_PRINT("info",
                   ("backpatch_goto: (m_ip %d, label %p <%s>) to dest %d",
                    bp->instr->m_ip, lab, lab->name.str, dest));
        bp->instr->backpatch(dest, lab->ctx);
        // Jump resolved, remove from the list
        li.remove();
        continue;
      }
      if (bp->instr_type == CPOP)
      {
        uint n= bp->instr->m_ctx->diff_cursors(lab_begin_block->ctx, true);
        if (n == 0)
        {
          // Remove cpop instr
          replace_instr_to_nop(thd,bp->instr->m_ip);
        }
        else
        {
          // update count of cpop
          static_cast<sp_instr_cpop*>(bp->instr)->update_count(n);
          n= 1;
        }
        li.remove();
        continue;
      }
      if (bp->instr_type == HPOP)
      {
        uint n= bp->instr->m_ctx->diff_handlers(lab_begin_block->ctx, true);
        if (n == 0)
        {
          // Remove hpop instr
          replace_instr_to_nop(thd,bp->instr->m_ip);
        }
        else
        {
          // update count of cpop
          static_cast<sp_instr_hpop*>(bp->instr)->update_count(n);
          n= 1;
        }
        li.remove();
        continue;
      }
    }
  }
  DBUG_VOID_RETURN;
}

bool
sp_head::check_unresolved_goto()
{
  DBUG_ENTER("sp_head::check_unresolved_goto");
  bool has_unresolved_label=false;
  if (m_backpatch_goto.elements > 0)
  {
    List_iterator_fast<bp_t> li(m_backpatch_goto);
    while (bp_t* bp= li++)
    {
      if (bp->instr_type == GOTO)
      {
        my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "GOTO", bp->lab->name.str);
        has_unresolved_label=true;
      }
    }
  }
  DBUG_RETURN(has_unresolved_label);
}

int
sp_head::new_cont_backpatch(sp_instr_opt_meta *i)
{
  m_cont_level+= 1;
  if (i)
  {
    /* Use the cont. destination slot to store the level */
    i->m_cont_dest= m_cont_level;
    if (m_cont_backpatch.push_front(i))
      return 1;
  }
  return 0;
}

int
sp_head::add_cont_backpatch(sp_instr_opt_meta *i)
{
  i->m_cont_dest= m_cont_level;
  return m_cont_backpatch.push_front(i);
}

void
sp_head::do_cont_backpatch()
{
  uint dest= instructions();
  uint lev= m_cont_level--;
  sp_instr_opt_meta *i;

  while ((i= m_cont_backpatch.head()) && i->m_cont_dest == lev)
  {
    i->m_cont_dest= dest;
    (void)m_cont_backpatch.pop();
  }
}


bool
sp_head::sp_add_instr_cpush_for_cursors(THD *thd, sp_pcontext *pcontext)
{
  for (uint i= 0; i < pcontext->frame_cursor_count(); i++)
  {
    const sp_pcursor *c= pcontext->get_cursor_by_local_frame_offset(i);
    sp_instr_cpush *instr= new (thd->mem_root)
                             sp_instr_cpush(instructions(), pcontext, c->lex(),
                                            pcontext->cursor_offset() + i);
    if (instr == NULL || add_instr(instr))
      return true;
  }
  return false;
}


void
sp_head::set_chistics(const st_sp_chistics &chistics)
{
  m_chistics.set(chistics);
  if (m_chistics.comment.length == 0)
    m_chistics.comment.str= 0;
  else
    m_chistics.comment.str= strmake_root(mem_root,
                                         m_chistics.comment.str,
                                         m_chistics.comment.length);
}


void
sp_head::set_c_chistics(const st_sp_chistics &chistics)
{
  // Set all chistics but preserve agg_type.
  enum_sp_aggregate_type save_agg_type= agg_type();
  set_chistics(chistics);
  set_chistics_agg_type(save_agg_type);
}


void
sp_head::set_info(longlong created, longlong modified,
                  const st_sp_chistics &chistics, sql_mode_t sql_mode)
{
  m_created= created;
  m_modified= modified;
  set_chistics(chistics);
  m_sql_mode= sql_mode;
}


void
sp_head::reset_thd_mem_root(THD *thd)
{
  DBUG_ENTER("sp_head::reset_thd_mem_root");
  m_thd_root= thd->mem_root;
  thd->mem_root= &main_mem_root;
  DBUG_PRINT("info", ("mem_root %p moved to thd mem root %p",
                      &mem_root, &thd->mem_root));
  free_list= thd->free_list; // Keep the old list
  thd->free_list= NULL; // Start a new one
  m_thd= thd;
  DBUG_VOID_RETURN;
}

void
sp_head::restore_thd_mem_root(THD *thd)
{
  DBUG_ENTER("sp_head::restore_thd_mem_root");

  /*
   In some cases our parser detects a syntax error and calls
   LEX::cleanup_lex_after_parse_error() method only after
   finishing parsing the whole routine. In such a situation
   sp_head::restore_thd_mem_root() will be called twice - the
   first time as part of normal parsing process and the second
   time by cleanup_lex_after_parse_error().
   To avoid ruining active arena/mem_root state in this case we
   skip restoration of old arena/mem_root if this method has been
   already called for this routine.
  */
  if (!m_thd)
    DBUG_VOID_RETURN;

  Item *flist= free_list;	// The old list
  set_query_arena(thd);         // Get new free_list and mem_root
  state= STMT_INITIALIZED_FOR_SP;

  DBUG_PRINT("info", ("mem_root %p returned from thd mem root %p",
                      &mem_root, &thd->mem_root));
  thd->free_list= flist;        // Restore the old one
  thd->mem_root= m_thd_root;
  m_thd= NULL;
  DBUG_VOID_RETURN;
}


/**
  Check if a user has access right to a routine.

  @param thd          Thread handler
  @param sp           SP
  @param full_access  Set to 1 if the user has SELECT right to the
                      'mysql.proc' able or is the owner of the routine
  @retval
    false ok
  @retval
    true  error
*/

bool check_show_routine_access(THD *thd, sp_head *sp, bool *full_access)
{
  TABLE_LIST tables;
  bzero((char*) &tables,sizeof(tables));
  tables.db= MYSQL_SCHEMA_NAME;
  tables.table_name= MYSQL_PROC_NAME;
  tables.alias= MYSQL_PROC_NAME;

  *full_access= ((!check_table_access(thd, SELECT_ACL, &tables, FALSE,
                                     1, TRUE) &&
                  (tables.grant.privilege & SELECT_ACL) != NO_ACL) ||
                 /* Check if user owns the routine. */
                 (!strcmp(sp->m_definer.user.str,
                          thd->security_ctx->priv_user) &&
                  !strcmp(sp->m_definer.host.str,
                          thd->security_ctx->priv_host)) ||
                 /* Check if current role or any of the sub-granted roles
                    own the routine. */
                 (sp->m_definer.host.length == 0 &&
                  (!strcmp(sp->m_definer.user.str,
                           thd->security_ctx->priv_role) ||
                   check_role_is_granted(thd->security_ctx->priv_role, NULL,
                                         sp->m_definer.user.str))));
  if (!*full_access)
    return check_some_routine_access(thd, sp->m_db.str, sp->m_name.str,
                                     sp->m_handler);
  return 0;
}


/**
  Collect metadata for SHOW CREATE statement for stored routines.

  @param thd  Thread context.
  @param sph          Stored routine handler
  @param fields       Item list to populate

  @return Error status.
    @retval FALSE on success
    @retval TRUE on error
*/

void
sp_head::show_create_routine_get_fields(THD *thd, const Sp_handler *sph,
                                                  List<Item> *fields)
{
  const char *col1_caption= sph->show_create_routine_col1_caption();
  const char *col3_caption= sph->show_create_routine_col3_caption();

  MEM_ROOT *mem_root= thd->mem_root;

  /* Send header. */

  fields->push_back(new (mem_root)
                    Item_empty_string(thd, col1_caption, NAME_CHAR_LEN),
                    mem_root);
  fields->push_back(new (mem_root)
                    Item_empty_string(thd, "sql_mode", 256),
                    mem_root);

  {
    /*
      NOTE: SQL statement field must be not less than 1024 in order not to
      confuse old clients.
    */

    Item_empty_string *stmt_fld=
      new (mem_root) Item_empty_string(thd, col3_caption, 1024);
    stmt_fld->set_maybe_null();

    fields->push_back(stmt_fld, mem_root);
  }

  fields->push_back(new (mem_root)
                   Item_empty_string(thd, "character_set_client",
                                     MY_CS_NAME_SIZE),
                   mem_root);

  fields->push_back(new (mem_root)
                   Item_empty_string(thd, "collation_connection",
                                     MY_CS_NAME_SIZE),
                   mem_root);

  fields->push_back(new (mem_root)
                   Item_empty_string(thd, "Database Collation",
                                     MY_CS_NAME_SIZE),
                   mem_root);
}


/**
  Implement SHOW CREATE statement for stored routines.

  @param thd  Thread context.
  @param sph  Stored routine handler

  @return Error status.
    @retval FALSE on success
    @retval TRUE on error
*/

bool
sp_head::show_create_routine(THD *thd, const Sp_handler *sph)
{
  const char *col1_caption= sph->show_create_routine_col1_caption();
  const char *col3_caption= sph->show_create_routine_col3_caption();

  bool err_status;

  Protocol *protocol= thd->protocol;
  List<Item> fields;

  LEX_CSTRING sql_mode;

  bool full_access;
  MEM_ROOT *mem_root= thd->mem_root;

  DBUG_ENTER("sp_head::show_create_routine");
  DBUG_PRINT("info", ("routine %s", m_name.str));

  if (check_show_routine_access(thd, this, &full_access))
    DBUG_RETURN(TRUE);

  sql_mode_string_representation(thd, m_sql_mode, &sql_mode);

  /* Send header. */

  fields.push_back(new (mem_root)
                   Item_empty_string(thd, col1_caption, NAME_CHAR_LEN),
                   thd->mem_root);
  fields.push_back(new (mem_root)
                   Item_empty_string(thd, "sql_mode", (uint)sql_mode.length),
                   thd->mem_root);

  {
    /*
      NOTE: SQL statement field must be not less than 1024 in order not to
      confuse old clients.
    */

    Item_empty_string *stmt_fld=
      new (mem_root) Item_empty_string(thd, col3_caption,
                            (uint)MY_MAX(m_defstr.length, 1024));

    stmt_fld->set_maybe_null();

    fields.push_back(stmt_fld, thd->mem_root);
  }

  fields.push_back(new (mem_root)
                   Item_empty_string(thd, "character_set_client",
                                     MY_CS_NAME_SIZE),
                   thd->mem_root);

  fields.push_back(new (mem_root)
                   Item_empty_string(thd, "collation_connection",
                                     MY_CS_NAME_SIZE),
                   thd->mem_root);

  fields.push_back(new (mem_root)
                   Item_empty_string(thd, "Database Collation",
                                     MY_CS_NAME_SIZE),
                   thd->mem_root);

  if (protocol->send_result_set_metadata(&fields,
                            Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF))
  {
    DBUG_RETURN(TRUE);
  }

  /* Send data. */

  protocol->prepare_for_resend();

  protocol->store(m_name.str, m_name.length, system_charset_info);
  protocol->store(sql_mode.str, sql_mode.length, system_charset_info);

  if (full_access)
    protocol->store(m_defstr.str, m_defstr.length,
                    m_creation_ctx->get_client_cs());
  else
    protocol->store_null();


  protocol->store(&m_creation_ctx->get_client_cs()->cs_name,
                  system_charset_info);
  protocol->store(&m_creation_ctx->get_connection_cl()->coll_name,
                  system_charset_info);
  protocol->store(&m_creation_ctx->get_db_cl()->coll_name,
                  system_charset_info);

  err_status= protocol->write();

  if (!err_status)
    my_eof(thd);

  DBUG_RETURN(err_status);
}


/**
  Add instruction to SP.

  @param instr   Instruction
*/

int sp_head::add_instr(sp_instr *instr)
{
  instr->free_list= m_thd->free_list;
  m_thd->free_list= 0;
  /*
    Memory root of every instruction is designated for permanent
    transformations (optimizations) made on the parsed tree during
    the first execution. It points to the memory root of the
    entire stored procedure, as their life span is equal.
  */
  instr->mem_root= &main_mem_root;
  instr->m_lineno= m_thd->m_parser_state->m_lip.yylineno;
  return insert_dynamic(&m_instr, (uchar*)&instr);
}


bool sp_head::add_instr_jump(THD *thd, sp_pcontext *spcont)
{
  sp_instr_jump *i= new (thd->mem_root) sp_instr_jump(instructions(), spcont);
  return i == NULL || add_instr(i);
}


bool sp_head::add_instr_jump(THD *thd, sp_pcontext *spcont, uint dest)
{
  sp_instr_jump *i= new (thd->mem_root) sp_instr_jump(instructions(),
                                                      spcont, dest);
  return i == NULL || add_instr(i);
}


bool sp_head::add_instr_jump_forward_with_backpatch(THD *thd,
                                                    sp_pcontext *spcont,
                                                    sp_label *lab)
{
  sp_instr_jump  *i= new (thd->mem_root) sp_instr_jump(instructions(), spcont);
  if (i == NULL || add_instr(i))
    return true;
  push_backpatch(thd, i, lab);
  return false;
}


bool sp_head::add_instr_freturn(THD *thd, sp_pcontext *spcont,
                                Item *item, LEX *lex)
{
  sp_instr_freturn *i= new (thd->mem_root)
                       sp_instr_freturn(instructions(), spcont, item,
                       m_return_field_def.type_handler(), lex);
  if (i == NULL || add_instr(i))
    return true;
  m_flags|= sp_head::HAS_RETURN;
  return false;
}


bool sp_head::add_instr_preturn(THD *thd, sp_pcontext *spcont)
{
  sp_instr_preturn *i= new (thd->mem_root)
                       sp_instr_preturn(instructions(), spcont);
  if (i == NULL || add_instr(i))
    return true;
  return false;
}


/*
  Replace an instruction at position to "no operation".

  @param thd - use mem_root of this THD for "new".
  @param ip  - position of the operation
  @returns   - true on error, false on success

  When we need to remove an instruction that during compilation
  appeared to be useless (typically as useless jump), we replace
  it to a jump to exactly the next instruction.
  Such jumps are later removed during sp_head::optimize().

  QQ: Perhaps we need a dedicated sp_instr_nop for this purpose.
*/

bool sp_head::replace_instr_to_nop(THD *thd, uint ip)
{
  sp_instr *instr= get_instr(ip);
  sp_instr_jump *nop= new (thd->mem_root) sp_instr_jump(instr->m_ip,
                                                        instr->m_ctx,
                                                        instr->m_ip + 1);
  if (!nop)
    return true;
  delete instr;
  set_dynamic(&m_instr, (uchar *) &nop, ip);
  return false;
}


/**
  Do some minimal optimization of the code:
    -# Mark used instructions
    -# While doing this, shortcut jumps to jump instructions
    -# Compact the code, removing unused instructions.

  This is the main mark and move loop; it relies on the following methods
  in sp_instr and its subclasses:

    - opt_mark()         :  Mark instruction as reachable
    - opt_shortcut_jump():  Shortcut jumps to the final destination;
                           used by opt_mark().
    - opt_move()         :  Update moved instruction
    - set_destination()  :  Set the new destination (jump instructions only)
*/

void sp_head::optimize()
{
  List<sp_instr> bp;
  sp_instr *i;
  uint src, dst;

  DBUG_EXECUTE_IF("sp_head_optimize_disable", return; );

  opt_mark();

  bp.empty();
  src= dst= 0;
  while ((i= get_instr(src)))
  {
    if (! i->marked)
    {
      delete i;
      src+= 1;
    }
    else
    {
      if (src != dst)
      {
        /* Move the instruction and update prev. jumps */
        sp_instr *ibp;
        List_iterator_fast<sp_instr> li(bp);

        set_dynamic(&m_instr, (uchar*)&i, dst);
        while ((ibp= li++))
        {
          sp_instr_opt_meta *im= static_cast<sp_instr_opt_meta *>(ibp);
          im->set_destination(src, dst);
        }
      }
      i->opt_move(dst, &bp);
      src+= 1;
      dst+= 1;
    }
  }
  m_instr.elements= dst;
  bp.empty();
}

void sp_head::add_mark_lead(uint ip, List<sp_instr> *leads)
{
  sp_instr *i= get_instr(ip);

  if (i && ! i->marked)
    leads->push_front(i);
}

void
sp_head::opt_mark()
{
  uint ip;
  sp_instr *i;
  List<sp_instr> leads;

  /*
    Forward flow analysis algorithm in the instruction graph:
    - first, add the entry point in the graph (the first instruction) to the
      'leads' list of paths to explore.
    - while there are still leads to explore:
      - pick one lead, and follow the path forward. Mark instruction reached.
        Stop only if the end of the routine is reached, or the path converge
        to code already explored (marked).
      - while following a path, collect in the 'leads' list any fork to
        another path (caused by conditional jumps instructions), so that these
        paths can be explored as well.
  */

  /* Add the entry point */
  i= get_instr(0);
  leads.push_front(i);

  /* For each path of code ... */
  while (leads.elements != 0)
  {
    i= leads.pop();

    /* Mark the entire path, collecting new leads. */
    while (i && ! i->marked)
    {
      ip= i->opt_mark(this, & leads);
      i= get_instr(ip);
    }
  }
}


#ifndef DBUG_OFF
/**
  Return the routine instructions as a result set.
  @return
    0 if ok, !=0 on error.
*/

int
sp_head::show_routine_code(THD *thd)
{
  Protocol *protocol= thd->protocol;
  char buff[2048];
  String buffer(buff, sizeof(buff), system_charset_info);
  List<Item> field_list;
  sp_instr *i;
  bool full_access;
  int res= 0;
  uint ip;
  DBUG_ENTER("sp_head::show_routine_code");
  DBUG_PRINT("info", ("procedure: %s", m_name.str));

  if (check_show_routine_access(thd, this, &full_access) || !full_access)
    DBUG_RETURN(1);

  field_list.push_back(new (thd->mem_root) Item_uint(thd, "Pos", 9),
                       thd->mem_root);
  // 1024 is for not to confuse old clients
  field_list.push_back(new (thd->mem_root)
                       Item_empty_string(thd, "Instruction",
                                         MY_MAX(buffer.length(), 1024)),
                       thd->mem_root);
  if (protocol->send_result_set_metadata(&field_list, Protocol::SEND_NUM_ROWS |
                                         Protocol::SEND_EOF))
    DBUG_RETURN(1);

  for (ip= 0; (i = get_instr(ip)) ; ip++)
  {
    /*
      Consistency check. If these are different something went wrong
      during optimization.
    */
    if (ip != i->m_ip)
    {
      const char *format= "Instruction at position %u has m_ip=%u";
      char tmp[sizeof(format) + 2*SP_INSTR_UINT_MAXLEN + 1];

      my_snprintf(tmp, sizeof(tmp), format, ip, i->m_ip);
      /*
        Since this is for debugging purposes only, we don't bother to
        introduce a special error code for it.
      */
      push_warning(thd, Sql_condition::WARN_LEVEL_WARN, ER_UNKNOWN_ERROR, tmp);
    }
    protocol->prepare_for_resend();
    protocol->store_long(ip);

    buffer.set("", 0, system_charset_info);
    i->print(&buffer);
    protocol->store(buffer.ptr(), buffer.length(), system_charset_info);
    if ((res= protocol->write()))
      break;
  }

  if (!res)
    my_eof(thd);

  DBUG_RETURN(res);
}
#endif // ifndef DBUG_OFF


/**
  Prepare LEX and thread for execution of instruction, if requested open
  and lock LEX's tables, execute instruction's core function, perform
  cleanup afterwards.

  @param thd           thread context
  @param nextp         out - next instruction
  @param open_tables   if TRUE then check read access to tables in LEX's table
                       list and open and lock them (used in instructions which
                       need to calculate some expression and don't execute
                       complete statement).
  @param sp_instr      instruction for which we prepare context, and which core
                       function execute by calling its exec_core() method.

  @note
    We are not saving/restoring some parts of THD which may need this because
    we do this once for whole routine execution in sp_head::execute().

  @return
    0/non-0 - Success/Failure
*/

int
sp_lex_keeper::reset_lex_and_exec_core(THD *thd, uint *nextp,
                                       bool open_tables, sp_instr* instr)
{
  int res= 0;
  DBUG_ENTER("reset_lex_and_exec_core");

  /*
    The flag is saved at the entry to the following substatement.
    It's reset further in the common code part.
    It's merged with the saved parent's value at the exit of this func.
  */
  bool parent_modified_non_trans_table=
    thd->transaction->stmt.modified_non_trans_table;
  unsigned int parent_unsafe_rollback_flags=
    thd->transaction->stmt.m_unsafe_rollback_flags;
  thd->transaction->stmt.modified_non_trans_table= FALSE;
  thd->transaction->stmt.m_unsafe_rollback_flags= 0;

  DBUG_ASSERT(!thd->derived_tables);
  DBUG_ASSERT(thd->Item_change_list::is_empty());
  /*
    Use our own lex.
    We should not save old value since it is saved/restored in
    sp_head::execute() when we are entering/leaving routine.
  */
  thd->lex= m_lex;

  thd->set_query_id(next_query_id());

  if (thd->locked_tables_mode <= LTM_LOCK_TABLES)
  {
    /*
      This statement will enter/leave prelocked mode on its own.
      Entering prelocked mode changes table list and related members
      of LEX, so we'll need to restore them.
    */
    if (lex_query_tables_own_last)
    {
      /*
        We've already entered/left prelocked mode with this statement.
        Attach the list of tables that need to be prelocked and mark m_lex
        as having such list attached.
      */
      *lex_query_tables_own_last= prelocking_tables;
      m_lex->mark_as_requiring_prelocking(lex_query_tables_own_last);
    }
  }

  reinit_stmt_before_use(thd, m_lex);

#ifndef EMBEDDED_LIBRARY
  /*
    If there was instruction which changed tracking state,
    the result of changed tracking state send to client in OK packed.
    So it changes result sent to client and probably can be different
    independent on query text. So we can't cache such results.
  */
  if ((thd->client_capabilities & CLIENT_SESSION_TRACK) &&
      (thd->server_status & SERVER_SESSION_STATE_CHANGED))
    thd->lex->safe_to_cache_query= 0;
#endif

  Opt_trace_start ots(thd);
  ots.init(thd, m_lex->query_tables, SQLCOM_SELECT, &m_lex->var_list,
           NULL, 0, thd->variables.character_set_client);

  Json_writer_object trace_command(thd);
  Json_writer_array trace_command_steps(thd, "steps");
  if (open_tables)
    res= instr->exec_open_and_lock_tables(thd, m_lex->query_tables);

  if (likely(!res))
  {
    res= instr->exec_core(thd, nextp);
    DBUG_PRINT("info",("exec_core returned: %d", res));
  }

  /*
    Call after unit->cleanup() to close open table
    key read.
  */
  if (open_tables)
  {
    m_lex->unit.cleanup();
    /* Here we also commit or rollback the current statement. */
    if (! thd->in_sub_stmt)
    {
      thd->get_stmt_da()->set_overwrite_status(true);
      thd->is_error() ? trans_rollback_stmt(thd) : trans_commit_stmt(thd);
      thd->get_stmt_da()->set_overwrite_status(false);
    }
    close_thread_tables(thd);
    thd_proc_info(thd, 0);

    if (! thd->in_sub_stmt)
    {
      if (thd->transaction_rollback_request)
      {
        trans_rollback_implicit(thd);
        thd->release_transactional_locks();
      }
      else if (! thd->in_multi_stmt_transaction_mode())
        thd->release_transactional_locks();
      else
        thd->mdl_context.release_statement_locks();
    }
  }
  //TODO: why is this here if log_slow_query is in sp_instr_stmt::execute?
  delete_explain_query(m_lex);

  if (m_lex->query_tables_own_last)
  {
    /*
      We've entered and left prelocking mode when executing statement
      stored in m_lex. 
      m_lex->query_tables(->next_global)* list now has a 'tail' - a list
      of tables that are added for prelocking. (If this is the first
      execution, the 'tail' was added by open_tables(), otherwise we've
      attached it above in this function).
      Now we'll save the 'tail', and detach it.
    */
    lex_query_tables_own_last= m_lex->query_tables_own_last;
    prelocking_tables= *lex_query_tables_own_last;
    *lex_query_tables_own_last= NULL;
    m_lex->query_tables_last= m_lex->query_tables_own_last;
    m_lex->mark_as_requiring_prelocking(NULL);
  }
  thd->rollback_item_tree_changes();
  /*
    Update the state of the active arena if no errors on
    open_tables stage.
  */
  if (likely(!res) || likely(!thd->is_error()))
    thd->stmt_arena->state= Query_arena::STMT_EXECUTED;

  /*
    Merge here with the saved parent's values
    what is needed from the substatement gained
  */
  thd->transaction->stmt.modified_non_trans_table |= parent_modified_non_trans_table;
  thd->transaction->stmt.m_unsafe_rollback_flags |= parent_unsafe_rollback_flags;

  TRANSACT_TRACKER(add_trx_state_from_thd(thd));

  /*
    Unlike for PS we should not call Item's destructors for newly created
    items after execution of each instruction in stored routine. This is
    because SP often create Item (like Item_int, Item_string etc...) when
    they want to store some value in local variable, pass return value and
    etc... So their life time should be longer than one instruction.

    cleanup_items() is called in sp_head::execute()
  */
  thd->lex->restore_set_statement_var();
  DBUG_RETURN(res || thd->is_error());
}


int sp_lex_keeper::cursor_reset_lex_and_exec_core(THD *thd, uint *nextp,
                                                  bool open_tables,
                                                  sp_instr *instr)
{
  Query_arena *old_arena= thd->stmt_arena;
  /*
    Get the Query_arena from the cursor statement LEX, which contains
    the free_list of the query, so new items (if any) are stored in
    the right free_list, and we can cleanup after each cursor operation,
    e.g. open or cursor_copy_struct (for cursor%ROWTYPE variables).
  */
  thd->stmt_arena= m_lex->query_arena();
  int res= reset_lex_and_exec_core(thd, nextp, open_tables, instr);
  cleanup_items(thd->stmt_arena->free_list);
  thd->stmt_arena= old_arena;
  return res;
}


/*
  sp_instr class functions
*/

int sp_instr::exec_open_and_lock_tables(THD *thd, TABLE_LIST *tables)
{
  int result;

  /*
    Check whenever we have access to tables for this statement
    and open and lock them before executing instructions core function.
  */
  if (thd->open_temporary_tables(tables) ||
      check_table_access(thd, SELECT_ACL, tables, FALSE, UINT_MAX, FALSE)
      || open_and_lock_tables(thd, tables, TRUE, 0))
    result= -1;
  else
    result= 0;
  /* Prepare all derived tables/views to catch possible errors. */
  if (!result)
    result= mysql_handle_derived(thd->lex, DT_PREPARE) ? -1 : 0;

  return result;
}

uint sp_instr::get_cont_dest() const
{
  return (m_ip+1);
}


int sp_instr::exec_core(THD *thd, uint *nextp)
{
  DBUG_ASSERT(0);
  return 0;
}

/*
  sp_instr_stmt class functions
*/

PSI_statement_info sp_instr_stmt::psi_info=
{ 0, "stmt", 0};

int
sp_instr_stmt::execute(THD *thd, uint *nextp)
{
  int res;
  bool save_enable_slow_log;
  const CSET_STRING query_backup= thd->query_string;
  Sub_statement_state backup_state;
  DBUG_ENTER("sp_instr_stmt::execute");
  DBUG_PRINT("info", ("command: %d", m_lex_keeper.sql_command()));

  MYSQL_SET_STATEMENT_TEXT(thd->m_statement_psi, m_query.str, static_cast<uint>(m_query.length));

#if defined(ENABLED_PROFILING)
  /* This s-p instr is profilable and will be captured. */
  thd->profiling.set_query_source(m_query.str, m_query.length);
#endif

  save_enable_slow_log= thd->enable_slow_log;
  thd->store_slow_query_state(&backup_state);

  if (!(res= alloc_query(thd, m_query.str, m_query.length)) &&
      !(res=subst_spvars(thd, this, &m_query)))
  {
    /*
      (the order of query cache and subst_spvars calls is irrelevant because
      queries with SP vars can't be cached)
    */
    general_log_write(thd, COM_QUERY, thd->query(), thd->query_length());

    if (query_cache_send_result_to_client(thd, thd->query(),
                                          thd->query_length()) <= 0)
    {
      thd->reset_slow_query_state();
      res= m_lex_keeper.reset_lex_and_exec_core(thd, nextp, FALSE, this);
      bool log_slow= !res && thd->enable_slow_log;

      /* Finalize server status flags after executing a statement. */
      if (log_slow || thd->get_stmt_da()->is_eof())
        thd->update_server_status();

      if (thd->get_stmt_da()->is_eof())
        thd->protocol->end_statement();

      query_cache_end_of_result(thd);

      mysql_audit_general(thd, MYSQL_AUDIT_GENERAL_STATUS,
                          thd->get_stmt_da()->is_error() ?
                                 thd->get_stmt_da()->sql_errno() : 0,
                          command_name[COM_QUERY].str);

      if (log_slow)
        log_slow_statement(thd);

      /*
        Restore enable_slow_log, that can be changed by a admin or call
        command
      */
      thd->enable_slow_log= save_enable_slow_log;

      /* Add the number of rows to thd for the 'call' statistics */
      thd->add_slow_query_state(&backup_state);
    }
    else
    {
      /* change statistics */
      enum_sql_command save_sql_command= thd->lex->sql_command;
      thd->lex->sql_command= SQLCOM_SELECT;
      status_var_increment(thd->status_var.com_stat[SQLCOM_SELECT]);
      thd->update_stats();
      thd->lex->sql_command= save_sql_command;
      *nextp= m_ip+1;
    }
    thd->set_query(query_backup);
    thd->query_name_consts= 0;

    if (likely(!thd->is_error()))
    {
      res= 0;
      thd->get_stmt_da()->reset_diagnostics_area();
    }
  }

  DBUG_RETURN(res || thd->is_error());
}


void
sp_instr_stmt::print(String *str)
{
  size_t i, len;

  /* stmt CMD "..." */
  if (str->reserve(SP_STMT_PRINT_MAXLEN+SP_INSTR_UINT_MAXLEN+8))
    return;
  str->qs_append(STRING_WITH_LEN("stmt "));
  str->qs_append((uint)m_lex_keeper.sql_command());
  str->qs_append(STRING_WITH_LEN(" \""));
  len= m_query.length;
  /*
    Print the query string (but not too much of it), just to indicate which
    statement it is.
  */
  if (len > SP_STMT_PRINT_MAXLEN)
    len= SP_STMT_PRINT_MAXLEN-3;
  /* Copy the query string and replace '\n' with ' ' in the process */
  for (i= 0 ; i < len ; i++)
  {
    char c= m_query.str[i];
    if (c == '\n')
      c= ' ';
    str->qs_append(c);
  }
  if (m_query.length > SP_STMT_PRINT_MAXLEN)
    str->qs_append(STRING_WITH_LEN("...")); /* Indicate truncated string */
  str->qs_append('"');
}


int
sp_instr_stmt::exec_core(THD *thd, uint *nextp)
{
  MYSQL_QUERY_EXEC_START(thd->query(),
                         thd->thread_id,
                         thd->get_db(),
                         &thd->security_ctx->priv_user[0],
                         (char *)thd->security_ctx->host_or_ip,
                         3);
  int res= mysql_execute_command(thd);
  MYSQL_QUERY_EXEC_DONE(res);
  *nextp= m_ip+1;
  return res;
}


/*
  sp_instr_set class functions
*/

PSI_statement_info sp_instr_set::psi_info=
{ 0, "set", 0};

int
sp_instr_set::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_set::execute");
  DBUG_PRINT("info", ("offset: %u", m_offset));

  DBUG_RETURN(m_lex_keeper.reset_lex_and_exec_core(thd, nextp, TRUE, this));
}


sp_rcontext *sp_instr_set::get_rcontext(THD *thd) const
{
  return m_rcontext_handler->get_rcontext(thd->spcont);
}


int
sp_instr_set::exec_core(THD *thd, uint *nextp)
{
  int res= get_rcontext(thd)->set_variable(thd, m_offset, &m_value);
  delete_explain_query(thd->lex);
  *nextp = m_ip+1;
  return res;
}

void
sp_instr_set::print(String *str)
{
  /* set name@offset ... */
  size_t rsrv = SP_INSTR_UINT_MAXLEN+6;
  sp_variable *var = m_ctx->find_variable(m_offset);
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();

  /* 'var' should always be non-null, but just in case... */
  if (var)
    rsrv+= var->name.length + prefix->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("set "));
  str->qs_append(prefix->str, prefix->length);
  if (var)
  {
    str->qs_append(&var->name);
    str->qs_append('@');
  }
  str->qs_append(m_offset);
  str->qs_append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


/*
  sp_instr_set_field class functions
*/

int
sp_instr_set_row_field::exec_core(THD *thd, uint *nextp)
{
  int res= get_rcontext(thd)->set_variable_row_field(thd, m_offset,
                                                     m_field_offset,
                                                     &m_value);
  delete_explain_query(thd->lex);
  *nextp= m_ip + 1;
  return res;
}


void
sp_instr_set_row_field::print(String *str)
{
  /* set name@offset[field_offset] ... */
  size_t rsrv= SP_INSTR_UINT_MAXLEN + 6 + 6 + 3;
  sp_variable *var= m_ctx->find_variable(m_offset);
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  DBUG_ASSERT(var);
  DBUG_ASSERT(var->field_def.is_row());
  const Column_definition *def=
    var->field_def.row_field_definitions()->elem(m_field_offset);
  DBUG_ASSERT(def);

  rsrv+= var->name.length + def->field_name.length + prefix->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("set "));
  str->qs_append(prefix);
  str->qs_append(&var->name);
  str->qs_append('.');
  str->qs_append(&def->field_name);
  str->qs_append('@');
  str->qs_append(m_offset);
  str->qs_append('[');
  str->qs_append(m_field_offset);
  str->qs_append(']');
  str->qs_append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


/*
  sp_instr_set_field_by_name class functions
*/

int
sp_instr_set_row_field_by_name::exec_core(THD *thd, uint *nextp)
{
  int res= get_rcontext(thd)->set_variable_row_field_by_name(thd, m_offset,
                                                             m_field_name,
                                                             &m_value);
  delete_explain_query(thd->lex);
  *nextp= m_ip + 1;
  return res;
}


void
sp_instr_set_row_field_by_name::print(String *str)
{
  /* set name.field@offset["field"] ... */
  size_t rsrv= SP_INSTR_UINT_MAXLEN + 6 + 6 + 3 + 2;
  sp_variable *var= m_ctx->find_variable(m_offset);
  const LEX_CSTRING *prefix= m_rcontext_handler->get_name_prefix();
  DBUG_ASSERT(var);
  DBUG_ASSERT(var->field_def.is_table_rowtype_ref() ||
              var->field_def.is_cursor_rowtype_ref());

  rsrv+= var->name.length + 2 * m_field_name.length + prefix->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("set "));
  str->qs_append(prefix);
  str->qs_append(&var->name);
  str->qs_append('.');
  str->qs_append(&m_field_name);
  str->qs_append('@');
  str->qs_append(m_offset);
  str->qs_append("[\"",2);
  str->qs_append(&m_field_name);
  str->qs_append("\"]",2);
  str->qs_append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


/*
  sp_instr_set_trigger_field class functions
*/

PSI_statement_info sp_instr_set_trigger_field::psi_info=
{ 0, "set_trigger_field", 0};

int
sp_instr_set_trigger_field::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_set_trigger_field::execute");
  thd->count_cuted_fields= CHECK_FIELD_ERROR_FOR_NULL;
  DBUG_RETURN(m_lex_keeper.reset_lex_and_exec_core(thd, nextp, TRUE, this));
}


int
sp_instr_set_trigger_field::exec_core(THD *thd, uint *nextp)
{
  Abort_on_warning_instant_set aws(thd, thd->is_strict_mode() && !thd->lex->ignore);
  const int res= (trigger_field->set_value(thd, &value) ? -1 : 0);
  *nextp = m_ip+1;
  return res;
}

void
sp_instr_set_trigger_field::print(String *str)
{
  str->append(STRING_WITH_LEN("set_trigger_field "));
  trigger_field->print(str, enum_query_type(QT_ORDINARY |
                                            QT_ITEM_ORIGINAL_FUNC_NULLIF));
  str->append(STRING_WITH_LEN(":="));
  value->print(str, enum_query_type(QT_ORDINARY |
                                    QT_ITEM_ORIGINAL_FUNC_NULLIF));
}

/*
  sp_instr_opt_meta
*/

uint sp_instr_opt_meta::get_cont_dest() const
{
  return m_cont_dest;
}


/*
 sp_instr_jump class functions
*/

PSI_statement_info sp_instr_jump::psi_info=
{ 0, "jump", 0};

int
sp_instr_jump::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_jump::execute");
  DBUG_PRINT("info", ("destination: %u", m_dest));

  *nextp= m_dest;
  DBUG_RETURN(0);
}

void
sp_instr_jump::print(String *str)
{
  /* jump dest */
  if (str->reserve(SP_INSTR_UINT_MAXLEN+5))
    return;
  str->qs_append(STRING_WITH_LEN("jump "));
  str->qs_append(m_dest);
}

uint
sp_instr_jump::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  m_dest= opt_shortcut_jump(sp, this);
  if (m_dest != m_ip+1)   /* Jumping to following instruction? */
    marked= 1;
  m_optdest= sp->get_instr(m_dest);
  return m_dest;
}

uint
sp_instr_jump::opt_shortcut_jump(sp_head *sp, sp_instr *start)
{
  uint dest= m_dest;
  sp_instr *i;

  while ((i= sp->get_instr(dest)))
  {
    uint ndest;

    if (start == i || this == i)
      break;
    ndest= i->opt_shortcut_jump(sp, start);
    if (ndest == dest)
      break;
    dest= ndest;
  }
  return dest;
}

void
sp_instr_jump::opt_move(uint dst, List<sp_instr> *bp)
{
  if (m_dest > m_ip)
    bp->push_back(this);      // Forward
  else if (m_optdest)
    m_dest= m_optdest->m_ip;  // Backward
  m_ip= dst;
}


/*
  sp_instr_jump_if_not class functions
*/

PSI_statement_info sp_instr_jump_if_not::psi_info=
{ 0, "jump_if_not", 0};

int
sp_instr_jump_if_not::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_jump_if_not::execute");
  DBUG_PRINT("info", ("destination: %u", m_dest));
  DBUG_RETURN(m_lex_keeper.reset_lex_and_exec_core(thd, nextp, TRUE, this));
}


int
sp_instr_jump_if_not::exec_core(THD *thd, uint *nextp)
{
  Item *it;
  int res;

  it= thd->sp_prepare_func_item(&m_expr);
  if (! it)
  {
    res= -1;
  }
  else
  {
    res= 0;
    if (! it->val_bool())
      *nextp = m_dest;
    else
      *nextp = m_ip+1;
  }

  return res;
}


void
sp_instr_jump_if_not::print(String *str)
{
  /* jump_if_not dest(cont) ... */
  if (str->reserve(2*SP_INSTR_UINT_MAXLEN+14+32)) // Add some for the expr. too
    return;
  str->qs_append(STRING_WITH_LEN("jump_if_not "));
  str->qs_append(m_dest);
  str->qs_append('(');
  str->qs_append(m_cont_dest);
  str->qs_append(STRING_WITH_LEN(") "));
  m_expr->print(str, enum_query_type(QT_ORDINARY |
                                     QT_ITEM_ORIGINAL_FUNC_NULLIF));
}


uint
sp_instr_jump_if_not::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  sp_instr *i;

  marked= 1;
  if ((i= sp->get_instr(m_dest)))
  {
    m_dest= i->opt_shortcut_jump(sp, this);
    m_optdest= sp->get_instr(m_dest);
  }
  sp->add_mark_lead(m_dest, leads);
  if ((i= sp->get_instr(m_cont_dest)))
  {
    m_cont_dest= i->opt_shortcut_jump(sp, this);
    m_cont_optdest= sp->get_instr(m_cont_dest);
  }
  sp->add_mark_lead(m_cont_dest, leads);
  return m_ip+1;
}

void
sp_instr_jump_if_not::opt_move(uint dst, List<sp_instr> *bp)
{
  /*
    cont. destinations may point backwards after shortcutting jumps
    during the mark phase. If it's still pointing forwards, only
    push this for backpatching if sp_instr_jump::opt_move() will not
    do it (i.e. if the m_dest points backwards).
   */
  if (m_cont_dest > m_ip)
  {                             // Forward
    if (m_dest < m_ip)
      bp->push_back(this);
  }
  else if (m_cont_optdest)
    m_cont_dest= m_cont_optdest->m_ip; // Backward
  /* This will take care of m_dest and m_ip */
  sp_instr_jump::opt_move(dst, bp);
}


/*
  sp_instr_freturn class functions
*/

PSI_statement_info sp_instr_freturn::psi_info=
{ 0, "freturn", 0};

int
sp_instr_freturn::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_freturn::execute");
  DBUG_RETURN(m_lex_keeper.reset_lex_and_exec_core(thd, nextp, TRUE, this));
}


int
sp_instr_freturn::exec_core(THD *thd, uint *nextp)
{
  /*
    RETURN is a "procedure statement" (in terms of the SQL standard).
    That means, Diagnostics Area should be clean before its execution.
  */

  if (!(thd->variables.sql_mode & MODE_ORACLE))
  {
    /*
      Don't clean warnings in ORACLE mode,
      as they are needed for SQLCODE and SQLERRM:
        BEGIN
          SELECT a INTO a FROM t1;
          RETURN 'No exception ' || SQLCODE || ' ' || SQLERRM;
        EXCEPTION WHEN NO_DATA_FOUND THEN
          RETURN 'Exception ' || SQLCODE || ' ' || SQLERRM;
        END;
    */
    Diagnostics_area *da= thd->get_stmt_da();
    da->clear_warning_info(da->warning_info_id());
  }

  /*
    Change <next instruction pointer>, so that this will be the last
    instruction in the stored function.
  */

  *nextp= UINT_MAX;

  /*
    Evaluate the value of return expression and store it in current runtime
    context.

    NOTE: It's necessary to evaluate result item right here, because we must
    do it in scope of execution the current context/block.
  */

  return thd->spcont->set_return_value(thd, &m_value);
}

void
sp_instr_freturn::print(String *str)
{
  /* freturn type expr... */
  if (str->reserve(1024+8+32)) // Add some for the expr. too
    return;
  str->qs_append(STRING_WITH_LEN("freturn "));
  LEX_CSTRING name= m_type_handler->name().lex_cstring();
  str->qs_append(&name);
  str->qs_append(' ');
  m_value->print(str, enum_query_type(QT_ORDINARY |
                                      QT_ITEM_ORIGINAL_FUNC_NULLIF));
}

/*
  sp_instr_preturn class functions
*/

PSI_statement_info sp_instr_preturn::psi_info=
{ 0, "preturn", 0};

int
sp_instr_preturn::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_preturn::execute");
  *nextp= UINT_MAX;
  DBUG_RETURN(0);
}

void
sp_instr_preturn::print(String *str)
{
  str->append(STRING_WITH_LEN("preturn"));
}

/*
  sp_instr_hpush_jump class functions
*/

PSI_statement_info sp_instr_hpush_jump::psi_info=
{ 0, "hpush_jump", 0};

int
sp_instr_hpush_jump::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hpush_jump::execute");

  int ret= thd->spcont->push_handler(this);

  *nextp= m_dest;

  DBUG_RETURN(ret);
}


void
sp_instr_hpush_jump::print(String *str)
{
  /* hpush_jump dest fsize type */
  if (str->reserve(SP_INSTR_UINT_MAXLEN*2 + 21))
    return;

  str->qs_append(STRING_WITH_LEN("hpush_jump "));
  str->qs_append(m_dest);
  str->qs_append(' ');
  str->qs_append(m_frame);

  switch (m_handler->type) {
  case sp_handler::EXIT:
    str->qs_append(STRING_WITH_LEN(" EXIT"));
    break;
  case sp_handler::CONTINUE:
    str->qs_append(STRING_WITH_LEN(" CONTINUE"));
    break;
  default:
    // The handler type must be either CONTINUE or EXIT.
    DBUG_ASSERT(0);
  }
}


uint
sp_instr_hpush_jump::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  sp_instr *i;

  marked= 1;
  if ((i= sp->get_instr(m_dest)))
  {
    m_dest= i->opt_shortcut_jump(sp, this);
    m_optdest= sp->get_instr(m_dest);
  }
  sp->add_mark_lead(m_dest, leads);

  /*
    For continue handlers, all instructions in the scope of the handler
    are possible leads. For example, the instruction after freturn might
    be executed if the freturn triggers the condition handled by the
    continue handler.

    m_dest marks the start of the handler scope. It's added as a lead
    above, so we start on m_dest+1 here.
    m_opt_hpop is the hpop marking the end of the handler scope.
  */
  if (m_handler->type == sp_handler::CONTINUE)
  {
    for (uint scope_ip= m_dest+1; scope_ip <= m_opt_hpop; scope_ip++)
      sp->add_mark_lead(scope_ip, leads);
  }

  return m_ip+1;
}


/*
  sp_instr_hpop class functions
*/

PSI_statement_info sp_instr_hpop::psi_info=
{ 0, "hpop", 0};

int
sp_instr_hpop::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hpop::execute");
  thd->spcont->pop_handlers(m_count);
  *nextp= m_ip+1;
  DBUG_RETURN(0);
}

void
sp_instr_hpop::print(String *str)
{
  /* hpop count */
  if (str->reserve(SP_INSTR_UINT_MAXLEN+5))
    return;
  str->qs_append(STRING_WITH_LEN("hpop "));
  str->qs_append(m_count);
}


/*
  sp_instr_hreturn class functions
*/

PSI_statement_info sp_instr_hreturn::psi_info=
{ 0, "hreturn", 0};

int
sp_instr_hreturn::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_hreturn::execute");

  uint continue_ip= thd->spcont->exit_handler(thd->get_stmt_da());

  *nextp= m_dest ? m_dest : continue_ip;

  DBUG_RETURN(0);
}


void
sp_instr_hreturn::print(String *str)
{
  /* hreturn framesize dest */
  if (str->reserve(SP_INSTR_UINT_MAXLEN*2 + 9))
    return;
  str->qs_append(STRING_WITH_LEN("hreturn "));
  if (m_dest)
  {
    // NOTE: this is legacy: hreturn instruction for EXIT handler
    // should print out 0 as frame index.
    str->qs_append(STRING_WITH_LEN("0 "));
    str->qs_append(m_dest);
  }
  else
  {
    str->qs_append(m_frame);
  }
}


uint
sp_instr_hreturn::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  marked= 1;

  if (m_dest)
  {
    /*
      This is an EXIT handler; next instruction step is in m_dest.
     */
    return m_dest;
  }

  /*
    This is a CONTINUE handler; next instruction step will come from
    the handler stack and not from opt_mark.
   */
  return UINT_MAX;
}


/*
  sp_instr_cpush class functions
*/

PSI_statement_info sp_instr_cpush::psi_info=
{ 0, "cpush", 0};

int
sp_instr_cpush::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cpush::execute");

  sp_cursor::reset(thd, &m_lex_keeper);
  m_lex_keeper.disable_query_cache();
  thd->spcont->push_cursor(this);

  *nextp= m_ip+1;

  DBUG_RETURN(false);
}


void
sp_instr_cpush::print(String *str)
{
  const LEX_CSTRING *cursor_name= m_ctx->find_cursor(m_cursor);

  /* cpush name@offset */
  size_t rsrv= SP_INSTR_UINT_MAXLEN+7;

  if (cursor_name)
    rsrv+= cursor_name->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("cpush "));
  if (cursor_name)
  {
    str->qs_append(cursor_name->str, cursor_name->length);
    str->qs_append('@');
  }
  str->qs_append(m_cursor);
}


/*
  sp_instr_cpop class functions
*/

PSI_statement_info sp_instr_cpop::psi_info=
{ 0, "cpop", 0};

int
sp_instr_cpop::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cpop::execute");
  thd->spcont->pop_cursors(thd, m_count);
  *nextp= m_ip+1;
  DBUG_RETURN(0);
}


void
sp_instr_cpop::print(String *str)
{
  /* cpop count */
  if (str->reserve(SP_INSTR_UINT_MAXLEN+5))
    return;
  str->qs_append(STRING_WITH_LEN("cpop "));
  str->qs_append(m_count);
}


/*
  sp_instr_copen class functions
*/

/**
  @todo
    Assert that we either have an error or a cursor
*/

PSI_statement_info sp_instr_copen::psi_info=
{ 0, "copen", 0};

int
sp_instr_copen::execute(THD *thd, uint *nextp)
{
  /*
    We don't store a pointer to the cursor in the instruction to be
    able to reuse the same instruction among different threads in future.
  */
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;
  DBUG_ENTER("sp_instr_copen::execute");

  if (! c)
    res= -1;
  else
  {
    sp_lex_keeper *lex_keeper= c->get_lex_keeper();
    res= lex_keeper->cursor_reset_lex_and_exec_core(thd, nextp, FALSE, this);
    /* TODO: Assert here that we either have an error or a cursor */
  }
  DBUG_RETURN(res);
}


int
sp_instr_copen::exec_core(THD *thd, uint *nextp)
{
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res= c->open(thd);
  *nextp= m_ip+1;
  return res;
}

void
sp_instr_copen::print(String *str)
{
  const LEX_CSTRING *cursor_name= m_ctx->find_cursor(m_cursor);

  /* copen name@offset */
  size_t rsrv= SP_INSTR_UINT_MAXLEN+7;

  if (cursor_name)
    rsrv+= cursor_name->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("copen "));
  if (cursor_name)
  {
    str->qs_append(cursor_name->str, cursor_name->length);
    str->qs_append('@');
  }
  str->qs_append(m_cursor);
}


/*
  sp_instr_cclose class functions
*/

PSI_statement_info sp_instr_cclose::psi_info=
{ 0, "cclose", 0};

int
sp_instr_cclose::execute(THD *thd, uint *nextp)
{
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;
  DBUG_ENTER("sp_instr_cclose::execute");

  if (! c)
    res= -1;
  else
    res= c->close(thd);
  *nextp= m_ip+1;
  DBUG_RETURN(res);
}


void
sp_instr_cclose::print(String *str)
{
  const LEX_CSTRING *cursor_name= m_ctx->find_cursor(m_cursor);

  /* cclose name@offset */
  size_t rsrv= SP_INSTR_UINT_MAXLEN+8;

  if (cursor_name)
    rsrv+= cursor_name->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("cclose "));
  if (cursor_name)
  {
    str->qs_append(cursor_name->str, cursor_name->length);
    str->qs_append('@');
  }
  str->qs_append(m_cursor);
}


/*
  sp_instr_cfetch class functions
*/

PSI_statement_info sp_instr_cfetch::psi_info=
{ 0, "cfetch", 0};

int
sp_instr_cfetch::execute(THD *thd, uint *nextp)
{
  sp_cursor *c= thd->spcont->get_cursor(m_cursor);
  int res;
  Query_arena backup_arena;
  DBUG_ENTER("sp_instr_cfetch::execute");

  res= c ? c->fetch(thd, &m_varlist, m_error_on_no_data) : -1;

  *nextp= m_ip+1;
  DBUG_RETURN(res);
}


void
sp_instr_cfetch::print(String *str)
{
  List_iterator_fast<sp_variable> li(m_varlist);
  sp_variable *pv;
  const LEX_CSTRING *cursor_name= m_ctx->find_cursor(m_cursor);

  /* cfetch name@offset vars... */
  size_t rsrv= SP_INSTR_UINT_MAXLEN+8;

  if (cursor_name)
    rsrv+= cursor_name->length;
  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("cfetch "));
  if (cursor_name)
  {
    str->qs_append(cursor_name->str, cursor_name->length);
    str->qs_append('@');
  }
  str->qs_append(m_cursor);
  while ((pv= li++))
  {
    if (str->reserve(pv->name.length+SP_INSTR_UINT_MAXLEN+2))
      return;
    str->qs_append(' ');
    str->qs_append(&pv->name);
    str->qs_append('@');
    str->qs_append(pv->offset);
  }
}

/*
  sp_instr_agg_cfetch class functions
*/

PSI_statement_info sp_instr_agg_cfetch::psi_info=
{ 0, "agg_cfetch", 0};

int
sp_instr_agg_cfetch::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_agg_cfetch::execute");
  int res= 0;
  if (!thd->spcont->instr_ptr)
  {
    *nextp= m_ip+1;
    thd->spcont->instr_ptr= m_ip + 1;
  }
  else if (!thd->spcont->pause_state)
    thd->spcont->pause_state= TRUE;
  else
  {
    thd->spcont->pause_state= FALSE;
    if (thd->server_status & SERVER_STATUS_LAST_ROW_SENT)
    {
      my_message(ER_SP_FETCH_NO_DATA,
                 ER_THD(thd, ER_SP_FETCH_NO_DATA), MYF(0));
      res= -1;
      thd->spcont->quit_func= TRUE;
    }
    else
      *nextp= m_ip + 1;
  }
  DBUG_RETURN(res);
}

void
sp_instr_agg_cfetch::print(String *str)
{

  uint rsrv= SP_INSTR_UINT_MAXLEN+11;

  if (str->reserve(rsrv))
    return;
  str->qs_append(STRING_WITH_LEN("agg_cfetch"));
}

/*
  sp_instr_cursor_copy_struct class functions
*/

/**
  This methods processes cursor %ROWTYPE declarations, e.g.:
    CURSOR cur IS SELECT * FROM t1;
    rec cur%ROWTYPE;
  and does the following:
  - opens the cursor without copying data (materialization).
  - copies the cursor structure to the associated %ROWTYPE variable.
*/

PSI_statement_info sp_instr_cursor_copy_struct::psi_info=
{ 0, "cursor_copy_struct", 0};

int
sp_instr_cursor_copy_struct::exec_core(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cursor_copy_struct::exec_core");
  int ret= 0;
  Item_field_row *row= (Item_field_row*) thd->spcont->get_variable(m_var);
  DBUG_ASSERT(row->type_handler() == &type_handler_row);

  /*
    Copy structure only once. If the cursor%ROWTYPE variable is declared
    inside a LOOP block, it gets its structure on the first loop interation
    and remembers the structure for all consequent loop iterations.
    It we recreated the structure on every iteration, we would get
    potential memory leaks, and it would be less efficient.
  */
  if (!row->arguments())
  {
    sp_cursor tmp(thd, &m_lex_keeper, true);
    // Open the cursor without copying data
    if (!(ret= tmp.open(thd)))
    {
      Row_definition_list defs;
      /*
        Create row elements on the caller arena.
        It's the same arena that was used during sp_rcontext::create().
        This puts cursor%ROWTYPE elements on the same mem_root
        where explicit ROW elements and table%ROWTYPE reside:
        - tmp.export_structure() allocates new Spvar_definition instances
          and their components (such as TYPELIBs).
        - row->row_create_items() creates new Item_field instances.
        They all are created on the same mem_root.
      */
      Query_arena current_arena;
      thd->set_n_backup_active_arena(thd->spcont->callers_arena, &current_arena);
      if (!(ret= tmp.export_structure(thd, &defs)))
        row->row_create_items(thd, &defs);
      thd->restore_active_arena(thd->spcont->callers_arena, &current_arena);
      tmp.close(thd);
    }
  }
  *nextp= m_ip + 1;
  DBUG_RETURN(ret);
}


int
sp_instr_cursor_copy_struct::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_cursor_copy_struct::execute");
  int ret= m_lex_keeper.cursor_reset_lex_and_exec_core(thd, nextp, FALSE, this);
  DBUG_RETURN(ret);
}


void
sp_instr_cursor_copy_struct::print(String *str)
{
  sp_variable *var= m_ctx->find_variable(m_var);
  const LEX_CSTRING *name= m_ctx->find_cursor(m_cursor);
  str->append(STRING_WITH_LEN("cursor_copy_struct "));
  str->append(name);
  str->append(' ');
  str->append(&var->name);
  str->append('@');
  str->append_ulonglong(m_var);
}


/*
  sp_instr_error class functions
*/

PSI_statement_info sp_instr_error::psi_info=
{ 0, "error", 0};

int
sp_instr_error::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_error::execute");
  my_message(m_errcode, ER_THD(thd, m_errcode), MYF(0));
  WSREP_DEBUG("sp_instr_error: %s %d", ER_THD(thd, m_errcode), thd->is_error());
  *nextp= m_ip+1;
  DBUG_RETURN(-1);
}


void
sp_instr_error::print(String *str)
{
  /* error code */
  if (str->reserve(SP_INSTR_UINT_MAXLEN+6))
    return;
  str->qs_append(STRING_WITH_LEN("error "));
  str->qs_append(m_errcode);
}


/**************************************************************************
  sp_instr_set_case_expr class implementation
**************************************************************************/

PSI_statement_info sp_instr_set_case_expr::psi_info=
{ 0, "set_case_expr", 0};

int
sp_instr_set_case_expr::execute(THD *thd, uint *nextp)
{
  DBUG_ENTER("sp_instr_set_case_expr::execute");

  DBUG_RETURN(m_lex_keeper.reset_lex_and_exec_core(thd, nextp, TRUE, this));
}


int
sp_instr_set_case_expr::exec_core(THD *thd, uint *nextp)
{
  int res= thd->spcont->set_case_expr(thd, m_case_expr_id, &m_case_expr);

  if (res && !thd->spcont->get_case_expr(m_case_expr_id))
  {
    /*
      Failed to evaluate the value, the case expression is still not
      initialized. Set to NULL so we can continue.
    */

    Item *null_item= new (thd->mem_root) Item_null(thd);

    if (!null_item ||
        thd->spcont->set_case_expr(thd, m_case_expr_id, &null_item))
    {
      /* If this also failed, we have to abort. */
      my_error(ER_OUT_OF_RESOURCES, MYF(ME_FATAL));
    }
  }
  else
    *nextp= m_ip+1;

  return res;
}


void
sp_instr_set_case_expr::print(String *str)
{
  /* set_case_expr (cont) id ... */
  str->reserve(2*SP_INSTR_UINT_MAXLEN+18+32); // Add some extra for expr too
  str->qs_append(STRING_WITH_LEN("set_case_expr ("));
  str->qs_append(m_cont_dest);
  str->qs_append(STRING_WITH_LEN(") "));
  str->qs_append(m_case_expr_id);
  str->qs_append(' ');
  m_case_expr->print(str, enum_query_type(QT_ORDINARY |
                                          QT_ITEM_ORIGINAL_FUNC_NULLIF));
}

uint
sp_instr_set_case_expr::opt_mark(sp_head *sp, List<sp_instr> *leads)
{
  sp_instr *i;

  marked= 1;
  if ((i= sp->get_instr(m_cont_dest)))
  {
    m_cont_dest= i->opt_shortcut_jump(sp, this);
    m_cont_optdest= sp->get_instr(m_cont_dest);
  }
  sp->add_mark_lead(m_cont_dest, leads);
  return m_ip+1;
}

void
sp_instr_set_case_expr::opt_move(uint dst, List<sp_instr> *bp)
{
  if (m_cont_dest > m_ip)
    bp->push_back(this);        // Forward
  else if (m_cont_optdest)
    m_cont_dest= m_cont_optdest->m_ip; // Backward
  m_ip= dst;
}


/* ------------------------------------------------------------------ */


/*
  Structure that represent all instances of one table
  in optimized multi-set of tables used by routine.
*/

typedef struct st_sp_table
{
  /*
    Multi-set key:
      db_name\0table_name\0alias\0 - for normal tables
      db_name\0table_name\0        - for temporary tables
  */
  LEX_STRING qname;
  size_t db_length, table_name_length;
  bool temp;               /* true if corresponds to a temporary table */
  thr_lock_type lock_type; /* lock type used for prelocking */
  uint lock_count;
  uint query_lock_count;
  uint8 trg_event_map;
  my_bool for_insert_data;
} SP_TABLE;


uchar *sp_table_key(const uchar *ptr, size_t *plen, my_bool first)
{
  SP_TABLE *tab= (SP_TABLE *)ptr;
  *plen= tab->qname.length;
  return (uchar *)tab->qname.str;
}


/**
  Merge the list of tables used by some query into the multi-set of
  tables used by routine.

  @param thd                 thread context
  @param table               table list
  @param lex_for_tmp_check   LEX of the query for which we are merging
                             table list.

  @note
    This method will use LEX provided to check whenever we are creating
    temporary table and mark it as such in target multi-set.

  @retval
    TRUE    Success
  @retval
    FALSE   Error
*/

bool
sp_head::merge_table_list(THD *thd, TABLE_LIST *table, LEX *lex_for_tmp_check)
{
  SP_TABLE *tab;

  if ((lex_for_tmp_check->sql_command == SQLCOM_DROP_TABLE ||
      lex_for_tmp_check->sql_command == SQLCOM_DROP_SEQUENCE) &&
      lex_for_tmp_check->tmp_table())
    return TRUE;

  for (uint i= 0 ; i < m_sptabs.records ; i++)
  {
    tab= (SP_TABLE*) my_hash_element(&m_sptabs, i);
    tab->query_lock_count= 0;
  }

  for (; table ; table= table->next_global)
    if (!table->derived && !table->schema_table && !table->table_function)
    {
      /*
        Structure of key for the multi-set is "db\0table\0alias\0".
        Since "alias" part can have arbitrary length we use String
        object to construct the key. By default String will use
        buffer allocated on stack with NAME_LEN bytes reserved for
        alias, since in most cases it is going to be smaller than
        NAME_LEN bytes.
      */
      char tname_buff[(SAFE_NAME_LEN + 1) * 3];
      String tname(tname_buff, sizeof(tname_buff), &my_charset_bin);
      uint temp_table_key_length;

      tname.length(0);
      tname.append(&table->db);
      tname.append('\0');
      tname.append(&table->table_name);
      tname.append('\0');
      temp_table_key_length= tname.length();
      tname.append(&table->alias);
      tname.append('\0');

      /*
        Upgrade the lock type because this table list will be used
        only in pre-locked mode, in which DELAYED inserts are always
        converted to normal inserts.
      */
      if (table->lock_type == TL_WRITE_DELAYED)
        table->lock_type= TL_WRITE;

      /*
        We ignore alias when we check if table was already marked as temporary
        (and therefore should not be prelocked). Otherwise we will erroneously
        treat table with same name but with different alias as non-temporary.
      */
      if ((tab= (SP_TABLE*) my_hash_search(&m_sptabs, (uchar *)tname.ptr(),
                                           tname.length())) ||
          ((tab= (SP_TABLE*) my_hash_search(&m_sptabs, (uchar *)tname.ptr(),
                                            temp_table_key_length)) &&
           tab->temp))
      {
        if (tab->lock_type < table->lock_type)
          tab->lock_type= table->lock_type; // Use the table with the highest lock type
        tab->query_lock_count++;
        if (tab->query_lock_count > tab->lock_count)
          tab->lock_count++;
        tab->trg_event_map|= table->trg_event_map;
        tab->for_insert_data|= table->for_insert_data;
      }
      else
      {
        if (!(tab= (SP_TABLE *)thd->calloc(sizeof(SP_TABLE))))
          return FALSE;
        if ((lex_for_tmp_check->sql_command == SQLCOM_CREATE_TABLE ||
             lex_for_tmp_check->sql_command == SQLCOM_CREATE_SEQUENCE) &&
            lex_for_tmp_check->query_tables == table &&
            lex_for_tmp_check->tmp_table())
        {
          tab->temp= TRUE;
          tab->qname.length= temp_table_key_length;
        }
        else
          tab->qname.length= tname.length();
        tab->qname.str= (char*) thd->memdup(tname.ptr(), tab->qname.length);
        if (!tab->qname.str)
          return FALSE;
        tab->table_name_length= table->table_name.length;
        tab->db_length= table->db.length;
        tab->lock_type= table->lock_type;
        tab->lock_count= tab->query_lock_count= 1;
        tab->trg_event_map= table->trg_event_map;
        tab->for_insert_data= table->for_insert_data;
        if (my_hash_insert(&m_sptabs, (uchar *)tab))
          return FALSE;
      }
    }
  return TRUE;
}


/**
  Add tables used by routine to the table list.

    Converts multi-set of tables used by this routine to table list and adds
    this list to the end of table list specified by 'query_tables_last_ptr'.

    Elements of list will be allocated in PS memroot, so this list will be
    persistent between PS executions.

  @param[in] thd                        Thread context
  @param[in,out] query_tables_last_ptr  Pointer to the next_global member of
    last element of the list where tables
    will be added (or to its root).
  @param[in] belong_to_view             Uppermost view which uses this routine,
    0 if none.

  @retval
    TRUE    if some elements were added
  @retval
    FALSE   otherwise.
*/

bool
sp_head::add_used_tables_to_table_list(THD *thd,
                                       TABLE_LIST ***query_tables_last_ptr,
                                       TABLE_LIST *belong_to_view)
{
  uint i;
  Query_arena *arena, backup;
  bool result= FALSE;
  DBUG_ENTER("sp_head::add_used_tables_to_table_list");

  /*
    Use persistent arena for table list allocation to be PS/SP friendly.
    Note that we also have to copy database/table names and alias to PS/SP
    memory since current instance of sp_head object can pass away before
    next execution of PS/SP for which tables are added to prelocking list.
    This will be fixed by introducing of proper invalidation mechanism
    once new TDC is ready.
  */
  arena= thd->activate_stmt_arena_if_needed(&backup);

  for (i=0 ; i < m_sptabs.records ; i++)
  {
    char *tab_buff, *key_buff;
    SP_TABLE *stab= (SP_TABLE*) my_hash_element(&m_sptabs, i);
    LEX_CSTRING db_name;
    if (stab->temp)
      continue;

    if (!(tab_buff= (char *)thd->alloc(ALIGN_SIZE(sizeof(TABLE_LIST)) *
                                        stab->lock_count)) ||
        !(key_buff= (char*)thd->memdup(stab->qname.str,
                                       stab->qname.length)))
      DBUG_RETURN(FALSE);

    db_name.str=    key_buff;
    db_name.length= stab->db_length;


    for (uint j= 0; j < stab->lock_count; j++)
    {
      TABLE_LIST *table= (TABLE_LIST *)tab_buff;
      LEX_CSTRING table_name= { key_buff + stab->db_length + 1,
                                stab->table_name_length };
      LEX_CSTRING alias= { table_name.str + table_name.length + 1,
                           strlen(table_name.str + table_name.length + 1) };

      table->init_one_table_for_prelocking(&db_name,
                                           &table_name,
                                           &alias,
                                           stab->lock_type,
                                           TABLE_LIST::PRELOCK_ROUTINE,
                                           belong_to_view,
                                           stab->trg_event_map,
                                           query_tables_last_ptr,
                                           stab->for_insert_data);
      tab_buff+= ALIGN_SIZE(sizeof(TABLE_LIST));
      result= TRUE;
    }
  }

  if (arena)
    thd->restore_active_arena(arena, &backup);

  DBUG_RETURN(result);
}


/**
  Simple function for adding an explicitly named (systems) table to
  the global table list, e.g. "mysql", "proc".
*/

TABLE_LIST *
sp_add_to_query_tables(THD *thd, LEX *lex,
		       const LEX_CSTRING *db, const LEX_CSTRING *name,
                       thr_lock_type locktype,
                       enum_mdl_type mdl_type)
{
  TABLE_LIST *table;

  if (!(table= (TABLE_LIST *)thd->calloc(sizeof(TABLE_LIST))))
    return NULL;
  if (!thd->make_lex_string(&table->db, db->str, db->length) ||
      !thd->make_lex_string(&table->table_name, name->str, name->length) ||
      !thd->make_lex_string(&table->alias, name->str, name->length))
    return NULL;

  table->lock_type= locktype;
  table->select_lex= lex->current_select;
  table->cacheable_table= 1;
  MDL_REQUEST_INIT(&table->mdl_request, MDL_key::TABLE, table->db.str,
                   table->table_name.str, mdl_type, MDL_TRANSACTION);

  lex->add_to_query_tables(table);
  return table;
}


Item *sp_head::adjust_assignment_source(THD *thd, Item *val, Item *val2)
{
  return val ? val : val2 ? val2 : new (thd->mem_root) Item_null(thd);
}

/**
  Helper action for a SET statement.
  Used to push a SP local variable into the assignment list.

  @param var_type the SP local variable
  @param val      the value being assigned to the variable

  @return TRUE if error, FALSE otherwise.
*/

bool
sp_head::set_local_variable(THD *thd, sp_pcontext *spcont,
                            const Sp_rcontext_handler *rh,
                            sp_variable *spv, Item *val, LEX *lex,
                            bool responsible_to_free_lex)
{
  if (!(val= adjust_assignment_source(thd, val, spv->default_value)))
    return true;

  if (val->walk(&Item::unknown_splocal_processor, false, NULL))
    return true;

  sp_instr_set *sp_set= new (thd->mem_root)
                        sp_instr_set(instructions(), spcont, rh,
                                     spv->offset, val, lex,
                                     responsible_to_free_lex);

  return sp_set == NULL || add_instr(sp_set);
}


/**
  Similar to set_local_variable(), but for ROW variable fields.
*/

bool
sp_head::set_local_variable_row_field(THD *thd, sp_pcontext *spcont,
                                      const Sp_rcontext_handler *rh,
                                      sp_variable *spv, uint field_idx,
                                      Item *val, LEX *lex)
{
  if (!(val= adjust_assignment_source(thd, val, NULL)))
    return true;

  sp_instr_set_row_field *sp_set= new (thd->mem_root)
                                  sp_instr_set_row_field(instructions(),
                                                         spcont, rh,
                                                         spv->offset,
                                                         field_idx, val,
                                                         lex, true);
  return sp_set == NULL || add_instr(sp_set);
}


bool
sp_head::set_local_variable_row_field_by_name(THD *thd, sp_pcontext *spcont,
                                              const Sp_rcontext_handler *rh,
                                              sp_variable *spv,
                                              const LEX_CSTRING *field_name,
                                              Item *val, LEX *lex)
{
  if (!(val= adjust_assignment_source(thd, val, NULL)))
    return true;

  sp_instr_set_row_field_by_name *sp_set=
    new (thd->mem_root) sp_instr_set_row_field_by_name(instructions(),
                                                       spcont, rh,
                                                       spv->offset,
                                                       *field_name,
                                                       val,
                                                       lex, true);
  return sp_set == NULL || add_instr(sp_set);
}


bool sp_head::add_open_cursor(THD *thd, sp_pcontext *spcont, uint offset,
                              sp_pcontext *param_spcont,
                              List<sp_assignment_lex> *parameters)
{
  /*
    The caller must make sure that the number of formal parameters matches
    the number of actual parameters.
  */
  DBUG_ASSERT((param_spcont ? param_spcont->context_var_count() :  0) ==
              (parameters ? parameters->elements : 0));

  if (parameters &&
      add_set_cursor_param_variables(thd, param_spcont, parameters))
    return true;

  sp_instr_copen *i= new (thd->mem_root)
                     sp_instr_copen(instructions(), spcont, offset);
  return i == NULL || add_instr(i);
}


bool sp_head::add_for_loop_open_cursor(THD *thd, sp_pcontext *spcont,
                                       sp_variable *index,
                                       const sp_pcursor *pcursor, uint coffset,
                                       sp_assignment_lex *param_lex,
                                       Item_args *parameters)
{
  if (parameters &&
      add_set_for_loop_cursor_param_variables(thd, pcursor->param_context(),
                                              param_lex, parameters))
    return true;

  sp_instr *instr_copy_struct=
    new (thd->mem_root) sp_instr_cursor_copy_struct(instructions(),
                                                    spcont, coffset,
                                                    pcursor->lex(),
                                                    index->offset);
  if (instr_copy_struct == NULL || add_instr(instr_copy_struct))
    return true;

  sp_instr_copen *instr_copen=
    new (thd->mem_root) sp_instr_copen(instructions(), spcont, coffset);
  if (instr_copen == NULL || add_instr(instr_copen))
    return true;

  sp_instr_cfetch *instr_cfetch=
    new (thd->mem_root) sp_instr_cfetch(instructions(),
                                        spcont, coffset, false);
  if (instr_cfetch == NULL || add_instr(instr_cfetch))
    return true;
  instr_cfetch->add_to_varlist(index);
  return false;
}


bool
sp_head::add_set_for_loop_cursor_param_variables(THD *thd,
                                                 sp_pcontext *param_spcont,
                                                 sp_assignment_lex *param_lex,
                                                 Item_args *parameters)
{
  DBUG_ASSERT(param_spcont->context_var_count() == parameters->argument_count());
  for (uint idx= 0; idx < parameters->argument_count(); idx ++)
  {
    /*
      param_lex is shared between multiple items (cursor parameters).
      Only the last sp_instr_set is responsible for freeing param_lex.
      See more comments in LEX::sp_for_loop_cursor_declarations in sql_lex.cc.
    */
    bool last= idx + 1 == parameters->argument_count();
    sp_variable *spvar= param_spcont->get_context_variable(idx);
    if (set_local_variable(thd, param_spcont,
                           &sp_rcontext_handler_local,
                           spvar, parameters->arguments()[idx],
                           param_lex, last))
      return true;
  }
  return false;
}


bool sp_head::spvar_fill_row(THD *thd,
                             sp_variable *spvar,
                             Row_definition_list *defs)
{
  spvar->field_def.set_row_field_definitions(defs);
  spvar->field_def.field_name= spvar->name;
  if (fill_spvar_definition(thd, &spvar->field_def))
    return true;
  row_fill_field_definitions(thd, defs);
  return false;
}


bool sp_head::spvar_fill_type_reference(THD *thd,
                                        sp_variable *spvar,
                                        const LEX_CSTRING &table,
                                        const LEX_CSTRING &col)
{
  Qualified_column_ident *ref;
  if (!(ref= new (thd->mem_root) Qualified_column_ident(&table, &col)))
    return true;
  fill_spvar_using_type_reference(spvar, ref);
  return false;
}


bool sp_head::spvar_fill_type_reference(THD *thd,
                                        sp_variable *spvar,
                                        const LEX_CSTRING &db,
                                        const LEX_CSTRING &table,
                                        const LEX_CSTRING &col)
{
  Qualified_column_ident *ref;
  if (!(ref= new (thd->mem_root) Qualified_column_ident(thd, &db, &table, &col)))
    return true;
  fill_spvar_using_type_reference(spvar, ref);
  return false;
}


bool sp_head::spvar_fill_table_rowtype_reference(THD *thd,
                                                 sp_variable *spvar,
                                                 const LEX_CSTRING &table)
{
  Table_ident *ref;
  if (!(ref= new (thd->mem_root) Table_ident(&table)))
    return true;
  fill_spvar_using_table_rowtype_reference(thd, spvar, ref);
  return false;
}


bool sp_head::spvar_fill_table_rowtype_reference(THD *thd,
                                                 sp_variable *spvar,
                                                 const LEX_CSTRING &db,
                                                 const LEX_CSTRING &table)
{
  Table_ident *ref;
  if (!(ref= new (thd->mem_root) Table_ident(thd, &db, &table, false)))
    return true;
  fill_spvar_using_table_rowtype_reference(thd, spvar, ref);
  return false;
}


bool sp_head::check_group_aggregate_instructions_forbid() const
{
  if (unlikely(m_flags & sp_head::HAS_AGGREGATE_INSTR))
  {
    my_error(ER_NOT_AGGREGATE_FUNCTION, MYF(0));
    return true;
  }
  return false;
}


bool sp_head::check_group_aggregate_instructions_require() const
{
  if (unlikely(!(m_flags & HAS_AGGREGATE_INSTR)))
  {
    my_error(ER_INVALID_AGGREGATE_FUNCTION, MYF(0));
    return true;
  }
  return false;
}


bool sp_head::check_group_aggregate_instructions_function() const
{
  return agg_type() == GROUP_AGGREGATE ?
         check_group_aggregate_instructions_require() :
         check_group_aggregate_instructions_forbid();
}


/*
  In Oracle mode stored routines have an optional name
  at the end of a declaration:
    PROCEDURE p1 AS
    BEGIN
      NULL
    END p1;
  Check that the first p1 and the last p1 match.
*/

bool sp_head::check_package_routine_end_name(const LEX_CSTRING &end_name) const
{
  LEX_CSTRING non_qualified_name= m_name;
  const char *errpos;
  size_t ofs;
  if (!end_name.length)
    return false; // No end name
  if (!(errpos= strrchr(m_name.str, '.')))
  {
    errpos= m_name.str;
    goto err;
  }
  errpos++;
  ofs= errpos - m_name.str;
  non_qualified_name.str+= ofs;
  non_qualified_name.length-= ofs;
  if (Sp_handler::eq_routine_name(end_name, non_qualified_name))
    return false;
err:
  my_error(ER_END_IDENTIFIER_DOES_NOT_MATCH, MYF(0), end_name.str, errpos);
  return true;
}


bool
sp_head::check_standalone_routine_end_name(const sp_name *end_name) const
{
  if (end_name && !end_name->eq(this))
  {
    my_error(ER_END_IDENTIFIER_DOES_NOT_MATCH, MYF(0),
             ErrConvDQName(end_name).ptr(), ErrConvDQName(this).ptr());
    return true;
  }
  return false;
}


ulong sp_head::sp_cache_version() const
{
  return m_parent ? m_parent->sp_cache_version() : m_sp_cache_version;
}
