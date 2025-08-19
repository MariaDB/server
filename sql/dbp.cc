/* Copyright (c) 2026, MariaDB

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

#ifndef DBUG_OFF

#include "dbp.h"

extern const char *dbug_print_unit(st_select_lex_unit *un); // in item.cc

static char dbug_item_small_buffer[DBUG_ITEM_BUFFER_SIZE];
void dbug_add_print_join( JOIN *join );
bool show_field_values= TRUE;
extern const char *dbug_print_items(Item *item);
static void dbug_add_print_item(Item *item);
char dbug_print_row_buffer[DBUG_ROW_BUFFER_SIZE];

// test if this is uninitialized/trash memory
template<typename t>
bool is_trash(t ptr)
{
  uint s= sizeof(t);
  uchar *m= (uchar *)&ptr;
  for (uint i= 0; i < s; i++)
    if (m[i] != DBUG_TRASH_CHAR)
      return false;

  return true;
}


// test if this is uninitialized/trash memory or null
template<typename t>
bool is_trash_or_null(t ptr)
{
  if (!ptr)
    return true;

  return is_trash(ptr);
}


void dbug_strlcat( char *dest, uint dest_size, const char *src)
{
  uint left= dest_size-strlen(dest)-1;
  if (left > 0)
  {
    strncat( dest, src, left );
    dest[left+1]= (char)0;
  }
}


void dbug_sprintfcat(char *dest, uint dest_size, const char *format, ...)
{
  uint len= strlen(dest);
  uint left= dest_size-len-1;
  if (left > 0)
  {
    va_list argptr;
    va_start(argptr, format);
    vsnprintf(dbug_item_small_buffer, DBUG_ITEM_BUFFER_SIZE-1, format, argptr);
    va_end(argptr);
    dbug_strlcat(dest, dest_size, dbug_item_small_buffer);
  }
}


#define SMALL_BUF 1023
static void dbug_add_print_item_contents(Item *item)
{
  if (item)
  {
    bool eliminated= false;

    if (item->type() == Item::SUBSELECT_ITEM &&
        ((Item_subselect*)item)->eliminated)
      eliminated= true;

    if (!eliminated)
    {
      if (item->type() == Item::REF_ITEM)
      {
        Item **ref= ((Item_ref *)item)->ref;
        if (ref)
        {
          if (((Item_ref*)item)->table_name &&
              ((Item_ref*)item)->table_name.str )
            DBUG_SPRINTF_CAT("(REF_ITEM*)(name:'%s.%s',", ((Item_ref*)item)->table_name.str, item->name);
          else
            DBUG_SPRINTF_CAT("(REF_ITEM*)(name:'%s',", item->name);
          if (ref == not_found_item)
            DBUG_CAT("not_found_item");
          else
          {
            Item *refd_item= *ref;
            dbug_add_print_item(refd_item);
          }
          DBUG_CAT(")");
        }
        else
          DBUG_SPRINTF_CAT("(REF_ITEM*)nullptr");
      }
      else
      {
        char *buf= (char *)alloca( SMALL_BUF+1 );
        String str(buf, SMALL_BUF, &my_charset_bin);
        str.length(0);
        item->print(&str, QT_VIEW_INTERNAL);
        DBUG_CAT(str.c_ptr_safe());
      }
    }
    else
      DBUG_CAT("<Item_subselect eliminated>");
  }
  else
    DBUG_CAT("<nullptr>");
}


static void dbug_add_print_items(List<Item> &list)
{
  List_iterator<Item> li(list);
  Item *sub_item;
  uint cnt= 0;
  DBUG_CAT( "Children:" );
  while ((sub_item= li++))
    cnt++;
  DBUG_SPRINTF_CAT( "[%d]", cnt );
  li.rewind();
  while ((sub_item= li++))
  {
    if (strlen(dbug_big_buffer) < DBUG_BIG_BUFFER_SIZE-DBUG_ITEM_BUFFER_SIZE-7)
    {
      DBUG_SPRINTF_CAT( "[%p: ", (void*)sub_item );
      dbug_add_print_item(sub_item);
      DBUG_CAT( "]" );
    }
    else
    {
      DBUG_CAT("...");
      break;
    }
  }
  return;
}


static void dbug_add_print_items_o_r(List<Item_outer_ref> &list)
{
  List_iterator<Item_outer_ref> li(list);
  Item *sub_item;
  uint cnt= 0;
  DBUG_CAT( "Children:" );
  while ((sub_item= li++))
    cnt++;
  DBUG_SPRINTF_CAT( "[%d]", cnt );
  li.rewind();
  while ((sub_item= li++))
  {
    if (strlen(dbug_big_buffer) < DBUG_BIG_BUFFER_SIZE-DBUG_ITEM_BUFFER_SIZE-7)
    {
      DBUG_SPRINTF_CAT( "[%p: ", (void*)sub_item );
      dbug_add_print_item(sub_item);
      DBUG_CAT( "]" );
    }
    else
    {
      DBUG_CAT("...");
      break;
    }
  }
  return;
}


const char *dbug_print_items(List<Item> &list)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<Item> %p:{", (void*) &list );
  dbug_add_print_items( list );
  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *dbug_print_items_o_r(List<Item_outer_ref> &list)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<Item> %p:{", (void*) &list );
  dbug_add_print_items_o_r( list );
  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


static void dbug_add_print_item(Item *item)
{
  if (item)
  {
    DBUG_SPRINTF_CAT( "[%p:", item);

#ifdef _DBUG_HAVE_ITEM_THD
    if (item->dbug_mem_root == item->dbug_thd->stmt_arena->mem_root)
      DBUG_CAT( "S:" );
    else
      DBUG_CAT( "E:" );
#endif
    switch (item->type())
    {
    case Item::REF_ITEM:
      dbug_add_print_item_contents(item);
      break;
    case Item::FUNC_ITEM:
      {
        Item_func *func_item= (Item_func *)item;
        switch(func_item->functype())
        {
        case Item_func::MULT_EQUAL_FUNC:
          {
          DBUG_CAT("(MULT_EQUAL_FUNC*)");
          Item_equal_fields_iterator it(*(Item_equal *)func_item);
          Item *itm;
          while ((itm= it++))
            dbug_add_print_item(itm);
          break;
          }

        case Item_func::EQ_FUNC: DBUG_CAT("(EQ_FUNC*)"); break;
        case Item_func::LIKE_FUNC: DBUG_CAT("(LIKE_FUNC*)"); break;
        case Item_func::EQUAL_FUNC: DBUG_CAT("(EQUAL_FUNC*)"); break;
        case Item_func::ISNULL_FUNC: DBUG_CAT("(ISNULL_FUNC*)"); break;
        case Item_func::UNKNOWN_FUNC:
          if ( func_item->func_name() )
            DBUG_SPRINTF_CAT("(%s)", func_item->func_name());
          else
            DBUG_CAT("(UNKNOWN_FUNC*)");
          break;
        case Item_func::NE_FUNC: DBUG_CAT("(NE_FUNC*)"); break;
        case Item_func::LT_FUNC: DBUG_CAT("(LT_FUNC*)"); break;
        case Item_func::LE_FUNC: DBUG_CAT("(LE_FUNC*)"); break;
        case Item_func::GE_FUNC: DBUG_CAT("(GE_FUNC*)"); break;
        case Item_func::GT_FUNC: DBUG_CAT("(GT_FUNC*)"); break;
        case Item_func::FT_FUNC: DBUG_CAT("(FT_FUNC*)"); break;
        case Item_func::ISNOTNULL_FUNC: DBUG_CAT("(ISNOTNULL_FUNC*)"); break;
        case Item_func::COND_AND_FUNC: DBUG_CAT("(COND_AND_FUNC*)"); break;
        case Item_func::COND_OR_FUNC: DBUG_CAT("(COND_OR_FUNC*)"); break;
        case Item_func::XOR_FUNC: DBUG_CAT("(XOR_FUNC*)"); break;
        case Item_func::BETWEEN: DBUG_CAT("(BETWEEN*)"); break;
        case Item_func::IN_FUNC: DBUG_CAT("(IN_FUNC*)"); break;
        case Item_func::INTERVAL_FUNC: DBUG_CAT("(INTERVAL_FUNC*)"); break;
        case Item_func::ISNOTNULLTEST_FUNC: DBUG_CAT("(ISNOTNULLTEST_FUNC*)"); break;
        case Item_func::SP_EQUALS_FUNC: DBUG_CAT("(SP_EQUALS_FUNC*)"); break;
        case Item_func::SP_RELATE_FUNC: DBUG_CAT("(SP_RELATE_FUNC*)"); break;
        case Item_func::NOT_FUNC: DBUG_CAT("(NOT_FUNC*)"); break;
        case Item_func::NOT_ALL_FUNC: DBUG_CAT("(NOT_ALL_FUNC*)"); break;
        case Item_func::TEMPTABLE_ROWID: DBUG_CAT("(TEMPTABLE_ROWID*)"); break;
        case Item_func::NOW_FUNC: DBUG_CAT("(NOW_FUNC*)"); break;
        case Item_func::NOW_UTC_FUNC: DBUG_CAT("(NOW_UTC_FUNC*)"); break;
        case Item_func::SYSDATE_FUNC: DBUG_CAT("(SYSDATE_FUNC*)"); break;
        case Item_func::TRIG_COND_FUNC: DBUG_CAT("(TRIG_COND_FUNC*)"); break;
        case Item_func::SUSERVAR_FUNC: DBUG_CAT("(SUSERVAR_FUNC*)"); break;
        case Item_func::GUSERVAR_FUNC: DBUG_CAT("(GUSERVAR_FUNC*)"); break;
        case Item_func::COLLATE_FUNC: DBUG_CAT("(COLLATE_FUNC*)"); break;
        case Item_func::EXTRACT_FUNC: DBUG_CAT("(EXTRACT_FUNC*)"); break;
        case Item_func::CHAR_TYPECAST_FUNC: DBUG_CAT("(CHAR_TYPECAST_FUNC*)"); break;
        case Item_func::FUNC_SP: DBUG_CAT("(FUNC_SP*)"); break;
        case Item_func::UDF_FUNC: DBUG_CAT("(UDF_FUNC*)"); break;
        case Item_func::NEG_FUNC: DBUG_CAT("(NEG_FUNC*)"); break;
        case Item_func::GSYSVAR_FUNC: DBUG_CAT("(GSYSVAR_FUNC*)"); break;
        case Item_func::IN_OPTIMIZER_FUNC: DBUG_CAT("(IN_OPTIMIZER_FUNC*)"); break;
        case Item_func::DYNCOL_FUNC: DBUG_CAT("(DYNCOL_FUNC*)"); break;
        case Item_func::JSON_EXTRACT_FUNC: DBUG_CAT("(JSON_EXTRACT_FUNC*)"); break;
        case Item_func::CASE_SEARCHED_FUNC: DBUG_CAT("(CASE_SEARCHED_FUNC*)"); break;
        case Item_func::CASE_SIMPLE_FUNC: DBUG_CAT("(CASE_SIMPLE_FUNC*)"); break;
        default:
          DBUG_CAT("(OTHER*)");
        }

        for (uint i= 0; i < func_item->argument_count(); i++)
        {
          DBUG_SPRINTF_CAT("arg%d[",i);
          dbug_add_print_item_contents(func_item->arguments()[i]);
          DBUG_CAT("]");
        }
      }
      break;
    case Item::COND_ITEM:
      {
        DBUG_CAT("(COND_ITEM*)");
        List_iterator<Item> li= *((Item_cond*) item)->argument_list();
        dbug_add_print_item_contents(item);
        Item *sub_item;
        while ((sub_item= li++))
        {
          if (strlen(dbug_big_buffer) <
                DBUG_BIG_BUFFER_SIZE-DBUG_ITEM_BUFFER_SIZE-7)
            dbug_add_print_item(sub_item);
          else
          {
            strcat( dbug_big_buffer, "..." );
            break;
          }
        }
      }
      break;
    case Item::FIELD_ITEM:
      DBUG_CAT("(FIELD_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
#if MYSQL_VERSION_ID > 100400
    case Item::CONST_ITEM:
      DBUG_CAT("(CONST_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
#endif
    case Item::SUM_FUNC_ITEM:
      DBUG_CAT("(SUM_FUNC_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::WINDOW_FUNC_ITEM:
      DBUG_CAT("(WINDOW_FUNC_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::NULL_ITEM:
      DBUG_CAT("(NULL_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::COPY_STR_ITEM:
      DBUG_CAT("(COPY_STR_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::FIELD_AVG_ITEM:
      DBUG_CAT("(FIELD_AVG_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::DEFAULT_VALUE_ITEM:
      DBUG_CAT("(DEFAULT_VALUE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
#if MYSQL_VERSION_ID > 100414
    case Item::CONTEXTUALLY_TYPED_VALUE_ITEM:
      DBUG_CAT("(CONTEXTUALLY_TYPED_VALUE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
#endif
    case Item::PROC_ITEM:
      DBUG_CAT("(PROC_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::FIELD_STD_ITEM:
      DBUG_CAT("(FIELD_STD_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::FIELD_VARIANCE_ITEM:
      DBUG_CAT("(FIELD_VARIANCE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::INSERT_VALUE_ITEM:
      DBUG_CAT("(INSERT_VALUE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::ROW_ITEM:
      DBUG_CAT("(ROW_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::CACHE_ITEM:
      DBUG_CAT("(CACHE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::TYPE_HOLDER:
      DBUG_CAT("(TYPE_HOLDER*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::PARAM_ITEM:
      DBUG_CAT("(PARAM_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::TRIGGER_FIELD_ITEM:
      DBUG_CAT("(TRIGGER_FIELD_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::EXPR_CACHE_ITEM:
      DBUG_CAT("(EXPR_CACHE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::SUBSELECT_ITEM:
      DBUG_CAT("(SUBSELECT_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    default:
      DBUG_CAT("(UNKNOWN_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    }
    DBUG_CAT("]");
  }
  else
    DBUG_CAT("<nullptr>");
}


extern const char *dbug_print_items(Item *item)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "%p:{", (void*) &item );
  dbug_add_print_item( item );
  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *dbug_print_ref_array(Ref_ptr_array *ptr_array)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "(Ref_ptr_array*)%p:{", (void*) ptr_array );
  Item *item;
  for (size_t i= 0; i < ptr_array->size(); i++)
  {
    item= (*ptr_array)[i];
    DBUG_SPRINTF_CAT( "[%u:", i);
    dbug_add_print_item( item );
    DBUG_CAT( "]" );
  }
  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


#if MYSQL_VERSION_ID > 100400
const char *dbug_print_trace( Json_writer *x )
{
  if (x)
  {
    String *s= const_cast<String *>(x->output.get_string());
    return s->c_ptr();
  }
  else
    return "NULL";
}


const char *dbug_print_trace()
{
  if (current_thd)
  {
    if (current_thd->opt_trace.is_started())
      return dbug_print_trace( current_thd->opt_trace.get_current_json() );
    else
      return "Trace empty";
  }
  else
    return "No Thread";
}
#endif


static void dbug_add_print_selects(st_select_lex *sl)
{
  if (!sl)
    DBUG_CAT( "(st_select_lex *)NULL" );
  else
  {
    char *buf= dbug_item_small_buffer;
    String str(buf, sizeof(dbug_item_small_buffer), &my_charset_bin);
    str.length(0);

    THD *thd= current_thd;
    ulonglong save_option_bits= thd->variables.option_bits;
    thd->variables.option_bits &= ~OPTION_QUOTE_SHOW_CREATE;

    sl->print(thd, &str, enum_query_type(QT_SHOW_SELECT_NUMBER | QT_EXPLAIN));

    thd->variables.option_bits= save_option_bits;

    DBUG_CAT( str.c_ptr());

  //  if (sl->having)

  }
}

void dbug_add_print_jointab( JOIN_TAB *join_tab );

void dbug_add_print_lex( LEX *lex)
{
#if MYSQL_VERSION_ID > 100400
  st_select_lex *sl, *select_lex= lex->first_select_lex();
#else
  st_select_lex *sl, *select_lex= &lex->select_lex;
#endif

  DBUG_SPRINTF_CAT( " select_lex:%p:{", select_lex );
  dbug_add_print_selects( select_lex );
  DBUG_CAT( "}" );

  DBUG_CAT( " all_selects_list:" );
  for( sl= lex->all_selects_list; sl;
      sl= sl->next_select_in_list())
  {
    DBUG_SPRINTF_CAT( "%p:[", sl );
    dbug_add_print_selects( sl );
    DBUG_CAT( "]" );
  }

  if (select_lex->master_unit() && ( select_lex->master_unit()->is_unit_op() ||
      select_lex->master_unit()->fake_select_lex))
  {
    // this is union or similar of more than one select
    DBUG_CAT( "UNION");     // TODO...
  }
  else
  {
    DBUG_SPRINTF_CAT( " table_list:" );
    for (TABLE_LIST *tbl= select_lex->table_list.first; tbl;
        tbl= tbl->next_global)
    {
      DBUG_CAT( "[" );
      if (tbl->select_stmt.str)
      {
        DBUG_CAT( " select_stmt:" );
        DBUG_CAT( tbl->select_stmt.str);
      }
      DBUG_CAT( "]" );
    }

    JOIN *join= select_lex->join;
    DBUG_SPRINTF_CAT( " join:%p:{", join );
    if (join)
    {
      dbug_add_print_join( join );
      if (join->join_tab)
      {
        DBUG_SPRINTF_CAT( " join->join_tab:%p:{", join->join_tab );
        dbug_add_print_jointab( join->join_tab );
        DBUG_CAT( "}" );
      }
    }
    DBUG_CAT( "}" );
  }
}


const char *dbug_print_lex( LEX *lex)
{
  *dbug_big_buffer= (char)0;
  DBUG_CAT( "(LEX *)" );
  dbug_add_print_lex( lex );
  return dbug_big_buffer;
}


static void dbug_add_print_table( TABLE *table )
{
  if (table)
  {
    DBUG_SPRINTF_CAT("%s,Fields:", table->alias.c_ptr());
    for(Field **f= table->field; *f; f++)
      dbug_add_print_field(*f);
  }
  else
    DBUG_CAT("table:NULL");
}


const char *dbug_print_table( TABLE *table)
{
  *dbug_big_buffer= (char)0;
  DBUG_CAT( "(TABLE *)" );
  dbug_add_print_table( table );
  return dbug_big_buffer;
}


static void dbug_add_print_table_list( TABLE_LIST *table_list)
{
  if(table_list->nested_join)
    DBUG_SPRINTF_CAT( "nested_join:%p", table_list->nested_join );

  show_field_values= FALSE;
  for(TABLE_LIST *cur_table= table_list;
      cur_table != NULL;
      cur_table= cur_table->next_global)
  {
    DBUG_SPRINTF_CAT("['%s'", cur_table->alias.str);
    dbug_add_print_table(cur_table->table);
    DBUG_CAT("]");
  }
  show_field_values= TRUE;
}


const char *dbug_print_table_list(  TABLE_LIST *table_list)
{
  *dbug_big_buffer= (char)0;
  DBUG_CAT( "(TABLE_LIST *)" );
  dbug_add_print_table_list( table_list );
  return dbug_big_buffer;
}


void dbug_add_print_jointab( JOIN_TAB *join_tab)
{
  if (!join_tab)
  {
    DBUG_CAT( "(JOIN_TAB *)NULL" );
  }
  else
  {
    DBUG_CAT( "(JOIN_TAB *)" );
    if (join_tab->table)
    {
      DBUG_CAT( " table:" );
      if (join_tab->table->alias.c_ptr())
        DBUG_CAT( join_tab->table->alias.c_ptr() );
      else
        DBUG_CAT( "NULL" );
    }

    if (join_tab->tab_list)
    {
      DBUG_CAT( " tab_list(global):" );
      TABLE_LIST *table_list = join_tab->tab_list;
      for ( ; table_list; table_list= table_list->next_global)
      {
        char name[10];
        strncpy(name, table_list->table_name.str, table_list->table_name.length);
        name[table_list->table_name.length]= 0;
        if (*name)
          DBUG_SPRINTF_CAT( "[Name:%s type:", name );
        else
          DBUG_SPRINTF_CAT( "[Name:NULL type:" );
        if (!is_trash_or_null(table_list->view))
        {
          if (table_list->algorithm == VIEW_ALGORITHM_MERGE)
            DBUG_CAT( "merge" );
          else
            if (table_list->algorithm == VIEW_ALGORITHM_TMPTABLE)
              DBUG_CAT( "tmptable" );
            else
              DBUG_CAT( "view?" );
        }
        else
        {
          if (!is_trash_or_null(table_list->derived == NULL))
            DBUG_CAT( "plain" );
        }
        if (!is_trash_or_null(table_list->nested_join))
          DBUG_CAT( ",Nested" );
        if (!is_trash_or_null(table_list->jtbm_subselect))
          DBUG_CAT( ",JoinTableMaterialization" );
        else
        {
          if (!is_trash_or_null(table_list->sj_subq_pred))
            DBUG_CAT( " SubselectPredicate" );
        }
        DBUG_CAT( ", " );
        if ( !is_trash_or_null(table_list->prep_on_expr))
        {
          DBUG_CAT( " on(orig):{" );
          dbug_add_print_item( table_list->prep_on_expr );
          DBUG_CAT( "}" );
        }
        if (!is_trash_or_null(table_list->jtbm_subselect))
        {
          DBUG_CAT( " JtabM:{" );
          dbug_add_print_item( table_list->jtbm_subselect );
          DBUG_CAT( "}" );
        }
        else
        {
          if (!is_trash_or_null(table_list->sj_subq_pred))
          {
            DBUG_CAT( " SubQ:{" );
            dbug_add_print_item( table_list->sj_subq_pred );
            DBUG_CAT( "}" );
          }
        }
        if (!is_trash_or_null(table_list->derived))
        {
          DBUG_CAT( " Derived:{" );
          DBUG_CAT( dbug_print_unit( table_list->derived ) );
          DBUG_CAT( "}" );
        }
        DBUG_CAT( "]" );

        if (table_list == table_list->next_global)      // no loop forever
          break;
      }
      DBUG_CAT( "}" );
    }
  }
}


const char *dbug_print_jointab( JOIN_TAB *join_tab)
{
  *dbug_big_buffer= (char)0;
  dbug_add_print_jointab( join_tab );
  return dbug_big_buffer;
}


void dbug_add_print_join(JOIN *join)
{
  DBUG_CAT( " Conds:{" );
  dbug_add_print_item( join->conds );
  DBUG_CAT( "}" );
  DBUG_CAT( " Order:{" );
  for (ORDER *order= join->order; order;  order= order->next)
    dbug_add_print_item_contents( *order->item );
  DBUG_CAT( "}" );
  DBUG_CAT( " Group:{" );
  for (ORDER *group= join->group_list; group;  group= group->next)
    dbug_add_print_item_contents( *group->item );
  DBUG_CAT( "}" );
  if (join->fields)
  {
    DBUG_CAT( " Fields:{" );
    dbug_add_print_items( *join->fields );
    DBUG_CAT( "}" );
  }
  DBUG_CAT( " Tables:{" );
  dbug_add_print_jointab( join->join_tab );
  DBUG_CAT( "}" );
  if (join->pushdown_query)
  {
    DBUG_CAT( " Pushdown:{" );
    dbug_add_print_selects( join->pushdown_query->select_lex );
    DBUG_CAT( "}" );
  }
}


const char *dbug_print_join(JOIN *join)
{
  *dbug_big_buffer= (char)0;
  DBUG_CAT( "(JOIN *)" );
  dbug_add_print_join( join );
  return dbug_big_buffer;
}


const char *dbug_print_select_lex(st_select_lex *sl)
{
  // show master unit
  *dbug_big_buffer= (char)0;
  *dbug_item_small_buffer= (char)0;
  DBUG_CAT( "(st_select_lex *)" );
  dbug_print_unit(sl->master_unit());   // into dbug_item_small_buffer
  if (*dbug_item_small_buffer)
  {
    DBUG_CAT( " Master Unit:{" );
    DBUG_CAT( dbug_item_small_buffer );
    DBUG_SPRINTF_CAT( "}\n" );
  }
  DBUG_SPRINTF_CAT( "%p:{", (void*) sl );
  dbug_add_print_selects( sl );
  strcat( dbug_big_buffer, "}" );
  return dbug_big_buffer;
}


const char *dbug_print_lex_node( st_select_lex_node *node )
{
  st_select_lex *lex= (st_select_lex *)(node);

  return dbug_print_select_lex( lex );
}


const char *dbug_add_print_field(Field *field)
{
  if (show_field_values)
  {
    DBUG_SPRINTF_CAT("{%s.%s=", field->table->alias.c_ptr(),
                    field->field_name.str);
    if (field->is_real_null())
      DBUG_CAT("NULL");
    else
    {
      String tmp;
      if(field->type() == MYSQL_TYPE_BIT)
        (void) field->val_int_as_str(&tmp, 1);
      else
        field->val_str(&tmp);
      DBUG_CAT(tmp.c_ptr());
    }
    DBUG_CAT("}");
  }
  else
  {
    DBUG_SPRINTF_CAT("{%s.%s}", field->table->alias.c_ptr(),
                               field->field_name.str);
  }
  return dbug_big_buffer;
}


const char *dbug_print_fields(Field **field)
{
  *dbug_big_buffer= (char)0;
  DBUG_CAT("(Field **)");
  for( int i=0; field[i]; i++)
    dbug_add_print_field(field[i]);
  return dbug_big_buffer;
}


const char *dbug_print_field(Field *field)
{
  *dbug_big_buffer= (char)0;
  DBUG_CAT("(Field *)");
  dbug_add_print_field(field);
  return dbug_big_buffer;
}


#if MYSQL_VERSION_ID > 100338 && MYSQL_VERSION_ID < 100400
#include "sql_statistics.h"

const char *dbug_print_histogram( Histogram_binary *hist )
{
  *dbug_big_buffer= (char)0;
  uchar *values= hist->get_values();

  DBUG_CAT("(Histogram *)");
  if( hist->get_type() == SINGLE_PREC_HB )
  {
    DBUG_CAT("(SINGLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint val= (uint) (((uint8 *) values)[i]);
      DBUG_SPRINTF_CAT( "values[%u] = %u\n", i, val );
    }
    DBUG_CAT("\n");
  }
  if( hist->get_type() == DOUBLE_PREC_HB )
  {
    DBUG_CAT("(DOUBLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint val= (uint) uint2korr(values + i * 2);
      DBUG_SPRINTF_CAT( "values[%u] = %u\n", i, val );
    }
    DBUG_CAT("\n");
  }

  return dbug_big_buffer;
}

const char *dbug_print_col_stats( Column_statistics *stats )
{
  *dbug_big_buffer= (char)0;
  Histogram_base *hist= stats->histogram;
  uchar *values= hist->get_values();
  double min= stats->min_value->val_real();
  double max= stats->max_value->val_real();

  DBUG_CAT("(Column_statistics *)\nHistogram Data\n");
  if( hist->get_type() == SINGLE_PREC_HB )
  {
    DBUG_CAT("(SINGLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint rval= (uint) (((uint8 *) values)[i]);
      double val= min + (max-min)*(double)rval/((uint) (1 << 8) - 1);
      DBUG_SPRINTF_CAT( "values[%u] = %f\n", i, val );
    }
    DBUG_CAT("\n");
  }
  if( hist->get_type() == DOUBLE_PREC_HB )
  {
    DBUG_CAT("(DOUBLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint rval= (uint) uint2korr(values + i * 2);
      double val= min + (max-min)*(double)rval/((uint) (1 << 16) - 1);
      DBUG_SPRINTF_CAT( "values[%u] = %f\n", i, val );
    }
    DBUG_CAT("\n");
  }

  return dbug_big_buffer;
}
#endif


extern const char* dbug_print_table_row(TABLE *table);
extern const char *dbug_print_unit(st_select_lex_unit *un);
extern const char *dbug_print_sel_arg(SEL_ARG *sel_arg);



#if MYSQL_VERSION_ID > 100400
const char *DBUG_PRINT_TRACE()                   { return dbug_print_trace(); }
const char *DBUG_PRINT_TRACE( Json_writer *x )   { return dbug_print_trace(x); }
#endif

const char *DBUG_PRINT_FUNCTION(table_map m)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "(table_map) 0x%p", (void *)m );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<Lex_ident_sys> &x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<Lex_ident_sys> %p:{", (void*) &x );

  List_iterator<Lex_ident_sys> iter(x);
  Lex_ident_sys *str;

  for (uint i= 0;(str= iter++);i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str->str );

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<LEX_STRING> &x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<LEX_STRING> %p:{", (void*) &x );

  List_iterator<LEX_STRING> iter(x);
  LEX_STRING *str;

  for (uint i= 0;(str= iter++);i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str->str );

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<LEX_CSTRING> &x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<LEX_STRING> %p:{", (void*) &x );

  List_iterator<LEX_CSTRING> iter(x);
  LEX_CSTRING *str;

  for (uint i= 0;(str= iter++);i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str->str );

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<const char *> x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<const char *> %p:{", (void*) &x );

  List_iterator<const char *> iter(x);
  const char *str;

  for (uint i= 0;(str= *(iter++));i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str );

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<String> &x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<String> %p:{", (void*) &x );

  List_iterator<String> iter(x);
  String *str;

  for (uint i= 0;(str= iter++);i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str->ptr() );

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<TABLE_LIST> &x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<TABLE_LIST> %p:{", (void*) &x );

  List_iterator<TABLE_LIST> iter(x);
  TABLE_LIST *str;

  for (uint i= 0;(str= iter++);i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str->alias.str );

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<JOIN_TAB_RANGE> &x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<JOIN_TAB_RANGE> %p:{", (void*) &x );

#if 0  ///TODO
  List_iterator<JOIN_TAB_RANGE> iter(x);
  JOIN_TAB_RANGE *str;

  for (uint i= 0;(str= iter++);i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str-> );
#endif

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<Key_part_spec> &x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<Key_part_spec> %p:{", (void*) &x );

#if 0  ///TODO
  List_iterator<Key_part_spec> iter(x);
  Key_part_spec *str;

  for (uint i= 0;(str= iter++);i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str-> );
#endif

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<Sql_condition> &x)
{
  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "List<Sql_condition> %p:{", (void*) &x );

#if 0  ///TODO
  List_iterator<Sql_condition> iter(x);
  Sql_condition *str;

  for (uint i= 0;(str= iter++);i++)
    DBUG_SPRINTF_CAT( "[%d:%s]", i, str-> );
#endif

  DBUG_CAT( "}" );
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(ORDER *x)
{
  *dbug_big_buffer= (char)0;
  Item *item;
  ORDER *o= x;

  // TODO, print out item_list at *x->item
  do {
    item= *o->item;
    dbug_add_print_item(item);
  } while ((o= o->next));

  return dbug_big_buffer;
}


const char* DBUG_PRINT_ROW(TABLE *table, bool print_rowid)
{
  Field **pfield;
  String tmp(dbug_print_row_buffer,
             sizeof(dbug_print_row_buffer),&my_charset_bin);

  *dbug_big_buffer= (char)0;
  DBUG_SPRINTF_CAT( "(TABLE *) %p:{", (void*) &table );

  if (table->alias.ptr())
  {
    DBUG_SPRINTF_CAT( "[name:%s]", table->alias.ptr());
  }

  bool first= true;

  for (pfield= table->field; *pfield ; pfield++)
  {
    if (table->read_set &&
        !bitmap_is_set(table->read_set, (*pfield)->field_index))
      continue;

    if (first)
      first= false;
    else
      DBUG_CAT(",");

    if ((*pfield)->field_name.str)
      DBUG_CAT((*pfield)->field_name.str);
    else
      DBUG_CAT("NULL");
  }

  DBUG_CAT(")=(");

  first= true;
  for (pfield= table->field; *pfield ; pfield++)
  {
    Field *field=  *pfield;

    if (table->read_set &&
        !bitmap_is_set(table->read_set, (*pfield)->field_index))
      continue;

    if (first)
      first= false;
    else
      DBUG_CAT(",");

    if (field->is_null())
      DBUG_CAT("NULL");
    else
    {
      if (field->type() == MYSQL_TYPE_BIT)
        (void) field->val_int_as_str(&tmp, 1);
      else
        field->val_str(&tmp);
      if (*tmp.ptr())
        DBUG_CAT(tmp.c_ptr_safe());
      else
        DBUG_CAT("NULL");
    }
  }
  DBUG_CAT(")");
  if (print_rowid)
  {
    DBUG_CAT(" rowid");
    for (uint i=0; i < table->file->ref_length; i++)
    {
      DBUG_SPRINTF_CAT(":%x", (uchar)table->file->ref[i]);
    }
  }

  return dbug_big_buffer;
}


const char* DBUG_PRINT_ROW(TABLE *table)
{
  return DBUG_PRINT_ROW(table, true);
}


const char *DBUG_PRINT_FUNCTION(TABLE_LIST &tl)
{
  return dbug_print_table_list( &tl );
}


const char *DBUG_PRINT_FUNCTION(TABLE_LIST *tl)
{
  return dbug_print_table_list( tl );
}


const char *dbug_add_print_key_part( KEY_PART_INFO *kp )
{
  DBUG_CAT( "(KEY_PART_INFO *):" );
  if (kp)
    dbug_add_print_field( kp->field );
  else
    DBUG_CAT( "NULL");
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION( KEY_PART_INFO *kp )
{
  *dbug_big_buffer= (char)0;
  return dbug_add_print_key_part(kp);
}


void dbug_add_print_key( KEY *k )
{
  if (k)
  {
    if(k->name.str)
      DBUG_SPRINTF_CAT( "Name:%s,", k->name.str);
//    if(k->table && k->table->alias.ptr())
//      DBUG_SPRINTF_CAT("table:%s,", k->table->alias.ptr());
    DBUG_SPRINTF_CAT("length:%d,", k->key_length);
    DBUG_SPRINTF_CAT("usable parts:%d,", k->usable_key_parts);
    DBUG_SPRINTF_CAT("parts:%d,", k->user_defined_key_parts);
    for(uint i= 0; i < k->ext_key_parts; i++)
      dbug_add_print_key_part(&k->key_part[i]);
  }
}


const char *DBUG_PRINT_FUNCTION( KEY *k )
{
  *dbug_big_buffer= (char)0;
  dbug_add_print_key( k );
  return dbug_big_buffer;
}



const char *DBUG_PRINT_FUNCTION( LEX_CSTRING *k )
{
  DBUG_CAT( "(LEX_CSTRING) " );
  uint npos= strlen(dbug_big_buffer);
  strncpy( dbug_big_buffer+npos, k->str, k->length );
  npos+= k->length;
  dbug_big_buffer[npos]= (char)0;
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION( SQL_I_List<TABLE_LIST> *k )
{
  TABLE_LIST *element= k->first;

  *dbug_big_buffer= (char)0;
  for (uint i= 0; i < k->elements; i++)
  {
    DBUG_SPRINTF_CAT("[i:%u,", i);
    dbug_add_print_table_list( element );
    element= element->next_global;
    DBUG_CAT("]");
  }
  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION( KEYUSE *k)
{
  TABLE *t= k->table;
  uint key= k->key;
  *dbug_big_buffer= (char)0;

  if (t)
    DBUG_SPRINTF_CAT( "TABLE:%s,", t->alias.c_ptr() );

  dbug_add_print_key( &t->key_info[key] );

  if (k->val)
  {
    DBUG_CAT("Value:");
    dbug_add_print_item( k->val );
  }

  return dbug_big_buffer;
}


// return which parts of operand are selected by this bitmap
const char *DBUG_PRINT_FUNCTION(MY_BITMAP *bitmap, void *operand)
{

  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION( Name_resolution_context *ctx )
{
  *dbug_big_buffer= (char)0;

  if (ctx)
  {
    DBUG_CAT( "select_lex:" );
    /*
    if (ctx->get_select_lex())
      dbug_add_print_selects( ctx->get_select_lex() );
    else
      DBUG_CAT( "NULL" );
      */

    DBUG_CAT( ",first table:" );
    if (ctx->first_name_resolution_table)
      dbug_add_print_table_list( ctx->first_name_resolution_table );
    else
      DBUG_CAT( "NULL" );

    DBUG_CAT( ",last table:" );
    if (ctx->last_name_resolution_table)
      dbug_add_print_table_list( ctx->last_name_resolution_table );
    else
      DBUG_CAT( "NULL" );

    /*
    DBUG_SPRINTF_CAT(",outer_context:(Name_resolution_context *)%x",
        ctx->get_outer_context());
        */
  }

  return dbug_big_buffer;
}


void dbug_add_print_ref_to_outside( Item_subselect::Ref_to_outside *ref )
{
  if (ref)
  {
    DBUG_CAT( "Item:" );
    if (ref->item)
      dbug_add_print_item( ref->item );
    else
      DBUG_CAT( "NULL" );

    DBUG_CAT( ",select_lex:" );

    if (ref->select)
      dbug_add_print_selects( ref->select );
    else
      DBUG_CAT( "NULL" );
  }
}


const char *DBUG_PRINT_FUNCTION( Item_subselect::Ref_to_outside *ref )
{
  *dbug_big_buffer= (char)0;

  dbug_add_print_ref_to_outside( ref );

  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION( List<Item_subselect::Ref_to_outside> *ref )
{
  *dbug_big_buffer= (char)0;

  if (ref)
  {
    List_iterator_fast<Item_subselect::Ref_to_outside> it(*ref);
    Item_subselect::Ref_to_outside *upper;

    while ((upper= it++))
    {
      DBUG_CAT( "[" );
      dbug_add_print_ref_to_outside( upper );
      DBUG_CAT( "]" );
    }
  }

  return dbug_big_buffer;
}


const char *DBUG_PRINT_FUNCTION(List<Item_ident> &x)
  { return dbug_print_items((List<Item>&)x); }
const char *DBUG_PRINT_FUNCTION(List<Item_ident> *x)
  { return dbug_print_items((List<Item>&)*x); }
const char *DBUG_PRINT_FUNCTION(List<Item> &x)
  { return dbug_print_items(x); }
const char *DBUG_PRINT_FUNCTION(List<Item> *x)
  { return dbug_print_items(*x); }
const char *DBUG_PRINT_FUNCTION(List<Item_outer_ref> *x)
  { return dbug_print_items_o_r(*x); }
const char *DBUG_PRINT_FUNCTION(List<Item_outer_ref> x)
  { return dbug_print_items_o_r(x); }
const char *DBUG_PRINT_FUNCTION(Item *x)
  { return dbug_print_items(x); }
const char *DBUG_PRINT_FUNCTION(Item **x)
  { return dbug_print_items(*x); }
const char *DBUG_PRINT_FUNCTION(st_select_lex *x)
  { return dbug_print_select_lex(x); }
const char *DBUG_PRINT_FUNCTION(st_select_lex x)
  { return dbug_print_select_lex(&x); }
const char *DBUG_PRINT_FUNCTION(JOIN *x)
  { return dbug_print_join(x); }
const char *DBUG_PRINT_FUNCTION(JOIN x)
  { return dbug_print_join(&x); }
const char *DBUG_PRINT_FUNCTION(JOIN_TAB *x)
  { return dbug_print_jointab(x); }
const char *DBUG_PRINT_FUNCTION(JOIN_TAB x)
  { return dbug_print_jointab(&x); }
const char *DBUG_PRINT_FUNCTION(LEX *x)
  { return dbug_print_lex(x); }
const char *DBUG_PRINT_FUNCTION(TABLE *x)
  { return dbug_print_table_row(x); }
const char *DBUG_PRINT_FUNCTION(st_select_lex_unit *x)
  { return dbug_print_unit(x); }
const char *DBUG_PRINT_FUNCTION(st_select_lex_unit x)
  { return dbug_print_unit(&x); }
const char *DBUG_PRINT_FUNCTION(st_select_lex_node *node)
  { return dbug_print_lex_node( node ); }
const char *DBUG_PRINT_FUNCTION(st_select_lex_node node)
  { return dbug_print_lex_node( &node ); }
const char *DBUG_PRINT_FUNCTION(READ_RECORD *x)
  { return dbug_print_table_row(x->table); }
const char *DBUG_PRINT_FUNCTION(Field *x)
  { return dbug_print_field(x); }
const char *DBUG_PRINT_FUNCTION(Field **x)
  { return dbug_print_fields(x); }
const char *DBUG_PRINT_FUNCTION(Ref_ptr_array *x)
  { return dbug_print_ref_array(x); }
const char *DBUG_PRINT_FUNCTION(Ref_ptr_array x)
  { return dbug_print_ref_array(&x); }
const char *DBUG_PRINT_FUNCTION(SEL_ARG *x)
  { return dbug_print_sel_arg(x); }
const char *DBUG_PRINT_FUNCTION(SEL_ARG x)
  { return dbug_print_sel_arg(&x); }
const char *DBUG_PRINT_FUNCTION(List<Lex_ident_sys> *x)
  { return DBUG_PRINT_FUNCTION(*x); }
const char *DBUG_PRINT_FUNCTION(List<LEX_STRING> *x)
  { return DBUG_PRINT_FUNCTION(*x); }
const char *DBUG_PRINT_FUNCTION(List<LEX_CSTRING> *x)
  { return DBUG_PRINT_FUNCTION(*x); }
const char *DBUG_PRINT_FUNCTION(List<TABLE_LIST> *x)
  { return DBUG_PRINT_FUNCTION(*x); }
const char *DBUG_PRINT_FUNCTION(List<JOIN_TAB_RANGE> *x)
  { return DBUG_PRINT_FUNCTION(*x); }
const char *DBUG_PRINT_FUNCTION(List<Sql_condition> *x)
  { return DBUG_PRINT_FUNCTION(*x); }
const char *DBUG_PRINT_FUNCTION(List<String> *x)
  { return DBUG_PRINT_FUNCTION(*x); }
const char *DBUG_PRINT_FUNCTION(List<Key_part_spec> *x)
  { return DBUG_PRINT_FUNCTION(*x); }
const char *DBUG_PRINT_FUNCTION( KEY_PART_INFO kp )
  { return DBUG_PRINT_FUNCTION(&kp); }
const char *DBUG_PRINT_FUNCTION( KEY k )
  { return DBUG_PRINT_FUNCTION(&k); }
const char *DBUG_PRINT_FUNCTION( List<Item_subselect::Ref_to_outside> &ref )
  {return DBUG_PRINT_FUNCTION(&ref);}
const char *DBUG_PRINT_FUNCTION( List<Item_field> *ref )
  {return DBUG_PRINT_FUNCTION((List<Item> *)ref);}
const char *DBUG_PRINT_FUNCTION( List<Item_field> ref )
  {return DBUG_PRINT_FUNCTION((List<Item> *)&ref);}
const char *DBUG_PRINT_FUNCTION( SQL_I_List<TABLE_LIST> &k )
  { return DBUG_PRINT_FUNCTION(&k); }
const char *DBUG_PRINT_FUNCTION( LEX_CSTRING k )
  { return DBUG_PRINT_FUNCTION(&k); }


#endif /*DBUG_OFF*/
