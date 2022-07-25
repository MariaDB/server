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
   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1335  USA */

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
#include <my_global.h>
#include "sql_priv.h"
#include "sql_parse.h"                        /* comp_*_creator */
#include "sql_table.h"                        /* primary_key_name */
#include "sql_partition.h"  /* mem_alloc_error, partition_info, HASH_PARTITION */
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

/* this is to get the bison compilation windows warnings out */
#ifdef _MSC_VER
/* warning C4065: switch statement contains 'default' but no 'case' labels */
/* warning C4102: 'yyexhaustedlab': unreferenced label */
#pragma warning (disable : 4065 4102)
#endif
#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wunused-label" /* yyexhaustedlab: */
#endif

int yylex(void *yylval, void *yythd);

#define yyoverflow(A,B,C,D,E,F)               \
  {                                           \
    size_t val= *(F);                         \
    if (my_yyoverflow((B), (D), &val))        \
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
  if (!(A))                                      \
  {                                              \
    my_parse_error(thd, ER_SYNTAX_ERROR);        \
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
  @brief Push an error message into MySQL error stack with line
  and position information.

  This function provides semantic action implementers with a way
  to push the famous "You have a syntax error near..." error
  message into the error stack, which is normally produced only if
  a parse error is discovered internally by the Bison generated
  parser.
*/

static void my_parse_error_intern(THD *thd, const char *err_text,
                                  const char *yytext)
{
  Lex_input_stream *lip= &thd->m_parser_state->m_lip;
  if (!yytext)
  {
    if (!(yytext= lip->get_tok_start()))
      yytext= "";
  }
  /* Push an error into the error stack */
  ErrConvString err(yytext, strlen(yytext),
                    thd->variables.character_set_client);
  my_error(ER_PARSE_ERROR, MYF(0), err_text, err.ptr(), lip->yylineno);
}


static void my_parse_error(THD *thd, uint err_number, const char *yytext=0)
{
  return my_parse_error_intern(thd, ER_THD(thd, err_number), yytext);
}

void LEX::parse_error()
{
  my_parse_error(thd, ER_SYNTAX_ERROR);
}


static Item* escape(THD *thd)
{
  thd->lex->escape_used= false;
  const char *esc= thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES ? "" : "\\";
  return new (thd->mem_root) Item_string_ascii(thd, esc, MY_TEST(esc[0]));
}


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
  In semantic actions, please use my_parse_error or my_error to
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
  my_parse_error_intern(thd, s, 0);
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

static bool is_native_function(THD *thd, const LEX_STRING *name)
{
  if (find_native_function_builder(thd, *name))
    return true;

  if (is_lex_native_function(name))
    return true;

  return false;
}


static sp_head *make_sp_head(THD *thd, sp_name *name,
                             enum stored_procedure_type type)
{
  LEX *lex= thd->lex;
  sp_head *sp;

  /* Order is important here: new - reset - init */
  if ((sp= sp_head::create()))
  {
    sp->reset_thd_mem_root(thd);
    sp->init(lex);
    sp->m_type= type;
    if (name)
      sp->init_sp_name(thd, name);
    sp->m_chistics= &lex->sp_chistics;
    lex->sphead= sp;
  }
  bzero(&lex->sp_chistics, sizeof(lex->sp_chistics));
  return sp;
}

static bool maybe_start_compound_statement(THD *thd)
{
  if (!thd->lex->sphead)
  {
    if (!make_sp_head(thd, NULL, TYPE_ENUM_PROCEDURE))
      return 1;

    Lex->sp_chistics.suid= SP_IS_NOT_SUID;
    Lex->sphead->set_body_start(thd, YYLIP->get_cpp_ptr());
  }
  return 0;
}

static bool push_sp_label(THD *thd, LEX_STRING label)
{
  sp_pcontext *ctx= thd->lex->spcont;
  sp_label *lab= ctx->find_label(label);

  if (lab)
  {
    my_error(ER_SP_LABEL_REDEFINE, MYF(0), label.str);
    return 1;
  }
  else
  {
    lab= thd->lex->spcont->push_label(thd, label,
        thd->lex->sphead->instructions());
    lab->type= sp_label::ITERATION;
  }
  return 0;
}

static bool push_sp_empty_label(THD *thd)
{
  if (maybe_start_compound_statement(thd))
    return 1;
  /* Unlabeled controls get an empty label. */
  thd->lex->spcont->push_label(thd, empty_lex_str,
      thd->lex->sphead->instructions());
  return 0;
}

/**
  Helper action for a case expression statement (the expr in 'CASE expr').
  This helper is used for 'searched' cases only.
  @param lex the parser lex context
  @param expr the parsed expression
  @return 0 on success
*/

int case_stmt_action_expr(LEX *lex, Item* expr)
{
  sp_head *sp= lex->sphead;
  sp_pcontext *parsing_ctx= lex->spcont;
  int case_expr_id= parsing_ctx->register_case_expr();
  sp_instr_set_case_expr *i;

  if (parsing_ctx->push_case_expr_id(case_expr_id))
    return 1;

  i= new (lex->thd->mem_root)
    sp_instr_set_case_expr(sp->instructions(), parsing_ctx, case_expr_id, expr,
                           lex);

  sp->add_cont_backpatch(i);
  return sp->add_instr(i);
}

/**
  Helper action for a case when condition.
  This helper is used for both 'simple' and 'searched' cases.
  @param lex the parser lex context
  @param when the parsed expression for the WHEN clause
  @param simple true for simple cases, false for searched cases
*/

int case_stmt_action_when(LEX *lex, Item *when, bool simple)
{
  sp_head *sp= lex->sphead;
  sp_pcontext *ctx= lex->spcont;
  uint ip= sp->instructions();
  sp_instr_jump_if_not *i;
  Item_case_expr *var;
  Item *expr;
  THD *thd= lex->thd;

  if (simple)
  {
    var= new (thd->mem_root)
         Item_case_expr(thd, ctx->get_current_case_expr_id());

#ifndef DBUG_OFF
    if (var)
    {
      var->m_sp= sp;
    }
#endif

    expr= new (thd->mem_root) Item_func_eq(thd, var, when);
    i= new (thd->mem_root) sp_instr_jump_if_not(ip, ctx, expr, lex);
  }
  else
    i= new (thd->mem_root) sp_instr_jump_if_not(ip, ctx, when, lex);

  /*
    BACKPATCH: Registering forward jump from
    "case_stmt_action_when" to "case_stmt_action_then"
    (jump_if_not from instruction 2 to 5, 5 to 8 ... in the example)
  */

  return !MY_TEST(i) ||
         sp->push_backpatch(thd, i, ctx->push_label(thd, empty_lex_str, 0)) ||
         sp->add_cont_backpatch(i) ||
         sp->add_instr(i);
}

/**
  Helper action for a case then statements.
  This helper is used for both 'simple' and 'searched' cases.
  @param lex the parser lex context
*/

int case_stmt_action_then(LEX *lex)
{
  sp_head *sp= lex->sphead;
  sp_pcontext *ctx= lex->spcont;
  uint ip= sp->instructions();
  sp_instr_jump *i= new (lex->thd->mem_root) sp_instr_jump(ip, ctx);
  if (!MY_TEST(i) || sp->add_instr(i))
    return 1;

  /*
    BACKPATCH: Resolving forward jump from
    "case_stmt_action_when" to "case_stmt_action_then"
    (jump_if_not from instruction 2 to 5, 5 to 8 ... in the example)
  */

  sp->backpatch(ctx->pop_label());

  /*
    BACKPATCH: Registering forward jump from
    "case_stmt_action_then" to after END CASE
    (jump from instruction 4 to 12, 7 to 12 ... in the example)
  */

  return sp->push_backpatch(lex->thd, i, ctx->last_label());
}

static bool
find_sys_var_null_base(THD *thd, struct sys_var_with_base *tmp)
{
  tmp->var= find_sys_var(thd, tmp->base_name.str, tmp->base_name.length);

  if (tmp->var != NULL)
    tmp->base_name= null_lex_str;

  return thd->is_error();
}


/**
  Helper action for a SET statement.
  Used to push a system variable into the assignment list.

  @param thd      the current thread
  @param tmp      the system variable with base name
  @param var_type the scope of the variable
  @param val      the value being assigned to the variable

  @return TRUE if error, FALSE otherwise.
*/

static bool
set_system_variable(THD *thd, struct sys_var_with_base *tmp,
                    enum enum_var_type var_type, Item *val)
{
  set_var *var;
  LEX *lex= thd->lex;

  /* No AUTOCOMMIT from a stored function or trigger. */
  if (lex->spcont && tmp->var == Sys_autocommit_ptr)
    lex->sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;

  if (val && val->type() == Item::FIELD_ITEM &&
      ((Item_field*)val)->table_name)
  {
    my_error(ER_WRONG_TYPE_FOR_VAR, MYF(0), tmp->var->name.str);
    return TRUE;
  }

  if (! (var= new (thd->mem_root)
         set_var(thd, var_type, tmp->var, &tmp->base_name, val)))
    return TRUE;

  return lex->var_list.push_back(var, thd->mem_root);
}


/**
  Helper action for a SET statement.
  Used to push a SP local variable into the assignment list.

  @param thd      the current thread
  @param var_type the SP local variable
  @param val      the value being assigned to the variable

  @return TRUE if error, FALSE otherwise.
*/

static bool
set_local_variable(THD *thd, sp_variable *spv, Item *val)
{
  Item *it;
  LEX *lex= thd->lex;
  sp_instr_set *sp_set;

  if (val)
    it= val;
  else if (spv->default_value)
    it= spv->default_value;
  else
  {
    it= new (thd->mem_root) Item_null(thd);
    if (it == NULL)
      return TRUE;
  }

  sp_set= new (thd->mem_root)
         sp_instr_set(lex->sphead->instructions(), lex->spcont,
                                   spv->offset, it, spv->sql_type(),
                                   lex, TRUE);

  return (sp_set == NULL || lex->sphead->add_instr(sp_set));
}


/**
  Helper action for a SET statement.
  Used to SET a field of NEW row.

  @param thd      the current thread
  @param name     the field name
  @param val      the value being assigned to the row

  @return TRUE if error, FALSE otherwise.
*/

static bool
set_trigger_new_row(THD *thd, LEX_STRING *name, Item *val)
{
  LEX *lex= thd->lex;
  Item_trigger_field *trg_fld;
  sp_instr_set_trigger_field *sp_fld;

  /* QQ: Shouldn't this be field's default value ? */
  if (! val)
    val= new (thd->mem_root) Item_null(thd);

  DBUG_ASSERT(lex->trg_chistics.action_time == TRG_ACTION_BEFORE &&
              (lex->trg_chistics.event == TRG_EVENT_INSERT ||
               lex->trg_chistics.event == TRG_EVENT_UPDATE));

  trg_fld= new (thd->mem_root)
            Item_trigger_field(thd, lex->current_context(),
                               Item_trigger_field::NEW_ROW,
                               name->str, UPDATE_ACL, FALSE);

  if (trg_fld == NULL)
    return TRUE;

  sp_fld= new (thd->mem_root)
        sp_instr_set_trigger_field(lex->sphead->instructions(),
                                                 lex->spcont, trg_fld, val,
         lex);

  if (sp_fld == NULL)
    return TRUE;

  /*
    Let us add this item to list of all Item_trigger_field
    objects in trigger.
  */
  lex->trg_table_fields.link_in_list(trg_fld, &trg_fld->next_trg_field);

  return lex->sphead->add_instr(sp_fld);
}


/**
  Create an object to represent a SP variable in the Item-hierarchy.

  @param  thd         The current thread.
  @param  name        The SP variable name.
  @param  spvar       The SP variable (optional).
  @param  start_in_q  Start position of the SP variable name in the query.
  @param  end_in_q    End position of the SP variable name in the query.

  @remark If spvar is not specified, the name is used to search for the
          variable in the parse-time context. If the variable does not
          exist, a error is set and NULL is returned to the caller.

  @return An Item_splocal object representing the SP variable, or NULL on error.
*/
static Item_splocal*
create_item_for_sp_var(THD *thd, LEX_STRING name, sp_variable *spvar,
                       const char *start_in_q, const char *end_in_q)
{
  Item_splocal *item;
  LEX *lex= thd->lex;
  uint pos_in_q, len_in_q;
  sp_pcontext *spc = lex->spcont;

  /* If necessary, look for the variable. */
  if (spc && !spvar)
    spvar= spc->find_variable(name, false);

  if (!spvar)
  {
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), name.str);
    return NULL;
  }

  DBUG_ASSERT(spc && spvar);

  /* Position and length of the SP variable name in the query. */
  pos_in_q= (uint)(start_in_q - lex->sphead->m_tmp_query);
  len_in_q= (uint)(end_in_q - start_in_q);

  item= new (thd->mem_root)
    Item_splocal(thd, name, spvar->offset, spvar->sql_type(),
                 pos_in_q, len_in_q);

#ifndef DBUG_OFF
  if (item)
    item->m_sp= lex->sphead;
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
   @brief Creates a new SELECT_LEX for a UNION branch.

   Sets up and initializes a SELECT_LEX structure for a query once the parser
   discovers a UNION token. The current SELECT_LEX is pushed on the stack and
   the new SELECT_LEX becomes the current one.

   @param lex The parser state.

   @param is_union_distinct True if the union preceding the new select
          statement uses UNION DISTINCT.

   @param is_top_level This should be @c TRUE if the newly created SELECT_LEX
                       is a non-nested statement.

   @return <code>false</code> if successful, <code>true</code> if an error was
   reported. In the latter case parsing should stop.
 */
bool add_select_to_union_list(LEX *lex, bool is_union_distinct, 
                              bool is_top_level)
{
  /* 
     Only the last SELECT can have INTO. Since the grammar won't allow INTO in
     a nested SELECT, we make this check only when creating a top-level SELECT.
  */
  if (is_top_level && lex->result)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "UNION", "INTO");
    return TRUE;
  }
  if (lex->current_select->order_list.first && !lex->current_select->braces)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "UNION", "ORDER BY");
    return TRUE;
  }

  if (lex->current_select->explicit_limit && !lex->current_select->braces)
  {
    my_error(ER_WRONG_USAGE, MYF(0), "UNION", "LIMIT");
    return TRUE;
  }
  if (lex->current_select->linkage == GLOBAL_OPTIONS_TYPE)
  {
    my_parse_error(lex->thd, ER_SYNTAX_ERROR);
    return TRUE;
  }
  /* This counter shouldn't be incremented for UNION parts */
  lex->nest_level--;
  if (mysql_new_select(lex, 0))
    return TRUE;
  mysql_init_select(lex);
  lex->current_select->linkage=UNION_TYPE;
  lex->current_select->with_all_modifier= !is_union_distinct;
  if (is_union_distinct) /* UNION DISTINCT - remember position */
    lex->current_select->master_unit()->union_distinct=
      lex->current_select;
  return FALSE;
}


static bool add_create_index_prepare(LEX *lex, Table_ident *table)
{
  lex->sql_command= SQLCOM_CREATE_INDEX;
  if (!lex->current_select->add_table_to_list(lex->thd, table, NULL,
                                              TL_OPTION_UPDATING,
                                              TL_READ_NO_INSERT,
                                              MDL_SHARED_UPGRADABLE))
    return TRUE;
  lex->alter_info.reset();
  lex->alter_info.flags= Alter_info::ALTER_ADD_INDEX;
  lex->option_list= NULL;
  return FALSE;
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

static void sp_create_assignment_lex(THD *thd, bool no_lookahead)
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
  }
}


/**
  Create a SP instruction for a SET assignment.

  @see sp_create_assignment_lex

  @param thd           Thread context
  @param no_lookahead  True if the parser has no lookahead

  @return false if success, true otherwise.
*/

static bool sp_create_assignment_instr(THD *thd, bool no_lookahead)
{
  LEX *lex= thd->lex;

  if (lex->sphead)
  {
    sp_head *sp= lex->sphead;

    if (!lex->var_list.is_empty())
    {
      /*
        We have assignment to user or system variable or
        option setting, so we should construct sp_instr_stmt
        for it.
      */
      LEX_STRING qbuff;
      sp_instr_stmt *i;
      Lex_input_stream *lip= &thd->m_parser_state->m_lip;

      if (!(i= new (thd->mem_root)
        sp_instr_stmt(sp->instructions(), lex->spcont, lex)))
        return true;

      /*
        Extract the query statement from the tokenizer.  The
        end is either lip->ptr, if there was no lookahead,
        lip->tok_end otherwise.
      */
      if (no_lookahead)
        qbuff.length= lip->get_ptr() - sp->m_tmp_query;
      else
        qbuff.length= lip->get_tok_end() - sp->m_tmp_query;

      if (!(qbuff.str= (char*) alloc_root(thd->mem_root,
                                          qbuff.length + 5)))
        return true;

      strmake(strmake(qbuff.str, "SET ", 4), sp->m_tmp_query,
              qbuff.length);
      qbuff.length+= 4;
      i->m_query= qbuff;
      if (sp->add_instr(i))
        return true;
    }
    enum_var_type inner_option_type= lex->option_type;
    if (lex->sphead->restore_lex(thd))
      return true;
    /* Copy option_type to outer lex in case it has changed. */
    thd->lex->option_type= inner_option_type;
  }
  return false;
}


static void add_key_to_list(LEX *lex, LEX_STRING *field_name,
                            enum Key::Keytype type, bool check_exists)
{
  Key *key;
  MEM_ROOT *mem_root= lex->thd->mem_root;
  key= new (mem_root)
        Key(type, null_lex_str, HA_KEY_ALG_UNDEF, false,
             DDL_options(check_exists ?
                         DDL_options::OPT_IF_NOT_EXISTS :
                         DDL_options::OPT_NONE));
  key->columns.push_back(new (mem_root) Key_part_spec(*field_name, 0),
                         mem_root);
  lex->alter_info.key_list.push_back(key, mem_root);
}

void LEX::init_last_field(Column_definition *field, const char *field_name,
         CHARSET_INFO *cs)
{
  last_field= field;

  field->field_name= field_name;

  /* reset LEX fields that are used in Create_field::set_and_check() */
  charset= cs;
}

void LEX::set_last_field_type(const Lex_field_type_st &type)
{
  last_field->sql_type= type.field_type();
  last_field->charset= charset;

  if (type.length())
  {
    int err;
    last_field->length= my_strtoll10(type.length(), NULL, &err);
    if (err)
      last_field->length= ~0ULL; // safety
  }
  else
    last_field->length= 0;

  last_field->decimals= type.dec() ? (uint)atoi(type.dec()) : 0;
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
     if (Lex->set_bincmp(X,Y))          \
       MYSQL_YYABORT;                   \
  } while(0)

Virtual_column_info *add_virtual_expression(THD *thd, Item *expr)
{
  Virtual_column_info *v= new (thd->mem_root) Virtual_column_info();
  if (!v)
  {
     mem_alloc_error(sizeof(Virtual_column_info));
     return 0;
   }
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

  /* structs */
  LEX_STRING lex_str;
  LEX_SYMBOL symbol;
  struct sys_var_with_base variable;
  struct { int vars, conds, hndlrs, curs; } spblock;
  Lex_length_and_dec_st Lex_length_and_dec;
  Lex_cast_type_st Lex_cast_type;
  Lex_field_type_st Lex_field_type;
  Lex_dyncol_type_st Lex_dyncol_type;

  /* pointers */
  Create_field *create_field;
  CHARSET_INFO *charset;
  Condition_information_item *cond_info_item;
  DYNCALL_CREATE_DEF *dyncol_def;
  Diagnostics_information *diag_info;
  Item *item;
  Item_num *item_num;
  Item_param *item_param;
  Key_part_spec *key_part;
  LEX *lex;
  LEX_STRING *lex_str_ptr;
  LEX_USER *lex_user;
  List<Condition_information_item> *cond_info_list;
  List<DYNCALL_CREATE_DEF> *dyncol_def_list;
  List<Item> *item_list;
  List<Statement_information_item> *stmt_info_list;
  List<String> *string_list;
  List<LEX_STRING> *lex_str_list;
  Statement_information_item *stmt_info_item;
  String *string;
  TABLE_LIST *table_list;
  Table_ident *table;
  char *simple_string;
  const char *const_simple_string;
  chooser_compare_func_creator boolfunc2creator;
  class my_var *myvar;
  class sp_condition_value *spcondvalue;
  class sp_head *sphead;
  class sp_label *splabel;
  class sp_name *spname;
  class sp_variable *spvar;
  class With_element_head *with_element_head;
  class With_clause *with_clause;
  class Virtual_column_info *virtual_column;

  handlerton *db_type;
  st_select_lex *select_lex;
  struct p_elem_val *p_elem_value;
  class Window_frame *window_frame;
  class Window_frame_bound *window_frame_bound;
  udf_func *udf;
  st_trg_execution_order trg_execution_order;

  /* enums */
  enum Condition_information_item::Name cond_info_item_name;
  enum enum_diag_condition_item_name diag_condition_item_name;
  enum Diagnostics_information::Which_area diag_area;
  enum Field::geometry_type geom_type;
  enum enum_fk_option m_fk_option;
  enum Item_udftype udf_type;
  enum Key::Keytype key_type;
  enum Statement_information_item::Name stmt_info_item_name;
  enum enum_field_types field_type;
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
}

%{
bool my_yyoverflow(short **a, YYSTYPE **b, size_t *yystacksize);
%}

%pure-parser                                    /* We have threads */
%parse-param { THD *thd }
%lex-param { THD *thd }
/*
  Currently there are 119 shift/reduce conflicts.
  We should not introduce new conflicts any more.
*/
%expect 119

/*
   Comments for TOKENS.
   For each token, please include in the same line a comment that contains
   the following tags:
   SQL-2011-N : Non Reserved keywird as per SQL-2011
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
 
%token  ABORT_SYM                     /* INTERNAL (used in lex) */
%token  ACCESSIBLE_SYM
%token  ACTION                        /* SQL-2003-N */
%token  ADD                           /* SQL-2003-R */
%token  ADMIN_SYM                     /* SQL-2003-N */
%token  ADDDATE_SYM                   /* MYSQL-FUNC */
%token  AFTER_SYM                     /* SQL-2003-N */
%token  AGAINST
%token  AGGREGATE_SYM
%token  ALGORITHM_SYM
%token  ALL                           /* SQL-2003-R */
%token  ALTER                         /* SQL-2003-R */
%token  ALWAYS_SYM
%token  ANALYZE_SYM
%token  AND_AND_SYM                   /* OPERATOR */
%token  AND_SYM                       /* SQL-2003-R */
%token  ANY_SYM                       /* SQL-2003-R */
%token  AS                            /* SQL-2003-R */
%token  ASC                           /* SQL-2003-N */
%token  ASCII_SYM                     /* MYSQL-FUNC */
%token  ASENSITIVE_SYM                /* FUTURE-USE */
%token  AT_SYM                        /* SQL-2003-R */
%token  ATOMIC_SYM                    /* SQL-2003-R */
%token  AUTHORS_SYM
%token  AUTOEXTEND_SIZE_SYM
%token  AUTO_INC
%token  AUTO_SYM
%token  AVG_ROW_LENGTH
%token  AVG_SYM                       /* SQL-2003-N */
%token  BACKUP_SYM
%token  BEFORE_SYM                    /* SQL-2003-N */
%token  BEGIN_SYM                     /* SQL-2003-R */
%token  BETWEEN_SYM                   /* SQL-2003-R */
%token  BIGINT                        /* SQL-2003-R */
%token  BINARY                        /* SQL-2003-R */
%token  BINLOG_SYM
%token  BIN_NUM
%token  BIT_AND                       /* MYSQL-FUNC */
%token  BIT_OR                        /* MYSQL-FUNC */
%token  BIT_SYM                       /* MYSQL-FUNC */
%token  BIT_XOR                       /* MYSQL-FUNC */
%token  BLOB_SYM                      /* SQL-2003-R */
%token  BLOCK_SYM
%token  BOOLEAN_SYM                   /* SQL-2003-R */
%token  BOOL_SYM
%token  BOTH                          /* SQL-2003-R */
%token  BTREE_SYM
%token  BY                            /* SQL-2003-R */
%token  BYTE_SYM
%token  CACHE_SYM
%token  CALL_SYM                      /* SQL-2003-R */
%token  CASCADE                       /* SQL-2003-N */
%token  CASCADED                      /* SQL-2003-R */
%token  CASE_SYM                      /* SQL-2003-R */
%token  CAST_SYM                      /* SQL-2003-R */
%token  CATALOG_NAME_SYM              /* SQL-2003-N */
%token  CHAIN_SYM                     /* SQL-2003-N */
%token  CHANGE
%token  CHANGED
%token  CHARSET
%token  CHAR_SYM                      /* SQL-2003-R */
%token  CHECKPOINT_SYM
%token  CHECKSUM_SYM
%token  CHECK_SYM                     /* SQL-2003-R */
%token  CIPHER_SYM
%token  CLASS_ORIGIN_SYM              /* SQL-2003-N */
%token  CLIENT_SYM
%token  CLOSE_SYM                     /* SQL-2003-R */
%token  COALESCE                      /* SQL-2003-N */
%token  CODE_SYM
%token  COLLATE_SYM                   /* SQL-2003-R */
%token  COLLATION_SYM                 /* SQL-2003-N */
%token  COLUMNS
%token  COLUMN_ADD_SYM
%token  COLUMN_CHECK_SYM
%token  COLUMN_CREATE_SYM
%token  COLUMN_DELETE_SYM
%token  COLUMN_GET_SYM
%token  COLUMN_SYM                    /* SQL-2003-R */
%token  COLUMN_NAME_SYM               /* SQL-2003-N */
%token  COMMENT_SYM
%token  COMMITTED_SYM                 /* SQL-2003-N */
%token  COMMIT_SYM                    /* SQL-2003-R */
%token  COMPACT_SYM
%token  COMPLETION_SYM
%token  COMPRESSED_SYM
%token  CONCURRENT
%token  CONDITION_SYM                 /* SQL-2003-R, SQL-2008-R */
%token  CONNECTION_SYM
%token  CONSISTENT_SYM
%token  CONSTRAINT                    /* SQL-2003-R */
%token  CONSTRAINT_CATALOG_SYM        /* SQL-2003-N */
%token  CONSTRAINT_NAME_SYM           /* SQL-2003-N */
%token  CONSTRAINT_SCHEMA_SYM         /* SQL-2003-N */
%token  CONTAINS_SYM                  /* SQL-2003-N */
%token  CONTEXT_SYM
%token  CONTINUE_SYM                  /* SQL-2003-R */
%token  CONTRIBUTORS_SYM
%token  CONVERT_SYM                   /* SQL-2003-N */
%token  COUNT_SYM                     /* SQL-2003-N */
%token  CPU_SYM
%token  CREATE                        /* SQL-2003-R */
%token  CROSS                         /* SQL-2003-R */
%token  CUBE_SYM                      /* SQL-2003-R */
%token  CUME_DIST_SYM
%token  CURDATE                       /* MYSQL-FUNC */
%token  CURRENT_SYM                   /* SQL-2003-R */
%token  CURRENT_USER                  /* SQL-2003-R */
%token  CURRENT_ROLE                  /* SQL-2003-R */
%token  CURRENT_POS_SYM
%token  CURSOR_SYM                    /* SQL-2003-R */
%token  CURSOR_NAME_SYM               /* SQL-2003-N */
%token  CURTIME                       /* MYSQL-FUNC */
%token  DATABASE
%token  DATABASES
%token  DATAFILE_SYM
%token  DATA_SYM                      /* SQL-2003-N */
%token  DATETIME
%token  DATE_ADD_INTERVAL             /* MYSQL-FUNC */
%token  DATE_SUB_INTERVAL             /* MYSQL-FUNC */
%token  DATE_SYM                      /* SQL-2003-R */
%token  DAY_HOUR_SYM
%token  DAY_MICROSECOND_SYM
%token  DAY_MINUTE_SYM
%token  DAY_SECOND_SYM
%token  DAY_SYM                       /* SQL-2003-R */
%token  DEALLOCATE_SYM                /* SQL-2003-R */
%token  DECIMAL_NUM
%token  DECIMAL_SYM                   /* SQL-2003-R */
%token  DECLARE_SYM                   /* SQL-2003-R */
%token  DEFAULT                       /* SQL-2003-R */
%token  DEFINER_SYM
%token  DELAYED_SYM
%token  DELAY_KEY_WRITE_SYM
%token  DELETE_DOMAIN_ID_SYM
%token  DELETE_SYM                    /* SQL-2003-R */
%token  DENSE_RANK_SYM
%token  DESC                          /* SQL-2003-N */
%token  DESCRIBE                      /* SQL-2003-R */
%token  DES_KEY_FILE
%token  DETERMINISTIC_SYM             /* SQL-2003-R */
%token  DIAGNOSTICS_SYM               /* SQL-2003-N */
%token  DIRECTORY_SYM
%token  DISABLE_SYM
%token  DISCARD
%token  DISK_SYM
%token  DISTINCT                      /* SQL-2003-R */
%token  DIV_SYM
%token  DOUBLE_SYM                    /* SQL-2003-R */
%token  DO_DOMAIN_IDS_SYM
%token  DO_SYM
%token  DROP                          /* SQL-2003-R */
%token  DUAL_SYM
%token  DUMPFILE
%token  DUPLICATE_SYM
%token  DYNAMIC_SYM                   /* SQL-2003-R */
%token  EACH_SYM                      /* SQL-2003-R */
%token  ELSE                          /* SQL-2003-R */
%token  ELSEIF_SYM
%token  ENABLE_SYM
%token  ENCLOSED
%token  END                           /* SQL-2003-R */
%token  ENDS_SYM
%token  END_OF_INPUT                  /* INTERNAL */
%token  ENGINES_SYM
%token  ENGINE_SYM
%token  ENUM
%token  EQUAL_SYM                     /* OPERATOR */
%token  ERROR_SYM
%token  ERRORS
%token  ESCAPED
%token  ESCAPE_SYM                    /* SQL-2003-R */
%token  EVENTS_SYM
%token  EVENT_SYM
%token  EVERY_SYM                     /* SQL-2003-N */
%token  EXCHANGE_SYM
%token  EXAMINED_SYM
%token  EXCLUDE_SYM                   /* SQL-2011-N */
%token  EXECUTE_SYM                   /* SQL-2003-R */
%token  EXISTS                        /* SQL-2003-R */
%token  EXIT_SYM
%token  EXPANSION_SYM
%token  EXPORT_SYM
%token  EXTENDED_SYM
%token  EXTENT_SIZE_SYM
%token  EXTRACT_SYM                   /* SQL-2003-N */
%token  FALSE_SYM                     /* SQL-2003-R */
%token  FAST_SYM
%token  FAULTS_SYM
%token  FETCH_SYM                     /* SQL-2003-R */
%token  FILE_SYM
%token  FIRST_VALUE_SYM               /* SQL-2011 */
%token  FIRST_SYM                     /* SQL-2003-N */
%token  FIXED_SYM
%token  FLOAT_NUM
%token  FLOAT_SYM                     /* SQL-2003-R */
%token  FLUSH_SYM
%token  FOLLOWS_SYM                   /* MYSQL trigger*/
%token  FOLLOWING_SYM                 /* SQL-2011-N */
%token  FORCE_SYM
%token  FORCE_LOOKAHEAD               /* INTERNAL never returned by the lexer */
%token  FOREIGN                       /* SQL-2003-R */
%token  FOR_SYM                       /* SQL-2003-R */
%token  FORMAT_SYM
%token  FOUND_SYM                     /* SQL-2003-R */
%token  FROM
%token  FULL                          /* SQL-2003-R */
%token  FULLTEXT_SYM
%token  FUNCTION_SYM                  /* SQL-2003-R */
%token  GE
%token  GENERAL
%token  GENERATED_SYM
%token  GEOMETRYCOLLECTION
%token  GEOMETRY_SYM
%token  GET_FORMAT                    /* MYSQL-FUNC */
%token  GET_SYM                       /* SQL-2003-R */
%token  GLOBAL_SYM                    /* SQL-2003-R */
%token  GRANT                         /* SQL-2003-R */
%token  GRANTS
%token  GROUP_SYM                     /* SQL-2003-R */
%token  GROUP_CONCAT_SYM
%token  LAG_SYM                       /* SQL-2011 */
%token  LEAD_SYM                      /* SQL-2011 */
%token  HANDLER_SYM
%token  HARD_SYM
%token  HASH_SYM
%token  HAVING                        /* SQL-2003-R */
%token  HELP_SYM
%token  HEX_NUM
%token  HEX_STRING
%token  HIGH_PRIORITY
%token  HOST_SYM
%token  HOSTS_SYM
%token  HOUR_MICROSECOND_SYM
%token  HOUR_MINUTE_SYM
%token  HOUR_SECOND_SYM
%token  HOUR_SYM                      /* SQL-2003-R */
%token  ID_SYM                        /* MYSQL */
%token  IDENT
%token  IDENTIFIED_SYM
%token  IDENT_QUOTED
%token  IF_SYM
%token  IGNORE_DOMAIN_IDS_SYM
%token  IGNORE_SYM
%token  IGNORE_SERVER_IDS_SYM
%token  IMMEDIATE_SYM                 /* SQL-2003-R */
%token  IMPORT
%token  INDEXES
%token  INDEX_SYM
%token  INFILE
%token  INITIAL_SIZE_SYM
%token  INNER_SYM                     /* SQL-2003-R */
%token  INOUT_SYM                     /* SQL-2003-R */
%token  INSENSITIVE_SYM               /* SQL-2003-R */
%token  INSERT                        /* SQL-2003-R */
%token  INSERT_METHOD
%token  INSTALL_SYM
%token  INTERVAL_SYM                  /* SQL-2003-R */
%token  INTO                          /* SQL-2003-R */
%token  INT_SYM                       /* SQL-2003-R */
%token  INVOKER_SYM
%token  IN_SYM                        /* SQL-2003-R */
%token  IO_SYM
%token  IPC_SYM
%token  IS                            /* SQL-2003-R */
%token  ISOLATION                     /* SQL-2003-R */
%token  ISSUER_SYM
%token  ITERATE_SYM
%token  JOIN_SYM                      /* SQL-2003-R */
%token  JSON_SYM
%token  KEYS
%token  KEY_BLOCK_SIZE
%token  KEY_SYM                       /* SQL-2003-N */
%token  KILL_SYM
%token  LANGUAGE_SYM                  /* SQL-2003-R */
%token  LAST_SYM                      /* SQL-2003-N */
%token  LAST_VALUE
%token  LE                            /* OPERATOR */
%token  LEADING                       /* SQL-2003-R */
%token  LEAVES
%token  LEAVE_SYM
%token  LEFT                          /* SQL-2003-R */
%token  LESS_SYM
%token  LEVEL_SYM
%token  LEX_HOSTNAME
%token  LIKE                          /* SQL-2003-R */
%token  LIMIT
%token  LINEAR_SYM
%token  LINES
%token  LINESTRING
%token  LIST_SYM
%token  LOAD
%token  LOCAL_SYM                     /* SQL-2003-R */
%token  LOCATOR_SYM                   /* SQL-2003-N */
%token  LOCKS_SYM
%token  LOCK_SYM
%token  LOGFILE_SYM
%token  LOGS_SYM
%token  LONGBLOB
%token  LONGTEXT
%token  LONG_NUM
%token  LONG_SYM
%token  LOOP_SYM
%token  LOW_PRIORITY
%token  MASTER_CONNECT_RETRY_SYM
%token  MASTER_DELAY_SYM
%token  MASTER_GTID_POS_SYM
%token  MASTER_HOST_SYM
%token  MASTER_LOG_FILE_SYM
%token  MASTER_LOG_POS_SYM
%token  MASTER_PASSWORD_SYM
%token  MASTER_PORT_SYM
%token  MASTER_SERVER_ID_SYM
%token  MASTER_SSL_CAPATH_SYM
%token  MASTER_SSL_CA_SYM
%token  MASTER_SSL_CERT_SYM
%token  MASTER_SSL_CIPHER_SYM
%token  MASTER_SSL_CRL_SYM
%token  MASTER_SSL_CRLPATH_SYM
%token  MASTER_SSL_KEY_SYM
%token  MASTER_SSL_SYM
%token  MASTER_SSL_VERIFY_SERVER_CERT_SYM
%token  MASTER_SYM
%token  MASTER_USER_SYM
%token  MASTER_USE_GTID_SYM
%token  MASTER_HEARTBEAT_PERIOD_SYM
%token  MATCH                         /* SQL-2003-R */
%token  MAX_CONNECTIONS_PER_HOUR
%token  MAX_QUERIES_PER_HOUR
%token  MAX_ROWS
%token  MAX_SIZE_SYM
%token  MAX_SYM                       /* SQL-2003-N */
%token  MAX_UPDATES_PER_HOUR
%token  MAX_STATEMENT_TIME_SYM
%token  MAX_USER_CONNECTIONS_SYM
%token  MAX_VALUE_SYM                 /* SQL-2003-N */
%token  MEDIUMBLOB
%token  MEDIUMINT
%token  MEDIUMTEXT
%token  MEDIUM_SYM
%token  MEMORY_SYM
%token  MERGE_SYM                     /* SQL-2003-R */
%token  MESSAGE_TEXT_SYM              /* SQL-2003-N */
%token  MICROSECOND_SYM               /* MYSQL-FUNC */
%token  MIGRATE_SYM
%token  MINUTE_MICROSECOND_SYM
%token  MINUTE_SECOND_SYM
%token  MINUTE_SYM                    /* SQL-2003-R */
%token  MIN_ROWS
%token  MIN_SYM                       /* SQL-2003-N */
%token  MODE_SYM
%token  MODIFIES_SYM                  /* SQL-2003-R */
%token  MODIFY_SYM
%token  MOD_SYM                       /* SQL-2003-N */
%token  MONTH_SYM                     /* SQL-2003-R */
%token  MULTILINESTRING
%token  MULTIPOINT
%token  MULTIPOLYGON
%token  MUTEX_SYM
%token  MYSQL_SYM
%token  MYSQL_ERRNO_SYM
%token  NAMES_SYM                     /* SQL-2003-N */
%token  NAME_SYM                      /* SQL-2003-N */
%token  NATIONAL_SYM                  /* SQL-2003-R */
%token  NATURAL                       /* SQL-2003-R */
%token  NCHAR_STRING
%token  NCHAR_SYM                     /* SQL-2003-R */
%token  NE                            /* OPERATOR */
%token  NEG
%token  NEW_SYM                       /* SQL-2003-R */
%token  NEXT_SYM                      /* SQL-2003-N */
%token  NODEGROUP_SYM
%token  NONE_SYM                      /* SQL-2003-R */
%token  NOT2_SYM
%token  NOT_SYM                       /* SQL-2003-R */
%token  NOW_SYM
%token  NO_SYM                        /* SQL-2003-R */
%token  NO_WAIT_SYM
%token  NO_WRITE_TO_BINLOG
%token  NTILE_SYM
%token  NULL_SYM                      /* SQL-2003-R */
%token  NUM
%token  NUMBER_SYM                    /* SQL-2003-N */
%token  NUMERIC_SYM                   /* SQL-2003-R */
%token  NTH_VALUE_SYM                 /* SQL-2011 */
%token  NVARCHAR_SYM
%token  OFFSET_SYM
%token  OLD_PASSWORD_SYM
%token  ON                            /* SQL-2003-R */
%token  ONE_SYM
%token  ONLY_SYM                      /* SQL-2003-R */
%token  ONLINE_SYM
%token  OPEN_SYM                      /* SQL-2003-R */
%token  OPTIMIZE
%token  OPTIONS_SYM
%token  OPTION                        /* SQL-2003-N */
%token  OPTIONALLY
%token  OR2_SYM
%token  ORDER_SYM                     /* SQL-2003-R */
%token  OR_OR_SYM                     /* OPERATOR */
%token  OR_SYM                        /* SQL-2003-R */
%token  OTHERS_SYM                    /* SQL-2011-N */
%token  OUTER
%token  OUTFILE
%token  OUT_SYM                       /* SQL-2003-R */
%token  OVER_SYM
%token  OWNER_SYM
%token  PACK_KEYS_SYM
%token  PAGE_SYM
%token  PAGE_CHECKSUM_SYM
%token  PARAM_MARKER
%token  PARSER_SYM
%token  PARSE_VCOL_EXPR_SYM
%token  PARTIAL                       /* SQL-2003-N */
%token  PARTITION_SYM                 /* SQL-2003-R */
%token  PARTITIONS_SYM
%token  PARTITIONING_SYM
%token  PASSWORD_SYM
%token  PERCENT_RANK_SYM
%token  PERSISTENT_SYM
%token  PHASE_SYM
%token  PLUGINS_SYM
%token  PLUGIN_SYM
%token  POINT_SYM
%token  POLYGON
%token  PORT_SYM
%token  POSITION_SYM                  /* SQL-2003-N */
%token  PRECEDES_SYM                  /* MYSQL */
%token  PRECEDING_SYM                 /* SQL-2011-N */
%token  PRECISION                     /* SQL-2003-R */
%token  PREPARE_SYM                   /* SQL-2003-R */
%token  PRESERVE_SYM
%token  PREV_SYM
%token  PRIMARY_SYM                   /* SQL-2003-R */
%token  PRIVILEGES                    /* SQL-2003-N */
%token  PROCEDURE_SYM                 /* SQL-2003-R */
%token  PROCESS
%token  PROCESSLIST_SYM
%token  PROFILE_SYM
%token  PROFILES_SYM
%token  PROXY_SYM
%token  PURGE
%token  QUARTER_SYM
%token  QUERY_SYM
%token  QUICK
%token  RANGE_SYM                     /* SQL-2003-R */
%token  RANK_SYM        
%token  READS_SYM                     /* SQL-2003-R */
%token  READ_ONLY_SYM
%token  READ_SYM                      /* SQL-2003-N */
%token  READ_WRITE_SYM
%token  REAL                          /* SQL-2003-R */
%token  REBUILD_SYM
%token  RECOVER_SYM
%token  RECURSIVE_SYM
%token  REDOFILE_SYM
%token  REDO_BUFFER_SIZE_SYM
%token  REDUNDANT_SYM
%token  REFERENCES                    /* SQL-2003-R */
%token  REGEXP
%token  RELAY
%token  RELAYLOG_SYM
%token  RELAY_LOG_FILE_SYM
%token  RELAY_LOG_POS_SYM
%token  RELAY_THREAD
%token  RELEASE_SYM                   /* SQL-2003-R */
%token  RELOAD
%token  REMOVE_SYM
%token  RENAME
%token  REORGANIZE_SYM
%token  REPAIR
%token  REPEATABLE_SYM                /* SQL-2003-N */
%token  REPEAT_SYM                    /* MYSQL-FUNC */
%token  REPLACE                       /* MYSQL-FUNC */
%token  REPLICATION
%token  REQUIRE_SYM
%token  RESET_SYM
%token  RESIGNAL_SYM                  /* SQL-2003-R */
%token  RESOURCES
%token  RESTORE_SYM
%token  RESTRICT
%token  RESUME_SYM
%token  RETURNED_SQLSTATE_SYM         /* SQL-2003-N */
%token  RETURNING_SYM
%token  RETURNS_SYM                   /* SQL-2003-R */
%token  RETURN_SYM                    /* SQL-2003-R */
%token  REVERSE_SYM
%token  REVOKE                        /* SQL-2003-R */
%token  RIGHT                         /* SQL-2003-R */
%token  ROLE_SYM
%token  ROLLBACK_SYM                  /* SQL-2003-R */
%token  ROLLUP_SYM                    /* SQL-2003-R */
%token  ROUTINE_SYM                   /* SQL-2003-N */
%token  ROW_SYM                       /* SQL-2003-R */
%token  ROWS_SYM                      /* SQL-2003-R */
%token  ROW_COUNT_SYM                 /* SQL-2003-N */
%token  ROW_FORMAT_SYM
%token  ROW_NUMBER_SYM
%token  RTREE_SYM
%token  SAVEPOINT_SYM                 /* SQL-2003-R */
%token  SCHEDULE_SYM
%token  SCHEMA_NAME_SYM               /* SQL-2003-N */
%token  SECOND_MICROSECOND_SYM
%token  SECOND_SYM                    /* SQL-2003-R */
%token  SECURITY_SYM                  /* SQL-2003-N */
%token  SELECT_SYM                    /* SQL-2003-R */
%token  SENSITIVE_SYM                 /* FUTURE-USE */
%token  SEPARATOR_SYM
%token  SERIALIZABLE_SYM              /* SQL-2003-N */
%token  SERIAL_SYM
%token  SESSION_SYM                   /* SQL-2003-N */
%token  SERVER_SYM
%token  SERVER_OPTIONS
%token  SET                           /* SQL-2003-R */
%token  SET_VAR
%token  SHARE_SYM
%token  SHIFT_LEFT                    /* OPERATOR */
%token  SHIFT_RIGHT                   /* OPERATOR */
%token  SHOW
%token  SHUTDOWN
%token  SIGNAL_SYM                    /* SQL-2003-R */
%token  SIGNED_SYM
%token  SIMPLE_SYM                    /* SQL-2003-N */
%token  SLAVE
%token  SLAVES
%token  SLAVE_POS_SYM
%token  SLOW
%token  SMALLINT                      /* SQL-2003-R */
%token  SNAPSHOT_SYM
%token  SOCKET_SYM
%token  SOFT_SYM
%token  SONAME_SYM
%token  SOUNDS_SYM
%token  SOURCE_SYM
%token  SPATIAL_SYM
%token  SPECIFIC_SYM                  /* SQL-2003-R */
%token  SQLEXCEPTION_SYM              /* SQL-2003-R */
%token  SQLSTATE_SYM                  /* SQL-2003-R */
%token  SQLWARNING_SYM                /* SQL-2003-R */
%token  SQL_BIG_RESULT
%token  SQL_BUFFER_RESULT
%token  SQL_CACHE_SYM
%token  SQL_CALC_FOUND_ROWS
%token  SQL_NO_CACHE_SYM
%token  SQL_SMALL_RESULT
%token  SQL_SYM                       /* SQL-2003-R */
%token  SQL_THREAD
%token  REF_SYSTEM_ID_SYM
%token  SSL_SYM
%token  STARTING
%token  STARTS_SYM
%token  START_SYM                     /* SQL-2003-R */
%token  STATEMENT_SYM
%token  STATS_AUTO_RECALC_SYM
%token  STATS_PERSISTENT_SYM
%token  STATS_SAMPLE_PAGES_SYM
%token  STATUS_SYM
%token  STDDEV_SAMP_SYM               /* SQL-2003-N */
%token  STD_SYM
%token  STOP_SYM
%token  STORAGE_SYM
%token  STORED_SYM
%token  STRAIGHT_JOIN
%token  STRING_SYM
%token  SUBCLASS_ORIGIN_SYM           /* SQL-2003-N */
%token  SUBDATE_SYM
%token  SUBJECT_SYM
%token  SUBPARTITIONS_SYM
%token  SUBPARTITION_SYM
%token  SUBSTRING                     /* SQL-2003-N */
%token  SUM_SYM                       /* SQL-2003-N */
%token  SUPER_SYM
%token  SUSPEND_SYM
%token  SWAPS_SYM
%token  SWITCHES_SYM
%token  SYSDATE
%token  TABLES
%token  TABLESPACE
%token  TABLE_REF_PRIORITY
%token  TABLE_SYM                     /* SQL-2003-R */
%token  TABLE_CHECKSUM_SYM
%token  TABLE_NAME_SYM                /* SQL-2003-N */
%token  TEMPORARY                     /* SQL-2003-N */
%token  TEMPTABLE_SYM
%token  TERMINATED
%token  TEXT_STRING
%token  TEXT_SYM
%token  THAN_SYM
%token  THEN_SYM                      /* SQL-2003-R */
%token  TIES_SYM                      /* SQL-2011-N */
%token  TIMESTAMP                     /* SQL-2003-R */
%token  TIMESTAMP_ADD
%token  TIMESTAMP_DIFF
%token  TIME_SYM                      /* SQL-2003-R */
%token  TINYBLOB
%token  TINYINT
%token  TINYTEXT
%token  TO_SYM                        /* SQL-2003-R */
%token  TRAILING                      /* SQL-2003-R */
%token  TRANSACTION_SYM
%token  TRANSACTIONAL_SYM
%token  TRIGGERS_SYM
%token  TRIGGER_SYM                   /* SQL-2003-R */
%token  TRIM                          /* SQL-2003-N */
%token  TRUE_SYM                      /* SQL-2003-R */
%token  TRUNCATE_SYM
%token  TYPES_SYM
%token  TYPE_SYM                      /* SQL-2003-N */
%token  UDF_RETURNS_SYM
%token  ULONGLONG_NUM
%token  UNBOUNDED_SYM                 /* SQL-2011-N */
%token  UNCOMMITTED_SYM               /* SQL-2003-N */
%token  UNDEFINED_SYM
%token  UNDERSCORE_CHARSET
%token  UNDOFILE_SYM
%token  UNDO_BUFFER_SIZE_SYM
%token  UNDO_SYM                      /* FUTURE-USE */
%token  UNICODE_SYM
%token  UNINSTALL_SYM
%token  UNION_SYM                     /* SQL-2003-R */
%token  UNIQUE_SYM
%token  UNKNOWN_SYM                   /* SQL-2003-R */
%token  UNLOCK_SYM
%token  UNSIGNED
%token  UNTIL_SYM
%token  UPDATE_SYM                    /* SQL-2003-R */
%token  UPGRADE_SYM
%token  USAGE                         /* SQL-2003-N */
%token  USER_SYM                      /* SQL-2003-R */
%token  USE_FRM
%token  USE_SYM
%token  USING                         /* SQL-2003-R */
%token  UTC_DATE_SYM
%token  UTC_TIMESTAMP_SYM
%token  UTC_TIME_SYM
%token  VALUES                        /* SQL-2003-R */
%token  VALUE_SYM                     /* SQL-2003-R */
%token  VARBINARY
%token  VARCHAR                       /* SQL-2003-R */
%token  VARIABLES
%token  VARIANCE_SYM
%token  VARYING                       /* SQL-2003-R */
%token  VAR_SAMP_SYM
%token  VIA_SYM
%token  VIEW_SYM                      /* SQL-2003-N */
%token  VIRTUAL_SYM
%token  WAIT_SYM
%token  WARNINGS
%token  WEEK_SYM
%token  WEIGHT_STRING_SYM
%token  WHEN_SYM                      /* SQL-2003-R */
%token  WHERE                         /* SQL-2003-R */
%token  WINDOW_SYM
%token  WHILE_SYM
%token  WITH                          /* SQL-2003-R */
%token  WITH_CUBE_SYM                 /* INTERNAL */
%token  WITH_ROLLUP_SYM               /* INTERNAL */
%token  WORK_SYM                      /* SQL-2003-N */
%token  WRAPPER_SYM
%token  WRITE_SYM                     /* SQL-2003-N */
%token  X509_SYM
%token  XA_SYM
%token  XML_SYM
%token  XOR
%token  YEAR_MONTH_SYM
%token  YEAR_SYM                      /* SQL-2003-R */
%token  ZEROFILL

%token IMPOSSIBLE_ACTION		/* To avoid warning for yyerrlab1 */

/* A dummy token to force the priority of table_ref production in a join. */
%left   CONDITIONLESS_JOIN
%left   JOIN_SYM INNER_SYM STRAIGHT_JOIN CROSS LEFT RIGHT ON_SYM USING
%left   SET_VAR
%left   OR_OR_SYM OR_SYM OR2_SYM
%left   XOR
%left   AND_SYM AND_AND_SYM
%nonassoc NOT_SYM
%left   '=' EQUAL_SYM GE '>' LE '<' NE
%nonassoc IS
%right BETWEEN_SYM
%left   LIKE REGEXP IN_SYM
%left   '|'
%left   '&'
%left   SHIFT_LEFT SHIFT_RIGHT
%left   '-' '+'
%left   '*' '/' '%' DIV_SYM MOD_SYM
%left   '^'
%nonassoc NEG '~' NOT2_SYM BINARY
%nonassoc COLLATE_SYM
%left  INTERVAL_SYM

%type <lex_str>
        IDENT IDENT_QUOTED TEXT_STRING DECIMAL_NUM FLOAT_NUM NUM LONG_NUM
        HEX_NUM HEX_STRING
        LEX_HOSTNAME ULONGLONG_NUM field_ident select_alias ident ident_or_text
        IDENT_sys TEXT_STRING_sys TEXT_STRING_literal
        NCHAR_STRING opt_component key_cache_name
        sp_opt_label BIN_NUM label_ident TEXT_STRING_filesystem ident_or_empty
        opt_constraint constraint opt_ident ident_table_alias

%type <lex_str_ptr>
        opt_table_alias

%type <table>
        table_ident table_ident_nodb references xid
        table_ident_opt_wild create_like

%type <simple_string>
        remember_name remember_end opt_db
        remember_tok_start remember_tok_end
        wild_and_where
        field_length opt_field_length opt_field_length_default_1

%type <const_simple_string>
        opt_place

%type <string>
        text_string hex_or_bin_String opt_gconcat_separator

%type <field_type> int_type real_type

%type <Lex_field_type> field_type

%type <Lex_dyncol_type> opt_dyncol_type dyncol_type
        numeric_dyncol_type temporal_dyncol_type string_dyncol_type

%type <create_field> field_spec column_def

%type <geom_type> spatial_type

%type <num>
        order_dir lock_option
        udf_type opt_local opt_no_write_to_binlog
        opt_temporary all_or_any opt_distinct
        opt_ignore_leaves fulltext_options union_option
        opt_not
        select_derived_init transaction_access_mode_types
        opt_natural_language_mode opt_query_expansion
        opt_ev_status opt_ev_on_completion ev_on_completion opt_ev_comment
        ev_alter_on_schedule_completion opt_ev_rename_to opt_ev_sql_stmt
        optional_flush_tables_arguments
        opt_time_precision kill_type kill_option int_num
        opt_default_time_precision
        case_stmt_body opt_bin_mod
        opt_if_exists_table_element opt_if_not_exists_table_element
	opt_recursive

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
        ws_nweights
        ws_level_flag_desc ws_level_flag_reverse ws_level_flags
        opt_ws_levels ws_level_list ws_level_list_item ws_level_number
        ws_level_range ws_level_list_or_range  

%type <ulonglong_number>
        ulonglong_num real_ulonglong_num size_number

%type <choice> choice

%type <lock_type>
        replace_lock_option opt_low_priority insert_lock_option load_data_lock

%type <item>
        literal text_literal insert_ident order_ident temporal_literal
        simple_ident expr opt_expr opt_else sum_expr in_sum_expr
        variable variable_aux
        predicate bit_expr parenthesized_expr
        table_wild simple_expr column_default_non_parenthesized_expr udf_expr
        expr_or_default set_expr_or_default
        geometry_function signed_literal expr_or_literal
        sp_opt_default
        simple_ident_nospvar simple_ident_q
        field_or_var limit_option
        part_func_expr
        window_func_expr
        window_func
        simple_window_func
        function_call_keyword
        function_call_nonkeyword
        function_call_generic
        function_call_conflict kill_expr
        signal_allowed_expr
        simple_target_specification
        condition_number

%type <item_param> param_marker

%type <item_num>
        NUM_literal

%type <item_list>
        expr_list opt_udf_expr_list udf_expr_list when_list
        ident_list ident_list_arg opt_expr_list

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
        select_derived derived_table_list
        select_derived_union
        derived_query_specification
%type <date_time_type> date_time_type;
%type <interval> interval

%type <interval_time_st> interval_time_stamp

%type <db_type> storage_engines known_storage_engines

%type <row_type> row_types

%type <tx_isolation> isolation_types

%type <ha_rkey_mode> handler_rkey_mode

%type <Lex_cast_type> cast_type cast_type_numeric cast_type_temporal

%type <Lex_length_and_dec> precision opt_precision float_options

%type <symbol> keyword keyword_sp keyword_alias

%type <lex_user> user grant_user grant_role user_or_role current_role
                 admin_option_for_role user_maybe_role

%type <charset>
        opt_collate
        collate
        charset_name
        charset_or_alias
        charset_name_or_default
        old_or_new_charset_name
        old_or_new_charset_name_or_default
        collation_name
        collation_name_or_default
        opt_load_data_charset
        UNDERSCORE_CHARSET

%type <variable> internal_variable_name

%type <select_lex> subselect
        get_select_lex get_select_lex_derived
        query_specification
        query_term_union_not_ready
        query_term_union_ready
        query_expression_body
        select_paren_derived

%type <boolfunc2creator> comp_op

%type <dyncol_def> dyncall_create_element

%type <dyncol_def_list> dyncall_create_list

%type <myvar> select_outvar

%type <virtual_column> opt_check_constraint check_constraint virtual_column_func
        column_default_expr

%type <NONE>
        analyze_stmt_command
        query verb_clause create change select do drop insert replace insert2
        insert_values update delete truncate rename compound_statement
        show describe load alter optimize keycache preload flush
        reset purge begin commit rollback savepoint release
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
        opt_limit_clause delete_limit_clause fields opt_values values
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
        union_clause union_list
        subselect_start opt_and charset
        subselect_end select_var_list select_var_list_init help 
        opt_extended_describe shutdown
        opt_format_json
        prepare prepare_src execute deallocate
        statement sp_suid
        sp_c_chistics sp_a_chistics sp_chistic sp_c_chistic xa
        opt_field_or_var_spec fields_or_vars opt_load_data_set_spec
        view_algorithm view_or_trigger_or_sp_or_event
        definer_tail no_definer_tail
        view_suid view_tail view_list_opt view_list view_select
        view_check_option trigger_tail sp_tail sf_tail event_tail
        udf_tail udf_tail2
        install uninstall partition_entry binlog_base64_event
        normal_key_options normal_key_opts all_key_opt 
        spatial_key_options fulltext_key_options normal_key_opt 
        fulltext_key_opt spatial_key_opt fulltext_key_opts spatial_key_opts
	keep_gcc_happy
        key_using_alg
        part_column_list
        server_def server_options_list server_option
        definer_opt no_definer definer get_diagnostics
        parse_vcol_expr vcol_opt_specifier vcol_opt_attribute
        vcol_opt_attribute_list vcol_attribute
        opt_serial_attribute opt_serial_attribute_list serial_attribute
        explainable_command
        opt_delete_gtid_domain
END_OF_INPUT

%type <NONE> call sp_proc_stmts sp_proc_stmts1 sp_proc_stmt
%type <NONE> sp_proc_stmt_statement sp_proc_stmt_return
             sp_proc_stmt_in_returns_clause
%type <NONE> sp_proc_stmt_compound_ok
%type <NONE> sp_proc_stmt_if
%type <NONE> sp_labeled_control sp_unlabeled_control
%type <NONE> sp_labeled_block sp_unlabeled_block sp_unlabeled_block_not_atomic
%type <NONE> sp_proc_stmt_leave
%type <NONE> sp_proc_stmt_iterate
%type <NONE> sp_proc_stmt_open sp_proc_stmt_fetch sp_proc_stmt_close
%type <NONE> case_stmt_specification
%type <NONE> loop_body while_body repeat_body

%type <num>  sp_decl_idents sp_handler_type sp_hcond_list
%type <spcondvalue> sp_cond sp_hcond sqlstate signal_value opt_signal_value
%type <spblock> sp_decls sp_decl
%type <lex> sp_cursor_stmt
%type <spname> sp_name
%type <splabel> sp_block_content
%type <spvar> sp_param_name_and_type
%type <spvar_mode> sp_opt_inout
%type <index_hint> index_hint_type
%type <num> index_hint_clause normal_join inner_join
%type <filetype> data_or_xml

%type <NONE> signal_stmt resignal_stmt
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

%type <NONE> opt_window_clause window_def_list window_def window_spec
%type <lex_str_ptr> window_name
%type <NONE> opt_window_ref opt_window_frame_clause
%type <frame_units> window_frame_units;
%type <NONE> window_frame_extent;
%type <frame_exclusion> opt_window_frame_exclusion;
%type <window_frame_bound> window_frame_start window_frame_bound;


%type <NONE>
        '-' '+' '*' '/' '%' '(' ')'
        ',' '!' '{' '}' '&' '|' AND_SYM OR_SYM OR_OR_SYM BETWEEN_SYM CASE_SYM
        THEN_SYM WHEN_SYM DIV_SYM MOD_SYM OR2_SYM AND_AND_SYM DELETE_SYM
        ROLE_SYM

%type <with_clause> opt_with_clause with_clause

%type <with_element_head> with_element_head

%type <lex_str_list> opt_with_column_list

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
              (!(thd->lex->select_lex.options & OPTION_FOUND_COMMENT)))
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
        | begin
        | compound_statement
        ;

/* Verb clauses, except begin and compound_statement */
statement:
          alter
        | analyze
        | analyze_stmt_command
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
            if (lex->table_or_sp_used())
              my_yyabort_error((ER_SUBQUERIES_NOT_SUPPORTED, MYF(0),
                               "PREPARE..FROM"));
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
          {}
        | EXECUTE_SYM IMMEDIATE_SYM prepare_src
          {
            if (Lex->table_or_sp_used())
              my_yyabort_error((ER_SUBQUERIES_NOT_SUPPORTED, MYF(0),
                               "EXECUTE IMMEDIATE"));
            Lex->sql_command= SQLCOM_EXECUTE_IMMEDIATE;
          }
          execute_using
          {}
        ;

execute_using:
          /* nothing */
        | USING            { Lex->expr_allows_subselect= false; }
          execute_var_list
          {
            if (Lex->table_or_sp_used())
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
            if (Lex->prepared_stmt_params.push_back($1, thd->mem_root))
              MYSQL_YYABORT;
          }
        ;

/* help */

help:
          HELP_SYM
          {
            if (Lex->sphead)
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
                       (uint) $3, (uint) MASTER_DELAY_MAX);
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
            if (Lex->mi.heartbeat_period > SLAVE_MAX_HEARTBEAT_PERIOD ||
                Lex->mi.heartbeat_period < 0.0)
               my_yyabort_error((ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE, MYF(0),
                                 SLAVE_MAX_HEARTBEAT_PERIOD));

            if (Lex->mi.heartbeat_period > slave_net_timeout)
            {
              push_warning_printf(thd, Sql_condition::WARN_LEVEL_WARN,
                                  ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE_MAX,
                                  ER_THD(thd, ER_SLAVE_HEARTBEAT_VALUE_OUT_OF_RANGE_MAX));
            }
            if (Lex->mi.heartbeat_period < 0.001)
            {
              if (Lex->mi.heartbeat_period != 0.0)
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
            if (Lex->mi.use_gtid_opt != LEX_MASTER_INFO::LEX_GTID_UNCHANGED)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MASTER_use_gtid"));
            Lex->mi.use_gtid_opt= LEX_MASTER_INFO::LEX_GTID_CURRENT_POS;
          }
        | MASTER_USE_GTID_SYM '=' SLAVE_POS_SYM
          {
            if (Lex->mi.use_gtid_opt != LEX_MASTER_INFO::LEX_GTID_UNCHANGED)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MASTER_use_gtid"));
            Lex->mi.use_gtid_opt= LEX_MASTER_INFO::LEX_GTID_SLAVE_POS;
          }
        | MASTER_USE_GTID_SYM '=' NO_SYM
          {
            if (Lex->mi.use_gtid_opt != LEX_MASTER_INFO::LEX_GTID_UNCHANGED)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "MASTER_use_gtid"));
            Lex->mi.use_gtid_opt= LEX_MASTER_INFO::LEX_GTID_NO;
          }
        ;

optional_connection_name:
          /* empty */
          {
            LEX *lex= thd->lex;
            lex->mi.connection_name= null_lex_str;
          }
        | connection_name
        ;

connection_name:
        TEXT_STRING_sys
        {
           Lex->mi.connection_name= $1;
#ifdef HAVE_REPLICATION
           if (check_master_connection_name(&$1))
              my_yyabort_error((ER_WRONG_ARGUMENTS, MYF(0), "MASTER_CONNECTION_NAME"));
#endif
         }
         ;

/* create a table */

create:
          create_or_replace opt_temporary TABLE_SYM opt_if_not_exists table_ident
          {
            LEX *lex= thd->lex;
            if (!(lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_create_table()))
              MYSQL_YYABORT;
            lex->create_info.init();
            if (lex->set_command_with_check(SQLCOM_CREATE_TABLE, $2, $1 | $4))
               MYSQL_YYABORT;
            if (!lex->select_lex.add_table_to_list(thd, $5, NULL,
                                                   TL_OPTION_UPDATING,
                                                   TL_WRITE, MDL_EXCLUSIVE))
              MYSQL_YYABORT;
            lex->alter_info.reset();
            /*
              For CREATE TABLE we should not open the table even if it exists.
              If the table exists, we should either not create it or replace it
            */
            lex->query_tables->open_strategy= TABLE_LIST::OPEN_STUB;
            lex->create_info.default_table_charset= NULL;
            lex->name= null_lex_str;
            lex->create_last_non_select_table= lex->last_table();
          }
          create_body
          {
            LEX *lex= thd->lex;
            lex->current_select= &lex->select_lex; 
            create_table_set_open_action_and_adjust_tables(lex);
          }
        | create_or_replace opt_unique INDEX_SYM opt_if_not_exists ident
          opt_key_algorithm_clause
          ON table_ident
          {
            if (add_create_index_prepare(Lex, $8))
              MYSQL_YYABORT;
            if (Lex->add_create_index($2, $5, $6, $1 | $4))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options
          opt_index_lock_algorithm { }
        | create_or_replace fulltext INDEX_SYM opt_if_not_exists ident
          ON table_ident
          {
            if (add_create_index_prepare(Lex, $7))
              MYSQL_YYABORT;
            if (Lex->add_create_index($2, $5, HA_KEY_ALG_UNDEF, $1 | $4))
              MYSQL_YYABORT;
          }
          '(' key_list ')' fulltext_key_options
          opt_index_lock_algorithm { }
        | create_or_replace spatial INDEX_SYM opt_if_not_exists ident
          ON table_ident
          {
            if (add_create_index_prepare(Lex, $7))
              MYSQL_YYABORT;
            if (Lex->add_create_index($2, $5, HA_KEY_ALG_UNDEF, $1 | $4))
              MYSQL_YYABORT;
          }
          '(' key_list ')' spatial_key_options
          opt_index_lock_algorithm { }
        | create_or_replace DATABASE opt_if_not_exists ident
          {
            Lex->create_info.default_table_charset= NULL;
            Lex->create_info.used_fields= 0;
          }
          opt_create_database_options
          {
            LEX *lex=Lex;
            if (lex->set_command_with_check(SQLCOM_CREATE_DB, 0, $1 | $3))
               MYSQL_YYABORT;
            lex->name= $4;
          }
        | create_or_replace
          {
            Lex->create_info.set($1);
            Lex->create_view_mode= ($1.or_replace() ? VIEW_CREATE_OR_REPLACE :
                                                      VIEW_CREATE_NEW);
            Lex->create_view_algorithm= DTYPE_ALGORITHM_UNDEFINED;
            Lex->create_view_suid= TRUE;
          }
          view_or_trigger_or_sp_or_event { }
        | create_or_replace USER_SYM opt_if_not_exists clear_privileges grant_list
          opt_require_clause opt_resource_options
          {
            if (Lex->set_command_with_check(SQLCOM_CREATE_USER, $1 | $3))
              MYSQL_YYABORT;
          }
        | create_or_replace ROLE_SYM opt_if_not_exists
          clear_privileges role_list opt_with_admin
          {
            if (Lex->set_command_with_check(SQLCOM_CREATE_ROLE, $1 | $3))
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

server_def:
          SERVER_SYM opt_if_not_exists ident_or_text
          {
            if (Lex->add_create_options_with_check($2))
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

/* this rule is used to force look-ahead in the parser */
force_lookahead: {} | FORCE_LOOKAHEAD {} ;

event_tail:
          remember_name EVENT_SYM opt_if_not_exists sp_name
          {
            LEX *lex=Lex;

            lex->stmt_definition_begin= $1;
            if (lex->add_create_options_with_check($3))
              MYSQL_YYABORT;
            if (!(lex->event_parse_data= Event_parse_data::new_instance(thd)))
              MYSQL_YYABORT;
            lex->event_parse_data->identifier= $4;
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
            if (item == NULL)
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
            if (lex->sphead)
              my_yyabort_error((ER_EVENT_RECURSION_FORBIDDEN, MYF(0)));
              
            if (!make_sp_head(thd, lex->event_parse_data->identifier, TYPE_ENUM_PROCEDURE))
              MYSQL_YYABORT;

            lex->sp_chistics.suid= SP_IS_SUID;  //always the definer!
            lex->sphead->set_body_start(thd, lip->get_cpp_ptr());
          }
          sp_proc_stmt force_lookahead
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
           lex->select_lex.db= 0;
           lex->ssl_type= SSL_TYPE_NOT_SPECIFIED;
           lex->ssl_cipher= lex->x509_subject= lex->x509_issuer= 0;
           bzero((char *)&(lex->mqh),sizeof(lex->mqh));
         }
        ;

sp_name:
          ident '.' ident
          {
            if (!$1.str || check_db_name(&$1))
              my_yyabort_error((ER_WRONG_DB_NAME, MYF(0), $1.str));
            if (check_routine_name(&$3))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) sp_name($1, $3, true);
            if ($$ == NULL)
              MYSQL_YYABORT;
            $$->init_qname(thd);
          }
        | ident
          {
            LEX *lex= thd->lex;
            LEX_STRING db;
            if (check_routine_name(&$1))
            {
              MYSQL_YYABORT;
            }
            if (lex->copy_db_to(&db.str, &db.length))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) sp_name(db, $1, false);
            if ($$ == NULL)
              MYSQL_YYABORT;
            $$->init_qname(thd);
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
            LEX *lex = Lex;

            lex->sql_command= SQLCOM_CALL;
            lex->spname= $2;
            lex->value_list.empty();
            sp_add_used_routine(lex, thd, $2, TYPE_ENUM_PROCEDURE);
          }
          opt_sp_cparam_list
          {
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;
          }
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

sp_param_name_and_type:
          ident
          {
            LEX *lex= Lex;
            sp_pcontext *spc= lex->spcont;

            if (spc->find_variable($1, TRUE))
              my_yyabort_error((ER_SP_DUP_PARAM, MYF(0), $1.str));

            sp_variable *spvar= spc->add_variable(thd, $1);

            lex->init_last_field(&spvar->field_def, $1.str,
                                 thd->variables.collation_database);
            $<spvar>$= spvar;
          }
          field_type
          {
            LEX *lex= Lex;
            sp_variable *spvar= $<spvar>2;

            Lex->set_last_field_type($3);
            if (lex->sphead->fill_field_definition(thd, lex, lex->last_field))
            {
              MYSQL_YYABORT;
            }
            spvar->field_def.field_name= spvar->name.str;
            spvar->field_def.pack_flag |= FIELDFLAG_MAYBE_NULL;

            $$= spvar;
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
            $$.vars= $$.conds= $$.hndlrs= $$.curs= 0;
          }
        | sp_decls sp_decl ';'
          {
            /* We check for declarations out of (standard) order this way
              because letting the grammar rules reflect it caused tricky
               shift/reduce conflicts with the wrong result. (And we get
               better error handling this way.) */
            if (($2.vars || $2.conds) && ($1.curs || $1.hndlrs))
              my_yyabort_error((ER_SP_VARCOND_AFTER_CURSHNDLR, MYF(0)));
            if ($2.curs && $1.hndlrs)
              my_yyabort_error((ER_SP_CURSOR_AFTER_HANDLER, MYF(0)));
            $$.vars= $1.vars + $2.vars;
            $$.conds= $1.conds + $2.conds;
            $$.hndlrs= $1.hndlrs + $2.hndlrs;
            $$.curs= $1.curs + $2.curs;
          }
        ;

sp_decl:
          DECLARE_SYM sp_decl_idents
          {
            LEX *lex= Lex;
            sp_pcontext *pctx= lex->spcont;

            // get the last variable:
            uint num_vars= pctx->context_var_count();
            uint var_idx= pctx->var_context2runtime(num_vars - 1);
            sp_variable *spvar= pctx->find_variable(var_idx);

            lex->sphead->reset_lex(thd);
            pctx->declare_var_boundary($2);
            thd->lex->init_last_field(&spvar->field_def, spvar->name.str,
                                      thd->variables.collation_database);
          }
          field_type
          sp_opt_default
          {
            LEX *lex= Lex;
            sp_pcontext *pctx= lex->spcont;
            uint num_vars= pctx->context_var_count();
            Item *dflt_value_item= $5;
            const bool has_default_clause = (dflt_value_item != NULL);
            Lex->set_last_field_type($4);


            if (!has_default_clause)
            {
              dflt_value_item= new (thd->mem_root) Item_null(thd);
              if (dflt_value_item == NULL)
                MYSQL_YYABORT;
              /* QQ Set to the var_type with null_value? */
            }

            sp_variable *first_spvar = NULL;
            const uint first_var_num = num_vars - $2;

            for (uint i = first_var_num ; i < num_vars ; i++)
            {
              uint var_idx= pctx->var_context2runtime(i);
              sp_variable *spvar= pctx->find_variable(var_idx);
              bool last= i == num_vars - 1;
            
              if (!spvar)
                MYSQL_YYABORT;
            
              if (!last)
                spvar->field_def= *lex->last_field;

              if (i == first_var_num) {
                first_spvar = spvar;
              } else if (has_default_clause) {
                Item_splocal *item =
                  new (thd->mem_root)
                    Item_splocal(thd, first_spvar->name, first_spvar->offset,
                                       first_spvar->sql_type(), 0, 0);
                if (item == NULL)
                  MYSQL_YYABORT; // OOM
#ifndef DBUG_OFF
                item->m_sp = lex->sphead;
#endif
                dflt_value_item = item;
              }

              spvar->default_value= dflt_value_item;
              spvar->field_def.field_name= spvar->name.str;

              if (lex->sphead->fill_field_definition(thd, lex,
                                                     &spvar->field_def))
              {
                MYSQL_YYABORT;
              }
            
              spvar->field_def.pack_flag |= FIELDFLAG_MAYBE_NULL;
            
              /* The last instruction is responsible for freeing LEX. */

              sp_instr_set *is= new (lex->thd->mem_root)
                                 sp_instr_set(lex->sphead->instructions(),
                                               pctx, var_idx, dflt_value_item,
                                               $4.field_type(), lex, last);
              if (is == NULL || lex->sphead->add_instr(is))
                MYSQL_YYABORT;
            }

            pctx->declare_var_boundary(0);
            if (lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
            $$.vars= $2;
            $$.conds= $$.hndlrs= $$.curs= 0;
          }
        | DECLARE_SYM ident CONDITION_SYM FOR_SYM sp_cond
          {
            LEX *lex= Lex;
            sp_pcontext *spc= lex->spcont;

            if (spc->find_condition($2, TRUE))
              my_yyabort_error((ER_SP_DUP_COND, MYF(0), $2.str));
            if(spc->add_condition(thd, $2, $5))
              MYSQL_YYABORT;
            $$.vars= $$.hndlrs= $$.curs= 0;
            $$.conds= 1;
          }
        | DECLARE_SYM sp_handler_type HANDLER_SYM FOR_SYM
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;

            sp_handler *h= lex->spcont->add_handler(thd,
                                                    (sp_handler::enum_type) $2);

            lex->spcont= lex->spcont->push_context(thd,
                                                   sp_pcontext::HANDLER_SCOPE);

            sp_pcontext *ctx= lex->spcont;
            sp_instr_hpush_jump *i=
              new (thd->mem_root) sp_instr_hpush_jump(sp->instructions(),
                   ctx, h);

            if (i == NULL || sp->add_instr(i))
              MYSQL_YYABORT;

            /* For continue handlers, mark end of handler scope. */
            if ($2 == sp_handler::CONTINUE &&
                sp->push_backpatch(thd, i, ctx->last_label()))
              MYSQL_YYABORT;

            if (sp->push_backpatch(thd, i, ctx->push_label(thd, empty_lex_str, 0)))
              MYSQL_YYABORT;
          }
          sp_hcond_list sp_proc_stmt
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *ctx= lex->spcont;
            sp_label *hlab= lex->spcont->pop_label(); /* After this hdlr */
            sp_instr_hreturn *i;

            if ($2 == sp_handler::CONTINUE)
            {
              i= new (thd->mem_root)
                 sp_instr_hreturn(sp->instructions(), ctx);
              if (i == NULL ||
                  sp->add_instr(i))
                MYSQL_YYABORT;
            }
            else
            {  /* EXIT or UNDO handler, just jump to the end of the block */
              i= new (thd->mem_root)
                 sp_instr_hreturn(sp->instructions(), ctx);
              if (i == NULL ||
                  sp->add_instr(i) ||
                  sp->push_backpatch(thd, i, lex->spcont->last_label())) /* Block end */
                MYSQL_YYABORT;
            }
            lex->sphead->backpatch(hlab);

            lex->spcont= ctx->pop_context();

            $$.vars= $$.conds= $$.curs= 0;
            $$.hndlrs= 1;
          }
        | DECLARE_SYM ident CURSOR_SYM FOR_SYM sp_cursor_stmt
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *ctx= lex->spcont;
            uint offp;
            sp_instr_cpush *i;

            if (ctx->find_cursor($2, &offp, TRUE))
              my_yyabort_error((ER_SP_DUP_CURS, MYF(0), $2.str));

            i= new (thd->mem_root)
                 sp_instr_cpush(sp->instructions(), ctx, $5,
                                ctx->current_cursor_count());
            if (i == NULL || sp->add_instr(i) || ctx->add_cursor($2))
              MYSQL_YYABORT;
            $$.vars= $$.conds= $$.hndlrs= 0;
            $$.curs= 1;
          }
        ;

sp_cursor_stmt:
          {
            Lex->sphead->reset_lex(thd);
          }
          select
          {
            LEX *lex= Lex;

            DBUG_ASSERT(lex->sql_command == SQLCOM_SELECT);

            if (lex->result)
              my_yyabort_error((ER_SP_BAD_CURSOR_SELECT, MYF(0)));
            lex->sp_lex_in_use= TRUE;
            $$= lex;
            if (lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
          }
        ;

sp_handler_type:
          EXIT_SYM      { $$= sp_handler::EXIT; }
        | CONTINUE_SYM  { $$= sp_handler::CONTINUE; }
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

            if (ctx->check_duplicate_handler($1))
              my_yyabort_error((ER_SP_DUP_HANDLER, MYF(0)));

            sp_instr_hpush_jump *i= (sp_instr_hpush_jump *)sp->last_instruction();
            i->add_condition($1);
          }
        ;

sp_cond:
          ulong_num
          { /* mysql errno */
            if ($1 == 0)
              my_yyabort_error((ER_WRONG_VALUE, MYF(0), "CONDITION", "0"));
            $$= new (thd->mem_root) sp_condition_value($1);
            if ($$ == NULL)
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
            if (!is_sqlstate_valid(&$3) || is_sqlstate_completion($3.str))
              my_yyabort_error((ER_SP_BAD_SQLSTATE, MYF(0), $3.str));
            $$= new (thd->mem_root) sp_condition_value($3.str);
            if ($$ == NULL)
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
            $$= Lex->spcont->find_condition($1, false);
            if ($$ == NULL)
              my_yyabort_error((ER_SP_COND_MISMATCH, MYF(0), $1.str));
          }
        | SQLWARNING_SYM /* SQLSTATEs 01??? */
          {
            $$= new (thd->mem_root) sp_condition_value(sp_condition_value::WARNING);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | not FOUND_SYM /* SQLSTATEs 02??? */
          {
            $$= new (thd->mem_root) sp_condition_value(sp_condition_value::NOT_FOUND);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SQLEXCEPTION_SYM /* All other SQLSTATEs */
          {
            $$= new (thd->mem_root) sp_condition_value(sp_condition_value::EXCEPTION);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

signal_stmt:
          SIGNAL_SYM signal_value opt_set_signal_information
          {
            LEX *lex= thd->lex;
            Yacc_state *state= & thd->m_parser_state->m_yacc;

            lex->sql_command= SQLCOM_SIGNAL;
            lex->m_sql_cmd=
              new (thd->mem_root) Sql_cmd_signal($2, state->m_set_signal_info);
            if (lex->m_sql_cmd == NULL)
              MYSQL_YYABORT;
          }
        ;

signal_value:
          ident
          {
            LEX *lex= Lex;
            sp_condition_value *cond;

            /* SIGNAL foo cannot be used outside of stored programs */
            if (lex->spcont == NULL)
              my_yyabort_error((ER_SP_COND_MISMATCH, MYF(0), $1.str));
            cond= lex->spcont->find_condition($1, false);
            if (cond == NULL)
              my_yyabort_error((ER_SP_COND_MISMATCH, MYF(0), $1.str));
            if (cond->type != sp_condition_value::SQLSTATE)
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
            if (info->m_item[index] != NULL)
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
              if (item->functype() == Item_func::SUSERVAR_FUNC)
              {
                /*
                  Don't allow the following syntax:
                    SIGNAL/RESIGNAL ...
                    SET <signal condition item name> = @foo := expr
                */
                my_parse_error(thd, ER_SYNTAX_ERROR);
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
            LEX *lex= thd->lex;
            Yacc_state *state= & thd->m_parser_state->m_yacc;

            lex->sql_command= SQLCOM_RESIGNAL;
            lex->m_sql_cmd=
              new (thd->mem_root) Sql_cmd_resignal($2,
                                                   state->m_set_signal_info);
            if (lex->m_sql_cmd == NULL)
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

            if (Lex->m_sql_cmd == NULL)
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
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | CONDITION_SYM condition_number condition_information
          {
            $$= new (thd->mem_root) Condition_information($2, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

statement_information:
          statement_information_item
          {
            $$= new (thd->mem_root) List<Statement_information_item>;
            if ($$ == NULL || $$->push_back($1, thd->mem_root))
              MYSQL_YYABORT;
          }
        | statement_information ',' statement_information_item
          {
            if ($1->push_back($3, thd->mem_root))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

statement_information_item:
          simple_target_specification '=' statement_information_item_name
          {
            $$= new (thd->mem_root) Statement_information_item($3, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

simple_target_specification:
          ident
          {
            Lex_input_stream *lip= &thd->m_parser_state->m_lip;
            $$= create_item_for_sp_var(thd, $1, NULL,
                                       lip->get_tok_start(), lip->get_ptr());

            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | '@' ident_or_text
          {
            $$= new (thd->mem_root) Item_func_get_user_var(thd, $2);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

statement_information_item_name:
          NUMBER_SYM
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
            if ($$ == NULL || $$->push_back($1, thd->mem_root))
              MYSQL_YYABORT;
          }
        | condition_information ',' condition_information_item
          {
            if ($1->push_back($3, thd->mem_root))
              MYSQL_YYABORT;
            $$= $1;
          }
        ;

condition_information_item:
          simple_target_specification '=' condition_information_item_name
          {
            $$= new (thd->mem_root) Condition_information_item($3, $1);
            if ($$ == NULL)
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

sp_decl_idents:
          ident
          {
            /* NOTE: field definition is filled in sp_decl section. */

            LEX *lex= Lex;
            sp_pcontext *spc= lex->spcont;

            if (spc->find_variable($1, TRUE))
              my_yyabort_error((ER_SP_DUP_VAR, MYF(0), $1.str));
            spc->add_variable(thd, $1);
            $$= 1;
          }
        | sp_decl_idents ',' ident
          {
            /* NOTE: field definition is filled in sp_decl section. */

            LEX *lex= Lex;
            sp_pcontext *spc= lex->spcont;

            if (spc->find_variable($3, TRUE))
              my_yyabort_error((ER_SP_DUP_VAR, MYF(0), $3.str));
            spc->add_variable(thd, $3);
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
        | sp_proc_stmt_leave
        | sp_proc_stmt_iterate
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
            if (maybe_start_compound_statement(thd))
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
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;
            sp_head *sp= lex->sphead;

            sp->m_flags|= sp_get_flags_for_command(lex);
            /* "USE db" doesn't work in a procedure */
            if (lex->sql_command == SQLCOM_CHANGE_DB)
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "USE"));
            /*
              Don't add an instruction for SET statements, since all
              instructions for them were already added during processing
              of "set" rule.
            */
            DBUG_ASSERT(lex->sql_command != SQLCOM_SET_OPTION ||
                        lex->var_list.is_empty());
            if (lex->sql_command != SQLCOM_SET_OPTION)
            {
              sp_instr_stmt *i=new (thd->mem_root)
                sp_instr_stmt(sp->instructions(), lex->spcont, lex);
              if (i == NULL)
                MYSQL_YYABORT;

              /*
                Extract the query statement from the tokenizer.  The
                end is either lex->ptr, if there was no lookahead,
                lex->tok_end otherwise.
              */
              if (yychar == YYEMPTY)
                i->m_query.length= lip->get_ptr() - sp->m_tmp_query;
              else
                i->m_query.length= lip->get_tok_start() - sp->m_tmp_query;;
              if (!(i->m_query.str= strmake_root(thd->mem_root,
                                                 sp->m_tmp_query,
                                                 i->m_query.length)) ||
                    sp->add_instr(i))
                MYSQL_YYABORT;
            }
            if (sp->restore_lex(thd))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_return:
          RETURN_SYM 
          { Lex->sphead->reset_lex(thd); }
          expr
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;

            if (sp->m_type != TYPE_ENUM_FUNCTION)
              my_yyabort_error((ER_SP_BADRETURN, MYF(0)));

            sp_instr_freturn *i;

            i= new (thd->mem_root)
                 sp_instr_freturn(sp->instructions(), lex->spcont, $3,
                                  sp->m_return_field_def.sql_type, lex);
            if (i == NULL || sp->add_instr(i))
              MYSQL_YYABORT;
            sp->m_flags|= sp_head::HAS_RETURN;

            if (sp->restore_lex(thd))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_leave:
          LEAVE_SYM label_ident
          {
            LEX *lex= Lex;
            sp_head *sp = lex->sphead;
            sp_pcontext *ctx= lex->spcont;
            sp_label *lab= ctx->find_label($2);

            if (! lab)
              my_yyabort_error((ER_SP_LILABEL_MISMATCH, MYF(0), "LEAVE", $2.str));

            sp_instr_jump *i;
            uint ip= sp->instructions();
            uint n;
            /*
              When jumping to a BEGIN-END block end, the target jump
              points to the block hpop/cpop cleanup instructions,
              so we should exclude the block context here.
              When jumping to something else (i.e., SP_LAB_ITER),
              there are no hpop/cpop at the jump destination,
              so we should include the block context here for cleanup.
            */
            bool exclusive= (lab->type == sp_label::BEGIN);

            n= ctx->diff_handlers(lab->ctx, exclusive);
            if (n)
            {
              sp_instr_hpop *hpop= new (thd->mem_root)
                sp_instr_hpop(ip++, ctx, n);
              if (hpop == NULL)
                MYSQL_YYABORT;
              sp->add_instr(hpop);
            }
            n= ctx->diff_cursors(lab->ctx, exclusive);
            if (n)
            {
              sp_instr_cpop *cpop= new (thd->mem_root)
                sp_instr_cpop(ip++, ctx, n);
              if (cpop == NULL)
                MYSQL_YYABORT;
              sp->add_instr(cpop);
            }
            i= new (thd->mem_root) sp_instr_jump(ip, ctx);
            if (i == NULL)
              MYSQL_YYABORT;
            sp->push_backpatch(thd, i, lab);  /* Jumping forward */
            sp->add_instr(i);
          }
        ;

sp_proc_stmt_iterate:
          ITERATE_SYM label_ident
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *ctx= lex->spcont;
            sp_label *lab= ctx->find_label($2);

            if (! lab || lab->type != sp_label::ITERATION)
              my_yyabort_error((ER_SP_LILABEL_MISMATCH, MYF(0), "ITERATE", $2.str));

            sp_instr_jump *i;
            uint ip= sp->instructions();
            uint n;

            n= ctx->diff_handlers(lab->ctx, FALSE);  /* Inclusive the dest. */
            if (n)
            {
              sp_instr_hpop *hpop= new (thd->mem_root)
                sp_instr_hpop(ip++, ctx, n);
              if (hpop == NULL ||
                  sp->add_instr(hpop))
                MYSQL_YYABORT;
            }
            n= ctx->diff_cursors(lab->ctx, FALSE);  /* Inclusive the dest. */
            if (n)
            {
              sp_instr_cpop *cpop= new (thd->mem_root)
                sp_instr_cpop(ip++, ctx, n);
              if (cpop == NULL ||
                  sp->add_instr(cpop))
                MYSQL_YYABORT;
            }
            i= new (thd->mem_root)
              sp_instr_jump(ip, ctx, lab->ip); /* Jump back */
            if (i == NULL ||
                sp->add_instr(i))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_open:
          OPEN_SYM ident
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            uint offset;
            sp_instr_copen *i;

            if (! lex->spcont->find_cursor($2, &offset, false))
              my_yyabort_error((ER_SP_CURSOR_MISMATCH, MYF(0), $2.str));
            i= new (thd->mem_root)
              sp_instr_copen(sp->instructions(), lex->spcont, offset);
            if (i == NULL ||
                sp->add_instr(i))
              MYSQL_YYABORT;
          }
        ;

sp_proc_stmt_fetch:
          FETCH_SYM sp_opt_fetch_noise ident INTO
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            uint offset;
            sp_instr_cfetch *i;

            if (! lex->spcont->find_cursor($3, &offset, false))
              my_yyabort_error((ER_SP_CURSOR_MISMATCH, MYF(0), $3.str));
            i= new (thd->mem_root)
              sp_instr_cfetch(sp->instructions(), lex->spcont, offset);
            if (i == NULL ||
                sp->add_instr(i))
              MYSQL_YYABORT;
          }
          sp_fetch_list
          {}
        ;

sp_proc_stmt_close:
          CLOSE_SYM ident
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            uint offset;
            sp_instr_cclose *i;

            if (! lex->spcont->find_cursor($2, &offset, false))
              my_yyabort_error((ER_SP_CURSOR_MISMATCH, MYF(0), $2.str));
            i= new (thd->mem_root)
              sp_instr_cclose(sp->instructions(), lex->spcont,  offset);
            if (i == NULL ||
                sp->add_instr(i))
              MYSQL_YYABORT;
          }
        ;

sp_opt_fetch_noise:
          /* Empty */
        | NEXT_SYM FROM
        | FROM
        ;

sp_fetch_list:
          ident
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *spc= lex->spcont;
            sp_variable *spv;

            if (!spc || !(spv = spc->find_variable($1, false)))
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

            if (!spc || !(spv = spc->find_variable($3, false)))
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
            if (i == NULL ||
                sp->push_backpatch(thd, i, ctx->push_label(thd, empty_lex_str, 0)) ||
                sp->add_cont_backpatch(i) ||
                sp->add_instr(i))
              MYSQL_YYABORT;
            if (sp->restore_lex(thd))
              MYSQL_YYABORT;
          }
          sp_proc_stmts1
          {
            sp_head *sp= Lex->sphead;
            sp_pcontext *ctx= Lex->spcont;
            uint ip= sp->instructions();
            sp_instr_jump *i= new (thd->mem_root) sp_instr_jump(ip, ctx);
            if (i == NULL ||
                sp->add_instr(i))
              MYSQL_YYABORT;
            sp->backpatch(ctx->pop_label());
            sp->push_backpatch(thd, i, ctx->push_label(thd, empty_lex_str, 0));
          }
          sp_elseifs
          {
            LEX *lex= Lex;

            lex->sphead->backpatch(lex->spcont->pop_label());
          }
        ;

sp_elseifs:
          /* Empty */
        | ELSEIF_SYM sp_if
        | ELSE sp_proc_stmts1
        ;

case_stmt_specification:
          CASE_SYM
          {
            if (maybe_start_compound_statement(thd))
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
            Lex->spcont->push_label(thd, empty_lex_str, Lex->sphead->instructions());
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
            if (case_stmt_action_expr(Lex, $2))
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
            if (case_stmt_action_when(lex, $3, true))
              MYSQL_YYABORT;
            /* For expr $3 */
            if (lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
          }
          THEN_SYM
          sp_proc_stmts1
          {
            LEX *lex= Lex;
            if (case_stmt_action_then(lex))
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
            if (case_stmt_action_when(lex, $3, false))
              MYSQL_YYABORT;
            /* For expr $3 */
            if (lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
          }
          THEN_SYM
          sp_proc_stmts1
          {
            LEX *lex= Lex;
            if (case_stmt_action_then(lex))
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
            if (i == NULL ||
                sp->add_instr(i))
              MYSQL_YYABORT;
          }
        | ELSE sp_proc_stmts1
        ;

sp_opt_label:
          /* Empty  */  { $$= null_lex_str; }
        | label_ident   { $$= $1; }
        ;

sp_labeled_block:
          label_ident ':' BEGIN_SYM
          {
            LEX *lex= Lex;
            sp_pcontext *ctx= lex->spcont;
            sp_label *lab= ctx->find_label($1);

            if (lab)
              my_yyabort_error((ER_SP_LABEL_REDEFINE, MYF(0), $1.str));
            lex->name= $1;
          }
          sp_block_content sp_opt_label
          {
            if ($6.str)
            {
              if (my_strcasecmp(system_charset_info, $6.str, $5->name.str) != 0)
                my_yyabort_error((ER_SP_LABEL_MISMATCH, MYF(0), $6.str));
            }
          }
        ;

sp_unlabeled_block:
          BEGIN_SYM
          {
            Lex->name= empty_lex_str; // Unlabeled blocks get an empty label
          }
          sp_block_content
          { }
        ;

sp_unlabeled_block_not_atomic:
          BEGIN_SYM not ATOMIC_SYM /* TODO: BEGIN ATOMIC (not -> opt_not) */
          {
            if (maybe_start_compound_statement(thd))
              MYSQL_YYABORT;
            Lex->name= empty_lex_str; // Unlabeled blocks get an empty label
          }
          sp_block_content
          { }
        ;

sp_block_content:
          {
            LEX *lex= Lex;
            sp_label *lab= lex->spcont->push_label(thd, lex->name,
                                                   lex->sphead->instructions());
            lab->type= sp_label::BEGIN;
            lex->spcont= lex->spcont->push_context(thd,
                                                   sp_pcontext::REGULAR_SCOPE);
          }
          sp_decls
          sp_proc_stmts
          END
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            sp_pcontext *ctx= lex->spcont;
            sp_instr *i;

            sp->backpatch(ctx->last_label()); /* We always have a label */
            if ($2.hndlrs)
            {
              i= new (thd->mem_root)
                sp_instr_hpop(sp->instructions(), ctx, $2.hndlrs);
              if (i == NULL ||
                  sp->add_instr(i))
                MYSQL_YYABORT;
            }
            if ($2.curs)
            {
              i= new (thd->mem_root)
                sp_instr_cpop(sp->instructions(), ctx, $2.curs);
              if (i == NULL ||
                  sp->add_instr(i))
                MYSQL_YYABORT;
            }
            lex->spcont= ctx->pop_context();
            $$ = lex->spcont->pop_label();
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
            if (i == NULL ||
                lex->sphead->add_instr(i))
              MYSQL_YYABORT;
          }
        ;

while_body:
          expr DO_SYM
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;
            uint ip= sp->instructions();
            sp_instr_jump_if_not *i= new (thd->mem_root)
              sp_instr_jump_if_not(ip, lex->spcont, $1, lex);
            if (i == NULL ||
                /* Jumping forward */
                sp->push_backpatch(thd, i, lex->spcont->last_label()) ||
                sp->new_cont_backpatch(i) ||
                sp->add_instr(i))
              MYSQL_YYABORT;
            if (sp->restore_lex(thd))
              MYSQL_YYABORT;
          }
          sp_proc_stmts1 END WHILE_SYM
          {
            LEX *lex= Lex;
            uint ip= lex->sphead->instructions();
            sp_label *lab= lex->spcont->last_label();  /* Jumping back */
            sp_instr_jump *i= new (thd->mem_root)
              sp_instr_jump(ip, lex->spcont, lab->ip);
            if (i == NULL ||
                lex->sphead->add_instr(i))
              MYSQL_YYABORT;
            lex->sphead->do_cont_backpatch();
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
            if (i == NULL ||
                lex->sphead->add_instr(i))
              MYSQL_YYABORT;
            if (lex->sphead->restore_lex(thd))
              MYSQL_YYABORT;
            /* We can shortcut the cont_backpatch here */
            i->m_cont_dest= ip+1;
          }
        ;

pop_sp_label:
          sp_opt_label
          {
            sp_label *lab;
            Lex->sphead->backpatch(lab= Lex->spcont->pop_label());
            if ($1.str)
            {
              if (my_strcasecmp(system_charset_info, $1.str,
                                lab->name.str) != 0)
                my_yyabort_error((ER_SP_LABEL_MISMATCH, MYF(0), $1.str));
            }
          }
        ;

pop_sp_empty_label:
          {
            sp_label *lab;
            Lex->sphead->backpatch(lab= Lex->spcont->pop_label());
            DBUG_ASSERT(lab->name.length == 0);
          }
        ;

sp_labeled_control:
          label_ident ':' LOOP_SYM
          {
            if (push_sp_label(thd, $1))
              MYSQL_YYABORT;
          }
          loop_body pop_sp_label
          { }
        | label_ident ':' WHILE_SYM
          {
            if (push_sp_label(thd, $1))
              MYSQL_YYABORT;
            Lex->sphead->reset_lex(thd);
          }
          while_body pop_sp_label
          { }
        | label_ident ':' REPEAT_SYM
          {
            if (push_sp_label(thd, $1))
              MYSQL_YYABORT;
          }
          repeat_body pop_sp_label
          { }
        ;

sp_unlabeled_control:
          LOOP_SYM
          {
            if (push_sp_empty_label(thd))
              MYSQL_YYABORT;
          }
          loop_body
          pop_sp_empty_label
          { }
        | WHILE_SYM
          {
            if (push_sp_empty_label(thd))
              MYSQL_YYABORT;
            Lex->sphead->reset_lex(thd);
          }
          while_body
          pop_sp_empty_label
          { }
        | REPEAT_SYM
          {
            if (push_sp_empty_label(thd))
              MYSQL_YYABORT;
          }
          repeat_body
          pop_sp_empty_label
          { }
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
            if (lex->alter_tablespace_info == NULL)
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
            if (lex->alter_tablespace_info == NULL)
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
            if (lex->alter_tablespace_info->nodegroup_id != UNDEF_NODEGROUP)
              my_yyabort_error((ER_FILEGROUP_OPTION_ONLY_ONCE,MYF(0),"NODEGROUP"));
            lex->alter_tablespace_info->nodegroup_id= $3;
          }
        ;

opt_ts_comment:
          COMMENT_SYM opt_equal TEXT_STRING_sys
          {
            LEX *lex= Lex;
            if (lex->alter_tablespace_info->ts_comment != NULL)
              my_yyabort_error((ER_FILEGROUP_OPTION_ONLY_ONCE,MYF(0),"COMMENT"));
            lex->alter_tablespace_info->ts_comment= $3.str;
          }
        ;

opt_ts_engine:
          opt_storage ENGINE_SYM opt_equal storage_engines
          {
            LEX *lex= Lex;
            if (lex->alter_tablespace_info->storage_engine != NULL)
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
            if (!(lex->alter_tablespace_info->wait_until_completed))
              my_yyabort_error((ER_FILEGROUP_OPTION_ONLY_ONCE,MYF(0),"NO_WAIT"));
            lex->alter_tablespace_info->wait_until_completed= FALSE;
          }
        ;

size_number:
          real_ulonglong_num { $$= $1;}
        | IDENT_sys
          {
            ulonglong number;
            uint text_shift_number= 0;
            longlong prefix_number;
            char *start_ptr= $1.str;
            uint str_len= $1.length;
            char *end_ptr= start_ptr + str_len;
            int error;
            prefix_number= my_strtoll10(start_ptr, &end_ptr, &error);
            if ((start_ptr + str_len - 1) == end_ptr)
            {
              switch (end_ptr[0])
              {
                case 'g':
                case 'G': text_shift_number+=30; break;
                case 'm':
                case 'M': text_shift_number+=20; break;
                case 'k':
                case 'K': text_shift_number+=10; break;
                default:
                  my_yyabort_error((ER_WRONG_SIZE_NUMBER, MYF(0)));
              }
              if (prefix_number >> 31)
                my_yyabort_error((ER_SIZE_OVERFLOW_ERROR, MYF(0)));
              number= prefix_number << text_shift_number;
            }
            else
              my_yyabort_error((ER_WRONG_SIZE_NUMBER, MYF(0)));
            $$= number;
          }
        ;

/*
  End tablespace part
*/

create_body:
          '(' create_field_list ')'
          { Lex->create_info.option_list= NULL; }
          opt_create_table_options opt_create_partitioning opt_create_select {}
        | opt_create_table_options opt_create_partitioning opt_create_select {}
        /*
          the following rule is redundant, but there's a shift/reduce
          conflict that prevents the rule above from parsing a syntax like
          CREATE TABLE t1 (SELECT 1);
        */
        | '(' create_select_query_specification ')'
        | '(' create_select_query_specification ')'
          { Select->set_braces(1);} union_list {}
        | '(' create_select_query_specification ')'
          { Select->set_braces(1);} union_order_or_limit {}
        | create_like
          {

            Lex->create_info.add(DDL_options_st::OPT_LIKE);
            TABLE_LIST *src_table= Lex->select_lex.add_table_to_list(thd,
                                        $1, NULL, 0, TL_READ, MDL_SHARED_READ);
            if (! src_table)
              MYSQL_YYABORT;
            /* CREATE TABLE ... LIKE is not allowed for views. */
            src_table->required_type= FRMTYPE_TABLE;
          }
        ;

create_like:
          LIKE table_ident                      { $$= $2; }
        | '(' LIKE table_ident ')'              { $$= $3; }
        ;

opt_create_select:
          /* empty */ {}
        | opt_duplicate opt_as create_select_query_expression
          {
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;
          }
        ;

create_select_query_expression:
          opt_with_clause SELECT_SYM create_select_part2 opt_table_expression
          create_select_part4
          { 
            Select->set_braces(0);
            Select->set_with_clause($1);
          }
          union_clause
        | opt_with_clause SELECT_SYM create_select_part2 
          create_select_part3_union_not_ready create_select_part4
          {
            Select->set_with_clause($1);
          }
        | '(' create_select_query_specification ')'
        | '(' create_select_query_specification ')'
          { Select->set_braces(1);} union_list {}
        | '(' create_select_query_specification ')'
          { Select->set_braces(1);} union_order_or_limit {}
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
            if (!lex->part_info)
            {
              mem_alloc_error(sizeof(partition_info));
              MYSQL_YYABORT;
            }
            if (lex->sql_command == SQLCOM_ALTER_TABLE)
            {
              lex->alter_info.flags|= Alter_info::ALTER_PARTITION;
            }
          }
          partition
        ;

have_partitioning:
          /* empty */
          {
#ifdef WITH_PARTITION_STORAGE_ENGINE
            LEX_STRING partition_name={C_STRING_WITH_LEN("partition")};
            if (!plugin_is_ready(&partition_name, MYSQL_STORAGE_ENGINE_PLUGIN))
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
            LEX *lex= Lex;
            if (!lex->part_info)
            {
              my_parse_error(thd, ER_PARTITION_ENTRY_ERROR);
              MYSQL_YYABORT;
            }
            /*
              We enter here when opening the frm file to translate
              partition info string into part_info data structure.
            */
          }
          partition {}
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
        | LIST_SYM part_func
          { Lex->part_info->part_type= LIST_PARTITION; }
        | LIST_SYM part_column_list
          { Lex->part_info->part_type= LIST_PARTITION; }
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
              my_parse_error(thd, ER_SYNTAX_ERROR);
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
            if (part_info->part_field_list.push_back($1.str, thd->mem_root))
            {
              mem_alloc_error(1);
              MYSQL_YYABORT;
            }
            if (part_info->num_columns > MAX_REF_PARTS)
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
            if (part_info->set_part_expr(thd, $2 + 1, $3, $4, FALSE))
            { MYSQL_YYABORT; }
            part_info->num_columns= 1;
            part_info->column_list= FALSE;
          }
        ;

sub_part_func:
          '(' remember_name part_func_expr remember_end ')'
          {
            if (Lex->part_info->set_part_expr(thd, $2 + 1, $3, $4, TRUE))
            { MYSQL_YYABORT; }
          }
        ;


opt_num_parts:
          /* empty */ {}
        | PARTITIONS_SYM real_ulong_num
          { 
            uint num_parts= $2;
            partition_info *part_info= Lex->part_info;
            if (num_parts == 0)
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
            if (part_info->subpart_field_list.push_back($1.str, thd->mem_root))
            {
              mem_alloc_error(1);
              MYSQL_YYABORT;
            }
            if (part_info->subpart_field_list.elements > MAX_REF_PARTS)
              my_yyabort_error((ER_TOO_MANY_PARTITION_FUNC_FIELDS_ERROR, MYF(0),
                                "list of subpartition fields"));
          }
        ;

part_func_expr:
          bit_expr
          {
            if (!Lex->safe_to_cache_query)
            {
              my_parse_error(thd, ER_WRONG_EXPR_IN_PARTITION_FUNC_ERROR);
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
            if (num_parts == 0)
              my_yyabort_error((ER_NO_PARTS_ERROR, MYF(0), "subpartitions"));
            lex->part_info->num_subparts= num_parts;
            lex->part_info->use_default_num_subpartitions= FALSE;
          }
        ;

part_defs:
          /* empty */
          {
            partition_info *part_info= Lex->part_info;
            if (part_info->part_type == RANGE_PARTITION)
              my_yyabort_error((ER_PARTITIONS_MUST_BE_DEFINED_ERROR, MYF(0),
                                "RANGE"));
            if (part_info->part_type == LIST_PARTITION)
              my_yyabort_error((ER_PARTITIONS_MUST_BE_DEFINED_ERROR, MYF(0),
                                "LIST"));
          }
        | '(' part_def_list ')'
          {
            partition_info *part_info= Lex->part_info;
            uint count_curr_parts= part_info->partitions.elements;
            if (part_info->num_parts != 0)
            {
              if (part_info->num_parts !=
                  count_curr_parts)
              {
                my_parse_error(thd, ER_PARTITION_WRONG_NO_PART_ERROR);
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

            if (!p_elem ||
                 part_info->partitions.push_back(p_elem, thd->mem_root))
            {
              mem_alloc_error(sizeof(partition_element));
              MYSQL_YYABORT;
            }
            p_elem->part_state= PART_NORMAL;
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
            if (check_ident_length(&$1))
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
              if (part_info->error_if_requires_values())
                 MYSQL_YYABORT;
            }
            else
              part_info->part_type= HASH_PARTITION;
          }
        | VALUES LESS_SYM THAN_SYM
          {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            if (! lex->is_partition_management())
            {
              if (part_info->part_type != RANGE_PARTITION)
                my_yyabort_error((ER_PARTITION_WRONG_VALUES_ERROR, MYF(0),
                                  "RANGE", "LESS THAN"));
            }
            else
              part_info->part_type= RANGE_PARTITION;
          }
          part_func_max {}
        | VALUES IN_SYM
          {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            if (! lex->is_partition_management())
            {
              if (part_info->part_type != LIST_PARTITION)
                my_yyabort_error((ER_PARTITION_WRONG_VALUES_ERROR, MYF(0),
                                  "LIST", "IN"));
            }
            else
              part_info->part_type= LIST_PARTITION;
          }
          part_values_in {}
        | DEFAULT
         {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            if (! lex->is_partition_management())
            {
              if (part_info->part_type != LIST_PARTITION)
                my_yyabort_error((ER_PARTITION_WRONG_VALUES_ERROR, MYF(0),
                                  "LIST", "DEFAULT"));
            }
            else
              part_info->part_type= LIST_PARTITION;
            if (part_info->init_column_part(thd))
            {
              MYSQL_YYABORT;
            }
            if (part_info->add_max_value(thd))
            {
              MYSQL_YYABORT;
            }
         }
        ;

part_func_max:
          MAX_VALUE_SYM
          {
            partition_info *part_info= Lex->part_info;

            if (part_info->num_columns &&
                part_info->num_columns != 1U)
            {
              part_info->print_debug("Kilroy II", NULL);
              my_parse_error(thd, ER_PARTITION_COLUMN_LIST_ERROR);
              MYSQL_YYABORT;
            }
            else
              part_info->num_columns= 1U;
            if (part_info->init_column_part(thd))
            {
              MYSQL_YYABORT;
            }
            if (part_info->add_max_value(thd))
            {
              MYSQL_YYABORT;
            }
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
              if (!lex->is_partition_management() ||
                  part_info->num_columns == 0 ||
                  part_info->num_columns > MAX_REF_PARTS)
              {
                part_info->print_debug("Kilroy III", NULL);
                my_parse_error(thd, ER_PARTITION_COLUMN_LIST_ERROR);
                MYSQL_YYABORT;
              }
              /*
                Reorganize the current large array into a list of small
                arrays with one entry in each array. This can happen
                in the first partition of an ALTER TABLE statement where
                we ADD or REORGANIZE partitions. Also can only happen
                for LIST partitions.
              */
              if (part_info->reorganize_into_single_field_col_val(thd))
              {
                MYSQL_YYABORT;
              }
            }
          }
        | '(' part_value_list ')'
          {
            partition_info *part_info= Lex->part_info;
            if (part_info->num_columns < 2U)
            {
              my_parse_error(thd, ER_ROW_SINGLE_PARTITION_FIELD_ERROR);
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
            if (!(part_info->part_type == LIST_PARTITION &&
                  part_info->num_columns == 1U) &&
                 part_info->init_column_part(thd))
            {
              MYSQL_YYABORT;
            }
          }
          part_value_item_list {}
          ')'
          {
            partition_info *part_info= Lex->part_info;
            part_info->print_debug(") part_value_item", NULL);
            if (part_info->num_columns == 0)
              part_info->num_columns= part_info->curr_list_object;
            if (part_info->num_columns != part_info->curr_list_object)
            {
              /*
                All value items lists must be of equal length, in some cases
                which is covered by the above if-statement we don't know yet
                how many columns is in the partition so the assignment above
                ensures that we only report errors when we know we have an
                error.
              */
              part_info->print_debug("Kilroy I", NULL);
              my_parse_error(thd, ER_PARTITION_COLUMN_LIST_ERROR);
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
          MAX_VALUE_SYM
          {
            partition_info *part_info= Lex->part_info;
            if (part_info->part_type == LIST_PARTITION)
            {
              my_parse_error(thd, ER_MAXVALUE_IN_VALUES_IN);
              MYSQL_YYABORT;
            }
            if (part_info->add_max_value(thd))
            {
              MYSQL_YYABORT;
            }
          }
        | bit_expr
          {
            LEX *lex= Lex;
            partition_info *part_info= lex->part_info;
            Item *part_expr= $1;

            if (!lex->safe_to_cache_query)
            {
              my_parse_error(thd, ER_WRONG_EXPR_IN_PARTITION_FUNC_ERROR);
              MYSQL_YYABORT;
            }
            if (part_info->add_column_list_value(thd, part_expr))
            {
              MYSQL_YYABORT;
            }
          }
        ;


opt_sub_partition:
          /* empty */
          {
            partition_info *part_info= Lex->part_info;
            if (part_info->num_subparts != 0 &&
                !part_info->use_default_subpartitions)
            {
              /*
                We come here when we have defined subpartitions on the first
                partition but not on all the subsequent partitions. 
              */
              my_parse_error(thd, ER_PARTITION_WRONG_NO_SUBPART_ERROR);
              MYSQL_YYABORT;
            }
          }
        | '(' sub_part_list ')'
          {
            partition_info *part_info= Lex->part_info;
            if (part_info->num_subparts != 0)
            {
              if (part_info->num_subparts !=
                  part_info->count_curr_subparts)
              {
                my_parse_error(thd, ER_PARTITION_WRONG_NO_SUBPART_ERROR);
                MYSQL_YYABORT;
              }
            }
            else if (part_info->count_curr_subparts > 0)
            {
              if (part_info->partitions.elements > 1)
              {
                my_parse_error(thd, ER_PARTITION_WRONG_NO_SUBPART_ERROR);
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
            if (part_info->use_default_subpartitions &&
                part_info->partitions.elements >= 2)
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
              my_parse_error(thd, ER_PARTITION_WRONG_NO_SUBPART_ERROR);
              MYSQL_YYABORT;
            }
            if (!sub_p_elem ||
             curr_part->subpartitions.push_back(sub_p_elem, thd->mem_root))
            {
              mem_alloc_error(sizeof(partition_element));
              MYSQL_YYABORT;
            }
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
            if (check_ident_length(&$1))
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

/*
 End of partition parser part
*/

create_select_query_specification:
          opt_with_clause SELECT_SYM create_select_part2 create_select_part3
          create_select_part4
          {
            Select->set_with_clause($1);
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;
          }
        ;

create_select_part2:
          {
            LEX *lex=Lex;
            if (lex->sql_command == SQLCOM_INSERT)
              lex->sql_command= SQLCOM_INSERT_SELECT;
            else if (lex->sql_command == SQLCOM_REPLACE)
              lex->sql_command= SQLCOM_REPLACE_SELECT;
            /*
              The following work only with the local list, the global list
              is created correctly in this case
            */
            lex->current_select->table_list.save_and_clear(&lex->save_list);
            mysql_init_select(lex);
            lex->current_select->parsing_place= SELECT_LIST;
          }
          select_options select_item_list
          {
            Select->parsing_place= NO_MATTER;
          }
        ;

create_select_part3:
          opt_table_expression
        | create_select_part3_union_not_ready
        ;

create_select_part3_union_not_ready:
          table_expression order_or_limit
        | order_or_limit
        ;

create_select_part4:
          opt_select_lock_type
          {
            /*
              The following work only with the local list, the global list
              is created correctly in this case
            */
            Lex->current_select->table_list.push_front(&Lex->save_list);
          }
        ;

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
          ENGINE_SYM opt_equal ident_or_text
          {
            LEX *lex= Lex;
            if (!lex->m_sql_cmd)
            {
              DBUG_ASSERT(lex->sql_command == SQLCOM_ALTER_TABLE);
              if (!(lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_alter_table()))
                MYSQL_YYABORT;
            }
            Storage_engine_name *opt=
              lex->m_sql_cmd->option_storage_engine_name();
            DBUG_ASSERT(opt); // Expect a proper Sql_cmd
            *opt= Storage_engine_name($3);
            lex->create_info.used_fields|= HA_CREATE_USED_ENGINE;
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
                my_parse_error(thd, ER_SYNTAX_ERROR);
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
                my_parse_error(thd, ER_SYNTAX_ERROR);
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
                my_parse_error(thd, ER_SYNTAX_ERROR);
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
            if ($3 == 0 || $3 > 0xffff)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
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
            Lex->select_lex.table_list.save_and_clear(&Lex->save_list);
          }
          '(' opt_table_list ')'
          {
            /*
              Move the union list to the merge_list and exclude its tables
              from the global list.
            */
            LEX *lex=Lex;
            lex->create_info.merge_list= lex->select_lex.table_list.first;
            lex->select_lex.table_list= lex->save_list;
            /*
              When excluding union list from the global list we assume that
              elements of the former immediately follow elements which represent
              table being created/altered and parent tables.
            */
            TABLE_LIST *last_non_sel_table= lex->create_last_non_select_table;
            DBUG_ASSERT(last_non_sel_table->next_global ==
                        lex->create_info.merge_list);
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
            if ($3.length > ENGINE_OPTION_MAX_LENGTH)
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            new (thd->mem_root)
              engine_option_value($1, $3, true, &Lex->create_info.option_list,
                                  &Lex->option_list_last);
          }
        | IDENT_sys equal ident
          {
            if ($3.length > ENGINE_OPTION_MAX_LENGTH)
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            new (thd->mem_root)
              engine_option_value($1, $3, false, &Lex->create_info.option_list,
                                  &Lex->option_list_last);
          }
        | IDENT_sys equal real_ulonglong_num
          {
            new (thd->mem_root)
              engine_option_value($1, $3, &Lex->create_info.option_list,
                                  &Lex->option_list_last, thd->mem_root);
          }
        | IDENT_sys equal DEFAULT
          {
            new (thd->mem_root)
              engine_option_value($1, &Lex->create_info.option_list,
                                  &Lex->option_list_last);
          }
        ;

default_charset:
          opt_default charset opt_equal charset_name_or_default
          {
            if (Lex->create_info.add_table_option_default_charset($4))
              MYSQL_YYABORT;
          }
        ;

default_collation:
          opt_default COLLATE_SYM opt_equal collation_name_or_default
          {
            HA_CREATE_INFO *cinfo= &Lex->create_info;
            if ((cinfo->used_fields & HA_CREATE_USED_DEFAULT_CHARSET) &&
                 cinfo->default_table_charset && $4 &&
                 !($4= merge_charset_and_collation(cinfo->default_table_charset,
                                                   $4)))
            {
              MYSQL_YYABORT;
            }

            Lex->create_info.default_table_charset= $4;
            Lex->create_info.used_fields|= HA_CREATE_USED_DEFAULT_CHARSET;
          }
        ;

storage_engines:
          ident_or_text
          {
            if (Storage_engine_name($1).
                 resolve_storage_engine_with_error(thd, &$$,
                                            thd->lex->create_info.tmp_table()))
              MYSQL_YYABORT;
          }
        ;

known_storage_engines:
          ident_or_text
          {
            plugin_ref plugin;
            if ((plugin= ha_resolve_by_name(thd, &$1, false)))
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

field_list:
          field_list_item
        | field_list ',' field_list_item
        ;

field_list_item:
          column_def { }
        | key_def
        | constraint_def
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
            if (Lex->add_key(Key::MULTIPLE, $3, $4, $2))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options { }
        | key_or_index opt_if_not_exists ident TYPE_SYM btree_or_rtree
          {
            Lex->option_list= NULL;
            if (Lex->add_key(Key::MULTIPLE, $3, $5, $2))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options { }
        | fulltext opt_key_or_index opt_if_not_exists opt_ident
          {
            Lex->option_list= NULL;
            if (Lex->add_key($1, $4, HA_KEY_ALG_UNDEF, $3))
              MYSQL_YYABORT;
          }
          '(' key_list ')' fulltext_key_options { }
        | spatial opt_key_or_index opt_if_not_exists opt_ident
          {
            Lex->option_list= NULL;
            if (Lex->add_key($1, $4, HA_KEY_ALG_UNDEF, $3))
              MYSQL_YYABORT;
          }
          '(' key_list ')' spatial_key_options { }
        | opt_constraint constraint_key_type
          opt_if_not_exists opt_ident
          opt_USING_key_algorithm
          {
            Lex->option_list= NULL;
            if (Lex->add_key($2, $4.str ? $4 : $1, $5, $3))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options { }
        | opt_constraint constraint_key_type opt_if_not_exists ident
          TYPE_SYM btree_or_rtree
          {
            Lex->option_list= NULL;
            if (Lex->add_key($2, $4.str ? $4 : $1, $6, $3))
              MYSQL_YYABORT;
          }
          '(' key_list ')' normal_key_options { }
        | opt_constraint FOREIGN KEY_SYM opt_if_not_exists opt_ident
          {
            if (Lex->check_add_key($4) ||
               !(Lex->last_key= (new (thd->mem_root)
                                 Key(Key::MULTIPLE, $1.str ? $1 : $5,
                                     HA_KEY_ALG_UNDEF, true, $4))))
              MYSQL_YYABORT;
            Lex->option_list= NULL;
          }
          '(' key_list ')' references
          {
            LEX *lex=Lex;
            Key *key= (new (thd->mem_root)
                       Foreign_key($5.str ? $5 : $1,
                                   lex->last_key->columns,
                                   $10->db,
                                   $10->table,
                                   lex->ref_list,
                                   lex->fk_delete_opt,
                                   lex->fk_update_opt,
                                   lex->fk_match_option,
                                    $4));
            if (key == NULL)
              MYSQL_YYABORT;
            /*
              handle_if_exists_options() expectes the two keys in this order:
              the Foreign_key, followed by its auto-generated Key.
            */
            lex->alter_info.key_list.push_back(key, thd->mem_root);
            lex->alter_info.key_list.push_back(Lex->last_key, thd->mem_root);
            lex->option_list= NULL;

            /* Only used for ALTER TABLE. Ignored otherwise. */
            lex->alter_info.flags|= Alter_info::ADD_FOREIGN_KEY;
          }
	;

constraint_def:
         opt_constraint check_constraint
         {
           Lex->add_constraint(&$1, $2, FALSE);
         }
       ;

opt_check_constraint:
          /* empty */      { $$= (Virtual_column_info*) 0; }
        | check_constraint { $$= $1;}
        ;

check_constraint:
          CHECK_SYM '(' expr ')'
          {
            Virtual_column_info *v=
              add_virtual_expression(thd, $3);
            if (!v)
            {
              MYSQL_YYABORT;
            }
            $$= v;
          }
        ;

opt_constraint_no_id:
          /* Empty */  {}
        | CONSTRAINT   {}
        ;

opt_constraint:
          /* empty */ { $$= null_lex_str; }
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

            if (check_string_char_length(&$1, 0, NAME_CHAR_LEN,
                                         system_charset_info, 1))
              my_yyabort_error((ER_TOO_LONG_IDENT, MYF(0), $1.str));

            if (!f)
              MYSQL_YYABORT;

            lex->init_last_field(f, $1.str, NULL);
            $<create_field>$= f;
          }
          field_type_or_serial opt_check_constraint
          {
            LEX *lex=Lex;
            $$= $<create_field>2;

            $$->check_constraint= $4;

            if ($$->check(thd))
              MYSQL_YYABORT;

            lex->alter_info.create_list.push_back($$, thd->mem_root);

            $$->create_if_not_exists= Lex->check_exists;
            if ($$->flags & PRI_KEY_FLAG)
              add_key_to_list(lex, &$1, Key::PRIMARY, Lex->check_exists);
            else if ($$->flags & UNIQUE_KEY_FLAG)
              add_key_to_list(lex, &$1, Key::UNIQUE, Lex->check_exists);
          }
        ;

field_type_or_serial:
          field_type  { Lex->set_last_field_type($1); } field_def
        | SERIAL_SYM
          {
            Lex_field_type_st type;
            type.set(MYSQL_TYPE_LONGLONG);
            Lex->set_last_field_type(type);
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


field_def:
          opt_attribute
        | opt_generated_always AS virtual_column_func
         {
           Lex->last_field->vcol_info= $3;
           Lex->last_field->flags&= ~NOT_NULL_FLAG; // undo automatic NOT NULL for timestamps
         }
          vcol_opt_specifier vcol_opt_attribute
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
            lex->alter_info.flags|= Alter_info::ALTER_ADD_INDEX;
          }
        | UNIQUE_SYM KEY_SYM
          {
            LEX *lex=Lex;
            lex->last_field->flags|= UNIQUE_KEY_FLAG;
            lex->alter_info.flags|= Alter_info::ALTER_ADD_INDEX;
          }
        | COMMENT_SYM TEXT_STRING_sys { Lex->last_field->comment= $2; }
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
          }
          expr
          {
            Virtual_column_info *v= add_virtual_expression(thd, $3);
            if (!v)
              MYSQL_YYABORT;
            Lex->last_field->vcol_info= v;
          }
        ;

parenthesized_expr:
          subselect
          {
            $$= new (thd->mem_root) Item_singlerow_subselect(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr
        | expr ',' expr_list
          {
            $3->push_front($1, thd->mem_root);
            $$= new (thd->mem_root) Item_row(thd, *$3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
          ;

virtual_column_func:
          '(' parenthesized_expr ')'
          {
            Virtual_column_info *v=
              add_virtual_expression(thd, $2);
            if (!v)
            {
              MYSQL_YYABORT;
            }
            $$= v;
          }
        ;

expr_or_literal: column_default_non_parenthesized_expr | signed_literal ;

column_default_expr:
          virtual_column_func
        | expr_or_literal
          {
            if (!($$= add_virtual_expression(thd, $1)))
              MYSQL_YYABORT;
          }
        ;

field_type:
          int_type opt_field_length field_options { $$.set($1, $2); }
        | real_type opt_precision field_options   { $$.set($1, $2); }
        | FLOAT_SYM float_options field_options
          {
            $$.set(MYSQL_TYPE_FLOAT, $2);
            if ($2.length() && !$2.dec())
            {
              int err;
              ulonglong tmp_length= my_strtoll10($2.length(), NULL, &err);
              if (err || tmp_length > PRECISION_FOR_DOUBLE)
                my_yyabort_error((ER_WRONG_FIELD_SPEC, MYF(0),
                                  Lex->last_field->field_name));
              if (tmp_length > PRECISION_FOR_FLOAT)
                $$.set(MYSQL_TYPE_DOUBLE);
              else
                $$.set(MYSQL_TYPE_FLOAT);
            }
          }
        | BIT_SYM opt_field_length_default_1
          {
            $$.set(MYSQL_TYPE_BIT, $2);
          }
        | BOOL_SYM
          {
            $$.set(MYSQL_TYPE_TINY, "1");
          }
        | BOOLEAN_SYM
          {
            $$.set(MYSQL_TYPE_TINY, "1");
          }
        | char opt_field_length_default_1 opt_binary
          {
            $$.set(MYSQL_TYPE_STRING, $2);
          }
        | nchar opt_field_length_default_1 opt_bin_mod
          {
            $$.set(MYSQL_TYPE_STRING, $2);
            bincmp_collation(national_charset_info, $3);
          }
        | BINARY opt_field_length_default_1
          {
            Lex->charset=&my_charset_bin;
            $$.set(MYSQL_TYPE_STRING, $2);
          }
        | varchar field_length opt_binary
          {
            $$.set(MYSQL_TYPE_VARCHAR, $2);
          }
        | nvarchar field_length opt_bin_mod
          {
            $$.set(MYSQL_TYPE_VARCHAR, $2);
            bincmp_collation(national_charset_info, $3);
          }
        | VARBINARY field_length
          {
            Lex->charset=&my_charset_bin;
            $$.set(MYSQL_TYPE_VARCHAR, $2);
          }
        | YEAR_SYM opt_field_length field_options
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
            $$.set(MYSQL_TYPE_YEAR, $2);
          }
        | DATE_SYM
          { $$.set(MYSQL_TYPE_DATE); }
        | TIME_SYM opt_field_length
          { $$.set(opt_mysql56_temporal_format ?
                   MYSQL_TYPE_TIME2 : MYSQL_TYPE_TIME, $2); }
        | TIMESTAMP opt_field_length
          {
            if (thd->variables.sql_mode & MODE_MAXDB)
              $$.set(opt_mysql56_temporal_format ?
                     MYSQL_TYPE_DATETIME2 : MYSQL_TYPE_DATETIME, $2);
            else
            {
              /* 
                Unlike other types TIMESTAMP fields are NOT NULL by default.
                Unless --explicit-defaults-for-timestamp is given.
              */
              if (!opt_explicit_defaults_for_timestamp)
                Lex->last_field->flags|= NOT_NULL_FLAG;
              $$.set(opt_mysql56_temporal_format ? MYSQL_TYPE_TIMESTAMP2
                                                 : MYSQL_TYPE_TIMESTAMP, $2);
            }
          }
        | DATETIME opt_field_length
          { $$.set(opt_mysql56_temporal_format ?
                   MYSQL_TYPE_DATETIME2 : MYSQL_TYPE_DATETIME, $2); }
        | TINYBLOB
          {
            Lex->charset=&my_charset_bin;
            $$.set(MYSQL_TYPE_TINY_BLOB);
          }
        | BLOB_SYM opt_field_length
          {
            Lex->charset=&my_charset_bin;
            $$.set(MYSQL_TYPE_BLOB, $2);
          }
        | spatial_type float_options srid_option
          {
#ifdef HAVE_SPATIAL
            Lex->charset=&my_charset_bin;
            Lex->last_field->geom_type= $1;
            $$.set(MYSQL_TYPE_GEOMETRY, $2);
#else
            my_yyabort_error((ER_FEATURE_DISABLED, MYF(0), sym_group_geom.name,
                              sym_group_geom.needed_define));
#endif
          }
        | MEDIUMBLOB
          {
            Lex->charset=&my_charset_bin;
            $$.set(MYSQL_TYPE_MEDIUM_BLOB);
          }
        | LONGBLOB
          {
            Lex->charset=&my_charset_bin;
            $$.set(MYSQL_TYPE_LONG_BLOB);
          }
        | LONG_SYM VARBINARY
          {
            Lex->charset=&my_charset_bin;
            $$.set(MYSQL_TYPE_MEDIUM_BLOB);
          }
        | LONG_SYM varchar opt_binary
          { $$.set(MYSQL_TYPE_MEDIUM_BLOB); }
        | TINYTEXT opt_binary
          { $$.set(MYSQL_TYPE_TINY_BLOB); }
        | TEXT_SYM opt_field_length opt_binary
          { $$.set(MYSQL_TYPE_BLOB, $2); }
        | MEDIUMTEXT opt_binary
          { $$.set(MYSQL_TYPE_MEDIUM_BLOB); }
        | LONGTEXT opt_binary
          { $$.set(MYSQL_TYPE_LONG_BLOB); }
        | DECIMAL_SYM float_options field_options
          { $$.set(MYSQL_TYPE_NEWDECIMAL, $2);}
        | NUMERIC_SYM float_options field_options
          { $$.set(MYSQL_TYPE_NEWDECIMAL, $2);}
        | FIXED_SYM float_options field_options
          { $$.set(MYSQL_TYPE_NEWDECIMAL, $2);}
        | ENUM '(' string_list ')' opt_binary
          { $$.set(MYSQL_TYPE_ENUM); }
        | SET '(' string_list ')' opt_binary
          { $$.set(MYSQL_TYPE_SET); }
        | LONG_SYM opt_binary
          { $$.set(MYSQL_TYPE_MEDIUM_BLOB); }
        | JSON_SYM
          {
            Lex->charset= &my_charset_utf8mb4_bin;
            $$.set(MYSQL_TYPE_LONG_BLOB);
          }
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
          INT_SYM   { $$=MYSQL_TYPE_LONG; }
        | TINYINT   { $$=MYSQL_TYPE_TINY; }
        | SMALLINT  { $$=MYSQL_TYPE_SHORT; }
        | MEDIUMINT { $$=MYSQL_TYPE_INT24; }
        | BIGINT    { $$=MYSQL_TYPE_LONGLONG; }
        ;

real_type:
          REAL
          {
            $$= thd->variables.sql_mode & MODE_REAL_AS_FLOAT ?
              MYSQL_TYPE_FLOAT : MYSQL_TYPE_DOUBLE;
          }
        | DOUBLE_SYM
          { $$=MYSQL_TYPE_DOUBLE; }
        | DOUBLE_SYM PRECISION
          { $$=MYSQL_TYPE_DOUBLE; }
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
        | '(' NUM ')'           { $$= $2.str; }
        ;

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
            if (item == NULL)
              MYSQL_YYABORT;
            Lex->last_field->on_update= item;
          }
        | AUTO_INC { Lex->last_field->flags|= AUTO_INCREMENT_FLAG | NOT_NULL_FLAG; }
        | SERIAL_SYM DEFAULT VALUE_SYM
          { 
            LEX *lex=Lex;
            lex->last_field->flags|= AUTO_INCREMENT_FLAG | NOT_NULL_FLAG | UNIQUE_KEY_FLAG;
            lex->alter_info.flags|= Alter_info::ALTER_ADD_INDEX;
          }
        | COLLATE_SYM collation_name
          {
            if (Lex->charset && !my_charset_same(Lex->charset,$2))
              my_yyabort_error((ER_COLLATION_CHARSET_MISMATCH, MYF(0),
                                $2->name,Lex->charset->csname));
            Lex->last_field->charset= $2;
          }
        | serial_attribute
        ;

serial_attribute:
          not NULL_SYM { Lex->last_field->flags|= NOT_NULL_FLAG; }
        | opt_primary KEY_SYM
          {
            LEX *lex=Lex;
            lex->last_field->flags|= PRI_KEY_FLAG | NOT_NULL_FLAG;
            lex->alter_info.flags|= Alter_info::ALTER_ADD_INDEX;
          }
        | vcol_attribute
        | IDENT_sys equal TEXT_STRING_sys
          {
            if ($3.length > ENGINE_OPTION_MAX_LENGTH)
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            new (thd->mem_root)
              engine_option_value($1, $3, true, &Lex->last_field->option_list,
                                  &Lex->option_list_last);
          }
        | IDENT_sys equal ident
          {
            if ($3.length > ENGINE_OPTION_MAX_LENGTH)
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            new (thd->mem_root)
              engine_option_value($1, $3, false, &Lex->last_field->option_list,
                                  &Lex->option_list_last);
          }
        | IDENT_sys equal real_ulonglong_num
          {
            new (thd->mem_root)
              engine_option_value($1, $3, &Lex->last_field->option_list,
                                  &Lex->option_list_last, thd->mem_root);
          }
        | IDENT_sys equal DEFAULT
          {
            new (thd->mem_root)
              engine_option_value($1, &Lex->last_field->option_list, &Lex->option_list_last);
          }
        ;


charset:
          CHAR_SYM SET {}
        | CHARSET {}
        ;

charset_name:
          ident_or_text
          {
            if (!($$=get_charset_by_csname($1.str,MY_CS_PRIMARY,MYF(0))))
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
            if (!($$=get_charset_by_csname($1.str,MY_CS_PRIMARY,MYF(0))) &&
                !($$=get_old_charset_by_name($1.str)))
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
            if (!($$= mysqld_collation_get_by_name($1.str)))
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
            if (!($$= get_charset_by_csname("ucs2", MY_CS_PRIMARY,MYF(0))))
              my_yyabort_error((ER_UNKNOWN_CHARACTER_SET, MYF(0), "ucs2"));
          }
        ;

collate: COLLATE_SYM collation_name_or_default { $$= $2; }
       ;

opt_binary:
          /* empty */             { bincmp_collation(NULL, false); }
        | BYTE_SYM                { bincmp_collation(&my_charset_bin, false); }
        | charset_or_alias opt_bin_mod { bincmp_collation($1, $2); }
        | BINARY                  { bincmp_collation(NULL, true); }
        | BINARY charset_or_alias { bincmp_collation($2, true); }
        | charset_or_alias collate
          {
            if (!$2)
              Lex->charset= $1; // CHARACTER SET cs COLLATE DEFAULT
            else
            {
              if (!my_charset_same($2, $1))
                my_yyabort_error((ER_COLLATION_CHARSET_MISMATCH, MYF(0),
                                  $2->name, $1->csname));
              Lex->charset= $2;
            }
          }
        | collate { Lex->charset= $1; }
        ;

opt_bin_mod:
          /* empty */   { $$= false; }
        | BINARY        { $$= true; }
        ;

ws_nweights:
        '(' real_ulong_num
        {
          if ($2 == 0)
          {
            my_parse_error(thd, ER_SYNTAX_ERROR);
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
            Key_part_spec *key= new (thd->mem_root) Key_part_spec($3, 0);
            if (key == NULL)
              MYSQL_YYABORT;
            Lex->ref_list.push_back(key, thd->mem_root);
          }
        | ident
          {
            Key_part_spec *key= new (thd->mem_root) Key_part_spec($1, 0);
            if (key == NULL)
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
          { Lex->last_key->key_create_info.block_size= $3; }
        | COMMENT_SYM TEXT_STRING_sys
          { Lex->last_key->key_create_info.comment= $2; }
        | IDENT_sys equal TEXT_STRING_sys
          {
            if ($3.length > ENGINE_OPTION_MAX_LENGTH)
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            new (thd->mem_root)
              engine_option_value($1, $3, true, &Lex->option_list,
                                  &Lex->option_list_last);
          }
        | IDENT_sys equal ident
          {
            if ($3.length > ENGINE_OPTION_MAX_LENGTH)
              my_yyabort_error((ER_VALUE_TOO_LONG, MYF(0), $1.str));
            new (thd->mem_root)
              engine_option_value($1, $3, false, &Lex->option_list,
                                  &Lex->option_list_last);
          }
        | IDENT_sys equal real_ulonglong_num
          {
            new (thd->mem_root)
              engine_option_value($1, $3, &Lex->option_list,
                                  &Lex->option_list_last, thd->mem_root);
          }
        | IDENT_sys equal DEFAULT
          {
            new (thd->mem_root)
              engine_option_value($1, &Lex->option_list, &Lex->option_list_last);
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
            if (plugin_is_ready(&$3, MYSQL_FTPARSER_PLUGIN))
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
            $$= new (thd->mem_root) Key_part_spec($1, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | ident '(' NUM ')'
          {
            int key_part_len= atoi($3.str);
            if (!key_part_len)
              my_yyabort_error((ER_KEY_PART_0, MYF(0), $1.str));
            $$= new (thd->mem_root) Key_part_spec($1, (uint) key_part_len);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

opt_ident:
          /* empty */ { $$= null_lex_str; }
        | field_ident { $$= $1; }
        ;

opt_component:
          /* empty */    { $$= null_lex_str; }
        | '.' ident      { $$= $2; }
        ;

string_list:
          text_string
          { Lex->last_field->interval_list.push_back($1, thd->mem_root); }
        | string_list ',' text_string
          { Lex->last_field->interval_list.push_back($3, thd->mem_root); }
        ;

/*
** Alter table
*/

alter:
          ALTER
          {
            Lex->name= null_lex_str;
            Lex->only_view= FALSE;
            Lex->sql_command= SQLCOM_ALTER_TABLE;
            Lex->duplicates= DUP_ERROR; 
            Lex->select_lex.init_order();
            Lex->create_info.init();
            Lex->create_info.row_type= ROW_TYPE_NOT_USED;
            Lex->alter_info.reset();
            Lex->no_write_to_binlog= 0;
            Lex->create_info.storage_media= HA_SM_DEFAULT;
            DBUG_ASSERT(!Lex->m_sql_cmd);
          }
          alter_options TABLE_SYM table_ident
          {
            if (!Lex->select_lex.add_table_to_list(thd, $5, NULL,
                                                   TL_OPTION_UPDATING,
                                                   TL_READ_NO_INSERT,
                                                   MDL_SHARED_UPGRADABLE))
              MYSQL_YYABORT;
            Lex->select_lex.db= (Lex->select_lex.table_list.first)->db;
            Lex->create_last_non_select_table= Lex->last_table();
          }
          alter_commands
          {
            if (!Lex->m_sql_cmd)
            {
              /* Create a generic ALTER TABLE statment. */
              Lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_alter_table();
              if (Lex->m_sql_cmd == NULL)
                MYSQL_YYABORT;
            }
          }
        | ALTER DATABASE ident_or_empty
          {
            Lex->create_info.default_table_charset= NULL;
            Lex->create_info.used_fields= 0;
          }
          create_database_options
          {
            LEX *lex=Lex;
            lex->sql_command=SQLCOM_ALTER_DB;
            lex->name= $3;
            if (lex->name.str == NULL &&
                lex->copy_db_to(&lex->name.str, &lex->name.length))
              MYSQL_YYABORT;
          }
        | ALTER DATABASE ident UPGRADE_SYM DATA_SYM DIRECTORY_SYM NAME_SYM
          {
            LEX *lex= Lex;
            if (lex->sphead)
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "DATABASE"));
            lex->sql_command= SQLCOM_ALTER_DB_UPGRADE;
            lex->name= $3;
          }
        | ALTER PROCEDURE_SYM sp_name
          {
            LEX *lex= Lex;

            if (lex->sphead)
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "PROCEDURE"));
            bzero((char *)&lex->sp_chistics, sizeof(st_sp_chistics));
          }
          sp_a_chistics
          {
            LEX *lex=Lex;

            lex->sql_command= SQLCOM_ALTER_PROCEDURE;
            lex->spname= $3;
          }
        | ALTER FUNCTION_SYM sp_name
          {
            LEX *lex= Lex;

            if (lex->sphead)
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "FUNCTION"));
            bzero((char *)&lex->sp_chistics, sizeof(st_sp_chistics));
          }
          sp_a_chistics
          {
            LEX *lex=Lex;

            lex->sql_command= SQLCOM_ALTER_FUNCTION;
            lex->spname= $3;
          }
        | ALTER view_algorithm definer_opt
          {
            LEX *lex= Lex;

            if (lex->sphead)
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "ALTER VIEW"));
            lex->create_view_mode= VIEW_ALTER;
          }
          view_tail
          {}
        | ALTER definer_opt
          /*
            We have two separate rules for ALTER VIEW rather that
            optional view_algorithm above, to resolve the ambiguity
            with the ALTER EVENT below.
          */
          {
            LEX *lex= Lex;

            if (lex->sphead)
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "ALTER VIEW"));
            lex->create_view_algorithm= VIEW_ALGORITHM_INHERIT;
            lex->create_view_mode= VIEW_ALTER;
          }
          view_tail
          {}
        | ALTER definer_opt remember_name EVENT_SYM sp_name
          {
            /* 
              It is safe to use Lex->spname because
              ALTER EVENT xxx RENATE TO yyy DO ALTER EVENT RENAME TO
              is not allowed. Lex->spname is used in the case of RENAME TO
              If it had to be supported spname had to be added to
              Event_parse_data.
            */

            if (!(Lex->event_parse_data= Event_parse_data::new_instance(thd)))
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
            if (!($7 || $8 || $9 || $10 || $11))
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            /*
              sql_command is set here because some rules in ev_sql_stmt
              can overwrite it
            */
            Lex->sql_command= SQLCOM_ALTER_EVENT;
            Lex->stmt_definition_end= (char*)YYLIP->get_cpp_ptr();
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
        | ALTER USER_SYM opt_if_exists clear_privileges grant_list
          opt_require_clause opt_resource_options
          {
            Lex->create_info.set($3);
            Lex->sql_command= SQLCOM_ALTER_USER;
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
          /* empty */ { $$= null_lex_str; }
        | ident { $$= $1; }
        ;

alter_commands:
          /* empty */
        | DISCARD TABLESPACE
          {
            Lex->m_sql_cmd= new (thd->mem_root)
              Sql_cmd_discard_import_tablespace(
                Sql_cmd_discard_import_tablespace::DISCARD_TABLESPACE);
            if (Lex->m_sql_cmd == NULL)
              MYSQL_YYABORT;
          }
        | IMPORT TABLESPACE
          {
            Lex->m_sql_cmd= new (thd->mem_root)
              Sql_cmd_discard_import_tablespace(
                Sql_cmd_discard_import_tablespace::IMPORT_TABLESPACE);
            if (Lex->m_sql_cmd == NULL)
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
            Lex->alter_info.flags|= Alter_info::ALTER_DROP_PARTITION;
            DBUG_ASSERT(!Lex->if_exists());
            Lex->create_info.add($3);
          }
        | REBUILD_SYM PARTITION_SYM opt_no_write_to_binlog
          all_or_alt_part_name_list
          {
            LEX *lex= Lex;
            lex->alter_info.flags|= Alter_info::ALTER_REBUILD_PARTITION;
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
            if (lex->m_sql_cmd == NULL)
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
            if (lex->m_sql_cmd == NULL)
               MYSQL_YYABORT;
          }
        | CHECK_SYM PARTITION_SYM all_or_alt_part_name_list
          {
            LEX *lex= thd->lex;
            lex->check_opt.init();
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root)
                              Sql_cmd_alter_table_check_partition();
            if (lex->m_sql_cmd == NULL)
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
            if (lex->m_sql_cmd == NULL)
              MYSQL_YYABORT;
          }
          opt_mi_repair_type
        | COALESCE PARTITION_SYM opt_no_write_to_binlog real_ulong_num
          {
            LEX *lex= Lex;
            lex->alter_info.flags|= Alter_info::ALTER_COALESCE_PARTITION;
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
            if (lex->m_sql_cmd == NULL)
              MYSQL_YYABORT;
          }
        | reorg_partition_rule
        | EXCHANGE_SYM PARTITION_SYM alt_part_name_item
          WITH TABLE_SYM table_ident have_partitioning
          {
            LEX *lex= thd->lex;
            size_t dummy;
            lex->select_lex.db=$6->db.str;
            if (lex->select_lex.db == NULL &&
                lex->copy_db_to(&lex->select_lex.db, &dummy))
            {
              MYSQL_YYABORT;
            }
            lex->name= $6->table;
            lex->alter_info.flags|= Alter_info::ALTER_EXCHANGE_PARTITION;
            if (!lex->select_lex.add_table_to_list(thd, $6, NULL,
                                                   TL_OPTION_UPDATING,
                                                   TL_READ_NO_INSERT,
                                                   MDL_SHARED_NO_WRITE))
              MYSQL_YYABORT;
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root)
                               Sql_cmd_alter_table_exchange_partition();
            if (lex->m_sql_cmd == NULL)
              MYSQL_YYABORT;
          }
        ;

remove_partitioning:
          REMOVE_SYM PARTITIONING_SYM
          {
            Lex->alter_info.flags|= Alter_info::ALTER_REMOVE_PARTITIONING;
          }
        ;

all_or_alt_part_name_list:
          ALL
          {
            Lex->alter_info.flags|= Alter_info::ALTER_ALL_PARTITION;
          }
        | alt_part_name_list
        ;

add_partition_rule:
          ADD PARTITION_SYM opt_if_not_exists
          opt_no_write_to_binlog
          {
            LEX *lex= Lex;
            lex->part_info= new (thd->mem_root) partition_info();
            if (!lex->part_info)
            {
              mem_alloc_error(sizeof(partition_info));
              MYSQL_YYABORT;
            }
            lex->alter_info.flags|= Alter_info::ALTER_ADD_PARTITION;
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
            if (!lex->part_info)
            {
              mem_alloc_error(sizeof(partition_info));
              MYSQL_YYABORT;
            }
            lex->no_write_to_binlog= $3;
          }
          reorg_parts_rule
        ;

reorg_parts_rule:
          /* empty */
          {
            Lex->alter_info.flags|= Alter_info::ALTER_TABLE_REORG;
          }
        | alt_part_name_list
          {
            Lex->alter_info.flags|= Alter_info::ALTER_REORGANIZE_PARTITION;
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
            if (Lex->alter_info.partition_names.push_back($1.str,
                                                          thd->mem_root))
            {
              mem_alloc_error(1);
              MYSQL_YYABORT;
            }
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
          {
            LEX *lex=Lex;
            lex->alter_info.flags|= Alter_info::ALTER_ADD_COLUMN;
          }
        ;

alter_list_item:
          add_column column_def opt_place
          {
            Lex->create_last_non_select_table= Lex->last_table();
            $2->after= $3;
          }
        | ADD key_def
          {
            Lex->create_last_non_select_table= Lex->last_table();
            Lex->alter_info.flags|= Alter_info::ALTER_ADD_INDEX;
          }
        | add_column '(' create_field_list ')'
          {
            Lex->alter_info.flags|= Alter_info::ALTER_ADD_COLUMN |
                                    Alter_info::ALTER_ADD_INDEX;
          }
        | ADD constraint_def
          {
            Lex->alter_info.flags|= Alter_info::ALTER_ADD_CHECK_CONSTRAINT;
	  }
        | ADD CONSTRAINT IF_SYM not EXISTS field_ident check_constraint
         {
           Lex->alter_info.flags|= Alter_info::ALTER_ADD_CHECK_CONSTRAINT;
           Lex->add_constraint(&$6, $7, TRUE);
         }
        | CHANGE opt_column opt_if_exists_table_element field_ident
          field_spec opt_place
          {
            Lex->alter_info.flags|= (Alter_info::ALTER_CHANGE_COLUMN |
                                     Alter_info::ALTER_RENAME_COLUMN);
            Lex->create_last_non_select_table= Lex->last_table();
            $5->change= $4.str;
            $5->after= $6;
          }
        | MODIFY_SYM opt_column opt_if_exists_table_element
          field_spec opt_place
          {
            Lex->alter_info.flags|= Alter_info::ALTER_CHANGE_COLUMN;
            Lex->create_last_non_select_table= Lex->last_table();
            $4->change= $4->field_name;
            $4->after= $5;
          }
        | DROP opt_column opt_if_exists_table_element field_ident opt_restrict
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::COLUMN, $4.str, $3));
            if (ad == NULL)
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= Alter_info::ALTER_DROP_COLUMN;
          }
	| DROP CONSTRAINT opt_if_exists_table_element field_ident
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::CHECK_CONSTRAINT,
                                        $4.str, $3));
            if (ad == NULL)
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= Alter_info::ALTER_DROP_CHECK_CONSTRAINT;
          }
        | DROP FOREIGN KEY_SYM opt_if_exists_table_element field_ident
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::FOREIGN_KEY, $5.str, $4));
            if (ad == NULL)
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= Alter_info::DROP_FOREIGN_KEY;
          }
        | DROP opt_constraint_no_id PRIMARY_SYM KEY_SYM
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::KEY, primary_key_name,
                                        FALSE));
            if (ad == NULL)
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= Alter_info::ALTER_DROP_INDEX;
          }
        | DROP key_or_index opt_if_exists_table_element field_ident
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::KEY, $4.str, $3));
            if (ad == NULL)
              MYSQL_YYABORT;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            lex->alter_info.flags|= Alter_info::ALTER_DROP_INDEX;
          }
        | DISABLE_SYM KEYS
          {
            LEX *lex=Lex;
            lex->alter_info.keys_onoff= Alter_info::DISABLE;
            lex->alter_info.flags|= Alter_info::ALTER_KEYS_ONOFF;
          }
        | ENABLE_SYM KEYS
          {
            LEX *lex=Lex;
            lex->alter_info.keys_onoff= Alter_info::ENABLE;
            lex->alter_info.flags|= Alter_info::ALTER_KEYS_ONOFF;
          }
        | ALTER opt_column field_ident SET DEFAULT column_default_expr
          {
            LEX *lex=Lex;
            if (check_expression($6, $3.str, VCOL_DEFAULT))
              MYSQL_YYABORT;
            Alter_column *ac= new (thd->mem_root) Alter_column($3.str,$6);
            if (ac == NULL)
              MYSQL_YYABORT;
            lex->alter_info.alter_list.push_back(ac, thd->mem_root);
            lex->alter_info.flags|= Alter_info::ALTER_CHANGE_COLUMN_DEFAULT;
          }
        | ALTER opt_column field_ident DROP DEFAULT
          {
            LEX *lex=Lex;
            Alter_column *ac= (new (thd->mem_root)
                               Alter_column($3.str, (Virtual_column_info*) 0));
            if (ac == NULL)
              MYSQL_YYABORT;
            lex->alter_info.alter_list.push_back(ac, thd->mem_root);
            lex->alter_info.flags|= Alter_info::ALTER_CHANGE_COLUMN_DEFAULT;
          }
        | RENAME opt_to table_ident
          {
            LEX *lex=Lex;
            size_t dummy;
            lex->select_lex.db=$3->db.str;
            if (lex->select_lex.db == NULL &&
                lex->copy_db_to(&lex->select_lex.db, &dummy))
            {
              MYSQL_YYABORT;
            }
            if (check_table_name($3->table.str,$3->table.length, FALSE) ||
                ($3->db.str && check_db_name(&$3->db)))
              my_yyabort_error((ER_WRONG_TABLE_NAME, MYF(0), $3->table.str));
            lex->name= $3->table;
            lex->alter_info.flags|= Alter_info::ALTER_RENAME;
          }
        | CONVERT_SYM TO_SYM charset charset_name_or_default opt_collate
          {
            if (!$4)
            {
              $4= thd->variables.collation_database;
            }
            $5= $5 ? $5 : $4;
            if (!my_charset_same($4,$5))
              my_yyabort_error((ER_COLLATION_CHARSET_MISMATCH, MYF(0),
                                $5->name, $4->csname));
            if (Lex->create_info.add_alter_list_item_convert_to_charset($5))
              MYSQL_YYABORT;
            Lex->alter_info.flags|= Alter_info::ALTER_OPTIONS;
          }
        | create_table_options_space_separated
          {
            LEX *lex=Lex;
            lex->alter_info.flags|= Alter_info::ALTER_OPTIONS;
          }
        | FORCE_SYM
          {
            Lex->alter_info.flags|= Alter_info::ALTER_RECREATE;
          }
        | alter_order_clause
          {
            LEX *lex=Lex;
            lex->alter_info.flags|= Alter_info::ALTER_ORDER;
          }
        | alter_algorithm_option
        | alter_lock_option
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
            if (Lex->alter_info.set_requested_algorithm(&$3))
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
            if (Lex->alter_info.set_requested_lock(&$3))
              my_yyabort_error((ER_UNKNOWN_ALTER_LOCK, MYF(0), $3.str));
          }
        ;

opt_column:
          /* empty */ {}
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
          /* empty */ { $$= NULL; }
        | AFTER_SYM ident
          {
            $$= $2.str;
            Lex->alter_info.flags |= Alter_info::ALTER_COLUMN_ORDER;
          }
        | FIRST_SYM
          {
            $$= first_keyword;
            Lex->alter_info.flags |= Alter_info::ALTER_COLUMN_ORDER;
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
            if (($3 & MYSQL_START_TRANS_OPT_READ_WRITE) &&
                ($3 & MYSQL_START_TRANS_OPT_READ_ONLY))
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
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
            if (((lex->mi.log_file_name || lex->mi.pos) &&
                 (lex->mi.relay_log_name || lex->mi.relay_log_pos)) ||
                !((lex->mi.log_file_name && lex->mi.pos) ||
                  (lex->mi.relay_log_name && lex->mi.relay_log_pos)))
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
        | VIEW_SYM { Lex->only_view= TRUE; } table_list opt_view_repair_type
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
            if (lex->m_sql_cmd == NULL)
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
            if (lex->m_sql_cmd == NULL)
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
            if (lex->column_list == NULL)
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
            if (lex->index_list == NULL)
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
            Lex->ident.str=    NULL;
            Lex->ident.length= 0;
          }
          |
          BINLOG_SYM '@' ident_or_text ',' '@' ident_or_text
          {
            Lex->sql_command = SQLCOM_BINLOG_BASE64_EVENT;
            Lex->comment= $3;
            Lex->ident=   $6;
          }
          ;

check_view_or_table:
          table_or_tables table_list opt_mi_check_type
        | VIEW_SYM { Lex->only_view= TRUE; } table_list opt_view_check_type
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
            if (lex->sphead)
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "CHECK"));
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_check_table();
            if (lex->m_sql_cmd == NULL)
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
          table_list
          {
            LEX* lex= thd->lex;
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_optimize_table();
            if (lex->m_sql_cmd == NULL)
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
            if (Lex->users_list.push_back($1, thd->mem_root) ||
                Lex->users_list.push_back($3, thd->mem_root))
              MYSQL_YYABORT;
          }
        | rename_list ',' user TO_SYM user
          {
            if (Lex->users_list.push_back($3, thd->mem_root) ||
                Lex->users_list.push_back($5, thd->mem_root))
              MYSQL_YYABORT;
          }
        ;

table_to_table_list:
          table_to_table
        | table_to_table_list ',' table_to_table
        ;

table_to_table:
          table_ident TO_SYM table_ident
          {
            LEX *lex=Lex;
            SELECT_LEX *sl= lex->current_select;
            if (!sl->add_table_to_list(thd, $1,NULL,TL_OPTION_UPDATING,
                                       TL_IGNORE, MDL_EXCLUSIVE) ||
                !sl->add_table_to_list(thd, $3,NULL,TL_OPTION_UPDATING,
                                       TL_IGNORE, MDL_EXCLUSIVE))
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
            if (!Select->add_table_to_list(thd, $1, NULL, 0, TL_READ,
                                           MDL_SHARED_READ,
                                           Select->pop_index_hints()))
              MYSQL_YYABORT;
          }
        ;

assign_to_keycache_parts:
          table_ident adm_partition cache_keys_spec
          {
            if (!Select->add_table_to_list(thd, $1, NULL, 0, TL_READ, 
                                           MDL_SHARED_READ,
                                           Select->pop_index_hints()))
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
            if (!Select->add_table_to_list(thd, $1, NULL, $3, TL_READ,
                                           MDL_SHARED_READ,
                                           Select->pop_index_hints()))
              MYSQL_YYABORT;
          }
        ;

preload_keys_parts:
          table_ident adm_partition cache_keys_spec opt_ignore_leaves
          {
            if (!Select->add_table_to_list(thd, $1, NULL, $4, TL_READ,
                                           MDL_SHARED_READ,
                                           Select->pop_index_hints()))
              MYSQL_YYABORT;
          }
        ;

adm_partition:
          PARTITION_SYM have_partitioning
          {
            Lex->alter_info.flags|= Alter_info::ALTER_ADMIN_PARTITION;
          }
          '(' all_or_alt_part_name_list ')'
        ;

cache_keys_spec:
          {
            Lex->select_lex.alloc_index_hints(thd);
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
          opt_with_clause select_init
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SELECT;
            lex->current_select->set_with_clause($1);
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;
          }
        ;

select_init:
          SELECT_SYM select_options_and_item_list select_init3
        | '(' select_paren ')'
        | '(' select_paren ')' union_list
        | '(' select_paren ')' union_order_or_limit
        ;

union_list_part2:
          SELECT_SYM select_options_and_item_list select_init3_union_query_term
        | '(' select_paren_union_query_term ')'
        | '(' select_paren_union_query_term ')' union_list
        | '(' select_paren_union_query_term ')' union_order_or_limit
        ;

select_paren:
          {
            /*
              In order to correctly parse UNION's global ORDER BY we need to
              set braces before parsing the clause.
            */
            Lex->current_select->set_braces(true);
          }
          SELECT_SYM select_options_and_item_list select_part3
          opt_select_lock_type
          {
            DBUG_ASSERT(Lex->current_select->braces);
          }
        | '(' select_paren ')'
        ;

select_paren_union_query_term:
          {
            /*
              In order to correctly parse UNION's global ORDER BY we need to
              set braces before parsing the clause.
            */
            Lex->current_select->set_braces(true);
          }
          SELECT_SYM select_options_and_item_list select_part3_union_query_term
          opt_select_lock_type
          {
            DBUG_ASSERT(Lex->current_select->braces);
          }
        | '(' select_paren_union_query_term ')'
        ;

select_paren_view:
          {
            /*
              In order to correctly parse UNION's global ORDER BY we need to
              set braces before parsing the clause.
            */
            Lex->current_select->set_braces(true);
          }
          SELECT_SYM select_options_and_item_list select_part3_view
          opt_select_lock_type
          {
            DBUG_ASSERT(Lex->current_select->braces);
          }
        | '(' select_paren_view ')'
        ;

/* The equivalent of select_paren for nested queries. */
select_paren_derived:
          {
            Lex->current_select->set_braces(true);
          }
          SELECT_SYM select_part2_derived
          opt_table_expression
          opt_order_clause
          opt_limit_clause
          opt_select_lock_type
          {
            DBUG_ASSERT(Lex->current_select->braces);
            $$= Lex->current_select->master_unit()->first_select();
          }
        | '(' select_paren_derived ')'  { $$= $2; }
        ;

select_init3:
          opt_table_expression
          opt_select_lock_type
          {
            /* Parentheses carry no meaning here */
            Lex->current_select->set_braces(false);
          }
          union_clause
        | select_part3_union_not_ready
          opt_select_lock_type
          {
            /* Parentheses carry no meaning here */
            Lex->current_select->set_braces(false);
          }
        ;


select_init3_union_query_term:
          opt_table_expression
          opt_select_lock_type
          {
            /* Parentheses carry no meaning here */
            Lex->current_select->set_braces(false);
          }
          union_clause
        | select_part3_union_not_ready_noproc
          opt_select_lock_type
          {
            /* Parentheses carry no meaning here */
            Lex->current_select->set_braces(false);
          }
        ;


select_init3_view:
          opt_table_expression opt_select_lock_type
          {
            Lex->current_select->set_braces(false);
          }
        | opt_table_expression opt_select_lock_type
          {
            Lex->current_select->set_braces(false);
          }
          union_list_view
        | order_or_limit opt_select_lock_type
          {
            Lex->current_select->set_braces(false);
          }
        | table_expression order_or_limit opt_select_lock_type
          {
            Lex->current_select->set_braces(false);
          }
        ;

/*
  The SELECT parts after select_item_list that cannot be followed by UNION.
*/

select_part3:
          opt_table_expression
        | select_part3_union_not_ready
        ;

select_part3_union_query_term:
          opt_table_expression
        | select_part3_union_not_ready_noproc
        ;

select_part3_view:
          opt_table_expression
        | order_or_limit
        | table_expression order_or_limit
        ;

select_part3_union_not_ready:
          select_part3_union_not_ready_noproc
        | table_expression procedure_clause
        | table_expression order_or_limit procedure_clause
        ;

select_part3_union_not_ready_noproc:
          order_or_limit
        | into opt_table_expression opt_order_clause opt_limit_clause
        | table_expression into
        | table_expression order_or_limit
        | table_expression order_or_limit into
        ;

select_options_and_item_list:
          {
            LEX *lex= Lex;
            SELECT_LEX *sel= lex->current_select;
            if (sel->linkage != UNION_TYPE)
              mysql_init_select(lex);
            lex->current_select->parsing_place= SELECT_LIST;
          }
          select_options select_item_list
          {
            Select->parsing_place= NO_MATTER;
          }
        ;


/**
  <table expression>, as in the SQL standard.
*/
table_expression:
          from_clause
          opt_where_clause
          opt_group_clause
          opt_having_clause
          opt_window_clause
        ;

opt_table_expression:
            /* Empty */
          | table_expression
        ;

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
            if (Select->options & SELECT_DISTINCT && Select->options & SELECT_ALL)
              my_yyabort_error((ER_WRONG_USAGE, MYF(0), "ALL", "DISTINCT"));
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
              Allow this flag only on the first top-level SELECT statement, if
              SQL_CACHE wasn't specified, and only once per query.
             */
            if (Lex->current_select != &Lex->select_lex)
              my_yyabort_error((ER_CANT_USE_OPTION_HERE, MYF(0), "SQL_NO_CACHE"));
            if (Lex->select_lex.sql_cache == SELECT_LEX::SQL_CACHE)
              my_yyabort_error((ER_WRONG_USAGE, MYF(0), "SQL_CACHE", "SQL_NO_CACHE"));
            if (Lex->select_lex.sql_cache == SELECT_LEX::SQL_NO_CACHE)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "SQL_NO_CACHE"));

            Lex->safe_to_cache_query=0;
            Lex->select_lex.options&= ~OPTION_TO_QUERY_CACHE;
            Lex->select_lex.sql_cache= SELECT_LEX::SQL_NO_CACHE;
          }
        | SQL_CACHE_SYM
          {
            /* 
              Allow this flag only on the first top-level SELECT statement, if
              SQL_NO_CACHE wasn't specified, and only once per query.
             */
            if (Lex->current_select != &Lex->select_lex)
              my_yyabort_error((ER_CANT_USE_OPTION_HERE, MYF(0), "SQL_CACHE"));
            if (Lex->select_lex.sql_cache == SELECT_LEX::SQL_NO_CACHE)
              my_yyabort_error((ER_WRONG_USAGE, MYF(0), "SQL_NO_CACHE", "SQL_CACHE"));
            if (Lex->select_lex.sql_cache == SELECT_LEX::SQL_CACHE)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "SQL_CACHE"));

            Lex->safe_to_cache_query=1;
            Lex->select_lex.options|= OPTION_TO_QUERY_CACHE;
            Lex->select_lex.sql_cache= SELECT_LEX::SQL_CACHE;
          }
        ;

opt_select_lock_type:
          /* empty */
        | FOR_SYM UPDATE_SYM
          {
            LEX *lex=Lex;
            lex->current_select->lock_type= TL_WRITE;
            lex->current_select->set_lock_for_tables(TL_WRITE, false);
            lex->safe_to_cache_query=0;
          }
        | LOCK_SYM IN_SYM SHARE_SYM MODE_SYM
          {
            LEX *lex=Lex;
            lex->current_select->lock_type= TL_READ_WITH_SHARED_LOCKS;
            lex->current_select->
              set_lock_for_tables(TL_READ_WITH_SHARED_LOCKS, false);
            lex->safe_to_cache_query=0;
          }
        ;

select_item_list:
          select_item_list ',' select_item
        | select_item
        | '*'
          {
            Item *item= new (thd->mem_root)
                          Item_field(thd, &thd->lex->current_select->context,
                                     NULL, NULL, "*");
            if (item == NULL)
              MYSQL_YYABORT;
            if (add_item_to_list(thd, item))
              MYSQL_YYABORT;
            (thd->lex->current_select->with_wild)++;
          }
        ;

select_item:
          remember_name table_wild remember_end
          {
            if (add_item_to_list(thd, $2))
              MYSQL_YYABORT;
          }
        | remember_name expr remember_end select_alias
          {
            DBUG_ASSERT($1 < $3);

            if (add_item_to_list(thd, $2))
              MYSQL_YYABORT;
            if ($4.str)
            {
              if (Lex->sql_command == SQLCOM_CREATE_VIEW &&
                  check_column_name($4.str))
                my_yyabort_error((ER_WRONG_COLUMN_NAME, MYF(0), $4.str));
              $2->is_autogenerated_name= FALSE;
              $2->set_name(thd, $4.str, $4.length, system_charset_info);
            }
            else if (!$2->name)
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
            $$= (char*) YYLIP->get_cpp_tok_end();
          }
        ;

select_alias:
          /* empty */ { $$=null_lex_str;}
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
              if ($$ == NULL)
                MYSQL_YYABORT;
            }
          }
        | expr XOR expr %prec XOR
          {
            /* XOR is a proprietary extension */
            $$= new (thd->mem_root) Item_func_xor(thd, $1, $3);
            if ($$ == NULL)
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
              if ($$ == NULL)
                MYSQL_YYABORT;
            }
          }
        | NOT_SYM expr %prec NOT_SYM
          {
            $$= negate_expression(thd, $2);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr IS TRUE_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_istrue(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr IS not TRUE_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnottrue(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr IS FALSE_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isfalse(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr IS not FALSE_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnotfalse(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr IS UNKNOWN_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnull(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr IS not UNKNOWN_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnotnull(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr IS NULL_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnull(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr IS not NULL_SYM %prec IS
          {
            $$= new (thd->mem_root) Item_func_isnotnull(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr EQUAL_SYM predicate %prec EQUAL_SYM
          {
            $$= new (thd->mem_root) Item_func_equal(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr comp_op predicate %prec '='
          {
            $$= (*$2)(0)->create(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | expr comp_op all_or_any '(' subselect ')' %prec '='
          {
            $$= all_any_subquery_creator(thd, $1, $2, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate
        ;

predicate:
          predicate IN_SYM '(' subselect ')'
          {
            $$= new (thd->mem_root) Item_in_subselect(thd, $1, $4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate not IN_SYM '(' subselect ')'
          {
            Item *item= new (thd->mem_root) Item_in_subselect(thd, $1, $5);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= negate_expression(thd, item);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate IN_SYM '(' expr ')'
          {
            $$= handle_sql2003_note184_exception(thd, $1, true, $4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate IN_SYM '(' expr ',' expr_list ')'
          { 
            $6->push_front($4, thd->mem_root);
            $6->push_front($1, thd->mem_root);
            $$= new (thd->mem_root) Item_func_in(thd, *$6);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate not IN_SYM '(' expr ')'
          {
            $$= handle_sql2003_note184_exception(thd, $1, false, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate not IN_SYM '(' expr ',' expr_list ')'
          {
            $7->push_front($5, thd->mem_root);
            $7->push_front($1, thd->mem_root);
            Item_func_in *item= new (thd->mem_root) Item_func_in(thd, *$7);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= item->neg_transformer(thd);
          }
        | predicate BETWEEN_SYM predicate AND_SYM predicate %prec BETWEEN_SYM
          {
            $$= new (thd->mem_root) Item_func_between(thd, $1, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate not BETWEEN_SYM predicate AND_SYM predicate %prec BETWEEN_SYM
          {
            Item_func_between *item;
            item= new (thd->mem_root) Item_func_between(thd, $1, $4, $6);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= item->neg_transformer(thd);
          }
        | predicate SOUNDS_SYM LIKE predicate
          {
            Item *item1= new (thd->mem_root) Item_func_soundex(thd, $1);
            Item *item4= new (thd->mem_root) Item_func_soundex(thd, $4);
            if ((item1 == NULL) || (item4 == NULL))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_func_eq(thd, item1, item4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate LIKE predicate
          {
            $$= new (thd->mem_root) Item_func_like(thd, $1, $3, escape(thd), false);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate LIKE predicate ESCAPE_SYM predicate %prec LIKE
          {
            Lex->escape_used= true;
            $$= new (thd->mem_root) Item_func_like(thd, $1, $3, $5, true);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate not LIKE predicate
          {
            Item *item= new (thd->mem_root) Item_func_like(thd, $1, $4, escape(thd), false);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= item->neg_transformer(thd);
          }
        | predicate not LIKE predicate ESCAPE_SYM predicate %prec LIKE
          {
            Lex->escape_used= true;
            Item *item= new (thd->mem_root) Item_func_like(thd, $1, $4, $6, true);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= item->neg_transformer(thd);
          }
        | predicate REGEXP predicate
          {
            $$= new (thd->mem_root) Item_func_regex(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | predicate not REGEXP predicate
          {
            Item *item= new (thd->mem_root) Item_func_regex(thd, $1, $4);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= negate_expression(thd, item);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr
        ;

bit_expr:
          bit_expr '|' bit_expr %prec '|'
          {
            $$= new (thd->mem_root) Item_func_bit_or(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '&' bit_expr %prec '&'
          {
            $$= new (thd->mem_root) Item_func_bit_and(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr SHIFT_LEFT bit_expr %prec SHIFT_LEFT
          {
            $$= new (thd->mem_root) Item_func_shift_left(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr SHIFT_RIGHT bit_expr %prec SHIFT_RIGHT
          {
            $$= new (thd->mem_root) Item_func_shift_right(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '+' bit_expr %prec '+'
          {
            $$= new (thd->mem_root) Item_func_plus(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '-' bit_expr %prec '-'
          {
            $$= new (thd->mem_root) Item_func_minus(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '+' INTERVAL_SYM expr interval %prec '+'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $1, $4, $5, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '-' INTERVAL_SYM expr interval %prec '-'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $1, $4, $5, 1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '*' bit_expr %prec '*'
          {
            $$= new (thd->mem_root) Item_func_mul(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '/' bit_expr %prec '/'
          {
            $$= new (thd->mem_root) Item_func_div(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '%' bit_expr %prec '%'
          {
            $$= new (thd->mem_root) Item_func_mod(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr DIV_SYM bit_expr %prec DIV_SYM
          {
            $$= new (thd->mem_root) Item_func_int_div(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr MOD_SYM bit_expr %prec MOD_SYM
          {
            $$= new (thd->mem_root) Item_func_mod(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | bit_expr '^' bit_expr
          {
            $$= new (thd->mem_root) Item_func_bit_xor(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | simple_expr
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
     if ($$ == NULL)
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
         if ($$ == NULL)
           MYSQL_YYABORT;
         $$->push_back($1, thd->mem_root);
       }
   | dyncall_create_list ',' dyncall_create_element
       {
         $1->push_back($3, thd->mem_root);
         $$= $1;
       }
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
        | window_func_expr
        | ROW_SYM '(' expr ',' expr_list ')'
          {
            $5->push_front($3, thd->mem_root);
            $$= new (thd->mem_root) Item_row(thd, *$5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | EXISTS '(' subselect ')'
          {
            $$= new (thd->mem_root) Item_exists_subselect(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | '{' ident expr '}'
          {
            $$= NULL;
            /*
              If "expr" is reasonably short pure ASCII string literal,
              try to parse known ODBC style date, time or timestamp literals,
              e.g:
              SELECT {d'2001-01-01'};
              SELECT {t'10:20:30'};
              SELECT {ts'2001-01-01 10:20:30'};
            */
            if ($3->type() == Item::STRING_ITEM)
            {
              Item_string *item= (Item_string *) $3;
              enum_field_types type= item->odbc_temporal_literal_type(&$2);
              if (type != MYSQL_TYPE_STRING)
              {
                $$= create_temporal_literal(thd, item->val_str(NULL),
                                            type, false);
              }
            }
            if ($$ == NULL)
              $$= $3;
          }
        | MATCH ident_list_arg AGAINST '(' bit_expr fulltext_options ')'
          {
            $2->push_front($5, thd->mem_root);
            Item_func_match *i1= new (thd->mem_root) Item_func_match(thd, *$2,
                                                                     $6);
            if (i1 == NULL)
              MYSQL_YYABORT;
            Select->add_ftfunc_to_list(thd, i1);
            $$= i1;
          }
        | CAST_SYM '(' expr AS cast_type ')'
          {
            LEX *lex= Lex;
            $$= create_func_cast(thd, $3, $5.type(), $5.length(), $5.dec(),
                                 lex->charset);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | CASE_SYM opt_expr when_list opt_else END
          {
            $$= new (thd->mem_root) Item_func_case(thd, *$3, $2, $4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | CONVERT_SYM '(' expr ',' cast_type ')'
          {
            $$= create_func_cast(thd, $3, $5.type(), $5.length(), $5.dec(),
                                 Lex->charset);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | CONVERT_SYM '(' expr USING charset_name ')'
          {
            $$= new (thd->mem_root) Item_func_conv_charset(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | DEFAULT '(' simple_ident ')'
          {
            Item_splocal *il= $3->get_item_splocal();
            if (il)
              my_yyabort_error((ER_WRONG_COLUMN_NAME, MYF(0), il->my_name()->str));
            $$= new (thd->mem_root) Item_default_value_arg(thd, Lex->current_context(),
                                                           $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | VALUES '(' simple_ident_nospvar ')'
          {
            $$= new (thd->mem_root) Item_insert_value(thd, Lex->current_context(),
                                                        $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

simple_expr:
          column_default_non_parenthesized_expr
        | simple_expr COLLATE_SYM ident_or_text %prec NEG
          {
            Item *i1= new (thd->mem_root) Item_string(thd, $3.str,
                                                      $3.length,
                                                      thd->charset());
            if (i1 == NULL)
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_func_set_collation(thd, $1, i1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | '(' parenthesized_expr ')' { $$= $2; }
        | BINARY simple_expr %prec NEG
          {
            $$= create_func_cast(thd, $2, ITEM_CAST_CHAR, NULL, NULL,
                                 &my_charset_bin);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | simple_expr OR_OR_SYM simple_expr
          {
            $$= new (thd->mem_root) Item_func_concat(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | '+' simple_expr %prec NEG
          {
            $$= $2;
          }
        | '-' simple_expr %prec NEG
          {
            $$= $2->neg(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | '~' simple_expr %prec NEG
          {
            $$= new (thd->mem_root) Item_func_bit_neg(thd, $2);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | not2 simple_expr %prec NEG
          {
            $$= negate_expression(thd, $2);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | INTERVAL_SYM expr interval '+' expr %prec INTERVAL_SYM
          /* we cannot put interval before - */
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $5, $2, $3, 0);
            if ($$ == NULL)
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
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | CHAR_SYM '(' expr_list USING charset_name ')'
          {
            $$= new (thd->mem_root) Item_func_char(thd, *$3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | CURRENT_USER optional_braces
          {
            $$= new (thd->mem_root) Item_func_current_user(thd,
                                      Lex->current_context());
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query= 0;
          }
        | CURRENT_ROLE optional_braces
          {
            $$= new (thd->mem_root) Item_func_current_role(thd,
                                      Lex->current_context());
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query= 0;
          }
        | DATE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_date_typecast(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | DAY_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_dayofmonth(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | HOUR_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_hour(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | INSERT '(' expr ',' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_insert(thd, $3, $5, $7, $9);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | INTERVAL_SYM '(' expr ',' expr ')' %prec INTERVAL_SYM
          {
            List<Item> *list= new (thd->mem_root) List<Item>;
            if (list == NULL)
              MYSQL_YYABORT;
            list->push_front($5, thd->mem_root);
            list->push_front($3, thd->mem_root);
            Item_row *item= new (thd->mem_root) Item_row(thd, *list);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_func_interval(thd, item);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | INTERVAL_SYM '(' expr ',' expr ',' expr_list ')' %prec INTERVAL_SYM
          {
            $7->push_front($5, thd->mem_root);
            $7->push_front($3, thd->mem_root);
            Item_row *item= new (thd->mem_root) Item_row(thd, *$7);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_func_interval(thd, item);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | LEFT '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_left(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | MINUTE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_minute(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | MONTH_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_month(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | RIGHT '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_right(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SECOND_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_second(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TIME_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_time_typecast(thd, $3,
                                      AUTO_SEC_PART_DIGITS);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TIMESTAMP '(' expr ')'
          {
            $$= new (thd->mem_root) Item_datetime_typecast(thd, $3,
                                      AUTO_SEC_PART_DIGITS);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TIMESTAMP '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_add_time(thd, $3, $5, 1, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRIM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_trim(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRIM '(' LEADING expr FROM expr ')'
          {
            $$= new (thd->mem_root) Item_func_ltrim(thd, $6, $4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRIM '(' TRAILING expr FROM expr ')'
          {
            $$= new (thd->mem_root) Item_func_rtrim(thd, $6, $4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRIM '(' BOTH expr FROM expr ')'
          {
            $$= new (thd->mem_root) Item_func_trim(thd, $6, $4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRIM '(' LEADING FROM expr ')'
          {
            $$= new (thd->mem_root) Item_func_ltrim(thd, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRIM '(' TRAILING FROM expr ')'
          {
            $$= new (thd->mem_root) Item_func_rtrim(thd, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRIM '(' BOTH FROM expr ')'
          {
            $$= new (thd->mem_root) Item_func_trim(thd, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRIM '(' expr FROM expr ')'
          {
            $$= new (thd->mem_root) Item_func_trim(thd, $5, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | USER_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_func_user(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query=0;
          }
        | YEAR_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_year(thd, $3);
            if ($$ == NULL)
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
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | ADDDATE_SYM '(' expr ',' INTERVAL_SYM expr interval ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $6, $7, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | CURDATE optional_braces
          {
            $$= new (thd->mem_root) Item_func_curdate_local(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | CURTIME opt_time_precision
          {
            $$= new (thd->mem_root) Item_func_curtime_local(thd, $2);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | DATE_ADD_INTERVAL '(' expr ',' INTERVAL_SYM expr interval ')'
          %prec INTERVAL_SYM
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $6, $7, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | DATE_SUB_INTERVAL '(' expr ',' INTERVAL_SYM expr interval ')'
          %prec INTERVAL_SYM
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $6, $7, 1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | EXTRACT_SYM '(' interval FROM expr ')'
          {
            $$=new (thd->mem_root) Item_extract(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | GET_FORMAT '(' date_time_type  ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_get_format(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | NOW_SYM opt_time_precision
          {
            $$= new (thd->mem_root) Item_func_now_local(thd, $2);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | POSITION_SYM '(' bit_expr IN_SYM expr ')'
          {
            $$= new (thd->mem_root) Item_func_locate(thd, $5, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SUBDATE_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $5,
                                                             INTERVAL_DAY, 1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SUBDATE_SYM '(' expr ',' INTERVAL_SYM expr interval ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $3, $6, $7, 1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SUBSTRING '(' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_substr(thd, $3, $5, $7);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SUBSTRING '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_substr(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SUBSTRING '(' expr FROM expr FOR_SYM expr ')'
          {
            $$= new (thd->mem_root) Item_func_substr(thd, $3, $5, $7);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SUBSTRING '(' expr FROM expr ')'
          {
            $$= new (thd->mem_root) Item_func_substr(thd, $3, $5);
            if ($$ == NULL)
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
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | TIMESTAMP_ADD '(' interval_time_stamp ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_date_add_interval(thd, $7, $5, $3, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TIMESTAMP_DIFF '(' interval_time_stamp ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_timestamp_diff(thd, $5, $7, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | UTC_DATE_SYM optional_braces
          {
            $$= new (thd->mem_root) Item_func_curdate_utc(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | UTC_TIME_SYM opt_time_precision
          {
            $$= new (thd->mem_root) Item_func_curtime_utc(thd, $2);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | UTC_TIMESTAMP_SYM opt_time_precision
          {
            $$= new (thd->mem_root) Item_func_now_utc(thd, $2);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        |
          COLUMN_ADD_SYM '(' expr ',' dyncall_create_list ')'
          {
            $$= create_func_dyncol_add(thd, $3, *$5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          COLUMN_DELETE_SYM '(' expr ',' expr_list ')'
          {
            $$= create_func_dyncol_delete(thd, $3, *$5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          COLUMN_CHECK_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_dyncol_check(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          COLUMN_CREATE_SYM '(' dyncall_create_list ')'
          {
            $$= create_func_dyncol_create(thd, *$3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          COLUMN_GET_SYM '(' expr ',' expr AS cast_type ')'
          {
            LEX *lex= Lex;
            $$= create_func_dyncol_get(thd, $3, $5, $7.type(),
                                        $7.length(), $7.dec(),
                                        lex->charset);
            if ($$ == NULL)
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
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | CHARSET '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_charset(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | COALESCE '(' expr_list ')'
          {
            $$= new (thd->mem_root) Item_func_coalesce(thd, *$3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | COLLATION_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_collation(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | DATABASE '(' ')'
          {
            $$= new (thd->mem_root) Item_func_database(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->safe_to_cache_query=0;
          }
        | IF_SYM '(' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_if(thd, $3, $5, $7);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | FORMAT_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_format(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | FORMAT_SYM '(' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_format(thd, $3, $5, $7);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
          /* LAST_VALUE here conflicts with the definition for window functions.
             We have these 2 separate rules to remove the shift/reduce conflict.
          */
        | LAST_VALUE '(' expr ')'
          {
            List<Item> *list= new (thd->mem_root) List<Item>;
            if (list == NULL)
              MYSQL_YYABORT;
            list->push_back($3, thd->mem_root);

            $$= new (thd->mem_root) Item_func_last_value(thd, *list);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | LAST_VALUE '(' expr_list ',' expr ')'
          {
            $3->push_back($5, thd->mem_root);
            $$= new (thd->mem_root) Item_func_last_value(thd, *$3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | MICROSECOND_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_microsecond(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | MOD_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_mod(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | OLD_PASSWORD_SYM '(' expr ')'
          {
            $$=  new (thd->mem_root)
              Item_func_password(thd, $3, Item_func_password::OLD);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | PASSWORD_SYM '(' expr ')'
          {
            Item* i1;
            i1= new (thd->mem_root) Item_func_password(thd, $3);
            if (i1 == NULL)
              MYSQL_YYABORT;
            $$= i1;
          }
        | QUARTER_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_quarter(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | REPEAT_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_repeat(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | REPLACE '(' expr ',' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_replace(thd, $3, $5, $7);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | REVERSE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_reverse(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | ROW_COUNT_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_func_row_count(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
            Lex->safe_to_cache_query= 0;
          }
        | TRUNCATE_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_round(thd, $3, $5, 1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | WEEK_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_func_week(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | WEEK_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_func_week(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | WEIGHT_STRING_SYM '(' expr opt_ws_levels ')'
          {
            $$= new (thd->mem_root) Item_func_weight_string(thd, $3, 0, 0, $4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | WEIGHT_STRING_SYM '(' expr AS CHAR_SYM ws_nweights opt_ws_levels ')'
          {
            $$= new (thd->mem_root)
                Item_func_weight_string(thd, $3, 0, $6,
                                        $7 | MY_STRXFRM_PAD_WITH_SPACE);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | WEIGHT_STRING_SYM '(' expr AS BINARY ws_nweights ')'
          {
            Item *item= new (thd->mem_root) Item_char_typecast(thd, $3, $6,
                                                               &my_charset_bin);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= new (thd->mem_root)
                Item_func_weight_string(thd, item, 0, $6,
                                        MY_STRXFRM_PAD_WITH_SPACE);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | WEIGHT_STRING_SYM '(' expr ',' ulong_num ',' ulong_num ',' ulong_num ')'
          {
            $$= new (thd->mem_root) Item_func_weight_string(thd, $3, $5, $7,
                                                            $9);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | geometry_function
          {
#ifdef HAVE_SPATIAL
            $$= $1;
            /* $1 may be NULL, GEOM_NEW not tested for out of memory */
            if ($$ == NULL)
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
              if (lex->current_select->inc_in_sum_expr())
              {
                my_parse_error(thd, ER_SYNTAX_ERROR);
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

            if (check_routine_name(&$1))
            {
              MYSQL_YYABORT;
            }

            /*
              Implementation note:
              names are resolved with the following order:
              - MySQL native functions,
              - User Defined Functions,
              - Stored Functions (assuming the current <use> database)

              This will be revised with WL#2128 (SQL PATH)
            */
            builder= find_native_function_builder(thd, $1);
            if (builder)
            {
              item= builder->create_func(thd, $1, $4);
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
                item= builder->create_func(thd, $1, $4);
              }
            }

            if (! ($$= item))
            {
              MYSQL_YYABORT;
            }
          }
        | ident '.' ident '(' opt_expr_list ')'
          {
            Create_qfunc *builder;
            Item *item= NULL;

            /*
              The following in practice calls:
              <code>Create_sp_func::create()</code>
              and builds a stored function.

              However, it's important to maintain the interface between the
              parser and the implementation in item_create.cc clean,
              since this will change with WL#2128 (SQL PATH):
              - INFORMATION_SCHEMA.version() is the SQL 99 syntax for the native
              function version(),
              - MySQL.version() is the SQL 2003 syntax for the native function
              version() (a vendor can specify any schema).
            */

            if (!$1.str || check_db_name(&$1))
              my_yyabort_error((ER_WRONG_DB_NAME, MYF(0), $1.str));
            if (check_routine_name(&$3))
            {
              MYSQL_YYABORT;
            }

            builder= find_qualified_function_builder(thd);
            DBUG_ASSERT(builder);
            item= builder->create_with_db(thd, $1, $3, true, $5);

            if (! ($$= item))
            {
              MYSQL_YYABORT;
            }
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
            if ($$ == NULL)
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
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | AVG_SYM '(' DISTINCT in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_avg(thd, $4, TRUE);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | BIT_AND  '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_and(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | BIT_OR  '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_or(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | BIT_XOR  '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_xor(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | COUNT_SYM '(' opt_all '*' ')'
          {
            Item *item= new (thd->mem_root) Item_int(thd, (int32) 0L, 1);
            if (item == NULL)
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_sum_count(thd, item);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | COUNT_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_count(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | COUNT_SYM '(' DISTINCT
          { Select->in_sum_expr++; }
          expr_list
          { Select->in_sum_expr--; }
          ')'
          {
            $$= new (thd->mem_root) Item_sum_count(thd, *$5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | MIN_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_min(thd, $3);
            if ($$ == NULL)
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
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | MAX_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_max(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | MAX_SYM '(' DISTINCT in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_max(thd, $4);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | STD_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_std(thd, $3, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | VARIANCE_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_variance(thd, $3, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | STDDEV_SAMP_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_std(thd, $3, 1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | VAR_SAMP_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_variance(thd, $3, 1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SUM_SYM '(' in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_sum(thd, $3, FALSE);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | SUM_SYM '(' DISTINCT in_sum_expr ')'
          {
            $$= new (thd->mem_root) Item_sum_sum(thd, $4, TRUE);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | GROUP_CONCAT_SYM '(' opt_distinct
          { Select->in_sum_expr++; }
          expr_list opt_gorder_clause
          opt_gconcat_separator
          ')'
          {
            SELECT_LEX *sel= Select;
            sel->in_sum_expr--;
            $$= new (thd->mem_root)
                  Item_func_group_concat(thd, Lex->current_context(), $3, $5,
                                         sel->gorder_list, $7);
            if ($$ == NULL)
              MYSQL_YYABORT;
            $5->empty();
            sel->gorder_list.empty();
          }
        ;

window_func_expr:
          window_func OVER_SYM window_name
          {
            $$= new (thd->mem_root) Item_window_func(thd, (Item_sum *) $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
            if (Select->add_window_func((Item_window_func *) $$))
              MYSQL_YYABORT;
          }
        |
          window_func OVER_SYM window_spec
          {
            LEX *lex= Lex;
            if (Select->add_window_spec(thd, lex->win_ref,
                                        Select->group_list,
                                        Select->order_list,
                                        lex->win_frame))
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_window_func(thd, (Item_sum *) $1,
                                                      thd->lex->win_spec); 
            if ($$ == NULL)
              MYSQL_YYABORT;
            if (Select->add_window_func((Item_window_func *) $$))
              MYSQL_YYABORT;
          }
        ;

window_func:
          simple_window_func
        |
          sum_expr
        ;

simple_window_func:
          ROW_NUMBER_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_row_number(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          RANK_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_rank(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          DENSE_RANK_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_dense_rank(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          PERCENT_RANK_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_percent_rank(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          CUME_DIST_SYM '(' ')'
          {
            $$= new (thd->mem_root) Item_sum_cume_dist(thd);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          NTILE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_ntile(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          FIRST_VALUE_SYM '(' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_first_value(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          LAST_VALUE '(' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_last_value(thd, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          NTH_VALUE_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_nth_value(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          LEAD_SYM '(' expr ')'
          {
            /* No second argument defaults to 1. */
            Item* item_offset= new (thd->mem_root) Item_uint(thd, 1);
            if (item_offset == NULL)
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_sum_lead(thd, $3, item_offset);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          LEAD_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_lead(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          LAG_SYM '(' expr ')'
          {
            /* No second argument defaults to 1. */
            Item* item_offset= new (thd->mem_root) Item_uint(thd, 1);
            if (item_offset == NULL)
              MYSQL_YYABORT;
            $$= new (thd->mem_root) Item_sum_lag(thd, $3, item_offset);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        |
          LAG_SYM '(' expr ',' expr ')'
          {
            $$= new (thd->mem_root) Item_sum_lag(thd, $3, $5);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

window_name:
          ident
          {
            $$= (LEX_STRING *) thd->memdup(&$1, sizeof(LEX_STRING));
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

variable:
          '@'
          {
            if (! Lex->parsing_options.allows_variable)
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
            $$= item= new (thd->mem_root) Item_func_set_user_var(thd, $1, $3);
            if ($$ == NULL)
              MYSQL_YYABORT;
            LEX *lex= Lex;
            lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
            lex->set_var_list.push_back(item, thd->mem_root);
          }
        | ident_or_text
          {
            $$= new (thd->mem_root) Item_func_get_user_var(thd, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
            LEX *lex= Lex;
            lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
          }
        | '@' opt_var_ident_type ident_or_text opt_component
          {
            /* disallow "SELECT @@global.global.variable" */
            if ($3.str && $4.str && check_reserved_words(&$3))
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            if (!($$= get_system_var(thd, $2, $3, $4)))
              MYSQL_YYABORT;
            if (!((Item_func_get_system_var*) $$)->is_written_to_binlog())
              Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_VARIABLE);
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
            if ($$ == NULL)
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
          { if (add_gorder_to_list(thd, $3,(bool) $4)) MYSQL_YYABORT; }
        | order_ident order_dir
          { if (add_gorder_to_list(thd, $1,(bool) $2)) MYSQL_YYABORT; }
        ;

in_sum_expr:
          opt_all
          {
            LEX *lex= Lex;
            if (lex->current_select->inc_in_sum_expr())
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
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
          { $$.set(ITEM_CAST_CHAR, $2); Lex->charset= &my_charset_bin; }
        | CHAR_SYM opt_field_length
          { Lex->charset= thd->variables.collation_connection; }
          opt_binary
          { $$.set(ITEM_CAST_CHAR, $2); }
        | NCHAR_SYM opt_field_length
          {
            Lex->charset= national_charset_info;
            $$.set(ITEM_CAST_CHAR, $2, 0);
          }
        | cast_type_numeric  { $$= $1; Lex->charset= NULL; }
        | cast_type_temporal { $$= $1; Lex->charset= NULL; }
        ;

cast_type_numeric:
          INT_SYM                        { $$.set(ITEM_CAST_SIGNED_INT); }
        | SIGNED_SYM                     { $$.set(ITEM_CAST_SIGNED_INT); }
        | SIGNED_SYM INT_SYM             { $$.set(ITEM_CAST_SIGNED_INT); }
        | UNSIGNED                       { $$.set(ITEM_CAST_UNSIGNED_INT); }
        | UNSIGNED INT_SYM               { $$.set(ITEM_CAST_UNSIGNED_INT); }
        | DECIMAL_SYM float_options      { $$.set(ITEM_CAST_DECIMAL, $2); }
        | DOUBLE_SYM opt_precision       { $$.set(ITEM_CAST_DOUBLE, $2);  }
        ;

cast_type_temporal:
          DATE_SYM                       { $$.set(ITEM_CAST_DATE); }
        | TIME_SYM opt_field_length      { $$.set(ITEM_CAST_TIME, 0, $2); }
        | DATETIME opt_field_length      { $$.set(ITEM_CAST_DATETIME, 0, $2); }
        ;

opt_expr_list:
          /* empty */ { $$= NULL; }
        | expr_list { $$= $1;}
        ;

expr_list:
          expr
          {
            $$= new (thd->mem_root) List<Item>;
            if ($$ == NULL)
              MYSQL_YYABORT;
            $$->push_back($1, thd->mem_root);
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
            if ($$ == NULL)
              MYSQL_YYABORT;
            $$->push_back($1, thd->mem_root);
          }
        | ident_list ',' simple_ident
          {
            $1->push_back($3, thd->mem_root);
            $$= $1;
          }
        ;

opt_expr:
          /* empty */    { $$= NULL; }
        | expr           { $$= $1; }
        ;

opt_else:
          /* empty */  { $$= NULL; }
        | ELSE expr    { $$= $2; }
        ;

when_list:
          WHEN_SYM expr THEN_SYM expr
          {
            $$= new (thd->mem_root) List<Item>;
            if ($$ == NULL)
              MYSQL_YYABORT;
            $$->push_back($2, thd->mem_root);
            $$->push_back($4, thd->mem_root);
          }
        | when_list WHEN_SYM expr THEN_SYM expr
          {
            $1->push_back($3, thd->mem_root);
            $1->push_back($5, thd->mem_root);
            $$= $1;
          }
        ;

/* Equivalent to <table reference> in the SQL:2003 standard. */
/* Warning - may return NULL in case of incomplete SELECT */
table_ref:
          table_factor     { $$= $1; }
        | join_table
          {
            LEX *lex= Lex;
            if (!($$= lex->current_select->nest_last_join(thd)))
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
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
          esc_table_ref { $$=$1; }
        | derived_table_list ',' esc_table_ref
          {
            MYSQL_YYABORT_UNLESS($1 && ($$=$3));
          }
        ;

/*
  Notice that JOIN can be a left-associative operator in one context and
  a right-associative operator in another context (see the comment for
  st_select_lex::add_cross_joined_table).
*/
join_table:
          /* INNER JOIN variants */
          /*
            Use %prec to evaluate production 'table_ref' before 'normal_join'
            so that [INNER | CROSS] JOIN is properly nested as other
            left-associative joins.
          */
          table_ref normal_join table_ref %prec CONDITIONLESS_JOIN
          {
            MYSQL_YYABORT_UNLESS($1 && ($$=$3));

            if (unlikely(Select->add_cross_joined_table($1, $3, $2)))
              MYSQL_YYABORT;
          }
        | table_ref normal_join table_ref
          ON
          {
            MYSQL_YYABORT_UNLESS($1 && $3);
            /* Change the current name resolution context to a local context. */
            if (push_new_name_resolution_context(thd, $1, $3))
              MYSQL_YYABORT;
            Select->parsing_place= IN_ON;
          }
          expr
          {
	    $3->straight=$2;
            add_join_on(thd, $3, $6);
            $3->on_context= Lex->pop_context();
            Select->parsing_place= NO_MATTER;
          }
        | table_ref normal_join table_ref
          USING
          {
            MYSQL_YYABORT_UNLESS($1 && $3);
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
	    $4->straight=$3;
            add_join_natural($1,$4,NULL,Select);
          }

          /* LEFT JOIN variants */
        | table_ref LEFT opt_outer JOIN_SYM table_ref
          ON
          {
            MYSQL_YYABORT_UNLESS($1 && $5);
            /* Change the current name resolution context to a local context. */
            if (push_new_name_resolution_context(thd, $1, $5))
              MYSQL_YYABORT;
            Select->parsing_place= IN_ON;
          }
          expr
          {
            add_join_on(thd, $5, $8);
            $5->on_context= Lex->pop_context();
            $5->outer_join|=JOIN_TYPE_LEFT;
            $$=$5;
            Select->parsing_place= NO_MATTER;
          }
        | table_ref LEFT opt_outer JOIN_SYM table_factor
          {
            MYSQL_YYABORT_UNLESS($1 && $5);
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
            add_join_natural($1,$6,NULL,Select);
            $6->outer_join|=JOIN_TYPE_LEFT;
            $$=$6;
          }

          /* RIGHT JOIN variants */
        | table_ref RIGHT opt_outer JOIN_SYM table_ref
          ON
          {
            MYSQL_YYABORT_UNLESS($1 && $5);
            /* Change the current name resolution context to a local context. */
            if (push_new_name_resolution_context(thd, $1, $5))
              MYSQL_YYABORT;
            Select->parsing_place= IN_ON;
          }
          expr
          {
            LEX *lex= Lex;
            if (!($$= lex->current_select->convert_right_join()))
              MYSQL_YYABORT;
            add_join_on(thd, $$, $8);
            $1->on_context= Lex->pop_context();
            Select->parsing_place= NO_MATTER;
          }
        | table_ref RIGHT opt_outer JOIN_SYM table_factor
          {
            MYSQL_YYABORT_UNLESS($1 && $5);
          }
          USING '(' using_list ')'
          {
            LEX *lex= Lex;
            if (!($$= lex->current_select->convert_right_join()))
              MYSQL_YYABORT;
            add_join_natural($$,$5,$9,Select);
          }
        | table_ref NATURAL RIGHT opt_outer JOIN_SYM table_factor
          {
            MYSQL_YYABORT_UNLESS($1 && $6);
            add_join_natural($6,$1,NULL,Select);
            LEX *lex= Lex;
            if (!($$= lex->current_select->convert_right_join()))
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
  
/* 
   This is a flattening of the rules <table factor> and <table primary>
   in the SQL:2003 standard, since we don't have <sample clause>

   I.e.
   <table factor> ::= <table primary> [ <sample clause> ]
*/   
/* Warning - may return NULL in case of incomplete SELECT */
table_factor:
          table_primary_ident
        | table_primary_derived
        ;

table_primary_ident:
          {
            SELECT_LEX *sel= Select;
            sel->table_join_options= 0;
          }
          table_ident opt_use_partition opt_table_alias opt_key_definition
          {
            if (!($$= Select->add_table_to_list(thd, $2, $4,
                                                Select->get_table_join_options(),
                                                YYPS->m_lock_type,
                                                YYPS->m_mdl_type,
                                                Select->pop_index_hints(),
                                                $3)))
              MYSQL_YYABORT;
            Select->add_joined_table($$);
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
          '(' get_select_lex select_derived_union ')' opt_table_alias
          {
            /* Use $2 instead of Lex->current_select as derived table will
               alter value of Lex->current_select. */
            if (!($3 || $5) && $2->embedding &&
                !$2->embedding->nested_join->join_list.elements)
            {
              /* we have a derived table ($3 == NULL) but no alias,
                 Since we are nested in further parentheses so we
                 can pass NULL to the outer level parentheses
                 Permits parsing of "((((select ...))) as xyz)" */
              $$= 0;
            }
            else if (!$3)
            {
              /* Handle case of derived table, alias may be NULL if there
                 are no outer parentheses, add_table_to_list() will throw
                 error in this case */
              LEX *lex=Lex;
              SELECT_LEX *sel= lex->current_select;
              SELECT_LEX_UNIT *unit= sel->master_unit();
              lex->current_select= sel= unit->outer_select();
              Table_ident *ti= new (thd->mem_root) Table_ident(unit);
              if (ti == NULL)
                MYSQL_YYABORT;
              if (!($$= sel->add_table_to_list(thd,
                                               ti, $5, 0,
                                               TL_READ, MDL_SHARED_READ)))

                MYSQL_YYABORT;
              sel->add_joined_table($$);
              lex->pop_context();
              lex->nest_level--;
            }
            /*else if (($3->select_lex &&
                      $3->select_lex->master_unit()->is_union() &&
                      ($3->select_lex->master_unit()->first_select() ==
                       $3->select_lex || !$3->lifted)) || $5)*/
            else if ($5 != NULL)
            {
              /*
                Tables with or without joins within parentheses cannot
                have aliases, and we ruled out derived tables above.
              */
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            else
            {
              /* nested join: FROM (t1 JOIN t2 ...),
                 nest_level is the same as in the outer query */
              $$= $3;
            }
            /*
              Fields in derived table can be used in upper select in
              case of merge. We do not add HAVING fields because we do
              not merge such derived. We do not add union because
              also do not merge them
            */
            if ($$ && $$->derived &&
                !$$->derived->first_select()->next_select())
              $$->select_lex->add_where_field($$->derived->first_select());
          }
          /* Represents derived table with WITH clause */
        | '(' get_select_lex subselect_start
              with_clause query_expression_body
              subselect_end ')' opt_table_alias
          {
            LEX *lex=Lex;
            SELECT_LEX *sel= $2;
            SELECT_LEX_UNIT *unit= $5->master_unit();
            Table_ident *ti= new (thd->mem_root) Table_ident(unit);
            if (ti == NULL)
              MYSQL_YYABORT;
            $5->set_with_clause($4);
            lex->current_select= sel;
            if (!($$= sel->add_table_to_list(lex->thd,
                                             ti, $8, 0,
                                             TL_READ, MDL_SHARED_READ)))
              MYSQL_YYABORT;
            sel->add_joined_table($$);
          } 
        ;

/*
  This rule accepts just about anything. The reason is that we have
  empty-producing rules in the beginning of rules, in this case
  subselect_start. This forces bison to take a decision which rules to
  reduce by long before it has seen any tokens. This approach ties us
  to a very limited class of parseable languages, and unfortunately
  SQL is not one of them. The chosen 'solution' was this rule, which
  produces just about anything, even complete bogus statements, for
  instance ( table UNION SELECT 1 ).
  Fortunately, we know that the semantic value returned by
  select_derived is NULL if it contained a derived table, and a pointer to
  the base table's TABLE_LIST if it was a base table. So in the rule
  regarding union's, we throw a parse error manually and pretend it
  was bison that did it.
 
  Also worth noting is that this rule concerns query expressions in
  the from clause only. Top level select statements and other types of
  subqueries have their own union rules.
*/
select_derived_union:
          select_derived
        | select_derived union_order_or_limit
          {
            if ($1)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
          }
        | select_derived union_head_non_top
          {
            if ($1)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
          }
          union_list_derived_part2
        | derived_query_specification opt_select_lock_type
        | derived_query_specification order_or_limit opt_select_lock_type
        | derived_query_specification opt_select_lock_type union_list_derived
       ;

union_list_derived_part2:
         query_term_union_not_ready { Lex->pop_context(); }
       | query_term_union_ready     { Lex->pop_context(); }
       | query_term_union_ready     { Lex->pop_context(); } union_list_derived
       ;

union_list_derived:
         union_head_non_top union_list_derived_part2
       ;


/* The equivalent of select_init2 for nested queries. */
select_init2_derived:
          select_part2_derived
          {
            Select->set_braces(0);
          }
        ;

/* The equivalent of select_part2 for nested queries. */
select_part2_derived:
          {
            LEX *lex= Lex;
            SELECT_LEX *sel= lex->current_select;
            if (sel->linkage != UNION_TYPE)
              mysql_init_select(lex);
            lex->current_select->parsing_place= SELECT_LIST;
          }
          opt_query_expression_options select_item_list
          {
            Select->parsing_place= NO_MATTER;
          }
        ;

/* handle contents of parentheses in join expression */
select_derived:
          get_select_lex_derived derived_table_list
          {
            LEX *lex= Lex;
            /* for normal joins, $2 != NULL and end_nested_join() != NULL,
               for derived tables, both must equal NULL */

            if (!($$= $1->end_nested_join(lex->thd)) && $2)
              MYSQL_YYABORT;
            if (!$2 && $$)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
          }
        ;

/*
  Similar to query_specification, but for derived tables.
  Example: the inner parenthesized SELECT in this query:
    SELECT * FROM (SELECT * FROM t1);
*/
derived_query_specification:
          SELECT_SYM select_derived_init select_derived2
          {
            if ($2)
              Select->set_braces(1);
            $$= NULL;
          }
        ;

select_derived2:
          {
            LEX *lex= Lex;
            lex->derived_tables|= DERIVED_SUBQUERY;
            if (!lex->expr_allows_subselect ||
                lex->sql_command == (int)SQLCOM_PURGE)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            if (lex->current_select->linkage == GLOBAL_OPTIONS_TYPE ||
                mysql_new_select(lex, 1))
              MYSQL_YYABORT;
            mysql_init_select(lex);
            lex->current_select->linkage= DERIVED_TABLE_TYPE;
            lex->current_select->parsing_place= SELECT_LIST;
          }
          select_options select_item_list
          {
            Select->parsing_place= NO_MATTER;
          }
          opt_table_expression
        ;

get_select_lex:
          /* Empty */ { $$= Select; }
        ;

get_select_lex_derived:
          get_select_lex
          {
            LEX *lex= Lex;
            if ($1->init_nested_join(lex->thd))
              MYSQL_YYABORT;
          }
       ;

select_derived_init:
          {
            LEX *lex= Lex;

            TABLE_LIST *embedding= lex->current_select->embedding;
            $$= embedding &&
                !embedding->nested_join->join_list.elements;
            /* return true if we are deeply nested */
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
          { Select->add_index_hint(thd, (char *)"PRIMARY", 7); }
        ;

key_usage_list:
          key_usage_element
        | key_usage_list ',' key_usage_element
        ;

using_list:
          ident
          {
            if (!($$= new (thd->mem_root) List<String>))
              MYSQL_YYABORT;
            String *s= new (thd->mem_root) String((const char *) $1.str,
                                                    $1.length,
                                                    system_charset_info);
            if (s == NULL)
              MYSQL_YYABORT;
            $$->push_back(s, thd->mem_root);
          }
        | using_list ',' ident
          {
            String *s= new (thd->mem_root) String((const char *) $3.str,
                                                    $3.length,
                                                    system_charset_info);
            if (s == NULL)
              MYSQL_YYABORT;
            $1->push_back(s, thd->mem_root);
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

opt_table_alias:
          /* empty */ { $$=0; }
        | table_alias ident_table_alias
          {
            $$= (LEX_STRING*) thd->memdup(&$2,sizeof(LEX_STRING));
            if ($$ == NULL)
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

/*
   group by statement in select
*/

opt_group_clause:
          /* empty */
        | GROUP_SYM BY group_list olap_opt
        ;

group_list:
          group_list ',' order_ident order_dir
          { if (add_group_to_list(thd, $3,(bool) $4)) MYSQL_YYABORT; }
        | order_ident order_dir
          { if (add_group_to_list(thd, $1,(bool) $2)) MYSQL_YYABORT; }
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
            if (lex->current_select->linkage == GLOBAL_OPTIONS_TYPE)
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
            if (lex->current_select->linkage == GLOBAL_OPTIONS_TYPE)
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
            if (Select->add_window_def(thd, $1, lex->win_ref,
                                       Select->group_list,
                                       Select->order_list,
                                       lex->win_frame ))
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
            thd->lex->win_ref= (LEX_STRING *) thd->memdup(&$1, sizeof(LEX_STRING));
            if (thd->lex->win_ref == NULL)
              MYSQL_YYABORT;
          }
        ;

opt_window_partition_clause:
          /* empty */ { }
        | PARTITION_SYM BY group_list
        ;

opt_window_order_clause:
          /* empty */ { }
        | ORDER_SYM BY order_list
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
            if (lex->win_frame == NULL)
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
            if (lex->frame_bottom_bound == NULL)
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
            if ($$ == NULL)
              MYSQL_YYABORT;
          } 
        | CURRENT_SYM ROW_SYM
          { 
            $$= new (thd->mem_root)
                  Window_frame_bound(Window_frame_bound::CURRENT, NULL); 
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | literal PRECEDING_SYM
          {
            $$= new (thd->mem_root)
                  Window_frame_bound(Window_frame_bound::PRECEDING, $1); 
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

window_frame_bound:
          window_frame_start { $$= $1; }
        | UNBOUNDED_SYM FOLLOWING_SYM        
          {
            $$= new (thd->mem_root)
                  Window_frame_bound(Window_frame_bound::FOLLOWING, NULL); 
            if ($$ == NULL)
              MYSQL_YYABORT;
          } 
        | literal FOLLOWING_SYM
          {
            $$= new (thd->mem_root)
                  Window_frame_bound(Window_frame_bound::FOLLOWING, $1); 
            if ($$ == NULL)
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
        | EXCLUDE_SYM NO_SYM OTHERS_SYM
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
            if (add_order_to_list(thd, $1, ascending))
              MYSQL_YYABORT;
          }
        ;

/*
   Order by statement in select
*/

opt_order_clause:
          /* empty */
        | order_clause
        ;

order_clause:
          ORDER_SYM BY
          {
            LEX *lex=Lex;
            SELECT_LEX *sel= lex->current_select;
            SELECT_LEX_UNIT *unit= sel-> master_unit();
            if (sel->linkage != GLOBAL_OPTIONS_TYPE &&
                sel->olap != UNSPECIFIED_OLAP_TYPE &&
                (sel->linkage != UNION_TYPE || sel->braces))
            {
              my_error(ER_WRONG_USAGE, MYF(0),
                       "CUBE/ROLLUP", "ORDER BY");
              MYSQL_YYABORT;
            }
            if (lex->sql_command != SQLCOM_ALTER_TABLE &&
                !unit->fake_select_lex)
            {
              /*
                A query of the of the form (SELECT ...) ORDER BY order_list is
                executed in the same way as the query
                SELECT ... ORDER BY order_list
                unless the SELECT construct contains ORDER BY or LIMIT clauses.
                Otherwise we create a fake SELECT_LEX if it has not been created
                yet.
              */
              SELECT_LEX *first_sl= unit->first_select();
              if (!unit->is_union() &&
                  (first_sl->order_list.elements || 
                   first_sl->select_limit) &&            
                  unit->add_fake_select_lex(thd))
                MYSQL_YYABORT;
            }
            if (sel->master_unit()->is_union() && !sel->braces)
            {
               /*
                 At this point we don't know yet whether this is the last
                 select in union or not, but we move ORDER BY to
                 fake_select_lex anyway. If there would be one more select
                 in union mysql_new_select will correctly throw error.
               */
               DBUG_ASSERT(sel->master_unit()->fake_select_lex);
               lex->current_select= sel->master_unit()->fake_select_lex;
             }
          }
          order_list
          {

          }
         ;

order_list:
          order_list ',' order_ident order_dir
          { if (add_order_to_list(thd, $3,(bool) $4)) MYSQL_YYABORT; }
        | order_ident order_dir
          { if (add_order_to_list(thd, $1,(bool) $2)) MYSQL_YYABORT; }
        ;

order_dir:
          /* empty */ { $$ =  1; }
        | ASC  { $$ =1; }
        | DESC { $$ =0; }
        ;

opt_limit_clause:
          /* empty */ {}
        | limit_clause {}
        ;

limit_clause_init:
          LIMIT
          {
            SELECT_LEX *sel= Select;
            if (sel->master_unit()->is_union() && !sel->braces)
            {
              /* Move LIMIT that belongs to UNION to fake_select_lex */
              Lex->current_select= sel->master_unit()->fake_select_lex;
              DBUG_ASSERT(Select);
            }
          }
        ;  

limit_clause:
          limit_clause_init limit_options
          {
            SELECT_LEX *sel= Select;
            if (!sel->select_limit->basic_const_item() ||
                sel->select_limit->val_int() > 0)
              Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
          }
        | limit_clause_init limit_options
          ROWS_SYM EXAMINED_SYM limit_rows_option
          {
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
          }
        | limit_clause_init ROWS_SYM EXAMINED_SYM limit_rows_option
          {
            Lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_LIMIT);
          }
        ;

limit_options:
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

limit_option:
        ident
        {
          Item_splocal *splocal;
          LEX *lex= thd->lex;
          Lex_input_stream *lip= & thd->m_parser_state->m_lip;
          sp_variable *spv;
          sp_pcontext *spc = lex->spcont;
          if (spc && (spv = spc->find_variable($1, false)))
          {
            uint pos_in_query= 0;
            uint len_in_query= 0;
            if (!lex->clone_spec_offset)
            {
              pos_in_query= (uint)(lip->get_tok_start() -
                                   lex->sphead->m_tmp_query);
              len_in_query= (uint)(lip->get_ptr() -
                                   lip->get_tok_start());
            }
            splocal= new (thd->mem_root)
              Item_splocal(thd, $1, spv->offset, spv->sql_type(),
                           pos_in_query, len_in_query);
            if (splocal == NULL)
              MYSQL_YYABORT;
#ifndef DBUG_OFF
            splocal->m_sp= lex->sphead;
#endif
            lex->safe_to_cache_query=0;
          }
          else
            my_yyabort_error((ER_SP_UNDECLARED_VAR, MYF(0), $1.str));
          if (splocal->type() != Item::INT_ITEM)
            my_yyabort_error((ER_WRONG_SPVAR_TYPE_IN_LIMIT, MYF(0)));
          splocal->limit_clause_param= TRUE;
          $$= splocal;
        }
        | param_marker
        {
          $1->limit_clause_param= TRUE;
        }
        | ULONGLONG_NUM
          {
            $$= new (thd->mem_root) Item_uint(thd, $1.str, $1.length);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | LONG_NUM
          {
            $$= new (thd->mem_root) Item_uint(thd, $1.str, $1.length);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | NUM
          {
            $$= new (thd->mem_root) Item_uint(thd, $1.str, $1.length);
            if ($$ == NULL)
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
       | LIMIT ROWS_SYM EXAMINED_SYM { my_parse_error(thd, ER_SYNTAX_ERROR); MYSQL_YYABORT; }
       | LIMIT limit_option ROWS_SYM EXAMINED_SYM { my_parse_error(thd, ER_SYNTAX_ERROR); MYSQL_YYABORT; }
        ;

int_num:
          NUM           { int error; $$= (int) my_strtoll10($1.str, (char**) 0, &error); }
        | '-' NUM       { int error; $$= -(int) my_strtoll10($2.str, (char**) 0, &error); }
        | '-' LONG_NUM  { int error; $$= -(int) my_strtoll10($2.str, (char**) 0, &error); }
        ;

ulong_num:
          NUM           { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | HEX_NUM       { $$= (ulong) strtol($1.str, (char**) 0, 16); }
        | LONG_NUM      { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | ULONGLONG_NUM { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | DECIMAL_NUM   { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | FLOAT_NUM     { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        ;

real_ulong_num:
          NUM           { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | HEX_NUM       { $$= (ulong) strtol($1.str, (char**) 0, 16); }
        | LONG_NUM      { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | ULONGLONG_NUM { int error; $$= (ulong) my_strtoll10($1.str, (char**) 0, &error); }
        | dec_num_error { MYSQL_YYABORT; }
        ;

ulonglong_num:
          NUM           { int error; $$= (ulonglong) my_strtoll10($1.str, (char**) 0, &error); }
        | ULONGLONG_NUM { int error; $$= (ulonglong) my_strtoll10($1.str, (char**) 0, &error); }
        | LONG_NUM      { int error; $$= (ulonglong) my_strtoll10($1.str, (char**) 0, &error); }
        | DECIMAL_NUM   { int error; $$= (ulonglong) my_strtoll10($1.str, (char**) 0, &error); }
        | FLOAT_NUM     { int error; $$= (ulonglong) my_strtoll10($1.str, (char**) 0, &error); }
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
          { my_parse_error(thd, ER_ONLY_INTEGERS_ALLOWED); }
        ;

dec_num:
          DECIMAL_NUM
        | FLOAT_NUM
        ;

choice:
	ulong_num { $$= $1 != 0 ? HA_CHOICE_YES : HA_CHOICE_NO; }
	| DEFAULT { $$= HA_CHOICE_UNDEF; }
	;

procedure_clause:
          PROCEDURE_SYM ident /* Procedure name */
          {
            LEX *lex=Lex;

            DBUG_ASSERT(&lex->select_lex == lex->current_select);

            lex->proc_list.elements=0;
            lex->proc_list.first=0;
            lex->proc_list.next= &lex->proc_list.first;
            Item_field *item= new (thd->mem_root)
                                Item_field(thd, &lex->current_select->context,
                                           NULL, NULL, $2.str);
            if (item == NULL)
              MYSQL_YYABORT;
            if (add_proc_to_list(thd, item))
              MYSQL_YYABORT;
            Lex->uncacheable(UNCACHEABLE_SIDEEFFECT);

            /*
              PROCEDURE CLAUSE cannot handle subquery as one of its parameter,
              so set expr_allows_subselect as false to disallow any subqueries
              further. Reset expr_allows_subselect back to true once the
              parameters are reduced.
            */
            Lex->expr_allows_subselect= false;
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
            if (add_proc_to_list(thd, $2))
              MYSQL_YYABORT;
            if (!$2->name)
              $2->set_name(thd, $1, (uint) ($3 - $1), thd->charset());
          }
        ;

select_var_list_init:
          {
            LEX *lex=Lex;
            if (!lex->describe &&
                (!(lex->result= new (thd->mem_root) select_dumpvar(thd))))
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
              if ($1 == NULL)
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
            $$ = Lex->result ? new (thd->mem_root) my_var_user($2) : NULL;
          }
        | ident_or_text
          {
            sp_variable *t;

            if (!Lex->spcont || !(t= Lex->spcont->find_variable($1, false)))
              my_yyabort_error((ER_SP_UNDECLARED_VAR, MYF(0), $1.str));
            $$ = Lex->result ? (new (thd->mem_root)
                                my_var_sp($1, t->offset, t->sql_type(),
                                          Lex->sphead)) :
                                NULL;
          }
        ;

into:
          INTO into_destination
        ;

into_destination:
          OUTFILE TEXT_STRING_filesystem
          {
            LEX *lex= Lex;
            lex->uncacheable(UNCACHEABLE_SIDEEFFECT);
            if (!(lex->exchange=
                    new (thd->mem_root) sql_exchange($2.str, 0)) ||
                !(lex->result=
                    new (thd->mem_root) select_export(thd, lex->exchange)))
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
              if (!(lex->exchange= new (thd->mem_root) sql_exchange($2.str,1)))
                MYSQL_YYABORT;
              if (!(lex->result=
                      new (thd->mem_root) select_dump(thd, lex->exchange)))
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
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;
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
          table_list opt_restrict
          {}
        | DROP INDEX_SYM opt_if_exists_table_element ident ON table_ident {}
          {
            LEX *lex=Lex;
            Alter_drop *ad= (new (thd->mem_root)
                             Alter_drop(Alter_drop::KEY, $4.str, $3));
            if (ad == NULL)
              MYSQL_YYABORT;
            lex->sql_command= SQLCOM_DROP_INDEX;
            lex->alter_info.reset();
            lex->alter_info.flags= Alter_info::ALTER_DROP_INDEX;
            lex->alter_info.drop_list.push_back(ad, thd->mem_root);
            if (!lex->current_select->add_table_to_list(thd, $6, NULL,
                                                        TL_OPTION_UPDATING,
                                                        TL_READ_NO_INSERT,
                                                        MDL_SHARED_UPGRADABLE))
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
            if ($4.str && check_db_name(&$4))
               my_yyabort_error((ER_WRONG_DB_NAME, MYF(0), $4.str));
            if (lex->sphead)
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "FUNCTION"));
            lex->set_command(SQLCOM_DROP_FUNCTION, $3);
            spname= new (thd->mem_root) sp_name($4, $6, true);
            if (spname == NULL)
              MYSQL_YYABORT;
            spname->init_qname(thd);
            lex->spname= spname;
          }
        | DROP FUNCTION_SYM opt_if_exists ident
          {
            LEX *lex= thd->lex;
            LEX_STRING db= {0, 0};
            sp_name *spname;
            if (lex->sphead)
              my_yyabort_error((ER_SP_NO_DROP_SP, MYF(0), "FUNCTION"));
            if (thd->db && lex->copy_db_to(&db.str, &db.length))
              MYSQL_YYABORT;
            lex->set_command(SQLCOM_DROP_FUNCTION, $3);
            spname= new (thd->mem_root) sp_name(db, $4, false);
            if (spname == NULL)
              MYSQL_YYABORT;
            spname->init_qname(thd);
            lex->spname= spname;
          }
        | DROP PROCEDURE_SYM opt_if_exists sp_name
          {
            LEX *lex=Lex;
            if (lex->sphead)
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
        ;

table_list:
          table_name
        | table_list ',' table_name
        ;

table_name:
          table_ident
          {
            if (!Select->add_table_to_list(thd, $1, NULL,
                                           TL_OPTION_UPDATING,
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type))
              MYSQL_YYABORT;
          }
        ;

table_name_with_opt_use_partition:
          table_ident opt_use_partition
          {
            if (!Select->add_table_to_list(thd, $1, NULL,
                                           TL_OPTION_UPDATING,
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type,
                                           NULL,
                                           $2))
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
            if (!Select->add_table_to_list(thd, $1, NULL,
                                           TL_OPTION_UPDATING | TL_OPTION_ALIAS,
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type))
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
            mysql_init_select(lex);
          }
          insert_lock_option
          opt_ignore insert2
          {
            Select->set_lock_for_tables($3, true);
            Lex->current_select= &Lex->select_lex;
          }
          insert_field_spec opt_insert_update
          {
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;
          }
        ;

replace:
          REPLACE
          {
            LEX *lex=Lex;
            lex->sql_command = SQLCOM_REPLACE;
            lex->duplicates= DUP_REPLACE;
            mysql_init_select(lex);
          }
          replace_lock_option insert2
          {
            Select->set_lock_for_tables($3, true);
            Lex->current_select= &Lex->select_lex;
          }
          insert_field_spec
          {
            if (Lex->check_cte_dependencies_and_resolve_references())
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
          Lex->keyword_delayed_begin_offset= (uint)(YYLIP->get_tok_start() -
                                                    thd->query());
          Lex->keyword_delayed_end_offset= Lex->keyword_delayed_begin_offset +
                                           YYLIP->yyLength() + 1;
          $$= TL_WRITE_DELAYED;
        }
        | HIGH_PRIORITY { $$= TL_WRITE; }
        ;

replace_lock_option:
          opt_low_priority { $$= $1; }
        | DELAYED_SYM
        {
          Lex->keyword_delayed_begin_offset= (uint)(YYLIP->get_tok_start() -
                                                    thd->query());
          Lex->keyword_delayed_end_offset= Lex->keyword_delayed_begin_offset +
                                           YYLIP->yyLength() + 1;
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
            lex->field_list.empty();
            lex->many_values.empty();
            lex->insert_list=0;
          }
        ;

insert_field_spec:
          insert_values {}
        | '(' ')' insert_values {}
        | '(' fields ')' insert_values {}
        | SET
          {
            LEX *lex=Lex;
            if (!(lex->insert_list= new (thd->mem_root) List_item) ||
                lex->many_values.push_back(lex->insert_list, thd->mem_root))
              MYSQL_YYABORT;
          }
          ident_eq_list
        ;

fields:
          fields ',' insert_ident
          { Lex->field_list.push_back($3, thd->mem_root); }
        | insert_ident { Lex->field_list.push_back($1, thd->mem_root); }
        ;

insert_values:
          VALUES values_list {}
        | VALUE_SYM values_list {}
        | create_select_query_expression {}
        ;

values_list:
          values_list ','  no_braces
        | no_braces
        ;

ident_eq_list:
          ident_eq_list ',' ident_eq_value
        | ident_eq_value
        ;

ident_eq_value:
          simple_ident_nospvar equal expr_or_default
          {
            LEX *lex=Lex;
            if (lex->field_list.push_back($1, thd->mem_root) ||
                lex->insert_list->push_back($3, thd->mem_root))
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

no_braces:
          '('
          {
              if (!(Lex->insert_list= new (thd->mem_root) List_item))
                MYSQL_YYABORT;
          }
          opt_values ')'
          {
            LEX *lex=Lex;
            if (lex->many_values.push_back(lex->insert_list, thd->mem_root))
              MYSQL_YYABORT;
          }
        ;

opt_values:
          /* empty */ {}
        | values
        ;

values:
          values ','  expr_or_default
          {
            if (Lex->insert_list->push_back($3, thd->mem_root))
              MYSQL_YYABORT;
          }
        | expr_or_default
          {
            if (Lex->insert_list->push_back($1, thd->mem_root))
              MYSQL_YYABORT;
          }
        ;

expr_or_default:
          expr { $$= $1;}
        | DEFAULT
          {
            $$= new (thd->mem_root) Item_default_value(thd, Lex->current_context());
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | IGNORE_SYM
          {
            $$= new (thd->mem_root) Item_ignore_value(thd, Lex->current_context());
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

opt_insert_update:
          /* empty */
        | ON DUPLICATE_SYM { Lex->duplicates= DUP_UPDATE; }
          KEY_SYM UPDATE_SYM insert_update_list
        ;

/* Update rows in a table */

update:
          UPDATE_SYM
          {
            LEX *lex= Lex;
            mysql_init_select(lex);
            lex->sql_command= SQLCOM_UPDATE;
            lex->duplicates= DUP_ERROR; 
          }
          opt_low_priority opt_ignore join_table_list
          SET update_list
          {
            SELECT_LEX *slex= &Lex->select_lex;
            if (slex->table_list.elements > 1)
              Lex->sql_command= SQLCOM_UPDATE_MULTI;
            else if (slex->get_table_list()->derived)
            {
              /* it is single table update and it is update of derived table */
              my_error(ER_NON_UPDATABLE_TABLE, MYF(0),
                       slex->get_table_list()->alias, "UPDATE");
              MYSQL_YYABORT;
            }
            /*
              In case of multi-update setting write lock for all tables may
              be too pessimistic. We will decrease lock level if possible in
              mysql_multi_update().
            */
            slex->set_lock_for_tables($3, slex->table_list.elements == 1);
          }
          opt_where_clause opt_order_clause delete_limit_clause
          {
            if (Lex->check_cte_dependencies_and_resolve_references())
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
            if (add_item_to_list(thd, $1) || add_value_to_list(thd, $3))
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
          if (lex->update_list.push_back($1, thd->mem_root) || 
              lex->value_list.push_back($3, thd->mem_root))
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

            lex->ignore= 0;
            lex->select_lex.init_order();
          }
          opt_delete_options single_multi
          {
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;
          }
        ;

single_multi:
          FROM table_ident opt_use_partition
          {
            if (!Select->add_table_to_list(thd, $2, NULL, TL_OPTION_UPDATING,
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type,
                                           NULL,
                                           $3))
              MYSQL_YYABORT;
            YYPS->m_lock_type= TL_READ_DEFAULT;
            YYPS->m_mdl_type= MDL_SHARED_READ;
          }
          opt_where_clause opt_order_clause
          delete_limit_clause {}
          opt_select_expressions {}
        | table_wild_list
          {
            mysql_init_multi_delete(Lex);
            YYPS->m_lock_type= TL_READ_DEFAULT;
            YYPS->m_mdl_type= MDL_SHARED_READ;
          }
          FROM join_table_list opt_where_clause
          {
            if (multi_delete_set_locks_and_link_aux_tables(Lex))
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
            if (multi_delete_set_locks_and_link_aux_tables(Lex))
              MYSQL_YYABORT;
            if (Lex->check_cte_dependencies_and_resolve_references())
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
            Table_ident *ti= new (thd->mem_root) Table_ident($1);
            if (ti == NULL)
              MYSQL_YYABORT;
            if (!Select->add_table_to_list(thd,
                                           ti,
                                           NULL,
                                           TL_OPTION_UPDATING | TL_OPTION_ALIAS,
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type))
              MYSQL_YYABORT;
          }
        | ident '.' ident opt_wild
          {
            Table_ident *ti= new (thd->mem_root) Table_ident(thd, $1, $3, 0);
            if (ti == NULL)
              MYSQL_YYABORT;
            if (!Select->add_table_to_list(thd,
                                           ti,
                                           NULL,
                                           TL_OPTION_UPDATING | TL_OPTION_ALIAS,
                                           YYPS->m_lock_type,
                                           YYPS->m_mdl_type))
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
          TRUNCATE_SYM opt_table_sym
          {
            LEX* lex= Lex;
            lex->sql_command= SQLCOM_TRUNCATE;
            lex->alter_info.reset();
            lex->select_lex.options= 0;
            lex->select_lex.sql_cache= SELECT_LEX::SQL_CACHE_UNSPECIFIED;
            lex->select_lex.init_order();
            YYPS->m_lock_type= TL_WRITE;
            YYPS->m_mdl_type= MDL_EXCLUSIVE;
          }
          table_name
          {
            LEX* lex= thd->lex;
            DBUG_ASSERT(!lex->m_sql_cmd);
            lex->m_sql_cmd= new (thd->mem_root) Sql_cmd_truncate_table();
            if (lex->m_sql_cmd == NULL)
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
            lex->ident=null_lex_str;
            mysql_init_select(lex);
            lex->current_select->parsing_place= SELECT_LIST;
            lex->create_info.init();
          }
          show_param
          {
            Select->parsing_place= NO_MATTER;
          }
        ;

show_param:
           DATABASES wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_DATABASES;
             if (prepare_schema_table(thd, lex, 0, SCH_SCHEMATA))
               MYSQL_YYABORT;
           }
         | opt_full TABLES opt_db wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_TABLES;
             lex->select_lex.db= $3;
             if (prepare_schema_table(thd, lex, 0, SCH_TABLE_NAMES))
               MYSQL_YYABORT;
           }
         | opt_full TRIGGERS_SYM opt_db wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_TRIGGERS;
             lex->select_lex.db= $3;
             if (prepare_schema_table(thd, lex, 0, SCH_TRIGGERS))
               MYSQL_YYABORT;
           }
         | EVENTS_SYM opt_db wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_EVENTS;
             lex->select_lex.db= $2;
             if (prepare_schema_table(thd, lex, 0, SCH_EVENTS))
               MYSQL_YYABORT;
           }
         | TABLE_SYM STATUS_SYM opt_db wild_and_where
           {
             LEX *lex= Lex;
             lex->sql_command= SQLCOM_SHOW_TABLE_STATUS;
             lex->select_lex.db= $3;
             if (prepare_schema_table(thd, lex, 0, SCH_TABLES))
               MYSQL_YYABORT;
           }
        | OPEN_SYM TABLES opt_db wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_OPEN_TABLES;
            lex->select_lex.db= $3;
            if (prepare_schema_table(thd, lex, 0, SCH_OPEN_TABLES))
              MYSQL_YYABORT;
          }
        | PLUGINS_SYM
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_PLUGINS;
            if (prepare_schema_table(thd, lex, 0, SCH_PLUGINS))
              MYSQL_YYABORT;
          }
        | PLUGINS_SYM SONAME_SYM TEXT_STRING_sys
          {
            Lex->ident= $3;
            Lex->sql_command= SQLCOM_SHOW_PLUGINS;
            if (prepare_schema_table(thd, Lex, 0, SCH_ALL_PLUGINS))
              MYSQL_YYABORT;
          }
        | PLUGINS_SYM SONAME_SYM wild_and_where
          {
            Lex->sql_command= SQLCOM_SHOW_PLUGINS;
            if (prepare_schema_table(thd, Lex, 0, SCH_ALL_PLUGINS))
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
            if ($5)
              $4->change_db($5);
            if (prepare_schema_table(thd, lex, $4, SCH_COLUMNS))
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
          opt_limit_clause
        | RELAYLOG_SYM optional_connection_name EVENTS_SYM binlog_in binlog_from
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_RELAYLOG_EVENTS;
          } opt_limit_clause
        | keys_or_index from_or_in table_ident opt_db opt_where_clause
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_KEYS;
            if ($4)
              $3->change_db($4);
            if (prepare_schema_table(thd, lex, $3, SCH_STATISTICS))
              MYSQL_YYABORT;
          }
        | opt_storage ENGINES_SYM
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_SHOW_STORAGE_ENGINES;
            if (prepare_schema_table(thd, lex, 0, SCH_ENGINES))
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
          { (void) create_select_for_variable("warning_count"); }
        | COUNT_SYM '(' '*' ')' ERRORS
          { (void) create_select_for_variable("error_count"); }
        | WARNINGS opt_limit_clause
          { Lex->sql_command = SQLCOM_SHOW_WARNS;}
        | ERRORS opt_limit_clause
          { Lex->sql_command = SQLCOM_SHOW_ERRORS;}
        | PROFILES_SYM
          { Lex->sql_command = SQLCOM_SHOW_PROFILES; }
        | PROFILE_SYM opt_profile_defs opt_profile_args opt_limit_clause
          { 
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_PROFILE;
            if (prepare_schema_table(thd, lex, NULL, SCH_PROFILES) != 0)
              MYSQL_YYABORT;
          }
        | opt_var_type STATUS_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_STATUS;
            lex->option_type= $1;
            if (prepare_schema_table(thd, lex, 0, SCH_SESSION_STATUS))
              MYSQL_YYABORT;
          }
        | opt_full PROCESSLIST_SYM
          { Lex->sql_command= SQLCOM_SHOW_PROCESSLIST;}
        | opt_var_type  VARIABLES wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_VARIABLES;
            lex->option_type= $1;
            if (prepare_schema_table(thd, lex, 0, SCH_SESSION_VARIABLES))
              MYSQL_YYABORT;
          }
        | charset wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_CHARSETS;
            if (prepare_schema_table(thd, lex, 0, SCH_CHARSETS))
              MYSQL_YYABORT;
          }
        | COLLATION_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_COLLATIONS;
            if (prepare_schema_table(thd, lex, 0, SCH_COLLATIONS))
              MYSQL_YYABORT;
          }
        | GRANTS
          {
            Lex->sql_command= SQLCOM_SHOW_GRANTS;
            if (!(Lex->grant_user= (LEX_USER*)thd->alloc(sizeof(LEX_USER))))
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
            if (!lex->select_lex.add_table_to_list(thd, $3, NULL,0))
              MYSQL_YYABORT;
            lex->create_info.storage_media= HA_SM_DEFAULT;
          }
        | CREATE VIEW_SYM table_ident
          {
            LEX *lex= Lex;
            lex->sql_command = SQLCOM_SHOW_CREATE;
            if (!lex->select_lex.add_table_to_list(thd, $3, NULL, 0))
              MYSQL_YYABORT;
            lex->only_view= 1;
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
            lex->mi.connection_name= null_lex_str;
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
        | CREATE TRIGGER_SYM sp_name
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_CREATE_TRIGGER;
            lex->spname= $3;
          }
        | CREATE USER_SYM
          {
            Lex->sql_command= SQLCOM_SHOW_CREATE_USER;
            if (!(Lex->grant_user= (LEX_USER*)thd->alloc(sizeof(LEX_USER))))
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
            if (prepare_schema_table(thd, lex, 0, SCH_PROCEDURES))
              MYSQL_YYABORT;
          }
        | FUNCTION_SYM STATUS_SYM wild_and_where
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_SHOW_STATUS_FUNC;
            if (prepare_schema_table(thd, lex, 0, SCH_PROCEDURES))
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
        | CREATE EVENT_SYM sp_name
          {
            Lex->spname= $3;
            Lex->sql_command = SQLCOM_SHOW_CREATE_EVENT;
          }
        | describe_command FOR_SYM expr
          {
            Lex->sql_command= SQLCOM_SHOW_EXPLAIN;
            if (prepare_schema_table(thd, Lex, 0, SCH_EXPLAIN))
              MYSQL_YYABORT;
            add_value_to_list(thd, $3);
          }
        | IDENT_sys remember_tok_start wild_and_where
           {
             LEX *lex= Lex;
             bool in_plugin;
             lex->sql_command= SQLCOM_SHOW_GENERIC;
             ST_SCHEMA_TABLE *table= find_schema_table(thd, $1.str, &in_plugin);
             if (!table || !table->old_format || !in_plugin)
             {
               my_parse_error(thd, ER_SYNTAX_ERROR, $2);
               MYSQL_YYABORT;
             }
             if (lex->wild && table->idx_field1 < 0)
             {
               my_parse_error(thd, ER_SYNTAX_ERROR, $3);
               MYSQL_YYABORT;
             }
             if (make_schema_select(thd, Lex->current_select, table))
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
          /* empty */  { $$= 0; }
        | from_or_in ident { $$= $2.str; }
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
            if (Lex->wild == NULL)
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
            lex->select_lex.db= 0;
            lex->verbose= 0;
            if (prepare_schema_table(thd, lex, $2, SCH_COLUMNS))
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
            lex->select_lex.options|= SELECT_DESCRIBE;
          }
        ;

explainable_command:
          select
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
        | PARTITIONS_SYM { Lex->describe|= DESCRIBE_PARTITIONS; }
        | opt_format_json {}
        ;

opt_format_json:
          /* empty */ {}
        | FORMAT_SYM '=' ident_or_text
          {
            if (!my_strcasecmp(system_charset_info, $3.str, "JSON"))
              Lex->explain_json= true;
            else if (!my_strcasecmp(system_charset_info, $3.str, "TRADITIONAL"))
              DBUG_ASSERT(Lex->explain_json==false);
            else
              my_yyabort_error((ER_UNKNOWN_EXPLAIN_FORMAT, MYF(0), $3.str));
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
            if (Lex->wild == NULL)
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
          flush_options
          {}
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
        ;

opt_flush_lock:
          /* empty */ {}
        | flush_lock
        {
          TABLE_LIST *tables= Lex->query_tables;
          for (; tables; tables= tables->next_global)
          {
            tables->mdl_request.set_type(MDL_SHARED_NO_WRITE);
            tables->required_type= FRMTYPE_TABLE; /* Don't try to flush views. */
            tables->open_type= OT_BASE_ONLY;      /* Ignore temporary tables. */
          }
        }
        ;

flush_lock:
          WITH READ_SYM LOCK_SYM optional_flush_tables_arguments
          { Lex->type|= REFRESH_READ_LOCK | $4; }
        | FOR_SYM
          {
            if (Lex->query_tables == NULL) // Table list can't be empty
            {
              my_parse_error(thd, ER_NO_TABLES_USED);
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
            if (lex->type & REFRESH_RELAY_LOG)
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
            Lex->relay_log_connection_name= empty_lex_str;
          }
        | STATUS_SYM
          { Lex->type|= REFRESH_STATUS; }
        | SLAVE optional_connection_name 
          { 
            LEX *lex= Lex;
            if (lex->type & REFRESH_SLAVE)
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
             ST_SCHEMA_TABLE *table= find_schema_table(thd, $1.str);
             if (!table || !table->reset_table)
             {
               my_parse_error(thd, ER_SYNTAX_ERROR, $2);
               MYSQL_YYABORT;
             }
             Lex->view_list.push_back((LEX_STRING*)
                                       thd->memdup(&$1, sizeof(LEX_STRING)),
                                       thd->mem_root);
           }
        ;

opt_table_list:
          /* empty */  {}
        | table_list {}
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
          kill_type kill_option
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
          opt_connection kill_expr { $$= (int) KILL_CONNECTION; }
        | QUERY_SYM      kill_expr { $$= (int) KILL_QUERY; }
        | QUERY_SYM ID_SYM expr
          {
            $$= (int) KILL_QUERY;
            Lex->kill_type= KILL_TYPE_QUERY;
            Lex->value_list.push_front($3, thd->mem_root);
          }
        ;

opt_connection:
          /* empty */    { }
        | CONNECTION_SYM { }
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
            lex->select_lex.db= $2.str;
          }
        ;

/* import, export of files */

load:
          LOAD data_or_xml
          {
            LEX *lex= thd->lex;
            mysql_init_select(lex);

            if (lex->sphead)
            {
              my_error(ER_SP_BADSTATEMENT, MYF(0), 
                       $2 == FILETYPE_CSV ? "LOAD DATA" : "LOAD XML");
              MYSQL_YYABORT;
            }
          }
          load_data_lock opt_local INFILE TEXT_STRING_filesystem
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_LOAD;
            lex->local_file=  $5;
            lex->duplicates= DUP_ERROR;
            lex->ignore= 0;
            if (!(lex->exchange= new (thd->mem_root) sql_exchange($7.str, 0, $2)))
              MYSQL_YYABORT;
          }
          opt_duplicate INTO TABLE_SYM table_ident opt_use_partition
          {
            LEX *lex=Lex;
            if (!Select->add_table_to_list(thd, $12, NULL, TL_OPTION_UPDATING,
                                           $4, MDL_SHARED_WRITE, NULL, $13))
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
            if (Lex->check_cte_dependencies_and_resolve_references())
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
          { Lex->exchange->line_term = $4; }
        ;

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
            $$= new (thd->mem_root) Item_user_var_as_out_param(thd, $2);
            if ($$ == NULL)
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
            if (lex->update_list.push_back($1, thd->mem_root) || 
                lex->value_list.push_back($4, thd->mem_root))
                MYSQL_YYABORT;
            $4->set_name_no_truncate(thd, $3, (uint) ($5 - $3), thd->charset());
          }
        ;

/* Common definitions */

text_literal:
          TEXT_STRING
          {
            LEX_STRING tmp;
            CHARSET_INFO *cs_con= thd->variables.collation_connection;
            CHARSET_INFO *cs_cli= thd->variables.character_set_client;
            uint repertoire= thd->lex->text_string_is_7bit &&
                             my_charset_is_ascii_based(cs_cli) ?
                             MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
            if (thd->charset_is_collation_connection ||
                (repertoire == MY_REPERTOIRE_ASCII &&
                 my_charset_is_ascii_based(cs_con)))
              tmp= $1;
            else
            {
              if (thd->convert_string(&tmp, cs_con, $1.str, $1.length, cs_cli))
                MYSQL_YYABORT;
            }
            $$= new (thd->mem_root) Item_string(thd, tmp.str, tmp.length,
                                                cs_con,
                                                DERIVATION_COERCIBLE,
                                                repertoire);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | NCHAR_STRING
          {
            uint repertoire= Lex->text_string_is_7bit ?
                             MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30;
            DBUG_ASSERT(my_charset_is_ascii_based(national_charset_info));
            $$= new (thd->mem_root) Item_string(thd, $1.str, $1.length,
                                                  national_charset_info,
                                                  DERIVATION_COERCIBLE,
                                                  repertoire);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | UNDERSCORE_CHARSET TEXT_STRING
          {
            $$= new (thd->mem_root) Item_string_with_introducer(thd, $2.str,
                                                                $2.length, $1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | text_literal TEXT_STRING_literal
          {
            Item_string* item= (Item_string*) $1;
            item->append($2.str, $2.length);
            if (!(item->collation.repertoire & MY_REPERTOIRE_EXTENDED))
            {
              /*
                 If the string has been pure ASCII so far,
                 check the new part.
              */
              CHARSET_INFO *cs= thd->variables.collation_connection;
              item->collation.repertoire|= my_string_repertoire(cs,
                                                                $2.str,
                                                                $2.length);
            }
          }
        ;

text_string:
          TEXT_STRING_literal
          {
            $$= new (thd->mem_root) String($1.str,
                                             $1.length,
                                             thd->variables.collation_connection);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
          | hex_or_bin_String { $$= $1; }
          ;


hex_or_bin_String:
          HEX_NUM
          {
            Item *tmp= new (thd->mem_root) Item_hex_hybrid(thd, $1.str,
                                                           $1.length);
            if (tmp == NULL)
              MYSQL_YYABORT;
            /*
              it is OK only emulate fix_fields, because we need only
              value of constant
            */
            tmp->quick_fix_field();
            $$= tmp->val_str((String*) 0);
          }
        | HEX_STRING
          {
            Item *tmp= new (thd->mem_root) Item_hex_string(thd, $1.str,
                                                           $1.length);
            if (tmp == NULL)
              MYSQL_YYABORT;
            tmp->quick_fix_field();
            $$= tmp->val_str((String*) 0);
          }
        | BIN_NUM
          {
            Item *tmp= new (thd->mem_root) Item_bin_string(thd, $1.str,
                                                           $1.length);
            if (tmp == NULL)
              MYSQL_YYABORT;
            /*
              it is OK only emulate fix_fields, because we need only
              value of constant
            */
            tmp->quick_fix_field();
            $$= tmp->val_str((String*) 0);
          }
        ;

param_marker:
          PARAM_MARKER
          {
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;
            Item_param *item;
            bool rc;
            if (! lex->parsing_options.allows_variable)
              my_yyabort_error((ER_VIEW_SELECT_VARIABLE, MYF(0)));
            const char *query_start= lex->sphead && !lex->clone_spec_offset ?
                                     lex->sphead->m_tmp_query : lip->get_buf();
            item= new (thd->mem_root) Item_param(thd,
                                                 (uint)(lip->get_tok_start() -
                                                        query_start));
            if (!($$= item))
              MYSQL_YYABORT;
            if (!lex->clone_spec_offset)
              rc= lex->param_list.push_back(item, thd->mem_root);
            else
              rc= item->add_as_clone(thd);
            if (rc)
              my_yyabort_error((ER_OUT_OF_RESOURCES, MYF(0)));

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
            if ($$ == NULL)
              MYSQL_YYABORT;
            YYLIP->next_state= MY_LEX_OPERATOR_OR_IDENT;
          }
        | FALSE_SYM
          {
            $$= new (thd->mem_root) Item_bool(thd, (char*) "FALSE",0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | TRUE_SYM
          {
            $$= new (thd->mem_root) Item_bool(thd, (char*) "TRUE",1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | HEX_NUM
          {
            $$= new (thd->mem_root) Item_hex_hybrid(thd, $1.str, $1.length);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | HEX_STRING
          {
            $$= new (thd->mem_root) Item_hex_string(thd, $1.str, $1.length);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | BIN_NUM
          {
            $$= new (thd->mem_root) Item_bin_string(thd, $1.str, $1.length);
            if ($$ == NULL)
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
            if (!item_str || !item_str->check_well_formed_result(true))
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
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | LONG_NUM
          {
            int error;
            $$= new (thd->mem_root)
                  Item_int(thd, $1.str,
                           (longlong) my_strtoll10($1.str, NULL, &error),
                           $1.length);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | ULONGLONG_NUM
          {
            $$= new (thd->mem_root) Item_uint(thd, $1.str, $1.length);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | DECIMAL_NUM
          {
            $$= new (thd->mem_root) Item_decimal(thd, $1.str, $1.length,
                                                   thd->charset());
            if (($$ == NULL) || (thd->is_error()))
            {
              MYSQL_YYABORT;
            }
          }
        | FLOAT_NUM
          {
            $$= new (thd->mem_root) Item_float(thd, $1.str, $1.length);
            if (($$ == NULL) || (thd->is_error()))
            {
              MYSQL_YYABORT;
            }
          }
        ;


temporal_literal:
        DATE_SYM TEXT_STRING
          {
            if (!($$= create_temporal_literal(thd, $2.str, $2.length, YYCSCL,
                                              MYSQL_TYPE_DATE, true)))
              MYSQL_YYABORT;
          }
        | TIME_SYM TEXT_STRING
          {
            if (!($$= create_temporal_literal(thd, $2.str, $2.length, YYCSCL,
                                              MYSQL_TYPE_TIME, true)))
              MYSQL_YYABORT;
          }
        | TIMESTAMP TEXT_STRING
          {
            if (!($$= create_temporal_literal(thd, $2.str, $2.length, YYCSCL,
                                              MYSQL_TYPE_DATETIME, true)))
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
             With_clause *with_clause=
             new With_clause($2, Lex->curr_with_clause);
             if (with_clause == NULL)
               MYSQL_YYABORT;
             Lex->derived_tables|= DERIVED_WITH;
             Lex->with_cte_resolution= true;
             Lex->with_cte_resolution= true;
             Lex->curr_with_clause= with_clause;
             with_clause->add_to_list(Lex->with_clauses_list_last_next);
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
          with_element_head
	  opt_with_column_list 
          {
            $2= new List<LEX_STRING> (Lex->with_column_list);
            if ($2 == NULL)
              MYSQL_YYABORT;
            Lex->with_column_list.empty();
          }
          AS '(' remember_tok_start subselect remember_tok_end ')'
 	  {
            LEX *lex= thd->lex;
            const char *query_start= lex->sphead ? lex->sphead->m_tmp_query
                                                 : thd->query();
            char *spec_start= $6 + 1;
            With_element *elem= new With_element($1, *$2, $7->master_unit());
	    if (elem == NULL || Lex->curr_with_clause->add_with_element(elem))
	      MYSQL_YYABORT;
            if (elem->set_unparsed_spec(thd, spec_start, $8,
                                        (uint) (spec_start - query_start)))
              MYSQL_YYABORT;
            elem->set_tables_end_pos(lex->query_tables_last);
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
            Lex->with_column_list.push_back((LEX_STRING*)
                    thd->memdup(&$1, sizeof(LEX_STRING)));
	  }
        | with_column_list ',' ident
          {
            Lex->with_column_list.push_back((LEX_STRING*)
                    thd->memdup(&$3, sizeof(LEX_STRING)));
          }
        ;


with_element_head:
          ident
          {
            LEX_CSTRING *name=
              (LEX_CSTRING *) thd->memdup(&$1, sizeof(LEX_CSTRING));
            $$= new (thd->mem_root) With_element_head(name);
            if (unlikely(name == NULL || $$ == NULL))
              MYSQL_YYABORT;
            $$->tables_pos.set_start_pos(Lex->query_tables_last);
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
            SELECT_LEX *sel= Select;
            $$= new (thd->mem_root) Item_field(thd, Lex->current_context(),
                                                 NullS, $1.str, "*");
            if ($$ == NULL)
              MYSQL_YYABORT;
            sel->with_wild++;
          }
        | ident '.' ident '.' '*'
          {
            SELECT_LEX *sel= Select;
            const char* schema= thd->client_capabilities & CLIENT_NO_SCHEMA ?
                                  NullS : $1.str;
            $$= new (thd->mem_root) Item_field(thd, Lex->current_context(),
                                               schema,
                                               $3.str,"*");
            if ($$ == NULL)
              MYSQL_YYABORT;
            sel->with_wild++;
          }
        ;

order_ident:
          expr { $$=$1; }
        ;

simple_ident:
          ident
          {
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;
            sp_variable *spv;
            sp_pcontext *spc = lex->spcont;
            if (spc && (spv = spc->find_variable($1, false)))
            {
              /* We're compiling a stored procedure and found a variable */
              if (! lex->parsing_options.allows_variable)
                my_yyabort_error((ER_VIEW_SELECT_VARIABLE, MYF(0)));

              Item_splocal *splocal;
              uint pos_in_query= 0;
              uint len_in_query= 0;
              if (!lex->clone_spec_offset)
              {
                pos_in_query= (uint)(lip->get_tok_start_prev() -
                                     lex->sphead->m_tmp_query);
                len_in_query= (uint)(lip->get_tok_end() -
                                     lip->get_tok_start_prev());
              }
              splocal= new (thd->mem_root)
                         Item_splocal(thd, $1, spv->offset, spv->sql_type(),
                                      pos_in_query, len_in_query);
              if (splocal == NULL)
                MYSQL_YYABORT;
#ifndef DBUG_OFF
              splocal->m_sp= lex->sphead;
#endif
              $$= splocal;
              lex->safe_to_cache_query=0;
            }
            else
            {
              SELECT_LEX *sel=Select;
              if ((sel->parsing_place != IN_HAVING) ||
                  (sel->get_in_sum_expr() > 0))
              {
                $$= new (thd->mem_root) Item_field(thd, Lex->current_context(),
                                                   NullS, NullS, $1.str);
              }
              else
              {
                $$= new (thd->mem_root) Item_ref(thd, Lex->current_context(),
                                                 NullS, NullS, $1.str);
              }
              if ($$ == NULL)
                MYSQL_YYABORT;
            }
          }
        | simple_ident_q { $$= $1; }
        ;

simple_ident_nospvar:
          ident
          {
            SELECT_LEX *sel=Select;
            if ((sel->parsing_place != IN_HAVING) ||
                (sel->get_in_sum_expr() > 0))
            {
              $$= new (thd->mem_root) Item_field(thd, Lex->current_context(),
                                                 NullS, NullS, $1.str);
            }
            else
            {
              $$= new (thd->mem_root) Item_ref(thd, Lex->current_context(),
                                               NullS, NullS, $1.str);
            }
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | simple_ident_q { $$= $1; }
        ;

simple_ident_q:
          ident '.' ident
          {
            LEX *lex= thd->lex;

            /*
              FIXME This will work ok in simple_ident_nospvar case because
              we can't meet simple_ident_nospvar in trigger now. But it
              should be changed in future.
            */
            if (lex->sphead && lex->sphead->m_type == TYPE_ENUM_TRIGGER &&
                (!my_strcasecmp(system_charset_info, $1.str, "NEW") ||
                 !my_strcasecmp(system_charset_info, $1.str, "OLD")))
            {
              Item_trigger_field *trg_fld;
              bool new_row= ($1.str[0]=='N' || $1.str[0]=='n');

              if (lex->trg_chistics.event == TRG_EVENT_INSERT &&
                  !new_row)
                my_yyabort_error((ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0), "OLD", "on INSERT"));

              if (lex->trg_chistics.event == TRG_EVENT_DELETE &&
                  new_row)
                my_yyabort_error((ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0), "NEW", "on DELETE"));

              DBUG_ASSERT(!new_row ||
                          (lex->trg_chistics.event == TRG_EVENT_INSERT ||
                           lex->trg_chistics.event == TRG_EVENT_UPDATE));
              const bool tmp_read_only=
                !(new_row && lex->trg_chistics.action_time == TRG_ACTION_BEFORE);
              trg_fld= new (thd->mem_root)
                         Item_trigger_field(thd, Lex->current_context(),
                                            new_row ?
                                              Item_trigger_field::NEW_ROW:
                                              Item_trigger_field::OLD_ROW,
                                            $3.str,
                                            SELECT_ACL,
                                            tmp_read_only);
              if (trg_fld == NULL)
                MYSQL_YYABORT;

              /*
                Let us add this item to list of all Item_trigger_field objects
                in trigger.
              */
              lex->trg_table_fields.link_in_list(trg_fld,
                                                 &trg_fld->next_trg_field);

              $$= trg_fld;
            }
            else
            {
              SELECT_LEX *sel= lex->current_select;
              if (sel->no_table_names_allowed)
              {
                my_error(ER_TABLENAME_NOT_ALLOWED_HERE,
                         MYF(0), $1.str, thd->where);
              }
              if ((sel->parsing_place != IN_HAVING) ||
                  (sel->get_in_sum_expr() > 0))
              {
                $$= new (thd->mem_root) Item_field(thd, Lex->current_context(),
                                                   NullS, $1.str, $3.str);
              }
              else
              {
                $$= new (thd->mem_root) Item_ref(thd, Lex->current_context(),
                                                 NullS, $1.str, $3.str);
              }
              if ($$ == NULL)
                MYSQL_YYABORT;
            }
          }
        | '.' ident '.' ident
          {
            LEX *lex= thd->lex;
            SELECT_LEX *sel= lex->current_select;
            if (sel->no_table_names_allowed)
            {
              my_error(ER_TABLENAME_NOT_ALLOWED_HERE,
                       MYF(0), $2.str, thd->where);
            }
            if ((sel->parsing_place != IN_HAVING) ||
                (sel->get_in_sum_expr() > 0))
            {
              $$= new (thd->mem_root) Item_field(thd, Lex->current_context(),
                                                 NullS, $2.str, $4.str);

            }
            else
            {
              $$= new (thd->mem_root) Item_ref(thd, Lex->current_context(),
                                               NullS, $2.str, $4.str);
            }
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | ident '.' ident '.' ident
          {
            LEX *lex= thd->lex;
            SELECT_LEX *sel= lex->current_select;
            const char* schema= (thd->client_capabilities & CLIENT_NO_SCHEMA ?
                                 NullS : $1.str);
            if (sel->no_table_names_allowed)
            {
              my_error(ER_TABLENAME_NOT_ALLOWED_HERE,
                       MYF(0), $3.str, thd->where);
            }
            if ((sel->parsing_place != IN_HAVING) ||
                (sel->get_in_sum_expr() > 0))
            {
              $$= new (thd->mem_root) Item_field(thd, Lex->current_context(),
                                                 schema,
                                                 $3.str, $5.str);
            }
            else
            {
              $$= new (thd->mem_root) Item_ref(thd, Lex->current_context(),
                                               schema,
                                               $3.str, $5.str);
            }
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

field_ident:
          ident { $$=$1;}
        | ident '.' ident '.' ident
          {
            TABLE_LIST *table= Select->table_list.first;
            if (my_strcasecmp(table_alias_charset, $1.str, table->db))
              my_yyabort_error((ER_WRONG_DB_NAME, MYF(0), $1.str));
            if (my_strcasecmp(table_alias_charset, $3.str,
                              table->table_name))
              my_yyabort_error((ER_WRONG_TABLE_NAME, MYF(0), $3.str));
            $$=$5;
          }
        | ident '.' ident
          {
            TABLE_LIST *table= Select->table_list.first;
            if (my_strcasecmp(table_alias_charset, $1.str, table->alias))
              my_yyabort_error((ER_WRONG_TABLE_NAME, MYF(0), $1.str));
            $$=$3;
          }
        | '.' ident { $$=$2;} /* For Delphi */
        ;

table_ident:
          ident
          {
            $$= new (thd->mem_root) Table_ident($1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | ident '.' ident
          {
            $$= new (thd->mem_root) Table_ident(thd, $1, $3, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | '.' ident
          {
            /* For Delphi */
            $$= new (thd->mem_root) Table_ident($2);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

table_ident_opt_wild:
          ident opt_wild
          {
            $$= new (thd->mem_root) Table_ident($1);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | ident '.' ident opt_wild
          {
            $$= new (thd->mem_root) Table_ident(thd, $1, $3, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

table_ident_nodb:
          ident
          {
            LEX_STRING db={(char*) any_db,3};
            $$= new (thd->mem_root) Table_ident(thd, db, $1, 0);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

IDENT_sys:
          IDENT { $$= $1; }
        | IDENT_QUOTED
          {
            if (thd->charset_is_system_charset)
            {
              CHARSET_INFO *cs= system_charset_info;
              uint wlen= Well_formed_prefix(cs, $1.str, $1.length).length();
              if (wlen < $1.length)
              {
                ErrConvString err($1.str, $1.length, &my_charset_bin);
                my_error(ER_INVALID_CHARACTER_STRING, MYF(0),
                         cs->csname, err.ptr());
                MYSQL_YYABORT;
              }
              $$= $1;
            }
            else
            {
              if (thd->convert_with_error(system_charset_info, &$$,
                                          thd->charset(), $1.str, $1.length))
                MYSQL_YYABORT;
            }
          }
        ;

TEXT_STRING_sys:
          TEXT_STRING
          {
            if (thd->charset_is_system_charset)
              $$= $1;
            else
            {
              if (thd->convert_string(&$$, system_charset_info,
                                  $1.str, $1.length, thd->charset()))
                MYSQL_YYABORT;
            }
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
            if (thd->charset_is_character_set_filesystem)
              $$= $1;
            else
            {
              if (thd->convert_string(&$$,
                                      thd->variables.character_set_filesystem,
                                      $1.str, $1.length, thd->charset()))
                MYSQL_YYABORT;
            }
          }
        ;

ident_table_alias:
          IDENT_sys   { $$= $1; }
        | keyword_alias
          {
            $$.str= thd->strmake($1.str, $1.length);
            if ($$.str == NULL)
              MYSQL_YYABORT;
            $$.length= $1.length;
          }
        ;

ident:
          IDENT_sys    { $$=$1; }
        | keyword
          {
            $$.str= thd->strmake($1.str, $1.length);
            if ($$.str == NULL)
              MYSQL_YYABORT;
            $$.length= $1.length;
          }
        ;

label_ident:
          IDENT_sys    { $$=$1; }
        | keyword_sp
          {
            $$.str= thd->strmake($1.str, $1.length);
            if ($$.str == NULL)
              MYSQL_YYABORT;
            $$.length= $1.length;
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
            if (!($$=(LEX_USER*) thd->alloc(sizeof(st_lex_user))))
              MYSQL_YYABORT;
            $$->user = $1;
            $$->host= null_lex_str; // User or Role, see get_current_user()
            $$->reset_auth();

            if (check_string_char_length(&$$->user, ER_USERNAME,
                                         username_char_length,
                                         system_charset_info, 0))
              MYSQL_YYABORT;
          }
        | ident_or_text '@' ident_or_text
          {
            if (!($$=(LEX_USER*) thd->alloc(sizeof(st_lex_user))))
              MYSQL_YYABORT;
            $$->user = $1; $$->host=$3;
            $$->reset_auth();

            if (check_string_char_length(&$$->user, ER_USERNAME,
                                         username_char_length,
                                         system_charset_info, 0) ||
                check_host_name(&$$->host))
              MYSQL_YYABORT;
            if ($$->host.str[0])
            {
              /*
                Convert hostname part of username to lowercase.
                It's OK to use in-place lowercase as long as
                the character set is utf8.
              */
              my_casedn_str(system_charset_info, $$->host.str);
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
            if (!($$=(LEX_USER*)thd->calloc(sizeof(LEX_USER))))
              MYSQL_YYABORT;
            $$->user= current_user;
            $$->plugin= empty_lex_str;
            $$->auth= empty_lex_str;
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
keyword_alias:
          keyword_sp            {}
        | ASCII_SYM             {}
        | BACKUP_SYM            {}
        | BEGIN_SYM             {}
        | BINLOG_SYM            {}
        | BYTE_SYM              {}
        | CACHE_SYM             {}
        | CHARSET               {}
        | CHECKSUM_SYM          {}
        | CHECKPOINT_SYM        {}
        | CLOSE_SYM             {}
        | COLUMN_ADD_SYM        {}
        | COLUMN_CHECK_SYM      {}
        | COLUMN_CREATE_SYM     {}
        | COLUMN_DELETE_SYM     {}
        | COLUMN_GET_SYM        {}
        | COMMENT_SYM           {}
        | COMMIT_SYM            {}
        | CONTAINS_SYM          {}
        | DEALLOCATE_SYM        {}
        | DO_SYM                {}
        | END                   {}
        | EXAMINED_SYM          {}
        | EXCLUDE_SYM           {}
        | EXECUTE_SYM           {}
        | FLUSH_SYM             {}
        | FOLLOWS_SYM           {}
        | FOLLOWING_SYM         {}
        | FORMAT_SYM            {}
        | GET_SYM               {}
        | HANDLER_SYM           {}
        | HELP_SYM              {}
        | HOST_SYM              {}
        | INSTALL_SYM           {}
        | LANGUAGE_SYM          {}
        | NO_SYM                {}
        | OPEN_SYM              {}
        | OPTION                {}
        | OPTIONS_SYM           {}
        | OTHERS_SYM            {}
        | OWNER_SYM             {}
        | PARSER_SYM            {}
        | PORT_SYM              {}
        | PRECEDES_SYM          {}
        | PRECEDING_SYM         {}
        | PREPARE_SYM           {}
        | REMOVE_SYM            {}
        | REPAIR                {}
        | RESET_SYM             {}
        | RESTORE_SYM           {}
        | ROLLBACK_SYM          {}
        | SAVEPOINT_SYM         {}
        | SECURITY_SYM          {}
        | SERVER_SYM            {}
        | SHUTDOWN              {}
        | SIGNED_SYM            {}
        | SOCKET_SYM            {}
        | SLAVE                 {}
        | SLAVES                {}
        | SONAME_SYM            {}
        | START_SYM             {}
        | STOP_SYM              {}
        | STORED_SYM            {}
        | TIES_SYM              {}
        | TRUNCATE_SYM          {}
        | UNICODE_SYM           {}
        | UNINSTALL_SYM         {}
        | UNBOUNDED_SYM         {}
        | WRAPPER_SYM           {}
        | XA_SYM                {}
        | UPGRADE_SYM           {}
        ;


/* Keyword that we allow for identifiers (except SP labels) */
keyword: keyword_alias | WINDOW_SYM {};

/*
 * Keywords that we allow for labels in SPs.
 * Anything that's the beginning of a statement or characteristics
 * must be in keyword above, otherwise we get (harmful) shift/reduce
 * conflicts.
 */
keyword_sp:
          ACTION                   {}
        | ADDDATE_SYM              {}
        | ADMIN_SYM                {}
        | AFTER_SYM                {}
        | AGAINST                  {}
        | AGGREGATE_SYM            {}
        | ALGORITHM_SYM            {}
        | ALWAYS_SYM               {}
        | ANY_SYM                  {}
        | AT_SYM                   {}
        | ATOMIC_SYM               {}
        | AUTHORS_SYM              {}
        | AUTO_INC                 {}
        | AUTOEXTEND_SIZE_SYM      {}
        | AUTO_SYM                 {}
        | AVG_ROW_LENGTH           {}
        | AVG_SYM                  {}
        | BIT_SYM                  {}
        | BLOCK_SYM                {}
        | BOOL_SYM                 {}
        | BOOLEAN_SYM              {}
        | BTREE_SYM                {}
        | CASCADED                 {}
        | CATALOG_NAME_SYM         {}
        | CHAIN_SYM                {}
        | CHANGED                  {}
        | CIPHER_SYM               {}
        | CLIENT_SYM               {}
        | CLASS_ORIGIN_SYM         {}
        | COALESCE                 {}
        | CODE_SYM                 {}
        | COLLATION_SYM            {}
        | COLUMN_NAME_SYM          {}
        | COLUMNS                  {}
        | COMMITTED_SYM            {}
        | COMPACT_SYM              {}
        | COMPLETION_SYM           {}
        | COMPRESSED_SYM           {}
        | CONCURRENT               {}
        | CONNECTION_SYM           {}
        | CONSISTENT_SYM           {}
        | CONSTRAINT_CATALOG_SYM   {}
        | CONSTRAINT_SCHEMA_SYM    {}
        | CONSTRAINT_NAME_SYM      {}
        | CONTEXT_SYM              {}
        | CONTRIBUTORS_SYM         {}
        | CURRENT_POS_SYM          {}
        | CPU_SYM                  {}
        | CUBE_SYM                 {}
        /*
          Although a reserved keyword in SQL:2003 (and :2008),
          not reserved in MySQL per WL#2111 specification.
        */
        | CURRENT_SYM              {}
        | CURSOR_NAME_SYM          {}
        | DATA_SYM                 {}
        | DATAFILE_SYM             {}
        | DATETIME                 {}
        | DATE_SYM                 {}
        | DAY_SYM                  {}
        | DEFINER_SYM              {}
        | DELAY_KEY_WRITE_SYM      {}
        | DES_KEY_FILE             {}
        | DIAGNOSTICS_SYM          {}
        | DIRECTORY_SYM            {}
        | DISABLE_SYM              {}
        | DISCARD                  {}
        | DISK_SYM                 {}
        | DUMPFILE                 {}
        | DUPLICATE_SYM            {}
        | DYNAMIC_SYM              {}
        | ENDS_SYM                 {}
        | ENUM                     {}
        | ENGINE_SYM               {}
        | ENGINES_SYM              {}
        | ERROR_SYM                {}
        | ERRORS                   {}
        | ESCAPE_SYM               {}
        | EVENT_SYM                {}
        | EVENTS_SYM               {}
        | EVERY_SYM                {}
        | EXCHANGE_SYM             {}
        | EXPANSION_SYM            {}
        | EXPORT_SYM               {}
        | EXTENDED_SYM             {}
        | EXTENT_SIZE_SYM          {}
        | FAULTS_SYM               {}
        | FAST_SYM                 {}
        | FOUND_SYM                {}
        | ENABLE_SYM               {}
        | FULL                     {}
        | FILE_SYM                 {}
        | FIRST_SYM                {}
        | FIXED_SYM                {}
        | GENERAL                  {}
        | GENERATED_SYM            {}
        | GEOMETRY_SYM             {}
        | GEOMETRYCOLLECTION       {}
        | GET_FORMAT               {}
        | GRANTS                   {}
        | GLOBAL_SYM               {}
        | HASH_SYM                 {}
        | HARD_SYM                 {}
        | HOSTS_SYM                {}
        | HOUR_SYM                 {}
        | ID_SYM                   {}
        | IDENTIFIED_SYM           {}
        | IGNORE_SERVER_IDS_SYM    {}
        | IMMEDIATE_SYM            {} /* SQL-2003-R */
        | INVOKER_SYM              {}
        | IMPORT                   {}
        | INDEXES                  {}
        | INITIAL_SIZE_SYM         {}
        | IO_SYM                   {}
        | IPC_SYM                  {}
        | ISOLATION                {}
        | ISSUER_SYM               {}
        | JSON_SYM                 {}
        | INSERT_METHOD            {}
        | KEY_BLOCK_SIZE           {}
        | LAST_VALUE               {}
        | LAST_SYM                 {}
        | LEAVES                   {}
        | LESS_SYM                 {}
        | LEVEL_SYM                {}
        | LINESTRING               {}
        | LIST_SYM                 {}
        | LOCAL_SYM                {}
        | LOCKS_SYM                {}
        | LOGFILE_SYM              {}
        | LOGS_SYM                 {}
        | MAX_ROWS                 {}
        | MASTER_SYM               {}
        | MASTER_HEARTBEAT_PERIOD_SYM {}
        | MASTER_GTID_POS_SYM      {}
        | MASTER_HOST_SYM          {}
        | MASTER_PORT_SYM          {}
        | MASTER_LOG_FILE_SYM      {}
        | MASTER_LOG_POS_SYM       {}
        | MASTER_USER_SYM          {}
        | MASTER_USE_GTID_SYM      {}
        | MASTER_PASSWORD_SYM      {}
        | MASTER_SERVER_ID_SYM     {}
        | MASTER_CONNECT_RETRY_SYM {}
        | MASTER_DELAY_SYM         {}
        | MASTER_SSL_SYM           {}
        | MASTER_SSL_CA_SYM        {}
        | MASTER_SSL_CAPATH_SYM    {}
        | MASTER_SSL_CERT_SYM      {}
        | MASTER_SSL_CIPHER_SYM    {}
        | MASTER_SSL_CRL_SYM       {}
        | MASTER_SSL_CRLPATH_SYM   {}
        | MASTER_SSL_KEY_SYM       {}
        | MAX_CONNECTIONS_PER_HOUR {}
        | MAX_QUERIES_PER_HOUR     {}
        | MAX_SIZE_SYM             {}
        | MAX_STATEMENT_TIME_SYM   {}
        | MAX_UPDATES_PER_HOUR     {}
        | MAX_USER_CONNECTIONS_SYM {}
        | MEDIUM_SYM               {}
        | MEMORY_SYM               {}
        | MERGE_SYM                {}
        | MESSAGE_TEXT_SYM         {}
        | MICROSECOND_SYM          {}
        | MIGRATE_SYM              {}
        | MINUTE_SYM               {}
        | MIN_ROWS                 {}
        | MODIFY_SYM               {}
        | MODE_SYM                 {}
        | MONTH_SYM                {}
        | MULTILINESTRING          {}
        | MULTIPOINT               {}
        | MULTIPOLYGON             {}
        | MUTEX_SYM                {}
        | MYSQL_SYM                {}
        | MYSQL_ERRNO_SYM          {}
        | NAME_SYM                 {}
        | NAMES_SYM                {}
        | NATIONAL_SYM             {}
        | NCHAR_SYM                {}
        | NEXT_SYM                 {}
        | NEW_SYM                  {}
        | NO_WAIT_SYM              {}
        | NODEGROUP_SYM            {}
        | NONE_SYM                 {}
        | NUMBER_SYM               {}
        | NVARCHAR_SYM             {}
        | OFFSET_SYM               {}
        | OLD_PASSWORD_SYM         {}
        | ONE_SYM                  {}
        | ONLINE_SYM               {}
        | ONLY_SYM                 {}
        | PACK_KEYS_SYM            {}
        | PAGE_SYM                 {}
        | PARTIAL                  {}
        | PARTITIONING_SYM         {}
        | PARTITIONS_SYM           {}
        | PASSWORD_SYM             {}
        | PERSISTENT_SYM           {}
        | PHASE_SYM                {}
        | PLUGIN_SYM               {}
        | PLUGINS_SYM              {}
        | POINT_SYM                {}
        | POLYGON                  {}
        | PRESERVE_SYM             {}
        | PREV_SYM                 {}
        | PRIVILEGES               {}
        | PROCESS                  {}
        | PROCESSLIST_SYM          {}
        | PROFILE_SYM              {}
        | PROFILES_SYM             {}
        | PROXY_SYM                {}
        | QUARTER_SYM              {}
        | QUERY_SYM                {}
        | QUICK                    {}
        | READ_ONLY_SYM            {}
        | REBUILD_SYM              {}
        | RECOVER_SYM              {}
        | REDO_BUFFER_SIZE_SYM     {}
        | REDOFILE_SYM             {}
        | REDUNDANT_SYM            {}
        | RELAY                    {}
        | RELAYLOG_SYM             {}
        | RELAY_LOG_FILE_SYM       {}
        | RELAY_LOG_POS_SYM        {}
        | RELAY_THREAD             {}
        | RELOAD                   {}
        | REORGANIZE_SYM           {}
        | REPEATABLE_SYM           {}
        | REPLICATION              {}
        | RESOURCES                {}
        | RESUME_SYM               {}
        | RETURNED_SQLSTATE_SYM    {}
        | RETURNS_SYM              {}
        | REVERSE_SYM              {}
        | ROLE_SYM                 {}
        | ROLLUP_SYM               {}
        | ROUTINE_SYM              {}
        | ROW_COUNT_SYM            {}
        | ROW_FORMAT_SYM           {}
        | ROW_SYM                  {}
        | RTREE_SYM                {}
        | SCHEDULE_SYM             {}
        | SCHEMA_NAME_SYM          {}
        | SECOND_SYM               {}
        | SERIAL_SYM               {}
        | SERIALIZABLE_SYM         {}
        | SESSION_SYM              {}
        | SIMPLE_SYM               {}
        | SHARE_SYM                {}
        | SLAVE_POS_SYM            {}
        | SLOW                     {}
        | SNAPSHOT_SYM             {}
        | SOFT_SYM                 {}
        | SOUNDS_SYM               {}
        | SOURCE_SYM               {}
        | SQL_CACHE_SYM            {}
        | SQL_BUFFER_RESULT        {}
        | SQL_NO_CACHE_SYM         {}
        | SQL_THREAD               {}
        | STARTS_SYM               {}
        | STATEMENT_SYM            {}
        | STATUS_SYM               {}
        | STORAGE_SYM              {}
        | STRING_SYM               {}
        | SUBCLASS_ORIGIN_SYM      {}
        | SUBDATE_SYM              {}
        | SUBJECT_SYM              {}
        | SUBPARTITION_SYM         {}
        | SUBPARTITIONS_SYM        {}
        | SUPER_SYM                {}
        | SUSPEND_SYM              {}
        | SWAPS_SYM                {}
        | SWITCHES_SYM             {}
        | TABLE_NAME_SYM           {}
        | TABLES                   {}
        | TABLE_CHECKSUM_SYM       {}
        | TABLESPACE               {}
        | TEMPORARY                {}
        | TEMPTABLE_SYM            {}
        | TEXT_SYM                 {}
        | THAN_SYM                 {}
        | TRANSACTION_SYM          {}
        | TRANSACTIONAL_SYM        {}
        | TRIGGERS_SYM             {}
        | TIMESTAMP                {}
        | TIMESTAMP_ADD            {}
        | TIMESTAMP_DIFF           {}
        | TIME_SYM                 {}
        | TYPES_SYM                {}
        | TYPE_SYM                 {}
        | UDF_RETURNS_SYM          {}
        | FUNCTION_SYM             {}
        | UNCOMMITTED_SYM          {}
        | UNDEFINED_SYM            {}
        | UNDO_BUFFER_SIZE_SYM     {}
        | UNDOFILE_SYM             {}
        | UNKNOWN_SYM              {}
        | UNTIL_SYM                {}
        | USER_SYM                 {}
        | USE_FRM                  {}
        | VARIABLES                {}
        | VIEW_SYM                 {}
        | VIRTUAL_SYM              {}
        | VALUE_SYM                {}
        | WARNINGS                 {}
        | WAIT_SYM                 {}
        | WEEK_SYM                 {}
        | WEIGHT_STRING_SYM        {}
        | WORK_SYM                 {}
        | X509_SYM                 {}
        | XML_SYM                  {}
        | YEAR_SYM                 {}
        | VIA_SYM               {}
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
            lex->sql_command= SQLCOM_SET_OPTION;
            mysql_init_select(lex);
            lex->option_type=OPT_SESSION;
            lex->var_list.empty();
            lex->autocommit= 0;
            sp_create_assignment_lex(thd, yychar == YYEMPTY);
          }
          start_option_value_list
          {
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;
          }
        | SET STATEMENT_SYM
          {
            LEX *lex= Lex;
            mysql_init_select(lex);
            lex->option_type= OPT_SESSION;
            lex->sql_command= SQLCOM_SET_OPTION;
            lex->autocommit= 0;
          }
          set_stmt_option_value_following_option_type_list
          {
            LEX *lex= Lex;
            if (lex->table_or_sp_used())
              my_yyabort_error((ER_SUBQUERIES_NOT_SUPPORTED, MYF(0), "SET STATEMENT"));
            lex->stmt_var_list= lex->var_list;
            lex->var_list.empty();
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
            if (sp_create_assignment_instr(thd, yychar == YYEMPTY))
              MYSQL_YYABORT;
          }
          option_value_list_continued
        | TRANSACTION_SYM
          {
            Lex->option_type= OPT_DEFAULT;
          }
          transaction_characteristics
          {
            if (sp_create_assignment_instr(thd, yychar == YYEMPTY))
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
            if (sp_create_assignment_instr(thd, yychar == YYEMPTY))
              MYSQL_YYABORT; 
          }
          option_value_list_continued
        | TRANSACTION_SYM transaction_characteristics
          {
            if (sp_create_assignment_instr(thd, yychar == YYEMPTY))
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
            if (sp_create_assignment_instr(thd, yychar == YYEMPTY))
              MYSQL_YYABORT; 
          }
        | option_value_list ','
          {
            sp_create_assignment_lex(thd, yychar == YYEMPTY);
          }
          option_value
          {
            if (sp_create_assignment_instr(thd, yychar == YYEMPTY))
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
          internal_variable_name equal set_expr_or_default
          {
            LEX *lex= Lex;

            if ($1.var && $1.var != trg_new_row_fake_var)
            {
              /* It is a system variable. */
              if (set_system_variable(thd, &$1, lex->option_type, $3))
                MYSQL_YYABORT;
            }
            else
            {
              /*
                Not in trigger assigning value to new row,
                and option_type preceding local variable is illegal.
              */
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
          }
        ;

/* Option values without preceding option_type. */
option_value_no_option_type:
          internal_variable_name equal set_expr_or_default
          {
            LEX *lex= Lex;

            if ($1.var == trg_new_row_fake_var)
            {
              /* We are in trigger and assigning value to field of new row */
              if (set_trigger_new_row(thd, &$1.base_name, $3))
                MYSQL_YYABORT;
            }
            else if ($1.var)
            {
              /* It is a system variable. */
              if (set_system_variable(thd, &$1, lex->option_type, $3))
                MYSQL_YYABORT;
            }
            else
            {
              sp_pcontext *spc= lex->spcont;
              sp_variable *spv= spc->find_variable($1.base_name, false);

              /* It is a local variable. */
              if (set_local_variable(thd, spv, $3))
                MYSQL_YYABORT;
            }
          }
        | '@' ident_or_text equal expr
          {
            Item_func_set_user_var *item;
            item= new (thd->mem_root) Item_func_set_user_var(thd, $2, $4);
            if (item == NULL)
              MYSQL_YYABORT;
            set_var_user *var= new (thd->mem_root) set_var_user(item);
            if (var == NULL)
              MYSQL_YYABORT;
            Lex->var_list.push_back(var, thd->mem_root);
          }
        | '@' '@' opt_var_ident_type internal_variable_name equal set_expr_or_default
          {
            struct sys_var_with_base tmp= $4;
            if (tmp.var == trg_new_row_fake_var)
            {
              my_error(ER_UNKNOWN_SYSTEM_VARIABLE, MYF(0), 3, "NEW");
              MYSQL_YYABORT;
            }
            /* Lookup if necessary: must be a system variable. */
            if (tmp.var == NULL)
            {
              if (find_sys_var_null_base(thd, &tmp))
                MYSQL_YYABORT;
            }
            if (set_system_variable(thd, &tmp, $3, $6))
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
            if (var == NULL)
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
          }
        | NAMES_SYM equal expr
          {
            LEX *lex= Lex;
            sp_pcontext *spc= lex->spcont;
            LEX_STRING names;

            names.str= (char *)"names";
            names.length= 5;
            if (spc && spc->find_variable(names, false))
              my_error(ER_SP_BAD_VAR_SHADOW, MYF(0), names.str);
            else
              my_parse_error(thd, ER_SYNTAX_ERROR);

            MYSQL_YYABORT;
          }
        | NAMES_SYM charset_name_or_default opt_collate
          {
            LEX *lex= Lex;
            CHARSET_INFO *cs2;
            CHARSET_INFO *cs3;
            cs2= $2 ? $2 : global_system_variables.character_set_client;
            cs3= $3 ? $3 : cs2;
            if (!my_charset_same(cs2, cs3))
            {
              my_error(ER_COLLATION_CHARSET_MISMATCH, MYF(0),
                       cs3->name, cs2->csname);
              MYSQL_YYABORT;
            }
            set_var_collation_client *var;
            var= new (thd->mem_root) set_var_collation_client(cs3, cs3, cs3);
            if (var == NULL)
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
          }
        | DEFAULT ROLE_SYM grant_role
          {
            LEX *lex = Lex;
            LEX_USER *user;
            if (!(user=(LEX_USER *) thd->calloc(sizeof(LEX_USER))))
              MYSQL_YYABORT;
            user->user= current_user;
            set_var_default_role *var= (new (thd->mem_root)
                                        set_var_default_role(user,
                                                             $3->user));
            if (var == NULL)
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
            thd->lex->autocommit= TRUE;
            if (lex->sphead)
              lex->sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;
          }
        | DEFAULT ROLE_SYM grant_role FOR_SYM user
          {
            LEX *lex = Lex;
            set_var_default_role *var= (new (thd->mem_root)
                                        set_var_default_role($5, $3->user));
            if (var == NULL)
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
            thd->lex->autocommit= TRUE;
            if (lex->sphead)
              lex->sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;
          }
        | ROLE_SYM ident_or_text
          {
            LEX *lex = Lex;
            set_var_role *var= new (thd->mem_root) set_var_role($2);
            if (var == NULL)
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
          }
        | PASSWORD_SYM opt_for_user text_or_password
          {
            LEX *lex = Lex;
            set_var_password *var= (new (thd->mem_root)
                                    set_var_password(lex->definer));
            if (var == NULL)
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
            lex->autocommit= TRUE;
            if (lex->sphead)
              lex->sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;
          }
        ;


internal_variable_name:
          ident
          {
            sp_pcontext *spc= thd->lex->spcont;
            sp_variable *spv;

            /* Best effort lookup for system variable. */
            if (!spc || !(spv = spc->find_variable($1, false)))
            {
              struct sys_var_with_base tmp= {NULL, $1};

              /* Not an SP local variable */
              if (find_sys_var_null_base(thd, &tmp))
                MYSQL_YYABORT;

              $$= tmp;
            }
            else
            {
              /*
                Possibly an SP local variable (or a shadowed sysvar).
                Will depend on the context of the SET statement.
              */
              $$.var= NULL;
              $$.base_name= $1;
            }
          }
        | ident '.' ident
          {
            LEX *lex= Lex;
            if (check_reserved_words(&$1))
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            if (lex->sphead && lex->sphead->m_type == TYPE_ENUM_TRIGGER &&
                (!my_strcasecmp(system_charset_info, $1.str, "NEW") || 
                 !my_strcasecmp(system_charset_info, $1.str, "OLD")))
            {
              if ($1.str[0]=='O' || $1.str[0]=='o')
                my_yyabort_error((ER_TRG_CANT_CHANGE_ROW, MYF(0), "OLD", ""));
              if (lex->trg_chistics.event == TRG_EVENT_DELETE)
              {
                my_error(ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0),
                         "NEW", "on DELETE");
                MYSQL_YYABORT;
              }
              if (lex->trg_chistics.action_time == TRG_ACTION_AFTER)
                my_yyabort_error((ER_TRG_CANT_CHANGE_ROW, MYF(0), "NEW", "after "));
              /* This special combination will denote field of NEW row */
              $$.var= trg_new_row_fake_var;
              $$.base_name= $3;
            }
            else
            {
              sys_var *tmp=find_sys_var(thd, $3.str, $3.length);
              if (!tmp)
                MYSQL_YYABORT;
              if (!tmp->is_struct())
                my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), $3.str);
              $$.var= tmp;
              $$.base_name= $1;
            }
          }
        | DEFAULT '.' ident
          {
            sys_var *tmp=find_sys_var(thd, $3.str, $3.length);
            if (!tmp)
              MYSQL_YYABORT;
            if (!tmp->is_struct())
              my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), $3.str);
            $$.var= tmp;
            $$.base_name.str=    (char*) "default";
            $$.base_name.length= 7;
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
            if (item == NULL)
              MYSQL_YYABORT;
            set_var *var= (new (thd->mem_root)
                           set_var(thd, lex->option_type,
                                   find_sys_var(thd, "tx_read_only"),
                                   &null_lex_str,
                                   item));
            if (var == NULL)
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
          }
        ;

isolation_level:
          ISOLATION LEVEL_SYM isolation_types
          {
            LEX *lex=Lex;
            Item *item= new (thd->mem_root) Item_int(thd, (int32) $3);
            if (item == NULL)
              MYSQL_YYABORT;
            set_var *var= (new (thd->mem_root)
                           set_var(thd, lex->option_type,
                                   find_sys_var(thd, "tx_isolation"),
                                   &null_lex_str,
                                   item));
            if (var == NULL)
              MYSQL_YYABORT;
            lex->var_list.push_back(var, thd->mem_root);
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
            LEX_STRING pw= { C_STRING_WITH_LEN("password") };

            if (spc && spc->find_variable(pw, false))
              my_yyabort_error((ER_SP_BAD_VAR_SHADOW, MYF(0), pw.str));
            if (!(lex->definer= (LEX_USER*) thd->calloc(sizeof(LEX_USER))))
              MYSQL_YYABORT;
            lex->definer->user= current_user;
            lex->definer->plugin= empty_lex_str;
            lex->definer->auth= empty_lex_str;
          }
        | FOR_SYM user equal { Lex->definer= $2; }
        ;

text_or_password:
          TEXT_STRING { Lex->definer->pwhash= $1;}
        | PASSWORD_SYM '(' TEXT_STRING ')' { Lex->definer->pwtext= $3; }
        | OLD_PASSWORD_SYM '(' TEXT_STRING ')'
          {
            Lex->definer->pwtext= $3;
            Lex->definer->pwhash.str= Item_func_password::alloc(thd,
                                   $3.str, $3.length, Item_func_password::OLD);
            Lex->definer->pwhash.length=  SCRAMBLED_PASSWORD_CHAR_LENGTH_323;
          }
        ;

set_expr_or_default:
          expr { $$=$1; }
        | DEFAULT { $$=0; }
        | ON
          {
            $$=new (thd->mem_root) Item_string_sys(thd, "ON",  2);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | ALL
          {
            $$=new (thd->mem_root) Item_string_sys(thd, "ALL", 3);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        | BINARY
          {
            $$=new (thd->mem_root) Item_string_sys(thd, "binary", 6);
            if ($$ == NULL)
              MYSQL_YYABORT;
          }
        ;

/* Lock function */

lock:
          LOCK_SYM table_or_tables
          {
            LEX *lex= Lex;

            if (lex->sphead)
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "LOCK"));
            lex->sql_command= SQLCOM_LOCK_TABLES;
          }
          table_lock_list
          {}
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
          table_ident opt_table_alias lock_option
          {
            thr_lock_type lock_type= (thr_lock_type) $3;
            bool lock_for_write= lock_type >= TL_WRITE_ALLOW_WRITE;
            ulong table_options= lock_for_write ? TL_OPTION_UPDATING : 0;
            enum_mdl_type mdl_type= !lock_for_write
                                    ? MDL_SHARED_READ
                                    : lock_type == TL_WRITE_CONCURRENT_INSERT
                                      ? MDL_SHARED_WRITE
                                      : MDL_SHARED_NO_READ_WRITE;

            if (!Select->add_table_to_list(thd, $1, $2, table_options,
                                           lock_type, mdl_type))
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

            if (lex->sphead)
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
          HANDLER_SYM table_ident OPEN_SYM opt_table_alias
          {
            LEX *lex= Lex;
            if (lex->sphead)
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "HANDLER"));
            lex->sql_command = SQLCOM_HA_OPEN;
            if (!lex->current_select->add_table_to_list(thd, $2, $4, 0))
              MYSQL_YYABORT;
          }
        | HANDLER_SYM table_ident_nodb CLOSE_SYM
          {
            LEX *lex= Lex;
            if (lex->sphead)
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "HANDLER"));
            lex->sql_command = SQLCOM_HA_CLOSE;
            if (!lex->current_select->add_table_to_list(thd, $2, 0, 0))
              MYSQL_YYABORT;
          }
        | HANDLER_SYM table_ident_nodb READ_SYM
          {
            LEX *lex=Lex;
            if (lex->sphead)
              my_yyabort_error((ER_SP_BADSTATEMENT, MYF(0), "HANDLER"));
            lex->expr_allows_subselect= FALSE;
            lex->sql_command = SQLCOM_HA_READ;
            lex->ha_rkey_mode= HA_READ_KEY_EXACT; /* Avoid purify warnings */
            Item *one= new (thd->mem_root) Item_int(thd, (int32) 1);
            if (one == NULL)
              MYSQL_YYABORT;
            lex->current_select->select_limit= one;
            lex->current_select->offset_limit= 0;
            lex->limit_rows_examined= 0;
            if (!lex->current_select->add_table_to_list(thd, $2, 0, 0))
              MYSQL_YYABORT;
          }
          handler_read_or_scan opt_where_clause opt_limit_clause
          {
            Lex->expr_allows_subselect= TRUE;
            /* Stored functions are not supported for HANDLER READ. */
            if (Lex->uses_stored_routines())
            {
              my_error(ER_NOT_SUPPORTED_YET, MYF(0),
                       "stored functions in HANDLER ... READ");
              MYSQL_YYABORT;
            }
          }
        ;

handler_read_or_scan:
          handler_scan_function       { Lex->ident= null_lex_str; }
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
            if (!(lex->insert_list= new (thd->mem_root) List_item))
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
            LEX *lex= Lex;
            if (lex->columns.elements)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            lex->sql_command= SQLCOM_REVOKE;
            lex->type= TYPE_ENUM_FUNCTION;
          }
        | grant_privileges ON PROCEDURE_SYM grant_ident FROM user_and_role_list
          {
            LEX *lex= Lex;
            if (lex->columns.elements)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            lex->sql_command= SQLCOM_REVOKE;
            lex->type= TYPE_ENUM_PROCEDURE;
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
            if (Lex->users_list.push_front($1, thd->mem_root))
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
            LEX *lex= Lex;
            if (lex->columns.elements)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            lex->sql_command= SQLCOM_GRANT;
            lex->type= TYPE_ENUM_FUNCTION;
          }
        | grant_privileges ON PROCEDURE_SYM grant_ident TO_SYM grant_list
          opt_require_clause opt_grant_options
          {
            LEX *lex= Lex;
            if (lex->columns.elements)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            lex->sql_command= SQLCOM_GRANT;
            lex->type= TYPE_ENUM_PROCEDURE;
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
            if (Lex->users_list.push_front($1, thd->mem_root))
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
            if (Lex->users_list.push_back($1, thd->mem_root))
              MYSQL_YYABORT;
          }
        | role_list ',' grant_role
          {
            if (Lex->users_list.push_back($3, thd->mem_root))
              MYSQL_YYABORT;
          }
        ;

current_role:
          CURRENT_ROLE optional_braces
          {
            if (!($$=(LEX_USER*) thd->calloc(sizeof(LEX_USER))))
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
            $1.str[$1.length] = '\0';
            if ($1.length == 0)
              my_yyabort_error((ER_INVALID_ROLE, MYF(0), ""));
            if (!($$=(LEX_USER*) thd->alloc(sizeof(st_lex_user))))
              MYSQL_YYABORT;
            $$->user = $1;
            $$->host= empty_lex_str;
            $$->reset_auth();

            if (check_string_char_length(&$$->user, ER_USERNAME,
                                         username_char_length,
                                         cs, 0))
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
            if (lex->x509_subject)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "SUBJECT"));
            lex->x509_subject=$2.str;
          }
        | ISSUER_SYM TEXT_STRING
          {
            LEX *lex=Lex;
            if (lex->x509_issuer)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "ISSUER"));
            lex->x509_issuer=$2.str;
          }
        | CIPHER_SYM TEXT_STRING
          {
            LEX *lex=Lex;
            if (lex->ssl_cipher)
              my_yyabort_error((ER_DUP_ARGUMENT, MYF(0), "CIPHER"));
            lex->ssl_cipher=$2.str;
          }
        ;

grant_ident:
          '*'
          {
            LEX *lex= Lex;
            size_t dummy;
            if (lex->copy_db_to(&lex->current_select->db, &dummy))
              MYSQL_YYABORT;
            if (lex->grant == GLOBAL_ACLS)
              lex->grant = DB_ACLS & ~GRANT_ACL;
            else if (lex->columns.elements)
              my_yyabort_error((ER_ILLEGAL_GRANT_FOR_TABLE, MYF(0)));
          }
        | ident '.' '*'
          {
            LEX *lex= Lex;
            lex->current_select->db = $1.str;
            if (lex->grant == GLOBAL_ACLS)
              lex->grant = DB_ACLS & ~GRANT_ACL;
            else if (lex->columns.elements)
              my_yyabort_error((ER_ILLEGAL_GRANT_FOR_TABLE, MYF(0)));
          }
        | '*' '.' '*'
          {
            LEX *lex= Lex;
            lex->current_select->db = NULL;
            if (lex->grant == GLOBAL_ACLS)
              lex->grant= GLOBAL_ACLS & ~GRANT_ACL;
            else if (lex->columns.elements)
              my_yyabort_error((ER_ILLEGAL_GRANT_FOR_TABLE, MYF(0)));
          }
        | table_ident
          {
            LEX *lex=Lex;
            if (!lex->current_select->add_table_to_list(thd, $1,NULL,
                                                        TL_OPTION_UPDATING))
              MYSQL_YYABORT;
            if (lex->grant == GLOBAL_ACLS)
              lex->grant =  TABLE_ACLS & ~GRANT_ACL;
          }
        ;

user_list:
          user
          {
            if (Lex->users_list.push_back($1, thd->mem_root))
              MYSQL_YYABORT;
          }
        | user_list ',' user
          {
            if (Lex->users_list.push_back($3, thd->mem_root))
              MYSQL_YYABORT;
          }
        ;

grant_list:
          grant_user
          {
            if (Lex->users_list.push_back($1, thd->mem_root))
              MYSQL_YYABORT;
          }
        | grant_list ',' grant_user
          {
            if (Lex->users_list.push_back($3, thd->mem_root))
              MYSQL_YYABORT;
          }
        ;

user_and_role_list:
          user_or_role
          {
            if (Lex->users_list.push_back($1, thd->mem_root))
              MYSQL_YYABORT;
          }
        | user_and_role_list ',' user_or_role
          {
            if (Lex->users_list.push_back($3, thd->mem_root))
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
            if (Lex->sql_command == SQLCOM_REVOKE)
              MYSQL_YYABORT;
          }
        | user IDENTIFIED_SYM BY PASSWORD_SYM TEXT_STRING
          { 
            $$= $1; 
            $1->pwhash= $5;
          }
        | user IDENTIFIED_SYM via_or_with ident_or_text
          {
            $$= $1;
            $1->plugin= $4;
            $1->auth= empty_lex_str;
          }
        | user IDENTIFIED_SYM via_or_with ident_or_text using_or_as TEXT_STRING_sys
          {
            $$= $1;
            $1->plugin= $4;
            $1->auth= $6;
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
            if (new_str == NULL)
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
              if (col == NULL)
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

begin:
          BEGIN_SYM
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

opt_savepoint:
          /* empty */ {}
        | SAVEPOINT_SYM {}
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
        | ROLLBACK_SYM opt_work
          TO_SYM opt_savepoint ident
          {
            LEX *lex=Lex;
            lex->sql_command= SQLCOM_ROLLBACK_TO_SAVEPOINT;
            lex->ident= $5;
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


union_clause:
          /* empty */ {}
        | union_list
        ;

union_list:
          UNION_SYM union_option
          {
            if (add_select_to_union_list(Lex, (bool)$2, TRUE))
              MYSQL_YYABORT;
          }
          union_list_part2
          {
            /*
              Remove from the name resolution context stack the context of the
              last select in the union.
            */
            Lex->pop_context();
          }
        ;

union_list_view:
          UNION_SYM union_option
          {
            if (add_select_to_union_list(Lex, (bool)$2, TRUE))
              MYSQL_YYABORT;
          }
          query_expression_body_view
          {
            Lex->pop_context();
          }
        ;

union_order_or_limit:
          {
            LEX *lex= thd->lex;
            DBUG_ASSERT(lex->current_select->linkage != GLOBAL_OPTIONS_TYPE);
            SELECT_LEX *sel= lex->current_select;
            SELECT_LEX_UNIT *unit= sel->master_unit();
            SELECT_LEX *fake= unit->fake_select_lex;
            if (fake)
            {
              fake->no_table_names_allowed= 1;
              lex->current_select= fake;
            }
            thd->where= "global ORDER clause";
          }
          order_or_limit
          {
            thd->lex->current_select->no_table_names_allowed= 0;
            thd->where= "";
          }
        ;

order_or_limit:
          order_clause opt_limit_clause
        | limit_clause
        ;

/*
  Start a UNION, for non-top level query expressions.
*/
union_head_non_top:
          UNION_SYM union_option
          {
            if (add_select_to_union_list(Lex, (bool)$2, FALSE))
              MYSQL_YYABORT;
          }
        ;

union_option:
          /* empty */ { $$=1; }
        | DISTINCT  { $$=1; }
        | ALL       { $$=0; }
        ;

/*
  Corresponds to the SQL Standard
  <query specification> ::=
    SELECT [ <set quantifier> ] <select list> <table expression>

  Notes:
  - We allow more options in addition to <set quantifier>
  - <table expression> is optional in MariaDB
*/
query_specification:
          SELECT_SYM select_init2_derived opt_table_expression
          {
            $$= Lex->current_select->master_unit()->first_select();
          }
        ;

query_term_union_not_ready:
          query_specification order_or_limit opt_select_lock_type { $$= $1; }
        | '(' select_paren_derived ')' union_order_or_limit       { $$= $2; }
        ;

query_term_union_ready:
          query_specification opt_select_lock_type                { $$= $1; }
        | '(' select_paren_derived ')'                            { $$= $2; }
        ;

query_expression_body:
          query_term_union_not_ready                                { $$= $1; }
        | query_term_union_ready                                    { $$= $1; }
        | query_term_union_ready union_list_derived                 { $$= $1; }
        ;

/* Corresponds to <query expression> in the SQL:2003 standard. */
subselect:
          subselect_start opt_with_clause query_expression_body subselect_end
          { 
            $3->set_with_clause($2);
            $$= $3;
          }
        ;

subselect_start:
          {
            LEX *lex=Lex;
            if (!lex->expr_allows_subselect ||
               lex->sql_command == (int)SQLCOM_PURGE)
            {
              my_parse_error(thd, ER_SYNTAX_ERROR);
              MYSQL_YYABORT;
            }
            /* 
              we are making a "derived table" for the parenthesis
              as we need to have a lex level to fit the union 
              after the parenthesis, e.g. 
              (SELECT .. ) UNION ...  becomes 
              SELECT * FROM ((SELECT ...) UNION ...)
            */
            if (mysql_new_select(Lex, 1))
              MYSQL_YYABORT;
          }
        ;

subselect_end:
          {
            LEX *lex=Lex;

            lex->pop_context();
            SELECT_LEX *child= lex->current_select;
            lex->current_select = lex->current_select->return_after_parsing();
            lex->nest_level--;
            lex->current_select->n_child_sum_items += child->n_sum_items;

            /*
              A subquery (and all the subsequent query blocks in a UNION) can
              add columns to an outer query block. Reserve space for them.
              Aggregate functions in having clause can also add fields to an
              outer select.
            */
            for (SELECT_LEX *temp= child->master_unit()->first_select();
                 temp != NULL; temp= temp->next_select())
            {
              lex->current_select->select_n_where_fields+=
                temp->select_n_where_fields;
              lex->current_select->select_n_having_items+=
                temp->select_n_having_items;
            }
          }
        ;

opt_query_expression_options:
          /* empty */
        | query_expression_option_list
        ;

query_expression_option_list:
          query_expression_option_list query_expression_option
        | query_expression_option
        ;

query_expression_option:
          STRAIGHT_JOIN { Select->options|= SELECT_STRAIGHT_JOIN; }
        | HIGH_PRIORITY
          {
            if (check_simple_select())
              MYSQL_YYABORT;
            YYPS->m_lock_type= TL_READ_HIGH_PRIORITY;
            YYPS->m_mdl_type= MDL_SHARED_READ;
            Select->options|= SELECT_HIGH_PRIORITY;
          }
        | DISTINCT         { Select->options|= SELECT_DISTINCT; }
        | SQL_SMALL_RESULT { Select->options|= SELECT_SMALL_RESULT; }
        | SQL_BIG_RESULT   { Select->options|= SELECT_BIG_RESULT; }
        | SQL_BUFFER_RESULT
          {
            if (check_simple_select())
              MYSQL_YYABORT;
            Select->options|= OPTION_BUFFER_RESULT;
          }
        | SQL_CALC_FOUND_ROWS
          {
            if (check_simple_select())
              MYSQL_YYABORT;
            Select->options|= OPTION_FOUND_ROWS;
          }
        | ALL { Select->options|= SELECT_ALL; }
        ;

/**************************************************************************

 CREATE VIEW | TRIGGER | PROCEDURE statements.

**************************************************************************/

view_or_trigger_or_sp_or_event:
          definer definer_tail
          {}
        | no_definer no_definer_tail
          {}
        | view_algorithm definer_opt view_tail
          {}
        ;

definer_tail:
          view_tail
        | trigger_tail
        | sp_tail
        | sf_tail
        | event_tail
        ;

no_definer_tail:
          view_tail
        | trigger_tail
        | sp_tail
        | sf_tail
        | udf_tail
        | event_tail
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
          ALGORITHM_SYM '=' UNDEFINED_SYM
          { Lex->create_view_algorithm= DTYPE_ALGORITHM_UNDEFINED; }
        | ALGORITHM_SYM '=' MERGE_SYM
          { Lex->create_view_algorithm= VIEW_ALGORITHM_MERGE; }
        | ALGORITHM_SYM '=' TEMPTABLE_SYM
          { Lex->create_view_algorithm= VIEW_ALGORITHM_TMPTABLE; }
        ;

view_suid:
          /* empty */
          { Lex->create_view_suid= VIEW_SUID_DEFAULT; }
        | SQL_SYM SECURITY_SYM DEFINER_SYM
          { Lex->create_view_suid= VIEW_SUID_DEFINER; }
        | SQL_SYM SECURITY_SYM INVOKER_SYM
          { Lex->create_view_suid= VIEW_SUID_INVOKER; }
        ;

view_tail:
          view_suid VIEW_SYM opt_if_not_exists table_ident
          {
            LEX *lex= thd->lex;
            if (lex->add_create_options_with_check($3))
              MYSQL_YYABORT;
            lex->sql_command= SQLCOM_CREATE_VIEW;
            /* first table in list is target VIEW name */
            if (!lex->select_lex.add_table_to_list(thd, $4, NULL,
                                                   TL_OPTION_UPDATING,
                                                   TL_IGNORE,
                                                   MDL_EXCLUSIVE))
              MYSQL_YYABORT;
            lex->query_tables->open_strategy= TABLE_LIST::OPEN_STUB;
          }
          view_list_opt AS view_select
        ;

view_list_opt:
          /* empty */
          {}
        | '(' view_list ')'
        ;

view_list:
          ident 
          {
            Lex->view_list.push_back((LEX_STRING*)
                                     thd->memdup(&$1, sizeof(LEX_STRING)),
                                     thd->mem_root);
          }
        | view_list ',' ident
          {
            Lex->view_list.push_back((LEX_STRING*)
                                     thd->memdup(&$3, sizeof(LEX_STRING)),
                                     thd->mem_root);
          }
        ;

view_select:
          {
            LEX *lex= Lex;
            lex->parsing_options.allows_variable= FALSE;
            lex->create_view_select.str= (char *) YYLIP->get_cpp_ptr();
          }
          opt_with_clause query_expression_body_view view_check_option
          {
            LEX *lex= Lex;
            size_t len= YYLIP->get_cpp_ptr() - lex->create_view_select.str;
            uint not_used;
            void *create_view_select= thd->memdup(lex->create_view_select.str, len);
            lex->create_view_select.length= len;
            lex->create_view_select.str= (char *) create_view_select;
            trim_whitespace(thd->charset(), &lex->create_view_select,
                            &not_used);
            lex->parsing_options.allows_variable= TRUE;
            lex->current_select->set_with_clause($2);
            if (Lex->check_cte_dependencies_and_resolve_references())
              MYSQL_YYABORT;

          }
        ;

/*
  SQL Standard <query expression body> for VIEWs.
  Does not include INTO and PROCEDURE clauses.
*/
query_expression_body_view:
          SELECT_SYM select_options_and_item_list select_init3_view
        | '(' select_paren_view ')'
        | '(' select_paren_view ')' union_order_or_limit
        | '(' select_paren_view ')' union_list_view
        ;

view_check_option:
          /* empty */
          { Lex->create_view_check= VIEW_CHECK_NONE; }
        | WITH CHECK_SYM OPTION
          { Lex->create_view_check= VIEW_CHECK_CASCADED; }
        | WITH CASCADED CHECK_SYM OPTION
          { Lex->create_view_check= VIEW_CHECK_CASCADED; }
        | WITH LOCAL_SYM CHECK_SYM OPTION
          { Lex->create_view_check= VIEW_CHECK_LOCAL; }
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
          TRIGGER_SYM
          remember_name
          opt_if_not_exists
          {
            if (Lex->add_create_options_with_check($3))
              MYSQL_YYABORT;
          }
          sp_name
          trg_action_time
          trg_event
          ON
          remember_name /* $9 */
          { /* $10 */
            Lex->raw_trg_on_table_name_begin= YYLIP->get_tok_start();
          }
          table_ident /* $11 */
          FOR_SYM
          remember_name /* $13 */
          { /* $14 */
            Lex->raw_trg_on_table_name_end= YYLIP->get_tok_start();
          }
          EACH_SYM
          ROW_SYM
          {
            Lex->trg_chistics.ordering_clause_begin= YYLIP->get_cpp_ptr();
          }
          trigger_follows_precedes_clause /* $18 */
          { /* $19 */
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;

            if (lex->sphead)
              my_yyabort_error((ER_SP_NO_RECURSIVE_CREATE, MYF(0), "TRIGGER"));

            lex->stmt_definition_begin= $2;
            lex->ident.str= $9;
            lex->ident.length= $13 - $9;
            lex->spname= $5;
            (*static_cast<st_trg_execution_order*>(&lex->trg_chistics))= ($18);
            lex->trg_chistics.ordering_clause_end= lip->get_cpp_ptr();

            if (!make_sp_head(thd, $5, TYPE_ENUM_TRIGGER))
              MYSQL_YYABORT;

            lex->sphead->set_body_start(thd, lip->get_cpp_tok_start());
          }
          sp_proc_stmt force_lookahead
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;

            lex->sql_command= SQLCOM_CREATE_TRIGGER;
            sp->set_stmt_end(thd);
            sp->restore_thd_mem_root(thd);

            if (sp->is_not_allowed_in_function("trigger"))
              MYSQL_YYABORT;

            /*
              We have to do it after parsing trigger body, because some of
              sp_proc_stmt alternatives are not saving/restoring LEX, so
              lex->query_tables can be wiped out.
            */
            if (!lex->select_lex.add_table_to_list(thd, $11,
                                                   (LEX_STRING*) 0,
                                                   TL_OPTION_UPDATING,
                                                   TL_READ_NO_INSERT,
                                                   MDL_SHARED_NO_WRITE))
              MYSQL_YYABORT;
          }
        ;

/**************************************************************************

 CREATE FUNCTION | PROCEDURE statements parts.

**************************************************************************/

udf_tail:
          AGGREGATE_SYM udf_tail2 { thd->lex->udf.type= UDFTYPE_AGGREGATE; }
        | udf_tail2               { thd->lex->udf.type= UDFTYPE_FUNCTION;  }
        ;

udf_tail2:
          FUNCTION_SYM opt_if_not_exists ident
          RETURNS_SYM udf_type SONAME_SYM TEXT_STRING_sys
          {
            LEX *lex= thd->lex;
            if (lex->add_create_options_with_check($2))
              MYSQL_YYABORT;
            if (is_native_function(thd, & $3))
              my_yyabort_error((ER_NATIVE_FCT_NAME_COLLISION, MYF(0), $3.str));
            lex->sql_command= SQLCOM_CREATE_FUNCTION;
            lex->udf.name= $3;
            lex->udf.returns= (Item_result) $5;
            lex->udf.dl= $7.str;
          }
        ;

sf_tail:
          FUNCTION_SYM /* $1 */
          opt_if_not_exists /* $2 */
          sp_name /* $3 */
          '(' /* $4 */
          { /* $5 */
            LEX *lex= Lex;
            Lex_input_stream *lip= YYLIP;
            const char* tmp_param_begin;

            if (lex->add_create_options_with_check($2))
              MYSQL_YYABORT;
            lex->spname= $3;

            if (lex->sphead)
              my_yyabort_error((ER_SP_NO_RECURSIVE_CREATE, MYF(0), "FUNCTION"));

            if (!make_sp_head(thd, $3, TYPE_ENUM_FUNCTION))
              MYSQL_YYABORT;

            tmp_param_begin= lip->get_cpp_tok_start();
            tmp_param_begin++;
            lex->sphead->m_param_begin= tmp_param_begin;
          }
          sp_fdparam_list /* $6 */
          ')' /* $7 */
          { /* $8 */
            Lex->sphead->m_param_end= YYLIP->get_cpp_tok_start();
          }
          RETURNS_SYM /* $9 */
          { /* $10 */
            LEX *lex= Lex;
            lex->init_last_field(&lex->sphead->m_return_field_def, NULL,
                                 thd->variables.collation_database);
          }
          field_type /* $11 */
          { /* $12 */
            Lex->set_last_field_type($11);
            if (Lex->sphead->fill_field_definition(thd, Lex, Lex->last_field))
              MYSQL_YYABORT;
          }
          sp_c_chistics /* $13 */
          { /* $14 */
            LEX *lex= thd->lex;
            Lex_input_stream *lip= YYLIP;

            lex->sphead->set_body_start(thd, lip->get_cpp_tok_start());
          }
          sp_proc_stmt_in_returns_clause /* $15 */ force_lookahead
          {
            LEX *lex= thd->lex;
            sp_head *sp= lex->sphead;

            if (sp->is_not_allowed_in_function("function"))
              MYSQL_YYABORT;

            lex->sql_command= SQLCOM_CREATE_SPFUNCTION;
            sp->set_stmt_end(thd);
            if (!(sp->m_flags & sp_head::HAS_RETURN))
              my_yyabort_error((ER_SP_NORETURN, MYF(0), sp->m_qname.str));
            if (is_native_function(thd, & sp->m_name))
            {
              /*
                This warning will be printed when
                [1] A client query is parsed,
                [2] A stored function is loaded by db_load_routine.
                Printing the warning for [2] is intentional, to cover the
                following scenario:
                - A user define a SF 'foo' using MySQL 5.N
                - An application uses select foo(), and works.
                - MySQL 5.{N+1} defines a new native function 'foo', as
                part of a new feature.
                - MySQL 5.{N+1} documentation is updated, and should mention
                that there is a potential incompatible change in case of
                existing stored function named 'foo'.
                - The user deploys 5.{N+1}. At this point, 'select foo()'
                means something different, and the user code is most likely
                broken (it's only safe if the code is 'select db.foo()').
                With a warning printed when the SF is loaded (which has to
                occur before the call), the warning will provide a hint
                explaining the root cause of a later failure of 'select foo()'.
                With no warning printed, the user code will fail with no
                apparent reason.
                Printing a warning each time db_load_routine is executed for
                an ambiguous function is annoying, since that can happen a lot,
                but in practice should not happen unless there *are* name
                collisions.
                If a collision exists, it should not be silenced but fixed.
              */
              push_warning_printf(thd,
                                  Sql_condition::WARN_LEVEL_NOTE,
                                  ER_NATIVE_FCT_NAME_COLLISION,
                                  ER_THD(thd, ER_NATIVE_FCT_NAME_COLLISION),
                                  sp->m_name.str);
            }
            sp->restore_thd_mem_root(thd);
          }
        ;

sp_tail:
          PROCEDURE_SYM opt_if_not_exists sp_name
          {
            if (Lex->add_create_options_with_check($2))
              MYSQL_YYABORT;

            if (Lex->sphead)
              my_yyabort_error((ER_SP_NO_RECURSIVE_CREATE, MYF(0), "PROCEDURE"));

            if (!make_sp_head(thd, $3, TYPE_ENUM_PROCEDURE))
              MYSQL_YYABORT;
            Lex->spname= $3;
          }
          '('
          {
            const char* tmp_param_begin;

            tmp_param_begin= YYLIP->get_cpp_tok_start();
            tmp_param_begin++;
            Lex->sphead->m_param_begin= tmp_param_begin;
          }
          sp_pdparam_list
          ')'
          {
            Lex->sphead->m_param_end= YYLIP->get_cpp_tok_start();
          }
          sp_c_chistics
          {
            Lex->sphead->set_body_start(thd, YYLIP->get_cpp_tok_start());
          }
          sp_proc_stmt force_lookahead
          {
            LEX *lex= Lex;
            sp_head *sp= lex->sphead;

            sp->set_stmt_end(thd);
            lex->sql_command= SQLCOM_CREATE_PROCEDURE;
            sp->restore_thd_mem_root(thd);
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
        | XA_SYM RECOVER_SYM
          {
            Lex->sql_command = SQLCOM_XA_RECOVER;
          }
        ;

xid:
          text_string
          {
            MYSQL_YYABORT_UNLESS($1->length() <= MAXGTRIDSIZE);
            if (!(Lex->xid=(XID *)thd->alloc(sizeof(XID))))
              MYSQL_YYABORT;
            Lex->xid->set(1L, $1->ptr(), $1->length(), 0, 0);
          }
          | text_string ',' text_string
          {
            MYSQL_YYABORT_UNLESS($1->length() <= MAXGTRIDSIZE && $3->length() <= MAXBQUALSIZE);
            if (!(Lex->xid=(XID *)thd->alloc(sizeof(XID))))
              MYSQL_YYABORT;
            Lex->xid->set(1L, $1->ptr(), $1->length(), $3->ptr(), $3->length());
          }
          | text_string ',' text_string ',' ulong_num
          {
            MYSQL_YYABORT_UNLESS($1->length() <= MAXGTRIDSIZE && $3->length() <= MAXBQUALSIZE);
            if (!(Lex->xid=(XID *)thd->alloc(sizeof(XID))))
              MYSQL_YYABORT;
            Lex->xid->set($5, $1->ptr(), $1->length(), $3->ptr(), $3->length());
          }
        ;

begin_or_start:
          BEGIN_SYM {}
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
          INSTALL_SYM PLUGIN_SYM ident SONAME_SYM TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_INSTALL_PLUGIN;
            lex->comment= $3;
            lex->ident= $5;
          }
        | INSTALL_SYM SONAME_SYM TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_INSTALL_PLUGIN;
            lex->comment= null_lex_str;
            lex->ident= $3;
          }
        ;

uninstall:
          UNINSTALL_SYM PLUGIN_SYM ident
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_UNINSTALL_PLUGIN;
            lex->comment= $3;
          }
        | UNINSTALL_SYM SONAME_SYM TEXT_STRING_sys
          {
            LEX *lex= Lex;
            lex->sql_command= SQLCOM_UNINSTALL_PLUGIN;
            lex->comment= null_lex_str;
            lex->ident= $3;
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
