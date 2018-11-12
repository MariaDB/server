/*
   Copyright (c) 2000, 2015, Oracle and/or its affiliates.
   Copyright (c) 2010, 2016, MariaDB

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; version 2 of the License.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301  USA */

/* sql_yacc.yy */

/**
  @defgroup Parser Parser
  @{
*/

%{
#define YYLIP  (& thd->m_parser_state->m_lip)
#define YYPS   (& thd->m_parser_state->m_yacc)
#define YYCSCL (thd->variables.character_set_client)

#define MYSQL_YACC
#define YYINITDEPTH 100
#define YYMAXDEPTH 3200                        /* Because of 64K stack */
#define Lex (thd->lex)

#define Select Lex->current_select
#include "mariadb.h"
#include "sql_priv.h"
#include "sql_parse.h"                        /* comp_*_creator */
#include "sql_table.h"                        /* primary_key_name */
#include "sql_partition.h"  /* partition_info, HASH_PARTITION */
#include "sql_acl.h"                          /* *_ACL */
#include "sql_class.h"      /* Key_part_spec, enum_filetype, Diag_condition_item_name */
#include "slave.h"
#include "lex_symbol.h"
#include "item_create.h"
#include "sp_head.h"
#include "sp_rcontext.h"
#include "sp.h"
#include "sql_show.h"
#include "sql_alter.h"                         // Sql_cmd_alter_table*
#include "sql_truncate.h"                      // Sql_cmd_truncate_table
#include "sql_admin.h"                         // Sql_cmd_analyze/Check..._table
#include "sql_partition_admin.h"               // Sql_cmd_alter_table_*_part.
#include "sql_handler.h"                       // Sql_cmd_handler_*
#include "sql_signal.h"
#include "sql_get_diagnostics.h"               // Sql_cmd_get_diagnostics
#include "sql_cte.h"
#include "sql_window.h"
#include "item_windowfunc.h"
#include "event_parse_data.h"
#include "create_options.h"
#include <myisam.h>
#include <myisammrg.h>
#include "keycaches.h"
#include "set_var.h"
#include "rpl_mi.h"
#include "lex_token.h"
#include "sql_lex.h"
#include "sql_sequence.h"
#include "my_base.h"

/* this is to get the bison compilation windows warnings out */
#ifdef _MSC_VER
/* warning C4065: switch statement contains 'default' but no 'case' labels */
#pragma warning (disable : 4065)
#endif

int yylex(void *yylval, void *yythd);

#define yyoverflow(A,B,C,D,E,F)               \
  {                                           \
    size_t val= *(F);                          \
    if (unlikely(my_yyoverflow((B), (D), &val))) \
    {                                         \
      yyerror(thd, (char*) (A));              \
      return 2;                               \
    }                                         \
    else                                      \
    {                                         \
      *(F)= (YYSIZE_T)val;                    \
    }                                         \
  }

#define MYSQL_YYABORT                         \
  do                                          \
  {                                           \
    LEX::cleanup_lex_after_parse_error(thd);  \
    YYABORT;                                  \
  } while (0)

#define MYSQL_YYABORT_UNLESS(A)                  \
  if (unlikely(!(A)))                            \
  {                                              \
    thd->parse_error();                          \
    MYSQL_YYABORT;                               \
  }

#define my_yyabort_error(A)                      \
  do { my_error A; MYSQL_YYABORT; } while(0)

#ifndef DBUG_OFF
#define YYDEBUG 1
#else
#define YYDEBUG 0
#endif


/**
  @brief Bison callback to report a syntax/OOM error

  This function is invoked by the bison-generated parser
  when a syntax error, a parse error or an out-of-memory
  condition occurs. This function is not invoked when the
  parser is requested to abort by semantic action code
  by means of YYABORT or YYACCEPT macros. This is why these
  macros should not be used (use MYSQL_YYABORT/MYSQL_YYACCEPT
  instead).

  The parser will abort immediately after invoking this callback.

  This function is not for use in semantic actions and is internal to
  the parser, as it performs some pre-return cleanup. 
  In semantic actions, please use thd->parse_error() or my_error to
  push an error into the error stack and MYSQL_YYABORT
  to abort from the parser.
*/

void MYSQLerror(THD *thd, const char *s)
{
  /*
    Restore the original LEX if it was replaced when parsing
    a stored procedure. We must ensure that a parsing error
    does not leave any side effects in the THD.
  */
  LEX::cleanup_lex_after_parse_error(thd);

  /* "parse error" changed into "syntax error" between bison 1.75 and 1.875 */
  if (strcmp(s,"parse error") == 0 || strcmp(s,"syntax error") == 0)
    s= ER_THD(thd, ER_SYNTAX_ERROR);
  thd->parse_error(s, 0);
}


#ifndef DBUG_OFF
void turn_parser_debug_on()
{
  /*
     MYSQLdebug is in sql/sql_yacc.cc, in bison generated code.
     Turning this option on is **VERY** verbose, and should be
     used when investigating a syntax error problem only.

     The syntax to run with bison traces is as follows :
     - Starting a server manually :
       mysqld --debug-dbug="d,parser_debug" ...
     - Running a test :
       mysql-test-run.pl --mysqld="--debug-dbug=d,parser_debug" ...

     The result will be in the process stderr (var/log/master.err)
   */

  extern int yydebug;
  yydebug= 1;
}
#endif


/**
  Helper action for a case expression statement (the expr in 'CASE expr').
  This helper is used for 'searched' cases only.
  @param lex the parser lex context
  @param expr the parsed expression
  @return 0 on success
*/

int LEX::case_stmt_action_expr(Item* expr)
{
  int case_expr_id= spcont->register_case_expr();
  sp_instr_set_case_expr *i;

  if (spcont->push_case_expr_id(case_expr_id))
    return 1;

  i= new (thd->mem_root)
    sp_instr_set_case_expr(sphead->instructions(), spcont, case_expr_id, expr,
                           this);

  sphead->add_cont_backpatch(i);
  return sphead->add_instr(i);
}

/**
  Helper action for a case when condition.
  This helper is used for both 'simple' and 'searched' cases.
  @param lex the parser lex context
  @param when the parsed expression for the WHEN clause
  @param simple true for simple cases, false for searched cases
*/

int LEX::case_stmt_action_when(Item *when, bool simple)
{
  uint ip= sphead->instructions();
  sp_instr_jump_if_not *i;
  Item_case_expr *var;
  Item *expr;

  if (simple)
  {
    var= new (thd->mem_root)
         Item_case_expr(thd, spcont->get_current_case_expr_id());

#ifdef DBUG_ASSERT_EXISTS
    if (var)
    {
      var->m_sp= sphead;
    }
#endif

    expr= new (thd->mem_root) Item_func_eq(thd, var, when);
    i= new (thd->mem_root) sp_instr_jump_if_not(ip, spcont, expr, this);
  }
  else
    i= new (thd->mem_root) sp_instr_jump_if_not(ip, spcont, when, this);

  /*
    BACKPATCH: Registering forward jump from
    "case_stmt_action_when" to "case_stmt_action_then"
    (jump_if_not from instruction 2 to 5, 5 to 8 ... in the example)
  */

  return
    !MY_TEST(i) ||
    sphead->push_backpatch(thd, i, spcont->push_label(thd, &empty_clex_str, 0)) ||
    sphead->add_cont_backpatch(i) ||
    sphead->add_instr(i);
}

/**
  Helper action for a case then statements.
  This helper is used for both 'simple' and 'searched' cases.
  @param lex the parser lex context
*/

int LEX::case_stmt_action_then()
{
  uint ip= sphead->instructions();
  sp_instr_jump *i= new (thd->mem_root) sp_instr_jump(ip, spcont);
  if (!MY_TEST(i) || sphead->add_instr(i))
    return 1;

  /*
    BACKPATCH: Resolving forward jump from
    "case_stmt_action_when" to "case_stmt_action_then"
    (jump_if_not from instruction 2 to 5, 5 to 8 ... in the example)
  */

  sphead->backpatch(spcont->pop_label());

  /*
    BACKPATCH: Registering forward jump from
    "case_stmt_action_then" to after END CASE
    (jump from instruction 4 to 12, 7 to 12 ... in the example)
  */

  return sphead->push_backpatch(thd, i, spcont->last_label());
}


/**
  Helper action for a SET statement.
  Used to push a system variable into the assignment list.

  @param tmp      the system variable with base name
  @param var_type the scope of the variable
  @param val      the value being assigned to the variable

  @return TRUE if error, FALSE otherwise.
*/

bool
LEX::set_system_variable(enum enum_var_type var_type,
                         sys_var *sysvar, const LEX_CSTRING *base_name,
                         Item *val)
{
  set_var *setvar;

  /* No AUTOCOMMIT from a stored function or trigger. */
  if (spcont && sysvar == Sys_autocommit_ptr)
    sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;

  if (val && val->type() == Item::FIELD_ITEM &&
      ((Item_field*)val)->table_name)
  {
    my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0), sysvar->name.str);
    return TRUE;
  }

  if (!(setvar= new (thd->mem_root) set_var(thd, var_type, sysvar,
                                            base_name, val)))
    return TRUE;

  return var_list.push_back(setvar, thd->mem_root);
}


/**
  Helper action for a SET statement.
  Used to SET a field of NEW row.

  @param name     the field name
  @param val      the value being assigned to the row

  @return TRUE if error, FALSE otherwise.
*/

bool LEX::set_trigger_new_row(const LEX_CSTRING *name, Item *val)
{
  Item_trigger_field *trg_fld;
  sp_instr_set_trigger_field *sp_fld;

  /* QQ: Shouldn't this be field's default value ? */
  if (! val)
    val= new (thd->mem_root) Item_null(thd);

  DBUG_ASSERT(trg_chistics.action_time == TRG_ACTION_BEFORE &&
              (trg_chistics.event == TRG_EVENT_INSERT ||
               trg_chistics.event == TRG_EVENT_UPDATE));

  trg_fld= new (thd->mem_root)
            Item_trigger_field(thd, current_context(),
                               Item_trigger_field::NEW_ROW,
                               name, UPDATE_ACL, FALSE);

  if (unlikely(trg_fld == NULL))
    return TRUE;

  sp_fld= new (thd->mem_root)
        sp_instr_set_trigger_field(sphead->instructions(),
                                   spcont, trg_fld, val, this);

  if (unlikely(sp_fld == NULL))
    return TRUE;

  /*
    Let us add this item to list of all Item_trigger_field
    objects in trigger.
  */
  trg_table_fields.link_in_list(trg_fld, &trg_fld->next_trg_field);

  return sphead->add_instr(sp_fld);
}


/**
  Create an object to represent a SP variable in the Item-hierarchy.

  @param  name        The SP variable name.
  @param  spvar       The SP variable (optional).
  @param  start_in_q  Start position of the SP variable name in the query.
  @param  end_in_q    End position of the SP variable name in the query.

  @remark If spvar is not specified, the name is used to search for the
          variable in the parse-time context. If the variable does not
          exist, a error is set and NULL is returned to the caller.

  @return An Item_splocal object representing the SP variable, or NULL on error.
*/
Item_splocal*
LEX::create_item_for_sp_var(const Lex_ident_cli_st *cname, sp_variable *spvar)
{
  const Sp_rcontext_handler *rh;
  Item_splocal *item;
  const char *start_in_q= cname->pos();
  const char *end_in_q= cname->end();
  uint pos_in_q, len_in_q;
  Lex_ident_sys name(thd, cname);

  if (name.is_null())
    return NULL;  // EOM

  /* If necessary, look for the variable. */
  if (spcont && !spvar)
    spvar= find_variable(&name, &rh);

  if (!spvar)
  {
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), name.str);
    return NULL;
  }

  DBUG_ASSERT(spcont && spvar);

  /* Position and length of the SP variable name in the query. */
  pos_in_q= (uint)(start_in_q - sphead->m_tmp_query);
  len_in_q= (uint)(end_in_q - start_in_q);

  item= new (thd->mem_root)
    Item_splocal(thd, rh, &name, spvar->offset, spvar->type_handler(),
                 pos_in_q, len_in_q);

#ifdef DBUG_ASSERT_EXISTS
  if (item)
    item->m_sp= sphead;
#endif

  return item;
}

/**
  Helper to resolve the SQL:2003 Syntax exception 1) in <in predicate>.
  See SQL:2003, Part 2, section 8.4 <in predicate>, Note 184, page 383.
  This function returns the proper item for the SQL expression
  <code>left [NOT] IN ( expr )</code>
  @param thd the current thread
  @param left the in predicand
  @param equal true for IN predicates, false for NOT IN predicates
  @param expr first and only expression of the in value list
  @return an expression representing the IN predicate.
*/
Item* handle_sql2003_note184_exception(THD *thd, Item* left, bool equal,
                                       Item *expr)
{
  /*
    Relevant references for this issue:
    - SQL:2003, Part 2, section 8.4 <in predicate>, page 383,
    - SQL:2003, Part 2, section 7.2 <row value expression>, page 296,
    - SQL:2003, Part 2, section 6.3 <value expression primary>, page 174,
    - SQL:2003, Part 2, section 7.15 <subquery>, page 370,
    - SQL:2003 Feature F561, "Full value expressions".

    The exception in SQL:2003 Note 184 means:
    Item_singlerow_subselect, which corresponds to a <scalar subquery>,
    should be re-interpreted as an Item_in_subselect, which corresponds
    to a <table subquery> when used inside an <in predicate>.

    Our reading of Note 184 is reccursive, so that all:
    - IN (( <subquery> ))
    - IN ((( <subquery> )))
    - IN '('^N <subquery> ')'^N
    - etc
    should be interpreted as a <table subquery>, no matter how deep in the
    expression the <subquery> is.
  */

  Item *result;

  DBUG_ENTER("handle_sql2003_note184_exception");

  if (expr->type() == Item::SUBSELECT_ITEM)
  {
    Item_subselect *expr2 = (Item_subselect*) expr;

    if (expr2->substype() == Item_subselect::SINGLEROW_SUBS)
    {
      Item_singlerow_subselect *expr3 = (Item_singlerow_subselect*) expr2;
      st_select_lex *subselect;

      /*
        Implement the mandated change, by altering the semantic tree:
          left IN Item_singlerow_subselect(subselect)
        is modified to
          left IN (subselect)
        which is represented as
          Item_in_subselect(left, subselect)
      */
      subselect= expr3->invalidate_and_restore_select_lex();
      result= new (thd->mem_root) Item_in_subselect(thd, left, subselect);

      if (! equal)
        result = negate_expression(thd, result);

      DBUG_RETURN(result);
    }
  }

  if (equal)
    result= new (thd->mem_root) Item_func_eq(thd, left, expr);
  else
    result= new (thd->mem_root) Item_func_ne(thd, left, expr);

  DBUG_RETURN(result);
}

/**
  Create a separate LEX for each assignment if in SP.

  If we are in SP we want have own LEX for each assignment.
  This is mostly because it is hard for several sp_instr_set
  and sp_instr_set_trigger instructions share one LEX.
  (Well, it is theoretically possible but adds some extra
  overhead on preparation for execution stage and IMO less
  robust).

  QQ: May be we should simply prohibit group assignments in SP?

  @see sp_create_assignment_instr

  @param thd           Thread context
  @param no_lookahead  True if the parser has no lookahead
*/

void sp_create_assignment_lex(THD *thd, bool no_lookahead)
{
  LEX *lex= thd->lex;

  if (lex->sphead)
  {
    Lex_input_stream *lip= &thd->m_parser_state->m_lip;
    LEX *old_lex= lex;
    lex->sphead->reset_lex(thd);
    lex= thd->lex;

    /* Set new LEX as if we at start of set rule. */
    lex->sql_command= SQLCOM_SET_OPTION;
    mysql_init_select(lex);
    lex->var_list.empty();
    lex->autocommit= 0;
    /* get_ptr() is only correct with no lookahead. */
    if (no_lookahead)
        lex->sphead->m_tmp_query= lip->get_ptr();
    else
        lex->sphead->m_tmp_query= lip->get_tok_end();
    /* Inherit from outer lex. */
    lex->option_type= old_lex->option_type;
    lex->main_select_push();
  }
}


/**
  Create a SP instruction for a SET assignment.

  @see sp_create_assignment_lex

  @param thd           Thread context
  @param no_lookahead  True if the parser has no lookahead

  @return false if success, true otherwise.
*/

bool sp_create_assignment_instr(THD *thd, bool no_lookahead)
{
  LEX *lex= thd->lex;

  if (lex->sphead)
  {
    if (!lex->var_list.is_empty())
    {
      /*
        We have assignment to user or system variable or
        option setting, so we should construct sp_instr_stmt
        for it.
      */
      Lex_input_stream *lip= &thd->m_parser_state->m_lip;

      /*
        Extract the query statement from the tokenizer.  The
        end is either lip->ptr, if there was no lookahead,
        lip->tok_end otherwise.
      */
      static const LEX_CSTRING setsp= { STRING_WITH_LEN("SET ") };
      const char *qend= no_lookahead ? lip->get_ptr() : lip->get_tok_end();
      Lex_cstring qbuf(lex->sphead->m_tmp_query, qend);
      if (lex->new_sp_instr_stmt(thd, setsp, qbuf))
        return true;
    }
    lex->pop_select();
    if (Lex->check_main_unit_semantics())
      return true;
    enum_var_type inner_option_type= lex->option_type;
    if (lex->sphead->restore_lex(thd))
      return true;
    /* Copy option_type to outer lex in case it has changed. */
    thd->lex->option_type= inner_option_type;
  }
  return false;
}

void LEX::add_key_to_list(LEX_CSTRING *field_name,
                          enum Key::Keytype type, bool check_exists)
{
  Key *key;
  MEM_ROOT *mem_root= thd->mem_root;
  key= new (mem_root)
        Key(type, &null_clex_str, HA_KEY_ALG_UNDEF, false,
             DDL_options(check_exists ?
                         DDL_options::OPT_IF_NOT_EXISTS :
                         DDL_options::OPT_NONE));
  key->columns.push_back(new (mem_root) Key_part_spec(field_name, 0),
                         mem_root);
  alter_info.key_list.push_back(key, mem_root);
}

bool LEX::add_alter_list(const char *name, Virtual_column_info *expr,
                         bool exists)
{
  MEM_ROOT *mem_root= thd->mem_root;
  Alter_column *ac= new (mem_root) Alter_column(name, expr, exists);
  if (unlikely(ac == NULL))
    return true;
  alter_info.alter_list.push_back(ac, mem_root);
  alter_info.flags|= ALTER_CHANGE_COLUMN_DEFAULT;
  return false;
}

void LEX::init_last_field(Column_definition *field,
                          const LEX_CSTRING *field_name,
                          const CHARSET_INFO *cs)
{
  last_field= field;

  field->field_name= *field_name;

  /* reset LEX fields that are used in Create_field::set_and_check() */
  charset= cs;
}


bool LEX::set_bincmp(CHARSET_INFO *cs, bool bin)
{
  /*
     if charset is NULL - we're parsing a field declaration.
     we cannot call find_bin_collation for a field here, because actual
     field charset is determined in get_sql_field_charset() much later.
     so we only set a flag.
  */
  if (!charset)
  {
    charset= cs;
    last_field->flags|= bin ? BINCMP_FLAG : 0;
    return false;
  }

  charset= bin ? find_bin_collation(cs ? cs : charset)
               :                    cs ? cs : charset;
  return charset == NULL;
}

#define bincmp_collation(X,Y)           \
  do                                    \
  {                                     \
     if (unlikely(Lex->set_bincmp(X,Y))) \
       MYSQL_YYABORT;                   \
  } while(0)


Virtual_column_info *add_virtual_expression(THD *thd, Item *expr)
{
  Virtual_column_info *v= new (thd->mem_root) Virtual_column_info();
  if (unlikely(!v))
     return 0;
   v->expr= expr;
   v->utf8= 0;  /* connection charset */
   return v;
}


%}
%union {
  int  num;
  ulong ulong_num;
  ulonglong ulonglong_number;
  longlong longlong_number;
  uint sp_instr_addr;

  /* structs */
  LEX_CSTRING lex_str;
  Lex_ident_cli_st kwd;
  Lex_ident_cli_st ident_cli;
  Lex_ident_sys_st ident_sys;
  Lex_string_with_metadata_st lex_string_with_metadata;
  Lex_spblock_st spblock;
  Lex_spblock_handlers_st spblock_handlers;
  Lex_length_and_dec_st Lex_length_and_dec;
  Lex_cast_type_st Lex_cast_type;
  Lex_field_type_st Lex_field_type;
  Lex_dyncol_type_st Lex_dyncol_type;
  Lex_for_loop_st for_loop;
  Lex_for_loop_bounds_st for_loop_bounds;
  Lex_trim_st trim;
  vers_history_point_t vers_history_point;
  struct
  {
    enum sub_select_type unit_type;
    bool distinct;
  } unit_operation;
  struct
  {
    SELECT_LEX *first;
    SELECT_LEX *prev_last;
  } select_list;
  SQL_I_List<ORDER> *select_order;
  Lex_select_lock select_lock;
  Lex_select_limit select_limit;
  Lex_order_limit_lock *order_limit_lock;

  /* pointers */
  Create_field *create_field;
  Spvar_definition *spvar_definition;
  Row_definition_list *spvar_definition_list;
  const Type_handler *type_handler;
  CHARSET_INFO *charset;
  Condition_information_item *cond_info_item;
  DYNCALL_CREATE_DEF *dyncol_def;
  Diagnostics_information *diag_info;
  Item *item;
  Item_num *item_num;
  Item_param *item_param;
  Item_basic_constant *item_basic_constant;
  Key_part_spec *key_part;
  LEX *lex;
  sp_assignment_lex *assignment_lex;
  class sp_lex_cursor *sp_cursor_stmt;
  LEX_CSTRING *lex_str_ptr;
  LEX_USER *lex_user;
  List<Condition_information_item> *cond_info_list;
  List<DYNCALL_CREATE_DEF> *dyncol_def_list;
  List<Item> *item_list;
  List<sp_assignment_lex> *sp_assignment_lex_list;
  List<Statement_information_item> *stmt_info_list;
  List<String> *string_list;
  List<LEX_CSTRING> *lex_str_list;
  Statement_information_item *stmt_info_item;
  String *string;
  TABLE_LIST *table_list;
  Table_ident *table;
  Qualified_column_ident *qualified_column_ident;
  char *simple_string;
  const char *const_simple_string;
  chooser_compare_func_creator boolfunc2creator;
  class my_var *myvar;
  class sp_condition_value *spcondvalue;
  class sp_head *sphead;
  class sp_name *spname;
  class sp_variable *spvar;
  class With_clause *with_clause;
  class Virtual_column_info *virtual_column;

  handlerton *db_type;
  st_select_lex *select_lex;
  st_select_lex_unit *select_lex_unit;
  struct p_elem_val *p_elem_value;
  class Window_frame *window_frame;
  class Window_frame_bound *window_frame_bound;
  udf_func *udf;
  st_trg_execution_order trg_execution_order;

  /* enums */
  enum enum_view_suid view_suid;
  enum Condition_information_item::Name cond_info_item_name;
  enum enum_diag_condition_item_name diag_condition_item_name;
  enum Diagnostics_information::Which_area diag_area;
  enum Field::geometry_type geom_type;
  enum enum_fk_option m_fk_option;
  enum Item_udftype udf_type;
  enum Key::Keytype key_type;
  enum Statement_information_item::Name stmt_info_item_name;
  enum enum_filetype filetype;
  enum enum_tx_isolation tx_isolation;
  enum enum_var_type var_type;
  enum enum_yes_no_unknown m_yes_no_unk;
  enum ha_choice choice;
  enum ha_key_alg key_alg;
  enum ha_rkey_function ha_rkey_mode;
  enum index_hint_type index_hint;
  enum interval_type interval, interval_time_st;
  enum row_type row_type;
  enum sp_variable::enum_mode spvar_mode;
  enum thr_lock_type lock_type;
  enum enum_mysql_timestamp_type date_time_type;
  enum Window_frame_bound::Bound_precedence_type bound_precedence_type;
  enum Window_frame::Frame_units frame_units;
  enum Window_frame::Frame_exclusion frame_exclusion;
  enum trigger_order_type trigger_action_order_type;
  DDL_options_st object_ddl_options;
  enum vers_sys_type_t vers_range_unit;
  enum Column_definition::enum_column_versioning vers_column_versioning;
  enum plsql_cursor_attr_t plsql_cursor_attr;
}

%{
bool my_yyoverflow(short **a, YYSTYPE **b, size_t *yystacksize);
%}

%pure-parser                                    /* We have threads */
%parse-param { THD *thd }
%lex-param { THD *thd }
/*
  Currently there are 48 shift/reduce conflicts.
  We should not introduce new conflicts any more.
*/
%expect 48

/*
   Comments for TOKENS.
   For each token, please include in the same line a comment that contains
   the following tags:
   SQL-2011-R : Reserved keyword as per SQL-2011
   SQL-2011-N : Non Reserved keyword as per SQL-2011
   SQL-2003-R : Reserved keyword as per SQL-2003
   SQL-2003-N : Non Reserved keyword as per SQL-2003
   SQL-1999-R : Reserved keyword as per SQL-1999
   SQL-1999-N : Non Reserved keyword as per SQL-1999
   MYSQL      : MySQL extention (unspecified)
   MYSQL-FUNC : MySQL extention, function
   INTERNAL   : Not a real token, lex optimization
   OPERATOR   : SQL operator
   FUTURE-USE : Reserved for future use

   This makes the code grep-able, and helps maintenance.
*/


/*
  Reserved keywords and operators
*/
%token  ABORT_SYM                     /* INTERNAL (used in lex) */
%token  ACCESSIBLE_SYM
%token  ADD                           /* SQL-2003-R */
%token  ALL                           /* SQL-2003-R */
%token  ALTER                         /* SQL-2003-R */
%token  ANALYZE_SYM
%token  AND_AND_SYM                   /* OPERATOR */
%token  AND_SYM                       /* SQL-2003-R */
%token  AS                            /* SQL-2003-R */
%token  ASC                           /* SQL-2003-N */
%token  ASENSITIVE_SYM                /* FUTURE-USE */
%token  BEFORE_SYM                    /* SQL-2003-N */
%token  BETWEEN_SYM                   /* SQL-2003-R */
%token  BIGINT                        /* SQL-2003-R */
%token  BINARY                        /* SQL-2003-R */
%token  BIN_NUM
%token  BIT_AND                       /* MYSQL-FUNC */
%token  BIT_OR                        /* MYSQL-FUNC */
%token  BIT_XOR                       /* MYSQL-FUNC */
%token  BLOB_MARIADB_SYM              /* SQL-2003-R */
%token  BLOB_ORACLE_SYM               /* Oracle-R   */
%token  BODY_ORACLE_SYM               /* Oracle-R   */
%token  BOTH                          /* SQL-2003-R */
%token  BY                            /* SQL-2003-R */
%token  CALL_SYM                      /* SQL-2003-R */
%token  CASCADE                       /* SQL-2003-N */
%token  CASE_SYM                      /* SQL-2003-R */
%token  CAST_SYM                      /* SQL-2003-R */
%token  CHANGE
%token  CHAR_SYM                      /* SQL-2003-R */
%token  CHECK_SYM                     /* SQL-2003-R */
%token  COLLATE_SYM                   /* SQL-2003-R */
%token  CONDITION_SYM                 /* SQL-2003-R, SQL-2008-R */
%token  CONSTRAINT                    /* SQL-2003-R */
%token  CONTINUE_MARIADB_SYM          /* SQL-2003-R, Oracle-R */
%token  CONTINUE_ORACLE_SYM           /* SQL-2003-R, Oracle-R */
%token  CONVERT_SYM                   /* SQL-2003-N */
%token  COUNT_SYM                     /* SQL-2003-N */
%token  CREATE                        /* SQL-2003-R */
%token  CROSS                         /* SQL-2003-R */
%token  CUME_DIST_SYM
%token  CURDATE                       /* MYSQL-FUNC */
%token  CURRENT_USER                  /* SQL-2003-R */
%token  CURRENT_ROLE                  /* SQL-2003-R */
%token  CURSOR_SYM                    /* SQL-2003-R */
%token  CURTIME                       /* MYSQL-FUNC */
%token  DATABASE
%token  DATABASES
%token  DATE_ADD_INTERVAL             /* MYSQL-FUNC */
%token  DATE_SUB_INTERVAL             /* MYSQL-FUNC */
%token  DAY_HOUR_SYM
%token  DAY_MICROSECOND_SYM
%token  DAY_MINUTE_SYM
%token  DAY_SECOND_SYM
%token  DECIMAL_NUM
%token  DECIMAL_SYM                   /* SQL-2003-R */
%token  DECLARE_MARIADB_SYM           /* SQL-2003-R */
%token  DECLARE_ORACLE_SYM            /* Oracle-R   */
%token  DEFAULT                       /* SQL-2003-R */
%token  DELETE_DOMAIN_ID_SYM
%token  DELETE_SYM                    /* SQL-2003-R */
%token  DENSE_RANK_SYM
%token  DESC                          /* SQL-2003-N */
%token  DESCRIBE                      /* SQL-2003-R */
%token  DETERMINISTIC_SYM             /* SQL-2003-R */
%token  DISTINCT                      /* SQL-2003-R */
%token  DIV_SYM
%token  DOUBLE_SYM                    /* SQL-2003-R */
%token  DO_DOMAIN_IDS_SYM
%token  DOT_DOT_SYM
%token  DROP                          /* SQL-2003-R */
%token  DUAL_SYM
%token  EACH_SYM                      /* SQL-2003-R */
%token  ELSE                          /* SQL-2003-R */
%token  ELSEIF_MARIADB_SYM
%token  ELSIF_ORACLE_SYM              /* PLSQL-R    */
%token  ENCLOSED
%token  END_OF_INPUT                  /* INTERNAL */
%token  EQUAL_SYM                     /* OPERATOR */
%token  ESCAPED
%token  EXCEPT_SYM                    /* SQL-2003-R */
%token  EXISTS                        /* SQL-2003-R */
%token  EXTRACT_SYM                   /* SQL-2003-N */
%token  FALSE_SYM                     /* SQL-2003-R */
%token  FETCH_SYM                     /* SQL-2003-R */
%token  FIRST_VALUE_SYM               /* SQL-2011 */
%token  FLOAT_NUM
%token  FLOAT_SYM                     /* SQL-2003-R */
%token  FOREIGN                       /* SQL-2003-R */
%token  FOR_SYM                       /* SQL-2003-R */
%token  FOR_SYSTEM_TIME_SYM           /* INTERNAL */
%token  FROM
%token  FULLTEXT_SYM
%token  GE
%token  GOTO_ORACLE_SYM               /* Oracle-R   */
%token  GRANT                         /* SQL-2003-R */
%token  GROUP_SYM                     /* SQL-2003-R */
%token  GROUP_CONCAT_SYM
%token  LAG_SYM                       /* SQL-2011 */
%token  LEAD_SYM                      /* SQL-2011 */
%token  HAVING                        /* SQL-2003-R */
%token  HEX_NUM
%token  HEX_STRING
%token  HOUR_MICROSECOND_SYM
%token  HOUR_MINUTE_SYM
%token  HOUR_SECOND_SYM
%token  IDENT
%token  IDENT_QUOTED
%token  IF_SYM
%token  IGNORE_DOMAIN_IDS_SYM
%token  IGNORE_SYM
%token  INDEX_SYM
%token  INFILE
%token  INNER_SYM                     /* SQL-2003-R */
%token  INOUT_SYM                     /* SQL-2003-R */
%token  INSENSITIVE_SYM               /* SQL-2003-R */
%token  INSERT                        /* SQL-2003-R */
%token  INTERSECT_SYM                 /* SQL-2003-R */
%token  INTERVAL_SYM                  /* SQL-2003-R */
%token  INTO                          /* SQL-2003-R */
%token  INT_SYM                       /* SQL-2003-R */
%token  IN_SYM                        /* SQL-2003-R */
%token  IS                            /* SQL-2003-R */
%token  ITERATE_SYM
%token  JOIN_SYM                      /* SQL-2003-R */
%token  KEYS
%token  KEY_SYM                       /* SQL-2003-N */
%token  KILL_SYM
%token  LE                            /* OPERATOR */
%token  LEADING                       /* SQL-2003-R */
%token  LEAVE_SYM
%token  LEFT                          /* SQL-2003-R */
%token  LEFT_PAREN_ALT                /* INTERNAL */
%token  LEFT_PAREN_WITH               /* INTERNAL */
%token  LEFT_PAREN_LIKE               /* INTERNAL */
%token  LEX_HOSTNAME
%token  LIKE                          /* SQL-2003-R */
%token  LIMIT
%token  LINEAR_SYM
%token  LINES
%token  LOAD
%token  LOCATOR_SYM                   /* SQL-2003-N */
%token  LOCK_SYM
%token  LONGBLOB
%token  LONGTEXT
%token  LONG_NUM
%token  LONG_SYM
%token  LOOP_SYM
%token  LOW_PRIORITY
%token  MASTER_SSL_VERIFY_SERVER_CERT_SYM
%token  MATCH                         /* SQL-2003-R */
%token  MAX_SYM                       /* SQL-2003-N */
%token  MAXVALUE_SYM                 /* SQL-2003-N */
%token  MEDIAN_SYM
%token  MEDIUMBLOB
%token  MEDIUMINT
%token  MEDIUMTEXT
%token  MINUTE_MICROSECOND_SYM
%token  MINUTE_SECOND_SYM
%token  MIN_SYM                       /* SQL-2003-N */
%token  MODIFIES_SYM                  /* SQL-2003-R */
%token  MOD_SYM                       /* SQL-2003-N */
%token  MYSQL_CONCAT_SYM              /* OPERATOR */
%token  NATURAL                       /* SQL-2003-R */
%token  NCHAR_STRING
%token  NE                            /* OPERATOR */
%token  NEG
%token  NOT2_SYM
%token  NOT_SYM                       /* SQL-2003-R */
%token  NOW_SYM
%token  NO_WRITE_TO_BINLOG
%token  NTILE_SYM
%token  NULL_SYM                      /* SQL-2003-R */
%token  NUM
%token  NUMERIC_SYM                   /* SQL-2003-R */
%token  NTH_VALUE_SYM                 /* SQL-2011 */
%token  ON                            /* SQL-2003-R */
%token  OPTIMIZE
%token  OPTIONALLY
%token  ORACLE_CONCAT_SYM             /* INTERNAL */
%token  OR2_SYM
%token  ORDER_SYM                     /* SQL-2003-R */
%token  OR_SYM                        /* SQL-2003-R */
%token  OTHERS_ORACLE_SYM             /* SQL-2011-N, PLSQL-R */
%token  OUTER
%token  OUTFILE
%token  OUT_SYM                       /* SQL-2003-R */
%token  OVER_SYM
%token  PACKAGE_ORACLE_SYM            /* Oracle-R   */
%token  PAGE_CHECKSUM_SYM
%token  PARAM_MARKER
%token  PARSE_VCOL_EXPR_SYM
%token  PARTITION_SYM                 /* SQL-2003-R */
%token  PERCENT_ORACLE_SYM            /* INTERNAL   */
%token  PERCENT_RANK_SYM
%token  PERCENTILE_CONT_SYM
%token  PERCENTILE_DISC_SYM
%token  POSITION_SYM                  /* SQL-2003-N */
%token  PRECISION                     /* SQL-2003-R */
%token  PRIMARY_SYM                   /* SQL-2003-R */
%token  PROCEDURE_SYM                 /* SQL-2003-R */
%token  PURGE
%token  RAISE_ORACLE_SYM              /* PLSQL-R    */
%token  RANGE_SYM                     /* SQL-2003-R */
%token  RANK_SYM        
%token  READS_SYM                     /* SQL-2003-R */
%token  READ_SYM                      /* SQL-2003-N */
%token  READ_WRITE_SYM
%token  REAL                          /* SQL-2003-R */
%token  RECURSIVE_SYM
%token  REF_SYSTEM_ID_SYM
%token  REFERENCES                    /* SQL-2003-R */
%token  REGEXP
%token  RELEASE_SYM                   /* SQL-2003-R */
%token  RENAME
%token  REPEAT_SYM                    /* MYSQL-FUNC */
%token  REPLACE                       /* MYSQL-FUNC */
%token  REQUIRE_SYM
%token  RESIGNAL_SYM                  /* SQL-2003-R */
%token  RESTRICT
%token  RETURNING_SYM
%token  RETURN_MARIADB_SYM            /* SQL-2003-R, PLSQL-R */
%token  RETURN_ORACLE_SYM             /* SQL-2003-R, PLSQL-R */
%token  REVOKE                        /* SQL-2003-R */
%token  RIGHT                         /* SQL-2003-R */
%token  ROWS_SYM                      /* SQL-2003-R */
%token  ROWTYPE_ORACLE_SYM            /* PLSQL-R    */
%token  ROW_NUMBER_SYM
%token  SECOND_MICROSECOND_SYM
%token  SELECT_SYM                    /* SQL-2003-R */
%token  SENSITIVE_SYM                 /* FUTURE-USE */
%token  SEPARATOR_SYM
%token  SERVER_OPTIONS
%token  SET                           /* SQL-2003-R */
%token  SET_VAR
%token  SHIFT_LEFT                    /* OPERATOR */
%token  SHIFT_RIGHT                   /* OPERATOR */
%token  SHOW
%token  SIGNAL_SYM                    /* SQL-2003-R */
%token  SMALLINT                      /* SQL-2003-R */
%token  SPATIAL_SYM
%token  SPECIFIC_SYM                  /* SQL-2003-R */
%token  SQLEXCEPTION_SYM              /* SQL-2003-R */
%token  SQLSTATE_SYM                  /* SQL-2003-R */
%token  SQLWARNING_SYM                /* SQL-2003-R */
%token  SQL_BIG_RESULT
%token  SQL_SMALL_RESULT
%token  SQL_SYM                       /* SQL-2003-R */
%token  SSL_SYM
%token  STARTING
%token  STATS_AUTO_RECALC_SYM
%token  STATS_PERSISTENT_SYM
%token  STATS_SAMPLE_PAGES_SYM
%token  STDDEV_SAMP_SYM               /* SQL-2003-N */
%token  STD_SYM
%token  STRAIGHT_JOIN
%token  SUBSTRING                     /* SQL-2003-N */
%token  SUM_SYM                       /* SQL-2003-N */
%token  SYSDATE
%token  TABLE_REF_PRIORITY
%token  TABLE_SYM                     /* SQL-2003-R */
%token  TERMINATED
%token  TEXT_STRING
%token  THEN_SYM                      /* SQL-2003-R */
%token  TINYBLOB
%token  TINYINT
%token  TINYTEXT
%token  TO_SYM                        /* SQL-2003-R */
%token  TRAILING                      /* SQL-2003-R */
%token  TRIGGER_SYM                   /* SQL-2003-R */
%token  TRIM                          /* SQL-2003-N */
%token  TRUE_SYM                      /* SQL-2003-R */
%token  ULONGLONG_NUM
%token  UNDERSCORE_CHARSET
%token  UNDO_SYM                      /* FUTURE-USE */
%token  UNION_SYM                     /* SQL-2003-R */
%token  UNIQUE_SYM
%token  UNLOCK_SYM
%token  UNSIGNED
%token  UPDATE_SYM                    /* SQL-2003-R */
%token  USAGE                         /* SQL-2003-N */
%token  USE_SYM
%token  USING                         /* SQL-2003-R */
%token  UTC_DATE_SYM
%token  UTC_TIMESTAMP_SYM
%token  UTC_TIME_SYM
%token  VALUES                        /* SQL-2003-R */
%token  VALUES_IN_SYM
%token  VALUES_LESS_SYM
%token  VARBINARY
%token  VARCHAR                       /* SQL-2003-R */
%token  VARIANCE_SYM
%token  VARYING                       /* SQL-2003-R */
%token  VAR_SAMP_SYM
%token  WHEN_SYM                      /* SQL-2003-R */
%token  WHERE                         /* SQL-2003-R */
%token  WHILE_SYM
%token  WITH                          /* SQL-2003-R */
%token  WITH_CUBE_SYM                 /* INTERNAL */
%token  WITH_ROLLUP_SYM               /* INTERNAL */
%token  WITH_SYSTEM_SYM               /* INTERNAL */
%token  XOR
%token  YEAR_MONTH_SYM
%token  ZEROFILL

%token IMPOSSIBLE_ACTION                /* To avoid warning for yyerrlab1 */


/*
  Keywords that have different reserved status in std/oracle modes.
*/
%token  <kwd>  BODY_MARIADB_SYM              // Oracle-R
%token  <kwd>  ELSEIF_ORACLE_SYM
%token  <kwd>  ELSIF_MARIADB_SYM             // PLSQL-R
%token  <kwd>  EXCEPTION_ORACLE_SYM          // SQL-2003-N, PLSQL-R
%token  <kwd>  GOTO_MARIADB_SYM              // Oracle-R
%token  <kwd>  OTHERS_MARIADB_SYM            // SQL-2011-N, PLSQL-R
%token  <kwd>  PACKAGE_MARIADB_SYM           // Oracle-R
%token  <kwd>  RAISE_MARIADB_SYM             // PLSQL-R
%token  <kwd>  ROWTYPE_MARIADB_SYM           // PLSQL-R

/*
  Non-reserved keywords
*/

%token  <kwd>  ACTION                        /* SQL-2003-N */
%token  <kwd>  ADMIN_SYM                     /* SQL-2003-N */
%token  <kwd>  ADDDATE_SYM                   /* MYSQL-FUNC */
%token  <kwd>  AFTER_SYM                     /* SQL-2003-N */
%token  <kwd>  AGAINST
%token  <kwd>  AGGREGATE_SYM
%token  <kwd>  ALGORITHM_SYM
%token  <kwd>  ALWAYS_SYM
%token  <kwd>  ANY_SYM                       /* SQL-2003-R */
%token  <kwd>  ASCII_SYM                     /* MYSQL-FUNC */
%token  <kwd>  AT_SYM                        /* SQL-2003-R */
%token  <kwd>  ATOMIC_SYM                    /* SQL-2003-R */
%token  <kwd>  AUTHORS_SYM
%token  <kwd>  AUTOEXTEND_SIZE_SYM
%token  <kwd>  AUTO_INC
%token  <kwd>  AUTO_SYM
%token  <kwd>  AVG_ROW_LENGTH
%token  <kwd>  AVG_SYM                       /* SQL-2003-N */
%token  <kwd>  BACKUP_SYM
%token  <kwd>  BEGIN_MARIADB_SYM             /* SQL-2003-R, PLSQL-R */
%token  <kwd>  BEGIN_ORACLE_SYM              /* SQL-2003-R, PLSQL-R */
%token  <kwd>  BINLOG_SYM
%token  <kwd>  BIT_SYM                       /* MYSQL-FUNC */
%token  <kwd>  BLOCK_SYM
%token  <kwd>  BOOL_SYM
%token  <kwd>  BOOLEAN_SYM                   /* SQL-2003-R, PLSQL-R */
%token  <kwd>  BTREE_SYM
%token  <kwd>  BYTE_SYM
%token  <kwd>  CACHE_SYM
%token  <kwd>  CASCADED                      /* SQL-2003-R */
%token  <kwd>  CATALOG_NAME_SYM              /* SQL-2003-N */
%token  <kwd>  CHAIN_SYM                     /* SQL-2003-N */
%token  <kwd>  CHANGED
%token  <kwd>  CHARSET
%token  <kwd>  CHECKPOINT_SYM
%token  <kwd>  CHECKSUM_SYM
%token  <kwd>  CIPHER_SYM
%token  <kwd>  CLASS_ORIGIN_SYM              /* SQL-2003-N */
%token  <kwd>  CLIENT_SYM
%token  <kwd>  CLOB_MARIADB_SYM              /* SQL-2003-R */
%token  <kwd>  CLOB_ORACLE_SYM               /* Oracle-R   */
%token  <kwd>  CLOSE_SYM                     /* SQL-2003-R */
%token  <kwd>  COALESCE                      /* SQL-2003-N */
%token  <kwd>  CODE_SYM
%token  <kwd>  COLLATION_SYM                 /* SQL-2003-N */
%token  <kwd>  COLON_ORACLE_SYM              /* INTERNAL   */
%token  <kwd>  COLUMNS
%token  <kwd>  COLUMN_ADD_SYM
%token  <kwd>  COLUMN_CHECK_SYM
%token  <kwd>  COLUMN_CREATE_SYM
%token  <kwd>  COLUMN_DELETE_SYM
%token  <kwd>  COLUMN_GET_SYM
%token  <kwd>  COLUMN_SYM                    /* SQL-2003-R */
%token  <kwd>  COLUMN_NAME_SYM               /* SQL-2003-N */
%token  <kwd>  COMMENT_SYM                   /* Oracle-R   */
%token  <kwd>  COMMITTED_SYM                 /* SQL-2003-N */
%token  <kwd>  COMMIT_SYM                    /* SQL-2003-R */
%token  <kwd>  COMPACT_SYM
%token  <kwd>  COMPLETION_SYM
%token  <kwd>  COMPRESSED_SYM
%token  <kwd>  CONCURRENT
%token  <kwd>  CONNECTION_SYM
%token  <kwd>  CONSISTENT_SYM
%token  <kwd>  CONSTRAINT_CATALOG_SYM        /* SQL-2003-N */
%token  <kwd>  CONSTRAINT_NAME_SYM           /* SQL-2003-N */
%token  <kwd>  CONSTRAINT_SCHEMA_SYM         /* SQL-2003-N */
%token  <kwd>  CONTAINS_SYM                  /* SQL-2003-N */
%token  <kwd>  CONTEXT_SYM
%token  <kwd>  CONTRIBUTORS_SYM
%token  <kwd>  CPU_SYM
%token  <kwd>  CUBE_SYM                      /* SQL-2003-R */
%token  <kwd>  CURRENT_SYM                   /* SQL-2003-R */
%token  <kwd>  CURRENT_POS_SYM
%token  <kwd>  CURSOR_NAME_SYM               /* SQL-2003-N */
%token  <kwd>  CYCLE_SYM
%token  <kwd>  DATAFILE_SYM
%token  <kwd>  DATA_SYM                      /* SQL-2003-N */
%token  <kwd>  DATETIME
%token  <kwd>  DATE_FORMAT_SYM               /* MYSQL-FUNC */
%token  <kwd>  DATE_SYM                      /* SQL-2003-R, Oracle-R, PLSQL-R */
%token  <kwd>  DAY_SYM                       /* SQL-2003-R */
%token  <kwd>  DEALLOCATE_SYM                /* SQL-2003-R */
%token  <kwd>  DECODE_MARIADB_SYM            /* Function, non-reserved */
%token  <kwd>  DECODE_ORACLE_SYM             /* Function, non-reserved */
%token  <kwd>  DEFINER_SYM
%token  <kwd>  DELAYED_SYM
%token  <kwd>  DELAY_KEY_WRITE_SYM
%token  <kwd>  DES_KEY_FILE
%token  <kwd>  DIAGNOSTICS_SYM               /* SQL-2003-N */
%token  <kwd>  DIRECTORY_SYM
%token  <kwd>  DISABLE_SYM
%token  <kwd>  DISCARD
%token  <kwd>  DISK_SYM
%token  <kwd>  DO_SYM
%token  <kwd>  DUMPFILE
%token  <kwd>  DUPLICATE_SYM
%token  <kwd>  DYNAMIC_SYM                   /* SQL-2003-R */
%token  <kwd>  ENABLE_SYM
%token  <kwd>  END                           /* SQL-2003-R, PLSQL-R */
%token  <kwd>  ENDS_SYM
%token  <kwd>  ENGINES_SYM
%token  <kwd>  ENGINE_SYM
%token  <kwd>  ENUM
%token  <kwd>  ERROR_SYM
%token  <kwd>  ERRORS
%token  <kwd>  ESCAPE_SYM                    /* SQL-2003-R */
%token  <kwd>  EVENTS_SYM
%token  <kwd>  EVENT_SYM
%token  <kwd>  EVERY_SYM                     /* SQL-2003-N */
%token  <kwd>  EXCHANGE_SYM
%token  <kwd>  EXAMINED_SYM
%token  <kwd>  EXCLUDE_SYM                   /* SQL-2011-N */
%token  <kwd>  EXECUTE_SYM                   /* SQL-2003-R */
%token  <kwd>  EXCEPTION_MARIADB_SYM         /* SQL-2003-N, PLSQL-R */
%token  <kwd>  EXIT_MARIADB_SYM              /* PLSQL-R */
%token  <kwd>  EXIT_ORACLE_SYM               /* PLSQL-R */
%token  <kwd>  EXPANSION_SYM
%token  <kwd>  EXPORT_SYM
%token  <kwd>  EXTENDED_SYM
%token  <kwd>  EXTENT_SIZE_SYM
%token  <kwd>  FAST_SYM
%token  <kwd>  FAULTS_SYM
%token  <kwd>  FILE_SYM
%token  <kwd>  FIRST_SYM                     /* SQL-2003-N */
%token  <kwd>  FIXED_SYM
%token  <kwd>  FLUSH_SYM
%token  <kwd>  FOLLOWS_SYM                   /* MYSQL trigger*/
%token  <kwd>  FOLLOWING_SYM                 /* SQL-2011-N */
%token  <kwd>  FORCE_SYM
%token  <kwd>  FORMAT_SYM
%token  <kwd>  FOUND_SYM                     /* SQL-2003-R */
%token  <kwd>  FULL                          /* SQL-2003-R */
%token  <kwd>  FUNCTION_SYM                  /* SQL-2003-R, Oracle-R */
%token  <kwd>  GENERAL
%token  <kwd>  GENERATED_SYM
%token  <kwd>  GEOMETRYCOLLECTION
%token  <kwd>  GEOMETRY_SYM
%token  <kwd>  GET_FORMAT                    /* MYSQL-FUNC */
%token  <kwd>  GET_SYM                       /* SQL-2003-R */
%token  <kwd>  GLOBAL_SYM                    /* SQL-2003-R */
%token  <kwd>  GRANTS
%token  <kwd>  HANDLER_SYM
%token  <kwd>  HARD_SYM
%token  <kwd>  HASH_SYM
%token  <kwd>  HELP_SYM
%token  <kwd>  HIGH_PRIORITY
%token  <kwd>  HISTORY_SYM                   /* MYSQL */
%token  <kwd>  HOST_SYM
%token  <kwd>  HOSTS_SYM
%token  <kwd>  HOUR_SYM                      /* SQL-2003-R */
%token  <kwd>  ID_SYM                        /* MYSQL */
%token  <kwd>  IDENTIFIED_SYM
%token  <kwd>  IGNORE_SERVER_IDS_SYM
%token  <kwd>  IMMEDIATE_SYM                 /* SQL-2003-R */
%token  <kwd>  IMPORT
%token  <kwd>  INCREMENT_SYM
%token  <kwd>  INDEXES
%token  <kwd>  INITIAL_SIZE_SYM
%token  <kwd>  INSERT_METHOD
%token  <kwd>  INSTALL_SYM
%token  <kwd>  INVOKER_SYM
%token  <kwd>  IO_SYM
%token  <kwd>  IPC_SYM
%token  <kwd>  ISOLATION                     /* SQL-2003-R */
%token  <kwd>  ISOPEN_SYM                    /* Oracle-N   */
%token  <kwd>  ISSUER_SYM
%token  <kwd>  INVISIBLE_SYM
%token  <kwd>  JSON_SYM
%token  <kwd>  KEY_BLOCK_SIZE
%token  <kwd>  LANGUAGE_SYM                  /* SQL-2003-R */
%token  <kwd>  LAST_SYM                      /* SQL-2003-N */
%token  <kwd>  LAST_VALUE
%token  <kwd>  LASTVAL_SYM                   /* PostgreSQL sequence function */
%token  <kwd>  LEAVES
%token  <kwd>  LESS_SYM
%token  <kwd>  LEVEL_SYM
%token  <kwd>  LINESTRING
%token  <kwd>  LIST_SYM
%token  <kwd>  LOCAL_SYM                     /* SQL-2003-R */
%token  <kwd>  LOCKS_SYM
%token  <kwd>  LOGFILE_SYM
%token  <kwd>  LOGS_SYM
%token  <kwd>  MASTER_CONNECT_RETRY_SYM
%token  <kwd>  MASTER_DELAY_SYM
%token  <kwd>  MASTER_GTID_POS_SYM
%token  <kwd>  MASTER_HOST_SYM
%token  <kwd>  MASTER_LOG_FILE_SYM
%token  <kwd>  MASTER_LOG_POS_SYM
%token  <kwd>  MASTER_PASSWORD_SYM
%token  <kwd>  MASTER_PORT_SYM
%token  <kwd>  MASTER_SERVER_ID_SYM
%token  <kwd>  MASTER_SSL_CAPATH_SYM
%token  <kwd>  MASTER_SSL_CA_SYM
%token  <kwd>  MASTER_SSL_CERT_SYM
%token  <kwd>  MASTER_SSL_CIPHER_SYM
%token  <kwd>  MASTER_SSL_CRL_SYM
%token  <kwd>  MASTER_SSL_CRLPATH_SYM
%token  <kwd>  MASTER_SSL_KEY_SYM
%token  <kwd>  MASTER_SSL_SYM
%token  <kwd>  MASTER_SYM
%token  <kwd>  MASTER_USER_SYM
%token  <kwd>  MASTER_USE_GTID_SYM
%token  <kwd>  MASTER_HEARTBEAT_PERIOD_SYM
%token  <kwd>  MAX_CONNECTIONS_PER_HOUR
%token  <kwd>  MAX_QUERIES_PER_HOUR
%token  <kwd>  MAX_ROWS
%token  <kwd>  MAX_SIZE_SYM
%token  <kwd>  MAX_UPDATES_PER_HOUR
%token  <kwd>  MAX_STATEMENT_TIME_SYM
%token  <kwd>  MAX_USER_CONNECTIONS_SYM
%token  <kwd>  MEDIUM_SYM
%token  <kwd>  MEMORY_SYM
%token  <kwd>  MERGE_SYM                     /* SQL-2003-R */
%token  <kwd>  MESSAGE_TEXT_SYM              /* SQL-2003-N */
%token  <kwd>  MICROSECOND_SYM               /* MYSQL-FUNC */
%token  <kwd>  MIGRATE_SYM
%token  <kwd>  MINUTE_SYM                    /* SQL-2003-R */
%token  <kwd>  MINVALUE_SYM
%token  <kwd>  MIN_ROWS
%token  <kwd>  MODE_SYM
%token  <kwd>  MODIFY_SYM
%token  <kwd>  MONTH_SYM                     /* SQL-2003-R */
%token  <kwd>  MULTILINESTRING
%token  <kwd>  MULTIPOINT
%token  <kwd>  MULTIPOLYGON
%token  <kwd>  MUTEX_SYM
%token  <kwd>  MYSQL_SYM
%token  <kwd>  MYSQL_ERRNO_SYM
%token  <kwd>  NAMES_SYM                     /* SQL-2003-N */
%token  <kwd>  NAME_SYM                      /* SQL-2003-N */
%token  <kwd>  NATIONAL_SYM                  /* SQL-2003-R */
%token  <kwd>  NCHAR_SYM                     /* SQL-2003-R */
%token  <kwd>  NEW_SYM                       /* SQL-2003-R */
%token  <kwd>  NEXT_SYM                      /* SQL-2003-N */
%token  <kwd>  NEXTVAL_SYM                   /* PostgreSQL sequence function */
%token  <kwd>  NOCACHE_SYM
%token  <kwd>  NOCYCLE_SYM
%token  <kwd>  NODEGROUP_SYM
%token  <kwd>  NONE_SYM                      /* SQL-2003-R */
%token  <kwd>  NOTFOUND_SYM                  /* Oracle-R   */
%token  <kwd>  NO_SYM                        /* SQL-2003-R */
%token  <kwd>  NOMAXVALUE_SYM
%token  <kwd>  NOMINVALUE_SYM
%token  <kwd>  NO_WAIT_SYM
%token  <kwd>  NOWAIT_SYM
%token  <kwd>  NUMBER_MARIADB_SYM            /* SQL-2003-N  */
%token  <kwd>  NUMBER_ORACLE_SYM             /* Oracle-R, PLSQL-R */
%token  <kwd>  NVARCHAR_SYM
%token  <kwd>  OF_SYM                        /* SQL-1992-R, Oracle-R */
%token  <kwd>  OFFSET_SYM
%token  <kwd>  OLD_PASSWORD_SYM
%token  <kwd>  ONE_SYM
%token  <kwd>  ONLY_SYM                      /* SQL-2003-R */
%token  <kwd>  ONLINE_SYM
%token  <kwd>  OPEN_SYM                      /* SQL-2003-R */
%token  <kwd>  OPTIONS_SYM
%token  <kwd>  OPTION                        /* SQL-2003-N */
%token  <kwd>  OWNER_SYM
%token  <kwd>  PACK_KEYS_SYM
%token  <kwd>  PAGE_SYM
%token  <kwd>  PARSER_SYM
%token  <kwd>  PARTIAL                       /* SQL-2003-N */
%token  <kwd>  PARTITIONS_SYM
%token  <kwd>  PARTITIONING_SYM
%token  <kwd>  PASSWORD_SYM
%token  <kwd>  PERIOD_SYM                    /* SQL-2011-R */
%token  <kwd>  PERSISTENT_SYM
%token  <kwd>  PHASE_SYM
%token  <kwd>  PLUGINS_SYM
%token  <kwd>  PLUGIN_SYM
%token  <kwd>  POINT_SYM
%token  <kwd>  POLYGON
%token  <kwd>  PORT_SYM
%token  <kwd>  PRECEDES_SYM                  /* MYSQL */
%token  <kwd>  PRECEDING_SYM                 /* SQL-2011-N */
%token  <kwd>  PREPARE_SYM                   /* SQL-2003-R */
%token  <kwd>  PRESERVE_SYM
%token  <kwd>  PREV_SYM
%token  <kwd>  PREVIOUS_SYM
%token  <kwd>  PRIVILEGES                    /* SQL-2003-N */
%token  <kwd>  PROCESS
%token  <kwd>  PROCESSLIST_SYM
%token  <kwd>  PROFILE_SYM
%token  <kwd>  PROFILES_SYM
%token  <kwd>  PROXY_SYM
%token  <kwd>  QUARTER_SYM
%token  <kwd>  QUERY_SYM
%token  <kwd>  QUICK
%token  <kwd>  RAW_MARIADB_SYM
%token  <kwd>  RAW_ORACLE_SYM                /* Oracle-R */
%token  <kwd>  READ_ONLY_SYM
%token  <kwd>  REBUILD_SYM
%token  <kwd>  RECOVER_SYM
%token  <kwd>  REDOFILE_SYM
%token  <kwd>  REDO_BUFFER_SIZE_SYM
%token  <kwd>  REDUNDANT_SYM
%token  <kwd>  RELAY
%token  <kwd>  RELAYLOG_SYM
%token  <kwd>  RELAY_LOG_FILE_SYM
%token  <kwd>  RELAY_LOG_POS_SYM
%token  <kwd>  RELAY_THREAD
%token  <kwd>  RELOAD
%token  <kwd>  REMOVE_SYM
%token  <kwd>  REORGANIZE_SYM
%token  <kwd>  REPAIR
%token  <kwd>  REPEATABLE_SYM                /* SQL-2003-N */
%token  <kwd>  REPLICATION
%token  <kwd>  RESET_SYM
%token  <kwd>  RESTART_SYM
%token  <kwd>  RESOURCES
%token  <kwd>  RESTORE_SYM
%token  <kwd>  RESUME_SYM
%token  <kwd>  RETURNED_SQLSTATE_SYM         /* SQL-2003-N */
%token  <kwd>  RETURNS_SYM                   /* SQL-2003-R */
%token  <kwd>  REUSE_SYM                     /* Oracle-R   */
%token  <kwd>  REVERSE_SYM
%token  <kwd>  ROLE_SYM
%token  <kwd>  ROLLBACK_SYM                  /* SQL-2003-R */
%token  <kwd>  ROLLUP_SYM                    /* SQL-2003-R */
%token  <kwd>  ROUTINE_SYM                   /* SQL-2003-N */
%token  <kwd>  ROWCOUNT_SYM                  /* Oracle-N   */
%token  <kwd>  ROW_SYM                       /* SQL-2003-R */
%token  <kwd>  ROW_COUNT_SYM                 /* SQL-2003-N */
%token  <kwd>  ROW_FORMAT_SYM
%token  <kwd>  RTREE_SYM
%token  <kwd>  SAVEPOINT_SYM                 /* SQL-2003-R */
%token  <kwd>  SCHEDULE_SYM
%token  <kwd>  SCHEMA_NAME_SYM               /* SQL-2003-N */
%token  <kwd>  SECOND_SYM                    /* SQL-2003-R */
%token  <kwd>  SECURITY_SYM                  /* SQL-2003-N */
%token  <kwd>  SEQUENCE_SYM
%token  <kwd>  SERIALIZABLE_SYM              /* SQL-2003-N */
%token  <kwd>  SERIAL_SYM
%token  <kwd>  SESSION_SYM                   /* SQL-2003-N */
%token  <kwd>  SERVER_SYM
%token  <kwd>  SETVAL_SYM                    /* PostgreSQL sequence function */
%token  <kwd>  SHARE_SYM
%token  <kwd>  SHUTDOWN
%token  <kwd>  SIGNED_SYM
%token  <kwd>  SIMPLE_SYM                    /* SQL-2003-N */
%token  <kwd>  SLAVE
%token  <kwd>  SLAVES
%token  <kwd>  SLAVE_POS_SYM
%token  <kwd>  SLOW
%token  <kwd>  SNAPSHOT_SYM
%token  <kwd>  SOCKET_SYM
%token  <kwd>  SOFT_SYM
%token  <kwd>  SONAME_SYM
%token  <kwd>  SOUNDS_SYM
%token  <kwd>  SOURCE_SYM
%token  <kwd>  SQL_BUFFER_RESULT
%token  <kwd>  SQL_CACHE_SYM
%token  <kwd>  SQL_CALC_FOUND_ROWS
%token  <kwd>  SQL_NO_CACHE_SYM
%token  <kwd>  SQL_THREAD
%token  <kwd>  STAGE_SYM
%token  <kwd>  STARTS_SYM
%token  <kwd>  START_SYM                     /* SQL-2003-R */
%token  <kwd>  STATEMENT_SYM
%token  <kwd>  STATUS_SYM
%token  <kwd>  STOP_SYM
%token  <kwd>  STORAGE_SYM
%token  <kwd>  STORED_SYM
%token  <kwd>  STRING_SYM
%token  <kwd>  SUBCLASS_ORIGIN_SYM           /* SQL-2003-N */
%token  <kwd>  SUBDATE_SYM
%token  <kwd>  SUBJECT_SYM
%token  <kwd>  SUBPARTITIONS_SYM
%token  <kwd>  SUBPARTITION_SYM
%token  <kwd>  SUPER_SYM
%token  <kwd>  SUSPEND_SYM
%token  <kwd>  SWAPS_SYM
%token  <kwd>  SWITCHES_SYM
%token  <kwd>  SYSTEM                        /* SQL-2011-R */
%token  <kwd>  SYSTEM_TIME_SYM               /* SQL-2011-R */
%token  <kwd>  TABLES
%token  <kwd>  TABLESPACE
%token  <kwd>  TABLE_CHECKSUM_SYM
%token  <kwd>  TABLE_NAME_SYM                /* SQL-2003-N */
%token  <kwd>  TEMPORARY                     /* SQL-2003-N */
%token  <kwd>  TEMPTABLE_SYM
%token  <kwd>  TEXT_SYM
%token  <kwd>  THAN_SYM
%token  <kwd>  TIES_SYM                      /* SQL-2011-N */
%token  <kwd>  TIMESTAMP                     /* SQL-2003-R */
%token  <kwd>  TIMESTAMP_ADD
%token  <kwd>  TIMESTAMP_DIFF
%token  <kwd>  TIME_SYM                      /* SQL-2003-R, Oracle-R */
%token  <kwd>  TRANSACTION_SYM
%token  <kwd>  TRANSACTIONAL_SYM
%token  <kwd>  TRIGGERS_SYM
%token  <kwd>  TRIM_ORACLE
%token  <kwd>  TRUNCATE_SYM
%token  <kwd>  TYPES_SYM
%token  <kwd>  TYPE_SYM                      /* SQL-2003-N */
%token  <kwd>  UDF_RETURNS_SYM
%token  <kwd>  UNBOUNDED_SYM                 /* SQL-2011-N */
%token  <kwd>  UNCOMMITTED_SYM               /* SQL-2003-N */
%token  <kwd>  UNDEFINED_SYM
%token  <kwd>  UNDOFILE_SYM
%token  <kwd>  UNDO_BUFFER_SIZE_SYM
%token  <kwd>  UNICODE_SYM
%token  <kwd>  UNINSTALL_SYM
%token  <kwd>  UNKNOWN_SYM                   /* SQL-2003-R */
%token  <kwd>  UNTIL_SYM
%token  <kwd>  UPGRADE_SYM
%token  <kwd>  USER_SYM                      /* SQL-2003-R */
%token  <kwd>  USE_FRM
%token  <kwd>  VALUE_SYM                     /* SQL-2003-R */
%token  <kwd>  VARCHAR2_MARIADB_SYM
%token  <kwd>  VARCHAR2_ORACLE_SYM           /* Oracle-R, PLSQL-R */
%token  <kwd>  VARIABLES
%token  <kwd>  VERSIONING_SYM                /* SQL-2011-R */
%token  <kwd>  VIA_SYM
%token  <kwd>  VIEW_SYM                      /* SQL-2003-N */
%token  <kwd>  VIRTUAL_SYM
%token  <kwd>  WAIT_SYM
%token  <kwd>  WARNINGS
%token  <kwd>  WEEK_SYM
%token  <kwd>  WEIGHT_STRING_SYM
%token  <kwd>  WINDOW_SYM                    /* SQL-2003-R */
%token  <kwd>  WITHIN
%token  <kwd>  WITHOUT                       /* SQL-2003-R */
%token  <kwd>  WORK_SYM                      /* SQL-2003-N */
%token  <kwd>  WRAPPER_SYM
%token  <kwd>  WRITE_SYM                     /* SQL-2003-N */
%token  <kwd>  X509_SYM
%token  <kwd>  XA_SYM
%token  <kwd>  XML_SYM
%token  <kwd>  YEAR_SYM                      /* SQL-2003-R */


%left   JOIN_SYM INNER_SYM STRAIGHT_JOIN CROSS LEFT RIGHT
/* A dummy token to force the priority of table_ref production in a join. */
%left   TABLE_REF_PRIORITY

/*
  Give ESCAPE (in LIKE) a very low precedence.
  This allows the concatenation operator || to be used on the right
  side of "LIKE" with sql_mode=PIPES_AS_CONCAT (without ORACLE):
    SELECT 'ab' LIKE 'a'||'b'||'c';
*/
%left   PREC_BELOW_ESCAPE
%left   ESCAPE_SYM

%left   SET_VAR
%left   OR_SYM OR2_SYM
%left   XOR
%left   AND_SYM AND_AND_SYM

%left   PREC_BELOW_NOT
%left   NOT_SYM

%left   BETWEEN_SYM CASE_SYM WHEN_SYM THEN_SYM ELSE
%left   '=' EQUAL_SYM GE '>' LE '<' NE IS LIKE SOUNDS_SYM REGEXP IN_SYM
%left   '|'
%left   '&'
%left   SHIFT_LEFT SHIFT_RIGHT
%left   '-' '+' ORACLE_CONCAT_SYM
%left   '*' '/' '%' DIV_SYM MOD_SYM
%left   '^'
%left   MYSQL_CONCAT_SYM
%left   NEG '~' NOT2_SYM BINARY
%left   COLLATE_SYM

/*
  Tokens that can change their meaning from identifier to something else
  in certain context.

  - TRANSACTION: identifier, history unit:
      SELECT transaction FROM t1;
      SELECT * FROM t1 FOR SYSTEM_TIME AS OF TRANSACTION @var;

  - TIMESTAMP: identifier, literal, history unit:
      SELECT timestamp FROM t1;
      SELECT TIMESTAMP '2001-01-01 10:20:30';
      SELECT * FROM t1 FOR SYSTEM_TIME AS OF TIMESTAMP CONCAT(@date,' ',@time);

  - PERIOD: identifier, period for system time:
      SELECT period FROM t1;
      ALTER TABLE DROP PERIOD FOR SYSTEM TIME;

  - SYSTEM: identifier, system versioning:
      SELECT system FROM t1;
      ALTER TABLE DROP SYSTEM VERSIONIONG;

  - USER: identifier, user:
      SELECT user FROM t1;
      KILL USER foo;

   Note, we need here only tokens that cause shift/reduce conflicts
   with keyword identifiers. For example:
      opt_clause1: %empty | KEYWORD ... ;
      clause2: opt_clause1 ident;
   KEYWORD can appear both in opt_clause1 and in "ident" through the "keyword"
   rule. So the parser reports a conflict on how to interpret KEYWORD:
     - as a start of non-empty branch in opt_clause1, or
     - as an identifier which follows the empty branch in opt_clause1.

   Example#1:
     alter_list_item:
       DROP opt_column opt_if_exists_table_element field_ident
     | DROP SYSTEM VERSIONING_SYM
   SYSTEM can be a keyword in field_ident, or can be a start of
   SYSTEM VERSIONING.

   Example#2:
     system_time_expr: AS OF_SYM history_point
     history_point: opt_history_unit bit_expr
     opt_history_unit: | TRANSACTION_SYM
   TRANSACTION can be a non-empty history unit, or can be an identifier
   in bit_expr.

   In the grammar below we use %prec to explicitely tell Bison to go
   through the empty branch in the optional rule only when the lookahead
   token does not belong to a small set of selected tokens.

   Tokens NEXT_SYM and PREVIOUS_SYM also change their meaning from
   identifiers to sequence operations when followed by VALUE_SYM:
      SELECT NEXT VALUE FOR s1, PREVIOUS VALUE FOR s1;
   but we don't need to list them here as they do not seem to cause
   conflicts (according to bison -v), as both meanings
   (as identifier, and as a sequence operation) are parts of the same target
   column_default_non_parenthesized_expr, and there are no any optional
   clauses between the start of column_default_non_parenthesized_expr
   and until NEXT_SYM / PREVIOUS_SYM.
*/
%left   PREC_BELOW_IDENTIFIER_OPT_SPECIAL_CASE
%left   TRANSACTION_SYM TIMESTAMP PERIOD_SYM SYSTEM USER


/*
  Tokens that can appear in a token contraction on the second place
  and change the meaning of the previous token.

  - TEXT_STRING: changes the meaning of TIMESTAMP/TIME/DATE
    from identifier to literal:
      SELECT timestamp FROM t1;
      SELECT TIMESTAMP'2001-01-01 00:00:00' FROM t1;

  - Parenthesis: changes the meaning of TIMESTAMP/TIME/DATE
    from identifiers to CAST-alike functions:
      SELECT timestamp FROM t1;
      SELECT timestamp(1) FROM t1;

  - VALUE: changes NEXT and PREVIOUS from identifier to sequence operation:
      SELECT next, previous FROM t1;
      SELECT NEXT VALUE FOR s1, PREVIOUS VALUE FOR s1;

  - VERSIONING: changes SYSTEM from identifier to SYSTEM VERSIONING
      SELECT system FROM t1;
      ALTER TABLE t1 ADD SYSTEM VERSIONING;
*/
%left   PREC_BELOW_CONTRACTION_TOKEN2
%left   TEXT_STRING '(' VALUE_SYM VERSIONING_SYM

%type <lex_str>
        DECIMAL_NUM FLOAT_NUM NUM LONG_NUM
        HEX_NUM HEX_STRING
        LEX_HOSTNAME ULONGLONG_NUM field_ident select_alias ident_or_text
        TEXT_STRING_sys TEXT_STRING_literal
        key_cache_name
        sp_opt_label BIN_NUM TEXT_STRING_filesystem
        opt_constraint constraint opt_ident
        sp_block_label opt_place opt_db

%type <lex_str>
        sp_label

%type <ident_sys>
        IDENT_sys
        ident
        label_ident
        sp_decl_ident
        ident_set_usual_case
        ident_or_empty
        ident_table_alias
        ident_sysvar_name

%type <lex_string_with_metadata>
        TEXT_STRING
        NCHAR_STRING

%type <lex_str_ptr>
        opt_table_alias_clause
        table_alias_clause

%type <ident_cli>
        IDENT
        IDENT_QUOTED
        IDENT_cli
        ident_cli

%type <kwd>
        keyword_data_type
        keyword_ident
        keyword_label
        keyword_set_special_case
        keyword_set_usual_case
        keyword_sp_block_section
        keyword_sp_decl
        keyword_sp_head
        keyword_sp_var_and_label
        keyword_sp_var_not_label
        keyword_sysvar_name
        keyword_sysvar_type
        keyword_table_alias
        keyword_verb_clause

%type <table>
        table_ident table_ident_nodb references xid
        table_ident_opt_wild create_like

%type <qualified_column_ident>
        optionally_qualified_column_ident

%type <simple_string>
        remember_name remember_end
        remember_tok_start remember_tok_end
        wild_and_where

%type <const_simple_string>
        field_length opt_field_length opt_field_length_default_1
        opt_compression_method

%type <string>
        text_string hex_or_bin_String opt_gconcat_separator

%type <type_handler> int_type real_type

%type <Lex_field_type> type_with_opt_collate field_type
        field_type_numeric
        field_type_string
        field_type_lob
        field_type_temporal
        field_type_misc

%type <Lex_dyncol_type> opt_dyncol_type dyncol_type
        numeric_dyncol_type temporal_dyncol_type string_dyncol_type

%type <create_field> field_spec column_def

%type <geom_type> spatial_type

%type <num>
        order_dir lock_option
        udf_type opt_local opt_no_write_to_binlog
        opt_temporary all_or_any opt_distinct opt_glimit_clause
        opt_ignore_leaves fulltext_options union_option
        opt_not
        transaction_access_mode_types
        opt_natural_language_mode opt_query_expansion
        opt_ev_status opt_ev_on_completion ev_on_completion opt_ev_comment
        ev_alter_on_schedule_completion opt_ev_rename_to opt_ev_sql_stmt
        optional_flush_tables_arguments
        opt_time_precision kill_type kill_option int_num
        opt_default_time_precision
        case_stmt_body opt_bin_mod opt_for_system_time_clause
        opt_if_exists_table_element opt_if_not_exists_table_element
        opt_recursive opt_format_xid

%type <object_ddl_options>
        create_or_replace
        opt_if_not_exists
        opt_if_exists

/*
  Bit field of MYSQL_START_TRANS_OPT_* flags.
*/
%type <num> opt_start_transaction_option_list
%type <num> start_transaction_option_list
%type <num> start_transaction_option

%type <m_yes_no_unk>
        opt_chain opt_release

%type <m_fk_option>
        delete_option

%type <ulong_num>
        ulong_num real_ulong_num merge_insert_types
        ws_nweights opt_versioning_interval_start
        ws_level_flag_desc ws_level_flag_reverse ws_level_flags
        opt_ws_levels ws_level_list ws_level_list_item ws_level_number
        ws_level_range ws_level_list_or_range bool

%type <ulonglong_number>
        ulonglong_num real_ulonglong_num size_number

%type <longlong_number>
        longlong_num

%type <choice> choice

%type <lock_type>
        replace_lock_option opt_low_priority insert_lock_option load_data_lock

%type <item>
        literal insert_ident order_ident temporal_literal
        simple_ident expr sum_expr in_sum_expr
        variable variable_aux bool_pri
        predicate bit_expr parenthesized_expr
        table_wild simple_expr column_default_non_parenthesized_expr udf_expr
        primary_expr string_factor_expr mysql_concatenation_expr
        select_sublist_qualified_asterisk
        expr_or_default set_expr_or_default
        geometry_function signed_literal expr_or_literal
        opt_escape
        sp_opt_default
        simple_ident_nospvar
        field_or_var limit_option
        part_func_expr
        window_func_expr
        window_func
        simple_window_func
        inverse_distribution_function
        percentile_function
        inverse_distribution_function_def
        explicit_cursor_attr
        function_call_keyword
        function_call_keyword_timestamp
        function_call_nonkeyword
        function_call_generic
        function_call_conflict kill_expr
        signal_allowed_expr
        simple_target_specification
        condition_number
        reset_lex_expr

%type <item_param> param_marker

%type <item_num>
        NUM_literal

%type <item_basic_constant> text_literal

%type <item_list>
        expr_list opt_udf_expr_list udf_expr_list when_list when_list_opt_else
        ident_list ident_list_arg opt_expr_list
        decode_when_list_oracle

%type <sp_cursor_stmt>
        sp_cursor_stmt_lex
        sp_cursor_stmt

%type <assignment_lex>
        assignment_source_lex
        assignment_source_expr
        for_loop_bound_expr

%type <sp_assignment_lex_list>
        cursor_actual_parameters
        opt_parenthesized_cursor_actual_parameters

%type <var_type>
        option_type opt_var_type opt_var_ident_type

%type <key_type>
        opt_unique constraint_key_type fulltext spatial

%type <key_alg>
        btree_or_rtree opt_key_algorithm_clause opt_USING_key_algorithm

%type <string_list>
        using_list opt_use_partition use_partition

%type <key_part>
        key_part

%type <table_list>
        join_table_list  join_table
        table_factor table_ref esc_table_ref
        table_primary_ident table_primary_derived
        derived_table_list table_reference_list_parens
        nested_table_reference_list join_table_parens
%type <date_time_type> date_time_type;
%type <interval> interval

%type <interval_time_st> interval_time_stamp

%type <db_type> storage_engines known_storage_engines

%type <row_type> row_types

%type <tx_isolation> isolation_types

%type <ha_rkey_mode> handler_rkey_mode

%type <Lex_cast_type> cast_type cast_type_numeric cast_type_temporal

%type <Lex_length_and_dec> precision opt_precision float_options

%type <lex_user> user grant_user grant_role user_or_role current_role
                 admin_option_for_role user_maybe_role

%type <charset>
        opt_collate
        charset_name
        charset_or_alias
        charset_name_or_default
        old_or_new_charset_name
        old_or_new_charset_name_or_default
        collation_name
        collation_name_or_default
        opt_load_data_charset
        UNDERSCORE_CHARSET

%type <select_lex> subselect
        query_specification
        table_value_constructor
        simple_table
        query_primary
        query_primary_parens
        select_into_query_specification


%type <select_lex_unit>
        query_specification_start
        query_expression_body
        query_expression
        query_expression_unit

%type <boolfunc2creator> comp_op

%type <dyncol_def> dyncall_create_element

%type <dyncol_def_list> dyncall_create_list

%type <myvar> select_outvar

%type <virtual_column> opt_check_constraint check_constraint virtual_column_func
        column_default_expr

%type <unit_operation> unit_type_decl

%type <select_lock>
        opt_procedure_or_into
        opt_select_lock_type
        select_lock_type
        opt_lock_wait_timeout_new

%type <select_limit> opt_limit_clause limit_clause limit_options

%type <order_limit_lock>
        query_expression_tail
        order_or_limit
        opt_order_limit_lock

%type <select_order> opt_order_clause order_clause order_list

%type <NONE>
        analyze_stmt_command backup
        query verb_clause create change select select_into
        do drop insert replace insert2
        insert_values update delete truncate rename compound_statement
        show describe load alter optimize keycache preload flush
        reset purge begin_stmt_mariadb commit rollback savepoint release
        slave master_def master_defs master_file_def slave_until_opts
        repair analyze opt_with_admin opt_with_admin_option
        analyze_table_list analyze_table_elem_spec
        opt_persistent_stat_clause persistent_stat_spec
        persistent_column_stat_spec persistent_index_stat_spec
        table_column_list table_index_list table_index_name
        check start checksum
        field_list field_list_item kill key_def constraint_def
        keycache_list keycache_list_or_parts assign_to_keycache
        assign_to_keycache_parts
        preload_list preload_list_or_parts preload_keys preload_keys_parts
        select_item_list select_item values_list no_braces
        delete_limit_clause fields opt_values values
        no_braces_with_names opt_values_with_names values_with_names
        procedure_list procedure_list2 procedure_item
        field_def handler opt_generated_always
        opt_ignore opt_column opt_restrict
        grant revoke set lock unlock string_list field_options
        opt_binary table_lock_list table_lock
        ref_list opt_match_clause opt_on_update_delete use
        opt_delete_options opt_delete_option varchar nchar nvarchar
        opt_outer table_list table_name table_alias_ref_list table_alias_ref
        opt_attribute opt_attribute_list attribute column_list column_list_id
        opt_column_list grant_privileges grant_ident grant_list grant_option
        object_privilege object_privilege_list user_list user_and_role_list
        rename_list table_or_tables
        clear_privileges flush_options flush_option
        opt_flush_lock flush_lock flush_options_list
        equal optional_braces
        opt_mi_check_type opt_to mi_check_types 
        table_to_table_list table_to_table opt_table_list opt_as
        handler_rkey_function handler_read_or_scan
        single_multi table_wild_list table_wild_one opt_wild
        opt_and charset
        select_var_list select_var_list_init help
        opt_extended_describe shutdown
        opt_format_json
        prepare prepare_src execute deallocate
        statement sp_suid
        sp_c_chistics sp_a_chistics sp_chistic sp_c_chistic xa
        opt_field_or_var_spec fields_or_vars opt_load_data_set_spec
        view_list_opt view_list view_select
        trigger_tail sp_tail sf_tail event_tail
        udf_tail create_function_tail create_aggregate_function_tail
        install uninstall partition_entry binlog_base64_event
        normal_key_options normal_key_opts all_key_opt 
        spatial_key_options fulltext_key_options normal_key_opt 
        fulltext_key_opt spatial_key_opt fulltext_key_opts spatial_key_opts
	keep_gcc_happy
        key_using_alg
        part_column_list
        period_for_system_time
        server_def server_options_list server_option
        definer_opt no_definer definer get_diagnostics
        parse_vcol_expr vcol_opt_specifier vcol_opt_attribute
        vcol_opt_attribute_list vcol_attribute
        opt_serial_attribute opt_serial_attribute_list serial_attribute
        explainable_command
        opt_lock_wait_timeout
        opt_delete_gtid_domain
        asrow_attribute
END_OF_INPUT

%type <NONE> call sp_proc_stmts sp_proc_stmts1 sp_proc_stmt
%type <NONE> sp_proc_stmt_statement sp_proc_stmt_return
             sp_proc_stmt_in_returns_clause
%type <NONE> sp_proc_stmt_compound_ok
%type <NONE> sp_proc_stmt_if
%type <NONE> sp_labeled_control sp_unlabeled_control
%type <NONE> sp_labeled_block sp_unlabeled_block sp_unlabeled_block_not_atomic
%type <NONE> sp_proc_stmt_continue_oracle
%type <NONE> sp_proc_stmt_exit_oracle
%type <NONE> sp_proc_stmt_leave
%type <NONE> sp_proc_stmt_iterate
%type <NONE> sp_proc_stmt_goto_oracle
%type <NONE> sp_proc_stmt_open sp_proc_stmt_fetch sp_proc_stmt_close
%type <NONE> case_stmt_specification
%type <NONE> loop_body while_body repeat_body

%type <num> view_algorithm view_check_option
%type <view_suid> view_suid opt_view_suid

%type <plsql_cursor_attr> plsql_cursor_attr

%type <num> sp_decl_idents sp_decl_idents_init_vars
%type <num> sp_handler_type sp_hcond_list
%type <spcondvalue> sp_cond sp_hcond sqlstate signal_value opt_signal_value
%type <spblock> sp_decl_handler
%type <spblock> sp_decls sp_decl sp_decl_body sp_decl_variable_list
%type <spname> sp_name
%type <spvar> sp_param_name sp_param_name_and_type
%type <for_loop> sp_for_loop_index_and_bounds
%type <for_loop_bounds> sp_for_loop_bounds
%type <trim> trim_operands
%type <num> opt_sp_for_loop_direction
%type <spvar_mode> sp_opt_inout
%type <index_hint> index_hint_type
%type <num> index_hint_clause normal_join inner_join
%type <filetype> data_or_xml

%type <NONE> signal_stmt resignal_stmt raise_stmt_oracle
%type <diag_condition_item_name> signal_condition_information_item_name

%type <trg_execution_order> trigger_follows_precedes_clause;
%type <trigger_action_order_type> trigger_action_order;

%type <diag_area> which_area;
%type <diag_info> diagnostics_information;
%type <stmt_info_item> statement_information_item;
%type <stmt_info_item_name> statement_information_item_name;
%type <stmt_info_list> statement_information;
%type <cond_info_item> condition_information_item;
%type <cond_info_item_name> condition_information_item_name;
%type <cond_info_list> condition_information;

%type <spvar_definition> row_field_name row_field_definition
%type <spvar_definition_list> row_field_definition_list row_type_body

%type <NONE> opt_window_clause window_def_list window_def window_spec
%type <lex_str_ptr> window_name
%type <NONE> opt_window_ref opt_window_frame_clause
%type <frame_units> window_frame_units;
%type <NONE> window_frame_extent;
%type <frame_exclusion> opt_window_frame_exclusion;
%type <window_frame_bound> window_frame_start window_frame_bound;

%type <NONE>
        '-' '+' '*' '/' '%' '(' ')'
        ',' '!' '{' '}' '&' '|' AND_SYM OR_SYM BETWEEN_SYM CASE_SYM
        THEN_SYM WHEN_SYM DIV_SYM MOD_SYM OR2_SYM AND_AND_SYM DELETE_SYM
        MYSQL_CONCAT_SYM ORACLE_CONCAT_SYM

%type <with_clause> opt_with_clause with_clause

%type <lex_str_ptr> query_name

%type <lex_str_list> opt_with_column_list

%type <vers_range_unit> opt_history_unit
%type <vers_history_point> history_point
%type <vers_column_versioning> with_or_without_system
%%


/*
  Indentation of grammar rules:

rule: <-- starts at col 1
          rule1a rule1b rule1c <-- starts at col 11
          { <-- starts at col 11
            code <-- starts at col 13, indentation is 2 spaces
          }
        | rule2a rule2b
          {
            code
          }
        ; <-- on a line by itself, starts at col 9

  Also, please do not use any <TAB>, but spaces.
  Having a uniform indentation in this file helps
  code reviews, patches, merges, and make maintenance easier.
  Tip: grep [[:cntrl:]] sql_yacc.yy
  Thanks.
*/

query:
          END_OF_INPUT
          {
            if (!thd->bootstrap &&
              (!(thd->lex->lex_options & OPTION_LEX_FOUND_COMMENT)))
              my_yyabort_error((ER_EMPTY_QUERY, MYF(0)));

            thd->lex->sql_command= SQLCOM_EMPTY_QUERY;
            YYLIP->found_semicolon= NULL;
          }
        | verb_clause
          {
            Lex_input_stream *lip = YYLIP;

            if ((thd->client_capabilities & CLIENT_MULTI_QUERIES) &&
                lip->multi_statements &&
                ! lip->eof())
            {
              /*
                We found a well formed query, and multi queries are allowed:
                - force the parser to stop after the ';'
                - mark the start of the next query for the next invocation
                  of the parser.
              */
              lip->next_state= MY_LEX_END;
              lip->found_semicolon= lip->get_ptr();
            }
            else
            {
              /* Single query, terminated. */
              lip->found_semicolon= NULL;
            }
          }
          ';'
          opt_end_of_input
        | verb_clause END_OF_INPUT
          {
            /* Single query, not terminated. */
            YYLIP->found_semicolon= NULL;
          }
        ;

opt_end_of_input:
          /* empty */
        | END_OF_INPUT
        ;

verb_clause:
          statement
        | begin_stmt_mariadb
        | compound_statement
        ;

/* Verb clauses, except begin and compound_statement */
statement:
          alter
        | analyze
        | analyze_stmt_command
        | backup
        | binlog_base64_event
        | call
        | change
        | check
        | checksum
        | commit
        | create
        | deallocate
        | delete
        | describe
        | do
        | drop
        | execute
        | flush
        | get_diagnostics
        | grant
        | handler
        | help
        | insert
        | install
	| keep_gcc_happy
        | keycache
        | kill
        | load
        | lock
        | optimize
        | parse_vcol_expr
        | partition_entry
        | preload
        | prepare
        | purge
        | raise_stmt_oracle
        | release
        | rename
        | repair
        | replace
        | reset
        | resignal_stmt
        | revoke
        | rollback
        | savepoint
        | select
        | select_into
        | set
        | signal_stmt
        | show
        | shutdown
        | slave
        | start
        | truncate
        | uninstall
        | unlock
        | update
        | use
        | xa
        ;

deallocate:
          deallocate_or_drop PREPARE_SYM ident
          {
            LEX *lex= thd->lex;
            lex->sql_command= SQLCOM_DEALLOCATE_PREPARE;
            lex->prepared_stmt_name= $3;
          }
        ;

deallocate_or_drop:
          DEALLOCATE_SYM
        | DROP
        ;

prepare:
          PREPARE_SYM ident FROM prepare_src
          {
            LEX *lex= thd->lex;
            if (unlikely(lex->table_or_sp_used()))
              my_yyabort_error((ER_SUBQUERIES_NOT_SUPPORTED, MYF(0),
                               "PREPARE..FROM"));
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
            lex->sql_command= SQLCOM_PREPARE;
            lex->prepared_stmt_name= $2;
          }
        ;

prepare_src:
          { Lex->expr_allows_subselect= false; }
          expr
          {
            Lex->prepared_stmt_code= $2;
            Lex->expr_allows_subselect= true;
          }
        ;

execute:
          EXECUTE_SYM ident
          {
            LEX *lex= thd->lex;
            lex->sql_command= SQLCOM_EXECUTE;
            lex->prepared_stmt_name= $2;
          }
          execute_using
          {
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        | EXECUTE_SYM IMMEDIATE_SYM prepare_src
          {
            if (unlikely(Lex->table_or_sp_used()))
              my_yyabort_error((ER_SUBQUERIES_NOT_SUPPORTED, MYF(0),
                               "EXECUTE IMMEDIATE"));
            Lex->sql_command= SQLCOM_EXECUTE_IMMEDIATE;
          }
          execute_using
          {
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        ;

execute_using:
          /* nothing */
        | USING            { Lex->expr_allows_subselect= false; }
          execute_var_list
          {
            if (unlikely(Lex->table_or_sp_used()))
              my_yyabort_error((ER_SUBQUERIES_NOT_SUPPORTED, MYF(0),
                               "EXECUTE..USING"));
            Lex->expr_allows_subselect= true;
          }
        ;

execute_var_list:
          execute_var_list ',' execute_var_ident
        | execute_var_ident
        ;

execute_var_ident:
          expr_or_default
          {
            if (unlikely(Lex->prepared_stmt_params.push_back($1,
                         thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

/* help */

help:
          HELP_SYM
          {
            if (unlikely(Lex->sphead))
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "HELP"));
          }
          ident_or_text
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_HELP;
            lex->help_arg= $3.str;
          }
        ;

/* change master */

change:
          CHANGE MASTER_SYM optional_connection_name TO_SYM
          {
            Lex->sql_command = SQLCOM_CHANGE_MASTER;
          }
          master_defs
          {}
        ;

master_defs:
          master_def
        | master_defs ',' master_def
        ;

master_def:
          MASTER_HOST_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.host = $3.str;
          }
        | MASTER_USER_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.user = $3.str;
          }
        | MASTER_PASSWORD_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.password = $3.str;
          }
        | MASTER_PORT_SYM '=' ulong_num
          {
            Lex->mi.port = $3;
          }
        | MASTER_CONNECT_RETRY_SYM '=' ulong_num
          {
            Lex->mi.connect_retry = $3;
          }
        | MASTER_DELAY_SYM '=' ulong_num
          {
            if ($3 > MASTER_DELAY_MAX)
            {
              my_error(ER_MASTER_DELAY_VALUE_OUT_OF_RANGE, MYF(0),
                       (ulong) $3, (ulong) MASTER_DELAY_MAX);
            }
            else
              Lex->mi.sql_delay = $3;
          }
        | MASTER_SSL_SYM '=' ulong_num
          {
            Lex->mi.ssl= $3 ? 
              LEX_MASTER_INFO::LEX_MI_ENABLE : LEX_MASTER_INFO::LEX_MI_DISABLE;
          }
        | MASTER_SSL_CA_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.ssl_ca= $3.str;
          }
        | MASTER_SSL_CAPATH_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.ssl_capath= $3.str;
          }
        | MASTER_SSL_CERT_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.ssl_cert= $3.str;
          }
        | MASTER_SSL_CIPHER_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.ssl_cipher= $3.str;
          }
        | MASTER_SSL_KEY_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.ssl_key= $3.str;
          }
        | MASTER_SSL_VERIFY_SERVER_CERT_SYM '=' ulong_num
          {
            Lex->mi.ssl_verify_server_cert= $3 ?
              LEX_MASTER_INFO::LEX_MI_ENABLE : LEX_MASTER_INFO::LEX_MI_DISABLE;
          }
        | MASTER_SSL_CRL_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.ssl_crl= $3.str;
          }
        | MASTER_SSL_CRLPATH_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.ssl_crlpath= $3.str;
          }

        | MASTER_HEARTBEAT_PERIOD_SYM '=' NUM_literal
          {
            Lex->mi.heartbeat_period= (float) $3->val_real();
            if (unlikely(Lex->mi.heartbeat_period >
                         SLAVE_MAX_HEARTBEAT_PERIOD) ||
                unlikely(Lex->mi.heartbeat_period < 0.0))
               my_yyabort_error((ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE, MYF(0),
                                 SLAVE_MAX_HEARTBEAT_PERIOD));

            if (unlikely(Lex->mi.heartbeat_period > slave_net_timeout))
            {
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE_MAX,
                                  ER_THD(thd, ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE_MAX));
            }
            if (unlikely(Lex->mi.heartbeat_period < 0.001))
            {
              if (unlikely(Lex->mi.heartbeat_period != 0.0))
              {
                push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                    ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE_MIN,
                                    ER_THD(thd, ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE_MIN));
                Lex->mi.heartbeat_period= 0.0;
              }
              Lex->mi.heartbeat_opt=  LEX_MASTER_INFO::LEX_MI_DISABLE;
            }
            Lex->mi.heartbeat_opt=  LEX_MASTER_INFO::LEX_MI_ENABLE;
          }
        | IGNORE_SERVER_IDS_SYM '=' '(' ignore_server_id_list ')'
          {
            Lex->mi.repl_ignore_server_ids_opt= LEX_MASTER_INFO::LEX_MI_ENABLE;
           }
        | DO_DOMAIN_IDS_SYM '=' '(' do_domain_id_list ')'
          {
            Lex->mi.repl_do_domain_ids_opt= LEX_MASTER_INFO::LEX_MI_ENABLE;
          }
        | IGNORE_DOMAIN_IDS_SYM '=' '(' ignore_domain_id_list ')'
          {
            Lex->mi.repl_ignore_domain_ids_opt= LEX_MASTER_INFO::LEX_MI_ENABLE;
          }
        |
        master_file_def
        ;

ignore_server_id_list:
          /* Empty */
          | ignore_server_id
          | ignore_server_id_list ',' ignore_server_id
        ;

ignore_server_id:
          ulong_num
          {
            insert_dynamic(&Lex->mi.repl_ignore_server_ids, (uchar*) &($1));
          }
          ;

do_domain_id_list:
          /* Empty */
          | do_domain_id
          | do_domain_id_list ',' do_domain_id
        ;

do_domain_id:
          ulong_num
          {
            insert_dynamic(&Lex->mi.repl_do_domain_ids, (uchar*) &($1));
          }
          ;

ignore_domain_id_list:
          /* Empty */
          | ignore_domain_id
          | ignore_domain_id_list ',' ignore_domain_id
        ;

ignore_domain_id:
          ulong_num
          {
            insert_dynamic(&Lex->mi.repl_ignore_domain_ids, (uchar*) &($1));
          }
          ;

master_file_def:
          MASTER_LOG_FILE_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.log_file_name = $3.str;
          }
        | MASTER_LOG_POS_SYM '=' ulonglong_num
          {
            /* 
               If the user specified a value < BIN_LOG_HEADER_SIZE, adjust it
               instead of causing subsequent errors. 
               We need to do it in this file, because only there we know that 
               MASTER_LOG_POS has been explicitly specified. On the contrary
               in change_master() (sql_repl.cc) we cannot distinguish between 0
               (MASTER_LOG_POS explicitly specified as 0) and 0 (unspecified),
               whereas we want to distinguish (specified 0 means "read the binlog
               from 0" (4 in fact), unspecified means "don't change the position
               (keep the preceding value)").
            */
            Lex->mi.pos= MY_MAX(BIN_LOG_HEADER_SIZE, $3);
          }
        | RELAY_LOG_FILE_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.relay_log_name = $3.str;
          }
        | RELAY_LOG_POS_SYM '=' ulong_num
          {
            Lex->mi.relay_log_pos = $3;
            /* Adjust if < BIN_LOG_HEADER_SIZE (same comment as Lex->mi.pos) */
            Lex->mi.relay_log_pos= MY_MAX(BIN_LOG_HEADER_SIZE, Lex->mi.relay_log_pos);
          }
        | MASTER_USE_GTID_SYM '=' CURRENT_POS_SYM
          {
            if (unlikely(Lex->mi.use_gtid_opt != LEX_MASTER_INFO::LEX_GTID_UNCHANGED))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MASTER_use_gtid"));
            Lex->mi.use_gtid_opt= LEX_MASTER_INFO::LEX_GTID_CURRENT_POS;
          }
        | MASTER_USE_GTID_SYM '=' SLAVE_POS_SYM
          {
            if (unlikely(Lex->mi.use_gtid_opt != LEX_MASTER_INFO::LEX_GTID_UNCHANGED))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MASTER_use_gtid"));
            Lex->mi.use_gtid_opt= LEX_MASTER_INFO::LEX_GTID_SLAVE_POS;
          }
        | MASTER_USE_GTID_SYM '=' NO_SYM
          {
            if (unlikely(Lex->mi.use_gtid_opt != LEX_MASTER_INFO::LEX_GTID_UNCHANGED))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MASTER_use_gtid"));
            Lex->mi.use_gtid_opt= LEX_MASTER_INFO::LEX_GTID_NO;
          }
        ;

optional_connection_name:
          /* empty */
          {
            LEX *lex= thd->lex;
            lex->mi.connection_name= null_clex_str;
          }
        | connection_name
        ;

connection_name:
        TEXT_STRING_sys
        {
           Lex->mi.connection_name= $1;
#ifdef HAVE_REPLICATION
           if (unlikely(check_master_connection_name(&$1)))
              my_yyabort_error((ER_WRONG_ARGUMENTS, MYF(0), "MASTER_CONNECTION_NAME"));
#endif
         }
         ;

/* create a table */

create:
          create_or_replace opt_temporary TABLE_SYM opt_if_not_exists
          {
            LEX *lex= thd->lex;
            lex->create_info.init();
            if (lex->main_select_push())
              MYSQL_YYABORT;
            lex->current_select->parsing_place= BEFORE_OPT_LIST;
            if (lex->set_command_with_check(SQLCOM_CREATE_TABLE, $2, $1 | $4))
               MYSQL_YYABORT;
          }
          table_ident
          {
            LEX *lex= thd->lex;
            if (!lex->first_select_lex()->
                  add_table_to_list(thd, $6, NULL, TL_OPTION_UPDATING,
                                    TL_WRITE, MDL_EXCLUSIVE))
              MYSQL_YYABORT;
            lex->alter_info.reset();
            /*
              For CREATE TABLE we should not open the table even if it exists.
              If the table exists, we should either not create it or replace it
            */
            lex->query_tables->open_strategy= TABLE_LIST::OPEN_STUB;
            lex->create_info.default_table_charset= NULL;
            lex->name= null_clex_str;
            lex->create_last_non_select_table= lex->last_table();
          }
          create_body
          {
            LEX *lex= thd->lex;
            if ((lex->create_info.used_fields & HA_CREATE_USED_ENGINE) &&
                !lex->create_info.db_type)
            {
              lex->create_info.use_default_db_type(thd);
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_WARN_USING_OTHER_HANDLER,
                                  ER_THD(thd, ER_WARN_USING_OTHER_HANDLER),
                                  hton_name(lex->create_info.db_type)->str,
                                  $6->table.str);
            }
            create_table_set_open_action_and_adjust_tables(lex);
            Lex->pop_select(); //main select
          }
       | create_or_replace opt_temporary SEQUENCE_SYM opt_if_not_exists table_ident
         {
           LEX *lex= thd->lex;
           if (Lex->main_select_push())
             MYSQL_YYABORT;
           lex->create_info.init();
           if (unlikely(lex->set_command_with_check(SQLCOM_CREATE_SEQUENCE, $2,
                        $1 | $4)))
              MYSQL_YYABORT;

           if (!lex->first_select_lex()->
                 add_table_to_list(thd, $5, NULL, TL_OPTION_UPDATING,
                                   TL_WRITE, MDL_EXCLUSIVE))
             MYSQL_YYABORT;

               /*
                 For CREATE TABLE, an non-existing table is not an error.
                 Instruct open_tables() to just take an MDL lock if the
                 table does not exist.
               */
             lex->alter_info.reset();
             lex->query_tables->open_strategy= TABLE_LIST::OPEN_STUB;
             lex->name= null_clex_str;
             lex->create_last_non_select_table= lex->last_table();
             if (unlikely(!(lex->create_info.seq_create_info=
                            new (thd->mem_root) sequence_definition())))
               MYSQL_YYABORT;
         }
         opt_sequence opt_create_table_options
         {
            LEX *lex= thd->lex;

            if (unlikely(lex->create_info.seq_create_info->check_and_adjust(1)))
            {
              my_error(ER_SEQUENCE_INVALID_DATA, MYF(0),
                       lex->first_select_lex()->table_list.first->db.str,
                       lex->first_select_lex()->table_list.first->
                         table_name.str);
              MYSQL_YYABORT;
            }

            /* No fields specified, generate them */
            if (unlikely(prepare_sequence_fields(thd,
                         &lex->alter_info.create_list)))
               MYSQL_YYABORT;

            /* CREATE SEQUENCE always creates a sequence */
	    Lex->create_info.used_fields|= HA_CREATE_USED_SEQUENCE;
            Lex->create_info.sequence= 1;

            if ((lex->create_info.used_fields & HA_CREATE_USED_ENGINE) &&
                !lex->create_info.db_type)
            {
              lex->create_info.use_default_db_type(thd);
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_WARN_USING_OTHER_HANDLER,
                                  ER_THD(thd, ER_WARN_USING_OTHER_HANDLER),
                                  hton_name(lex->create_info.db_type)->str,
                                  $5->table.str);
            }
            create_table_set_open_action_and_adjust_tables(lex);
            Lex->pop_select(); //main select
          }
        | create_or_replace opt_unique INDEX_SYM opt_if_not_exists
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          ident
          opt_key_algorithm_clause
          ON table_ident
          {
            if (Lex->add_create_index_prepare($9))
              MYSQL_YYABORT;
            if (Lex->add_create_index($2, &$6, $7, $1 | $4))
              MYSQL_YYABORT;
          }
          '(' key_list ')' opt_lock_wait_timeout normal_key_options
          opt_index_lock_algorithm
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace fulltext INDEX_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          opt_if_not_exists ident
          ON table_ident
          {
            if (Lex->add_create_index_prepare($8))
              MYSQL_YYABORT;
            if (Lex->add_create_index($2, &$6, HA_KEY_ALG_UNDEF, $1 | $5))
              MYSQL_YYABORT;
          }
          '(' key_list ')' opt_lock_wait_timeout fulltext_key_options
          opt_index_lock_algorithm
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace spatial INDEX_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          opt_if_not_exists ident
          ON table_ident
          {
            if (Lex->add_create_index_prepare($8))
              MYSQL_YYABORT;
            if (Lex->add_create_index($2, &$6, HA_KEY_ALG_UNDEF, $1 | $5))
              MYSQL_YYABORT;
          }
          '(' key_list ')' opt_lock_wait_timeout spatial_key_options
          opt_index_lock_algorithm
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace DATABASE opt_if_not_exists ident
          {
            Lex->create_info.default_table_charset= NULL;
            Lex->create_info.used_fields= 0;
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          opt_create_database_options
          {
            LEX *lex=Lex;
            if (unlikely(lex->set_command_with_check(SQLCOM_CREATE_DB, 0,
                         $1 | $3)))
               MYSQL_YYABORT;
            lex->name= $4;
            Lex->pop_select(); //main select
          }
        | create_or_replace definer_opt opt_view_suid VIEW_SYM
          opt_if_not_exists table_ident
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            if (Lex->add_create_view(thd, $1 | $5,
                                     DTYPE_ALGORITHM_UNDEFINED, $3, $6))
              MYSQL_YYABORT;
          }
          view_list_opt AS view_select
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace view_algorithm definer_opt opt_view_suid VIEW_SYM
          opt_if_not_exists table_ident
          {
            if (unlikely(Lex->add_create_view(thd, $1 | $6, $2, $4, $7)))
              MYSQL_YYABORT;
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          view_list_opt AS view_select
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace definer_opt TRIGGER_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            Lex->create_info.set($1);
          }
          trigger_tail
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace definer_opt PROCEDURE_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            Lex->create_info.set($1);
          }
          sp_tail
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace definer_opt EVENT_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            Lex->create_info.set($1);
          }
          event_tail
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace definer FUNCTION_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            Lex->create_info.set($1);
          }
          sf_tail
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace definer AGGREGATE_SYM FUNCTION_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            Lex->create_info.set($1);
          }
          sf_tail_aggregate
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace no_definer FUNCTION_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            Lex->create_info.set($1);
          }
          create_function_tail
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace no_definer AGGREGATE_SYM FUNCTION_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            Lex->create_info.set($1);
          }
          create_aggregate_function_tail
          {
            Lex->pop_select(); //main select
          }
        | create_or_replace USER_SYM opt_if_not_exists clear_privileges
          grant_list opt_require_clause opt_resource_options
          {
            if (unlikely(Lex->set_command_with_check(SQLCOM_CREATE_USER,
                                                     $1 | $3)))
              MYSQL_YYABORT;
          }
        | create_or_replace ROLE_SYM opt_if_not_exists
          clear_privileges role_list opt_with_admin
          {
            if (unlikely(Lex->set_command_with_check(SQLCOM_CREATE_ROLE,
                         $1 | $3)))
              MYSQL_YYABORT;
          }
        | CREATE LOGFILE_SYM GROUP_SYM logfile_group_info 
          {
            Lex->alter_tablespace_info->ts_cmd_type= CREATE_LOGFILE_GROUP;
          }
        | CREATE TABLESPACE tablespace_info
          {
            Lex->alter_tablespace_info->ts_cmd_type= CREATE_TABLESPACE;
          }
        | create_or_replace { Lex->set_command(SQLCOM_CREATE_SERVER, $1); }
          server_def
          { }
        ;

sf_tail_not_aggregate:
        sf_tail
        {
          if (unlikely(Lex->sphead->m_flags & sp_head::HAS_AGGREGATE_INSTR))
          {
            my_yyabort_error((ER_NOT_AGGREGATE_FUNCTION, MYF(0)));
          }
          Lex->sphead->set_chistics_agg_type(NOT_AGGREGATE);
        }
        ;

sf_tail_aggregate:
        sf_tail
        {
          if (unlikely(!(Lex->sphead->m_flags & sp_head::HAS_AGGREGATE_INSTR)))
          {
            my_yyabort_error((ER_INVALID_AGGREGATE_FUNCTION, MYF(0)));
          }
          Lex->sphead->set_chistics_agg_type(GROUP_AGGREGATE);
        }
        ;

create_function_tail:
          sf_tail_not_aggregate { }
        | udf_tail { Lex->udf.type= UDFTYPE_FUNCTION; }
        ;

create_aggregate_function_tail:
          sf_tail_aggregate
          { }
        | udf_tail { Lex->udf.type= UDFTYPE_AGGREGATE; }
        ;
opt_sequence:
         /* empty */ { }
        | sequence_defs
        ;

sequence_defs:
          sequence_def
        | sequence_defs sequence_def
        ;

sequence_def:
        MINVALUE_SYM opt_equal longlong_num
          {
            Lex->create_info.seq_create_info->min_value= $3;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_min_value;
          }
        | NO_SYM MINVALUE_SYM
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields & seq_field_used_min_value))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MINVALUE"));
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_min_value;
          }
        | NOMINVALUE_SYM
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields & seq_field_used_min_value))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MINVALUE"));
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_min_value;
          }
        | MAXVALUE_SYM opt_equal longlong_num
          {
           if (unlikely(Lex->create_info.seq_create_info->used_fields &
               seq_field_used_max_value))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MAXVALUE"));
            Lex->create_info.seq_create_info->max_value= $3;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_max_value;
          }
        | NO_SYM MAXVALUE_SYM
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields & seq_field_used_max_value))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MAXVALUE"));
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_max_value;
          }
        | NOMAXVALUE_SYM
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields & seq_field_used_max_value))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MAXVALUE"));
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_max_value;
          }
        | START_SYM opt_with longlong_num
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields &
                         seq_field_used_start))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "START"));
            Lex->create_info.seq_create_info->start= $3;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_start;
          }
        | INCREMENT_SYM opt_by longlong_num
          {
             if (unlikely(Lex->create_info.seq_create_info->used_fields &
                seq_field_used_increment))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "INCREMENT"));
            Lex->create_info.seq_create_info->increment= $3;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_increment;
          }
        | CACHE_SYM opt_equal longlong_num
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields &
                seq_field_used_cache))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "CACHE"));
            Lex->create_info.seq_create_info->cache= $3;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_cache;
          }
        | NOCACHE_SYM
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields &
                seq_field_used_cache))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "CACHE"));
            Lex->create_info.seq_create_info->cache= 0;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_cache;
          }
        | CYCLE_SYM
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields &
                seq_field_used_cycle))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "CYCLE"));
            Lex->create_info.seq_create_info->cycle= 1;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_cycle;
          }
        | NOCYCLE_SYM
          {
            if (unlikely(Lex->create_info.seq_create_info->used_fields &
                seq_field_used_cycle))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "CYCLE"));
            Lex->create_info.seq_create_info->cycle= 0;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_cycle;
          }
        | RESTART_SYM
          {
            if (unlikely(Lex->sql_command != SQLCOM_ALTER_SEQUENCE))
            {
              thd->parse_error(ER_SYNTAX_ERROR, "RESTART");
              YYABORT;
            }
            if (unlikely(Lex->create_info.seq_create_info->used_fields &
                         seq_field_used_restart))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "RESTART"));
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_restart;
          }
        | RESTART_SYM opt_with longlong_num
          {
            if (unlikely(Lex->sql_command != SQLCOM_ALTER_SEQUENCE))
            {
              thd->parse_error(ER_SYNTAX_ERROR, "RESTART");
              YYABORT;
            }
            if (unlikely(Lex->create_info.seq_create_info->used_fields &
                         seq_field_used_restart))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "RESTART"));
            Lex->create_info.seq_create_info->restart= $3;
            Lex->create_info.seq_create_info->used_fields|= seq_field_used_restart | seq_field_used_restart_value;
          }
        ;

server_def:
          SERVER_SYM opt_if_not_exists ident_or_text
          {
            if (unlikely(Lex->add_create_options_with_check($2)))
              MYSQL_YYABORT;
            Lex->server_options.reset($3);
          }
          FOREIGN DATA_SYM WRAPPER_SYM ident_or_text
          OPTIONS_SYM '(' server_options_list ')'
          { Lex->server_options.scheme= $8; }
        ;

server_options_list:
          server_option
        | server_options_list ',' server_option
        ;

server_option:
          USER_SYM TEXT_STRING_sys
          {
            MYSQL_YYABORT_UNLESS(Lex->server_options.username.str == 0);
            Lex->server_options.username= $2;
          }
        | HOST_SYM TEXT_STRING_sys
          {
            MYSQL_YYABORT_UNLESS(Lex->server_options.host.str == 0);
            Lex->server_options.host= $2;
            my_casedn_str(system_charset_info,
		         (char*) Lex->server_options.host.str);
          }
        | DATABASE TEXT_STRING_sys
          {
            MYSQL_YYABORT_UNLESS(Lex->server_options.db.str == 0);
            Lex->server_options.db= $2;
          }
        | OWNER_SYM TEXT_STRING_sys
          {
            MYSQL_YYABORT_UNLESS(Lex->server_options.owner.str == 0);
            Lex->server_options.owner= $2;
          }
        | PASSWORD_SYM TEXT_STRING_sys
          {
            MYSQL_YYABORT_UNLESS(Lex->server_options.password.str == 0);
            Lex->server_options.password= $2;
          }
        | SOCKET_SYM TEXT_STRING_sys
          {
            MYSQL_YYABORT_UNLESS(Lex->server_options.socket.str == 0);
            Lex->server_options.socket= $2;
          }
        | PORT_SYM ulong_num
          {
            Lex->server_options.port= $2;
          }
        ;

event_tail:
          remember_name opt_if_not_exists sp_name
          {
            LEX *lex=Lex;

            lex->stmt_definition_begin= $1;
            if (unlikely(lex->add_create_options_with_check($2)))
              MYSQL_YYABORT;
            if (unlikely(!(lex->event_parse_data=
                           Event_parse_data::new_instance(thd))))
              MYSQL_YYABORT;
            lex->event_parse_data->identifier= $3;
            lex->event_parse_data->on_completion=
                                  Event_parse_data::ON_COMPLETION_DROP;

            lex->sql_command= SQLCOM_CREATE_EVENT;
            /* We need that for disallowing subqueries */
          }
          ON SCHEDULE_SYM ev_schedule_time
          opt_ev_on_completion
          opt_ev_status
          opt_ev_comment
          DO_SYM ev_sql_stmt
          {
            /*
              sql_command is set here because some rules in ev_sql_stmt
              can overwrite it
            */
            Lex->sql_command= SQLCOM_CREATE_EVENT;
          }
        ;

ev_schedule_time:
          EVERY_SYM expr interval
          {
            Lex->event_parse_data->item_expression= $2;
            Lex->event_parse_data->interval= $3;
          }
          ev_starts
          ev_ends
        | AT_SYM expr
          {
            Lex->event_parse_data->item_execute_at= $2;
          }
        ;

opt_ev_status:
          /* empty */ { $$= 0; }
        | ENABLE_SYM
          {
            Lex->event_parse_data->status= Event_parse_data::ENABLED;
            Lex->event_parse_data->status_changed= true;
            $$= 1;
          }
        | DISABLE_SYM ON SLAVE
          {
            Lex->event_parse_data->status= Event_parse_data::SLAVESIDE_DISABLED;
            Lex->event_parse_data->status_changed= true; 
            $$= 1;
          }
        | DISABLE_SYM
          {
            Lex->event_parse_data->status= Event_parse_data::DISABLED;
            Lex->event_parse_data->status_changed= true;
            $$= 1;
          }
        ;

ev_starts:
          /* empty */
          {
            Item *item= new (thd->mem_root) Item_func_now_local(thd, 0);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            Lex->event_parse_data->item_starts= item;
          }
        | STARTS_SYM expr
          {
            Lex->event_parse_data->item_starts= $2;
          }
        ;

ev_ends:
          /* empty */
        | ENDS_SYM expr
          {
            Lex->event_parse_data->item_ends= $2;
          }
        ;

opt_ev_on_completion:
          /* empty */ { $$= 0; }
        | ev_on_completion
        ;

ev_on_completion:
          ON COMPLETION_SYM opt_not PRESERVE_SYM
          {
            Lex->event_parse_data->on_completion= $3
                                    ? Event_parse_data::ON_COMPLETION_DROP
                                    : Event_parse_data::ON_COMPLETION_PRESERVE;
            $$= 1;
          }
        ;

opt_ev_comment:
          /* empty */ { $$= 0; }
        | COMMENT_SYM TEXT_STRING_sys
          {
            Lex->comment= Lex->event_parse_data->comment= $2;
            $$= 1;
          }
        ;

ev_sql_stmt:
          {
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;

            /*
              This stops the following :
              - CREATE EVENT ... DO CREATE EVENT ...;
              - ALTER  EVENT ... DO CREATE EVENT ...;
              - CREATE EVENT ... DO ALTER EVENT DO ....;
              - CREATE PROCEDURE ... BEGIN CREATE EVENT ... END|
              This allows:
              - CREATE EVENT ... DO DROP EVENT yyy;
              - CREATE EVENT ... DO ALTER EVENT yyy;
                (the nested ALTER EVENT can have anything but DO clause)
              - ALTER  EVENT ... DO ALTER EVENT yyy;
                (the nested ALTER EVENT can have anything but DO clause)
              - ALTER  EVENT ... DO DROP EVENT yyy;
              - CREATE PROCEDURE ... BEGIN ALTER EVENT ... END|
                (the nested ALTER EVENT can have anything but DO clause)
              - CREATE PROCEDURE ... BEGIN DROP EVENT ... END|
            */
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_EVENT_RECURSION_FORBIDDEN, MYF(0)));
              
            if (unlikely(!lex->make_sp_head(thd,
                                            lex->event_parse_data->identifier,
                                            &sp_handler_procedure)))
              MYSQL_YYABORT;

            lex->sphead->set_body_start(thd, lip->get_cpp_ptr());
          }
          sp_proc_stmt
          {
            LEX *lex= thd->lex;

            /* return back to the original memory root ASAP */
            lex->sphead->set_stmt_end(thd);
            lex->sphead->restore_thd_mem_root(thd);

            lex->event_parse_data->body_changed= TRUE;
          }
        ;

clear_privileges:
          /* Nothing */
          {
           LEX *lex=Lex;
           lex->users_list.empty();
           lex->columns.empty();
           lex->grant= lex->grant_tot_col= 0;
           lex->all_privileges= 0;
           lex->first_select_lex()->db= null_clex_str;
           lex->ssl_type= SSL_TYPE_NOT_SPECIFIED;
           lex->ssl_cipher= lex->x509_subject= lex->x509_issuer= 0;
           bzero((char *)&(lex->mqh),sizeof(lex->mqh));
         }
        ;

sp_name:
          ident '.' ident
          {
            if (unlikely(!($$= Lex->make_sp_name(thd, &$1, &$3))))
              MYSQL_YYABORT;
          }
        | ident
          {
            if (unlikely(!($$= Lex->make_sp_name(thd, &$1))))
              MYSQL_YYABORT;
          }
        ;

sp_a_chistics:
          /* Empty */ {}
        | sp_a_chistics sp_chistic {}
        ;

sp_c_chistics:
          /* Empty */ {}
        | sp_c_chistics sp_c_chistic {}
        ;

/* Characteristics for both create and alter */
sp_chistic:
          COMMENT_SYM TEXT_STRING_sys
          { Lex->sp_chistics.comment= $2; }
        | LANGUAGE_SYM SQL_SYM
          { /* Just parse it, we only have one language for now. */ }
        | NO_SYM SQL_SYM
          { Lex->sp_chistics.daccess= SP_NO_SQL; }
        | CONTAINS_SYM SQL_SYM
          { Lex->sp_chistics.daccess= SP_CONTAINS_SQL; }
        | READS_SYM SQL_SYM DATA_SYM
          { Lex->sp_chistics.daccess= SP_READS_SQL_DATA; }
        | MODIFIES_SYM SQL_SYM DATA_SYM
          { Lex->sp_chistics.daccess= SP_MODIFIES_SQL_DATA; }
        | sp_suid
          {}
        ;

/* Create characteristics */
sp_c_chistic:
          sp_chistic            { }
        | opt_not DETERMINISTIC_SYM { Lex->sp_chistics.detistic= ! $1; }
        ;

sp_suid:
          SQL_SYM SECURITY_SYM DEFINER_SYM
          {
            Lex->sp_chistics.suid= SP_IS_SUID;
          }
        | SQL_SYM SECURITY_SYM INVOKER_SYM
          {
            Lex->sp_chistics.suid= SP_IS_NOT_SUID;
          }
        ;

call:
          CALL_SYM sp_name
          {
            if (unlikely(Lex->call_statement_start(thd, $2)))
              MYSQL_YYABORT;
          }
          opt_sp_cparam_list {}
        ;

/* CALL parameters */
opt_sp_cparam_list:
          /* Empty */
        | '(' opt_sp_cparams ')'
        ;

opt_sp_cparams:
          /* Empty */
        | sp_cparams
        ;

sp_cparams:
          sp_cparams ',' expr
          {
           Lex->value_list.push_back($3, thd->mem_root);
          }
        | expr
          {
            Lex->value_list.push_back($1, thd->mem_root);
          }
        ;

/* Stored FUNCTION parameter declaration list */
sp_fdparam_list:
          /* Empty */
        | sp_fdparams
        ;

sp_fdparams:
          sp_fdparams ',' sp_param_name_and_type
        | sp_param_name_and_type
        ;

sp_param_name:
          ident
          {
            if (unlikely(!($$= Lex->sp_param_init(&$1))))
              MYSQL_YYABORT;
          }
        ;

sp_param_name_and_type:
          sp_param_name type_with_opt_collate
          {
            if (unlikely(Lex->sp_param_fill_definition($$= $1)))
              MYSQL_YYABORT;
          }
        | sp_param_name TYPE_SYM OF_SYM ident '.' ident
          {
            if (unlikely(Lex->sphead->spvar_fill_type_reference(thd,
                                                                $$= $1, $4,
                                                                $6)))
              MYSQL_YYABORT;
          }
        | sp_param_name TYPE_SYM OF_SYM ident '.' ident '.' ident
          {
            if (unlikely(Lex->sphead->spvar_fill_type_reference(thd, $$= $1,
                                                                $4, $6, $8)))
              MYSQL_YYABORT;
          }
        | sp_param_name ROW_SYM TYPE_SYM OF_SYM ident
          {
            if (unlikely(Lex->sphead->spvar_fill_table_rowtype_reference(thd, $$= $1, $5)))
              MYSQL_YYABORT;
          }
        | sp_param_name ROW_SYM TYPE_SYM OF_SYM ident '.' ident
          {
            if (unlikely(Lex->sphead->spvar_fill_table_rowtype_reference(thd, $$= $1, $5, $7)))
              MYSQL_YYABORT;
          }
        | sp_param_name ROW_SYM row_type_body
          {
            if (unlikely(Lex->sphead->spvar_fill_row(thd, $$= $1, $3)))
              MYSQL_YYABORT;
          }
        ;

/* Stored PROCEDURE parameter declaration list */
sp_pdparam_list:
          /* Empty */
        | sp_pdparams
        ;

sp_pdparams:
          sp_pdparams ',' sp_pdparam
        | sp_pdparam
        ;

sp_pdparam:
          sp_opt_inout sp_param_name_and_type { $2->mode=$1; }
        ;

sp_opt_inout:
          /* Empty */ { $$= sp_variable::MODE_IN; }
        | IN_SYM      { $$= sp_variable::MODE_IN; }
        | OUT_SYM     { $$= sp_variable::MODE_OUT; }
        | INOUT_SYM   { $$= sp_variable::MODE_INOUT; }
        ;

sp_parenthesized_fdparam_list:
          '('
          {
            Lex->sphead->m_param_begin= YYLIP->get_cpp_tok_start() + 1;
          }
          sp_fdparam_list
          ')'
          {
            Lex->sphead->m_param_end= YYLIP->get_cpp_tok_start();
          }
        ;

sp_parenthesized_pdparam_list:
          '('
          {
            Lex->sphead->m_param_begin= YYLIP->get_cpp_tok_start() + 1;
          }
          sp_pdparam_list
          ')'
          {
            Lex->sphead->m_param_end= YYLIP->get_cpp_tok_start();
          }
        ;

sp_proc_stmts:
          /* Empty */ {}
        | sp_proc_stmts  sp_proc_stmt ';'
        ;

sp_proc_stmts1:
          sp_proc_stmt ';' {}
        | sp_proc_stmts1  sp_proc_stmt ';'
        ;

sp_decls:
          /* Empty */
          {
            $$.init();
          }
        | sp_decls sp_decl ';'
          {
            /* We check for declarations out of (standard) order this way
              because letting the grammar rules reflect it caused tricky
               shift/reduce conflicts with the wrong result. (And we get
               better error handling this way.) */
            if (unlikely(Lex->sp_declarations_join(&$$, $1, $2)))
              MYSQL_YYABORT;
          }
        ;

sp_decl:
          DECLARE_MARIADB_SYM sp_decl_body { $$= $2; }
        ;


optionally_qualified_column_ident:
          sp_decl_ident
          {
            if (unlikely(!($$= new (thd->mem_root)
                         Qualified_column_ident(&$1))))
              MYSQL_YYABORT;
          }
        | sp_decl_ident '.' ident
          {
            if (unlikely(!($$= new (thd->mem_root)
                           Qualified_column_ident(&$1, &$3))))
              MYSQL_YYABORT;
          }
        | sp_decl_ident '.' ident '.' ident
          {
            if (unlikely(!($$= new (thd->mem_root)
                           Qualified_column_ident(thd, &$1, &$3, &$5))))
              MYSQL_YYABORT;
          }
        ;

row_field_name:
          ident
          {
            if (unlikely(check_string_char_length(&$1, 0, NAME_CHAR_LEN,
                                                  system_charset_info, 1)))
              my_yyabort_error((ER_TOO_LONG_IDENT, MYF(0), $1.str));
            if (unlikely(!($$= new (thd->mem_root) Spvar_definition())))
              MYSQL_YYABORT;
            Lex->init_last_field($$, &$1, thd->variables.collation_database);
          }
        ;

row_field_definition:
          row_field_name type_with_opt_collate
        ;

row_field_definition_list:
          row_field_definition
          {
            if (unlikely(!($$= new (thd->mem_root) Row_definition_list())) ||
                unlikely($$->push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | row_field_definition_list ',' row_field_definition
          {
            uint unused;
            if (unlikely($1->find_row_field_by_name(&$3->field_name, &unused)))
              my_yyabort_error((ER_DUP_FIELDNAME, MYF(0), $3->field_name.str));
            $$= $1;
            if (unlikely($$->push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

row_type_body:
          '(' row_field_definition_list ')' { $$= $2; }
        ;

sp_decl_idents_init_vars:
          sp_decl_idents
          {
            Lex->sp_variable_declarations_init(thd, $1);
          }
        ;

sp_decl_variable_list:
          sp_decl_idents_init_vars
          type_with_opt_collate
          sp_opt_default
          {
            if (unlikely(Lex->sp_variable_declarations_finalize(thd, $1,
                                                                &Lex->last_field[0],
                                                                $3)))
              MYSQL_YYABORT;
            $$.init_using_vars($1);
          }
        | sp_decl_idents_init_vars
          TYPE_SYM OF_SYM optionally_qualified_column_ident
          sp_opt_default
          {
            if (unlikely(Lex->sp_variable_declarations_with_ref_finalize(thd, $1, $4, $5)))
              MYSQL_YYABORT;
            $$.init_using_vars($1);
          }
        | sp_decl_idents_init_vars
          ROW_SYM TYPE_SYM OF_SYM optionally_qualified_column_ident
          sp_opt_default
          {
            if (unlikely(Lex->sp_variable_declarations_rowtype_finalize(thd, $1, $5, $6)))
              MYSQL_YYABORT;
            $$.init_using_vars($1);
          }
        | sp_decl_idents_init_vars
          ROW_SYM row_type_body
          sp_opt_default
          {
            if (unlikely(Lex->sp_variable_declarations_row_finalize(thd, $1, $3, $4)))
              MYSQL_YYABORT;
            $$.init_using_vars($1);
          }
        ;

sp_decl_body:
          sp_decl_variable_list
        | sp_decl_ident CONDITION_SYM FOR_SYM sp_cond
          {
            if (unlikely(Lex->spcont->declare_condition(thd, &$1, $4)))
              MYSQL_YYABORT;
            $$.vars= $$.hndlrs= $$.curs= 0;
            $$.conds= 1;
          }
        | sp_decl_handler
        | sp_decl_ident CURSOR_SYM
          {
            Lex->sp_block_init(thd);
          }
          opt_parenthesized_cursor_formal_parameters
          FOR_SYM sp_cursor_stmt
          {
            sp_pcontext *param_ctx= Lex->spcont;
            if (unlikely(Lex->sp_block_finalize(thd)))
              MYSQL_YYABORT;
            if (unlikely(Lex->sp_declare_cursor(thd, &$1, $6, param_ctx, true)))
              MYSQL_YYABORT;
            $$.vars= $$.conds= $$.hndlrs= 0;
            $$.curs= 1;
          }
        ;

sp_decl_handler:
          sp_handler_type HANDLER_SYM FOR_SYM
          {
            if (unlikely(Lex->sp_handler_declaration_init(thd, $1)))
              MYSQL_YYABORT;
          }
          sp_hcond_list sp_proc_stmt
          {
            if (unlikely(Lex->sp_handler_declaration_finalize(thd, $1)))
              MYSQL_YYABORT;
            $$.vars= $$.conds= $$.curs= 0;
            $$.hndlrs= 1;
          }
        ;

opt_parenthesized_cursor_formal_parameters:
          /* Empty */
        | '(' sp_fdparams ')'
        ;


sp_cursor_stmt_lex:
          {
            DBUG_ASSERT(thd->lex->sphead);
            if (unlikely(!($$= new (thd->mem_root)
                           sp_lex_cursor(thd, thd->lex))))
              MYSQL_YYABORT;
          }
        ;

sp_cursor_stmt:
          sp_cursor_stmt_lex
          {
            DBUG_ASSERT(thd->free_list == NULL);
            Lex->sphead->reset_lex(thd, $1);
          }
          select
          {
            DBUG_ASSERT(Lex == $1);
            if (unlikely($1->stmt_finalize(thd)) ||
                unlikely($1->sphead->restore_lex(thd)))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

sp_handler_type:
          EXIT_MARIADB_SYM      { $$= sp_handler::EXIT; }
        | CONTINUE_MARIADB_SYM  { $$= sp_handler::CONTINUE; }
        | EXIT_ORACLE_SYM      { $$= sp_handler::EXIT; }
        | CONTINUE_ORACLE_SYM  { $$= sp_handler::CONTINUE; }
       /*| UNDO_SYM      { QQ No yet } */
        ;

sp_hcond_list:
          sp_hcond_element
          { $$= 1; }
        | sp_hcond_list ',' sp_hcond_element
          { $$+= 1; }
        ;

sp_hcond_element:
          sp_hcond
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *ctx= lex->spcont->parent_context();

            if (unlikely(ctx->check_duplicate_handler($1)))
              my_yyabort_error((ER_SP_DUP_HANDLER, MYF(0)));

            sp_instr_hpush_jump *i= (sp_instr_hpush_jump *)sp->last_instruction();
            i->add_condition($1);
          }
        ;

sp_cond:
          ulong_num
          { /* mysql errno */
            if (unlikely($1 == 0))
              my_yyabort_error((ER_WRONG_VALUE, MYF(0), "CONDITION", "0"));
            $$= new (thd->mem_root) sp_condition_value($1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | sqlstate
        ;

sqlstate:
          SQLSTATE_SYM opt_value TEXT_STRING_literal
          { /* SQLSTATE */

            /*
              An error is triggered:
                - if the specified string is not a valid SQLSTATE,
                - or if it represents the completion condition -- it is not
                  allowed to SIGNAL, or declare a handler for the completion
                  condition.
            */
            if (unlikely(!is_sqlstate_valid(&$3) ||
                         is_sqlstate_completion($3.str)))
              my_yyabort_error((ER_SP_BAD_SQLSTATE, MYF(0), $3.str));
            $$= new (thd->mem_root) sp_condition_value($3.str);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_value:
          /* Empty */  {}
        | VALUE_SYM    {}
        ;

sp_hcond:
          sp_cond
          {
            $$= $1;
          }
        | ident /* CONDITION name */
          {
            $$= Lex->spcont->find_declared_or_predefined_condition(thd, &$1);
            if (unlikely($$ == NULL))
              my_yyabort_error((ER_SP_COND_MISMATCH, MYF(0), $1.str));
          }
        | SQLWARNING_SYM /* SQLSTATEs 01??? */
          {
            $$= new (thd->mem_root) sp_condition_value(sp_condition_value::WARNING);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | not FOUND_SYM /* SQLSTATEs 02??? */
          {
            $$= new (thd->mem_root) sp_condition_value(sp_condition_value::NOT_FOUND);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SQLEXCEPTION_SYM /* All other SQLSTATEs */
          {
            $$= new (thd->mem_root) sp_condition_value(sp_condition_value::EXCEPTION);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | OTHERS_ORACLE_SYM /* All other SQLSTATEs */
          {
            $$= new (thd->mem_root) sp_condition_value(sp_condition_value::EXCEPTION);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;


raise_stmt_oracle:
          RAISE_ORACLE_SYM opt_set_signal_information
          {
            if (unlikely(Lex->add_resignal_statement(thd, NULL)))
              MYSQL_YYABORT;
          }
        | RAISE_ORACLE_SYM signal_value opt_set_signal_information
          {
            if (unlikely(Lex->add_signal_statement(thd, $2)))
              MYSQL_YYABORT;
          }
        ;

signal_stmt:
          SIGNAL_SYM signal_value opt_set_signal_information
          {
            if (Lex->add_signal_statement(thd, $2))
              MYSQL_YYABORT;
          }
        ;

signal_value:
          ident
          {
            LEX *lex= Lex;
            sp_condition_value *cond;

            /* SIGNAL foo cannot be used outside of stored programs */
            if (unlikely(lex->spcont == NULL))
              my_yyabort_error((ER_SP_COND_MISMATCH, MYF(0), $1.str));
            cond= lex->spcont->find_declared_or_predefined_condition(thd, &$1);
            if (unlikely(cond == NULL))
              my_yyabort_error((ER_SP_COND_MISMATCH, MYF(0), $1.str));
            if (unlikely(cond->type != sp_condition_value::SQLSTATE))
              my_yyabort_error((ER_SIGNAL_BAD_CONDITION_TYPE, MYF(0)));
            $$= cond;
          }
        | sqlstate
          { $$= $1; }
        ;

opt_signal_value:
          /* empty */
          { $$= NULL; }
        | signal_value
          { $$= $1; }
        ;

opt_set_signal_information:
          /* empty */
          {
            thd->m_parser_state->m_yacc.m_set_signal_info.clear();
          }
        | SET signal_information_item_list
        ;

signal_information_item_list:
          signal_condition_information_item_name '=' signal_allowed_expr
          {
            Set_signal_information *info;
            info= &thd->m_parser_state->m_yacc.m_set_signal_info;
            int index= (int) $1;
            info->clear();
            info->m_item[index]= $3;
          }
        | signal_information_item_list ','
          signal_condition_information_item_name '=' signal_allowed_expr
          {
            Set_signal_information *info;
            info= &thd->m_parser_state->m_yacc.m_set_signal_info;
            int index= (int) $3;
            if (unlikely(info->m_item[index] != NULL))
              my_yyabort_error((ER_DUP_SIGNAL_SET, MYF(0),
                                Diag_condition_item_names[index].str));
            info->m_item[index]= $5;
          }
        ;

/*
  Only a limited subset of <expr> are allowed in SIGNAL/RESIGNAL.
*/
signal_allowed_expr:
          literal
          { $$= $1; }
        | variable
          {
            if ($1->type() == Item::FUNC_ITEM)
            {
              Item_func *item= (Item_func*) $1;
              if (unlikely(item->functype() == Item_func::SUSERVAR_FUNC))
              {
                /*
                  Don't allow the following syntax:
                    SIGNAL/RESIGNAL ...
                    SET <signal condition item name> = @foo := expr
                */
                thd->parse_error();
                MYSQL_YYABORT;
              }
            }
            $$= $1;
          }
        | simple_ident
          { $$= $1; }
        ;

/* conditions that can be set in signal / resignal */
signal_condition_information_item_name:
          CLASS_ORIGIN_SYM
          { $$= DIAG_CLASS_ORIGIN; }
        | SUBCLASS_ORIGIN_SYM
          { $$= DIAG_SUBCLASS_ORIGIN; }
        | CONSTRAINT_CATALOG_SYM
          { $$= DIAG_CONSTRAINT_CATALOG; }
        | CONSTRAINT_SCHEMA_SYM
          { $$= DIAG_CONSTRAINT_SCHEMA; }
        | CONSTRAINT_NAME_SYM
          { $$= DIAG_CONSTRAINT_NAME; }
        | CATALOG_NAME_SYM
          { $$= DIAG_CATALOG_NAME; }
        | SCHEMA_NAME_SYM
          { $$= DIAG_SCHEMA_NAME; }
        | TABLE_NAME_SYM
          { $$= DIAG_TABLE_NAME; }
        | COLUMN_NAME_SYM
          { $$= DIAG_COLUMN_NAME; }
        | CURSOR_NAME_SYM
          { $$= DIAG_CURSOR_NAME; }
        | MESSAGE_TEXT_SYM
          { $$= DIAG_MESSAGE_TEXT; }
        | MYSQL_ERRNO_SYM
          { $$= DIAG_MYSQL_ERRNO; }
        ;

resignal_stmt:
          RESIGNAL_SYM opt_signal_value opt_set_signal_information
          {
            if (unlikely(Lex->add_resignal_statement(thd, $2)))
              MYSQL_YYABORT;
          }
        ;

get_diagnostics:
          GET_SYM which_area DIAGNOSTICS_SYM diagnostics_information
          {
            Diagnostics_information *info= $4;

            info->set_which_da($2);

            Lex->sql_command= SQLCOM_GET_DIAGNOSTICS;
            Lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_get_diagnostics(info);

            if (unlikely(Lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        ;

which_area:
        /* If <which area> is not specified, then CURRENT is implicit. */
          { $$= Diagnostics_information::CURRENT_AREA; }
        | CURRENT_SYM
          { $$= Diagnostics_information::CURRENT_AREA; }
        ;

diagnostics_information:
          statement_information
          {
            $$= new (thd->mem_root) Statement_information($1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | CONDITION_SYM condition_number condition_information
          {
            $$= new (thd->mem_root) Condition_information($2, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

statement_information:
          statement_information_item
          {
            $$= new (thd->mem_root) List<Statement_information_item>;
            if (unlikely($$ == NULL) ||
                unlikely($$->push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | statement_information ',' statement_information_item
          {
            if (unlikely($1->push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

statement_information_item:
          simple_target_specification '=' statement_information_item_name
          {
            $$= new (thd->mem_root) Statement_information_item($3, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

simple_target_specification:
          ident_cli
          {
            if (unlikely(!($$= thd->lex->create_item_for_sp_var(&$1, NULL))))
              MYSQL_YYABORT;
          }
        | '@' ident_or_text
          {
            $$= new (thd->mem_root) Item_func_get_user_var(thd, &$2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

statement_information_item_name:
          NUMBER_MARIADB_SYM
          { $$= Statement_information_item::NUMBER; }
        | NUMBER_ORACLE_SYM
          { $$= Statement_information_item::NUMBER; }
        | ROW_COUNT_SYM
          { $$= Statement_information_item::ROW_COUNT; }
        ;

/*
   Only a limited subset of <expr> are allowed in GET DIAGNOSTICS
   <condition number>, same subset as for SIGNAL/RESIGNAL.
*/
condition_number:
          signal_allowed_expr
          { $$= $1; }
        ;

condition_information:
          condition_information_item
          {
            $$= new (thd->mem_root) List<Condition_information_item>;
            if (unlikely($$ == NULL) ||
                unlikely($$->push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | condition_information ',' condition_information_item
          {
            if (unlikely($1->push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

condition_information_item:
          simple_target_specification '=' condition_information_item_name
          {
            $$= new (thd->mem_root) Condition_information_item($3, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

condition_information_item_name:
          CLASS_ORIGIN_SYM
          { $$= Condition_information_item::CLASS_ORIGIN; }
        | SUBCLASS_ORIGIN_SYM
          { $$= Condition_information_item::SUBCLASS_ORIGIN; }
        | CONSTRAINT_CATALOG_SYM
          { $$= Condition_information_item::CONSTRAINT_CATALOG; }
        | CONSTRAINT_SCHEMA_SYM
          { $$= Condition_information_item::CONSTRAINT_SCHEMA; }
        | CONSTRAINT_NAME_SYM
          { $$= Condition_information_item::CONSTRAINT_NAME; }
        | CATALOG_NAME_SYM
          { $$= Condition_information_item::CATALOG_NAME; }
        | SCHEMA_NAME_SYM
          { $$= Condition_information_item::SCHEMA_NAME; }
        | TABLE_NAME_SYM
          { $$= Condition_information_item::TABLE_NAME; }
        | COLUMN_NAME_SYM
          { $$= Condition_information_item::COLUMN_NAME; }
        | CURSOR_NAME_SYM
          { $$= Condition_information_item::CURSOR_NAME; }
        | MESSAGE_TEXT_SYM
          { $$= Condition_information_item::MESSAGE_TEXT; }
        | MYSQL_ERRNO_SYM
          { $$= Condition_information_item::MYSQL_ERRNO; }
        | RETURNED_SQLSTATE_SYM
          { $$= Condition_information_item::RETURNED_SQLSTATE; }
        ;

sp_decl_ident:
          IDENT_sys
        | keyword_sp_decl
          {
            if (unlikely($$.copy_ident_cli(thd, &$1)))
              MYSQL_YYABORT;
          }
        ;

sp_decl_idents:
          sp_decl_ident
          {
            /* NOTE: field definition is filled in sp_decl section. */

            LEX *lex= Lex;
            sp_pcontext *spc= lex->spcont;

            if (unlikely(spc->find_variable(&$1, TRUE)))
              my_yyabort_error((ER_SP_DUP_VAR, MYF(0), $1.str));
            spc->add_variable(thd, &$1);
            $$= 1;
          }
        | sp_decl_idents ',' ident
          {
            /* NOTE: field definition is filled in sp_decl section. */

            LEX *lex= Lex;
            sp_pcontext *spc= lex->spcont;

            if (unlikely(spc->find_variable(&$3, TRUE)))
              my_yyabort_error((ER_SP_DUP_VAR, MYF(0), $3.str));
            spc->add_variable(thd, &$3);
            $$= $1 + 1;
          }
        ;

sp_opt_default:
          /* Empty */ { $$ = NULL; }
        | DEFAULT expr { $$ = $2; }
        ;

/*
  ps_proc_stmt_in_returns_clause is a statement that is allowed
  in the RETURNS clause of a stored function definition directly,
  without the BEGIN..END  block.
  It should not include any syntax structures starting with '(', to avoid
  shift/reduce conflicts with the rule "field_type" and its sub-rules
  that scan an optional length, like CHAR(1) or YEAR(4).
  See MDEV-9166.
*/
sp_proc_stmt_in_returns_clause:
          sp_proc_stmt_return
        | sp_labeled_block
        | sp_unlabeled_block
        | sp_labeled_control
        | sp_proc_stmt_compound_ok
        ;

sp_proc_stmt:
          sp_proc_stmt_in_returns_clause
        | sp_proc_stmt_statement
        | sp_proc_stmt_continue_oracle
        | sp_proc_stmt_exit_oracle
        | sp_proc_stmt_leave
        | sp_proc_stmt_iterate
        | sp_proc_stmt_goto_oracle
        | sp_proc_stmt_open
        | sp_proc_stmt_fetch
        | sp_proc_stmt_close
        ;

sp_proc_stmt_compound_ok:
          sp_proc_stmt_if
        | case_stmt_specification
        | sp_unlabeled_block_not_atomic
        | sp_unlabeled_control
        ;

sp_proc_stmt_if:
          IF_SYM
          {
            if (unlikely(Lex->maybe_start_compound_statement(thd)))
              MYSQL_YYABORT;
            Lex->sphead->new_cont_backpatch(NULL);
          }
          sp_if END IF_SYM
          { Lex->sphead->do_cont_backpatch(); }
        ;
        
sp_proc_stmt_statement:
          {
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;

            lex->sphead->reset_lex(thd);
            lex->sphead->m_tmp_query= lip->get_tok_start();
          }
          statement
          {
            if (Lex->sp_proc_stmt_statement_finalize(thd, yychar == YYEMPTY) ||
                Lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
          }
        ;


RETURN_ALLMODES_SYM:
          RETURN_MARIADB_SYM
        | RETURN_ORACLE_SYM
        ;

sp_proc_stmt_return:
          RETURN_ALLMODES_SYM
          { Lex->sphead->reset_lex(thd); }
          expr
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            if (unlikely(sp->m_handler->add_instr_freturn(thd, sp, lex->spcont,
                                                          $3, lex)) ||
                unlikely(sp->restore_lex(thd)))
              MYSQL_YYABORT;
          }
        | RETURN_ORACLE_SYM
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            if (unlikely(sp->m_handler->add_instr_preturn(thd, sp,
                                                               lex->spcont)))
              MYSQL_YYABORT;
          }
        ;

reset_lex_expr:
          { Lex->sphead->reset_lex(thd); } expr { $$= $2; }
        ;

sp_proc_stmt_exit_oracle:
          EXIT_ORACLE_SYM
          {
            if (unlikely(Lex->sp_exit_statement(thd, NULL)))
              MYSQL_YYABORT;
          }
        | EXIT_ORACLE_SYM label_ident
          {
            if (unlikely(Lex->sp_exit_statement(thd, &$2, NULL)))
              MYSQL_YYABORT;
          }
        | EXIT_ORACLE_SYM WHEN_SYM reset_lex_expr
          {
            if (unlikely(Lex->sp_exit_statement(thd, $3)) ||
                unlikely(Lex->sphead->restore_lex(thd)))
              MYSQL_YYABORT;
          }
        | EXIT_ORACLE_SYM label_ident WHEN_SYM reset_lex_expr
          {
            if (unlikely(Lex->sp_exit_statement(thd, &$2, $4)) ||
                unlikely(Lex->sphead->restore_lex(thd)))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_continue_oracle:
          CONTINUE_ORACLE_SYM
          {
            if (unlikely(Lex->sp_continue_statement(thd, NULL)))
              MYSQL_YYABORT;
          }
        | CONTINUE_ORACLE_SYM label_ident
          {
            if (unlikely(Lex->sp_continue_statement(thd, &$2, NULL)))
              MYSQL_YYABORT;
          }
        | CONTINUE_ORACLE_SYM WHEN_SYM reset_lex_expr
          {
            if (unlikely(Lex->sp_continue_statement(thd, $3)) ||
                unlikely(Lex->sphead->restore_lex(thd)))
              MYSQL_YYABORT;
          }
        | CONTINUE_ORACLE_SYM label_ident WHEN_SYM reset_lex_expr
          {
            if (unlikely(Lex->sp_continue_statement(thd, &$2, $4)) ||
                unlikely(Lex->sphead->restore_lex(thd)))
              MYSQL_YYABORT;
          }
        ;


sp_proc_stmt_leave:
          LEAVE_SYM label_ident
          {
            if (unlikely(Lex->sp_leave_statement(thd, &$2)))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_iterate:
          ITERATE_SYM label_ident
          {
            if (unlikely(Lex->sp_iterate_statement(thd, &$2)))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_goto_oracle:
          GOTO_ORACLE_SYM label_ident
          {
            if (unlikely(Lex->sp_goto_statement(thd, &$2)))
              MYSQL_YYABORT;
          }
        ;

assignment_source_lex:
          {
            DBUG_ASSERT(Lex->sphead);
            if (unlikely(!($$= new (thd->mem_root)
                           sp_assignment_lex(thd, thd->lex))))
              MYSQL_YYABORT;
          }
        ;

assignment_source_expr:
          assignment_source_lex
          {
            DBUG_ASSERT(thd->free_list == NULL);
            Lex->sphead->reset_lex(thd, $1);
          }
          expr
          {
            DBUG_ASSERT($1 == thd->lex);
            $$= $1;
            $$->sp_lex_in_use= true;
            $$->set_item_and_free_list($3, thd->free_list);
            thd->free_list= NULL;
            if ($$->sphead->restore_lex(thd))
              MYSQL_YYABORT;
          }
        ;

for_loop_bound_expr:
          assignment_source_lex
          {
            Lex->sphead->reset_lex(thd, $1);
            Lex->current_select->parsing_place= FOR_LOOP_BOUND;
          }
          expr
          {
            DBUG_ASSERT($1 == thd->lex);
            $$= $1;
            $$->sp_lex_in_use= true;
            $$->set_item_and_free_list($3, NULL);
            if (unlikely($$->sphead->restore_lex(thd)))
              MYSQL_YYABORT;
            Lex->current_select->parsing_place= NO_MATTER;
          }
        ;

cursor_actual_parameters:
          assignment_source_expr
          {
            if (unlikely(!($$= new (thd->mem_root) List<sp_assignment_lex>)))
              MYSQL_YYABORT;
            $$->push_back($1, thd->mem_root);
          }
        | cursor_actual_parameters ',' assignment_source_expr
          {
            $$= $1;
            $$->push_back($3, thd->mem_root);
          }
        ;

opt_parenthesized_cursor_actual_parameters:
          /* Empty */                        { $$= NULL; }
        | '(' cursor_actual_parameters ')'   { $$= $2; }
        ;

sp_proc_stmt_open:
          OPEN_SYM ident opt_parenthesized_cursor_actual_parameters
          {
            if (unlikely(Lex->sp_open_cursor(thd, &$2, $3)))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_fetch_head:
          FETCH_SYM ident INTO
          {
            if (unlikely(Lex->sp_add_cfetch(thd, &$2)))
              MYSQL_YYABORT;
          }
        | FETCH_SYM FROM ident INTO
          {
            if (unlikely(Lex->sp_add_cfetch(thd, &$3)))
              MYSQL_YYABORT;
          }
       | FETCH_SYM NEXT_SYM FROM ident INTO
          {
            if (unlikely(Lex->sp_add_cfetch(thd, &$4)))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_fetch:
         sp_proc_stmt_fetch_head sp_fetch_list { }
       | FETCH_SYM GROUP_SYM NEXT_SYM ROW_SYM
         {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            lex->sphead->m_flags|= sp_head::HAS_AGGREGATE_INSTR;
            sp_instr_agg_cfetch *i=
              new (thd->mem_root) sp_instr_agg_cfetch(sp->instructions(),
                                                      lex->spcont);
            if (unlikely(i == NULL) ||
                unlikely(sp->add_instr(i)))
              MYSQL_YYABORT;
         }
        ;

sp_proc_stmt_close:
          CLOSE_SYM ident
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            uint offset;
            sp_instr_cclose *i;

            if (unlikely(!lex->spcont->find_cursor(&$2, &offset, false)))
              my_yyabort_error((ER_SP_CURSOR_MISMATCH, MYF(0), $2.str));
            i= new (thd->mem_root)
              sp_instr_cclose(sp->instructions(), lex->spcont,  offset);
            if (unlikely(i == NULL) ||
                unlikely(sp->add_instr(i)))
              MYSQL_YYABORT;
          }
        ;

sp_fetch_list:
          ident
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *spc= lex->spcont;
            sp_variable *spv;

            if (unlikely(!spc || !(spv = spc->find_variable(&$1, false))))
              my_yyabort_error((ER_SP_UNDECLARED_VAR, MYF(0), $1.str));

            /* An SP local variable */
            sp_instr_cfetch *i= (sp_instr_cfetch *)sp->last_instruction();
            i->add_to_varlist(spv);
          }
        | sp_fetch_list ',' ident
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *spc= lex->spcont;
            sp_variable *spv;

            if (unlikely(!spc || !(spv = spc->find_variable(&$3, false))))
              my_yyabort_error((ER_SP_UNDECLARED_VAR, MYF(0), $3.str));

            /* An SP local variable */
            sp_instr_cfetch *i= (sp_instr_cfetch *)sp->last_instruction();
            i->add_to_varlist(spv);
          }
        ;

sp_if:
          { Lex->sphead->reset_lex(thd); }
          expr THEN_SYM
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *ctx= lex->spcont;
            uint ip= sp->instructions();
            sp_instr_jump_if_not *i= new (thd->mem_root)
              sp_instr_jump_if_not(ip, ctx, $2, lex);
            if (unlikely(i == NULL) ||
                unlikely(sp->push_backpatch(thd, i, ctx->push_label(thd, &empty_clex_str, 0))) ||
                unlikely(sp->add_cont_backpatch(i)) ||
                unlikely(sp->add_instr(i)))
              MYSQL_YYABORT;
            if (unlikely(sp->restore_lex(thd)))
              MYSQL_YYABORT;
          }
          sp_proc_stmts1
          {
            sp_head *sp= Lex->sphead;
            sp_pcontext *ctx= Lex->spcont;
            uint ip= sp->instructions();
            sp_instr_jump *i= new (thd->mem_root) sp_instr_jump(ip, ctx);
            if (unlikely(i == NULL) ||
                unlikely(sp->add_instr(i)))
              MYSQL_YYABORT;
            sp->backpatch(ctx->pop_label());
            sp->push_backpatch(thd, i, ctx->push_label(thd, &empty_clex_str, 0));
          }
          sp_elseifs
          {
            LEX *lex= Lex;

            lex->sphead->backpatch(lex->spcont->pop_label());
          }
        ;

sp_elseifs:
          /* Empty */
        | ELSEIF_MARIADB_SYM sp_if
        | ELSE sp_proc_stmts1
        ;

case_stmt_specification:
          CASE_SYM
          {
            if (unlikely(Lex->maybe_start_compound_statement(thd)))
              MYSQL_YYABORT;

            /**
              An example of the CASE statement in use is
            <pre>
            CREATE PROCEDURE proc_19194_simple(i int)
            BEGIN
              DECLARE str CHAR(10);

              CASE i
                WHEN 1 THEN SET str="1";
                WHEN 2 THEN SET str="2";
                WHEN 3 THEN SET str="3";
                ELSE SET str="unknown";
              END CASE;

              SELECT str;
            END
            </pre>
              The actions are used to generate the following code:
            <pre>
            SHOW PROCEDURE CODE proc_19194_simple;
            Pos     Instruction
            0       set str@1 NULL
            1       set_case_expr (12) 0 i@0
            2       jump_if_not 5(12) (case_expr@0 = 1)
            3       set str@1 _latin1'1'
            4       jump 12
            5       jump_if_not 8(12) (case_expr@0 = 2)
            6       set str@1 _latin1'2'
            7       jump 12
            8       jump_if_not 11(12) (case_expr@0 = 3)
            9       set str@1 _latin1'3'
            10      jump 12
            11      set str@1 _latin1'unknown'
            12      stmt 0 "SELECT str"
            </pre>
            */

            Lex->sphead->new_cont_backpatch(NULL);

            /*
              BACKPATCH: Creating target label for the jump to after END CASE
              (instruction 12 in the example)
            */
            Lex->spcont->push_label(thd, &empty_clex_str, Lex->sphead->instructions());
          }
          case_stmt_body
          else_clause_opt
          END
          CASE_SYM
          {
            /*
              BACKPATCH: Resolving forward jump from
              "case_stmt_action_then" to after END CASE
              (jump from instruction 4 to 12, 7 to 12 ... in the example)
            */
            Lex->sphead->backpatch(Lex->spcont->pop_label());

            if ($3)
              Lex->spcont->pop_case_expr_id();

            Lex->sphead->do_cont_backpatch();
          }
        ;

case_stmt_body:
          { Lex->sphead->reset_lex(thd); /* For expr $2 */ }
          expr
          {
            if (unlikely(Lex->case_stmt_action_expr($2)))
              MYSQL_YYABORT;

            if (Lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
          }
          simple_when_clause_list
          { $$= 1; }
        | searched_when_clause_list
          { $$= 0; }
        ;

simple_when_clause_list:
          simple_when_clause
        | simple_when_clause_list simple_when_clause
        ;

searched_when_clause_list:
          searched_when_clause
        | searched_when_clause_list searched_when_clause
        ;

simple_when_clause:
          WHEN_SYM
          {
            Lex->sphead->reset_lex(thd); /* For expr $3 */
          }
          expr
          {
            /* Simple case: <caseval> = <whenval> */

            LEX *lex= Lex;
            if (unlikely(lex->case_stmt_action_when($3, true)))
              MYSQL_YYABORT;
            /* For expr $3 */
            if (unlikely(lex->sphead->restore_lex(thd)))
              MYSQL_YYABORT;
          }
          THEN_SYM
          sp_proc_stmts1
          {
            if (unlikely(Lex->case_stmt_action_then()))
              MYSQL_YYABORT;
          }
        ;

searched_when_clause:
          WHEN_SYM
          {
            Lex->sphead->reset_lex(thd); /* For expr $3 */
          }
          expr
          {
            LEX *lex= Lex;
            if (unlikely(lex->case_stmt_action_when($3, false)))
              MYSQL_YYABORT;
            /* For expr $3 */
            if (unlikely(lex->sphead->restore_lex(thd)))
              MYSQL_YYABORT;
          }
          THEN_SYM
          sp_proc_stmts1
          {
            if (unlikely(Lex->case_stmt_action_then()))
              MYSQL_YYABORT;
          }
        ;

else_clause_opt:
          /* empty */
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            uint ip= sp->instructions();
            sp_instr_error *i= new (thd->mem_root)
              sp_instr_error(ip, lex->spcont, ER_SP_CASE_NOT_FOUND);
            if (unlikely(i == NULL) ||
                unlikely(sp->add_instr(i)))
              MYSQL_YYABORT;
          }
        | ELSE sp_proc_stmts1
        ;

sp_label:
          label_ident ':' { $$= $1; }
        ;

sp_opt_label:
          /* Empty  */  { $$= null_clex_str; }
        | label_ident   { $$= $1; }
        ;

sp_block_label:
          sp_label
          {
            if (unlikely(Lex->spcont->block_label_declare(&$1)))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

sp_labeled_block:
          sp_block_label
          BEGIN_MARIADB_SYM
          {
            Lex->sp_block_init(thd, &$1);
          }
          sp_decls
          sp_proc_stmts
          END
          sp_opt_label
          {
            if (unlikely(Lex->sp_block_finalize(thd, $4, &$7)))
              MYSQL_YYABORT;
          }
        ;

sp_unlabeled_block:
          BEGIN_MARIADB_SYM
          {
            Lex->sp_block_init(thd);
          }
          sp_decls
          sp_proc_stmts
          END
          {
            if (unlikely(Lex->sp_block_finalize(thd, $3)))
              MYSQL_YYABORT;
          }
        ;

sp_unlabeled_block_not_atomic:
          BEGIN_MARIADB_SYM not ATOMIC_SYM /* TODO: BEGIN ATOMIC (not -> opt_not) */
          {
            if (unlikely(Lex->maybe_start_compound_statement(thd)))
              MYSQL_YYABORT;
            Lex->sp_block_init(thd);
          }
          sp_decls
          sp_proc_stmts
          END
          {
            if (unlikely(Lex->sp_block_finalize(thd, $5)))
              MYSQL_YYABORT;
          }
        ;

/* This adds one shift/reduce conflict */
opt_sp_for_loop_direction:
            /* Empty */ { $$= 1; }
          | REVERSE_SYM { $$= -1; }
        ;

sp_for_loop_index_and_bounds:
          ident sp_for_loop_bounds
          {
            if (unlikely(Lex->sp_for_loop_declarations(thd, &$$, &$1, $2)))
              MYSQL_YYABORT;
          }
        ;

sp_for_loop_bounds:
          IN_SYM opt_sp_for_loop_direction for_loop_bound_expr
          DOT_DOT_SYM for_loop_bound_expr
          {
            $$= Lex_for_loop_bounds_intrange($2, $3, $5);
          }
        | IN_SYM opt_sp_for_loop_direction for_loop_bound_expr
          {
            $$.m_direction= $2;
            $$.m_index= $3;
            $$.m_target_bound= NULL;
            $$.m_implicit_cursor= false;
          }
        | IN_SYM opt_sp_for_loop_direction '(' sp_cursor_stmt ')'
          {
            if (unlikely(Lex->sp_for_loop_implicit_cursor_statement(thd, &$$,
                                                                    $4)))
              MYSQL_YYABORT;
          }
        ;

loop_body:
          sp_proc_stmts1 END LOOP_SYM
          {
            LEX *lex= Lex;
            uint ip= lex->sphead->instructions();
            sp_label *lab= lex->spcont->last_label();  /* Jumping back */
            sp_instr_jump *i= new (thd->mem_root)
              sp_instr_jump(ip, lex->spcont, lab->ip);
            if (unlikely(i == NULL) ||
                unlikely(lex->sphead->add_instr(i)))
              MYSQL_YYABORT;
          }
        ;

while_body:
          expr DO_SYM
          {
            LEX *lex= Lex;
            if (unlikely(lex->sp_while_loop_expression(thd, $1)))
              MYSQL_YYABORT;
            if (lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
          }
          sp_proc_stmts1 END WHILE_SYM
          {
            if (unlikely(Lex->sp_while_loop_finalize(thd)))
              MYSQL_YYABORT;
          }
        ;

repeat_body:
          sp_proc_stmts1 UNTIL_SYM 
          { Lex->sphead->reset_lex(thd); }
          expr END REPEAT_SYM
          {
            LEX *lex= Lex;
            uint ip= lex->sphead->instructions();
            sp_label *lab= lex->spcont->last_label();  /* Jumping back */
            sp_instr_jump_if_not *i= new (thd->mem_root)
              sp_instr_jump_if_not(ip, lex->spcont, $4, lab->ip, lex);
            if (unlikely(i == NULL) ||
                unlikely(lex->sphead->add_instr(i)))
              MYSQL_YYABORT;
            if (lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
            /* We can shortcut the cont_backpatch here */
            i->m_cont_dest= ip+1;
          }
        ;

pop_sp_loop_label:
          sp_opt_label
          {
            if (unlikely(Lex->sp_pop_loop_label(thd, &$1)))
              MYSQL_YYABORT;
          }
        ;

sp_labeled_control:
          sp_label LOOP_SYM
          {
            if (unlikely(Lex->sp_push_loop_label(thd, &$1)))
              MYSQL_YYABORT;
          }
          loop_body pop_sp_loop_label
          { }
        | sp_label WHILE_SYM
          {
            if (unlikely(Lex->sp_push_loop_label(thd, &$1)))
              MYSQL_YYABORT;
            Lex->sphead->reset_lex(thd);
          }
          while_body pop_sp_loop_label
          { }
        | sp_label FOR_SYM
          {
            // See "The FOR LOOP statement" comments in sql_lex.cc
            Lex->sp_block_init(thd); // The outer DECLARE..BEGIN..END block
          }
          sp_for_loop_index_and_bounds
          {
            if (unlikely(Lex->sp_push_loop_label(thd, &$1))) // The inner WHILE block
              MYSQL_YYABORT;
            if (unlikely(Lex->sp_for_loop_condition_test(thd, $4)))
              MYSQL_YYABORT;
          }
          DO_SYM
          sp_proc_stmts1
          END FOR_SYM
          {
            if (unlikely(Lex->sp_for_loop_finalize(thd, $4)))
              MYSQL_YYABORT;
          }
          pop_sp_loop_label                    // The inner WHILE block
          {
            if (unlikely(Lex->sp_for_loop_outer_block_finalize(thd, $4)))
              MYSQL_YYABORT;
          }
        | sp_label REPEAT_SYM
          {
            if (unlikely(Lex->sp_push_loop_label(thd, &$1)))
              MYSQL_YYABORT;
          }
          repeat_body pop_sp_loop_label
          { }
        ;

sp_unlabeled_control:
          LOOP_SYM
          {
            if (unlikely(Lex->sp_push_loop_empty_label(thd)))
              MYSQL_YYABORT;
          }
          loop_body
          {
            Lex->sp_pop_loop_empty_label(thd);
          }
        | WHILE_SYM
          {
            if (unlikely(Lex->sp_push_loop_empty_label(thd)))
              MYSQL_YYABORT;
            Lex->sphead->reset_lex(thd);
          }
          while_body
          {
            Lex->sp_pop_loop_empty_label(thd);
          }
        | FOR_SYM
          {
            // See "The FOR LOOP statement" comments in sql_lex.cc
            if (unlikely(Lex->maybe_start_compound_statement(thd)))
              MYSQL_YYABORT;
            Lex->sp_block_init(thd); // The outer DECLARE..BEGIN..END block
          }
          sp_for_loop_index_and_bounds
          {
            if (unlikely(Lex->sp_push_loop_empty_label(thd))) // The inner WHILE block
              MYSQL_YYABORT;
            if (unlikely(Lex->sp_for_loop_condition_test(thd, $3)))
              MYSQL_YYABORT;
          }
          DO_SYM
          sp_proc_stmts1
          END FOR_SYM
          {
            if (unlikely(Lex->sp_for_loop_finalize(thd, $3)))
              MYSQL_YYABORT;
            Lex->sp_pop_loop_empty_label(thd); // The inner WHILE block
            if (unlikely(Lex->sp_for_loop_outer_block_finalize(thd, $3)))
              MYSQL_YYABORT;
          }
        | REPEAT_SYM
          {
            if (unlikely(Lex->sp_push_loop_empty_label(thd)))
              MYSQL_YYABORT;
          }
          repeat_body
          {
            Lex->sp_pop_loop_empty_label(thd);
          }
        ;

trg_action_time:
            BEFORE_SYM
            { Lex->trg_chistics.action_time= TRG_ACTION_BEFORE; }
          | AFTER_SYM
            { Lex->trg_chistics.action_time= TRG_ACTION_AFTER; }
          ;

trg_event:
            INSERT
            { Lex->trg_chistics.event= TRG_EVENT_INSERT; }
          | UPDATE_SYM
            { Lex->trg_chistics.event= TRG_EVENT_UPDATE; }
          | DELETE_SYM
            { Lex->trg_chistics.event= TRG_EVENT_DELETE; }
          ;
/*
  This part of the parser contains common code for all TABLESPACE
  commands.
  CREATE TABLESPACE name ...
  ALTER TABLESPACE name CHANGE DATAFILE ...
  ALTER TABLESPACE name ADD DATAFILE ...
  ALTER TABLESPACE name access_mode
  CREATE LOGFILE GROUP_SYM name ...
  ALTER LOGFILE GROUP_SYM name ADD UNDOFILE ..
  ALTER LOGFILE GROUP_SYM name ADD REDOFILE ..
  DROP TABLESPACE name
  DROP LOGFILE GROUP_SYM name
*/
change_tablespace_access:
          tablespace_name
          ts_access_mode
        ;

change_tablespace_info:
          tablespace_name
          CHANGE ts_datafile
          change_ts_option_list
        ;

tablespace_info:
          tablespace_name
          ADD ts_datafile
          opt_logfile_group_name
          tablespace_option_list
        ;

opt_logfile_group_name:
          /* empty */ {}
        | USE_SYM LOGFILE_SYM GROUP_SYM ident
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->logfile_group_name= $4.str;
          }
        ;

alter_tablespace_info:
          tablespace_name
          ADD ts_datafile
          alter_tablespace_option_list
          { 
            Lex->alter_tablespace_info->ts_alter_tablespace_type= ALTER_TABLESPACE_ADD_FILE; 
          }
        | tablespace_name
          DROP ts_datafile
          alter_tablespace_option_list
          { 
            Lex->alter_tablespace_info->ts_alter_tablespace_type= ALTER_TABLESPACE_DROP_FILE; 
          }
        ;

logfile_group_info:
          logfile_group_name
          add_log_file
          logfile_group_option_list
        ;

alter_logfile_group_info:
          logfile_group_name
          add_log_file
          alter_logfile_group_option_list
        ;

add_log_file:
          ADD lg_undofile
        | ADD lg_redofile
        ;

change_ts_option_list:
          /* empty */ {}
          change_ts_options
        ;

change_ts_options:
          change_ts_option
        | change_ts_options change_ts_option
        | change_ts_options ',' change_ts_option
        ;

change_ts_option:
          opt_ts_initial_size
        | opt_ts_autoextend_size
        | opt_ts_max_size
        ;

tablespace_option_list:
        tablespace_options
        ;

tablespace_options:
          tablespace_option
        | tablespace_options tablespace_option
        | tablespace_options ',' tablespace_option
        ;

tablespace_option:
          opt_ts_initial_size
        | opt_ts_autoextend_size
        | opt_ts_max_size
        | opt_ts_extent_size
        | opt_ts_nodegroup
        | opt_ts_engine
        | ts_wait
        | opt_ts_comment
        ;

alter_tablespace_option_list:
        alter_tablespace_options
        ;

alter_tablespace_options:
          alter_tablespace_option
        | alter_tablespace_options alter_tablespace_option
        | alter_tablespace_options ',' alter_tablespace_option
        ;

alter_tablespace_option:
          opt_ts_initial_size
        | opt_ts_autoextend_size
        | opt_ts_max_size
        | opt_ts_engine
        | ts_wait
        ;

logfile_group_option_list:
        logfile_group_options
        ;

logfile_group_options:
          logfile_group_option
        | logfile_group_options logfile_group_option
        | logfile_group_options ',' logfile_group_option
        ;

logfile_group_option:
          opt_ts_initial_size
        | opt_ts_undo_buffer_size
        | opt_ts_redo_buffer_size
        | opt_ts_nodegroup
        | opt_ts_engine
        | ts_wait
        | opt_ts_comment
        ;

alter_logfile_group_option_list:
          alter_logfile_group_options
        ;

alter_logfile_group_options:
          alter_logfile_group_option
        | alter_logfile_group_options alter_logfile_group_option
        | alter_logfile_group_options ',' alter_logfile_group_option
        ;

alter_logfile_group_option:
          opt_ts_initial_size
        | opt_ts_engine
        | ts_wait
        ;


ts_datafile:
          DATAFILE_SYM TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->data_file_name= $2.str;
          }
        ;

lg_undofile:
          UNDOFILE_SYM TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->undo_file_name= $2.str;
          }
        ;

lg_redofile:
          REDOFILE_SYM TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->redo_file_name= $2.str;
          }
        ;

tablespace_name:
          ident
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info= (new (thd->mem_root)
                                         st_alter_tablespace());
            if (unlikely(lex->alter_tablespace_info == NULL))
              MYSQL_YYABORT;
            lex->alter_tablespace_info->tablespace_name= $1.str;
            lex->sql_command= SQLCOM_ALTER_TABLESPACE;
          }
        ;

logfile_group_name:
          ident
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info= (new (thd->mem_root)
                                         st_alter_tablespace());
            if (unlikely(lex->alter_tablespace_info == NULL))
              MYSQL_YYABORT;
            lex->alter_tablespace_info->logfile_group_name= $1.str;
            lex->sql_command= SQLCOM_ALTER_TABLESPACE;
          }
        ;

ts_access_mode:
          READ_ONLY_SYM
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_access_mode= TS_READ_ONLY;
          }
        | READ_WRITE_SYM
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_access_mode= TS_READ_WRITE;
          }
        | NOT_SYM ACCESSIBLE_SYM
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_access_mode= TS_NOT_ACCESSIBLE;
          }
        ;

opt_ts_initial_size:
          INITIAL_SIZE_SYM opt_equal size_number
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->initial_size= $3;
          }
        ;

opt_ts_autoextend_size:
          AUTOEXTEND_SIZE_SYM opt_equal size_number
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->autoextend_size= $3;
          }
        ;

opt_ts_max_size:
          MAX_SIZE_SYM opt_equal size_number
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->max_size= $3;
          }
        ;

opt_ts_extent_size:
          EXTENT_SIZE_SYM opt_equal size_number
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->extent_size= $3;
          }
        ;

opt_ts_undo_buffer_size:
          UNDO_BUFFER_SIZE_SYM opt_equal size_number
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->undo_buffer_size= $3;
          }
        ;

opt_ts_redo_buffer_size:
          REDO_BUFFER_SIZE_SYM opt_equal size_number
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->redo_buffer_size= $3;
          }
        ;

opt_ts_nodegroup:
          NODEGROUP_SYM opt_equal real_ulong_num
          {
            LEX *lex= Lex;
            if (unlikely(lex->alter_tablespace_info->nodegroup_id != UNDEF_NODEGROUP))
              my_yyabort_error((ER_FILEGROUP_OPTION_ONLY_ONCE,MYF(0),"NODEGROUP"));
            lex->alter_tablespace_info->nodegroup_id= $3;
          }
        ;

opt_ts_comment:
          COMMENT_SYM opt_equal TEXT_STRING_sys
          {
            LEX *lex= Lex;
            if (unlikely(lex->alter_tablespace_info->ts_comment != NULL))
              my_yyabort_error((ER_FILEGROUP_OPTION_ONLY_ONCE,MYF(0),"COMMENT"));
            lex->alter_tablespace_info->ts_comment= $3.str;
          }
        ;

opt_ts_engine:
          opt_storage ENGINE_SYM opt_equal storage_engines
          {
            LEX *lex= Lex;
            if (unlikely(lex->alter_tablespace_info->storage_engine != NULL))
              my_yyabort_error((ER_FILEGROUP_OPTION_ONLY_ONCE, MYF(0),
                                "STORAGE ENGINE"));
            lex->alter_tablespace_info->storage_engine= $4;
          }
        ;

opt_ts_wait:
          /* empty */
        | ts_wait
        ;

ts_wait:
          WAIT_SYM
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->wait_until_completed= TRUE;
          }
        | NO_WAIT_SYM
          {
            LEX *lex= Lex;
            if (unlikely(!(lex->alter_tablespace_info->wait_until_completed)))
              my_yyabort_error((ER_FILEGROUP_OPTION_ONLY_ONCE,MYF(0),"NO_WAIT"));
            lex->alter_tablespace_info->wait_until_completed= FALSE;
          }
        ;

size_number:
          real_ulonglong_num { $$= $1;}
        | IDENT_sys
          {
            if ($1.to_size_number(&$$))
              MYSQL_YYABORT;
          }
        ;

/*
  End tablespace part
*/

create_body:
          create_field_list_parens
          { Lex->create_info.option_list= NULL; }
          opt_create_table_options opt_create_partitioning opt_create_select {}
        | opt_create_table_options opt_create_partitioning opt_create_select {}
        | create_like
          {

            Lex->create_info.add(DDL_options_st::OPT_LIKE);
            TABLE_LIST *src_table= Lex->first_select_lex()->
              add_table_to_list(thd, $1, NULL, 0, TL_READ, MDL_SHARED_READ);
            if (unlikely(! src_table))
              MYSQL_YYABORT;
            /* CREATE TABLE ... LIKE is not allowed for views. */
            src_table->required_type= TABLE_TYPE_NORMAL;
          }
        ;

create_like:
          LIKE table_ident                      { $$= $2; }
        | LEFT_PAREN_LIKE LIKE table_ident ')'  { $$= $3; }
        ;

opt_create_select:
          /* empty */ {}
        | opt_duplicate opt_as create_select_query_expression opt_versioning_option
        ;

create_select_query_expression:
          query_expression
          {
            if (Lex->parsed_insert_select($1->first_select()))
              MYSQL_YYABORT;
          }
        | LEFT_PAREN_WITH with_clause query_expression_body ')'
          {
            SELECT_LEX *first_select= $3->first_select();
            $3->set_with_clause($2);
            $2->attach_to(first_select);
            if (Lex->parsed_insert_select(first_select))
              MYSQL_YYABORT;
          }
        ;

opt_create_partitioning:
          opt_partitioning
          {
            /*
              Remove all tables used in PARTITION clause from the global table
              list. Partitioning with subqueries is not allowed anyway.
            */
            TABLE_LIST *last_non_sel_table= Lex->create_last_non_select_table;
            last_non_sel_table->next_global= 0;
            Lex->query_tables_last= &last_non_sel_table->next_global;
          }
        ;

/*
 This part of the parser is about handling of the partition information.

 It's first version was written by Mikael Ronstrm with lots of answers to
 questions provided by Antony Curtis.

 The partition grammar can be called from three places.
 1) CREATE TABLE ... PARTITION ..
 2) ALTER TABLE table_name PARTITION ...
 3) PARTITION ...

 The first place is called when a new table is created from a MySQL client.
 The second place is called when a table is altered with the ALTER TABLE
 command from a MySQL client.
 The third place is called when opening an frm file and finding partition
 info in the .frm file. It is necessary to avoid allowing PARTITION to be
 an allowed entry point for SQL client queries. This is arranged by setting
 some state variables before arriving here.

 To be able to handle errors we will only set error code in this code
 and handle the error condition in the function calling the parser. This
 is necessary to ensure we can also handle errors when calling the parser
 from the openfrm function.
*/
opt_partitioning:
          /* empty */ {}
        | partitioning
        ;

partitioning:
          PARTITION_SYM have_partitioning
          {
            LEX *lex= Lex;
            lex->part_info= new (thd->mem_root) partition_info();
            if (unlikely(!lex->part_info))
              MYSQL_YYABORT;
            if (lex->sql_command == SQLCOM_ALTER_TABLE)
            {
              lex->alter_info.partition_flags|= ALTER_PARTITION_INFO;
            }
          }
          partition
        ;

have_partitioning:
          /* empty */
          {
#ifdef WITH_PARTITION_STORAGE_ENGINE
            LEX_CSTRING partition_name={STRING_WITH_LEN("partition")};
            if (unlikely(!plugin_is_ready(&partition_name, MYSQL_STORAGE_ENGINE_PLUGIN)))
              my_yyabort_error((ER_OPTION_PREVENTS_STATEMENT, MYF(0),
                                "--skip-partition"));
#else
            my_yyabort_error((ER_FEATURE_DISABLED, MYF(0), "partitioning",
                              "--with-plugin-partition"));
#endif
          }
        ;

partition_entry:
          PARTITION_SYM
          {
            if (unlikely(!Lex->part_info))
            {
              thd->parse_error(ER_PARTITION_ENTRY_ERROR);
              MYSQL_YYABORT;
            }
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            /*
              We enter here when opening the frm file to translate
              partition info string into part_info data structure.
            */
          }
          partition
          {
            Lex->pop_select(); //main select
          }
        ;

partition:
          BY
	  { Lex->safe_to_cache_query= 1; }
	   part_type_def opt_num_parts opt_sub_part part_defs
        ;

part_type_def:
          opt_linear KEY_SYM opt_key_algo '(' part_field_list ')'
          {
            partition_info *part_info= Lex->part_info;
            part_info->list_of_part_fields= TRUE;
            part_info->column_list= FALSE;
            part_info->part_type= HASH_PARTITION;
          }
        | opt_linear HASH_SYM
          { Lex->part_info->part_type= HASH_PARTITION; }
          part_func {}
        | RANGE_SYM part_func
          { Lex->part_info->part_type= RANGE_PARTITION; }
        | RANGE_SYM part_column_list
          { Lex->part_info->part_type= RANGE_PARTITION; }
        | LIST_SYM 
	  {
	    Select->parsing_place= IN_PART_FUNC;
          }
          part_func
          { 
	    Lex->part_info->part_type= LIST_PARTITION; 
	    Select->parsing_place= NO_MATTER;
	  }
        | LIST_SYM part_column_list
          { Lex->part_info->part_type= LIST_PARTITION; }
        | SYSTEM_TIME_SYM
          {
             if (unlikely(Lex->part_info->vers_init_info(thd)))
               MYSQL_YYABORT;
          }
          opt_versioning_rotation
        ;

opt_linear:
          /* empty */ {}
        | LINEAR_SYM
          { Lex->part_info->linear_hash_ind= TRUE;}
        ;

opt_key_algo:
          /* empty */
          { Lex->part_info->key_algorithm= partition_info::KEY_ALGORITHM_NONE;}
        | ALGORITHM_SYM '=' real_ulong_num
          {
            switch ($3) {
            case 1:
              Lex->part_info->key_algorithm= partition_info::KEY_ALGORITHM_51;
              break;
            case 2:
              Lex->part_info->key_algorithm= partition_info::KEY_ALGORITHM_55;
              break;
            default:
              thd->parse_error();
              MYSQL_YYABORT;
            }
          }
        ;

part_field_list:
          /* empty */ {}
        | part_field_item_list {}
        ;

part_field_item_list:
          part_field_item {}
        | part_field_item_list ',' part_field_item {}
        ;

part_field_item:
          ident
          {
            partition_info *part_info= Lex->part_info;
            part_info->num_columns++;
            if (unlikely(part_info->part_field_list.push_back($1.str,
                         thd->mem_root)))
              MYSQL_YYABORT;
            if (unlikely(part_info->num_columns > MAX_REF_PARTS))
              my_yyabort_error((ER_TOO_MANY_PARTITION_FUNC_FIELDS_ERROR, MYF(0),
                                "list of partition fields"));
          }
        ;

part_column_list:
          COLUMNS '(' part_field_list ')'
          {
            partition_info *part_info= Lex->part_info;
            part_info->column_list= TRUE;
            part_info->list_of_part_fields= TRUE;
          }
        ;


part_func:
          '(' remember_name part_func_expr remember_end ')'
          {
            partition_info *part_info= Lex->part_info;
            if (unlikely(part_info->set_part_expr(thd, $2 + 1, $3, $4, FALSE)))
              MYSQL_YYABORT;
            part_info->num_columns= 1;
            part_info->column_list= FALSE;
          }
        ;

sub_part_func:
          '(' remember_name part_func_expr remember_end ')'
          {
            if (unlikely(Lex->part_info->set_part_expr(thd, $2 + 1, $3, $4, TRUE)))
              MYSQL_YYABORT;
          }
        ;


opt_num_parts:
          /* empty */ {}
        | PARTITIONS_SYM real_ulong_num
          { 
            uint num_parts= $2;
            partition_info *part_info= Lex->part_info;
            if (unlikely(num_parts == 0))
              my_yyabort_error((ER_NO_PARTS_ERROR, MYF(0), "partitions"));

            part_info->num_parts= num_parts;
            part_info->use_default_num_partitions= FALSE;
          }
        ;

opt_sub_part:
          /* empty */ {}
        | SUBPARTITION_SYM BY opt_linear HASH_SYM sub_part_func
          { Lex->part_info->subpart_type= HASH_PARTITION; }
          opt_num_subparts {}
        | SUBPARTITION_SYM BY opt_linear KEY_SYM opt_key_algo
          '(' sub_part_field_list ')'
          {
            partition_info *part_info= Lex->part_info;
            part_info->subpart_type= HASH_PARTITION;
            part_info->list_of_subpart_fields= TRUE;
          }
          opt_num_subparts {}
        ;

sub_part_field_list:
          sub_part_field_item {}
        | sub_part_field_list ',' sub_part_field_item {}
        ;

sub_part_field_item:
          ident
          {
            partition_info *part_info= Lex->part_info;
            if (unlikely(part_info->subpart_field_list.push_back($1.str,
                         thd->mem_root)))
              MYSQL_YYABORT;

            if (unlikely(part_info->subpart_field_list.elements > MAX_REF_PARTS))
              my_yyabort_error((ER_TOO_MANY_PARTITION_FUNC_FIELDS_ERROR, MYF(0),
                                "list of subpartition fields"));
          }
        ;

part_func_expr:
          bit_expr
          {
            if (unlikely(!Lex->safe_to_cache_query))
            {
              thd->parse_error(ER_WRONG_EXPR_IN_PARTITION_FUNC_ERROR);
              MYSQL_YYABORT;
            }
            $$=$1;
          }
        ;

opt_num_subparts:
          /* empty */ {}
        | SUBPARTITIONS_SYM real_ulong_num
          {
            uint num_parts= $2;
            LEX *lex= Lex;
            if (unlikely(num_parts == 0))
              my_yyabort_error((ER_NO_PARTS_ERROR, MYF(0), "subpartitions"));
            lex->part_info->num_subparts= num_parts;
            lex->part_info->use_default_num_subpartitions= FALSE;
          }
        ;

part_defs:
          /* empty */
          {
            partition_info *part_info= Lex->part_info;
            if (unlikely(part_info->part_type == RANGE_PARTITION))
              my_yyabort_error((ER_PARTITIONS_MUST_BE_DEFINED_ERROR, MYF(0),
                                "RANGE"));
            if (unlikely(part_info->part_type == LIST_PARTITION))
              my_yyabort_error((ER_PARTITIONS_MUST_BE_DEFINED_ERROR, MYF(0),
                                "LIST"));
          }
        | '(' part_def_list ')'
          {
            partition_info *part_info= Lex->part_info;
            uint count_curr_parts= part_info->partitions.elements;
            if (part_info->num_parts != 0)
            {
              if (unlikely(part_info->num_parts !=
                           count_curr_parts))
              {
                thd->parse_error(ER_PARTITION_WRONG_NO_PART_ERROR);
                MYSQL_YYABORT;
              }
            }
            else if (count_curr_parts > 0)
            {
              part_info->num_parts= count_curr_parts;
            }
            part_info->count_curr_subparts= 0;
          }
        ;

part_def_list:
          part_definition {}
        | part_def_list ',' part_definition {}
        ;

part_definition:
          PARTITION_SYM
          {
            partition_info *part_info= Lex->part_info;
            partition_element *p_elem= new (thd->mem_root) partition_element();

            if (unlikely(!p_elem) ||
                unlikely(part_info->partitions.push_back(p_elem, thd->mem_root)))
              MYSQL_YYABORT;

            p_elem->part_state= PART_NORMAL;
            p_elem->id= part_info->partitions.elements - 1;
            part_info->curr_part_elem= p_elem;
            part_info->current_partition= p_elem;
            part_info->use_default_partitions= FALSE;
            part_info->use_default_num_partitions= FALSE;
          }
          part_name
          opt_part_values
          opt_part_options
          opt_sub_partition
          {}
        ;

part_name:
          ident
          {
            partition_info *part_info= Lex->part_info;
            partition_element *p_elem= part_info->curr_part_elem;
            if (unlikely(check_ident_length(&$1)))
              MYSQL_YYABORT;
            p_elem->partition_name= $1.str;
          }
        ;

opt_part_values:
          /* empty */
          {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            if (! lex->is_partition_management())
            {
              if (unlikely(part_info->error_if_requires_values()))
                MYSQL_YYABORT;
              if (unlikely(part_info->part_type == VERSIONING_PARTITION))
                my_yyabort_error((ER_VERS_WRONG_PARTS, MYF(0),
                                  lex->create_last_non_select_table->
                                  table_name.str));
            }
            else
              part_info->part_type= HASH_PARTITION;
          }
        | VALUES_LESS_SYM THAN_SYM
          {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            if (! lex->is_partition_management())
            {
              if (unlikely(part_info->part_type != RANGE_PARTITION))
                my_yyabort_error((ER_PARTITION_WRONG_VALUES_ERROR, MYF(0),
                                  "RANGE", "LESS THAN"));
            }
            else
              part_info->part_type= RANGE_PARTITION;
          }
          part_func_max {}
        | VALUES_IN_SYM
          {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            if (! lex->is_partition_management())
            {
              if (unlikely(part_info->part_type != LIST_PARTITION))
                my_yyabort_error((ER_PARTITION_WRONG_VALUES_ERROR, MYF(0),
                                  "LIST", "IN"));
            }
            else
              part_info->part_type= LIST_PARTITION;
          }
          part_values_in {}
        | CURRENT_SYM
          {
            if (Lex->part_values_current(thd))
              MYSQL_YYABORT;
          }
        | HISTORY_SYM
          {
            if (Lex->part_values_history(thd))
              MYSQL_YYABORT;
          }
        | DEFAULT
         {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            if (! lex->is_partition_management())
            {
              if (unlikely(part_info->part_type != LIST_PARTITION))
                my_yyabort_error((ER_PARTITION_WRONG_VALUES_ERROR, MYF(0),
                                  "LIST", "DEFAULT"));
            }
            else
              part_info->part_type= LIST_PARTITION;
            if (unlikely(part_info->init_column_part(thd)))
              MYSQL_YYABORT;
            if (unlikely(part_info->add_max_value(thd)))
              MYSQL_YYABORT;
         }
        ;

part_func_max:
          MAXVALUE_SYM
          {
            partition_info *part_info= Lex->part_info;

            if (unlikely(part_info->num_columns &&
                         part_info->num_columns != 1U))
            {
              part_info->print_debug("Kilroy II", NULL);
              thd->parse_error(ER_PARTITION_COLUMN_LIST_ERROR);
              MYSQL_YYABORT;
            }
            else
              part_info->num_columns= 1U;
            if (unlikely(part_info->init_column_part(thd)))
              MYSQL_YYABORT;
            if (unlikely(part_info->add_max_value(thd)))
              MYSQL_YYABORT;
          }
        | part_value_item {}
        ;

part_values_in:
          part_value_item
          {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            part_info->print_debug("part_values_in: part_value_item", NULL);

            if (part_info->num_columns != 1U)
            {
              if (unlikely(!lex->is_partition_management() ||
                           part_info->num_columns == 0 ||
                           part_info->num_columns > MAX_REF_PARTS))
              {
                part_info->print_debug("Kilroy III", NULL);
                thd->parse_error(ER_PARTITION_COLUMN_LIST_ERROR);
                MYSQL_YYABORT;
              }
              /*
                Reorganize the current large array into a list of small
                arrays with one entry in each array. This can happen
                in the first partition of an ALTER TABLE statement where
                we ADD or REORGANIZE partitions. Also can only happen
                for LIST partitions.
              */
              if (unlikely(part_info->reorganize_into_single_field_col_val(thd)))
                MYSQL_YYABORT;
            }
          }
        | '(' part_value_list ')'
          {
            partition_info *part_info= Lex->part_info;
            if (unlikely(part_info->num_columns < 2U))
            {
              thd->parse_error(ER_ROW_SINGLE_PARTITION_FIELD_ERROR);
              MYSQL_YYABORT;
            }
          }
        ;

part_value_list:
          part_value_item {}
        | part_value_list ',' part_value_item {}
        ;

part_value_item:
          '('
          {
            partition_info *part_info= Lex->part_info;
            part_info->print_debug("( part_value_item", NULL);
            /* Initialisation code needed for each list of value expressions */
            if (unlikely(!(part_info->part_type == LIST_PARTITION &&
                           part_info->num_columns == 1U) &&
                           part_info->init_column_part(thd)))
              MYSQL_YYABORT;
          }
          part_value_item_list {}
          ')'
          {
            partition_info *part_info= Lex->part_info;
            part_info->print_debug(") part_value_item", NULL);
            if (part_info->num_columns == 0)
              part_info->num_columns= part_info->curr_list_object;
            if (unlikely(part_info->num_columns != part_info->curr_list_object))
            {
              /*
                All value items lists must be of equal length, in some cases
                which is covered by the above if-statement we don't know yet
                how many columns is in the partition so the assignment above
                ensures that we only report errors when we know we have an
                error.
              */
              part_info->print_debug("Kilroy I", NULL);
              thd->parse_error(ER_PARTITION_COLUMN_LIST_ERROR);
              MYSQL_YYABORT;
            }
            part_info->curr_list_object= 0;
          }
        ;

part_value_item_list:
          part_value_expr_item {}
        | part_value_item_list ',' part_value_expr_item {}
        ;

part_value_expr_item:
          MAXVALUE_SYM
          {
            partition_info *part_info= Lex->part_info;
            if (unlikely(part_info->part_type == LIST_PARTITION))
            {
              thd->parse_error(ER_MAXVALUE_IN_VALUES_IN);
              MYSQL_YYABORT;
            }
            if (unlikely(part_info->add_max_value(thd)))
              MYSQL_YYABORT;
          }
        | bit_expr
          {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            Item *part_expr= $1;

            if (unlikely(!lex->safe_to_cache_query))
            {
              thd->parse_error(ER_WRONG_EXPR_IN_PARTITION_FUNC_ERROR);
              MYSQL_YYABORT;
            }
            if (unlikely(part_info->add_column_list_value(thd, part_expr)))
              MYSQL_YYABORT;
          }
        ;


opt_sub_partition:
          /* empty */
          {
            partition_info *part_info= Lex->part_info;
            if (unlikely(part_info->num_subparts != 0 &&
                         !part_info->use_default_subpartitions))
            {
              /*
                We come here when we have defined subpartitions on the first
                partition but not on all the subsequent partitions. 
              */
              thd->parse_error(ER_PARTITION_WRONG_NO_SUBPART_ERROR);
              MYSQL_YYABORT;
            }
          }
        | '(' sub_part_list ')'
          {
            partition_info *part_info= Lex->part_info;
            if (part_info->num_subparts != 0)
            {
              if (unlikely(part_info->num_subparts !=
                           part_info->count_curr_subparts))
              {
                thd->parse_error(ER_PARTITION_WRONG_NO_SUBPART_ERROR);
                MYSQL_YYABORT;
              }
            }
            else if (part_info->count_curr_subparts > 0)
            {
              if (unlikely(part_info->partitions.elements > 1))
              {
                thd->parse_error(ER_PARTITION_WRONG_NO_SUBPART_ERROR);
                MYSQL_YYABORT;
              }
              part_info->num_subparts= part_info->count_curr_subparts;
            }
            part_info->count_curr_subparts= 0;
          }
        ;

sub_part_list:
          sub_part_definition {}
        | sub_part_list ',' sub_part_definition {}
        ;

sub_part_definition:
          SUBPARTITION_SYM
          {
            partition_info *part_info= Lex->part_info;
            partition_element *curr_part= part_info->current_partition;
            partition_element *sub_p_elem= new (thd->mem_root)
                                           partition_element(curr_part);
            if (unlikely(part_info->use_default_subpartitions &&
                         part_info->partitions.elements >= 2))
            {
              /*
                create table t1 (a int)
                partition by list (a) subpartition by hash (a)
                (partition p0 values in (1),
                 partition p1 values in (2) subpartition sp11);
                causes use to arrive since we are on the second
                partition, but still use_default_subpartitions
                is set. When we come here we're processing at least
                the second partition (the current partition processed
                have already been put into the partitions list.
              */
              thd->parse_error(ER_PARTITION_WRONG_NO_SUBPART_ERROR);
              MYSQL_YYABORT;
            }
            if (unlikely(!sub_p_elem) ||
                unlikely(curr_part->subpartitions.push_back(sub_p_elem, thd->mem_root)))
              MYSQL_YYABORT;

            sub_p_elem->id= curr_part->subpartitions.elements - 1;
            part_info->curr_part_elem= sub_p_elem;
            part_info->use_default_subpartitions= FALSE;
            part_info->use_default_num_subpartitions= FALSE;
            part_info->count_curr_subparts++;
          }
          sub_name opt_part_options {}
        ;

sub_name:
          ident_or_text
          {
            if (unlikely(check_ident_length(&$1)))
              MYSQL_YYABORT;
            Lex->part_info->curr_part_elem->partition_name= $1.str;
          }
        ;

opt_part_options:
         /* empty */ {}
       | opt_part_option_list {}
       ;

opt_part_option_list:
         opt_part_option_list opt_part_option {}
       | opt_part_option {}
       ;

opt_part_option:
          TABLESPACE opt_equal ident_or_text
          { Lex->part_info->curr_part_elem->tablespace_name= $3.str; }
        | opt_storage ENGINE_SYM opt_equal storage_engines
          {
            partition_info *part_info= Lex->part_info;
            part_info->curr_part_elem->engine_type= $4;
            part_info->default_engine_type= $4;
          }
        | CONNECTION_SYM opt_equal TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->part_info->curr_part_elem->connect_string.str= $3.str;
            lex->part_info->curr_part_elem->connect_string.length= $3.length;
          }
        | NODEGROUP_SYM opt_equal real_ulong_num
          { Lex->part_info->curr_part_elem->nodegroup_id= (uint16) $3; }
        | MAX_ROWS opt_equal real_ulonglong_num
          { Lex->part_info->curr_part_elem->part_max_rows= (ha_rows) $3; }
        | MIN_ROWS opt_equal real_ulonglong_num
          { Lex->part_info->curr_part_elem->part_min_rows= (ha_rows) $3; }
        | DATA_SYM DIRECTORY_SYM opt_equal TEXT_STRING_sys
          { Lex->part_info->curr_part_elem->data_file_name= $4.str; }
        | INDEX_SYM DIRECTORY_SYM opt_equal TEXT_STRING_sys
          { Lex->part_info->curr_part_elem->index_file_name= $4.str; }
        | COMMENT_SYM opt_equal TEXT_STRING_sys
          { Lex->part_info->curr_part_elem->part_comment= $3.str; }
        ;

opt_versioning_rotation:
         /* empty */ {}
       | INTERVAL_SYM expr interval opt_versioning_interval_start
         {
           partition_info *part_info= Lex->part_info;
           if (unlikely(part_info->vers_set_interval(thd, $2, $3, $4)))
           {
             my_error(ER_PART_WRONG_VALUE, MYF(0),
                      Lex->create_last_non_select_table->table_name.str,
                      "INTERVAL");
             MYSQL_YYABORT;
           }
         }
       | LIMIT ulonglong_num
       {
         partition_info *part_info= Lex->part_info;
         if (unlikely(part_info->vers_set_limit($2)))
         {
           my_error(ER_PART_WRONG_VALUE, MYF(0),
                    Lex->create_last_non_select_table->table_name.str,
                    "LIMIT");
           MYSQL_YYABORT;
         }
       }
       ;


opt_versioning_interval_start:
         /* empty */
         {
           $$= thd->query_start();
         }
       | STARTS_SYM ulong_num
         {
           /* only allowed from mysql_unpack_partition() */
           if (unlikely(!Lex->part_info->table))
           {
             thd->parse_error(ER_SYNTAX_ERROR, $1.pos());
             MYSQL_YYABORT;
           }
           $$= (ulong)$2;
         }
       ;

/*
 End of partition parser part
*/

opt_as:
          /* empty */ {}
        | AS {}
        ;

opt_create_database_options:
          /* empty */ {}
        | create_database_options {}
        ;

create_database_options:
          create_database_option {}
        | create_database_options create_database_option {}
        ;

create_database_option:
          default_collation {}
        | default_charset {}
        ;

opt_if_not_exists_table_element:
          /* empty */
          {
            Lex->check_exists= FALSE;
          }
        | IF_SYM not EXISTS
          {
            Lex->check_exists= TRUE;
          }
         ;

opt_if_not_exists:
          /* empty */
          {
            $$.init();
          }
        | IF_SYM not EXISTS
          {
            $$.set(DDL_options_st::OPT_IF_NOT_EXISTS);
          }
         ;

create_or_replace:
          CREATE /* empty */
          {
            $$.init();
          }
        | CREATE OR_SYM REPLACE
          {
            $$.set(DDL_options_st::OPT_OR_REPLACE);
          }
         ;

opt_create_table_options:
          /* empty */
        | create_table_options
        ;

create_table_options_space_separated:
          create_table_option
        | create_table_option create_table_options_space_separated
        ;

create_table_options:
          create_table_option
        | create_table_option     create_table_options
        | create_table_option ',' create_table_options
        ;

create_table_option:
          ENGINE_SYM opt_equal storage_engines
          {
            Lex->create_info.db_type= $3;
            Lex->create_info.used_fields|= HA_CREATE_USED_ENGINE;
          }
        | MAX_ROWS opt_equal ulonglong_num
          {
            Lex->create_info.max_rows= $3;
            Lex->create_info.used_fields|= HA_CREATE_USED_MAX_ROWS;
          }
        | MIN_ROWS opt_equal ulonglong_num
          {
            Lex->create_info.min_rows= $3;
            Lex->create_info.used_fields|= HA_CREATE_USED_MIN_ROWS;
          }
        | AVG_ROW_LENGTH opt_equal ulong_num
          {
            Lex->create_info.avg_row_length=$3;
            Lex->create_info.used_fields|= HA_CREATE_USED_AVG_ROW_LENGTH;
          }
        | PASSWORD_SYM opt_equal TEXT_STRING_sys
          {
            Lex->create_info.password=$3.str;
            Lex->create_info.used_fields|= HA_CREATE_USED_PASSWORD;
          }
        | COMMENT_SYM opt_equal TEXT_STRING_sys
          {
            Lex->create_info.comment=$3;
            Lex->create_info.used_fields|= HA_CREATE_USED_COMMENT;
          }
        | AUTO_INC opt_equal ulonglong_num
          {
            Lex->create_info.auto_increment_value=$3;
            Lex->create_info.used_fields|= HA_CREATE_USED_AUTO;
          }
        | PACK_KEYS_SYM opt_equal ulong_num
          {
            switch($3) {
            case 0:
                Lex->create_info.table_options|= HA_OPTION_NO_PACK_KEYS;
                break;
            case 1:
                Lex->create_info.table_options|= HA_OPTION_PACK_KEYS;
                break;
            default:
                thd->parse_error();
                MYSQL_YYABORT;
            }
            Lex->create_info.used_fields|= HA_CREATE_USED_PACK_KEYS;
          }
        | PACK_KEYS_SYM opt_equal DEFAULT
          {
            Lex->create_info.table_options&=
              ~(HA_OPTION_PACK_KEYS | HA_OPTION_NO_PACK_KEYS);
            Lex->create_info.used_fields|= HA_CREATE_USED_PACK_KEYS;
          }
        | STATS_AUTO_RECALC_SYM opt_equal ulong_num
          {
            switch($3) {
            case 0:
                Lex->create_info.stats_auto_recalc= HA_STATS_AUTO_RECALC_OFF;
                break;
            case 1:
                Lex->create_info.stats_auto_recalc= HA_STATS_AUTO_RECALC_ON;
                break;
            default:
                thd->parse_error();
                MYSQL_YYABORT;
            }
            Lex->create_info.used_fields|= HA_CREATE_USED_STATS_AUTO_RECALC;
          }
        | STATS_AUTO_RECALC_SYM opt_equal DEFAULT
          {
            Lex->create_info.stats_auto_recalc= HA_STATS_AUTO_RECALC_DEFAULT;
            Lex->create_info.used_fields|= HA_CREATE_USED_STATS_AUTO_RECALC;
          }
        | STATS_PERSISTENT_SYM opt_equal ulong_num
          {
            switch($3) {
            case 0:
                Lex->create_info.table_options|= HA_OPTION_NO_STATS_PERSISTENT;
                break;
            case 1:
                Lex->create_info.table_options|= HA_OPTION_STATS_PERSISTENT;
                break;
            default:
                thd->parse_error();
                MYSQL_YYABORT;
            }
            Lex->create_info.used_fields|= HA_CREATE_USED_STATS_PERSISTENT;
          }
        | STATS_PERSISTENT_SYM opt_equal DEFAULT
          {
            Lex->create_info.table_options&=
              ~(HA_OPTION_STATS_PERSISTENT | HA_OPTION_NO_STATS_PERSISTENT);
            Lex->create_info.used_fields|= HA_CREATE_USED_STATS_PERSISTENT;
          }
        | STATS_SAMPLE_PAGES_SYM opt_equal ulong_num
          {
            /* From user point of view STATS_SAMPLE_PAGES can be specified as
            STATS_SAMPLE_PAGES=N (where 0<N<=65535, it does not make sense to
            scan 0 pages) or STATS_SAMPLE_PAGES=default. Internally we record
            =default as 0. See create_frm() in sql/table.cc, we use only two
            bytes for stats_sample_pages and this is why we do not allow
            larger values. 65535 pages, 16kb each means to sample 1GB, which
            is impractical. If at some point this needs to be extended, then
            we can store the higher bits from stats_sample_pages in .frm too. */
            if (unlikely($3 == 0 || $3 > 0xffff))
            {
              thd->parse_error();
              MYSQL_YYABORT;
            }
            Lex->create_info.stats_sample_pages=$3;
            Lex->create_info.used_fields|= HA_CREATE_USED_STATS_SAMPLE_PAGES;
          }
        | STATS_SAMPLE_PAGES_SYM opt_equal DEFAULT
          {
            Lex->create_info.stats_sample_pages=0;
            Lex->create_info.used_fields|= HA_CREATE_USED_STATS_SAMPLE_PAGES;
          }
        | CHECKSUM_SYM opt_equal ulong_num
          {
            Lex->create_info.table_options|= $3 ? HA_OPTION_CHECKSUM : HA_OPTION_NO_CHECKSUM;
            Lex->create_info.used_fields|= HA_CREATE_USED_CHECKSUM;
          }
        | TABLE_CHECKSUM_SYM opt_equal ulong_num
          {
             Lex->create_info.table_options|= $3 ? HA_OPTION_CHECKSUM : HA_OPTION_NO_CHECKSUM;
             Lex->create_info.used_fields|= HA_CREATE_USED_CHECKSUM;
          }
        | PAGE_CHECKSUM_SYM opt_equal choice
          {
            Lex->create_info.used_fields|= HA_CREATE_USED_PAGE_CHECKSUM;
            Lex->create_info.page_checksum= $3;
          }
        | DELAY_KEY_WRITE_SYM opt_equal ulong_num
          {
            Lex->create_info.table_options|= $3 ? HA_OPTION_DELAY_KEY_WRITE : HA_OPTION_NO_DELAY_KEY_WRITE;
            Lex->create_info.used_fields|= HA_CREATE_USED_DELAY_KEY_WRITE;
          }
        | ROW_FORMAT_SYM opt_equal row_types
          {
            Lex->create_info.row_type= $3;
            Lex->create_info.used_fields|= HA_CREATE_USED_ROW_FORMAT;
          }
        | UNION_SYM opt_equal
          {
            Lex->first_select_lex()->table_list.save_and_clear(&Lex->save_list);
          }
          '(' opt_table_list ')'
          {
            /*
              Move the union list to the merge_list and exclude its tables
              from the global list.
            */
            LEX *lex=Lex;
            lex->create_info.merge_list= lex->first_select_lex()->table_list;
            lex->first_select_lex()->table_list= lex->save_list;
            /*
              When excluding union list from the global list we assume that
              elements of the former immediately follow elements which represent
              table being created/altered and parent tables.
            */
            TABLE_LIST *last_non_sel_table= lex->create_last_non_select_table;
            DBUG_ASSERT(last_non_sel_table->next_global ==
                        lex->create_info.merge_list.first);
            last_non_sel_table->next_global= 0;
            Lex->query_tables_last= &last_non_sel_table->next_global;

            lex->create_info.used_fields|= HA_CREATE_USED_UNION;
          }
        | default_charset
        | default_collation
        | INSERT_METHOD opt_equal merge_insert_types
          {
            Lex->create_info.merge_insert_method= $3;
            Lex->create_info.used_fields|= HA_CREATE_USED_INSERT_METHOD;
          }
        | DATA_SYM DIRECTORY_SYM opt_equal TEXT_STRING_sys
          {
            Lex->create_info.data_file_name= $4.str;
            Lex->create_info.used_fields|= HA_CREATE_USED_DATADIR;
          }
        | INDEX_SYM DIRECTORY_SYM opt_equal TEXT_STRING_sys
          {
            Lex->create_info.index_file_name= $4.str;
            Lex->create_info.used_fields|= HA_CREATE_USED_INDEXDIR;
          }
        | TABLESPACE ident
          {Lex->create_info.tablespace= $2.str;}
        | STORAGE_SYM DISK_SYM
          {Lex->create_info.storage_media= HA_SM_DISK;}
        | STORAGE_SYM MEMORY_SYM
          {Lex->create_info.storage_media= HA_SM_MEMORY;}
        | CONNECTION_SYM opt_equal TEXT_STRING_sys
          {
            Lex->create_info.connect_string.str= $3.str;
            Lex->create_info.connect_string.length= $3.length;
            Lex->create_info.used_fields|= HA_CREATE_USED_CONNECTION;
          }
        | KEY_BLOCK_SIZE opt_equal ulong_num
          {
            Lex->create_info.used_fields|= HA_CREATE_USED_KEY_BLOCK_SIZE;
            Lex->create_info.key_block_size= $3;
          }
        | TRANSACTIONAL_SYM opt_equal choice
          {
	    Lex->create_info.used_fields|= HA_CREATE_USED_TRANSACTIONAL;
            Lex->create_info.transactional= $3;
          }
        | IDENT_sys equal TEXT_STRING_sys
          {
            if (unlikely($3.length > ENGINE_OPTION_MAX_LENGTH))
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            (void) new (thd->mem_root)
                   engine_option_value($1, $3, true,
                                       &Lex->create_info.option_list,
                                       &Lex->option_list_last);
          }
        | IDENT_sys equal ident
          {
            if (unlikely($3.length > ENGINE_OPTION_MAX_LENGTH))
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            (void) new (thd->mem_root)
                   engine_option_value($1, $3, false,
                                       &Lex->create_info.option_list,
                                       &Lex->option_list_last);
          }
        | IDENT_sys equal real_ulonglong_num
          {
            (void) new (thd->mem_root)
                   engine_option_value($1, $3, &Lex->create_info.option_list,
                                       &Lex->option_list_last, thd->mem_root);
          }
        | IDENT_sys equal DEFAULT
          {
            (void) new (thd->mem_root)
                   engine_option_value($1, &Lex->create_info.option_list,
                                       &Lex->option_list_last);
          }
        | SEQUENCE_SYM opt_equal choice
          {
	    Lex->create_info.used_fields|= HA_CREATE_USED_SEQUENCE;
            Lex->create_info.sequence= ($3 == HA_CHOICE_YES);
	  }
        | versioning_option
        ;

opt_versioning_option:
          /* empty */
        | versioning_option
        ;

versioning_option:
        WITH_SYSTEM_SYM VERSIONING_SYM
          {
            if (unlikely(Lex->create_info.options & HA_LEX_CREATE_TMP_TABLE))
            {
              if (DBUG_EVALUATE_IF("sysvers_force", 0, 1))
              {
                my_error(ER_VERS_TEMPORARY, MYF(0));
                MYSQL_YYABORT;
              }
            }
            else
            {
              Lex->alter_info.flags|= ALTER_ADD_SYSTEM_VERSIONING;
              Lex->create_info.options|= HA_VERSIONED_TABLE;
            }
          }
        ;

default_charset:
          opt_default charset opt_equal charset_name_or_default
          {
            if (unlikely(Lex->create_info.add_table_option_default_charset($4)))
              MYSQL_YYABORT;
          }
        ;

default_collation:
          opt_default COLLATE_SYM opt_equal collation_name_or_default
          {
            HA_CREATE_INFO *cinfo= &Lex->create_info;
            if (unlikely((cinfo->used_fields & HA_CREATE_USED_DEFAULT_CHARSET) &&
                         cinfo->default_table_charset && $4 &&
                         !($4= merge_charset_and_collation(cinfo->default_table_charset,
                                                           $4))))
              MYSQL_YYABORT;

            Lex->create_info.default_table_charset= $4;
            Lex->create_info.used_fields|= HA_CREATE_USED_DEFAULT_CHARSET;
          }
        ;

storage_engines:
          ident_or_text
          {
            plugin_ref plugin= ha_resolve_by_name(thd, &$1,
                                            thd->lex->create_info.tmp_table());

            if (likely(plugin))
              $$= plugin_hton(plugin);
            else
            {
              if (thd->variables.sql_mode & MODE_NO_ENGINE_SUBSTITUTION)
                my_yyabort_error((ER_UNKNOWN_STORAGE_ENGINE, MYF(0), $1.str));
              $$= 0;
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_UNKNOWN_STORAGE_ENGINE,
                                  ER_THD(thd, ER_UNKNOWN_STORAGE_ENGINE),
                                  $1.str);
            }
          }
        ;

known_storage_engines:
          ident_or_text
          {
            plugin_ref plugin;
            if (likely((plugin= ha_resolve_by_name(thd, &$1, false))))
              $$= plugin_hton(plugin);
            else
              my_yyabort_error((ER_UNKNOWN_STORAGE_ENGINE, MYF(0), $1.str));
          }
        ;

row_types:
          DEFAULT        { $$= ROW_TYPE_DEFAULT; }
        | FIXED_SYM      { $$= ROW_TYPE_FIXED; }
        | DYNAMIC_SYM    { $$= ROW_TYPE_DYNAMIC; }
        | COMPRESSED_SYM { $$= ROW_TYPE_COMPRESSED; }
        | REDUNDANT_SYM  { $$= ROW_TYPE_REDUNDANT; }
        | COMPACT_SYM    { $$= ROW_TYPE_COMPACT; }
        | PAGE_SYM       { $$= ROW_TYPE_PAGE; }
        ;

merge_insert_types:
         NO_SYM          { $$= MERGE_INSERT_DISABLED; }
       | FIRST_SYM       { $$= MERGE_INSERT_TO_FIRST; }
       | LAST_SYM        { $$= MERGE_INSERT_TO_LAST; }
       ;

udf_type:
          STRING_SYM {$$ = (int) STRING_RESULT; }
        | REAL {$$ = (int) REAL_RESULT; }
        | DECIMAL_SYM {$$ = (int) DECIMAL_RESULT; }
        | INT_SYM {$$ = (int) INT_RESULT; }
        ;


create_field_list:
        field_list
        {
          Lex->create_last_non_select_table= Lex->last_table();
        }
        ;

create_field_list_parens:
        LEFT_PAREN_ALT field_list ')'
        {
          Lex->create_last_non_select_table= Lex->last_table();
        }
        ;

field_list:
          field_list_item
        | field_list ',' field_list_item
        ;

field_list_item:
          column_def { }
        | key_def
        | constraint_def
        | period_for_system_time
        ;

column_def:
          field_spec
          { $$= $1; }
        | field_spec references
          { $$= $1; }
        ;

key_def:
          key_or_index opt_if_not_exists opt_ident opt_USING_key_algorithm
          {
            Lex->option_list= NULL;
            if (unlikely(Lex->add_key(Key::MULTIPLE, &$3, $4, $2)))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options { }
        | key_or_index opt_if_not_exists ident TYPE_SYM btree_or_rtree
          {
            Lex->option_list= NULL;
            if (unlikely(Lex->add_key(Key::MULTIPLE, &$3, $5, $2)))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options { }
        | fulltext opt_key_or_index opt_if_not_exists opt_ident
          {
            Lex->option_list= NULL;
            if (unlikely(Lex->add_key($1, &$4, HA_KEY_ALG_UNDEF, $3)))
              MYSQL_YYABORT;
          }
          '(' key_list ')' fulltext_key_options { }
        | spatial opt_key_or_index opt_if_not_exists opt_ident
          {
            Lex->option_list= NULL;
            if (unlikely(Lex->add_key($1, &$4, HA_KEY_ALG_UNDEF, $3)))
              MYSQL_YYABORT;
          }
          '(' key_list ')' spatial_key_options { }
        | opt_constraint constraint_key_type
          opt_if_not_exists opt_ident
          opt_USING_key_algorithm
          {
            Lex->option_list= NULL;
            if (unlikely(Lex->add_key($2, $4.str ? &$4 : &$1, $5, $3)))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options { }
        | opt_constraint constraint_key_type opt_if_not_exists ident
          TYPE_SYM btree_or_rtree
          {
            Lex->option_list= NULL;
            if (unlikely(Lex->add_key($2, $4.str ? &$4 : &$1, $6, $3)))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options { }
        | opt_constraint FOREIGN KEY_SYM opt_if_not_exists opt_ident
          {
            if (unlikely(Lex->check_add_key($4)) ||
                unlikely(!(Lex->last_key= (new (thd->mem_root)
                                           Key(Key::MULTIPLE,
                                           $1.str ? &$1 : &$5,
                                           HA_KEY_ALG_UNDEF, true, $4)))))
              MYSQL_YYABORT;
            Lex->option_list= NULL;
          }
          '(' key_list ')' references
          {
            LEX *lex=Lex;
            Key *key= (new (thd->mem_root)
                       Foreign_key($5.str ? &$5 : &$1,
                                   &lex->last_key->columns,
                                   &$10->db,
                                   &$10->table,
                                   &lex->ref_list,
                                   lex->fk_delete_opt,
                                   lex->fk_update_opt,
                                   lex->fk_match_option,
                                    $4));
            if (unlikely(key == NULL))
              MYSQL_YYABORT;
            /*
              handle_if_exists_options() expectes the two keys in this order:
              the Foreign_key, followed by its auto-generated Key.
            */
            lex->alter_info.key_list.push_back(key, thd->mem_root);
            lex->alter_info.key_list.push_back(Lex->last_key, thd->mem_root);
            lex->option_list= NULL;

            /* Only used for ALTER TABLE. Ignored otherwise. */
            lex->alter_info.flags|= ALTER_ADD_FOREIGN_KEY;
          }
	;

constraint_def:
         opt_constraint check_constraint
         {
           Lex->add_constraint(&$1, $2, FALSE);
         }
       ;

period_for_system_time:
          // If FOR_SYM is followed by SYSTEM_TIME_SYM then they are merged to: FOR_SYSTEM_TIME_SYM .
          PERIOD_SYM FOR_SYSTEM_TIME_SYM '(' ident ',' ident ')'
          {
            Vers_parse_info &info= Lex->vers_get_info();
            info.set_system_time($4, $6);
          }
        ;

opt_check_constraint:
          /* empty */      { $$= (Virtual_column_info*) 0; }
        | check_constraint { $$= $1;}
        ;

check_constraint:
          CHECK_SYM '(' expr ')'
          {
            Virtual_column_info *v= add_virtual_expression(thd, $3);
            if (unlikely(!v))
              MYSQL_YYABORT;
            $$= v;
          }
        ;

opt_constraint:
          /* empty */ { $$= null_clex_str; }
        | constraint { $$= $1; }
        ;

constraint:
          CONSTRAINT opt_ident { $$=$2; }
        ;

field_spec:
          field_ident
          {
            LEX *lex=Lex;
            Create_field *f= new (thd->mem_root) Create_field();

            if (unlikely(check_string_char_length(&$1, 0, NAME_CHAR_LEN,
                                                  system_charset_info, 1)))
              my_yyabort_error((ER_TOO_LONG_IDENT, MYF(0), $1.str));

            if (unlikely(!f))
              MYSQL_YYABORT;

            lex->init_last_field(f, &$1, NULL);
            $<create_field>$= f;
          }
          field_type_or_serial opt_check_constraint
          {
            LEX *lex=Lex;
            $$= $<create_field>2;

            $$->check_constraint= $4;

            if (unlikely($$->check(thd)))
              MYSQL_YYABORT;

            lex->alter_info.create_list.push_back($$, thd->mem_root);

            $$->create_if_not_exists= Lex->check_exists;
            if ($$->flags & PRI_KEY_FLAG)
              lex->add_key_to_list(&$1, Key::PRIMARY, lex->check_exists);
            else if ($$->flags & UNIQUE_KEY_FLAG)
              lex->add_key_to_list(&$1, Key::UNIQUE, lex->check_exists);
          }
        ;

field_type_or_serial:
          field_type  { Lex->last_field->set_attributes($1, Lex->charset); }
          field_def
        | SERIAL_SYM
          {
            Lex->last_field->set_handler(&type_handler_longlong);
            Lex->last_field->flags|= AUTO_INCREMENT_FLAG | NOT_NULL_FLAG
                                     | UNSIGNED_FLAG | UNIQUE_KEY_FLAG;
          }
          opt_serial_attribute
        ;

opt_serial_attribute:
          /* empty */ {}
        | opt_serial_attribute_list {}
        ;

opt_serial_attribute_list:
          opt_serial_attribute_list serial_attribute {}
        | serial_attribute
        ;

opt_asrow_attribute:
          /* empty */ {}
        | opt_asrow_attribute_list {}
        ;

opt_asrow_attribute_list:
          opt_asrow_attribute_list asrow_attribute {}
        | asrow_attribute
        ;

field_def:
          opt_attribute
        | opt_generated_always AS virtual_column_func
         {
           Lex->last_field->vcol_info= $3;
           Lex->last_field->flags&= ~NOT_NULL_FLAG; // undo automatic NOT NULL for timestamps
         }
          vcol_opt_specifier vcol_opt_attribute
        | opt_generated_always AS ROW_SYM START_SYM opt_asrow_attribute
          {
            if (Lex->last_field_generated_always_as_row_start())
              MYSQL_YYABORT;
          }
        | opt_generated_always AS ROW_SYM END opt_asrow_attribute
          {
            if (Lex->last_field_generated_always_as_row_end())
              MYSQL_YYABORT;
          }
        ;

opt_generated_always:
          /* empty */ {}
        | GENERATED_SYM ALWAYS_SYM {}
        ;

vcol_opt_specifier:
          /* empty */
          {
            Lex->last_field->vcol_info->set_stored_in_db_flag(FALSE);
          }
        | VIRTUAL_SYM
          {
            Lex->last_field->vcol_info->set_stored_in_db_flag(FALSE);
          }
        | PERSISTENT_SYM
          {
            Lex->last_field->vcol_info->set_stored_in_db_flag(TRUE);
          }
        | STORED_SYM
          {
            Lex->last_field->vcol_info->set_stored_in_db_flag(TRUE);
          }
        ;

vcol_opt_attribute:
          /* empty */ {}
        | vcol_opt_attribute_list {}
        ;

vcol_opt_attribute_list:
          vcol_opt_attribute_list vcol_attribute {}
        | vcol_attribute
        ;

vcol_attribute:
          UNIQUE_SYM
          {
            LEX *lex=Lex;
            lex->last_field->flags|= UNIQUE_KEY_FLAG;
            lex->alter_info.flags|= ALTER_ADD_INDEX;
          }
        | UNIQUE_SYM KEY_SYM
          {
            LEX *lex=Lex;
            lex->last_field->flags|= UNIQUE_KEY_FLAG;
            lex->alter_info.flags|= ALTER_ADD_INDEX;
          }
        | COMMENT_SYM TEXT_STRING_sys { Lex->last_field->comment= $2; }
        | INVISIBLE_SYM
          {
            Lex->last_field->invisible= INVISIBLE_USER;
          }
        ;

parse_vcol_expr:
          PARSE_VCOL_EXPR_SYM
          {
            /*
              "PARSE_VCOL_EXPR" can only be used by the SQL server
              when reading a '*.frm' file.
              Prevent the end user from invoking this command.
            */
            MYSQL_YYABORT_UNLESS(Lex->parse_vcol_expr);
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          expr
          {
            Virtual_column_info *v= add_virtual_expression(thd, $3);
            if (unlikely(!v))
              MYSQL_YYABORT;
            Lex->last_field->vcol_info= v;
            Lex->pop_select(); //main select
          }
        ;

parenthesized_expr:
          remember_tok_start
          query_expression
          {
            if (!($$= Lex->create_item_query_expression(thd, $1, $2)))
              MYSQL_YYABORT;
          }
        | expr
        | expr ',' expr_list
          {
            $3->push_front($1, thd->mem_root);
            $$= new (thd->mem_root) Item_row(thd, *$3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
          ;

virtual_column_func:
          '(' parenthesized_expr ')'
          {
            Virtual_column_info *v=
              add_virtual_expression(thd, $2);
            if (unlikely(!v))
              MYSQL_YYABORT;
            $$= v;
          }
        ;

expr_or_literal: column_default_non_parenthesized_expr | signed_literal ;

column_default_expr:
          virtual_column_func
        | expr_or_literal
          {
            if (unlikely(!($$= add_virtual_expression(thd, $1))))
              MYSQL_YYABORT;
          }
        ;

field_type:
          field_type_numeric
        | field_type_temporal
        | field_type_string
        | field_type_lob
        | field_type_misc
        ;

field_type_numeric:
          int_type opt_field_length field_options { $$.set($1, $2); }
        | real_type opt_precision field_options   { $$.set($1, $2); }
        | FLOAT_SYM float_options field_options
          {
            $$.set(&type_handler_float, $2);
            if ($2.length() && !$2.dec())
            {
              int err;
              ulonglong tmp_length= my_strtoll10($2.length(), NULL, &err);
              if (unlikely(err || tmp_length > PRECISION_FOR_DOUBLE))
                my_yyabort_error((ER_WRONG_FIELD_SPEC, MYF(0),
                                  Lex->last_field->field_name.str));
              if (tmp_length > PRECISION_FOR_FLOAT)
                $$.set(&type_handler_double);
              else
                $$.set(&type_handler_float);
            }
          }
        | BIT_SYM opt_field_length_default_1
          {
            $$.set(&type_handler_bit, $2);
          }
        | BOOL_SYM
          {
            $$.set(&type_handler_tiny, "1");
          }
        | BOOLEAN_SYM
          {
            $$.set(&type_handler_tiny, "1");
          }
        | DECIMAL_SYM float_options field_options
          { $$.set(&type_handler_newdecimal, $2);}
        | NUMBER_ORACLE_SYM float_options field_options
          {
            if ($2.length() != 0)
              $$.set(&type_handler_newdecimal, $2);
            else
              $$.set(&type_handler_double);
          }
        | NUMERIC_SYM float_options field_options
          { $$.set(&type_handler_newdecimal, $2);}
        | FIXED_SYM float_options field_options
          { $$.set(&type_handler_newdecimal, $2);}
        ;


field_type_string:
          char opt_field_length_default_1 opt_binary
          {
            $$.set(&type_handler_string, $2);
          }
        | nchar opt_field_length_default_1 opt_bin_mod
          {
            $$.set(&type_handler_string, $2);
            bincmp_collation(national_charset_info, $3);
          }
        | BINARY opt_field_length_default_1
          {
            Lex->charset=&my_charset_bin;
            $$.set(&type_handler_string, $2);
          }
        | varchar field_length opt_binary
          {
            $$.set(&type_handler_varchar, $2);
          }
        | VARCHAR2_ORACLE_SYM field_length opt_binary
          {
            $$.set(&type_handler_varchar, $2);
          }
        | nvarchar field_length opt_bin_mod
          {
            $$.set(&type_handler_varchar, $2);
            bincmp_collation(national_charset_info, $3);
          }
        | VARBINARY field_length
          {
            Lex->charset=&my_charset_bin;
            $$.set(&type_handler_varchar, $2);
          }
        | RAW_ORACLE_SYM field_length
          {
            Lex->charset= &my_charset_bin;
            $$.set(&type_handler_varchar, $2);
          }
        ;

field_type_temporal:
          YEAR_SYM opt_field_length field_options
          {
            if ($2)
            {
              errno= 0;
              ulong length= strtoul($2, NULL, 10);
              if (errno == 0 && length <= MAX_FIELD_BLOBLENGTH && length != 4)
              {
                char buff[sizeof("YEAR()") + MY_INT64_NUM_DECIMAL_DIGITS + 1];
                my_snprintf(buff, sizeof(buff), "YEAR(%lu)", length);
                push_warning_printf(thd, Sql_condition::WARN_LEVEL_NOTE,
                                    ER_WARN_DEPRECATED_SYNTAX,
                                    ER_THD(thd, ER_WARN_DEPRECATED_SYNTAX),
                                    buff, "YEAR(4)");
              }
            }
            $$.set(&type_handler_year, $2);
          }
        | DATE_SYM { $$.set(thd->type_handler_for_date()); }
        | TIME_SYM opt_field_length
          {
            $$.set(opt_mysql56_temporal_format ?
                   static_cast<const Type_handler*>(&type_handler_time2) :
                   static_cast<const Type_handler*>(&type_handler_time),
                   $2);
          }
        | TIMESTAMP opt_field_length
          {
            if (thd->variables.sql_mode & MODE_MAXDB)
              $$.set(opt_mysql56_temporal_format ?
                     static_cast<const Type_handler*>(&type_handler_datetime2) :
                     static_cast<const Type_handler*>(&type_handler_datetime),
                     $2);
            else
            {
              /* 
                Unlike other types TIMESTAMP fields are NOT NULL by default.
                Unless --explicit-defaults-for-timestamp is given.
              */
              if (!opt_explicit_defaults_for_timestamp)
                Lex->last_field->flags|= NOT_NULL_FLAG;
              $$.set(opt_mysql56_temporal_format ?
                     static_cast<const Type_handler*>(&type_handler_timestamp2):
                     static_cast<const Type_handler*>(&type_handler_timestamp),
                     $2);
            }
          }
        | DATETIME opt_field_length
          {
            $$.set(opt_mysql56_temporal_format ?
                   static_cast<const Type_handler*>(&type_handler_datetime2) :
                   static_cast<const Type_handler*>(&type_handler_datetime),
                   $2);
          }
        ;


field_type_lob:
          TINYBLOB
          {
            Lex->charset=&my_charset_bin;
            $$.set(&type_handler_tiny_blob);
          }
        | BLOB_MARIADB_SYM opt_field_length
          {
            Lex->charset=&my_charset_bin;
            $$.set(&type_handler_blob, $2);
          }
        | BLOB_ORACLE_SYM opt_field_length
          {
            Lex->charset=&my_charset_bin;
            $$.set(&type_handler_long_blob);
          }
        | spatial_type float_options srid_option
          {
#ifdef HAVE_SPATIAL
            Lex->charset=&my_charset_bin;
            Lex->last_field->geom_type= $1;
            $$.set(&type_handler_geometry, $2);
#else
            my_yyabort_error((ER_FEATURE_DISABLED, MYF(0), sym_group_geom.name,
                              sym_group_geom.needed_define));
#endif
          }
        | MEDIUMBLOB
          {
            Lex->charset=&my_charset_bin;
            $$.set(&type_handler_medium_blob);
          }
        | LONGBLOB
          {
            Lex->charset=&my_charset_bin;
            $$.set(&type_handler_long_blob);
          }
        | LONG_SYM VARBINARY
          {
            Lex->charset=&my_charset_bin;
            $$.set(&type_handler_medium_blob);
          }
        | LONG_SYM varchar opt_binary
          { $$.set(&type_handler_medium_blob); }
        | TINYTEXT opt_binary
          { $$.set(&type_handler_tiny_blob); }
        | TEXT_SYM opt_field_length opt_binary
          { $$.set(&type_handler_blob, $2); }
        | MEDIUMTEXT opt_binary
          { $$.set(&type_handler_medium_blob); }
        | LONGTEXT opt_binary
          { $$.set(&type_handler_long_blob); }
        | CLOB_ORACLE_SYM opt_binary
          { $$.set(&type_handler_long_blob); }
        | LONG_SYM opt_binary
          { $$.set(&type_handler_medium_blob); }
        | JSON_SYM
          {
            Lex->charset= &my_charset_utf8mb4_bin;
            $$.set(&type_handler_long_blob);
          }
        ;

field_type_misc:
          ENUM '(' string_list ')' opt_binary
          { $$.set(&type_handler_enum); }
        | SET '(' string_list ')' opt_binary
          { $$.set(&type_handler_set); }
        ;

spatial_type:
          GEOMETRY_SYM        { $$= Field::GEOM_GEOMETRY; }
        | GEOMETRYCOLLECTION  { $$= Field::GEOM_GEOMETRYCOLLECTION; }
        | POINT_SYM           { $$= Field::GEOM_POINT; }
        | MULTIPOINT          { $$= Field::GEOM_MULTIPOINT; }
        | LINESTRING          { $$= Field::GEOM_LINESTRING; }
        | MULTILINESTRING     { $$= Field::GEOM_MULTILINESTRING; }
        | POLYGON             { $$= Field::GEOM_POLYGON; }
        | MULTIPOLYGON        { $$= Field::GEOM_MULTIPOLYGON; }
        ;

char:
          CHAR_SYM {}
        ;

nchar:
          NCHAR_SYM {}
        | NATIONAL_SYM CHAR_SYM {}
        ;

varchar:
          char VARYING {}
        | VARCHAR {}
        ;

nvarchar:
          NATIONAL_SYM VARCHAR {}
        | NVARCHAR_SYM {}
        | NCHAR_SYM VARCHAR {}
        | NATIONAL_SYM CHAR_SYM VARYING {}
        | NCHAR_SYM VARYING {}
        ;

int_type:
          INT_SYM   { $$= &type_handler_long; }
        | TINYINT   { $$= &type_handler_tiny; }
        | SMALLINT  { $$= &type_handler_short; }
        | MEDIUMINT { $$= &type_handler_int24; }
        | BIGINT    { $$= &type_handler_longlong; }
        ;

real_type:
          REAL
          {
            $$= thd->variables.sql_mode & MODE_REAL_AS_FLOAT ?
              static_cast<const Type_handler *>(&type_handler_float) :
              static_cast<const Type_handler *>(&type_handler_double);
          }
        | DOUBLE_SYM           { $$= &type_handler_double; }
        | DOUBLE_SYM PRECISION { $$= &type_handler_double; }
        ;

srid_option:
          /* empty */
          { Lex->last_field->srid= 0; }
        |
          REF_SYSTEM_ID_SYM '=' NUM
          {
            Lex->last_field->srid=atoi($3.str);
          }
        ;

float_options:
          /* empty */  { $$.set(0, 0);  }
        | field_length { $$.set($1, 0); }
        | precision    { $$= $1; }
        ;

precision:
          '(' NUM ',' NUM ')' { $$.set($2.str, $4.str); }
        ;

field_options:
          /* empty */ {}
        | SIGNED_SYM {}
        | UNSIGNED { Lex->last_field->flags|= UNSIGNED_FLAG;}
        | ZEROFILL { Lex->last_field->flags|= UNSIGNED_FLAG | ZEROFILL_FLAG; }
        | UNSIGNED ZEROFILL { Lex->last_field->flags|= UNSIGNED_FLAG | ZEROFILL_FLAG; }
        | ZEROFILL UNSIGNED { Lex->last_field->flags|= UNSIGNED_FLAG | ZEROFILL_FLAG; }
        ;

field_length:
          '(' LONG_NUM ')'      { $$= $2.str; }
        | '(' ULONGLONG_NUM ')' { $$= $2.str; }
        | '(' DECIMAL_NUM ')'   { $$= $2.str; }
        | '(' NUM ')'           { $$= $2.str; };

opt_field_length:
          /* empty */  { $$= (char*) 0; /* use default length */ }
        | field_length { $$= $1; }
        ;

opt_field_length_default_1:
          /* empty */  { $$= (char*) "1"; }
        | field_length { $$= $1; }
        ;

opt_precision:
          /* empty */    { $$.set(0, 0); }
        | precision      { $$= $1; }
        ;

opt_attribute:
          /* empty */ {}
        | opt_attribute_list {}
        ;

opt_attribute_list:
          opt_attribute_list attribute {}
        | attribute
        ;

attribute:
          NULL_SYM { Lex->last_field->flags&= ~ NOT_NULL_FLAG; }
        | DEFAULT column_default_expr { Lex->last_field->default_value= $2; }
        | ON UPDATE_SYM NOW_SYM opt_default_time_precision
          {
            Item *item= new (thd->mem_root) Item_func_now_local(thd, $4);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            Lex->last_field->on_update= item;
          }
        | AUTO_INC { Lex->last_field->flags|= AUTO_INCREMENT_FLAG | NOT_NULL_FLAG; }
        | SERIAL_SYM DEFAULT VALUE_SYM
          {
            LEX *lex=Lex;
            lex->last_field->flags|= AUTO_INCREMENT_FLAG | NOT_NULL_FLAG | UNIQUE_KEY_FLAG;
            lex->alter_info.flags|= ALTER_ADD_INDEX;
          }
        | COLLATE_SYM collation_name
          {
            if (unlikely(Lex->charset && !my_charset_same(Lex->charset,$2)))
              my_yyabort_error((ER_COLLATION_CHARSET_MISMATCH, MYF(0),
                                $2->name,Lex->charset->csname));
            Lex->last_field->charset= $2;
          }
        | COMPRESSED_SYM opt_compression_method
          {
            if (unlikely(Lex->last_field->set_compressed($2)))
              MYSQL_YYABORT;
          }
        | serial_attribute
        ;

opt_compression_method:
          /* empty */ { $$= NULL; }
        | equal ident { $$= $2.str; }
        ;

asrow_attribute:
          not NULL_SYM
          {
            Lex->last_field->flags|= NOT_NULL_FLAG;
          }
        | opt_primary KEY_SYM
          {
            LEX *lex=Lex;
            lex->last_field->flags|= PRI_KEY_FLAG | NOT_NULL_FLAG;
            lex->alter_info.flags|= ALTER_ADD_INDEX;
          }
        | vcol_attribute
        ;

serial_attribute:
          asrow_attribute
        | IDENT_sys equal TEXT_STRING_sys
          {
            if (unlikely($3.length > ENGINE_OPTION_MAX_LENGTH))
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            (void) new (thd->mem_root)
                   engine_option_value($1, $3, true,
                                       &Lex->last_field->option_list,
                                       &Lex->option_list_last);
          }
        | IDENT_sys equal ident
          {
            if (unlikely($3.length > ENGINE_OPTION_MAX_LENGTH))
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            (void) new (thd->mem_root)
                   engine_option_value($1, $3, false,
                                       &Lex->last_field->option_list,
                                       &Lex->option_list_last);
          }
        | IDENT_sys equal real_ulonglong_num
          {
            (void) new (thd->mem_root)
                   engine_option_value($1, $3, &Lex->last_field->option_list,
                                       &Lex->option_list_last, thd->mem_root);
          }
        | IDENT_sys equal DEFAULT
          {
            (void) new (thd->mem_root)
                   engine_option_value($1, &Lex->last_field->option_list,
                                       &Lex->option_list_last);
          }
        | with_or_without_system VERSIONING_SYM
          {
            Lex->last_field->versioning= $1;
            Lex->create_info.options|= HA_VERSIONED_TABLE;
          }
        ;

with_or_without_system:
        WITH_SYSTEM_SYM
          {
            Lex->alter_info.flags|= ALTER_COLUMN_UNVERSIONED;
            Lex->create_info.vers_info.versioned_fields= true;
            $$= Column_definition::WITH_VERSIONING;
          }
        | WITHOUT SYSTEM
          {
            Lex->alter_info.flags|= ALTER_COLUMN_UNVERSIONED;
            Lex->create_info.vers_info.unversioned_fields= true;
            $$= Column_definition::WITHOUT_VERSIONING;
          }
        ;


type_with_opt_collate:
        field_type opt_collate
        {
          $$= $1;

          if ($2)
          {
            if (unlikely(!(Lex->charset= merge_charset_and_collation(Lex->charset, $2))))
              MYSQL_YYABORT;
          }
          Lex->last_field->set_attributes($1, Lex->charset);
        }
        ;

charset:
          CHAR_SYM SET {}
        | CHARSET {}
        ;

charset_name:
          ident_or_text
          {
            if (unlikely(!($$=get_charset_by_csname($1.str,MY_CS_PRIMARY,MYF(0)))))
              my_yyabort_error((ER_UNKNOWN_CHARACTER_SET, MYF(0), $1.str));
          }
        | BINARY { $$= &my_charset_bin; }
        ;

charset_name_or_default:
          charset_name { $$=$1;   }
        | DEFAULT    { $$=NULL; }
        ;

opt_load_data_charset:
          /* Empty */ { $$= NULL; }
        | charset charset_name_or_default { $$= $2; }
        ;

old_or_new_charset_name:
          ident_or_text
          {
            if (unlikely(!($$=get_charset_by_csname($1.str,
                                                    MY_CS_PRIMARY,MYF(0))) &&
                         !($$=get_old_charset_by_name($1.str))))
              my_yyabort_error((ER_UNKNOWN_CHARACTER_SET, MYF(0), $1.str));
          }
        | BINARY { $$= &my_charset_bin; }
        ;

old_or_new_charset_name_or_default:
          old_or_new_charset_name { $$=$1;   }
        | DEFAULT    { $$=NULL; }
        ;

collation_name:
          ident_or_text
          {
            if (unlikely(!($$= mysqld_collation_get_by_name($1.str))))
              MYSQL_YYABORT;
          }
        ;

opt_collate:
          /* empty */ { $$=NULL; }
        | COLLATE_SYM collation_name_or_default { $$=$2; }
        ;

collation_name_or_default:
          collation_name { $$=$1; }
        | DEFAULT    { $$=NULL; }
        ;

opt_default:
          /* empty */ {}
        | DEFAULT {}
        ;

charset_or_alias:
          charset charset_name { $$= $2; }
        | ASCII_SYM { $$= &my_charset_latin1; }
        | UNICODE_SYM
          {
            if (unlikely(!($$= get_charset_by_csname("ucs2", MY_CS_PRIMARY,MYF(0)))))
              my_yyabort_error((ER_UNKNOWN_CHARACTER_SET, MYF(0), "ucs2"));
          }
        ;

opt_binary:
          /* empty */             { bincmp_collation(NULL, false); }
        | BYTE_SYM                { bincmp_collation(&my_charset_bin, false); }
        | charset_or_alias opt_bin_mod { bincmp_collation($1, $2); }
        | BINARY                  { bincmp_collation(NULL, true); }
        | BINARY charset_or_alias { bincmp_collation($2, true); }
        ;

opt_bin_mod:
          /* empty */   { $$= false; }
        | BINARY        { $$= true; }
        ;

ws_nweights:
        '(' real_ulong_num
        {
          if (unlikely($2 == 0))
          {
            thd->parse_error();
            MYSQL_YYABORT;
          }
        }
        ')'
        { $$= $2; }
        ;

ws_level_flag_desc:
        ASC { $$= 0; }
        | DESC { $$= 1 << MY_STRXFRM_DESC_SHIFT; }
        ;

ws_level_flag_reverse:
        REVERSE_SYM { $$= 1 << MY_STRXFRM_REVERSE_SHIFT; } ;

ws_level_flags:
        /* empty */ { $$= 0; }
        | ws_level_flag_desc { $$= $1; }
        | ws_level_flag_desc ws_level_flag_reverse { $$= $1 | $2; }
        | ws_level_flag_reverse { $$= $1 ; }
        ;

ws_level_number:
        real_ulong_num
        {
          $$= $1 < 1 ? 1 : ($1 > MY_STRXFRM_NLEVELS ? MY_STRXFRM_NLEVELS : $1);
          $$--;
        }
        ;

ws_level_list_item:
        ws_level_number ws_level_flags
        {
          $$= (1 | $2) << $1;
        }
        ;

ws_level_list:
        ws_level_list_item { $$= $1; }
        | ws_level_list ',' ws_level_list_item { $$|= $3; }
        ;

ws_level_range:
        ws_level_number '-' ws_level_number
        {
          uint start= $1;
          uint end= $3;
          for ($$= 0; start <= end; start++)
            $$|= (1 << start);
        }
        ;

ws_level_list_or_range:
        ws_level_list { $$= $1; }
        | ws_level_range { $$= $1; }
        ;

opt_ws_levels:
        /* empty*/ { $$= 0; }
        | LEVEL_SYM ws_level_list_or_range { $$= $2; }
        ;

opt_primary:
          /* empty */
        | PRIMARY_SYM
        ;

references:
          REFERENCES
          table_ident
          opt_ref_list
          opt_match_clause
          opt_on_update_delete
          {
            $$=$2;
          }
        ;

opt_ref_list:
          /* empty */
          { Lex->ref_list.empty(); }
        | '(' ref_list ')'
        ;

ref_list:
          ref_list ',' ident
          {
            Key_part_spec *key= new (thd->mem_root) Key_part_spec(&$3, 0);
            if (unlikely(key == NULL))
              MYSQL_YYABORT;
            Lex->ref_list.push_back(key, thd->mem_root);
          }
        | ident
          {
            Key_part_spec *key= new (thd->mem_root) Key_part_spec(&$1, 0);
            if (unlikely(key == NULL))
              MYSQL_YYABORT;
            LEX *lex= Lex;
            lex->ref_list.empty();
            lex->ref_list.push_back(key, thd->mem_root);
          }
        ;

opt_match_clause:
          /* empty */
          { Lex->fk_match_option= Foreign_key::FK_MATCH_UNDEF; }
        | MATCH FULL
          { Lex->fk_match_option= Foreign_key::FK_MATCH_FULL; }
        | MATCH PARTIAL
          { Lex->fk_match_option= Foreign_key::FK_MATCH_PARTIAL; }
        | MATCH SIMPLE_SYM
          { Lex->fk_match_option= Foreign_key::FK_MATCH_SIMPLE; }
        ;

opt_on_update_delete:
          /* empty */
          {
            LEX *lex= Lex;
            lex->fk_update_opt= FK_OPTION_UNDEF;
            lex->fk_delete_opt= FK_OPTION_UNDEF;
          }
        | ON UPDATE_SYM delete_option
          {
            LEX *lex= Lex;
            lex->fk_update_opt= $3;
            lex->fk_delete_opt= FK_OPTION_UNDEF;
          }
        | ON DELETE_SYM delete_option
          {
            LEX *lex= Lex;
            lex->fk_update_opt= FK_OPTION_UNDEF;
            lex->fk_delete_opt= $3;
          }
        | ON UPDATE_SYM delete_option
          ON DELETE_SYM delete_option
          {
            LEX *lex= Lex;
            lex->fk_update_opt= $3;
            lex->fk_delete_opt= $6;
          }
        | ON DELETE_SYM delete_option
          ON UPDATE_SYM delete_option
          {
            LEX *lex= Lex;
            lex->fk_update_opt= $6;
            lex->fk_delete_opt= $3;
          }
        ;

delete_option:
          RESTRICT      { $$= FK_OPTION_RESTRICT; }
        | CASCADE       { $$= FK_OPTION_CASCADE; }
        | SET NULL_SYM  { $$= FK_OPTION_SET_NULL; }
        | NO_SYM ACTION { $$= FK_OPTION_NO_ACTION; }
        | SET DEFAULT   { $$= FK_OPTION_SET_DEFAULT; }
        ;

constraint_key_type:
          PRIMARY_SYM KEY_SYM { $$= Key::PRIMARY; }
        | UNIQUE_SYM opt_key_or_index { $$= Key::UNIQUE; }
        ;

key_or_index:
          KEY_SYM {}
        | INDEX_SYM {}
        ;

opt_key_or_index:
          /* empty */ {}
        | key_or_index
        ;

keys_or_index:
          KEYS {}
        | INDEX_SYM {}
        | INDEXES {}
        ;

opt_unique:
          /* empty */  { $$= Key::MULTIPLE; }
        | UNIQUE_SYM   { $$= Key::UNIQUE; }
        ;

fulltext:
          FULLTEXT_SYM { $$= Key::FULLTEXT;}
        ;

spatial:
          SPATIAL_SYM
          {
#ifdef HAVE_SPATIAL
            $$= Key::SPATIAL;
#else
            my_yyabort_error((ER_FEATURE_DISABLED, MYF(0), sym_group_geom.name,
                              sym_group_geom.needed_define));
#endif
          }
        ;

normal_key_options:
          /* empty */ {}
        | normal_key_opts { Lex->last_key->option_list= Lex->option_list; }
        ;

fulltext_key_options:
          /* empty */ {}
        | fulltext_key_opts { Lex->last_key->option_list= Lex->option_list; }
        ;

spatial_key_options:
          /* empty */ {}
        | spatial_key_opts { Lex->last_key->option_list= Lex->option_list; }
        ;

normal_key_opts:
          normal_key_opt
        | normal_key_opts normal_key_opt
        ;

spatial_key_opts:
          spatial_key_opt
        | spatial_key_opts spatial_key_opt
        ;

fulltext_key_opts:
          fulltext_key_opt
        | fulltext_key_opts fulltext_key_opt
        ;

opt_USING_key_algorithm:
          /* Empty*/              { $$= HA_KEY_ALG_UNDEF; }
        | USING    btree_or_rtree { $$= $2; }
        ;

/* TYPE is a valid identifier, so it's handled differently than USING */
opt_key_algorithm_clause:
          /* Empty*/              { $$= HA_KEY_ALG_UNDEF; }
        | USING    btree_or_rtree { $$= $2; }
        | TYPE_SYM btree_or_rtree { $$= $2; }
        ;

key_using_alg:
          USING btree_or_rtree
          { Lex->last_key->key_create_info.algorithm= $2; }
        | TYPE_SYM btree_or_rtree
          { Lex->last_key->key_create_info.algorithm= $2; }
        ;

all_key_opt:
          KEY_BLOCK_SIZE opt_equal ulong_num
          {
            Lex->last_key->key_create_info.block_size= $3;
            Lex->last_key->key_create_info.flags|= HA_USES_BLOCK_SIZE;
         }
        | COMMENT_SYM TEXT_STRING_sys
          { Lex->last_key->key_create_info.comment= $2; }
        | IDENT_sys equal TEXT_STRING_sys
          {
            if (unlikely($3.length > ENGINE_OPTION_MAX_LENGTH))
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            (void) new (thd->mem_root)
                   engine_option_value($1, $3, true, &Lex->option_list,
                                       &Lex->option_list_last);
          }
        | IDENT_sys equal ident
          {
            if (unlikely($3.length > ENGINE_OPTION_MAX_LENGTH))
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            (void) new (thd->mem_root)
                   engine_option_value($1, $3, false, &Lex->option_list,
                                       &Lex->option_list_last);
          }
        | IDENT_sys equal real_ulonglong_num
          {
            (void) new (thd->mem_root)
                  engine_option_value($1, $3, &Lex->option_list,
                                      &Lex->option_list_last, thd->mem_root);
          }
        | IDENT_sys equal DEFAULT
          {
            (void) new (thd->mem_root)
                   engine_option_value($1, &Lex->option_list,
                                       &Lex->option_list_last);
          }
        ;

normal_key_opt:
          all_key_opt
        | key_using_alg
        ;

spatial_key_opt:
          all_key_opt
        ;

fulltext_key_opt:
          all_key_opt
        | WITH PARSER_SYM IDENT_sys
          {
            if (likely(plugin_is_ready(&$3, MYSQL_FTPARSER_PLUGIN)))
              Lex->last_key->key_create_info.parser_name= $3;
            else
              my_yyabort_error((ER_FUNCTION_NOT_DEFINED, MYF(0), $3.str));
          }
        ;

btree_or_rtree:
          BTREE_SYM { $$= HA_KEY_ALG_BTREE; }
        | RTREE_SYM { $$= HA_KEY_ALG_RTREE; }
        | HASH_SYM  { $$= HA_KEY_ALG_HASH; }
        ;

key_list:
          key_list ',' key_part order_dir
          {
            Lex->last_key->columns.push_back($3, thd->mem_root);
          }
        | key_part order_dir
          {
            Lex->last_key->columns.push_back($1, thd->mem_root);
          }
        ;

key_part:
          ident
          {
            $$= new (thd->mem_root) Key_part_spec(&$1, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | ident '(' NUM ')'
          {
            int key_part_len= atoi($3.str);
            if (unlikely(!key_part_len))
              my_yyabort_error((ER_KEY_PART_0, MYF(0), $1.str));
            $$= new (thd->mem_root) Key_part_spec(&$1, (uint) key_part_len);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_ident:
          /* empty */ { $$= null_clex_str; }
        | field_ident { $$= $1; }
        ;

string_list:
          text_string
          { Lex->last_field->interval_list.push_back($1, thd->mem_root); }
        | string_list ',' text_string
          { Lex->last_field->interval_list.push_back($3, thd->mem_root); };

/*
** Alter table
*/

alter:
          ALTER
          {
            Lex->name= null_clex_str;
            Lex->table_type= TABLE_TYPE_UNKNOWN;
            Lex->sql_command= SQLCOM_ALTER_TABLE;
            Lex->duplicates= DUP_ERROR;
            Lex->first_select_lex()->order_list.empty();
            Lex->create_info.init();
            Lex->create_info.row_type= ROW_TYPE_NOT_USED;
            Lex->alter_info.reset();
            Lex->no_write_to_binlog= 0;
            Lex->create_info.storage_media= HA_SM_DEFAULT;
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            DBUG_ASSERT(!Lex->m_sql_cmd);
          }
          alter_options TABLE_SYM table_ident opt_lock_wait_timeout
          {
            if (!Lex->first_select_lex()->
                 add_table_to_list(thd, $5, NULL, TL_OPTION_UPDATING,
                                   TL_READ_NO_INSERT, MDL_SHARED_UPGRADABLE))
              MYSQL_YYABORT;
            Lex->first_select_lex()->db=
              (Lex->first_select_lex()->table_list.first)->db;
            Lex->create_last_non_select_table= Lex->last_table();
          }
          alter_commands
          {
            if (likely(!Lex->m_sql_cmd))
            {
              /* Create a generic ALTER TABLE statment. */
              Lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_alter_table();
              if (unlikely(Lex->m_sql_cmd == NULL))
                MYSQL_YYABORT;
            }
            Lex->pop_select(); //main select
          }
        | ALTER DATABASE ident_or_empty
          {
            Lex->create_info.default_table_charset= NULL;
            Lex->create_info.used_fields= 0;
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          create_database_options
          {
            LEX *lex=Lex;
            lex->sql_command=SQLCOM_ALTER_DB;
            lex->name= $3;
            if (lex->name.str == NULL &&
                unlikely(lex->copy_db_to(&lex->name)))
              MYSQL_YYABORT;
            Lex->pop_select(); //main select
          }
        | ALTER DATABASE ident UPGRADE_SYM DATA_SYM DIRECTORY_SYM NAME_SYM
          {
            LEX *lex= Lex;
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "DATABASE"));
            lex->sql_command= SQLCOM_ALTER_DB_UPGRADE;
            lex->name= $3;
          }
        | ALTER PROCEDURE_SYM sp_name
          {
            LEX *lex= Lex;

            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "PROCEDURE"));
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            lex->sp_chistics.init();
          }
          sp_a_chistics
          {
            LEX *lex=Lex;

            lex->sql_command= SQLCOM_ALTER_PROCEDURE;
            lex->spname= $3;
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        | ALTER FUNCTION_SYM sp_name
          {
            LEX *lex= Lex;

            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "FUNCTION"));
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            lex->sp_chistics.init();
          }
          sp_a_chistics
          {
            LEX *lex=Lex;

            lex->sql_command= SQLCOM_ALTER_FUNCTION;
            lex->spname= $3;
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        | ALTER view_algorithm definer_opt opt_view_suid VIEW_SYM table_ident
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            if (Lex->add_alter_view(thd, $2, $4, $6))
              MYSQL_YYABORT;
          }
          view_list_opt AS view_select
          {
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        | ALTER definer_opt opt_view_suid VIEW_SYM table_ident
          /*
            We have two separate rules for ALTER VIEW rather that
            optional view_algorithm above, to resolve the ambiguity
            with the ALTER EVENT below.
          */
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            if (Lex->add_alter_view(thd, VIEW_ALGORITHM_INHERIT, $3, $5))
              MYSQL_YYABORT;
          }
          view_list_opt AS view_select
          {
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        | ALTER definer_opt remember_name EVENT_SYM sp_name
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            /*
              It is safe to use Lex->spname because
              ALTER EVENT xxx RENATE TO yyy DO ALTER EVENT RENAME TO
              is not allowed. Lex->spname is used in the case of RENAME TO
              If it had to be supported spname had to be added to
              Event_parse_data.
            */

            if (unlikely(!(Lex->event_parse_data= Event_parse_data::new_instance(thd))))
              MYSQL_YYABORT;
            Lex->event_parse_data->identifier= $5;

            Lex->sql_command= SQLCOM_ALTER_EVENT;
            Lex->stmt_definition_begin= $3;
          }
          ev_alter_on_schedule_completion
          opt_ev_rename_to
          opt_ev_status
          opt_ev_comment
          opt_ev_sql_stmt
          {
            if (unlikely(!($7 || $8 || $9 || $10 || $11)))
            {
              thd->parse_error();
              MYSQL_YYABORT;
            }
            /*
              sql_command is set here because some rules in ev_sql_stmt
              can overwrite it
            */
            Lex->sql_command= SQLCOM_ALTER_EVENT;
            Lex->stmt_definition_end= (char*)YYLIP->get_cpp_ptr();

            Lex->pop_select(); //main select
          }
        | ALTER TABLESPACE alter_tablespace_info
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_cmd_type= ALTER_TABLESPACE;
          }
        | ALTER LOGFILE_SYM GROUP_SYM alter_logfile_group_info
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_cmd_type= ALTER_LOGFILE_GROUP;
          }
        | ALTER TABLESPACE change_tablespace_info
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_cmd_type= CHANGE_FILE_TABLESPACE;
          }
        | ALTER TABLESPACE change_tablespace_access
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_cmd_type= ALTER_ACCESS_MODE_TABLESPACE;
          }
        | ALTER SERVER_SYM ident_or_text
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_ALTER_SERVER;
            lex->server_options.reset($3);
          } OPTIONS_SYM '(' server_options_list ')' { }
          /* ALTER USER foo is allowed for MySQL compatibility. */
        | ALTER opt_if_exists USER_SYM clear_privileges grant_list
          opt_require_clause opt_resource_options
          {
            Lex->create_info.set($2);
            Lex->sql_command= SQLCOM_ALTER_USER;
          }
        | ALTER SEQUENCE_SYM opt_if_exists
          {
            LEX *lex= Lex;
            lex->name= null_clex_str;
            lex->table_type= TABLE_TYPE_UNKNOWN;
            lex->sql_command= SQLCOM_ALTER_SEQUENCE;
            lex->create_info.init();
            lex->no_write_to_binlog= 0;
            DBUG_ASSERT(!lex->m_sql_cmd);
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          table_ident
          {
            LEX *lex= Lex;
            if (!(lex->create_info.seq_create_info= new (thd->mem_root)
                                                     sequence_definition()) ||
                !lex->first_select_lex()->
                  add_table_to_list(thd, $5, NULL, TL_OPTION_SEQUENCE,
                                    TL_WRITE, MDL_EXCLUSIVE))
              MYSQL_YYABORT;
          }
          sequence_defs
          {
            /* Create a generic ALTER SEQUENCE statment. */
            Lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_alter_sequence($3);
            if (unlikely(Lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        ;

ev_alter_on_schedule_completion:
          /* empty */ { $$= 0;}
        | ON SCHEDULE_SYM ev_schedule_time { $$= 1; }
        | ev_on_completion { $$= 1; }
        | ON SCHEDULE_SYM ev_schedule_time ev_on_completion { $$= 1; }
        ;

opt_ev_rename_to:
          /* empty */ { $$= 0;}
        | RENAME TO_SYM sp_name
          {
            /*
              Use lex's spname to hold the new name.
              The original name is in the Event_parse_data object
            */
            Lex->spname= $3; 
            $$= 1;
          }
        ;

opt_ev_sql_stmt:
          /* empty*/ { $$= 0;}
        | DO_SYM ev_sql_stmt { $$= 1; }
        ;

ident_or_empty:
          /* empty */ { $$= Lex_ident_sys(); }
        | ident
        ;

alter_commands:
          /* empty */
        | DISCARD TABLESPACE
          {
            Lex->m_sql_cmd= new (thd->mem_root)
              Sql_cmd_discard_import_tablespace(
                Sql_cmd_discard_import_tablespace::DISCARD_TABLESPACE);
            if (unlikely(Lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        | IMPORT TABLESPACE
          {
            Lex->m_sql_cmd= new (thd->mem_root)
              Sql_cmd_discard_import_tablespace(
                Sql_cmd_discard_import_tablespace::IMPORT_TABLESPACE);
            if (unlikely(Lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        | alter_list
          opt_partitioning
        | alter_list
          remove_partitioning
        | remove_partitioning
        | partitioning
/*
  This part was added for release 5.1 by Mikael Ronstrm.
  From here we insert a number of commands to manage the partitions of a
  partitioned table such as adding partitions, dropping partitions,
  reorganising partitions in various manners. In future releases the list
  will be longer.
*/
        | add_partition_rule
        | DROP PARTITION_SYM opt_if_exists alt_part_name_list
          {
            Lex->alter_info.partition_flags|= ALTER_PARTITION_DROP;
            DBUG_ASSERT(!Lex->if_exists());
            Lex->create_info.add($3);
          }
        | REBUILD_SYM PARTITION_SYM opt_no_write_to_binlog
          all_or_alt_part_name_list
          {
            LEX *lex= Lex;
            lex->alter_info.partition_flags|= ALTER_PARTITION_REBUILD;
            lex->no_write_to_binlog= $3;
          }
        | OPTIMIZE PARTITION_SYM opt_no_write_to_binlog
          all_or_alt_part_name_list
          {
            LEX *lex= thd->lex;
            lex->no_write_to_binlog= $3;
            lex->check_opt.init();
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root)
                              Sql_cmd_alter_table_optimize_partition();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
          opt_no_write_to_binlog
        | ANALYZE_SYM PARTITION_SYM opt_no_write_to_binlog
          all_or_alt_part_name_list
          {
            LEX *lex= thd->lex;
            lex->no_write_to_binlog= $3;
            lex->check_opt.init();
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root)
                              Sql_cmd_alter_table_analyze_partition();
            if (unlikely(lex->m_sql_cmd == NULL))
               MYSQL_YYABORT;
          }
        | CHECK_SYM PARTITION_SYM all_or_alt_part_name_list
          {
            LEX *lex= thd->lex;
            lex->check_opt.init();
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root)
                              Sql_cmd_alter_table_check_partition();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
          opt_mi_check_type
        | REPAIR PARTITION_SYM opt_no_write_to_binlog
          all_or_alt_part_name_list
          {
            LEX *lex= thd->lex;
            lex->no_write_to_binlog= $3;
            lex->check_opt.init();
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root)
                              Sql_cmd_alter_table_repair_partition();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
          opt_mi_repair_type
        | COALESCE PARTITION_SYM opt_no_write_to_binlog real_ulong_num
          {
            LEX *lex= Lex;
            lex->alter_info.partition_flags|= ALTER_PARTITION_COALESCE;
            lex->no_write_to_binlog= $3;
            lex->alter_info.num_parts= $4;
          }
        | TRUNCATE_SYM PARTITION_SYM all_or_alt_part_name_list
          {
            LEX *lex= thd->lex;
            lex->check_opt.init();
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root)
                              Sql_cmd_alter_table_truncate_partition();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        | reorg_partition_rule
        | EXCHANGE_SYM PARTITION_SYM alt_part_name_item
          WITH TABLE_SYM table_ident have_partitioning
          {
            LEX *lex= thd->lex;
            lex->first_select_lex()->db=$6->db;
            if (lex->first_select_lex()->db.str == NULL &&
                lex->copy_db_to(&lex->first_select_lex()->db))
            {
              MYSQL_YYABORT;
            }
            lex->name= $6->table;
            lex->alter_info.partition_flags|= ALTER_PARTITION_EXCHANGE;
            if (!lex->first_select_lex()->
              add_table_to_list(thd, $6, NULL, TL_OPTION_UPDATING,
                                TL_READ_NO_INSERT, MDL_SHARED_NO_WRITE))
              MYSQL_YYABORT;
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root)
                               Sql_cmd_alter_table_exchange_partition();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        ;

remove_partitioning:
          REMOVE_SYM PARTITIONING_SYM
          {
            Lex->alter_info.partition_flags|= ALTER_PARTITION_REMOVE;
          }
        ;

all_or_alt_part_name_list:
          ALL
          {
            Lex->alter_info.partition_flags|= ALTER_PARTITION_ALL;
          }
        | alt_part_name_list
        ;

add_partition_rule:
          ADD PARTITION_SYM opt_if_not_exists
          opt_no_write_to_binlog
          {
            LEX *lex= Lex;
            lex->part_info= new (thd->mem_root) partition_info();
            if (unlikely(!lex->part_info))
              MYSQL_YYABORT;

            lex->alter_info.partition_flags|= ALTER_PARTITION_ADD;
            DBUG_ASSERT(!Lex->create_info.if_not_exists());
            lex->create_info.set($3);
            lex->no_write_to_binlog= $4;
          }
          add_part_extra
          {}
        ;

add_part_extra:
          /* empty */
        | '(' part_def_list ')'
          {
            LEX *lex= Lex;
            lex->part_info->num_parts= lex->part_info->partitions.elements;
          }
        | PARTITIONS_SYM real_ulong_num
          {
            Lex->part_info->num_parts= $2;
          }
        ;

reorg_partition_rule:
          REORGANIZE_SYM PARTITION_SYM opt_no_write_to_binlog
          {
            LEX *lex= Lex;
            lex->part_info= new (thd->mem_root) partition_info();
            if (unlikely(!lex->part_info))
              MYSQL_YYABORT;

            lex->no_write_to_binlog= $3;
          }
          reorg_parts_rule
        ;

reorg_parts_rule:
          /* empty */
          {
            Lex->alter_info.partition_flags|= ALTER_PARTITION_TABLE_REORG;
          }
        | alt_part_name_list
          {
            Lex->alter_info.partition_flags|= ALTER_PARTITION_REORGANIZE;
          }
          INTO '(' part_def_list ')'
          {
            partition_info *part_info= Lex->part_info;
            part_info->num_parts= part_info->partitions.elements;
          }
        ;

alt_part_name_list:
          alt_part_name_item {}
        | alt_part_name_list ',' alt_part_name_item {}
        ;

alt_part_name_item:
          ident
          {
            if (unlikely(Lex->alter_info.partition_names.push_back($1.str,
                                                                   thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

/*
  End of management of partition commands
*/

alter_list:
          alter_list_item
        | alter_list ',' alter_list_item
        ;

add_column:
          ADD opt_column opt_if_not_exists_table_element
        ;

alter_list_item:
          add_column column_def opt_place
          {
            LEX *lex=Lex;
            lex->create_last_non_select_table= lex->last_table();
            lex->alter_info.flags|= ALTER_PARSER_ADD_COLUMN;
            $2->after= $3;
          }
        | ADD key_def
          {
            Lex->create_last_non_select_table= Lex->last_table();
            Lex->alter_info.flags|= ALTER_ADD_INDEX;
          }
        | ADD period_for_system_time
          {
            Lex->alter_info.flags|= ALTER_ADD_PERIOD;
          }
        | add_column '(' create_field_list ')'
          {
            LEX *lex=Lex;
            lex->alter_info.flags|= ALTER_PARSER_ADD_COLUMN;
            if (!lex->alter_info.key_list.is_empty())
              lex->alter_info.flags|= ALTER_ADD_INDEX;
          }
        | ADD constraint_def
          {
            Lex->alter_info.flags|= ALTER_ADD_CHECK_CONSTRAINT;
	  }
        | ADD CONSTRAINT IF_SYM not EXISTS field_ident check_constraint
         {
           Lex->alter_info.flags|= ALTER_ADD_CHECK_CONSTRAINT;
           Lex->add_constraint(&$6, $7, TRUE);
         }
        | CHANGE opt_column opt_if_exists_table_element field_ident
          field_spec opt_place
          {
            Lex->alter_info.flags|= ALTER_CHANGE_COLUMN | ALTER_RENAME_COLUMN;
            Lex->create_last_non_select_table= Lex->last_table();
            $5->change= $4;
            $5->after= $6;
          }
        | MODIFY_SYM opt_column opt_if_exists_table_element
          field_spec opt_place
          {
            Lex->alter_info.flags|= ALTER_CHANGE_COLUMN;
            Lex->create_last_non_select_table= Lex->last_table();
            $4->change= $4->field_name;
            $4->after= $5;
          }
        | DROP opt_column opt_if_exists_table_element field_ident opt_restrict
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::COLUMN, $4.str, $3));
            if (unlikely(ad == NULL))
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= ALTER_PARSER_DROP_COLUMN;
          }
	| DROP CONSTRAINT opt_if_exists_table_element field_ident
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::CHECK_CONSTRAINT,
                                        $4.str, $3));
            if (unlikely(ad == NULL))
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= ALTER_DROP_CHECK_CONSTRAINT;
          }
        | DROP FOREIGN KEY_SYM opt_if_exists_table_element field_ident
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::FOREIGN_KEY, $5.str, $4));
            if (unlikely(ad == NULL))
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= ALTER_DROP_FOREIGN_KEY;
          }
        | DROP PRIMARY_SYM KEY_SYM
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::KEY, primary_key_name,
                                        FALSE));
            if (unlikely(ad == NULL))
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= ALTER_DROP_INDEX;
          }
        | DROP key_or_index opt_if_exists_table_element field_ident
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::KEY, $4.str, $3));
            if (unlikely(ad == NULL))
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= ALTER_DROP_INDEX;
          }
        | DISABLE_SYM KEYS
          {
            LEX *lex=Lex;
            lex->alter_info.keys_onoff= Alter_info::DISABLE;
            lex->alter_info.flags|= ALTER_KEYS_ONOFF;
          }
        | ENABLE_SYM KEYS
          {
            LEX *lex=Lex;
            lex->alter_info.keys_onoff= Alter_info::ENABLE;
            lex->alter_info.flags|= ALTER_KEYS_ONOFF;
          }
        | ALTER opt_column opt_if_exists_table_element field_ident SET DEFAULT column_default_expr
          {
            if (unlikely(Lex->add_alter_list($4.str, $7, $3)))
              MYSQL_YYABORT;
          }
        | ALTER opt_column opt_if_exists_table_element field_ident DROP DEFAULT
          {
            if (unlikely(Lex->add_alter_list($4.str, (Virtual_column_info*) 0,
                                             $3)))
              MYSQL_YYABORT;
          }
        | RENAME opt_to table_ident
          {
            LEX *lex=Lex;
            lex->first_select_lex()->db= $3->db;
            if (lex->first_select_lex()->db.str == NULL &&
                lex->copy_db_to(&lex->first_select_lex()->db))
              MYSQL_YYABORT;
            if (unlikely(check_table_name($3->table.str,$3->table.length,
                                          FALSE)) ||
                ($3->db.str && unlikely(check_db_name((LEX_STRING*) &$3->db))))
              my_yyabort_error((ER_WRONG_TABLE_NAME, MYF(0), $3->table.str));
            lex->name= $3->table;
            lex->alter_info.flags|= ALTER_RENAME;
          }
        | CONVERT_SYM TO_SYM charset charset_name_or_default opt_collate
          {
            if (!$4)
            {
              $4= thd->variables.collation_database;
            }
            $5= $5 ? $5 : $4;
            if (unlikely(!my_charset_same($4,$5)))
              my_yyabort_error((ER_COLLATION_CHARSET_MISMATCH, MYF(0),
                                $5->name, $4->csname));
            if (unlikely(Lex->create_info.add_alter_list_item_convert_to_charset($5)))
              MYSQL_YYABORT;
            Lex->alter_info.flags|= ALTER_OPTIONS;
          }
        | create_table_options_space_separated
          {
            LEX *lex=Lex;
            lex->alter_info.flags|= ALTER_OPTIONS;
            if ((lex->create_info.used_fields & HA_CREATE_USED_ENGINE) &&
                !lex->create_info.db_type)
            {
              lex->create_info.used_fields&= ~HA_CREATE_USED_ENGINE;
            }
          }
        | FORCE_SYM
          {
            Lex->alter_info.flags|= ALTER_RECREATE;
          }
        | alter_order_clause
          {
            LEX *lex=Lex;
            lex->alter_info.flags|= ALTER_ORDER;
          }
        | alter_algorithm_option
        | alter_lock_option
        | ADD SYSTEM VERSIONING_SYM
          {
            Lex->alter_info.flags|= ALTER_ADD_SYSTEM_VERSIONING;
            Lex->create_info.options|= HA_VERSIONED_TABLE;
          }
        | DROP SYSTEM VERSIONING_SYM
          {
            Lex->alter_info.flags|= ALTER_DROP_SYSTEM_VERSIONING;
          }
        | DROP PERIOD_SYM FOR_SYSTEM_TIME_SYM
          {
            Lex->alter_info.flags|= ALTER_DROP_PERIOD;
          }
        ;

opt_index_lock_algorithm:
          /* empty */
        | alter_lock_option
        | alter_algorithm_option
        | alter_lock_option alter_algorithm_option
        | alter_algorithm_option alter_lock_option
        ;

alter_algorithm_option:
          ALGORITHM_SYM opt_equal DEFAULT
          {
            Lex->alter_info.requested_algorithm=
              Alter_info::ALTER_TABLE_ALGORITHM_DEFAULT;
          }
        | ALGORITHM_SYM opt_equal ident
          {
            if (unlikely(Lex->alter_info.set_requested_algorithm(&$3)))
              my_yyabort_error((ER_UNKNOWN_ALTER_ALGORITHM, MYF(0), $3.str));
          }
        ;

alter_lock_option:
          LOCK_SYM opt_equal DEFAULT
          {
            Lex->alter_info.requested_lock=
              Alter_info::ALTER_TABLE_LOCK_DEFAULT;
          }
        | LOCK_SYM opt_equal ident
          {
            if (unlikely(Lex->alter_info.set_requested_lock(&$3)))
              my_yyabort_error((ER_UNKNOWN_ALTER_LOCK, MYF(0), $3.str));
          }
        ;

opt_column:
          /* empty */ {}     %prec PREC_BELOW_IDENTIFIER_OPT_SPECIAL_CASE
        | COLUMN_SYM {}
        ;

opt_ignore:
          /* empty */ { Lex->ignore= 0;}
        | IGNORE_SYM { Lex->ignore= 1;}
        ;

alter_options:
        { Lex->ignore= 0;} alter_options_part2
	;
	
alter_options_part2:
          /* empty */ 
        | alter_option_list
        ;

alter_option_list:
        alter_option_list alter_option
        | alter_option
        ;

alter_option:
	  IGNORE_SYM { Lex->ignore= 1;}
        | ONLINE_SYM
          {
            Lex->alter_info.requested_lock=
              Alter_info::ALTER_TABLE_LOCK_NONE;
          }
        ;

opt_restrict:
          /* empty */ { Lex->drop_mode= DROP_DEFAULT; }
        | RESTRICT    { Lex->drop_mode= DROP_RESTRICT; }
        | CASCADE     { Lex->drop_mode= DROP_CASCADE; }
        ;

opt_place:
        /* empty */ { $$= null_clex_str; }
        | AFTER_SYM ident
          {
            $$= $2;
            Lex->alter_info.flags |= ALTER_COLUMN_ORDER;
          }
        | FIRST_SYM
          {
            $$.str=    first_keyword;
	    $$.length= 5; /* Length of "first" */
            Lex->alter_info.flags |= ALTER_COLUMN_ORDER;
          }
        ;

opt_to:
          /* empty */ {}
        | TO_SYM {}
        | '=' {}
        | AS {}
        ;

slave:
          START_SYM SLAVE optional_connection_name slave_thread_opts
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_SLAVE_START;
            lex->type = 0;
            /* If you change this code don't forget to update SLAVE START too */
          }
          slave_until
          {}
        | START_SYM ALL SLAVES slave_thread_opts
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_SLAVE_ALL_START;
            lex->type = 0;
            /* If you change this code don't forget to update STOP SLAVE too */
          }
          {}
        | STOP_SYM SLAVE optional_connection_name slave_thread_opts
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_SLAVE_STOP;
            lex->type = 0;
            /* If you change this code don't forget to update SLAVE STOP too */
          }
        | STOP_SYM ALL SLAVES slave_thread_opts
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_SLAVE_ALL_STOP;
            lex->type = 0;
            /* If you change this code don't forget to update SLAVE STOP too */
          }
        ;

start:
          START_SYM TRANSACTION_SYM opt_start_transaction_option_list
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_BEGIN;
            /* READ ONLY and READ WRITE are mutually exclusive. */
            if (unlikely(($3 & MYSQL_START_TRANS_OPT_READ_WRITE) &&
                         ($3 & MYSQL_START_TRANS_OPT_READ_ONLY)))
            {
              thd->parse_error();
              MYSQL_YYABORT;
            }
            lex->start_transaction_opt= $3;
          }
        ;

opt_start_transaction_option_list:
          /* empty */
          {
            $$= 0;
          }
        | start_transaction_option_list
          {
            $$= $1;
          }
        ;

start_transaction_option_list:
          start_transaction_option
          {
            $$= $1;
          }
        | start_transaction_option_list ',' start_transaction_option
          {
            $$= $1 | $3;
          }
        ;

start_transaction_option:
          WITH CONSISTENT_SYM SNAPSHOT_SYM
          {
            $$= MYSQL_START_TRANS_OPT_WITH_CONS_SNAPSHOT;
          }
        | READ_SYM ONLY_SYM
          {
            $$= MYSQL_START_TRANS_OPT_READ_ONLY;
          }
        | READ_SYM WRITE_SYM
          {
            $$= MYSQL_START_TRANS_OPT_READ_WRITE;
          }
        ;

slave_thread_opts:
          { Lex->slave_thd_opt= 0; }
          slave_thread_opt_list
          {}
        ;

slave_thread_opt_list:
          slave_thread_opt
        | slave_thread_opt_list ',' slave_thread_opt
        ;

slave_thread_opt:
          /*empty*/ {}
        | SQL_THREAD   { Lex->slave_thd_opt|=SLAVE_SQL; }
        | RELAY_THREAD { Lex->slave_thd_opt|=SLAVE_IO; }
        ;

slave_until:
          /*empty*/ {}
        | UNTIL_SYM slave_until_opts
          {
            LEX *lex=Lex;
            if (unlikely(((lex->mi.log_file_name || lex->mi.pos) &&
                         (lex->mi.relay_log_name || lex->mi.relay_log_pos)) ||
                         !((lex->mi.log_file_name && lex->mi.pos) ||
                           (lex->mi.relay_log_name && lex->mi.relay_log_pos))))
               my_yyabort_error((ER_BAD_SLAVE_UNTIL_COND, MYF(0)));
          }
        | UNTIL_SYM MASTER_GTID_POS_SYM '=' TEXT_STRING_sys
          {
            Lex->mi.gtid_pos_str = $4;
          }
        ;

slave_until_opts:
          master_file_def
        | slave_until_opts ',' master_file_def
        ;

checksum:
          CHECKSUM_SYM table_or_tables
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_CHECKSUM;
            /* Will be overridden during execution. */
            YYPS->m_lock_type= TL_UNLOCK;
          }
          table_list opt_checksum_type
          {}
        ;

opt_checksum_type:
          /* nothing */ { Lex->check_opt.flags= 0; }
        | QUICK         { Lex->check_opt.flags= T_QUICK; }
        | EXTENDED_SYM  { Lex->check_opt.flags= T_EXTEND; }
        ;

repair_table_or_view:
          table_or_tables table_list opt_mi_repair_type
        | VIEW_SYM
          { Lex->table_type= TABLE_TYPE_VIEW; }
          table_list opt_view_repair_type
        ;

repair:
          REPAIR opt_no_write_to_binlog
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_REPAIR;
            lex->no_write_to_binlog= $2;
            lex->check_opt.init();
            lex->alter_info.reset();
            /* Will be overridden during execution. */
            YYPS->m_lock_type= TL_UNLOCK;
          }
          repair_table_or_view
          {
            LEX* lex= thd->lex;
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_repair_table();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_mi_repair_type:
          /* empty */ { Lex->check_opt.flags = T_MEDIUM; }
        | mi_repair_types {}
        ;

mi_repair_types:
          mi_repair_type {}
        | mi_repair_type mi_repair_types {}
        ;

mi_repair_type:
          QUICK        { Lex->check_opt.flags|= T_QUICK; }
        | EXTENDED_SYM { Lex->check_opt.flags|= T_EXTEND; }
        | USE_FRM      { Lex->check_opt.sql_flags|= TT_USEFRM; }
        ;

opt_view_repair_type:
          /* empty */    { }
        | FROM MYSQL_SYM { Lex->check_opt.sql_flags|= TT_FROM_MYSQL; }
        ;

analyze:
          ANALYZE_SYM opt_no_write_to_binlog table_or_tables
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_ANALYZE;
            lex->no_write_to_binlog= $2;
            lex->check_opt.init();
            lex->alter_info.reset();
            /* Will be overridden during execution. */
            YYPS->m_lock_type= TL_UNLOCK;
          }
          analyze_table_list
          {
            LEX* lex= thd->lex;
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_analyze_table();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        ;

analyze_table_list:
          analyze_table_elem_spec
        | analyze_table_list ',' analyze_table_elem_spec
        ;

analyze_table_elem_spec:
          table_name opt_persistent_stat_clause
        ;

opt_persistent_stat_clause:
          /* empty */
          {}        
        | PERSISTENT_SYM FOR_SYM persistent_stat_spec  
          { 
            thd->lex->with_persistent_for_clause= TRUE;
          }
        ;

persistent_stat_spec:
          ALL
          {}
        | COLUMNS persistent_column_stat_spec INDEXES persistent_index_stat_spec
          {}
        ;

persistent_column_stat_spec:
          ALL {}
        | '('
          { 
            LEX* lex= thd->lex;
            lex->column_list= new (thd->mem_root) List<LEX_STRING>;
            if (unlikely(lex->column_list == NULL))
              MYSQL_YYABORT;
          }
          table_column_list
          ')' 
        ;
 
persistent_index_stat_spec:
          ALL {}
        | '('
          { 
            LEX* lex= thd->lex;
            lex->index_list= new (thd->mem_root) List<LEX_STRING>;
            if (unlikely(lex->index_list == NULL))
              MYSQL_YYABORT;
          }
          table_index_list
          ')' 
        ;

table_column_list:
          /* empty */
          {}
        | ident 
          {
            Lex->column_list->push_back((LEX_STRING*)
                thd->memdup(&$1, sizeof(LEX_STRING)), thd->mem_root);
          }
        | table_column_list ',' ident
          {
            Lex->column_list->push_back((LEX_STRING*)
                thd->memdup(&$3, sizeof(LEX_STRING)), thd->mem_root);
          }
        ;

table_index_list:
          /* empty */
          {}
        | table_index_name 
        | table_index_list ',' table_index_name
        ;

table_index_name:
          ident
          {
            Lex->index_list->push_back((LEX_STRING*)
                                       thd->memdup(&$1, sizeof(LEX_STRING)),
                                       thd->mem_root);
          }
        |
          PRIMARY_SYM
          {
            LEX_STRING str= {(char*) "PRIMARY", 7};
            Lex->index_list->push_back((LEX_STRING*)
                                        thd->memdup(&str, sizeof(LEX_STRING)),
                                        thd->mem_root);
          }  
        ;  

binlog_base64_event:
          BINLOG_SYM TEXT_STRING_sys
          {
            Lex->sql_command = SQLCOM_BINLOG_BASE64_EVENT;
            Lex->comment= $2;
          }
        ;

check_view_or_table:
          table_or_tables table_list opt_mi_check_type
        | VIEW_SYM
          { Lex->table_type= TABLE_TYPE_VIEW; }
          table_list opt_view_check_type
        ;

check:    CHECK_SYM
          {
            LEX *lex=Lex;

            lex->sql_command = SQLCOM_CHECK;
            lex->check_opt.init();
            lex->alter_info.reset();
            /* Will be overridden during execution. */
            YYPS->m_lock_type= TL_UNLOCK;
          }
          check_view_or_table
          {
            LEX* lex= thd->lex;
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "CHECK"));
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_check_table();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_mi_check_type:
          /* empty */ { Lex->check_opt.flags = T_MEDIUM; }
        | mi_check_types {}
        ;

mi_check_types:
          mi_check_type {}
        | mi_check_type mi_check_types {}
        ;

mi_check_type:
          QUICK               { Lex->check_opt.flags|= T_QUICK; }
        | FAST_SYM            { Lex->check_opt.flags|= T_FAST; }
        | MEDIUM_SYM          { Lex->check_opt.flags|= T_MEDIUM; }
        | EXTENDED_SYM        { Lex->check_opt.flags|= T_EXTEND; }
        | CHANGED             { Lex->check_opt.flags|= T_CHECK_ONLY_CHANGED; }
        | FOR_SYM UPGRADE_SYM { Lex->check_opt.sql_flags|= TT_FOR_UPGRADE; }
        ;

opt_view_check_type:
          /* empty */         { }
        | FOR_SYM UPGRADE_SYM { Lex->check_opt.sql_flags|= TT_FOR_UPGRADE; }
        ;

optimize:
          OPTIMIZE opt_no_write_to_binlog table_or_tables
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_OPTIMIZE;
            lex->no_write_to_binlog= $2;
            lex->check_opt.init();
            lex->alter_info.reset();
            /* Will be overridden during execution. */
            YYPS->m_lock_type= TL_UNLOCK;
          }
          table_list opt_lock_wait_timeout
          {
            LEX* lex= thd->lex;
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_optimize_table();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_no_write_to_binlog:
          /* empty */ { $$= 0; }
        | NO_WRITE_TO_BINLOG { $$= 1; }
        | LOCAL_SYM { $$= 1; }
        ;

rename:
          RENAME table_or_tables
          {
            Lex->sql_command= SQLCOM_RENAME_TABLE;
          }
          table_to_table_list
          {}
        | RENAME USER_SYM clear_privileges rename_list
          {
            Lex->sql_command = SQLCOM_RENAME_USER;
          }
        ;

rename_list:
          user TO_SYM user
          {
            if (unlikely(Lex->users_list.push_back($1, thd->mem_root) ||
                         Lex->users_list.push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | rename_list ',' user TO_SYM user
          {
            if (unlikely(Lex->users_list.push_back($3, thd->mem_root) ||
                         Lex->users_list.push_back($5, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

table_to_table_list:
          table_to_table
        | table_to_table_list ',' table_to_table
        ;

table_to_table:
          table_ident opt_lock_wait_timeout TO_SYM table_ident
          {
            LEX *lex=Lex;
            SELECT_LEX *sl= lex->current_select;
            if (unlikely(!sl->add_table_to_list(thd, $1,NULL,
                                                TL_OPTION_UPDATING,
                                                TL_IGNORE, MDL_EXCLUSIVE)) ||
                unlikely(!sl->add_table_to_list(thd, $4, NULL,
                                                TL_OPTION_UPDATING,
                                                TL_IGNORE, MDL_EXCLUSIVE)))
              MYSQL_YYABORT;
          }
        ;

keycache:
          CACHE_SYM INDEX_SYM
          {
            Lex->alter_info.reset();
          }
          keycache_list_or_parts IN_SYM key_cache_name
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_ASSIGN_TO_KEYCACHE;
            lex->ident= $6;
          }
        ;

keycache_list_or_parts:
          keycache_list
        | assign_to_keycache_parts
        ;

keycache_list:
          assign_to_keycache
        | keycache_list ',' assign_to_keycache
        ;

assign_to_keycache:
          table_ident cache_keys_spec
          {
            if (unlikely(!Select->add_table_to_list(thd, $1, NULL, 0, TL_READ,
                                                    MDL_SHARED_READ,
                                                    Select->
                                                    pop_index_hints())))
              MYSQL_YYABORT;
          }
        ;

assign_to_keycache_parts:
          table_ident adm_partition cache_keys_spec
          {
            if (unlikely(!Select->add_table_to_list(thd, $1, NULL, 0, TL_READ,
                                                    MDL_SHARED_READ,
                                                    Select->
                                                    pop_index_hints())))
              MYSQL_YYABORT;
          }
        ;

key_cache_name:
          ident    { $$= $1; }
        | DEFAULT  { $$ = default_key_cache_base; }
        ;

preload:
          LOAD INDEX_SYM INTO CACHE_SYM
          {
            LEX *lex=Lex;
            lex->sql_command=SQLCOM_PRELOAD_KEYS;
            lex->alter_info.reset();
          }
          preload_list_or_parts
          {}
        ;

preload_list_or_parts:
          preload_keys_parts
        | preload_list
        ;

preload_list:
          preload_keys
        | preload_list ',' preload_keys
        ;

preload_keys:
          table_ident cache_keys_spec opt_ignore_leaves
          {
            if (unlikely(!Select->add_table_to_list(thd, $1, NULL, $3, TL_READ,
                                                    MDL_SHARED_READ,
                                                    Select->
                                                    pop_index_hints())))
              MYSQL_YYABORT;
          }
        ;

preload_keys_parts:
          table_ident adm_partition cache_keys_spec opt_ignore_leaves
          {
            if (unlikely(!Select->add_table_to_list(thd, $1, NULL, $4, TL_READ,
                                                    MDL_SHARED_READ,
                                                    Select->
                                                    pop_index_hints())))
              MYSQL_YYABORT;
          }
        ;

adm_partition:
          PARTITION_SYM have_partitioning
          {
            Lex->alter_info.partition_flags|= ALTER_PARTITION_ADMIN;
          }
          '(' all_or_alt_part_name_list ')'
        ;

cache_keys_spec:
          {
            Lex->first_select_lex()->alloc_index_hints(thd);
            Select->set_index_hint_type(INDEX_HINT_USE,
                                        INDEX_HINT_MASK_ALL);
          }
          cache_key_list_or_empty
        ;

cache_key_list_or_empty:
          /* empty */ { }
        | key_or_index '(' opt_key_usage_list ')'
        ;

opt_ignore_leaves:
          /* empty */
          { $$= 0; }
        | IGNORE_SYM LEAVES { $$= TL_OPTION_IGNORE_LEAVES; }
        ;

/*
  Select : retrieve data from table
*/

select:
          query_expression_body
          {
            if (Lex->push_select($1->fake_select_lex ?
                                 $1->fake_select_lex :
                                 $1->first_select()))
              MYSQL_YYABORT;
          }
          opt_procedure_or_into
          {
            Lex->pop_select();
            if ($1->set_lock_to_the_last_select($3))
              MYSQL_YYABORT;
            if (Lex->select_finalize($1))
              MYSQL_YYABORT;
          }
        | with_clause query_expression_body
          {
            if (Lex->push_select($2->fake_select_lex ?
                                 $2->fake_select_lex :
                                 $2->first_select()))
              MYSQL_YYABORT;
          }
          opt_procedure_or_into
          {
            Lex->pop_select();
            $2->set_with_clause($1);
            $1->attach_to($2->first_select());
            if ($2->set_lock_to_the_last_select($4))
              MYSQL_YYABORT;
            if (Lex->select_finalize($2))
              MYSQL_YYABORT;
          }
        ;


select_into:
          select_into_query_specification
          {
            if (Lex->push_select($1))
              MYSQL_YYABORT;
          }
          opt_order_limit_lock
          {
            st_select_lex_unit *unit;
            if (!(unit= Lex->parsed_body_select($1, $3)))
              MYSQL_YYABORT;
            if (Lex->select_finalize(unit))
              MYSQL_YYABORT;
           }
         ;


simple_table:
          query_specification      { $$= $1; }
        | table_value_constructor  { $$= $1; }
        ;

table_value_constructor:
	  VALUES
	  {
            if (Lex->parsed_TVC_start())
              MYSQL_YYABORT;
	  }
	  values_list
	  {
            if (!($$= Lex->parsed_TVC_end()))
	      MYSQL_YYABORT;
	  }
	;

query_specification_start:
          SELECT_SYM
          {
            SELECT_LEX *sel;
            LEX *lex= Lex;
            if (!(sel= lex->alloc_select(TRUE)) ||
                  lex->push_select(sel))
              MYSQL_YYABORT;
            sel->init_select();
            sel->braces= FALSE;
          }
          select_options
          {
            Select->parsing_place= SELECT_LIST;
          }
          select_item_list
          {
            Select->parsing_place= NO_MATTER;
          }
          ;

query_specification:
          query_specification_start
          opt_from_clause
          opt_where_clause
          opt_group_clause
          opt_having_clause
          opt_window_clause
          {
            $$= Lex->pop_select();
          }
        ;

select_into_query_specification:
          query_specification_start
          into
          opt_from_clause
          opt_where_clause
          opt_group_clause
          opt_having_clause
          opt_window_clause
          {
            $$= Lex->pop_select();
          }
        ;

opt_from_clause:
        /* Empty */
        | from_clause
        ;


query_primary:
          simple_table
          { $$= $1; }
        | query_primary_parens
          { $$= $1; }
        ;

query_primary_parens:
          '(' query_expression_unit
          {
            if (Lex->parsed_unit_in_brackets($2))
              MYSQL_YYABORT;
          }
          query_expression_tail ')'
          {
            $$= Lex->parsed_unit_in_brackets_tail($2, $4);
          }
        | '(' query_primary
          {
            Lex->push_select($2);
          }
          query_expression_tail ')'
          {
            if (!($$= Lex->parsed_select_in_brackets($2, $4)))
              YYABORT;
          }
        ;

query_expression_unit:
          query_primary
          unit_type_decl
          query_primary
          {
            if (!($$= Lex->parsed_select_expr_start($1, $3, $2.unit_type,
                                                    $2.distinct)))
              YYABORT;
          }
        | query_expression_unit
          unit_type_decl
          query_primary
          {
            if (!($$= Lex->parsed_select_expr_cont($1, $3, $2.unit_type,
                                                   $2.distinct, FALSE)))
              YYABORT;
          }
        ;

query_expression_body:
          query_primary
          {
            Lex->push_select($1);
          }
          query_expression_tail
          {
            if (!($$= Lex->parsed_body_select($1, $3)))
              MYSQL_YYABORT;
          }
        | query_expression_unit
          {
            if (Lex->parsed_body_unit($1))
              MYSQL_YYABORT;
          }
          query_expression_tail
          {
            if (!($$= Lex->parsed_body_unit_tail($1, $3)))
              MYSQL_YYABORT;
          }
        ;

query_expression:
          opt_with_clause
          query_expression_body
          {
            if ($1)
            {
              $2->set_with_clause($1);
              $1->attach_to($2->first_select());
            }
            $$= $2;
          }
        ;

subselect:
          remember_tok_start
          query_expression
          {
            if (!($$= Lex->parsed_subselect($2, $1)))
              YYABORT;
          }
        ;


/**
  <table expression>, as in the SQL standard.
*/

from_clause:
          FROM table_reference_list
        ;

table_reference_list:
          join_table_list
          {
            Select->context.table_list=
              Select->context.first_name_resolution_table=
                Select->table_list.first;
          }
        | DUAL_SYM
          /* oracle compatibility: oracle always requires FROM clause,
             and DUAL is system table without fields.
             Is "SELECT 1 FROM DUAL" any better than "SELECT 1" ?
          Hmmm :) */
        ;

select_options:
          /* empty*/
        | select_option_list
          {
            if (unlikely((Select->options & SELECT_DISTINCT) &&
                         (Select->options & SELECT_ALL)))
              my_yyabort_error((ER_WRONG_USAGE, MYF(0), "ALL", "DISTINCT"));
          }
        ;

opt_history_unit:
          /* empty*/         %prec PREC_BELOW_IDENTIFIER_OPT_SPECIAL_CASE
          {
            $$= VERS_UNDEFINED;
          }
        | TRANSACTION_SYM
          {
            $$= VERS_TRX_ID;
          }
        | TIMESTAMP
          {
            $$= VERS_TIMESTAMP;
          }
        ;

history_point:
          TIMESTAMP TEXT_STRING
          {
            Item *item;
            if (!(item= type_handler_datetime.create_literal_item(thd,
                                                     $2.str, $2.length,
                                                     YYCSCL, true)))
              MYSQL_YYABORT;
            $$= Vers_history_point(VERS_TIMESTAMP, item);
          }
        | function_call_keyword_timestamp
          {
            $$= Vers_history_point(VERS_TIMESTAMP, $1);
          }
        | opt_history_unit bit_expr
          {
            $$= Vers_history_point($1, $2);
          }
        ;

opt_for_system_time_clause:
          /* empty */
          {
            $$= false;
          }
        | FOR_SYSTEM_TIME_SYM system_time_expr
          {
            $$= true;
          }
        ;

system_time_expr:
          AS OF_SYM history_point
          {
            Lex->vers_conditions.init(SYSTEM_TIME_AS_OF, $3);
          }
        | ALL
          {
            Lex->vers_conditions.init(SYSTEM_TIME_ALL);
          }
        | FROM history_point TO_SYM history_point
          {
            Lex->vers_conditions.init(SYSTEM_TIME_FROM_TO, $2, $4);
          }
        | BETWEEN_SYM history_point AND_SYM history_point
          {
            Lex->vers_conditions.init(SYSTEM_TIME_BETWEEN, $2, $4);
          }
        ;

select_option_list:
          select_option_list select_option
        | select_option
        ;

select_option:
          query_expression_option
        | SQL_NO_CACHE_SYM
          {
            /*
              Allow this flag once per query.
            */
            if (Select->options & OPTION_NO_QUERY_CACHE)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "SQL_NO_CACHE"));
            Select->options|= OPTION_NO_QUERY_CACHE;
          }
        | SQL_CACHE_SYM
          {
            /*
              Allow this flag once per query.
            */
            if (Select->options & OPTION_TO_QUERY_CACHE)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "SQL_CACHE"));
            Select->options|= OPTION_TO_QUERY_CACHE;
          }
        ;


select_lock_type:
          FOR_SYM UPDATE_SYM opt_lock_wait_timeout_new
          {
            $$= $3;
            $$.defined_lock= TRUE;
            $$.update_lock= TRUE;
          }
        | LOCK_SYM IN_SYM SHARE_SYM MODE_SYM opt_lock_wait_timeout_new
          {
            $$= $5;
            $$.defined_lock= TRUE;
            $$.update_lock= FALSE;
          }
        ;

opt_select_lock_type:
        /* empty */
        {
          $$.empty();
        }
        | select_lock_type
        {
          $$= $1;
        }
        ;

opt_lock_wait_timeout_new:
        /* empty */
        {
          $$.empty();
        }
        | WAIT_SYM ulong_num
        {
          $$.defined_timeout= TRUE;
          $$.timeout= $2;
        }
        | NOWAIT_SYM
        {
          $$.defined_timeout= TRUE;
          $$.timeout= 0;
        }
      ;

select_item_list:
          select_item_list ',' select_item
        | select_item
        | '*'
          {
            Item *item= new (thd->mem_root)
                          Item_field(thd, &thd->lex->current_select->context,
                                     NULL, NULL, &star_clex_str);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            if (unlikely(add_item_to_list(thd, item)))
              MYSQL_YYABORT;
            (thd->lex->current_select->with_wild)++;
          }
        ;

select_item:
          remember_name select_sublist_qualified_asterisk remember_end
          {
            if (unlikely(add_item_to_list(thd, $2)))
              MYSQL_YYABORT;
          }
        | remember_name expr remember_end select_alias
          {
            DBUG_ASSERT($1 < $3);

            if (unlikely(add_item_to_list(thd, $2)))
              MYSQL_YYABORT;
            if ($4.str)
            {
              if (unlikely(Lex->sql_command == SQLCOM_CREATE_VIEW &&
                          check_column_name($4.str)))
                my_yyabort_error((ER_WRONG_COLUMN_NAME, MYF(0), $4.str));
              $2->is_autogenerated_name= FALSE;
              $2->set_name(thd, $4.str, $4.length, system_charset_info);
            }
            else if (!$2->name.str || $2->name.str == item_empty_name)
            {
              $2->set_name(thd, $1, (uint) ($3 - $1), thd->charset());
            }
          }
        ;

remember_tok_start:
          {
            $$= (char*) YYLIP->get_tok_start();
          }
        ;

remember_tok_end:
          {
            $$= (char*) YYLIP->get_tok_end();
          }
        ;

remember_name:
          {
            $$= (char*) YYLIP->get_cpp_tok_start();
          }
        ;

remember_end:
          {
            $$= (char*) YYLIP->get_cpp_tok_end_rtrim();
          }
        ;

select_alias:
          /* empty */ { $$=null_clex_str;}
        | AS ident { $$=$2; }
        | AS TEXT_STRING_sys { $$=$2; }
        | ident { $$=$1; }
        | TEXT_STRING_sys { $$=$1; }
        ;

opt_default_time_precision:
          /* empty */             { $$= NOT_FIXED_DEC;  }
        | '(' ')'                 { $$= NOT_FIXED_DEC;  }
        | '(' real_ulong_num ')'  { $$= $2; }
        ;

opt_time_precision:
          /* empty */             { $$= 0;  }
        | '(' ')'                 { $$= 0;  }
        | '(' real_ulong_num ')'  { $$= $2; }
        ;

optional_braces:
          /* empty */ {}
        | '(' ')' {}
        ;

/* all possible expressions */
expr:
          expr or expr %prec OR_SYM
          {
            /*
              Design notes:
              Do not use a manually maintained stack like thd->lex->xxx_list,
              but use the internal bison stack ($$, $1 and $3) instead.
              Using the bison stack is:
              - more robust to changes in the grammar,
              - guaranteed to be in sync with the parser state,
              - better for performances (no memory allocation).
            */
            Item_cond_or *item1;
            Item_cond_or *item3;
            if (is_cond_or($1))
            {
              item1= (Item_cond_or*) $1;
              if (is_cond_or($3))
              {
                item3= (Item_cond_or*) $3;
                /*
                  (X1 OR X2) OR (Y1 OR Y2) ==> OR (X1, X2, Y1, Y2)
                */
                item3->add_at_head(item1->argument_list());
                $$ = $3;
              }
              else
              {
                /*
                  (X1 OR X2) OR Y ==> OR (X1, X2, Y)
                */
                item1->add($3, thd->mem_root);
                $$ = $1;
              }
            }
            else if (is_cond_or($3))
            {
              item3= (Item_cond_or*) $3;
              /*
                X OR (Y1 OR Y2) ==> OR (X, Y1, Y2)
              */
              item3->add_at_head($1, thd->mem_root);
              $$ = $3;
            }
            else
            {
              /* X OR Y */
              $$= new (thd->mem_root) Item_cond_or(thd, $1, $3);
              if (unlikely($$ == NULL))
                MYSQL_YYABORT;
            }
          }
        | expr XOR expr %prec XOR
          {
            /* XOR is a proprietary extension */
            $$= new (thd->mem_root) Item_func_xor(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | expr and expr %prec AND_SYM
          {
            /* See comments in rule expr: expr or expr */
            Item_cond_and *item1;
            Item_cond_and *item3;
            if (is_cond_and($1))
            {
              item1= (Item_cond_and*) $1;
              if (is_cond_and($3))
              {
                item3= (Item_cond_and*) $3;
                /*
                  (X1 AND X2) AND (Y1 AND Y2) ==> AND (X1, X2, Y1, Y2)
                */
                item3->add_at_head(item1->argument_list());
                $$ = $3;
              }
              else
              {
                /*
                  (X1 AND X2) AND Y ==> AND (X1, X2, Y)
                */
                item1->add($3, thd->mem_root);
                $$ = $1;
              }
            }
            else if (is_cond_and($3))
            {
              item3= (Item_cond_and*) $3;
              /*
                X AND (Y1 AND Y2) ==> AND (X, Y1, Y2)
              */
              item3->add_at_head($1, thd->mem_root);
              $$ = $3;
            }
            else
            {
              /* X AND Y */
              $$= new (thd->mem_root) Item_cond_and(thd, $1, $3);
              if (unlikely($$ == NULL))
                MYSQL_YYABORT;
            }
          }
        | NOT_SYM expr %prec NOT_SYM
          {
            $$= negate_expression(thd, $2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri IS TRUE_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_istrue(thd, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri IS not TRUE_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnottrue(thd, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri IS FALSE_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isfalse(thd, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri IS not FALSE_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnotfalse(thd, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri IS UNKNOWN_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnull(thd, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri IS not UNKNOWN_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnotnull(thd, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri %prec PREC_BELOW_NOT
        ;

bool_pri:
          bool_pri IS NULL_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnull(thd, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri IS not NULL_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnotnull(thd, $1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri EQUAL_SYM predicate %prec EQUAL_SYM
          {
            $$= new (thd->mem_root) Item_func_equal(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri comp_op predicate %prec '='
          {
            $$= (*$2)(0)->create(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bool_pri comp_op all_or_any '(' subselect ')' %prec '='
          {
            $$= all_any_subquery_creator(thd, $1, $2, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | predicate
        ;

predicate:
          bit_expr IN_SYM '(' subselect ')'
          {
            $$= new (thd->mem_root) Item_in_subselect(thd, $1, $4);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr not IN_SYM '(' subselect ')'
          {
            Item *item= new (thd->mem_root) Item_in_subselect(thd, $1, $5);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= negate_expression(thd, item);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr IN_SYM '(' expr ')'
          {
            $$= handle_sql2003_note184_exception(thd, $1, true, $4);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr IN_SYM '(' expr ',' expr_list ')'
          { 
            $6->push_front($4, thd->mem_root);
            $6->push_front($1, thd->mem_root);
            $$= new (thd->mem_root) Item_func_in(thd, *$6);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr not IN_SYM '(' expr ')'
          {
            $$= handle_sql2003_note184_exception(thd, $1, false, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr not IN_SYM '(' expr ',' expr_list ')'
          {
            $7->push_front($5, thd->mem_root);
            $7->push_front($1, thd->mem_root);
            Item_func_in *item= new (thd->mem_root) Item_func_in(thd, *$7);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= item->neg_transformer(thd);
          }
        | bit_expr BETWEEN_SYM bit_expr AND_SYM predicate
          {
            $$= new (thd->mem_root) Item_func_between(thd, $1, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr not BETWEEN_SYM bit_expr AND_SYM predicate
          {
            Item_func_between *item;
            item= new (thd->mem_root) Item_func_between(thd, $1, $4, $6);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= item->neg_transformer(thd);
          }
        | bit_expr SOUNDS_SYM LIKE bit_expr
          {
            Item *item1= new (thd->mem_root) Item_func_soundex(thd, $1);
            Item *item4= new (thd->mem_root) Item_func_soundex(thd, $4);
            if (unlikely(item1 == NULL) || unlikely(item4 == NULL))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_func_eq(thd, item1, item4);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr LIKE bit_expr opt_escape
          {
            $$= new (thd->mem_root) Item_func_like(thd, $1, $3, $4,
                                                   Lex->escape_used);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr not LIKE bit_expr opt_escape
          {
            Item *item= new (thd->mem_root) Item_func_like(thd, $1, $4, $5,
                                                             Lex->escape_used);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= item->neg_transformer(thd);
          }
        | bit_expr REGEXP bit_expr
          {
            $$= new (thd->mem_root) Item_func_regex(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr not REGEXP bit_expr
          {
            Item *item= new (thd->mem_root) Item_func_regex(thd, $1, $4);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= negate_expression(thd, item);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr %prec PREC_BELOW_NOT
        ;

bit_expr:
          bit_expr '|' bit_expr %prec '|'
          {
            $$= new (thd->mem_root) Item_func_bit_or(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '&' bit_expr %prec '&'
          {
            $$= new (thd->mem_root) Item_func_bit_and(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr SHIFT_LEFT bit_expr %prec SHIFT_LEFT
          {
            $$= new (thd->mem_root) Item_func_shift_left(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr SHIFT_RIGHT bit_expr %prec SHIFT_RIGHT
          {
            $$= new (thd->mem_root) Item_func_shift_right(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr ORACLE_CONCAT_SYM bit_expr
          {
            $$= new (thd->mem_root) Item_func_concat_operator_oracle(thd,
                                                                     $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '+' bit_expr %prec '+'
          {
            $$= new (thd->mem_root) Item_func_plus(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '-' bit_expr %prec '-'
          {
            $$= new (thd->mem_root) Item_func_minus(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '+' INTERVAL_SYM expr interval %prec '+'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $1, $4, $5, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '-' INTERVAL_SYM expr interval %prec '-'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $1, $4, $5, 1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | INTERVAL_SYM expr interval '+' expr
          /* we cannot put interval before - */
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $5, $2, $3, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | '+' INTERVAL_SYM expr interval '+' expr %prec NEG
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $6, $3, $4, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | '-' INTERVAL_SYM expr interval '+' expr %prec NEG
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $6, $3, $4, 1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '*' bit_expr %prec '*'
          {
            $$= new (thd->mem_root) Item_func_mul(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '/' bit_expr %prec '/'
          {
            $$= new (thd->mem_root) Item_func_div(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '%' bit_expr %prec '%'
          {
            $$= new (thd->mem_root) Item_func_mod(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr DIV_SYM bit_expr %prec DIV_SYM
          {
            $$= new (thd->mem_root) Item_func_int_div(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr MOD_SYM bit_expr %prec MOD_SYM
          {
            $$= new (thd->mem_root) Item_func_mod(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | bit_expr '^' bit_expr
          {
            $$= new (thd->mem_root) Item_func_bit_xor(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | mysql_concatenation_expr %prec '^'
        ;

or:
          OR_SYM
       | OR2_SYM
       ;

and:
          AND_SYM
       | AND_AND_SYM
       ;

not:
          NOT_SYM
        | NOT2_SYM
        ;

not2:
          '!'
        | NOT2_SYM
        ;

comp_op:
          '='     { $$ = &comp_eq_creator; }
        | GE     { $$ = &comp_ge_creator; }
        | '>' { $$ = &comp_gt_creator; }
        | LE     { $$ = &comp_le_creator; }
        | '<'     { $$ = &comp_lt_creator; }
        | NE     { $$ = &comp_ne_creator; }
        ;

all_or_any:
          ALL     { $$ = 1; }
        | ANY_SYM { $$ = 0; }
        ;

opt_dyncol_type:
          /* empty */ 
          {
            $$.set(DYN_COL_NULL); /* automatic type */
            Lex->charset= NULL;
	  }
        | AS dyncol_type { $$= $2; }
        ;

dyncol_type:
          numeric_dyncol_type             { $$= $1; Lex->charset= NULL; }
        | temporal_dyncol_type            { $$= $1; Lex->charset= NULL; }
        | string_dyncol_type              { $$= $1; }
        ;

numeric_dyncol_type:
          INT_SYM                         { $$.set(DYN_COL_INT); }
        | UNSIGNED INT_SYM                { $$.set(DYN_COL_UINT);  }
        | DOUBLE_SYM                      { $$.set(DYN_COL_DOUBLE);  }
        | REAL                            { $$.set(DYN_COL_DOUBLE); }
        | FLOAT_SYM                       { $$.set(DYN_COL_DOUBLE); }
        | DECIMAL_SYM float_options       { $$.set(DYN_COL_DECIMAL, $2); }
        ;

temporal_dyncol_type:
          DATE_SYM                        { $$.set(DYN_COL_DATE); }
        | TIME_SYM opt_field_length       { $$.set(DYN_COL_TIME, 0, $2); }
        | DATETIME opt_field_length       { $$.set(DYN_COL_DATETIME, 0, $2); }
        ;

string_dyncol_type:
          char
          { Lex->charset= thd->variables.collation_connection; }
          opt_binary
          {
            $$.set(DYN_COL_STRING);
          }
        | nchar
          {
            $$.set(DYN_COL_STRING);
            Lex->charset= national_charset_info;
          }
        ;

dyncall_create_element:
   expr ',' expr opt_dyncol_type
   {
     LEX *lex= Lex;
     $$= (DYNCALL_CREATE_DEF *)
       alloc_root(thd->mem_root, sizeof(DYNCALL_CREATE_DEF));
     if (unlikely($$ == NULL))
       MYSQL_YYABORT;
     $$->key= $1;
     $$->value= $3;
     $$->type= (DYNAMIC_COLUMN_TYPE)$4.dyncol_type();
     $$->cs= lex->charset;
     if ($4.length())
       $$->len= strtoul($4.length(), NULL, 10);
     else
       $$->len= 0;
     if ($4.dec())
       $$->frac= strtoul($4.dec(), NULL, 10);
     else
       $$->len= 0;
   }
   ;

dyncall_create_list:
     dyncall_create_element
       {
         $$= new (thd->mem_root) List<DYNCALL_CREATE_DEF>;
         if (unlikely($$ == NULL))
           MYSQL_YYABORT;
         $$->push_back($1, thd->mem_root);
       }
   | dyncall_create_list ',' dyncall_create_element
       {
         $1->push_back($3, thd->mem_root);
         $$= $1;
       }
   ;


plsql_cursor_attr:
          ISOPEN_SYM    { $$= PLSQL_CURSOR_ATTR_ISOPEN; }
        | FOUND_SYM     { $$= PLSQL_CURSOR_ATTR_FOUND; }
        | NOTFOUND_SYM  { $$= PLSQL_CURSOR_ATTR_NOTFOUND; }
        | ROWCOUNT_SYM  { $$= PLSQL_CURSOR_ATTR_ROWCOUNT; }
        ;

explicit_cursor_attr:
          ident PERCENT_ORACLE_SYM plsql_cursor_attr
          {
            if (unlikely(!($$= Lex->make_item_plsql_cursor_attr(thd, &$1, $3))))
              MYSQL_YYABORT;
          }
        ;


trim_operands:
          expr                     { $$.set(TRIM_BOTH, $1);         }
        | LEADING  expr FROM expr  { $$.set(TRIM_LEADING, $2, $4);  }
        | TRAILING expr FROM expr  { $$.set(TRIM_TRAILING, $2, $4); }
        | BOTH     expr FROM expr  { $$.set(TRIM_BOTH, $2, $4);     }
        | LEADING       FROM expr  { $$.set(TRIM_LEADING, $3);      }
        | TRAILING      FROM expr  { $$.set(TRIM_TRAILING, $3);     }
        | BOTH          FROM expr  { $$.set(TRIM_BOTH, $3);         }
        | expr          FROM expr  { $$.set(TRIM_BOTH, $1, $3);     }
        ;

/*
  Expressions that the parser allows in a column DEFAULT clause
  without parentheses. These expressions cannot end with a COLLATE clause.

  If we allowed any "expr" in DEFAULT clause, there would be a confusion
  in queries like this:
    CREATE TABLE t1 (a TEXT DEFAULT 'a' COLLATE latin1_bin);
  It would be not clear what COLLATE stands for:
  - the collation of the column `a`, or
  - the collation of the string literal 'a'

  This restriction allows to parse the above query unambiguiusly:
  COLLATE belongs to the column rather than the literal.
  If one needs COLLATE to belong to the literal, parentheses must be used:
    CREATE TABLE t1 (a TEXT DEFAULT ('a' COLLATE latin1_bin));
  Note: the COLLATE clause is rather meaningless here, but the query
  is syntactically correct.

  Note, some of the expressions are not actually allowed in DEFAULT,
  e.g. sum_expr, window_func_expr, ROW(...), VALUES().
  We could move them to simple_expr, but that would make
  these two queries return a different error messages:
    CREATE TABLE t1 (a INT DEFAULT AVG(1));
    CREATE TABLE t1 (a INT DEFAULT (AVG(1)));
  The first query would return "syntax error".
  Currenly both return:
   Function or expression 'avg(' is not allowed for 'DEFAULT' ...
*/
column_default_non_parenthesized_expr:
          simple_ident
        | function_call_keyword
        | function_call_nonkeyword
        | function_call_generic
        | function_call_conflict
        | literal
        | param_marker { $$= $1; }
        | variable
        | sum_expr
          {
            if (!Lex->select_stack_top)
            {
              my_error(ER_INVALID_GROUP_FUNC_USE, MYF(0));
              MYSQL_YYABORT;
            }
          }
        | window_func_expr
          {
            if (!Lex->select_stack_top)
            {
               my_error(ER_WRONG_PLACEMENT_OF_WINDOW_FUNCTION, MYF(0));
               MYSQL_YYABORT;
            }
          }
        | inverse_distribution_function
        | ROW_SYM '(' expr ',' expr_list ')'
          {
            $5->push_front($3, thd->mem_root);
            $$= new (thd->mem_root) Item_row(thd, *$5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | EXISTS '(' subselect ')'
          {
            $$= new (thd->mem_root) Item_exists_subselect(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | '{' ident expr '}'
          {
            if (unlikely(!($$= $3->make_odbc_literal(thd, &$2))))
              MYSQL_YYABORT;
          }
        | MATCH ident_list_arg AGAINST '(' bit_expr fulltext_options ')'
          {
            $2->push_front($5, thd->mem_root);
            Item_func_match *i1= new (thd->mem_root) Item_func_match(thd, *$2,
                                                                     $6);
            if (unlikely(i1 == NULL))
              MYSQL_YYABORT;
            Select->add_ftfunc_to_list(thd, i1);
            $$= i1;
          }
        | CAST_SYM '(' expr AS cast_type ')'
          {
            if (unlikely(!($$= $5.create_typecast_item(thd, $3, Lex->charset))))
              MYSQL_YYABORT;
          }
        | CASE_SYM when_list_opt_else END
          {
            if (unlikely(!($$= new(thd->mem_root) Item_func_case_searched(thd, *$2))))
              MYSQL_YYABORT;
          }
        | CASE_SYM expr when_list_opt_else END
          {
            $3->push_front($2, thd->mem_root);
            if (unlikely(!($$= new (thd->mem_root) Item_func_case_simple(thd, *$3))))
              MYSQL_YYABORT;
          }
        | CONVERT_SYM '(' expr ',' cast_type ')'
          {
            if (unlikely(!($$= $5.create_typecast_item(thd, $3, Lex->charset))))
              MYSQL_YYABORT;
          }
        | CONVERT_SYM '(' expr USING charset_name ')'
          {
            $$= new (thd->mem_root) Item_func_conv_charset(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DEFAULT '(' simple_ident ')'
          {
            Item_splocal *il= $3->get_item_splocal();
            if (unlikely(il))
              my_yyabort_error((ER_WRONG_COLUMN_NAME, MYF(0), il->my_name()->str));
            $$= new (thd->mem_root) Item_default_value(thd, Lex->current_context(),
                                                         $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->default_used= TRUE;
          }
        | VALUE_SYM '(' simple_ident_nospvar ')'
          {
            $$= new (thd->mem_root) Item_insert_value(thd, Lex->current_context(),
                                                        $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | NEXT_SYM VALUE_SYM FOR_SYM table_ident
          {
            if (unlikely(!($$= Lex->create_item_func_nextval(thd, $4))))
              MYSQL_YYABORT;
          }
        | NEXTVAL_SYM '(' table_ident ')'
          {
            if (unlikely(!($$= Lex->create_item_func_nextval(thd, $3))))
              MYSQL_YYABORT;
          }
        | PREVIOUS_SYM VALUE_SYM FOR_SYM table_ident
          {
            if (unlikely(!($$= Lex->create_item_func_lastval(thd, $4))))
              MYSQL_YYABORT;
          }
        | LASTVAL_SYM '(' table_ident ')'
          {
            if (unlikely(!($$= Lex->create_item_func_lastval(thd, $3))))
              MYSQL_YYABORT;
          }
        | SETVAL_SYM '(' table_ident ',' longlong_num ')'
          {
            if (unlikely(!($$= Lex->create_item_func_setval(thd, $3, $5, 0, 1))))
              MYSQL_YYABORT;
          }
        | SETVAL_SYM '(' table_ident ',' longlong_num ',' bool ')'
          {
            if (unlikely(!($$= Lex->create_item_func_setval(thd, $3, $5, 0, $7))))
              MYSQL_YYABORT;
          }
        | SETVAL_SYM '(' table_ident ',' longlong_num ',' bool ',' ulonglong_num ')'
          {
            if (unlikely(!($$= Lex->create_item_func_setval(thd, $3, $5, $9, $7))))
              MYSQL_YYABORT;
          }
        ;

primary_expr:
          column_default_non_parenthesized_expr
        | explicit_cursor_attr
        | '(' parenthesized_expr ')' { $$= $2; }
        ;

string_factor_expr:
          primary_expr
        | string_factor_expr COLLATE_SYM collation_name
          {
            if (unlikely(!($$= new (thd->mem_root) Item_func_set_collation(thd, $1, $3))))
              MYSQL_YYABORT;
          }
        ;

simple_expr:
          string_factor_expr %prec NEG
        | BINARY simple_expr
          {
            Type_cast_attributes at(&my_charset_bin);
            if (unlikely(!($$= type_handler_long_blob.create_typecast_item(thd, $2, at))))
              MYSQL_YYABORT;
          }
        | '+' simple_expr %prec NEG
          {
            $$= $2;
          }
        | '-' simple_expr %prec NEG
          {
            $$= $2->neg(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | '~' simple_expr %prec NEG
          {
            $$= new (thd->mem_root) Item_func_bit_neg(thd, $2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | not2 simple_expr %prec NEG
          {
            $$= negate_expression(thd, $2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

mysql_concatenation_expr:
          simple_expr
        | mysql_concatenation_expr MYSQL_CONCAT_SYM simple_expr
          {
            $$= new (thd->mem_root) Item_func_concat(thd, $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

function_call_keyword_timestamp:
          TIMESTAMP '(' expr ')'
          {
            $$= new (thd->mem_root) Item_datetime_typecast(thd, $3,
                                      AUTO_SEC_PART_DIGITS);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | TIMESTAMP '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_timestamp(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;
/*
  Function call syntax using official SQL 2003 keywords.
  Because the function name is an official token,
  a dedicated grammar rule is needed in the parser.
  There is no potential for conflicts
*/
function_call_keyword:
          CHAR_SYM '(' expr_list ')'
          {
            $$= new (thd->mem_root) Item_func_char(thd, *$3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | CHAR_SYM '(' expr_list USING charset_name ')'
          {
            $$= new (thd->mem_root) Item_func_char(thd, *$3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | CURRENT_USER optional_braces
          {
            $$= new (thd->mem_root) Item_func_current_user(thd,
                                      Lex->current_context());
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query= 0;
          }
        | CURRENT_ROLE optional_braces
          {
            $$= new (thd->mem_root) Item_func_current_role(thd,
                                      Lex->current_context());
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query= 0;
          }
        | DATE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_date_typecast(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DAY_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_dayofmonth(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | HOUR_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_hour(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | INSERT '(' expr ',' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_insert(thd, $3, $5, $7, $9);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | INTERVAL_SYM '(' expr ',' expr ')'
          {
            List<Item> *list= new (thd->mem_root) List<Item>;
            if (unlikely(list == NULL))
              MYSQL_YYABORT;
            if (unlikely(list->push_front($5, thd->mem_root)) ||
                unlikely(list->push_front($3, thd->mem_root)))
              MYSQL_YYABORT;
            Item_row *item= new (thd->mem_root) Item_row(thd, *list);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_func_interval(thd, item);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | INTERVAL_SYM '(' expr ',' expr ',' expr_list ')'
          {
            $7->push_front($5, thd->mem_root);
            $7->push_front($3, thd->mem_root);
            Item_row *item= new (thd->mem_root) Item_row(thd, *$7);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_func_interval(thd, item);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | LEFT '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_left(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | MINUTE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_minute(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | MONTH_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_month(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | RIGHT '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_right(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SECOND_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_second(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SQL_SYM PERCENT_ORACLE_SYM ROWCOUNT_SYM
          {
            $$= new (thd->mem_root) Item_func_oracle_sql_rowcount(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query= 0;
          }
        | TIME_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_time_typecast(thd, $3,
                                      AUTO_SEC_PART_DIGITS);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | function_call_keyword_timestamp
          {
            $$= $1;
          }
        | TRIM '(' trim_operands ')'
          {
            if (unlikely(!($$= $3.make_item_func_trim(thd))))
              MYSQL_YYABORT;
          }
        | USER_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_func_user(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query=0;
          }
        | YEAR_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_year(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

/*
  Function calls using non reserved keywords, with special syntaxic forms.
  Dedicated grammar rules are needed because of the syntax,
  but also have the potential to cause incompatibilities with other
  parts of the language.
  MAINTAINER:
  The only reasons a function should be added here are:
  - for compatibility reasons with another SQL syntax (CURDATE),
  - for typing reasons (GET_FORMAT)
  Any other 'Syntaxic sugar' enhancements should be *STRONGLY*
  discouraged.
*/
function_call_nonkeyword:
          ADDDATE_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $5,
                                                             INTERVAL_DAY, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | ADDDATE_SYM '(' expr ',' INTERVAL_SYM expr interval ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $6, $7, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | CURDATE optional_braces
          {
            $$= new (thd->mem_root) Item_func_curdate_local(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | CURTIME opt_time_precision
          {
            $$= new (thd->mem_root) Item_func_curtime_local(thd, $2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | DATE_ADD_INTERVAL '(' expr ',' INTERVAL_SYM expr interval ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $6, $7, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DATE_SUB_INTERVAL '(' expr ',' INTERVAL_SYM expr interval ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $6, $7, 1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DATE_FORMAT_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_date_format(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DATE_FORMAT_SYM '(' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_date_format(thd, $3, $5, $7);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DECODE_MARIADB_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_decode(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DECODE_ORACLE_SYM '(' expr ',' decode_when_list_oracle ')'
          {
            $5->push_front($3, thd->mem_root);
            if (unlikely(!($$= new (thd->mem_root) Item_func_decode_oracle(thd, *$5))))
              MYSQL_YYABORT;
          }
        | EXTRACT_SYM '(' interval FROM expr ')'
          {
            $$=new (thd->mem_root) Item_extract(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | GET_FORMAT '(' date_time_type  ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_get_format(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | NOW_SYM opt_time_precision
          {
            $$= new (thd->mem_root) Item_func_now_local(thd, $2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | POSITION_SYM '(' bit_expr IN_SYM expr ')'
          {
            $$= new (thd->mem_root) Item_func_locate(thd, $5, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SUBDATE_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $5,
                                                             INTERVAL_DAY, 1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SUBDATE_SYM '(' expr ',' INTERVAL_SYM expr interval ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $6, $7, 1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SUBSTRING '(' expr ',' expr ',' expr ')'
          {
            if (unlikely(!($$= Lex->make_item_func_substr(thd, $3, $5, $7))))
              MYSQL_YYABORT;
          }
        | SUBSTRING '(' expr ',' expr ')'
          {
            if (unlikely(!($$= Lex->make_item_func_substr(thd, $3, $5))))
              MYSQL_YYABORT;
          }
        | SUBSTRING '(' expr FROM expr FOR_SYM expr ')'
          {
            if (unlikely(!($$= Lex->make_item_func_substr(thd, $3, $5, $7))))
              MYSQL_YYABORT;
          }
        | SUBSTRING '(' expr FROM expr ')'
          {
            if (unlikely(!($$= Lex->make_item_func_substr(thd, $3, $5))))
              MYSQL_YYABORT;
          }
        | SYSDATE opt_time_precision
          {
            /*
              Unlike other time-related functions, SYSDATE() is
              replication-unsafe because it is not affected by the
              TIMESTAMP variable.  It is unsafe even if
              sysdate_is_now=1, because the slave may have
              sysdate_is_now=0.
            */
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            if (global_system_variables.sysdate_is_now == 0)
              $$= new (thd->mem_root) Item_func_sysdate_local(thd, $2);
            else
              $$= new (thd->mem_root) Item_func_now_local(thd, $2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | TIMESTAMP_ADD '(' interval_time_stamp ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $7, $5, $3, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | TIMESTAMP_DIFF '(' interval_time_stamp ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_timestamp_diff(thd, $5, $7, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | TRIM_ORACLE '(' trim_operands ')'
          {
            if (unlikely(!($$= $3.make_item_func_trim_oracle(thd))))
              MYSQL_YYABORT;
          }
        | UTC_DATE_SYM optional_braces
          {
            $$= new (thd->mem_root) Item_func_curdate_utc(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | UTC_TIME_SYM opt_time_precision
          {
            $$= new (thd->mem_root) Item_func_curtime_utc(thd, $2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | UTC_TIMESTAMP_SYM opt_time_precision
          {
            $$= new (thd->mem_root) Item_func_now_utc(thd, $2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        |
          COLUMN_ADD_SYM '(' expr ',' dyncall_create_list ')'
          {
            $$= create_func_dyncol_add(thd, $3, *$5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          COLUMN_DELETE_SYM '(' expr ',' expr_list ')'
          {
            $$= create_func_dyncol_delete(thd, $3, *$5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          COLUMN_CHECK_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_dyncol_check(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          COLUMN_CREATE_SYM '(' dyncall_create_list ')'
          {
            $$= create_func_dyncol_create(thd, *$3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          COLUMN_GET_SYM '(' expr ',' expr AS cast_type ')'
          {
            LEX *lex= Lex;
            $$= create_func_dyncol_get(thd, $3, $5, $7.type_handler(),
                                        $7.length(), $7.dec(),
                                        lex->charset);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

/*
  Functions calls using a non reserved keyword, and using a regular syntax.
  Because the non reserved keyword is used in another part of the grammar,
  a dedicated rule is needed here.
*/
function_call_conflict:
          ASCII_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_ascii(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | CHARSET '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_charset(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | COALESCE '(' expr_list ')'
          {
            $$= new (thd->mem_root) Item_func_coalesce(thd, *$3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | COLLATION_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_collation(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DATABASE '(' ')'
          {
            $$= new (thd->mem_root) Item_func_database(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | IF_SYM '(' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_if(thd, $3, $5, $7);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | FORMAT_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_format(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | FORMAT_SYM '(' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_format(thd, $3, $5, $7);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
          /* LAST_VALUE here conflicts with the definition for window functions.
             We have these 2 separate rules to remove the shift/reduce conflict.
          */
        | LAST_VALUE '(' expr ')'
          {
            List<Item> *list= new (thd->mem_root) List<Item>;
            if (unlikely(list == NULL))
              MYSQL_YYABORT;
            list->push_back($3, thd->mem_root);

            $$= new (thd->mem_root) Item_func_last_value(thd, *list);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | LAST_VALUE '(' expr_list ',' expr ')'
          {
            $3->push_back($5, thd->mem_root);
            $$= new (thd->mem_root) Item_func_last_value(thd, *$3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | MICROSECOND_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_microsecond(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | MOD_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_mod(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | OLD_PASSWORD_SYM '(' expr ')'
          {
            $$=  new (thd->mem_root)
              Item_func_password(thd, $3, Item_func_password::OLD);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | PASSWORD_SYM '(' expr ')'
          {
            Item* i1;
            i1= new (thd->mem_root) Item_func_password(thd, $3);
            if (unlikely(i1 == NULL))
              MYSQL_YYABORT;
            $$= i1;
          }
        | QUARTER_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_quarter(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | REPEAT_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_repeat(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | REPLACE '(' expr ',' expr ',' expr ')'
          {
            if (unlikely(!($$= Lex->make_item_func_replace(thd, $3, $5, $7))))
              MYSQL_YYABORT;
          }
        | REVERSE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_reverse(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | ROW_COUNT_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_func_row_count(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query= 0;
          }
        | TRUNCATE_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_round(thd, $3, $5, 1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | WEEK_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_week(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | WEEK_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_week(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | WEIGHT_STRING_SYM '(' expr opt_ws_levels ')'
          {
            $$= new (thd->mem_root) Item_func_weight_string(thd, $3, 0, 0, $4);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | WEIGHT_STRING_SYM '(' expr AS CHAR_SYM ws_nweights opt_ws_levels ')'
          {
            $$= new (thd->mem_root)
                Item_func_weight_string(thd, $3, 0, $6,
                                        $7 | MY_STRXFRM_PAD_WITH_SPACE);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | WEIGHT_STRING_SYM '(' expr AS BINARY ws_nweights ')'
          {
            Item *item= new (thd->mem_root) Item_char_typecast(thd, $3, $6,
                                                               &my_charset_bin);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= new (thd->mem_root)
                Item_func_weight_string(thd, item, 0, $6,
                                        MY_STRXFRM_PAD_WITH_SPACE);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | WEIGHT_STRING_SYM '(' expr ',' ulong_num ',' ulong_num ',' ulong_num ')'
          {
            $$= new (thd->mem_root) Item_func_weight_string(thd, $3, $5, $7,
                                                            $9);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | geometry_function
          {
#ifdef HAVE_SPATIAL
            $$= $1;
            /* $1 may be NULL, GEOM_NEW not tested for out of memory */
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
#else
            my_yyabort_error((ER_FEATURE_DISABLED, MYF(0), sym_group_geom.name,
                              sym_group_geom.needed_define));
#endif
          }
        ;

geometry_function:
          CONTAINS_SYM '(' expr ',' expr ')'
          {
            $$= GEOM_NEW(thd,
                         Item_func_spatial_precise_rel(thd, $3, $5,
                                                 Item_func::SP_CONTAINS_FUNC));
          }
        | GEOMETRYCOLLECTION '(' expr_list ')'
          {
            $$= GEOM_NEW(thd,
                         Item_func_spatial_collection(thd, *$3,
                           Geometry::wkb_geometrycollection,
                           Geometry::wkb_point));
          }
        | LINESTRING '(' expr_list ')'
          {
            $$= GEOM_NEW(thd,
                         Item_func_spatial_collection(thd, *$3,
                           Geometry::wkb_linestring,
                           Geometry::wkb_point));
          }
        | MULTILINESTRING '(' expr_list ')'
          {
            $$= GEOM_NEW(thd,
                         Item_func_spatial_collection(thd, *$3,
                           Geometry::wkb_multilinestring,
                           Geometry::wkb_linestring));
          }
        | MULTIPOINT '(' expr_list ')'
          {
            $$= GEOM_NEW(thd,
                         Item_func_spatial_collection(thd, *$3,
                           Geometry::wkb_multipoint,
                           Geometry::wkb_point));
          }
        | MULTIPOLYGON '(' expr_list ')'
          {
            $$= GEOM_NEW(thd,
                         Item_func_spatial_collection(thd, *$3,
                           Geometry::wkb_multipolygon,
                           Geometry::wkb_polygon));
          }
        | POINT_SYM '(' expr ',' expr ')'
          {
            $$= GEOM_NEW(thd, Item_func_point(thd, $3, $5));
          }
        | POLYGON '(' expr_list ')'
          {
            $$= GEOM_NEW(thd,
                         Item_func_spatial_collection(thd, *$3,
                           Geometry::wkb_polygon,
                           Geometry::wkb_linestring));
          }
        | WITHIN '(' expr ',' expr ')'
          {
            $$= GEOM_NEW(thd, Item_func_spatial_precise_rel(thd, $3, $5,
                                                    Item_func::SP_WITHIN_FUNC));
          }
        ;

/*
  Regular function calls.
  The function name is *not* a token, and therefore is guaranteed to not
  introduce side effects to the language in general.
  MAINTAINER:
  All the new functions implemented for new features should fit into
  this category. The place to implement the function itself is
  in sql/item_create.cc
*/
function_call_generic:
          IDENT_sys '('
          {
#ifdef HAVE_DLOPEN
            udf_func *udf= 0;
            LEX *lex= Lex;
            if (using_udf_functions &&
                (udf= find_udf($1.str, $1.length)) &&
                udf->type == UDFTYPE_AGGREGATE)
            {
              if (unlikely(lex->current_select->inc_in_sum_expr()))
              {
                thd->parse_error();
                MYSQL_YYABORT;
              }
            }
            /* Temporary placing the result of find_udf in $3 */
            $<udf>$= udf;
#endif
          }
          opt_udf_expr_list ')'
          {
            Create_func *builder;
            Item *item= NULL;

            if (unlikely(check_routine_name(&$1)))
              MYSQL_YYABORT;

            /*
              Implementation note:
              names are resolved with the following order:
              - MySQL native functions,
              - User Defined Functions,
              - Stored Functions (assuming the current <use> database)

              This will be revised with WL#2128 (SQL PATH)
            */
            builder= find_native_function_builder(thd, &$1);
            if (builder)
            {
              item= builder->create_func(thd, &$1, $4);
            }
            else
            {
#ifdef HAVE_DLOPEN
              /* Retrieving the result of find_udf */
              udf_func *udf= $<udf>3;

              if (udf)
              {
                if (udf->type == UDFTYPE_AGGREGATE)
                {
                  Select->in_sum_expr--;
                }

                item= Create_udf_func::s_singleton.create(thd, udf, $4);
              }
              else
#endif
              {
                builder= find_qualified_function_builder(thd);
                DBUG_ASSERT(builder);
                item= builder->create_func(thd, &$1, $4);
              }
            }

            if (unlikely(! ($$= item)))
              MYSQL_YYABORT;
          }
        | ident_cli '.' ident_cli '(' opt_expr_list ')'
          {
            if (unlikely(!($$= Lex->make_item_func_call_generic(thd, &$1, &$3, $5))))
              MYSQL_YYABORT;
          }
        ;

fulltext_options:
          opt_natural_language_mode opt_query_expansion
          { $$= $1 | $2; }
        | IN_SYM BOOLEAN_SYM MODE_SYM
          { $$= FT_BOOL; }
        ;

opt_natural_language_mode:
          /* nothing */                         { $$= FT_NL; }
        | IN_SYM NATURAL LANGUAGE_SYM MODE_SYM  { $$= FT_NL; }
        ;

opt_query_expansion:
          /* nothing */                         { $$= 0;         }
        | WITH QUERY_SYM EXPANSION_SYM          { $$= FT_EXPAND; }
        ;

opt_udf_expr_list:
        /* empty */     { $$= NULL; }
        | udf_expr_list { $$= $1; }
        ;

udf_expr_list:
          udf_expr
          {
            $$= new (thd->mem_root) List<Item>;
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            $$->push_back($1, thd->mem_root);
          }
        | udf_expr_list ',' udf_expr
          {
            $1->push_back($3, thd->mem_root);
            $$= $1;
          }
        ;

udf_expr:
          remember_name expr remember_end select_alias
          {
            /*
             Use Item::name as a storage for the attribute value of user
             defined function argument. It is safe to use Item::name
             because the syntax will not allow having an explicit name here.
             See WL#1017 re. udf attributes.
            */
            if ($4.str)
            {
              $2->is_autogenerated_name= FALSE;
              $2->set_name(thd, $4.str, $4.length, system_charset_info);
            }
            /* 
               A field has to have its proper name in order for name
               resolution to work, something we are only guaranteed if we
               parse it out. If we hijack the input stream with
               remember_name we may get quoted or escaped names.
            */
            else if ($2->type() != Item::FIELD_ITEM &&
                     $2->type() != Item::REF_ITEM /* For HAVING */ )
              $2->set_name(thd, $1, (uint) ($3 - $1), thd->charset());
            $$= $2;
          }
        ;

sum_expr:
          AVG_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_avg(thd, $3, FALSE);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | AVG_SYM '(' DISTINCT in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_avg(thd, $4, TRUE);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | BIT_AND  '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_and(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | BIT_OR  '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_or(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | BIT_XOR  '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_xor(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | COUNT_SYM '(' opt_all '*' ')'
          {
            Item *item= new (thd->mem_root) Item_int(thd, (int32) 0L, 1);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_sum_count(thd, item);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | COUNT_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_count(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | COUNT_SYM '(' DISTINCT
          { Select->in_sum_expr++; }
          expr_list
          { Select->in_sum_expr--; }
          ')'
          {
            $$= new (thd->mem_root) Item_sum_count(thd, *$5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | MIN_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_min(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        /*
          According to ANSI SQL, DISTINCT is allowed and has
          no sense inside MIN and MAX grouping functions; so MIN|MAX(DISTINCT ...)
          is processed like an ordinary MIN | MAX()
        */
        | MIN_SYM '(' DISTINCT in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_min(thd, $4);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | MAX_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_max(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | MAX_SYM '(' DISTINCT in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_max(thd, $4);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | STD_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_std(thd, $3, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | VARIANCE_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_variance(thd, $3, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | STDDEV_SAMP_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_std(thd, $3, 1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | VAR_SAMP_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_variance(thd, $3, 1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SUM_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_sum(thd, $3, FALSE);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SUM_SYM '(' DISTINCT in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_sum(thd, $4, TRUE);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | GROUP_CONCAT_SYM '(' opt_distinct
          { Select->in_sum_expr++; }
          expr_list opt_gorder_clause
          opt_gconcat_separator opt_glimit_clause
          ')'
          {
            SELECT_LEX *sel= Select;
            sel->in_sum_expr--;
            $$= new (thd->mem_root)
                  Item_func_group_concat(thd, Lex->current_context(),
                                        $3, $5,
                                        sel->gorder_list, $7, $8,
                                        sel->select_limit,
                                        sel->offset_limit);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            sel->select_limit= NULL;
            sel->offset_limit= NULL;
            sel->explicit_limit= 0;
            $5->empty();
            sel->gorder_list.empty();
          }
        ;

window_func_expr:
          window_func OVER_SYM window_name
          {
            $$= new (thd->mem_root) Item_window_func(thd, (Item_sum *) $1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            if (unlikely(Select->add_window_func((Item_window_func *) $$)))
              MYSQL_YYABORT;
          }
        |
          window_func OVER_SYM window_spec
          {
            LEX *lex= Lex;
            if (unlikely(Select->add_window_spec(thd, lex->win_ref,
                                                 Select->group_list,
                                                 Select->order_list,
                                                 lex->win_frame)))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_window_func(thd, (Item_sum *) $1,
                                                      thd->lex->win_spec); 
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            if (unlikely(Select->add_window_func((Item_window_func *) $$)))
              MYSQL_YYABORT;
          }
        ;

window_func:
          simple_window_func
        |
          sum_expr
          {
            ((Item_sum *) $1)->mark_as_window_func_sum_expr();
          }
        |
          function_call_generic
          {
            Item* item = (Item*)$1;
            /* Only UDF aggregate here possible */
            if ((item == NULL) ||
                (item->type() != Item::SUM_FUNC_ITEM)
                || (((Item_sum *)item)->sum_func() != Item_sum::UDF_SUM_FUNC))
            {
              thd->parse_error();
              MYSQL_YYABORT;
            }

            ((Item_sum *) $1)->mark_as_window_func_sum_expr();
          }
        ;

simple_window_func:
          ROW_NUMBER_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_row_number(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          RANK_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_rank(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          DENSE_RANK_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_dense_rank(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          PERCENT_RANK_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_percent_rank(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          CUME_DIST_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_cume_dist(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          NTILE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_ntile(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          FIRST_VALUE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_first_value(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          LAST_VALUE '(' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_last_value(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          NTH_VALUE_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_nth_value(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          LEAD_SYM '(' expr ')'
          {
            /* No second argument defaults to 1. */
            Item* item_offset= new (thd->mem_root) Item_uint(thd, 1);
            if (unlikely(item_offset == NULL))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_sum_lead(thd, $3, item_offset);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          LEAD_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_lead(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          LAG_SYM '(' expr ')'
          {
            /* No second argument defaults to 1. */
            Item* item_offset= new (thd->mem_root) Item_uint(thd, 1);
            if (unlikely(item_offset == NULL))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_sum_lag(thd, $3, item_offset);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |
          LAG_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_lag(thd, $3, $5);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;



inverse_distribution_function:
          percentile_function OVER_SYM
          '(' opt_window_partition_clause ')'
          {
            LEX *lex= Lex;
            if (unlikely(Select->add_window_spec(thd, lex->win_ref,
                                                 Select->group_list,
                                                 Select->order_list,
                                                 NULL)))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_window_func(thd, (Item_sum *) $1,
                                                     thd->lex->win_spec);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            if (unlikely(Select->add_window_func((Item_window_func *) $$)))
              MYSQL_YYABORT;
          }
        ;

percentile_function:
          inverse_distribution_function_def  WITHIN GROUP_SYM '('
           { Select->prepare_add_window_spec(thd); }
           order_by_single_element_list ')'
           {
             $$= $1;
           }
        | MEDIAN_SYM '(' expr ')'
          {
            Item *args= new (thd->mem_root) Item_decimal(thd, "0.5", 3,
                                                   thd->charset());
            if (unlikely(args == NULL) || unlikely(thd->is_error()))
              MYSQL_YYABORT;
            Select->prepare_add_window_spec(thd);
            if (unlikely(add_order_to_list(thd, $3,FALSE)))
              MYSQL_YYABORT;

            $$= new (thd->mem_root) Item_sum_percentile_cont(thd, args);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

inverse_distribution_function_def:
          PERCENTILE_CONT_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_percentile_cont(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        |  PERCENTILE_DISC_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_percentile_disc(thd, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

order_by_single_element_list:
          ORDER_SYM BY order_ident order_dir
          {
            if (unlikely(add_order_to_list(thd, $3,(bool) $4)))
              MYSQL_YYABORT;
          }
        ;


window_name:
          ident
          {
            $$= (LEX_CSTRING *) thd->memdup(&$1, sizeof(LEX_CSTRING));
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

variable:
          '@'
          {
            if (unlikely(! Lex->parsing_options.allows_variable))
              my_yyabort_error((ER_VIEW_SELECT_VARIABLE, MYF(0)));
          }
          variable_aux
          {
            $$= $3;
          }
        ;

variable_aux:
          ident_or_text SET_VAR expr
          {
            Item_func_set_user_var *item;
            $$= item= new (thd->mem_root) Item_func_set_user_var(thd, &$1, $3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            LEX *lex= Lex;
            lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
            lex->set_var_list.push_back(item, thd->mem_root);
          }
        | ident_or_text
          {
            $$= new (thd->mem_root) Item_func_get_user_var(thd, &$1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            LEX *lex= Lex;
            lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
          }
        | '@' opt_var_ident_type ident_sysvar_name
          {
            if (unlikely(!($$= Lex->make_item_sysvar(thd, $2, &$3))))
              MYSQL_YYABORT;
          }
        | '@' opt_var_ident_type ident_sysvar_name '.' ident
          {
            if (unlikely(!($$= Lex->make_item_sysvar(thd, $2, &$3, &$5))))
              MYSQL_YYABORT;
          }
        ;

opt_distinct:
          /* empty */ { $$ = 0; }
        | DISTINCT    { $$ = 1; }
        ;

opt_gconcat_separator:
          /* empty */
          {
            $$= new (thd->mem_root) String(",", 1, &my_charset_latin1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | SEPARATOR_SYM text_string { $$ = $2; }
        ;

opt_gorder_clause:
          /* empty */
        | ORDER_SYM BY gorder_list
        ;

gorder_list:
          gorder_list ',' order_ident order_dir
          {
            if (unlikely(add_gorder_to_list(thd, $3,(bool) $4)))
              MYSQL_YYABORT;
           }
        | order_ident order_dir
          {
            if (unlikely(add_gorder_to_list(thd, $1,(bool) $2)))
              MYSQL_YYABORT;
           }
        ;

opt_glimit_clause:
          /* empty */ { $$ = 0; }
        | glimit_clause { $$ = 1; }
        ;

glimit_clause_init:
          LIMIT{}
        ;

glimit_clause:
          glimit_clause_init glimit_options
          {
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
          }
        ;

glimit_options:
          limit_option
          {
            SELECT_LEX *sel= Select;
            sel->select_limit= $1;
            sel->offset_limit= 0;
            sel->explicit_limit= 1;
          }
        | limit_option ',' limit_option
          {
            SELECT_LEX *sel= Select;
            sel->select_limit= $3;
            sel->offset_limit= $1;
            sel->explicit_limit= 1;
          }
        | limit_option OFFSET_SYM limit_option
          {
            SELECT_LEX *sel= Select;
            sel->select_limit= $1;
            sel->offset_limit= $3;
            sel->explicit_limit= 1;
          }
        ;



in_sum_expr:
          opt_all
          {
            LEX *lex= Lex;
            if (unlikely(lex->current_select->inc_in_sum_expr()))
            {
              thd->parse_error();
              MYSQL_YYABORT;
            }
          }
          expr
          {
            Select->in_sum_expr--;
            $$= $3;
          }
        ;

cast_type:
          BINARY opt_field_length
          { $$.set(&type_handler_long_blob, $2); Lex->charset= &my_charset_bin; }
        | CHAR_SYM opt_field_length
          { Lex->charset= thd->variables.collation_connection; }
          opt_binary
          { $$.set(&type_handler_long_blob, $2); }
        | VARCHAR field_length
          { Lex->charset= thd->variables.collation_connection; }
          opt_binary
          { $$.set(&type_handler_long_blob, $2); }
        | VARCHAR2_ORACLE_SYM field_length
          { Lex->charset= thd->variables.collation_connection; }
          opt_binary
          { $$.set(&type_handler_long_blob, $2); }
        | NCHAR_SYM opt_field_length
          {
            Lex->charset= national_charset_info;
            $$.set(&type_handler_long_blob, $2, 0);
          }
        | cast_type_numeric  { $$= $1; Lex->charset= NULL; }
        | cast_type_temporal { $$= $1; Lex->charset= NULL; }
        ;

cast_type_numeric:
          INT_SYM                        { $$.set(&type_handler_longlong); }
        | SIGNED_SYM                     { $$.set(&type_handler_longlong); }
        | SIGNED_SYM INT_SYM             { $$.set(&type_handler_longlong); }
        | UNSIGNED                       { $$.set(&type_handler_ulonglong); }
        | UNSIGNED INT_SYM               { $$.set(&type_handler_ulonglong); }
        | DECIMAL_SYM float_options      { $$.set(&type_handler_newdecimal, $2); }
        | DOUBLE_SYM opt_precision       { $$.set(&type_handler_double, $2);  }
        ;

cast_type_temporal:
          DATE_SYM                       { $$.set(&type_handler_newdate); }
        | TIME_SYM opt_field_length      { $$.set(&type_handler_time2, 0, $2); }
        | DATETIME opt_field_length      { $$.set(&type_handler_datetime2, 0, $2); }
        | INTERVAL_SYM DAY_SECOND_SYM field_length
          {
            $$.set(&type_handler_interval_DDhhmmssff, 0, $3);
          }
        ;

opt_expr_list:
          /* empty */ { $$= NULL; }
        | expr_list { $$= $1;}
        ;

expr_list:
          expr
          {
            $$= new (thd->mem_root) List<Item>;
            if (unlikely($$ == NULL) ||
                unlikely($$->push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | expr_list ',' expr
          {
            $1->push_back($3, thd->mem_root);
            $$= $1;
          }
        ;

ident_list_arg:
          ident_list          { $$= $1; }
        | '(' ident_list ')'  { $$= $2; }
        ;

ident_list:
          simple_ident
          {
            $$= new (thd->mem_root) List<Item>;
            if (unlikely($$ == NULL) ||
                unlikely($$->push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | ident_list ',' simple_ident
          {
            $1->push_back($3, thd->mem_root);
            $$= $1;
          }
        ;

when_list:
          WHEN_SYM expr THEN_SYM expr
          {
            $$= new (thd->mem_root) List<Item>;
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            if (unlikely($$->push_back($2, thd->mem_root) ||
                         $$->push_back($4, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | when_list WHEN_SYM expr THEN_SYM expr
          {
            if (unlikely($1->push_back($3, thd->mem_root) ||
                         $1->push_back($5, thd->mem_root)))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

when_list_opt_else:
          when_list
        | when_list ELSE expr
          {
            if (unlikely($1->push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

decode_when_list_oracle:
          expr ',' expr
          {
            $$= new (thd->mem_root) List<Item>;
            if (unlikely($$ == NULL) ||
                unlikely($$->push_back($1, thd->mem_root)) ||
                unlikely($$->push_back($3, thd->mem_root)))
              MYSQL_YYABORT;

          }
        | decode_when_list_oracle ',' expr
          {
            $$= $1;
            if (unlikely($$->push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;


/* Equivalent to <table reference> in the SQL:2003 standard. */
/* Warning - may return NULL in case of incomplete SELECT */
table_ref:
          table_factor     { $$= $1; }
        | join_table
          {
            LEX *lex= Lex;
            if (unlikely(!($$= lex->current_select->nest_last_join(thd))))
            {
              thd->parse_error();
              MYSQL_YYABORT;
            }
          }
        ;

join_table_list:
          derived_table_list { MYSQL_YYABORT_UNLESS($$=$1); }
        ;

/*
  The ODBC escape syntax for Outer Join is: '{' OJ join_table '}'
  The parser does not define OJ as a token, any ident is accepted
  instead in $2 (ident). Also, all productions from table_ref can
  be escaped, not only join_table. Both syntax extensions are safe
  and are ignored.
*/
esc_table_ref:
        table_ref { $$=$1; }
      | '{' ident table_ref '}' { $$=$3; }
      ;

/* Equivalent to <table reference list> in the SQL:2003 standard. */
/* Warning - may return NULL in case of incomplete SELECT */
derived_table_list:
          esc_table_ref
         {
           $$=$1;
           Select->add_joined_table($1);
         }
        | derived_table_list ',' esc_table_ref
          {
            MYSQL_YYABORT_UNLESS($1 && ($$=$3));
            Select->add_joined_table($3);
          }
        ;

/*
  Notice that JOIN is a left-associative operation, and it must be parsed
  as such, that is, the parser must process first the left join operand
  then the right one. Such order of processing ensures that the parser
  produces correct join trees which is essential for semantic analysis
  and subsequent optimization phases.
*/
join_table:
          /* INNER JOIN variants */
          /*
            Use %prec to evaluate production 'table_ref' before 'normal_join'
            so that [INNER | CROSS] JOIN is properly nested as other
            left-associative joins.
          */
          table_ref normal_join table_ref %prec TABLE_REF_PRIORITY
          {
            MYSQL_YYABORT_UNLESS($1 && ($$=$3));
            Select->add_joined_table($1);
            Select->add_joined_table($3);
            $3->straight=$2;
          }
        | table_ref normal_join table_ref
          ON
          {
            MYSQL_YYABORT_UNLESS($1 && $3);
            Select->add_joined_table($1);
            Select->add_joined_table($3);
            /* Change the current name resolution context to a local context. */
            if (unlikely(push_new_name_resolution_context(thd, $1, $3)))
              MYSQL_YYABORT;
            Select->parsing_place= IN_ON;
          }
          expr
          {
	    $3->straight=$2;
            add_join_on(thd, $3, $6);
            Lex->pop_context();
            Select->parsing_place= NO_MATTER;
          }
        | table_ref normal_join table_ref
          USING
          {
            MYSQL_YYABORT_UNLESS($1 && $3);
            Select->add_joined_table($1);
            Select->add_joined_table($3);
          }
          '(' using_list ')'
          {
	    $3->straight=$2;
            add_join_natural($1,$3,$7,Select); 
	    $$=$3; 
          }
        | table_ref NATURAL inner_join table_factor
          {
            MYSQL_YYABORT_UNLESS($1 && ($$=$4));
            Select->add_joined_table($1);
            Select->add_joined_table($4);
	    $4->straight=$3;
            add_join_natural($1,$4,NULL,Select);
          }

          /* LEFT JOIN variants */
        | table_ref LEFT opt_outer JOIN_SYM table_ref
          ON
          {
            MYSQL_YYABORT_UNLESS($1 && $5);
            Select->add_joined_table($1);
            Select->add_joined_table($5);
            /* Change the current name resolution context to a local context. */
            if (unlikely(push_new_name_resolution_context(thd, $1, $5)))
              MYSQL_YYABORT;
            Select->parsing_place= IN_ON;
          }
          expr
          {
            add_join_on(thd, $5, $8);
            Lex->pop_context();
            $5->outer_join|=JOIN_TYPE_LEFT;
            $$=$5;
            Select->parsing_place= NO_MATTER;
          }
        | table_ref LEFT opt_outer JOIN_SYM table_factor
          {
            MYSQL_YYABORT_UNLESS($1 && $5);
            Select->add_joined_table($1);
            Select->add_joined_table($5);
          }
          USING '(' using_list ')'
          { 
            add_join_natural($1,$5,$9,Select); 
            $5->outer_join|=JOIN_TYPE_LEFT; 
            $$=$5; 
          }
        | table_ref NATURAL LEFT opt_outer JOIN_SYM table_factor
          {
            MYSQL_YYABORT_UNLESS($1 && $6);
            Select->add_joined_table($1);
            Select->add_joined_table($6);
            add_join_natural($1,$6,NULL,Select);
            $6->outer_join|=JOIN_TYPE_LEFT;
            $$=$6;
          }

          /* RIGHT JOIN variants */
        | table_ref RIGHT opt_outer JOIN_SYM table_ref
          ON
          {
            MYSQL_YYABORT_UNLESS($1 && $5);
            Select->add_joined_table($1);
            Select->add_joined_table($5);
            /* Change the current name resolution context to a local context. */
            if (unlikely(push_new_name_resolution_context(thd, $1, $5)))
              MYSQL_YYABORT;
            Select->parsing_place= IN_ON;
          }
          expr
          {
            LEX *lex= Lex;
            if (unlikely(!($$= lex->current_select->convert_right_join())))
              MYSQL_YYABORT;
            add_join_on(thd, $$, $8);
            Lex->pop_context();
            Select->parsing_place= NO_MATTER;
          }
        | table_ref RIGHT opt_outer JOIN_SYM table_factor
          {
            MYSQL_YYABORT_UNLESS($1 && $5);
            Select->add_joined_table($1);
            Select->add_joined_table($5);
          }
          USING '(' using_list ')'
          {
            LEX *lex= Lex;
            if (unlikely(!($$= lex->current_select->convert_right_join())))
              MYSQL_YYABORT;
            add_join_natural($$,$5,$9,Select);
          }
        | table_ref NATURAL RIGHT opt_outer JOIN_SYM table_factor
          {
            MYSQL_YYABORT_UNLESS($1 && $6);
            Select->add_joined_table($1);
            Select->add_joined_table($6);
            add_join_natural($6,$1,NULL,Select);
            LEX *lex= Lex;
            if (unlikely(!($$= lex->current_select->convert_right_join())))
              MYSQL_YYABORT;
          }
        ;


inner_join: /* $$ set if using STRAIGHT_JOIN, false otherwise */
          JOIN_SYM           { $$ = 0; }
        | INNER_SYM JOIN_SYM { $$ = 0; }
        | STRAIGHT_JOIN      { $$ = 1; }
        ;

normal_join:
          inner_join         { $$ = $1; }
        | CROSS JOIN_SYM     { $$ = 0; }
        ;

/*
  table PARTITION (list of partitions), reusing using_list instead of creating
  a new rule for partition_list.
*/
opt_use_partition:
          /* empty */ { $$= 0;}
        | use_partition
        ;
        
use_partition:
          PARTITION_SYM '(' using_list ')' have_partitioning
          {
            $$= $3;
          }
        ;

table_factor:
          table_primary_ident { $$= $1; }
        | table_primary_derived { $$= $1; }
        | join_table_parens { $$= $1; }
        | table_reference_list_parens { $$= $1; }
        ;

table_reference_list_parens:
          '(' table_reference_list_parens ')' { $$= $2; }
        | '(' nested_table_reference_list ')'
          {
            if (!($$= Select->end_nested_join(thd)))
              MYSQL_YYABORT;
          }
        ;

nested_table_reference_list:
          table_ref ',' table_ref
          {
            if (Select->init_nested_join(thd))
              MYSQL_YYABORT;
            Select->add_joined_table($1);
            Select->add_joined_table($3);
            $$= $1->embedding;
          }
        | nested_table_reference_list ',' table_ref
          {
            Select->add_joined_table($3);
            $$= $1;
          }
        ;

join_table_parens:
          '(' join_table_parens ')' { $$= $2; }
        | '(' join_table ')'
          {
            LEX *lex= Lex;
            if (!($$= lex->current_select->nest_last_join(thd)))
            {
              thd->parse_error();
              MYSQL_YYABORT;
            }
          }
        ;


table_primary_ident:
          table_ident opt_use_partition opt_for_system_time_clause
          opt_table_alias_clause opt_key_definition
          {
            SELECT_LEX *sel= Select;
            sel->table_join_options= 0;
            if (!($$= Select->add_table_to_list(thd, $1, $4,
                                                Select->get_table_join_options(),
                                                YYPS->m_lock_type,
                                                YYPS->m_mdl_type,
                                                Select->pop_index_hints(),
                                                $2)))
              MYSQL_YYABORT;
            TABLE_LIST *tl= $$;
            if ($3)
              tl->vers_conditions= Lex->vers_conditions;
          }
        ;


/*
  Represents a flattening of the following rules from the SQL:2003
  standard. This sub-rule corresponds to the sub-rule
  <table primary> ::= ... | <derived table> [ AS ] <correlation name>

  <derived table> ::= <table subquery>
  <table subquery> ::= <subquery>
  <subquery> ::= <left paren> <query expression> <right paren>
  <query expression> ::= [ <with clause> ] <query expression body>

  For the time being we use the non-standard rule
  select_derived_union which is a compromise between the standard
  and our parser. Possibly this rule could be replaced by our
  query_expression_body.
*/

table_primary_derived:
          query_primary_parens opt_for_system_time_clause table_alias_clause
          {
            if (!($$= Lex->parsed_derived_select($1, $2, $3)))
              YYABORT;
          }
        | '('
          query_expression
          ')' opt_for_system_time_clause table_alias_clause
          {
            if (!($$= Lex->parsed_derived_unit($2, $4, $5)))
              YYABORT;
          }
        ;

opt_outer:
          /* empty */ {}
        | OUTER {}
        ;

index_hint_clause:
          /* empty */
          {
            $$= thd->variables.old_mode ?  INDEX_HINT_MASK_JOIN : INDEX_HINT_MASK_ALL; 
          }
        | FOR_SYM JOIN_SYM      { $$= INDEX_HINT_MASK_JOIN;  }
        | FOR_SYM ORDER_SYM BY  { $$= INDEX_HINT_MASK_ORDER; }
        | FOR_SYM GROUP_SYM BY  { $$= INDEX_HINT_MASK_GROUP; }
        ;

index_hint_type:
          FORCE_SYM  { $$= INDEX_HINT_FORCE; }
        | IGNORE_SYM { $$= INDEX_HINT_IGNORE; } 
        ;

index_hint_definition:
          index_hint_type key_or_index index_hint_clause
          {
            Select->set_index_hint_type($1, $3);
          }
          '(' key_usage_list ')'
        | USE_SYM key_or_index index_hint_clause
          {
            Select->set_index_hint_type(INDEX_HINT_USE, $3);
          }
          '(' opt_key_usage_list ')'
       ;

index_hints_list:
          index_hint_definition
        | index_hints_list index_hint_definition
        ;

opt_index_hints_list:
          /* empty */
        | { Select->alloc_index_hints(thd); } index_hints_list
        ;

opt_key_definition:
          {  Select->clear_index_hints(); }
          opt_index_hints_list
        ;

opt_key_usage_list:
          /* empty */ { Select->add_index_hint(thd, NULL, 0); }
        | key_usage_list {}
        ;

key_usage_element:
          ident
          { Select->add_index_hint(thd, $1.str, $1.length); }
        | PRIMARY_SYM
          { Select->add_index_hint(thd, "PRIMARY", 7); }
        ;

key_usage_list:
          key_usage_element
        | key_usage_list ',' key_usage_element
        ;

using_list:
          ident
          {
            if (unlikely(!($$= new (thd->mem_root) List<String>)))
              MYSQL_YYABORT;
            String *s= new (thd->mem_root) String((const char *) $1.str,
                                                    $1.length,
                                                    system_charset_info);
            if (unlikely(unlikely(s == NULL)))
              MYSQL_YYABORT;
            $$->push_back(s, thd->mem_root);
          }
        | using_list ',' ident
          {
            String *s= new (thd->mem_root) String((const char *) $3.str,
                                                    $3.length,
                                                    system_charset_info);
            if (unlikely(unlikely(s == NULL)))
              MYSQL_YYABORT;
            if (unlikely($1->push_back(s, thd->mem_root)))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

interval:
          interval_time_stamp    {}
        | DAY_HOUR_SYM           { $$=INTERVAL_DAY_HOUR; }
        | DAY_MICROSECOND_SYM    { $$=INTERVAL_DAY_MICROSECOND; }
        | DAY_MINUTE_SYM         { $$=INTERVAL_DAY_MINUTE; }
        | DAY_SECOND_SYM         { $$=INTERVAL_DAY_SECOND; }
        | HOUR_MICROSECOND_SYM   { $$=INTERVAL_HOUR_MICROSECOND; }
        | HOUR_MINUTE_SYM        { $$=INTERVAL_HOUR_MINUTE; }
        | HOUR_SECOND_SYM        { $$=INTERVAL_HOUR_SECOND; }
        | MINUTE_MICROSECOND_SYM { $$=INTERVAL_MINUTE_MICROSECOND; }
        | MINUTE_SECOND_SYM      { $$=INTERVAL_MINUTE_SECOND; }
        | SECOND_MICROSECOND_SYM { $$=INTERVAL_SECOND_MICROSECOND; }
        | YEAR_MONTH_SYM         { $$=INTERVAL_YEAR_MONTH; }
        ;

interval_time_stamp:
          DAY_SYM         { $$=INTERVAL_DAY; }
        | WEEK_SYM        { $$=INTERVAL_WEEK; }
        | HOUR_SYM        { $$=INTERVAL_HOUR; }
        | MINUTE_SYM      { $$=INTERVAL_MINUTE; }
        | MONTH_SYM       { $$=INTERVAL_MONTH; }
        | QUARTER_SYM     { $$=INTERVAL_QUARTER; }
        | SECOND_SYM      { $$=INTERVAL_SECOND; }
        | MICROSECOND_SYM { $$=INTERVAL_MICROSECOND; }
        | YEAR_SYM        { $$=INTERVAL_YEAR; }
        ;

date_time_type:
          DATE_SYM  {$$=MYSQL_TIMESTAMP_DATE;}
        | TIME_SYM  {$$=MYSQL_TIMESTAMP_TIME;}
        | DATETIME  {$$=MYSQL_TIMESTAMP_DATETIME;}
        | TIMESTAMP {$$=MYSQL_TIMESTAMP_DATETIME;}
        ;

table_alias:
          /* empty */
        | AS
        | '='
        ;

opt_table_alias_clause:
          /* empty */ { $$=0; }

        | table_alias_clause { $$= $1; }
        ;

table_alias_clause:
          table_alias ident_table_alias
          {
            $$= (LEX_CSTRING*) thd->memdup(&$2,sizeof(LEX_STRING));
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_all:
          /* empty */
        | ALL
        ;

opt_where_clause:
          /* empty */  { Select->where= 0; }
        | WHERE
          {
            Select->parsing_place= IN_WHERE;
          }
          expr
          {
            SELECT_LEX *select= Select;
            select->where= normalize_cond(thd, $3);
            select->parsing_place= NO_MATTER;
            if ($3)
              $3->top_level_item();
          }
        ;

opt_having_clause:
          /* empty */
        | HAVING
          {
            Select->parsing_place= IN_HAVING;
          }
          expr
          {
            SELECT_LEX *sel= Select;
            sel->having= normalize_cond(thd, $3);
            sel->parsing_place= NO_MATTER;
            if ($3)
              $3->top_level_item();
          }
        ;

opt_escape:
          ESCAPE_SYM simple_expr 
          {
            Lex->escape_used= TRUE;
            $$= $2;
          }
        | /* empty */        %prec PREC_BELOW_ESCAPE
          {
            Lex->escape_used= FALSE;
            $$= ((thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES) ?
                 new (thd->mem_root) Item_string_ascii(thd, "", 0) :
                 new (thd->mem_root) Item_string_ascii(thd, "\\", 1));
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

/*
   group by statement in select
*/

opt_group_clause:
          /* empty */
        | GROUP_SYM BY group_list olap_opt
        ;

group_list:
          group_list ',' order_ident order_dir
          {
             if (unlikely(add_group_to_list(thd, $3,(bool) $4)))
               MYSQL_YYABORT;
           }
        | order_ident order_dir
          {
            if (unlikely(add_group_to_list(thd, $1,(bool) $2)))
              MYSQL_YYABORT;
           }
        ;

olap_opt:
          /* empty */ {}
        | WITH_CUBE_SYM
          {
            /*
              'WITH CUBE' is reserved in the MySQL syntax, but not implemented,
              and cause LALR(2) conflicts.
              This syntax is not standard.
              MySQL syntax: GROUP BY col1, col2, col3 WITH CUBE
              SQL-2003: GROUP BY ... CUBE(col1, col2, col3)
            */
            LEX *lex=Lex;
            if (unlikely(lex->current_select->get_linkage() == GLOBAL_OPTIONS_TYPE))
              my_yyabort_error((ER_WRONG_USAGE, MYF(0), "WITH CUBE",
                                "global union parameters"));
            lex->current_select->olap= CUBE_TYPE;

            my_yyabort_error((ER_NOT_SUPPORTED_YET, MYF(0), "CUBE"));
          }
        | WITH_ROLLUP_SYM
          {
            /*
              'WITH ROLLUP' is needed for backward compatibility,
              and cause LALR(2) conflicts.
              This syntax is not standard.
              MySQL syntax: GROUP BY col1, col2, col3 WITH ROLLUP
              SQL-2003: GROUP BY ... ROLLUP(col1, col2, col3)
            */
            LEX *lex= Lex;
            if (unlikely(lex->current_select->get_linkage() == GLOBAL_OPTIONS_TYPE))
              my_yyabort_error((ER_WRONG_USAGE, MYF(0), "WITH ROLLUP",
                                "global union parameters"));
            lex->current_select->olap= ROLLUP_TYPE;
          }
        ;

/*
  optional window clause in select
*/

opt_window_clause:
          /* empty */
          {}
        | WINDOW_SYM
          window_def_list
          {}
        ;

window_def_list:
          window_def_list ',' window_def
        | window_def
        ;

window_def:
          window_name AS window_spec
          { 
            LEX *lex= Lex;
            if (unlikely(Select->add_window_def(thd, $1, lex->win_ref,
                                                Select->group_list,
                                                Select->order_list,
                                                lex->win_frame)))
              MYSQL_YYABORT;
          }
        ;

window_spec:
          '(' 
          { Select->prepare_add_window_spec(thd); }
          opt_window_ref opt_window_partition_clause
          opt_window_order_clause opt_window_frame_clause
          ')'
        ;

opt_window_ref:
          /* empty */ {} 
        | ident
          {
            thd->lex->win_ref= (LEX_CSTRING *) thd->memdup(&$1, sizeof(LEX_CSTRING));
            if (unlikely(thd->lex->win_ref == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_window_partition_clause:
          /* empty */ { }
        | PARTITION_SYM BY group_list
        ;

opt_window_order_clause:
          /* empty */ { }
        | ORDER_SYM BY order_list { Select->order_list= *($3); } 
        ;

opt_window_frame_clause:
          /* empty */ {}
        | window_frame_units window_frame_extent opt_window_frame_exclusion
          {
            LEX *lex= Lex;
            lex->win_frame=
              new (thd->mem_root) Window_frame($1,
                                               lex->frame_top_bound,
                                               lex->frame_bottom_bound,
                                               $3);
            if (unlikely(lex->win_frame == NULL))
              MYSQL_YYABORT;
          }
        ;

window_frame_units:
          ROWS_SYM { $$= Window_frame::UNITS_ROWS; }
        | RANGE_SYM { $$= Window_frame::UNITS_RANGE; }
        ;
         
window_frame_extent:
          window_frame_start
          {
            LEX *lex= Lex;
            lex->frame_top_bound= $1;
            lex->frame_bottom_bound=
              new (thd->mem_root)
                Window_frame_bound(Window_frame_bound::CURRENT, NULL);
            if (unlikely(lex->frame_bottom_bound == NULL))
              MYSQL_YYABORT;
          }
        | BETWEEN_SYM window_frame_bound AND_SYM window_frame_bound
          {
            LEX *lex= Lex;
            lex->frame_top_bound= $2;
            lex->frame_bottom_bound= $4;
          }
        ;

window_frame_start:
          UNBOUNDED_SYM PRECEDING_SYM
          {
            $$= new (thd->mem_root) 
                  Window_frame_bound(Window_frame_bound::PRECEDING, NULL); 
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          } 
        | CURRENT_SYM ROW_SYM
          { 
            $$= new (thd->mem_root)
                  Window_frame_bound(Window_frame_bound::CURRENT, NULL); 
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | literal PRECEDING_SYM
          {
            $$= new (thd->mem_root)
                  Window_frame_bound(Window_frame_bound::PRECEDING, $1); 
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

window_frame_bound:
          window_frame_start { $$= $1; }
        | UNBOUNDED_SYM FOLLOWING_SYM        
          {
            $$= new (thd->mem_root)
                  Window_frame_bound(Window_frame_bound::FOLLOWING, NULL); 
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          } 
        | literal FOLLOWING_SYM
          {
            $$= new (thd->mem_root)
                  Window_frame_bound(Window_frame_bound::FOLLOWING, $1); 
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_window_frame_exclusion:
          /* empty */ { $$= Window_frame::EXCL_NONE; }
        | EXCLUDE_SYM CURRENT_SYM ROW_SYM
          { $$= Window_frame::EXCL_CURRENT_ROW; }
        | EXCLUDE_SYM GROUP_SYM
          { $$= Window_frame::EXCL_GROUP; }
        | EXCLUDE_SYM TIES_SYM
          { $$= Window_frame::EXCL_TIES; }
        | EXCLUDE_SYM NO_SYM OTHERS_MARIADB_SYM
          { $$= Window_frame::EXCL_NONE; }
        | EXCLUDE_SYM NO_SYM OTHERS_ORACLE_SYM
          { $$= Window_frame::EXCL_NONE; }
        ;      
       
/*
  Order by statement in ALTER TABLE
*/

alter_order_clause:
          ORDER_SYM BY alter_order_list
        ;

alter_order_list:
          alter_order_list ',' alter_order_item
        | alter_order_item
        ;

alter_order_item:
          simple_ident_nospvar order_dir
          {
            bool ascending= ($2 == 1) ? true : false;
            if (unlikely(add_order_to_list(thd, $1, ascending)))
              MYSQL_YYABORT;
          }
        ;

/*
   Order by statement in select
*/

opt_order_clause:
          /* empty */
          { $$= NULL; }
        | order_clause
          { $$= $1; }
        ;

order_clause:
          ORDER_SYM BY
          {
            thd->where= "ORDER clause";
          }
          order_list
          {
            $$= $4;
          }
         ;

order_list:
          order_list ',' order_ident order_dir
          {
            $$= $1;
            if (add_to_list(thd, *$$, $3,(bool) $4))
              MYSQL_YYABORT;
          }
        | order_ident order_dir
          {
            $$= new (thd->mem_root) SQL_I_List<ORDER>();
            if (add_to_list(thd, *$$, $1, (bool) $2))
              MYSQL_YYABORT;
          }
        ;

order_dir:
          /* empty */ { $$ =  1; }
        | ASC  { $$ =1; }
        | DESC { $$ =0; }
        ;

opt_limit_clause:
          /* empty */
          { $$.empty(); }
        | limit_clause
          { $$= $1; }
        ;

limit_clause:
          LIMIT limit_options
          {
            $$= $2;
            if (!$$.select_limit->basic_const_item() ||
                $$.select_limit->val_int() > 0)
              Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
          }
        | LIMIT limit_options
          ROWS_SYM EXAMINED_SYM limit_rows_option
          {
            $$= $2;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
          }
        | LIMIT ROWS_SYM EXAMINED_SYM limit_rows_option
          {
            $$.select_limit= 0;
            $$.offset_limit= 0;
            $$.explicit_limit= 1;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
          }
        ;

opt_global_limit_clause:
          opt_limit_clause
          {
            Select->explicit_limit= $1.explicit_limit;
            Select->select_limit= $1.select_limit;
            Select->offset_limit= $1.offset_limit;
          }
        ;

limit_options:
          limit_option
          {
            $$.select_limit= $1;
            $$.offset_limit= 0;
            $$.explicit_limit= 1;
          }
        | limit_option ',' limit_option
          {
            $$.select_limit= $3;
            $$.offset_limit= $1;
            $$.explicit_limit= 1;
          }
        | limit_option OFFSET_SYM limit_option
          {
            $$.select_limit= $1;
            $$.offset_limit= $3;
            $$.explicit_limit= 1;
          }
        ;

limit_option:
          ident_cli
          {
            if (unlikely(!($$= Lex->create_item_limit(thd, &$1))))
              MYSQL_YYABORT;
          }
        | ident_cli '.' ident_cli
          {
            if (unlikely(!($$= Lex->create_item_limit(thd, &$1, &$3))))
              MYSQL_YYABORT;
          }
        | param_marker
          {
            $1->limit_clause_param= TRUE;
          }
        | ULONGLONG_NUM
          {
            $$= new (thd->mem_root) Item_uint(thd, $1.str, $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | LONG_NUM
          {
            $$= new (thd->mem_root) Item_uint(thd, $1.str, $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | NUM
          {
            $$= new (thd->mem_root) Item_uint(thd, $1.str, $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

limit_rows_option:
          limit_option
          { 
            LEX *lex=Lex;
            lex->limit_rows_examined= $1;
          }
        ;

delete_limit_clause:
          /* empty */
          {
            LEX *lex=Lex;
            lex->current_select->select_limit= 0;
          }
        | LIMIT limit_option
          {
            SELECT_LEX *sel= Select;
            sel->select_limit= $2;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
            sel->explicit_limit= 1;
          }
       | LIMIT ROWS_SYM EXAMINED_SYM { thd->parse_error(); MYSQL_YYABORT; }
       | LIMIT limit_option ROWS_SYM EXAMINED_SYM { thd->parse_error(); MYSQL_YYABORT; }
        ;

opt_order_limit_lock:
          /* empty */ 
          { $$= NULL; }
        | order_or_limit
          {
            $$= $1;
            $$->lock.empty();
          }
        | order_or_limit select_lock_type
          {
            $$= $1;
            $$->lock= $2;
          }
        | select_lock_type
          {
            $$= new(thd->mem_root) Lex_order_limit_lock;
            if (!$$)
              YYABORT;
            $$->order_list= NULL;
            $$->limit.empty();
            $$->lock= $1;
          }
        ;
query_expression_tail:
          opt_order_limit_lock
        ;

opt_procedure_or_into:
          /* empty */ 
        {
          $$.empty();
        }
        | procedure_clause opt_select_lock_type
        {
          $$= $2;
        }
        | into opt_select_lock_type
        {
          push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                              ER_WARN_DEPRECATED_SYNTAX,
                              ER_THD(thd, ER_WARN_DEPRECATED_SYNTAX),
                              "<select expression> INTO <destination>;",
                              "'SELECT <select list> INTO <destination>"
                              " FROM...'");
          $$= $2;
        }
        ;


order_or_limit:
          order_clause opt_limit_clause
          {
            $$= new(thd->mem_root) Lex_order_limit_lock;
            if (!$$)
              YYABORT;
            $$->order_list= $1;
            $$->limit= $2;
          }
        | limit_clause
          {
            Lex_order_limit_lock *op= $$= new(thd->mem_root) Lex_order_limit_lock;
            if (!$$)
              YYABORT;
            op->order_list= NULL;
            op->limit= $1;
            $$->order_list= NULL;
            $$->limit= $1;
          }
        ;


opt_plus:
          /* empty */
        | '+'
        ;

int_num:
          opt_plus NUM           { int error; $$= (int) my_strtoll10($2.str, (char**) 0, &error); }
        | '-' NUM       { int error; $$= -(int) my_strtoll10($2.str, (char**) 0, &error); }
        ;

ulong_num:
          opt_plus NUM           { int error; $$= (ulong) my_strtoll10($2.str, (char**) 0, &error); }
        | HEX_NUM       { $$= (ulong) strtol($1.str, (char**) 0, 16); }
        | opt_plus LONG_NUM      { int error; $$= (ulong) my_strtoll10($2.str, (char**) 0, &error); }
        | opt_plus ULONGLONG_NUM { int error; $$= (ulong) my_strtoll10($2.str, (char**) 0, &error); }
        | opt_plus DECIMAL_NUM   { int error; $$= (ulong) my_strtoll10($2.str, (char**) 0, &error); }
        | opt_plus FLOAT_NUM     { int error; $$= (ulong) my_strtoll10($2.str, (char**) 0, &error); }
        ;

real_ulong_num:
          NUM           { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | HEX_NUM       { $$= (ulong) strtol($1.str, (char**) 0, 16); }
        | LONG_NUM      { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | ULONGLONG_NUM { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | dec_num_error { MYSQL_YYABORT; }
        ;

longlong_num:
          opt_plus NUM           { int error; $$= (longlong) my_strtoll10($2.str, (char**) 0, &error); }
        | LONG_NUM      { int error; $$= (longlong) my_strtoll10($1.str, (char**) 0, &error); }
        | '-' NUM         { int error; $$= -(longlong) my_strtoll10($2.str, (char**) 0, &error); }
        | '-' LONG_NUM  { int error; $$= -(longlong) my_strtoll10($2.str, (char**) 0, &error); }
        ;

ulonglong_num:
          opt_plus NUM           { int error; $$= (ulonglong) my_strtoll10($2.str, (char**) 0, &error); }
        | opt_plus ULONGLONG_NUM { int error; $$= (ulonglong) my_strtoll10($2.str, (char**) 0, &error); }
        | opt_plus LONG_NUM      { int error; $$= (ulonglong) my_strtoll10($2.str, (char**) 0, &error); }
        | opt_plus DECIMAL_NUM   { int error; $$= (ulonglong) my_strtoll10($2.str, (char**) 0, &error); }
        | opt_plus FLOAT_NUM     { int error; $$= (ulonglong) my_strtoll10($2.str, (char**) 0, &error); }
        ;

real_ulonglong_num:
          NUM           { int error; $$= (ulonglong) my_strtoll10($1.str, (char**) 0, &error); }
        | ULONGLONG_NUM { int error; $$= (ulonglong) my_strtoll10($1.str, (char**) 0, &error); }
        | HEX_NUM       { $$= strtoull($1.str, (char**) 0, 16); }
        | LONG_NUM      { int error; $$= (ulonglong) my_strtoll10($1.str, (char**) 0, &error); }
        | dec_num_error { MYSQL_YYABORT; }
        ;

dec_num_error:
          dec_num
          { thd->parse_error(ER_ONLY_INTEGERS_ALLOWED); }
        ;

dec_num:
          DECIMAL_NUM
        | FLOAT_NUM
        ;

choice:
	ulong_num { $$= $1 != 0 ? HA_CHOICE_YES : HA_CHOICE_NO; }
	| DEFAULT { $$= HA_CHOICE_UNDEF; }
	;

bool:
        ulong_num   { $$= $1 != 0; }
        | TRUE_SYM  { $$= 1; }
        | FALSE_SYM { $$= 0; }
        ;

procedure_clause:
          PROCEDURE_SYM ident /* Procedure name */
          {
            LEX *lex=Lex;

            lex->proc_list.elements=0;
            lex->proc_list.first=0;
            lex->proc_list.next= &lex->proc_list.first;
            Item_field *item= new (thd->mem_root)
                                Item_field(thd, &lex->current_select->context,
                                           NULL, NULL, &$2);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            if (unlikely(add_proc_to_list(thd, item)))
              MYSQL_YYABORT;
            Lex->uncacheable(UNCACHEABLE_SIDEEFFECT);

            /*
              PROCEDURE CLAUSE cannot handle subquery as one of its parameter,
              so set expr_allows_subselect as false to disallow any subqueries
              further. Reset expr_allows_subselect back to true once the
              parameters are reduced.
            */
            Lex->expr_allows_subselect= false;
            Select->options|= OPTION_PROCEDURE_CLAUSE;
          }
          '(' procedure_list ')'
          {
            /* Subqueries are allowed from now.*/
            Lex->expr_allows_subselect= true;
          }
        ;

procedure_list:
          /* empty */ {}
        | procedure_list2 {}
        ;

procedure_list2:
          procedure_list2 ',' procedure_item
        | procedure_item
        ;

procedure_item:
          remember_name expr remember_end
          {
            if (unlikely(add_proc_to_list(thd, $2)))
              MYSQL_YYABORT;
            if (!$2->name.str || $2->name.str == item_empty_name)
              $2->set_name(thd, $1, (uint) ($3 - $1), thd->charset());
          }
        ;

select_var_list_init:
          {
            LEX *lex=Lex;
            if (!lex->describe &&
                unlikely((!(lex->result= new (thd->mem_root)
                            select_dumpvar(thd)))))
              MYSQL_YYABORT;
          }
          select_var_list
          {}
        ;

select_var_list:
          select_var_list ',' select_var_ident
        | select_var_ident {}
        ;

select_var_ident: select_outvar
          {
            if (Lex->result)
            {
              if (unlikely($1 == NULL))
                MYSQL_YYABORT;
              ((select_dumpvar *)Lex->result)->var_list.push_back($1, thd->mem_root);
            }
            else
            {
              /*
                The parser won't create select_result instance only
                if it's an EXPLAIN.
              */
              DBUG_ASSERT(Lex->describe);
            }
          }
        ;

select_outvar:
          '@' ident_or_text
          {
            $$ = Lex->result ? new (thd->mem_root) my_var_user(&$2) : NULL;
          }
        | ident_or_text
          {
            if (unlikely(!($$= Lex->create_outvar(thd, &$1)) && Lex->result))
              MYSQL_YYABORT;
          }
        | ident '.' ident
          {
            if (unlikely(!($$= Lex->create_outvar(thd, &$1, &$3)) && Lex->result))
              MYSQL_YYABORT;
          }
        ;

into:
          INTO into_destination
          {}
        ;

into_destination:
          OUTFILE TEXT_STRING_filesystem
          {
            LEX *lex= Lex;
            lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
            if (unlikely(!(lex->exchange=
                         new (thd->mem_root) sql_exchange($2.str, 0))) ||
                unlikely(!(lex->result=
                         new (thd->mem_root)
                         select_export(thd, lex->exchange))))
              MYSQL_YYABORT;
          }
          opt_load_data_charset
          { Lex->exchange->cs= $4; }
          opt_field_term opt_line_term
        | DUMPFILE TEXT_STRING_filesystem
          {
            LEX *lex=Lex;
            if (!lex->describe)
            {
              lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
              if (unlikely(!(lex->exchange=
                             new (thd->mem_root) sql_exchange($2.str,1))))
                MYSQL_YYABORT;
              if (unlikely(!(lex->result=
                           new (thd->mem_root)
                           select_dump(thd, lex->exchange))))
                MYSQL_YYABORT;
            }
          }
        | select_var_list_init
          {
            Lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
          }
        ;

/*
  DO statement
*/

do:
          DO_SYM
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_DO;
            mysql_init_select(lex);
          }
          expr_list
          {
            Lex->insert_list= $3;
          }
        ;

/*
  Drop : delete tables or index or user
*/

drop:
          DROP opt_temporary table_or_tables opt_if_exists
          {
            LEX *lex=Lex;
            lex->set_command(SQLCOM_DROP_TABLE, $2, $4);
            YYPS->m_lock_type= TL_UNLOCK;
            YYPS->m_mdl_type= MDL_EXCLUSIVE;
          }
          table_list opt_lock_wait_timeout opt_restrict
          {}
        | DROP INDEX_SYM opt_if_exists_table_element ident ON table_ident opt_lock_wait_timeout
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::KEY, $4.str, $3));
            if (unlikely(ad == NULL))
              MYSQL_YYABORT;
            lex->sql_command= SQLCOM_DROP_INDEX;
            lex->alter_info.reset();
            lex->alter_info.flags= ALTER_DROP_INDEX;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            if (unlikely(!lex->current_select->
                         add_table_to_list(thd, $6, NULL, TL_OPTION_UPDATING,
                                           TL_READ_NO_INSERT,
                                           MDL_SHARED_UPGRADABLE)))
              MYSQL_YYABORT;
          }
        | DROP DATABASE opt_if_exists ident
          {
            LEX *lex=Lex;
            lex->set_command(SQLCOM_DROP_DB, $3);
            lex->name= $4;
          }
        | DROP FUNCTION_SYM opt_if_exists ident '.' ident
          {
            LEX *lex= thd->lex;
            sp_name *spname;
            if (unlikely($4.str && check_db_name((LEX_STRING*) &$4)))
              my_yyabort_error((ER_WRONG_DB_NAME, MYF(0), $4.str));
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "FUNCTION"));
            lex->set_command(SQLCOM_DROP_FUNCTION, $3);
            spname= new (thd->mem_root) sp_name(&$4, &$6, true);
            if (unlikely(spname == NULL))
              MYSQL_YYABORT;
            lex->spname= spname;
          }
        | DROP FUNCTION_SYM opt_if_exists ident
          {
            LEX *lex= thd->lex;
            LEX_CSTRING db= {0, 0};
            sp_name *spname;
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "FUNCTION"));
            if (thd->db.str && unlikely(lex->copy_db_to(&db)))
              MYSQL_YYABORT;
            lex->set_command(SQLCOM_DROP_FUNCTION, $3);
            spname= new (thd->mem_root) sp_name(&db, &$4, false);
            if (unlikely(spname == NULL))
              MYSQL_YYABORT;
            lex->spname= spname;
          }
        | DROP PROCEDURE_SYM opt_if_exists sp_name
          {
            LEX *lex=Lex;
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "PROCEDURE"));
            lex->set_command(SQLCOM_DROP_PROCEDURE, $3);
            lex->spname= $4;
          }
        | DROP USER_SYM opt_if_exists clear_privileges user_list
          {
            Lex->set_command(SQLCOM_DROP_USER, $3);
          }
        | DROP ROLE_SYM opt_if_exists clear_privileges role_list
          {
            Lex->set_command(SQLCOM_DROP_ROLE, $3);
          }
        | DROP VIEW_SYM opt_if_exists
          {
            LEX *lex= Lex;
            lex->set_command(SQLCOM_DROP_VIEW, $3);
            YYPS->m_lock_type= TL_UNLOCK;
            YYPS->m_mdl_type= MDL_EXCLUSIVE;
          }
          table_list opt_restrict
          {}
        | DROP EVENT_SYM opt_if_exists sp_name
          {
            Lex->spname= $4;
            Lex->set_command(SQLCOM_DROP_EVENT, $3);
          }
        | DROP TRIGGER_SYM opt_if_exists sp_name
          {
            LEX *lex= Lex;
            lex->set_command(SQLCOM_DROP_TRIGGER, $3);
            lex->spname= $4;
          }
        | DROP TABLESPACE tablespace_name opt_ts_engine opt_ts_wait
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_cmd_type= DROP_TABLESPACE;
          }
        | DROP LOGFILE_SYM GROUP_SYM logfile_group_name opt_ts_engine opt_ts_wait
          {
            LEX *lex= Lex;
            lex->alter_tablespace_info->ts_cmd_type= DROP_LOGFILE_GROUP;
          }
        | DROP SERVER_SYM opt_if_exists ident_or_text
          {
            Lex->set_command(SQLCOM_DROP_SERVER, $3);
            Lex->server_options.reset($4);
          }
        | DROP opt_temporary SEQUENCE_SYM opt_if_exists

          {
            LEX *lex= Lex;
            lex->set_command(SQLCOM_DROP_SEQUENCE, $2, $4);
            lex->table_type= TABLE_TYPE_SEQUENCE;
            YYPS->m_lock_type= TL_UNLOCK;
            YYPS->m_mdl_type= MDL_EXCLUSIVE;
          }
          table_list
          {}
        ;

table_list:
          table_name
        | table_list ',' table_name
        ;

table_name:
          table_ident
          {
            if (!thd->lex->current_select_or_default()->
                                           add_table_to_list(thd, $1, NULL,
                                           TL_OPTION_UPDATING,
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type))
              MYSQL_YYABORT;
          }
        ;

table_name_with_opt_use_partition:
          table_ident opt_use_partition
          {
            if (unlikely(!Select->add_table_to_list(thd, $1, NULL,
                                                    TL_OPTION_UPDATING,
                                                    YYPS->m_lock_type,
                                                    YYPS->m_mdl_type,
                                                    NULL,
                                                    $2)))
              MYSQL_YYABORT;
          }
        ;

table_alias_ref_list:
          table_alias_ref
        | table_alias_ref_list ',' table_alias_ref
        ;

table_alias_ref:
          table_ident_opt_wild
          {
            if (unlikely(!Select->
                         add_table_to_list(thd, $1, NULL,
                                           (TL_OPTION_UPDATING |
                                            TL_OPTION_ALIAS),
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type)))
              MYSQL_YYABORT;
          }
        ;

opt_if_exists_table_element:
          /* empty */
        {
          Lex->check_exists= FALSE;
          $$= 0;
        }
        | IF_SYM EXISTS
        {
          Lex->check_exists= TRUE;
          $$= 1;
        }
        ;

opt_if_exists:
          /* empty */
        {
          $$.set(DDL_options_st::OPT_NONE);
        }
        | IF_SYM EXISTS
        {
          $$.set(DDL_options_st::OPT_IF_EXISTS);
        }
        ;

opt_temporary:
          /* empty */ { $$= 0; }
        | TEMPORARY { $$= HA_LEX_CREATE_TMP_TABLE; }
        ;
/*
** Insert : add new data to table
*/

insert:
          INSERT
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_INSERT;
            lex->duplicates= DUP_ERROR;
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            mysql_init_select(lex);
            lex->current_select->parsing_place= BEFORE_OPT_LIST;
          }
          insert_lock_option
          opt_ignore insert2
          {
            Select->set_lock_for_tables($3);
            Lex->current_select= Lex->first_select_lex();
          }
          insert_field_spec opt_insert_update
          {
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        ;

replace:
          REPLACE
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_REPLACE;
            lex->duplicates= DUP_REPLACE;
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            mysql_init_select(lex);
            lex->current_select->parsing_place= BEFORE_OPT_LIST;
          }
          replace_lock_option insert2
          {
            Select->set_lock_for_tables($3);
            Lex->current_select= Lex->first_select_lex();
          }
          insert_field_spec
          {
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        ;

insert_lock_option:
          /* empty */
          {
            /*
              If it is SP we do not allow insert optimisation when result of
              insert visible only after the table unlocking but everyone can
              read table.
            */
            $$= (Lex->sphead ? TL_WRITE_DEFAULT : TL_WRITE_CONCURRENT_INSERT);
          }
        | LOW_PRIORITY  { $$= TL_WRITE_LOW_PRIORITY; }
        | DELAYED_SYM
        {
         // QQ: why was +1?
          Lex->keyword_delayed_begin_offset= (uint)($1.pos() - thd->query());
          Lex->keyword_delayed_end_offset= (uint)($1.end() - thd->query());
          $$= TL_WRITE_DELAYED;
        }
        | HIGH_PRIORITY { $$= TL_WRITE; }
        ;

replace_lock_option:
          opt_low_priority { $$= $1; }
        | DELAYED_SYM
        {
          Lex->keyword_delayed_begin_offset= (uint)($1.pos() - thd->query());
          Lex->keyword_delayed_end_offset= (uint)($1.end() - thd->query());
          $$= TL_WRITE_DELAYED;
        }
        ;

insert2:
          INTO insert_table {}
        | insert_table {}
        ;

insert_table:
          table_name_with_opt_use_partition
          {
            LEX *lex=Lex;
            //lex->field_list.empty();
            lex->many_values.empty();
            lex->insert_list=0;
          };

insert_field_spec:
          insert_values {}
        | insert_field_list insert_values {}
        | SET
          {
            LEX *lex=Lex;
            if (unlikely(!(lex->insert_list= new (thd->mem_root) List_item)) ||
                unlikely(lex->many_values.push_back(lex->insert_list,
                         thd->mem_root)))
              MYSQL_YYABORT;
            lex->current_select->parsing_place= NO_MATTER;
          }
          ident_eq_list
        ;

insert_field_list:
          LEFT_PAREN_ALT opt_fields ')'
          {
            Lex->current_select->parsing_place= AFTER_LIST;
          }
        ;

opt_fields:
          /* empty */
        | fields
        ;

fields:
          fields ',' insert_ident
          { Lex->field_list.push_back($3, thd->mem_root); }
        | insert_ident { Lex->field_list.push_back($1, thd->mem_root); }
        ;



insert_values:
         create_select_query_expression {}
        ;

values_list:
          values_list ','  no_braces
        | no_braces_with_names
        ;

ident_eq_list:
          ident_eq_list ',' ident_eq_value
        | ident_eq_value
        ;

ident_eq_value:
          simple_ident_nospvar equal expr_or_default
          {
            LEX *lex=Lex;
            if (unlikely(lex->field_list.push_back($1, thd->mem_root)) ||
                unlikely(lex->insert_list->push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

equal:
          '=' {}
        | SET_VAR {}
        ;

opt_equal:
          /* empty */ {}
        | equal {}
        ;

opt_with:
          opt_equal {}
        | WITH {}
        ;

opt_by:
          opt_equal {}
        | BY {}
        ;

no_braces:
          '('
          {
            if (unlikely(!(Lex->insert_list= new (thd->mem_root) List_item)))
              MYSQL_YYABORT;
          }
          opt_values ')'
          {
            LEX *lex=Lex;
            if (unlikely(lex->many_values.push_back(lex->insert_list,
                                                    thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

no_braces_with_names:
          '('
          {
            if (unlikely(!(Lex->insert_list= new (thd->mem_root) List_item)))
              MYSQL_YYABORT;
          }
          opt_values_with_names ')'
          {
            LEX *lex=Lex;
            if (unlikely(lex->many_values.push_back(lex->insert_list,
                                                    thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

opt_values:
          /* empty */ {}
        | values
        ;

opt_values_with_names:
          /* empty */ {}
        | values_with_names
        ;

values:
          values ','  expr_or_default
          {
            if (unlikely(Lex->insert_list->push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | expr_or_default
          {
            if (unlikely(Lex->insert_list->push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

values_with_names:
          values_with_names ','  remember_name expr_or_default remember_end
          {
            if (unlikely(Lex->insert_list->push_back($4, thd->mem_root)))
               MYSQL_YYABORT;
            // give some name in case of using in table value constuctor (TVC)
            if (!$4->name.str || $4->name.str == item_empty_name)
              $4->set_name(thd, $3, (uint) ($5 - $3), thd->charset());
           }
        | remember_name expr_or_default remember_end
          {
            if (unlikely(Lex->insert_list->push_back($2, thd->mem_root)))
               MYSQL_YYABORT;
            // give some name in case of using in table value constuctor (TVC)
            if (!$2->name.str || $2->name.str == item_empty_name)
              $2->set_name(thd, $1, (uint) ($3 - $1), thd->charset());
          }
        ;

expr_or_default:
          expr { $$= $1;}
        | DEFAULT
          {
            $$= new (thd->mem_root) Item_default_value(thd, Lex->current_context());
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | IGNORE_SYM
          {
            $$= new (thd->mem_root) Item_ignore_value(thd, Lex->current_context());
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_insert_update:
          /* empty */
        | ON DUPLICATE_SYM { Lex->duplicates= DUP_UPDATE; }
          KEY_SYM UPDATE_SYM 
          {
	    Select->parsing_place= IN_UPDATE_ON_DUP_KEY;
          }
          insert_update_list
          {
	    Select->parsing_place= NO_MATTER;
          }
        ;

/* Update rows in a table */

update:
          UPDATE_SYM
          {
            LEX *lex= Lex;
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            mysql_init_select(lex);
            lex->sql_command= SQLCOM_UPDATE;
            lex->duplicates= DUP_ERROR; 
          }
          opt_low_priority opt_ignore join_table_list
          SET update_list
          {
            LEX *lex= Lex;
            if (lex->first_select_lex()->table_list.elements > 1)
              lex->sql_command= SQLCOM_UPDATE_MULTI;
            else if (lex->first_select_lex()->get_table_list()->derived)
            {
              /* it is single table update and it is update of derived table */
              my_error(ER_NON_UPDATABLE_TABLE, MYF(0),
                       lex->first_select_lex()->get_table_list()->alias.str,
                       "UPDATE");
              MYSQL_YYABORT;
            }
            /*
              In case of multi-update setting write lock for all tables may
              be too pessimistic. We will decrease lock level if possible in
              mysql_multi_update().
            */
            Select->set_lock_for_tables($3);
          }
          opt_where_clause opt_order_clause delete_limit_clause
          {
            if ($10)
              Select->order_list= *($10);
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        ;

update_list:
          update_list ',' update_elem
        | update_elem
        ;

update_elem:
          simple_ident_nospvar equal expr_or_default
          {
            if (unlikely(add_item_to_list(thd, $1)) ||
                unlikely(add_value_to_list(thd, $3)))
              MYSQL_YYABORT;
          }
        ;

insert_update_list:
          insert_update_list ',' insert_update_elem
        | insert_update_elem
        ;

insert_update_elem:
          simple_ident_nospvar equal expr_or_default
          {
          LEX *lex= Lex;
          if (unlikely(lex->update_list.push_back($1, thd->mem_root)) ||
              unlikely(lex->value_list.push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

opt_low_priority:
          /* empty */ { $$= TL_WRITE_DEFAULT; }
        | LOW_PRIORITY { $$= TL_WRITE_LOW_PRIORITY; }
        ;

/* Delete rows from a table */

delete:
          DELETE_SYM
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_DELETE;
            mysql_init_select(lex);
            YYPS->m_lock_type= TL_WRITE_DEFAULT;
            YYPS->m_mdl_type= MDL_SHARED_WRITE;
            if (Lex->main_select_push())
              MYSQL_YYABORT;

            lex->ignore= 0;
            lex->first_select_lex()->order_list.empty();
          }
          delete_part2
          ;

opt_delete_system_time:
            /* empty */
          {
            Lex->vers_conditions.init(SYSTEM_TIME_ALL);
          }
          | BEFORE_SYM SYSTEM_TIME_SYM history_point
          {
            Lex->vers_conditions.init(SYSTEM_TIME_BEFORE, $3);
          }
          ;

delete_part2:
          opt_delete_options single_multi {}
        | HISTORY_SYM delete_single_table opt_delete_system_time
          {
            Lex->last_table()->vers_conditions= Lex->vers_conditions;
            Lex->pop_select(); //main select
          }
        ;

delete_single_table:
          FROM table_ident opt_use_partition
          {
            if (unlikely(!Select->
                         add_table_to_list(thd, $2, NULL, TL_OPTION_UPDATING,
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type,
                                           NULL,
                                           $3)))
              MYSQL_YYABORT;
            YYPS->m_lock_type= TL_READ_DEFAULT;
            YYPS->m_mdl_type= MDL_SHARED_READ;
          }
        ;

single_multi:
          delete_single_table
          opt_where_clause
          opt_order_clause
          delete_limit_clause
          opt_select_expressions 
          {
            if ($3)
              Select->order_list= *($3);
            Lex->pop_select(); //main select
          }
        | table_wild_list
          {
            mysql_init_multi_delete(Lex);
            YYPS->m_lock_type= TL_READ_DEFAULT;
            YYPS->m_mdl_type= MDL_SHARED_READ;
          }
          FROM join_table_list opt_where_clause
          {
            if (unlikely(multi_delete_set_locks_and_link_aux_tables(Lex)))
              MYSQL_YYABORT;
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        | FROM table_alias_ref_list
          {
            mysql_init_multi_delete(Lex);
            YYPS->m_lock_type= TL_READ_DEFAULT;
            YYPS->m_mdl_type= MDL_SHARED_READ;
          }
          USING join_table_list opt_where_clause
          {
            if (unlikely(multi_delete_set_locks_and_link_aux_tables(Lex)))
              MYSQL_YYABORT;
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        ;

opt_select_expressions:
          /* empty */ 
        | RETURNING_SYM select_item_list 
        ;

table_wild_list:
          table_wild_one
        | table_wild_list ',' table_wild_one
        ;

table_wild_one:
          ident opt_wild
          {
            Table_ident *ti= new (thd->mem_root) Table_ident(&$1);
            if (unlikely(ti == NULL))
              MYSQL_YYABORT;
            if (unlikely(!Select->
                         add_table_to_list(thd,
                                           ti,
                                           NULL,
                                           (TL_OPTION_UPDATING |
                                            TL_OPTION_ALIAS),
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type)))
              MYSQL_YYABORT;
          }
        | ident '.' ident opt_wild
          {
            Table_ident *ti= new (thd->mem_root) Table_ident(thd, &$1, &$3, 0);
            if (unlikely(ti == NULL))
              MYSQL_YYABORT;
            if (unlikely(!Select->
                         add_table_to_list(thd,
                                           ti,
                                           NULL,
                                           (TL_OPTION_UPDATING |
                                            TL_OPTION_ALIAS),
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type)))
              MYSQL_YYABORT;
          }
        ;

opt_wild:
          /* empty */ {}
        | '.' '*' {}
        ;

opt_delete_options:
          /* empty */ {}
        | opt_delete_option opt_delete_options {}
        ;

opt_delete_option:
          QUICK        { Select->options|= OPTION_QUICK; }
        | LOW_PRIORITY { YYPS->m_lock_type= TL_WRITE_LOW_PRIORITY; }
        | IGNORE_SYM   { Lex->ignore= 1; }
        ;

truncate:
          TRUNCATE_SYM
          {
            LEX* lex= Lex;
            lex->sql_command= SQLCOM_TRUNCATE;
            lex->alter_info.reset();
            lex->first_select_lex()->options= 0;
            lex->sql_cache= LEX::SQL_CACHE_UNSPECIFIED;
            lex->first_select_lex()->order_list.empty();
            YYPS->m_lock_type= TL_WRITE;
            YYPS->m_mdl_type= MDL_EXCLUSIVE;
          }
          opt_table_sym table_name opt_lock_wait_timeout
          {
            LEX* lex= thd->lex;
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_truncate_table();
            if (unlikely(lex->m_sql_cmd == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_table_sym:
          /* empty */
        | TABLE_SYM
        ;

opt_profile_defs:
  /* empty */
  | profile_defs;

profile_defs:
  profile_def
  | profile_defs ',' profile_def;

profile_def:
  CPU_SYM
    {
      Lex->profile_options|= PROFILE_CPU;
    }
  | MEMORY_SYM
    {
      Lex->profile_options|= PROFILE_MEMORY;
    }
  | BLOCK_SYM IO_SYM
    {
      Lex->profile_options|= PROFILE_BLOCK_IO;
    }
  | CONTEXT_SYM SWITCHES_SYM
    {
      Lex->profile_options|= PROFILE_CONTEXT;
    }
  | PAGE_SYM FAULTS_SYM
    {
      Lex->profile_options|= PROFILE_PAGE_FAULTS;
    }
  | IPC_SYM
    {
      Lex->profile_options|= PROFILE_IPC;
    }
  | SWAPS_SYM
    {
      Lex->profile_options|= PROFILE_SWAPS;
    }
  | SOURCE_SYM
    {
      Lex->profile_options|= PROFILE_SOURCE;
    }
  | ALL
    {
      Lex->profile_options|= PROFILE_ALL;
    }
  ;

opt_profile_args:
  /* empty */
    {
      Lex->profile_query_id= 0;
    }
  | FOR_SYM QUERY_SYM NUM
    {
      Lex->profile_query_id= atoi($3.str);
    }
  ;

/* Show things */

show:
          SHOW
          {
            LEX *lex=Lex;
            lex->wild=0;
            lex->ident= null_clex_str;
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            mysql_init_select(lex);
            lex->current_select->parsing_place= SELECT_LIST;
            lex->create_info.init();
          }
          show_param
          {
            Select->parsing_place= NO_MATTER;
            Lex->pop_select(); //main select
          }
        ;

show_param:
           DATABASES wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_DATABASES;
             if (unlikely(prepare_schema_table(thd, lex, 0, SCH_SCHEMATA)))
               MYSQL_YYABORT;
           }
         | opt_full TABLES opt_db wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_TABLES;
             lex->first_select_lex()->db= $3;
             if (prepare_schema_table(thd, lex, 0, SCH_TABLE_NAMES))
               MYSQL_YYABORT;
           }
         | opt_full TRIGGERS_SYM opt_db wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_TRIGGERS;
             lex->first_select_lex()->db= $3;
             if (prepare_schema_table(thd, lex, 0, SCH_TRIGGERS))
               MYSQL_YYABORT;
           }
         | EVENTS_SYM opt_db wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_EVENTS;
             lex->first_select_lex()->db= $2;
             if (prepare_schema_table(thd, lex, 0, SCH_EVENTS))
               MYSQL_YYABORT;
           }
         | TABLE_SYM STATUS_SYM opt_db wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_TABLE_STATUS;
             lex->first_select_lex()->db= $3;
             if (prepare_schema_table(thd, lex, 0, SCH_TABLES))
               MYSQL_YYABORT;
           }
        | OPEN_SYM TABLES opt_db wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_OPEN_TABLES;
            lex->first_select_lex()->db= $3;
            if (prepare_schema_table(thd, lex, 0, SCH_OPEN_TABLES))
              MYSQL_YYABORT;
          }
        | PLUGINS_SYM
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_PLUGINS;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_PLUGINS)))
              MYSQL_YYABORT;
          }
        | PLUGINS_SYM SONAME_SYM TEXT_STRING_sys
          {
            Lex->ident= $3;
            Lex->sql_command= SQLCOM_SHOW_PLUGINS;
            if (unlikely(prepare_schema_table(thd, Lex, 0, SCH_ALL_PLUGINS)))
              MYSQL_YYABORT;
          }
        | PLUGINS_SYM SONAME_SYM wild_and_where
          {
            Lex->sql_command= SQLCOM_SHOW_PLUGINS;
            if (unlikely(prepare_schema_table(thd, Lex, 0, SCH_ALL_PLUGINS)))
              MYSQL_YYABORT;
          }
        | ENGINE_SYM known_storage_engines show_engine_param
          { Lex->create_info.db_type= $2; }
        | ENGINE_SYM ALL show_engine_param
          { Lex->create_info.db_type= NULL; }
        | opt_full COLUMNS from_or_in table_ident opt_db wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_FIELDS;
            if ($5.str)
              $4->change_db(&$5);
            if (unlikely(prepare_schema_table(thd, lex, $4, SCH_COLUMNS)))
              MYSQL_YYABORT;
          }
        | master_or_binary LOGS_SYM
          {
            Lex->sql_command = SQLCOM_SHOW_BINLOGS;
          }
        | SLAVE HOSTS_SYM
          {
            Lex->sql_command = SQLCOM_SHOW_SLAVE_HOSTS;
          }
        | BINLOG_SYM EVENTS_SYM binlog_in binlog_from
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_BINLOG_EVENTS;
          }
          opt_global_limit_clause
        | RELAYLOG_SYM optional_connection_name EVENTS_SYM binlog_in binlog_from
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_RELAYLOG_EVENTS;
          }
          opt_global_limit_clause
        | keys_or_index from_or_in table_ident opt_db opt_where_clause
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_KEYS;
            if ($4.str)
              $3->change_db(&$4);
            if (unlikely(prepare_schema_table(thd, lex, $3, SCH_STATISTICS)))
              MYSQL_YYABORT;
          }
        | opt_storage ENGINES_SYM
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_SHOW_STORAGE_ENGINES;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_ENGINES)))
              MYSQL_YYABORT;
          }
        | AUTHORS_SYM
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_SHOW_AUTHORS;
          }
        | CONTRIBUTORS_SYM
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_SHOW_CONTRIBUTORS;
          }
        | PRIVILEGES
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_SHOW_PRIVILEGES;
          }
        | COUNT_SYM '(' '*' ')' WARNINGS
          {
            LEX_CSTRING var= {STRING_WITH_LEN("warning_count")};
            (void) create_select_for_variable(thd, &var);
          }
        | COUNT_SYM '(' '*' ')' ERRORS
          {
            LEX_CSTRING var= {STRING_WITH_LEN("error_count")};
            (void) create_select_for_variable(thd, &var);
          }
        | WARNINGS opt_global_limit_clause
          { Lex->sql_command = SQLCOM_SHOW_WARNS;}
        | ERRORS opt_global_limit_clause
          { Lex->sql_command = SQLCOM_SHOW_ERRORS;}
        | PROFILES_SYM
          { Lex->sql_command = SQLCOM_SHOW_PROFILES; }
        | PROFILE_SYM opt_profile_defs opt_profile_args opt_global_limit_clause
          { 
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_PROFILE;
            if (unlikely(prepare_schema_table(thd, lex, NULL, SCH_PROFILES)))
              MYSQL_YYABORT;
          }
        | opt_var_type STATUS_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_STATUS;
            lex->option_type= $1;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_SESSION_STATUS)))
              MYSQL_YYABORT;
          }
        | opt_full PROCESSLIST_SYM
          { Lex->sql_command= SQLCOM_SHOW_PROCESSLIST;}
        | opt_var_type  VARIABLES wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_VARIABLES;
            lex->option_type= $1;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_SESSION_VARIABLES)))
              MYSQL_YYABORT;
          }
        | charset wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_CHARSETS;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_CHARSETS)))
              MYSQL_YYABORT;
          }
        | COLLATION_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_COLLATIONS;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_COLLATIONS)))
              MYSQL_YYABORT;
          }
        | GRANTS
          {
            Lex->sql_command= SQLCOM_SHOW_GRANTS;
            if (unlikely(!(Lex->grant_user=
                          (LEX_USER*)thd->alloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            Lex->grant_user->user= current_user_and_current_role;
          }
        | GRANTS FOR_SYM user_or_role clear_privileges
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_SHOW_GRANTS;
            lex->grant_user=$3;
          }
        | CREATE DATABASE opt_if_not_exists ident
          {
            Lex->set_command(SQLCOM_SHOW_CREATE_DB, $3);
            Lex->name= $4;
          }
        | CREATE TABLE_SYM table_ident
          {
            LEX *lex= Lex;
            lex->sql_command = SQLCOM_SHOW_CREATE;
            if (!lex->first_select_lex()->add_table_to_list(thd, $3, NULL,0))
              MYSQL_YYABORT;
            lex->create_info.storage_media= HA_SM_DEFAULT;
          }
        | CREATE VIEW_SYM table_ident
          {
            LEX *lex= Lex;
            lex->sql_command = SQLCOM_SHOW_CREATE;
            if (!lex->first_select_lex()->add_table_to_list(thd, $3, NULL, 0))
              MYSQL_YYABORT;
            lex->table_type= TABLE_TYPE_VIEW;
          }
        | CREATE SEQUENCE_SYM table_ident
          {
            LEX *lex= Lex;
            lex->sql_command = SQLCOM_SHOW_CREATE;
            if (!lex->first_select_lex()->add_table_to_list(thd, $3, NULL, 0))
              MYSQL_YYABORT;
            lex->table_type= TABLE_TYPE_SEQUENCE;
          }
        | MASTER_SYM STATUS_SYM
          {
            Lex->sql_command = SQLCOM_SHOW_MASTER_STAT;
          }
        | ALL SLAVES STATUS_SYM
          {
            Lex->sql_command = SQLCOM_SHOW_SLAVE_STAT;
            Lex->verbose= 1;
          }
        | SLAVE STATUS_SYM
          {
            LEX *lex= thd->lex;
            lex->mi.connection_name= null_clex_str;
            lex->sql_command = SQLCOM_SHOW_SLAVE_STAT;
            lex->verbose= 0;
          }
        | SLAVE connection_name STATUS_SYM
          {
            Lex->sql_command = SQLCOM_SHOW_SLAVE_STAT;
            Lex->verbose= 0;
          }
        | CREATE PROCEDURE_SYM sp_name
          {
            LEX *lex= Lex;

            lex->sql_command = SQLCOM_SHOW_CREATE_PROC;
            lex->spname= $3;
          }
        | CREATE FUNCTION_SYM sp_name
          {
            LEX *lex= Lex;

            lex->sql_command = SQLCOM_SHOW_CREATE_FUNC;
            lex->spname= $3;
          }
        | CREATE PACKAGE_MARIADB_SYM sp_name
          {
            LEX *lex= Lex;
            lex->sql_command = SQLCOM_SHOW_CREATE_PACKAGE;
            lex->spname= $3;
          }
        | CREATE PACKAGE_MARIADB_SYM BODY_MARIADB_SYM sp_name
          {
            LEX *lex= Lex;
            lex->sql_command = SQLCOM_SHOW_CREATE_PACKAGE_BODY;
            lex->spname= $4;
          }
        | CREATE TRIGGER_SYM sp_name
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_CREATE_TRIGGER;
            lex->spname= $3;
          }
        | CREATE USER_SYM
          {
            Lex->sql_command= SQLCOM_SHOW_CREATE_USER;
            if (unlikely(!(Lex->grant_user=
                          (LEX_USER*)thd->alloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            Lex->grant_user->user= current_user;
          }
        | CREATE USER_SYM user
          {
             Lex->sql_command= SQLCOM_SHOW_CREATE_USER;
             Lex->grant_user= $3;
          }
        | PROCEDURE_SYM STATUS_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_STATUS_PROC;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_PROCEDURES)))
              MYSQL_YYABORT;
          }
        | FUNCTION_SYM STATUS_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_STATUS_FUNC;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_PROCEDURES)))
              MYSQL_YYABORT;
          }
        | PACKAGE_MARIADB_SYM STATUS_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_STATUS_PACKAGE;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_PROCEDURES)))
              MYSQL_YYABORT;
          }
        | PACKAGE_MARIADB_SYM BODY_MARIADB_SYM STATUS_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_STATUS_PACKAGE_BODY;
            if (unlikely(prepare_schema_table(thd, lex, 0, SCH_PROCEDURES)))
              MYSQL_YYABORT;
          }
        | PROCEDURE_SYM CODE_SYM sp_name
          {
            Lex->sql_command= SQLCOM_SHOW_PROC_CODE;
            Lex->spname= $3;
          }
        | FUNCTION_SYM CODE_SYM sp_name
          {
            Lex->sql_command= SQLCOM_SHOW_FUNC_CODE;
            Lex->spname= $3;
          }
        | PACKAGE_MARIADB_SYM BODY_MARIADB_SYM CODE_SYM sp_name
          {
            Lex->sql_command= SQLCOM_SHOW_PACKAGE_BODY_CODE;
            Lex->spname= $4;
          }
        | CREATE EVENT_SYM sp_name
          {
            Lex->spname= $3;
            Lex->sql_command = SQLCOM_SHOW_CREATE_EVENT;
          }
        | describe_command FOR_SYM expr
          {
            Lex->sql_command= SQLCOM_SHOW_EXPLAIN;
            if (unlikely(prepare_schema_table(thd, Lex, 0, SCH_EXPLAIN)))
              MYSQL_YYABORT;
            add_value_to_list(thd, $3);
          }
        | IDENT_sys remember_tok_start wild_and_where
           {
             LEX *lex= Lex;
             bool in_plugin;
             lex->sql_command= SQLCOM_SHOW_GENERIC;
             ST_SCHEMA_TABLE *table= find_schema_table(thd, &$1, &in_plugin);
             if (unlikely(!table || !table->old_format || !in_plugin))
             {
               thd->parse_error(ER_SYNTAX_ERROR, $2);
               MYSQL_YYABORT;
             }
             if (unlikely(lex->wild && table->idx_field1 < 0))
             {
               thd->parse_error(ER_SYNTAX_ERROR, $3);
               MYSQL_YYABORT;
             }
             if (unlikely(make_schema_select(thd, Lex->current_select, table)))
               MYSQL_YYABORT;
           }
        ;

show_engine_param:
          STATUS_SYM
          { Lex->sql_command= SQLCOM_SHOW_ENGINE_STATUS; }
        | MUTEX_SYM
          { Lex->sql_command= SQLCOM_SHOW_ENGINE_MUTEX; }
        | LOGS_SYM
          { Lex->sql_command= SQLCOM_SHOW_ENGINE_LOGS; }
        ;

master_or_binary:
          MASTER_SYM
        | BINARY
        ;

opt_storage:
          /* empty */
        | STORAGE_SYM
        ;

opt_db:
          /* empty */      { $$= null_clex_str; }
        | from_or_in ident { $$= $2; }
        ;

opt_full:
          /* empty */ { Lex->verbose=0; }
        | FULL        { Lex->verbose=1; }
        ;

from_or_in:
          FROM
        | IN_SYM
        ;

binlog_in:
          /* empty */            { Lex->mi.log_file_name = 0; }
        | IN_SYM TEXT_STRING_sys { Lex->mi.log_file_name = $2.str; }
        ;

binlog_from:
          /* empty */        { Lex->mi.pos = 4; /* skip magic number */ }
        | FROM ulonglong_num { Lex->mi.pos = $2; }
        ;

wild_and_where:
          /* empty */ { $$= 0; }
        | LIKE remember_tok_start TEXT_STRING_sys
          {
            Lex->wild= new (thd->mem_root) String($3.str, $3.length,
                                                    system_charset_info);
            if (unlikely(Lex->wild == NULL))
              MYSQL_YYABORT;
            $$= $2;
          }
        | WHERE remember_tok_start expr
          {
            Select->where= normalize_cond(thd, $3);
            if ($3)
              $3->top_level_item();
            $$= $2;
          }
        ;

/* A Oracle compatible synonym for show */
describe:
          describe_command table_ident
          {
            LEX *lex= Lex;
            mysql_init_select(lex);
            lex->current_select->parsing_place= SELECT_LIST;
            lex->sql_command= SQLCOM_SHOW_FIELDS;
            lex->first_select_lex()->db= null_clex_str;
            lex->verbose= 0;
            if (unlikely(prepare_schema_table(thd, lex, $2, SCH_COLUMNS)))
              MYSQL_YYABORT;
          }
          opt_describe_column
          {
            Select->parsing_place= NO_MATTER;
          }
        | describe_command opt_extended_describe
          { Lex->describe|= DESCRIBE_NORMAL; }
          explainable_command
          {
            LEX *lex=Lex;
            lex->first_select_lex()->options|= SELECT_DESCRIBE;
          }
        ;

explainable_command:
          select
        | select_into
        | insert
        | replace
        | update
        | delete
        ;

describe_command:
          DESC
        | DESCRIBE
        ;

analyze_stmt_command:
          ANALYZE_SYM opt_format_json explainable_command
          {
            Lex->analyze_stmt= true;
          }
        ;

opt_extended_describe:
          EXTENDED_SYM   { Lex->describe|= DESCRIBE_EXTENDED; }
        | EXTENDED_SYM ALL
          { Lex->describe|= DESCRIBE_EXTENDED | DESCRIBE_EXTENDED2; }
        | PARTITIONS_SYM { Lex->describe|= DESCRIBE_PARTITIONS; }
        | opt_format_json {}
        ;

opt_format_json:
          /* empty */ {}
        | FORMAT_SYM '=' ident_or_text
          {
            if (lex_string_eq(&$3, STRING_WITH_LEN("JSON")))
              Lex->explain_json= true;
            else if (lex_string_eq(&$3, STRING_WITH_LEN("TRADITIONAL")))
              DBUG_ASSERT(Lex->explain_json==false);
            else
              my_yyabort_error((ER_UNKNOWN_EXPLAIN_FORMAT, MYF(0), "EXPLAIN",
                               $3.str));
          }
        ;

opt_describe_column:
          /* empty */ {}
        | text_string { Lex->wild= $1; }
        | ident
          {
            Lex->wild= new (thd->mem_root) String((const char*) $1.str,
                                                    $1.length,
                                                    system_charset_info);
            if (unlikely(Lex->wild == NULL))
              MYSQL_YYABORT;
          }
        ;


/* flush things */

flush:
          FLUSH_SYM opt_no_write_to_binlog
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_FLUSH;
            lex->type= 0;
            lex->no_write_to_binlog= $2;
          }
          flush_options {}
        ;

flush_options:
          table_or_tables
          {
            Lex->type|= REFRESH_TABLES;
            /*
              Set type of metadata and table locks for
              FLUSH TABLES table_list [WITH READ LOCK].
            */
            YYPS->m_lock_type= TL_READ_NO_INSERT;
            YYPS->m_mdl_type= MDL_SHARED_HIGH_PRIO;
          }
          opt_table_list opt_flush_lock
          {}
        | flush_options_list
          {}
        ;

opt_flush_lock:
          /* empty */ {}
        | flush_lock
        {
          TABLE_LIST *tables= Lex->query_tables;
          for (; tables; tables= tables->next_global)
          {
            tables->mdl_request.set_type(MDL_SHARED_NO_WRITE);
            /* Don't try to flush views. */
            tables->required_type= TABLE_TYPE_NORMAL;
            /* Ignore temporary tables. */
            tables->open_type= OT_BASE_ONLY;
          }
        }
        ;

flush_lock:
          WITH READ_SYM LOCK_SYM optional_flush_tables_arguments
          { Lex->type|= REFRESH_READ_LOCK | $4; }
        | FOR_SYM
          {
            if (unlikely(Lex->query_tables == NULL))
            {
              // Table list can't be empty
              thd->parse_error(ER_NO_TABLES_USED);
              MYSQL_YYABORT;
            } 
            Lex->type|= REFRESH_FOR_EXPORT;
          } EXPORT_SYM {}
        ;

flush_options_list:
          flush_options_list ',' flush_option
        | flush_option
          {}
        ;

flush_option:
          ERROR_SYM LOGS_SYM
          { Lex->type|= REFRESH_ERROR_LOG; }
        | ENGINE_SYM LOGS_SYM
          { Lex->type|= REFRESH_ENGINE_LOG; } 
        | GENERAL LOGS_SYM
          { Lex->type|= REFRESH_GENERAL_LOG; }
        | SLOW LOGS_SYM
          { Lex->type|= REFRESH_SLOW_LOG; }
        | BINARY LOGS_SYM opt_delete_gtid_domain
          { Lex->type|= REFRESH_BINARY_LOG; }
        | RELAY LOGS_SYM optional_connection_name
          {
            LEX *lex= Lex;
            if (unlikely(lex->type & REFRESH_RELAY_LOG))
              my_yyabort_error((ER_WRONG_USAGE, MYF(0), "FLUSH", "RELAY LOGS"));
            lex->type|= REFRESH_RELAY_LOG;
            lex->relay_log_connection_name= lex->mi.connection_name;
           }
        | QUERY_SYM CACHE_SYM
          { Lex->type|= REFRESH_QUERY_CACHE_FREE; }
        | HOSTS_SYM
          { Lex->type|= REFRESH_HOSTS; }
        | PRIVILEGES
          { Lex->type|= REFRESH_GRANT; }
        | LOGS_SYM
          {
            Lex->type|= REFRESH_LOG;
            Lex->relay_log_connection_name= empty_clex_str;
          }
        | STATUS_SYM
          { Lex->type|= REFRESH_STATUS; }
        | SLAVE optional_connection_name 
          { 
            LEX *lex= Lex;
            if (unlikely(lex->type & REFRESH_SLAVE))
              my_yyabort_error((ER_WRONG_USAGE, MYF(0), "FLUSH","SLAVE"));
            lex->type|= REFRESH_SLAVE;
            lex->reset_slave_info.all= false;
          }
        | MASTER_SYM
          { Lex->type|= REFRESH_MASTER; }
        | DES_KEY_FILE
          { Lex->type|= REFRESH_DES_KEY_FILE; }
        | RESOURCES
          { Lex->type|= REFRESH_USER_RESOURCES; }
        | IDENT_sys remember_tok_start
           {
             Lex->type|= REFRESH_GENERIC;
             ST_SCHEMA_TABLE *table= find_schema_table(thd, &$1);
             if (unlikely(!table || !table->reset_table))
             {
               thd->parse_error(ER_SYNTAX_ERROR, $2);
               MYSQL_YYABORT;
             }
             if (unlikely(Lex->view_list.push_back((LEX_CSTRING*)
                                                   thd->memdup(&$1, sizeof(LEX_CSTRING)),
                                                   thd->mem_root)))
               MYSQL_YYABORT;
           }
        ;

opt_table_list:
          /* empty */  {}
        | table_list {}
        ;

backup:
        BACKUP_SYM STAGE_SYM ident
        {
          int type;
          if (unlikely(Lex->sphead))
            my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "BACKUP STAGE"));
          if ((type= find_type($3.str, &backup_stage_names,
                               FIND_TYPE_NO_PREFIX)) <= 0)
            my_yyabort_error((ER_BACKUP_UNKNOWN_STAGE, MYF(0), $3.str));
          Lex->sql_command= SQLCOM_BACKUP;
          Lex->backup_stage= (backup_stages) (type-1);
          break;
        }
        ;

opt_delete_gtid_domain:
          /* empty */ {}
        | DELETE_DOMAIN_ID_SYM '=' '(' delete_domain_id_list ')'
          {}
        ;
delete_domain_id_list:
          /* Empty */
        | delete_domain_id
        | delete_domain_id_list ',' delete_domain_id
        ;

delete_domain_id:
          ulonglong_num
          {
            uint32 value= (uint32) $1;
            if ($1 > UINT_MAX32)
            {
              my_printf_error(ER_BINLOG_CANT_DELETE_GTID_DOMAIN,
                              "The value of gtid domain being deleted ('%llu') "
                              "exceeds its maximum size "
                              "of 32 bit unsigned integer", MYF(0), $1);
              MYSQL_YYABORT;
            }
            insert_dynamic(&Lex->delete_gtid_domain, (uchar*) &value);
          }
        ;

optional_flush_tables_arguments:
          /* empty */        {$$= 0;}
        | AND_SYM DISABLE_SYM CHECKPOINT_SYM {$$= REFRESH_CHECKPOINT; } 
        ;

reset:
          RESET_SYM
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_RESET; lex->type=0;
          }
          reset_options
          {}
        ;

reset_options:
          reset_options ',' reset_option
        | reset_option
        ;

reset_option:
          SLAVE               { Lex->type|= REFRESH_SLAVE; }
          optional_connection_name
          slave_reset_options { }
        | MASTER_SYM
          {
             Lex->type|= REFRESH_MASTER;
             Lex->next_binlog_file_number= 0;
          }
          master_reset_options
        | QUERY_SYM CACHE_SYM { Lex->type|= REFRESH_QUERY_CACHE;}
        ;

slave_reset_options:
          /* empty */ { Lex->reset_slave_info.all= false; }
        | ALL         { Lex->reset_slave_info.all= true; }
        ;

master_reset_options:
          /* empty */ {}
        | TO_SYM ulong_num
          {
            Lex->next_binlog_file_number = $2;
          }
        ;

purge:
          PURGE
          {
            LEX *lex=Lex;
            lex->type=0;
            lex->sql_command = SQLCOM_PURGE;
          }
          purge_options
          {}
        ;

purge_options:
          master_or_binary LOGS_SYM purge_option
        ;

purge_option:
          TO_SYM TEXT_STRING_sys
          {
            Lex->to_log = $2.str;
          }
        | BEFORE_SYM expr
          {
            LEX *lex= Lex;
            lex->value_list.empty();
            lex->value_list.push_front($2, thd->mem_root);
            lex->sql_command= SQLCOM_PURGE_BEFORE;
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        ;

/* kill threads */

kill:
          KILL_SYM
          {
            LEX *lex=Lex;
            lex->value_list.empty();
            lex->users_list.empty();
            lex->sql_command= SQLCOM_KILL;
            lex->kill_type= KILL_TYPE_ID;
          }
          kill_type kill_option kill_expr
          {
            Lex->kill_signal= (killed_state) ($3 | $4);
          }
        ;

kill_type:
        /* Empty */    { $$= (int) KILL_HARD_BIT; }
        | HARD_SYM     { $$= (int) KILL_HARD_BIT; }
        | SOFT_SYM     { $$= 0; }
        ;

kill_option:
          /* empty */    { $$= (int) KILL_CONNECTION; }
        | CONNECTION_SYM { $$= (int) KILL_CONNECTION; }
        | QUERY_SYM      { $$= (int) KILL_QUERY; }
        | QUERY_SYM ID_SYM
          {
            $$= (int) KILL_QUERY;
            Lex->kill_type= KILL_TYPE_QUERY;
          }
        ;

kill_expr:
        expr
        {
          Lex->value_list.push_front($$, thd->mem_root);
         }
        | USER_SYM user
          {
            Lex->users_list.push_back($2, thd->mem_root);
            Lex->kill_type= KILL_TYPE_USER;
          }
        ;


shutdown:
        SHUTDOWN { Lex->sql_command= SQLCOM_SHUTDOWN; }
        ;

/* change database */

use:
          USE_SYM ident
          {
            LEX *lex=Lex;
            lex->sql_command=SQLCOM_CHANGE_DB;
            lex->first_select_lex()->db= $2;
          }
        ;

/* import, export of files */

load:
          LOAD data_or_xml
          {
            LEX *lex= thd->lex;

            if (unlikely(lex->sphead))
            {
              my_error(ER_SP_BADSTATEMENT, MYF(0), 
                       $2 == FILETYPE_CSV ? "LOAD DATA" : "LOAD XML");
              MYSQL_YYABORT;
            }
            if (lex->main_select_push())
              MYSQL_YYABORT;
            mysql_init_select(lex);
          }
          load_data_lock opt_local INFILE TEXT_STRING_filesystem
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_LOAD;
            lex->local_file=  $5;
            lex->duplicates= DUP_ERROR;
            lex->ignore= 0;
            if (unlikely(!(lex->exchange= new (thd->mem_root)
                         sql_exchange($7.str, 0, $2))))
              MYSQL_YYABORT;
          }
          opt_duplicate INTO TABLE_SYM table_ident opt_use_partition
          {
            LEX *lex=Lex;
            if (unlikely(!Select->add_table_to_list(thd, $12, NULL,
                                                   TL_OPTION_UPDATING,
                                                   $4, MDL_SHARED_WRITE,
                                                   NULL, $13)))
              MYSQL_YYABORT;
            lex->field_list.empty();
            lex->update_list.empty();
            lex->value_list.empty();
            lex->many_values.empty();
          }
          opt_load_data_charset
          { Lex->exchange->cs= $15; }
          opt_xml_rows_identified_by
          opt_field_term opt_line_term opt_ignore_lines opt_field_or_var_spec
          opt_load_data_set_spec
          {
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
          ;

data_or_xml:
        DATA_SYM  { $$= FILETYPE_CSV; }
        | XML_SYM { $$= FILETYPE_XML; }
        ;

opt_local:
          /* empty */ { $$=0;}
        | LOCAL_SYM { $$=1;}
        ;

load_data_lock:
          /* empty */ { $$= TL_WRITE_DEFAULT; }
        | CONCURRENT
          {
            /*
              Ignore this option in SP to avoid problem with query cache and
              triggers with non default priority locks
            */
            $$= (Lex->sphead ? TL_WRITE_DEFAULT : TL_WRITE_CONCURRENT_INSERT);
          }
        | LOW_PRIORITY { $$= TL_WRITE_LOW_PRIORITY; }
        ;

opt_duplicate:
          /* empty */ { Lex->duplicates=DUP_ERROR; }
        | REPLACE { Lex->duplicates=DUP_REPLACE; }
        | IGNORE_SYM { Lex->ignore= 1; }
        ;

opt_field_term:
          /* empty */
        | COLUMNS field_term_list
        ;

field_term_list:
          field_term_list field_term
        | field_term
        ;

field_term:
          TERMINATED BY text_string 
          {
            DBUG_ASSERT(Lex->exchange != 0);
            Lex->exchange->field_term= $3;
          }
        | OPTIONALLY ENCLOSED BY text_string
          {
            LEX *lex= Lex;
            DBUG_ASSERT(lex->exchange != 0);
            lex->exchange->enclosed= $4;
            lex->exchange->opt_enclosed= 1;
          }
        | ENCLOSED BY text_string
          {
            DBUG_ASSERT(Lex->exchange != 0);
            Lex->exchange->enclosed= $3;
          }
        | ESCAPED BY text_string
          {
            DBUG_ASSERT(Lex->exchange != 0);
            Lex->exchange->escaped= $3;
          }
        ;

opt_line_term:
          /* empty */
        | LINES line_term_list
        ;

line_term_list:
          line_term_list line_term
        | line_term
        ;

line_term:
          TERMINATED BY text_string
          {
            DBUG_ASSERT(Lex->exchange != 0);
            Lex->exchange->line_term= $3;
          }
        | STARTING BY text_string
          {
            DBUG_ASSERT(Lex->exchange != 0);
            Lex->exchange->line_start= $3;
          }
        ;

opt_xml_rows_identified_by:
        /* empty */ { }
        | ROWS_SYM IDENTIFIED_SYM BY text_string
          { Lex->exchange->line_term = $4; };

opt_ignore_lines:
          /* empty */
        | IGNORE_SYM NUM lines_or_rows
          {
            DBUG_ASSERT(Lex->exchange != 0);
            Lex->exchange->skip_lines= atol($2.str);
          }
        ;

lines_or_rows:
          LINES { }
        | ROWS_SYM { }
        ;

opt_field_or_var_spec:
          /* empty */ {}
        | '(' fields_or_vars ')' {}
        | '(' ')' {}
        ;

fields_or_vars:
          fields_or_vars ',' field_or_var
          { Lex->field_list.push_back($3, thd->mem_root); }
        | field_or_var
          { Lex->field_list.push_back($1, thd->mem_root); }
        ;

field_or_var:
          simple_ident_nospvar {$$= $1;}
        | '@' ident_or_text
          {
            $$= new (thd->mem_root) Item_user_var_as_out_param(thd, &$2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

opt_load_data_set_spec:
          /* empty */ {}
        | SET load_data_set_list {}
        ;

load_data_set_list:
          load_data_set_list ',' load_data_set_elem
        | load_data_set_elem
        ;

load_data_set_elem:
          simple_ident_nospvar equal remember_name expr_or_default remember_end
          {
            LEX *lex= Lex;
            if (unlikely(lex->update_list.push_back($1, thd->mem_root)) ||
                unlikely(lex->value_list.push_back($4, thd->mem_root)))
                MYSQL_YYABORT;
            $4->set_name_no_truncate(thd, $3, (uint) ($5 - $3), thd->charset());
          }
        ;

/* Common definitions */

text_literal:
          TEXT_STRING
          {
            if (unlikely(!($$= thd->make_string_literal($1))))
              MYSQL_YYABORT;
          }
        | NCHAR_STRING
          {
            if (unlikely(!($$= thd->make_string_literal_nchar($1))))
              MYSQL_YYABORT;
          }
        | UNDERSCORE_CHARSET TEXT_STRING
          {
            if (unlikely(!($$= thd->make_string_literal_charset($2, $1))))
              MYSQL_YYABORT;
          }
        | text_literal TEXT_STRING_literal
          {
            if (unlikely(!($$= $1->make_string_literal_concat(thd, &$2))))
              MYSQL_YYABORT;
          }
        ;

text_string:
          TEXT_STRING_literal
          {
            $$= new (thd->mem_root) String($1.str,
                                             $1.length,
                                             thd->variables.collation_connection);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
          | hex_or_bin_String { $$= $1; }
          ;


hex_or_bin_String:
          HEX_NUM
          {
            Item *tmp= new (thd->mem_root) Item_hex_hybrid(thd, $1.str,
                                                           $1.length);
            if (unlikely(tmp == NULL))
              MYSQL_YYABORT;
            $$= tmp->val_str((String*) 0);
          }
        | HEX_STRING
          {
            Item *tmp= new (thd->mem_root) Item_hex_string(thd, $1.str,
                                                           $1.length);
            if (unlikely(tmp == NULL))
              MYSQL_YYABORT;
            $$= tmp->val_str((String*) 0);
          }
        | BIN_NUM
          {
            Item *tmp= new (thd->mem_root) Item_bin_string(thd, $1.str,
                                                           $1.length);
            if (unlikely(tmp == NULL))
              MYSQL_YYABORT;
            /*
              it is OK only emulate fix_fields, because we need only
              value of constant
            */
            $$= tmp->val_str((String*) 0);
          }
        ;

param_marker:
          PARAM_MARKER
          {
            if (unlikely(!($$= Lex->add_placeholder(thd, &param_clex_str,
                                                    YYLIP->get_tok_start(),
                                                    YYLIP->get_tok_start() + 1))))
              MYSQL_YYABORT;
          }
        | COLON_ORACLE_SYM ident_cli
          {
            if (unlikely(!($$= Lex->add_placeholder(thd, &null_clex_str,
                                                    $1.pos(), $2.end()))))
              MYSQL_YYABORT;
          }
        | COLON_ORACLE_SYM NUM
          {
            if (unlikely(!($$= Lex->add_placeholder(thd, &null_clex_str,
                                                    $1.pos(),
                                                    YYLIP->get_ptr()))))
              MYSQL_YYABORT;
          }
        ;

signed_literal:
        '+' NUM_literal { $$ = $2; }
        | '-' NUM_literal
          {
            $2->max_length++;
            $$= $2->neg(thd);
          }
        ;

literal:
          text_literal { $$ = $1; }
        | NUM_literal { $$ = $1; }
        | temporal_literal { $$= $1; }
        | NULL_SYM
          {
            /*
              For the digest computation, in this context only,
              NULL is considered a literal, hence reduced to '?'
              REDUCE:
                TOK_GENERIC_VALUE := NULL_SYM
            */
            YYLIP->reduce_digest_token(TOK_GENERIC_VALUE, NULL_SYM);
            $$= new (thd->mem_root) Item_null(thd);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
            YYLIP->next_state= MY_LEX_OPERATOR_OR_IDENT;
          }
        | FALSE_SYM
          {
            $$= new (thd->mem_root) Item_bool(thd, (char*) "FALSE",0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | TRUE_SYM
          {
            $$= new (thd->mem_root) Item_bool(thd, (char*) "TRUE",1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | HEX_NUM
          {
            $$= new (thd->mem_root) Item_hex_hybrid(thd, $1.str, $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | HEX_STRING
          {
            $$= new (thd->mem_root) Item_hex_string(thd, $1.str, $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | BIN_NUM
          {
            $$= new (thd->mem_root) Item_bin_string(thd, $1.str, $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | UNDERSCORE_CHARSET hex_or_bin_String
          {
            Item_string_with_introducer *item_str;
            /*
              Pass NULL as name. Name will be set in the "select_item" rule and
              will include the introducer and the original hex/bin notation.
            */
            item_str= new (thd->mem_root)
               Item_string_with_introducer(thd, NULL, $2->ptr(), $2->length(),
                                           $1);
            if (unlikely(!item_str ||
                         !item_str->check_well_formed_result(true)))
              MYSQL_YYABORT;

            $$= item_str;
          }
        ;

NUM_literal:
          NUM
          {
            int error;
            $$= new (thd->mem_root)
                  Item_int(thd, $1.str,
                           (longlong) my_strtoll10($1.str, NULL, &error),
                           $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | LONG_NUM
          {
            int error;
            $$= new (thd->mem_root)
                  Item_int(thd, $1.str,
                           (longlong) my_strtoll10($1.str, NULL, &error),
                           $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | ULONGLONG_NUM
          {
            $$= new (thd->mem_root) Item_uint(thd, $1.str, $1.length);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | DECIMAL_NUM
          {
            $$= new (thd->mem_root) Item_decimal(thd, $1.str, $1.length,
                                                   thd->charset());
            if (unlikely($$ == NULL) || unlikely(thd->is_error()))
              MYSQL_YYABORT;
          }
        | FLOAT_NUM
          {
            $$= new (thd->mem_root) Item_float(thd, $1.str, $1.length);
            if (unlikely($$ == NULL) || unlikely(thd->is_error()))
              MYSQL_YYABORT;
          }
        ;


temporal_literal:
        DATE_SYM TEXT_STRING
          {
            if (unlikely(!($$= type_handler_newdate.create_literal_item(thd,
                                                           $2.str, $2.length,
                                                           YYCSCL, true))))
              MYSQL_YYABORT;
          }
        | TIME_SYM TEXT_STRING
          {
            if (unlikely(!($$= type_handler_time2.create_literal_item(thd,
                                                         $2.str, $2.length,
                                                         YYCSCL, true))))
              MYSQL_YYABORT;
          }
        | TIMESTAMP TEXT_STRING
          {
            if (unlikely(!($$= type_handler_datetime.create_literal_item(thd,
                                                            $2.str, $2.length,
                                                            YYCSCL, true))))
              MYSQL_YYABORT;
          }
        ;


opt_with_clause:
	  /*empty */ { $$= 0; }
	| with_clause
          {
            $$= $1;
          }
	;


with_clause:
          WITH opt_recursive
          {
             LEX *lex= Lex;
             With_clause *with_clause=
             new With_clause($2, Lex->curr_with_clause);
             if (unlikely(with_clause == NULL))
               MYSQL_YYABORT;
             lex->derived_tables|= DERIVED_WITH;
             lex->curr_with_clause= with_clause;
             with_clause->add_to_list(Lex->with_clauses_list_last_next);
             if (lex->current_select &&
                 lex->current_select->parsing_place == BEFORE_OPT_LIST)
               lex->current_select->parsing_place= NO_MATTER;
          }
          with_list
          {
            $$= Lex->curr_with_clause;
            Lex->curr_with_clause= Lex->curr_with_clause->pop();
          } 
        ;


opt_recursive:
 	  /*empty*/ { $$= 0; }
	| RECURSIVE_SYM { $$= 1; }
	;


with_list:
	  with_list_element 	
	| with_list ',' with_list_element
	;


with_list_element:
	  query_name
	  opt_with_column_list 
          {
            $2= new List<LEX_CSTRING> (Lex->with_column_list);
            if (unlikely($2 == NULL))
              MYSQL_YYABORT;
            Lex->with_column_list.empty();
          }
          AS '(' remember_tok_start query_expression remember_tok_end ')'
 	  {
            LEX *lex= thd->lex;
            const char *query_start= lex->sphead ? lex->sphead->m_tmp_query
                                                 : thd->query();
            char *spec_start= $6 + 1;
            With_element *elem= new With_element($1, *$2, $7);
	    if (elem == NULL || Lex->curr_with_clause->add_with_element(elem))
	      MYSQL_YYABORT;
            if (elem->set_unparsed_spec(thd, spec_start, $8,
                                        spec_start - query_start))
              MYSQL_YYABORT;
	  }
	;


opt_with_column_list:
          /* empty */
          { $$= NULL; }
        | '(' with_column_list ')'
          { $$= NULL; }
        ;


with_column_list:
          ident 
          {
            Lex->with_column_list.push_back((LEX_CSTRING*)
                    thd->memdup(&$1, sizeof(LEX_CSTRING)));
	  }
        | with_column_list ',' ident
          {
            Lex->with_column_list.push_back((LEX_CSTRING*)
                    thd->memdup(&$3, sizeof(LEX_CSTRING)));
          }
        ;


query_name: 
          ident
          {
            $$= (LEX_CSTRING *) thd->memdup(&$1, sizeof(LEX_CSTRING));
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;


	
/**********************************************************************
** Creating different items.
**********************************************************************/

insert_ident:
          simple_ident_nospvar { $$=$1; }
        | table_wild { $$=$1; }
        ;

table_wild:
          ident '.' '*'
          {
            if (unlikely(!($$= Lex->create_item_qualified_asterisk(thd, &$1))))
              MYSQL_YYABORT;
          }
        | ident '.' ident '.' '*'
          {
            if (unlikely(!($$= Lex->create_item_qualified_asterisk(thd, &$1, &$3))))
              MYSQL_YYABORT;
          }
        ;

select_sublist_qualified_asterisk:
          ident_cli '.' '*'
          {
            if (unlikely(!($$= Lex->create_item_qualified_asterisk(thd, &$1))))
              MYSQL_YYABORT;
          }
        | ident_cli '.' ident_cli '.' '*'
          {
            if (unlikely(!($$= Lex->create_item_qualified_asterisk(thd, &$1, &$3))))
              MYSQL_YYABORT;
          }
        ;

order_ident:
          expr { $$=$1; }
        ;


simple_ident:
          ident_cli
          {
            if (unlikely(!($$= Lex->create_item_ident(thd, &$1))))
              MYSQL_YYABORT;
          }
        | ident_cli '.' ident_cli
          {
            if (unlikely(!($$= Lex->create_item_ident(thd, &$1, &$3))))
              MYSQL_YYABORT;
          }
        | '.' ident_cli '.' ident_cli
          {
            Lex_ident_cli empty($2.pos(), 0);
            if (unlikely(!($$= Lex->create_item_ident(thd, &empty, &$2, &$4))))
              MYSQL_YYABORT;
          }
        | ident_cli '.' ident_cli '.' ident_cli
          {
            if (unlikely(!($$= Lex->create_item_ident(thd, &$1, &$3, &$5))))
              MYSQL_YYABORT;
          }
        | COLON_ORACLE_SYM ident_cli '.' ident_cli
          {
            if (unlikely(!($$= Lex->make_item_colon_ident_ident(thd, &$2, &$4))))
              MYSQL_YYABORT;
          }
        ;

simple_ident_nospvar:
          ident
          {
            if (unlikely(!($$= Lex->create_item_ident_nosp(thd, &$1))))
              MYSQL_YYABORT;
          }
        | ident '.' ident
          {
            if (unlikely(!($$= Lex->create_item_ident_nospvar(thd, &$1, &$3))))
              MYSQL_YYABORT;
          }
        | COLON_ORACLE_SYM ident_cli '.' ident_cli
          {
            if (unlikely(!($$= Lex->make_item_colon_ident_ident(thd, &$2, &$4))))
              MYSQL_YYABORT;
          }
        | '.' ident '.' ident
          {
            Lex_ident_sys none;
            if (unlikely(!($$= Lex->create_item_ident(thd, &none, &$2, &$4))))
              MYSQL_YYABORT;
          }
        | ident '.' ident '.' ident
          {
            if (unlikely(!($$= Lex->create_item_ident(thd, &$1, &$3, &$5))))
              MYSQL_YYABORT;
          }
        ;

field_ident:
          ident { $$=$1;}
        | ident '.' ident '.' ident
          {
            TABLE_LIST *table= Select->table_list.first;
            if (unlikely(my_strcasecmp(table_alias_charset, $1.str,
                                       table->db.str)))
              my_yyabort_error((ER_WRONG_DB_NAME, MYF(0), $1.str));
            if (unlikely(my_strcasecmp(table_alias_charset, $3.str,
                                       table->table_name.str)))
              my_yyabort_error((ER_WRONG_TABLE_NAME, MYF(0), $3.str));
            $$=$5;
          }
        | ident '.' ident
          {
            TABLE_LIST *table= Select->table_list.first;
            if (unlikely(my_strcasecmp(table_alias_charset, $1.str,
                         table->alias.str)))
              my_yyabort_error((ER_WRONG_TABLE_NAME, MYF(0), $1.str));
            $$=$3;
          }
        | '.' ident { $$=$2;} /* For Delphi */
        ;

table_ident:
          ident
          {
            $$= new (thd->mem_root) Table_ident(&$1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | ident '.' ident
          {
            $$= new (thd->mem_root) Table_ident(thd, &$1, &$3, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | '.' ident
          {
            /* For Delphi */
            $$= new (thd->mem_root) Table_ident(&$2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

table_ident_opt_wild:
          ident opt_wild
          {
            $$= new (thd->mem_root) Table_ident(&$1);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | ident '.' ident opt_wild
          {
            $$= new (thd->mem_root) Table_ident(thd, &$1, &$3, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

table_ident_nodb:
          ident
          {
            LEX_CSTRING db={(char*) any_db,3};
            $$= new (thd->mem_root) Table_ident(thd, &db, &$1, 0);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

IDENT_cli:
          IDENT
        | IDENT_QUOTED
        ;

ident_cli:
          IDENT
        | IDENT_QUOTED
        | keyword_ident { $$= $1; }
        ;

IDENT_sys:
          IDENT_cli
          {
            if (unlikely(thd->to_ident_sys_alloc(&$$, &$1)))
              MYSQL_YYABORT;
          }
        ;

TEXT_STRING_sys:
          TEXT_STRING
          {
            if (thd->make_text_string_sys(&$$, &$1))
              MYSQL_YYABORT;
          }
        ;

TEXT_STRING_literal:
          TEXT_STRING
          {
            if (thd->make_text_string_connection(&$$, &$1))
              MYSQL_YYABORT;
          }
        ;

TEXT_STRING_filesystem:
          TEXT_STRING
          {
            if (thd->make_text_string_filesystem(&$$, &$1))
              MYSQL_YYABORT;
          }
        ;

ident_table_alias:
          IDENT_sys
        | keyword_table_alias
          {
            if (unlikely($$.copy_keyword(thd, &$1)))
              MYSQL_YYABORT;
          }
        ;

ident_set_usual_case:
          IDENT_sys
        | keyword_set_usual_case
          {
            if (unlikely($$.copy_keyword(thd, &$1)))
              MYSQL_YYABORT;
          }
        ;

ident_sysvar_name:
          IDENT_sys
        | keyword_sysvar_name
          {
            if (unlikely($$.copy_keyword(thd, &$1)))
              MYSQL_YYABORT;
          }
        | TEXT_STRING_sys
          {
            if (unlikely($$.copy_sys(thd, &$1)))
              MYSQL_YYABORT;
          }
        ;


ident:
          IDENT_sys
        | keyword_ident
          {
            if (unlikely($$.copy_keyword(thd, &$1)))
              MYSQL_YYABORT;
          }
        ;

label_ident:
          IDENT_sys
        | keyword_label
          {
            if (unlikely($$.copy_keyword(thd, &$1)))
              MYSQL_YYABORT;
          }
        ;

ident_or_text:
          ident           { $$=$1;}
        | TEXT_STRING_sys { $$=$1;}
        | LEX_HOSTNAME { $$=$1;}
        ;

user_maybe_role:
          ident_or_text
          {
            if (unlikely(!($$=(LEX_USER*) thd->alloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            $$->user = $1;
            $$->host= null_clex_str; // User or Role, see get_current_user()
            $$->reset_auth();

            if (unlikely(check_string_char_length(&$$->user, ER_USERNAME,
                                                  username_char_length,
                                                  system_charset_info, 0)))
              MYSQL_YYABORT;
          }
        | ident_or_text '@' ident_or_text
          {
            if (unlikely(!($$=(LEX_USER*) thd->alloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            $$->user = $1; $$->host=$3;
            $$->reset_auth();

            if (unlikely(check_string_char_length(&$$->user, ER_USERNAME,
                                                  username_char_length,
                                                 system_charset_info, 0)) ||
                unlikely(check_host_name(&$$->host)))
              MYSQL_YYABORT;
            if ($$->host.str[0])
            {
              /*
                Convert hostname part of username to lowercase.
                It's OK to use in-place lowercase as long as
                the character set is utf8.
              */
              my_casedn_str(system_charset_info, (char*) $$->host.str);
            }
            else
            {
              /*
                fix historical undocumented convention that empty host is the
                same as '%'
              */
              $$->host= host_not_specified;
            }
          }
        | CURRENT_USER optional_braces
          {
            if (unlikely(!($$=(LEX_USER*)thd->calloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            $$->user= current_user;
            $$->plugin= empty_clex_str;
            $$->auth= empty_clex_str;
          }
        ;

user_or_role: user_maybe_role | current_role;

user: user_maybe_role
         {
           if ($1->user.str != current_user.str && $1->host.str == 0)
             $1->host= host_not_specified;
           $$= $1;
         }
         ;

/* Keywords which we allow as table aliases. */
keyword_table_alias:
          keyword_data_type
        | keyword_set_special_case
        | keyword_sp_block_section
        | keyword_sp_head
        | keyword_sp_var_and_label
        | keyword_sp_var_not_label
        | keyword_sysvar_type
        | keyword_verb_clause
        ;

/* Keyword that we allow for identifiers (except SP labels) */
keyword_ident:
          keyword_data_type
        | keyword_set_special_case
        | keyword_sp_block_section
        | keyword_sp_head
        | keyword_sp_var_and_label
        | keyword_sp_var_not_label
        | keyword_sysvar_type
        | keyword_verb_clause
        | WINDOW_SYM
        ;

/*
  Keywords that we allow for labels in SPs.
  Should not include keywords that start a statement or SP characteristics.
*/
keyword_label:
          keyword_data_type
        | keyword_set_special_case
        | keyword_sp_var_and_label
        | keyword_sysvar_type
        ;

keyword_sysvar_name:
          keyword_data_type
        | keyword_set_special_case
        | keyword_sp_block_section
        | keyword_sp_head
        | keyword_sp_var_and_label
        | keyword_sp_var_not_label
        | keyword_verb_clause
        | WINDOW_SYM
        ;

keyword_sp_decl:
          keyword_data_type
        | keyword_set_special_case
        | keyword_sp_block_section
        | keyword_sp_head
        | keyword_sp_var_and_label
        | keyword_sp_var_not_label
        | keyword_sysvar_type
        | keyword_verb_clause
        | WINDOW_SYM
        ;

keyword_set_usual_case:
          keyword_data_type
        | keyword_sp_block_section
        | keyword_sp_head
        | keyword_sp_var_and_label
        | keyword_sp_var_not_label
        | keyword_sysvar_type
        | keyword_verb_clause
        | WINDOW_SYM
        ;

/*
  Keywords that we allow in Oracle-style direct assignments:
    xxx := 10;
  but do not allow in labels in the default sql_mode:
    label:
      stmt1;
      stmt2;
  TODO: check if some of them can migrate to keyword_sp_var_and_label.
*/
keyword_sp_var_not_label:
          ASCII_SYM
        | BACKUP_SYM
        | BINLOG_SYM
        | BYTE_SYM
        | CACHE_SYM
        | CHECKSUM_SYM
        | CHECKPOINT_SYM
        | COLUMN_ADD_SYM
        | COLUMN_CHECK_SYM
        | COLUMN_CREATE_SYM
        | COLUMN_DELETE_SYM
        | COLUMN_GET_SYM
        | COMMENT_SYM
        | DEALLOCATE_SYM
        | EXAMINED_SYM
        | EXCLUDE_SYM
        | EXECUTE_SYM
        | FLUSH_SYM
        | FOLLOWING_SYM
        | FORMAT_SYM
        | GET_SYM
        | HELP_SYM
        | HOST_SYM
        | INSTALL_SYM
        | OPTION
        | OPTIONS_SYM
        | OTHERS_MARIADB_SYM
        | OWNER_SYM
        | PARSER_SYM
        | PERIOD_SYM
        | PORT_SYM
        | PRECEDING_SYM
        | PREPARE_SYM
        | REMOVE_SYM
        | RESET_SYM
        | RESTORE_SYM
        | SECURITY_SYM
        | SERVER_SYM
        | SIGNED_SYM
        | SOCKET_SYM
        | SLAVE
        | SLAVES
        | SONAME_SYM
        | START_SYM
        | STOP_SYM
        | STORED_SYM
        | TIES_SYM
        | UNICODE_SYM
        | UNINSTALL_SYM
        | UNBOUNDED_SYM
        | WITHIN
        | WRAPPER_SYM
        | XA_SYM
        | UPGRADE_SYM
        ;

/*
  Keywords that can start optional clauses in SP or trigger declarations
  Allowed as identifiers (e.g. table, column names),
  but:
  - not allowed as SP label names
  - not allowed as variable names in Oracle-style assignments:
    xxx := 10;

  If we allowed these variables in assignments, there would be conflicts
  with SP characteristics, or verb clauses, or compound statements, e.g.:
    CREATE PROCEDURE p1 LANGUAGE ...
  would be either:
    CREATE PROCEDURE p1 LANGUAGE SQL BEGIN END;
  or
    CREATE PROCEDURE p1 LANGUAGE:=10;

  Note, these variables can still be assigned using quoted identifiers:
    `do`:= 10;
    "do":= 10; (when ANSI_QUOTES)
  or using a SET statement:
    SET do= 10;

  Note, some of these keywords are reserved keywords in Oracle.
  In case if heavy grammar conflicts are found in the future,
  we'll possibly need to make them reserved for sql_mode=ORACLE.

  TODO: Allow these variables as SP lables when sql_mode=ORACLE.
  TODO: Allow assigning of "SP characteristics" marked variables
        inside compound blocks.
  TODO: Allow "follows" and "precedes" as variables in compound blocks:
        BEGIN
          follows := 10;
        END;
        as they conflict only with non-block FOR EACH ROW statement:
          CREATE TRIGGER .. FOR EACH ROW follows:= 10;
          CREATE TRIGGER .. FOR EACH ROW FOLLOWS tr1 a:= 10;
*/
keyword_sp_head:
          CONTAINS_SYM           /* SP characteristic               */
        | LANGUAGE_SYM           /* SP characteristic               */
        | NO_SYM                 /* SP characteristic               */
        | CHARSET                /* SET CHARSET utf8;               */
        | FOLLOWS_SYM            /* Conflicts with assignment in FOR EACH */
        | PRECEDES_SYM           /* Conflicts with assignment in FOR EACH */
        ;

/*
  Keywords that start a statement.
  Generally allowed as identifiers (e.g. table, column names)
  - not allowed as SP label names
  - not allowed as variable names in Oracle-style assignments:
    xxx:=10
*/
keyword_verb_clause:
          CLOSE_SYM             /* Verb clause. Reserved in Oracle */
        | COMMIT_SYM            /* Verb clause. Reserved in Oracle */
        | DO_SYM                /* Verb clause                     */
        | HANDLER_SYM           /* Verb clause                     */
        | OPEN_SYM              /* Verb clause. Reserved in Oracle */
        | REPAIR                /* Verb clause                     */
        | ROLLBACK_SYM          /* Verb clause. Reserved in Oracle */
        | SAVEPOINT_SYM         /* Verb clause. Reserved in Oracle */
        | SHUTDOWN              /* Verb clause                     */
        | TRUNCATE_SYM          /* Verb clause. Reserved in Oracle */
        ;

keyword_set_special_case:
          NAMES_SYM
        | ROLE_SYM
        | PASSWORD_SYM
        ;

/*
  Keywords that start an SP block section.
*/
keyword_sp_block_section:
          BEGIN_MARIADB_SYM
        | EXCEPTION_ORACLE_SYM
        | END
        ;

keyword_sysvar_type:
          GLOBAL_SYM
        | LOCAL_SYM
        | SESSION_SYM
        ;


/*
  These keywords are generally allowed as identifiers,
  but not allowed as non-delimited SP variable names in sql_mode=ORACLE.
*/
keyword_data_type:
          BIT_SYM
        | BOOLEAN_SYM
        | BOOL_SYM
        | CLOB_MARIADB_SYM
        | CLOB_ORACLE_SYM
        | DATE_SYM           %prec PREC_BELOW_CONTRACTION_TOKEN2
        | DATETIME
        | ENUM
        | FIXED_SYM
        | GEOMETRYCOLLECTION
        | GEOMETRY_SYM
        | JSON_SYM
        | LINESTRING
        | MEDIUM_SYM
        | MULTILINESTRING
        | MULTIPOINT
        | MULTIPOLYGON
        | NATIONAL_SYM
        | NCHAR_SYM
        | NUMBER_MARIADB_SYM
        | NUMBER_ORACLE_SYM
        | NVARCHAR_SYM
        | POINT_SYM
        | POLYGON
        | RAW_MARIADB_SYM
        | RAW_ORACLE_SYM
        | ROW_SYM
        | SERIAL_SYM
        | TEXT_SYM
        | TIMESTAMP          %prec PREC_BELOW_CONTRACTION_TOKEN2
        | TIME_SYM           %prec PREC_BELOW_CONTRACTION_TOKEN2
        | VARCHAR2_MARIADB_SYM
        | VARCHAR2_ORACLE_SYM
        | YEAR_SYM
        ;


/*
  These keywords are fine for both SP variable names and SP labels.
*/
keyword_sp_var_and_label:
          ACTION
        | ADDDATE_SYM
        | ADMIN_SYM
        | AFTER_SYM
        | AGAINST
        | AGGREGATE_SYM
        | ALGORITHM_SYM
        | ALWAYS_SYM
        | ANY_SYM
        | AT_SYM
        | ATOMIC_SYM
        | AUTHORS_SYM
        | AUTO_INC
        | AUTOEXTEND_SIZE_SYM
        | AUTO_SYM
        | AVG_ROW_LENGTH
        | AVG_SYM
        | BLOCK_SYM
        | BODY_MARIADB_SYM
        | BTREE_SYM
        | CASCADED
        | CATALOG_NAME_SYM
        | CHAIN_SYM
        | CHANGED
        | CIPHER_SYM
        | CLIENT_SYM
        | CLASS_ORIGIN_SYM
        | COALESCE
        | CODE_SYM
        | COLLATION_SYM
        | COLUMN_NAME_SYM
        | COLUMNS
        | COMMITTED_SYM
        | COMPACT_SYM
        | COMPLETION_SYM
        | COMPRESSED_SYM
        | CONCURRENT
        | CONNECTION_SYM
        | CONSISTENT_SYM
        | CONSTRAINT_CATALOG_SYM
        | CONSTRAINT_SCHEMA_SYM
        | CONSTRAINT_NAME_SYM
        | CONTEXT_SYM
        | CONTRIBUTORS_SYM
        | CURRENT_POS_SYM
        | CPU_SYM
        | CUBE_SYM
        /*
          Although a reserved keyword in SQL:2003 (and :2008),
          not reserved in MySQL per WL#2111 specification.
        */
        | CURRENT_SYM
        | CURSOR_NAME_SYM
        | CYCLE_SYM
        | DATA_SYM
        | DATAFILE_SYM
        | DATE_FORMAT_SYM
        | DAY_SYM
        | DECODE_MARIADB_SYM
        | DECODE_ORACLE_SYM
        | DEFINER_SYM
        | DELAY_KEY_WRITE_SYM
        | DES_KEY_FILE
        | DIAGNOSTICS_SYM
        | DIRECTORY_SYM
        | DISABLE_SYM
        | DISCARD
        | DISK_SYM
        | DUMPFILE
        | DUPLICATE_SYM
        | DYNAMIC_SYM
        | ELSEIF_ORACLE_SYM
        | ELSIF_MARIADB_SYM
        | ENDS_SYM
        | ENGINE_SYM
        | ENGINES_SYM
        | ERROR_SYM
        | ERRORS
        | ESCAPE_SYM
        | EVENT_SYM
        | EVENTS_SYM
        | EVERY_SYM
        | EXCEPTION_MARIADB_SYM
        | EXCHANGE_SYM
        | EXPANSION_SYM
        | EXPORT_SYM
        | EXTENDED_SYM
        | EXTENT_SIZE_SYM
        | FAULTS_SYM
        | FAST_SYM
        | FOUND_SYM
        | ENABLE_SYM
        | FULL
        | FILE_SYM
        | FIRST_SYM
        | GENERAL
        | GENERATED_SYM
        | GET_FORMAT
        | GRANTS
        | GOTO_MARIADB_SYM
        | HASH_SYM
        | HARD_SYM
        | HISTORY_SYM
        | HOSTS_SYM
        | HOUR_SYM
        | ID_SYM
        | IDENTIFIED_SYM
        | IGNORE_SERVER_IDS_SYM
        | INCREMENT_SYM
        | IMMEDIATE_SYM
        | INVOKER_SYM
        | IMPORT
        | INDEXES
        | INITIAL_SIZE_SYM
        | IO_SYM
        | IPC_SYM
        | ISOLATION
        | ISOPEN_SYM
        | ISSUER_SYM
        | INSERT_METHOD
        | INVISIBLE_SYM
        | KEY_BLOCK_SIZE
        | LAST_VALUE
        | LAST_SYM
        | LASTVAL_SYM
        | LEAVES
        | LESS_SYM
        | LEVEL_SYM
        | LIST_SYM
        | LOCKS_SYM
        | LOGFILE_SYM
        | LOGS_SYM
        | MAX_ROWS
        | MASTER_SYM
        | MASTER_HEARTBEAT_PERIOD_SYM
        | MASTER_GTID_POS_SYM
        | MASTER_HOST_SYM
        | MASTER_PORT_SYM
        | MASTER_LOG_FILE_SYM
        | MASTER_LOG_POS_SYM
        | MASTER_USER_SYM
        | MASTER_USE_GTID_SYM
        | MASTER_PASSWORD_SYM
        | MASTER_SERVER_ID_SYM
        | MASTER_CONNECT_RETRY_SYM
        | MASTER_DELAY_SYM
        | MASTER_SSL_SYM
        | MASTER_SSL_CA_SYM
        | MASTER_SSL_CAPATH_SYM
        | MASTER_SSL_CERT_SYM
        | MASTER_SSL_CIPHER_SYM
        | MASTER_SSL_CRL_SYM
        | MASTER_SSL_CRLPATH_SYM
        | MASTER_SSL_KEY_SYM
        | MAX_CONNECTIONS_PER_HOUR
        | MAX_QUERIES_PER_HOUR
        | MAX_SIZE_SYM
        | MAX_STATEMENT_TIME_SYM
        | MAX_UPDATES_PER_HOUR
        | MAX_USER_CONNECTIONS_SYM
        | MEMORY_SYM
        | MERGE_SYM
        | MESSAGE_TEXT_SYM
        | MICROSECOND_SYM
        | MIGRATE_SYM
        | MINUTE_SYM
        | MINVALUE_SYM
        | MIN_ROWS
        | MODIFY_SYM
        | MODE_SYM
        | MONTH_SYM
        | MUTEX_SYM
        | MYSQL_SYM
        | MYSQL_ERRNO_SYM
        | NAME_SYM
        | NEXT_SYM           %prec PREC_BELOW_CONTRACTION_TOKEN2
        | NEXTVAL_SYM
        | NEW_SYM
        | NOCACHE_SYM
        | NOCYCLE_SYM
        | NOMINVALUE_SYM
        | NOMAXVALUE_SYM
        | NO_WAIT_SYM
        | NOWAIT_SYM
        | NODEGROUP_SYM
        | NONE_SYM
        | NOTFOUND_SYM
        | OF_SYM
        | OFFSET_SYM
        | OLD_PASSWORD_SYM
        | ONE_SYM
        | ONLINE_SYM
        | ONLY_SYM
        | PACKAGE_MARIADB_SYM
        | PACK_KEYS_SYM
        | PAGE_SYM
        | PARTIAL
        | PARTITIONING_SYM
        | PARTITIONS_SYM
        | PERSISTENT_SYM
        | PHASE_SYM
        | PLUGIN_SYM
        | PLUGINS_SYM
        | PRESERVE_SYM
        | PREV_SYM
        | PREVIOUS_SYM       %prec PREC_BELOW_CONTRACTION_TOKEN2
        | PRIVILEGES
        | PROCESS
        | PROCESSLIST_SYM
        | PROFILE_SYM
        | PROFILES_SYM
        | PROXY_SYM
        | QUARTER_SYM
        | QUERY_SYM
        | QUICK
        | RAISE_MARIADB_SYM
        | READ_ONLY_SYM
        | REBUILD_SYM
        | RECOVER_SYM
        | REDO_BUFFER_SIZE_SYM
        | REDOFILE_SYM
        | REDUNDANT_SYM
        | RELAY
        | RELAYLOG_SYM
        | RELAY_LOG_FILE_SYM
        | RELAY_LOG_POS_SYM
        | RELAY_THREAD
        | RELOAD
        | REORGANIZE_SYM
        | REPEATABLE_SYM
        | REPLICATION
        | RESOURCES
        | RESTART_SYM
        | RESUME_SYM
        | RETURNED_SQLSTATE_SYM
        | RETURNS_SYM
        | REUSE_SYM
        | REVERSE_SYM
        | ROLLUP_SYM
        | ROUTINE_SYM
        | ROWCOUNT_SYM
        | ROWTYPE_MARIADB_SYM
        | ROW_COUNT_SYM
        | ROW_FORMAT_SYM
        | RTREE_SYM
        | SCHEDULE_SYM
        | SCHEMA_NAME_SYM
        | SECOND_SYM
        | SEQUENCE_SYM
        | SERIALIZABLE_SYM
        | SETVAL_SYM
        | SIMPLE_SYM
        | SHARE_SYM
        | SLAVE_POS_SYM
        | SLOW
        | SNAPSHOT_SYM
        | SOFT_SYM
        | SOUNDS_SYM
        | SOURCE_SYM
        | SQL_CACHE_SYM
        | SQL_BUFFER_RESULT
        | SQL_NO_CACHE_SYM
        | SQL_THREAD
        | STAGE_SYM
        | STARTS_SYM
        | STATEMENT_SYM
        | STATUS_SYM
        | STORAGE_SYM
        | STRING_SYM
        | SUBCLASS_ORIGIN_SYM
        | SUBDATE_SYM
        | SUBJECT_SYM
        | SUBPARTITION_SYM
        | SUBPARTITIONS_SYM
        | SUPER_SYM
        | SUSPEND_SYM
        | SWAPS_SYM
        | SWITCHES_SYM
        | SYSTEM
        | SYSTEM_TIME_SYM
        | TABLE_NAME_SYM
        | TABLES
        | TABLE_CHECKSUM_SYM
        | TABLESPACE
        | TEMPORARY
        | TEMPTABLE_SYM
        | THAN_SYM
        | TRANSACTION_SYM    %prec PREC_BELOW_CONTRACTION_TOKEN2
        | TRANSACTIONAL_SYM
        | TRIGGERS_SYM
        | TRIM_ORACLE
        | TIMESTAMP_ADD
        | TIMESTAMP_DIFF
        | TYPES_SYM
        | TYPE_SYM
        | UDF_RETURNS_SYM
        | FUNCTION_SYM
        | UNCOMMITTED_SYM
        | UNDEFINED_SYM
        | UNDO_BUFFER_SIZE_SYM
        | UNDOFILE_SYM
        | UNKNOWN_SYM
        | UNTIL_SYM
        | USER_SYM           %prec PREC_BELOW_CONTRACTION_TOKEN2
        | USE_FRM
        | VARIABLES
        | VERSIONING_SYM
        | VIEW_SYM
        | VIRTUAL_SYM
        | VALUE_SYM
        | WARNINGS
        | WAIT_SYM
        | WEEK_SYM
        | WEIGHT_STRING_SYM
        | WITHOUT
        | WORK_SYM
        | X509_SYM
        | XML_SYM
        | VIA_SYM
        ;

/*
  SQLCOM_SET_OPTION statement.

  Note that to avoid shift/reduce conflicts, we have separate rules for the
  first option listed in the statement.
*/

set:
          SET
          {
            LEX *lex=Lex;
            if (lex->main_select_push())
              MYSQL_YYABORT;
            lex->set_stmt_init();
            lex->var_list.empty();
            sp_create_assignment_lex(thd, yychar == YYEMPTY);
          }
          start_option_value_list
          {
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
        | SET STATEMENT_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
            Lex->set_stmt_init();
          }
          set_stmt_option_value_following_option_type_list
          {
            LEX *lex= Lex;
            if (unlikely(lex->table_or_sp_used()))
              my_yyabort_error((ER_SUBQUERIES_NOT_SUPPORTED, MYF(0), "SET STATEMENT"));
            lex->stmt_var_list= lex->var_list;
            lex->var_list.empty();
            Lex->pop_select(); //main select
            if (Lex->check_main_unit_semantics())
              MYSQL_YYABORT;
          }
          FOR_SYM verb_clause
	  {}
        ;

set_stmt_option_value_following_option_type_list:
       /*
         Only system variables can be used here. If this condition is changed
         please check careful code under lex->option_type == OPT_STATEMENT
         condition on wrong type casts.
       */
          option_value_following_option_type
        | set_stmt_option_value_following_option_type_list ',' option_value_following_option_type
        ;

/* Start of option value list */
start_option_value_list:
          option_value_no_option_type
          {
            if (unlikely(sp_create_assignment_instr(thd, yychar == YYEMPTY)))
              MYSQL_YYABORT;
          }
          option_value_list_continued
        | TRANSACTION_SYM
          {
            Lex->option_type= OPT_DEFAULT;
          }
          transaction_characteristics
          {
            if (unlikely(sp_create_assignment_instr(thd, yychar == YYEMPTY)))
              MYSQL_YYABORT;
          }
        | option_type
          {
            Lex->option_type= $1;
          }
          start_option_value_list_following_option_type
        ;


/* Start of option value list, option_type was given */
start_option_value_list_following_option_type:
          option_value_following_option_type
          {
            if (unlikely(sp_create_assignment_instr(thd, yychar == YYEMPTY)))
              MYSQL_YYABORT;
          }
          option_value_list_continued
        | TRANSACTION_SYM transaction_characteristics
          {
            if (unlikely(sp_create_assignment_instr(thd, yychar == YYEMPTY)))
              MYSQL_YYABORT;
          }
        ;

/* Remainder of the option value list after first option value. */
option_value_list_continued:
          /* empty */
        | ',' option_value_list
        ;

/* Repeating list of option values after first option value. */
option_value_list:
          {
            sp_create_assignment_lex(thd, yychar == YYEMPTY);
          }
          option_value
          {
            if (unlikely(sp_create_assignment_instr(thd, yychar == YYEMPTY)))
              MYSQL_YYABORT;
          }
        | option_value_list ','
          {
            sp_create_assignment_lex(thd, yychar == YYEMPTY);
          }
          option_value
          {
            if (unlikely(sp_create_assignment_instr(thd, yychar == YYEMPTY)))
              MYSQL_YYABORT;
          }
        ;

/* Wrapper around option values following the first option value in the stmt. */
option_value:
          option_type
          {
            Lex->option_type= $1;
          }
          option_value_following_option_type
        | option_value_no_option_type
        ;

option_type:
          GLOBAL_SYM  { $$=OPT_GLOBAL; }
        | LOCAL_SYM   { $$=OPT_SESSION; }
        | SESSION_SYM { $$=OPT_SESSION; }
        ;

opt_var_type:
          /* empty */ { $$=OPT_SESSION; }
        | GLOBAL_SYM  { $$=OPT_GLOBAL; }
        | LOCAL_SYM   { $$=OPT_SESSION; }
        | SESSION_SYM { $$=OPT_SESSION; }
        ;

opt_var_ident_type:
          /* empty */     { $$=OPT_DEFAULT; }
        | GLOBAL_SYM '.'  { $$=OPT_GLOBAL; }
        | LOCAL_SYM '.'   { $$=OPT_SESSION; }
        | SESSION_SYM '.' { $$=OPT_SESSION; }
        ;

/* Option values with preceding option_type. */
option_value_following_option_type:
          ident equal set_expr_or_default
          {
            if (unlikely(Lex->set_system_variable(Lex->option_type, &$1, $3)))
              MYSQL_YYABORT;
          }
        | ident '.' ident equal set_expr_or_default
          {
            if (unlikely(Lex->set_system_variable(thd, Lex->option_type, &$1, &$3, $5)))
              MYSQL_YYABORT;
          }
        | DEFAULT '.' ident equal set_expr_or_default
          {
            if (unlikely(Lex->set_default_system_variable(Lex->option_type, &$3, $5)))
              MYSQL_YYABORT;
          }
        ;

/* Option values without preceding option_type. */
option_value_no_option_type:
          ident_set_usual_case equal set_expr_or_default
          {
            if (unlikely(Lex->set_variable(&$1, $3)))
              MYSQL_YYABORT;
          }
        | ident '.' ident equal set_expr_or_default
          {
            if (unlikely(Lex->set_variable(&$1, &$3, $5)))
              MYSQL_YYABORT;
          }
        | DEFAULT '.' ident equal set_expr_or_default
          {
            if (unlikely(Lex->set_default_system_variable(Lex->option_type, &$3, $5)))
              MYSQL_YYABORT;
          }
        | '@' ident_or_text equal expr
          {
            if (unlikely(Lex->set_user_variable(thd, &$2, $4)))
              MYSQL_YYABORT;
          }
        | '@' '@' opt_var_ident_type ident_sysvar_name equal set_expr_or_default
          {
            if (unlikely(Lex->set_system_variable($3, &$4, $6)))
              MYSQL_YYABORT;
          }
        | '@' '@' opt_var_ident_type ident_sysvar_name '.' ident equal set_expr_or_default
          {
            if (unlikely(Lex->set_system_variable(thd, $3, &$4, &$6, $8)))
              MYSQL_YYABORT;
          }
        | '@' '@' opt_var_ident_type DEFAULT '.' ident equal set_expr_or_default
          {
            if (unlikely(Lex->set_default_system_variable($3, &$6, $8)))
              MYSQL_YYABORT;
          }
        | charset old_or_new_charset_name_or_default
          {
            LEX *lex= thd->lex;
            CHARSET_INFO *cs2;
            cs2= $2 ? $2: global_system_variables.character_set_client;
            set_var_collation_client *var;
            var= (new (thd->mem_root)
                  set_var_collation_client(cs2,
                                           thd->variables.collation_database,
                                            cs2));
            if (unlikely(var == NULL))
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
          }
        | NAMES_SYM equal expr
          {
            LEX *lex= Lex;
            sp_pcontext *spc= lex->spcont;
            LEX_CSTRING names= { STRING_WITH_LEN("names") };
            if (unlikely(spc && spc->find_variable(&names, false)))
              my_error(ER_SP_BAD_VAR_SHADOW, MYF(0), names.str);
            else
              thd->parse_error();
            MYSQL_YYABORT;
          }
        | NAMES_SYM charset_name_or_default opt_collate
          {
            LEX *lex= Lex;
            CHARSET_INFO *cs2;
            CHARSET_INFO *cs3;
            cs2= $2 ? $2 : global_system_variables.character_set_client;
            cs3= $3 ? $3 : cs2;
            if (unlikely(!my_charset_same(cs2, cs3)))
            {
              my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0),
                       cs3->name, cs2->csname);
              MYSQL_YYABORT;
            }
            set_var_collation_client *var;
            var= new (thd->mem_root) set_var_collation_client(cs3, cs3, cs3);
            if (unlikely(var == NULL) ||
                unlikely(lex->var_list.push_back(var, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | DEFAULT ROLE_SYM grant_role
          {
            LEX *lex = Lex;
            LEX_USER *user;
            if (unlikely(!(user=(LEX_USER *) thd->calloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            user->user= current_user;
            set_var_default_role *var= (new (thd->mem_root)
                                        set_var_default_role(user,
                                                             $3->user));
            if (unlikely(var == NULL) ||
                unlikely(lex->var_list.push_back(var, thd->mem_root)))
              MYSQL_YYABORT;

            thd->lex->autocommit= TRUE;
            if (lex->sphead)
              lex->sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;
          }
        | DEFAULT ROLE_SYM grant_role FOR_SYM user
          {
            LEX *lex = Lex;
            set_var_default_role *var= (new (thd->mem_root)
                                        set_var_default_role($5, $3->user));
            if (unlikely(var == NULL) ||
                unlikely(lex->var_list.push_back(var, thd->mem_root)))
              MYSQL_YYABORT;
            thd->lex->autocommit= TRUE;
            if (lex->sphead)
              lex->sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;
          }
        | ROLE_SYM ident_or_text
          {
            LEX *lex = Lex;
            set_var_role *var= new (thd->mem_root) set_var_role($2);
            if (unlikely(var == NULL) ||
                unlikely(lex->var_list.push_back(var, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | ROLE_SYM equal set_expr_or_default
          {
            if (unlikely(Lex->set_variable(&$1, $3)))
              MYSQL_YYABORT;
          }
        | PASSWORD_SYM opt_for_user text_or_password
          {
            LEX *lex = Lex;
            set_var_password *var= (new (thd->mem_root)
                                    set_var_password(lex->definer));
            if (unlikely(var == NULL) ||
                unlikely(lex->var_list.push_back(var, thd->mem_root)))
              MYSQL_YYABORT;
            lex->autocommit= TRUE;
            if (lex->sphead)
              lex->sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;
          }
        ;


transaction_characteristics:
          transaction_access_mode
        | isolation_level
        | transaction_access_mode ',' isolation_level
        | isolation_level ',' transaction_access_mode
        ;

transaction_access_mode:
          transaction_access_mode_types
          {
            LEX *lex=Lex;
            Item *item= new (thd->mem_root) Item_int(thd, (int32) $1);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            set_var *var= (new (thd->mem_root)
                           set_var(thd, lex->option_type,
                                   find_sys_var(thd, "tx_read_only"),
                                   &null_clex_str,
                                   item));
            if (unlikely(var == NULL))
              MYSQL_YYABORT;
            if (unlikely(lex->var_list.push_back(var, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

isolation_level:
          ISOLATION LEVEL_SYM isolation_types
          {
            LEX *lex=Lex;
            Item *item= new (thd->mem_root) Item_int(thd, (int32) $3);
            if (unlikely(item == NULL))
              MYSQL_YYABORT;
            set_var *var= (new (thd->mem_root)
                           set_var(thd, lex->option_type,
                                   find_sys_var(thd, "tx_isolation"),
                                   &null_clex_str,
                                   item));
            if (unlikely(var == NULL) ||
                unlikely(lex->var_list.push_back(var, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

transaction_access_mode_types:
          READ_SYM ONLY_SYM { $$= true; }
        | READ_SYM WRITE_SYM { $$= false; }
        ;

isolation_types:
          READ_SYM UNCOMMITTED_SYM { $$= ISO_READ_UNCOMMITTED; }
        | READ_SYM COMMITTED_SYM   { $$= ISO_READ_COMMITTED; }
        | REPEATABLE_SYM READ_SYM  { $$= ISO_REPEATABLE_READ; }
        | SERIALIZABLE_SYM         { $$= ISO_SERIALIZABLE; }
        ;

opt_for_user:
        equal
          {
            LEX *lex= thd->lex;
            sp_pcontext *spc= lex->spcont;
            LEX_CSTRING pw= { STRING_WITH_LEN("password") };

            if (unlikely(spc && spc->find_variable(&pw, false)))
              my_yyabort_error((ER_SP_BAD_VAR_SHADOW, MYF(0), pw.str));
            if (unlikely(!(lex->definer= (LEX_USER*)
                           thd->calloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            lex->definer->user= current_user;
            lex->definer->plugin= empty_clex_str;
            lex->definer->auth=   empty_clex_str;
          }
        | FOR_SYM user equal { Lex->definer= $2; }
        ;

text_or_password:
          TEXT_STRING { Lex->definer->auth= $1;}
        | PASSWORD_SYM '(' TEXT_STRING ')' { Lex->definer->pwtext= $3; }
        | OLD_PASSWORD_SYM '(' TEXT_STRING ')'
          {
            Lex->definer->pwtext= $3;
            Lex->definer->auth.str= Item_func_password::alloc(thd,
                                   $3.str, $3.length, Item_func_password::OLD);
            Lex->definer->auth.length=  SCRAMBLED_PASSWORD_CHAR_LENGTH_323;
          }
        ;

set_expr_or_default:
          expr { $$=$1; }
        | DEFAULT { $$=0; }
        | ON
          {
            $$=new (thd->mem_root) Item_string_sys(thd, "ON",  2);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | ALL
          {
            $$=new (thd->mem_root) Item_string_sys(thd, "ALL", 3);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        | BINARY
          {
            $$=new (thd->mem_root) Item_string_sys(thd, "binary", 6);
            if (unlikely($$ == NULL))
              MYSQL_YYABORT;
          }
        ;

/* Lock function */

lock:
          LOCK_SYM table_or_tables
          {
            LEX *lex= Lex;

            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "LOCK"));
            lex->sql_command= SQLCOM_LOCK_TABLES;
          }
          table_lock_list opt_lock_wait_timeout
          {}
        ;

opt_lock_wait_timeout:
        /* empty */
        {}
        | WAIT_SYM ulong_num
        {
          if (unlikely(set_statement_var_if_exists(thd, STRING_WITH_LEN("lock_wait_timeout"), $2)) ||
              unlikely(set_statement_var_if_exists(thd, STRING_WITH_LEN("innodb_lock_wait_timeout"), $2)))
            MYSQL_YYABORT;
        }
        | NOWAIT_SYM
        {
          if (unlikely(set_statement_var_if_exists(thd, STRING_WITH_LEN("lock_wait_timeout"), 0)) ||
              unlikely(set_statement_var_if_exists(thd, STRING_WITH_LEN("innodb_lock_wait_timeout"), 0)))
            MYSQL_YYABORT;
        }
      ;

table_or_tables:
          TABLE_SYM        { }
        | TABLES           { }
        ;

table_lock_list:
          table_lock
        | table_lock_list ',' table_lock
        ;

table_lock:
          table_ident opt_table_alias_clause lock_option
          {
            thr_lock_type lock_type= (thr_lock_type) $3;
            bool lock_for_write= (lock_type >= TL_WRITE_ALLOW_WRITE);
            if (unlikely(!Select->
                         add_table_to_list(thd, $1, $2, 0, lock_type,
                                           (lock_for_write ?
                                            lock_type == TL_WRITE_CONCURRENT_INSERT ?
                                            MDL_SHARED_WRITE :
                                            MDL_SHARED_NO_READ_WRITE :
                                            MDL_SHARED_READ))))
              MYSQL_YYABORT;
          }
        ;

lock_option:
          READ_SYM               { $$= TL_READ_NO_INSERT; }
        | WRITE_SYM              { $$= TL_WRITE_DEFAULT; }
        | WRITE_SYM CONCURRENT
          {
            $$= (Lex->sphead ? TL_WRITE_DEFAULT : TL_WRITE_CONCURRENT_INSERT);
          }

        | LOW_PRIORITY WRITE_SYM { $$= TL_WRITE_LOW_PRIORITY; }
        | READ_SYM LOCAL_SYM     { $$= TL_READ; }
        ;

unlock:
          UNLOCK_SYM
          {
            LEX *lex= Lex;

            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "UNLOCK"));
            lex->sql_command= SQLCOM_UNLOCK_TABLES;
          }
          table_or_tables
          {}
        ;

/*
** Handler: direct access to ISAM functions
*/

handler:
          HANDLER_SYM
          {
            if (Lex->main_select_push())
              MYSQL_YYABORT;
          }
          handler_tail
          {
            Lex->pop_select(); //main select
          }
        ;

handler_tail:
          table_ident OPEN_SYM opt_table_alias_clause
          {
            LEX *lex= Lex;
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "HANDLER"));
            lex->sql_command = SQLCOM_HA_OPEN;
            if (!lex->current_select->add_table_to_list(thd, $1, $3, 0))
              MYSQL_YYABORT;
          }
        | table_ident_nodb CLOSE_SYM
          {
            LEX *lex= Lex;
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "HANDLER"));
            lex->sql_command = SQLCOM_HA_CLOSE;
            if (!lex->current_select->add_table_to_list(thd, $1, 0, 0))
              MYSQL_YYABORT;
          }
        | table_ident_nodb READ_SYM
          {
            LEX *lex=Lex;
            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "HANDLER"));
            lex->expr_allows_subselect= FALSE;
            lex->sql_command = SQLCOM_HA_READ;
            lex->ha_rkey_mode= HA_READ_KEY_EXACT; /* Avoid purify warnings */
            Item *one= new (thd->mem_root) Item_int(thd, (int32) 1);
            if (unlikely(one == NULL))
              MYSQL_YYABORT;
            lex->current_select->select_limit= one;
            lex->current_select->offset_limit= 0;
            lex->limit_rows_examined= 0;
            if (!lex->current_select->add_table_to_list(thd, $1, 0, 0))
              MYSQL_YYABORT;
          }
          handler_read_or_scan opt_where_clause opt_global_limit_clause
          {
            LEX *lex=Lex;
            lex->expr_allows_subselect= TRUE;
            if (!lex->current_select->explicit_limit)
            {
              Item *one= new (thd->mem_root) Item_int(thd, (int32) 1);
              if (one == NULL)
                MYSQL_YYABORT;
              lex->current_select->select_limit= one;
              lex->current_select->offset_limit= 0;
              lex->limit_rows_examined= 0;
            }
            /* Stored functions are not supported for HANDLER READ. */
            if (lex->uses_stored_routines())
            {
              my_error(ER_NOT_SUPPORTED_YET, MYF(0),
                       "stored functions in HANDLER ... READ");
              MYSQL_YYABORT;
            }
          }
        ;

handler_read_or_scan:
          handler_scan_function       { Lex->ident= null_clex_str; }
        | ident handler_rkey_function { Lex->ident= $1; }
        ;

handler_scan_function:
          FIRST_SYM { Lex->ha_read_mode = RFIRST; }
        | NEXT_SYM  { Lex->ha_read_mode = RNEXT;  }
        ;

handler_rkey_function:
          FIRST_SYM { Lex->ha_read_mode = RFIRST; }
        | NEXT_SYM  { Lex->ha_read_mode = RNEXT;  }
        | PREV_SYM  { Lex->ha_read_mode = RPREV;  }
        | LAST_SYM  { Lex->ha_read_mode = RLAST;  }
        | handler_rkey_mode
          {
            LEX *lex=Lex;
            lex->ha_read_mode = RKEY;
            lex->ha_rkey_mode=$1;
            if (unlikely(!(lex->insert_list= new (thd->mem_root) List_item)))
              MYSQL_YYABORT;
          }
          '(' values ')'
          {}
        ;

handler_rkey_mode:
          '='     { $$=HA_READ_KEY_EXACT;   }
        | GE     { $$=HA_READ_KEY_OR_NEXT; }
        | LE     { $$=HA_READ_KEY_OR_PREV; }
        | '>' { $$=HA_READ_AFTER_KEY;   }
        | '<'     { $$=HA_READ_BEFORE_KEY;  }
        ;

/* GRANT / REVOKE */

revoke:
          REVOKE clear_privileges revoke_command
          {}
        ;

revoke_command:
          grant_privileges ON opt_table grant_ident FROM user_and_role_list
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_REVOKE;
            lex->type= 0;
          }
        | grant_privileges ON FUNCTION_SYM grant_ident FROM user_and_role_list
          {
            if (unlikely(Lex->add_grant_command(thd, SQLCOM_REVOKE,
                                                TYPE_ENUM_FUNCTION)))
              MYSQL_YYABORT;
          }
        | grant_privileges ON PROCEDURE_SYM grant_ident FROM user_and_role_list
          {
            if (unlikely(Lex->add_grant_command(thd, SQLCOM_REVOKE,
                         TYPE_ENUM_PROCEDURE)))
              MYSQL_YYABORT;
          }
        | grant_privileges ON PACKAGE_ORACLE_SYM grant_ident
          FROM user_and_role_list
          {
            if (unlikely(Lex->add_grant_command(thd, SQLCOM_REVOKE,
                                                TYPE_ENUM_PACKAGE)))
              MYSQL_YYABORT;
          }
        | grant_privileges ON PACKAGE_ORACLE_SYM BODY_ORACLE_SYM grant_ident
          FROM user_and_role_list
          {
            if (unlikely(Lex->add_grant_command(thd, SQLCOM_REVOKE,
                                                TYPE_ENUM_PACKAGE_BODY)))
              MYSQL_YYABORT;
          }
        | ALL opt_privileges ',' GRANT OPTION FROM user_and_role_list
          {
            Lex->sql_command = SQLCOM_REVOKE_ALL;
          }
        | PROXY_SYM ON user FROM user_list
          {
            LEX *lex= Lex;
            lex->users_list.push_front ($3);
            lex->sql_command= SQLCOM_REVOKE;
            lex->type= TYPE_ENUM_PROXY;
          }
        | admin_option_for_role FROM user_and_role_list
          {
            Lex->sql_command= SQLCOM_REVOKE_ROLE;
            if (unlikely(Lex->users_list.push_front($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

admin_option_for_role:
        ADMIN_SYM OPTION FOR_SYM grant_role
        { Lex->with_admin_option= true; $$= $4; }
      | grant_role
        { Lex->with_admin_option= false; $$= $1; }
      ;

grant:
          GRANT clear_privileges grant_command
          {}
        ;

grant_command:
          grant_privileges ON opt_table grant_ident TO_SYM grant_list
          opt_require_clause opt_grant_options
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_GRANT;
            lex->type= 0;
          }
        | grant_privileges ON FUNCTION_SYM grant_ident TO_SYM grant_list
          opt_require_clause opt_grant_options
          {
            if (unlikely(Lex->add_grant_command(thd, SQLCOM_GRANT,
                                                TYPE_ENUM_FUNCTION)))
              MYSQL_YYABORT;
          }
        | grant_privileges ON PROCEDURE_SYM grant_ident TO_SYM grant_list
          opt_require_clause opt_grant_options
          {
            if (unlikely(Lex->add_grant_command(thd, SQLCOM_GRANT,
                                                TYPE_ENUM_PROCEDURE)))
              MYSQL_YYABORT;
          }
        | grant_privileges ON PACKAGE_ORACLE_SYM grant_ident TO_SYM grant_list
          opt_require_clause opt_grant_options
          {
            if (unlikely(Lex->add_grant_command(thd, SQLCOM_GRANT,
                                                TYPE_ENUM_PACKAGE)))
              MYSQL_YYABORT;
          }
        | grant_privileges ON PACKAGE_ORACLE_SYM BODY_ORACLE_SYM grant_ident TO_SYM grant_list
          opt_require_clause opt_grant_options
          {
            if (unlikely(Lex->add_grant_command(thd, SQLCOM_GRANT,
                                                TYPE_ENUM_PACKAGE_BODY)))
              MYSQL_YYABORT;
          }
        | PROXY_SYM ON user TO_SYM grant_list opt_grant_option
          {
            LEX *lex= Lex;
            lex->users_list.push_front ($3);
            lex->sql_command= SQLCOM_GRANT;
            lex->type= TYPE_ENUM_PROXY;
          }
        | grant_role TO_SYM grant_list opt_with_admin_option
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_GRANT_ROLE;
            /* The first role is the one that is granted */
            if (unlikely(Lex->users_list.push_front($1, thd->mem_root)))
              MYSQL_YYABORT;
          }

        ;

opt_with_admin:
          /* nothing */               { Lex->definer = 0; }
        | WITH ADMIN_SYM user_or_role { Lex->definer = $3; }
        ;

opt_with_admin_option:
          /* nothing */               { Lex->with_admin_option= false; }
        | WITH ADMIN_SYM OPTION       { Lex->with_admin_option= true; }
        ;

role_list:
          grant_role
          {
            if (unlikely(Lex->users_list.push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | role_list ',' grant_role
          {
            if (unlikely(Lex->users_list.push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

current_role:
          CURRENT_ROLE optional_braces
          {
            if (unlikely(!($$=(LEX_USER*) thd->calloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            $$->user= current_role;
            $$->reset_auth();
          }
          ;

grant_role:
          ident_or_text
          {
            CHARSET_INFO *cs= system_charset_info;
            /* trim end spaces (as they'll be lost in mysql.user anyway) */
            $1.length= cs->cset->lengthsp(cs, $1.str, $1.length);
            ((char*) $1.str)[$1.length] = '\0';
            if (unlikely($1.length == 0))
              my_yyabort_error((ER_INVALID_ROLE, MYF(0), ""));
            if (unlikely(!($$=(LEX_USER*) thd->alloc(sizeof(LEX_USER)))))
              MYSQL_YYABORT;
            $$->user= $1;
            $$->host= empty_clex_str;
            $$->reset_auth();

            if (unlikely(check_string_char_length(&$$->user, ER_USERNAME,
                                                  username_char_length,
                                                  cs, 0)))
              MYSQL_YYABORT;
          }
        | current_role
        ;

opt_table:
          /* Empty */
        | TABLE_SYM
        ;

grant_privileges:
          object_privilege_list {}
        | ALL opt_privileges
          { 
            Lex->all_privileges= 1; 
            Lex->grant= GLOBAL_ACLS;
          }
        ;

opt_privileges:
          /* empty */
        | PRIVILEGES
        ;

object_privilege_list:
          object_privilege
        | object_privilege_list ',' object_privilege
        ;

object_privilege:
          SELECT_SYM
          { Lex->which_columns = SELECT_ACL;}
          opt_column_list {}
        | INSERT
          { Lex->which_columns = INSERT_ACL;}
          opt_column_list {}
        | UPDATE_SYM
          { Lex->which_columns = UPDATE_ACL; }
          opt_column_list {}
        | REFERENCES
          { Lex->which_columns = REFERENCES_ACL;}
          opt_column_list {}
        | DELETE_SYM              { Lex->grant |= DELETE_ACL;}
        | USAGE                   {}
        | INDEX_SYM               { Lex->grant |= INDEX_ACL;}
        | ALTER                   { Lex->grant |= ALTER_ACL;}
        | CREATE                  { Lex->grant |= CREATE_ACL;}
        | DROP                    { Lex->grant |= DROP_ACL;}
        | EXECUTE_SYM             { Lex->grant |= EXECUTE_ACL;}
        | RELOAD                  { Lex->grant |= RELOAD_ACL;}
        | SHUTDOWN                { Lex->grant |= SHUTDOWN_ACL;}
        | PROCESS                 { Lex->grant |= PROCESS_ACL;}
        | FILE_SYM                { Lex->grant |= FILE_ACL;}
        | GRANT OPTION            { Lex->grant |= GRANT_ACL;}
        | SHOW DATABASES          { Lex->grant |= SHOW_DB_ACL;}
        | SUPER_SYM               { Lex->grant |= SUPER_ACL;}
        | CREATE TEMPORARY TABLES { Lex->grant |= CREATE_TMP_ACL;}
        | LOCK_SYM TABLES         { Lex->grant |= LOCK_TABLES_ACL; }
        | REPLICATION SLAVE       { Lex->grant |= REPL_SLAVE_ACL; }
        | REPLICATION CLIENT_SYM  { Lex->grant |= REPL_CLIENT_ACL; }
        | CREATE VIEW_SYM         { Lex->grant |= CREATE_VIEW_ACL; }
        | SHOW VIEW_SYM           { Lex->grant |= SHOW_VIEW_ACL; }
        | CREATE ROUTINE_SYM      { Lex->grant |= CREATE_PROC_ACL; }
        | ALTER ROUTINE_SYM       { Lex->grant |= ALTER_PROC_ACL; }
        | CREATE USER_SYM         { Lex->grant |= CREATE_USER_ACL; }
        | EVENT_SYM               { Lex->grant |= EVENT_ACL;}
        | TRIGGER_SYM             { Lex->grant |= TRIGGER_ACL; }
        | CREATE TABLESPACE       { Lex->grant |= CREATE_TABLESPACE_ACL; }
        | DELETE_SYM HISTORY_SYM  { Lex->grant |= DELETE_HISTORY_ACL; }
        ;

opt_and:
          /* empty */ {}
        | AND_SYM {}
        ;

require_list:
          require_list_element opt_and require_list
        | require_list_element
        ;

require_list_element:
          SUBJECT_SYM TEXT_STRING
          {
            LEX *lex=Lex;
            if (unlikely(lex->x509_subject))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "SUBJECT"));
            lex->x509_subject=$2.str;
          }
        | ISSUER_SYM TEXT_STRING
          {
            LEX *lex=Lex;
            if (unlikely(lex->x509_issuer))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "ISSUER"));
            lex->x509_issuer=$2.str;
          }
        | CIPHER_SYM TEXT_STRING
          {
            LEX *lex=Lex;
            if (unlikely(lex->ssl_cipher))
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "CIPHER"));
            lex->ssl_cipher=$2.str;
          }
        ;

grant_ident:
          '*'
          {
            LEX *lex= Lex;
            if (unlikely(lex->copy_db_to(&lex->current_select->db)))
              MYSQL_YYABORT;
            if (lex->grant == GLOBAL_ACLS)
              lex->grant = DB_ACLS & ~GRANT_ACL;
            else if (unlikely(lex->columns.elements))
              my_yyabort_error((ER_ILLEGAL_GRANT_FOR_TABLE, MYF(0)));
          }
        | ident '.' '*'
          {
            LEX *lex= Lex;
            lex->current_select->db= $1;
            if (lex->grant == GLOBAL_ACLS)
              lex->grant = DB_ACLS & ~GRANT_ACL;
            else if (unlikely(lex->columns.elements))
              my_yyabort_error((ER_ILLEGAL_GRANT_FOR_TABLE, MYF(0)));
          }
        | '*' '.' '*'
          {
            LEX *lex= Lex;
            lex->current_select->db= null_clex_str;
            if (lex->grant == GLOBAL_ACLS)
              lex->grant= GLOBAL_ACLS & ~GRANT_ACL;
            else if (unlikely(lex->columns.elements))
              my_yyabort_error((ER_ILLEGAL_GRANT_FOR_TABLE, MYF(0)));
          }
        | table_ident
          {
            LEX *lex=Lex;
            if (unlikely(!lex->current_select->
                         add_table_to_list(thd, $1,NULL,
                                           TL_OPTION_UPDATING)))
              MYSQL_YYABORT;
            if (lex->grant == GLOBAL_ACLS)
              lex->grant =  TABLE_ACLS & ~GRANT_ACL;
          }
        ;

user_list:
          user
          {
            if (unlikely(Lex->users_list.push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | user_list ',' user
          {
            if (unlikely(Lex->users_list.push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

grant_list:
          grant_user
          {
            if (unlikely(Lex->users_list.push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | grant_list ',' grant_user
          {
            if (unlikely(Lex->users_list.push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

user_and_role_list:
          user_or_role
          {
            if (unlikely(Lex->users_list.push_back($1, thd->mem_root)))
              MYSQL_YYABORT;
          }
        | user_and_role_list ',' user_or_role
          {
            if (unlikely(Lex->users_list.push_back($3, thd->mem_root)))
              MYSQL_YYABORT;
          }
        ;

via_or_with: VIA_SYM | WITH ;
using_or_as: USING | AS ;

grant_user:
          user IDENTIFIED_SYM BY TEXT_STRING
          {
            $$= $1;
            $1->pwtext= $4;
          }
        | user IDENTIFIED_SYM BY PASSWORD_SYM TEXT_STRING
          { 
            $$= $1; 
            $1->auth= $5;
          }
        | user IDENTIFIED_SYM via_or_with ident_or_text
          {
            $$= $1;
            $1->plugin= $4;
            $1->auth= empty_clex_str;
          }
        | user IDENTIFIED_SYM via_or_with ident_or_text using_or_as
          TEXT_STRING_sys
          {
            $$= $1;
            $1->plugin= $4;
            $1->auth= $6;
          }
        | user IDENTIFIED_SYM via_or_with ident_or_text using_or_as
          PASSWORD_SYM '(' TEXT_STRING ')'
          {
            $$= $1;
            $1->plugin= $4;
            $1->pwtext= $8;
          }
        | user_or_role
          { $$= $1; }
        ;

opt_column_list:
          /* empty */
          {
            LEX *lex=Lex;
            lex->grant |= lex->which_columns;
          }
        | '(' column_list ')'
        ;

column_list:
          column_list ',' column_list_id
        | column_list_id
        ;

column_list_id:
          ident
          {
            String *new_str= new (thd->mem_root) String((const char*) $1.str,$1.length,system_charset_info);
            if (unlikely(new_str == NULL))
              MYSQL_YYABORT;
            List_iterator <LEX_COLUMN> iter(Lex->columns);
            class LEX_COLUMN *point;
            LEX *lex=Lex;
            while ((point=iter++))
            {
              if (!my_strcasecmp(system_charset_info,
                                 point->column.c_ptr(), new_str->c_ptr()))
                break;
            }
            lex->grant_tot_col|= lex->which_columns;
            if (point)
              point->rights |= lex->which_columns;
            else
            {
              LEX_COLUMN *col= (new (thd->mem_root)
                                LEX_COLUMN(*new_str,lex->which_columns));
              if (unlikely(col == NULL))
                MYSQL_YYABORT;
              lex->columns.push_back(col, thd->mem_root);
            }
          }
        ;

opt_require_clause:
          /* empty */
        | REQUIRE_SYM require_list
          {
            Lex->ssl_type=SSL_TYPE_SPECIFIED;
          }
        | REQUIRE_SYM SSL_SYM
          {
            Lex->ssl_type=SSL_TYPE_ANY;
          }
        | REQUIRE_SYM X509_SYM
          {
            Lex->ssl_type=SSL_TYPE_X509;
          }
        | REQUIRE_SYM NONE_SYM
          {
            Lex->ssl_type=SSL_TYPE_NONE;
          }
        ;

resource_option:
        MAX_QUERIES_PER_HOUR ulong_num
          {
            LEX *lex=Lex;
            lex->mqh.questions=$2;
            lex->mqh.specified_limits|= USER_RESOURCES::QUERIES_PER_HOUR;
          }
        | MAX_UPDATES_PER_HOUR ulong_num
          {
            LEX *lex=Lex;
            lex->mqh.updates=$2;
            lex->mqh.specified_limits|= USER_RESOURCES::UPDATES_PER_HOUR;
          }
        | MAX_CONNECTIONS_PER_HOUR ulong_num
          {
            LEX *lex=Lex;
            lex->mqh.conn_per_hour= $2;
            lex->mqh.specified_limits|= USER_RESOURCES::CONNECTIONS_PER_HOUR;
          }
        | MAX_USER_CONNECTIONS_SYM int_num
          {
            LEX *lex=Lex;
            lex->mqh.user_conn= $2;
            lex->mqh.specified_limits|= USER_RESOURCES::USER_CONNECTIONS;
          }
        | MAX_STATEMENT_TIME_SYM NUM_literal
          {
            LEX *lex=Lex;
            lex->mqh.max_statement_time= $2->val_real();
            lex->mqh.specified_limits|= USER_RESOURCES::MAX_STATEMENT_TIME;
          }
        ;

resource_option_list:
	  resource_option_list resource_option {}
	| resource_option {}
        ;

opt_resource_options:
	  /* empty */ {}
	| WITH resource_option_list
        ;


opt_grant_options:
          /* empty */ {}
        | WITH grant_option_list {}
        ;

opt_grant_option:
          /* empty */ {}
        | WITH GRANT OPTION { Lex->grant |= GRANT_ACL;}
        ;

grant_option_list:
          grant_option_list grant_option {}
        | grant_option {}
        ;

grant_option:
          GRANT OPTION { Lex->grant |= GRANT_ACL;}
	| resource_option {}
        ;

begin_stmt_mariadb:
          BEGIN_MARIADB_SYM
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_BEGIN;
            lex->start_transaction_opt= 0;
          }
          opt_work {}
          ;

compound_statement:
          sp_proc_stmt_compound_ok
          {
            Lex->sql_command= SQLCOM_COMPOUND;
            Lex->sphead->set_stmt_end(thd);
            Lex->sphead->restore_thd_mem_root(thd);
          }
        ;

opt_not:
        /* nothing */  { $$= 0; }
        | not          { $$= 1; }
        ;

opt_work:
          /* empty */ {}
        | WORK_SYM  {}
        ;

opt_chain:
          /* empty */
          { $$= TVL_UNKNOWN; }
        | AND_SYM NO_SYM CHAIN_SYM { $$= TVL_NO; }
        | AND_SYM CHAIN_SYM        { $$= TVL_YES; }
        ;

opt_release:
          /* empty */
          { $$= TVL_UNKNOWN; }
        | RELEASE_SYM        { $$= TVL_YES; }
        | NO_SYM RELEASE_SYM { $$= TVL_NO; }
;

commit:
          COMMIT_SYM opt_work opt_chain opt_release
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_COMMIT;
            /* Don't allow AND CHAIN RELEASE. */
            MYSQL_YYABORT_UNLESS($3 != TVL_YES || $4 != TVL_YES);
            lex->tx_chain= $3;
            lex->tx_release= $4;
          }
        ;

rollback:
          ROLLBACK_SYM opt_work opt_chain opt_release
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_ROLLBACK;
            /* Don't allow AND CHAIN RELEASE. */
            MYSQL_YYABORT_UNLESS($3 != TVL_YES || $4 != TVL_YES);
            lex->tx_chain= $3;
            lex->tx_release= $4;
          }
        | ROLLBACK_SYM opt_work TO_SYM SAVEPOINT_SYM ident
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_ROLLBACK_TO_SAVEPOINT;
            lex->ident= $5;
          }
        | ROLLBACK_SYM opt_work TO_SYM ident
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_ROLLBACK_TO_SAVEPOINT;
            lex->ident= $4;
          }
        ;

savepoint:
          SAVEPOINT_SYM ident
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_SAVEPOINT;
            lex->ident= $2;
          }
        ;

release:
          RELEASE_SYM SAVEPOINT_SYM ident
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_RELEASE_SAVEPOINT;
            lex->ident= $3;
          }
        ;

/*
   UNIONS : glue selects together
*/

unit_type_decl:
          UNION_SYM union_option
          { $$.unit_type= UNION_TYPE; $$.distinct= $2; }
        | INTERSECT_SYM
          { $$.unit_type= INTERSECT_TYPE; $$.distinct= 1; }
        | EXCEPT_SYM
          { $$.unit_type= EXCEPT_TYPE; $$.distinct= 1; }
        ;

/*
  Start a UNION, for non-top level query expressions.
*/
union_option:
          /* empty */ { $$=1; }
        | DISTINCT  { $$=1; }
        | ALL       { $$=0; }
        ;

query_expression_option:
          STRAIGHT_JOIN { Select->options|= SELECT_STRAIGHT_JOIN; }
        | HIGH_PRIORITY
          {
            YYPS->m_lock_type= TL_READ_HIGH_PRIORITY;
            YYPS->m_mdl_type= MDL_SHARED_READ;
            Select->options|= SELECT_HIGH_PRIORITY;
          }
        | DISTINCT         { Select->options|= SELECT_DISTINCT; }
        | SQL_SMALL_RESULT { Select->options|= SELECT_SMALL_RESULT; }
        | SQL_BIG_RESULT   { Select->options|= SELECT_BIG_RESULT; }
        | SQL_BUFFER_RESULT { Select->options|= OPTION_BUFFER_RESULT; }
        | SQL_CALC_FOUND_ROWS { Select->options|= OPTION_FOUND_ROWS; }
        | ALL { Select->options|= SELECT_ALL; }
        ;

/**************************************************************************

 DEFINER clause support.

**************************************************************************/

definer_opt:
          no_definer
        | definer
        ;

no_definer:
          /* empty */
          {
            /*
              We have to distinguish missing DEFINER-clause from case when
              CURRENT_USER specified as definer explicitly in order to properly
              handle CREATE TRIGGER statements which come to replication thread
              from older master servers (i.e. to create non-suid trigger in this
              case).
            */
            thd->lex->definer= 0;
          }
        ;

definer:
          DEFINER_SYM '=' user_or_role
          {
            Lex->definer= $3;
            Lex->ssl_type= SSL_TYPE_NOT_SPECIFIED;
            Lex->ssl_cipher= Lex->x509_subject= Lex->x509_issuer= 0;
            bzero(&(Lex->mqh), sizeof(Lex->mqh));
          }
        ;

/**************************************************************************

 CREATE VIEW statement parts.

**************************************************************************/

view_algorithm:
          ALGORITHM_SYM '=' UNDEFINED_SYM { $$= DTYPE_ALGORITHM_UNDEFINED; }
        | ALGORITHM_SYM '=' MERGE_SYM     { $$= VIEW_ALGORITHM_MERGE; }
        | ALGORITHM_SYM '=' TEMPTABLE_SYM { $$= VIEW_ALGORITHM_TMPTABLE; }
        ;

opt_view_suid:
          /* empty */                      { $$= VIEW_SUID_DEFAULT; }
        | view_suid                        { $$= $1; }
        ;

view_suid:
          SQL_SYM SECURITY_SYM DEFINER_SYM { $$= VIEW_SUID_DEFINER; }
        | SQL_SYM SECURITY_SYM INVOKER_SYM { $$= VIEW_SUID_INVOKER; }
        ;

view_list_opt:
          /* empty */
          {}
        | '(' view_list ')'
        ;

view_list:
          ident 
          {
            Lex->view_list.push_back((LEX_CSTRING*)
                                     thd->memdup(&$1, sizeof(LEX_CSTRING)),
                                     thd->mem_root);
          }
        | view_list ',' ident
          {
            Lex->view_list.push_back((LEX_CSTRING*)
                                     thd->memdup(&$3, sizeof(LEX_CSTRING)),
                                     thd->mem_root);
          }
        ;

view_select:
          {
            LEX *lex= Lex;
            lex->parsing_options.allows_variable= FALSE;
            lex->create_view->select.str= (char *) YYLIP->get_cpp_ptr();
          }
          query_expression
          view_check_option
          {
            if (Lex->parsed_create_view($2, $3))
              MYSQL_YYABORT;
          }
        ;

view_check_option:
          /* empty */                     { $$= VIEW_CHECK_NONE; }
        | WITH CHECK_SYM OPTION           { $$= VIEW_CHECK_CASCADED; }
        | WITH CASCADED CHECK_SYM OPTION  { $$= VIEW_CHECK_CASCADED; }
        | WITH LOCAL_SYM CHECK_SYM OPTION { $$= VIEW_CHECK_LOCAL; }
        ;

/**************************************************************************

 CREATE TRIGGER statement parts.

**************************************************************************/

trigger_action_order:
            FOLLOWS_SYM
            { $$= TRG_ORDER_FOLLOWS; }
          | PRECEDES_SYM
            { $$= TRG_ORDER_PRECEDES; }
          ;

trigger_follows_precedes_clause:
            /* empty */
            {
              $$.ordering_clause= TRG_ORDER_NONE;
              $$.anchor_trigger_name.str= NULL;
              $$.anchor_trigger_name.length= 0;
            }
          |
            trigger_action_order ident_or_text
            {
              $$.ordering_clause= $1;
              $$.anchor_trigger_name= $2;
            }
          ;

trigger_tail:
          remember_name
          opt_if_not_exists
          {
            if (unlikely(Lex->add_create_options_with_check($2)))
              MYSQL_YYABORT;
          }
          sp_name
          trg_action_time
          trg_event
          ON
          remember_name /* $8 */
          { /* $9 */
            Lex->raw_trg_on_table_name_begin= YYLIP->get_tok_start();
          }
          table_ident /* $10 */
          FOR_SYM
          remember_name /* $12 */
          { /* $13 */
            Lex->raw_trg_on_table_name_end= YYLIP->get_tok_start();
          }
          EACH_SYM
          ROW_SYM
          {
            Lex->trg_chistics.ordering_clause_begin= YYLIP->get_cpp_ptr();
          }
          trigger_follows_precedes_clause /* $17 */
          { /* $18 */
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;

            if (unlikely(lex->sphead))
              my_yyabort_error((ER_SP_NO_RECURSIVE_CREATE, MYF(0), "TRIGGER"));

            lex->stmt_definition_begin= $1;
            lex->ident.str= $8;
            lex->ident.length= $12 - $8;
            lex->spname= $4;
            (*static_cast<st_trg_execution_order*>(&lex->trg_chistics))= ($17);
            lex->trg_chistics.ordering_clause_end= lip->get_cpp_ptr();

            if (unlikely(!lex->make_sp_head(thd, $4, &sp_handler_trigger)))
              MYSQL_YYABORT;

            lex->sphead->set_body_start(thd, lip->get_cpp_tok_start());
          }
          sp_proc_stmt /* $19 */
          { /* $20 */
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;

            lex->sql_command= SQLCOM_CREATE_TRIGGER;
            sp->set_stmt_end(thd);
            sp->restore_thd_mem_root(thd);

            if (unlikely(sp->is_not_allowed_in_function("trigger")))
              MYSQL_YYABORT;

            /*
              We have to do it after parsing trigger body, because some of
              sp_proc_stmt alternatives are not saving/restoring LEX, so
              lex->query_tables can be wiped out.
            */
            if (!lex->first_select_lex()->
                 add_table_to_list(thd, $10, (LEX_CSTRING*) 0,
                                   TL_OPTION_UPDATING, TL_READ_NO_INSERT,
                                   MDL_SHARED_NO_WRITE))
              MYSQL_YYABORT;
          }
        ;

/**************************************************************************

 CREATE FUNCTION | PROCEDURE statements parts.

**************************************************************************/

udf_tail:
          opt_if_not_exists ident
          RETURNS_SYM udf_type SONAME_SYM TEXT_STRING_sys
          {
            LEX *lex= thd->lex;
            if (unlikely(lex->add_create_options_with_check($1)))
              MYSQL_YYABORT;
            if (unlikely(is_native_function(thd, & $2)))
              my_yyabort_error((ER_NATIVE_FCT_NAME_COLLISION, MYF(0), $2.str));
            lex->sql_command= SQLCOM_CREATE_FUNCTION;
            lex->udf.name= $2;
            lex->udf.returns= (Item_result) $4;
            lex->udf.dl= $6.str;
          }
        ;


sf_return_type:
          RETURNS_SYM
          {
            LEX *lex= Lex;
            lex->init_last_field(&lex->sphead->m_return_field_def,
                                 &empty_clex_str,
                                 thd->variables.collation_database);
          }
          type_with_opt_collate
          {
            if (unlikely(Lex->sphead->fill_field_definition(thd,
                                                            Lex->last_field)))
              MYSQL_YYABORT;
          }
        ;

sf_tail:
          opt_if_not_exists
          sp_name
          {
            Lex->sql_command= SQLCOM_CREATE_SPFUNCTION;
            if (unlikely(!Lex->make_sp_head_no_recursive(thd, $1, $2,
                                                         &sp_handler_function)))
              MYSQL_YYABORT;
          }
          sp_parenthesized_fdparam_list
          sf_return_type
          sp_c_chistics
          {
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;

            lex->sphead->set_chistics(lex->sp_chistics);
            lex->sphead->set_body_start(thd, lip->get_cpp_tok_start());
          }
          sp_proc_stmt_in_returns_clause
          {
            if (unlikely(Lex->sp_body_finalize_function(thd)))
              MYSQL_YYABORT;
          }
        ;

sp_tail:
          opt_if_not_exists sp_name
          {
            Lex->sql_command= SQLCOM_CREATE_PROCEDURE;
            if (unlikely(!Lex->make_sp_head_no_recursive(thd, $1, $2,
                                                         &sp_handler_procedure)))
              MYSQL_YYABORT;
          }
          sp_parenthesized_pdparam_list
          sp_c_chistics
          {
            Lex->sphead->set_chistics(Lex->sp_chistics);
            Lex->sphead->set_body_start(thd, YYLIP->get_cpp_tok_start());
          }
          sp_proc_stmt
          {
            if (unlikely(Lex->sp_body_finalize_procedure(thd)))
              MYSQL_YYABORT;
          }
        ;

/*************************************************************************/

xa:
          XA_SYM begin_or_start xid opt_join_or_resume
          {
            Lex->sql_command = SQLCOM_XA_START;
          }
        | XA_SYM END xid opt_suspend
          {
            Lex->sql_command = SQLCOM_XA_END;
          }
        | XA_SYM PREPARE_SYM xid
          {
            Lex->sql_command = SQLCOM_XA_PREPARE;
          }
        | XA_SYM COMMIT_SYM xid opt_one_phase
          {
            Lex->sql_command = SQLCOM_XA_COMMIT;
          }
        | XA_SYM ROLLBACK_SYM xid
          {
            Lex->sql_command = SQLCOM_XA_ROLLBACK;
          }
        | XA_SYM RECOVER_SYM opt_format_xid
          {
            Lex->sql_command = SQLCOM_XA_RECOVER;
            Lex->verbose= $3;
          }
        ;

opt_format_xid:
         /* empty */ { $$= false; }
        | FORMAT_SYM '=' ident_or_text
          {
            if (lex_string_eq(&$3, STRING_WITH_LEN("SQL")))
              $$= true;
            else if (lex_string_eq(&$3, STRING_WITH_LEN("RAW")))
              $$= false;
            else
            {
              my_yyabort_error((ER_UNKNOWN_EXPLAIN_FORMAT, MYF(0),
                               "XA RECOVER", $3.str));
              $$= false;
            }
          }
        ;

xid:
          text_string
          {
            MYSQL_YYABORT_UNLESS($1->length() <= MAXGTRIDSIZE);
            if (unlikely(!(Lex->xid=(XID *)thd->alloc(sizeof(XID)))))
              MYSQL_YYABORT;
            Lex->xid->set(1L, $1->ptr(), $1->length(), 0, 0);
          }
          | text_string ',' text_string
          {
            MYSQL_YYABORT_UNLESS($1->length() <= MAXGTRIDSIZE && $3->length() <= MAXBQUALSIZE);
            if (unlikely(!(Lex->xid=(XID *)thd->alloc(sizeof(XID)))))
              MYSQL_YYABORT;
            Lex->xid->set(1L, $1->ptr(), $1->length(), $3->ptr(), $3->length());
          }
          | text_string ',' text_string ',' ulong_num
          {
            MYSQL_YYABORT_UNLESS($1->length() <= MAXGTRIDSIZE && $3->length() <= MAXBQUALSIZE);
            if (unlikely(!(Lex->xid=(XID *)thd->alloc(sizeof(XID)))))
              MYSQL_YYABORT;
            Lex->xid->set($5, $1->ptr(), $1->length(), $3->ptr(), $3->length());
          }
        ;

begin_or_start:
          BEGIN_MARIADB_SYM {}
        | BEGIN_ORACLE_SYM {}
        | START_SYM {}
        ;

opt_join_or_resume:
          /* nothing */ { Lex->xa_opt=XA_NONE;        }
        | JOIN_SYM      { Lex->xa_opt=XA_JOIN;        }
        | RESUME_SYM    { Lex->xa_opt=XA_RESUME;      }
        ;

opt_one_phase:
          /* nothing */     { Lex->xa_opt=XA_NONE;        }
        | ONE_SYM PHASE_SYM { Lex->xa_opt=XA_ONE_PHASE;   }
        ;

opt_suspend:
          /* nothing */
          { Lex->xa_opt=XA_NONE;        }
        | SUSPEND_SYM
          { Lex->xa_opt=XA_SUSPEND;     }
          opt_migrate
        ;

opt_migrate:
          /* nothing */       {}
        | FOR_SYM MIGRATE_SYM { Lex->xa_opt=XA_FOR_MIGRATE; }
        ;

install:
          INSTALL_SYM PLUGIN_SYM opt_if_not_exists ident SONAME_SYM TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->create_info.init();
            if (lex->add_create_options_with_check($3))
              MYSQL_YYABORT;
            lex->sql_command= SQLCOM_INSTALL_PLUGIN;
            lex->comment= $4;
            lex->ident= $6;
          }
        | INSTALL_SYM SONAME_SYM TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_INSTALL_PLUGIN;
            lex->comment= null_clex_str;
            lex->ident= $3;
          }
        ;

uninstall:
          UNINSTALL_SYM PLUGIN_SYM opt_if_exists ident
          {
            LEX *lex= Lex;
            lex->check_opt.init();
            if (lex->add_create_options_with_check($3))
              MYSQL_YYABORT;
            lex->sql_command= SQLCOM_UNINSTALL_PLUGIN;
            lex->comment= $4;
          }
        | UNINSTALL_SYM SONAME_SYM opt_if_exists TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->check_opt.init();
            if (lex->add_create_options_with_check($3))
              MYSQL_YYABORT;
            lex->sql_command= SQLCOM_UNINSTALL_PLUGIN;
            lex->comment= null_clex_str;
            lex->ident= $4;
          }
        ;

/* Avoid compiler warning from sql_yacc.cc where yyerrlab1 is not used */
keep_gcc_happy:
          IMPOSSIBLE_ACTION
        {
          YYERROR;
        }
        ;

/**
  @} (end of group Parser)
*/
