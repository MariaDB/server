/* Copyright (c) 2000, 2019, Oracle and/or its affiliates.
   Copyright (c) 2009, 2020, MariaDB Corporation

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


/* A lexical scanner on a temporary buffer with a yacc interface */

#define MYSQL_LEX 1
#include "mariadb.h"
#include "sql_priv.h"
#include "sql_class.h"                          // sql_lex.h: SQLCOM_END
#include "sql_lex.h"
#include "sql_parse.h"                          // add_to_list
#include "item_create.h"
#include <m_ctype.h>
#include <hash.h>
#include "sp_head.h"
#include "sp.h"
#include "sql_select.h"
#include "sql_cte.h"
#include "sql_signal.h"
#include "sql_truncate.h"                      // Sql_cmd_truncate_table
#include "sql_admin.h"                         // Sql_cmd_analyze/Check..._table
#include "sql_partition.h"
#include "sql_partition_admin.h"               // Sql_cmd_alter_table_*_part
#include "event_parse_data.h"

void LEX::parse_error(uint err_number)
{
  thd->parse_error(err_number);
}


/**
  LEX_STRING constant for null-string to be used in parser and other places.
*/
const LEX_STRING empty_lex_str=   {(char *) "", 0};
const LEX_CSTRING null_clex_str=  {NULL, 0};
const LEX_CSTRING empty_clex_str= {"", 0};
const LEX_CSTRING star_clex_str=  {"*", 1};
const LEX_CSTRING param_clex_str= {"?", 1};


/**
  Helper action for a case expression statement (the expr in 'CASE expr').
  This helper is used for 'searched' cases only.
  @param lex the parser lex context
  @param expr the parsed expression
  @return 0 on success
*/

int sp_expr_lex::case_stmt_action_expr()
{
  int case_expr_id= spcont->register_case_expr();
  sp_instr_set_case_expr *i;

  if (spcont->push_case_expr_id(case_expr_id))
    return 1;

  i= new (thd->mem_root)
    sp_instr_set_case_expr(sphead->instructions(), spcont, case_expr_id,
                           get_item(), this);

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

int sp_expr_lex::case_stmt_action_when(bool simple)
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

    expr= new (thd->mem_root) Item_func_eq(thd, var, get_item());
    i= new (thd->mem_root) sp_instr_jump_if_not(ip, spcont, expr, this);
  }
  else
    i= new (thd->mem_root) sp_instr_jump_if_not(ip, spcont, get_item(), this);

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
                         sys_var *sysvar, const Lex_ident_sys_st *base_name,
                         Item *val)
{
  set_var *setvar;

  /* No AUTOCOMMIT from a stored function or trigger. */
  if (spcont && sysvar == Sys_autocommit_ptr)
    sphead->m_flags|= sp_head::HAS_SET_AUTOCOMMIT_STMT;

  if (val && val->type() == Item::FIELD_ITEM &&
      ((Item_field*)val)->table_name.str)
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
                               *name, UPDATE_ACL, FALSE);

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
  @param pos           The position in the raw SQL buffer
*/


bool sp_create_assignment_lex(THD *thd, const char *pos)
{
  if (thd->lex->sphead)
  {
    sp_lex_local *new_lex;
    if (!(new_lex= new (thd->mem_root) sp_lex_set_var(thd, thd->lex)) ||
        new_lex->main_select_push())
      return true;
    new_lex->sphead->m_tmp_query= pos;
    return thd->lex->sphead->reset_lex(thd, new_lex);
  }
  return false;
}


/**
  Create a SP instruction for a SET assignment.

  @see sp_create_assignment_lex

  @param thd              - Thread context
  @param no_lookahead     - True if the parser has no lookahead
  @param need_set_keyword - if a SET statement "SET a=10",
                            or a direct assignment overwise "a:=10"
  @return false if success, true otherwise.
*/

bool sp_create_assignment_instr(THD *thd, bool no_lookahead,
                                bool need_set_keyword)
{
  LEX *lex= thd->lex;

  if (lex->sphead)
  {
    if (!lex->var_list.is_empty())
    {
      /*
        - Every variable assignment from the same SET command, e.g.:
            SET @var1=expr1, @var2=expr2;
          produce each own sp_create_assignment_instr() call
          lex->var_list.elements is 1 in this case.
        - This query:
            SET TRANSACTION READ ONLY, ISOLATION LEVEL SERIALIZABLE;
          in translated to:
            SET tx_read_only=1, tx_isolation=ISO_SERIALIZABLE;
          but produces a single sp_create_assignment_instr() call
          which includes the query fragment covering both options.
      */
      DBUG_ASSERT(lex->var_list.elements >= 1 && lex->var_list.elements <= 2);
      /*
        sql_mode=ORACLE's direct assignment of a global variable
        is not possible by the grammar.
      */
      DBUG_ASSERT(lex->option_type != OPT_GLOBAL || need_set_keyword);
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
      static const LEX_CSTRING setlc= { STRING_WITH_LEN("SET ") };
      static const LEX_CSTRING setgl= { STRING_WITH_LEN("SET GLOBAL ") };
      const char *qend= no_lookahead ? lip->get_ptr() : lip->get_tok_end();
      Lex_cstring qbuf(lex->sphead->m_tmp_query, qend);
      if (lex->new_sp_instr_stmt(thd,
                                 lex->option_type == OPT_GLOBAL ? setgl :
                                 need_set_keyword ?               setlc :
                                                                  null_clex_str,
                                 qbuf))
        return true;
    }
    lex->pop_select();
    if (lex->check_main_unit_semantics())
    {
      /*
        "lex" can be referrenced by:
        - sp_instr_set                          SET a= expr;
        - sp_instr_set_row_field                SET r.a= expr;
        - sp_instr_stmt (just generated above)  SET @a= expr;
        In this case, "lex" is fully owned by sp_instr_xxx and it will
        be deleted by the destructor ~sp_instr_xxx().
        So we should remove "lex" from the stack sp_head::m_lex,
        to avoid double free.
        Note, in case "lex" is not owned by any sp_instr_xxx,
        it's also safe to remove it from the stack right now.
        So we can remove it unconditionally, without testing lex->sp_lex_in_use.
      */
      lex->sphead->restore_lex(thd);
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


bool LEX::add_alter_list(LEX_CSTRING name, Virtual_column_info *expr,
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


bool LEX::add_alter_list(LEX_CSTRING name, LEX_CSTRING new_name, bool exists)
{
  Alter_column *ac= new (thd->mem_root) Alter_column(name, new_name, exists);
  if (unlikely(ac == NULL))
    return true;
  alter_info.alter_list.push_back(ac, thd->mem_root);
  alter_info.flags|= ALTER_RENAME_COLUMN;
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


Virtual_column_info *add_virtual_expression(THD *thd, Item *expr)
{
  Virtual_column_info *v= new (thd->mem_root) Virtual_column_info();
  if (unlikely(!v))
     return 0;
   v->expr= expr;
   v->utf8= 0;  /* connection charset */
   return v;
}



/**
  @note The order of the elements of this array must correspond to
  the order of elements in enum_binlog_stmt_unsafe.
*/
const int
Query_tables_list::binlog_stmt_unsafe_errcode[BINLOG_STMT_UNSAFE_COUNT] =
{
  ER_BINLOG_UNSAFE_LIMIT,
  ER_BINLOG_UNSAFE_INSERT_DELAYED,
  ER_BINLOG_UNSAFE_SYSTEM_TABLE,
  ER_BINLOG_UNSAFE_AUTOINC_COLUMNS,
  ER_BINLOG_UNSAFE_UDF,
  ER_BINLOG_UNSAFE_SYSTEM_VARIABLE,
  ER_BINLOG_UNSAFE_SYSTEM_FUNCTION,
  ER_BINLOG_UNSAFE_NONTRANS_AFTER_TRANS,
  ER_BINLOG_UNSAFE_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE,
  ER_BINLOG_UNSAFE_MIXED_STATEMENT,
  ER_BINLOG_UNSAFE_INSERT_IGNORE_SELECT,
  ER_BINLOG_UNSAFE_INSERT_SELECT_UPDATE,
  ER_BINLOG_UNSAFE_WRITE_AUTOINC_SELECT,
  ER_BINLOG_UNSAFE_REPLACE_SELECT,
  ER_BINLOG_UNSAFE_CREATE_IGNORE_SELECT,
  ER_BINLOG_UNSAFE_CREATE_REPLACE_SELECT,
  ER_BINLOG_UNSAFE_CREATE_SELECT_AUTOINC,
  ER_BINLOG_UNSAFE_UPDATE_IGNORE,
  ER_BINLOG_UNSAFE_INSERT_TWO_KEYS,
  ER_BINLOG_UNSAFE_AUTOINC_NOT_FIRST
};


/* Longest standard keyword name */

#define TOCK_NAME_LENGTH 24

/*
  The following data is based on the latin1 character set, and is only
  used when comparing keywords
*/

static uchar to_upper_lex[]=
{
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14, 15,
   16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31,
   32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47,
   48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
   64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90, 91, 92, 93, 94, 95,
   96, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
   80, 81, 82, 83, 84, 85, 86, 87, 88, 89, 90,123,124,125,126,127,
  128,129,130,131,132,133,134,135,136,137,138,139,140,141,142,143,
  144,145,146,147,148,149,150,151,152,153,154,155,156,157,158,159,
  160,161,162,163,164,165,166,167,168,169,170,171,172,173,174,175,
  176,177,178,179,180,181,182,183,184,185,186,187,188,189,190,191,
  192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,215,216,217,218,219,220,221,222,223,
  192,193,194,195,196,197,198,199,200,201,202,203,204,205,206,207,
  208,209,210,211,212,213,214,247,216,217,218,219,220,221,222,255
};

/* 
  Names of the index hints (for error messages). Keep in sync with 
  index_hint_type 
*/

const char * index_hint_type_name[] =
{
  "IGNORE INDEX", 
  "USE INDEX", 
  "FORCE INDEX"
};

inline int lex_casecmp(const char *s, const char *t, uint len)
{
  while (len-- != 0 &&
         to_upper_lex[(uchar) *s++] == to_upper_lex[(uchar) *t++]) ;
  return (int) len+1;
}

#include <lex_hash.h>


void lex_init(void)
{
  uint i;
  DBUG_ENTER("lex_init");
  for (i=0 ; i < array_elements(symbols) ; i++)
    symbols[i].length=(uchar) strlen(symbols[i].name);
  for (i=0 ; i < array_elements(sql_functions) ; i++)
    sql_functions[i].length=(uchar) strlen(sql_functions[i].name);

  DBUG_VOID_RETURN;
}


void lex_free(void)
{                                        // Call this when daemon ends
  DBUG_ENTER("lex_free");
  DBUG_VOID_RETURN;
}

/**
  Initialize lex object for use in fix_fields and parsing.

  SYNOPSIS
    init_lex_with_single_table()
    @param thd                 The thread object
    @param table               The table object
  @return Operation status
    @retval TRUE                An error occurred, memory allocation error
    @retval FALSE               Ok

  DESCRIPTION
    This function is used to initialize a lex object on the
    stack for use by fix_fields and for parsing. In order to
    work properly it also needs to initialize the
    Name_resolution_context object of the lexer.
    Finally it needs to set a couple of variables to ensure
    proper functioning of fix_fields.
*/

int
init_lex_with_single_table(THD *thd, TABLE *table, LEX *lex)
{
  TABLE_LIST *table_list;
  Table_ident *table_ident;
  SELECT_LEX *select_lex= lex->first_select_lex();
  Name_resolution_context *context= &select_lex->context;
  /*
    We will call the parser to create a part_info struct based on the
    partition string stored in the frm file.
    We will use a local lex object for this purpose. However we also
    need to set the Name_resolution_object for this lex object. We
    do this by using add_table_to_list where we add the table that
    we're working with to the Name_resolution_context.
  */
  thd->lex= lex;
  lex_start(thd);
  context->init();
  if (unlikely((!(table_ident= new Table_ident(thd,
                                               &table->s->db,
                                               &table->s->table_name,
                                               TRUE)))) ||
      (unlikely(!(table_list= select_lex->add_table_to_list(thd,
                                                            table_ident,
                                                            NULL,
                                                            0)))))
    return TRUE;
  context->resolve_in_table_list_only(table_list);
  lex->use_only_table_context= TRUE;
  lex->context_analysis_only|= CONTEXT_ANALYSIS_ONLY_VCOL_EXPR;
  select_lex->cur_pos_in_select_list= UNDEF_POS;
  table->map= 1; //To ensure correct calculation of const item
  table_list->table= table;
  table_list->cacheable_table= false;
  lex->create_last_non_select_table= table_list;
  return FALSE;
}

/**
  End use of local lex with single table

  SYNOPSIS
    end_lex_with_single_table()
    @param thd               The thread object
    @param table             The table object
    @param old_lex           The real lex object connected to THD

  DESCRIPTION
    This function restores the real lex object after calling
    init_lex_with_single_table and also restores some table
    variables temporarily set.
*/

void
end_lex_with_single_table(THD *thd, TABLE *table, LEX *old_lex)
{
  LEX *lex= thd->lex;
  table->map= 0;
  table->get_fields_in_item_tree= FALSE;
  lex_end(lex);
  thd->lex= old_lex;
}


void
st_parsing_options::reset()
{
  allows_variable= TRUE;
}


/**
  Perform initialization of Lex_input_stream instance.

  Basically, a buffer for pre-processed query. This buffer should be large
  enough to keep multi-statement query. The allocation is done once in
  Lex_input_stream::init() in order to prevent memory pollution when
  the server is processing large multi-statement queries.
*/

bool Lex_input_stream::init(THD *thd,
                            char* buff,
                            size_t length)
{
  DBUG_EXECUTE_IF("bug42064_simulate_oom",
                  DBUG_SET("+d,simulate_out_of_memory"););

  m_cpp_buf= (char*) thd->alloc(length + 1);

  DBUG_EXECUTE_IF("bug42064_simulate_oom",
                  DBUG_SET("-d,bug42064_simulate_oom");); 

  if (m_cpp_buf == NULL)
    return true;

  m_thd= thd;
  reset(buff, length);

  return false;
}


/**
  Prepare Lex_input_stream instance state for use for handling next SQL statement.

  It should be called between two statements in a multi-statement query.
  The operation resets the input stream to the beginning-of-parse state,
  but does not reallocate m_cpp_buf.
*/

void
Lex_input_stream::reset(char *buffer, size_t length)
{
  yylineno= 1;
  lookahead_token= -1;
  lookahead_yylval= NULL;
  m_ptr= buffer;
  m_tok_start= NULL;
  m_tok_end= NULL;
  m_end_of_query= buffer + length;
  m_tok_start_prev= NULL;
  m_buf= buffer;
  m_buf_length= length;
  m_echo= TRUE;
  m_cpp_tok_start= NULL;
  m_cpp_tok_start_prev= NULL;
  m_cpp_tok_end= NULL;
  m_body_utf8= NULL;
  m_cpp_utf8_processed_ptr= NULL;
  next_state= MY_LEX_START;
  found_semicolon= NULL;
  ignore_space= MY_TEST(m_thd->variables.sql_mode & MODE_IGNORE_SPACE);
  stmt_prepare_mode= FALSE;
  multi_statements= TRUE;
  in_comment=NO_COMMENT;
  m_underscore_cs= NULL;
  m_cpp_ptr= m_cpp_buf;
}


/**
  The operation is called from the parser in order to
  1) designate the intention to have utf8 body;
  1) Indicate to the lexer that we will need a utf8 representation of this
     statement;
  2) Determine the beginning of the body.

  @param thd        Thread context.
  @param begin_ptr  Pointer to the start of the body in the pre-processed
                    buffer.
*/

void Lex_input_stream::body_utf8_start(THD *thd, const char *begin_ptr)
{
  DBUG_ASSERT(begin_ptr);
  DBUG_ASSERT(m_cpp_buf <= begin_ptr && begin_ptr <= m_cpp_buf + m_buf_length);

  size_t body_utf8_length= get_body_utf8_maximum_length(thd);

  m_body_utf8= (char *) thd->alloc(body_utf8_length + 1);
  m_body_utf8_ptr= m_body_utf8;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= begin_ptr;
}


size_t Lex_input_stream::get_body_utf8_maximum_length(THD *thd)
{
  /*
    String literals can grow during escaping:
    1a. Character string '<TAB>' can grow to '\t', 3 bytes to 4 bytes growth.
    1b. Character string '1000 times <TAB>' grows from
        1002 to 2002 bytes (including quotes), which gives a little bit
        less than 2 times growth.
    "2" should be a reasonable multiplier that safely covers escaping needs.
  */
  return (m_buf_length / thd->variables.character_set_client->mbminlen) *
          my_charset_utf8mb3_bin.mbmaxlen * 2/*for escaping*/;
}


/**
  @brief The operation appends unprocessed part of pre-processed buffer till
  the given pointer (ptr) and sets m_cpp_utf8_processed_ptr to end_ptr.

  The idea is that some tokens in the pre-processed buffer (like character
  set introducers) should be skipped.

  Example:
    CPP buffer: SELECT 'str1', _latin1 'str2';
    m_cpp_utf8_processed_ptr -- points at the "SELECT ...";
    In order to skip "_latin1", the following call should be made:
      body_utf8_append(<pointer to "_latin1 ...">, <pointer to " 'str2'...">)

  @param ptr      Pointer in the pre-processed buffer, which specifies the
                  end of the chunk, which should be appended to the utf8
                  body.
  @param end_ptr  Pointer in the pre-processed buffer, to which
                  m_cpp_utf8_processed_ptr will be set in the end of the
                  operation.
*/

void Lex_input_stream::body_utf8_append(const char *ptr,
                                        const char *end_ptr)
{
  DBUG_ASSERT(m_cpp_buf <= ptr && ptr <= m_cpp_buf + m_buf_length);
  DBUG_ASSERT(m_cpp_buf <= end_ptr && end_ptr <= m_cpp_buf + m_buf_length);

  if (!m_body_utf8)
    return;

  if (m_cpp_utf8_processed_ptr >= ptr)
    return;

  size_t bytes_to_copy= ptr - m_cpp_utf8_processed_ptr;

  memcpy(m_body_utf8_ptr, m_cpp_utf8_processed_ptr, bytes_to_copy);
  m_body_utf8_ptr += bytes_to_copy;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= end_ptr;
}

/**
  The operation appends unprocessed part of the pre-processed buffer till
  the given pointer (ptr) and sets m_cpp_utf8_processed_ptr to ptr.

  @param ptr  Pointer in the pre-processed buffer, which specifies the end
              of the chunk, which should be appended to the utf8 body.
*/

void Lex_input_stream::body_utf8_append(const char *ptr)
{
  body_utf8_append(ptr, ptr);
}

/**
  The operation converts the specified text literal to the utf8 and appends
  the result to the utf8-body.

  @param thd      Thread context.
  @param txt      Text literal.
  @param txt_cs   Character set of the text literal.
  @param end_ptr  Pointer in the pre-processed buffer, to which
                  m_cpp_utf8_processed_ptr will be set in the end of the
                  operation.
*/

void
Lex_input_stream::body_utf8_append_ident(THD *thd,
                                         const Lex_string_with_metadata_st *txt,
                                         const char *end_ptr)
{
  if (!m_cpp_utf8_processed_ptr)
    return;

  LEX_CSTRING utf_txt;
  thd->make_text_string_sys(&utf_txt, txt); // QQ: check return value?

  /* NOTE: utf_txt.length is in bytes, not in symbols. */
  memcpy(m_body_utf8_ptr, utf_txt.str, utf_txt.length);
  m_body_utf8_ptr += utf_txt.length;
  *m_body_utf8_ptr= 0;

  m_cpp_utf8_processed_ptr= end_ptr;
}




extern "C" {

/**
  Escape a character. Consequently puts "escape" and "wc" characters into
  the destination utf8 string.
  @param cs     - the character set (utf8)
  @param escape - the escape character (backslash, single quote, double quote)
  @param wc     - the character to be escaped
  @param str    - the destination string
  @param end    - the end of the destination string
  @returns      - a code according to the wc_mb() convension.
*/
int my_wc_mb_utf8mb3_with_escape(CHARSET_INFO *cs, my_wc_t escape, my_wc_t wc,
                                 uchar *str, uchar *end)
{
  DBUG_ASSERT(escape > 0);
  if (str + 1 >= end)
    return MY_CS_TOOSMALL2;  // Not enough space, need at least two bytes.
  *str= (uchar)escape;
  int cnvres= my_charset_utf8mb3_handler.wc_mb(cs, wc, str + 1, end);
  if (cnvres > 0)
    return cnvres + 1;       // The character was normally put
  if (cnvres == MY_CS_ILUNI)
    return MY_CS_ILUNI;      // Could not encode "wc" (e.g. non-BMP character)
  DBUG_ASSERT(cnvres <= MY_CS_TOOSMALL);
  return cnvres - 1;         // Not enough space
}


/**
  Optionally escape a character.
  If "escape" is non-zero, then both "escape" and "wc" are put to
  the destination string. Otherwise, only "wc" is put.
  @param cs     - the character set (utf8)
  @param wc     - the character to be optionally escaped
  @param escape - the escape character, or 0
  @param ewc    - the escaped replacement of "wc" (e.g. 't' for '\t')
  @param str    - the destination string
  @param end    - the end of the destination string
  @returns      - a code according to the wc_mb() conversion.
*/
int my_wc_mb_utf8mb3_opt_escape(CHARSET_INFO *cs,
                                my_wc_t wc, my_wc_t escape, my_wc_t ewc,
                                uchar *str, uchar *end)
{
  return escape ? my_wc_mb_utf8mb3_with_escape(cs, escape, ewc, str, end) :
                  my_charset_utf8mb3_handler.wc_mb(cs, wc, str, end);
}

/**
  Encode a character with optional backlash escaping and quote escaping.
  Quote marks are escaped using another quote mark.
  Additionally, if "escape" is non-zero, then special characters are
  also escaped using "escape".
  Otherwise (if "escape" is zero, e.g. in case of MODE_NO_BACKSLASH_ESCAPES),
  then special characters are not escaped and handled as normal characters.

  @param cs        - the character set (utf8)
  @param wc        - the character to be encoded
  @param str       - the destination string
  @param end       - the end of the destination string
  @param sep       - the string delimiter (e.g. ' or ")
  @param escape    - the escape character (backslash, or 0)
  @returns         - a code according to the wc_mb() convension.
*/
int my_wc_mb_utf8mb3_escape(CHARSET_INFO *cs, my_wc_t wc,
                            uchar *str, uchar *end,
                            my_wc_t sep, my_wc_t escape)
{
  DBUG_ASSERT(escape == 0 || escape == '\\');
  DBUG_ASSERT(sep == '"' || sep == '\'');
  switch (wc) {
  case 0:      return my_wc_mb_utf8mb3_opt_escape(cs, wc, escape, '0', str, end);
  case '\t':   return my_wc_mb_utf8mb3_opt_escape(cs, wc, escape, 't', str, end);
  case '\r':   return my_wc_mb_utf8mb3_opt_escape(cs, wc, escape, 'r', str, end);
  case '\n':   return my_wc_mb_utf8mb3_opt_escape(cs, wc, escape, 'n', str, end);
  case '\032': return my_wc_mb_utf8mb3_opt_escape(cs, wc, escape, 'Z', str, end);
  case '\'':
  case '\"':
    if (wc == sep)
      return my_wc_mb_utf8mb3_with_escape(cs, wc, wc, str, end);
  }
  return my_charset_utf8mb3_handler.wc_mb(cs, wc, str, end); // No escaping needed
}


/** wc_mb() compatible routines for all sql_mode and delimiter combinations */
int my_wc_mb_utf8mb3_escape_single_quote_and_backslash(CHARSET_INFO *cs,
                                                    my_wc_t wc,
                                                    uchar *str, uchar *end)
{
  return my_wc_mb_utf8mb3_escape(cs, wc, str, end, '\'', '\\');
}


int my_wc_mb_utf8mb3_escape_double_quote_and_backslash(CHARSET_INFO *cs,
                                                    my_wc_t wc,
                                                    uchar *str, uchar *end)
{
  return my_wc_mb_utf8mb3_escape(cs, wc, str, end, '"', '\\');
}


int my_wc_mb_utf8mb3_escape_single_quote(CHARSET_INFO *cs, my_wc_t wc,
                                      uchar *str, uchar *end)
{
  return my_wc_mb_utf8mb3_escape(cs, wc, str, end, '\'', 0);
}


int my_wc_mb_utf8mb3_escape_double_quote(CHARSET_INFO *cs, my_wc_t wc,
                                      uchar *str, uchar *end)
{
  return my_wc_mb_utf8mb3_escape(cs, wc, str, end, '"', 0);
}

}; // End of extern "C"


/**
  Get an escaping function, depending on the current sql_mode and the
  string separator.
*/
my_charset_conv_wc_mb
Lex_input_stream::get_escape_func(THD *thd, my_wc_t sep) const
{
  return thd->backslash_escapes() ?
         (sep == '"' ? my_wc_mb_utf8mb3_escape_double_quote_and_backslash:
                       my_wc_mb_utf8mb3_escape_single_quote_and_backslash) :
         (sep == '"' ? my_wc_mb_utf8mb3_escape_double_quote:
                       my_wc_mb_utf8mb3_escape_single_quote);
}


/**
  Append a text literal to the end of m_body_utf8.
  The string is escaped according to the current sql_mode and the
  string delimiter (e.g. ' or ").

  @param thd       - current THD
  @param txt       - the string to be appended to m_body_utf8.
                     Note, the string must be already unescaped.
  @param cs        - the character set of the string
  @param end_ptr   - m_cpp_utf8_processed_ptr will be set to this value
                     (see body_utf8_append_ident for details)
  @param sep       - the string delimiter (single or double quote)
*/
void Lex_input_stream::body_utf8_append_escape(THD *thd,
                                               const LEX_CSTRING *txt,
                                               CHARSET_INFO *cs,
                                               const char *end_ptr,
                                               my_wc_t sep)
{
  DBUG_ASSERT(sep == '\'' || sep == '"');
  if (!m_cpp_utf8_processed_ptr)
    return;
  uint errors;
  /**
    We previously alloced m_body_utf8 to be able to store the query with all
    strings properly escaped. See get_body_utf8_maximum_length().
    So here we have guaranteedly enough space to append any string literal
    with escaping. Passing txt->length*2 as "available space" is always safe.
    For better safety purposes we could calculate get_body_utf8_maximum_length()
    every time we append a string, but this would affect performance negatively,
    so let's check that we don't get beyond the allocated buffer in
    debug build only.
  */
  DBUG_ASSERT(m_body_utf8 + get_body_utf8_maximum_length(thd) >=
              m_body_utf8_ptr + txt->length * 2);
  uint32 cnv_length= my_convert_using_func(m_body_utf8_ptr, txt->length * 2,
                                           &my_charset_utf8mb3_general_ci,
                                           get_escape_func(thd, sep),
                                           txt->str, txt->length,
                                           cs, cs->cset->mb_wc,
                                           &errors);
  m_body_utf8_ptr+= cnv_length;
  *m_body_utf8_ptr= 0;
  m_cpp_utf8_processed_ptr= end_ptr;
}


void Lex_input_stream::add_digest_token(uint token, LEX_YYSTYPE yylval)
{
  if (m_digest != NULL)
  {
    m_digest= digest_add_token(m_digest, token, yylval);
  }
}

void Lex_input_stream::reduce_digest_token(uint token_left, uint token_right)
{
  if (m_digest != NULL)
  {
    m_digest= digest_reduce_token(m_digest, token_left, token_right);
  }
}

/**
  lex starting operations for builtin select collected together
*/

void SELECT_LEX::lex_start(LEX *plex)
{
  SELECT_LEX_UNIT *unit= &plex->unit;
  /* 'parent_lex' is used in init_query() so it must be before it. */
  parent_lex= plex;
  init_query();
  master= unit;
  prev= &unit->slave;
  link_next= slave= next= 0;
  link_prev= (st_select_lex_node**)&(plex->all_selects_list);
  DBUG_ASSERT(!group_list_ptrs);
  select_number= 1;
  in_sum_expr=0;
  ftfunc_list_alloc.empty();
  ftfunc_list= &ftfunc_list_alloc;
  group_list.empty();
  order_list.empty();
  gorder_list.empty();
}

void lex_start(THD *thd)
{
  DBUG_ENTER("lex_start");
  thd->lex->start(thd);
  DBUG_VOID_RETURN;
}


/*
  This is called before every query that is to be parsed.
  Because of this, it's critical to not do too much things here.
  (We already do too much here)
*/

void LEX::start(THD *thd_arg)
{
  DBUG_ENTER("LEX::start");
  DBUG_PRINT("info", ("This: %p thd_arg->lex: %p", this, thd_arg->lex));

  thd= unit.thd= thd_arg;
  stmt_lex= this; // default, should be rewritten for VIEWs And CTEs

  DBUG_ASSERT(!explain);

  builtin_select.lex_start(this);
  lex_options= 0;
  context_stack.empty();
  //empty select_stack
  select_stack_top= 0;
  unit.init_query();
  current_select_number= 0;
  curr_with_clause= 0;
  with_clauses_list= 0;
  with_clauses_list_last_next= &with_clauses_list;
  clone_spec_offset= 0;
  create_view= NULL;
  field_list.empty();
  value_list.empty();
  update_list.empty();
  set_var_list.empty();
  param_list.empty();
  view_list.empty();
  with_persistent_for_clause= FALSE;
  column_list= NULL;
  index_list= NULL;
  prepared_stmt.lex_start();
  auxiliary_table_list.empty();
  unit.next= unit.master= unit.link_next= unit.return_to= 0;
  unit.prev= unit.link_prev= 0;
  unit.slave= current_select= all_selects_list= &builtin_select;
  sql_cache= LEX::SQL_CACHE_UNSPECIFIED;
  describe= 0;
  analyze_stmt= 0;
  explain_json= false;
  context_analysis_only= 0;
  derived_tables= 0;
  safe_to_cache_query= 1;
  parsing_options.reset();
  empty_field_list_on_rset= 0;
  part_info= 0;
  m_sql_cmd= NULL;
  duplicates= DUP_ERROR;
  ignore= 0;
  spname= NULL;
  spcont= NULL;
  proc_list.first= 0;
  escape_used= FALSE;
  default_used= FALSE;
  query_tables= 0;
  reset_query_tables_list(FALSE);
  clause_that_disallows_subselect= NULL;
  selects_allow_into= FALSE;
  selects_allow_procedure= FALSE;
  use_only_table_context= FALSE;
  parse_vcol_expr= FALSE;
  check_exists= FALSE;
  create_info.lex_start();
  verbose= 0;

  name= null_clex_str;
  event_parse_data= NULL;
  profile_options= PROFILE_NONE;
  nest_level= 0;
  builtin_select.nest_level_base= &unit;
  allow_sum_func.clear_all();
  in_sum_func= NULL;

  used_tables= 0;
  table_type= TABLE_TYPE_UNKNOWN;
  reset_slave_info.all= false;
  limit_rows_examined= 0;
  limit_rows_examined_cnt= ULONGLONG_MAX;
  var_list.empty();
  stmt_var_list.empty();
  proc_list.elements=0;

  save_group_list.empty();
  save_order_list.empty();
  win_ref= NULL;
  win_frame= NULL;
  frame_top_bound= NULL;
  frame_bottom_bound= NULL;
  win_spec= NULL;

  vers_conditions.empty();
  period_conditions.empty();

  is_lex_started= TRUE;

  next_is_main= FALSE;
  next_is_down= FALSE;

  wild= 0;
  exchange= 0;

  DBUG_VOID_RETURN;
}

void lex_end(LEX *lex)
{
  DBUG_ENTER("lex_end");
  DBUG_PRINT("enter", ("lex: %p", lex));

  lex_end_stage1(lex);
  lex_end_stage2(lex);

  DBUG_VOID_RETURN;
}

void lex_end_stage1(LEX *lex)
{
  DBUG_ENTER("lex_end_stage1");

  /* release used plugins */
  if (lex->plugins.elements) /* No function call and no mutex if no plugins. */
  {
    plugin_unlock_list(0, (plugin_ref*)lex->plugins.buffer, 
                       lex->plugins.elements);
  }
  reset_dynamic(&lex->plugins);

  if (lex->context_analysis_only & CONTEXT_ANALYSIS_ONLY_PREPARE)
  {
    /*
      Don't delete lex->sphead, it'll be needed for EXECUTE.
      Note that of all statements that populate lex->sphead
      only SQLCOM_COMPOUND can be PREPAREd
    */
    DBUG_ASSERT(lex->sphead == 0 || lex->sql_command == SQLCOM_COMPOUND);
  }
  else
  {
    sp_head::destroy(lex->sphead);
    lex->sphead= NULL;
  }

  DBUG_VOID_RETURN;
}

/*
  MASTER INFO parameters (or state) is normally cleared towards the end
  of a statement. But in case of PS, the state needs to be preserved during
  its lifetime and should only be cleared on PS close or deallocation.
*/
void lex_end_stage2(LEX *lex)
{
  DBUG_ENTER("lex_end_stage2");

  /* Reset LEX_MASTER_INFO */
  lex->mi.reset(lex->sql_command == SQLCOM_CHANGE_MASTER);
  delete_dynamic(&lex->delete_gtid_domain);

  DBUG_VOID_RETURN;
}

Yacc_state::~Yacc_state()
{
  if (yacc_yyss)
  {
    my_free(yacc_yyss);
    my_free(yacc_yyvs);
  }
}

int Lex_input_stream::find_keyword(Lex_ident_cli_st *kwd,
                                   uint len, bool function)
{
  const char *tok= m_tok_start;

  SYMBOL *symbol= get_hash_symbol(tok, len, function);
  if (symbol)
  {
    kwd->set_keyword(tok, len);
    DBUG_ASSERT(tok >= get_buf());
    DBUG_ASSERT(tok < get_end_of_query());

    if (m_thd->variables.sql_mode & MODE_ORACLE)
    {
      switch (symbol->tok) {
      case BEGIN_MARIADB_SYM:          return BEGIN_ORACLE_SYM;
      case BLOB_MARIADB_SYM:           return BLOB_ORACLE_SYM;
      case BODY_MARIADB_SYM:           return BODY_ORACLE_SYM;
      case CLOB_MARIADB_SYM:           return CLOB_ORACLE_SYM;
      case CONTINUE_MARIADB_SYM:       return CONTINUE_ORACLE_SYM;
      case DECLARE_MARIADB_SYM:        return DECLARE_ORACLE_SYM;
      case DECODE_MARIADB_SYM:         return DECODE_ORACLE_SYM;
      case ELSEIF_MARIADB_SYM:         return ELSEIF_ORACLE_SYM;
      case ELSIF_MARIADB_SYM:          return ELSIF_ORACLE_SYM;
      case EXCEPTION_MARIADB_SYM:      return EXCEPTION_ORACLE_SYM;
      case EXIT_MARIADB_SYM:           return EXIT_ORACLE_SYM;
      case GOTO_MARIADB_SYM:           return GOTO_ORACLE_SYM;
      case NUMBER_MARIADB_SYM:         return NUMBER_ORACLE_SYM;
      case OTHERS_MARIADB_SYM:         return OTHERS_ORACLE_SYM;
      case PACKAGE_MARIADB_SYM:        return PACKAGE_ORACLE_SYM;
      case RAISE_MARIADB_SYM:          return RAISE_ORACLE_SYM;
      case RAW_MARIADB_SYM:            return RAW_ORACLE_SYM;
      case RETURN_MARIADB_SYM:         return RETURN_ORACLE_SYM;
      case ROWTYPE_MARIADB_SYM:        return ROWTYPE_ORACLE_SYM;
      case VARCHAR2_MARIADB_SYM:       return VARCHAR2_ORACLE_SYM;
      }
    }

    if ((symbol->tok == NOT_SYM) &&
        (m_thd->variables.sql_mode & MODE_HIGH_NOT_PRECEDENCE))
      return NOT2_SYM;
    if ((symbol->tok == OR2_SYM) &&
        (m_thd->variables.sql_mode & MODE_PIPES_AS_CONCAT))
    {
      return (m_thd->variables.sql_mode & MODE_ORACLE) ?
             ORACLE_CONCAT_SYM : MYSQL_CONCAT_SYM;
    }

    return symbol->tok;
  }
  return 0;
}

/*
  Check if name is a keyword

  SYNOPSIS
    is_keyword()
    name      checked name (must not be empty)
    len       length of checked name

  RETURN VALUES
    0         name is a keyword
    1         name isn't a keyword
*/

bool is_keyword(const char *name, uint len)
{
  DBUG_ASSERT(len != 0);
  return get_hash_symbol(name,len,0)!=0;
}

/**
  Check if name is a sql function

    @param name      checked name

    @return is this a native function or not
    @retval 0         name is a function
    @retval 1         name isn't a function
*/

bool is_lex_native_function(const LEX_CSTRING *name)
{
  DBUG_ASSERT(name != NULL);
  return (get_hash_symbol(name->str, (uint) name->length, 1) != 0);
}


bool is_native_function(THD *thd, const LEX_CSTRING *name)
{
  if (find_native_function_builder(thd, name))
    return true;

  if (is_lex_native_function(name))
    return true;

  if (Type_handler::handler_by_name(thd, *name))
    return true;

  return false;
}


bool is_native_function_with_warn(THD *thd, const LEX_CSTRING *name)
{
  if (!is_native_function(thd, name))
    return false;
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
                      name->str);
  return true;
}


/* make a copy of token before ptr and set yytoklen */

LEX_CSTRING Lex_input_stream::get_token(uint skip, uint length)
{
  LEX_CSTRING tmp;
  yyUnget();                       // ptr points now after last token char
  tmp.length= length;
  tmp.str= m_thd->strmake(m_tok_start + skip, tmp.length);

  m_cpp_text_start= m_cpp_tok_start + skip;
  m_cpp_text_end= m_cpp_text_start + tmp.length;

  return tmp;
}


static size_t
my_unescape(CHARSET_INFO *cs, char *to, const char *str, const char *end,
            int sep, bool backslash_escapes)
{
  char *start= to;
  for ( ; str != end ; str++)
  {
#ifdef USE_MB
    int l;
    if (cs->use_mb() && (l= my_ismbchar(cs, str, end)))
    {
      while (l--)
        *to++ = *str++;
      str--;
      continue;
    }
#endif
    if (backslash_escapes && *str == '\\' && str + 1 != end)
    {
      switch(*++str) {
      case 'n':
        *to++='\n';
        break;
      case 't':
        *to++= '\t';
        break;
      case 'r':
        *to++ = '\r';
        break;
      case 'b':
        *to++ = '\b';
        break;
      case '0':
        *to++= 0;                      // Ascii null
        break;
      case 'Z':                        // ^Z must be escaped on Win32
        *to++='\032';
        break;
      case '_':
      case '%':
        *to++= '\\';                   // remember prefix for wildcard
        /* Fall through */
      default:
        *to++= *str;
        break;
      }
    }
    else if (*str == sep)
      *to++= *str++;                // Two ' or "
    else
      *to++ = *str;
  }
  *to= 0;
  return to - start;
}


size_t
Lex_input_stream::unescape(CHARSET_INFO *cs, char *to,
                           const char *str, const char *end,
                           int sep)
{
  return my_unescape(cs, to, str, end, sep, m_thd->backslash_escapes());
}


/*
  Return an unescaped text literal without quotes
  Fix sometimes to do only one scan of the string
*/

bool Lex_input_stream::get_text(Lex_string_with_metadata_st *dst, uint sep,
                                int pre_skip, int post_skip)
{
  uchar c;
  uint found_escape=0;
  CHARSET_INFO *cs= m_thd->charset();
  bool is_8bit= false;

  while (! eof())
  {
    c= yyGet();
    if (c & 0x80)
      is_8bit= true;
#ifdef USE_MB
    {
      int l;
      if (cs->use_mb() &&
          (l = my_ismbchar(cs,
                           get_ptr() -1,
                           get_end_of_query()))) {
        skip_binary(l-1);
        continue;
      }
    }
#endif
    if (c == '\\' &&
        !(m_thd->variables.sql_mode & MODE_NO_BACKSLASH_ESCAPES))
    {                                        // Escaped character
      found_escape=1;
      if (eof())
        return true;
      yySkip();
    }
    else if (c == sep)
    {
      if (c == yyGet())                 // Check if two separators in a row
      {
        found_escape=1;                 // duplicate. Remember for delete
        continue;
      }
      else
        yyUnget();

      /* Found end. Unescape and return string */
      const char *str, *end;
      char *to;

      str= m_tok_start;
      end= get_ptr();
      /* Extract the text from the token */
      str += pre_skip;
      end -= post_skip;
      DBUG_ASSERT(end >= str);

      if (!(to= (char*) m_thd->alloc((uint) (end - str) + 1)))
      {
        dst->set(&empty_clex_str, 0, '\0');
        return true;                   // Sql_alloc has set error flag
      }

      m_cpp_text_start= m_cpp_tok_start + pre_skip;
      m_cpp_text_end= get_cpp_ptr() - post_skip;

      if (!found_escape)
      {
        size_t len= (end - str);
        memcpy(to, str, len);
        to[len]= '\0';
        dst->set(to, len, is_8bit, '\0');
      }
      else
      {
        size_t len= unescape(cs, to, str, end, sep);
        dst->set(to, len, is_8bit, '\0');
      }
      return false;
    }
  }
  return true;                         // unexpected end of query
}


/*
** Calc type of integer; long integer, longlong integer or real.
** Returns smallest type that match the string.
** When using unsigned long long values the result is converted to a real
** because else they will be unexpected sign changes because all calculation
** is done with longlong or double.
*/

static const char *long_str="2147483647";
static const uint long_len=10;
static const char *signed_long_str="-2147483648";
static const char *longlong_str="9223372036854775807";
static const uint longlong_len=19;
static const char *signed_longlong_str="-9223372036854775808";
static const uint signed_longlong_len=19;
static const char *unsigned_longlong_str="18446744073709551615";
static const uint unsigned_longlong_len=20;

static inline uint int_token(const char *str,uint length)
{
  if (length < long_len)                        // quick normal case
    return NUM;
  bool neg=0;

  if (*str == '+')                              // Remove sign and pre-zeros
  {
    str++; length--;
  }
  else if (*str == '-')
  {
    str++; length--;
    neg=1;
  }
  while (*str == '0' && length)
  {
    str++; length --;
  }
  if (length < long_len)
    return NUM;

  uint smaller,bigger;
  const char *cmp;
  if (neg)
  {
    if (length == long_len)
    {
      cmp= signed_long_str + 1;
      smaller= NUM;                                   // If <= signed_long_str
      bigger= LONG_NUM;                               // If >= signed_long_str
    }
    else if (length < signed_longlong_len)
      return LONG_NUM;
    else if (length > signed_longlong_len)
      return DECIMAL_NUM;
    else
    {
      cmp= signed_longlong_str + 1;
      smaller= LONG_NUM;                              // If <= signed_longlong_str
      bigger=DECIMAL_NUM;
    }
  }
  else
  {
    if (length == long_len)
    {
      cmp= long_str;
      smaller=NUM;
      bigger=LONG_NUM;
    }
    else if (length < longlong_len)
      return LONG_NUM;
    else if (length > longlong_len)
    {
      if (length > unsigned_longlong_len)
        return DECIMAL_NUM;
      cmp=unsigned_longlong_str;
      smaller=ULONGLONG_NUM;
      bigger=DECIMAL_NUM;
    }
    else
    {
      cmp=longlong_str;
      smaller=LONG_NUM;
      bigger= ULONGLONG_NUM;
    }
  }
  while (*cmp && *cmp++ == *str++) ;
  return ((uchar) str[-1] <= (uchar) cmp[-1]) ? smaller : bigger;
}


/**
  Given a stream that is advanced to the first contained character in 
  an open comment, consume the comment.  Optionally, if we are allowed, 
  recurse so that we understand comments within this current comment.

  At this level, we do not support version-condition comments.  We might 
  have been called with having just passed one in the stream, though.  In 
  that case, we probably want to tolerate mundane comments inside.  Thus,
  the case for recursion.

  @retval  Whether EOF reached before comment is closed.
*/
bool Lex_input_stream::consume_comment(int remaining_recursions_permitted)
{
  // only one level of nested comments are allowed
  DBUG_ASSERT(remaining_recursions_permitted == 0 ||
              remaining_recursions_permitted == 1);
  uchar c;
  while (!eof())
  {
    c= yyGet();

    if (remaining_recursions_permitted == 1)
    {
      if ((c == '/') && (yyPeek() == '*'))
      {
        yyUnput('(');  // Replace nested "/*..." with "(*..."
        yySkip();      // and skip "("

        yySkip(); /* Eat asterisk */
        if (consume_comment(0))
          return true;

        yyUnput(')');  // Replace "...*/" with "...*)"
        yySkip();      // and skip ")"
        continue;
      }
    }

    if (c == '*')
    {
      if (yyPeek() == '/')
      {
        yySkip(); // Eat slash
        return FALSE;
      }
    }

    if (c == '\n')
      yylineno++;
  }

  return TRUE;
}


/*
  MYSQLlex remember the following states from the following MYSQLlex()

  @param yylval         [out]  semantic value of the token being parsed (yylval)
  @param thd            THD

  - MY_LEX_EOQ                  Found end of query
  - MY_LEX_OPERATOR_OR_IDENT    Last state was an ident, text or number
                                (which can't be followed by a signed number)
*/

int MYSQLlex(YYSTYPE *yylval, THD *thd)
{
  return thd->m_parser_state->m_lip.lex_token(yylval, thd);
}


int ORAlex(YYSTYPE *yylval, THD *thd)
{
  return thd->m_parser_state->m_lip.lex_token(yylval, thd);
}


int Lex_input_stream::lex_token(YYSTYPE *yylval, THD *thd)
{
  int token;
  const int left_paren= (int) '(';

  if (lookahead_token >= 0)
  {
    /*
      The next token was already parsed in advance,
      return it.
    */
    token= lookahead_token;
    lookahead_token= -1;
    *yylval= *(lookahead_yylval);
    lookahead_yylval= NULL;
    return token;
  }

  token= lex_one_token(yylval, thd);
  add_digest_token(token, yylval);

  SELECT_LEX *curr_sel= thd->lex->current_select;

  switch(token) {
  case WITH:
    /*
      Parsing 'WITH' 'ROLLUP' or 'WITH' 'CUBE' requires 2 look ups,
      which makes the grammar LALR(2).
      Replace by a single 'WITH_ROLLUP' or 'WITH_CUBE' token,
      to transform the grammar into a LALR(1) grammar,
      which sql_yacc.yy can process.
    */
    token= lex_one_token(yylval, thd);
    add_digest_token(token, yylval);
    switch(token) {
    case CUBE_SYM:
      return WITH_CUBE_SYM;
    case ROLLUP_SYM:
      return WITH_ROLLUP_SYM;
    case SYSTEM:
      return WITH_SYSTEM_SYM;
    default:
      /*
        Save the token following 'WITH'
      */
      lookahead_yylval= yylval;
      lookahead_token= token;
      return WITH;
    }
    break;
  case FOR_SYM:
    /*
     * Additional look-ahead to resolve doubtful cases like:
     * SELECT ... FOR UPDATE
     * SELECT ... FOR SYSTEM_TIME ... .
     */
    token= lex_one_token(yylval, thd);
    add_digest_token(token, yylval);
    switch(token) {
    case SYSTEM_TIME_SYM:
      return FOR_SYSTEM_TIME_SYM;
    default:
      /*
        Save the token following 'FOR_SYM'
      */
      lookahead_yylval= yylval;
      lookahead_token= token;
      return FOR_SYM;
    }
    break;
  case VALUES:
    if (curr_sel &&
        (curr_sel->parsing_place == BEFORE_OPT_LIST ||
         curr_sel->parsing_place == AFTER_LIST))
    {
      curr_sel->parsing_place= NO_MATTER;
      break;
    }
    if (curr_sel &&
        (curr_sel->parsing_place == IN_UPDATE_ON_DUP_KEY ||
         curr_sel->parsing_place == IN_PART_FUNC))
      return VALUE_SYM;
    token= lex_one_token(yylval, thd);
    add_digest_token(token, yylval);
    switch(token) {
    case LESS_SYM:
      return VALUES_LESS_SYM;
    case IN_SYM:
      return VALUES_IN_SYM;
    default:
      lookahead_yylval= yylval;
      lookahead_token= token;
      return VALUES;
    }
  case VALUE_SYM:
    if (curr_sel &&
        (curr_sel->parsing_place == BEFORE_OPT_LIST ||
         curr_sel->parsing_place == AFTER_LIST))
    {
      curr_sel->parsing_place= NO_MATTER;
      return VALUES;
    }
    break;
  case PARTITION_SYM:
  case SELECT_SYM:
  case UNION_SYM:
    if (curr_sel &&
        (curr_sel->parsing_place == BEFORE_OPT_LIST ||
         curr_sel->parsing_place == AFTER_LIST))
    {
      curr_sel->parsing_place= NO_MATTER;
    }
    break;
  case left_paren:
    if (!curr_sel ||
        curr_sel->parsing_place != BEFORE_OPT_LIST)
      return token;
    token= lex_one_token(yylval, thd);
    add_digest_token(token, yylval);
    lookahead_yylval= yylval;
    yylval= NULL;
    lookahead_token= token;
    curr_sel->parsing_place= NO_MATTER;
    if (token == LIKE)
      return LEFT_PAREN_LIKE;
    if (token == WITH)
      return LEFT_PAREN_WITH;
    if (token != left_paren && token != SELECT_SYM && token != VALUES)
      return LEFT_PAREN_ALT;
    else
      return left_paren;
    break;
  default:
    break;
  }
  return token;
}


int Lex_input_stream::lex_one_token(YYSTYPE *yylval, THD *thd)
{
  uchar UNINIT_VAR(c);
  bool comment_closed;
  int tokval;
  uint length;
  enum my_lex_states state;
  LEX *lex= thd->lex;
  CHARSET_INFO *const cs= thd->charset();
  const uchar *const state_map= cs->state_map;
  const uchar *const ident_map= cs->ident_map;

  start_token();
  state= next_state;
  next_state= MY_LEX_OPERATOR_OR_IDENT;
  for (;;)
  {
    switch (state) {
    case MY_LEX_OPERATOR_OR_IDENT:        // Next is operator or keyword
    case MY_LEX_START:                    // Start of token
      // Skip starting whitespace
      while(state_map[c= yyPeek()] == MY_LEX_SKIP)
      {
        if (c == '\n')
          yylineno++;

        yySkip();
      }

      /* Start of real token */
      restart_token();
      c= yyGet();
      state= (enum my_lex_states) state_map[c];
      break;
    case MY_LEX_ESCAPE:
      if (!eof() && yyGet() == 'N')
      {                                        // Allow \N as shortcut for NULL
        yylval->lex_str.str= (char*) "\\N";
        yylval->lex_str.length= 2;
        return NULL_SYM;
      }
      /* Fall through */
    case MY_LEX_CHAR:                          // Unknown or single char token
      if (c == '%' && (m_thd->variables.sql_mode & MODE_ORACLE))
      {
        next_state= MY_LEX_START;
        return PERCENT_ORACLE_SYM;
      }
      if (c == '[' && (m_thd->variables.sql_mode & MODE_MSSQL))
        return scan_ident_delimited(thd, &yylval->ident_cli, ']');
      /* Fall through */
    case MY_LEX_SKIP:                          // This should not happen
      if (c != ')')
        next_state= MY_LEX_START;         // Allow signed numbers
      yylval->kwd.set_keyword(m_tok_start, 1);
      return((int) c);

    case MY_LEX_MINUS_OR_COMMENT:
      if (yyPeek() == '-' &&
          (my_isspace(cs,yyPeekn(1)) ||
           my_iscntrl(cs,yyPeekn(1))))
      {
        state=MY_LEX_COMMENT;
        break;
      }
      next_state= MY_LEX_START;        // Allow signed numbers
      return((int) c);

    case MY_LEX_PLACEHOLDER:
      /*
        Check for a placeholder: it should not precede a possible identifier
        because of binlogging: when a placeholder is replaced with
        its value in a query for the binlog, the query must stay
        grammatically correct.
      */
      next_state= MY_LEX_START;        // Allow signed numbers
      if (stmt_prepare_mode && !ident_map[(uchar) yyPeek()])
        return(PARAM_MARKER);
      return((int) c);

    case MY_LEX_COMMA:
      next_state= MY_LEX_START;        // Allow signed numbers
      /*
        Warning:
        This is a work around, to make the "remember_name" rule in
        sql/sql_yacc.yy work properly.
        The problem is that, when parsing "select expr1, expr2",
        the code generated by bison executes the *pre* action
        remember_name (see select_item) *before* actually parsing the
        first token of expr2.
      */
      restart_token();
      return((int) c);

    case MY_LEX_IDENT_OR_NCHAR:
    {
      uint sep;
      if (yyPeek() != '\'')
      {
        state= MY_LEX_IDENT;
        break;
      }
      /* Found N'string' */
      yySkip();                         // Skip '
      if (get_text(&yylval->lex_string_with_metadata, (sep= yyGetLast()), 2, 1))
      {
        state= MY_LEX_CHAR;                    // Read char by char
        break;
      }

      body_utf8_append(m_cpp_text_start);
      body_utf8_append_escape(thd, &yylval->lex_string_with_metadata,
                                   national_charset_info,
                                   m_cpp_text_end, sep);
      return(NCHAR_STRING);
    }
    case MY_LEX_IDENT_OR_HEX:
      if (yyPeek() == '\'')
      {                                      // Found x'hex-number'
        state= MY_LEX_HEX_NUMBER;
        break;
      }
      /* fall through */
    case MY_LEX_IDENT_OR_BIN:
      if (yyPeek() == '\'')
      {                                 // Found b'bin-number'
        state= MY_LEX_BIN_NUMBER;
        break;
      }
      /* fall through */
    case MY_LEX_IDENT:
    {
      tokval= scan_ident_middle(thd, &yylval->ident_cli,
                                &yylval->charset, &state);
      if (!tokval)
        continue;
      if (tokval == UNDERSCORE_CHARSET)
        m_underscore_cs= yylval->charset;
      return tokval;
    }

    case MY_LEX_IDENT_SEP:                  // Found ident and now '.'
      yylval->lex_str.str= (char*) get_ptr();
      yylval->lex_str.length= 1;
      c= yyGet();                          // should be '.'
      next_state= MY_LEX_IDENT_START;      // Next is ident (not keyword)
      if (!ident_map[(uchar) yyPeek()])    // Probably ` or "
        next_state= MY_LEX_START;
      return((int) c);

    case MY_LEX_NUMBER_IDENT:                   // number or ident which num-start
      if (yyGetLast() == '0')
      {
        c= yyGet();
        if (c == 'x')
        {
          while (my_isxdigit(cs, (c = yyGet()))) ;
          if ((yyLength() >= 3) && !ident_map[c])
          {
            /* skip '0x' */
            yylval->lex_str= get_token(2, yyLength() - 2);
            return (HEX_NUM);
          }
          yyUnget();
          state= MY_LEX_IDENT_START;
          break;
        }
        else if (c == 'b')
        {
          while ((c= yyGet()) == '0' || c == '1')
            ;
          if ((yyLength() >= 3) && !ident_map[c])
          {
            /* Skip '0b' */
            yylval->lex_str= get_token(2, yyLength() - 2);
            return (BIN_NUM);
          }
          yyUnget();
          state= MY_LEX_IDENT_START;
          break;
        }
        yyUnget();
      }

      while (my_isdigit(cs, (c= yyGet()))) ;
      if (!ident_map[c])
      {                                        // Can't be identifier
        state=MY_LEX_INT_OR_REAL;
        break;
      }
      if (c == 'e' || c == 'E')
      {
        // The following test is written this way to allow numbers of type 1e1
        if (my_isdigit(cs, yyPeek()) ||
            (c=(yyGet())) == '+' || c == '-')
        {                                       // Allow 1E+10
          if (my_isdigit(cs, yyPeek()))         // Number must have digit after sign
          {
            yySkip();
            while (my_isdigit(cs, yyGet())) ;
            yylval->lex_str= get_token(0, yyLength());
            return(FLOAT_NUM);
          }
        }
        /*
          We've found:
          - A sequence of digits
          - Followed by 'e' or 'E'
          - Followed by some byte XX which is not a known mantissa start,
            and it's known to be a valid identifier part.
            XX can be either a 8bit identifier character, or a multi-byte head.
        */
        yyUnget();
        return scan_ident_start(thd, &yylval->ident_cli);
      }
      /*
        We've found:
        - A sequence of digits
        - Followed by some character XX, which is neither 'e' nor 'E',
          and it's known to be a valid identifier part.
          XX can be a 8bit identifier character, or a multi-byte head.
      */
      yyUnget();
      return scan_ident_start(thd, &yylval->ident_cli);

    case MY_LEX_IDENT_START:                    // We come here after '.'
      return scan_ident_start(thd, &yylval->ident_cli);

    case MY_LEX_USER_VARIABLE_DELIMITER:        // Found quote char
      return scan_ident_delimited(thd, &yylval->ident_cli, m_tok_start[0]);

    case MY_LEX_INT_OR_REAL:                    // Complete int or incomplete real
      if (c != '.' || yyPeek() == '.')
      {
        /*
          Found a complete integer number:
          - the number is either not followed by a dot at all, or
          - the number is followed by a double dot as in: FOR i IN 1..10
        */
        yylval->lex_str= get_token(0, yyLength());
        return int_token(yylval->lex_str.str, (uint) yylval->lex_str.length);
      }
      // fall through
    case MY_LEX_REAL:                           // Incomplete real number
      while (my_isdigit(cs, c= yyGet())) ;

      if (c == 'e' || c == 'E')
      {
        c= yyGet();
        if (c == '-' || c == '+')
          c= yyGet();                           // Skip sign
        if (!my_isdigit(cs, c))
        {                                       // No digit after sign
          state= MY_LEX_CHAR;
          break;
        }
        while (my_isdigit(cs, yyGet())) ;
        yylval->lex_str= get_token(0, yyLength());
        return(FLOAT_NUM);
      }
      yylval->lex_str= get_token(0, yyLength());
      return(DECIMAL_NUM);

    case MY_LEX_HEX_NUMBER:             // Found x'hexstring'
      yySkip();                    // Accept opening '
      while (my_isxdigit(cs, (c= yyGet()))) ;
      if (c != '\'')
        return(ABORT_SYM);              // Illegal hex constant
      yySkip();                    // Accept closing '
      length= yyLength();          // Length of hexnum+3
      if ((length % 2) == 0)
        return(ABORT_SYM);              // odd number of hex digits
      yylval->lex_str= get_token(2,            // skip x'
                                 length - 3);  // don't count x' and last '
      return HEX_STRING;

    case MY_LEX_BIN_NUMBER:           // Found b'bin-string'
      yySkip();                  // Accept opening '
      while ((c= yyGet()) == '0' || c == '1')
        ;
      if (c != '\'')
        return(ABORT_SYM);            // Illegal hex constant
      yySkip();                  // Accept closing '
      length= yyLength();        // Length of bin-num + 3
      yylval->lex_str= get_token(2,           // skip b'
                                 length - 3); // don't count b' and last '
      return (BIN_NUM);

    case MY_LEX_CMP_OP:                     // Incomplete comparison operator
      next_state= MY_LEX_START;        // Allow signed numbers
      if (state_map[(uchar) yyPeek()] == MY_LEX_CMP_OP ||
          state_map[(uchar) yyPeek()] == MY_LEX_LONG_CMP_OP)
      {
        yySkip();
        if ((tokval= find_keyword(&yylval->kwd, 2, 0)))
          return(tokval);
        yyUnget();
      }
      return(c);

    case MY_LEX_LONG_CMP_OP:                // Incomplete comparison operator
      next_state= MY_LEX_START;
      if (state_map[(uchar) yyPeek()] == MY_LEX_CMP_OP ||
          state_map[(uchar) yyPeek()] == MY_LEX_LONG_CMP_OP)
      {
        yySkip();
        if (state_map[(uchar) yyPeek()] == MY_LEX_CMP_OP)
        {
          yySkip();
          if ((tokval= find_keyword(&yylval->kwd, 3, 0)))
            return(tokval);
          yyUnget();
        }
        if ((tokval= find_keyword(&yylval->kwd, 2, 0)))
          return(tokval);
        yyUnget();
      }
      return(c);

    case MY_LEX_BOOL:
      if (c != yyPeek())
      {
        state= MY_LEX_CHAR;
        break;
      }
      yySkip();
      tokval= find_keyword(&yylval->kwd, 2, 0);  // Is a bool operator
      next_state= MY_LEX_START;                  // Allow signed numbers
      return(tokval);

    case MY_LEX_STRING_OR_DELIMITER:
      if (thd->variables.sql_mode & MODE_ANSI_QUOTES)
      {
        state= MY_LEX_USER_VARIABLE_DELIMITER;
        break;
      }
      /* " used for strings */
      /* fall through */
    case MY_LEX_STRING:                        // Incomplete text string
    {
      uint sep;
      if (get_text(&yylval->lex_string_with_metadata, (sep= yyGetLast()), 1, 1))
      {
        state= MY_LEX_CHAR;                     // Read char by char
        break;
      }
      CHARSET_INFO *strcs= m_underscore_cs ? m_underscore_cs : cs;
      body_utf8_append(m_cpp_text_start);

      body_utf8_append_escape(thd, &yylval->lex_string_with_metadata,
                                   strcs, m_cpp_text_end, sep);
      m_underscore_cs= NULL;
      return(TEXT_STRING);
    }
    case MY_LEX_COMMENT:                       //  Comment
      lex->lex_options|= OPTION_LEX_FOUND_COMMENT;
      while ((c= yyGet()) != '\n' && c) ;
      yyUnget();                          // Safety against eof
      state= MY_LEX_START;                     // Try again
      break;
    case MY_LEX_LONG_COMMENT:                  // Long C comment?
      if (yyPeek() != '*')
      {
        state= MY_LEX_CHAR;                     // Probable division
        break;
      }
      lex->lex_options|= OPTION_LEX_FOUND_COMMENT;
      /* Reject '/' '*', since we might need to turn off the echo */
      yyUnget();

      save_in_comment_state();

      if (yyPeekn(2) == '!' ||
          (yyPeekn(2) == 'M' && yyPeekn(3) == '!'))
      {
        bool maria_comment_syntax= yyPeekn(2) == 'M';
        in_comment= DISCARD_COMMENT;
        /* Accept '/' '*' '!', but do not keep this marker. */
        set_echo(FALSE);
        yySkipn(maria_comment_syntax ? 4 : 3);

        /*
          The special comment format is very strict:
          '/' '*' '!', followed by an optional 'M' and exactly
          1-2 digits (major), 2 digits (minor), then 2 digits (dot).
          32302  -> 3.23.02
          50032  -> 5.0.32
          50114  -> 5.1.14
          100000 -> 10.0.0
        */
        if (  my_isdigit(cs, yyPeekn(0))
           && my_isdigit(cs, yyPeekn(1))
           && my_isdigit(cs, yyPeekn(2))
           && my_isdigit(cs, yyPeekn(3))
           && my_isdigit(cs, yyPeekn(4))
           )
        {
          ulong version;
          uint length= 5;
          char *end_ptr= (char*) get_ptr() + length;
          int error;
          if (my_isdigit(cs, yyPeekn(5)))
          {
            end_ptr++;                          // 6 digit number
            length++;
          }

          version= (ulong) my_strtoll10(get_ptr(), &end_ptr, &error);

          /*
            MySQL-5.7 has new features and might have new SQL syntax that
            MariaDB-10.0 does not understand. Ignore all versioned comments
            with MySQL versions in the range 50700-999999, but
            do not ignore MariaDB specific comments for the same versions.
          */ 
          if (version <= MYSQL_VERSION_ID &&
              (version < 50700 || version > 99999 || maria_comment_syntax))
          {
            /* Accept 'M' 'm' 'm' 'd' 'd' */
            yySkipn(length);
            /* Expand the content of the special comment as real code */
            set_echo(TRUE);
            state=MY_LEX_START;
            break;  /* Do not treat contents as a comment.  */
          }
          else
          {
#ifdef WITH_WSREP
            if (WSREP(thd) && version == 99997 && wsrep_thd_is_local(thd))
            {
              WSREP_DEBUG("consistency check: %s", thd->query());
              thd->wsrep_consistency_check= CONSISTENCY_CHECK_DECLARED;
              yySkipn(5);
              set_echo(TRUE);
              state= MY_LEX_START;
              break;  /* Do not treat contents as a comment.  */
            }
#endif /* WITH_WSREP */
            /*
              Patch and skip the conditional comment to avoid it
              being propagated infinitely (eg. to a slave).
            */
            char *pcom= yyUnput(' ');
            comment_closed= ! consume_comment(1);
            if (! comment_closed)
            {
              *pcom= '!';
            }
            /* version allowed to have one level of comment inside. */
          }
        }
        else
        {
          /* Not a version comment. */
          state=MY_LEX_START;
          set_echo(TRUE);
          break;
        }
      }
      else
      {
        in_comment= PRESERVE_COMMENT;
        yySkip();                  // Accept /
        yySkip();                  // Accept *
        comment_closed= ! consume_comment(0);
        /* regular comments can have zero comments inside. */
      }
      /*
        Discard:
        - regular '/' '*' comments,
        - special comments '/' '*' '!' for a future version,
        by scanning until we find a closing '*' '/' marker.

        Nesting regular comments isn't allowed.  The first 
        '*' '/' returns the parser to the previous state.

        /#!VERSI oned containing /# regular #/ is allowed #/

                Inside one versioned comment, another versioned comment
                is treated as a regular discardable comment.  It gets
                no special parsing.
      */

      /* Unbalanced comments with a missing '*' '/' are a syntax error */
      if (! comment_closed)
        return (ABORT_SYM);
      state = MY_LEX_START;             // Try again
      restore_in_comment_state();
      break;
    case MY_LEX_END_LONG_COMMENT:
      if ((in_comment != NO_COMMENT) && yyPeek() == '/')
      {
        /* Reject '*' '/' */
        yyUnget();
        /* Accept '*' '/', with the proper echo */
        set_echo(in_comment == PRESERVE_COMMENT);
        yySkipn(2);
        /* And start recording the tokens again */
        set_echo(TRUE);
        in_comment= NO_COMMENT;
        state=MY_LEX_START;
      }
      else
        state= MY_LEX_CHAR;              // Return '*'
      break;
    case MY_LEX_SET_VAR:                // Check if ':='
      if (yyPeek() != '=')
      {
        next_state= MY_LEX_START;
        if (m_thd->variables.sql_mode & MODE_ORACLE)
        {
          yylval->kwd.set_keyword(m_tok_start, 1);
          return COLON_ORACLE_SYM;
        }
        return (int) ':';
      }
      yySkip();
      return (SET_VAR);
    case MY_LEX_SEMICOLON:              // optional line terminator
      state= MY_LEX_CHAR;               // Return ';'
      break;
    case MY_LEX_EOL:
      if (eof())
      {
        yyUnget();                 // Reject the last '\0'
        set_echo(FALSE);
        yySkip();
        set_echo(TRUE);
        /* Unbalanced comments with a missing '*' '/' are a syntax error */
        if (in_comment != NO_COMMENT)
          return (ABORT_SYM);
        next_state= MY_LEX_END;     // Mark for next loop
        return(END_OF_INPUT);
      }
      state=MY_LEX_CHAR;
      break;
    case MY_LEX_END:
      next_state= MY_LEX_END;
      return(0);                        // We found end of input last time

      /* Actually real shouldn't start with . but allow them anyhow */
    case MY_LEX_REAL_OR_POINT:
      if (my_isdigit(cs, (c= yyPeek())))
        state = MY_LEX_REAL;            // Real
      else if (c == '.')
      {
        yySkip();
        return DOT_DOT_SYM;
      }
      else
      {
        state= MY_LEX_IDENT_SEP;        // return '.'
        yyUnget();                 // Put back '.'
      }
      break;
    case MY_LEX_USER_END:               // end '@' of user@hostname
      switch (state_map[(uchar) yyPeek()]) {
      case MY_LEX_STRING:
      case MY_LEX_USER_VARIABLE_DELIMITER:
      case MY_LEX_STRING_OR_DELIMITER:
        break;
      case MY_LEX_USER_END:
        next_state= MY_LEX_SYSTEM_VAR;
        break;
      default:
        next_state= MY_LEX_HOSTNAME;
        break;
      }
      yylval->lex_str.str= (char*) get_ptr() - 1;
      yylval->lex_str.length= 1;
      return((int) '@');
    case MY_LEX_HOSTNAME:               // end '@' of user@hostname
      for (c= yyGet() ;
           my_isalnum(cs, c) || c == '.' || c == '_' ||  c == '$';
           c= yyGet()) ;
      yylval->lex_str= get_token(0, yyLength());
      return(LEX_HOSTNAME);
    case MY_LEX_SYSTEM_VAR:
      yylval->lex_str.str= (char*) get_ptr();
      yylval->lex_str.length= 1;
      yySkip();                                    // Skip '@'
      next_state= (state_map[(uchar) yyPeek()] ==
                        MY_LEX_USER_VARIABLE_DELIMITER ?
                        MY_LEX_OPERATOR_OR_IDENT :
                        MY_LEX_IDENT_OR_KEYWORD);
      return((int) '@');
    case MY_LEX_IDENT_OR_KEYWORD:
      /*
        We come here when we have found two '@' in a row.
        We should now be able to handle:
        [(global | local | session) .]variable_name
      */
      return scan_ident_sysvar(thd, &yylval->ident_cli);
    }
  }
}


bool Lex_input_stream::get_7bit_or_8bit_ident(THD *thd, uchar *last_char)
{
  uchar c;
  CHARSET_INFO *const cs= thd->charset();
  const uchar *const ident_map= cs->ident_map;
  bool is_8bit= false;
  for ( ; ident_map[c= yyGet()]; )
  {
    if (c & 0x80)
      is_8bit= true; // will convert
  }
  *last_char= c;
  return is_8bit;
}


int Lex_input_stream::scan_ident_sysvar(THD *thd, Lex_ident_cli_st *str)
{
  uchar last_char;
  uint length;
  int tokval;
  bool is_8bit;
  DBUG_ASSERT(m_tok_start == m_ptr);

  is_8bit= get_7bit_or_8bit_ident(thd, &last_char);

  if (last_char == '.')
    next_state= MY_LEX_IDENT_SEP;
  if (!(length= yyLength()))
    return ABORT_SYM;                  // Names must be nonempty.
  if ((tokval= find_keyword(str, length, 0)))
  {
    yyUnget();                         // Put back 'c'
    return tokval;                     // Was keyword
  }

  yyUnget();                       // ptr points now after last token char
  str->set_ident(m_tok_start, length, is_8bit);

  m_cpp_text_start= m_cpp_tok_start;
  m_cpp_text_end= m_cpp_text_start + length;
  body_utf8_append(m_cpp_text_start);
  body_utf8_append_ident(thd, str, m_cpp_text_end);

  return is_8bit ? IDENT_QUOTED : IDENT;
}


/*
  We can come here if different parsing stages:
  - In an identifier chain:
       SELECT t1.cccc FROM t1;
    (when the "cccc" part starts)
    In this case both m_tok_start and m_ptr point to "cccc".
  - When a sequence of digits has changed to something else,
    therefore the token becomes an identifier rather than a number:
       SELECT 12345_6 FROM t1;
    In this case m_tok_start points to the entire "12345_678",
    while m_ptr points to "678".
*/
int Lex_input_stream::scan_ident_start(THD *thd, Lex_ident_cli_st *str)
{
  uchar c;
  bool is_8bit;
  CHARSET_INFO *const cs= thd->charset();
  const uchar *const ident_map= cs->ident_map;
  DBUG_ASSERT(m_tok_start <= m_ptr);

  if (cs->use_mb())
  {
    is_8bit= true;
    while (ident_map[c= yyGet()])
    {
      int char_length= cs->charlen(get_ptr() - 1, get_end_of_query());
      if (char_length <= 0)
        break;
      skip_binary(char_length - 1);
    }
  }
  else
  {
    is_8bit= get_7bit_or_8bit_ident(thd, &c);
  }
  if (c == '.' && ident_map[(uchar) yyPeek()])
    next_state= MY_LEX_IDENT_SEP;// Next is '.'

  uint length= yyLength();
  yyUnget(); // ptr points now after last token char
  str->set_ident(m_tok_start, length, is_8bit);
  m_cpp_text_start= m_cpp_tok_start;
  m_cpp_text_end= m_cpp_text_start + length;
  body_utf8_append(m_cpp_text_start);
  body_utf8_append_ident(thd, str, m_cpp_text_end);
  return is_8bit ? IDENT_QUOTED : IDENT;
}


int Lex_input_stream::scan_ident_middle(THD *thd, Lex_ident_cli_st *str,
                                        CHARSET_INFO **introducer,
                                        my_lex_states *st)
{
  CHARSET_INFO *const cs= thd->charset();
  const uchar *const ident_map= cs->ident_map;
  const uchar *const state_map= cs->state_map;
  const char *start;
  uint length;
  uchar c;
  bool is_8bit;
  bool resolve_introducer= true;
  DBUG_ASSERT(m_ptr == m_tok_start + 1); // m_ptr points to the second byte

  if (cs->use_mb())
  {
    is_8bit= true;
    int char_length= cs->charlen(get_ptr() - 1, get_end_of_query());
    if (char_length <= 0)
    {
      *st= MY_LEX_CHAR;
      return 0;
    }
    skip_binary(char_length - 1);

    while (ident_map[c= yyGet()])
    {
      char_length= cs->charlen(get_ptr() - 1, get_end_of_query());
      if (char_length <= 0)
        break;
      if (char_length > 1 || (c & 0x80))
        resolve_introducer= false;
      skip_binary(char_length - 1);
    }
  }
  else
  {
    is_8bit= get_7bit_or_8bit_ident(thd, &c) || (m_tok_start[0] & 0x80);
    resolve_introducer= !is_8bit;
  }
  length= yyLength();
  start= get_ptr();
  if (ignore_space)
  {
    /*
      If we find a space then this can't be an identifier. We notice this
      below by checking start != lex->ptr.
    */
    for (; state_map[(uchar) c] == MY_LEX_SKIP ; c= yyGet())
    {
      if (c == '\n')
        yylineno++;
    }
  }
  if (start == get_ptr() && c == '.' && ident_map[(uchar) yyPeek()])
    next_state= MY_LEX_IDENT_SEP;
  else
  {                                    // '(' must follow directly if function
    int tokval;
    yyUnget();
    if ((tokval= find_keyword(str, length, c == '(')))
    {
      next_state= MY_LEX_START;        // Allow signed numbers
      return(tokval);                  // Was keyword
    }
    yySkip();                  // next state does a unget
  }

  /*
     Note: "SELECT _bla AS 'alias'"
     _bla should be considered as a IDENT if charset haven't been found.
     So we don't use MYF(MY_WME) with get_charset_by_csname to avoid
     producing an error.
  */
  DBUG_ASSERT(length > 0);
  if (resolve_introducer && m_tok_start[0] == '_')
  {

    yyUnget();                       // ptr points now after last token char
    str->set_ident(m_tok_start, length, false);

    m_cpp_text_start= m_cpp_tok_start;
    m_cpp_text_end= m_cpp_text_start + length;
    body_utf8_append(m_cpp_text_start, m_cpp_tok_start + length);
    ErrConvString csname(str->str + 1, str->length - 1, &my_charset_bin);
    CHARSET_INFO *cs= get_charset_by_csname(csname.ptr(),
                                            MY_CS_PRIMARY, MYF(0));
    if (cs)
    {
      *introducer= cs;
      return UNDERSCORE_CHARSET;
    }
    return IDENT;
  }

  yyUnget();                       // ptr points now after last token char
  str->set_ident(m_tok_start, length, is_8bit);
  m_cpp_text_start= m_cpp_tok_start;
  m_cpp_text_end= m_cpp_text_start + length;
  body_utf8_append(m_cpp_text_start);
  body_utf8_append_ident(thd, str, m_cpp_text_end);
  return is_8bit ? IDENT_QUOTED : IDENT;
}


int Lex_input_stream::scan_ident_delimited(THD *thd,
                                           Lex_ident_cli_st *str,
                                           uchar quote_char)
{
  CHARSET_INFO *const cs= thd->charset();
  uint double_quotes= 0;
  uchar c;
  DBUG_ASSERT(m_ptr == m_tok_start + 1);

  while ((c= yyGet()))
  {
    int var_length= cs->charlen(get_ptr() - 1, get_end_of_query());
    if (var_length == 1)
    {
      if (c == quote_char)
      {
        if (yyPeek() != quote_char)
          break;
        c= yyGet();
        double_quotes++;
        continue;
      }
    }
    else if (var_length > 1)
    {
      skip_binary(var_length - 1);
    }
  }

  str->set_ident_quoted(m_tok_start + 1, yyLength() - 1, true, quote_char);
  yyUnget();                       // ptr points now after last token char

  m_cpp_text_start= m_cpp_tok_start + 1;
  m_cpp_text_end= m_cpp_text_start + str->length;

  if (c == quote_char)
    yySkip();                  // Skip end `
  next_state= MY_LEX_START;
  body_utf8_append(m_cpp_text_start);
  // QQQ: shouldn't it add unescaped version ????
  body_utf8_append_ident(thd, str, m_cpp_text_end);
  return IDENT_QUOTED;
}


void trim_whitespace(CHARSET_INFO *cs, LEX_CSTRING *str, size_t * prefix_length)
{
  /*
    TODO:
    This code assumes that there are no multi-bytes characters
    that can be considered white-space.
  */

  size_t plen= 0;
  while ((str->length > 0) && (my_isspace(cs, str->str[0])))
  {
    plen++;
    str->length --;
    str->str ++;
  }
  if (prefix_length)
    *prefix_length= plen;
  /*
    FIXME:
    Also, parsing backward is not safe with multi bytes characters
  */
  while ((str->length > 0) && (my_isspace(cs, str->str[str->length-1])))
  {
    str->length --;
  }
}


/*
  st_select_lex structures initialisations
*/

void st_select_lex_node::init_query_common()
{
  options= 0;
  set_linkage(UNSPECIFIED_TYPE);
  distinct= TRUE;
  no_table_names_allowed= 0;
  uncacheable= 0;
}

void st_select_lex_unit::init_query()
{
  init_query_common();
  set_linkage(GLOBAL_OPTIONS_TYPE);
  lim.set_unlimited();
  union_distinct= 0;
  prepared= optimized= optimized_2= executed= 0;
  bag_set_op_optimized= 0;
  optimize_started= 0;
  item= 0;
  union_result= 0;
  table= 0;
  fake_select_lex= 0;
  saved_fake_select_lex= 0;
  cleaned= 0;
  item_list.empty();
  describe= 0;
  found_rows_for_union= 0;
  derived= 0;
  is_view= false;
  with_clause= 0;
  with_element= 0;
  columns_are_renamed= false;
  with_wrapped_tvc= false;
  have_except_all_or_intersect_all= false;
}

void st_select_lex::init_query()
{
  init_query_common();
  table_list.empty();
  top_join_list.empty();
  join_list= &top_join_list;
  embedding= 0;
  leaf_tables_prep.empty();
  leaf_tables.empty();
  item_list.empty();
  min_max_opt_list.empty();
  join= 0;
  having= prep_having= where= prep_where= 0;
  cond_pushed_into_where= cond_pushed_into_having= 0;
  attach_to_conds.empty();
  olap= UNSPECIFIED_OLAP_TYPE;
  having_fix_field= 0;
  having_fix_field_for_pushed_cond= 0;
  context.select_lex= this;
  context.init();
  cond_count= between_count= with_wild= 0;
  max_equal_elems= 0;
  ref_pointer_array.reset();
  select_n_where_fields= 0;
  select_n_reserved= 0;
  select_n_having_items= 0;
  n_sum_items= 0;
  n_child_sum_items= 0;
  hidden_bit_fields= 0;
  subquery_in_having= explicit_limit= 0;
  is_item_list_lookup= 0;
  changed_elements= 0;
  first_natural_join_processing= 1;
  first_cond_optimization= 1;
  parsing_place= NO_MATTER;
  save_parsing_place= NO_MATTER;
  exclude_from_table_unique_test= no_wrap_view_item= FALSE;
  nest_level= 0;
  link_next= 0;
  prep_leaf_list_state= UNINIT;
  have_merged_subqueries= FALSE;
  bzero((char*) expr_cache_may_be_used, sizeof(expr_cache_may_be_used));
  select_list_tables= 0;
  m_non_agg_field_used= false;
  m_agg_func_used= false;
  m_custom_agg_func_used= false;
  window_specs.empty();
  window_funcs.empty();
  tvc= 0;
  in_tvc= false;
  versioned_tables= 0;
  pushdown_select= 0;
}

void st_select_lex::init_select()
{
  sj_nests.empty();
  sj_subselects.empty();
  group_list.empty();
  if (group_list_ptrs)
    group_list_ptrs->clear();
  type= 0;
  db= null_clex_str;
  having= 0;
  table_join_options= 0;
  in_sum_expr= with_wild= 0;
  options= 0;
  ftfunc_list_alloc.empty();
  inner_sum_func_list= 0;
  ftfunc_list= &ftfunc_list_alloc;
  order_list.empty();
  /* Set limit and offset to default values */
  select_limit= 0;      /* denotes the default limit = HA_POS_ERROR */
  offset_limit= 0;      /* denotes the default offset = 0 */
  is_set_query_expr_tail= false;
  with_sum_func= 0;
  with_all_modifier= 0;
  is_correlated= 0;
  cur_pos_in_select_list= UNDEF_POS;
  cond_value= having_value= Item::COND_UNDEF;
  inner_refs_list.empty();
  insert_tables= 0;
  merged_into= 0;
  m_non_agg_field_used= false;
  m_agg_func_used= false;
  m_custom_agg_func_used= false;
  name_visibility_map.clear_all();
  with_dep= 0;
  join= 0;
  lock_type= TL_READ_DEFAULT;
  tvc= 0;
  in_funcs.empty();
  curr_tvc_name= 0;
  in_tvc= false;
  versioned_tables= 0;
  nest_flags= 0;
}

/*
  st_select_lex structures linking
*/

/* include on level down */
void st_select_lex_node::include_down(st_select_lex_node *upper)
{
  if ((next= upper->slave))
    next->prev= &next;
  prev= &upper->slave;
  upper->slave= this;
  master= upper;
  slave= 0;
}


void st_select_lex_node::add_slave(st_select_lex_node *slave_arg)
{
  for (; slave; slave= slave->next)
    if (slave == slave_arg)
      return;

  if (slave)
  {
    st_select_lex_node *slave_arg_slave= slave_arg->slave;
    /* Insert in the front of list of slaves if any. */
    slave_arg->include_neighbour(slave);
    /* include_neighbour() sets slave_arg->slave=0, restore it. */
    slave_arg->slave= slave_arg_slave;
    /* Count on include_neighbour() setting the master. */
    DBUG_ASSERT(slave_arg->master == this);
  }
  else
  {
    slave= slave_arg;
    slave_arg->master= this;
  }
}

void st_select_lex_node::link_chain_down(st_select_lex_node *first)
{
  st_select_lex_node *last_node;
  st_select_lex_node *node= first;
  do
  {
    last_node= node;
    node->master= this;
    node= node->next;
  } while (node);
  if ((last_node->next= slave))
  {
    slave->prev= &last_node->next;
  }
  first->prev= &slave;
  slave= first;
}

/*
  include on level down (but do not link)

  SYNOPSYS
    st_select_lex_node::include_standalone()
    upper - reference on node underr which this node should be included
    ref - references on reference on this node
*/
void st_select_lex_node::include_standalone(st_select_lex_node *upper,
                                            st_select_lex_node **ref)
{
  next= 0;
  prev= ref;
  master= upper;
  slave= 0;
}

/* include neighbour (on same level) */
void st_select_lex_node::include_neighbour(st_select_lex_node *before)
{
  if ((next= before->next))
    next->prev= &next;
  prev= &before->next;
  before->next= this;
  master= before->master;
  slave= 0;
}

/* including in global SELECT_LEX list */
void st_select_lex_node::include_global(st_select_lex_node **plink)
{
  if ((link_next= *plink))
    link_next->link_prev= &link_next;
  link_prev= plink;
  *plink= this;
}

//excluding from global list (internal function)
void st_select_lex_node::fast_exclude()
{
  if (link_prev)
  {
    if ((*link_prev= link_next))
      link_next->link_prev= link_prev;
  }
  // Remove slave structure
  for (; slave; slave= slave->next)
    slave->fast_exclude();

}


/**
  @brief
    Insert a new chain of nodes into another chain before a particular link

  @param in/out
    ptr_pos_to_insert  the address of the chain pointer pointing to the link
                       before which the subchain has to be inserted
  @param   
    end_chain_node     the last link of the subchain to be inserted

  @details
    The method inserts the chain of nodes starting from this node and ending
    with the node nd_chain_node into another chain of nodes before the node
    pointed to by *ptr_pos_to_insert.
    It is assumed that ptr_pos_to_insert belongs to the chain where we insert.
    So it must be updated.

  @retval
    The method returns the pointer to the first link of the inserted chain
*/

st_select_lex_node *st_select_lex_node:: insert_chain_before(
                                         st_select_lex_node **ptr_pos_to_insert,
                                         st_select_lex_node *end_chain_node)
{
  end_chain_node->link_next= *ptr_pos_to_insert;
  (*ptr_pos_to_insert)->link_prev= &end_chain_node->link_next;
  link_prev= ptr_pos_to_insert;
  return this;
}


/*
  Detach the node from its master and attach it to a new master
*/

void st_select_lex_node::move_as_slave(st_select_lex_node *new_master)
{
  exclude_from_tree();
  if (new_master->slave)
  {
    st_select_lex_node *curr= new_master->slave;
    for ( ; curr->next ; curr= curr->next) ;
    prev= &curr->next;
  }
  else
    prev= &new_master->slave;
  *prev= this;
  next= 0;
  master= new_master;
}


/*
  Exclude a node from the tree lex structure, but leave it in the global
  list of nodes.
*/

void st_select_lex_node::exclude_from_tree()
{
  if ((*prev= next))
    next->prev= prev;
}


/*
  Exclude select_lex structure (except first (first select can't be
  deleted, because it is most upper select))
*/
void st_select_lex_node::exclude()
{
  /* exclude from global list */
  fast_exclude();
  /* exclude from other structures */
  exclude_from_tree();
  /* 
     We do not need following statements, because prev pointer of first 
     list element point to master->slave
     if (master->slave == this)
       master->slave= next;
  */
}


/*
  Exclude level of current unit from tree of SELECTs

  SYNOPSYS
    st_select_lex_unit::exclude_level()

  NOTE: units which belong to current will be brought up on level of
  currernt unit 
*/
void st_select_lex_unit::exclude_level()
{
  SELECT_LEX_UNIT *units= 0, **units_last= &units;
  for (SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    // unlink current level from global SELECTs list
    if (sl->link_prev && (*sl->link_prev= sl->link_next))
      sl->link_next->link_prev= sl->link_prev;

    // bring up underlay levels
    SELECT_LEX_UNIT **last= 0;
    for (SELECT_LEX_UNIT *u= sl->first_inner_unit(); u; u= u->next_unit())
    {
      u->master= master;
      last= (SELECT_LEX_UNIT**)&(u->next);
    }
    if (last)
    {
      (*units_last)= sl->first_inner_unit();
      units_last= last;
    }
  }
  if (units)
  {
    // include brought up levels in place of current
    (*prev)= units;
    (*units_last)= (SELECT_LEX_UNIT*)next;
    if (next)
      next->prev= (SELECT_LEX_NODE**)units_last;
    units->prev= prev;
  }
  else
  {
    // exclude currect unit from list of nodes
    (*prev)= next;
    if (next)
      next->prev= prev;
  }
  // Mark it excluded
  prev= NULL;
}


#if 0
/*
  Exclude subtree of current unit from tree of SELECTs

  SYNOPSYS
    st_select_lex_unit::exclude_tree()
*/
void st_select_lex_unit::exclude_tree()
{
  for (SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    // unlink current level from global SELECTs list
    if (sl->link_prev && (*sl->link_prev= sl->link_next))
      sl->link_next->link_prev= sl->link_prev;

    // unlink underlay levels
    for (SELECT_LEX_UNIT *u= sl->first_inner_unit(); u; u= u->next_unit())
    {
      u->exclude_level();
    }
  }
  // exclude currect unit from list of nodes
  (*prev)= next;
  if (next)
    next->prev= prev;
}
#endif


/*
  st_select_lex_node::mark_as_dependent mark all st_select_lex struct from 
  this to 'last' as dependent

  SYNOPSIS
    last - pointer to last st_select_lex struct, before which all 
           st_select_lex have to be marked as dependent

  NOTE
    'last' should be reachable from this st_select_lex_node
*/

bool st_select_lex::mark_as_dependent(THD *thd, st_select_lex *last,
                                      Item *dependency)
{

  DBUG_ASSERT(this != last);

  /*
    Mark all selects from resolved to 1 before select where was
    found table as depended (of select where was found table)
  */
  SELECT_LEX *s= this;
  do
  {
    if (!(s->uncacheable & UNCACHEABLE_DEPENDENT_GENERATED))
    {
      // Select is dependent of outer select
      s->uncacheable= (s->uncacheable & ~UNCACHEABLE_UNITED) |
                       UNCACHEABLE_DEPENDENT_GENERATED;
      SELECT_LEX_UNIT *munit= s->master_unit();
      munit->uncacheable= (munit->uncacheable & ~UNCACHEABLE_UNITED) |
                       UNCACHEABLE_DEPENDENT_GENERATED;
      for (SELECT_LEX *sl= munit->first_select(); sl ; sl= sl->next_select())
      {
        if (sl != s &&
            !(sl->uncacheable & (UNCACHEABLE_DEPENDENT_GENERATED |
                                 UNCACHEABLE_UNITED)))
          sl->uncacheable|= UNCACHEABLE_UNITED;
      }
    }

    Item_subselect *subquery_expr= s->master_unit()->item;
    if (subquery_expr && subquery_expr->mark_as_dependent(thd, last, 
                                                          dependency))
      return TRUE;
  } while ((s= s->outer_select()) != last && s != 0);
  is_correlated= TRUE;
  master_unit()->item->is_correlated= TRUE;
  return FALSE;
}

/*
  prohibit using LIMIT clause
*/
bool st_select_lex::test_limit()
{
  if (select_limit != 0)
  {
    my_error(ER_NOT_SUPPORTED_YET, MYF(0),
             "LIMIT & IN/ALL/ANY/SOME subquery");
    return(1);
  }
  return(0);
}



st_select_lex* st_select_lex_unit::outer_select()
{
  return (st_select_lex*) master;
}


ha_rows st_select_lex::get_offset()
{
  ulonglong val= 0;

  if (offset_limit)
  {
    // see comment for st_select_lex::get_limit()
    bool err= offset_limit->fix_fields_if_needed(master_unit()->thd, NULL);
    DBUG_ASSERT(!err);
    val= err ? HA_POS_ERROR : offset_limit->val_uint();
  }

  return (ha_rows)val;
}


ha_rows st_select_lex::get_limit()
{
  ulonglong val= HA_POS_ERROR;

  if (select_limit)
  {
    /*
      fix_fields() has not been called for select_limit. That's due to the
      historical reasons -- this item could be only of type Item_int, and
      Item_int does not require fix_fields(). Thus, fix_fields() was never
      called for select_limit.

      Some time ago, Item_splocal was also allowed for LIMIT / OFFSET clauses.
      However, the fix_fields() behavior was not updated, which led to a crash
      in some cases.

      There is no single place where to call fix_fields() for LIMIT / OFFSET
      items during the fix-fields-phase. Thus, for the sake of readability,
      it was decided to do it here, on the evaluation phase (which is a
      violation of design, but we chose the lesser of two evils).

      We can call fix_fields() here, because select_limit can be of two
      types only: Item_int and Item_splocal. Item_int::fix_fields() is trivial,
      and Item_splocal::fix_fields() (or rather Item_sp_variable::fix_fields())
      has the following properties:
        1) it does not affect other items;
        2) it does not fail.

      Nevertheless DBUG_ASSERT was added to catch future changes in
      fix_fields() implementation. Also added runtime check against a result
      of fix_fields() in order to handle error condition in non-debug build.
    */
    bool err= select_limit->fix_fields_if_needed(master_unit()->thd, NULL);
    DBUG_ASSERT(!err);
    val= err ? HA_POS_ERROR : select_limit->val_uint();
  }

  return (ha_rows)val;
}


bool st_select_lex::add_order_to_list(THD *thd, Item *item, bool asc)
{
  return add_to_list(thd, order_list, item, asc);
}


bool st_select_lex::add_gorder_to_list(THD *thd, Item *item, bool asc)
{
  return add_to_list(thd, gorder_list, item, asc);
}


bool st_select_lex::add_item_to_list(THD *thd, Item *item)
{
  DBUG_ENTER("st_select_lex::add_item_to_list");
  DBUG_PRINT("info", ("Item: %p", item));
  DBUG_RETURN(item_list.push_back(item, thd->mem_root));
}


bool st_select_lex::add_group_to_list(THD *thd, Item *item, bool asc)
{
  return add_to_list(thd, group_list, item, asc);
}


bool st_select_lex::add_ftfunc_to_list(THD *thd, Item_func_match *func)
{
  return !func || ftfunc_list->push_back(func, thd->mem_root); // end of memory?
}


st_select_lex* st_select_lex::outer_select()
{
  return (st_select_lex*) master->get_master();
}


bool st_select_lex::inc_in_sum_expr()
{
  in_sum_expr++;
  return 0;
}


uint st_select_lex::get_in_sum_expr()
{
  return in_sum_expr;
}


TABLE_LIST* st_select_lex::get_table_list()
{
  return table_list.first;
}

List<Item>* st_select_lex::get_item_list()
{
  return &item_list;
}

ulong st_select_lex::get_table_join_options()
{
  return table_join_options;
}


bool st_select_lex::setup_ref_array(THD *thd, uint order_group_num)
{

  if (!((options & SELECT_DISTINCT) && !group_list.elements))
    hidden_bit_fields= 0;

  // find_order_in_list() may need some extra space, so multiply by two.
  order_group_num*= 2;

  /*
    We have to create array in prepared statement memory if it is a
    prepared statement
  */
  Query_arena *arena= thd->stmt_arena;
  const uint n_elems= (n_sum_items +
                       n_child_sum_items +
                       item_list.elements +
                       select_n_reserved +
                       select_n_having_items +
                       select_n_where_fields +
                       order_group_num +
                       hidden_bit_fields) * 5;
  if (!ref_pointer_array.is_null())
  {
    /*
      We need to take 'n_sum_items' into account when allocating the array,
      and this may actually increase during the optimization phase due to
      MIN/MAX rewrite in Item_in_subselect::single_value_transformer.
      In the usual case we can reuse the array from the prepare phase.
      If we need a bigger array, we must allocate a new one.
     */
    if (ref_pointer_array.size() >= n_elems)
      return false;
   }
  Item **array= static_cast<Item**>(arena->alloc(sizeof(Item*) * n_elems));
  if (likely(array != NULL))
    ref_pointer_array= Ref_ptr_array(array, n_elems);

  return array == NULL;
}


/*
  @brief
    Print the whole statement

    @param str         Print into this string
    @param query_type  Flags describing how to print

  @detail
    The intent is to allow to eventually print back any query.

    This is useful e.g. for storage engines that take over diferrent kinds of
    queries
*/

void LEX::print(String *str, enum_query_type query_type)
{
  if (sql_command == SQLCOM_UPDATE)
  {
    SELECT_LEX *sel= first_select_lex();
    str->append(STRING_WITH_LEN("UPDATE "));
    if (ignore)
      str->append(STRING_WITH_LEN("IGNORE "));
    // table name
    str->append(query_tables->alias);
    str->append(STRING_WITH_LEN(" SET "));
    // print item assignments
    List_iterator<Item> it(sel->item_list);
    List_iterator<Item> it2(value_list);
    Item *col_ref, *value;
    bool first= true;
    while ((col_ref= it++) && (value= it2++))
    {
      if (first)
        first= false;
      else
        str->append(STRING_WITH_LEN(", "));
      col_ref->print(str, query_type);
      str->append(STRING_WITH_LEN("="));
      value->print(str, query_type);
    }

    str->append(STRING_WITH_LEN(" WHERE "));
    sel->where->print(str, query_type);

    if (sel->order_list.elements)
    {
      str->append(STRING_WITH_LEN(" ORDER BY "));
      for (ORDER *ord= sel->order_list.first; ord; ord= ord->next)
      {
        if (ord != sel->order_list.first)
          str->append(STRING_WITH_LEN(", "));
        (*ord->item)->print(str, query_type);
      }
    }
    if (sel->select_limit)
    {
      str->append(STRING_WITH_LEN(" LIMIT "));
      sel->select_limit->print(str, query_type);
    }
  }
  else
    DBUG_ASSERT(0); // Not implemented yet
}

void st_select_lex_unit::print(String *str, enum_query_type query_type)
{
  if (with_clause)
    with_clause->print(thd, str, query_type);
  for (SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    if (sl != first_select())
    {
      switch (sl->linkage)
      {
      default:
        DBUG_ASSERT(0);
        /* fall through */
      case UNION_TYPE:
        str->append(STRING_WITH_LEN(" union "));
        break;
      case INTERSECT_TYPE:
        str->append(STRING_WITH_LEN(" intersect "));
        break;
      case EXCEPT_TYPE:
        str->append(STRING_WITH_LEN(" except "));
        break;
      }
      if (!sl->distinct)
        str->append(STRING_WITH_LEN("all "));
    }
    if (sl->braces)
      str->append('(');
    sl->print(thd, str, query_type);
    if (sl->braces)
      str->append(')');
  }
  if (fake_select_lex)
  {
    if (fake_select_lex->order_list.elements)
    {
      str->append(STRING_WITH_LEN(" order by "));
      fake_select_lex->print_order(str,
        fake_select_lex->order_list.first,
        query_type);
    }
    fake_select_lex->print_limit(thd, str, query_type);
  }
  else if (saved_fake_select_lex)
    saved_fake_select_lex->print_limit(thd, str, query_type);
}


void st_select_lex::print_order(String *str,
                                ORDER *order,
                                enum_query_type query_type)
{
  for (; order; order= order->next)
  {
    if (order->counter_used)
    {
      char buffer[20];
      size_t length= my_snprintf(buffer, 20, "%d", order->counter);
      str->append(buffer, (uint) length);
    }
    else
    {
      /* replace numeric reference with equivalent for ORDER constant */
      if (order->item[0]->is_order_clause_position())
      {
        /* make it expression instead of integer constant */
        str->append(STRING_WITH_LEN("''"));
      }
      else
        (*order->item)->print(str, query_type);
    }
    if (order->direction == ORDER::ORDER_DESC)
       str->append(STRING_WITH_LEN(" desc"));
    if (order->next)
      str->append(',');
  }
}
 

void st_select_lex::print_limit(THD *thd,
                                String *str,
                                enum_query_type query_type)
{
  SELECT_LEX_UNIT *unit= master_unit();
  Item_subselect *item= unit->item;

  if (item && unit->global_parameters() == this)
  {
    Item_subselect::subs_type subs_type= item->substype();
    if (subs_type == Item_subselect::IN_SUBS ||
        subs_type == Item_subselect::ALL_SUBS)
    {
      return;
    }
  }
  if (explicit_limit && select_limit)
  {
    str->append(STRING_WITH_LEN(" limit "));
    if (offset_limit)
    {
      offset_limit->print(str, query_type);
      str->append(',');
    }
    select_limit->print(str, query_type);
  }
}


/**
  @brief Restore the LEX and THD in case of a parse error.

  This is a clean up call that is invoked by the Bison generated
  parser before returning an error from MYSQLparse. If your
  semantic actions manipulate with the global thread state (which
  is a very bad practice and should not normally be employed) and
  need a clean-up in case of error, and you can not use %destructor
  rule in the grammar file itself, this function should be used
  to implement the clean up.
*/

void LEX::cleanup_lex_after_parse_error(THD *thd)
{
  /*
    Delete sphead for the side effect of restoring of the original
    LEX state, thd->lex, thd->mem_root and thd->free_list if they
    were replaced when parsing stored procedure statements.  We
    will never use sphead object after a parse error, so it's okay
    to delete it only for the sake of the side effect.
    TODO: make this functionality explicit in sp_head class.
    Sic: we must nullify the member of the main lex, not the
    current one that will be thrown away
  */
  if (thd->lex->sphead)
  {
    sp_package *pkg;
    thd->lex->sphead->restore_thd_mem_root(thd);
    if ((pkg= thd->lex->sphead->m_parent))
    {
      /*
        If a syntax error happened inside a package routine definition,
        then thd->lex points to the routine sublex. We need to restore to
        the top level LEX.
      */
      DBUG_ASSERT(pkg->m_top_level_lex);
      DBUG_ASSERT(pkg == pkg->m_top_level_lex->sphead);
      pkg->restore_thd_mem_root(thd);
      LEX *top= pkg->m_top_level_lex;
      sp_package::destroy(pkg);
      thd->lex= top;
      thd->lex->sphead= NULL;
    }
    else
    {
      sp_head::destroy(thd->lex->sphead);
      thd->lex->sphead= NULL;
    }
  }
}

/*
  Initialize (or reset) Query_tables_list object.

  SYNOPSIS
    reset_query_tables_list()
      init  TRUE  - we should perform full initialization of object with
                    allocating needed memory
            FALSE - object is already initialized so we should only reset
                    its state so it can be used for parsing/processing
                    of new statement

  DESCRIPTION
    This method initializes Query_tables_list so it can be used as part
    of LEX object for parsing/processing of statement. One can also use
    this method to reset state of already initialized Query_tables_list
    so it can be used for processing of new statement.
*/

void Query_tables_list::reset_query_tables_list(bool init)
{
  sql_command= SQLCOM_END;
  if (!init && query_tables)
  {
    TABLE_LIST *table= query_tables;
    for (;;)
    {
      delete table->view;
      if (query_tables_last == &table->next_global ||
          !(table= table->next_global))
        break;
    }
  }
  query_tables= 0;
  query_tables_last= &query_tables;
  query_tables_own_last= 0;
  if (init)
  {
    /*
      We delay real initialization of hash (and therefore related
      memory allocation) until first insertion into this hash.
    */
    my_hash_clear(&sroutines);
  }
  else if (sroutines.records)
  {
    /* Non-zero sroutines.records means that hash was initialized. */
    my_hash_reset(&sroutines);
  }
  sroutines_list.empty();
  sroutines_list_own_last= sroutines_list.next;
  sroutines_list_own_elements= 0;
  binlog_stmt_flags= 0;
  stmt_accessed_table_flag= 0;
}


/*
  Destroy Query_tables_list object with freeing all resources used by it.

  SYNOPSIS
    destroy_query_tables_list()
*/

void Query_tables_list::destroy_query_tables_list()
{
  my_hash_free(&sroutines);
}


/*
  Initialize LEX object.

  SYNOPSIS
    LEX::LEX()

  NOTE
    LEX object initialized with this constructor can be used as part of
    THD object for which one can safely call open_tables(), lock_tables()
    and close_thread_tables() functions. But it is not yet ready for
    statement parsing. On should use lex_start() function to prepare LEX
    for this.
*/

LEX::LEX()
  : explain(NULL), result(0), part_info(NULL), arena_for_set_stmt(0), mem_root_for_set_stmt(0),
    option_type(OPT_DEFAULT), context_analysis_only(0), sphead(0),
    default_used(0), is_lex_started(0), limit_rows_examined_cnt(ULONGLONG_MAX)
{

  init_dynamic_array2(PSI_INSTRUMENT_ME, &plugins, sizeof(plugin_ref),
                      plugins_static_buffer, INITIAL_LEX_PLUGIN_LIST_SIZE,
                      INITIAL_LEX_PLUGIN_LIST_SIZE, 0);
  reset_query_tables_list(TRUE);
  mi.init();
  init_dynamic_array2(PSI_INSTRUMENT_ME, &delete_gtid_domain, sizeof(uint32),
                      gtid_domain_static_buffer,
                      initial_gtid_domain_buffer_size,
                      initial_gtid_domain_buffer_size, 0);
  unit.slave= &builtin_select;
}


/*
  Check whether the merging algorithm can be used on this VIEW

  SYNOPSIS
    LEX::can_be_merged()

  DESCRIPTION
    We can apply merge algorithm if it is single SELECT view  with
    subqueries only in WHERE clause (we do not count SELECTs of underlying
    views, and second level subqueries) and we have not grpouping, ordering,
    HAVING clause, aggregate functions, DISTINCT clause, LIMIT clause and
    several underlying tables.

  RETURN
    FALSE - only temporary table algorithm can be used
    TRUE  - merge algorithm can be used
*/

bool LEX::can_be_merged()
{
  // TODO: do not forget implement case when select_lex.table_list.elements==0

  /* find non VIEW subqueries/unions */
  bool selects_allow_merge= (first_select_lex()->next_select() == 0 &&
                             !(first_select_lex()->uncacheable &
                               UNCACHEABLE_RAND));
  if (selects_allow_merge)
  {
    for (SELECT_LEX_UNIT *tmp_unit= first_select_lex()->first_inner_unit();
         tmp_unit;
         tmp_unit= tmp_unit->next_unit())
    {
      if (tmp_unit->first_select()->parent_lex == this &&
          (tmp_unit->item != 0 &&
           (tmp_unit->item->place() != IN_WHERE &&
            tmp_unit->item->place() != IN_ON &&
            tmp_unit->item->place() != SELECT_LIST)))
      {
        selects_allow_merge= 0;
        break;
      }
    }
  }

  return (selects_allow_merge &&
          first_select_lex()->group_list.elements == 0 &&
          first_select_lex()->having == 0 &&
          first_select_lex()->with_sum_func == 0 &&
          first_select_lex()->table_list.elements >= 1 &&
          !(first_select_lex()->options & SELECT_DISTINCT) &&
          first_select_lex()->select_limit == 0);
}


/*
  check if command can use VIEW with MERGE algorithm (for top VIEWs)

  SYNOPSIS
    LEX::can_use_merged()

  DESCRIPTION
    Only listed here commands can use merge algorithm in top level
    SELECT_LEX (for subqueries will be used merge algorithm if
    LEX::can_not_use_merged() is not TRUE).

  RETURN
    FALSE - command can't use merged VIEWs
    TRUE  - VIEWs with MERGE algorithms can be used
*/

bool LEX::can_use_merged()
{
  switch (sql_command)
  {
  case SQLCOM_SELECT:
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_UPDATE:
  case SQLCOM_UPDATE_MULTI:
  case SQLCOM_DELETE:
  case SQLCOM_DELETE_MULTI:
  case SQLCOM_INSERT:
  case SQLCOM_INSERT_SELECT:
  case SQLCOM_REPLACE:
  case SQLCOM_REPLACE_SELECT:
  case SQLCOM_LOAD:
    return TRUE;
  default:
    return FALSE;
  }
}

/*
  Check if command can't use merged views in any part of command

  SYNOPSIS
    LEX::can_not_use_merged()

  DESCRIPTION
    Temporary table algorithm will be used on all SELECT levels for queries
    listed here (see also LEX::can_use_merged()).

  RETURN
    FALSE - command can't use merged VIEWs
    TRUE  - VIEWs with MERGE algorithms can be used
*/

bool LEX::can_not_use_merged()
{
  switch (sql_command)
  {
  case SQLCOM_CREATE_VIEW:
  case SQLCOM_SHOW_CREATE:
  /*
    SQLCOM_SHOW_FIELDS is necessary to make 
    information schema tables working correctly with views.
    see get_schema_tables_result function
  */
  case SQLCOM_SHOW_FIELDS:
    return TRUE;
  default:
    return FALSE;
  }
}

/*
  Detect that we need only table structure of derived table/view

  SYNOPSIS
    only_view_structure()

  RETURN
    TRUE yes, we need only structure
    FALSE no, we need data
*/

bool LEX::only_view_structure()
{
  switch (sql_command) {
  case SQLCOM_SHOW_CREATE:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_SHOW_FIELDS:
  case SQLCOM_REVOKE_ALL:
  case SQLCOM_REVOKE:
  case SQLCOM_GRANT:
  case SQLCOM_CREATE_VIEW:
    return TRUE;
  default:
    return FALSE;
  }
}


/*
  Should Items_ident be printed correctly

  SYNOPSIS
    need_correct_ident()

  RETURN
    TRUE yes, we need only structure
    FALSE no, we need data
*/


bool LEX::need_correct_ident()
{
  switch(sql_command)
  {
  case SQLCOM_SHOW_CREATE:
  case SQLCOM_SHOW_TABLES:
  case SQLCOM_CREATE_VIEW:
    return TRUE;
  default:
    return FALSE;
  }
}

/*
  Get effective type of CHECK OPTION for given view

  SYNOPSIS
    get_effective_with_check()
    view    given view

  NOTE
    It have not sense to set CHECK OPTION for SELECT satement or subqueries,
    so we do not.

  RETURN
    VIEW_CHECK_NONE      no need CHECK OPTION
    VIEW_CHECK_LOCAL     CHECK OPTION LOCAL
    VIEW_CHECK_CASCADED  CHECK OPTION CASCADED
*/

uint8 LEX::get_effective_with_check(TABLE_LIST *view)
{
  if (view->select_lex->master_unit() == &unit &&
      which_check_option_applicable())
    return (uint8)view->with_check;
  return VIEW_CHECK_NONE;
}


/**
  This method should be called only during parsing.
  It is aware of compound statements (stored routine bodies)
  and will initialize the destination with the default
  database of the stored routine, rather than the default
  database of the connection it is parsed in.
  E.g. if one has no current database selected, or current database 
  set to 'bar' and then issues:

  CREATE PROCEDURE foo.p1() BEGIN SELECT * FROM t1 END//

  t1 is meant to refer to foo.t1, not to bar.t1.

  This method is needed to support this rule.

  @return TRUE in case of error (parsing should be aborted, FALSE in
  case of success
*/

bool LEX::copy_db_to(LEX_CSTRING *to)
{
  if (sphead && sphead->m_name.str)
  {
    DBUG_ASSERT(sphead->m_db.str && sphead->m_db.length);
    /*
      It is safe to assign the string by-pointer, both sphead and
      its statements reside in the same memory root.
    */
    *to= sphead->m_db;
    return FALSE;
  }
  return thd->copy_db_to(to);
}

/**
  Initialize offset and limit counters.

  @param sl SELECT_LEX to get offset and limit from.
*/

void st_select_lex_unit::set_limit(st_select_lex *sl)
{
  DBUG_ASSERT(!thd->stmt_arena->is_stmt_prepare());

  lim.set_limit(sl->get_limit(), sl->get_offset());
}


/**
  Decide if a temporary table is needed for the UNION.

  @retval true  A temporary table is needed.
  @retval false A temporary table is not needed.
 */

bool st_select_lex_unit::union_needs_tmp_table()
{
  if (with_element && with_element->is_recursive)
    return true;
  if (!with_wrapped_tvc)
  {
    for (st_select_lex *sl= first_select(); sl; sl=sl->next_select())
    {
      if (sl->tvc && sl->tvc->to_be_wrapped_as_with_tail())
      {
        with_wrapped_tvc= true;
        break;
      }
      if (sl != first_select() && sl->linkage != UNION_TYPE)
        return true;
    }
  }
  if (with_wrapped_tvc)
    return true;
  return union_distinct != NULL ||
    global_parameters()->order_list.elements != 0 ||
    thd->lex->sql_command == SQLCOM_INSERT_SELECT ||
    thd->lex->sql_command == SQLCOM_REPLACE_SELECT;
}  

/**
  @brief Set the initial purpose of this TABLE_LIST object in the list of used
    tables.

  We need to track this information on table-by-table basis, since when this
  table becomes an element of the pre-locked list, it's impossible to identify
  which SQL sub-statement it has been originally used in.

  E.g.:

  User request:                 SELECT * FROM t1 WHERE f1();
  FUNCTION f1():                DELETE FROM t2; RETURN 1;
  BEFORE DELETE trigger on t2:  INSERT INTO t3 VALUES (old.a);

  For this user request, the pre-locked list will contain t1, t2, t3
  table elements, each needed for different DML.

  The trigger event map is updated to reflect INSERT, UPDATE, DELETE,
  REPLACE, LOAD DATA, CREATE TABLE .. SELECT, CREATE TABLE ..
  REPLACE SELECT statements, and additionally ON DUPLICATE KEY UPDATE
  clause.
*/

void LEX::set_trg_event_type_for_tables()
{
  uint8 new_trg_event_map= 0;
  DBUG_ENTER("LEX::set_trg_event_type_for_tables");

  /*
    Some auxiliary operations
    (e.g. GRANT processing) create TABLE_LIST instances outside
    the parser. Additionally, some commands (e.g. OPTIMIZE) change
    the lock type for a table only after parsing is done. Luckily,
    these do not fire triggers and do not need to pre-load them.
    For these TABLE_LISTs set_trg_event_type is never called, and
    trg_event_map is always empty. That means that the pre-locking
    algorithm will ignore triggers defined on these tables, if
    any, and the execution will either fail with an assert in
    sql_trigger.cc or with an error that a used table was not
    pre-locked, in case of a production build.

    TODO: this usage pattern creates unnecessary module dependencies
    and should be rewritten to go through the parser.
    Table list instances created outside the parser in most cases
    refer to mysql.* system tables. It is not allowed to have
    a trigger on a system table, but keeping track of
    initialization provides extra safety in case this limitation
    is circumvented.
  */

  switch (sql_command) {
  case SQLCOM_LOCK_TABLES:
  /*
    On a LOCK TABLE, all triggers must be pre-loaded for this TABLE_LIST
    when opening an associated TABLE.
  */
    new_trg_event_map= trg2bit(TRG_EVENT_INSERT) | trg2bit(TRG_EVENT_UPDATE) |
                       trg2bit(TRG_EVENT_DELETE);
    break;
  /*
    Basic INSERT. If there is an additional ON DUPLIATE KEY UPDATE
    clause, it will be handled later in this method.
  */
  case SQLCOM_INSERT:                           /* fall through */
  case SQLCOM_INSERT_SELECT:
  /*
    LOAD DATA ... INFILE is expected to fire BEFORE/AFTER INSERT
    triggers.
    If the statement also has REPLACE clause, it will be
    handled later in this method.
  */
  case SQLCOM_LOAD:                             /* fall through */
  /*
    REPLACE is semantically equivalent to INSERT. In case
    of a primary or unique key conflict, it deletes the old
    record and inserts a new one. So we also may need to
    fire ON DELETE triggers. This functionality is handled
    later in this method.
  */
  case SQLCOM_REPLACE:                          /* fall through */
  case SQLCOM_REPLACE_SELECT:
  /*
    CREATE TABLE ... SELECT defaults to INSERT if the table or
    view already exists. REPLACE option of CREATE TABLE ...
    REPLACE SELECT is handled later in this method.
  */
  case SQLCOM_CREATE_TABLE:
  case SQLCOM_CREATE_SEQUENCE:
    new_trg_event_map|= trg2bit(TRG_EVENT_INSERT);
    break;
  /* Basic update and multi-update */
  case SQLCOM_UPDATE:                           /* fall through */
  case SQLCOM_UPDATE_MULTI:
    new_trg_event_map|= trg2bit(TRG_EVENT_UPDATE);
    break;
  /* Basic delete and multi-delete */
  case SQLCOM_DELETE:                           /* fall through */
  case SQLCOM_DELETE_MULTI:
    new_trg_event_map|= trg2bit(TRG_EVENT_DELETE);
    break;
  default:
    break;
  }

  switch (duplicates) {
  case DUP_UPDATE:
    new_trg_event_map|= trg2bit(TRG_EVENT_UPDATE);
    break;
  case DUP_REPLACE:
    new_trg_event_map|= trg2bit(TRG_EVENT_DELETE);
    break;
  case DUP_ERROR:
  default:
    break;
  }

  if (period_conditions.is_set())
  {
    switch (sql_command)
    {
    case SQLCOM_DELETE:
    case SQLCOM_UPDATE:
    case SQLCOM_REPLACE:
      new_trg_event_map |= trg2bit(TRG_EVENT_INSERT);
    default:
      break;
    }
  }


  /*
    Do not iterate over sub-selects, only the tables in the outermost
    SELECT_LEX can be modified, if any.
  */
  TABLE_LIST *tables= first_select_lex()->get_table_list();

  while (tables)
  {
    /*
      This is a fast check to filter out statements that do
      not change data, or tables  on the right side, in case of
      INSERT .. SELECT, CREATE TABLE .. SELECT and so on.
      Here we also filter out OPTIMIZE statement and non-updateable
      views, for which lock_type is TL_UNLOCK or TL_READ after
      parsing.
    */
    if (static_cast<int>(tables->lock_type) >=
        static_cast<int>(TL_WRITE_ALLOW_WRITE))
      tables->trg_event_map= new_trg_event_map;
    tables= tables->next_local;
  }
  DBUG_VOID_RETURN;
}


/*
  Unlink the first table from the global table list and the first table from
  outer select (lex->select_lex) local list

  SYNOPSIS
    unlink_first_table()
    link_to_local   Set to 1 if caller should link this table to local list

  NOTES
    We assume that first tables in both lists is the same table or the local
    list is empty.

  RETURN
    0      If 'query_tables' == 0
    unlinked table
      In this case link_to_local is set.

*/
TABLE_LIST *LEX::unlink_first_table(bool *link_to_local)
{
  TABLE_LIST *first;
  if ((first= query_tables))
  {
    /*
      Exclude from global table list
    */
    if ((query_tables= query_tables->next_global))
      query_tables->prev_global= &query_tables;
    else
      query_tables_last= &query_tables;
    first->next_global= 0;

    /*
      and from local list if it is not empty
    */
    if ((*link_to_local= MY_TEST(first_select_lex()->table_list.first)))
    {
      first_select_lex()->context.table_list=
         first_select_lex()->context.first_name_resolution_table=
         first->next_local;
      first_select_lex()->table_list.first= first->next_local;
      first_select_lex()->table_list.elements--;  //safety
      first->next_local= 0;
      /*
        Ensure that the global list has the same first table as the local
        list.
      */
      first_lists_tables_same();
    }
  }
  return first;
}


/*
  Bring first local table of first most outer select to first place in global
  table list

  SYNOPSYS
     LEX::first_lists_tables_same()

  NOTES
    In many cases (for example, usual INSERT/DELETE/...) the first table of
    main SELECT_LEX have special meaning => check that it is the first table
    in global list and re-link to be first in the global list if it is
    necessary.  We need such re-linking only for queries with sub-queries in
    the select list, as only in this case tables of sub-queries will go to
    the global list first.
*/

void LEX::first_lists_tables_same()
{
  TABLE_LIST *first_table= first_select_lex()->table_list.first;
  if (query_tables != first_table && first_table != 0)
  {
    TABLE_LIST *next;
    if (query_tables_last == &first_table->next_global)
      query_tables_last= first_table->prev_global;

    if (query_tables_own_last == &first_table->next_global)
      query_tables_own_last= first_table->prev_global;

    if ((next= *first_table->prev_global= first_table->next_global))
      next->prev_global= first_table->prev_global;
    /* include in new place */
    first_table->next_global= query_tables;
    /*
       We are sure that query_tables is not 0, because first_table was not
       first table in the global list => we can use
       query_tables->prev_global without check of query_tables
    */
    query_tables->prev_global= &first_table->next_global;
    first_table->prev_global= &query_tables;
    query_tables= first_table;
  }
}

void LEX::fix_first_select_number()
{
  SELECT_LEX *first= first_select_lex();
  if (first && first->select_number != 1)
  {
    uint num= first->select_number;
    for (SELECT_LEX *sel= all_selects_list;
         sel;
         sel= sel->next_select_in_list())
    {
      if (sel->select_number < num)
        sel->select_number++;
    }
    first->select_number= 1;
  }
}


/*
  Link table back that was unlinked with unlink_first_table()

  SYNOPSIS
    link_first_table_back()
    link_to_local        do we need link this table to local

  RETURN
    global list
*/

void LEX::link_first_table_back(TABLE_LIST *first,
                                bool link_to_local)
{
  if (first)
  {
    if ((first->next_global= query_tables))
      query_tables->prev_global= &first->next_global;
    else
      query_tables_last= &first->next_global;
    query_tables= first;

    if (link_to_local)
    {
      first->next_local= first_select_lex()->table_list.first;
      first_select_lex()->context.table_list= first;
      first_select_lex()->table_list.first= first;
      first_select_lex()->table_list.elements++; //safety
    }
  }
}



/*
  cleanup lex for case when we open table by table for processing

  SYNOPSIS
    LEX::cleanup_after_one_table_open()

  NOTE
    This method is mostly responsible for cleaning up of selects lists and
    derived tables state. To rollback changes in Query_tables_list one has
    to call Query_tables_list::reset_query_tables_list(FALSE).
*/

void LEX::cleanup_after_one_table_open()
{
  /*
    thd->lex->derived_tables & additional units may be set if we open
    a view. It is necessary to clear thd->lex->derived_tables flag
    to prevent processing of derived tables during next open_and_lock_tables
    if next table is a real table and cleanup & remove underlying units
    NOTE: all units will be connected to thd->lex->select_lex, because we
    have not UNION on most upper level.
    */
  if (all_selects_list != first_select_lex())
  {
    derived_tables= 0;
    first_select_lex()->exclude_from_table_unique_test= false;
    /* cleunup underlying units (units of VIEW) */
    for (SELECT_LEX_UNIT *un= first_select_lex()->first_inner_unit();
         un;
         un= un->next_unit())
      un->cleanup();
    /* reduce all selects list to default state */
    all_selects_list= first_select_lex();
    /* remove underlying units (units of VIEW) subtree */
    first_select_lex()->cut_subtree();
  }
}


/*
  Save current state of Query_tables_list for this LEX, and prepare it
  for processing of new statemnt.

  SYNOPSIS
    reset_n_backup_query_tables_list()
      backup  Pointer to Query_tables_list instance to be used for backup
*/

void LEX::reset_n_backup_query_tables_list(Query_tables_list *backup)
{
  backup->set_query_tables_list(this);
  /*
    We have to perform full initialization here since otherwise we
    will damage backed up state.
  */
  reset_query_tables_list(TRUE);
}


/*
  Restore state of Query_tables_list for this LEX from backup.

  SYNOPSIS
    restore_backup_query_tables_list()
      backup  Pointer to Query_tables_list instance used for backup
*/

void LEX::restore_backup_query_tables_list(Query_tables_list *backup)
{
  destroy_query_tables_list();
  set_query_tables_list(backup);
}


/*
  Checks for usage of routines and/or tables in a parsed statement

  SYNOPSIS
    LEX:table_or_sp_used()

  RETURN
    FALSE  No routines and tables used
    TRUE   Either or both routines and tables are used.
*/

bool LEX::table_or_sp_used()
{
  DBUG_ENTER("table_or_sp_used");

  if (sroutines.records || query_tables)
    DBUG_RETURN(TRUE);

  DBUG_RETURN(FALSE);
}


/*
  Do end-of-prepare fixup for list of tables and their merge-VIEWed tables

  SYNOPSIS
    fix_prepare_info_in_table_list()
      thd  Thread handle
      tbl  List of tables to process

  DESCRIPTION
    Perform end-end-of prepare fixup for list of tables, if any of the tables
    is a merge-algorithm VIEW, recursively fix up its underlying tables as
    well.

*/

static void fix_prepare_info_in_table_list(THD *thd, TABLE_LIST *tbl)
{
  for (; tbl; tbl= tbl->next_local)
  {
    if (tbl->on_expr && !tbl->prep_on_expr)
    {
      thd->check_and_register_item_tree(&tbl->prep_on_expr, &tbl->on_expr);
      tbl->on_expr= tbl->on_expr->copy_andor_structure(thd);
    }
    if (tbl->is_view_or_derived() && tbl->is_merged_derived())
    {
      SELECT_LEX *sel= tbl->get_single_select();
      fix_prepare_info_in_table_list(thd, sel->get_table_list());
    }
  }
}


/*
  Save WHERE/HAVING/ON clauses and replace them with disposable copies

  SYNOPSIS
    st_select_lex::fix_prepare_information
      thd          thread handler
      conds        in/out pointer to WHERE condition to be met at execution
      having_conds in/out pointer to HAVING condition to be met at execution
  
  DESCRIPTION
    The passed WHERE and HAVING are to be saved for the future executions.
    This function saves it, and returns a copy which can be thrashed during
    this execution of the statement. By saving/thrashing here we mean only
    We also save the chain of ORDER::next in group_list, in case
    the list is modified by remove_const().
    AND/OR trees.
    The function also calls fix_prepare_info_in_table_list that saves all
    ON expressions.    
*/

void st_select_lex::fix_prepare_information(THD *thd, Item **conds, 
                                            Item **having_conds)
{
  DBUG_ENTER("st_select_lex::fix_prepare_information");
  if (!thd->stmt_arena->is_conventional() &&
      !(changed_elements & TOUCHED_SEL_COND))
  {
    Query_arena_stmt on_stmt_arena(thd);
    changed_elements|= TOUCHED_SEL_COND;
    if (group_list.first)
    {
      if (!group_list_ptrs)
      {
        void *mem= thd->stmt_arena->alloc(sizeof(Group_list_ptrs));
        group_list_ptrs= new (mem) Group_list_ptrs(thd->stmt_arena->mem_root);
      }
      group_list_ptrs->reserve(group_list.elements);
      for (ORDER *order= group_list.first; order; order= order->next)
      {
        group_list_ptrs->push_back(order);
      }
    }
    if (*conds)
    {
      thd->check_and_register_item_tree(&prep_where, conds);
      *conds= where= prep_where->copy_andor_structure(thd);
    }
    if (*having_conds)
    {
      thd->check_and_register_item_tree(&prep_having, having_conds);
      *having_conds= having= prep_having->copy_andor_structure(thd);
    }
    fix_prepare_info_in_table_list(thd, table_list.first);
  }
  DBUG_VOID_RETURN;
}


/*
  There are st_select_lex::add_table_to_list &
  st_select_lex::set_lock_for_tables are in sql_parse.cc

  st_select_lex::print is in sql_select.cc

  st_select_lex_unit::prepare, st_select_lex_unit::exec,
  st_select_lex_unit::cleanup, st_select_lex_unit::reinit_exec_mechanism,
  st_select_lex_unit::change_result
  are in sql_union.cc
*/

/*
  Sets the kind of hints to be added by the calls to add_index_hint().

  SYNOPSIS
    set_index_hint_type()
      type_arg     The kind of hints to be added from now on.
      clause       The clause to use for hints to be added from now on.

  DESCRIPTION
    Used in filling up the tagged hints list.
    This list is filled by first setting the kind of the hint as a 
    context variable and then adding hints of the current kind.
    Then the context variable index_hint_type can be reset to the
    next hint type.
*/
void st_select_lex::set_index_hint_type(enum index_hint_type type_arg,
                                        index_clause_map clause)
{ 
  current_index_hint_type= type_arg;
  current_index_hint_clause= clause;
}


/*
  Makes an array to store index usage hints (ADD/FORCE/IGNORE INDEX).

  SYNOPSIS
    alloc_index_hints()
      thd         current thread.
*/

void st_select_lex::alloc_index_hints (THD *thd)
{ 
  index_hints= new (thd->mem_root) List<Index_hint>(); 
}



/*
  adds an element to the array storing index usage hints 
  (ADD/FORCE/IGNORE INDEX).

  SYNOPSIS
    add_index_hint()
      thd         current thread.
      str         name of the index.
      length      number of characters in str.

  RETURN VALUE
    0 on success, non-zero otherwise
*/
bool st_select_lex::add_index_hint (THD *thd, const char *str, size_t length)
{
  return index_hints->push_front(new (thd->mem_root) 
                                 Index_hint(current_index_hint_type,
                                            current_index_hint_clause,
                                            str, length), thd->mem_root);
}


/**
  Optimize all subqueries that have not been flattened into semi-joins.

  @details
  This functionality is a method of SELECT_LEX instead of JOIN because
  SQL statements as DELETE/UPDATE do not have a corresponding JOIN object.

  @see JOIN::optimize_unflattened_subqueries

  @param const_only  Restrict subquery optimization to constant subqueries

  @return Operation status
  @retval FALSE     success.
  @retval TRUE      error occurred.
*/

bool st_select_lex::optimize_unflattened_subqueries(bool const_only)
{
  SELECT_LEX_UNIT *next_unit= NULL;
  for (SELECT_LEX_UNIT *un= first_inner_unit();
       un;
       un= next_unit ? next_unit : un->next_unit())
  {
    Item_subselect *subquery_predicate= un->item;
    next_unit= NULL;

    if (subquery_predicate)
    {
      if (!subquery_predicate->fixed)
      {
        /*
         This subquery was excluded as part of some expression so it is
         invisible from all prepared expression.
       */
        next_unit= un->next_unit();
        un->exclude_level();
        if (next_unit)
          continue;
        break;
      }
      if (subquery_predicate->substype() == Item_subselect::IN_SUBS)
      {
        Item_in_subselect *in_subs= (Item_in_subselect*) subquery_predicate;
        if (in_subs->is_jtbm_merged)
          continue;
      }

      if (const_only && !subquery_predicate->const_item())
      {
        /* Skip non-constant subqueries if the caller asked so. */
        continue;
      }

      bool empty_union_result= true;
      bool is_correlated_unit= false;
      bool first= true;
      bool union_plan_saved= false;
      /*
        If the subquery is a UNION, optimize all the subqueries in the UNION. If
        there is no UNION, then the loop will execute once for the subquery.
      */
      for (SELECT_LEX *sl= un->first_select(); sl; sl= sl->next_select())
      {
        JOIN *inner_join= sl->join;
        if (first)
          first= false;
        else
        {
          if (!union_plan_saved)
          {
            union_plan_saved= true;
            if (un->save_union_explain(un->thd->lex->explain))
              return true; /* Failure */
          }
        }
        if (!inner_join)
          continue;
        SELECT_LEX *save_select= un->thd->lex->current_select;
        ulonglong save_options;
        int res;
        /* We need only 1 row to determine existence */
        un->set_limit(un->global_parameters());
        un->thd->lex->current_select= sl;
        save_options= inner_join->select_options;
        if (options & SELECT_DESCRIBE)
        {
          /* Optimize the subquery in the context of EXPLAIN. */
          sl->set_explain_type(FALSE);
          sl->options|= SELECT_DESCRIBE;
          inner_join->select_options|= SELECT_DESCRIBE;
        }
        res= inner_join->optimize();
        if (!inner_join->cleaned)
          sl->update_used_tables();
        sl->update_correlated_cache();
        is_correlated_unit|= sl->is_correlated;
        inner_join->select_options= save_options;
        un->thd->lex->current_select= save_select;

        Explain_query *eq;
        if ((eq= inner_join->thd->lex->explain))
        {
          Explain_select *expl_sel;
          if ((expl_sel= eq->get_select(inner_join->select_lex->select_number)))
          {
            sl->set_explain_type(TRUE);
            expl_sel->select_type= sl->type;
          }
        }

        if (empty_union_result)
        {
          /*
            If at least one subquery in a union is non-empty, the UNION result
            is non-empty. If there is no UNION, the only subquery is non-empy.
          */
          empty_union_result= inner_join->empty_result();
        }
        if (res)
          return TRUE;
      }
      if (empty_union_result)
        subquery_predicate->no_rows_in_result();
      if (!is_correlated_unit)
        un->uncacheable&= ~UNCACHEABLE_DEPENDENT;
      subquery_predicate->is_correlated= is_correlated_unit;
    }
  }
  return FALSE;
}



/**
  @brief Process all derived tables/views of the SELECT.

  @param lex    LEX of this thread
  @param phase  phases to run derived tables/views through

  @details
  This function runs specified 'phases' on all tables from the
  table_list of this select.

  @return FALSE ok.
  @return TRUE an error occur.
*/

bool st_select_lex::handle_derived(LEX *lex, uint phases)
{
  return lex->handle_list_of_derived(table_list.first, phases);
}


/**
  @brief
  Returns first unoccupied table map and table number

  @param map     [out] return found map
  @param tablenr [out] return found tablenr

  @details
  Returns first unoccupied table map and table number in this select.
  Map and table are returned in *'map' and *'tablenr' accordingly.

  @retrun TRUE  no free table map/table number
  @return FALSE found free table map/table number
*/

bool st_select_lex::get_free_table_map(table_map *map, uint *tablenr)
{
  *map= 0;
  *tablenr= 0;
  TABLE_LIST *tl;
  List_iterator<TABLE_LIST> ti(leaf_tables);
  while ((tl= ti++))
  {
    if (tl->table->map > *map)
      *map= tl->table->map;
    if (tl->table->tablenr > *tablenr)
      *tablenr= tl->table->tablenr;
  }
  (*map)<<= 1;
  (*tablenr)++;
  if (*tablenr >= MAX_TABLES)
    return TRUE;
  return FALSE;
}


/**
  @brief
  Append given table to the leaf_tables list.

  @param link  Offset to which list in table structure to use
  @param table Table to append

  @details
  Append given 'table' to the leaf_tables list using the 'link' offset.
  If the 'table' is linked with other tables through next_leaf/next_local
  chains then whole list will be appended.
*/

void st_select_lex::append_table_to_list(TABLE_LIST *TABLE_LIST::*link,
                                         TABLE_LIST *table)
{
  TABLE_LIST *tl;
  for (tl= leaf_tables.head(); tl->*link; tl= tl->*link) ;
  tl->*link= table;
}


/*
  @brief
  Replace given table from the leaf_tables list for a list of tables 

  @param table Table to replace
  @param list  List to substititute the table for

  @details
  Replace 'table' from the leaf_tables list for a list of tables 'tbl_list'.
*/

void st_select_lex::replace_leaf_table(TABLE_LIST *table, List<TABLE_LIST> &tbl_list)
{
  TABLE_LIST *tl;
  List_iterator<TABLE_LIST> ti(leaf_tables);
  while ((tl= ti++))
  {
    if (tl == table)
    {
      ti.replace(tbl_list);
      break;
    }
  }
}


/**
  @brief
  Assigns new table maps to tables in the leaf_tables list

  @param derived    Derived table to take initial table map from
  @param map        table map to begin with
  @param tablenr    table number to begin with
  @param parent_lex new parent select_lex

  @details
  Assign new table maps/table numbers to all tables in the leaf_tables list.
  'map'/'tablenr' are used for the first table and shifted to left/
  increased for each consequent table in the leaf_tables list.
  If the 'derived' table is given then it's table map/number is used for the
  first table in the list and 'map'/'tablenr' are used for the second and
  all consequent tables.
  The 'parent_lex' is set as the new parent select_lex for all tables in the
  list.
*/

void st_select_lex::remap_tables(TABLE_LIST *derived, table_map map,
                                 uint tablenr, SELECT_LEX *parent_lex)
{
  bool first_table= TRUE;
  TABLE_LIST *tl;
  table_map first_map;
  uint first_tablenr;

  if (derived && derived->table)
  {
    first_map= derived->table->map;
    first_tablenr= derived->table->tablenr;
  }
  else
  {
    first_map= map;
    map<<= 1;
    first_tablenr= tablenr++;
  }
  /*
    Assign table bit/table number.
    To the first table of the subselect the table bit/tablenr of the
    derived table is assigned. The rest of tables are getting bits
    sequentially, starting from the provided table map/tablenr.
  */
  List_iterator<TABLE_LIST> ti(leaf_tables);
  while ((tl= ti++))
  {
    if (first_table)
    {
      first_table= FALSE;
      tl->table->set_table_map(first_map, first_tablenr);
    }
    else
    {
      tl->table->set_table_map(map, tablenr);
      tablenr++;
      map<<= 1;
    }
    SELECT_LEX *old_sl= tl->select_lex;
    tl->select_lex= parent_lex;
    for(TABLE_LIST *emb= tl->embedding;
        emb && emb->select_lex == old_sl;
        emb= emb->embedding)
      emb->select_lex= parent_lex;
  }
}

/**
  @brief
  Merge a subquery into this select.

  @param derived     derived table of the subquery to be merged
  @param subq_select select_lex of the subquery
  @param map         table map for assigning to merged tables from subquery
  @param table_no    table number for assigning to merged tables from subquery

  @details
  This function merges a subquery into its parent select. In short the
  merge operation appends the subquery FROM table list to the parent's
  FROM table list. In more details:
    .) the top_join_list of the subquery is wrapped into a join_nest
       and attached to 'derived'
    .) subquery's leaf_tables list  is merged with the leaf_tables
       list of this select_lex
    .) the table maps and table numbers of the tables merged from
       the subquery are adjusted to reflect their new binding to
       this select

  @return TRUE  an error occur
  @return FALSE ok
*/

bool SELECT_LEX::merge_subquery(THD *thd, TABLE_LIST *derived,
                                SELECT_LEX *subq_select,
                                uint table_no, table_map map)
{
  derived->wrap_into_nested_join(subq_select->top_join_list);

  ftfunc_list->append(subq_select->ftfunc_list);
  if (join ||
      thd->lex->sql_command == SQLCOM_UPDATE_MULTI ||
      thd->lex->sql_command == SQLCOM_DELETE_MULTI)
  {
    List_iterator_fast<Item_in_subselect> li(subq_select->sj_subselects);
    Item_in_subselect *in_subq;
    while ((in_subq= li++))
    {
      sj_subselects.push_back(in_subq, thd->mem_root);
      if (in_subq->emb_on_expr_nest == NO_JOIN_NEST)
         in_subq->emb_on_expr_nest= derived;
    }

    uint cnt= sizeof(expr_cache_may_be_used)/sizeof(bool);
    for (uint i= 0; i < cnt; i++)
    {
      if (subq_select->expr_cache_may_be_used[i])
        expr_cache_may_be_used[i]= true;
    }

    List_iterator_fast<Item_func_in> it(subq_select->in_funcs);
    Item_func_in *in_func;
    while ((in_func= it++))
    {
      in_funcs.push_back(in_func, thd->mem_root);
      if (in_func->emb_on_expr_nest == NO_JOIN_NEST)
        in_func->emb_on_expr_nest= derived;
    }
  }

  /* Walk through child's tables and adjust table map, tablenr,
   * parent_lex */
  subq_select->remap_tables(derived, map, table_no, this);
  subq_select->merged_into= this;

  replace_leaf_table(derived, subq_select->leaf_tables);

  return FALSE;
}


/**
  @brief
  Mark tables from the leaf_tables list as belong to a derived table.

  @param derived   tables will be marked as belonging to this derived

  @details
  Run through the leaf_list and mark all tables as belonging to the 'derived'.
*/

void SELECT_LEX::mark_as_belong_to_derived(TABLE_LIST *derived)
{
  /* Mark tables as belonging to this DT */
  TABLE_LIST *tl;
  List_iterator<TABLE_LIST> ti(leaf_tables);
  while ((tl= ti++))
    tl->belong_to_derived= derived;
}


/**
  @brief
  Update used_tables cache for this select

  @details
  This function updates used_tables cache of ON expressions of all tables
  in the leaf_tables list and of the conds expression (if any).
*/

void SELECT_LEX::update_used_tables()
{
  TABLE_LIST *tl;
  List_iterator<TABLE_LIST> ti(leaf_tables);

  while ((tl= ti++))
  {
    if (tl->table && !tl->is_view_or_derived())
    {
      TABLE_LIST *embedding= tl->embedding;
      for (embedding= tl->embedding; embedding; embedding=embedding->embedding)
      {
        if (embedding->is_view_or_derived())
        {
          DBUG_ASSERT(embedding->is_merged_derived());
          TABLE *tab= tl->table;
          tab->covering_keys= tab->s->keys_for_keyread;
          tab->covering_keys.intersect(tab->keys_in_use_for_query);
          /*
            View/derived was merged. Need to recalculate read_set
            bitmaps here. For example:
              CREATE VIEW v1 AS SELECT f1,f2,f3 FROM t1;
              SELECT f1 FROM v1;
            Initially, the view definition will put all f1,f2,f3 in the
            read_set for t1. But after the view is merged, only f1 should
            be in the read_set.
          */
          bitmap_clear_all(tab->read_set);
          break;
        }
      }
    }
  }

  ti.rewind();
  while ((tl= ti++))
  {
    TABLE_LIST *embedding= tl;
    do
    {
      bool maybe_null;
      if ((maybe_null= MY_TEST(embedding->outer_join)))
      {
        tl->table->maybe_null= maybe_null;
        break;
      }
    }
    while ((embedding= embedding->embedding));
    if (tl->on_expr)
    {
      tl->on_expr->update_used_tables();
      tl->on_expr->walk(&Item::eval_not_null_tables, 0, NULL);
    }
    /*
      - There is no need to check sj_on_expr, because merged semi-joins inject
        sj_on_expr into the parent's WHERE clase.
      - For non-merged semi-joins (aka JTBMs), we need to check their
        left_expr. There is no need to check the rest of the subselect, we know
        it is uncorrelated and so cannot refer to any tables in this select.
    */
    if (tl->jtbm_subselect)
    {
      Item *left_expr= tl->jtbm_subselect->left_expr;
      left_expr->walk(&Item::update_table_bitmaps_processor, FALSE, NULL);
    }

    embedding= tl->embedding;
    while (embedding)
    {
      if (embedding->on_expr && 
          embedding->nested_join->join_list.head() == tl)
      {
        embedding->on_expr->update_used_tables();
        embedding->on_expr->walk(&Item::eval_not_null_tables, 0, NULL);
      }
      tl= embedding;
      embedding= tl->embedding;
    }
  }

  if (join->conds)
  {
    join->conds->update_used_tables();
    join->conds->walk(&Item::eval_not_null_tables, 0, NULL);
  }
  if (join->having)
  {
    join->having->update_used_tables();
  }

  Item *item;
  List_iterator_fast<Item> it(join->all_fields);
  select_list_tables= 0;
  while ((item= it++))
  {
    item->update_used_tables();
    select_list_tables|= item->used_tables();
  }
  Item_outer_ref *ref;
  List_iterator_fast<Item_outer_ref> ref_it(inner_refs_list);
  while ((ref= ref_it++))
  {
    item= ref->outer_ref;
    item->update_used_tables();
  }
  for (ORDER *order= group_list.first; order; order= order->next)
    (*order->item)->update_used_tables();
  if (!master_unit()->is_unit_op() ||
      master_unit()->global_parameters() != this)
  {
    for (ORDER *order= order_list.first; order; order= order->next)
      (*order->item)->update_used_tables();
  }
  join->result->update_used_tables();
}


/**
  @brief
  Update is_correlated cache for this select

  @details
*/

void st_select_lex::update_correlated_cache()
{
  TABLE_LIST *tl;
  List_iterator<TABLE_LIST> ti(leaf_tables);

  is_correlated= false;

  while ((tl= ti++))
  {
    //    is_correlated|= tl->is_with_table_recursive_reference();
    if (tl->on_expr)
      is_correlated|= MY_TEST(tl->on_expr->used_tables() & OUTER_REF_TABLE_BIT);
    for (TABLE_LIST *embedding= tl->embedding ; embedding ;
         embedding= embedding->embedding)
    {
      if (embedding->on_expr)
        is_correlated|= MY_TEST(embedding->on_expr->used_tables() &
                                OUTER_REF_TABLE_BIT);
    }
  }

  if (join->conds)
    is_correlated|= MY_TEST(join->conds->used_tables() & OUTER_REF_TABLE_BIT);

  is_correlated|= join->having_is_correlated;

  if (join->having)
    is_correlated|= MY_TEST(join->having->used_tables() & OUTER_REF_TABLE_BIT);

  if (join->tmp_having)
    is_correlated|= MY_TEST(join->tmp_having->used_tables() &
                            OUTER_REF_TABLE_BIT);

  Item *item;
  List_iterator_fast<Item> it(join->fields_list);
  while ((item= it++))
    is_correlated|= MY_TEST(item->used_tables() & OUTER_REF_TABLE_BIT);

  for (ORDER *order= group_list.first; order; order= order->next)
    is_correlated|= MY_TEST((*order->item)->used_tables() &
                            OUTER_REF_TABLE_BIT);

  if (!master_unit()->is_unit_op())
  {
    for (ORDER *order= order_list.first; order; order= order->next)
      is_correlated|= MY_TEST((*order->item)->used_tables() &
                              OUTER_REF_TABLE_BIT);
  }

  if (!is_correlated)
    uncacheable&= ~UNCACHEABLE_DEPENDENT;
}


/**
  Set the EXPLAIN type for this subquery.
  
  @param on_the_fly  TRUE<=> We're running a SHOW EXPLAIN command, so we must 
                     not change any variables
*/

void st_select_lex::set_explain_type(bool on_the_fly)
{
  bool is_primary= FALSE;
  if (next_select())
    is_primary= TRUE;

  if (!is_primary && first_inner_unit())
  {
    /*
      If there is at least one materialized derived|view then it's a PRIMARY select.
      Otherwise, all derived tables/views were merged and this select is a SIMPLE one.
    */
    for (SELECT_LEX_UNIT *un= first_inner_unit(); un; un= un->next_unit())
    {
      if ((!un->derived || un->derived->is_materialized_derived()))
      {
        is_primary= TRUE;
        break;
      }
    }
  }

  if (on_the_fly && !is_primary && have_merged_subqueries)
    is_primary= TRUE;

  SELECT_LEX *first= master_unit()->first_select();
  /* drop UNCACHEABLE_EXPLAIN, because it is for internal usage only */
  uint8 is_uncacheable= (uncacheable & ~UNCACHEABLE_EXPLAIN);
  
  bool using_materialization= FALSE;
  Item_subselect *parent_item;
  if ((parent_item= master_unit()->item) &&
      parent_item->substype() == Item_subselect::IN_SUBS)
  {
    Item_in_subselect *in_subs= (Item_in_subselect*)parent_item;
    /*
      Surprisingly, in_subs->is_set_strategy() can return FALSE here,
      even for the last invocation of this function for the select.
    */
    if (in_subs->test_strategy(SUBS_MATERIALIZATION))
      using_materialization= TRUE;
  }

  if (master_unit()->thd->lex->first_select_lex() == this)
  {
    if (pushdown_select)
      type= pushed_select_text;
    else
      type= is_primary ? "PRIMARY" : "SIMPLE";
  }
  else
  {
    if (this == first)
    {
      /* If we're a direct child of a UNION, we're the first sibling there */
      if (linkage == DERIVED_TABLE_TYPE)
      {
        bool is_pushed_master_unit= master_unit()->derived &&
	                            master_unit()->derived->pushdown_derived;
        if (is_pushed_master_unit)
          type= pushed_derived_text;
        else if (is_uncacheable & UNCACHEABLE_DEPENDENT)
          type= "LATERAL DERIVED";
        else
          type= "DERIVED";
      }
      else if (using_materialization)
        type= "MATERIALIZED";
      else
      {
         if (is_uncacheable & UNCACHEABLE_DEPENDENT)
           type= "DEPENDENT SUBQUERY";
         else
         {
           type= is_uncacheable? "UNCACHEABLE SUBQUERY" :
                                 "SUBQUERY";
         }
      }
    }
    else
    {
      switch (linkage)
      {
      case INTERSECT_TYPE:
        type= "INTERSECT";
        break;
      case EXCEPT_TYPE:
        type= "EXCEPT";
        break;
      default:
        /* This a non-first sibling in UNION */
        if (is_uncacheable & UNCACHEABLE_DEPENDENT)
          type= "DEPENDENT UNION";
        else if (using_materialization)
          type= "MATERIALIZED UNION";
        else
        {
          type= is_uncacheable ? "UNCACHEABLE UNION": "UNION";
          if (this == master_unit()->fake_select_lex)
            type= unit_operation_text[master_unit()->common_op()];
          /*
            join below may be =NULL when this functions is called at an early
            stage. It will be later called again and we will set the correct
            value.
          */
          if (join)
          {
            bool uses_cte= false;
            for (JOIN_TAB *tab= first_linear_tab(join, WITHOUT_BUSH_ROOTS,
                                                       WITH_CONST_TABLES);
                 tab;
                 tab= next_linear_tab(join, tab, WITHOUT_BUSH_ROOTS))
            {
              /*
                pos_in_table_list=NULL for e.g. post-join aggregation JOIN_TABs.
              */
              if (tab->table && tab->table->pos_in_table_list &&
                  tab->table->pos_in_table_list->with &&
                  tab->table->pos_in_table_list->with->is_recursive)
              {
                uses_cte= true;
                break;
              }
            }
            if (uses_cte)
              type= "RECURSIVE UNION";
          }
        }
        break;
      }
    }
  }

  if (!on_the_fly)
    options|= SELECT_DESCRIBE;
}


/**
  @brief
  Increase estimated number of records for a derived table/view

  @param records  number of records to increase estimate by

  @details
  This function increases estimated number of records by the 'records'
  for the derived table to which this select belongs to.
*/

void SELECT_LEX::increase_derived_records(ha_rows records)
{
  SELECT_LEX_UNIT *unit= master_unit();
  DBUG_ASSERT(unit->derived);

  if (unit->with_element && unit->with_element->is_recursive)
  {
    st_select_lex *first_recursive= unit->with_element->first_recursive;
    st_select_lex *sl= unit->first_select();
    for ( ; sl != first_recursive; sl= sl->next_select())
    {
      if (sl == this)
        break;
    }
    if (sl == first_recursive)
      return; 
  }
  
  select_result *result= unit->result;
  switch (linkage)
  {
  case INTERSECT_TYPE:
    // result of intersect can't be more then one of components
    set_if_smaller(result->est_records, records);
  case EXCEPT_TYPE:
    // in worse case none of record will be removed
    break;
  default:
    // usual UNION
    if (HA_ROWS_MAX - records > result->est_records)
      result->est_records+= records;
    else
      result->est_records= HA_ROWS_MAX;
    break;
  }
}


/**
  @brief
  Mark select's derived table as a const one.

  @param empty Whether select has an empty result set

  @details
  Mark derived table/view of this select as a constant one (to
  materialize it at the optimization phase) unless this select belongs to a
  union. Estimated number of rows is incremented if this select has non empty
  result set.
*/

void SELECT_LEX::mark_const_derived(bool empty)
{
  TABLE_LIST *derived= master_unit()->derived;
  /* join == NULL in  DELETE ... RETURNING */
  if (!(join && join->thd->lex->describe) && derived)
  {
    if (!empty)
      increase_derived_records(1);
    if (!master_unit()->is_unit_op() && !derived->is_merged_derived() &&
        !(join && join->with_two_phase_optimization))
      derived->fill_me= TRUE;
  }
}


bool st_select_lex::save_leaf_tables(THD *thd)
{
  Query_arena *arena, backup;
  arena= thd->activate_stmt_arena_if_needed(&backup);

  List_iterator_fast<TABLE_LIST> li(leaf_tables);
  TABLE_LIST *table;
  while ((table= li++))
  {
    if (leaf_tables_exec.push_back(table, thd->mem_root))
      return 1;
    table->tablenr_exec= table->get_tablenr();
    table->map_exec= table->get_map();
    if (join && (join->select_options & SELECT_DESCRIBE))
      table->maybe_null_exec= 0;
    else
      table->maybe_null_exec= table->table?  table->table->maybe_null: 0;
  }
  if (arena)
    thd->restore_active_arena(arena, &backup);

  return 0;
}


bool LEX::save_prep_leaf_tables()
{
  if (!thd->save_prep_leaf_list)
    return FALSE;

  Query_arena *arena= thd->stmt_arena, backup;
  arena= thd->activate_stmt_arena_if_needed(&backup);
  //It is used for DETETE/UPDATE so top level has only one SELECT
  DBUG_ASSERT(first_select_lex()->next_select() == NULL);
  bool res= first_select_lex()->save_prep_leaf_tables(thd);

  if (arena)
    thd->restore_active_arena(arena, &backup);

  if (res)
    return TRUE;

  thd->save_prep_leaf_list= FALSE;
  return FALSE;
}


bool st_select_lex::save_prep_leaf_tables(THD *thd)
{
  List_iterator_fast<TABLE_LIST> li(leaf_tables);
  TABLE_LIST *table;

  /*
    Check that the SELECT_LEX was really prepared and so tables are setup.

    It can be subquery in SET clause of UPDATE which was not prepared yet, so
    its tables are not yet setup and ready for storing.
  */
  if (prep_leaf_list_state != READY)
    return FALSE;

  while ((table= li++))
  {
    if (leaf_tables_prep.push_back(table))
      return TRUE;
  }
  prep_leaf_list_state= SAVED;
  for (SELECT_LEX_UNIT *u= first_inner_unit(); u; u= u->next_unit())
  {
    for (SELECT_LEX *sl= u->first_select(); sl; sl= sl->next_select())
    {
      if (sl->save_prep_leaf_tables(thd))
        return TRUE;
    }
  }

  return FALSE;
}


/*
  Return true if this select_lex has been converted into a semi-join nest
  within 'ancestor'.

  We need a loop to check this because there could be several nested
  subselects, like

    SELECT ... FROM grand_parent 
      WHERE expr1 IN (SELECT ... FROM parent 
                        WHERE expr2 IN ( SELECT ... FROM child)

  which were converted into:
  
    SELECT ... 
    FROM grand_parent SEMI_JOIN (parent JOIN child) 
    WHERE 
      expr1 AND expr2

  In this case, both parent and child selects were merged into the parent.
*/

bool st_select_lex::is_merged_child_of(st_select_lex *ancestor)
{
  bool all_merged= TRUE;
  for (SELECT_LEX *sl= this; sl && sl!=ancestor;
       sl=sl->outer_select())
  {
    Item *subs= sl->master_unit()->item;
    if (subs && subs->type() == Item::SUBSELECT_ITEM && 
        ((Item_subselect*)subs)->substype() == Item_subselect::IN_SUBS &&
        ((Item_in_subselect*)subs)->test_strategy(SUBS_SEMI_JOIN))
    {
      continue;
    }

    if (sl->master_unit()->derived &&
      sl->master_unit()->derived->is_merged_derived())
    {
      continue;
    }
    all_merged= FALSE;
    break;
  }
  return all_merged;
}

/* 
  This is used by SHOW EXPLAIN. It assuses query plan has been already 
  collected into QPF structures and we only need to print it out.
*/

int LEX::print_explain(select_result_sink *output, uint8 explain_flags,
                       bool is_analyze, bool *printed_anything)
{
  int res;
  if (explain && explain->have_query_plan())
  {
    res= explain->print_explain(output, explain_flags, is_analyze);
    *printed_anything= true;
  }
  else
  {
    res= 0;
    *printed_anything= false;
  }
  return res;
}


/**
  Allocates and set arena for SET STATEMENT old values.

  @param backup          where to save backup of arena.

  @retval 1 Error
  @retval 0 OK
*/

bool LEX::set_arena_for_set_stmt(Query_arena *backup)
{
  DBUG_ENTER("LEX::set_arena_for_set_stmt");
  DBUG_ASSERT(arena_for_set_stmt== 0);
  if (!mem_root_for_set_stmt)
  {
    mem_root_for_set_stmt= new MEM_ROOT();
    if (unlikely(!(mem_root_for_set_stmt)))
      DBUG_RETURN(1);
    init_sql_alloc(PSI_INSTRUMENT_ME, mem_root_for_set_stmt, ALLOC_ROOT_SET,
                   ALLOC_ROOT_SET, MYF(MY_THREAD_SPECIFIC));
  }
  if (unlikely(!(arena_for_set_stmt= new(mem_root_for_set_stmt)
                 Query_arena_memroot(mem_root_for_set_stmt,
                                     Query_arena::STMT_INITIALIZED))))
    DBUG_RETURN(1);
  DBUG_PRINT("info", ("mem_root: %p  arena: %p",
                      mem_root_for_set_stmt,
                      arena_for_set_stmt));
  thd->set_n_backup_active_arena(arena_for_set_stmt, backup);
  DBUG_RETURN(0);
}


void LEX::reset_arena_for_set_stmt(Query_arena *backup)
{
  DBUG_ENTER("LEX::reset_arena_for_set_stmt");
  DBUG_ASSERT(arena_for_set_stmt);
  thd->restore_active_arena(arena_for_set_stmt, backup);
  DBUG_PRINT("info", ("mem_root: %p  arena: %p",
                      arena_for_set_stmt->mem_root,
                      arena_for_set_stmt));
  DBUG_VOID_RETURN;
}


void LEX::free_arena_for_set_stmt()
{
  DBUG_ENTER("LEX::free_arena_for_set_stmt");
  if (!arena_for_set_stmt)
    return;
  DBUG_PRINT("info", ("mem_root: %p  arena: %p",
                      arena_for_set_stmt->mem_root,
                      arena_for_set_stmt));
  arena_for_set_stmt->free_items();
  delete(arena_for_set_stmt);
  free_root(mem_root_for_set_stmt, MYF(MY_KEEP_PREALLOC));
  arena_for_set_stmt= 0;
  DBUG_VOID_RETURN;
}

void LEX::restore_set_statement_var()
{
  DBUG_ENTER("LEX::restore_set_statement_var");
  if (!old_var_list.is_empty())
  {
    DBUG_PRINT("info", ("vars: %d", old_var_list.elements));
    sql_set_variables(thd, &old_var_list, false);
    old_var_list.empty();
    free_arena_for_set_stmt();
  }
  DBUG_ASSERT(!is_arena_for_set_stmt());
  DBUG_VOID_RETURN;
}

unit_common_op st_select_lex_unit::common_op()
{
  SELECT_LEX *first= first_select();
  bool first_op= TRUE;
  unit_common_op operation= OP_MIX; // if no op
  for (SELECT_LEX *sl= first; sl; sl= sl->next_select())
  {
    if (sl != first)
    {
      unit_common_op op;
      switch (sl->linkage)
      {
      case INTERSECT_TYPE:
        op= OP_INTERSECT;
        break;
      case EXCEPT_TYPE:
        op= OP_EXCEPT;
        break;
      default:
        op= OP_UNION;
        break;
      }
      if (first_op)
      {
        operation= op;
        first_op= FALSE;
      }
      else
      {
        if (operation != op)
          operation= OP_MIX;
      }
    }
  }
  return operation;
}
/*
  Save explain structures of a UNION. The only variable member is whether the 
  union has "Using filesort".

  There is also save_union_explain_part2() function, which is called before we read
  UNION's output.

  The reason for it is examples like this:

     SELECT col1 FROM t1 UNION SELECT col2 FROM t2 ORDER BY (select ... from t3 ...)

  Here, the (select ... from t3 ...) subquery must be a child of UNION's
  st_select_lex. However, it is not connected as child until a very late 
  stage in execution.
*/

int st_select_lex_unit::save_union_explain(Explain_query *output)
{
  SELECT_LEX *first= first_select();

  if (output->get_union(first->select_number))
    return 0; /* Already added */
    
  Explain_union *eu= 
    new (output->mem_root) Explain_union(output->mem_root, 
                                         thd->lex->analyze_stmt);
  if (unlikely(!eu))
    return 0;

  if (with_element && with_element->is_recursive)
    eu->is_recursive_cte= true;
 
  if (derived)
    eu->connection_type= Explain_node::EXPLAIN_NODE_DERIVED;
  /* 
    Note: Non-merged semi-joins cannot be made out of UNIONs currently, so we
    don't ever set EXPLAIN_NODE_NON_MERGED_SJ.
  */
  for (SELECT_LEX *sl= first; sl; sl= sl->next_select())
    eu->add_select(sl->select_number);

  eu->fake_select_type= unit_operation_text[eu->operation= common_op()];
  eu->using_filesort= MY_TEST(global_parameters()->order_list.first);
  eu->using_tmp= union_needs_tmp_table();

  // Save the UNION node
  output->add_node(eu);

  if (eu->get_select_id() == 1)
    output->query_plan_ready();

  return 0;
}


/*
  @see  st_select_lex_unit::save_union_explain
*/

int st_select_lex_unit::save_union_explain_part2(Explain_query *output)
{
  Explain_union *eu= output->get_union(first_select()->select_number);
  if (fake_select_lex)
  {
    for (SELECT_LEX_UNIT *unit= fake_select_lex->first_inner_unit(); 
         unit; unit= unit->next_unit())
    {
      if (unit->explainable())
        eu->add_child(unit->first_select()->select_number);
    }
    fake_select_lex->join->explain= &eu->fake_select_lex_explain;
  }
  return 0;
}


/**
  A routine used by the parser to decide whether we are specifying a full
  partitioning or if only partitions to add or to split.

  @note  This needs to be outside of WITH_PARTITION_STORAGE_ENGINE since it
  is used from the sql parser that doesn't have any ifdef's

  @retval  TRUE    Yes, it is part of a management partition command
  @retval  FALSE          No, not a management partition command
*/

bool LEX::is_partition_management() const
{
  return (sql_command == SQLCOM_ALTER_TABLE &&
          (alter_info.partition_flags ==  ALTER_PARTITION_ADD ||
           alter_info.partition_flags ==  ALTER_PARTITION_REORGANIZE));
}


/**
  Exclude last added SELECT_LEX (current) in the UNIT and return pointer in it
  (previous become currect)

  @return detached SELECT_LEX or NULL in case of error
*/

SELECT_LEX *LEX::exclude_last_select()
{
  return exclude_not_first_select(current_select);
}

SELECT_LEX *LEX::exclude_not_first_select(SELECT_LEX *exclude)
{
  DBUG_ENTER("LEX::exclude_not_first_select");
  DBUG_PRINT("enter", ("exclude %p #%u", exclude, exclude->select_number));
  SELECT_LEX_UNIT *unit= exclude->master_unit();
  SELECT_LEX *sl;
  DBUG_ASSERT(unit->first_select() != exclude);
  /* we should go through the list to correctly set current_select */
  for(sl= unit->first_select();
      sl->next_select() && sl->next_select() != exclude;
      sl= sl->next_select());
  DBUG_PRINT("info", ("excl: %p  unit: %p  prev: %p", exclude, unit, sl));
  if (!sl)
    DBUG_RETURN(NULL);
  DBUG_ASSERT(&sl->next == exclude->prev);

  exclude->prev= NULL;

  current_select= sl;
  DBUG_RETURN(exclude);
}


SELECT_LEX_UNIT *LEX::alloc_unit()
{
  SELECT_LEX_UNIT *unit;
  DBUG_ENTER("LEX::alloc_unit");
  if (!(unit= new (thd->mem_root) SELECT_LEX_UNIT()))
    DBUG_RETURN(NULL);

  unit->init_query();
  /* TODO: reentrant problem */
  unit->thd= thd;
  unit->link_next= 0;
  unit->link_prev= 0;
  /* TODO: remove return_to */
  unit->return_to= NULL;
  DBUG_RETURN(unit);
}


SELECT_LEX *LEX::alloc_select(bool select)
{
  SELECT_LEX *select_lex;
  DBUG_ENTER("LEX::alloc_select");
  if (!(select_lex= new (thd->mem_root) SELECT_LEX()))
    DBUG_RETURN(NULL);
  DBUG_PRINT("info", ("Allocate select: %p #%u  statement lex: %p",
                      select_lex, thd->lex->stmt_lex->current_select_number,
                      thd->lex->stmt_lex));
  /*
    TODO: move following init to constructor when we get rid of builtin
    select
  */
  select_lex->select_number= ++thd->lex->stmt_lex->current_select_number;
  select_lex->parent_lex= this; /* Used in init_query. */
  select_lex->init_query();
  if (select)
    select_lex->init_select();
  select_lex->nest_level_base= &this->unit;
  select_lex->include_global((st_select_lex_node**)&all_selects_list);
  select_lex->context.resolve_in_select_list= TRUE;
  DBUG_RETURN(select_lex);
}

SELECT_LEX_UNIT *
LEX::create_unit(SELECT_LEX *first_sel)
{
  SELECT_LEX_UNIT *unit;
  DBUG_ENTER("LEX::create_unit");

  unit = first_sel->master_unit();

  if (!unit && !(unit= alloc_unit()))
    DBUG_RETURN(NULL);

  unit->register_select_chain(first_sel);
  if (first_sel->next_select())
  {
    unit->reset_distinct();
    DBUG_ASSERT(!unit->fake_select_lex);
    if (unit->add_fake_select_lex(thd))
      DBUG_RETURN(NULL);
  }
  DBUG_RETURN(unit);
}

SELECT_LEX_UNIT *
SELECT_LEX::attach_selects_chain(SELECT_LEX *first_sel,
                                 Name_resolution_context *context)
{
  SELECT_LEX_UNIT *unit;
  DBUG_ENTER("SELECT_LEX::attach_select_chain");

  if (!(unit= parent_lex->alloc_unit()))
    DBUG_RETURN(NULL);

  unit->register_select_chain(first_sel);
  register_unit(unit, context);
  if (first_sel->next_select())
  {
    unit->reset_distinct();
    DBUG_ASSERT(!unit->fake_select_lex);
    if (unit->add_fake_select_lex(parent_lex->thd))
      DBUG_RETURN(NULL);
  }

  DBUG_RETURN(unit);
}

SELECT_LEX *
LEX::wrap_unit_into_derived(SELECT_LEX_UNIT *unit)
{
  SELECT_LEX *wrapping_sel;
  Table_ident *ti;
  DBUG_ENTER("LEX::wrap_unit_into_derived");

  if (!(wrapping_sel= alloc_select(TRUE)))
    DBUG_RETURN(NULL);
  Name_resolution_context *context= &wrapping_sel->context;
  context->init();
  wrapping_sel->automatic_brackets= FALSE;
  wrapping_sel->mark_as_unit_nest();
  wrapping_sel->register_unit(unit, context);

  /* stuff dummy SELECT * FROM (...) */

  if (push_select(wrapping_sel)) // for Items & TABLE_LIST
    DBUG_RETURN(NULL);

  /* add SELECT list*/
  {
    Item *item= new (thd->mem_root) Item_field(thd, context, star_clex_str);
    if (item == NULL)
      goto err;
    if (add_item_to_list(thd, item))
      goto err;
    (wrapping_sel->with_wild)++;
  }

  unit->first_select()->set_linkage(DERIVED_TABLE_TYPE);

  ti= new (thd->mem_root) Table_ident(unit);
  if (ti == NULL)
    goto err;
  {
    TABLE_LIST *table_list;
    LEX_CSTRING alias;
    if (wrapping_sel->make_unique_derived_name(thd, &alias))
      goto err;

    if (!(table_list= wrapping_sel->add_table_to_list(thd, ti, &alias,
                                                      0, TL_READ,
                                                      MDL_SHARED_READ)))
      goto err;

    context->resolve_in_table_list_only(table_list);
    wrapping_sel->add_joined_table(table_list);
  }

  pop_select();

  derived_tables|= DERIVED_SUBQUERY;

  DBUG_RETURN(wrapping_sel);

err:
  pop_select();
  DBUG_RETURN(NULL);
}

SELECT_LEX *LEX::wrap_select_chain_into_derived(SELECT_LEX *sel)
{
  SELECT_LEX *dummy_select;
  SELECT_LEX_UNIT *unit;
  Table_ident *ti;
  DBUG_ENTER("LEX::wrap_select_chain_into_derived");

  if (!(dummy_select= alloc_select(TRUE)))
     DBUG_RETURN(NULL);
  Name_resolution_context *context= &dummy_select->context;
  dummy_select->automatic_brackets= FALSE;
  sel->distinct= TRUE; // First select has not this attribute (safety)

  if (!(unit= dummy_select->attach_selects_chain(sel, context)))
    DBUG_RETURN(NULL);

  /* stuff dummy SELECT * FROM (...) */

  if (push_select(dummy_select)) // for Items & TABLE_LIST
    DBUG_RETURN(NULL);

  /* add SELECT list*/
  {
    Item *item= new (thd->mem_root) Item_field(thd, context, star_clex_str);
    if (item == NULL)
      goto err;
    if (add_item_to_list(thd, item))
      goto err;
    (dummy_select->with_wild)++;
  }

  sel->set_linkage(DERIVED_TABLE_TYPE);

  ti= new (thd->mem_root) Table_ident(unit);
  if (ti == NULL)
    goto err;
  {
    TABLE_LIST *table_list;
    LEX_CSTRING alias;
    if (dummy_select->make_unique_derived_name(thd, &alias))
      goto err;

    if (!(table_list= dummy_select->add_table_to_list(thd, ti, &alias,
                                                      0, TL_READ,
                                                      MDL_SHARED_READ)))
      goto err;

    context->resolve_in_table_list_only(table_list);
    dummy_select->add_joined_table(table_list);
  }

  pop_select();

  derived_tables|= DERIVED_SUBQUERY;

  DBUG_RETURN(dummy_select);

err:
  pop_select();
  DBUG_RETURN(NULL);
}

bool LEX::push_context(Name_resolution_context *context)
{
  DBUG_ENTER("LEX::push_context");
  DBUG_PRINT("info", ("Context: %p Select: %p (%d)",
                       context, context->select_lex,
                       (context->select_lex ?
                        context->select_lex->select_number:
                        0)));
  bool res= context_stack.push_front(context, thd->mem_root);
  DBUG_RETURN(res);
}


Name_resolution_context *LEX::pop_context()
{
  DBUG_ENTER("LEX::pop_context");
  Name_resolution_context *context= context_stack.pop();
  DBUG_PRINT("info", ("Context: %p Select: %p (%d)",
                       context, context->select_lex,
                       (context->select_lex ?
                        context->select_lex->select_number:
                        0)));
  DBUG_RETURN(context);
}


SELECT_LEX *LEX::create_priority_nest(SELECT_LEX *first_in_nest)
{
  DBUG_ENTER("LEX::create_priority_nest");
  DBUG_ASSERT(first_in_nest->first_nested);
  enum sub_select_type wr_unit_type= first_in_nest->get_linkage();
  bool wr_distinct= first_in_nest->distinct;
  SELECT_LEX *attach_to= first_in_nest->first_nested;
  attach_to->cut_next();
  SELECT_LEX *wrapper= wrap_select_chain_into_derived(first_in_nest);
  if (wrapper)
  {
    first_in_nest->first_nested= NULL;
    wrapper->set_linkage_and_distinct(wr_unit_type, wr_distinct);
    wrapper->first_nested= attach_to->first_nested;
    wrapper->set_master_unit(attach_to->master_unit());
    attach_to->link_neighbour(wrapper);
  }
  DBUG_RETURN(wrapper);
}


/**
  Checks if we need finish "automatic brackets" mode

  INTERSECT has higher priority then UNION and EXCEPT, so when it is need we
  automatically create lower layer for INTERSECT (automatic brackets) and
  here we check if we should return back one level up during parsing procedure.
*/

void LEX::check_automatic_up(enum sub_select_type type)
{
  if (type != INTERSECT_TYPE &&
      current_select->get_linkage() == INTERSECT_TYPE &&
      current_select->outer_select() &&
      current_select->outer_select()->automatic_brackets)
  {
    nest_level--;
    current_select= current_select->outer_select();
  }
}


sp_variable *LEX::sp_param_init(LEX_CSTRING *name)
{
  if (spcont->find_variable(name, true))
  {
    my_error(ER_SP_DUP_PARAM, MYF(0), name->str);
    return NULL;
  }
  sp_variable *spvar= spcont->add_variable(thd, name);
  init_last_field(&spvar->field_def, name,
                  thd->variables.collation_database);
  return spvar;
}


bool LEX::sp_param_fill_definition(sp_variable *spvar,
                                   const Lex_field_type_st &def)
{
  return
    last_field->set_attributes(thd, def, charset,
                               COLUMN_DEFINITION_ROUTINE_PARAM) ||
    sphead->fill_spvar_definition(thd, last_field, &spvar->name);
}


bool LEX::sf_return_fill_definition(const Lex_field_type_st &def)
{
  return
    last_field->set_attributes(thd, def, charset,
                               COLUMN_DEFINITION_FUNCTION_RETURN) ||
    sphead->fill_field_definition(thd, last_field);
}


void LEX::set_stmt_init()
{
  sql_command= SQLCOM_SET_OPTION;
  mysql_init_select(this);
  option_type= OPT_SESSION;
  autocommit= 0;
  var_list.empty();
};


/**
  Find a local or a package body variable by name.
  @param IN  name    - the variable name
  @param OUT ctx     - NULL, if the variable was not found,
                       or LEX::spcont (if a local variable was found)
                       or the package top level context
                       (if a package variable was found)
  @param OUT handler - NULL, if the variable was not found,
                       or a pointer to rcontext handler
  @retval            - the variable (if found), or NULL otherwise.
*/
sp_variable *
LEX::find_variable(const LEX_CSTRING *name,
                   sp_pcontext **ctx,
                   const Sp_rcontext_handler **rh) const
{
  sp_variable *spv;
  if (spcont && (spv= spcont->find_variable(name, false)))
  {
    *ctx= spcont;
    *rh= &sp_rcontext_handler_local;
    return spv;
  }
  sp_package *pkg= sphead ? sphead->m_parent : NULL;
  if (pkg && (spv= pkg->find_package_variable(name)))
  {
    *ctx= pkg->get_parse_context()->child_context(0);
    *rh= &sp_rcontext_handler_package_body;
    return spv;
  }
  *ctx= NULL;
  *rh= NULL;
  return NULL;
}


static bool is_new(const char *str)
{
  return (str[0] == 'n' || str[0] == 'N') &&
         (str[1] == 'e' || str[1] == 'E') &&
         (str[2] == 'w' || str[2] == 'W');
}

static bool is_old(const char *str)
{
  return (str[0] == 'o' || str[0] == 'O') &&
         (str[1] == 'l' || str[1] == 'L') &&
         (str[2] == 'd' || str[2] == 'D');
}


bool LEX::is_trigger_new_or_old_reference(const LEX_CSTRING *name) const
{
  // "name" is not necessarily NULL-terminated!
  return sphead && sphead->m_handler->type() == SP_TYPE_TRIGGER &&
         name->length == 3 && (is_new(name->str) || is_old(name->str));
}


void LEX::sp_variable_declarations_init(THD *thd, int nvars)
{
  sp_variable *spvar= spcont->get_last_context_variable();

  sphead->reset_lex(thd);
  spcont->declare_var_boundary(nvars);
  thd->lex->init_last_field(&spvar->field_def, &spvar->name,
                            thd->variables.collation_database);
}


bool LEX::sp_variable_declarations_set_default(THD *thd, int nvars,
                                               Item *dflt_value_item)
{
  if (!dflt_value_item &&
      unlikely(!(dflt_value_item= new (thd->mem_root) Item_null(thd))))
    return true;

  for (uint i= 0 ; i < (uint) nvars ; i++)
  {
    sp_variable *spvar= spcont->get_last_context_variable((uint) nvars - 1 - i);
    bool last= i + 1 == (uint) nvars;
    spvar->default_value= dflt_value_item;
    /* The last instruction is responsible for freeing LEX. */
    sp_instr_set *is= new (thd->mem_root)
                      sp_instr_set(sphead->instructions(),
                                   spcont, &sp_rcontext_handler_local,
                                   spvar->offset, dflt_value_item,
                                   this, last);
    if (unlikely(is == NULL || sphead->add_instr(is)))
      return true;
  }
  return false;
}


bool
LEX::sp_variable_declarations_copy_type_finalize(THD *thd, int nvars,
                                                 const Column_definition &ref,
                                                 Row_definition_list *fields,
                                                 Item *default_value)
{
  for (uint i= 0 ; i < (uint) nvars; i++)
  {
    sp_variable *spvar= spcont->get_last_context_variable((uint) nvars - 1 - i);
    spvar->field_def.set_type(ref);
    if (fields)
    {
      DBUG_ASSERT(ref.type_handler() == &type_handler_row);
      spvar->field_def.set_row_field_definitions(fields);
    }
    spvar->field_def.field_name= spvar->name;
  }
  if (unlikely(sp_variable_declarations_set_default(thd, nvars,
                                                    default_value)))
    return true;
  spcont->declare_var_boundary(0);
  return sphead->restore_lex(thd);
}


bool LEX::sp_variable_declarations_finalize(THD *thd, int nvars,
                                            const Column_definition *cdef,
                                            Item *dflt_value_item)
{
  DBUG_ASSERT(cdef);
  Column_definition tmp(*cdef);
  if (sphead->fill_spvar_definition(thd, &tmp))
    return true;
  return sp_variable_declarations_copy_type_finalize(thd, nvars, tmp, NULL,
                                                     dflt_value_item);
}


bool LEX::sp_variable_declarations_row_finalize(THD *thd, int nvars,
                                                Row_definition_list *row,
                                                Item *dflt_value_item)
{
  DBUG_ASSERT(row);
  /*
    Prepare all row fields.
    Note, we do it only one time outside of the below loop.
    The converted list in "row" is further reused by all variable
    declarations processed by the current call.
    Example:
      DECLARE
        a, b, c ROW(x VARCHAR(10) CHARACTER SET utf8);
      BEGIN
        ...
      END;
  */
  if (sphead->row_fill_field_definitions(thd, row))
    return true;

  for (uint i= 0 ; i < (uint) nvars ; i++)
  {
    sp_variable *spvar= spcont->get_last_context_variable((uint) nvars - 1 - i);
    spvar->field_def.set_row_field_definitions(row);
    if (sphead->fill_spvar_definition(thd, &spvar->field_def, &spvar->name))
      return true;
  }

  if (sp_variable_declarations_set_default(thd, nvars, dflt_value_item))
    return true;
  spcont->declare_var_boundary(0);
  return sphead->restore_lex(thd);
}


/**
  Finalize a %ROWTYPE declaration, e.g.:
    DECLARE a,b,c,d t1%ROWTYPE := ROW(1,2,3);

  @param thd   - the current thd
  @param nvars - the number of variables in the declaration
  @param ref   - the table or cursor name (see comments below)
  @param def   - the default value, e.g., ROW(1,2,3), or NULL (no default).
*/
bool
LEX::sp_variable_declarations_rowtype_finalize(THD *thd, int nvars,
                                               Qualified_column_ident *ref,
                                               Item *def)
{
  uint coffp;
  const sp_pcursor *pcursor= ref->table.str && ref->db.str ? NULL :
                             spcont->find_cursor(&ref->m_column, &coffp,
                                                 false);
  if (pcursor)
    return sp_variable_declarations_cursor_rowtype_finalize(thd, nvars,
                                                            coffp, def);
  /*
    When parsing a qualified identifier chain, the parser does not know yet
    if it's going to be a qualified column name (for %TYPE),
    or a qualified table name (for %ROWTYPE). So it collects the chain
    into Qualified_column_ident.
    Now we know that it was actually a qualified table name (%ROWTYPE).
    Create a new Table_ident from Qualified_column_ident,
    shifting fields as follows:
    - ref->m_column becomes table_ref->table
    - ref->table    becomes table_ref->db
  */
  return sp_variable_declarations_table_rowtype_finalize(thd, nvars,
                                                         ref->table,
                                                         ref->m_column,
                                                         def);
}


bool
LEX::sp_variable_declarations_table_rowtype_finalize(THD *thd, int nvars,
                                                     const LEX_CSTRING &db,
                                                     const LEX_CSTRING &table,
                                                     Item *def)
{
  Table_ident *table_ref;
  if (unlikely(!(table_ref=
                 new (thd->mem_root) Table_ident(thd, &db, &table, false))))
    return true;
  // Loop through all variables in the same declaration
  for (uint i= 0 ; i < (uint) nvars; i++)
  {
    sp_variable *spvar= spcont->get_last_context_variable((uint) nvars - 1 - i);
    spvar->field_def.set_table_rowtype_ref(table_ref);
    sphead->fill_spvar_definition(thd, &spvar->field_def, &spvar->name);
  }
  if (sp_variable_declarations_set_default(thd, nvars, def))
    return true;
  // Make sure sp_rcontext is created using the invoker security context:
  sphead->m_flags|= sp_head::HAS_COLUMN_TYPE_REFS;
  spcont->declare_var_boundary(0);
  return sphead->restore_lex(thd);
}


bool
LEX::sp_variable_declarations_cursor_rowtype_finalize(THD *thd, int nvars,
                                                      uint offset,
                                                      Item *def)
{
  const sp_pcursor *pcursor= spcont->find_cursor(offset);

  // Loop through all variables in the same declaration
  for (uint i= 0 ; i < (uint) nvars; i++)
  {
    sp_variable *spvar= spcont->get_last_context_variable((uint) nvars - 1 - i);

    spvar->field_def.set_cursor_rowtype_ref(offset);
    sp_instr_cursor_copy_struct *instr=
      new (thd->mem_root) sp_instr_cursor_copy_struct(sphead->instructions(),
                                                      spcont, offset,
                                                      pcursor->lex(),
                                                      spvar->offset);
    if (instr == NULL || sphead->add_instr(instr))
     return true;

    sphead->fill_spvar_definition(thd, &spvar->field_def, &spvar->name);
  }
  if (unlikely(sp_variable_declarations_set_default(thd, nvars, def)))
    return true;
  // Make sure sp_rcontext is created using the invoker security context:
  sphead->m_flags|= sp_head::HAS_COLUMN_TYPE_REFS;
  spcont->declare_var_boundary(0);
  return sphead->restore_lex(thd);
}


/*
  Add declarations for table column and SP variable anchor types:
  - DECLARE spvar1 TYPE OF db1.table1.column1;
  - DECLARE spvar1 TYPE OF table1.column1;
  - DECLARE spvar1 TYPE OF spvar0;
*/
bool
LEX::sp_variable_declarations_with_ref_finalize(THD *thd, int nvars,
                                                Qualified_column_ident *ref,
                                                Item *def)
{
  return ref->db.length == 0 && ref->table.length == 0 ?
    sp_variable_declarations_vartype_finalize(thd, nvars, ref->m_column, def) :
    sp_variable_declarations_column_type_finalize(thd, nvars, ref, def);
}


bool
LEX::sp_variable_declarations_column_type_finalize(THD *thd, int nvars,
                                                   Qualified_column_ident *ref,
                                                   Item *def)
{
  for (uint i= 0 ; i < (uint) nvars; i++)
  {
    sp_variable *spvar= spcont->get_last_context_variable((uint) nvars - 1 - i);
    spvar->field_def.set_column_type_ref(ref);
    spvar->field_def.field_name= spvar->name;
  }
  sphead->m_flags|= sp_head::HAS_COLUMN_TYPE_REFS;
  if (sp_variable_declarations_set_default(thd, nvars, def))
    return true;
  spcont->declare_var_boundary(0);
  return sphead->restore_lex(thd);
}


bool
LEX::sp_variable_declarations_vartype_finalize(THD *thd, int nvars,
                                               const LEX_CSTRING &ref,
                                               Item *default_value)
{
  sp_variable *t;
  if (!spcont || !(t= spcont->find_variable(&ref, false)))
  {
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), ref.str);
    return true;
  }

  if (t->field_def.is_cursor_rowtype_ref())
  {
    uint offset= t->field_def.cursor_rowtype_offset();
    return sp_variable_declarations_cursor_rowtype_finalize(thd, nvars,
                                                            offset,
                                                            default_value);
  }

  if (t->field_def.is_column_type_ref())
  {
    Qualified_column_ident *tmp= t->field_def.column_type_ref();
    return sp_variable_declarations_column_type_finalize(thd, nvars, tmp,
                                                         default_value);
  }

  if (t->field_def.is_table_rowtype_ref())
  {
    const Table_ident *tmp= t->field_def.table_rowtype_ref();
    return sp_variable_declarations_table_rowtype_finalize(thd, nvars,
                                                           tmp->db,
                                                           tmp->table,
                                                           default_value);
  }

  // A reference to a scalar or a row variable with an explicit data type
  return sp_variable_declarations_copy_type_finalize(thd, nvars,
                                                     t->field_def,
                                                     t->field_def.
                                                       row_field_definitions(),
                                                     default_value);
}


/**********************************************************************
  The FOR LOOP statement

  This syntax:
    FOR i IN lower_bound .. upper_bound
    LOOP
      statements;
    END LOOP;

  is translated into:

    DECLARE
      i INT := lower_bound;
      j INT := upper_bound;
    BEGIN
      WHILE i <= j
      LOOP
        statements;
        i:= i + 1;
      END LOOP;
    END;
*/


sp_variable *LEX::sp_add_for_loop_variable(THD *thd, const LEX_CSTRING *name,
                                           Item *value)
{
  sp_variable *spvar= spcont->add_variable(thd, name);
  spcont->declare_var_boundary(1);
  spvar->field_def.field_name= spvar->name;
  spvar->field_def.set_handler(&type_handler_slonglong);
  type_handler_slonglong.Column_definition_prepare_stage2(&spvar->field_def,
                                                          NULL, HA_CAN_GEOMETRY);
  if (!value && unlikely(!(value= new (thd->mem_root) Item_null(thd))))
    return NULL;

  spvar->default_value= value;
  sp_instr_set *is= new (thd->mem_root)
                    sp_instr_set(sphead->instructions(),
                                 spcont, &sp_rcontext_handler_local,
                                 spvar->offset, value,
                                 this, true);
  if (unlikely(is == NULL || sphead->add_instr(is)))
    return NULL;
  spcont->declare_var_boundary(0);
  return spvar;
}


bool LEX::sp_for_loop_implicit_cursor_statement(THD *thd,
                                                Lex_for_loop_bounds_st *bounds,
                                                sp_lex_cursor *cur)
{
  Item *item;
  DBUG_ASSERT(sphead);
  LEX_CSTRING name= {STRING_WITH_LEN("[implicit_cursor]") };
  if (sp_declare_cursor(thd, &name, cur, NULL, true))
    return true;
  DBUG_ASSERT(thd->lex == this);
  if (unlikely(!(bounds->m_index=
                 new (thd->mem_root) sp_assignment_lex(thd, this))))
    return true;
  bounds->m_index->sp_lex_in_use= true;
  sphead->reset_lex(thd, bounds->m_index);
  DBUG_ASSERT(thd->lex != this);
  /*
    We pass NULL as Name_resolution_context here.
    It's OK, fix_fields() will not be called for this Item_field created.
    Item_field is only needed for LEX::sp_for_loop_cursor_declarations()
    and is used to transfer the loop index variable name, "rec" in this example:
      FOR rec IN (SELECT * FROM t1)
      DO
        SELECT rec.a, rec.b;
      END FOR;
  */
  if (!(item= new (thd->mem_root) Item_field(thd, NULL, name)))
    return true;
  bounds->m_index->set_item_and_free_list(item, NULL);
  if (thd->lex->sphead->restore_lex(thd))
    return true;
  DBUG_ASSERT(thd->lex == this);
  bounds->m_direction= 1;
  bounds->m_target_bound= NULL;
  bounds->m_implicit_cursor= true;
  return false;
}

sp_variable *
LEX::sp_add_for_loop_cursor_variable(THD *thd,
                                     const LEX_CSTRING *name,
                                     const sp_pcursor *pcursor,
                                     uint coffset,
                                     sp_assignment_lex *param_lex,
                                     Item_args *parameters)
{
  sp_variable *spvar= spcont->add_variable(thd, name);
  if (!spvar)
    return NULL;
  spcont->declare_var_boundary(1);
  sphead->fill_spvar_definition(thd, &spvar->field_def, &spvar->name);
  if (unlikely(!(spvar->default_value= new (thd->mem_root) Item_null(thd))))
    return NULL;

  spvar->field_def.set_cursor_rowtype_ref(coffset);

  if (unlikely(sphead->add_for_loop_open_cursor(thd, spcont, spvar, pcursor,
                                                coffset,
                                                param_lex, parameters)))
    return NULL;

  spcont->declare_var_boundary(0);
  return spvar;
}


/**
  Generate a code for a FOR loop condition:
  - Make Item_splocal for the FOR loop index variable
  - Make Item_splocal for the FOR loop upper bound variable
  - Make a comparison function item on top of these two variables
*/
bool LEX::sp_for_loop_condition(THD *thd, const Lex_for_loop_st &loop)
{
  Item_splocal *args[2];
  for (uint i= 0 ; i < 2; i++)
  {
    sp_variable *src= i == 0 ? loop.m_index : loop.m_target_bound;
    args[i]= new (thd->mem_root)
              Item_splocal(thd, &sp_rcontext_handler_local,
                           &src->name, src->offset, src->type_handler());
    if (unlikely(args[i] == NULL))
      return true;
#ifdef DBUG_ASSERT_EXISTS
    args[i]->m_sp= sphead;
#endif
  }

  Item *expr= loop.m_direction > 0 ?
    (Item *) new (thd->mem_root) Item_func_le(thd, args[0], args[1]) :
    (Item *) new (thd->mem_root) Item_func_ge(thd, args[0], args[1]);
  return unlikely(!expr) || unlikely(sp_while_loop_expression(thd, expr));
}


/**
  Generate the FOR LOOP condition code in its own lex
*/
bool LEX::sp_for_loop_intrange_condition_test(THD *thd,
                                              const Lex_for_loop_st &loop)
{
  spcont->set_for_loop(loop);
  sphead->reset_lex(thd);
  if (unlikely(thd->lex->sp_for_loop_condition(thd, loop)))
    return true;
  return thd->lex->sphead->restore_lex(thd);
}


bool LEX::sp_for_loop_cursor_condition_test(THD *thd,
                                            const Lex_for_loop_st &loop)
{
  const LEX_CSTRING *cursor_name;
  Item *expr;
  spcont->set_for_loop(loop);
  sphead->reset_lex(thd);
  cursor_name= spcont->find_cursor(loop.m_cursor_offset);
  DBUG_ASSERT(cursor_name);
  if (unlikely(!(expr=
                 new (thd->mem_root)
                 Item_func_cursor_found(thd, cursor_name,
                                        loop.m_cursor_offset))))
    return true;
  if (thd->lex->sp_while_loop_expression(thd, expr))
    return true;
  return thd->lex->sphead->restore_lex(thd);
}


bool LEX::sp_for_loop_intrange_declarations(THD *thd, Lex_for_loop_st *loop,
                                            const LEX_CSTRING *index,
                                            const Lex_for_loop_bounds_st &bounds)
{
  Item *item;
  if ((item= bounds.m_index->get_item())->type() == Item::FIELD_ITEM)
  {
    // We're here is the lower bound is unknown identifier
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), item->full_name());
    return true;
  }
  if ((item= bounds.m_target_bound->get_item())->type() == Item::FIELD_ITEM)
  {
    // We're here is the upper bound is unknown identifier
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), item->full_name());
    return true;
  }
  if (!(loop->m_index=
        bounds.m_index->sp_add_for_loop_variable(thd, index,
                                                 bounds.m_index->get_item())))
    return true;
  if (unlikely(!(loop->m_target_bound=
                 bounds.m_target_bound->
                 sp_add_for_loop_target_bound(thd,
                                              bounds.
                                              m_target_bound->get_item()))))
     return true;
  loop->m_direction= bounds.m_direction;
  loop->m_implicit_cursor= 0;
  return false;
}


bool LEX::sp_for_loop_cursor_declarations(THD *thd,
                                          Lex_for_loop_st *loop,
                                          const LEX_CSTRING *index,
                                          const Lex_for_loop_bounds_st &bounds)
{
  Item *item= bounds.m_index->get_item();
  Item_splocal *item_splocal;
  Item_field *item_field;
  Item_func_sp *item_func_sp= NULL;
  LEX_CSTRING name;
  uint coffs, param_count= 0;
  const sp_pcursor *pcursor;

  if ((item_splocal= item->get_item_splocal()))
    name= item_splocal->m_name;
  else if ((item_field= item->type() == Item::FIELD_ITEM ?
                        static_cast<Item_field *>(item) : NULL) &&
           item_field->table_name.str == NULL)
    name= item_field->field_name;
  else if (item->type() == Item::FUNC_ITEM &&
           static_cast<Item_func*>(item)->functype() == Item_func::FUNC_SP &&
           !static_cast<Item_func_sp*>(item)->get_sp_name()->m_explicit_name)
  {
    /*
      When a FOR LOOP for a cursor with parameters is parsed:
        FOR index IN cursor(1,2,3) LOOP
          statements;
        END LOOP;
      the parser scans "cursor(1,2,3)" using the "expr" rule,
      so it thinks that cursor(1,2,3) is a stored function call.
      It's not easy to implement this without using "expr" because
      of grammar conflicts.
      As a side effect, the Item_func_sp and its arguments in the parentheses
      belong to the same LEX. This is different from an explicit
      "OPEN cursor(1,2,3)" where every expression belongs to a separate LEX.
    */
    item_func_sp= static_cast<Item_func_sp*>(item);
    name= item_func_sp->get_sp_name()->m_name;
    param_count= item_func_sp->argument_count();
  }
  else
  {
    thd->parse_error();
    return true;
  }
  if (unlikely(!(pcursor= spcont->find_cursor_with_error(&name, &coffs,
                                                         false)) ||
               pcursor->check_param_count_with_error(param_count)))
    return true;

  if (!(loop->m_index= sp_add_for_loop_cursor_variable(thd, index,
                                                       pcursor, coffs,
                                                       bounds.m_index,
                                                       item_func_sp)))
    return true;
  loop->m_target_bound= NULL;
  loop->m_direction= bounds.m_direction;
  loop->m_cursor_offset= coffs;
  loop->m_implicit_cursor= bounds.m_implicit_cursor;
  return false;
}


/**
  Generate a code for a FOR loop index increment
*/
bool LEX::sp_for_loop_increment(THD *thd, const Lex_for_loop_st &loop)
{
  Item_splocal *splocal= new (thd->mem_root)
    Item_splocal(thd, &sp_rcontext_handler_local,
                      &loop.m_index->name, loop.m_index->offset,
                      loop.m_index->type_handler());
  if (unlikely(splocal == NULL))
    return true;
#ifdef DBUG_ASSERT_EXISTS
  splocal->m_sp= sphead;
#endif
  Item_int *inc= new (thd->mem_root) Item_int(thd, loop.m_direction);
  if (unlikely(!inc))
    return true;
  Item *expr= new (thd->mem_root) Item_func_plus(thd, splocal, inc);
  if (unlikely(!expr) ||
      unlikely(sphead->set_local_variable(thd, spcont,
                                          &sp_rcontext_handler_local,
                                          loop.m_index, expr, this, true)))
    return true;
  return false;
}


bool LEX::sp_for_loop_intrange_finalize(THD *thd, const Lex_for_loop_st &loop)
{
  sphead->reset_lex(thd);

  // Generate FOR LOOP index increment in its own lex
  DBUG_ASSERT(this != thd->lex);
  if (unlikely(thd->lex->sp_for_loop_increment(thd, loop) ||
               thd->lex->sphead->restore_lex(thd)))
    return true;

  // Generate a jump to the beginning of the loop
  DBUG_ASSERT(this == thd->lex);
  return sp_while_loop_finalize(thd);
}


bool LEX::sp_for_loop_cursor_finalize(THD *thd, const Lex_for_loop_st &loop)
{
  sp_instr_cfetch *instr=
    new (thd->mem_root) sp_instr_cfetch(sphead->instructions(),
                                        spcont, loop.m_cursor_offset, false);
  if (unlikely(instr == NULL) || unlikely(sphead->add_instr(instr)))
    return true;
  instr->add_to_varlist(loop.m_index);
  // Generate a jump to the beginning of the loop
  return sp_while_loop_finalize(thd);
}

bool LEX::sp_for_loop_outer_block_finalize(THD *thd,
                                           const Lex_for_loop_st &loop)
{
  Lex_spblock tmp;
  tmp.curs= MY_TEST(loop.m_implicit_cursor);
  if (unlikely(sp_block_finalize(thd, tmp))) // The outer DECLARE..BEGIN..END
    return true;
  if (!loop.is_for_loop_explicit_cursor())
    return false;
  /*
    Explicit cursor FOR loop must close the cursor automatically.
    Note, implicit cursor FOR loop does not need to close the cursor,
    it's closed by sp_instr_cpop.
  */
  sp_instr_cclose *ic= new (thd->mem_root)
                       sp_instr_cclose(sphead->instructions(), spcont,
                                       loop.m_cursor_offset);
  return ic == NULL || sphead->add_instr(ic);
}

/***************************************************************************/

bool LEX::sp_declare_cursor(THD *thd, const LEX_CSTRING *name,
                            sp_lex_cursor *cursor_stmt,
                            sp_pcontext *param_ctx, bool add_cpush_instr)
{
  uint offp;
  sp_instr_cpush *i;

  if (spcont->find_cursor(name, &offp, true))
  {
    my_error(ER_SP_DUP_CURS, MYF(0), name->str);
    return true;
  }

  if (unlikely(spcont->add_cursor(name, param_ctx, cursor_stmt)))
    return true;

  if (add_cpush_instr)
  {
    i= new (thd->mem_root)
         sp_instr_cpush(sphead->instructions(), spcont, cursor_stmt,
                        spcont->current_cursor_count() - 1);
    return unlikely(i == NULL) || unlikely(sphead->add_instr(i));
  }
  return false;
}


/**
  Generate an SP code for an "OPEN cursor_name" statement.
  @param thd
  @param name       - Name of the cursor
  @param parameters - Cursor parameters, e.g. OPEN c(1,2,3)
  @returns          - false on success, true on error
*/
bool LEX::sp_open_cursor(THD *thd, const LEX_CSTRING *name,
                         List<sp_assignment_lex> *parameters)
{
  uint offset;
  const sp_pcursor *pcursor;
  uint param_count= parameters ? parameters->elements : 0;
  return !(pcursor= spcont->find_cursor_with_error(name, &offset, false)) ||
         pcursor->check_param_count_with_error(param_count) ||
         sphead->add_open_cursor(thd, spcont, offset,
                                 pcursor->param_context(), parameters);
}


bool LEX::sp_handler_declaration_init(THD *thd, int type)
{
  sp_handler *h= spcont->add_handler(thd, (sp_handler::enum_type) type);

  spcont= spcont->push_context(thd, sp_pcontext::HANDLER_SCOPE);

  sp_instr_hpush_jump *i=
    new (thd->mem_root) sp_instr_hpush_jump(sphead->instructions(), spcont, h);

  if (unlikely(i == NULL) || unlikely(sphead->add_instr(i)))
    return true;

  /* For continue handlers, mark end of handler scope. */
  if (type == sp_handler::CONTINUE &&
      unlikely(sphead->push_backpatch(thd, i, spcont->last_label())))
    return true;

  if (unlikely(sphead->push_backpatch(thd, i,
                                      spcont->push_label(thd, &empty_clex_str,
                                                         0))))
    return true;

  return false;
}


bool LEX::sp_handler_declaration_finalize(THD *thd, int type)
{
  sp_label *hlab= spcont->pop_label(); /* After this hdlr */
  sp_instr_hreturn *i;

  if (type == sp_handler::CONTINUE)
  {
    i= new (thd->mem_root) sp_instr_hreturn(sphead->instructions(), spcont);
    if (unlikely(i == NULL) ||
        unlikely(sphead->add_instr(i)))
      return true;
  }
  else
  {  /* EXIT or UNDO handler, just jump to the end of the block */
    i= new (thd->mem_root) sp_instr_hreturn(sphead->instructions(), spcont);
    if (unlikely(i == NULL) ||
        unlikely(sphead->add_instr(i)) ||
        unlikely(sphead->push_backpatch(thd, i, spcont->last_label()))) /* Block end */
      return true;
  }
  sphead->backpatch(hlab);
  spcont= spcont->pop_context();
  return false;
}


void LEX::sp_block_init(THD *thd, const LEX_CSTRING *label)
{
  spcont->push_label(thd, label, sphead->instructions(), sp_label::BEGIN);
  spcont= spcont->push_context(thd, sp_pcontext::REGULAR_SCOPE);
}


bool LEX::sp_block_finalize(THD *thd, const Lex_spblock_st spblock,
                                      class sp_label **splabel)
{
  sp_head *sp= sphead;
  sp_pcontext *ctx= spcont;
  sp_instr *i;

  sp->backpatch(ctx->last_label()); /* We always have a label */
  if (spblock.hndlrs)
  {
    i= new (thd->mem_root)
      sp_instr_hpop(sp->instructions(), ctx, spblock.hndlrs);
    if (unlikely(i == NULL) ||
        unlikely(sp->add_instr(i)))
      return true;
  }
  if (spblock.curs)
  {
    i= new (thd->mem_root)
      sp_instr_cpop(sp->instructions(), ctx, spblock.curs);
    if (unlikely(i == NULL) ||
        unlikely(sp->add_instr(i)))
      return true;
  }
  spcont= ctx->pop_context();
  *splabel= spcont->pop_label();
  return false;
}


bool LEX::sp_block_finalize(THD *thd, const Lex_spblock_st spblock,
                            const LEX_CSTRING *end_label)
{
  sp_label *splabel;
  if (unlikely(sp_block_finalize(thd, spblock, &splabel)))
    return true;
  if (unlikely(end_label->str &&
               lex_string_cmp(system_charset_info,
                              end_label, &splabel->name) != 0))
  {
    my_error(ER_SP_LABEL_MISMATCH, MYF(0), end_label->str);
    return true;
  }
  return false;
}


sp_name *LEX::make_sp_name(THD *thd, const LEX_CSTRING *name)
{
  sp_name *res;
  LEX_CSTRING db;
  if (unlikely(check_routine_name(name)) ||
      unlikely(copy_db_to(&db)) ||
      unlikely((!(res= new (thd->mem_root) sp_name(&db, name, false)))))
    return NULL;
  return res;
}


/**
  When a package routine name is stored in memory in Database_qualified_name,
  the dot character is used to delimit package name from the routine name,
  e.g.:
    m_db=   'test';   -- database 'test'
    m_name= 'p1.p1';  -- package 'p1', routine 'p1'
  See database_qualified_name::make_package_routine_name() for details.
  Disallow package routine names with dots,
  to avoid ambiguity when interpreting m_name='p1.p1.p1', between:
    a.  package 'p1.p1' + routine 'p1'
    b.  package 'p1'    + routine 'p1.p1'
  m_name='p1.p1.p1' will always mean (a).
*/
sp_name *LEX::make_sp_name_package_routine(THD *thd, const LEX_CSTRING *name)
{
  sp_name *res= make_sp_name(thd, name);
  if (likely(res) && unlikely(strchr(res->m_name.str, '.')))
  {
    my_error(ER_SP_WRONG_NAME, MYF(0), res->m_name.str);
    res= NULL;
  }
  return res;
}


sp_name *LEX::make_sp_name(THD *thd, const LEX_CSTRING *name1,
                                     const LEX_CSTRING *name2)
{
  sp_name *res;
  LEX_CSTRING norm_name1;
  if (unlikely(!name1->str) ||
      unlikely(!thd->make_lex_string(&norm_name1, name1->str,
                                     name1->length)) ||
      unlikely(check_db_name((LEX_STRING *) &norm_name1)))
  {
    my_error(ER_WRONG_DB_NAME, MYF(0), name1->str);
    return NULL;
  }
  if (unlikely(check_routine_name(name2)) ||
      unlikely(!(res= new (thd->mem_root) sp_name(&norm_name1, name2, true))))
    return NULL;
  return res;
}


sp_head *LEX::make_sp_head(THD *thd, const sp_name *name,
                           const Sp_handler *sph,
                           enum_sp_aggregate_type agg_type)
{
  sp_package *package= get_sp_package();
  sp_head *sp;

  /* Order is important here: new - reset - init */
  if (likely((sp= sp_head::create(package, sph, agg_type))))
  {
    sp->reset_thd_mem_root(thd);
    sp->init(this);
    if (name)
    {
      if (package)
        sp->make_package_routine_name(sp->get_main_mem_root(),
                                      package->m_db,
                                      package->m_name,
                                      name->m_name);
      else
        sp->init_sp_name(name);
      sp->make_qname(sp->get_main_mem_root(), &sp->m_qname);
    }
    sphead= sp;
  }
  sp_chistics.init();
  return sp;
}


sp_head *LEX::make_sp_head_no_recursive(THD *thd, const sp_name *name,
                                        const Sp_handler *sph,
                                        enum_sp_aggregate_type agg_type)
{
  sp_package *package= thd->lex->get_sp_package();
  /*
    Sp_handler::sp_clone_and_link_routine() generates a standalone-alike
    statement to clone package routines for recursion, e.g.:
      CREATE PROCEDURE p1 AS BEGIN NULL; END;
    Translate a standalone routine handler to the corresponding
    package routine handler if we're cloning a package routine, e.g.:
      sp_handler_procedure -> sp_handler_package_procedure
      sp_handler_function  -> sp_handler_package_function
  */
  if (package && package->m_is_cloning_routine)
    sph= sph->package_routine_handler();
  if (!sphead ||
      (package &&
       (sph == &sp_handler_package_procedure ||
        sph == &sp_handler_package_function)))
    return make_sp_head(thd, name, sph, agg_type);
  my_error(ER_SP_NO_RECURSIVE_CREATE, MYF(0), sph->type_str());
  return NULL;
}


bool LEX::sp_body_finalize_routine(THD *thd)
{
  if (sphead->check_unresolved_goto())
    return true;
  sphead->set_stmt_end(thd);
  sphead->restore_thd_mem_root(thd);
  return false;
}


bool LEX::sp_body_finalize_procedure(THD *thd)
{
  return sphead->check_group_aggregate_instructions_forbid() ||
         sp_body_finalize_routine(thd);
}


bool LEX::sp_body_finalize_procedure_standalone(THD *thd,
                                                const sp_name *end_name)
{
  return sp_body_finalize_procedure(thd) ||
         sphead->check_standalone_routine_end_name(end_name);
}


bool LEX::sp_body_finalize_function(THD *thd)
{
  if (sphead->is_not_allowed_in_function("function") ||
      sphead->check_group_aggregate_instructions_function())
    return true;
  if (!(sphead->m_flags & sp_head::HAS_RETURN))
  {
    my_error(ER_SP_NORETURN, MYF(0), ErrConvDQName(sphead).ptr());
    return true;
  }
  if (sp_body_finalize_routine(thd))
    return true;
  (void) is_native_function_with_warn(thd, &sphead->m_name);
  return false;
}


bool LEX::sp_body_finalize_trigger(THD *thd)
{
  return sphead->is_not_allowed_in_function("trigger") ||
         sp_body_finalize_procedure(thd);
}


bool LEX::sp_body_finalize_event(THD *thd)
{
  event_parse_data->body_changed= true;
  return sp_body_finalize_procedure(thd);
}


bool LEX::stmt_create_stored_function_finalize_standalone(const sp_name *end_name)
{
  if (sphead->check_standalone_routine_end_name(end_name))
    return true;
  stmt_create_routine_finalize();
  return false;
}


bool LEX::sp_block_with_exceptions_finalize_declarations(THD *thd)
{
  /*
    [ DECLARE declarations ]
    BEGIN executable_section
    [ EXCEPTION exceptions ]
    END

    We are now at the "BEGIN" keyword.
    We have collected all declarations, including DECLARE HANDLER directives.
    But there will be possibly more handlers in the EXCEPTION section.

    Generate a forward jump from the end of the DECLARE section to the
    beginning of the EXCEPTION section, over the executable section.
  */
  return sphead->add_instr_jump(thd, spcont);
}


bool
LEX::sp_block_with_exceptions_finalize_executable_section(THD *thd,
                                         uint executable_section_ip)
{
  /*
    We're now at the end of "executable_section" of the block,
    near the "EXCEPTION" or the "END" keyword.
    Generate a jump to the END of the block over the EXCEPTION section.
  */
  if (sphead->add_instr_jump_forward_with_backpatch(thd, spcont))
    return true;
  /*
    Set the destination for the jump that we added in
    sp_block_with_exceptions_finalize_declarations().
  */
  sp_instr *instr= sphead->get_instr(executable_section_ip - 1);
  instr->backpatch(sphead->instructions(), spcont);
  return false;
}


bool
LEX::sp_block_with_exceptions_finalize_exceptions(THD *thd,
                                                  uint executable_section_ip,
                                                  uint exception_count)
{
  if (!exception_count)
  {
    /*
      The jump from the end of DECLARE section to
      the beginning of the EXCEPTION section that we added in
      sp_block_with_exceptions_finalize_declarations() is useless
      if there were no exceptions.
      Replace it to "no operation".
    */
    return sphead->replace_instr_to_nop(thd, executable_section_ip - 1);
  }
  /*
    Generate a jump from the end of the EXCEPTION code
    to the executable section.
  */
  return sphead->add_instr_jump(thd, spcont, executable_section_ip);
}


bool LEX::sp_block_with_exceptions_add_empty(THD *thd)
{
  uint ip= sphead->instructions();
  return sp_block_with_exceptions_finalize_executable_section(thd, ip) ||
         sp_block_with_exceptions_finalize_exceptions(thd, ip, 0);
}


bool LEX::sp_change_context(THD *thd, const sp_pcontext *ctx, bool exclusive)
{
  uint n;
  uint ip= sphead->instructions();
  if ((n= spcont->diff_handlers(ctx, exclusive)))
  {
    sp_instr_hpop *hpop= new (thd->mem_root) sp_instr_hpop(ip++, spcont, n);
    if (unlikely(hpop == NULL) || unlikely(sphead->add_instr(hpop)))
      return true;
  }
  if ((n= spcont->diff_cursors(ctx, exclusive)))
  {
    sp_instr_cpop *cpop= new (thd->mem_root) sp_instr_cpop(ip++, spcont, n);
    if (unlikely(cpop == NULL) || unlikely(sphead->add_instr(cpop)))
      return true;
  }
  return false;
}


bool LEX::sp_leave_statement(THD *thd, const LEX_CSTRING *label_name)
{
  sp_label *lab= spcont->find_label(label_name);
  if (unlikely(!lab))
  {
    my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "LEAVE", label_name->str);
    return true;
  }
  return sp_exit_block(thd, lab, NULL);
}

bool LEX::sp_goto_statement(THD *thd, const LEX_CSTRING *label_name)
{
  sp_label *lab= spcont->find_goto_label(label_name);
  if (!lab || lab->ip == 0)
  {
    sp_label *delayedlabel;
    if (!lab)
    {
      // Label not found --> add forward jump to an unknown label
      spcont->push_goto_label(thd, label_name, 0, sp_label::GOTO);
      delayedlabel= spcont->last_goto_label();
    }
    else
    {
      delayedlabel= lab;
    }
    return sphead->push_backpatch_goto(thd, spcont, delayedlabel);
  }
  else
  {
    // Label found (backward goto)
    return sp_change_context(thd, lab->ctx, false) ||
           sphead->add_instr_jump(thd, spcont, lab->ip); /* Jump back */
  }
  return false;
}

bool LEX::sp_push_goto_label(THD *thd, const LEX_CSTRING *label_name)
{
  sp_label *lab= spcont->find_goto_label(label_name, false);
  if (lab)
  {
    if (unlikely(lab->ip != 0))
    {
      my_error(ER_SP_LABEL_REDEFINE, MYF(0), label_name->str);
      return true;
    }
    lab->ip= sphead->instructions();

    sp_label *beginblocklabel= spcont->find_label(&empty_clex_str);
    sphead->backpatch_goto(thd, lab, beginblocklabel);
  }
  else
  {
    spcont->push_goto_label(thd, label_name, sphead->instructions());
  }
  return false;
}

bool LEX::sp_exit_block(THD *thd, sp_label *lab)
{
  /*
    When jumping to a BEGIN-END block end, the target jump
    points to the block hpop/cpop cleanup instructions,
    so we should exclude the block context here.
    When jumping to something else (i.e., SP_LAB_ITER),
    there are no hpop/cpop at the jump destination,
    so we should include the block context here for cleanup.
  */
  bool exclusive= (lab->type == sp_label::BEGIN);
  return sp_change_context(thd, lab->ctx, exclusive) ||
         sphead->add_instr_jump_forward_with_backpatch(thd, spcont, lab);
}


bool LEX::sp_exit_block(THD *thd, sp_label *lab, Item *when)
{
  if (!when)
    return sp_exit_block(thd, lab);

  DBUG_ASSERT(sphead == thd->lex->sphead);
  DBUG_ASSERT(spcont == thd->lex->spcont);
  sp_instr_jump_if_not *i= new (thd->mem_root)
                           sp_instr_jump_if_not(sphead->instructions(),
                                                spcont,
                                                when, this);
  if (unlikely(i == NULL) ||
      unlikely(sphead->add_instr(i)) ||
      unlikely(sp_exit_block(thd, lab)))
    return true;
  i->backpatch(sphead->instructions(), spcont);
  return false;
}


bool LEX::sp_exit_statement(THD *thd, Item *item)
{
  sp_label *lab= spcont->find_label_current_loop_start();
  if (unlikely(!lab))
  {
    my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "EXIT", "");
    return true;
  }
  DBUG_ASSERT(lab->type == sp_label::ITERATION);
  return sp_exit_block(thd, lab, item);
}


bool LEX::sp_exit_statement(THD *thd, const LEX_CSTRING *label_name, Item *item)
{
  sp_label *lab= spcont->find_label(label_name);
  if (unlikely(!lab || lab->type != sp_label::ITERATION))
  {
    my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "EXIT", label_name->str);
    return true;
  }
  return sp_exit_block(thd, lab, item);
}


bool LEX::sp_iterate_statement(THD *thd, const LEX_CSTRING *label_name)
{
  sp_label *lab= spcont->find_label(label_name);
  if (unlikely(!lab || lab->type != sp_label::ITERATION))
  {
    my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "ITERATE", label_name->str);
    return true;
  }
  return sp_continue_loop(thd, lab);
}


bool LEX::sp_continue_loop(THD *thd, sp_label *lab)
{
  if (lab->ctx->for_loop().m_index)
  {
    // We're in a FOR loop, increment the index variable before backward jump
    sphead->reset_lex(thd);
    DBUG_ASSERT(this != thd->lex);
    if (thd->lex->sp_for_loop_increment(thd, lab->ctx->for_loop()) ||
        thd->lex->sphead->restore_lex(thd))
      return true;
  }
  return sp_change_context(thd, lab->ctx, false) ||
         sphead->add_instr_jump(thd, spcont, lab->ip); /* Jump back */
}


bool LEX::sp_continue_statement(THD *thd)
{
  sp_label *lab= spcont->find_label_current_loop_start();
  if (unlikely(!lab))
  {
    my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "CONTINUE", "");
    return true;
  }
  DBUG_ASSERT(lab->type == sp_label::ITERATION);
  return sp_continue_loop(thd, lab);
}


bool LEX::sp_continue_statement(THD *thd, const LEX_CSTRING *label_name)
{
  sp_label *lab= spcont->find_label(label_name);
  if (!lab || lab->type != sp_label::ITERATION)
  {
    my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "CONTINUE", label_name->str);
    return true;
  }
  return sp_continue_loop(thd, lab);
}


bool LEX::sp_continue_loop(THD *thd, sp_label *lab, Item *when)
{
  DBUG_ASSERT(when);
  DBUG_ASSERT(sphead == thd->lex->sphead);
  DBUG_ASSERT(spcont == thd->lex->spcont);
  sp_instr_jump_if_not *i= new (thd->mem_root)
                           sp_instr_jump_if_not(sphead->instructions(),
                                                spcont,
                                                when, this);
  if (unlikely(i == NULL) ||
      unlikely(sphead->add_instr(i)) ||
      unlikely(sp_continue_loop(thd, lab)))
    return true;
  i->backpatch(sphead->instructions(), spcont);
  return false;
}


bool sp_expr_lex::sp_continue_when_statement(THD *thd)
{
  sp_label *lab= spcont->find_label_current_loop_start();
  if (unlikely(!lab))
  {
    my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "CONTINUE", "");
    return true;
  }
  DBUG_ASSERT(lab->type == sp_label::ITERATION);
  return sp_continue_loop(thd, lab, get_item());
}


bool sp_expr_lex::sp_continue_when_statement(THD *thd,
                                             const LEX_CSTRING *label_name)
{
  sp_label *lab= spcont->find_label(label_name);
  if (!lab || lab->type != sp_label::ITERATION)
  {
    my_error(ER_SP_LILABEL_MISMATCH, MYF(0), "CONTINUE", label_name->str);
    return true;
  }
  return sp_continue_loop(thd, lab, get_item());
}


bool LEX::maybe_start_compound_statement(THD *thd)
{
  if (!sphead)
  {
    if (!make_sp_head(thd, NULL, &sp_handler_procedure, DEFAULT_AGGREGATE))
      return true;
    sphead->set_suid(SP_IS_NOT_SUID);
    sphead->set_body_start(thd, thd->m_parser_state->m_lip.get_cpp_ptr());
  }
  return false;
}


bool LEX::sp_push_loop_label(THD *thd, const LEX_CSTRING *label_name)
{
  sp_label *lab= spcont->find_label(label_name);
  if (lab)
  {
    my_error(ER_SP_LABEL_REDEFINE, MYF(0), label_name->str);
    return true;
  }
  spcont->push_label(thd, label_name, sphead->instructions(),
                     sp_label::ITERATION);
  return false;
}


bool LEX::sp_push_loop_empty_label(THD *thd)
{
  if (maybe_start_compound_statement(thd))
    return true;
  /* Unlabeled controls get an empty label. */
  spcont->push_label(thd, &empty_clex_str, sphead->instructions(),
                     sp_label::ITERATION);
  return false;
}


bool LEX::sp_pop_loop_label(THD *thd, const LEX_CSTRING *label_name)
{
  sp_label *lab= spcont->pop_label();
  sphead->backpatch(lab);
  if (label_name->str &&
      lex_string_cmp(system_charset_info, label_name,
                     &lab->name) != 0)
  {
    my_error(ER_SP_LABEL_MISMATCH, MYF(0), label_name->str);
    return true;
  }
  return false;
}


void LEX::sp_pop_loop_empty_label(THD *thd)
{
  sp_label *lab= spcont->pop_label();
  sphead->backpatch(lab);
  DBUG_ASSERT(lab->name.length == 0);
}


bool LEX::sp_while_loop_expression(THD *thd, Item *item)
{
  sp_instr_jump_if_not *i= new (thd->mem_root)
    sp_instr_jump_if_not(sphead->instructions(), spcont, item, this);
  return (unlikely(i == NULL) ||
          /* Jumping forward */
          unlikely(sphead->push_backpatch(thd, i, spcont->last_label())) ||
          unlikely(sphead->new_cont_backpatch(i)) ||
          unlikely(sphead->add_instr(i)));
}


bool LEX::sp_while_loop_finalize(THD *thd)
{
  sp_label *lab= spcont->last_label();  /* Jumping back */
  sp_instr_jump *i= new (thd->mem_root)
    sp_instr_jump(sphead->instructions(), spcont, lab->ip);
  if (unlikely(i == NULL) ||
      unlikely(sphead->add_instr(i)))
    return true;
  sphead->do_cont_backpatch();
  return false;
}


Item *LEX::create_and_link_Item_trigger_field(THD *thd,
                                              const LEX_CSTRING *name,
                                              bool new_row)
{
  Item_trigger_field *trg_fld;

  if (unlikely(trg_chistics.event == TRG_EVENT_INSERT && !new_row))
  {
    my_error(ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0), "OLD", "on INSERT");
    return NULL;
  }

  if (unlikely(trg_chistics.event == TRG_EVENT_DELETE && new_row))
  {
    my_error(ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0), "NEW", "on DELETE");
    return NULL;
  }

  DBUG_ASSERT(!new_row ||
              (trg_chistics.event == TRG_EVENT_INSERT ||
               trg_chistics.event == TRG_EVENT_UPDATE));

  const bool tmp_read_only=
    !(new_row && trg_chistics.action_time == TRG_ACTION_BEFORE);
  trg_fld= new (thd->mem_root)
             Item_trigger_field(thd, current_context(),
                                new_row ?
                                  Item_trigger_field::NEW_ROW:
                                  Item_trigger_field::OLD_ROW,
                                *name, SELECT_ACL, tmp_read_only);
  /*
    Let us add this item to list of all Item_trigger_field objects
    in trigger.
  */
  if (likely(trg_fld))
    trg_table_fields.link_in_list(trg_fld, &trg_fld->next_trg_field);

  return trg_fld;
}


Item *LEX::make_item_colon_ident_ident(THD *thd,
                                       const Lex_ident_cli_st *ca,
                                       const Lex_ident_cli_st *cb)
{
  Lex_ident_sys a(thd, ca), b(thd, cb);
  if (a.is_null() || b.is_null())
    return NULL; // OEM
  if (!is_trigger_new_or_old_reference(&a))
  {
    thd->parse_error();
    return NULL;
  }
  bool new_row= (a.str[0] == 'N' || a.str[0] == 'n');
  return create_and_link_Item_trigger_field(thd, &b, new_row);
}


Item *LEX::make_item_plsql_cursor_attr(THD *thd, const LEX_CSTRING *name,
                                       plsql_cursor_attr_t attr)
{
  uint offset;
  if (unlikely(!spcont || !spcont->find_cursor(name, &offset, false)))
  {
    my_error(ER_SP_CURSOR_MISMATCH, MYF(0), name->str);
    return NULL;
  }
  switch (attr) {
  case PLSQL_CURSOR_ATTR_ISOPEN:
    return new (thd->mem_root) Item_func_cursor_isopen(thd, name, offset);
  case PLSQL_CURSOR_ATTR_FOUND:
    return new (thd->mem_root) Item_func_cursor_found(thd, name, offset);
  case PLSQL_CURSOR_ATTR_NOTFOUND:
    return new (thd->mem_root) Item_func_cursor_notfound(thd, name, offset);
  case PLSQL_CURSOR_ATTR_ROWCOUNT:
    return new (thd->mem_root) Item_func_cursor_rowcount(thd, name, offset);
  }
  DBUG_ASSERT(0);
  return NULL;
}


Item *LEX::make_item_sysvar(THD *thd,
                            enum_var_type type,
                            const LEX_CSTRING *name,
                            const LEX_CSTRING *component)

{
  Item *item;
  DBUG_ASSERT(name->str);
  /*
    "SELECT @@global.global.variable" is not allowed
    Note, "global" can come through TEXT_STRING_sys.
  */
  if (component->str && unlikely(check_reserved_words(name)))
  {
    thd->parse_error();
    return NULL;
  }
  if (unlikely(!(item= get_system_var(thd, type, name, component))))
    return NULL;
  if (!((Item_func_get_system_var*) item)->is_written_to_binlog())
    set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_VARIABLE);
  return item;
}


static bool param_push_or_clone(THD *thd, LEX *lex, Item_param *item)
{
  return !lex->clone_spec_offset ?
         lex->param_list.push_back(item, thd->mem_root) :
         item->add_as_clone(thd);
}


Item_param *LEX::add_placeholder(THD *thd, const LEX_CSTRING *name,
                                 const char *start, const char *end)
{
  if (unlikely(!thd->m_parser_state->m_lip.stmt_prepare_mode))
  {
    thd->parse_error(ER_SYNTAX_ERROR, start);
    return NULL;
  }
  if (unlikely(!parsing_options.allows_variable))
  {
    my_error(ER_VIEW_SELECT_VARIABLE, MYF(0));
    return NULL;
  }
  Query_fragment pos(thd, sphead, start, end);
  Item_param *item= new (thd->mem_root) Item_param(thd, name,
                                                   pos.pos(), pos.length());
  if (unlikely(!item) || unlikely(param_push_or_clone(thd, this, item)))
  {
    my_error(ER_OUT_OF_RESOURCES, MYF(0));
    return NULL;
  }
  return item;
}


bool LEX::add_signal_statement(THD *thd, const sp_condition_value *v)
{
  Yacc_state *state= &thd->m_parser_state->m_yacc;
  sql_command= SQLCOM_SIGNAL;
  m_sql_cmd= new (thd->mem_root) Sql_cmd_signal(v, state->m_set_signal_info);
  return m_sql_cmd == NULL;
}


bool LEX::add_resignal_statement(THD *thd, const sp_condition_value *v)
{
  Yacc_state *state= &thd->m_parser_state->m_yacc;
  sql_command= SQLCOM_RESIGNAL;
  m_sql_cmd= new (thd->mem_root) Sql_cmd_resignal(v, state->m_set_signal_info);
  return m_sql_cmd == NULL;
}


/*
  Make an Item when an identifier is found in the FOR loop bounds:
    FOR rec IN cursor
    FOR var IN var1 .. xxx
    FOR var IN row1.field1 .. xxx
  When we parse the first expression after the "IN" keyword,
  we don't know yet if it's a cursor name, or a scalar SP variable name,
  or a field of a ROW SP variable. Here we create Item_field to remember
  the fully qualified name. Later sp_for_loop_cursor_declarations()
  detects how to treat this name properly.
*/
Item *LEX::create_item_for_loop_bound(THD *thd,
                                      const LEX_CSTRING *a,
                                      const LEX_CSTRING *b,
                                      const LEX_CSTRING *c)
{
  /*
    Pass NULL as the name resolution context.
    This is OK, fix_fields() won't be called for this Item_field.
  */
  return new (thd->mem_root) Item_field(thd, NULL, *a, *b, *c);
}


bool LEX::check_expr_allows_fields_or_error(THD *thd, const char *name) const
{
  if (select_stack_top > 0)
    return false; // OK, fields are allowed
  my_error(ER_BAD_FIELD_ERROR, MYF(0), name, thd->where);
  return true;    // Error, fields are not allowed
}

Item *LEX::create_item_ident_nospvar(THD *thd,
                                     const Lex_ident_sys_st *a,
                                     const Lex_ident_sys_st *b)
{
  DBUG_ASSERT(this == thd->lex);
  /*
    FIXME This will work ok in simple_ident_nospvar case because
    we can't meet simple_ident_nospvar in trigger now. But it
    should be changed in future.
  */
  if (is_trigger_new_or_old_reference(a))
  {
    bool new_row= (a->str[0]=='N' || a->str[0]=='n');

    return create_and_link_Item_trigger_field(thd, b, new_row);
  }

  if (unlikely(current_select->no_table_names_allowed))
  {
    my_error(ER_TABLENAME_NOT_ALLOWED_HERE, MYF(0), a->str, thd->where);
    return NULL;
  }

  if (current_select->parsing_place == FOR_LOOP_BOUND)
    return create_item_for_loop_bound(thd, &null_clex_str, a, b);

  return create_item_ident_field(thd, Lex_ident_sys(), *a, *b);
}


Item_splocal *LEX::create_item_spvar_row_field(THD *thd,
                                               const Sp_rcontext_handler *rh,
                                               const Lex_ident_sys *a,
                                               const Lex_ident_sys *b,
                                               sp_variable *spv,
                                               const char *start,
                                               const char *end)
{
  if (unlikely(!parsing_options.allows_variable))
  {
    my_error(ER_VIEW_SELECT_VARIABLE, MYF(0));
    return NULL;
  }

  Query_fragment pos(thd, sphead, start, end);
  Item_splocal *item;
  if (spv->field_def.is_table_rowtype_ref() ||
      spv->field_def.is_cursor_rowtype_ref())
  {
    if (unlikely(!(item= new (thd->mem_root)
                   Item_splocal_row_field_by_name(thd, rh, a, b, spv->offset,
                                                  &type_handler_null,
                                                  pos.pos(), pos.length()))))
      return NULL;
  }
  else
  {
    uint row_field_offset;
    const Spvar_definition *def;
    if (unlikely(!(def= spv->find_row_field(a, b, &row_field_offset))))
      return NULL;

    if (unlikely(!(item= new (thd->mem_root)
                   Item_splocal_row_field(thd, rh, a, b,
                                          spv->offset, row_field_offset,
                                          def->type_handler(),
                                          pos.pos(), pos.length()))))
      return NULL;
  }
#ifdef DBUG_ASSERT_EXISTS
  item->m_sp= sphead;
#endif
  safe_to_cache_query=0;
  return item;
}


my_var *LEX::create_outvar(THD *thd, const LEX_CSTRING *name)
{
  const Sp_rcontext_handler *rh;
  sp_variable *spv;
  if (likely((spv= find_variable(name, &rh))))
    return result ? new (thd->mem_root)
                    my_var_sp(rh, name, spv->offset,
                              spv->type_handler(), sphead) :
                    NULL /* EXPLAIN */;
  my_error(ER_SP_UNDECLARED_VAR, MYF(0), name->str);
  return NULL;
}


my_var *LEX::create_outvar(THD *thd,
                           const LEX_CSTRING *a,
                           const LEX_CSTRING *b)
{
  const Sp_rcontext_handler *rh;
  sp_variable *t;
  if (unlikely(!(t= find_variable(a, &rh))))
  {
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), a->str);
    return NULL;
  }
  uint row_field_offset;
  if (!t->find_row_field(a, b, &row_field_offset))
    return NULL;
  return result ?
    new (thd->mem_root) my_var_sp_row_field(rh, a, b, t->offset,
                                            row_field_offset, sphead) :
    NULL /* EXPLAIN */;
}


Item *LEX::create_item_func_nextval(THD *thd, Table_ident *table_ident)
{
  TABLE_LIST *table;
  if (unlikely(!(table= current_select->add_table_to_list(thd, table_ident, 0,
                                                          TL_OPTION_SEQUENCE,
                                                          TL_WRITE_ALLOW_WRITE,
                                                          MDL_SHARED_WRITE))))
    return NULL;
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  return new (thd->mem_root) Item_func_nextval(thd, table);
}


Item *LEX::create_item_func_lastval(THD *thd, Table_ident *table_ident)
{
  TABLE_LIST *table;
  if (unlikely(!(table= current_select->add_table_to_list(thd, table_ident, 0,
                                                          TL_OPTION_SEQUENCE,
                                                          TL_READ,
                                                          MDL_SHARED_READ))))
    return NULL;
  thd->lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);
  return new (thd->mem_root) Item_func_lastval(thd, table);
}


Item *LEX::create_item_func_nextval(THD *thd,
                                    const LEX_CSTRING *db,
                                    const LEX_CSTRING *name)
{
  Table_ident *table_ident;
  if (unlikely(!(table_ident=
                 new (thd->mem_root) Table_ident(thd, db, name, false))))
    return NULL;
  return create_item_func_nextval(thd, table_ident);
}


Item *LEX::create_item_func_lastval(THD *thd,
                                    const LEX_CSTRING *db,
                                    const LEX_CSTRING *name)
{
  Table_ident *table_ident;
  if (unlikely(!(table_ident=
                 new (thd->mem_root) Table_ident(thd, db, name, false))))
    return NULL;
  return create_item_func_lastval(thd, table_ident);
}


Item *LEX::create_item_func_setval(THD *thd, Table_ident *table_ident,
                                   longlong nextval, ulonglong round,
                                   bool is_used)
{
  TABLE_LIST *table;
  if (unlikely(!(table= current_select->add_table_to_list(thd, table_ident, 0,
                                                          TL_OPTION_SEQUENCE,
                                                          TL_WRITE_ALLOW_WRITE,
                                                          MDL_SHARED_WRITE))))
    return NULL;
  return new (thd->mem_root) Item_func_setval(thd, table, nextval, round,
                                              is_used);
}


Item *LEX::create_item_ident(THD *thd,
                             const Lex_ident_cli_st *ca,
                             const Lex_ident_cli_st *cb)
{
  const char *start= ca->pos();
  const char *end= cb->end();
  const Sp_rcontext_handler *rh;
  sp_variable *spv;
  DBUG_ASSERT(thd->m_parser_state->m_lip.get_buf() <= start);
  DBUG_ASSERT(start <= end);
  DBUG_ASSERT(end <= thd->m_parser_state->m_lip.get_end_of_query());
  Lex_ident_sys a(thd, ca), b(thd, cb);
  if (a.is_null() || b.is_null())
    return NULL; // OEM
  if ((spv= find_variable(&a, &rh)) &&
      (spv->field_def.is_row() ||
       spv->field_def.is_table_rowtype_ref() ||
       spv->field_def.is_cursor_rowtype_ref()))
    return create_item_spvar_row_field(thd, rh, &a, &b, spv, start, end);

  if ((thd->variables.sql_mode & MODE_ORACLE) && b.length == 7)
  {
    if (!system_charset_info->strnncoll(
                      (const uchar *) b.str, 7,
                      (const uchar *) "NEXTVAL", 7))
      return create_item_func_nextval(thd, &null_clex_str, &a);
    else if (!system_charset_info->strnncoll(
                          (const uchar *) b.str, 7,
                          (const uchar *) "CURRVAL", 7))
      return create_item_func_lastval(thd, &null_clex_str, &a);
  }

  return create_item_ident_nospvar(thd, &a, &b);
}


Item *LEX::create_item_ident(THD *thd,
                             const Lex_ident_sys_st *a,
                             const Lex_ident_sys_st *b,
                             const Lex_ident_sys_st *c)
{
  Lex_ident_sys_st schema= thd->client_capabilities & CLIENT_NO_SCHEMA ?
                           Lex_ident_sys() : *a;
  if ((thd->variables.sql_mode & MODE_ORACLE) && c->length == 7)
  {
    if (!system_charset_info->strnncoll(
                      (const uchar *) c->str, 7,
                      (const uchar *) "NEXTVAL", 7))
      return create_item_func_nextval(thd, a, b);
    else if (!system_charset_info->strnncoll(
                          (const uchar *) c->str, 7,
                          (const uchar *) "CURRVAL", 7))
      return create_item_func_lastval(thd, a, b);
  }

  if (current_select->no_table_names_allowed)
  {
    my_error(ER_TABLENAME_NOT_ALLOWED_HERE, MYF(0), b->str, thd->where);
    return NULL;
  }

  if (current_select->parsing_place == FOR_LOOP_BOUND)
    return create_item_for_loop_bound(thd, &null_clex_str, b, c);

  return create_item_ident_field(thd, schema, *b, *c);
}


Item *LEX::create_item_limit(THD *thd, const Lex_ident_cli_st *ca)
{
  DBUG_ASSERT(thd->m_parser_state->m_lip.get_buf() <= ca->pos());
  DBUG_ASSERT(ca->pos() <= ca->end());
  DBUG_ASSERT(ca->end() <= thd->m_parser_state->m_lip.get_end_of_query());

  const Sp_rcontext_handler *rh;
  sp_variable *spv;
  Lex_ident_sys sa(thd, ca);
  if (sa.is_null())
    return NULL; // EOM
  if (!(spv= find_variable(&sa, &rh)))
  {
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), sa.str);
    return NULL;
  }

  Query_fragment pos(thd, sphead, ca->pos(), ca->end());
  Item_splocal *item;
  if (unlikely(!(item= new (thd->mem_root)
                 Item_splocal(thd, rh, &sa,
                              spv->offset, spv->type_handler(),
                              clone_spec_offset ? 0 : pos.pos(),
                              clone_spec_offset ? 0 : pos.length()))))
    return NULL;
#ifdef DBUG_ASSERT_EXISTS
  item->m_sp= sphead;
#endif
  safe_to_cache_query= 0;

  if (!item->is_valid_limit_clause_variable_with_error())
    return NULL;

  item->limit_clause_param= true;
  return item;
}


Item *LEX::create_item_limit(THD *thd,
                             const Lex_ident_cli_st *ca,
                             const Lex_ident_cli_st *cb)
{
  DBUG_ASSERT(thd->m_parser_state->m_lip.get_buf() <= ca->pos());
  DBUG_ASSERT(ca->pos() <= cb->end());
  DBUG_ASSERT(cb->end() <= thd->m_parser_state->m_lip.get_end_of_query());

  const Sp_rcontext_handler *rh;
  sp_variable *spv;
  Lex_ident_sys sa(thd, ca), sb(thd, cb);
  if (unlikely(sa.is_null() || sb.is_null()))
    return NULL; // EOM
  if (!(spv= find_variable(&sa, &rh)))
  {
    my_error(ER_SP_UNDECLARED_VAR, MYF(0), sa.str);
    return NULL;
  }
  // Qualified %TYPE variables are not possible
  DBUG_ASSERT(!spv->field_def.column_type_ref());
  Item_splocal *item;
  if (unlikely(!(item= create_item_spvar_row_field(thd, rh, &sa, &sb, spv,
                                                   ca->pos(), cb->end()))))
    return NULL;
  if (!item->is_valid_limit_clause_variable_with_error())
    return NULL;
  item->limit_clause_param= true;
  return item;
}


bool LEX::set_user_variable(THD *thd, const LEX_CSTRING *name, Item *val)
{
  Item_func_set_user_var *item;
  set_var_user *var;
  if (unlikely(!(item= new (thd->mem_root) Item_func_set_user_var(thd, name,
                                                                  val))) ||
      unlikely(!(var= new (thd->mem_root) set_var_user(item))))
    return true;
  if (unlikely(var_list.push_back(var, thd->mem_root)))
    return true;
  return false;
}


Item *LEX::create_item_ident_field(THD *thd,
                                   const Lex_ident_sys_st &db,
                                   const Lex_ident_sys_st &table,
                                   const Lex_ident_sys_st &name)
{
  if (check_expr_allows_fields_or_error(thd, name.str))
    return NULL;

  if (current_select->parsing_place != IN_HAVING ||
      current_select->get_in_sum_expr() > 0)
    return new (thd->mem_root) Item_field(thd, current_context(),
                                          db, table, name);

  return new (thd->mem_root) Item_ref(thd, current_context(),
                                      db, table, name);
}


Item *LEX::create_item_ident_sp(THD *thd, Lex_ident_sys_st *name,
                                const char *start,
                                const char *end)
{
  DBUG_ASSERT(thd->m_parser_state->m_lip.get_buf() <= start);
  DBUG_ASSERT(start <= end);
  DBUG_ASSERT(end <= thd->m_parser_state->m_lip.get_end_of_query());

  const Sp_rcontext_handler *rh;
  sp_variable *spv;
  DBUG_ASSERT(spcont);
  DBUG_ASSERT(sphead);
  if ((spv= find_variable(name, &rh)))
  {
    /* We're compiling a stored procedure and found a variable */
    if (!parsing_options.allows_variable)
    {
      my_error(ER_VIEW_SELECT_VARIABLE, MYF(0));
      return NULL;
    }

    Query_fragment pos(thd, sphead, start, end);
    uint f_pos= clone_spec_offset ? 0 : pos.pos();
    uint f_length= clone_spec_offset ? 0 : pos.length();
    Item_splocal *splocal= spv->field_def.is_column_type_ref() ?
      new (thd->mem_root) Item_splocal_with_delayed_data_type(thd, rh, name,
                                                              spv->offset,
                                                              f_pos, f_length) :
      new (thd->mem_root) Item_splocal(thd, rh, name,
                                       spv->offset, spv->type_handler(),
                                       f_pos, f_length);
    if (unlikely(splocal == NULL))
      return NULL;
#ifdef DBUG_ASSERT_EXISTS
    splocal->m_sp= sphead;
#endif
    safe_to_cache_query= 0;
    return splocal;
  }

  if (thd->variables.sql_mode & MODE_ORACLE)
  {
    if (lex_string_eq(name, STRING_WITH_LEN("SQLCODE")))
      return new (thd->mem_root) Item_func_sqlcode(thd);
    if (lex_string_eq(name, STRING_WITH_LEN("SQLERRM")))
      return new (thd->mem_root) Item_func_sqlerrm(thd);
  }

  if (current_select->parsing_place == FOR_LOOP_BOUND)
    return create_item_for_loop_bound(thd, &null_clex_str, &null_clex_str,
                                      name);

  return create_item_ident_nosp(thd, name);
}



bool LEX::set_variable(const Lex_ident_sys_st *name, Item *item)
{
  sp_pcontext *ctx;
  const Sp_rcontext_handler *rh;
  sp_variable *spv= find_variable(name, &ctx, &rh);
  return spv ? sphead->set_local_variable(thd, ctx, rh, spv, item, this, true) :
               set_system_variable(option_type, name, item);
}


/**
  Generate instructions for:
    SET x.y= expr;
*/
bool LEX::set_variable(const Lex_ident_sys_st *name1,
                       const Lex_ident_sys_st *name2,
                       Item *item)
{
  const Sp_rcontext_handler *rh;
  sp_pcontext *ctx;
  sp_variable *spv;
  if (spcont && (spv= find_variable(name1, &ctx, &rh)))
  {
    if (spv->field_def.is_table_rowtype_ref() ||
        spv->field_def.is_cursor_rowtype_ref())
      return sphead->set_local_variable_row_field_by_name(thd, ctx,
                                                          rh,
                                                          spv, name2,
                                                          item, this);
    // A field of a ROW variable
    uint row_field_offset;
    return !spv->find_row_field(name1, name2, &row_field_offset) ||
           sphead->set_local_variable_row_field(thd, ctx, rh,
                                                spv, row_field_offset,
                                                item, this);
  }

  if (is_trigger_new_or_old_reference(name1))
    return set_trigger_field(name1, name2, item);

  return set_system_variable(thd, option_type, name1, name2, item);
}


bool LEX::set_default_system_variable(enum_var_type var_type,
                                      const Lex_ident_sys_st *name,
                                      Item *val)
{
  static Lex_ident_sys default_base_name= {STRING_WITH_LEN("default")};
  sys_var *var= find_sys_var(thd, name->str, name->length);
  if (!var)
    return true;
  if (unlikely(!var->is_struct()))
  {
    my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), name->str);
    return true;
  }
  return set_system_variable(var_type, var, &default_base_name, val);
}


bool LEX::set_system_variable(enum_var_type var_type,
                              const Lex_ident_sys_st *name,
                              Item *val)
{
  sys_var *var= find_sys_var(thd, name->str, name->length);
  DBUG_ASSERT(thd->is_error() || var != NULL);
  static Lex_ident_sys null_str;
  return likely(var) ? set_system_variable(var_type, var, &null_str, val) : true;
}


bool LEX::set_system_variable(THD *thd, enum_var_type var_type,
                              const Lex_ident_sys_st *name1,
                              const Lex_ident_sys_st *name2,
                              Item *val)
{
  sys_var *tmp;
  if (unlikely(check_reserved_words(name1)) ||
      unlikely(!(tmp= find_sys_var(thd, name2->str, name2->length, true))))
  {
    my_error(ER_UNKNOWN_STRUCTURED_VARIABLE, MYF(0),
             (int) name1->length, name1->str);
    return true;
  }
  if (unlikely(!tmp->is_struct()))
  {
    my_error(ER_VARIABLE_IS_NOT_STRUCT, MYF(0), name2->str);
    return true;
  }
  return set_system_variable(var_type, tmp, name1, val);
}


bool LEX::set_trigger_field(const LEX_CSTRING *name1, const LEX_CSTRING *name2,
                            Item *val)
{
  DBUG_ASSERT(is_trigger_new_or_old_reference(name1));
  if (unlikely(name1->str[0]=='O' || name1->str[0]=='o'))
  {
    my_error(ER_TRG_CANT_CHANGE_ROW, MYF(0), "OLD", "");
    return true;
  }
  if (unlikely(trg_chistics.event == TRG_EVENT_DELETE))
  {
    my_error(ER_TRG_NO_SUCH_ROW_IN_TRG, MYF(0), "NEW", "on DELETE");
    return true;
  }
  if (unlikely(trg_chistics.action_time == TRG_ACTION_AFTER))
  {
    my_error(ER_TRG_CANT_CHANGE_ROW, MYF(0), "NEW", "after ");
    return true;
  }
  return set_trigger_new_row(name2, val);
}


#ifdef MYSQL_SERVER
uint binlog_unsafe_map[256];

#define UNSAFE(a, b, c) \
  { \
  DBUG_PRINT("unsafe_mixed_statement", ("SETTING BASE VALUES: %s, %s, %02X", \
    LEX::stmt_accessed_table_string(a), \
    LEX::stmt_accessed_table_string(b), \
    c)); \
  unsafe_mixed_statement(a, b, c); \
  }

/*
  Sets the combination given by "a" and "b" and automatically combinations
  given by other types of access, i.e. 2^(8 - 2), as unsafe.

  It may happen a colision when automatically defining a combination as unsafe.
  For that reason, a combination has its unsafe condition redefined only when
  the new_condition is greater then the old. For instance,
  
     . (BINLOG_DIRECT_ON & TRX_CACHE_NOT_EMPTY) is never overwritten by 
     . (BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF).
*/
void unsafe_mixed_statement(LEX::enum_stmt_accessed_table a,
                            LEX::enum_stmt_accessed_table b, uint condition)
{
  int type= 0;
  int index= (1U << a) | (1U << b);
  
  
  for (type= 0; type < 256; type++)
  {
    if ((type & index) == index)
    {
      binlog_unsafe_map[type] |= condition;
    }
  }
}
/*
  The BINLOG_* AND TRX_CACHE_* values can be combined by using '&' or '|',
  which means that both conditions need to be satisfied or any of them is
  enough. For example, 
    
    . BINLOG_DIRECT_ON & TRX_CACHE_NOT_EMPTY means that the statment is
    unsafe when the option is on and trx-cache is not empty;

    . BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF means the statement is unsafe
    in all cases.

    . TRX_CACHE_EMPTY | TRX_CACHE_NOT_EMPTY means the statement is unsafe
    in all cases. Similar as above.
*/
void binlog_unsafe_map_init()
{
  memset((void*) binlog_unsafe_map, 0, sizeof(uint) * 256);

  /*
    Classify a statement as unsafe when there is a mixed statement and an
    on-going transaction at any point of the execution if:

      1. The mixed statement is about to update a transactional table and
      a non-transactional table.

      2. The mixed statement is about to update a transactional table and
      read from a non-transactional table.

      3. The mixed statement is about to update a non-transactional table
      and temporary transactional table.

      4. The mixed statement is about to update a temporary transactional
      table and read from a non-transactional table.

      5. The mixed statement is about to update a transactional table and
      a temporary non-transactional table.
     
      6. The mixed statement is about to update a transactional table and
      read from a temporary non-transactional table.

      7. The mixed statement is about to update a temporary transactional
      table and temporary non-transactional table.

      8. The mixed statement is about to update a temporary transactional
      table and read from a temporary non-transactional table.

    After updating a transactional table if:

      9. The mixed statement is about to update a non-transactional table
      and read from a transactional table.

      10. The mixed statement is about to update a non-transactional table
      and read from a temporary transactional table.

      11. The mixed statement is about to update a temporary non-transactional
      table and read from a transactional table.
      
      12. The mixed statement is about to update a temporary non-transactional
      table and read from a temporary transactional table.

      13. The mixed statement is about to update a temporary non-transactional
      table and read from a non-transactional table.

    The reason for this is that locks acquired may not protected a concurrent
    transaction of interfering in the current execution and by consequence in
    the result.
  */
  /* Case 1. */
  UNSAFE(LEX::STMT_WRITES_TRANS_TABLE, LEX::STMT_WRITES_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF);
  /* Case 2. */
  UNSAFE(LEX::STMT_WRITES_TRANS_TABLE, LEX::STMT_READS_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF);
  /* Case 3. */
  UNSAFE(LEX::STMT_WRITES_NON_TRANS_TABLE, LEX::STMT_WRITES_TEMP_TRANS_TABLE,
    BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF);
  /* Case 4. */
  UNSAFE(LEX::STMT_WRITES_TEMP_TRANS_TABLE, LEX::STMT_READS_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF);
  /* Case 5. */
  UNSAFE(LEX::STMT_WRITES_TRANS_TABLE, LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON);
  /* Case 6. */
  UNSAFE(LEX::STMT_WRITES_TRANS_TABLE, LEX::STMT_READS_TEMP_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON);
  /* Case 7. */
  UNSAFE(LEX::STMT_WRITES_TEMP_TRANS_TABLE, LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON);
  /* Case 8. */
  UNSAFE(LEX::STMT_WRITES_TEMP_TRANS_TABLE, LEX::STMT_READS_TEMP_NON_TRANS_TABLE,
    BINLOG_DIRECT_ON);
  /* Case 9. */
  UNSAFE(LEX::STMT_WRITES_NON_TRANS_TABLE, LEX::STMT_READS_TRANS_TABLE,
    (BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF) & TRX_CACHE_NOT_EMPTY);
  /* Case 10 */
  UNSAFE(LEX::STMT_WRITES_NON_TRANS_TABLE, LEX::STMT_READS_TEMP_TRANS_TABLE,
    (BINLOG_DIRECT_ON | BINLOG_DIRECT_OFF) & TRX_CACHE_NOT_EMPTY);
  /* Case 11. */
  UNSAFE(LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE, LEX::STMT_READS_TRANS_TABLE,
    BINLOG_DIRECT_ON & TRX_CACHE_NOT_EMPTY);
  /* Case 12. */
  UNSAFE(LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE, LEX::STMT_READS_TEMP_TRANS_TABLE,
    BINLOG_DIRECT_ON & TRX_CACHE_NOT_EMPTY);
  /* Case 13. */
  UNSAFE(LEX::STMT_WRITES_TEMP_NON_TRANS_TABLE, LEX::STMT_READS_NON_TRANS_TABLE,
     BINLOG_DIRECT_OFF & TRX_CACHE_NOT_EMPTY);
}
#endif


/**
  @brief
    Collect fiels that are used in the GROUP BY of this st_select_lex
    
  @param thd  The thread handle

  @details
    This method looks through the fields that are used in the GROUP BY of this
    st_select_lex and saves info on these fields.
*/

void st_select_lex::collect_grouping_fields_for_derived(THD *thd,
                                                        ORDER *grouping_list)
{
  grouping_tmp_fields.empty();
  List_iterator<Item> li(join->fields_list);
  Item *item= li++;
  for (uint i= 0; i < master_unit()->derived->table->s->fields;
       i++, (item=li++))
  {
    for (ORDER *ord= grouping_list; ord; ord= ord->next)
    {
      if ((*ord->item)->eq((Item*)item, 0))
      {
        Field_pair *grouping_tmp_field=
          new Field_pair(master_unit()->derived->table->field[i], item);
        grouping_tmp_fields.push_back(grouping_tmp_field);
      }
    }
  }
}


/**
  Collect fields that are used in the GROUP BY of this SELECT
*/

bool st_select_lex::collect_grouping_fields(THD *thd)
{
  grouping_tmp_fields.empty();

  for (ORDER *ord= group_list.first; ord; ord= ord->next)
  {
    Item *item= *ord->item;
    if (item->type() != Item::FIELD_ITEM &&
        !(item->type() == Item::REF_ITEM &&
          item->real_type() == Item::FIELD_ITEM &&
          ((((Item_ref *) item)->ref_type() == Item_ref::VIEW_REF) ||
           (((Item_ref *) item)->ref_type() == Item_ref::REF))))
      continue;

    Field_pair *grouping_tmp_field=
      new Field_pair(((Item_field *)item->real_item())->field, item);
    if (grouping_tmp_fields.push_back(grouping_tmp_field, thd->mem_root))
      return false;
  }
  if (grouping_tmp_fields.elements)
    return false;
  return true;
}


/**
  @brief
   For a condition check possibility of exraction a formula over grouping fields 

  @param thd      The thread handle
  @param cond     The condition whose subformulas are to be analyzed
  @param checker  The checker callback function to be applied to the nodes
                  of the tree of the object
  
  @details
    This method traverses the AND-OR condition cond and for each subformula of
    the condition it checks whether it can be usable for the extraction of a
    condition over the grouping fields of this select. The method uses
    the call-back parameter checker to check whether a primary formula
    depends only on grouping fields.
    The subformulas that are not usable are marked with the flag NO_EXTRACTION_FL.
    The subformulas that can be entierly extracted are marked with the flag 
    FULL_EXTRACTION_FL.
  @note
    This method is called before any call of extract_cond_for_grouping_fields.
    The flag NO_EXTRACTION_FL set in a subformula allows to avoid building clone
    for the subformula when extracting the pushable condition.
    The flag FULL_EXTRACTION_FL allows to delete later all top level conjuncts
    from cond.
*/ 

void 
st_select_lex::check_cond_extraction_for_grouping_fields(THD *thd, Item *cond)
{
  if (cond->get_extraction_flag() == NO_EXTRACTION_FL)
    return;
  cond->clear_extraction_flag();
  if (cond->type() == Item::COND_ITEM)
  {
    Item_cond_and *and_cond=
      (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC) ?
      ((Item_cond_and*) cond) : 0;

    List<Item> *arg_list=  ((Item_cond*) cond)->argument_list();
    List_iterator<Item> li(*arg_list);
    uint count= 0;         // to count items not containing NO_EXTRACTION_FL
    uint count_full= 0;    // to count items with FULL_EXTRACTION_FL
    Item *item;
    while ((item=li++))
    {
      check_cond_extraction_for_grouping_fields(thd, item);
      if (item->get_extraction_flag() !=  NO_EXTRACTION_FL)
      {
        count++;
        if (item->get_extraction_flag() == FULL_EXTRACTION_FL)
          count_full++;
      }
      else if (!and_cond)
        break;
    }
    if ((and_cond && count == 0) || item)
      cond->set_extraction_flag(NO_EXTRACTION_FL);
    if (count_full == arg_list->elements)
    {
      cond->set_extraction_flag(FULL_EXTRACTION_FL);
    }
    if (cond->get_extraction_flag() != 0)
    {
      li.rewind();
      while ((item=li++))
        item->clear_extraction_flag();
    }
  }
  else
  {
    int fl= cond->excl_dep_on_grouping_fields(this) && !cond->is_expensive() ?
      FULL_EXTRACTION_FL : NO_EXTRACTION_FL;
    cond->set_extraction_flag(fl);
  }
}


/**
  @brief
  Build condition extractable from the given one depended on grouping fields
 
  @param thd           The thread handle
  @param cond          The condition from which the condition depended 
                       on grouping fields is to be extracted
  @param no_top_clones If it's true then no clones for the top fully 
                       extractable conjuncts are built

  @details
    For the given condition cond this method finds out what condition depended
    only on the grouping fields can be extracted from cond. If such condition C
    exists the method builds the item for it.
    This method uses the flags NO_EXTRACTION_FL and FULL_EXTRACTION_FL set by the
    preliminary call of st_select_lex::check_cond_extraction_for_grouping_fields
    to figure out whether a subformula depends only on these fields or not.
  @note
    The built condition C is always implied by the condition cond
    (cond => C). The method tries to build the least restictive such
    condition (i.e. for any other condition C' such that cond => C'
    we have C => C').
  @note
    The build item is not ready for usage: substitution for the field items
    has to be done and it has to be re-fixed.
  
  @retval
    the built condition depended only on grouping fields if such a condition exists
    NULL if there is no such a condition
*/ 

Item *st_select_lex::build_cond_for_grouping_fields(THD *thd, Item *cond,
                                                    bool no_top_clones)
{
  if (cond->get_extraction_flag() == FULL_EXTRACTION_FL)
  {
    if (no_top_clones)
      return cond;
    cond->clear_extraction_flag();
    return cond->build_clone(thd);
  }
  if (cond->type() == Item::COND_ITEM)
  {
    bool cond_and= false;
    Item_cond *new_cond;
    if (((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
    {
      cond_and= true;
      new_cond=  new (thd->mem_root) Item_cond_and(thd);
    }
    else
      new_cond= new (thd->mem_root) Item_cond_or(thd);
    if (unlikely(!new_cond))
      return 0;
    List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
    {
      if (item->get_extraction_flag() == NO_EXTRACTION_FL)
      {
        DBUG_ASSERT(cond_and);
        item->clear_extraction_flag();
        continue;
      }
      Item *fix= build_cond_for_grouping_fields(thd, item,
                                                no_top_clones & cond_and);
      if (unlikely(!fix))
      {
        if (cond_and)
          continue;
        break;
      }
      new_cond->argument_list()->push_back(fix, thd->mem_root);
    }
    
    if (!cond_and && item)
    {
      while((item= li++))
        item->clear_extraction_flag();
      return 0;
    }
    switch (new_cond->argument_list()->elements) 
    {
    case 0:
      return 0;
    case 1:
      return new_cond->argument_list()->head();
    default:
      return new_cond;
    }
  }
  return 0;
}


bool st_select_lex::set_nest_level(int new_nest_level)
{
  DBUG_ENTER("st_select_lex::set_nest_level");
  DBUG_PRINT("enter", ("select #%d %p nest level: %d",
                       select_number, this, new_nest_level));
  if (new_nest_level > (int) MAX_SELECT_NESTING)
  {
    my_error(ER_TOO_HIGH_LEVEL_OF_NESTING_FOR_SELECT, MYF(0));
    DBUG_RETURN(TRUE);
  }
  nest_level= new_nest_level;
  new_nest_level++;
  for (SELECT_LEX_UNIT *u= first_inner_unit(); u; u= u->next_unit())
  {
    if (u->set_nest_level(new_nest_level))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}

bool st_select_lex_unit::set_nest_level(int new_nest_level)
{
  DBUG_ENTER("st_select_lex_unit::set_nest_level");
  for(SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    if (sl->set_nest_level(new_nest_level))
      DBUG_RETURN(TRUE);
  }
  if (fake_select_lex &&
      fake_select_lex->set_nest_level(new_nest_level))
    DBUG_RETURN(TRUE);
  DBUG_RETURN(FALSE);
}


bool st_select_lex::check_parameters(SELECT_LEX *main_select)
{
  DBUG_ENTER("st_select_lex::check_parameters");
  DBUG_PRINT("enter", ("select #%d %p nest level: %d",
                       select_number, this, nest_level));


  if ((options & OPTION_PROCEDURE_CLAUSE) &&
      (!parent_lex->selects_allow_procedure ||
        next_select() != NULL ||
        this != master_unit()->first_select() ||
        nest_level != 0))
  {
    my_error(ER_CANT_USE_OPTION_HERE, MYF(0), "PROCEDURE");
    DBUG_RETURN(TRUE);
  }

  if ((options & SELECT_HIGH_PRIORITY) && this != main_select)
  {
    my_error(ER_CANT_USE_OPTION_HERE, MYF(0), "HIGH_PRIORITY");
    DBUG_RETURN(TRUE);
  }
  if ((options & OPTION_BUFFER_RESULT) && this != main_select)
  {
    my_error(ER_CANT_USE_OPTION_HERE, MYF(0), "SQL_BUFFER_RESULT");
    DBUG_RETURN(TRUE);
  }
  if ((options & OPTION_FOUND_ROWS) && this != main_select)
  {
    my_error(ER_CANT_USE_OPTION_HERE, MYF(0), "SQL_CALC_FOUND_ROWS");
    DBUG_RETURN(TRUE);
  }
  if (options & OPTION_NO_QUERY_CACHE)
  {
    /*
      Allow this flag only on the first top-level SELECT statement, if
      SQL_CACHE wasn't specified.
    */
    if (this != main_select)
    {
      my_error(ER_CANT_USE_OPTION_HERE, MYF(0), "SQL_NO_CACHE");
      DBUG_RETURN(TRUE);
    }
    if (parent_lex->sql_cache == LEX::SQL_CACHE)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "SQL_CACHE", "SQL_NO_CACHE");
      DBUG_RETURN(TRUE);
    }
    parent_lex->safe_to_cache_query=0;
    parent_lex->sql_cache= LEX::SQL_NO_CACHE;
  }
  if (options & OPTION_TO_QUERY_CACHE)
  {
    /*
      Allow this flag only on the first top-level SELECT statement, if
      SQL_NO_CACHE wasn't specified.
    */
    if (this != main_select)
    {
      my_error(ER_CANT_USE_OPTION_HERE, MYF(0), "SQL_CACHE");
      DBUG_RETURN(TRUE);
    }
    if (parent_lex->sql_cache == LEX::SQL_NO_CACHE)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "SQL_NO_CACHE", "SQL_CACHE");
      DBUG_RETURN(TRUE);
    }
    parent_lex->safe_to_cache_query=1;
    parent_lex->sql_cache= LEX::SQL_CACHE;
  }

  for (SELECT_LEX_UNIT *u= first_inner_unit(); u; u= u->next_unit())
  {
    if (u->check_parameters(main_select))
      DBUG_RETURN(TRUE);
  }
  DBUG_RETURN(FALSE);
}


bool st_select_lex_unit::check_parameters(SELECT_LEX *main_select)
{
  for(SELECT_LEX *sl= first_select(); sl; sl= sl->next_select())
  {
    if (sl->check_parameters(main_select))
      return TRUE;
  }
  return fake_select_lex && fake_select_lex->check_parameters(main_select);
}


bool LEX::check_main_unit_semantics()
{
  if (unit.set_nest_level(0) ||
      unit.check_parameters(first_select_lex()))
    return TRUE;
  return FALSE;
}

int set_statement_var_if_exists(THD *thd, const char *var_name,
                                size_t var_name_length, ulonglong value)
{
  sys_var *sysvar;
  if (unlikely(thd->lex->sql_command == SQLCOM_CREATE_VIEW))
  {
    my_error(ER_VIEW_SELECT_CLAUSE, MYF(0), "[NO]WAIT");
    return 1;
  }
  if (unlikely(thd->lex->sphead))
  {
    my_error(ER_SP_BADSTATEMENT, MYF(0), "[NO]WAIT");
    return 1;
  }
  if ((sysvar= find_sys_var(thd, var_name, var_name_length, true)))
  {
    Item *item= new (thd->mem_root) Item_uint(thd, value);
    set_var *var= new (thd->mem_root) set_var(thd, OPT_SESSION, sysvar,
                                              &null_clex_str, item);

    if (unlikely(!item) || unlikely(!var) ||
        unlikely(thd->lex->stmt_var_list.push_back(var, thd->mem_root)))
    {
      my_error(ER_OUT_OF_RESOURCES, MYF(0));
      return 1;
    }
  }
  return 0;
}


bool LEX::sp_add_cfetch(THD *thd, const LEX_CSTRING *name)
{
  uint offset;
  sp_instr_cfetch *i;

  if (!spcont->find_cursor(name, &offset, false))
  {
    my_error(ER_SP_CURSOR_MISMATCH, MYF(0), name->str);
    return true;
  }
  i= new (thd->mem_root)
    sp_instr_cfetch(sphead->instructions(), spcont, offset,
                    !(thd->variables.sql_mode & MODE_ORACLE));
  if (unlikely(i == NULL) || unlikely(sphead->add_instr(i)))
    return true;
  return false;
}


bool LEX::sp_add_agg_cfetch()
{
  sphead->m_flags|= sp_head::HAS_AGGREGATE_INSTR;
  sp_instr_agg_cfetch *i=
    new (thd->mem_root) sp_instr_agg_cfetch(sphead->instructions(), spcont);
  return i == NULL || sphead->add_instr(i);
}


bool LEX::create_or_alter_view_finalize(THD *thd, Table_ident *table_ident)
{
  sql_command= SQLCOM_CREATE_VIEW;
  /* first table in list is target VIEW name */
  if (!first_select_lex()->add_table_to_list(thd, table_ident, NULL,
                                             TL_OPTION_UPDATING,
                                             TL_IGNORE,
                                             MDL_EXCLUSIVE))
    return true;
  query_tables->open_strategy= TABLE_LIST::OPEN_STUB;
  return false;
}


bool LEX::add_alter_view(THD *thd, uint16 algorithm,
                         enum_view_suid suid,
                         Table_ident *table_ident)
{
  if (unlikely(sphead))
  {
    my_error(ER_SP_BADSTATEMENT, MYF(0), "ALTER VIEW");
    return true;
  }
  if (unlikely(!(create_view= new (thd->mem_root)
                 Create_view_info(VIEW_ALTER, algorithm, suid))))
    return true;
  return create_or_alter_view_finalize(thd, table_ident);
}


bool LEX::add_create_view(THD *thd, DDL_options_st ddl,
                          uint16 algorithm, enum_view_suid suid,
                          Table_ident *table_ident)
{
  if (unlikely(set_create_options_with_check(ddl)))
    return true;
  if (unlikely(!(create_view= new (thd->mem_root)
                 Create_view_info(ddl.or_replace() ?
                                  VIEW_CREATE_OR_REPLACE :
                                  VIEW_CREATE_NEW,
                                  algorithm, suid))))
    return true;
  return create_or_alter_view_finalize(thd, table_ident);
}


bool LEX::call_statement_start(THD *thd, sp_name *name)
{
  Database_qualified_name pkgname(&null_clex_str, &null_clex_str);
  const Sp_handler *sph= &sp_handler_procedure;
  sql_command= SQLCOM_CALL;
  value_list.empty();
  if (unlikely(sph->sp_resolve_package_routine(thd, thd->lex->sphead,
                                               name, &sph, &pkgname)))
    return true;
  if (unlikely(!(m_sql_cmd= new (thd->mem_root) Sql_cmd_call(name, sph))))
    return true;
  sph->add_used_routine(this, thd, name);
  if (pkgname.m_name.length)
    sp_handler_package_body.add_used_routine(this, thd, &pkgname);
  return false;
}


bool LEX::call_statement_start(THD *thd, const Lex_ident_sys_st *name)
{
  sp_name *spname= make_sp_name(thd, name);
  return unlikely(!spname) || call_statement_start(thd, spname);
}


bool LEX::call_statement_start(THD *thd, const Lex_ident_sys_st *name1,
                                         const Lex_ident_sys_st *name2)
{
  sp_name *spname= make_sp_name(thd, name1, name2);
  return unlikely(!spname) || call_statement_start(thd, spname);
}


sp_package *LEX::get_sp_package() const
{
  return sphead ? sphead->get_package() : NULL;
}


sp_package *LEX::create_package_start(THD *thd,
                                      enum_sql_command command,
                                      const Sp_handler *sph,
                                      const sp_name *name_arg,
                                      DDL_options_st options)
{
  sp_package *pkg;

  if (unlikely(sphead))
  {
    my_error(ER_SP_NO_RECURSIVE_CREATE, MYF(0), sph->type_str());
    return NULL;
  }
  if (unlikely(set_command_with_check(command, options)))
    return NULL;
  if (sph->type() == SP_TYPE_PACKAGE_BODY)
  {
    /*
      If we start parsing a "CREATE PACKAGE BODY", we need to load
      the corresponding "CREATE PACKAGE", for the following reasons:
      1. "CREATE PACKAGE BODY" is allowed only if "CREATE PACKAGE"
         was done earlier for the same package name.
         So if "CREATE PACKAGE" does not exist, we throw an error here.
      2. When parsing "CREATE PACKAGE BODY", we need to know all package
         public and private routine names, to translate procedure and
         function calls correctly.
         For example, this statement inside a package routine:
           CALL p;
         can be translated to:
           CALL db.pkg.p; -- p is a known (public or private) package routine
           CALL db.p;     -- p is not a known package routine
    */
    sp_head *spec;
    int ret= sp_handler_package_spec.
               sp_cache_routine_reentrant(thd, name_arg, &spec);
    if (unlikely(!spec))
    {
      if (!ret)
        my_error(ER_SP_DOES_NOT_EXIST, MYF(0),
                 "PACKAGE", ErrConvDQName(name_arg).ptr());
      return 0;
    }
  }
  if (unlikely(!(pkg= sp_package::create(this, name_arg, sph))))
    return NULL;
  pkg->reset_thd_mem_root(thd);
  pkg->init(this);
  pkg->make_qname(pkg->get_main_mem_root(), &pkg->m_qname);
  sphead= pkg;
  return pkg;
}


bool LEX::create_package_finalize(THD *thd,
                                  const sp_name *name,
                                  const sp_name *name2,
                                  const char *body_start,
                                  const char *body_end)
{
  if (name2 &&
      (name2->m_explicit_name != name->m_explicit_name ||
       strcmp(name2->m_db.str, name->m_db.str) ||
       !Sp_handler::eq_routine_name(name2->m_name, name->m_name)))
  {
    bool exp= name2->m_explicit_name || name->m_explicit_name;
    my_error(ER_END_IDENTIFIER_DOES_NOT_MATCH, MYF(0),
             exp ? ErrConvDQName(name2).ptr() : name2->m_name.str,
             exp ? ErrConvDQName(name).ptr() : name->m_name.str);
    return true;
  }
  // TODO: reuse code in LEX::create_package_finalize and sp_head::set_stmt_end
  sphead->m_body.length= body_end - body_start;
  if (unlikely(!(sphead->m_body.str= thd->strmake(body_start,
                                                  sphead->m_body.length))))
    return true;

  size_t not_used;
  Lex_input_stream *lip= & thd->m_parser_state->m_lip;
  sphead->m_defstr.length= lip->get_cpp_ptr() - lip->get_cpp_buf();
  sphead->m_defstr.str= thd->strmake(lip->get_cpp_buf(), sphead->m_defstr.length);
  trim_whitespace(thd->charset(), &sphead->m_defstr, &not_used);

  sphead->restore_thd_mem_root(thd);
  sp_package *pkg= sphead->get_package();
  DBUG_ASSERT(pkg);
  return sphead->check_group_aggregate_instructions_forbid() ||
         pkg->validate_after_parser(thd);
}


bool LEX::add_grant_command(THD *thd, const List<LEX_COLUMN> &columns)
{
  if (columns.elements)
  {
    thd->parse_error();
    return true;
  }
  return false;
}


Item *LEX::make_item_func_substr(THD *thd, Item *a, Item *b, Item *c)
{
  return (thd->variables.sql_mode & MODE_ORACLE) ?
    new (thd->mem_root) Item_func_substr_oracle(thd, a, b, c) :
    new (thd->mem_root) Item_func_substr(thd, a, b, c);
}


Item *LEX::make_item_func_substr(THD *thd, Item *a, Item *b)
{
  return (thd->variables.sql_mode & MODE_ORACLE) ?
    new (thd->mem_root) Item_func_substr_oracle(thd, a, b) :
    new (thd->mem_root) Item_func_substr(thd, a, b);
}


Item *LEX::make_item_func_replace(THD *thd,
                                  Item *org,
                                  Item *find,
                                  Item *replace)
{
  return (thd->variables.sql_mode & MODE_ORACLE) ?
    new (thd->mem_root) Item_func_replace_oracle(thd, org, find, replace) :
    new (thd->mem_root) Item_func_replace(thd, org, find, replace);
}


bool SELECT_LEX::vers_push_field(THD *thd, TABLE_LIST *table,
                                 const LEX_CSTRING field_name)
{
  DBUG_ASSERT(field_name.str);
  Item_field *fld= new (thd->mem_root) Item_field(thd, &context,
                                                  table->db,
                                                  table->alias,
                                                  field_name);
  if (unlikely(!fld) || unlikely(item_list.push_back(fld)))
    return true;

  if (thd->lex->view_list.elements)
  {
    LEX_CSTRING *l;
    if (unlikely(!(l= thd->make_clex_string(field_name.str,
                                            field_name.length))) ||
        unlikely(thd->lex->view_list.push_back(l)))
      return true;
  }

  return false;
}


Item *Lex_trim_st::make_item_func_trim_std(THD *thd) const
{
  if (m_remove)
  {
    switch (m_spec) {
    case TRIM_BOTH:
      return new (thd->mem_root) Item_func_trim(thd, m_source, m_remove);
    case TRIM_LEADING:
      return new (thd->mem_root) Item_func_ltrim(thd, m_source, m_remove);
    case TRIM_TRAILING:
     return new (thd->mem_root) Item_func_rtrim(thd, m_source, m_remove);
    }
  }

  switch (m_spec) {
  case TRIM_BOTH:
    return new (thd->mem_root) Item_func_trim(thd, m_source);
  case TRIM_LEADING:
    return new (thd->mem_root) Item_func_ltrim(thd, m_source);
  case TRIM_TRAILING:
   return new (thd->mem_root) Item_func_rtrim(thd, m_source);
  }
  DBUG_ASSERT(0);
  return NULL;
}


Item *Lex_trim_st::make_item_func_trim_oracle(THD *thd) const
{
  if (m_remove)
  {
    switch (m_spec) {
    case TRIM_BOTH:
      return new (thd->mem_root) Item_func_trim_oracle(thd, m_source, m_remove);
    case TRIM_LEADING:
      return new (thd->mem_root) Item_func_ltrim_oracle(thd, m_source, m_remove);
    case TRIM_TRAILING:
     return new (thd->mem_root) Item_func_rtrim_oracle(thd, m_source, m_remove);
    }
  }

  switch (m_spec) {
  case TRIM_BOTH:
    return new (thd->mem_root) Item_func_trim_oracle(thd, m_source);
  case TRIM_LEADING:
    return new (thd->mem_root) Item_func_ltrim_oracle(thd, m_source);
  case TRIM_TRAILING:
   return new (thd->mem_root) Item_func_rtrim_oracle(thd, m_source);
  }
  DBUG_ASSERT(0);
  return NULL;
}


Item *Lex_trim_st::make_item_func_trim(THD *thd) const
{
  return (thd->variables.sql_mode & MODE_ORACLE) ?
         make_item_func_trim_oracle(thd) :
         make_item_func_trim_std(thd);
}


Item *LEX::make_item_func_call_generic(THD *thd, Lex_ident_cli_st *cdb,
                                       Lex_ident_cli_st *cname, List<Item> *args)
{
  Lex_ident_sys db(thd, cdb), name(thd, cname);
  if (db.is_null() || name.is_null())
    return NULL; // EOM
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

  if (!name.str || check_db_name((LEX_STRING*) static_cast<LEX_CSTRING*>(&db)))
  {
    my_error(ER_WRONG_DB_NAME, MYF(0), db.str);
    return NULL;
  }
  if (check_routine_name(&name))
    return NULL;

  Create_qfunc *builder= find_qualified_function_builder(thd);
  DBUG_ASSERT(builder);
  return builder->create_with_db(thd, &db, &name, true, args);
}


Item *LEX::make_item_func_call_native_or_parse_error(THD *thd,
                                                     Lex_ident_cli_st &name,
                                                     List<Item> *args)
{
  Create_func *builder= find_native_function_builder(thd, &name);
  DBUG_EXECUTE_IF("make_item_func_call_native_simulate_not_found",
                  builder= NULL;);
  if (builder)
    return builder->create_func(thd, &name, args);
  thd->parse_error(ER_SYNTAX_ERROR, name.end());
  return NULL;
}


Item *LEX::create_item_qualified_asterisk(THD *thd,
                                          const Lex_ident_sys_st *name)
{
  Item *item;
  if (!(item= new (thd->mem_root) Item_field(thd, current_context(),
                                             null_clex_str, *name,
                                             star_clex_str)))
    return NULL;
  current_select->with_wild++;
  return item;
}


Item *LEX::create_item_qualified_asterisk(THD *thd,
                                          const Lex_ident_sys_st *a,
                                          const Lex_ident_sys_st *b)
{
  Item *item;
  Lex_ident_sys_st schema= thd->client_capabilities & CLIENT_NO_SCHEMA ?
                           Lex_ident_sys() : *a;
  if (!(item= new (thd->mem_root) Item_field(thd, current_context(),
                                             schema, *b, star_clex_str)))
   return NULL;
  current_select->with_wild++;
  return item;
}


bool Lex_ident_sys_st::copy_ident_cli(THD *thd, const Lex_ident_cli_st *str)
{
  return thd->to_ident_sys_alloc(this, str);
}

bool Lex_ident_sys_st::copy_keyword(THD *thd, const Lex_ident_cli_st *str)
{
  return thd->make_lex_string(static_cast<LEX_CSTRING*>(this),
                              str->str, str->length) == NULL;
}

bool Lex_ident_sys_st::copy_or_convert(THD *thd,
                                       const Lex_ident_cli_st *src,
                                       CHARSET_INFO *cs)
{
  if (!src->is_8bit())
    return copy_keyword(thd, src); // 7bit string makes a wellformed identifier
  return convert(thd, src, cs);
}


bool Lex_ident_sys_st::copy_sys(THD *thd, const LEX_CSTRING *src)
{
  if (thd->check_string_for_wellformedness(src->str, src->length,
                                           system_charset_info))
    return true;
  return thd->make_lex_string(this, src->str, src->length) == NULL;
}


bool Lex_ident_sys_st::convert(THD *thd,
                               const LEX_CSTRING *src, CHARSET_INFO *cs)
{
  LEX_STRING tmp;
  if (thd->convert_with_error(system_charset_info, &tmp, cs,
                              src->str, src->length))
    return true;
  str=    tmp.str;
  length= tmp.length;
  return false;
}


bool Lex_ident_sys_st::to_size_number(ulonglong *to) const
{
  ulonglong number;
  uint text_shift_number= 0;
  longlong prefix_number;
  const char *start_ptr= str;
  size_t str_len= length;
  const char *end_ptr= start_ptr + str_len;
  int error;
  prefix_number= my_strtoll10(start_ptr, (char**) &end_ptr, &error);
  if (likely((start_ptr + str_len - 1) == end_ptr))
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
        my_error(ER_WRONG_SIZE_NUMBER, MYF(0));
        return true;
    }
    if (unlikely(prefix_number >> 31))
    {
      my_error(ER_SIZE_OVERFLOW_ERROR, MYF(0));
      return true;
    }
    number= prefix_number << text_shift_number;
  }
  else
  {
    my_error(ER_WRONG_SIZE_NUMBER, MYF(0));
    return true;
  }
  *to= number;
  return false;
}


bool LEX::part_values_current(THD *thd)
{
  partition_element *elem= part_info->curr_part_elem;
  if (!is_partition_management())
  {
    if (unlikely(part_info->part_type != VERSIONING_PARTITION))
    {
      my_error(ER_PARTITION_WRONG_TYPE, MYF(0), "SYSTEM_TIME");
      return true;
    }
  }
  else
  {
    DBUG_ASSERT(create_last_non_select_table);
    DBUG_ASSERT(create_last_non_select_table->table_name.str);
    // FIXME: other ALTER commands?
    my_error(ER_VERS_WRONG_PARTS, MYF(0),
             create_last_non_select_table->table_name.str);
    return true;
  }
  elem->type= partition_element::CURRENT;
  DBUG_ASSERT(part_info->vers_info);
  part_info->vers_info->now_part= elem;
  return false;
}


bool LEX::part_values_history(THD *thd)
{
  partition_element *elem= part_info->curr_part_elem;
  if (!is_partition_management())
  {
    if (unlikely(part_info->part_type != VERSIONING_PARTITION))
    {
      my_error(ER_PARTITION_WRONG_TYPE, MYF(0), "SYSTEM_TIME");
      return true;
    }
  }
  else
  {
    part_info->vers_init_info(thd);
    elem->id= UINT_MAX32;
  }
  DBUG_ASSERT(part_info->vers_info);
  if (unlikely(part_info->vers_info->now_part))
  {
    DBUG_ASSERT(create_last_non_select_table);
    DBUG_ASSERT(create_last_non_select_table->table_name.str);
    my_error(ER_VERS_WRONG_PARTS, MYF(0),
             create_last_non_select_table->table_name.str);
    return true;
  }
  elem->type= partition_element::HISTORY;
  return false;
}


bool LEX::last_field_generated_always_as_row_start_or_end(Lex_ident *p,
                                                          const char *type,
                                                          uint flag)
{
  if (unlikely(p->str))
  {
    my_error(ER_VERS_DUPLICATE_ROW_START_END, MYF(0), type,
             last_field->field_name.str);
    return true;
  }
  last_field->flags|= (flag | NOT_NULL_FLAG);
  DBUG_ASSERT(p);
  *p= last_field->field_name;
  return false;
}



bool LEX::last_field_generated_always_as_row_start()
{
  Vers_parse_info &info= vers_get_info();
  Lex_ident *p= &info.as_row.start;
  return last_field_generated_always_as_row_start_or_end(p, "START",
                                                         VERS_SYS_START_FLAG);
}


bool LEX::last_field_generated_always_as_row_end()
{
  Vers_parse_info &info= vers_get_info();
  Lex_ident *p= &info.as_row.end;
  return last_field_generated_always_as_row_start_or_end(p, "END",
                                                         VERS_SYS_END_FLAG);
}


void st_select_lex_unit::reset_distinct()
{
  union_distinct= NULL;
  for(SELECT_LEX *sl= first_select()->next_select();
      sl;
      sl= sl->next_select())
  {
    if (sl->distinct)
    {
      union_distinct= sl;
    }
  }
}


void st_select_lex_unit::fix_distinct()
{
  if (union_distinct && this != union_distinct->master_unit())
    reset_distinct();
}


void st_select_lex_unit::register_select_chain(SELECT_LEX *first_sel)
{
  DBUG_ASSERT(first_sel != 0);
  slave= first_sel;
  first_sel->prev= &slave;
  for(SELECT_LEX *sel=first_sel; sel; sel= sel->next_select())
  {
    sel->master= (st_select_lex_node *)this;
    uncacheable|= sel->uncacheable;
  }
}


void st_select_lex::register_unit(SELECT_LEX_UNIT *unit,
                                  Name_resolution_context *outer_context)
{
  if ((unit->next= slave))
    slave->prev= &unit->next;
  unit->prev= &slave;
  slave= unit;
  unit->master= this;
  uncacheable|= unit->uncacheable;

  for(SELECT_LEX *sel= unit->first_select();sel; sel= sel->next_select())
  {
    sel->context.outer_context= outer_context;
  }
}


void st_select_lex::add_statistics(SELECT_LEX_UNIT *unit)
{
  for (;
       unit;
       unit= unit->next_unit())
    for(SELECT_LEX *child= unit->first_select();
        child;
        child= child->next_select())
    {
      /*
        A subselect can add fields to an outer select.
        Reserve space for them.
      */
      select_n_where_fields+= child->select_n_where_fields;
      /*
        Aggregate functions in having clause may add fields
        to an outer select. Count them also.
      */
      select_n_having_items+= child->select_n_having_items;
    }
}


bool LEX::main_select_push()
{
  DBUG_ENTER("LEX::main_select_push");
  current_select_number= 1;
  builtin_select.select_number= 1;
  if (push_select(&builtin_select))
    DBUG_RETURN(TRUE);
  DBUG_RETURN(FALSE);
}

void Lex_select_lock::set_to(SELECT_LEX *sel)
{
  if (defined_lock)
  {
    if (sel->master_unit() &&
        sel == sel->master_unit()->fake_select_lex)
      sel->master_unit()->set_lock_to_the_last_select(*this);
    else
    {
      sel->parent_lex->safe_to_cache_query= 0;
      if (update_lock)
      {
        sel->lock_type= TL_WRITE;
        sel->set_lock_for_tables(TL_WRITE, false);
      }
      else
      {
        sel->lock_type= TL_READ_WITH_SHARED_LOCKS;
        sel->set_lock_for_tables(TL_READ_WITH_SHARED_LOCKS, false);
      }
    }
  }
}

bool Lex_order_limit_lock::set_to(SELECT_LEX *sel)
{
  /*TODO: lock */
  //if (lock.defined_lock && sel == sel->master_unit()->fake_select_lex)
  //  return TRUE;
  if (lock.defined_timeout)
  {
    THD *thd= sel->parent_lex->thd;
     if (set_statement_var_if_exists(thd,
                                     C_STRING_WITH_LEN("lock_wait_timeout"),
                                     lock.timeout) ||
         set_statement_var_if_exists(thd,
                                     C_STRING_WITH_LEN("innodb_lock_wait_timeout"),
                                     lock.timeout))
       return TRUE;
  }
  lock.set_to(sel);
  sel->explicit_limit= limit.explicit_limit;
  sel->select_limit= limit.select_limit;
  sel->offset_limit= limit.offset_limit;
  if (order_list)
  {
    if (sel->get_linkage() != GLOBAL_OPTIONS_TYPE &&
        sel->olap != UNSPECIFIED_OLAP_TYPE &&
        (sel->get_linkage() != UNION_TYPE || sel->braces))
    {
      my_error(ER_WRONG_USAGE, MYF(0),
          "CUBE/ROLLUP", "ORDER BY");
      return TRUE;
    }
    sel->order_list= *(order_list);
  }
  sel->is_set_query_expr_tail= true;
  return FALSE;
}


static void change_item_list_context(List<Item> *list,
                                     Name_resolution_context *context)
{
  List_iterator_fast<Item> it (*list);
  Item *item;
  while((item= it++))
  {
    item->walk(&Item::change_context_processor, FALSE, (void *)context);
  }
}


bool LEX::insert_select_hack(SELECT_LEX *sel)
{
  DBUG_ENTER("LEX::insert_select_hack");

  DBUG_ASSERT(first_select_lex() == &builtin_select);
  DBUG_ASSERT(sel != NULL);

  DBUG_ASSERT(builtin_select.first_inner_unit() == NULL);

  if (builtin_select.link_prev)
  {
    if ((*builtin_select.link_prev= builtin_select.link_next))
      ((st_select_lex *)builtin_select.link_next)->link_prev=
        builtin_select.link_prev;
    builtin_select.link_prev= NULL; // indicator of removal
  }

  if (set_main_unit(sel->master_unit()))
    return true;

  DBUG_ASSERT(builtin_select.table_list.elements == 1);
  TABLE_LIST *insert_table= builtin_select.table_list.first;

  if (!(insert_table->next_local= sel->table_list.first))
  {
    sel->table_list.next= &insert_table->next_local;
  }
  sel->table_list.first= insert_table;
  sel->table_list.elements++;
  insert_table->select_lex= sel;

  sel->context.first_name_resolution_table= insert_table;
  builtin_select.context= sel->context;
  change_item_list_context(&field_list, &sel->context);

  if (sel->tvc && !sel->next_select() &&
      (sql_command == SQLCOM_INSERT_SELECT ||
       sql_command == SQLCOM_REPLACE_SELECT))
  {
    DBUG_PRINT("info", ("'Usual' INSERT detected"));
    many_values= sel->tvc->lists_of_values;
    sel->options= sel->tvc->select_options;
    sel->tvc= NULL;
    if (sql_command == SQLCOM_INSERT_SELECT)
      sql_command= SQLCOM_INSERT;
    else
      sql_command= SQLCOM_REPLACE;
  }


  for (SELECT_LEX *sel= all_selects_list;
       sel;
       sel= sel->next_select_in_list())
  {
    if (sel->select_number != 1)
      sel->select_number--;
  };

  DBUG_RETURN(FALSE);
}


/**
  Create an Item_singlerow_subselect for a query expression.
*/

Item *LEX::create_item_query_expression(THD *thd,
                                        st_select_lex_unit *unit)
{
  if (clause_that_disallows_subselect)
  {
    my_error(ER_SUBQUERIES_NOT_SUPPORTED, MYF(0),
             clause_that_disallows_subselect);
    return NULL;
  }

  // Add the subtree of subquery to the current SELECT_LEX
  SELECT_LEX *curr_sel= select_stack_head();
  DBUG_ASSERT(current_select == curr_sel);
  if (!curr_sel)
  {
    curr_sel= &builtin_select;
    curr_sel->register_unit(unit, &curr_sel->context);
    curr_sel->add_statistics(unit);
  }

  return new (thd->mem_root)
    Item_singlerow_subselect(thd, unit->first_select());
}


SELECT_LEX_UNIT *LEX::parsed_select_expr_start(SELECT_LEX *s1, SELECT_LEX *s2,
                                               enum sub_select_type unit_type,
                                               bool distinct)
{
  SELECT_LEX_UNIT *res;
  SELECT_LEX *sel1;
  SELECT_LEX *sel2;
  if (!s1->next_select())
    sel1= s1;
  else
  {
    sel1= wrap_unit_into_derived(s1->master_unit());
    if (!sel1)
      return NULL;
  }
  if (!s2->next_select())
    sel2= s2;
  else
  {
    sel2= wrap_unit_into_derived(s2->master_unit());
    if (!sel2)
      return NULL;
  }
  sel1->link_neighbour(sel2);
  sel2->set_linkage_and_distinct(unit_type, distinct);
  sel2->first_nested= sel1->first_nested= sel1;
  res= create_unit(sel1);
  if (res == NULL)
    return NULL;
  res->pre_last_parse= sel1;
  push_select(res->fake_select_lex);
  return res;
}


SELECT_LEX_UNIT *LEX::parsed_select_expr_cont(SELECT_LEX_UNIT *unit,
                                              SELECT_LEX *s2,
                                              enum sub_select_type unit_type,
                                              bool distinct, bool oracle)
{
  DBUG_ASSERT(!s2->next_select());
  SELECT_LEX *sel1= s2;
  SELECT_LEX *last= unit->pre_last_parse->next_select();

  int cmp= oracle? 0 : cmp_unit_op(unit_type, last->get_linkage());
  if (cmp == 0)
  {
    sel1->first_nested= last->first_nested;
  }
  else if (cmp > 0)
  {
    last->first_nested= unit->pre_last_parse;
    sel1->first_nested= last;
  }
  else /* cmp < 0 */
  {
    SELECT_LEX *first_in_nest= last->first_nested;
    if (first_in_nest->first_nested != first_in_nest)
    {
      /* There is a priority jump starting from first_in_nest */
      if ((last= create_priority_nest(first_in_nest)) == NULL)
        return NULL;
      unit->fix_distinct();
    }
    sel1->first_nested= last->first_nested;
  }
  last->link_neighbour(sel1);
  sel1->set_master_unit(unit);
  sel1->set_linkage_and_distinct(unit_type, distinct);
  unit->pre_last_parse= last;
  return unit;
}


/**
  Add primary expression as the next term in a given query expression body
  pruducing a new query expression body
*/

SELECT_LEX_UNIT *
LEX::add_primary_to_query_expression_body(SELECT_LEX_UNIT *unit,
                                          SELECT_LEX *sel,
                                          enum sub_select_type unit_type,
                                          bool distinct,
                                          bool oracle)
{
  SELECT_LEX *sel2= sel;
  if (sel->master_unit() && sel->master_unit()->first_select()->next_select())
  {
    sel2= wrap_unit_into_derived(sel->master_unit());
    if (!sel2)
      return NULL;
  }
  SELECT_LEX *sel1= unit->first_select();
  if (!sel1->next_select())
    unit= parsed_select_expr_start(sel1, sel2, unit_type, distinct);
  else
    unit= parsed_select_expr_cont(unit, sel2, unit_type, distinct, oracle);
  return unit;
}


SELECT_LEX_UNIT *
LEX::add_primary_to_query_expression_body(SELECT_LEX_UNIT *unit,
                                          SELECT_LEX *sel,
                                          enum sub_select_type unit_type,
                                          bool distinct)
{
  return
    add_primary_to_query_expression_body(unit, sel, unit_type, distinct,
                                         thd->variables.sql_mode & MODE_ORACLE);
}

/**
  Add query primary to a parenthesized query primary
  pruducing a new query expression body
*/

SELECT_LEX_UNIT *
LEX::add_primary_to_query_expression_body_ext_parens(
                                                 SELECT_LEX_UNIT *unit,
                                                 SELECT_LEX *sel,
                                                 enum sub_select_type unit_type,
                                                 bool distinct)
{
  SELECT_LEX *sel1= unit->first_select();
  if (unit->first_select()->next_select())
  {
    sel1= wrap_unit_into_derived(unit);
    if (!sel1)
      return NULL;
    if (!create_unit(sel1))
      return NULL;
  }
  SELECT_LEX *sel2= sel;
  if (sel->master_unit() && sel->master_unit()->first_select()->next_select())
  {
    sel2= wrap_unit_into_derived(sel->master_unit());
    if (!sel2)
      return NULL;
  }
  unit= parsed_select_expr_start(sel1, sel2, unit_type, distinct);
  return unit;
}


/**
  Process multi-operand query expression body
*/

bool LEX::parsed_multi_operand_query_expression_body(SELECT_LEX_UNIT *unit)
{
  SELECT_LEX *first_in_nest=
    unit->pre_last_parse->next_select()->first_nested;
  if (first_in_nest->first_nested != first_in_nest)
  {
    /* There is a priority jump starting from first_in_nest */
    if (create_priority_nest(first_in_nest) == NULL)
      return true;
    unit->fix_distinct();
  }
  return false;
}


/**
  Add non-empty tail to a query expression body
*/

SELECT_LEX_UNIT *LEX::add_tail_to_query_expression_body(SELECT_LEX_UNIT *unit,
                                                        Lex_order_limit_lock *l)
{
  DBUG_ASSERT(l != NULL);
  pop_select();
  SELECT_LEX *sel= unit->first_select()->next_select() ? unit->fake_select_lex :
                                                         unit->first_select();
  l->set_to(sel);
  return unit;
}


/**
  Add non-empty tail to a parenthesized query primary
*/

SELECT_LEX_UNIT *
LEX::add_tail_to_query_expression_body_ext_parens(SELECT_LEX_UNIT *unit,
                                                  Lex_order_limit_lock *l)
{
  SELECT_LEX *sel= unit->first_select()->next_select() ? unit->fake_select_lex :
                                                         unit->first_select();

  DBUG_ASSERT(l != NULL);

  pop_select();
  if (sel->is_set_query_expr_tail)
  {
    if (!l->order_list && !sel->explicit_limit)
      l->order_list= &sel->order_list;
    else
    {
      if (!unit)
        return NULL;
      sel= wrap_unit_into_derived(unit);
      if (!sel)
        return NULL;
     if (!create_unit(sel))
      return NULL;
   }
  }
  l->set_to(sel);
  return sel->master_unit();
}


/**
  Process subselect parsing
*/

SELECT_LEX *LEX::parsed_subselect(SELECT_LEX_UNIT *unit)
{
  if (clause_that_disallows_subselect)
  {
    my_error(ER_SUBQUERIES_NOT_SUPPORTED, MYF(0),
             clause_that_disallows_subselect);
    return NULL;
  }

  // Add the subtree of subquery to the current SELECT_LEX
  SELECT_LEX *curr_sel= select_stack_head();
  DBUG_ASSERT(current_select == curr_sel);
  if (curr_sel)
  {
    curr_sel->register_unit(unit, &curr_sel->context);
    curr_sel->add_statistics(unit);
  }

  return unit->first_select();
}


/**
  Process INSERT-like select
*/

bool LEX::parsed_insert_select(SELECT_LEX *first_select)
{
  if (sql_command == SQLCOM_INSERT ||
      sql_command == SQLCOM_REPLACE)
  {
    if (sql_command == SQLCOM_INSERT)
      sql_command= SQLCOM_INSERT_SELECT;
    else
      sql_command= SQLCOM_REPLACE_SELECT;
  }
  insert_select_hack(first_select);
  if (check_main_unit_semantics())
    return true;

  // fix "main" select
  SELECT_LEX *blt __attribute__((unused))= pop_select();
  DBUG_ASSERT(blt == &builtin_select);
  push_select(first_select);
  return false;
}


bool LEX::parsed_TVC_start()
{
  SELECT_LEX *sel;
  many_values.empty();
  insert_list= 0;
  if (!(sel= alloc_select(TRUE)) ||
        push_select(sel))
    return true;
  sel->init_select();
  sel->braces= FALSE; // just initialisation
  return false;
}


SELECT_LEX *LEX::parsed_TVC_end()
{

  SELECT_LEX *res= pop_select(); // above TVC select
  if (!(res->tvc=
        new (thd->mem_root) table_value_constr(many_values,
          res,
          res->options)))
    return NULL;
  many_values.empty();
  return res;
}



TABLE_LIST *LEX::parsed_derived_table(SELECT_LEX_UNIT *unit,
                                     int for_system_time,
                                     LEX_CSTRING *alias)
{
  TABLE_LIST *res;
  derived_tables|= DERIVED_SUBQUERY;
  unit->first_select()->set_linkage(DERIVED_TABLE_TYPE);

  // Add the subtree of subquery to the current SELECT_LEX
  SELECT_LEX *curr_sel= select_stack_head();
  DBUG_ASSERT(current_select == curr_sel);

  Table_ident *ti= new (thd->mem_root) Table_ident(unit);
  if (ti == NULL)
    return NULL;
  if (!(res= curr_sel->add_table_to_list(thd, ti, alias, 0,
                                         TL_READ, MDL_SHARED_READ)))
    return NULL;
  if (for_system_time)
  {
    res->vers_conditions= vers_conditions;
  }
  return res;
}

bool LEX::parsed_create_view(SELECT_LEX_UNIT *unit, int check)
{
  SQL_I_List<TABLE_LIST> *save= &first_select_lex()->table_list;
  if (set_main_unit(unit))
    return true;
  if (check_main_unit_semantics())
    return true;
  first_select_lex()->table_list.push_front(save);
  current_select= first_select_lex();
  size_t len= thd->m_parser_state->m_lip.get_cpp_ptr() -
    create_view->select.str;
  void *create_view_select= thd->memdup(create_view->select.str, len);
  create_view->select.length= len;
  create_view->select.str= (char *) create_view_select;
  size_t not_used;
  trim_whitespace(thd->charset(),
      &create_view->select, &not_used);
  create_view->check= check;
  parsing_options.allows_variable= TRUE;
  return false;
}

bool LEX::select_finalize(st_select_lex_unit *expr)
{
  sql_command= SQLCOM_SELECT;
  selects_allow_into= TRUE;
  selects_allow_procedure= TRUE;
  if (set_main_unit(expr))
    return true;
  return check_main_unit_semantics();
}


bool LEX::select_finalize(st_select_lex_unit *expr, Lex_select_lock l)
{
  return expr->set_lock_to_the_last_select(l) ||
         select_finalize(expr);
}


/*
  "IN" and "EXISTS" subselect can appear in two statement types:

  1. Statements that can have table columns, such as SELECT, DELETE, UPDATE
  2. Statements that cannot have table columns, e.g:
     RETURN ((1) IN (SELECT * FROM t1))
     IF ((1) IN (SELECT * FROM t1))

  Statements of the first type call master_select_push() in the beginning.
  In such case everything is properly linked.

  Statements of the second type do not call mastr_select_push().
  Here we catch the second case and relink thd->lex->builtin_select and
  select_lex to properly point to each other.

  QQ: Shouldn't subselects of other type also call relink_hack()?
  QQ: Can we do it at constructor time instead?
*/

void LEX::relink_hack(st_select_lex *select_lex)
{
  if (!select_stack_top) // Statements of the second type
  {
    if (!select_lex->get_master()->get_master())
      ((st_select_lex *) select_lex->get_master())->
        set_master(&builtin_select);
    if (!builtin_select.get_slave())
      builtin_select.set_slave(select_lex->get_master());
  }
}


bool SELECT_LEX_UNIT::set_lock_to_the_last_select(Lex_select_lock l)
{
  if (l.defined_lock)
  {
    SELECT_LEX *sel= first_select();
    while (sel->next_select())
      sel= sel->next_select();
    if (sel->braces)
    {
      my_error(ER_WRONG_USAGE, MYF(0), "lock options",
               "SELECT in brackets");
      return TRUE;
    }
    l.set_to(sel);
  }
  return FALSE;
}

/**
  Generate unique name for generated derived table for this SELECT
*/

bool SELECT_LEX::make_unique_derived_name(THD *thd, LEX_CSTRING *alias)
{
  // uint32 digits + two underscores + trailing '\0'
  char buff[MAX_INT_WIDTH + 2 + 1];
  alias->length= my_snprintf(buff, sizeof(buff), "__%u", select_number);
  alias->str= thd->strmake(buff, alias->length);
  return !alias->str;
}


/*
  Make a new sp_instr_stmt and set its m_query to a concatenation
  of two strings.
*/
bool LEX::new_sp_instr_stmt(THD *thd,
                            const LEX_CSTRING &prefix,
                            const LEX_CSTRING &suffix)
{
  LEX_STRING qbuff;
  sp_instr_stmt *i;

  if (!(i= new (thd->mem_root) sp_instr_stmt(sphead->instructions(),
                                             spcont, this)))
    return true;

  qbuff.length= prefix.length + suffix.length;
  if (!(qbuff.str= (char*) alloc_root(thd->mem_root, qbuff.length + 1)))
    return true;
  memcpy(qbuff.str, prefix.str, prefix.length);
  strmake(qbuff.str + prefix.length, suffix.str, suffix.length);
  i->m_query= qbuff;
  return sphead->add_instr(i);
}


bool LEX::sp_proc_stmt_statement_finalize_buf(THD *thd, const LEX_CSTRING &qbuf)
{
  sphead->m_flags|= sp_get_flags_for_command(this);
  /* "USE db" doesn't work in a procedure */
  if (unlikely(sql_command == SQLCOM_CHANGE_DB))
  {
    my_error(ER_SP_BADSTATEMENT, MYF(0), "USE");
    return true;
  }
  /*
    Don't add an instruction for SET statements, since all
    instructions for them were already added during processing
    of "set" rule.
  */
  DBUG_ASSERT(sql_command != SQLCOM_SET_OPTION || var_list.is_empty());
  if (sql_command != SQLCOM_SET_OPTION)
    return new_sp_instr_stmt(thd, empty_clex_str, qbuf);
  return false;
}


bool LEX::sp_proc_stmt_statement_finalize(THD *thd, bool no_lookahead)
{
  // Extract the query statement from the tokenizer
  Lex_input_stream *lip= &thd->m_parser_state->m_lip;
  Lex_cstring qbuf(sphead->m_tmp_query, no_lookahead ? lip->get_ptr() :
                                                       lip->get_tok_start());
  return LEX::sp_proc_stmt_statement_finalize_buf(thd, qbuf);
}


/**
  @brief
    Extract the condition that can be pushed into WHERE clause

  @param thd             the thread handle
  @param cond            the condition from which to extract a pushed condition
  @param remaining_cond  IN/OUT the condition that will remain of cond after
                         the extraction
  @param transformer     the transformer callback function to be
                         applied to the fields of the condition so it
                         can be pushed`
  @param arg             parameter to be passed to the transformer

  @details
    This function builds the most restrictive condition depending only on
    the fields used in the GROUP BY of this SELECT. These fields were
    collected before in grouping_tmp_fields list of this SELECT.

    First this method checks if this SELECT doesn't have any aggregation
    functions and has no GROUP BY clause. If so cond can be entirely pushed
    into WHERE.

    Otherwise the method checks if there is a condition depending only on
    grouping fields that can be extracted from cond.

    The condition that can be pushed into WHERE should be transformed.
    It is done by transformer.

    The extracted condition is saved in cond_pushed_into_where of this select.
    cond can remain un empty after the extraction of the condition that can be
    pushed into WHERE. It is saved in remaining_cond.

  @note
    This method is called for pushdown conditions into materialized
    derived tables/views optimization.
    Item::derived_field_transformer_for_where is passed as the actual
    callback function.
    Also it is called for pushdown into materialized IN subqueries.
    Item::in_subq_field_transformer_for_where is passed as the actual
    callback function.
*/

void st_select_lex::pushdown_cond_into_where_clause(THD *thd, Item *cond,
                                                    Item **remaining_cond,
                                                    Item_transformer transformer,
                                                    uchar *arg)
{
  if (!cond_pushdown_is_allowed())
    return;
  thd->lex->current_select= this;
  if (have_window_funcs())
  {
    Item *cond_over_partition_fields;
    check_cond_extraction_for_grouping_fields(thd, cond);
    cond_over_partition_fields=
      build_cond_for_grouping_fields(thd, cond, true);
    if (cond_over_partition_fields)
      cond_over_partition_fields= cond_over_partition_fields->transform(thd,
                                &Item::grouping_field_transformer_for_where,
                                (uchar*) this);
    if (cond_over_partition_fields)
    {
      cond_over_partition_fields->walk(
        &Item::cleanup_excluding_const_fields_processor, 0, 0);
      cond_pushed_into_where= cond_over_partition_fields;
    }

    return;
  }

  if (!join->group_list && !with_sum_func)
  {
    cond=
      cond->transform(thd, transformer, arg);
    if (cond)
    {
      cond->walk(
        &Item::cleanup_excluding_const_fields_processor, 0, 0);
      cond_pushed_into_where= cond;
    }

    return;
  }

  /*
    Figure out what can be extracted from cond and pushed into
    the WHERE clause of this select.
  */
  Item *cond_over_grouping_fields;
  check_cond_extraction_for_grouping_fields(thd, cond);
  cond_over_grouping_fields=
    build_cond_for_grouping_fields(thd, cond, true);

  /*
    Transform references to the columns of condition that can be pushed
    into WHERE so it can be pushed.
  */
  if (cond_over_grouping_fields)
    cond_over_grouping_fields= cond_over_grouping_fields->transform(thd,
                            &Item::grouping_field_transformer_for_where,
                            (uchar*) this);

  if (cond_over_grouping_fields)
  {

    /*
      Remove top conjuncts in cond that has been pushed into the WHERE
      clause of this select
    */
    cond= remove_pushed_top_conjuncts(thd, cond);

    cond_over_grouping_fields->walk(
      &Item::cleanup_excluding_const_fields_processor, 0, 0);
    cond_pushed_into_where= cond_over_grouping_fields;
  }

  *remaining_cond= cond;
}


/**
  @brief
    Mark OR-conditions as non-pushable to avoid repeatable pushdown

  @param cond  the processed condition

  @details
    Consider pushdown into the materialized derived table/view.
    Consider OR condition that can be pushed into HAVING and some
    parts of this OR condition that can be pushed into WHERE.

    On example:

    SELECT *
    FROM t1,
    (
      SELECT a,MAX(c) AS m_c
      GROUP BY a
    ) AS dt
    WHERE ((dt.m_c>10) AND (dt.a>2)) OR ((dt.m_c<7) and (dt.a<3)) AND
          (t1.a=v1.a);


    Here ((dt.m_c>10) AND (dt.a>2)) OR ((dt.m_c<7) and (dt.a<3)) or1
    can be pushed down into the HAVING of the materialized
    derived table dt.

    (dt.a>2) OR (dt.a<3) part of or1 depends only on grouping fields
    of dt and can be pushed into WHERE.

    As a result:

    SELECT *
    FROM t1,
    (
      SELECT a,MAX(c) AS m_c
      WHERE (dt.a>2) OR (dt.a<3)
      GROUP BY a
      HAVING ((dt.m_c>10) AND (dt.a>2)) OR ((dt.m_c<7) and (dt.a<3))
    ) AS dt
    WHERE ((dt.m_c>10) AND (dt.a>2)) OR ((dt.m_c<7) and (dt.a<3)) AND
          (t1.a=v1.a);


    Here (dt.a>2) OR (dt.a<3) also remains in HAVING of dt.
    When SELECT that defines df is processed HAVING pushdown optimization
    is made. In HAVING pushdown optimization it will extract
    (dt.a>2) OR (dt.a<3) condition from or1 again and push it into WHERE.
    This will cause duplicate conditions in WHERE of dt.

    To avoid repeatable pushdown such OR conditions as or1 describen
    above are marked with NO_EXTRACTION_FL.

  @note
    This method is called for pushdown into materialized
    derived tables/views/IN subqueries optimization.
*/

void mark_or_conds_to_avoid_pushdown(Item *cond)
{
  if (cond->type() == Item::COND_ITEM &&
      ((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
  {
    List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
    {
      if (item->type() == Item::COND_ITEM &&
          ((Item_cond*) item)->functype() == Item_func::COND_OR_FUNC)
        item->set_extraction_flag(NO_EXTRACTION_FL);
    }
  }
  else if (cond->type() == Item::COND_ITEM &&
          ((Item_cond*) cond)->functype() == Item_func::COND_OR_FUNC)
    cond->set_extraction_flag(NO_EXTRACTION_FL);
}

/**
  @brief
    Get condition that can be pushed from HAVING into WHERE

  @param thd   the thread handle
  @param cond  the condition from which to extract the condition

  @details
    The method collects in attach_to_conds list conditions from cond
    that can be pushed from HAVING into WHERE.

    Conditions that can be pushed were marked with FULL_EXTRACTION_FL in
    check_cond_extraction_for_grouping_fields() method.
    Conditions that can't be pushed were marked with NO_EXTRACTION_FL.
    Conditions which parts can be pushed weren't marked.

    There are two types of conditions that can be pushed:
    1. Condition that can be simply moved from HAVING
       (if cond is marked with FULL_EXTRACTION_FL or
           cond is an AND condition and some of its parts are marked with
           FULL_EXTRACTION_FL)
       In this case condition is transformed and pushed into attach_to_conds
       list.
    2. Part of some other condition c1 that can't be entirely pushed
       (if с1 isn't marked with any flag).

       For example:

       SELECT t1.a,MAX(t1.b),t1.c
       FROM t1
       GROUP BY t1.a
       HAVING ((t1.a > 5) AND (t1.c < 3)) OR (t1.a = 3);

       Here (t1.a > 5) OR (t1.a = 3) from HAVING can be pushed into WHERE.

       In this case build_pushable_cond() is called for c1.
       This method builds a clone of the c1 part that can be pushed.

    Transformation mentioned above is made with multiple_equality_transformer
    transformer. It transforms all multiple equalities in the extracted
    condition into the set of equalities.

  @note
    Conditions that can be pushed are collected in attach_to_conds in this way:
    1. if cond is an AND condition its parts that can be pushed into WHERE
       are added to attach_to_conds list separately.
    2. in all other cases conditions are pushed into the list entirely.

  @retval
    true  - if an error occurs
    false - otherwise
*/

bool
st_select_lex::build_pushable_cond_for_having_pushdown(THD *thd, Item *cond)
{
  List<Item> equalities;

  /* Condition can't be pushed */
  if (cond->get_extraction_flag() == NO_EXTRACTION_FL)
    return false;

  /**
    Condition can be pushed entirely.
    Transform its multiple equalities and add to attach_to_conds list.
  */
  if (cond->get_extraction_flag() == FULL_EXTRACTION_FL)
  {
    Item *result= cond->transform(thd,
                                  &Item::multiple_equality_transformer,
                                  (uchar *)this);
    if (!result)
      return true;
    if (result->type() == Item::COND_ITEM &&
        ((Item_cond*) result)->functype() == Item_func::COND_AND_FUNC)
    {
      List_iterator<Item> li(*((Item_cond*) result)->argument_list());
      Item *item;
      while ((item= li++))
      {
        if (attach_to_conds.push_back(item, thd->mem_root))
          return true;
      }
    }
    else
    {
      if (attach_to_conds.push_back(result, thd->mem_root))
        return true;
    }
    return false;
  }

  /**
    There is no flag set for this condition. It means that some
    part of this condition can be pushed.
  */
  if (cond->type() != Item::COND_ITEM)
    return false;

  if (((Item_cond *)cond)->functype() != Item_cond::COND_AND_FUNC)
  {
    /*
      cond is not a conjunctive formula and it cannot be pushed into WHERE.
      Try to extract a formula that can be pushed.
    */
    Item *fix= cond->build_pushable_cond(thd, 0, 0);
    if (!fix)
      return false;
    if (attach_to_conds.push_back(fix, thd->mem_root))
      return true;
  }
  else
  {
    List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
    {
      if (item->get_extraction_flag() == NO_EXTRACTION_FL)
        continue;
      else if (item->get_extraction_flag() == FULL_EXTRACTION_FL)
      {
        Item *result= item->transform(thd,
                                      &Item::multiple_equality_transformer,
                                      (uchar *)item);
        if (!result)
          return true;
        if (result->type() == Item::COND_ITEM &&
           ((Item_cond*) result)->functype() == Item_func::COND_AND_FUNC)
        {
          List_iterator<Item> li(*((Item_cond*) result)->argument_list());
          Item *item;
          while ((item=li++))
          {
            if (attach_to_conds.push_back(item, thd->mem_root))
              return true;
          }
        }
        else
        {
          if (attach_to_conds.push_back(result, thd->mem_root))
            return true;
        }
      }
      else
      {
        Item *fix= item->build_pushable_cond(thd, 0, 0);
        if (!fix)
          continue;
        if (attach_to_conds.push_back(fix, thd->mem_root))
          return true;
      }
    }
  }
  return false;
}


/**
  Check if item is equal to some field in Field_pair 'field_pair'
  from 'pair_list' and return found 'field_pair' if it exists.
*/

Field_pair *get_corresponding_field_pair(Item *item,
                                         List<Field_pair> pair_list)
{
  DBUG_ASSERT(item->type() == Item::FIELD_ITEM ||
              (item->type() == Item::REF_ITEM &&
               ((((Item_ref *) item)->ref_type() == Item_ref::VIEW_REF) ||
               (((Item_ref *) item)->ref_type() == Item_ref::REF))));

  List_iterator<Field_pair> it(pair_list);
  Field_pair *field_pair;
  Item_field *field_item= (Item_field *) (item->real_item());
  while ((field_pair= it++))
  {
    if (field_item->field == field_pair->field)
      return field_pair;
  }
  return NULL;
}


/**
  @brief
    Collect fields from multiple equalities which are equal to grouping

  @param thd  the thread handle

  @details
    This method checks if multiple equalities of the WHERE clause contain
    fields from GROUP BY of this SELECT. If so all fields of such multiple
    equalities are collected in grouping_tmp_fields list without repetitions.

  @retval
    true  - if an error occurs
    false - otherwise
*/

bool st_select_lex::collect_fields_equal_to_grouping(THD *thd)
{
  if (!join->cond_equal || join->cond_equal->is_empty())
    return false;

  List_iterator_fast<Item_equal> li(join->cond_equal->current_level);
  Item_equal *item_equal;

  while ((item_equal= li++))
  {
    Item_equal_fields_iterator it(*item_equal);
    Item *item;
    while ((item= it++))
    {
      if (get_corresponding_field_pair(item, grouping_tmp_fields))
        break;
    }
    if (!item)
      break;

    it.rewind();
    while ((item= it++))
    {
      if (get_corresponding_field_pair(item, grouping_tmp_fields))
        continue;
      Field_pair *grouping_tmp_field=
        new Field_pair(((Item_field *)item->real_item())->field, item);
      if (grouping_tmp_fields.push_back(grouping_tmp_field, thd->mem_root))
        return true;
    }
  }
  return false;
}


/**
  @brief
    Remove marked top conjuncts of HAVING for having pushdown

  @param thd   the thread handle
  @param cond  the condition which subformulas are to be removed

  @details
    This method removes from cond all subformulas that can be moved from HAVING
    into WHERE.

  @retval
     condition without removed subformulas
     0 if the whole 'cond' is removed
*/

Item *remove_pushed_top_conjuncts_for_having(THD *thd, Item *cond)
{
  /* Nothing to extract */
  if (cond->get_extraction_flag() == NO_EXTRACTION_FL)
  {
    cond->clear_extraction_flag();
    return cond;
  }
  /* cond can be pushed in WHERE entirely */
  if (cond->get_extraction_flag() == FULL_EXTRACTION_FL)
  {
    cond->clear_extraction_flag();
    return 0;
  }

  /* Some parts of cond can be pushed */
  if (cond->type() == Item::COND_ITEM &&
      ((Item_cond*) cond)->functype() == Item_func::COND_AND_FUNC)
  {
    List_iterator<Item> li(*((Item_cond*) cond)->argument_list());
    Item *item;
    while ((item=li++))
    {
      if (item->get_extraction_flag() == NO_EXTRACTION_FL)
        item->clear_extraction_flag();
      else if (item->get_extraction_flag() == FULL_EXTRACTION_FL)
      {
        if (item->type() == Item::FUNC_ITEM &&
            ((Item_func*) item)->functype() == Item_func::MULT_EQUAL_FUNC)
          item->set_extraction_flag(DELETION_FL);
        else
        {
          item->clear_extraction_flag();
          li.remove();
        }
      }
    }
    switch (((Item_cond*) cond)->argument_list()->elements)
    {
    case 0:
      return 0;
    case 1:
      return (((Item_cond*) cond)->argument_list()->head());
    default:
      return cond;
    }
  }
  return cond;
}


/**
  @brief
    Extract condition that can be pushed from HAVING into WHERE

  @param thd           the thread handle
  @param having        the HAVING clause of this select
  @param having_equal  multiple equalities of HAVING

  @details
    This method builds a set of conditions dependent only on
    fields used in the GROUP BY of this select (directly or indirectly
    through equalities). These conditions are extracted from the HAVING
    clause of this select.
    The method saves these conditions into attach_to_conds list and removes
    from HAVING conditions that can be entirely pushed into WHERE.

    Example of the HAVING pushdown transformation:

    SELECT t1.a,MAX(t1.b)
    FROM t1
    GROUP BY t1.a
    HAVING (t1.a>2) AND (MAX(c)>12);

    =>

    SELECT t1.a,MAX(t1.b)
    FROM t1
    WHERE (t1.a>2)
    GROUP BY t1.a
    HAVING (MAX(c)>12);

    In this method (t1.a>2) is not attached to the WHERE clause.
    It is pushed into the attach_to_conds list to be attached to
    the WHERE clause later.

    In details:
    1. Collect fields used in the GROUP BY grouping_fields of this SELECT
    2. Collect fields equal to grouping_fields from the WHERE clause
       of this SELECT and add them to the grouping_fields list.
    3. Extract the most restrictive condition from the HAVING clause of this
       select that depends only on the grouping fields (directly or indirectly
       through equality).
       If the extracted condition is an AND condition it is transformed into a
       list of all its conjuncts saved in attach_to_conds. Otherwise,
       the condition is put into attach_to_conds as the only its element.
    4. Remove conditions from HAVING clause that can be entirely pushed
       into WHERE.
       Multiple equalities are not removed but marked with DELETION_FL flag.
       They will be deleted later in substitite_for_best_equal_field() called
       for the HAVING condition.
    5. Unwrap fields wrapped in Item_ref wrappers contained in the condition
       of attach_to_conds so the condition could be pushed into WHERE.

  @note
    This method is similar to st_select_lex::pushdown_cond_into_where_clause().

  @retval TRUE   if an error occurs
  @retval FALSE  otherwise
*/

Item *st_select_lex::pushdown_from_having_into_where(THD *thd, Item *having)
{
  if (!having || !group_list.first)
    return having;
  if (!cond_pushdown_is_allowed())
    return having;

  st_select_lex *save_curr_select= thd->lex->current_select;
  thd->lex->current_select= this;

  /*
    1. Collect fields used in the GROUP BY grouping fields of this SELECT
    2. Collect fields equal to grouping_fields from the WHERE clause
       of this SELECT and add them to the grouping fields list.
  */
  if (collect_grouping_fields(thd) ||
      collect_fields_equal_to_grouping(thd))
    return having;

  /*
    3. Extract the most restrictive condition from the HAVING clause of this
       select that depends only on the grouping fields (directly or indirectly
       through equality).
       If the extracted condition is an AND condition it is transformed into a
       list of all its conjuncts saved in attach_to_conds. Otherwise,
       the condition is put into attach_to_conds as the only its element.
  */
  List_iterator_fast<Item> it(attach_to_conds);
  Item *item;
  check_cond_extraction_for_grouping_fields(thd, having);
  if (build_pushable_cond_for_having_pushdown(thd, having))
  {
    attach_to_conds.empty();
    goto exit;
  }
  if (!attach_to_conds.elements)
    goto exit;

  /*
    4. Remove conditions from HAVING clause that can be entirely pushed
       into WHERE.
       Multiple equalities are not removed but marked with DELETION_FL flag.
       They will be deleted later in substitite_for_best_equal_field() called
       for the HAVING condition.
  */
  having= remove_pushed_top_conjuncts_for_having(thd, having);

  /*
    Change join->cond_equal which points to the multiple equalities of
    the top level of HAVING.
    Removal of AND conditions may leave only one conjunct in HAVING.

    Example 1:
    SELECT *
    FROM t1
    GROUP BY t1.a
    (t1.a < 2) AND (t1.b = 2)

    (t1.a < 2) is pushed into WHERE.
    join->cond_equal should point on (t1.b = 2) multiple equality now.

    Example 2:
    SELECT *
    FROM t1
    GROUP BY t1.a
    (t1.a = 2) AND (t1.b < 2)

    (t1.a = 2) is pushed into WHERE.
    join->cond_equal should be NULL now.
  */
  if (having &&
      having->type() == Item::FUNC_ITEM &&
      ((Item_func*) having)->functype() == Item_func::MULT_EQUAL_FUNC)
    join->having_equal= new (thd->mem_root) COND_EQUAL((Item_equal *)having,
                                                       thd->mem_root);
  else if (!having ||
           having->type() != Item::COND_ITEM ||
           ((Item_cond *)having)->functype() != Item_cond::COND_AND_FUNC)
    join->having_equal= 0;

  /*
    5. Unwrap fields wrapped in Item_ref wrappers contained in the condition
       of attach_to_conds so the condition could be pushed into WHERE.
  */
  it.rewind();
  while ((item=it++))
  {
    item= item->transform(thd,
                          &Item::field_transformer_for_having_pushdown,
                          (uchar *)this);

    if (item->walk(&Item::cleanup_excluding_immutables_processor, 0, STOP_PTR)
        || item->fix_fields(thd, NULL))
    {
      attach_to_conds.empty();
      goto exit;
    }
  }
exit:
  thd->lex->current_select= save_curr_select;
  return having;
}


bool LEX::stmt_install_plugin(const DDL_options_st &opt,
                              const Lex_ident_sys_st &name,
                              const LEX_CSTRING &soname)
{
  create_info.init();
  if (add_create_options_with_check(opt))
    return true;
  sql_command= SQLCOM_INSTALL_PLUGIN;
  comment= name;
  ident= soname;
  return false;
}


void LEX::stmt_install_plugin(const LEX_CSTRING &soname)
{
  sql_command= SQLCOM_INSTALL_PLUGIN;
  comment= null_clex_str;
  ident= soname;
}


bool LEX::stmt_uninstall_plugin_by_name(const DDL_options_st &opt,
                                        const Lex_ident_sys_st &name)
{
  check_opt.init();
  if (add_create_options_with_check(opt))
    return true;
  sql_command= SQLCOM_UNINSTALL_PLUGIN;
  comment= name;
  ident= null_clex_str;
  return false;
}


bool LEX::stmt_uninstall_plugin_by_soname(const DDL_options_st &opt,
                                          const LEX_CSTRING &soname)
{
  check_opt.init();
  if (add_create_options_with_check(opt))
    return true;
  sql_command= SQLCOM_UNINSTALL_PLUGIN;
  comment= null_clex_str;
  ident= soname;
  return false;
}


bool LEX::stmt_prepare_validate(const char *stmt_type)
{
  if (unlikely(table_or_sp_used()))
  {
    my_error(ER_SUBQUERIES_NOT_SUPPORTED, MYF(0), stmt_type);
    return true;
  }
  return check_main_unit_semantics();
}


bool LEX::stmt_prepare(const Lex_ident_sys_st &ident, Item *code)
{
  sql_command= SQLCOM_PREPARE;
  if (stmt_prepare_validate("PREPARE..FROM"))
    return true;
  prepared_stmt.set(ident, code, NULL);
  return false;
}


bool LEX::stmt_execute_immediate(Item *code, List<Item> *params)
{
  sql_command= SQLCOM_EXECUTE_IMMEDIATE;
  if (stmt_prepare_validate("EXECUTE IMMEDIATE"))
    return true;
  static const Lex_ident_sys immediate(STRING_WITH_LEN("IMMEDIATE"));
  prepared_stmt.set(immediate, code, params);
  return false;
}


bool LEX::stmt_execute(const Lex_ident_sys_st &ident, List<Item> *params)
{
  sql_command= SQLCOM_EXECUTE;
  prepared_stmt.set(ident, NULL, params);
  return stmt_prepare_validate("EXECUTE..USING");
}


void LEX::stmt_deallocate_prepare(const Lex_ident_sys_st &ident)
{
  sql_command= SQLCOM_DEALLOCATE_PREPARE;
  prepared_stmt.set(ident, NULL, NULL);
}


bool LEX::stmt_alter_table_exchange_partition(Table_ident *table)
{
  DBUG_ASSERT(sql_command == SQLCOM_ALTER_TABLE);
  first_select_lex()->db= table->db;
  if (first_select_lex()->db.str == NULL &&
      copy_db_to(&first_select_lex()->db))
    return true;
  name= table->table;
  alter_info.partition_flags|= ALTER_PARTITION_EXCHANGE;
  if (!first_select_lex()->add_table_to_list(thd, table, NULL,
                                             TL_OPTION_UPDATING,
                                             TL_READ_NO_INSERT,
                                             MDL_SHARED_NO_WRITE))
    return true;
  DBUG_ASSERT(!m_sql_cmd);
  m_sql_cmd= new (thd->mem_root) Sql_cmd_alter_table_exchange_partition();
  return m_sql_cmd == NULL;
}


void LEX::stmt_purge_to(const LEX_CSTRING &to)
{
  type= 0;
  sql_command= SQLCOM_PURGE;
  to_log= to.str;
}


bool LEX::stmt_purge_before(Item *item)
{
  type= 0;
  sql_command= SQLCOM_PURGE_BEFORE;
  value_list.empty();
  value_list.push_front(item, thd->mem_root);
  return check_main_unit_semantics();
}


bool LEX::stmt_create_udf_function(const DDL_options_st &options,
                                   enum_sp_aggregate_type agg_type,
                                   const Lex_ident_sys_st &name,
                                   Item_result return_type,
                                   const LEX_CSTRING &soname)
{
  if (stmt_create_function_start(options))
    return true;

   if (unlikely(is_native_function(thd, &name)))
   {
     my_error(ER_NATIVE_FCT_NAME_COLLISION, MYF(0), name.str);
     return true;
   }
   sql_command= SQLCOM_CREATE_FUNCTION;
   udf.name= name;
   udf.returns= return_type;
   udf.dl= soname.str;
   udf.type= agg_type == GROUP_AGGREGATE ? UDFTYPE_AGGREGATE :
                                           UDFTYPE_FUNCTION;
   stmt_create_routine_finalize();
   return false;
}


bool LEX::stmt_create_stored_function_start(const DDL_options_st &options,
                                            enum_sp_aggregate_type agg_type,
                                            const sp_name *spname)
{
  if (stmt_create_function_start(options) ||
      unlikely(!make_sp_head_no_recursive(thd, spname,
                                          &sp_handler_function, agg_type)))
    return true;
  return false;
}


bool LEX::stmt_drop_function(const DDL_options_st &options,
                             const Lex_ident_sys_st &db,
                             const Lex_ident_sys_st &name)
{
  if (unlikely(db.str && check_db_name((LEX_STRING*) &db)))
  {
    my_error(ER_WRONG_DB_NAME, MYF(0), db.str);
    return true;
  }
  if (unlikely(sphead))
  {
    my_error(ER_SP_NO_DROP_SP, MYF(0), "FUNCTION");
    return true;
  }
  set_command(SQLCOM_DROP_FUNCTION, options);
  spname= new (thd->mem_root) sp_name(&db, &name, true);
  return spname == NULL;
}


bool LEX::stmt_drop_function(const DDL_options_st &options,
                             const Lex_ident_sys_st &name)
{
  LEX_CSTRING db= {0, 0};
  if (unlikely(sphead))
  {
    my_error(ER_SP_NO_DROP_SP, MYF(0), "FUNCTION");
    return true;
  }
  if (thd->db.str && unlikely(copy_db_to(&db)))
    return true;
  set_command(SQLCOM_DROP_FUNCTION, options);
  spname= new (thd->mem_root) sp_name(&db, &name, false);
  return spname == NULL;
}


bool LEX::stmt_drop_procedure(const DDL_options_st &options,
                              sp_name *name)
{
  if (unlikely(sphead))
  {
    my_error(ER_SP_NO_DROP_SP, MYF(0), "PROCEDURE");
    return true;
  }
  set_command(SQLCOM_DROP_PROCEDURE, options);
  spname= name;
  return false;
}


bool LEX::stmt_alter_function_start(sp_name *name)
{
  if (unlikely(sphead))
  {
    my_error(ER_SP_NO_DROP_SP, MYF(0), "FUNCTION");
    return true;
  }
  if (main_select_push())
    return true;
  sp_chistics.init();
  sql_command= SQLCOM_ALTER_FUNCTION;
  spname= name;
  return false;
}


bool LEX::stmt_alter_procedure_start(sp_name *name)
{
  if (unlikely(sphead))
  {
    my_error(ER_SP_NO_DROP_SP, MYF(0), "PROCEDURE");
    return true;
  }
  if (main_select_push())
    return true;
  sp_chistics.init();
  sql_command= SQLCOM_ALTER_PROCEDURE;
  spname= name;
  return false;
}


Spvar_definition *LEX::row_field_name(THD *thd, const Lex_ident_sys_st &name)
{
  Spvar_definition *res;
  if (unlikely(check_string_char_length(&name, 0, NAME_CHAR_LEN,
                                        system_charset_info, 1)))
  {
    my_error(ER_TOO_LONG_IDENT, MYF(0), name.str);
    return NULL;
  }
  if (unlikely(!(res= new (thd->mem_root) Spvar_definition())))
    return NULL;
  init_last_field(res, &name, thd->variables.collation_database);
  return res;
}


Item *
Lex_cast_type_st::create_typecast_item_or_error(THD *thd, Item *item,
                                                CHARSET_INFO *cs) const
{
  Item *tmp= create_typecast_item(thd, item, cs);
  if (!tmp)
  {
    Name name= m_type_handler->name();
    char buf[128];
    size_t length= my_snprintf(buf, sizeof(buf), "CAST(expr AS %.*s)",
                               (int) name.length(), name.ptr());
    my_error(ER_UNKNOWN_OPERATOR, MYF(0),
             ErrConvString(buf, length, system_charset_info).ptr());
  }
  return tmp;
}


void Lex_field_type_st::set_handler_length_flags(const Type_handler *handler,
                                                 const char *length,
                                                 uint32 flags)
{
  DBUG_ASSERT(!handler->is_unsigned());
  if (flags & UNSIGNED_FLAG)
    handler= handler->type_handler_unsigned();
  set(handler, length, NULL);
}


bool LEX::set_field_type_udt(Lex_field_type_st *type,
                             const LEX_CSTRING &name,
                             const Lex_length_and_dec_st &attr)
{
  const Type_handler *h;
  if (!(h= Type_handler::handler_by_name_or_error(thd, name)))
    return true;
  type->set(h, attr);
  charset= &my_charset_bin;
  return false;
}


bool LEX::set_cast_type_udt(Lex_cast_type_st *type,
                             const LEX_CSTRING &name)
{
  const Type_handler *h;
  if (!(h= Type_handler::handler_by_name_or_error(thd, name)))
    return true;
  type->set(h);
  charset= NULL;
  return false;
}


bool sp_expr_lex::sp_repeat_loop_finalize(THD *thd)
{
  uint ip= sphead->instructions();
  sp_label *lab= spcont->last_label();  /* Jumping back */
  sp_instr_jump_if_not *i= new (thd->mem_root)
    sp_instr_jump_if_not(ip, spcont, get_item(), lab->ip, this);
  if (unlikely(i == NULL) ||
      unlikely(sphead->add_instr(i)))
    return true;
  /* We can shortcut the cont_backpatch here */
  i->m_cont_dest= ip+1;
  return false;
}


bool sp_expr_lex::sp_if_expr(THD *thd)
{
  uint ip= sphead->instructions();
  sp_instr_jump_if_not *i= new (thd->mem_root)
                           sp_instr_jump_if_not(ip, spcont, get_item(), this);
  return
    (unlikely(i == NULL) ||
    unlikely(sphead->push_backpatch(thd, i,
                                    spcont->push_label(thd, &empty_clex_str,
                                                       0))) ||
    unlikely(sphead->add_cont_backpatch(i)) ||
    unlikely(sphead->add_instr(i)));
}


bool LEX::sp_if_after_statements(THD *thd)
{
  uint ip= sphead->instructions();
  sp_instr_jump *i= new (thd->mem_root) sp_instr_jump(ip, spcont);
  if (unlikely(i == NULL) ||
      unlikely(sphead->add_instr(i)))
    return true;
  sphead->backpatch(spcont->pop_label());
  sphead->push_backpatch(thd, i, spcont->push_label(thd, &empty_clex_str, 0));
  return false;
}


sp_condition_value *LEX::stmt_signal_value(const Lex_ident_sys_st &ident)
{
  sp_condition_value *cond;
  /* SIGNAL foo cannot be used outside of stored programs */
  if (unlikely(spcont == NULL))
  {
    my_error(ER_SP_COND_MISMATCH, MYF(0), ident.str);
    return NULL;
  }
  cond= spcont->find_declared_or_predefined_condition(thd, &ident);
  if (unlikely(cond == NULL))
  {
    my_error(ER_SP_COND_MISMATCH, MYF(0), ident.str);
    return NULL;
  }
  bool bad= thd->variables.sql_mode & MODE_ORACLE ?
            !cond->has_sql_state() :
            cond->type != sp_condition_value::SQLSTATE;
  if (unlikely(bad))
  {
    my_error(ER_SIGNAL_BAD_CONDITION_TYPE, MYF(0));
    return NULL;
  }
  return cond;
}


bool LEX::add_table_foreign_key(const LEX_CSTRING *name,
                                const LEX_CSTRING *constraint_name,
                                Table_ident *ref_table_name,
                                DDL_options ddl_options)
{
  Key *key= new (thd->mem_root) Foreign_key(name,
                                            &last_key->columns,
                                            constraint_name,
                                            &ref_table_name->db,
                                            &ref_table_name->table,
                                            &ref_list,
                                            fk_delete_opt,
                                            fk_update_opt,
                                            fk_match_option,
                                            ddl_options);
  if (unlikely(key == NULL))
    return true;

  /*
    handle_if_exists_options() expects the two keys in this order:
    the Foreign_key, followed by its auto-generated Key.
  */
  alter_info.key_list.push_back(key, thd->mem_root);
  alter_info.key_list.push_back(last_key, thd->mem_root);

  option_list= NULL;

  /* Only used for ALTER TABLE. Ignored otherwise. */
  alter_info.flags|= ALTER_ADD_FOREIGN_KEY;

  return false;
}


bool LEX::add_column_foreign_key(const LEX_CSTRING *name,
                                 const LEX_CSTRING *constraint_name,
                                 Table_ident *ref_table_name,
                                 DDL_options ddl_options)
{
  if (last_field->vcol_info || last_field->vers_sys_field())
  {
    thd->parse_error();
    return true;
  }
  if (unlikely(!(last_key= (new (thd->mem_root)
                            Key(Key::MULTIPLE, constraint_name,
                            HA_KEY_ALG_UNDEF, true, ddl_options)))))
    return true;
  Key_part_spec *key= new (thd->mem_root) Key_part_spec(name, 0);
  if (unlikely(key == NULL))
    return true;
  last_key->columns.push_back(key, thd->mem_root);
  if (ref_list.is_empty())
  {
    ref_list.push_back(key, thd->mem_root);
  }
  if (unlikely(add_table_foreign_key(constraint_name, constraint_name,
                                     ref_table_name, ddl_options)))
      return true;
  option_list= NULL;

  /* Only used for ALTER TABLE. Ignored otherwise. */
  alter_info.flags|= ALTER_ADD_FOREIGN_KEY;

  return false;
}


bool LEX::stmt_grant_table(THD *thd,
                           Grant_privilege *grant,
                           const Lex_grant_object_name &ident,
                           privilege_t grant_option)
{
  sql_command= SQLCOM_GRANT;
  return
    grant->set_object_name(thd, ident, current_select, grant_option) ||
    !(m_sql_cmd= new (thd->mem_root) Sql_cmd_grant_table(sql_command, *grant));
}


bool LEX::stmt_revoke_table(THD *thd,
                            Grant_privilege *grant,
                            const Lex_grant_object_name &ident)
{
  sql_command= SQLCOM_REVOKE;
  return
    grant->set_object_name(thd, ident, current_select, NO_ACL) ||
    !(m_sql_cmd= new (thd->mem_root) Sql_cmd_grant_table(sql_command, *grant));
}


bool LEX::stmt_grant_sp(THD *thd,
                        Grant_privilege *grant,
                        const Lex_grant_object_name &ident,
                        const Sp_handler &sph,
                        privilege_t grant_option)
{
  sql_command= SQLCOM_GRANT;
  return
    grant->set_object_name(thd, ident, current_select, grant_option) ||
    add_grant_command(thd, grant->columns()) ||
    !(m_sql_cmd= new (thd->mem_root) Sql_cmd_grant_sp(sql_command,
                                                      *grant, sph));
}


bool LEX::stmt_revoke_sp(THD *thd,
                         Grant_privilege *grant,
                         const Lex_grant_object_name &ident,
                         const Sp_handler &sph)
{
  sql_command= SQLCOM_REVOKE;
  return
    grant->set_object_name(thd, ident, current_select, NO_ACL) ||
    add_grant_command(thd, grant->columns()) ||
    !(m_sql_cmd= new (thd->mem_root) Sql_cmd_grant_sp(sql_command,
                                                      *grant, sph));
}


bool LEX::stmt_grant_proxy(THD *thd, LEX_USER *user, privilege_t grant_option)
{
  users_list.push_front(user);
  sql_command= SQLCOM_GRANT;
  return !(m_sql_cmd= new (thd->mem_root) Sql_cmd_grant_proxy(sql_command,
                                                              grant_option));
}


bool LEX::stmt_revoke_proxy(THD *thd, LEX_USER *user)
{
  users_list.push_front(user);
  sql_command= SQLCOM_REVOKE;
  return !(m_sql_cmd= new (thd->mem_root) Sql_cmd_grant_proxy(sql_command,
                                                              NO_ACL));
}
