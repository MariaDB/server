/*
   Copyright (c) 2023, MariaDB Plc.
*/


#ifdef USE_PRAGMA_IMPLEMENTATION
#pragma implementation				// gcc: Class implementation
#endif
#include "mariadb.h"
#include "sql_priv.h"
#include <mysql.h>
#include <m_ctype.h>
#include "my_dir.h"
#include "sp_rcontext.h"
#include "sp_head.h"
#include "sql_trigger.h"
#include "sql_select.h"
#include "sql_show.h"
#include "sql_view.h"
#include "sql_time.h"
#include "sql_acl.h"
#include "sql_base.h"
#include "sql_expression_cache.h"
#include "my_json_writer.h"

#ifndef DBUG_OFF

#include "dbp.h"

extern const char *dbug_print_unit(st_select_lex_unit *un); // in item.cc

/* Debugger help function */
#define DBUG_PRINT_SIZE 2048
static char dbug_item_small_buffer[DBUG_PRINT_SIZE];
void dbug_add_print_join( JOIN *join );

#define DBG_CAT(...) dbug_strlcat(dbug_big_buffer, DBUG_BIG_BUF_SIZE, \
                                   __VA_ARGS__)

#define DBG_SPRINTF_CAT(...) dbug_sprintfcat(dbug_big_buffer,\
                                DBUG_BIG_BUF_SIZE, __VA_ARGS__)

#define TRASH_CHAR      0xa5

// test if this is uninitialized/trash memory
template<typename t>
bool is_trash(t ptr)
{
  uint s= sizeof(t);
  uchar *m= (uchar *)&ptr;
  for (uint i= 0; i < s; i++)
    if (m[i] != TRASH_CHAR)
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


#define DBUG_BIG_BUF_SIZE 20480
static char dbug_big_buffer[DBUG_BIG_BUF_SIZE];
bool show_field_values= TRUE;
extern const char *dbug_print_items(Item *item);
static void dbug_add_print_item(Item *item);

static void dbug_strlcat( char *dest, uint dest_size, const char *src)
{
  uint left= dest_size-strlen(dest)-1;
  if (left > 0)
  {
    strncat( dest, src, left );
    dest[left+1]= (char)0;
  }
}


static void dbug_sprintfcat(char *dest, uint dest_size, const char *format, ...)
{
  uint len= strlen(dest);
  uint left= dest_size-len-1;
  if (left > 0)
  {
    va_list argptr;
    va_start(argptr, format);
    vsnprintf(dbug_item_small_buffer, DBUG_PRINT_SIZE-1, format, argptr);
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
            DBG_SPRINTF_CAT("(REF_ITEM*)(name:'%s.%s',", ((Item_ref*)item)->table_name.str, item->name);
          else
            DBG_SPRINTF_CAT("(REF_ITEM*)(name:'%s',", item->name);
          if (ref == not_found_item)
            DBG_CAT("not_found_item");
          else
          {
            Item *refd_item= *ref;
            dbug_add_print_item(refd_item);
          }
          DBG_CAT(")");
        }
        else
          DBG_SPRINTF_CAT("(REF_ITEM*)nullptr");
      }
      else
      {
        char *buf= (char *)alloca( SMALL_BUF+1 );
        String str(buf, SMALL_BUF, &my_charset_bin);
        str.length(0);
        item->print(&str, QT_VIEW_INTERNAL);
        DBG_CAT(str.c_ptr_safe());
      }
    }
    else
      DBG_CAT("<Item_subselect eliminated>");
  } 
  else
    DBG_CAT("<nullptr>");
}


static void dbug_add_print_items(List<Item> &list)
{
  List_iterator<Item> li(list);
  Item *sub_item;
  uint cnt= 0;
  DBG_CAT( "Children:" );
  while ((sub_item= li++))
    cnt++;
  DBG_SPRINTF_CAT( "[%d]", cnt );
  li.rewind();
  while ((sub_item= li++))
  {
    if ( strlen( dbug_big_buffer ) < DBUG_BIG_BUF_SIZE - DBUG_PRINT_SIZE - 7)
    {
      DBG_SPRINTF_CAT( "[%p: ", (void*)sub_item );
      dbug_add_print_item(sub_item);
      DBG_CAT( "]" );
    }
    else
    {
      DBG_CAT("...");
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
  DBG_CAT( "Children:" );
  while ((sub_item= li++))
    cnt++;
  DBG_SPRINTF_CAT( "[%d]", cnt );
  li.rewind();
  while ((sub_item= li++))
  {
    if ( strlen( dbug_big_buffer ) < DBUG_BIG_BUF_SIZE - DBUG_PRINT_SIZE - 7)
    {
      DBG_SPRINTF_CAT( "[%p: ", (void*)sub_item );
      dbug_add_print_item(sub_item);
      DBG_CAT( "]" );
    }
    else
    {
      DBG_CAT("...");
      break;
    }
  }
  return;
}


extern const char *dbug_print_items(List<Item> &list)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<Item> %p:{", (void*) &list );
  dbug_add_print_items( list );
  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbug_print_items_o_r(List<Item_outer_ref> &list)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<Item> %p:{", (void*) &list );
  dbug_add_print_items_o_r( list );
  DBG_CAT( "}" );
  return dbug_big_buffer;
}


static void dbug_add_print_item(Item *item)
{
  if (item)
  {
    DBG_SPRINTF_CAT( "[%p:", item);

#ifdef _DBUG_HAVE_ITEM_THD
    if (item->dbug_mem_root == item->dbug_thd->stmt_arena->mem_root)
      DBG_CAT( "S:" );
    else
      DBG_CAT( "E:" );
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
          DBG_CAT("(MULT_EQUAL_FUNC*)");
          Item_equal_fields_iterator it(*(Item_equal *)func_item);
          Item *itm;
          while ((itm= it++))
            dbug_add_print_item(itm);
          break;
          }

        case Item_func::EQ_FUNC: DBG_CAT("(EQ_FUNC*)"); break;
        case Item_func::LIKE_FUNC: DBG_CAT("(LIKE_FUNC*)"); break;
        case Item_func::EQUAL_FUNC: DBG_CAT("(EQUAL_FUNC*)"); break;
        case Item_func::ISNULL_FUNC: DBG_CAT("(ISNULL_FUNC*)"); break;
        case Item_func::UNKNOWN_FUNC:
          if ( func_item->func_name() )
            DBG_SPRINTF_CAT("(%s)", func_item->func_name());
          else
            DBG_CAT("(UNKNOWN_FUNC*)");
          break;
        case Item_func::NE_FUNC: DBG_CAT("(NE_FUNC*)"); break;
        case Item_func::LT_FUNC: DBG_CAT("(LT_FUNC*)"); break;
        case Item_func::LE_FUNC: DBG_CAT("(LE_FUNC*)"); break;
        case Item_func::GE_FUNC: DBG_CAT("(GE_FUNC*)"); break;
        case Item_func::GT_FUNC: DBG_CAT("(GT_FUNC*)"); break;
        case Item_func::FT_FUNC: DBG_CAT("(FT_FUNC*)"); break;
        case Item_func::ISNOTNULL_FUNC: DBG_CAT("(ISNOTNULL_FUNC*)"); break;
        case Item_func::COND_AND_FUNC: DBG_CAT("(COND_AND_FUNC*)"); break;
        case Item_func::COND_OR_FUNC: DBG_CAT("(COND_OR_FUNC*)"); break;
        case Item_func::XOR_FUNC: DBG_CAT("(XOR_FUNC*)"); break;
        case Item_func::BETWEEN: DBG_CAT("(BETWEEN*)"); break;
        case Item_func::IN_FUNC: DBG_CAT("(IN_FUNC*)"); break;
        case Item_func::INTERVAL_FUNC: DBG_CAT("(INTERVAL_FUNC*)"); break;
        case Item_func::ISNOTNULLTEST_FUNC: DBG_CAT("(ISNOTNULLTEST_FUNC*)"); break;
        case Item_func::SP_EQUALS_FUNC: DBG_CAT("(SP_EQUALS_FUNC*)"); break;
        case Item_func::SP_RELATE_FUNC: DBG_CAT("(SP_RELATE_FUNC*)"); break;
        case Item_func::NOT_FUNC: DBG_CAT("(NOT_FUNC*)"); break;
        case Item_func::NOT_ALL_FUNC: DBG_CAT("(NOT_ALL_FUNC*)"); break;
        case Item_func::TEMPTABLE_ROWID: DBG_CAT("(TEMPTABLE_ROWID*)"); break;
        case Item_func::NOW_FUNC: DBG_CAT("(NOW_FUNC*)"); break;
        case Item_func::NOW_UTC_FUNC: DBG_CAT("(NOW_UTC_FUNC*)"); break;
        case Item_func::SYSDATE_FUNC: DBG_CAT("(SYSDATE_FUNC*)"); break;
        case Item_func::TRIG_COND_FUNC: DBG_CAT("(TRIG_COND_FUNC*)"); break;
        case Item_func::SUSERVAR_FUNC: DBG_CAT("(SUSERVAR_FUNC*)"); break;
        case Item_func::GUSERVAR_FUNC: DBG_CAT("(GUSERVAR_FUNC*)"); break;
        case Item_func::COLLATE_FUNC: DBG_CAT("(COLLATE_FUNC*)"); break;
        case Item_func::EXTRACT_FUNC: DBG_CAT("(EXTRACT_FUNC*)"); break;
        case Item_func::CHAR_TYPECAST_FUNC: DBG_CAT("(CHAR_TYPECAST_FUNC*)"); break;
        case Item_func::FUNC_SP: DBG_CAT("(FUNC_SP*)"); break;
        case Item_func::UDF_FUNC: DBG_CAT("(UDF_FUNC*)"); break;
        case Item_func::NEG_FUNC: DBG_CAT("(NEG_FUNC*)"); break;
        case Item_func::GSYSVAR_FUNC: DBG_CAT("(GSYSVAR_FUNC*)"); break;
        case Item_func::IN_OPTIMIZER_FUNC: DBG_CAT("(IN_OPTIMIZER_FUNC*)"); break;
        case Item_func::DYNCOL_FUNC: DBG_CAT("(DYNCOL_FUNC*)"); break;
        case Item_func::JSON_EXTRACT_FUNC: DBG_CAT("(JSON_EXTRACT_FUNC*)"); break;
        case Item_func::CASE_SEARCHED_FUNC: DBG_CAT("(CASE_SEARCHED_FUNC*)"); break;
        case Item_func::CASE_SIMPLE_FUNC: DBG_CAT("(CASE_SIMPLE_FUNC*)"); break;
        default:
          DBG_CAT("(OTHER*)");
        }

        for (uint i= 0; i < func_item->argument_count(); i++)
        {
          DBG_SPRINTF_CAT("arg%d[",i);
          dbug_add_print_item_contents(func_item->arguments()[i]);
          DBG_CAT("]");
        }
      }
      break;
    case Item::COND_ITEM:
      {
        DBG_CAT("(COND_ITEM*)");
        List_iterator<Item> li= *((Item_cond*) item)->argument_list();
        dbug_add_print_item_contents(item);
        Item *sub_item;
        while ((sub_item= li++))
        {
          if (strlen(dbug_big_buffer) < DBUG_BIG_BUF_SIZE-DBUG_PRINT_SIZE-7)
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
      DBG_CAT("(FIELD_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
#if MYSQL_VERSION_ID > 100400
    case Item::CONST_ITEM:
      DBG_CAT("(CONST_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
#endif
    case Item::SUM_FUNC_ITEM:
      DBG_CAT("(SUM_FUNC_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::WINDOW_FUNC_ITEM:
      DBG_CAT("(WINDOW_FUNC_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::NULL_ITEM:
      DBG_CAT("(NULL_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::COPY_STR_ITEM:
      DBG_CAT("(COPY_STR_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::FIELD_AVG_ITEM:
      DBG_CAT("(FIELD_AVG_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::DEFAULT_VALUE_ITEM:
      DBG_CAT("(DEFAULT_VALUE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
#if MYSQL_VERSION_ID > 100414
    case Item::CONTEXTUALLY_TYPED_VALUE_ITEM:
      DBG_CAT("(CONTEXTUALLY_TYPED_VALUE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
#endif
    case Item::PROC_ITEM:
      DBG_CAT("(PROC_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::FIELD_STD_ITEM:
      DBG_CAT("(FIELD_STD_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::FIELD_VARIANCE_ITEM:
      DBG_CAT("(FIELD_VARIANCE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::INSERT_VALUE_ITEM:
      DBG_CAT("(INSERT_VALUE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::ROW_ITEM:
      DBG_CAT("(ROW_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::CACHE_ITEM:
      DBG_CAT("(CACHE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::TYPE_HOLDER:
      DBG_CAT("(TYPE_HOLDER*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::PARAM_ITEM:
      DBG_CAT("(PARAM_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::TRIGGER_FIELD_ITEM:
      DBG_CAT("(TRIGGER_FIELD_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::EXPR_CACHE_ITEM:
      DBG_CAT("(EXPR_CACHE_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    case Item::SUBSELECT_ITEM:
      DBG_CAT("(SUBSELECT_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    default:
      DBG_CAT("(UNKNOWN_ITEM*)");
      dbug_add_print_item_contents(item);
      break;
    }
    DBG_CAT("]");
  }
  else
    DBG_CAT("<nullptr>");
}


extern const char *dbug_print_items(Item *item)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "%p:{", (void*) &item );
  dbug_add_print_item( item );
  DBG_CAT( "}" );
  return dbug_big_buffer;
}


const char *dbug_print_ref_array(Ref_ptr_array *ptr_array)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "(Ref_ptr_array*)%p:{", (void*) ptr_array );
  Item *item;
  for (size_t i= 0; i < ptr_array->size(); i++)
  {
    item= (*ptr_array)[i];
    DBG_SPRINTF_CAT( "[%u:", i);
    dbug_add_print_item( item );
    DBG_CAT( "]" );
  }
  DBG_CAT( "}" );
  return dbug_big_buffer;
}

extern const char *dbug_print(List<Item> &x)      {return dbug_print_items(x);}

/*
  Return the optimizer trace collected so far for the current thread.
*/


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
    DBG_CAT( "(st_select_lex *)NULL" );
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

    DBG_CAT( str.c_ptr());

  //  if (sl->having)

  }
}

static void dbug_add_print_jointab( JOIN_TAB *join_tab );

void dbug_add_print_lex( LEX *lex)
{
#if MYSQL_VERSION_ID > 100400
  st_select_lex *sl, *select_lex= lex->first_select_lex();
#else
  st_select_lex *sl, *select_lex= &lex->select_lex;
#endif

  DBG_SPRINTF_CAT( " select_lex:%p:{", select_lex );
  dbug_add_print_selects( select_lex );
  DBG_CAT( "}" );

  DBG_CAT( " all_selects_list:" );
  for( sl= lex->all_selects_list; sl;
      sl= sl->next_select_in_list())
  {
    DBG_SPRINTF_CAT( "%p:[", sl );
    dbug_add_print_selects( sl );
    DBG_CAT( "]" );
  }

  if (select_lex->master_unit() && ( select_lex->master_unit()->is_unit_op() ||
      select_lex->master_unit()->fake_select_lex))
  {
    // this is union or similar of more than one select
    DBG_CAT( "UNION");     // TODO...
  }
  else
  {
    DBG_SPRINTF_CAT( " table_list:" );
    for (TABLE_LIST *tbl= select_lex->table_list.first; tbl;
        tbl= tbl->next_global)
    {
      DBG_CAT( "[" );
      if (tbl->select_stmt.str)
      {
        DBG_CAT( " select_stmt:" );
        DBG_CAT( tbl->select_stmt.str);
      }
      DBG_CAT( "]" );
    }

    JOIN *join= select_lex->join;
    DBG_SPRINTF_CAT( " join:%p:{", join );
    if (join)
    {
      dbug_add_print_join( join );
      if (join->join_tab)
      {
        DBG_SPRINTF_CAT( " join->join_tab:%p:{", join->join_tab );
        dbug_add_print_jointab( join->join_tab );
        DBG_CAT( "}" );
      }
    }
    DBG_CAT( "}" );
  }
}


const char *dbug_print_lex( LEX *lex)
{
  *dbug_big_buffer= (char)0;
  DBG_CAT( "(LEX *)" );
  dbug_add_print_lex( lex );
  return dbug_big_buffer;
}


const char *dbug_add_print_field(Field *field);

static void dbug_add_print_table( TABLE *table )
{
  if (table)
  {
    DBG_SPRINTF_CAT("%s,Fields:", table->alias.c_ptr());
    for(Field **f= table->field; *f; f++)
      dbug_add_print_field(*f);
  }
  else
    DBG_CAT("table:NULL");
}


const char *dbug_print_table( TABLE *table)
{
  *dbug_big_buffer= (char)0;
  DBG_CAT( "(TABLE *)" );
  dbug_add_print_table( table );
  return dbug_big_buffer;
}


static void dbug_add_print_table_list( TABLE_LIST *table_list)
{
  if(table_list->nested_join)
    DBG_SPRINTF_CAT( "nested_join:%p", table_list->nested_join );

  show_field_values= FALSE;
  for(TABLE_LIST *cur_table= table_list; 
      cur_table != NULL;
      cur_table= cur_table->next_global)
  {
    DBG_SPRINTF_CAT("['%s'", cur_table->alias.str);
    dbug_add_print_table(cur_table->table);
    DBG_CAT("]");
  }
  show_field_values= TRUE;
}


const char *dbug_print_table_list(  TABLE_LIST *table_list)
{
  *dbug_big_buffer= (char)0;
  DBG_CAT( "(TABLE_LIST *)" );
  dbug_add_print_table_list( table_list );
  return dbug_big_buffer;
}


static void dbug_add_print_jointab( JOIN_TAB *join_tab)
{
  if (!join_tab)
  {
    DBG_CAT( "(JOIN_TAB *)NULL" );
  }
  else
  {
    DBG_CAT( "(JOIN_TAB *)" );
    if (join_tab->table)
    {
      DBG_CAT( " table:" );
      if (join_tab->table->alias.c_ptr())
        DBG_CAT( join_tab->table->alias.c_ptr() );
      else
        DBG_CAT( "NULL" );
    }

    if (join_tab->tab_list)
    {
      DBG_CAT( " tab_list(global):" );
      TABLE_LIST *table_list = join_tab->tab_list;
      for ( ; table_list; table_list= table_list->next_global)
      {
        char name[10];
        strncpy( name, table_list->table_name.str, table_list->table_name.length);
        name[table_list->table_name.length]= 0;
        if (*name)
          DBG_SPRINTF_CAT( "[Name:%s type:", name );
        else
          DBG_SPRINTF_CAT( "[Name:NULL type:" );
        if (!is_trash_or_null(table_list->view))
        {
          if (table_list->algorithm == VIEW_ALGORITHM_MERGE)
            DBG_CAT( "merge" );
          else
            if (table_list->algorithm == VIEW_ALGORITHM_TMPTABLE)
              DBG_CAT( "tmptable" );
            else
              DBG_CAT( "view?" );
        }
        else
        {
          if (!is_trash_or_null(table_list->derived == NULL))
            DBG_CAT( "plain" );
        }
        if (!is_trash_or_null(table_list->nested_join))
          DBG_CAT( ",Nested" );
        if (!is_trash_or_null(table_list->jtbm_subselect))
          DBG_CAT( ",JoinTableMaterialization" );
        else
        {
          if (!is_trash_or_null(table_list->sj_subq_pred))
            DBG_CAT( " SubselectPredicate" );
        }
        DBG_CAT( ", " );
        if ( !is_trash_or_null(table_list->prep_on_expr))
        {
          DBG_CAT( " on(orig):{" );
          dbug_add_print_item( table_list->prep_on_expr );
          DBG_CAT( "}" );
        }
        if (!is_trash_or_null(table_list->jtbm_subselect))
        {
          DBG_CAT( " JtabM:{" );
          dbug_add_print_item( table_list->jtbm_subselect );
          DBG_CAT( "}" );
        }
        else
        {
          if (!is_trash_or_null(table_list->sj_subq_pred))
          {
            DBG_CAT( " SubQ:{" );
            dbug_add_print_item( table_list->sj_subq_pred );
            DBG_CAT( "}" );
          }
        }
        if (!is_trash_or_null(table_list->derived))
        {
          DBG_CAT( " Derived:{" );
          DBG_CAT( dbug_print_unit( table_list->derived ) );
          DBG_CAT( "}" );
        }
        DBG_CAT( "]" );

        if (table_list == table_list->next_global)      // lets not loop forever
          break;
      }
      DBG_CAT( "}" );
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
  DBG_CAT( " Conds:{" );
  dbug_add_print_item( join->conds );
  DBG_CAT( "}" );
  DBG_CAT( " Order:{" );
  for (ORDER *order= join->order; order;  order= order->next)
    dbug_add_print_item_contents( *order->item );
  DBG_CAT( "}" );
  DBG_CAT( " Group:{" );
  for (ORDER *group= join->group_list; group;  group= group->next)
    dbug_add_print_item_contents( *group->item );
  DBG_CAT( "}" );
  if (join->fields)
  {
    DBG_CAT( " Fields:{" );
    dbug_add_print_items( *join->fields );
    DBG_CAT( "}" );
  }
  DBG_CAT( " Tables:{" );
  dbug_add_print_jointab( join->join_tab );
  DBG_CAT( "}" );
  if (join->pushdown_query)
  {
    DBG_CAT( " Pushdown:{" );
    dbug_add_print_selects( join->pushdown_query->select_lex );
    DBG_CAT( "}" );
  }
}

const char *dbug_print_join(JOIN *join)
{
  *dbug_big_buffer= (char)0;
  DBG_CAT( "(JOIN *)" );
  dbug_add_print_join( join );
  return dbug_big_buffer;
}

const char *dbug_print_select_lex(st_select_lex *sl)
{
  // show master unit
  *dbug_big_buffer= (char)0;
  *dbug_item_small_buffer= (char)0;
  DBG_CAT( "(st_select_lex *)" );
  dbug_print_unit(sl->master_unit());   // into dbug_item_small_buffer
  if (*dbug_item_small_buffer)
  {
    DBG_CAT( " Master Unit:{" );
    DBG_CAT( dbug_item_small_buffer );
    DBG_SPRINTF_CAT( "}\n" );
  }
  DBG_SPRINTF_CAT( "%p:{", (void*) sl );
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
    DBG_SPRINTF_CAT("{%s.%s=", field->table->alias.c_ptr(), field->field_name.str);
    if (field->is_real_null())
      DBG_CAT("NULL");
    else
    {
      String tmp;
      if(field->type() == MYSQL_TYPE_BIT)
        (void) field->val_int_as_str(&tmp, 1);
      else
        field->val_str(&tmp);
      DBG_CAT(tmp.c_ptr());
    }
    DBG_CAT("}");
  }
  else
  {
    DBG_SPRINTF_CAT("{%s.%s}", field->table->alias.c_ptr(),
                               field->field_name.str);
  }
  return dbug_big_buffer;
}

const char *dbug_print_fields(Field **field)
{
  *dbug_big_buffer= (char)0;
  DBG_CAT("(Field **)");
  for( int i=0; field[i]; i++)
    dbug_add_print_field(field[i]);
  return dbug_big_buffer;
}

const char *dbug_print_field(Field *field)
{
  *dbug_big_buffer= (char)0;
  DBG_CAT("(Field *)");
  dbug_add_print_field(field);
  return dbug_big_buffer;
}

#include "sql_statistics.h"

#if MYSQL_VERSION_ID > 100338 && MYSQL_VERSION_ID < 100400
const char *dbug_print_histogram( Histogram_binary *hist )
{
  *dbug_big_buffer= (char)0;
  uchar *values= hist->get_values();

  DBG_CAT("(Histogram *)");
  if( hist->get_type() == SINGLE_PREC_HB )
  {
    DBG_CAT("(SINGLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint val= (uint) (((uint8 *) values)[i]);
      DBG_SPRINTF_CAT( "values[%u] = %u\n", i, val );
    }
    DBG_CAT("\n");
  }
  if( hist->get_type() == DOUBLE_PREC_HB )
  {
    DBG_CAT("(DOUBLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint val= (uint) uint2korr(values + i * 2);
      DBG_SPRINTF_CAT( "values[%u] = %u\n", i, val );
    }
    DBG_CAT("\n");
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

  DBG_CAT("(Column_statistics *)\nHistogram Data\n");
  if( hist->get_type() == SINGLE_PREC_HB )
  {
    DBG_CAT("(SINGLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint rval= (uint) (((uint8 *) values)[i]);
      double val= min + (max-min)*(double)rval/((uint) (1 << 8) - 1);
      DBG_SPRINTF_CAT( "values[%u] = %f\n", i, val );
    }
    DBG_CAT("\n");
  }
  if( hist->get_type() == DOUBLE_PREC_HB )
  {
    DBG_CAT("(DOUBLE_PREC_HB)\n");
    for(uint i= 0; i < hist->get_width(); i++)
    {
      uint rval= (uint) uint2korr(values + i * 2);
      double val= min + (max-min)*(double)rval/((uint) (1 << 16) - 1);
      DBG_SPRINTF_CAT( "values[%u] = %f\n", i, val );
    }
    DBG_CAT("\n");
  }

  return dbug_big_buffer;
}
#endif


extern const char* dbug_print_table_row(TABLE *table);
extern const char *dbug_print_unit(st_select_lex_unit *un);
extern const char *dbug_print_sel_arg(SEL_ARG *sel_arg);

const char *dbp(List<Item_ident> &x)      { return dbug_print_items((List<Item>&)x); }
const char *dbp(List<Item_ident> *x)      { return dbug_print_items((List<Item>&)*x); }
const char *dbp(List<Item> &x)            { return dbug_print_items(x); }
const char *dbp(List<Item> *x)            { return dbug_print_items(*x); }
const char *dbp(List<Item_outer_ref> *x)  { return dbug_print_items_o_r(*x); }
const char *dbp(List<Item_outer_ref> x)   { return dbug_print_items_o_r(x); }
const char *dbp(Item *x)                  { return dbug_print_items(x); }
const char *dbp(Item **x)                 { return dbug_print_items(*x); }
const char *dbp(st_select_lex *x)         { return dbug_print_select_lex(x); }
const char *dbp(st_select_lex x)          { return dbug_print_select_lex(&x); }
const char *dbp(JOIN *x)                  { return dbug_print_join(x); }
const char *dbp(JOIN x)                   { return dbug_print_join(&x); }
const char *dbp(JOIN_TAB *x)              { return dbug_print_jointab(x); }
const char *dbp(JOIN_TAB x)               { return dbug_print_jointab(&x); }
const char *dbp(LEX *x)                   { return dbug_print_lex(x); }
const char *dbp(TABLE *x)                 { return dbug_print_table_row(x); }
const char *dbp(st_select_lex_unit *x)    { return dbug_print_unit(x); }
const char *dbp(st_select_lex_unit x)     { return dbug_print_unit(&x); }
const char *dbp(st_select_lex_node *node) { return dbug_print_lex_node( node ); }
const char *dbp(st_select_lex_node node)  { return dbug_print_lex_node( &node ); }
const char *dbp(READ_RECORD *x)           { return dbug_print_table_row(x->table); }
const char *dbp(Field *x)                 { return dbug_print_field(x); }
const char *dbp(Field **x)                { return dbug_print_fields(x); }
const char *dbp(Ref_ptr_array *x)         { return dbug_print_ref_array(x); }
const char *dbp(Ref_ptr_array x)          { return dbug_print_ref_array(&x); }
const char *dbp(SEL_ARG *x)               { return dbug_print_sel_arg(x); }
const char *dbp(SEL_ARG x)                { return dbug_print_sel_arg(&x); }
#if MYSQL_VERSION_ID > 100400
const char *dbptrace()                    { return dbug_print_trace(); }
const char *dbptrace( Json_writer *x )    { return dbug_print_trace(x); }
#endif

const char *dbp(table_map m)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "(table_map) 0x%p", (void *)m );
  return dbug_big_buffer;
}

const char *dbp(List<Lex_ident_sys> &x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<Lex_ident_sys> %p:{", (void*) &x );

  List_iterator<Lex_ident_sys> iter(x);
  Lex_ident_sys *str;

  for (uint i= 0;(str= iter++);i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str->str );

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<Lex_ident_sys> *x)
{
  return dbp(*x);
}


const char *dbp(List<LEX_STRING> &x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<LEX_STRING> %p:{", (void*) &x );

  List_iterator<LEX_STRING> iter(x);
  LEX_STRING *str;

  for (uint i= 0;(str= iter++);i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str->str );

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<LEX_STRING> *x)
{
  return dbp(*x);
}

const char *dbp(List<LEX_CSTRING> &x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<LEX_STRING> %p:{", (void*) &x );

  List_iterator<LEX_CSTRING> iter(x);
  LEX_CSTRING *str;

  for (uint i= 0;(str= iter++);i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str->str );

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<const char *> x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<const char *> %p:{", (void*) &x );

  List_iterator<const char *> iter(x);
  const char *str;

  for (uint i= 0;(str= *(iter++));i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str );

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<LEX_CSTRING> *x)
{
  return dbp(*x);
}

const char *dbp(List<String> &x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<String> %p:{", (void*) &x );

  List_iterator<String> iter(x);
  String *str;

  for (uint i= 0;(str= iter++);i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str->ptr() );

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<String> *x)
{
  return dbp(*x);
}

const char *dbp(List<TABLE_LIST> &x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<TABLE_LIST> %p:{", (void*) &x );

  List_iterator<TABLE_LIST> iter(x);
  TABLE_LIST *str;

  for (uint i= 0;(str= iter++);i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str->alias.str );

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<TABLE_LIST> *x)
{
  return dbp(*x);
}


const char *dbp(List<JOIN_TAB_RANGE> &x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<JOIN_TAB_RANGE> %p:{", (void*) &x );

#if 0  ///TODO
  List_iterator<JOIN_TAB_RANGE> iter(x);
  JOIN_TAB_RANGE *str;

  for (uint i= 0;(str= iter++);i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str-> );
#endif

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<JOIN_TAB_RANGE> *x)
{
  return dbp(*x);
}

const char *dbp(List<Key_part_spec> &x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<Key_part_spec> %p:{", (void*) &x );

#if 0  ///TODO
  List_iterator<Key_part_spec> iter(x);
  Key_part_spec *str;

  for (uint i= 0;(str= iter++);i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str-> );
#endif

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<Key_part_spec> *x)
{
  return dbp(*x);
}

const char *dbp(List<Sql_condition> &x)
{
  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "List<Sql_condition> %p:{", (void*) &x );

#if 0  ///TODO
  List_iterator<Sql_condition> iter(x);
  Sql_condition *str;

  for (uint i= 0;(str= iter++);i++)
    DBG_SPRINTF_CAT( "[%d:%s]", i, str-> );
#endif

  DBG_CAT( "}" );
  return dbug_big_buffer;
}

const char *dbp(List<Sql_condition> *x)
{
  return dbp(*x);
}

const char *dbp(ORDER *x)
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

char dbug_print_row_buffer[512];

const char* dbp_row(TABLE *table, bool print_rowid)
{
  Field **pfield;
  String tmp(dbug_print_row_buffer,
             sizeof(dbug_print_row_buffer),&my_charset_bin);

  *dbug_big_buffer= (char)0;
  DBG_SPRINTF_CAT( "(TABLE *) %p:{", (void*) &table );

  if (table->alias.ptr())
  {
    DBG_SPRINTF_CAT( "[name:%s]", table->alias.ptr());
  }

  bool first= true;

  for (pfield= table->field; *pfield ; pfield++)
  {
    if (table->read_set && !bitmap_is_set(table->read_set, (*pfield)->field_index))
      continue;
    
    if (first)
      first= false;
    else
      DBG_CAT(",");

    if ((*pfield)->field_name.str)
      DBG_CAT((*pfield)->field_name.str);
    else
      DBG_CAT("NULL");
  }

  DBG_CAT(")=(");

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
      DBG_CAT(",");

    if (field->is_null())
      DBG_CAT("NULL");
    else
    {
      if (field->type() == MYSQL_TYPE_BIT)
        (void) field->val_int_as_str(&tmp, 1);
      else
        field->val_str(&tmp);
      if (*tmp.ptr())
        DBG_CAT(tmp.c_ptr_safe());
      else
        DBG_CAT("NULL");
    }
  }
  DBG_CAT(")");
  if (print_rowid)
  {
    DBG_CAT(" rowid");
    for (uint i=0; i < table->file->ref_length; i++)
    {
      DBG_SPRINTF_CAT(":%x", (uchar)table->file->ref[i]);
    }
  }

  return dbug_big_buffer;
}

const char* dbp_row(TABLE *table)
{
  return dbp_row(table, true);
}

const char *dbp(TABLE_LIST &tl)
{
  return dbug_print_table_list( &tl );
}

const char *dbp(TABLE_LIST *tl)
{
  return dbug_print_table_list( tl );
}

const char *dbp_add_print_key_part( KEY_PART_INFO *kp )
{
  DBG_CAT( "(KEY_PART_INFO *):" );
  if (kp)
    dbug_add_print_field( kp->field );
  else
    DBG_CAT( "NULL");
  return dbug_big_buffer;
}

const char *dbp( KEY_PART_INFO *kp )
{
  *dbug_big_buffer= (char)0;
  return dbp_add_print_key_part(kp);
}

const char *dbp( KEY_PART_INFO kp )
{
  return dbp(&kp);
}


void dbp_add_print_key( KEY *k )
{
  if (k)
  {
    if(k->name.str)
      DBG_SPRINTF_CAT( "Name:%s,", k->name.str);
//    if(k->table && k->table->alias.ptr())
//      DBG_SPRINTF_CAT("table:%s,", k->table->alias.ptr());
    DBG_SPRINTF_CAT("length:%d,", k->key_length);
    DBG_SPRINTF_CAT("usable parts:%d,", k->usable_key_parts);
    DBG_SPRINTF_CAT("parts:%d,", k->user_defined_key_parts);
    for(uint i= 0; i < k->ext_key_parts; i++)
      dbp_add_print_key_part(&k->key_part[i]);
  }
}


const char *dbp( KEY *k )
{
  *dbug_big_buffer= (char)0;
  dbp_add_print_key( k );
  return dbug_big_buffer;
}



const char *dbp( KEY k )
{
  return dbp(&k);
}

const char *dbp( LEX_CSTRING *k )
{
  DBG_CAT( "(LEX_CSTRING) " );
  uint npos= strlen(dbug_big_buffer);
  strncpy( dbug_big_buffer+npos, k->str, k->length );
  npos+= k->length;
  dbug_big_buffer[npos]= (char)0;
  return dbug_big_buffer;
}

const char *dbp( LEX_CSTRING k )
{
  return dbp(&k);
}

const char *dbp( SQL_I_List<TABLE_LIST> *k )
{
  TABLE_LIST *element= k->first;

  *dbug_big_buffer= (char)0;
  for (uint i= 0; i < k->elements; i++)
  {
    DBG_SPRINTF_CAT("[i:%u,", i);
    dbug_add_print_table_list( element );
    element= element->next_global;
    DBG_CAT("]");
  }
  return dbug_big_buffer;
}

const char *dbp( SQL_I_List<TABLE_LIST> &k )
{
  return dbp(&k);
}

const char *dbp( KEYUSE *k)
{
  TABLE *t= k->table;
  uint key= k->key;
  *dbug_big_buffer= (char)0;

  if (t)
    DBG_SPRINTF_CAT( "TABLE:%s,", t->alias.c_ptr() );

  dbp_add_print_key( &t->key_info[key] );

  if (k->val)
  {
    DBG_CAT("Value:");
    dbug_add_print_item( k->val );
  }

  return dbug_big_buffer;
}

void dbug_add_print_sel_arg(SEL_ARG *arg, KEY_PART *part)
{
  if (!(arg->min_flag & NO_MIN_RANGE))
  {
    store_key_image_to_rec(part->field, arg->min_value, part->length);
    dbug_add_print_field( part->field );
    if (arg->min_flag & NEAR_MIN)
      DBG_CAT(" < ");
    else
      DBG_CAT(" <= ");
  }

  DBG_SPRINTF_CAT("%s", part->field->field_name.str);

  if (!(arg->max_flag & NO_MAX_RANGE))
  {
    if (arg->max_flag & NEAR_MAX)
      DBG_CAT(" < ");
    else
      DBG_CAT(" <= ");
    store_key_image_to_rec(part->field, arg->max_value, part->length);
    dbug_add_print_field( part->field );
  }
}

const char *dbp(SEL_TREE *arg, KEY_PART *parts)
{
  *dbug_big_buffer= (char)0;
  DBG_CAT( "(SEL_TREE *) type:");
  switch (arg->type)
  {
    case 0:
    DBG_CAT( "IMPOSSIBLE" );
    break;
    case 1:
    DBG_CAT( "ALWAYS" );
    break;
    case 2:
    DBG_CAT( "MAYBE" );
    break;
    case 3:
    DBG_CAT( "KEY" );
    break;
    case 4:
    DBG_CAT( "KEY_SMALLER" );
    break;
    default:
    DBG_CAT( "UNKNOWN" );
    break;
  }
  key_map::Iterator it(arg->keys_map);
  int key_no;
  while ((key_no= it++) != key_map::Iterator::BITMAP_END)
  {
    SEL_ARG *key1= arg->get_key(key_no);
    dbug_add_print_sel_arg( key1, parts+key1->part );
  }

  return dbug_big_buffer;
}


const char *dbp(SEL_ARG *arg, KEY_PART *part)
{
  *dbug_big_buffer= (char)0;
  dbug_add_print_sel_arg(arg, part);
  return dbug_big_buffer;
}


// return which parts of operand are selected by this bitmap
const char *dbp(MY_BITMAP *bitmap, void *operand)
{

  return dbug_big_buffer;
}


const char *dbp( Name_resolution_context *ctx )
{
  *dbug_big_buffer= (char)0;

  if (ctx)
  {
    DBG_CAT( "select_lex:" );
    /*
    if (ctx->get_select_lex())
      dbug_add_print_selects( ctx->get_select_lex() );
    else
      DBG_CAT( "NULL" );
      */

    DBG_CAT( ",first table:" );
    if (ctx->first_name_resolution_table)
      dbug_add_print_table_list( ctx->first_name_resolution_table );
    else
      DBG_CAT( "NULL" );

    DBG_CAT( ",last table:" );
    if (ctx->last_name_resolution_table)
      dbug_add_print_table_list( ctx->last_name_resolution_table );
    else
      DBG_CAT( "NULL" );

    /*
    DBG_SPRINTF_CAT(",outer_context:(Name_resolution_context *)%x",
        ctx->get_outer_context());
        */
  }

  return dbug_big_buffer;
}


void dbug_add_print_ref_to_outside( Item_subselect::Ref_to_outside *ref )
{
  if (ref)
  {
    DBG_CAT( "Item:" );
    if (ref->item)
      dbug_add_print_item( ref->item );
    else
      DBG_CAT( "NULL" );

    DBG_CAT( ",select_lex:" );

    if (ref->select)
      dbug_add_print_selects( ref->select );
    else
      DBG_CAT( "NULL" );
  }
}


const char *dbp( Item_subselect::Ref_to_outside *ref )
{
  *dbug_big_buffer= (char)0;

  dbug_add_print_ref_to_outside( ref );

  return dbug_big_buffer;
}


const char *dbp( List<Item_subselect::Ref_to_outside> *ref )
{
  *dbug_big_buffer= (char)0;

  if (ref)
  {
    List_iterator_fast<Item_subselect::Ref_to_outside> it(*ref);
    Item_subselect::Ref_to_outside *upper;

    while ((upper= it++))
    {
      DBG_CAT( "[" );
      dbug_add_print_ref_to_outside( upper );
      DBG_CAT( "]" );
    }
  }

  return dbug_big_buffer;
}


const char *dbp( List<Item_subselect::Ref_to_outside> &ref ) {return dbp(&ref);}
const char *dbp( List<Item_field> *ref ) {return dbp((List<Item> *)ref);}
const char *dbp( List<Item_field> ref ) {return dbp((List<Item> *)&ref);}


#endif /*DBUG_OFF*/
