/*
   Copyright (c) 2015, Oracle and/or its affiliates. All rights reserved.

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

/*
  Optimizer hint parser grammar
*/

%{
#include "my_global.h"
#include "sql_class.h"
#include "sql_parse.h"
#include "parse_tree_hints.h"
#include "sql_lex_hints.h"

/* this is to get the bison compilation windows warnings out */
#ifdef _MSC_VER
/* warning C4065: switch statement contains 'default' but no 'case' labels */
/* warning C4102: 'yyexhaustedlab': unreferenced label */
#pragma warning (disable : 4065 4102)
#endif
#if defined (__GNUC__) || defined (__clang__)
#pragma GCC diagnostic ignored "-Wunused-label" /* yyexhaustedlab: */
#endif

#define NEW_PTN new (thd->mem_root)

template<typename T>
bool my_yyoverflow(T **yyss, YYSTYPE **yyvs, size_t *yystacksize);

#define yyoverflow(A,B,C,D,E,F)               \
  {                                           \
    size_t val= *(F);                         \
    if (unlikely(my_yyoverflow((B), (D), &val))) \
    {                                         \
      yyerror(thd, (A));                      \
      return 2;                               \
    }                                         \
    else                                      \
    {                                         \
      *(F)= (YYSIZE_T)val;                    \
    }                                         \
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
  In semantic actions, please use thd->parse_error() or my_error to
  push an error into the error stack and YYABORT
  to abort from the parser.
*/

static void yyerror(THD *thd, const char *s)
{
  /* "parse error" changed into "syntax error" between bison 1.75 and 1.875 */
  if (strcmp(s,"parse error") == 0 || strcmp(s,"syntax error") == 0)
    s= ER_THD(thd, ER_SYNTAX_ERROR);
  thd->parse_error(s, 0);
}
%}

%define api.pure
%yacc

%parse-param { class THD *thd }
%parse-param { class Hint_scanner *scanner }
%parse-param { class PT_hint_list **ret }

%lex-param { class Hint_scanner *scanner }

%expect 0


/* Hint keyword tokens */

%token BKA_HINT
%token BNL_HINT
%token INDEX_MERGE_HINT
%token NO_BKA_HINT
%token NO_BNL_HINT
%token NO_ICP_HINT
%token NO_INDEX_MERGE_HINT
%token NO_MRR_HINT
%token NO_RANGE_OPTIMIZATION_HINT
%token MRR_HINT

%token QB_NAME_HINT

/* Other tokens */

%token HINT_ARG_NUMBER
%token HINT_ARG_IDENT
%token HINT_ARG_QB_NAME

%token HINT_CLOSE
%token HINT_ERROR

/* Types */
%type <hint_type>
  key_level_hint_type_on
  key_level_hint_type_off
  table_level_hint_type_on
  table_level_hint_type_off

%type <hint>
  hint
  index_level_hint
  table_level_hint
  qb_name_hint

%type <hint_list> hint_list

%type <hint_string> hint_param_index
%type <hint_param_index_list> hint_param_index_list opt_hint_param_index_list
%type <hint_param_table>
  hint_param_table
  hint_param_table_ext
  hint_param_table_empty_qb

%type <hint_param_table_list>
  hint_param_table_list
  opt_hint_param_table_list
  hint_param_table_list_empty_qb
  opt_hint_param_table_list_empty_qb

%type <hint_string>
  HINT_ARG_IDENT
  HINT_ARG_NUMBER
  HINT_ARG_QB_NAME
  opt_qb_name

%%


start:
          hint_list HINT_CLOSE
          { *ret= $1; }
        | hint_list error HINT_CLOSE
          { *ret= $1; }
        | error HINT_CLOSE
          { *ret= NULL; }
        ;

hint_list:
          hint
          {
            $$= NEW_PTN PT_hint_list(thd->mem_root);
            if ($$ == NULL || $$->push_back($1))
              YYABORT; // OOM
          }
        | hint_list hint
          {
            $1->push_back($2);
            $$= $1;
          }
        ;

hint:
          index_level_hint
        | table_level_hint
        | qb_name_hint
        ;

opt_hint_param_table_list:
         /* empty */
          {
            $$= NEW_PTN Hint_param_table_list(thd->mem_root);
            if ($$ == NULL)
              YYABORT;
          }
        | hint_param_table_list
        ;

hint_param_table_list:
          hint_param_table
          {
            $$= NEW_PTN Hint_param_table_list(thd->mem_root);
            if ($$ == NULL)
              YYABORT;
            if ($$->push_back($1))
              YYABORT; // OOM
          }
        | hint_param_table_list ',' hint_param_table
          {
            if ($1->push_back($3))
              YYABORT; // OOM
            $$= $1;
          }
        ;

opt_hint_param_table_list_empty_qb:
         /* empty */
          {
            $$= NEW_PTN Hint_param_table_list(thd->mem_root);
            if ($$ == NULL)
              YYABORT;
          }
        | hint_param_table_list_empty_qb
        ;

hint_param_table_list_empty_qb:
         hint_param_table_empty_qb
         {
           $$= NEW_PTN Hint_param_table_list(thd->mem_root);
           if ($$ == NULL)
             YYABORT;
           if ($$->push_back($1))
             YYABORT;
         }
       | hint_param_table_list_empty_qb ',' hint_param_table_empty_qb
         {
           if ($1->push_back($3))
             YYABORT; // OOM
           $$= $1;
         }
       ;

opt_hint_param_index_list:
         /* empty */
          {
            $$= NEW_PTN Hint_param_index_list(thd->mem_root);
            if ($$ == NULL)
              YYABORT;
          }
       | hint_param_index_list
       ;

hint_param_index_list:
          hint_param_index
          {
            $$= NEW_PTN Hint_param_index_list(thd->mem_root);
            if ($$ == NULL)
              YYABORT;
            if ($$->push_back(&$1))
              YYABORT; // OOM
          }
        | hint_param_index_list ',' hint_param_index
          {
            if ($1->push_back(&$3))
              YYABORT; // OOM
            $$= $1;
          }
        ;

hint_param_index:
          HINT_ARG_IDENT
        ;

hint_param_table_empty_qb:
         HINT_ARG_IDENT
         {
           $$= (Hint_param_table *)
               thd->alloc(sizeof(Hint_param_table));
           if ($$ == NULL)
             YYABORT;
           new ($$) Hint_param_table;
           $$->table= $1;
           $$->opt_query_block= NULL_CSTR;
         }
       ;

hint_param_table:
          HINT_ARG_IDENT opt_qb_name
          {
            $$= (Hint_param_table *)
                thd->alloc(sizeof(Hint_param_table));
            if ($$ == NULL)
              YYABORT;
            new ($$) Hint_param_table;
            $$->table= $1;
            $$->opt_query_block= $2;
          }
        ;

hint_param_table_ext:
         hint_param_table
       | HINT_ARG_QB_NAME HINT_ARG_IDENT
         {
           $$= (Hint_param_table *)
               thd->alloc(sizeof(Hint_param_table));
           if ($$ == NULL)
             YYABORT;
           new ($$) Hint_param_table;
           $$->table= $2;
           $$->opt_query_block= $1;
         }
       ;

opt_qb_name:
          /* empty */ { $$= NULL_CSTR; }
        | HINT_ARG_QB_NAME
        ;

table_level_hint:
         table_level_hint_type_on '(' opt_hint_param_table_list ')'
         {
           $$= NEW_PTN PT_table_level_hint(NULL_CSTR, *$3, TRUE, $1);
           if ($$ == NULL)
             YYABORT; // OOM
         }
       | table_level_hint_type_on
         '(' HINT_ARG_QB_NAME opt_hint_param_table_list_empty_qb ')'
         {
           $$= NEW_PTN PT_table_level_hint($3, *$4, TRUE, $1);
           if ($$ == NULL)
             YYABORT; // OOM
         }
       | table_level_hint_type_off '(' opt_hint_param_table_list ')'
         {
           $$= NEW_PTN PT_table_level_hint(NULL_CSTR, *$3, FALSE, $1);
           if ($$ == NULL)
             YYABORT; // OOM
         }
       | table_level_hint_type_off
         '(' HINT_ARG_QB_NAME opt_hint_param_table_list_empty_qb ')'
         {
           $$= NEW_PTN PT_table_level_hint($3, *$4, FALSE, $1);
           if ($$ == NULL)
             YYABORT; // OOM
         }
       ;

index_level_hint:
         key_level_hint_type_on
         '(' hint_param_table_ext opt_hint_param_index_list ')'
         {
           $$= NEW_PTN PT_key_level_hint(*$3, *$4, TRUE, $1);
           if ($$ == NULL)
             YYABORT; // OOM
         }
       | key_level_hint_type_off
         '(' hint_param_table_ext opt_hint_param_index_list ')'
         {
           $$= NEW_PTN PT_key_level_hint(*$3, *$4, FALSE, $1);
           if ($$ == NULL)
             YYABORT; // OOM
         }
       ;

table_level_hint_type_on:
         BKA_HINT
         {
           $$= BKA_HINT_ENUM;
         }
       | BNL_HINT
         {
           $$= BNL_HINT_ENUM;
         }
       ;

table_level_hint_type_off:
         NO_BKA_HINT
         {
           $$= BKA_HINT_ENUM;
         }
       | NO_BNL_HINT
         {
           $$= BNL_HINT_ENUM;
         }
       ;

key_level_hint_type_on:
         MRR_HINT
         {
           $$= MRR_HINT_ENUM;
         }
       | NO_RANGE_OPTIMIZATION_HINT
         {
           $$= NO_RANGE_HINT_ENUM;
         }
       ;

key_level_hint_type_off:
         NO_ICP_HINT
         {
           $$= ICP_HINT_ENUM;
         }
       | NO_MRR_HINT
         {
           $$= MRR_HINT_ENUM;
         }
       ;

qb_name_hint:
         QB_NAME_HINT '(' HINT_ARG_IDENT ')'
         {
           $$= NEW_PTN PT_hint_qb_name($3);
           if ($$ == NULL)
             YYABORT; // OOM
         }
       ;
